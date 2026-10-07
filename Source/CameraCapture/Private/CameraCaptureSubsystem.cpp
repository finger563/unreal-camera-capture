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

	// Let anything already queued on the render thread finish first. The render
	// commands hold their readbacks by shared pointer so they cannot dangle, but
	// draining here means the staging buffers are actually released now rather
	// than whenever the last command happens to retire.
	FlushRenderingCommands();

	// And the pooled ones, so their GPU staging buffers go with the subsystem
	// rather than outliving it.
	ReadbackPool.Empty();

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
	if (IsSingleCaptureMode())
	{
		// The whole point of this mode is that there is no second camera and so
		// no second render. Depth comes out of the colour capture's alpha.
		if (RgbCamera && RgbCamera->HasSeparateDepthIntrinsics())
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[CameraCaptureSubsystem] %s has separate depth intrinsics, but single-capture mode takes both planes ")
					TEXT("from one render target and so one resolution; the depth intrinsics are ignored. Use ")
						TEXT("ColorPlusDepthMotion if the depth camera needs its own resolution."),
				*RgbCamera->GetName());
		}
		if (bCaptureMotionVectors)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[CameraCaptureSubsystem] Motion vectors were requested, but single-capture mode has no DMV pass to ")
					TEXT("produce them; none will be written."));
		}
		return;
	}

	if (!RgbCamera || !DmvCaptureMaterialBase)
	{
		return;
	}

	// Determine intrinsics for the depth camera — use separate depth intrinsics if configured,
	// otherwise fall back to the RGB camera's intrinsics
	FCameraIntrinsics DepthIntrinsics = RgbCamera->GetActiveDepthIntrinsics();
	int32			  Width = DepthIntrinsics.ImageWidth;
	int32			  Height = DepthIntrinsics.ImageHeight;

	// Create DMV camera using RGB camera's actual class so subclass-specific
	// properties/overrides are preserved on the copy
	FString							   DmvName = RgbCamera->GetName() + TEXT("_dmv");
	UIntrinsicSceneCaptureComponent2D* DmvCamera = NewObject<UIntrinsicSceneCaptureComponent2D>(
		RgbCamera->GetOwner(),
		RgbCamera->GetClass(),
		FName(*DmvName),
		RF_Transient,
		RgbCamera // Use RGB camera as template — copies all properties
	);

	if (!DmvCamera)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to create DMV camera for %s"), *RgbCamera->GetName());
		return;
	}

	// If the RGB camera has separate depth intrinsics, override the DMV camera's
	// regular intrinsics with them BEFORE RegisterComponent triggers BeginPlay/ApplyIntrinsics
	if (RgbCamera->HasSeparateDepthIntrinsics())
	{
		DmvCamera->bUseDepthIntrinsics = false; // DMV camera doesn't need depth intrinsics itself
		DmvCamera->bUseIntrinsicsAsset = RgbCamera->bUseDepthIntrinsicsAsset;
		DmvCamera->IntrinsicsAsset = RgbCamera->DepthIntrinsicsAsset;
		DmvCamera->InlineIntrinsics = RgbCamera->DepthInlineIntrinsics;

		UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] DMV camera '%s' using separate depth intrinsics (%dx%d)"),
			*DmvName, Width, Height);
	}

	DmvCamera->SetupAttachment(RgbCamera);

	// Apply depth sensor offset if configured, otherwise zero out relative transform
	if (RgbCamera->bUseDepthSensorOffset)
	{
		DmvCamera->SetRelativeLocation(RgbCamera->DepthSensorOffset.GetLocation());
		DmvCamera->SetRelativeRotation(RgbCamera->DepthSensorOffset.GetRotation().Rotator());
		DmvCamera->SetRelativeScale3D(RgbCamera->DepthSensorOffset.GetScale3D());

		UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] DMV camera '%s' using depth sensor offset: Loc=(%.2f, %.2f, %.2f) Rot=(%.2f, %.2f, %.2f)"),
			*DmvName,
			RgbCamera->DepthSensorOffset.GetLocation().X,
			RgbCamera->DepthSensorOffset.GetLocation().Y,
			RgbCamera->DepthSensorOffset.GetLocation().Z,
			RgbCamera->DepthSensorOffset.GetRotation().Rotator().Pitch,
			RgbCamera->DepthSensorOffset.GetRotation().Rotator().Yaw,
			RgbCamera->DepthSensorOffset.GetRotation().Rotator().Roll);
	}
	else
	{
		DmvCamera->SetRelativeLocation(FVector::ZeroVector);
		DmvCamera->SetRelativeRotation(FRotator::ZeroRotator);
	}

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
	// Uses depth intrinsics dimensions (may differ from RGB if separate depth intrinsics are set)
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

void UCameraCaptureSubsystem::SetCaptureMode(ERammsCaptureMode Mode)
{
	if (Mode == CaptureMode)
	{
		return;
	}
	CaptureMode = Mode;
	ReconfigureCamerasForCaptureMode();
}

