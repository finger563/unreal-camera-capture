// Fill out your copyright notice in the Description page of Project Settings.

#include "CaptureComponent.h"

#include "Components/SceneCaptureComponent2D.h"
#include "IntrinsicSceneCaptureComponent2D.h"
#include "Utilities.h"
#include "Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/FileHelper.h"

// stl includes
#include <limits>
#include <cmath>

// Sets default values
UCaptureComponent::UCaptureComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetComponentTickEnabled(true);
}

// Called when the game starts or when spawned
void UCaptureComponent::BeginPlay()
{
	Super::BeginPlay();

	// Only find UIntrinsicSceneCaptureComponent2D cameras (not base USceneCaptureComponent2D)
	// This allows us to have per-camera intrinsics and only capture from marked cameras
	TArray<UIntrinsicSceneCaptureComponent2D*> IntrinsicCameras;
	GetOwner()->GetComponents(IntrinsicCameras);

	// Cast to base type for existing code compatibility
	for (auto* IntrinsicCamera : IntrinsicCameras)
	{
		RgbCameras.Add(IntrinsicCamera);
	}

	if (RgbCameras.Num())
	{
		UE_LOG(LogTemp, Log, TEXT("UCaptureComponent:: Found %d UIntrinsicSceneCaptureComponent2D cameras"), RgbCameras.Num());
		ConfigureCameras();

		if (TimerPeriod > 0.0f)
		{
			UE_LOG(LogTemp, Log, TEXT("Timer period > 0, capturing every %f seconds!"), TimerPeriod);
			GetOwner()->GetWorldTimerManager().SetTimer(CaptureTimerHandle,
				this,
				&UCaptureComponent::TimerUpdateCallback,
				TimerPeriod,
				true,
				TimerDelay);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("Timer Period <= 0, capturing every frame!"));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("UCaptureComponent:: Could not find any UIntrinsicSceneCaptureComponent2D components on this actor!"));
		UE_LOG(LogTemp, Warning, TEXT("UCaptureComponent:: Make sure to use UIntrinsicSceneCaptureComponent2D instead of base USceneCaptureComponent2D"));
	}
}

void UCaptureComponent::TimerUpdateCallback()
{
	UpdateTransformFile();
	CaptureData();
}

// Called every frame
void UCaptureComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Collect anything the GPU has finished first, so a frame captured earlier
	// is written before this tick queues another one.
	HarvestAndWriteReadyFrames();

	// If the timer period is set to 0 (or less), we're capturing every frame, so
	// we can want to capture in the TickComponent function. Otherwise it's called
	// within the timer callback function
	if (TimerPeriod <= 0.0f)
	{
		UpdateTransformFile();
		CaptureData();
	}
}

