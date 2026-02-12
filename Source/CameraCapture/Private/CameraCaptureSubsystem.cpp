#include "CameraCaptureSubsystem.h"
#include "IntrinsicSceneCaptureComponent2D.h"
#include "Engine/World.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "ImageUtils.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "ImageWriteQueue.h"
#include "ImageWriteTask.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"

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
	FString MaterialPath = TEXT("/Script/Engine.Material'/CameraCapture/M_DmvCapture.M_DmvCapture'");
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
	// Only tick if we're initialized and actively capturing
	return IsInitialized() && bIsCapturing && !IsTemplate();
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
	int32 Width = Intrinsics.ImageWidth;
	int32 Height = Intrinsics.ImageHeight;

	// Create new DMV camera component attached to the RGB camera
	FString DmvName = RgbCamera->GetName() + TEXT("_dmv");
	USceneCaptureComponent2D* DmvCamera = NewObject<USceneCaptureComponent2D>(
		RgbCamera->GetOwner(),
		USceneCaptureComponent2D::StaticClass(),
		FName(*DmvName),
		RF_Transient,
		RgbCamera  // Use RGB camera as template
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
	DmvRT->RenderTargetFormat = RTF_RGBA32f;  // Float format for depth+motion
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
	double StartTime = FPlatformTime::Seconds();

	int32 CapturedCount = 0;
	int32 FailedCount = 0;

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] ExecuteSynchronizedCapture: %d registered cameras"), RegisteredCameras.Num());

	// Iterate all registered cameras
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

		// Capture data from this camera
		FCaptureData CaptureData;
		if (CaptureCameraData(Camera, CaptureData))
		{
			// Serialize to disk
			SerializeCaptureData(CaptureData);
			CapturedCount++;
		}
		else
		{
			FailedCount++;
		}
	}

	TotalFramesCaptured += CapturedCount;
    FrameIdCounter++;

	// Update statistics
	double EndTime = FPlatformTime::Seconds();
	LastCaptureDurationMs = (EndTime - StartTime) * 1000.0f;
	
	// Running average
	if (AverageCaptureTimeMs == 0.0f)
	{
		AverageCaptureTimeMs = LastCaptureDurationMs;
	}
	else
	{
		AverageCaptureTimeMs = AverageCaptureTimeMs * 0.9f + LastCaptureDurationMs * 0.1f;
	}

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Captured %d/%d cameras successfully, %d failed (%.2fms)"), 
		CapturedCount, RegisteredCameras.Num(), FailedCount, LastCaptureDurationMs);

	if (CapturedCount > 0)
	{
		UE_LOG(LogTemp, Verbose, TEXT("[CameraCaptureSubsystem] Captured %d cameras in %.2fms (avg: %.2fms)"),
			CapturedCount, LastCaptureDurationMs, AverageCaptureTimeMs);
	}
}

