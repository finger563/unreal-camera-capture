#include "CameraCaptureSubsystem.h"
#include "IntrinsicSceneCaptureComponent2D.h"
#include "Utilities.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "Async/Async.h"
#include "ImageUtils.h"

// ============================================================================
// FCameraIdentifier Implementation
// ============================================================================

FCameraIdentifier FCameraIdentifier::Generate(const UIntrinsicSceneCaptureComponent2D* Camera)
{
	FCameraIdentifier Identifier;

	if (!Camera || !Camera->GetOwner())
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Cannot generate ID for null camera"));
		Identifier.FallbackGUID = FGuid::NewGuid();
		Identifier.ActorName = Identifier.FallbackGUID.ToString();
		Identifier.ComponentName = TEXT("UnknownCamera");
		Identifier.UniqueID = FString::Printf(TEXT("%s::%s"), *Identifier.ActorName, *Identifier.ComponentName);
		return Identifier;
	}

	AActor* Owner = Camera->GetOwner();

	// Get actor name (e.g., "Robot_BP_C_0")
	Identifier.ActorName = Owner->GetName();

	// Get component name (e.g., "HeadCamera")
	Identifier.ComponentName = Camera->GetName();

	// Generate unique ID
	Identifier.UniqueID = FString::Printf(TEXT("%s::%s"), *Identifier.ActorName, *Identifier.ComponentName);

	// Generate fallback GUID
	Identifier.FallbackGUID = FGuid::NewGuid();

	return Identifier;
}

FString FCameraIdentifier::GetFullPath(const FString& BaseDir) const
{
	return FPaths::Combine(BaseDir, ActorName, ComponentName);
}

// ============================================================================
// UCameraCaptureSubsystem Implementation
// ============================================================================

UCameraCaptureSubsystem::UCameraCaptureSubsystem()
{
	// Default output directory
	OutputDirectory = FPaths::ProjectSavedDir() / TEXT("CameraCaptures");
}

void UCameraCaptureSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Load M_DmvCapture material from plugin Content folder
	FString MaterialPath = TEXT("/Script/Engine.Material'/CameraCapture/Materials/M_DmvCapture.M_DmvCapture'");
	DmvCaptureMaterialBase = Cast<UMaterial>(StaticLoadObject(UMaterial::StaticClass(), nullptr, *MaterialPath));

	if (!DmvCaptureMaterialBase)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Failed to load M_DmvCapture material from: %s"), *MaterialPath);
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Depth+motion capture will be disabled unless material is set with SetDmvMaterial()"));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Loaded M_DmvCapture material successfully from plugin"));
	}

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Initialized"));
}

void UCameraCaptureSubsystem::Deinitialize()
{
	// Stop capture if active
	if (bIsCapturing)
	{
		StopCapture();
	}

	// Drop any pending readbacks
	PendingCaptures.Empty();

	// Clear all registrations
	RegisteredCameras.Empty();
	CameraIDMap.Empty();
	UsedActorNames.Empty();
	DmvRenderTargets.Empty();
	DmvCameras.Empty();

	Super::Deinitialize();

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Deinitialized"));
}

void UCameraCaptureSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Always harvest completed readbacks (even between kick frames)
	HarvestReadyReadbacks();

	// Safety check - only tick if initialized and capturing
	if (!IsInitialized() || !bIsCapturing)
	{
		return;
	}

	CurrentFrameCounter++;

	// Check if we should capture this frame
	if (CurrentFrameCounter % CaptureEveryNFrames == 0)
	{
		KickAllCaptures();
	}
}

TStatId UCameraCaptureSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCameraCaptureSubsystem, STATGROUP_Tickables);
}

bool UCameraCaptureSubsystem::IsTickable() const
{
	// Tick if we're capturing OR if there are pending readbacks to harvest
	return IsInitialized() && (bIsCapturing || PendingCaptures.Num() > 0) && !IsTemplate();
}

void UCameraCaptureSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] World begin play"));
}

// ============================================================================
// Camera Registration
// ============================================================================