void UCameraCaptureSubsystem::ReconfigureCamerasForCaptureMode()
{
	const bool bSingle = IsSingleCaptureMode();

	for (int32 i = RegisteredCameras.Num() - 1; i >= 0; --i)
	{
		UIntrinsicSceneCaptureComponent2D* Camera = RegisteredCameras[i].Get();
		if (!Camera)
		{
			continue;
		}

		if (bSingle)
		{
			// The second camera is the cost this mode exists to avoid, so it
			// goes rather than sitting idle holding an RGBA32f target.
			if (TWeakObjectPtr<USceneCaptureComponent2D>* DmvPtr = DmvCameras.Find(Camera))
			{
				if (USceneCaptureComponent2D* Dmv = DmvPtr->Get())
				{
					Dmv->DestroyComponent();
				}
			}
			DmvCameras.Remove(Camera);
			DmvRenderTargets.Remove(Camera);

			// Alpha has to hold a distance in centimetres. An 8-bit target
			// cannot, and keeping one would have produced depth quantised to
			// 256 steps of the full range instead of an obvious failure.
			UTextureRenderTarget2D* Existing = Camera->TextureTarget;
			if (Existing && Existing->RenderTargetFormat != RTF_RGBA32f && Existing->RenderTargetFormat != RTF_RGBA16f)
			{
				UE_LOG(LogTemp, Log,
					TEXT("[CameraCaptureSubsystem] %s has an 8-bit render target, which cannot carry depth in alpha; ")
						TEXT("recreating it as RGBA32f for single-capture mode"),
					*Camera->GetName());
				Camera->TextureTarget = nullptr;
			}
			else if (Existing && Existing->RenderTargetFormat == RTF_RGBA16f)
			{
				// Kept, not replaced. A half can hold a distance -- just coarsely,
				// about 8 cm of error at 100 m -- and the harvest accepts
				// PF_FloatRGBA for exactly that reason. Destroying a target
				// somebody configured by hand is the worse failure, so this says
				// what the cost is instead of silently charging it.
				UE_LOG(LogTemp, Warning,
					TEXT("[CameraCaptureSubsystem] %s has an RGBA16f render target; single-capture mode keeps it, but ")
						TEXT("half-float alpha carries roughly 8 cm of depth error at 100 m. Use RGBA32f, or let the ")
							TEXT("subsystem create the target, for full precision."),
					*Camera->GetName());
			}
		}
		else if (!DmvCameras.Contains(Camera) && (bCaptureDepth || bCaptureMotionVectors) && DmvCaptureMaterialBase)
		{
			// Depth and motion come from the DMV camera in this mode, and a
			// camera registered under single capture never got one.
			SetupDmvCamera(Camera);
		}

		// Format, capture source and size are all settled here, and a null
		// target left above is rebuilt.
		EnsureCameraRenderTarget(Camera);
	}

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Capture mode is now %s; reconfigured %d camera(s)"),
		bSingle ? TEXT("SingleCaptureColorDepth") : TEXT("ColorPlusDepthMotion"), RegisteredCameras.Num());
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
		// In single-capture mode this one readback carries BOTH planes, so it is
		// needed whenever either is wanted. Gated on bCaptureRGB alone, a
		// depth-only configuration in that mode kicked no readback at all (the
		// DMV branch below is skipped by design) and produced no frame.
		const bool bColorReadbackCarriesDepth = IsSingleCaptureMode() && bCaptureDepth;
		if ((bCaptureRGB || bColorReadbackCarriesDepth) && Camera->TextureTarget)
		{
			Camera->CaptureScene();

			UTextureRenderTarget2D* RgbRT = Camera->TextureTarget;
			Pending.RgbReadback.Width = RgbRT->SizeX;
			Pending.RgbReadback.Height = RgbRT->SizeY;
			Pending.RgbReadback.PixelFormat = GetPixelFormatFromRenderTargetFormat(RgbRT->RenderTargetFormat);
			// Single capture: this one readback carries both planes, so the
			// harvest splits depth out of alpha and there is no DMV readback.
			Pending.RgbReadback.bDepthInAlpha = IsSingleCaptureMode();
			if (IsSingleCaptureMode())
			{
				Pending.Metadata.DepthWidth = RgbRT->SizeX;
				Pending.Metadata.DepthHeight = RgbRT->SizeY;
			}
			Pending.Metadata.Width = RgbRT->SizeX;
			Pending.Metadata.Height = RgbRT->SizeY;

			EnqueueAsyncReadback(RgbRT, Pending.RgbReadback.Readback, Pending.RgbReadback.CopyIssued);
			Pending.bHasRgb = true;
		}

		// --- Kick DMV capture + enqueue async readback ---
		// Skipped entirely in single-capture mode: that is the render this mode
		// exists to avoid.
		if (!IsSingleCaptureMode() && (bCaptureDepth || bCaptureMotionVectors))
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
					// Read the format off the target rather than asserting it. The
					// DMV target this subsystem creates is RGBA32f, but a camera
					// can arrive with one somebody else made.
					Pending.DmvReadback.PixelFormat = GetPixelFormatFromRenderTargetFormat(DmvRT->RenderTargetFormat);

					// Depth keeps its own dimensions all the way through. With
					// separate depth intrinsics these differ from the colour ones,
					// and everything downstream needs to know which is which.
					Pending.Metadata.DepthWidth = DmvRT->SizeX;
					Pending.Metadata.DepthHeight = DmvRT->SizeY;

					EnqueueAsyncReadback(DmvRT, Pending.DmvReadback.Readback, Pending.DmvReadback.CopyIssued);
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

namespace
{
	/**
	 * Create an output directory at most once per path per session.
	 *
	 * The serializer used to stat and create the camera's directory on every
	 * frame -- two filesystem calls per camera per frame, for a path that
	 * changes only when a camera is added. Shared because serialization runs on
	 * background tasks that can overlap.
	 */
	void EnsureOutputDirectoryOnce(const FString& CameraPath)
	{
		static FCriticalSection Lock;
		static TSet<FString>	Created;
		{
			FScopeLock Guard(&Lock);
			if (Created.Contains(CameraPath))
			{
				return;
			}
		}

		if (!IFileManager::Get().DirectoryExists(*CameraPath))
		{
			IFileManager::Get().MakeDirectory(*CameraPath, true);
		}

		FScopeLock Guard(&Lock);
		Created.Add(CameraPath);
	}
} // namespace

TSharedPtr<FRHIGPUTextureReadback> UCameraCaptureSubsystem::AcquireReadback(const FReadbackShape& Shape)
{
	// Only a readback already shaped for this exact size and format: see
	// FReadbackShape. Anything else copies into a staging texture of the wrong
	// geometry, which the harvest rejects.
	if (TArray<TSharedPtr<FRHIGPUTextureReadback>>* Bucket = ReadbackPool.Find(Shape))
	{
		if (Bucket->Num() > 0)
		{
			return Bucket->Pop(EAllowShrinking::No);
		}
	}
	return MakeShared<FRHIGPUTextureReadback>(TEXT("CamCaptureReadback"));
}