bool UCameraCaptureSubsystem::CaptureCameraData(UIntrinsicSceneCaptureComponent2D* Camera, FCaptureData& OutData)
{
	if (!Camera)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Null camera pointer"));
		return false;
	}

	// Get camera identifier
	FCameraIdentifier* CameraID = CameraIDMap.Find(Camera);
	if (!CameraID)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Camera not found in ID map: %s"), *Camera->GetName());
		return false;
	}

	UE_LOG(LogTemp, Verbose, TEXT("[CameraCaptureSubsystem] Capturing from camera: %s"), *CameraID->ToString());

	OutData.CameraID = *CameraID;
	OutData.FrameNumber = FrameIdCounter;
	OutData.Timestamp = FPlatformTime::Seconds() - CaptureStartTime;

	// Get world transform
	OutData.WorldTransform = Camera->GetComponentTransform();

	// Get intrinsics
	OutData.Intrinsics = Camera->GetActiveIntrinsics();
	OutData.bUsedCustomProjectionMatrix = Camera->bUseCustomProjectionMatrix;
	if (OutData.bUsedCustomProjectionMatrix)
	{
		OutData.ProjectionMatrix = Camera->CustomProjectionMatrix;
	}

	// Get actor path and level name
	if (AActor* Owner = Camera->GetOwner())
	{
		OutData.ActorPath = Owner->GetPathName();
	}
	
	UWorld* World = GetWorld();
	if (World)
	{
		OutData.LevelName = World->GetMapName();
	}

	// Get intrinsics for render target creation
	FCameraIntrinsics Intrinsics = Camera->GetActiveIntrinsics();
	int32 Width = Intrinsics.ImageWidth;
	int32 Height = Intrinsics.ImageHeight;

	// Ensure camera has a render target for RGB
	if (!Camera->TextureTarget)
	{
		UTextureRenderTarget2D* NewRenderTarget = NewObject<UTextureRenderTarget2D>(Camera);
		NewRenderTarget->RenderTargetFormat = RTF_RGBA8;
		NewRenderTarget->InitAutoFormat(Width, Height);
		NewRenderTarget->UpdateResourceImmediate(true);
		
		Camera->TextureTarget = NewRenderTarget;
		
		UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Created dynamic RGB render target (%dx%d) for camera %s"),
			Width, Height, *CameraID->ToString());
	}

	// === RGB Capture ===
	if (bCaptureRGB)
	{
		Camera->CaptureScene();

		UTextureRenderTarget2D* RenderTarget = Camera->TextureTarget;
		OutData.Width = RenderTarget->SizeX;
		OutData.Height = RenderTarget->SizeY;

		FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
		if (RTResource)
		{
			OutData.ImageData.SetNum(OutData.Width * OutData.Height);
			if (!RTResource->ReadPixels(OutData.ImageData))
			{
				UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to read RGB pixels from %s"), *CameraID->ToString());
				return false;
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] No render target resource for %s"), *CameraID->ToString());
			return false;
		}
	}

	// === Depth + Motion Vector Capture using separate DMV camera ===
	if ((bCaptureDepth || bCaptureMotionVectors))
	{
		// Look up the DMV camera we created during registration
		TWeakObjectPtr<USceneCaptureComponent2D>* DmvCameraPtr = DmvCameras.Find(Camera);
		
		if (DmvCameraPtr && DmvCameraPtr->IsValid())
		{
			USceneCaptureComponent2D* DmvCamera = DmvCameraPtr->Get();
			
			// Capture depth+motion
			DmvCamera->CaptureScene();
			
			UTextureRenderTarget2D* DmvRT = DmvCamera->TextureTarget;
			if (DmvRT)
			{
				FTextureRenderTargetResource* DmvResource = DmvRT->GameThread_GetRenderTargetResource();
				
				if (DmvResource)
				{
					TArray<FLinearColor> DmvPixels;
					DmvPixels.SetNum(Width * Height);
					
					if (DmvResource->ReadLinearColorPixels(DmvPixels))
					{
						OutData.DepthData.SetNum(Width * Height);
						OutData.MotionVectorData.SetNum(Width * Height);
						
						// Decode: R=Depth (cm), G=MotionX (px/frame), B=MotionY (px/frame), A=1
						for (int32 i = 0; i < DmvPixels.Num(); i++)
						{
							OutData.DepthData[i] = DmvPixels[i].R;
							OutData.MotionVectorData[i] = FVector2D(DmvPixels[i].G, DmvPixels[i].B);
						}
						
						UE_LOG(LogTemp, Verbose, TEXT("[CameraCaptureSubsystem] Captured depth+motion from DMV camera for %s"), *CameraID->ToString());
					}
					else
					{
						UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Failed to read depth+motion pixels from %s"), *CameraID->ToString());
					}
				}
			}
		}
		else
		{
			// DMV camera not set up (material might not have loaded)
			OutData.DepthData.SetNumZeroed(Width * Height);
			OutData.MotionVectorData.SetNumZeroed(Width * Height);
			UE_LOG(LogTemp, VeryVerbose, TEXT("[CameraCaptureSubsystem] No DMV camera available for %s"), *CameraID->ToString());
		}
	}

	return true;
}

