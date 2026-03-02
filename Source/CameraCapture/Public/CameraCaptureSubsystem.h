#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "CameraIntrinsics.h"
#include "RHIGPUReadback.h"
#include "Async/Async.h"
#include "CameraCaptureSubsystem.generated.h"

class UIntrinsicSceneCaptureComponent2D;

/**
 * Unique identifier for a camera component within the capture system
 */
USTRUCT(BlueprintType)
struct CAMERACAPTURE_API FCameraIdentifier
{
	GENERATED_BODY()

	/** Owner actor name (e.g., "Robot_BP_C_0") */
	UPROPERTY(BlueprintReadOnly, Category = "Camera Identifier")
	FString ActorName;

	/** Component name (e.g., "HeadCamera") */
	UPROPERTY(BlueprintReadOnly, Category = "Camera Identifier")
	FString ComponentName;

	/** Unique ID for logging/keys (e.g., "Robot_BP_C_0::HeadCamera") */
	UPROPERTY(BlueprintReadOnly, Category = "Camera Identifier")
	FString UniqueID;

	/** Fallback GUID if names collide */
	FGuid FallbackGUID;

	/** Generate identifier from a camera component */
	static FCameraIdentifier Generate(const UIntrinsicSceneCaptureComponent2D* Camera);

	/** Get actor directory name for filesystem */
	FString GetActorDirectoryName() const { return ActorName; }

	/** Get camera directory name for filesystem */
	FString GetCameraDirectoryName() const { return ComponentName; }

	/** Get full path: BaseDir/ActorName/CameraName */
	FString GetFullPath(const FString& BaseDir) const;

	/** String representation for logging */
	FString ToString() const { return UniqueID; }

	/** Comparison operator for TMap keys */
	bool operator==(const FCameraIdentifier& Other) const
	{
		return UniqueID == Other.UniqueID;
	}

	friend uint32 GetTypeHash(const FCameraIdentifier& Identifier)
	{
		return GetTypeHash(Identifier.UniqueID);
	}
};

/**
 * Data captured from a single camera in a single frame
 */
USTRUCT(BlueprintType)
struct CAMERACAPTURE_API FCaptureData
{
	GENERATED_BODY()

	/** Camera identity */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	FCameraIdentifier CameraID;

	/** Frame number in capture session */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	int64 FrameNumber = 0;

	/** Timestamp in seconds since capture started */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	double Timestamp = 0.0;

	/** World transform of the camera */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	FTransform WorldTransform;

	/** Camera intrinsics used for this capture */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	FCameraIntrinsics Intrinsics;

	/** Whether custom projection matrix was used */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	bool bUsedCustomProjectionMatrix = false;

	/** Projection matrix used (if custom) */
	FMatrix ProjectionMatrix;

	/** Image pixel data (RGBA) */
	TArray<FColor> ImageData;

	/** Depth data (in cm, world-space) */
	TArray<float> DepthData;

	/** Motion vector data (pixels per frame, 2D) */
	TArray<FVector2D> MotionVectorData;

	/** Image width in pixels */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	int32 Width = 0;

	/** Image height in pixels */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	int32 Height = 0;

	/** Actor path in world */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	FString ActorPath;

	/** Level name */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	FString LevelName;
};

/**
 * Capture statistics for monitoring performance
 */
USTRUCT(BlueprintType)
struct CAMERACAPTURE_API FCaptureStatistics
{
	GENERATED_BODY()

	/** Total frames captured across all cameras */
	UPROPERTY(BlueprintReadOnly, Category = "Statistics")
	int64 TotalFramesCaptured = 0;

	/** Number of currently registered cameras */
	UPROPERTY(BlueprintReadOnly, Category = "Statistics")
	int32 RegisteredCameraCount = 0;

	/** Average capture time per frame (milliseconds) */
	UPROPERTY(BlueprintReadOnly, Category = "Statistics")
	float AverageCaptureTimeMs = 0.0f;