void UCaptureComponent::ConfigureCameras()
{
	for (auto rgb : RgbCameras)
	{
		UE_LOG(LogTemp, Log, TEXT("UCaptureComponent:: Found camera %s!"), *rgb->GetFName().ToString());

		// Get image dimensions from camera intrinsics
		UIntrinsicSceneCaptureComponent2D* IntrinsicCamera = Cast<UIntrinsicSceneCaptureComponent2D>(rgb);
		int32							   ImageWidth = 640;
		int32							   ImageHeight = 480;
		int32							   DepthWidth = 640;
		int32							   DepthHeight = 480;

		if (IntrinsicCamera)
		{
			FCameraIntrinsics Intrinsics = IntrinsicCamera->GetActiveIntrinsics();
			ImageWidth = Intrinsics.ImageWidth;
			ImageHeight = Intrinsics.ImageHeight;
			UE_LOG(LogTemp, Log, TEXT("  Using RGB resolution %dx%d from camera intrinsics"), ImageWidth, ImageHeight);

			// Use depth intrinsics for DMV camera if available
			FCameraIntrinsics DepthIntrinsics = IntrinsicCamera->GetActiveDepthIntrinsics();
			DepthWidth = DepthIntrinsics.ImageWidth;
			DepthHeight = DepthIntrinsics.ImageHeight;

			if (IntrinsicCamera->HasSeparateDepthIntrinsics())
			{
				UE_LOG(LogTemp, Log, TEXT("  Using separate depth resolution %dx%d"), DepthWidth, DepthHeight);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("  Camera is not UIntrinsicSceneCaptureComponent2D, using default 640x480"));
		}

		// configure the rgb camera
		ConfigureRgbCamera(rgb);
		// make the rgb texture
		auto rgb_rt = MakeRenderTexture(ImageWidth, ImageHeight);
		RgbTextures.Add(rgb_rt);
		rgb->TextureTarget = rgb_rt;

		// make the dmv camera based on the rgb camera — defer registration so we can
		// apply depth intrinsics/offset BEFORE BeginPlay runs ApplyIntrinsics
		bool bNeedsDeferral = IntrinsicCamera && (IntrinsicCamera->HasSeparateDepthIntrinsics() || IntrinsicCamera->bUseDepthSensorOffset);
		auto dmv = CopyAndAttachCamera(rgb, FString("_depth_motion"), bNeedsDeferral);

		if (bNeedsDeferral)
		{
			UIntrinsicSceneCaptureComponent2D* DmvIntrinsicCamera = Cast<UIntrinsicSceneCaptureComponent2D>(dmv);

			// Override intrinsics before registration so BeginPlay applies the correct projection
			if (DmvIntrinsicCamera && IntrinsicCamera->HasSeparateDepthIntrinsics())
			{
				DmvIntrinsicCamera->bUseDepthIntrinsics = false;
				DmvIntrinsicCamera->bUseIntrinsicsAsset = IntrinsicCamera->bUseDepthIntrinsicsAsset;
				DmvIntrinsicCamera->IntrinsicsAsset = IntrinsicCamera->DepthIntrinsicsAsset;
				DmvIntrinsicCamera->InlineIntrinsics = IntrinsicCamera->DepthInlineIntrinsics;
			}

			// Apply depth sensor offset before registration
			if (IntrinsicCamera->bUseDepthSensorOffset)
			{
				dmv->SetRelativeLocation(IntrinsicCamera->DepthSensorOffset.GetLocation());
				dmv->SetRelativeRotation(IntrinsicCamera->DepthSensorOffset.GetRotation().Rotator());
				dmv->SetRelativeScale3D(IntrinsicCamera->DepthSensorOffset.GetScale3D());
			}

			// Now register — BeginPlay/ApplyIntrinsics will use the correct depth intrinsics
			dmv->RegisterComponent();
		}

		ConfigureDmvCamera(dmv);
		DmvCameras.Add(dmv);
		// make the dmv texture using depth intrinsics dimensions
		auto dmv_rt = MakeRenderTexture(DepthWidth, DepthHeight);
		DmvTextures.Add(dmv_rt);
		dmv->TextureTarget = dmv_rt;
	}
	// set the hidden actors for all the cameras
	SetHiddenActors(HiddenActors);
}

void UCaptureComponent::ConfigureDmvCamera(USceneCaptureComponent2D* camera)
{
	// we control the capture of the cameras, so turn off the
	// auto-capture
	camera->bCaptureEveryFrame = false;
	if (!camera->bCaptureEveryFrame)
	{
		// Make sure the cameras don't auto-capture on movement
		camera->bCaptureOnMovement = false;
		// make sure the camera rendering state is saved between frames
		// (otherwise we get a black image)
		camera->bAlwaysPersistRenderingState = true;
	}
	// where do we pull color data from? options are
	// https://docs.unrealengine.com/en-US/API/Runtime/Engine/Engine/ESceneCaptureSource/index.html
	camera->CaptureSource = CaptureSource;

	// make the DmvMaterial
	if (DmvMaterialBase)
	{
		auto DmvMaterial = UMaterialInstanceDynamic::Create(DmvMaterialBase, this);
		if (!DmvMaterial)
		{
			UE_LOG(LogTemp, Error, TEXT("Error, could not create DmvMaterial!"));
		}
		// set the post process material (DMV)
		// https://docs.unrealengine.com/en-US/API/Runtime/Engine/Components/USceneCaptureComponent2D/PostProcessSettings/index.html
		camera->PostProcessSettings.WeightedBlendables.Array.Empty();
		camera->PostProcessSettings.WeightedBlendables.Array.Add(FWeightedBlendable(1.0f, DmvMaterial));
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("No DmvMaterial set!"));
	}
}