void UCameraCaptureSubsystem::SerializeCaptureData(const FCaptureData& Data)
{
	// Convert relative path to absolute path (same as original CaptureComponent)
	FString AbsoluteOutputDir = OutputDirectory;
	if (FPaths::IsRelative(AbsoluteOutputDir))
	{
		AbsoluteOutputDir = FPaths::Combine(*FPaths::ProjectDir(), *OutputDirectory);
	}
	
	// Create directory structure: OutputDir/ActorName/CameraName/
	FString CameraPath = Data.CameraID.GetFullPath(AbsoluteOutputDir);

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Absolute path: %s"), *CameraPath);

	// Create directory using IFileManager (same as original CaptureComponent)
	if (!IFileManager::Get().DirectoryExists(*CameraPath))
	{
		if (!IFileManager::Get().MakeDirectory(*CameraPath, true))
		{
			UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to create directory: %s"), *CameraPath);
			return;
		}
	}
	
	// Verify directory was created
	if (!IFileManager::Get().DirectoryExists(*CameraPath))
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Directory does not exist after creation: %s"), *CameraPath);
		return;
	}

	// Generate frame filename (frame_0000000.exr)
	FString FrameNumberStr = FString::Printf(TEXT("%07lld"), Data.FrameNumber);
	
	// Write EXR file with all channels (use absolute path)
	FString ExrPath = FPaths::Combine(CameraPath, FString::Printf(TEXT("frame_%s.exr"), *FrameNumberStr));
	
	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Writing EXR: %s"), *ExrPath);
	
	bool bSuccess = WriteEXRFile(ExrPath, Data);

	if (!bSuccess)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to write EXR: %s"), *ExrPath);
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] Submitted EXR write task: %s"), *ExrPath);

	// Write metadata JSON (separate from EXR for easy access)
	FString MetadataPath = FPaths::Combine(CameraPath, FString::Printf(TEXT("frame_%s.json"), *FrameNumberStr));
	WriteMetadataFile(MetadataPath, Data);
}