	/** Last capture time (milliseconds) */
	UPROPERTY(BlueprintReadOnly, Category = "Statistics")
	float LastCaptureTimeMs = 0.0f;
};

/**
 * World subsystem for centralized camera capture management
 * Handles registration, synchronized capture, and serialization of multiple cameras
 */
UCLASS()
class CAMERACAPTURE_API UCameraCaptureSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	UCameraCaptureSubsystem();

	// USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableGameObject interface
	virtual void			  Tick(float DeltaTime) override;
	virtual TStatId			  GetStatId() const override;
	virtual bool			  IsTickable() const override;
	virtual bool			  IsTickableInEditor() const override { return false; }
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }
	virtual void			  OnWorldBeginPlay(UWorld& InWorld) override;

	// ============================================================================
	// Camera Registration
	// ============================================================================

	/** Register a camera component for centralized capture */
	void RegisterCamera(UIntrinsicSceneCaptureComponent2D* Camera);

	/** Unregister a camera component */
	void UnregisterCamera(UIntrinsicSceneCaptureComponent2D* Camera);

	/** Get number of registered cameras */
	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	int32 GetRegisteredCameraCount() const { return RegisteredCameras.Num(); }

	/** Get all registered cameras */
	TArray<UIntrinsicSceneCaptureComponent2D*> GetRegisteredCameras() const;

	// ============================================================================
	// Capture Control
	// ============================================================================

	/** Start capturing from all registered cameras */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture")
	void StartCapture();

	/** Stop capturing */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture")
	void StopCapture();

	/** Capture a single frame from all cameras */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture")
	void CaptureFrame();

	/** Check if currently capturing */
	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	bool IsCapturing() const { return bIsCapturing; }

	// ============================================================================
	// Configuration
	// ============================================================================

	/** Set how often to capture (1 = every frame, 2 = every other frame, etc.) */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture")
	void SetCaptureRate(int32 InCaptureEveryNFrames);

	/** Set output directory for captured data */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture")
	void SetOutputDirectory(const FString& Directory);

	/** Get current output directory */
	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	FString GetOutputDirectory() const { return OutputDirectory; }

	/** Set which channels to capture (RGB, Depth, Motion Vectors) */
	void SetCaptureChannels(bool bRGB, bool bDepth, bool bMotionVectors);

	/** Set the depth+motion capture material (M_DmvCapture) */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture")
	void SetDmvMaterial(UMaterial* Material);

	// ============================================================================
	// Statistics
	// ============================================================================

	/** Get capture statistics */
	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	FCaptureStatistics GetStatistics() const;