void UCaptureComponent::ConfigureRgbCamera(USceneCaptureComponent2D* camera)
{
	// we control the capture of the cameras, so turn off the
	// auto-capture
	camera->bCaptureEveryFrame = false;
	if (!camera->bCaptureEveryFrame)
	{
		// Make sure the cameras don't auto-capture on movement
		camera->bCaptureOnMovement = false;
		// make sure the camera rendering state is saved between frames
		// (otherwise we get a black image)
		camera->bAlwaysPersistRenderingState = true;
	}
	// where do we pull color data from? options are
	// https://docs.unrealengine.com/en-US/API/Runtime/Engine/Engine/ESceneCaptureSource/index.html
	camera->CaptureSource = CaptureSource;
	// make sure there are no post process materials / effects on the
	// rgb camera (since we're just copying the dmv camera's config)
	camera->PostProcessSettings.WeightedBlendables.Array.Empty();

	// Camera intrinsics are now handled by URammsSceneCaptureComponent2D itself
	// They are applied in BeginPlay and when properties change in editor
}

USceneCaptureComponent2D* UCaptureComponent::CopyAndAttachCamera(USceneCaptureComponent2D* camera, FString name_suffix, bool bDeferRegistration)
{
	auto name = camera->GetFName().ToString() + name_suffix;
	UE_LOG(LogTemp, Log, TEXT("Copying camera '%s'"), *name);
	// Use the source camera's actual class so subclass properties (e.g. intrinsics) are preserved
	// Outer must be the owning actor for proper component hierarchy
	auto copy = NewObject<USceneCaptureComponent2D>(GetOwner(),
		camera->GetClass(),
		FName(*name),
		EObjectFlags::RF_NoFlags, // flags
		camera					  // template object for initializing
	);

	// Use SetupAttachment (pre-registration API) instead of AttachToComponent.
	// This is critical when the source camera is the root component — AttachToComponent
	// on an unregistered component won't properly establish the parent-child relationship.
	copy->SetupAttachment(camera);
	copy->SetRelativeLocation(FVector::ZeroVector);
	copy->SetRelativeRotation(FRotator::ZeroRotator);

	if (!bDeferRegistration)
	{
		copy->RegisterComponent();
	}
	// When bDeferRegistration is true, the caller is responsible for calling
	// RegisterComponent() after applying any property overrides (e.g. depth intrinsics).
	// This avoids BeginPlay/ApplyIntrinsics running with stale properties.

	return copy;
}

UTextureRenderTarget2D* UCaptureComponent::MakeRenderTexture(int width, int height)
{
	auto renderTexture = NewObject<UTextureRenderTarget2D>();
	renderTexture->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA32f;
	renderTexture->ResizeTarget(width, height);
	renderTexture->UpdateResource();
	return renderTexture;
}

void UCaptureComponent::InitializeFiles()
{
	if (!HasInitializedFiles)
	{
		InitOutput();
		WriteConfigFile();
		WriteTransformHeader();
	}
	HasInitializedFiles = true;
}

void UCaptureComponent::StartCapturing()
{
	ShouldCaptureData = true;
}

void UCaptureComponent::StopCapturing()
{
	ShouldCaptureData = false;
}

bool UCaptureComponent::IsCapturing()
{
	return ShouldCaptureData;
}

bool UCaptureComponent::ToggleCapturing()
{
	if (ShouldCaptureData)
	{
		StopCapturing();
	}
	else
	{
		StartCapturing();
	}
	return IsCapturing();
}

void UCaptureComponent::StartSavingData()
{
	// set the ShouldSaveData variable to true to resume / start saving
	// rendering and transform data
	ShouldSaveData = true;
	// Ensure that we create the necessary folders and files if
	// necessary
	InitializeFiles();
}