void UCameraCaptureSubsystem::ReleaseReadback(TSharedPtr<FRHIGPUTextureReadback> Readback, const FReadbackShape& Shape)
{
	if (!Readback)
	{
		return;
	}
	// Only the game thread touches the pool, and only once the render command
	// that used this readback has finished with it.
	if (!Readback.IsUnique())
	{
		// Something still holds it; dropping our reference is the safe move.
		return;
	}
	// A readback with no recorded shape cannot be matched to a future texture
	// safely, so it is dropped rather than pooled under a guess.
	if (Shape.Width <= 0 || Shape.Height <= 0 || Shape.Format == PF_Unknown)
	{
		return;
	}
	// The cap is across all shapes: it exists to stop a burst keeping staging
	// textures alive for the session, and a per-bucket cap would miss that when
	// cameras come and go at many resolutions.
	if (CountPooledReadbacks() >= MaxPooledReadbacks)
	{
		return;
	}
	ReadbackPool.FindOrAdd(Shape).Add(MoveTemp(Readback));
}

int32 UCameraCaptureSubsystem::CountPooledReadbacks() const
{
	int32 Total = 0;
	for (const TPair<FReadbackShape, TArray<TSharedPtr<FRHIGPUTextureReadback>>>& Pair : ReadbackPool)
	{
		Total += Pair.Value.Num();
	}
	return Total;
}

void UCameraCaptureSubsystem::EnqueueAsyncReadback(UTextureRenderTarget2D* RenderTarget, TSharedPtr<FRHIGPUTextureReadback>& OutReadback,
	TSharedPtr<FThreadSafeBool>& OutCopyIssued)
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

	// Reused rather than allocated: each one owns a GPU staging texture, and
	// this used to allocate two per camera every capture frame. Reuse is keyed
	// by shape, because EnqueueCopy will NOT reshape an existing staging
	// texture -- see FReadbackShape.
	FReadbackShape Shape;
	Shape.Width = RenderTarget->SizeX;
	Shape.Height = RenderTarget->SizeY;
	Shape.Format = GetPixelFormatFromRenderTargetFormat(RenderTarget->RenderTargetFormat);
	OutReadback = AcquireReadback(Shape);

	// Captured by SHARED pointer, not raw. A raw one let the game thread destroy
	// the readback -- Deinitialize empties the pool, and PIE teardown does that
	// while copies are still queued -- so the render command then dispatched
	// through freed memory and landed on FRHIGPUMemoryReadback's unimplemented()
	// base, asserting on the render thread after the subsystem was already gone.
	TSharedPtr<FRHIGPUTextureReadback> Readback = OutReadback;
	FTextureRenderTargetResource*	   ResourcePtr = RTResource;

	// Fresh flag per enqueue: a pooled readback's fence is still signalled from
	// its last use, so "is the copy done" cannot be asked until the copy has been
	// issued. Without this the harvest read a previous frame's pixels.
	OutCopyIssued = MakeShared<FThreadSafeBool>(false);
	TSharedPtr<FThreadSafeBool> CopyIssued = OutCopyIssued;

	ENQUEUE_RENDER_COMMAND(CameraCaptureEnqueueReadback)
	(
		[Readback, ResourcePtr, CopyIssued](FRHICommandListImmediate& RHICmdList) {
			FRHITexture* Texture = ResourcePtr->GetRenderTargetTexture();
			if (Texture)
			{
				// The five-argument virtual, as the engine itself calls it
				// (Renderer/Private/SceneViewState.cpp), rather than the
				// two-argument convenience overload.
				Readback->EnqueueCopy(RHICmdList, Texture, FIntVector(0, 0, 0), 0, FIntVector(0, 0, 0));
				// EnqueueCopy cleared the fence, so from here IsReady() refers to
				// THIS copy rather than whatever the pooled object did last.
				*CopyIssued = true;
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
		const bool bRgbReady = !Pending.bHasRgb || !Pending.RgbReadback.Readback || Pending.RgbReadback.IsReadyForHarvest();
		const bool bDmvReady = !Pending.bHasDmv || !Pending.DmvReadback.Readback || Pending.DmvReadback.IsReadyForHarvest();

		if (bRgbReady && bDmvReady)
		{
			// The copy out of the staging buffer has to happen on the RENDER
			// thread: FRHIGPUTextureReadback::Lock goes through
			// FRHICommandListImmediate::Get(), which checks IsInRenderingThread().
			// Doing it here, in Tick, asserted and took the editor down on the
			// very first harvested frame.
			//
			// Everything the render command touches is moved out of
			// PendingCaptures first and held by shared pointer, because the array
			// is about to be mutated and a reference into it would dangle.
			TSharedRef<FCaptureData>	 DataRef = MakeShared<FCaptureData>(MoveTemp(Pending.Metadata));
			TSharedPtr<FPendingReadback> RgbRb;
			TSharedPtr<FPendingReadback> DmvRb;
			if (Pending.bHasRgb && Pending.RgbReadback.Readback)
			{
				RgbRb = MakeShared<FPendingReadback>(MoveTemp(Pending.RgbReadback));
			}
			if (Pending.bHasDmv && Pending.DmvReadback.Readback)
			{
				DmvRb = MakeShared<FPendingReadback>(MoveTemp(Pending.DmvReadback));
			}
			PendingCaptures.RemoveAt(i);

			// Weak: the harvest outlives this tick, and the world (with this
			// subsystem) can go away while a copy is still in flight.
			TWeakObjectPtr<UCameraCaptureSubsystem> WeakThis(this);

			ENQUEUE_RENDER_COMMAND(CameraCaptureHarvestReadbacks)
			(
				[WeakThis, DataRef, RgbRb, DmvRb](FRHICommandListImmediate& RHICmdList) {
					if (RgbRb.IsValid())
					{
						HarvestRgbReadback(*RgbRb, *DataRef);
					}
					if (DmvRb.IsValid())
					{
						HarvestDmvReadback(*DmvRb, *DataRef);
					}

					// Back to the game thread to publish: listeners expect it,
					// and the readback pool is not synchronised.
					AsyncTask(ENamedThreads::GameThread, [WeakThis, DataRef, RgbRb, DmvRb]() {
						UCameraCaptureSubsystem* Self = WeakThis.Get();
						if (!Self)
						{
							return;
						}

						Self->OnFrameCaptured.Broadcast(DataRef);

						if (Self->bSerializationEnabled)
						{
							Self->SerializeCaptureData(DataRef);
						}

						Self->TotalFramesCaptured++;

						// MOVED, not copied. ReleaseReadback only pools a readback
						// it is the sole owner of, and passing the member by value
						// left the member itself as a second owner -- so
						// IsUnique() was false every time and the pool this
						// branch exists for stayed permanently empty.
						//
						// The shape goes with it: a readback can only be handed
						// to a texture of the same size and format.
						if (RgbRb.IsValid())
						{
							Self->ReleaseReadback(MoveTemp(RgbRb->Readback), RgbRb->GetShape());
						}
						if (DmvRb.IsValid())
						{
							Self->ReleaseReadback(MoveTemp(DmvRb->Readback), DmvRb->GetShape());
						}
					});
				});
		}
		else if (Pending.FramesWaiting > MaxReadbackWaitFrames)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Dropping capture for %s (readback timed out after %d frames)"),
				*Pending.Metadata.CameraID.ToString(), Pending.FramesWaiting);
			// A timed-out readback is not known to be finished on the GPU, so it
			// is destroyed rather than pooled -- reusing it could enqueue a copy
			// into a buffer still being written.
			PendingCaptures.RemoveAt(i);
		}
	}
}

