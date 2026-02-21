#include "CameraCaptureSubsystem.h"
#include "IntrinsicSceneCaptureComponent2D.h"
#include "Utilities.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
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
	// This is the same path used by CaptureComponent
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

	// Process any pending deferred captures
	if (CamerasAwaitingRGB.Num() > 0 || CamerasAwaitingDMV.Num() > 0)
	{
		ProcessDeferredCaptures();
	}

	// Safety check - only tick if initialized and capturing
	if (!IsInitialized() || !bIsCapturing)
	{
		return;
	}

	CurrentFrameCounter++;

	// Check if we should capture this frame
	if (CurrentFrameCounter % CaptureEveryNFrames == 0)
	{
		ExecuteSynchronizedCapture();
	}
}

TStatId UCameraCaptureSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCameraCaptureSubsystem, STATGROUP_Tickables);
}

bool UCameraCaptureSubsystem::IsTickable() const
{
	// Tick if we have pending captures or are actively capturing
	return IsInitialized() && (bIsCapturing || bHasPendingCaptures) && !IsTemplate();
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

	// Create new DMV camera component attached to the RGB camera
	FString					  DmvName = RgbCamera->GetName() + TEXT("_dmv");
	USceneCaptureComponent2D* DmvCamera = NewObject<USceneCaptureComponent2D>(
		RgbCamera->GetOwner(),
		USceneCaptureComponent2D::StaticClass(),
		FName(*DmvName),
		RF_Transient,
		RgbCamera // Use RGB camera as template
	);

	if (!DmvCamera)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to create DMV camera for %s"), *RgbCamera->GetName());
		return;
	}

	// Attach to RGB camera so it follows its transform
	FAttachmentTransformRules AttachRules(EAttachmentRule::KeepRelative, true);
	DmvCamera->AttachToComponent(RgbCamera, AttachRules);
	DmvCamera->SetRelativeLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator);

	// Configure DMV camera (same as CaptureComponent.cpp ConfigureDmvCamera)
	DmvCamera->bCaptureEveryFrame = false;
	DmvCamera->bCaptureOnMovement = false;
	DmvCamera->bAlwaysPersistRenderingState = true;
	DmvCamera->CaptureSource = SCS_FinalColorLDR;

	// Copy custom projection matrix if RGB camera uses one
	if (RgbCamera->bUseCustomProjectionMatrix)
	{
		DmvCamera->bUseCustomProjectionMatrix = true;
		DmvCamera->CustomProjectionMatrix = RgbCamera->CustomProjectionMatrix;
	}

	// Create dynamic material instance
	UMaterialInstanceDynamic* DmvMaterial = UMaterialInstanceDynamic::Create(DmvCaptureMaterialBase, this);
	if (DmvMaterial)
	{
		DmvCamera->PostProcessSettings.WeightedBlendables.Array.Empty();
		DmvCamera->PostProcessSettings.WeightedBlendables.Array.Add(FWeightedBlendable(1.0f, DmvMaterial));
		UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Created DMV material instance for %s"), *RgbCamera->GetName());
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to create DMV material instance for %s"), *RgbCamera->GetName());
		return;
	}

	// Create render target for DMV
	UTextureRenderTarget2D* DmvRT = NewObject<UTextureRenderTarget2D>(this);
	DmvRT->RenderTargetFormat = RTF_RGBA32f; // Float format for depth+motion
	DmvRT->InitAutoFormat(Width, Height);
	DmvRT->UpdateResourceImmediate(true);
	DmvCamera->TextureTarget = DmvRT;

	// Register component so it gets ticked and rendered
	DmvCamera->RegisterComponent();

	// Store references
	DmvCameras.Add(RgbCamera, DmvCamera);
	DmvRenderTargets.Add(RgbCamera, DmvRT);

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Created DMV camera '%s' with render target (%dx%d)"),
		*DmvName, Width, Height);
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

		// PERFORMANCE: Clean up buffer pool entry
		BufferPool.Remove(Camera);

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
	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Tick will be called automatically (tickable subsystem)"));
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
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] No cameras registered"));
		return;
	}

	ExecuteSynchronizedCapture();
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
// Internal Capture Logic
// ============================================================================