void UCaptureComponent::StopSavingData()
{
	// set the ShouldSaveData state variable to false to stop saving rendering
	// & transform data
	ShouldSaveData = false;
}

bool UCaptureComponent::IsSavingData()
{
	return ShouldSaveData;
}

bool UCaptureComponent::ToggleSavingData()
{
	if (ShouldSaveData)
	{
		StopSavingData();
	}
	else
	{
		StartSavingData();
	}
	return IsSavingData();
}

TArray<AActor*> UCaptureComponent::GetHiddenActors()
{
	return HiddenActors;
}

void UCaptureComponent::SetHiddenActors(TArray<AActor*> actors)
{
	HiddenActors = actors;
	// make sure to update the hidden actors for all the cameras
	for (auto camera : RgbCameras)
	{
		camera->HiddenActors = HiddenActors;
	}
	for (auto camera : DmvCameras)
	{
		camera->HiddenActors = HiddenActors;
	}
}

void UCaptureComponent::InitOutput()
{
	if (SaveLocation.IsEmpty())
	{
		SaveLocation = FPaths::Combine(*FPaths::ProjectDir(), *FString("camera_data"));
		FString displayPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*SaveLocation);
		UE_LOG(LogTemp, Error,
			TEXT("No output directory provided, creating 'camera_data' folder in game directory: %s"),
			*displayPath);
	}
	if (!IFileManager::Get().DirectoryExists(*SaveLocation))
	{
		IFileManager::Get().MakeDirectory(*SaveLocation);
	}

	// Create actor-based directory structure (matching CameraCaptureManager)
	FString ActorName = GetOwner() ? GetOwner()->GetName() : TEXT("UnknownActor");
	FString ActorPath = FPaths::Combine(*SaveLocation, *ActorName);
	if (!IFileManager::Get().DirectoryExists(*ActorPath))
	{
		IFileManager::Get().MakeDirectory(*ActorPath, true);
	}

	// Legacy CSV files go in actor folder (not per-camera, since they contain all cameras on this actor)
	ConfigFile = FPaths::Combine(*ActorPath, *FString("camera_config.csv"));
	TransformFile = FPaths::Combine(*ActorPath, *FString("transformations.csv"));
}

void UCaptureComponent::WriteConfigFile()
{
	FString configString("name,width,height,focalLength,fov,nearClipPlane,farClipPlane,tx,ty,tz,qw,qx,qy,qz\n");
	for (auto camera : RgbCameras)
	{
		auto transform = camera->GetComponentTransform().GetRelativeTransform(GetOwner()->GetTransform());
		// for distances here we divide by 100.0 since unreal is in cm and we want to convert to m
		auto t = transform.GetTranslation() / 100.0f;
		auto q = transform.GetRotation();

		// Get image dimensions and focal length from camera intrinsics
		int32 ImageWidth = 640;
		int32 ImageHeight = 480;
		float focalLength = 0.0f;
		float fov = 0.0f;

		UIntrinsicSceneCaptureComponent2D* IntrinsicCamera = Cast<UIntrinsicSceneCaptureComponent2D>(camera);
		if (IntrinsicCamera && IntrinsicCamera->bUseCustomIntrinsics)
		{
			FCameraIntrinsics Intrinsics = IntrinsicCamera->GetActiveIntrinsics();
			ImageWidth = Intrinsics.ImageWidth;
			ImageHeight = Intrinsics.ImageHeight;
			// Average focal length for config file
			focalLength = (Intrinsics.FocalLengthX + Intrinsics.FocalLengthY) / 2.0f;

			// When using custom projection matrix, compute effective horizontal FOV from intrinsics
			// FOV = 2 * atan(width / (2 * fx))
			if (IntrinsicCamera->bUseCustomProjectionMatrix && !Intrinsics.bMaintainYAxis)
			{
				float fx = Intrinsics.FocalLengthX; // in pixels
				if (FMath::Abs(fx) > KINDA_SMALL_NUMBER)
				{
					fov = 2.0f * FMath::RadiansToDegrees(FMath::Atan(ImageWidth / (2.0f * fx)));
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("Invalid focal length X (%f) for camera %s; falling back to FOVAngle."), fx, *camera->GetName());
					fov = camera->FOVAngle;
				}
			}
			else
			{
				// Using FOV-based or Maintain Y-Axis mode - use camera's FOV
				fov = camera->FOVAngle;
			}
		}
		else
		{
			// Not an intrinsic camera or not using custom intrinsics - use default FOV
			fov = camera->FOVAngle;
		}

		// Note: all cameras share the same frustum configuration (near/far planes)
		float nearPlane = GNearClippingPlane / 100.0f;
		// there is no far clip plane in UE4, according to
		// https://forums.unrealengine.com/development-discussion/rendering/1580676-far-clip-plane
		float	farPlane = std::numeric_limits<float>::infinity();
		FString cameraString = FString::Printf(
			TEXT("%s,%d,%d,%.2f,%.2f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n"),
			*camera->GetFName().ToString(),
			ImageWidth, ImageHeight,
			focalLength,
			fov,
			nearPlane, farPlane,
			t.X, t.Y, t.Z,
			q.W, q.X, q.Y, q.Z);
		configString += cameraString;
	}
	bool	didWrite = FFileHelper::SaveStringToFile(configString, *ConfigFile);
	FString displayPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*ConfigFile);
	if (!didWrite)
	{
		UE_LOG(LogTemp, Error, TEXT("Error: could not write config file %s"), *displayPath);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Wrote config file %s"), *displayPath);
	}
}