/**
 * Check a readback against the staging texture it actually owns, before anyone
 * strides through it.
 *
 * The pitch/height guard in each harvest works in PIXELS, so it cannot see a
 * FORMAT disagreement -- and a format disagreement is the dangerous one. A
 * readback whose staging texture was created for an RGBA8 target is 4 bytes per
 * pixel; harvested as RGBA32f it is read at 16, which walks four times the
 * buffer and takes the render thread down. That is not hypothetical: it is what
 * a pool that ignored format produced, in HarvestDmvReadback, via a stack
 * through ExecuteCommand.
 *
 * FRHIGPUTextureReadback exposes its staging textures, so the real descriptor is
 * available and worth asking rather than inferring. Keyed pooling should make
 * this unreachable; it is here because the consequence of being wrong is a heap
 * overrun rather than a bad frame.
 */
bool UCameraCaptureSubsystem::ReadbackMatchesItsStagingTexture(const FPendingReadback& Readback, const TCHAR* Label)
{
	check(IsInRenderingThread());
	if (!Readback.Readback.IsValid())
	{
		return false;
	}

	const FRHITexture* Staging = Readback.Readback->DestinationStagingTextures[0].GetReference();
	if (!Staging)
	{
		// Nothing allocated yet means no copy has landed; the caller's readiness
		// check should have caught it, so say so rather than reading anyway.
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] %s readback has no staging texture; skipping"), Label);
		return false;
	}

	const FRHITextureDesc& Desc = Staging->GetDesc();
	if (Desc.Format != Readback.PixelFormat)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[CameraCaptureSubsystem] %s readback staging texture is format %d but the harvest expects %d; skipping ")
				TEXT("rather than reading it at the wrong stride"),
			Label, static_cast<int32>(Desc.Format), static_cast<int32>(Readback.PixelFormat));
		return false;
	}
	if (Desc.Extent.X < Readback.Width || Desc.Extent.Y < Readback.Height)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[CameraCaptureSubsystem] %s readback staging texture is %dx%d but %dx%d was requested; skipping"),
			Label, Desc.Extent.X, Desc.Extent.Y, Readback.Width, Readback.Height);
		return false;
	}
	return true;
}

