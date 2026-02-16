#include "CameraCaptureManager.h"
#include "CameraCaptureSubsystem.h"
#include "IntrinsicSceneCaptureComponent2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"

ACameraCaptureManager::ACameraCaptureManager()
{
	PrimaryActorTick.bCanEverTick = false;

	// Make this actor visible in editor
	bIsEditorOnlyActor = false;

#if WITH_EDITORONLY_DATA
	bListedInSceneOutliner = true;
#endif
}

void ACameraCaptureManager::BeginPlay()
{
	Super::BeginPlay();

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] BeginPlay started"));

	// Check for multiple managers (warn if found)
	CheckForMultipleManagers();

	// Get subsystem
	CachedSubsystem = GetCaptureSubsystem();
	if (!CachedSubsystem)
	{
		UE_LOG(LogTemp, Error, TEXT("[CameraCaptureManager] Failed to get CameraCaptureSubsystem"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] Got subsystem, configuring..."));

	// Configure subsystem
	CachedSubsystem->SetOutputDirectory(OutputDirectory);
	CachedSubsystem->SetCaptureRate(CaptureEveryNFrames);
	CachedSubsystem->SetCaptureChannels(bCaptureRGB, bCaptureDepth, bCaptureMotionVectors);

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] Configuration complete, registering cameras..."));

	// Register cameras
	RegisterCameras();

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] Cameras registered"));

	bInitialized = true;

	// Auto-start if configured
	if (bAutoStartOnBeginPlay)
	{
		UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] Auto-starting capture..."));
		StartCapture();
	}

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] Initialized with %d cameras"), GetRegisteredCameraCount());
}

void ACameraCaptureManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Stop capture if active
	if (IsCapturing())
	{
		StopCapture();
	}

	// Unregister cameras
	UnregisterAllCameras();

	CachedSubsystem = nullptr;
	bInitialized = false;

	Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
void ACameraCaptureManager::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (!bInitialized || !CachedSubsystem)
	{
		return;
	}

	// Update subsystem configuration when properties change
	if (PropertyChangedEvent.MemberProperty)
	{
		FName PropertyName = PropertyChangedEvent.MemberProperty->GetFName();

		if (PropertyName == GET_MEMBER_NAME_CHECKED(ACameraCaptureManager, OutputDirectory))
		{
			CachedSubsystem->SetOutputDirectory(OutputDirectory);
		}
		else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACameraCaptureManager, CaptureEveryNFrames))
		{
			CachedSubsystem->SetCaptureRate(CaptureEveryNFrames);
		}
		else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACameraCaptureManager, bCaptureRGB) || PropertyName == GET_MEMBER_NAME_CHECKED(ACameraCaptureManager, bCaptureDepth) || PropertyName == GET_MEMBER_NAME_CHECKED(ACameraCaptureManager, bCaptureMotionVectors))
		{
			CachedSubsystem->SetCaptureChannels(bCaptureRGB, bCaptureDepth, bCaptureMotionVectors);
		}
		else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACameraCaptureManager, RegistrationMode) || PropertyName == GET_MEMBER_NAME_CHECKED(ACameraCaptureManager, CamerasToCapture))
		{
			// Re-register cameras when mode or list changes
			UnregisterAllCameras();
			RegisterCameras();
		}
	}
}
#endif

// ============================================================================
// Runtime Control
// ============================================================================

void ACameraCaptureManager::StartCapture()
{
	UCameraCaptureSubsystem* Subsystem = GetCaptureSubsystem();
	if (Subsystem)
	{
		Subsystem->StartCapture();
		UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] Started capture"));
	}
}

void ACameraCaptureManager::StopCapture()
{
	UCameraCaptureSubsystem* Subsystem = GetCaptureSubsystem();
	if (Subsystem)
	{
		Subsystem->StopCapture();
		UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] Stopped capture"));
	}
}

void ACameraCaptureManager::CaptureSingleFrame()
{
	UCameraCaptureSubsystem* Subsystem = GetCaptureSubsystem();
	if (Subsystem)
	{
		Subsystem->CaptureFrame();
	}
}

bool ACameraCaptureManager::IsCapturing() const
{
	UCameraCaptureSubsystem* Subsystem = GetCaptureSubsystem();
	return Subsystem ? Subsystem->IsCapturing() : false;
}

