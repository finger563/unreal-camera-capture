#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "CameraIntrinsics.h"
#include "RHIGPUReadback.h"
#include "PixelFormat.h"
#include "Async/Async.h"
#include "CameraCaptureSubsystem.generated.h"

class UIntrinsicSceneCaptureComponent2D;

/**
 * Fired on the game thread after a frame has been harvested.
 * The payload is wrapped in a TSharedRef so listeners may safely hold a copy
 * beyond the scope of the broadcast (the underlying data is ref-counted).
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnFrameCaptured, TSharedRef<const FCaptureData> /*Data*/);

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

	/** Transform relative to the owning actor's root */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	FTransform RelativeTransform;

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

	/** Depth/motion width in pixels. A camera with separate depth intrinsics
	 *  captures depth at its own resolution, so DepthData and MotionVectorData
	 *  are NOT Width*Height in general -- index them by these instead. Zero
	 *  means no depth was captured; equal to Width/Height in the common case. */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	int32 DepthWidth = 0;

	/** Depth/motion height in pixels. See DepthWidth. */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	int32 DepthHeight = 0;

	/** True when depth was captured at a different resolution than colour, so a
	 *  consumer that needs them aligned has to resample one of them. */
	bool HasMismatchedDepthResolution() const
	{
		return DepthWidth > 0 && DepthHeight > 0 && (DepthWidth != Width || DepthHeight != Height);
	}

	/** Actor path in world */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	FString ActorPath;

	/** Level name */
	UPROPERTY(BlueprintReadOnly, Category = "Capture Data")
	FString LevelName;
};

/**
 * How the colour plane is written to disk.
 *
 * The capture targets are RGBA8, so colour is eight bits per channel. Writing
 * it as EXR expands every pixel to four 32-bit floats -- sixteen bytes on disk
 * for four bytes of information, plus the conversion. PNG keeps it at its
 * native depth and compresses it.
 */
UENUM(BlueprintType)
enum class ERammsCaptureColorFormat : uint8
{
	/** Colour and depth in one EXR, depth in the alpha channel. The original
	 *  behaviour, and what existing readers expect. */
	CombinedEXR UMETA(DisplayName = "Combined EXR (colour + depth in alpha)"),

	/** Colour as PNG, depth as its own EXR. Much smaller and cheaper, but the
	 *  two planes arrive as separate files. */
	SeparatePNGAndEXR UMETA(DisplayName = "Separate PNG colour + EXR depth")
};

/**
 * How many scene renders each camera costs.
 *
 * Measured at 640x480 colour + 320x240 depth, a camera costs 25-35 ms of frame
 * time, and almost all of it is rendering: the default mode renders the scene
 * TWICE per camera, once for colour and once through a post-process material
 * that writes depth and motion vectors. Serialization, by comparison, is under
 * a millisecond. So halving the renders is the one change that meaningfully
 * moves multi-camera cost.
 */
UENUM(BlueprintType)
enum class ERammsCaptureMode : uint8
{
	/**
	 * Two renders per camera: colour, plus a DMV pass for depth and motion
	 * vectors. Required for motion vectors, and the only mode where depth can
	 * have its own resolution.
	 */
	ColorPlusDepthMotion UMETA(DisplayName = "Colour + depth/motion (2 renders)"),

	/**
	 * One render per camera, using SCS_SceneColorSceneDepth: HDR scene colour in
	 * RGB and scene depth in alpha, straight from the engine.
	 *
	 * Roughly halves the per-camera cost. Three consequences worth knowing:
	 * motion vectors are not produced at all; colour is linear scene colour
	 * rather than the tone-mapped SCS_FinalColorHDR the other mode defaults to;
	 * and because both planes come out of one render target they necessarily
	 * share a resolution, so separate depth intrinsics are ignored.
	 *
	 * The depth is in centimetres, which is what FCaptureData has always
	 * documented -- the DMV material emits a normalised 0..1 instead.
	 */
	SingleCaptureColorDepth UMETA(DisplayName = "Single capture, colour + depth in alpha (1 render)")
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

	/** Enable/disable automatic serialization of captured data */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture")
	void SetSerializationEnabled(bool bEnabled);

	/** How the colour plane is written. Defaults to CombinedEXR, which is what
	 *  existing readers expect; SeparatePNGAndEXR is markedly cheaper. */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture")
	void SetColorFormat(ERammsCaptureColorFormat Format) { ColorFormat = Format; }

	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	ERammsCaptureColorFormat GetColorFormat() const { return ColorFormat; }

	/** How many renders each camera costs; see ERammsCaptureMode. Changing this
	 *  while capturing takes effect on the next StartCapture. */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture")
	void SetCaptureMode(ERammsCaptureMode Mode) { CaptureMode = Mode; }

	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	ERammsCaptureMode GetCaptureMode() const { return CaptureMode; }

	/** Check if serialization is enabled */
	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	bool IsSerializationEnabled() const { return bSerializationEnabled; }

	// ============================================================================
	// Statistics
	// ============================================================================

	/** Get capture statistics */
	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	FCaptureStatistics GetStatistics() const;