void UCameraCaptureSubsystem::HarvestRgbReadback(FPendingReadback& Readback, FCaptureData& OutData)
{
	check(IsInRenderingThread());
	// A readback is raw GPU memory with no self-describing format, so refuse to
	// interpret one we do not recognise rather than guessing a stride and walking
	// off the end of the staging buffer.
	const EPixelFormat Format = Readback.PixelFormat;
	if (Format != PF_B8G8R8A8 && Format != PF_R8G8B8A8 && Format != PF_A32B32G32R32F && Format != PF_FloatRGBA)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[CameraCaptureSubsystem] RGB readback has unsupported pixel format %d; skipping rather than mis-reading it. ")
				TEXT("Use an RGBA8, RGBA16f or RGBA32f render target."),
			static_cast<int32>(Format));
		return;
	}

	if (!ReadbackMatchesItsStagingTexture(Readback, TEXT("RGB")))
	{
		return;
	}

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

	// The staging buffer is only guaranteed to hold what the GPU actually copied.
	// If it is shorter than the rows we are about to walk, stop: this is the last
	// place we can catch a size disagreement before it becomes a wild read.
	if (Width <= 0 || Height <= 0 || RowPitchInPixels < Width || (BufferHeight > 0 && BufferHeight < Height))
	{
		UE_LOG(LogTemp, Error,
			TEXT("[CameraCaptureSubsystem] RGB readback geometry does not hold the expected image: %dx%d requested, pitch %d, buffer height %d"),
			Width, Height, RowPitchInPixels, BufferHeight);
		Readback.Readback->Unlock();
		return;
	}

	const int32 NumPixels = Width * Height;
	OutData.ImageData.SetNumUninitialized(NumPixels);
	FColor* RESTRICT Dst = OutData.ImageData.GetData();

	// Single-capture mode: alpha is scene depth in centimetres, straight from
	// the engine's SCS_SceneColorSceneDepth pass, so this one readback fills
	// both planes and no DMV render happened at all.
	float* RESTRICT DepthDst = nullptr;
	if (Readback.bDepthInAlpha)
	{
		OutData.DepthData.SetNumUninitialized(NumPixels);
		OutData.DepthWidth = Width;
		OutData.DepthHeight = Height;
		DepthDst = OutData.DepthData.GetData();
	}

	switch (Format)
	{
		case PF_A32B32G32R32F:
		{
			// 32-bit float per channel: 16 bytes/pixel.
			const FLinearColor* SrcRow = static_cast<const FLinearColor*>(SrcData);
			for (int32 y = 0; y < Height; y++)
			{
				for (int32 x = 0; x < Width; x++)
				{
					Dst[x] = SrcRow[x].ToFColor(true);
				}
				if (DepthDst)
				{
					for (int32 x = 0; x < Width; x++)
					{
						DepthDst[x] = SrcRow[x].A;
					}
					DepthDst += Width;
				}
				Dst += Width;
				SrcRow += RowPitchInPixels;
			}
			break;
		}
		case PF_FloatRGBA:
		{
			// 16-bit half per channel: 8 bytes/pixel. Reading this as FLinearColor
			// is the bug this switch exists to prevent -- it covered twice the
			// bytes the buffer held.
			const FFloat16Color* SrcRow = static_cast<const FFloat16Color*>(SrcData);
			for (int32 y = 0; y < Height; y++)
			{
				for (int32 x = 0; x < Width; x++)
				{
					Dst[x] = FLinearColor(SrcRow[x]).ToFColor(true);
				}
				if (DepthDst)
				{
					for (int32 x = 0; x < Width; x++)
					{
						DepthDst[x] = SrcRow[x].A.GetFloat();
					}
					DepthDst += Width;
				}
				Dst += Width;
				SrcRow += RowPitchInPixels;
			}
			break;
		}
		default:
		{
			// BGRA8 / RGBA8: 4 bytes/pixel, already FColor-shaped. An 8-bit
			// target cannot carry a distance, so depth-in-alpha is impossible
			// here -- EnsureCameraRenderTarget makes the target float in that
			// mode, and a caller who supplied their own 8-bit one gets told.
			if (DepthDst)
			{
				UE_LOG(LogTemp, Error,
					TEXT("[CameraCaptureSubsystem] Single-capture mode needs a float render target; this one is 8-bit, ")
						TEXT("so its alpha cannot hold depth in centimetres. No depth written."));
				OutData.DepthData.Reset();
				OutData.DepthWidth = 0;
				OutData.DepthHeight = 0;
				DepthDst = nullptr;
			}
			const FColor* SrcRow = static_cast<const FColor*>(SrcData);
			if (Width == RowPitchInPixels)
			{
				FMemory::Memcpy(Dst, SrcRow, static_cast<SIZE_T>(Width) * Height * sizeof(FColor));
			}
			else
			{
				for (int32 y = 0; y < Height; y++)
				{
					FMemory::Memcpy(Dst, SrcRow, Width * sizeof(FColor));
					Dst += Width;
					SrcRow += RowPitchInPixels;
				}
			}
			break;
		}
	}

	Readback.Readback->Unlock();
}