void UCaptureComponent::WriteTransformHeader()
{
	FString transformString("i,time,tx,ty,tz,qw,qx,qy,qz\n");
	bool	didWrite = FFileHelper::SaveStringToFile(transformString, *TransformFile);
	FString displayPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*TransformFile);
	if (!didWrite)
	{
		UE_LOG(LogTemp, Error, TEXT("Error: could not write transform file %s"), *displayPath);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Writing transform file %s"), *displayPath);
	}
}

void UCaptureComponent::UpdateTransformFile()
{
	if (!ShouldSaveData)
	{
		return;
	}
	auto	transform = GetOwner()->GetTransform();
	auto	t = transform.GetTranslation() / 100.0f; // unreal is in cm, convert to m
	auto	q = transform.GetRotation();
	float	time = GetWorld()->GetTimeSeconds(); // other option is UGameplayStatics::GetRealTimeSeconds()
	FString transformString = FString::Printf(
		TEXT("%d,%f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n"),
		ImageIndex,
		time,
		t.X, t.Y, t.Z,
		q.W, q.X, q.Y, q.Z);
	bool didWrite = FFileHelper::SaveStringToFile(
		transformString,
		*TransformFile,
		FFileHelper::EEncodingOptions::AutoDetect,
		&IFileManager::Get(),
		FILEWRITE_Append);
	if (!didWrite)
	{
		UE_LOG(LogTemp, Error, TEXT("Error: could not append to transform file %s"), *TransformFile);
	}
}

void UCaptureComponent::CaptureData()
{
	// if we're not capturing data, just return
	if (!ShouldCaptureData)
		return;

	// Start deferred capture of the scene (RGB)
	for (auto camera : RgbCameras)
	{
		camera->CaptureSceneDeferred();
	}
	// Start deferred capture of the scene (DMV)
	for (auto camera : DmvCameras)
	{
		camera->CaptureSceneDeferred();
	}

	// Nothing is read back unless it is going to be written. Reading costs a
	// GPU copy per camera per channel whether or not anyone wants the pixels.
	if (!ShouldSaveData)
	{
		return;
	}

	// Queue the copies now and collect them when the GPU is done. The previous
	// version called ReadLinearColorPixels on the next tick, which ends in
	// FlushRenderingCommands() -- the game thread sat waiting for the GPU twice
	// per camera, every capture.
	const int32 FrameIndex = ImageIndex++;
	for (int32 i = 0; i < RgbCameras.Num(); i++)
	{
		if (!RgbTextures.IsValidIndex(i) || !DmvTextures.IsValidIndex(i))
		{
			continue;
		}

		FPendingFrame Pending;
		Pending.CameraIndex = i;
		Pending.FrameIndex = FrameIndex;

		const bool bRgbQueued = CameraCaptureUtils::EnqueueReadback(RgbTextures[i], Pending.Rgb);
		const bool bDmvQueued = CameraCaptureUtils::EnqueueReadback(DmvTextures[i], Pending.Dmv);
		if (!bRgbQueued || !bDmvQueued)
		{
			UE_LOG(LogTemp, Warning, TEXT("Could not queue readback for camera %d; skipping this frame"), i);
			continue;
		}

		PendingFrames.Add(MoveTemp(Pending));
	}
}