void UCameraCaptureSubsystem::ExecuteSynchronizedCapture()
{
	UE_LOG(LogTemp, Verbose, TEXT("[CameraCaptureSubsystem] ExecuteSynchronizedCapture: %d registered cameras"), RegisteredCameras.Num());

	// Clear any previous pending data
	PendingCaptureData.Empty();
	CamerasAwaitingRGB.Empty();
	CamerasAwaitingDMV.Empty();
	bHasPendingCaptures = false;

	int32 InitiatedCount = 0;

	// Iterate all registered cameras and initiate deferred captures
	for (int32 i = RegisteredCameras.Num() - 1; i >= 0; --i)
	{
		TWeakObjectPtr<UIntrinsicSceneCaptureComponent2D>& WeakCamera = RegisteredCameras[i];

		// Remove invalid cameras
		if (!WeakCamera.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Removing invalid camera at index %d"), i);
			RegisteredCameras.RemoveAt(i);
			continue;
		}

		UIntrinsicSceneCaptureComponent2D* Camera = WeakCamera.Get();

		// Get camera identifier
		FCameraIdentifier* CameraID = CameraIDMap.Find(Camera);
		if (!CameraID)
		{
			UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Camera not found in ID map: %s"), *Camera->GetName());
			continue;
		}

		// Initiate deferred capture for this camera
		InitiateDeferredCapture(Camera, *CameraID);
		InitiatedCount++;
	}

	if (InitiatedCount > 0)
	{
		bHasPendingCaptures = true;
	}

	UE_LOG(LogTemp, Verbose, TEXT("[CameraCaptureSubsystem] Initiated deferred capture for %d cameras"), InitiatedCount);
}