void UCameraCaptureSubsystem::RegisterCamera(UIntrinsicSceneCaptureComponent2D* Camera)
{
	if (!Camera)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Attempted to register null camera"));
		return;
	}

	if (!IsInitialized())
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Cannot register camera - subsystem not initialized"));
		return;
	}

	// Check if already registered
	if (RegisteredCameras.Contains(Camera))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Camera already registered: %s"), *Camera->GetName());
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Registering camera: %s"), *Camera->GetName());

	// Generate unique identifier
	FCameraIdentifier CameraID = GenerateCameraID(Camera);

	// Add to registry
	RegisteredCameras.Add(Camera);
	CameraIDMap.Add(Camera, CameraID);

	// Create DMV camera if depth/motion capture is enabled
	if ((bCaptureDepth || bCaptureMotionVectors) && DmvCaptureMaterialBase)
	{
		SetupDmvCamera(Camera);
	}

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Registered camera: %s"), *CameraID.ToString());
}

void UCameraCaptureSubsystem::SetupDmvCamera(UIntrinsicSceneCaptureComponent2D* RgbCamera)
{
	if (!RgbCamera || !DmvCaptureMaterialBase)
	{
		return;
	}

	// Get intrinsics for render target creation
	FCameraIntrinsics Intrinsics = RgbCamera->GetActiveIntrinsics();
	int32			  Width = Intrinsics.ImageWidth;
	int32			  Height = Intrinsics.ImageHeight;

	// Create DMV camera using RGB camera's actual class so subclass-specific
	// properties/overrides are preserved on the copy
	FString							   DmvName = RgbCamera->GetName() + TEXT("_dmv");
	UIntrinsicSceneCaptureComponent2D* DmvCamera = NewObject<UIntrinsicSceneCaptureComponent2D>(
		RgbCamera->GetOwner(),
		RgbCamera->GetClass(),
		FName(*DmvName),
		RF_Transient,
		RgbCamera // Use RGB camera as template — copies all intrinsics properties
	);

	if (!DmvCamera)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to create DMV camera for %s"), *RgbCamera->GetName());
		return;
	}

	DmvCamera->SetupAttachment(RgbCamera);
	DmvCamera->SetRelativeLocation(FVector::ZeroVector);
	DmvCamera->SetRelativeRotation(FRotator::ZeroRotator);

	// Configure DMV camera capture settings
	DmvCamera->bCaptureEveryFrame = false;
	DmvCamera->bCaptureOnMovement = false;
	DmvCamera->bAlwaysPersistRenderingState = true;
	DmvCamera->CaptureSource = SCS_FinalColorLDR;

	// Disable frustum visualization on the DMV copy
	DmvCamera->bDrawFrustumInGame = false;
	DmvCamera->bDrawFrustumInEditor = false;

	// Note: Do NOT call ApplyIntrinsics() manually here — it is called automatically
	// via BeginPlay() (triggered by RegisterComponent below). The bMaintainYAxis path
	// is not idempotent (it mutates FOVAngle), so a second call would corrupt the FOV.

	// Create dynamic material instance
	UMaterialInstanceDynamic* DmvMaterial = UMaterialInstanceDynamic::Create(DmvCaptureMaterialBase, this);
	if (DmvMaterial)
	{
		DmvCamera->PostProcessSettings.WeightedBlendables.Array.Empty();
		DmvCamera->PostProcessSettings.WeightedBlendables.Array.Add(FWeightedBlendable(1.0f, DmvMaterial));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to create DMV material instance for %s"), *RgbCamera->GetName());
		return;
	}

	// Create render target for DMV — RGBA32f for depth float precision
	UTextureRenderTarget2D* DmvRT = NewObject<UTextureRenderTarget2D>(this);
	DmvRT->RenderTargetFormat = RTF_RGBA32f;
	DmvRT->InitAutoFormat(Width, Height);
	DmvRT->UpdateResourceImmediate(true);
	DmvCamera->TextureTarget = DmvRT;

	// Register component so it gets ticked and rendered
	DmvCamera->RegisterComponent();

	// Store references
	DmvCameras.Add(RgbCamera, DmvCamera);
	DmvRenderTargets.Add(RgbCamera, DmvRT);

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Created DMV camera '%s' with render target (%dx%d), intrinsics=%s"),
		*DmvName, Width, Height, DmvCamera->bUseCustomIntrinsics ? TEXT("custom") : TEXT("default"));
}