void UCaptureComponent::HarvestAndWriteReadyFrames()
{
	for (int32 i = PendingFrames.Num() - 1; i >= 0; --i)
	{
		FPendingFrame& Pending = PendingFrames[i];
		Pending.FramesWaiting++;

		if (!Pending.Rgb.IsReady() || !Pending.Dmv.IsReady())
		{
			if (Pending.FramesWaiting > MaxReadbackWaitFrames)
			{
				UE_LOG(LogTemp, Warning, TEXT("Dropping frame %d for camera %d (readback timed out after %d frames)"),
					Pending.FrameIndex, Pending.CameraIndex, Pending.FramesWaiting);
				PendingFrames.RemoveAt(i);
			}
			continue;
		}

		// Both copies have landed; collecting them now is a memcpy, not a stall.
		TArray<FLinearColor> rgb_data;
		TArray<FLinearColor> dmv_data;
		const int32			 RgbW = Pending.Rgb.Width;
		const int32			 RgbH = Pending.Rgb.Height;
		const int32			 DmvW = Pending.Dmv.Width;
		const int32			 DmvH = Pending.Dmv.Height;

		const bool bRgbOk = CameraCaptureUtils::HarvestLinearColor(Pending.Rgb, rgb_data);
		const bool bDmvOk = CameraCaptureUtils::HarvestLinearColor(Pending.Dmv, dmv_data);

		const int32 CameraIndex = Pending.CameraIndex;
		const int32 FrameIndex = Pending.FrameIndex;
		PendingFrames.RemoveAt(i);

		if (bRgbOk && bDmvOk)
		{
			WriteFrame(CameraIndex, FrameIndex, rgb_data, dmv_data, RgbW, RgbH, DmvW, DmvH);
		}
	}
}