void UCameraCaptureSubsystem::HarvestDmvReadback(FPendingReadback& Readback, FCaptureData& OutData)
{
	check(IsInRenderingThread());
	// Depth carries real distances, so a half-float target would quietly cap
	// precision; but refusing outright would break a working setup, so take both
	// float formats and reject only what we cannot read at all.
	const EPixelFormat Format = Readback.PixelFormat;
	if (Format != PF_A32B32G32R32F && Format != PF_FloatRGBA)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[CameraCaptureSubsystem] DMV readback has pixel format %d, which cannot carry float depth; skipping. ")
				TEXT("Use an RGBA32f render target for depth/motion."),
			static_cast<int32>(Format));
		return;
	}

	if (!ReadbackMatchesItsStagingTexture(Readback, TEXT("DMV")))
	{
		return;
	}

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

	if (Width <= 0 || Height <= 0 || RowPitchInPixels < Width || (BufferHeight > 0 && BufferHeight < Height))
	{
		UE_LOG(LogTemp, Error,
			TEXT("[CameraCaptureSubsystem] DMV readback geometry does not hold the expected image: %dx%d requested, pitch %d, buffer height %d"),
			Width, Height, RowPitchInPixels, BufferHeight);
		Readback.Readback->Unlock();
		return;
	}

	const int32 NumPixels = Width * Height;
	OutData.DepthData.SetNumUninitialized(NumPixels);
	OutData.MotionVectorData.SetNumUninitialized(NumPixels);

	// These are the dimensions the depth arrays are actually in. Set them here as
	// well as at kick time so a harvest is self-consistent even if the target was
	// swapped underneath us between kick and resolve.
	OutData.DepthWidth = Width;
	OutData.DepthHeight = Height;

	float* RESTRICT		DepthDst = OutData.DepthData.GetData();
	FVector2D* RESTRICT MotionDst = OutData.MotionVectorData.GetData();

	// DMV layout: R=Depth, G=MotionX, B=MotionY, A=1
	if (Format == PF_A32B32G32R32F)
	{
		const FLinearColor* SrcRow = static_cast<const FLinearColor*>(SrcData);
		for (int32 y = 0; y < Height; y++)
		{
			for (int32 x = 0; x < Width; x++)
			{
				const FLinearColor& Pixel = SrcRow[x];
				DepthDst[x] = Pixel.R;
				MotionDst[x] = FVector2D(Pixel.G, Pixel.B);
			}
			DepthDst += Width;
			MotionDst += Width;
			SrcRow += RowPitchInPixels;
		}
	}
	else
	{
		const FFloat16Color* SrcRow = static_cast<const FFloat16Color*>(SrcData);
		for (int32 y = 0; y < Height; y++)
		{
			for (int32 x = 0; x < Width; x++)
			{
				const FFloat16Color& Pixel = SrcRow[x];
				DepthDst[x] = Pixel.R.GetFloat();
				MotionDst[x] = FVector2D(Pixel.G.GetFloat(), Pixel.B.GetFloat());
			}
			DepthDst += Width;
			MotionDst += Width;
			SrcRow += RowPitchInPixels;
		}
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

	// Compute transform relative to the owning actor's root (not just the immediate parent)
	if (AActor* Owner = Camera->GetOwner())
	{
		Data.RelativeTransform = Data.WorldTransform.GetRelativeTransform(Owner->GetActorTransform());
	}
	else
	{
		Data.RelativeTransform = Camera->GetRelativeTransform();
	}

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

UTextureRenderTarget2D* UCameraCaptureSubsystem::GetDepthRenderTarget(UIntrinsicSceneCaptureComponent2D* Camera) const
{
	if (!Camera)
	{
		return nullptr;
	}
	if (const TWeakObjectPtr<UTextureRenderTarget2D>* Found = DmvRenderTargets.Find(Camera))
	{
		return Found->Get();
	}
	return nullptr;
}

void UCameraCaptureSubsystem::EnsureCameraRenderTarget(UIntrinsicSceneCaptureComponent2D* Camera)
{
	FCameraIntrinsics Intrinsics = Camera->GetActiveIntrinsics();
	int32			  Width = Intrinsics.ImageWidth;
	int32			  Height = Intrinsics.ImageHeight;

	if (Width < 1 || Height < 1)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Camera %s has invalid intrinsic dimensions %dx%d; leaving its render target alone"),
			*Camera->GetName(), Width, Height);
		return;
	}

	// Capture source first, before the early return below: a camera that arrived
	// with its own render target still needs this, and that is exactly the camera
	// somebody has configured by hand and will be looking at.
	//
	// Single capture takes colour and depth from one pass. Otherwise take the
	// finished image: left alone the component sits at the engine's
	// SCS_SceneColorHDR, which is pre-tonemap linear HDR squeezed into an 8-bit
	// target -- see ColorCaptureSource.
	Camera->CaptureSource = IsSingleCaptureMode() ? TEnumAsByte<ESceneCaptureSource>(SCS_SceneColorSceneDepth)
												  : ColorCaptureSource;

	if (Camera->TextureTarget)
	{
		// An existing target is kept, but not blindly: this used to return as soon
		// as one existed, so changing the intrinsics at runtime left the target at
		// its old size while the metadata reported the new one. The image then
		// failed its own pixel-count check downstream and was written as black.
		UTextureRenderTarget2D* Existing = Camera->TextureTarget;
		if (Existing->SizeX != Width || Existing->SizeY != Height)
		{
			UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Resizing render target for %s from %dx%d to %dx%d to match its intrinsics"),
				*Camera->GetName(), Existing->SizeX, Existing->SizeY, Width, Height);
			Existing->ResizeTarget(Width, Height);
			Existing->UpdateResourceImmediate(true);
		}
		return;
	}

	UTextureRenderTarget2D* NewRenderTarget = NewObject<UTextureRenderTarget2D>(Camera);
	if (IsSingleCaptureMode())
	{
		// Alpha has to hold a distance in centimetres, so the target cannot be
		// 8-bit. Full float rather than half: at 100 m a half carries about 8 cm
		// of error, which is not a depth measurement.
		NewRenderTarget->RenderTargetFormat = RTF_RGBA32f;
	}
	else
	{
		NewRenderTarget->RenderTargetFormat = RTF_RGBA8; // RGB only needs 8-bit
	}
	NewRenderTarget->InitAutoFormat(Width, Height);
	NewRenderTarget->UpdateResourceImmediate(true);

	Camera->TextureTarget = NewRenderTarget;

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Created %s render target (%dx%d) for camera %s"),
		IsSingleCaptureMode() ? TEXT("RGBA32f colour+depth") : TEXT("RGBA8"),
		Width, Height, *Camera->GetName());
}

// ============================================================================
// Serialization (dispatched to background thread)
// ============================================================================

void UCameraCaptureSubsystem::SerializeCaptureData(TSharedRef<const FCaptureData> Data)
{
	FString						   OutputDir = OutputDirectory;
	bool						   bRGB = bCaptureRGB;
	bool						   bDepth = bCaptureDepth;
	bool						   bMotion = bCaptureMotionVectors;
	const ERammsCaptureColorFormat Format = ColorFormat;

	// Lambda captures the shared ref — keeps data alive until async write completes
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
		[Data, OutputDir, bRGB, bDepth, bMotion, Format]() {
			FString AbsoluteOutputDir = OutputDir;
			if (FPaths::IsRelative(AbsoluteOutputDir))
			{
				AbsoluteOutputDir = FPaths::Combine(*FPaths::ProjectDir(), *OutputDir);
			}

			FString CameraPath = Data->CameraID.GetFullPath(AbsoluteOutputDir);

			EnsureOutputDirectoryOnce(CameraPath);

			FString FrameNumberStr = FString::Printf(TEXT("%07lld"), Data->FrameNumber);

			// Write EXR
			FString ExrPath = FPaths::Combine(CameraPath, FString::Printf(TEXT("frame_%s.exr"), *FrameNumberStr));
			WriteEXRFile_Static(ExrPath, *Data, bRGB, bDepth, bMotion, Format);

			// Write metadata JSON
			FString MetadataPath = FPaths::Combine(CameraPath, FString::Printf(TEXT("frame_%s.json"), *FrameNumberStr));
			WriteMetadataFile_Static(MetadataPath, *Data, Format, bDepth);
		});
}

