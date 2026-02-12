#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CameraCaptureManager.generated.h"

class UIntrinsicSceneCaptureComponent2D;
class UCameraCaptureSubsystem;

/**
 * Camera registration mode for determining which cameras to capture
 */
UENUM(BlueprintType)
enum class ECameraRegistrationMode : uint8
{
	/** Capture all IntrinsicSceneCaptureComponent2D cameras found in the level */
	AllInLevel UMETA(DisplayName = "All In Level"),
	
	/** Only capture cameras explicitly added to the CamerasToCapture array */
	Manual UMETA(DisplayName = "Manual Selection")
};

/**
 * Manager actor for centralized camera capture control
 * Place one of these in your level to configure and control multi-camera capture
 */
UCLASS(Blueprintable, ClassGroup = "Camera Capture", meta = (BlueprintSpawnableComponent))
class CAMERACAPTURE_API ACameraCaptureManager : public AActor
{
	GENERATED_BODY()

public:
	ACameraCaptureManager();

	// ============================================================================
	// Output Configuration
	// ============================================================================

	/** Output directory for captured data (supports absolute paths or project-relative) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output", meta = (DisplayName = "Output Directory"))
	FString OutputDirectory = TEXT("Saved/CameraCaptures");

	// ============================================================================
	// Capture Configuration
	// ============================================================================

	/** How often to capture (1 = every frame, 2 = every other frame, etc.) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture", meta = (ClampMin = "1", DisplayName = "Capture Every N Frames"))
	int32 CaptureEveryNFrames = 1;

	/** Automatically start capture when play begins */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture", meta = (DisplayName = "Auto Start On Begin Play"))
	bool bAutoStartOnBeginPlay = false;

	/** Capture RGB color data */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture", meta = (DisplayName = "Capture RGB"))
	bool bCaptureRGB = true;

	/** Capture depth data (world-space in cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture", meta = (DisplayName = "Capture Depth"))
	bool bCaptureDepth = true;

	/** Capture motion vector data (screen-space velocity) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture", meta = (DisplayName = "Capture Motion Vectors"))
	bool bCaptureMotionVectors = true;

	// ============================================================================
	// Camera Registration
	// ============================================================================

	/** How to determine which cameras to capture from */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cameras", meta = (DisplayName = "Registration Mode"))
	ECameraRegistrationMode RegistrationMode = ECameraRegistrationMode::AllInLevel;

	/** Explicit list of cameras to capture (only used in Manual mode) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cameras", meta = (EditCondition = "RegistrationMode == ECameraRegistrationMode::Manual", DisplayName = "Cameras To Capture"))
	TArray<UIntrinsicSceneCaptureComponent2D*> CamerasToCapture;

	// ============================================================================
	// Runtime Control (Blueprint API)
	// ============================================================================

	/** Start capturing from registered cameras */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture", meta = (DisplayName = "Start Capture"))
	void StartCapture();

	/** Stop capturing */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture", meta = (DisplayName = "Stop Capture"))
	void StopCapture();

	/** Capture a single frame */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture", meta = (DisplayName = "Capture Single Frame"))
	void CaptureSingleFrame();

	/** Check if currently capturing */
	UFUNCTION(BlueprintPure, Category = "Camera Capture", meta = (DisplayName = "Is Capturing"))
	bool IsCapturing() const;

	/** Get number of registered cameras */
	UFUNCTION(BlueprintPure, Category = "Camera Capture", meta = (DisplayName = "Get Camera Count"))
	int32 GetRegisteredCameraCount() const;

	/** Get total frames captured this session */
	UFUNCTION(BlueprintPure, Category = "Camera Capture", meta = (DisplayName = "Get Total Frames Captured"))
	int64 GetTotalFramesCaptured() const;

protected:
	// AActor interface
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Register cameras with subsystem based on current mode */
	void RegisterCameras();

	/** Unregister all cameras */
	void UnregisterAllCameras();

	/** Find all IntrinsicSceneCaptureComponent2D cameras in the level */
	TArray<UIntrinsicSceneCaptureComponent2D*> FindAllCamerasInLevel() const;

	/** Get the capture subsystem */
	UCameraCaptureSubsystem* GetCaptureSubsystem() const;

	/** Check if another manager exists in the level (for single-manager enforcement) */
	void CheckForMultipleManagers();

private:
	/** Cached reference to subsystem */
	UPROPERTY(Transient)
	UCameraCaptureSubsystem* CachedSubsystem = nullptr;

	/** Has this manager been initialized */
	bool bInitialized = false;
};