void UCameraCaptureSubsystem::InitiateDeferredCapture(UIntrinsicSceneCaptureComponent2D* Camera, const FCameraIdentifier& CameraID)
{
	if (!Camera)
	{
		return;
	}

	UE_LOG(LogTemp, Verbose, TEXT("[CameraCaptureSubsystem] Initiating deferred capture for: %s"), *CameraID.ToString());

	// Initialize capture data for this camera
	FCaptureData& CaptureData = PendingCaptureData.FindOrAdd(Camera);
	CaptureData.CameraID = CameraID;
	CaptureData.CameraPtr = Camera;  // Cache pointer for serialization
	CaptureData.FrameNumber = FrameIdCounter;
	CaptureData.Timestamp = FPlatformTime::Seconds() - CaptureStartTime;
	CaptureData.WorldTransform = Camera->GetComponentTransform();
	CaptureData.Intrinsics = Camera->GetActiveIntrinsics();
	CaptureData.bUsedCustomProjectionMatrix = Camera->bUseCustomProjectionMatrix;
	if (CaptureData.bUsedCustomProjectionMatrix)
	{
		CaptureData.ProjectionMatrix = Camera->CustomProjectionMatrix;
	}

	// Get actor path and level name
	if (AActor* Owner = Camera->GetOwner())
	{
		CaptureData.ActorPath = Owner->GetPathName();
	}

	UWorld* World = GetWorld();
	if (World)
	{
		CaptureData.LevelName = World->GetMapName();
	}

	// Get dimensions
	FCameraIntrinsics Intrinsics = Camera->GetActiveIntrinsics();
	int32 Width = Intrinsics.ImageWidth;
	int32 Height = Intrinsics.ImageHeight;
	int32 NumPixels = Width * Height;
	CaptureData.Width = Width;
	CaptureData.Height = Height;

	// PERFORMANCE: Get pre-allocated buffers from pool and assign to CaptureData
	// This reuses the same memory allocation across frames
	FCameraBuffers& Buffers = BufferPool.FindOrAdd(Camera);
	Buffers.EnsureSize(Width, Height);
	Buffers.LastFrameUsed = FrameIdCounter;

	// Assign pooled buffers to CaptureData (these are already allocated)
	// ReadLinearColorPixels will write directly into these pre-allocated arrays
	CaptureData.RgbData = Buffers.RgbBuffer;
	CaptureData.DmvData = Buffers.DmvBuffer;

	// Ensure camera has a render target for RGB
	if (!Camera->TextureTarget)
	{
		UTextureRenderTarget2D* NewRenderTarget = NewObject<UTextureRenderTarget2D>(Camera);
		NewRenderTarget->RenderTargetFormat = RTF_RGBA32f; // Use HDR format like CaptureComponent
		NewRenderTarget->InitAutoFormat(Width, Height);
		NewRenderTarget->UpdateResourceImmediate(true);

		Camera->TextureTarget = NewRenderTarget;

		UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Created dynamic RGB render target (%dx%d) for camera %s"),
			Width, Height, *CameraID.ToString());
	}

	// Set completion requirements (PERFORMANCE: Skip disabled channels)
	CaptureData.bNeedsRgb = bCaptureRGB;
	CaptureData.bNeedsDmv = (bCaptureDepth || bCaptureMotionVectors);
	CaptureData.bRgbComplete = false;
	CaptureData.bDmvComplete = false;

	// === Initiate RGB Capture (only if enabled) ===
	if (bCaptureRGB)
	{
		Camera->CaptureSceneDeferred();
		CamerasAwaitingRGB.Add(Camera);
		UE_LOG(LogTemp, VeryVerbose, TEXT("[CameraCaptureSubsystem] Initiated deferred RGB capture for %s"), *CameraID.ToString());
	}
	else
	{
		CaptureData.bRgbComplete = true;  // Mark as complete if not needed
		UE_LOG(LogTemp, VeryVerbose, TEXT("[CameraCaptureSubsystem] Skipped RGB capture (disabled) for %s"), *CameraID.ToString());
	}

	// === Initiate Depth + Motion Vector Capture (only if enabled) ===
	if (bCaptureDepth || bCaptureMotionVectors)
	{
		TWeakObjectPtr<USceneCaptureComponent2D>* DmvCameraPtr = DmvCameras.Find(Camera);

		if (DmvCameraPtr && DmvCameraPtr->IsValid())
		{
			USceneCaptureComponent2D* DmvCamera = DmvCameraPtr->Get();
			DmvCamera->CaptureSceneDeferred();
			CamerasAwaitingDMV.Add(Camera);
			UE_LOG(LogTemp, VeryVerbose, TEXT("[CameraCaptureSubsystem] Initiated deferred DMV capture for %s"), *CameraID.ToString());
		}
		else
		{
			// DMV camera not available, fill with zeros
			CaptureData.DmvData = Buffers.DmvBuffer;  // Use pooled buffer
			for (FLinearColor& Pixel : CaptureData.DmvData)
			{
				Pixel = FLinearColor::Black;
			}
			CaptureData.bDmvComplete = true;
			UE_LOG(LogTemp, VeryVerbose, TEXT("[CameraCaptureSubsystem] No DMV camera available for %s"), *CameraID.ToString());
		}
	}
	else
	{
		CaptureData.bDmvComplete = true;  // Mark as complete if not needed
		UE_LOG(LogTemp, VeryVerbose, TEXT("[CameraCaptureSubsystem] Skipped DMV capture (disabled) for %s"), *CameraID.ToString());
	}

	// If not waiting for anything, process immediately
	if (CaptureData.IsFullyComplete())
	{
		ProcessCompletedCapture(Camera, CameraID);
	}
}