void UCameraCaptureSubsystem::UnregisterCamera(UIntrinsicSceneCaptureComponent2D* Camera)
{
	if (!Camera)
	{
		return;
	}

	// Remove from registry
	int32 RemovedCount = RegisteredCameras.Remove(Camera);

	if (RemovedCount > 0)
	{
		FCameraIdentifier* CameraID = CameraIDMap.Find(Camera);
		if (CameraID)
		{
			// Remove actor name from used set if this was the last camera from that actor
			bool bActorStillUsed = false;
			for (const auto& Pair : CameraIDMap)
			{
				if (Pair.Value.ActorName == CameraID->ActorName && Pair.Key != Camera)
				{
					bActorStillUsed = true;
					break;
				}
			}

			if (!bActorStillUsed)
			{
				UsedActorNames.Remove(CameraID->ActorName);
			}

			UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Unregistered camera: %s"), *CameraID->ToString());
		}

		// Clean up DMV camera if it exists
		TWeakObjectPtr<USceneCaptureComponent2D>* DmvCameraPtr = DmvCameras.Find(Camera);
		if (DmvCameraPtr && DmvCameraPtr->IsValid())
		{
			USceneCaptureComponent2D* DmvCamera = DmvCameraPtr->Get();
			if (DmvCamera)
			{
				DmvCamera->DestroyComponent();
			}
		}
		DmvCameras.Remove(Camera);
		DmvRenderTargets.Remove(Camera);

		CameraIDMap.Remove(Camera);
	}
}

TArray<UIntrinsicSceneCaptureComponent2D*> UCameraCaptureSubsystem::GetRegisteredCameras() const
{
	TArray<UIntrinsicSceneCaptureComponent2D*> ValidCameras;

	for (const TWeakObjectPtr<UIntrinsicSceneCaptureComponent2D>& WeakCamera : RegisteredCameras)
	{
		if (WeakCamera.IsValid())
		{
			ValidCameras.Add(WeakCamera.Get());
		}
	}

	return ValidCameras;
}

// ============================================================================
// Capture Control
// ============================================================================

void UCameraCaptureSubsystem::StartCapture()
{
	if (bIsCapturing)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Already capturing"));
		return;
	}

	if (RegisteredCameras.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] No cameras registered, cannot start capture"));
		return;
	}

	bIsCapturing = true;
	CurrentFrameCounter = 0;
	TotalFramesCaptured = 0;
	FrameIdCounter = 0;
	CaptureStartTime = FPlatformTime::Seconds();

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Started capture with %d cameras"), RegisteredCameras.Num());
	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Output directory: %s"), *OutputDirectory);
}

void UCameraCaptureSubsystem::StopCapture()
{
	if (!bIsCapturing)
	{
		return;
	}

	bIsCapturing = false;

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Stopped capture. Total frames: %lld"), TotalFramesCaptured);
}

void UCameraCaptureSubsystem::CaptureFrame()
{
	if (RegisteredCameras.Num() == 0)
	{
		UE_LOG(LogTemp, Verbose, TEXT("[CameraCaptureSubsystem] No cameras registered"));
		return;
	}

	KickAllCaptures();
}

void UCameraCaptureSubsystem::SetCaptureRate(int32 InCaptureEveryNFrames)
{
	CaptureEveryNFrames = FMath::Max(1, InCaptureEveryNFrames);

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Set capture rate: every %d frame(s)"), CaptureEveryNFrames);
}

void UCameraCaptureSubsystem::SetOutputDirectory(const FString& Directory)
{
	OutputDirectory = Directory;

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Set output directory: %s"), *OutputDirectory);
}

void UCameraCaptureSubsystem::SetCaptureChannels(bool bRGB, bool bDepth, bool bMotionVectors)
{
	bCaptureRGB = bRGB;
	bCaptureDepth = bDepth;
	bCaptureMotionVectors = bMotionVectors;

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Set capture channels: RGB=%d, Depth=%d, Motion=%d"),
		bRGB, bDepth, bMotionVectors);
}

void UCameraCaptureSubsystem::SetDmvMaterial(UMaterial* Material)
{
	if (!Material)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] SetDmvMaterial called with null material"));
		return;
	}

	DmvCaptureMaterialBase = Material;
	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Set DMV material: %s"), *Material->GetName());

	// If we already have registered cameras, set up their DMV cameras now
	for (const TWeakObjectPtr<UIntrinsicSceneCaptureComponent2D>& WeakCamera : RegisteredCameras)
	{
		if (WeakCamera.IsValid())
		{
			UIntrinsicSceneCaptureComponent2D* Camera = WeakCamera.Get();

			// Only set up if not already set up
			if (!DmvCameras.Contains(Camera))
			{
				SetupDmvCamera(Camera);
			}
		}
	}
}

void UCameraCaptureSubsystem::SetSerializationEnabled(bool bEnabled)
{
	bSerializationEnabled = bEnabled;
	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Serialization %s"), bEnabled ? TEXT("enabled") : TEXT("disabled"));
}