bool UCameraCaptureSubsystem::WriteEXRFile_Static(const FString& FilePath, const FCaptureData& Data, bool bCaptureRGB, bool bCaptureDepth, bool bCaptureMotionVectors,
	ERammsCaptureColorFormat Format)
{
	// Safety checks
	if (Data.Width <= 0 || Data.Height <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Invalid image dimensions: %dx%d"), Data.Width, Data.Height);
		return false;
	}

	const int32 NumPixels = Data.Width * Data.Height;

	// Depth and motion are in their own resolution when the camera has separate
	// depth intrinsics. Older FCaptureData (and any built by hand) leaves the
	// depth dimensions at zero, which means "same as colour".
	const int32 DepthWidth = Data.DepthWidth > 0 ? Data.DepthWidth : Data.Width;
	const int32 DepthHeight = Data.DepthHeight > 0 ? Data.DepthHeight : Data.Height;
	const int32 DepthPixels = DepthWidth * DepthHeight;

	if (Data.ImageData.Num() == 0 && Data.DepthData.Num() == 0 && Data.MotionVectorData.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] No image data to write"));
		return false;
	}

	// Each plane is validated against its OWN geometry. This used to compare
	// depth against the colour pixel count, so a camera with separate depth
	// intrinsics failed the check every frame and wrote depth as all zeros --
	// the feature looked like it worked and silently produced nothing.
	const bool bHaveRgb = bCaptureRGB && Data.ImageData.Num() == NumPixels;
	const bool bHaveDepth = bCaptureDepth && Data.DepthData.Num() == DepthPixels;
	const bool bHaveMotion = bCaptureMotionVectors && Data.MotionVectorData.Num() == DepthPixels;

	if (bCaptureRGB && !bHaveRgb && Data.ImageData.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] RGB plane is %d values, expected %dx%d; writing black"),
			Data.ImageData.Num(), Data.Width, Data.Height);
	}
	if (bCaptureDepth && !bHaveDepth && Data.DepthData.Num() > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Depth plane is %d values, expected %dx%d; writing zero depth"),
			Data.DepthData.Num(), DepthWidth, DepthHeight);
	}

	// Depth goes into the combined file's alpha channel, which has to be on the
	// colour grid. Resample when the two differ -- nearest neighbour, so the
	// alpha channel only ever holds distances the scene really had.
	// Only the combined layout needs this: SeparatePNGAndEXR writes depth at the
	// resolution it was measured at, so resampling for it allocated and filled a
	// full colour-resolution array per frame and then threw it away.
	const bool	  bCombined = Format != ERammsCaptureColorFormat::SeparatePNGAndEXR;
	TArray<float> DepthOnColourGrid;
	if (bHaveDepth && bCombined)
	{
		DepthOnColourGrid = CameraCaptureUtils::ResampleDepthNearest(
			Data.DepthData, DepthWidth, DepthHeight, Data.Width, Data.Height);
		if (DepthOnColourGrid.Num() != NumPixels)
		{
			UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Could not resample depth %dx%d onto colour %dx%d"),
				DepthWidth, DepthHeight, Data.Width, Data.Height);
			DepthOnColourGrid.Reset();
		}
	}
	const bool bDepthInAlpha = DepthOnColourGrid.Num() == NumPixels;

	if (Format == ERammsCaptureColorFormat::SeparatePNGAndEXR)
	{
		// Colour at the depth it was captured at. The combined path below has to
		// promote it to float because EXR carries the depth channel alongside;
		// written on its own it stays 8-bit and compresses.
		if (bHaveRgb)
		{
			TArray64<FColor> ColorPixels;
			ColorPixels.SetNumUninitialized(NumPixels);
			FMemory::Memcpy(ColorPixels.GetData(), Data.ImageData.GetData(), static_cast<SIZE_T>(NumPixels) * sizeof(FColor));

			const FString ColorPath = FilePath.Replace(TEXT(".exr"), TEXT(".png"));
			if (!CameraCaptureUtils::WritePNGPixels(ColorPath, MoveTemp(ColorPixels), Data.Width, Data.Height))
			{
				UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to write colour PNG: %s"), *ColorPath);
			}
		}

		// Depth keeps its own grid; nothing is resampled, because there is no
		// shared file forcing the two onto one raster.
		if (bHaveDepth)
		{
			TArray64<FLinearColor> DepthOut;
			DepthOut.SetNumUninitialized(DepthPixels);
			FLinearColor* RESTRICT Dst = DepthOut.GetData();
			const float* RESTRICT  Src = Data.DepthData.GetData();
			for (int32 i = 0; i < DepthPixels; i++)
			{
				Dst[i] = FLinearColor(Src[i], 0.0f, 0.0f, Src[i]);
			}

			const FString DepthPath = FilePath.Replace(TEXT(".exr"), TEXT("_depth.exr"));
			if (!CameraCaptureUtils::WriteEXRPixels(DepthPath, MoveTemp(DepthOut), DepthWidth, DepthHeight))
			{
				UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to write depth EXR: %s"), *DepthPath);
				return false;
			}
		}

		if (bHaveMotion)
		{
			TArray64<FLinearColor> MotionPixels;
			MotionPixels.SetNumUninitialized(DepthPixels);
			FLinearColor* RESTRICT	  MotionOut = MotionPixels.GetData();
			const FVector2D* RESTRICT SrcMotion = Data.MotionVectorData.GetData();
			for (int32 i = 0; i < DepthPixels; i++)
			{
				MotionOut[i] = FLinearColor(static_cast<float>(SrcMotion[i].X), static_cast<float>(SrcMotion[i].Y), 0.0f, 0.0f);
			}
			const FString MotionPath = FilePath.Replace(TEXT(".exr"), TEXT("_motion.exr"));
			CameraCaptureUtils::WriteEXRPixels(MotionPath, MoveTemp(MotionPixels), DepthWidth, DepthHeight);
		}

		return true;
	}

	// One pass, one buffer. The previous version built two full FLinearColor
	// arrays here and WriteEXRFile built a third, so every frame allocated and
	// filled 48 bytes per pixel to emit 16.
	TArray64<FLinearColor> Pixels;
	Pixels.SetNumUninitialized(NumPixels);
	FLinearColor* RESTRICT Out = Pixels.GetData();

	const FColor* RESTRICT SrcRgb = bHaveRgb ? Data.ImageData.GetData() : nullptr;
	const float* RESTRICT  SrcDepth = bDepthInAlpha ? DepthOnColourGrid.GetData() : nullptr;

	for (int32 i = 0; i < NumPixels; i++)
	{
		if (SrcRgb)
		{
			const FColor& C = SrcRgb[i];
			// FLinearColor(FColor) applies sRGB decode; the capture path already
			// encoded to sRGB on the way out of the readback, so this is the
			// inverse and matches what the old code did via the same constructor.
			Out[i] = FLinearColor(C);
		}
		else
		{
			Out[i] = FLinearColor::Black;
		}
		Out[i].A = SrcDepth ? SrcDepth[i] : 0.0f;
	}

	if (!CameraCaptureUtils::WriteEXRPixels(FilePath, MoveTemp(Pixels), Data.Width, Data.Height))
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to write RGB+Depth EXR: %s"), *FilePath);
		return false;
	}

	// Motion vectors go out at their own resolution, which is the depth grid --
	// not the colour one. Writing them on the colour grid was the same bug in the
	// other direction: it either refused to write or wrote resampled vectors
	// whose magnitudes no longer matched the pixels they were measured in.
	if (bHaveMotion)
	{
		TArray64<FLinearColor> MotionPixels;
		MotionPixels.SetNumUninitialized(DepthPixels);
		FLinearColor* RESTRICT	  MotionOut = MotionPixels.GetData();
		const FVector2D* RESTRICT SrcMotion = Data.MotionVectorData.GetData();
		for (int32 i = 0; i < DepthPixels; i++)
		{
			MotionOut[i] = FLinearColor(static_cast<float>(SrcMotion[i].X), static_cast<float>(SrcMotion[i].Y), 0.0f, 0.0f);
		}

		const FString MotionPath = FilePath.Replace(TEXT(".exr"), TEXT("_motion.exr"));
		if (!CameraCaptureUtils::WriteEXRPixels(MotionPath, MoveTemp(MotionPixels), DepthWidth, DepthHeight))
		{
			UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Failed to write motion EXR: %s"), *MotionPath);
		}
	}

	// When depth is not on the colour grid, the alpha channel above is a
	// resampled copy. Emit the measured depth at its own resolution too, so the
	// native data is never only available through a resample.
	if (bHaveDepth && (DepthWidth != Data.Width || DepthHeight != Data.Height))
	{
		TArray64<FLinearColor> DepthPixelsOut;
		DepthPixelsOut.SetNumUninitialized(DepthPixels);
		FLinearColor* RESTRICT DepthOut = DepthPixelsOut.GetData();
		const float* RESTRICT  SrcNative = Data.DepthData.GetData();
		for (int32 i = 0; i < DepthPixels; i++)
		{
			DepthOut[i] = FLinearColor(SrcNative[i], 0.0f, 0.0f, SrcNative[i]);
		}

		const FString DepthPath = FilePath.Replace(TEXT(".exr"), TEXT("_depth.exr"));
		if (!CameraCaptureUtils::WriteEXRPixels(DepthPath, MoveTemp(DepthPixelsOut), DepthWidth, DepthHeight))
		{
			UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Failed to write native-resolution depth EXR: %s"), *DepthPath);
		}
	}

	return true;
}