void UCameraCaptureSubsystem::ProcessDeferredCaptures()
{
	double StartTime = FPlatformTime::Seconds();

	// PERFORMANCE: Iterator-based removal (avoid building intermediate arrays)
	// Process RGB captures
	for (auto It = CamerasAwaitingRGB.CreateIterator(); It; ++It)
	{
		TWeakObjectPtr<UIntrinsicSceneCaptureComponent2D> WeakCamera = *It;

		if (!WeakCamera.IsValid())
		{
			It.RemoveCurrent();
			continue;
		}

		UIntrinsicSceneCaptureComponent2D* Camera = WeakCamera.Get();
		UTextureRenderTarget2D* RenderTarget = Camera->TextureTarget;

		if (!RenderTarget)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] No render target for RGB capture"));
			It.RemoveCurrent();
			continue;
		}

		FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
		if (RTResource)
		{
			FCaptureData* CaptureData = PendingCaptureData.Find(Camera);
			if (CaptureData)
			{
				// PERFORMANCE: Read directly into pre-allocated buffer (no allocation or copy)
				if (RTResource->ReadLinearColorPixels(CaptureData->RgbData))
				{
					CaptureData->bRgbComplete = true;

					UE_LOG(LogTemp, VeryVerbose, TEXT("[CameraCaptureSubsystem] RGB capture complete for %s"), *CaptureData->CameraID.ToString());
					It.RemoveCurrent();

					// PERFORMANCE: Direct completion check (no TSet lookup)
					if (CaptureData->IsFullyComplete())
					{
						ProcessCompletedCapture(Camera, CaptureData->CameraID);
					}
				}
			}
		}
	}

	// Process DMV captures
	for (auto It = CamerasAwaitingDMV.CreateIterator(); It; ++It)
	{
		TWeakObjectPtr<UIntrinsicSceneCaptureComponent2D> WeakCamera = *It;

		if (!WeakCamera.IsValid())
		{
			It.RemoveCurrent();
			continue;
		}

		UIntrinsicSceneCaptureComponent2D* Camera = WeakCamera.Get();
		TWeakObjectPtr<USceneCaptureComponent2D>* DmvCameraPtr = DmvCameras.Find(Camera);

		if (!DmvCameraPtr || !DmvCameraPtr->IsValid())
		{
			It.RemoveCurrent();
			continue;
		}

		USceneCaptureComponent2D* DmvCamera = DmvCameraPtr->Get();
		UTextureRenderTarget2D* DmvRT = DmvCamera->TextureTarget;

		if (!DmvRT)
		{
			It.RemoveCurrent();
			continue;
		}

		FTextureRenderTargetResource* DmvResource = DmvRT->GameThread_GetRenderTargetResource();
		if (DmvResource)
		{
			FCaptureData* CaptureData = PendingCaptureData.Find(Camera);
			if (CaptureData)
			{
				// PERFORMANCE: Use pooled buffer instead of allocating
				FCameraBuffers* Buffers = BufferPool.Find(Camera);
				if (Buffers)
				{
					if (DmvResource->ReadLinearColorPixels(Buffers->DmvBuffer))
					{
						// Swap pooled buffer into capture data (zero-copy)
						CaptureData->DmvData = Buffers->DmvBuffer;
						CaptureData->bDmvComplete = true;

						UE_LOG(LogTemp, VeryVerbose, TEXT("[CameraCaptureSubsystem] DMV capture complete for %s"), *CaptureData->CameraID.ToString());
						It.RemoveCurrent();

						// PERFORMANCE: Direct completion check (no TSet lookup)
						if (CaptureData->IsFullyComplete())
						{
							ProcessCompletedCapture(Camera, CaptureData->CameraID);
						}
					}
				}
			}
		}
	}

	double EndTime = FPlatformTime::Seconds();
	double ProcessTime = (EndTime - StartTime) * 1000.0;
	
	#if !UE_BUILD_SHIPPING
	if (ProcessTime > 1.0 && UE_LOG_ACTIVE(LogTemp, VeryVerbose))
	{
		UE_LOG(LogTemp, VeryVerbose, TEXT("[CameraCaptureSubsystem] ProcessDeferredCaptures took %.2fms"), ProcessTime);
	}
	#endif
}