protected:
	/** Execute synchronized capture across all cameras */
	void ExecuteSynchronizedCapture();

	/** Phase 1: Kick all scene captures and enqueue async GPU readbacks */
	void KickAllCaptures();

	/** Phase 2: Poll pending readbacks and harvest any that are ready */
	void HarvestReadyReadbacks();

	/** Enqueue an async GPU readback for a render target */
	void EnqueueAsyncReadback(UTextureRenderTarget2D* RenderTarget, TUniquePtr<FRHIGPUTextureReadback>& OutReadback);

	/** Build FCaptureData metadata (transform, intrinsics, etc.) without pixel data */
	FCaptureData BuildCaptureMetadata(UIntrinsicSceneCaptureComponent2D* Camera);

	/** Serialize capture data to disk */
	void SerializeCaptureData(const FCaptureData& Data);

	/** Write EXR file with 6 channels (RGB + Depth + Motion) — called from background thread */
	static bool WriteEXRFile_Static(const FString& FilePath, const FCaptureData& Data, bool bCaptureRGB, bool bCaptureDepth, bool bCaptureMotionVectors);

	/** Write metadata JSON file — called from background thread */
	static bool WriteMetadataFile_Static(const FString& FilePath, const FCaptureData& Data);

	/** Generate unique camera ID, handling collisions */
	FCameraIdentifier GenerateCameraID(UIntrinsicSceneCaptureComponent2D* Camera);

	/** Check if actor name already exists and needs disambiguation */
	FString DisambiguateActorName(const FString& ActorName);

	/** Set up depth+motion capture camera for a registered RGB camera */
	void SetupDmvCamera(UIntrinsicSceneCaptureComponent2D* RgbCamera);

	/** Ensure camera has a render target assigned */
	void EnsureCameraRenderTarget(UIntrinsicSceneCaptureComponent2D* Camera);

	// ============================================================================
	// Async Readback State
	// ============================================================================

	/** Pending readback for a single camera's single channel (RGB or DMV) */
	struct FPendingReadback
	{
		TUniquePtr<FRHIGPUTextureReadback> Readback;
		int32							   Width = 0;
		int32							   Height = 0;
		bool							   bIsFloat = false; // true for RGBA32f (DMV), false for RGBA8 (RGB)
	};

	/** All pending state for a single camera in a single frame */
	struct FPendingCameraCapture
	{
		FCaptureData	 Metadata; // Pre-built metadata (no pixel data yet)
		FPendingReadback RgbReadback;
		FPendingReadback DmvReadback;
		bool			 bHasRgb = false;
		bool			 bHasDmv = false;
		int32			 FramesWaiting = 0; // Safety: drop after too many frames
	};

	/** Queue of pending captures awaiting GPU completion */
	TArray<FPendingCameraCapture> PendingCaptures;

	/** Maximum frames to wait for a readback before discarding */
	static constexpr int32 MaxReadbackWaitFrames = 10;

	/** Extract pixel data from a completed RGB readback into FCaptureData */
	void HarvestRgbReadback(FPendingReadback& Readback, FCaptureData& OutData);

	/** Extract pixel data from a completed DMV readback into FCaptureData */
	void HarvestDmvReadback(FPendingReadback& Readback, FCaptureData& OutData);

private:
	/** Registered cameras (weak pointers to handle component destruction) */
	TArray<TWeakObjectPtr<UIntrinsicSceneCaptureComponent2D>> RegisteredCameras;

	/** Map of camera to identifier (for fast lookup) */
	TMap<TWeakObjectPtr<UIntrinsicSceneCaptureComponent2D>, FCameraIdentifier> CameraIDMap;

	/** Set of used actor names (for collision detection) */
	TSet<FString> UsedActorNames;

	/** Is capture currently active */
	bool bIsCapturing = false;

	/** Capture every N frames (1 = every frame) */
	int32 CaptureEveryNFrames = 1;

	/** Current frame counter */
	int32 CurrentFrameCounter = 0;

	/** Unique frame ID counter for naming files */
	int64 FrameIdCounter = 0;

	/** Total frames captured this session */
	int64 TotalFramesCaptured = 0;

	/** Time when capture started */
	double CaptureStartTime = 0.0;

	/** Output directory for captured data */
	FString OutputDirectory;

	/** Which channels to capture */
	bool bCaptureRGB = true;
	bool bCaptureDepth = true;
	bool bCaptureMotionVectors = true;

	/** Last capture duration (for statistics) */
	float LastCaptureDurationMs = 0.0f;

	/** Running average of capture durations */
	float AverageCaptureTimeMs = 0.0f;

	/** Depth+Motion capture material (M_DmvCapture) */
	UPROPERTY()
	UMaterial* DmvCaptureMaterialBase = nullptr;

	/** Map of cameras to their depth+motion render targets */
	TMap<TWeakObjectPtr<UIntrinsicSceneCaptureComponent2D>, TWeakObjectPtr<UTextureRenderTarget2D>> DmvRenderTargets;

	/** Map of cameras to their depth+motion capture components */
	TMap<TWeakObjectPtr<UIntrinsicSceneCaptureComponent2D>, TWeakObjectPtr<USceneCaptureComponent2D>> DmvCameras;
};