void UCaptureComponent::WriteFrame(int32 CameraIndex, int32 FrameIndex,
	const TArray<FLinearColor>& rgb_data, const TArray<FLinearColor>& dmv_data,
	int32 RgbW, int32 RgbH, int32 DmvW, int32 DmvH)
{
	// A readback outlives the tick that queued it, so the camera it belongs to
	// can be gone by the time the pixels land.
	if (!RgbCameras.IsValidIndex(CameraIndex) || !RgbCameras[CameraIndex])
	{
		return;
	}
	auto rgb = RgbCameras[CameraIndex];

	// Get camera intrinsics
	UIntrinsicSceneCaptureComponent2D* IntrinsicCamera = Cast<UIntrinsicSceneCaptureComponent2D>(rgb);
	FCameraIntrinsics				   Intrinsics;
	if (IntrinsicCamera)
	{
		Intrinsics = IntrinsicCamera->GetActiveIntrinsics();
	}
	else
	{
		// Default intrinsics if not using IntrinsicSceneCaptureComponent2D
		Intrinsics.ImageWidth = RgbW;
		Intrinsics.ImageHeight = RgbH;
		Intrinsics.FocalLengthX = RgbW / 2.0f;
		Intrinsics.FocalLengthY = RgbH / 2.0f;
		Intrinsics.PrincipalPointX = RgbW / 2.0f;
		Intrinsics.PrincipalPointY = RgbH / 2.0f;
		Intrinsics.bMaintainYAxis = false;
	}

	// Setup output directories (actor-based, matching CameraCaptureManager)
	// Structure: SaveLocation/ActorName/CameraName/
	FString ActorName = GetOwner() ? GetOwner()->GetName() : TEXT("UnknownActor");
	FString CameraName = rgb->GetFName().ToString();
	FString CameraPath = FPaths::Combine(*SaveLocation, *ActorName, *CameraName);
	// Once per path, not once per frame: this was two filesystem calls per
	// camera per frame for a path that only changes when a camera is added.
	if (!DirectoriesEnsured.Contains(CameraPath))
	{
		if (!IFileManager::Get().DirectoryExists(*CameraPath))
		{
			IFileManager::Get().MakeDirectory(*CameraPath, true);
		}
		DirectoriesEnsured.Add(CameraPath);
	}

	// Generate frame filename (frame_0000000.exr format, matching CameraCaptureManager)
	FString FrameNumberStr = FString::Printf(TEXT("%07d"), FrameIndex);
	FString rgb_filename = FPaths::Combine(*CameraPath, FString::Printf(TEXT("frame_%s.exr"), *FrameNumberStr));
	FString dmv_filename = FPaths::Combine(*CameraPath, FString::Printf(TEXT("frame_%s_motion.exr"), *FrameNumberStr));
	FString metadata_filename = FPaths::Combine(*CameraPath, FString::Printf(TEXT("frame_%s.json"), *FrameNumberStr));

	// The depth/motion target can be a different size than the colour one when
	// the camera has separate depth intrinsics. Passing dmv_data with the colour
	// dimensions failed WriteEXRFile's size check, so the RGB+Depth EXR was never
	// written at all -- one error log per frame and no data.
	const bool bSameGrid = (DmvW == RgbW && DmvH == RgbH);

	TArray<FLinearColor> dmv_on_colour_grid;
	if (bSameGrid)
	{
		dmv_on_colour_grid = dmv_data;
	}
	else
	{
		// Only the depth channel has to reach the colour grid; the motion vectors
		// stay in their own file at their own resolution, because a resampled
		// vector no longer matches the pixels it was measured in.
		TArray<float> depth;
		depth.SetNumUninitialized(dmv_data.Num());
		for (int32 p = 0; p < dmv_data.Num(); ++p)
		{
			depth[p] = dmv_data[p].R;
		}

		const TArray<float> resampled = CameraCaptureUtils::ResampleDepthNearest(depth, DmvW, DmvH, RgbW, RgbH);

		if (resampled.Num() == RgbW * RgbH)
		{
			dmv_on_colour_grid.SetNumUninitialized(resampled.Num());
			for (int32 p = 0; p < resampled.Num(); ++p)
			{
				dmv_on_colour_grid[p] = FLinearColor(resampled[p], 0.0f, 0.0f, 0.0f);
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Could not resample depth %dx%d onto colour %dx%d for %s"),
				DmvW, DmvH, RgbW, RgbH, *CameraName);
		}
	}

	// Write RGB+Depth EXR (RGB in RGB channels, Depth in Alpha channel)
	if (dmv_on_colour_grid.Num() == rgb_data.Num())
	{
		CameraCaptureUtils::WriteEXRFile(rgb_filename, rgb_data, dmv_on_colour_grid, RgbW, RgbH, true);
	}

	// Write Motion Vectors EXR (X in R, Y in G channels) at the depth grid
	CameraCaptureUtils::WriteEXRFile(dmv_filename, dmv_data, dmv_data, DmvW, DmvH, false);

	// With differing grids the alpha channel above is resampled, so also emit the
	// measured depth at its own resolution.
	if (!bSameGrid)
	{
		const FString depth_filename = FPaths::Combine(*CameraPath, FString::Printf(TEXT("frame_%s_depth.exr"), *FrameNumberStr));
		CameraCaptureUtils::WriteEXRFile(depth_filename, dmv_data, dmv_data, DmvW, DmvH, true);
	}

	// Write metadata JSON
	FString ActorPath = GetOwner() ? GetOwner()->GetPathName() : TEXT("");
	FString LevelName = GetWorld() ? GetWorld()->GetName() : TEXT("");
	float	Timestamp = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

	CameraCaptureUtils::WriteMetadataFile(metadata_filename, rgb, Intrinsics, FrameIndex, Timestamp, ActorPath, LevelName);
}