FCaptureStatistics UCameraCaptureSubsystem::GetStatistics() const
{
	FCaptureStatistics Stats;
	Stats.TotalFramesCaptured = TotalFramesCaptured;
	Stats.RegisteredCameraCount = RegisteredCameras.Num();
	Stats.AverageCaptureTimeMs = AverageCaptureTimeMs;
	Stats.LastCaptureTimeMs = LastCaptureDurationMs;
	return Stats;
}

// ============================================================================
// Legacy wrapper
// ============================================================================

void UCameraCaptureSubsystem::ExecuteSynchronizedCapture()
{
	KickAllCaptures();
}

// ============================================================================
// Phase 1: Kick all scene captures + enqueue async GPU readbacks
// ============================================================================

void UCameraCaptureSubsystem::KickAllCaptures()
{
	double StartTime = FPlatformTime::Seconds();
	int32  KickedCount = 0;

	for (int32 i = RegisteredCameras.Num() - 1; i >= 0; --i)
	{
		TWeakObjectPtr<UIntrinsicSceneCaptureComponent2D>& WeakCamera = RegisteredCameras[i];

		if (!WeakCamera.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Removing invalid camera at index %d"), i);
			RegisteredCameras.RemoveAt(i);
			continue;
		}

		UIntrinsicSceneCaptureComponent2D* Camera = WeakCamera.Get();

		// Ensure render targets exist
		EnsureCameraRenderTarget(Camera);

		// Build metadata snapshot (cheap — no pixel data)
		FPendingCameraCapture Pending;
		Pending.Metadata = BuildCaptureMetadata(Camera);

		// --- Kick RGB capture + enqueue async readback ---
		if (bCaptureRGB && Camera->TextureTarget)
		{
			Camera->CaptureScene();

			UTextureRenderTarget2D* RgbRT = Camera->TextureTarget;
			Pending.RgbReadback.Width = RgbRT->SizeX;
			Pending.RgbReadback.Height = RgbRT->SizeY;
			Pending.RgbReadback.bIsFloat = (RgbRT->RenderTargetFormat == RTF_RGBA32f || RgbRT->RenderTargetFormat == RTF_RGBA16f);
			Pending.Metadata.Width = RgbRT->SizeX;
			Pending.Metadata.Height = RgbRT->SizeY;

			EnqueueAsyncReadback(RgbRT, Pending.RgbReadback.Readback);
			Pending.bHasRgb = true;
		}

		// --- Kick DMV capture + enqueue async readback ---
		if (bCaptureDepth || bCaptureMotionVectors)
		{
			TWeakObjectPtr<USceneCaptureComponent2D>* DmvCameraPtr = DmvCameras.Find(Camera);
			if (DmvCameraPtr && DmvCameraPtr->IsValid())
			{
				USceneCaptureComponent2D* DmvCamera = DmvCameraPtr->Get();
				DmvCamera->CaptureScene();

				UTextureRenderTarget2D* DmvRT = DmvCamera->TextureTarget;
				if (DmvRT)
				{
					Pending.DmvReadback.Width = DmvRT->SizeX;
					Pending.DmvReadback.Height = DmvRT->SizeY;
					Pending.DmvReadback.bIsFloat = true; // DMV is always RGBA32f

					EnqueueAsyncReadback(DmvRT, Pending.DmvReadback.Readback);
					Pending.bHasDmv = true;
				}
			}
		}

        // If neither RGB nor DMV was kicked, skip enqueueing this capture
        // (should be rare since RGB is usually enabled, but just in case)
        if (!Pending.bHasRgb && !Pending.bHasDmv)
        {
            continue;
        }

        // Only enqueue a pending capture if at least one channel was actually
        // kicked
		PendingCaptures.Add(MoveTemp(Pending));
		KickedCount++;
	}

	FrameIdCounter++;

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
	LastCaptureDurationMs = static_cast<float>(ElapsedMs);

	// Running average
	if (AverageCaptureTimeMs == 0.0f)
	{
		AverageCaptureTimeMs = LastCaptureDurationMs;
	}
	else
	{
		AverageCaptureTimeMs = AverageCaptureTimeMs * 0.9f + LastCaptureDurationMs * 0.1f;
	}

	UE_LOG(LogTemp, Verbose, TEXT("[CameraCaptureSubsystem] Kicked %d cameras in %.2fms (frame %lld, pending: %d)"),
		KickedCount, ElapsedMs, FrameIdCounter, PendingCaptures.Num());
}