bool UCameraCaptureSubsystem::WriteMetadataFile_Static(const FString& FilePath, const FCaptureData& Data,
	ERammsCaptureColorFormat Format, bool bCaptureDepth)
{
	// Create JSON object
	TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();

	JsonObject->SetNumberField(TEXT("frame_number"), Data.FrameNumber);
	JsonObject->SetNumberField(TEXT("timestamp"), Data.Timestamp);
	JsonObject->SetStringField(TEXT("camera_id"), Data.CameraID.ToString());

	// Transforms
	JsonObject->SetObjectField(TEXT("world_transform"),
		CameraCaptureUtils::TransformToJsonObject(Data.WorldTransform));
	JsonObject->SetObjectField(TEXT("relative_transform"),
		CameraCaptureUtils::TransformToJsonObject(Data.RelativeTransform));

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

	// The grid each plane is actually in. A reader cannot infer the depth size
	// from the intrinsics above, because those are the colour intrinsics, and a
	// camera with separate depth calibration captures depth at its own size.
	JsonObject->SetNumberField(TEXT("color_width"), Data.Width);
	JsonObject->SetNumberField(TEXT("color_height"), Data.Height);

	// Which files this frame produced, so a reader does not have to guess
	// whether colour is in the EXR's RGB channels or beside it as a PNG.
	const bool bSeparate = Format == ERammsCaptureColorFormat::SeparatePNGAndEXR;

	if (Data.DepthWidth > 0 && Data.DepthHeight > 0)
	{
		JsonObject->SetNumberField(TEXT("depth_width"), Data.DepthWidth);
		JsonObject->SetNumberField(TEXT("depth_height"), Data.DepthHeight);
		// Tells a reader whether the alpha channel of frame_N.exr is measured
		// depth or a nearest-neighbour resample of it, and that the measured
		// values are in frame_N_depth.exr when it is the latter.
		//
		// Derived from what was WRITTEN, not from the geometry. Geometry alone
		// claimed resampled alpha for the separate layout, which has no combined
		// EXR to put it in, and for captures with depth switched off, whose alpha
		// is zero -- the same conditions WriteEXRFile_Static uses to decide
		// whether depth reaches alpha at all.
		const int32 DepthPixels = Data.DepthWidth * Data.DepthHeight;
		const bool	bDepthWritten = bCaptureDepth && Data.DepthData.Num() == DepthPixels;
		JsonObject->SetBoolField(TEXT("depth_resampled_into_alpha"),
			!bSeparate && bDepthWritten && Data.HasMismatchedDepthResolution());
	}
	JsonObject->SetStringField(TEXT("color_format"), bSeparate ? TEXT("png") : TEXT("exr"));
	JsonObject->SetStringField(TEXT("layout"), bSeparate ? TEXT("separate") : TEXT("combined"));

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