int32 ACameraCaptureManager::GetRegisteredCameraCount() const
{
	UCameraCaptureSubsystem* Subsystem = GetCaptureSubsystem();
	return Subsystem ? Subsystem->GetRegisteredCameraCount() : 0;
}

int64 ACameraCaptureManager::GetTotalFramesCaptured() const
{
	UCameraCaptureSubsystem* Subsystem = GetCaptureSubsystem();
	if (Subsystem)
	{
		FCaptureStatistics Stats = Subsystem->GetStatistics();
		return Stats.TotalFramesCaptured;
	}
	return 0;
}

// ============================================================================
// Camera Registration
// ============================================================================

void ACameraCaptureManager::RegisterCameras()
{
	UCameraCaptureSubsystem* Subsystem = GetCaptureSubsystem();
	if (!Subsystem)
	{
		return;
	}

	TArray<UIntrinsicSceneCaptureComponent2D*> CamerasToRegister;

	switch (RegistrationMode)
	{
		case ECameraRegistrationMode::AllInLevel:
		{
			// Find all cameras in level
			CamerasToRegister = FindAllCamerasInLevel();
			UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] AllInLevel mode: Found %d cameras"), CamerasToRegister.Num());
			break;
		}

		case ECameraRegistrationMode::Manual:
		{
			// Use explicit list
			CamerasToRegister = CamerasToCapture;
			UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] Manual mode: Using %d cameras from list"), CamerasToRegister.Num());
			break;
		}
	}

	// Register each camera
	for (UIntrinsicSceneCaptureComponent2D* Camera : CamerasToRegister)
	{
		if (Camera)
		{
			Subsystem->RegisterCamera(Camera);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] Registered %d cameras"), CamerasToRegister.Num());
}

void ACameraCaptureManager::UnregisterAllCameras()
{
	UCameraCaptureSubsystem* Subsystem = GetCaptureSubsystem();
	if (!Subsystem)
	{
		return;
	}

	// Get all registered cameras and unregister them
	TArray<UIntrinsicSceneCaptureComponent2D*> RegisteredCameras = Subsystem->GetRegisteredCameras();
	for (UIntrinsicSceneCaptureComponent2D* Camera : RegisteredCameras)
	{
		if (Camera)
		{
			Subsystem->UnregisterCamera(Camera);
		}
	}
}

TArray<UIntrinsicSceneCaptureComponent2D*> ACameraCaptureManager::FindAllCamerasInLevel() const
{
	TArray<UIntrinsicSceneCaptureComponent2D*> FoundCameras;

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureManager] FindAllCamerasInLevel: No world"));
		return FoundCameras;
	}

	int32 TotalActorsChecked = 0;
	int32 TotalCameraComponentsFound = 0;

	// Iterate all actors in world
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		TotalActorsChecked++;

		// Find all IntrinsicSceneCaptureComponent2D components on this actor
		TArray<UIntrinsicSceneCaptureComponent2D*> CameraComponents;
		Actor->GetComponents<UIntrinsicSceneCaptureComponent2D>(CameraComponents);

		for (UIntrinsicSceneCaptureComponent2D* Camera : CameraComponents)
		{
			if (Camera)
			{
				TotalCameraComponentsFound++;
				FoundCameras.Add(Camera);

				UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] Found camera: %s on actor: %s"),
					*Camera->GetName(), *Actor->GetName());
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[CameraCaptureManager] Search complete: %d actors checked, %d camera components found"),
		TotalActorsChecked, TotalCameraComponentsFound);

	return FoundCameras;
}

// ============================================================================
// Helpers
// ============================================================================

UCameraCaptureSubsystem* ACameraCaptureManager::GetCaptureSubsystem() const
{
	if (CachedSubsystem)
	{
		return CachedSubsystem;
	}

	UWorld* World = GetWorld();
	if (World)
	{
		return World->GetSubsystem<UCameraCaptureSubsystem>();
	}

	return nullptr;
}

void ACameraCaptureManager::CheckForMultipleManagers()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	int32 ManagerCount = 0;
	for (TActorIterator<ACameraCaptureManager> It(World); It; ++It)
	{
		ManagerCount++;
	}

	if (ManagerCount > 1)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CameraCaptureManager] Multiple CameraCaptureManager actors found in level (%d). Only one manager per level is recommended."), ManagerCount);
	}
}