void UCameraCaptureSubsystem::EnqueueAsyncReadback(UTextureRenderTarget2D* RenderTarget, TUniquePtr<FRHIGPUTextureReadback>& OutReadback)
{
	if (!RenderTarget)
	{
		return;
	}

	FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] No render target resource for async readback"));
		return;
	}

	OutReadback = MakeUnique<FRHIGPUTextureReadback>(TEXT("CamCaptureReadback"));

	FRHIGPUTextureReadback*		  ReadbackPtr = OutReadback.Get();
	FTextureRenderTargetResource* ResourcePtr = RTResource;

	ENQUEUE_RENDER_COMMAND(CameraCaptureEnqueueReadback)
	(
		[ReadbackPtr, ResourcePtr](FRHICommandListImmediate& RHICmdList) {
			FRHITexture* Texture = ResourcePtr->GetRenderTargetTexture();
			if (Texture)
			{
				ReadbackPtr->EnqueueCopy(RHICmdList, Texture);
			}
		});
}

// ============================================================================
// Phase 2: Poll pending readbacks + harvest completed ones
// ============================================================================

void UCameraCaptureSubsystem::HarvestReadyReadbacks()
{
	if (PendingCaptures.Num() == 0)
	{
		return;
	}

	for (int32 i = PendingCaptures.Num() - 1; i >= 0; --i)
	{
		FPendingCameraCapture& Pending = PendingCaptures[i];
		Pending.FramesWaiting++;

		// Check if ALL readbacks for this camera are ready (non-blocking poll)
		bool bRgbReady = !Pending.bHasRgb || !Pending.RgbReadback.Readback || Pending.RgbReadback.Readback->IsReady();
		bool bDmvReady = !Pending.bHasDmv || !Pending.DmvReadback.Readback || Pending.DmvReadback.Readback->IsReady();

		if (bRgbReady && bDmvReady)
		{
			// Harvest pixel data from GPU staging buffers (fast memcpy, no stall)
			FCaptureData& Data = Pending.Metadata;

			if (Pending.bHasRgb && Pending.RgbReadback.Readback)
			{
				HarvestRgbReadback(Pending.RgbReadback, Data);
			}

			if (Pending.bHasDmv && Pending.DmvReadback.Readback)
			{
				HarvestDmvReadback(Pending.DmvReadback, Data);
			}

			// Wrap in shared ref so listeners can safely retain the data
			TSharedRef<const FCaptureData> SharedData = MakeShared<FCaptureData>(MoveTemp(Data));

			// Notify listeners (streaming, etc.)
			OnFrameCaptured.Broadcast(SharedData);

			if (bSerializationEnabled)
			{
				SerializeCaptureData(SharedData);
			}

			TotalFramesCaptured++;

			PendingCaptures.RemoveAt(i);
		}
		else if (Pending.FramesWaiting > MaxReadbackWaitFrames)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Dropping capture for %s (readback timed out after %d frames)"),
				*Pending.Metadata.CameraID.ToString(), Pending.FramesWaiting);
			PendingCaptures.RemoveAt(i);
		}
	}
}

void UCameraCaptureSubsystem::HarvestRgbReadback(FPendingReadback& Readback, FCaptureData& OutData)
{
	int32 RowPitchInPixels = 0;
	int32 BufferHeight = 0;
	void* SrcData = Readback.Readback->Lock(RowPitchInPixels, &BufferHeight);

	if (!SrcData)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to lock RGB readback"));
		Readback.Readback->Unlock();
		return;
	}

	const int32 Width = Readback.Width;
	const int32 Height = Readback.Height;
	OutData.ImageData.SetNumUninitialized(Width * Height);

	if (Readback.bIsFloat)
	{
		// Source is FLinearColor (RGBA32f) — convert to FColor
		const FLinearColor* SrcRow = static_cast<const FLinearColor*>(SrcData);
		for (int32 y = 0; y < Height; y++)
		{
			for (int32 x = 0; x < Width; x++)
			{
				OutData.ImageData[y * Width + x] = SrcRow[x].ToFColor(true);
			}
			SrcRow += RowPitchInPixels;
		}
	}
	else
	{
		// Source is BGRA8 (FColor) — fast memcpy path
		const FColor* SrcRow = static_cast<const FColor*>(SrcData);
		if (Width == RowPitchInPixels)
		{
			FMemory::Memcpy(OutData.ImageData.GetData(), SrcData, Width * Height * sizeof(FColor));
		}
		else
		{
			FColor* Dst = OutData.ImageData.GetData();
			for (int32 y = 0; y < Height; y++)
			{
				FMemory::Memcpy(Dst, SrcRow, Width * sizeof(FColor));
				Dst += Width;
				SrcRow += RowPitchInPixels;
			}
		}
	}

	Readback.Readback->Unlock();
}