bool UCameraCaptureSubsystem::WriteEXRFile(const FString& FilePath, const FCaptureData& Data)
{
	// Safety checks
	if (Data.Width <= 0 || Data.Height <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Invalid image dimensions: %dx%d"), Data.Width, Data.Height);
		return false;
	}
	
	int32 NumPixels = Data.Width * Data.Height;
	
	if (Data.ImageData.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] No image data to write"));
		return false;
	}
	
	// Prepare multi-channel pixel data for RGBA (RGB + Depth in Alpha)
	TArray64<FLinearColor> PixelData;
	PixelData.SetNum(NumPixels);
	
	// Convert RGB from FColor (0-255) to FLinearColor (0-1)
	if (bCaptureRGB && Data.ImageData.Num() == NumPixels)
	{
		for (int32 i = 0; i < NumPixels; i++)
		{
			PixelData[i] = FLinearColor(Data.ImageData[i]);
		}
	}
	else
	{
		// Fill with black if no RGB data
		for (int32 i = 0; i < NumPixels; i++)
		{
			PixelData[i] = FLinearColor::Black;
		}
	}
	
	// Store depth in Alpha channel
	if (bCaptureDepth && Data.DepthData.Num() == NumPixels)
	{
		for (int32 i = 0; i < NumPixels; i++)
		{
			// Normalize depth to 0-1 range for storage (assuming max depth of 10000cm = 100m)
			float NormalizedDepth = FMath::Clamp(Data.DepthData[i] / 10000.0f, 0.0f, 1.0f);
			PixelData[i].A = NormalizedDepth;
		}
	}
	
	// Store motion vectors separately if we have them
	TArray<float> MotionXData;
	TArray<float> MotionYData;
	bool bHasMotion = bCaptureMotionVectors && Data.MotionVectorData.Num() == NumPixels;
	
	if (bHasMotion)
	{
		MotionXData.SetNum(NumPixels);
		MotionYData.SetNum(NumPixels);
		
		for (int32 i = 0; i < NumPixels; i++)
		{
			MotionXData[i] = Data.MotionVectorData[i].X;
			MotionYData[i] = Data.MotionVectorData[i].Y;
		}
	}
	
	// Create image write task for base RGBA channels
	TUniquePtr<FImageWriteTask> ImageTask = MakeUnique<FImageWriteTask>();
	ImageTask->PixelData = MakeUnique<TImagePixelData<FLinearColor>>(FIntPoint(Data.Width, Data.Height), MoveTemp(PixelData));
	ImageTask->Filename = FilePath;
	ImageTask->Format = EImageFormat::EXR;
	ImageTask->CompressionQuality = (int32)EImageCompressionQuality::Default;
	ImageTask->bOverwriteFile = true;
	
	// Add completion callback
	ImageTask->OnCompleted = [FilePath](bool bSuccess)
	{
		if (bSuccess)
		{
			UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] ✓ EXR write complete: %s"), *FilePath);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] ✗ EXR write FAILED: %s"), *FilePath);
		}
	};
	
	// Get the image write queue module
	IImageWriteQueueModule* ImageWriteQueueModule = FModuleManager::Get().GetModulePtr<IImageWriteQueueModule>("ImageWriteQueue");
	if (!ImageWriteQueueModule)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureSubsystem] Failed to get ImageWriteQueue module"));
		return false;
	}
	
	// Submit main EXR task
	ImageWriteQueueModule->GetWriteQueue().Enqueue(MoveTemp(ImageTask));
	
	// If we have motion vectors, write them to a separate file asynchronously
	if (bHasMotion)
	{
		const FString MotionPath = FilePath.Replace(TEXT(".exr"), TEXT("_motion.exr"));
		const int32 Width = Data.Width;
		const int32 Height = Data.Height;
		
		AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [MotionPath, Width, Height, MotionXData, MotionYData]()
		{
			// Create motion image with X in R channel, Y in G channel
			TArray64<FLinearColor> MotionPixels;
			MotionPixels.SetNum(Width * Height);
			
			for (int32 i = 0; i < Width * Height; i++)
			{
				MotionPixels[i] = FLinearColor(MotionXData[i], MotionYData[i], 0.0f, 0.0f);
			}
			
			// Write motion EXR
			TUniquePtr<FImageWriteTask> MotionTask = MakeUnique<FImageWriteTask>();
			MotionTask->PixelData = MakeUnique<TImagePixelData<FLinearColor>>(FIntPoint(Width, Height), MoveTemp(MotionPixels));
			MotionTask->Filename = MotionPath;
			MotionTask->Format = EImageFormat::EXR;
			MotionTask->CompressionQuality = (int32)EImageCompressionQuality::Default;
			MotionTask->bOverwriteFile = true;
			
			MotionTask->OnCompleted = [MotionPath](bool bSuccess)
			{
				if (bSuccess)
				{
					UE_LOG(LogTemp, Log, TEXT("[CameraCaptureSubsystem] ✓ Motion EXR write complete: %s"), *MotionPath);
				}
			};
			
			IImageWriteQueueModule* MotionWriteModule = FModuleManager::Get().GetModulePtr<IImageWriteQueueModule>("ImageWriteQueue");
			if (MotionWriteModule)
			{
				MotionWriteModule->GetWriteQueue().Enqueue(MoveTemp(MotionTask));
			}
		});
	}
	
	return true;
}

bool UCameraCaptureSubsystem::WriteMetadataFile(const FString& FilePath, const FCaptureData& Data)
{
	// Create JSON object
	TSharedPtr<FJsonObject> JsonObject = MakeShared<FJsonObject>();

	JsonObject->SetNumberField(TEXT("frame_number"), Data.FrameNumber);
	JsonObject->SetNumberField(TEXT("timestamp"), Data.Timestamp);
	JsonObject->SetStringField(TEXT("camera_id"), Data.CameraID.ToString());

	// World transform
	TSharedPtr<FJsonObject> TransformJson = MakeShared<FJsonObject>();
	FVector Location = Data.WorldTransform.GetLocation();
	FRotator Rotation = Data.WorldTransform.Rotator();
	FVector Scale = Data.WorldTransform.GetScale3D();

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
	FString OutputString;
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
	int32 Suffix = 1;
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
		
	} while (Suffix < 100); // Safety limit

	UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureSubsystem] Actor/component name collision detected, using: %s"), *DisambiguatedName);

	return DisambiguatedName;
}