void UCameraCaptureSubsystem::ProcessCompletedCapture(UIntrinsicSceneCaptureComponent2D* Camera, const FCameraIdentifier& CameraID)
{
	FCaptureData* CaptureData = PendingCaptureData.Find(Camera);
	if (!CaptureData)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] No pending capture data for completed camera %s"), *CameraID.ToString());
		return;
	}

	UE_LOG(LogTemp, Verbose, TEXT("[CameraCaptureSubsystem] Capture fully complete for %s"), *CameraID.ToString());

	// Serialize to disk if enabled
	if (bSerializationEnabled)
	{
		SerializeCaptureData(*CaptureData);
	}

	// Update statistics
	TotalFramesCaptured++;

	// Remove from pending
	PendingCaptureData.Remove(Camera);

	// Check if all cameras are done for this frame
	if (PendingCaptureData.Num() == 0 && CamerasAwaitingRGB.Num() == 0 && CamerasAwaitingDMV.Num() == 0)
	{
		// Frame complete, increment counter and clear flag
		FrameIdCounter++;
		bHasPendingCaptures = false;

		double EndTime = FPlatformTime::Seconds();
		LastCaptureDurationMs = (EndTime - CaptureStartTime) * 1000.0f;

		// Running average
		if (AverageCaptureTimeMs == 0.0f)
		{
			AverageCaptureTimeMs = LastCaptureDurationMs;
		}
		else
		{
			AverageCaptureTimeMs = AverageCaptureTimeMs * 0.9f + LastCaptureDurationMs * 0.1f;
		}

		UE_LOG(LogTemp, Verbose, TEXT("[CameraCaptureSubsystem] Frame %lld capture complete (%.2fms avg)"), 
			FrameIdCounter - 1, AverageCaptureTimeMs);
	}
}


void UCameraCaptureSubsystem::SerializeCaptureData(const FCaptureData& Data)
{
	// Convert relative path to absolute path
	FString AbsoluteOutputDir = OutputDirectory;
	if (FPaths::IsRelative(AbsoluteOutputDir))
	{
		AbsoluteOutputDir = FPaths::Combine(*FPaths::ProjectDir(), *OutputDirectory);
	}

	// Create directory structure: OutputDir/ActorName/CameraName/
	FString CameraPath = Data.CameraID.GetFullPath(AbsoluteOutputDir);

	// Create directory using IFileManager
	if (!IFileManager::Get().DirectoryExists(*CameraPath))
	{
		if (!IFileManager::Get().MakeDirectory(*CameraPath, true))
		{
			UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to create directory: %s"), *CameraPath);
			return;
		}
	}

	// Generate frame filename (frame_0000000.exr format, matching CaptureComponent)
	FString FrameNumberStr = FString::Printf(TEXT("%07lld"), Data.FrameNumber);
	FString RgbFilePath = FPaths::Combine(CameraPath, FString::Printf(TEXT("frame_%s.exr"), *FrameNumberStr));
	FString DmvFilePath = FPaths::Combine(CameraPath, FString::Printf(TEXT("frame_%s_motion.exr"), *FrameNumberStr));
	FString MetadataPath = FPaths::Combine(CameraPath, FString::Printf(TEXT("frame_%s.json"), *FrameNumberStr));

	// Use shared utility functions from CaptureComponent
	// Write RGB+Depth EXR (RGB in RGB channels, Depth in Alpha channel)
	if (Data.bNeedsRgb && Data.RgbData.Num() > 0)
	{
		CameraCaptureUtils::WriteEXRFile(RgbFilePath, Data.RgbData, Data.DmvData, Data.Width, Data.Height, true);
	}

	// Write Motion Vectors EXR (X in R, Y in G channels)
	if (Data.bNeedsDmv && Data.DmvData.Num() > 0)
	{
		CameraCaptureUtils::WriteEXRFile(DmvFilePath, Data.DmvData, Data.DmvData, Data.Width, Data.Height, false);
	}

	// PERFORMANCE: Use cached camera pointer (avoid map iteration)
	if (Data.CameraPtr.IsValid())
	{
		UIntrinsicSceneCaptureComponent2D* Camera = Data.CameraPtr.Get();
		CameraCaptureUtils::WriteMetadataFile(MetadataPath, Camera, Data.Intrinsics, 
			Data.FrameNumber, Data.Timestamp, Data.ActorPath, Data.LevelName);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Camera pointer invalid for metadata writing: %s"), 
			*Data.CameraID.ToString());
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