void UCameraCaptureSubsystem::HarvestDmvReadback(FPendingReadback& Readback, FCaptureData& OutData)
{
	int32 RowPitchInPixels = 0;
	int32 BufferHeight = 0;
	void* SrcData = Readback.Readback->Lock(RowPitchInPixels, &BufferHeight);

	if (!SrcData)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to lock DMV readback"));
		Readback.Readback->Unlock();
		return;
	}

	const int32 Width = Readback.Width;
	const int32 Height = Readback.Height;
	const int32 NumPixels = Width * Height;

	OutData.DepthData.SetNumUninitialized(NumPixels);
	OutData.MotionVectorData.SetNumUninitialized(NumPixels);

	// DMV render target is RGBA32f: R=Depth, G=MotionX, B=MotionY, A=1
	const FLinearColor* SrcRow = static_cast<const FLinearColor*>(SrcData);
	for (int32 y = 0; y < Height; y++)
	{
		const int32 RowStart = y * Width;
		for (int32 x = 0; x < Width; x++)
		{
			const FLinearColor& Pixel = SrcRow[x];
			OutData.DepthData[RowStart + x] = Pixel.R;
			OutData.MotionVectorData[RowStart + x] = FVector2D(Pixel.G, Pixel.B);
		}
		SrcRow += RowPitchInPixels;
	}

	Readback.Readback->Unlock();
}

// ============================================================================
// Metadata + Render Target Helpers
// ============================================================================

FCaptureData UCameraCaptureSubsystem::BuildCaptureMetadata(UIntrinsicSceneCaptureComponent2D* Camera)
{
	FCaptureData Data;

	FCameraIdentifier* CameraID = CameraIDMap.Find(Camera);
	if (CameraID)
	{
		Data.CameraID = *CameraID;
	}

	Data.FrameNumber = FrameIdCounter;
	Data.Timestamp = FPlatformTime::Seconds() - CaptureStartTime;
	Data.WorldTransform = Camera->GetComponentTransform();
	Data.Intrinsics = Camera->GetActiveIntrinsics();
	Data.bUsedCustomProjectionMatrix = Camera->bUseCustomProjectionMatrix;

	if (Data.bUsedCustomProjectionMatrix)
	{
		Data.ProjectionMatrix = Camera->CustomProjectionMatrix;
	}

	if (AActor* Owner = Camera->GetOwner())
	{
		Data.ActorPath = Owner->GetPathName();
	}

	UWorld* World = GetWorld();
	if (World)
	{
		Data.LevelName = World->GetMapName();
	}

	Data.Width = Data.Intrinsics.ImageWidth;
	Data.Height = Data.Intrinsics.ImageHeight;

	return Data;
}

void UCameraCaptureSubsystem::EnsureCameraRenderTarget(UIntrinsicSceneCaptureComponent2D* Camera)
{
	if (Camera->TextureTarget)
	{
		return;
	}

	FCameraIntrinsics Intrinsics = Camera->GetActiveIntrinsics();
	int32			  Width = Intrinsics.ImageWidth;
	int32			  Height = Intrinsics.ImageHeight;

	UTextureRenderTarget2D* NewRenderTarget = NewObject<UTextureRenderTarget2D>(Camera);
	NewRenderTarget->RenderTargetFormat = RTF_RGBA8; // RGB only needs 8-bit
	NewRenderTarget->InitAutoFormat(Width, Height);
	NewRenderTarget->UpdateResourceImmediate(true);

	Camera->TextureTarget = NewRenderTarget;

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Created RGBA8 render target (%dx%d) for camera %s"),
		Width, Height, *Camera->GetName());
}

// ============================================================================
// Serialization (dispatched to background thread)
// ============================================================================