	/** Delegate fired after a frame has been harvested (game thread). */
	FOnFrameCaptured OnFrameCaptured;

protected:
	/** Execute synchronized capture across all cameras */
	void ExecuteSynchronizedCapture();

	/** Phase 1: Kick all scene captures and enqueue async GPU readbacks */
	void KickAllCaptures();

	/** Phase 2: Poll pending readbacks and harvest any that are ready */
	void HarvestReadyReadbacks();

	/** Enqueue an async GPU readback for a render target */
	void EnqueueAsyncReadback(UTextureRenderTarget2D* RenderTarget, TSharedPtr<FRHIGPUTextureReadback>& OutReadback);

	/** Build FCaptureData metadata (transform, intrinsics, etc.) without pixel data */
	FCaptureData BuildCaptureMetadata(UIntrinsicSceneCaptureComponent2D* Camera);

	/** Serialize capture data to disk (takes shared ownership, safe for async). */
	void SerializeCaptureData(TSharedRef<const FCaptureData> Data);

	/** Write EXR file with 6 channels (RGB + Depth + Motion) — called from background thread */
	static bool WriteEXRFile_Static(const FString& FilePath, const FCaptureData& Data, bool bCaptureRGB, bool bCaptureDepth, bool bCaptureMotionVectors,
		ERammsCaptureColorFormat Format = ERammsCaptureColorFormat::CombinedEXR);

	/** Write metadata JSON file — called from background thread */
	static bool WriteMetadataFile_Static(const FString& FilePath, const FCaptureData& Data,
		ERammsCaptureColorFormat Format = ERammsCaptureColorFormat::CombinedEXR);

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
		// Shared, not unique: the lock and copy happen in a render command that
		// outlives the game-thread entry this came from, and the continuation
		// that returns it to the pool runs later still.
		TSharedPtr<FRHIGPUTextureReadback> Readback;
		int32							   Width = 0;
		int32							   Height = 0;

		// The render target's actual pixel format, not a guess at its width. A
		// readback is raw GPU memory, so the harvest has to know the real stride:
		// this used to be a bool that grouped RGBA16f with RGBA32f, and since
		// RGBA16f is PF_FloatRGBA (8 bytes/pixel) while the harvest read
		// FLinearColor (16), it walked twice the staging buffer and corrupted the
		// heap. PF_Unknown means "do not interpret" rather than "assume 8-bit".
		EPixelFormat PixelFormat = PF_Unknown;

		/** Single-capture mode: alpha carries scene depth in cm, so the same
		 *  readback supplies both planes and there is no second one. */
		bool bDepthInAlpha = false;
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

	/**
	 * Readback objects kept for reuse rather than reallocated every frame.
	 *
	 * Each FRHIGPUTextureReadback owns a GPU staging buffer. Allocating one per
	 * camera per channel per capture frame meant creating and destroying two
	 * staging buffers per camera every frame; EnqueueCopy resizes an existing
	 * buffer when the texture changes, so the same object serves any camera.
	 */
	TArray<TSharedPtr<FRHIGPUTextureReadback>> ReadbackPool;

	/** Enough for several cameras with both channels in flight; past this,
	 *  returned readbacks are dropped rather than retained forever. */
	static constexpr int32 MaxPooledReadbacks = 32;

	/** Take a readback from the pool, or make one if the pool is empty. */
	TSharedPtr<FRHIGPUTextureReadback> AcquireReadback();

	/** Return a finished readback to the pool. Safe to call with null.
	 *  Game thread only -- the pool is not synchronised. */
	void ReleaseReadback(TSharedPtr<FRHIGPUTextureReadback> Readback);

	/**
	 * Extract pixel data from a completed RGB readback into FCaptureData.
	 *
	 * RENDER THREAD ONLY. FRHIGPUTextureReadback::Lock goes through
	 * FRHICommandListImmediate::Get(), which checks IsInRenderingThread() -- so
	 * calling this from Tick asserts and takes the editor with it.
	 */
	static void HarvestRgbReadback(FPendingReadback& Readback, FCaptureData& OutData);

	/** Extract pixel data from a completed DMV readback into FCaptureData.
	 *  RENDER THREAD ONLY; see HarvestRgbReadback. */
	static void HarvestDmvReadback(FPendingReadback& Readback, FCaptureData& OutData);

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

	/** Whether to automatically serialize captured data to disk */
	bool bSerializationEnabled = true;

	/** Colour output format; see ERammsCaptureColorFormat. */
	ERammsCaptureColorFormat ColorFormat = ERammsCaptureColorFormat::CombinedEXR;

	/** Renders per camera; see ERammsCaptureMode. Defaults to the two-render
	 *  mode, which is the existing behaviour and the only one that can produce
	 *  motion vectors. */
	ERammsCaptureMode CaptureMode = ERammsCaptureMode::ColorPlusDepthMotion;

	/** True when one render per camera supplies both planes. */
	bool IsSingleCaptureMode() const { return CaptureMode == ERammsCaptureMode::SingleCaptureColorDepth; }

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