void UCameraCaptureSubsystem::SerializeCaptureData(TSharedRef<const FCaptureData> Data)
{
	FString OutputDir = OutputDirectory;
	bool	bRGB = bCaptureRGB;
	bool	bDepth = bCaptureDepth;
	bool	bMotion = bCaptureMotionVectors;

	// Lambda captures the shared ref — keeps data alive until async write completes
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[Data, OutputDir, bRGB, bDepth, bMotion]() {
			FString AbsoluteOutputDir = OutputDir;
			if (FPaths::IsRelative(AbsoluteOutputDir))
			{
				AbsoluteOutputDir = FPaths::Combine(*FPaths::ProjectDir(), *OutputDir);
			}

			FString CameraPath = Data->CameraID.GetFullPath(AbsoluteOutputDir);

			if (!IFileManager::Get().DirectoryExists(*CameraPath))
			{
				IFileManager::Get().MakeDirectory(*CameraPath, true);
			}

			FString FrameNumberStr = FString::Printf(TEXT("%07lld"), Data->FrameNumber);

			// Write EXR
			FString ExrPath = FPaths::Combine(CameraPath, FString::Printf(TEXT("frame_%s.exr"), *FrameNumberStr));
			WriteEXRFile_Static(ExrPath, *Data, bRGB, bDepth, bMotion);

			// Write metadata JSON
			FString MetadataPath = FPaths::Combine(CameraPath, FString::Printf(TEXT("frame_%s.json"), *FrameNumberStr));
			WriteMetadataFile_Static(MetadataPath, *Data);
		});
}

bool UCameraCaptureSubsystem::WriteEXRFile_Static(const FString& FilePath, const FCaptureData& Data, bool bCaptureRGB, bool bCaptureDepth, bool bCaptureMotionVectors)
{
	// Safety checks
	if (Data.Width <= 0 || Data.Height <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Invalid image dimensions: %dx%d"), Data.Width, Data.Height);
		return false;
	}

	int32 NumPixels = Data.Width * Data.Height;

	if (Data.ImageData.Num() == 0 && Data.DepthData.Num() == 0 && Data.MotionVectorData.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] No image data to write"));
		return false;
	}

	// Convert FCaptureData format to TArray<FLinearColor> format expected by utility functions
	TArray<FLinearColor> RgbData;
	TArray<FLinearColor> DmvData;
	RgbData.SetNum(NumPixels);
	DmvData.SetNum(NumPixels);

	// Convert RGB from FColor to FLinearColor
	if (bCaptureRGB && Data.ImageData.Num() == NumPixels)
	{
		for (int32 i = 0; i < NumPixels; i++)
		{
			RgbData[i] = FLinearColor(Data.ImageData[i]);
		}
	}
	else
	{
		for (int32 i = 0; i < NumPixels; i++)
		{
			RgbData[i] = FLinearColor::Black;
		}
	}

	// Prepare DMV data: Depth in R, Motion X in G, Motion Y in B
	for (int32 i = 0; i < NumPixels; i++)
	{
		float Depth = (bCaptureDepth && Data.DepthData.Num() == NumPixels) ? Data.DepthData[i] : 0.0f;
		float MotionX = (bCaptureMotionVectors && Data.MotionVectorData.Num() == NumPixels) ? Data.MotionVectorData[i].X : 0.0f;
		float MotionY = (bCaptureMotionVectors && Data.MotionVectorData.Num() == NumPixels) ? Data.MotionVectorData[i].Y : 0.0f;

		DmvData[i] = FLinearColor(Depth, MotionX, MotionY, 0.0f);
	}

	// Use shared utility to write RGB+Depth EXR
	if (!CameraCaptureUtils::WriteEXRFile(FilePath, RgbData, DmvData, Data.Width, Data.Height, true))
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to write RGB+Depth EXR: %s"), *FilePath);
		return false;
	}

	// Write motion vectors to separate file if we have them
	if (bCaptureMotionVectors && Data.MotionVectorData.Num() == NumPixels)
	{
		FString MotionPath = FilePath.Replace(TEXT(".exr"), TEXT("_motion.exr"));
		if (!CameraCaptureUtils::WriteEXRFile(MotionPath, RgbData, DmvData, Data.Width, Data.Height, false))
		{
			UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Failed to write motion EXR: %s"), *MotionPath);
		}
	}

	return true;
}

bool UCameraCaptureSubsystem::WriteMetadataFile_Static(const FString& FilePath, const FCaptureData& Data)
{
	// Create JSON object
	TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();

	JsonObject->SetNumberField(TEXT("frame_number"), Data.FrameNumber);
	JsonObject->SetNumberField(TEXT("timestamp"), Data.Timestamp);
	JsonObject->SetStringField(TEXT("camera_id"), Data.CameraID.ToString());

	// World transform
	TSharedPtr<FJsonObject> TransformJson = MakeShared<FJsonObject>();
	FVector					Location = Data.WorldTransform.GetLocation();
	FRotator				Rotation = Data.WorldTransform.Rotator();
	FVector					Scale = Data.WorldTransform.GetScale3D();

	TArray<TSharedPtr<FJsonValue>> LocationArray;
	LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
	LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
	LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
	TransformJson->SetArrayField(TEXT("location"), LocationArray);

	TArray<TSharedPtr<FJsonValue>> RotationArray;
	RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
	RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
	RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
	TransformJson->SetArrayField(TEXT("rotation"), RotationArray);

	TArray<TSharedPtr<FJsonValue>> ScaleArray;
	ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
	ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
	ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
	TransformJson->SetArrayField(TEXT("scale"), ScaleArray);

	JsonObject->SetObjectField(TEXT("world_transform"), TransformJson);

	// Intrinsics
	TSharedPtr<FJsonObject> IntrinsicsJson = MakeShared<FJsonObject>();
	IntrinsicsJson->SetNumberField(TEXT("focal_length_x"), Data.Intrinsics.FocalLengthX);
	IntrinsicsJson->SetNumberField(TEXT("focal_length_y"), Data.Intrinsics.FocalLengthY);
	IntrinsicsJson->SetNumberField(TEXT("principal_point_x"), Data.Intrinsics.PrincipalPointX);
	IntrinsicsJson->SetNumberField(TEXT("principal_point_y"), Data.Intrinsics.PrincipalPointY);
	IntrinsicsJson->SetNumberField(TEXT("image_width"), Data.Intrinsics.ImageWidth);
	IntrinsicsJson->SetNumberField(TEXT("image_height"), Data.Intrinsics.ImageHeight);
	IntrinsicsJson->SetBoolField(TEXT("maintain_y_axis"), Data.Intrinsics.bMaintainYAxis);
	JsonObject->SetObjectField(TEXT("intrinsics"), IntrinsicsJson);

	JsonObject->SetStringField(TEXT("actor_path"), Data.ActorPath);
	JsonObject->SetStringField(TEXT("level_name"), Data.LevelName);

	// Serialize to string
	FString					  OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);

	// Write to file
	if (FFileHelper::SaveStringToFile(OutputString, *FilePath))
	{
		return true;
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to write metadata: %s"), *FilePath);
		return false;
	}
}

// ============================================================================
// Helper Functions
// ============================================================================

FCameraIdentifier UCameraCaptureSubsystem::GenerateCameraID(UIntrinsicSceneCaptureComponent2D* Camera)
{
	FCameraIdentifier ID = FCameraIdentifier::Generate(Camera);

	// Check if this exact combination of ActorName + ComponentName exists
	// Multiple cameras can be on the same actor, so only disambiguate if there's
	// an actual component name collision on the same actor
	FString ProposedUniqueID = FString::Printf(TEXT("%s::%s"), *ID.ActorName, *ID.ComponentName);

	// Check if this unique ID already exists
	bool bNeedsDisambiguation = false;
	for (const auto& Pair : CameraIDMap)
	{
		if (Pair.Value.UniqueID == ProposedUniqueID && Pair.Key.Get() != Camera)
		{
			// Same unique ID exists for a different camera - this is a true collision
			bNeedsDisambiguation = true;
			break;
		}
	}

	if (bNeedsDisambiguation)
	{
		// Disambiguate the actor name
		ID.ActorName = DisambiguateActorName(ID.ActorName);
		ID.UniqueID = FString::Printf(TEXT("%s::%s"), *ID.ActorName, *ID.ComponentName);
	}
	else
	{
		// No collision, use as-is
		ID.UniqueID = ProposedUniqueID;
	}

	return ID;
}

FString UCameraCaptureSubsystem::DisambiguateActorName(const FString& ActorName)
{
	// Find a unique suffix
	int32	Suffix = 1;
	FString DisambiguatedName;

	do
	{
		DisambiguatedName = FString::Printf(TEXT("%s_%d"), *ActorName, Suffix);
		Suffix++;

		// Check if this disambiguated name is already in use
		bool bInUse = false;
		for (const auto& Pair : CameraIDMap)
		{
			if (Pair.Value.ActorName == DisambiguatedName)
			{
				bInUse = true;
				break;
			}
		}

		if (!bInUse)
		{
			break;
		}
	}
	while (Suffix < 100); // Safety limit

	UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Actor/component name collision detected, using: %s"), *DisambiguatedName);

	return DisambiguatedName;
}
