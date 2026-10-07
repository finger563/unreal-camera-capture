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
	 * One render per camera, using SCS_SceneColorSceneDepth: scene colour in RGB
	 * and scene depth in alpha, straight from the engine.
	 *
	 * The render target is float, but the COLOUR plane is not delivered as
	 * float: the harvest converts it through FColor, so what reaches
	 * FCaptureData::ImageData is 8 bits per channel of linear scene colour --
	 * which is a poor fit for linear values, since without a transfer curve the
	 * quantisation falls almost entirely in the darks. Depth keeps its full float
	 * precision; it is read out of alpha directly. Carrying colour as float would
	 * mean widening FCaptureData, and every consumer of it, which has not been
	 * done.
	 *
	 * Roughly halves the per-camera cost. Three consequences worth knowing:
	 * motion vectors are not produced at all; colour is linear scene colour
	 * rather than the tone-mapped image ColorCaptureSource selects for the other
	 * mode (SCS_FinalColorLDR by default); and because both planes come out of
	 * one render target they necessarily share a resolution, so separate depth
	 * intrinsics are ignored.
	 *
	 * Depth is in centimetres, as FCaptureData documents. So is the other mode's:
	 * the DMV material reads SceneDepth unscaled, and SceneDepth is already
	 * centimetres. Both modes produce centimetres, measured differently.
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

	/**
	 * The depth/motion render target for a camera, or null.
	 *
	 * Null is a normal answer, not a failure: in SingleCaptureColorDepth mode
	 * there is no second target at all -- depth rides in the colour target's
	 * alpha -- and a camera that has not been set up yet has none either. A
	 * viewer wanting the colour feed reads Camera->TextureTarget directly.
	 */
	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	UTextureRenderTarget2D* GetDepthRenderTarget(UIntrinsicSceneCaptureComponent2D* Camera) const;

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

	/** Frames between captures; 1 means every frame. */
	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	int32 GetCaptureRate() const { return CaptureEveryNFrames; }

	/** Set output directory for captured data */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture")
	void SetOutputDirectory(const FString& Directory);

	/** Get current output directory */
	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	FString GetOutputDirectory() const { return OutputDirectory; }

	/** Set which channels to capture (RGB, Depth, Motion Vectors) */
	void SetCaptureChannels(bool bRGB, bool bDepth, bool bMotionVectors);

	/**
	 * Which channels are being captured.
	 *
	 * For a viewer rather than for the capture path: a UI offering a depth or
	 * motion view has to know whether there is anything behind it, and motion in
	 * particular is only ever produced by the DMV pass -- see GetCaptureMode.
	 */
	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	bool IsCapturingRGB() const { return bCaptureRGB; }

	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	bool IsCapturingDepth() const { return bCaptureDepth; }

	/** True only when motion vectors are both requested AND producible: single
	 *  capture mode has no DMV pass, so it never produces them whatever was
	 *  asked for. */
	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	bool IsCapturingMotionVectors() const { return bCaptureMotionVectors && !IsSingleCaptureMode(); }

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

	/** How many renders each camera costs; see ERammsCaptureMode. Safe to change
	 *  at any time, including mid-capture: registered cameras are reconfigured
	 *  for the new mode on the spot, since the two modes need different render
	 *  target formats and only one of them needs a second camera. */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture")
	void SetCaptureMode(ERammsCaptureMode Mode);

	/** Which image the colour cameras capture. See ColorCaptureSource. */
	UFUNCTION(BlueprintCallable, Category = "Camera Capture")
	void SetColorCaptureSource(TEnumAsByte<ESceneCaptureSource> Source) { ColorCaptureSource = Source; }

	UFUNCTION(BlueprintPure, Category = "Camera Capture")
	TEnumAsByte<ESceneCaptureSource> GetColorCaptureSource() const { return ColorCaptureSource; }

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
	void EnqueueAsyncReadback(UTextureRenderTarget2D* RenderTarget, TSharedPtr<FRHIGPUTextureReadback>& OutReadback,
		TSharedPtr<FThreadSafeBool>& OutCopyIssued);

	/** Build FCaptureData metadata (transform, intrinsics, etc.) without pixel data */
	FCaptureData BuildCaptureMetadata(UIntrinsicSceneCaptureComponent2D* Camera);

	/** Serialize capture data to disk (takes shared ownership, safe for async). */
	void SerializeCaptureData(TSharedRef<const FCaptureData> Data);

	/** Write EXR file with 6 channels (RGB + Depth + Motion) — called from background thread */
	static bool WriteEXRFile_Static(const FString& FilePath, const FCaptureData& Data, bool bCaptureRGB, bool bCaptureDepth, bool bCaptureMotionVectors,
		ERammsCaptureColorFormat Format = ERammsCaptureColorFormat::CombinedEXR);

	/** Write metadata JSON file — called from background thread.
	 *  bCaptureDepth is needed because the file reports what was actually
	 *  written, not what the geometry would allow. */
	static bool WriteMetadataFile_Static(const FString& FilePath, const FCaptureData& Data,
		ERammsCaptureColorFormat Format = ERammsCaptureColorFormat::CombinedEXR, bool bCaptureDepth = true);

	/** Generate unique camera ID, handling collisions */
	FCameraIdentifier GenerateCameraID(UIntrinsicSceneCaptureComponent2D* Camera);

	/** Check if actor name already exists and needs disambiguation */
	FString DisambiguateActorName(const FString& ActorName);

	/** Set up depth+motion capture camera for a registered RGB camera */
	void SetupDmvCamera(UIntrinsicSceneCaptureComponent2D* RgbCamera);

	/**
	 * Bring already-registered cameras in line with the current CaptureMode.
	 *
	 * The two modes are not interchangeable at the resource level: single
	 * capture needs a float target so alpha can hold a distance and needs no
	 * second camera, while the two-render mode needs the DMV camera that
	 * produces depth and motion. Flipping the enum alone left cameras configured
	 * for the mode they were registered under.
	 */
	void ReconfigureCamerasForCaptureMode();

	/** Ensure camera has a render target assigned */
	void EnsureCameraRenderTarget(UIntrinsicSceneCaptureComponent2D* Camera);

	// ============================================================================
	// Async Readback State
	// ============================================================================

	/** Pending readback for a single camera's single channel (RGB or DMV) */
	/**
	 * What a pooled readback's staging texture is shaped for.
	 *
	 * A readback may only be reused for a texture of the SAME size and format.
	 * FRHIGPUTextureReadback::EnqueueCopy recreates its staging texture only
	 * when the texture DIMENSION changes (2D vs 3D), and on platforms that read
	 * back through 2D textures only it never recreates one at all -- the engine
	 * says so itself: "Assume for now that every enqueue happens on a texture of
	 * the same format and size (when reused)."
	 *
	 * So a readback is welded to the geometry of the first texture it ever saw.
	 * Handing a 640x360 one to a 1280x720 camera copied into the smaller staging
	 * texture and Lock() then reported pitch 640, buffer height 360, which the
	 * harvest's geometry guard rejected -- every frame, for every camera that
	 * drew a readback shaped for a different one.
	 */
	struct FReadbackShape
	{
		int32		 Width = 0;
		int32		 Height = 0;
		EPixelFormat Format = PF_Unknown;

		bool operator==(const FReadbackShape& Other) const
		{
			return Width == Other.Width && Height == Other.Height && Format == Other.Format;
		}

		friend uint32 GetTypeHash(const FReadbackShape& Shape)
		{
			return HashCombine(HashCombine(::GetTypeHash(Shape.Width), ::GetTypeHash(Shape.Height)),
				::GetTypeHash(static_cast<int32>(Shape.Format)));
		}
	};

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

		/**
		 * Set by the render thread once EnqueueCopy has actually been issued.
		 *
		 * Readbacks are pooled, and a pooled one arrives with its fence still
		 * SIGNALLED from its previous use. IsReady() therefore answered true the
		 * moment it was handed out -- before the new copy had been issued -- so
		 * the harvest read the staging buffer from an earlier frame and kept
		 * reading it. Depth and motion froze on the first frame they ever
		 * captured while colour carried on updating.
		 *
		 * EnqueueCopy clears the fence as its first act, and that happens on the
		 * render thread, so once this is true the fence describes the new copy
		 * and IsReady() means what it says.
		 */
		TSharedPtr<FThreadSafeBool> CopyIssued;

		/** True when the copy has been issued AND has completed. */
		bool IsReadyForHarvest() const
		{
			return Readback.IsValid() && CopyIssued.IsValid() && *CopyIssued && Readback->IsReady();
		}

		/** The shape this readback's staging texture was built for, and so the
		 *  only shape it can be reused at. */
		FReadbackShape GetShape() const
		{
			return FReadbackShape{ Width, Height, PixelFormat };
		}
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
	 * Readback objects kept for reuse rather than reallocated every frame,
	 * bucketed by the shape they can serve.
	 *
	 * Each FRHIGPUTextureReadback owns a GPU staging texture. Allocating one per
	 * camera per channel per capture frame meant creating and destroying two of
	 * them per camera every frame. Reuse is still worth having -- a camera's
	 * shape does not change between frames, so the steady state is a hit -- it
	 * just cannot be shared across shapes.
	 */
	TMap<FReadbackShape, TArray<TSharedPtr<FRHIGPUTextureReadback>>> ReadbackPool;

	/** Enough for several cameras with both channels in flight; past this,
	 *  returned readbacks are dropped rather than retained forever. */
	static constexpr int32 MaxPooledReadbacks = 32;

	/** How many readbacks are pooled across every shape. */
	int32 CountPooledReadbacks() const;

	/** Take a readback shaped for this texture, or make one. */
	TSharedPtr<FRHIGPUTextureReadback> AcquireReadback(const FReadbackShape& Shape);

	/** Return a finished readback to the pool, under the shape it was used at.
	 *  Safe to call with null. Game thread only -- the pool is not
	 *  synchronised. */
	void ReleaseReadback(TSharedPtr<FRHIGPUTextureReadback> Readback, const FReadbackShape& Shape);

	/**
	 * Check a readback against the staging texture it actually owns, before
	 * anything strides through it. Render thread only.
	 *
	 * The pitch/height guard inside each harvest works in PIXELS and so cannot
	 * see a FORMAT disagreement -- which is the dangerous one, because reading a
	 * 4-byte-per-pixel buffer at 16 walks four times its length and takes the
	 * render thread down. Keyed pooling should make that unreachable; this is
	 * here because the cost of being wrong is a heap overrun and not a bad frame.
	 */
	static bool ReadbackMatchesItsStagingTexture(const FPendingReadback& Readback, const TCHAR* Label);

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

	/**
	 * Which image the colour cameras capture.
	 *
	 * Not the engine default. USceneCaptureComponent2D starts at
	 * SCS_SceneColorHDR -- "SceneColor (HDR) in RGB, Inv Opacity in A" -- which
	 * is the scene before tone mapping, before exposure and before any post
	 * processing, with inverted opacity where alpha should be. Written into the
	 * RGBA8 target this subsystem creates, everything above 1.0 clamps and the
	 * rest lands in the wrong transfer function: lit opaque geometry comes out
	 * visibly wrong while translucent surfaces, composited differently, come out
	 * looking fine. That is a confusing failure to look at and it was the
	 * default for every camera this subsystem drove.
	 *
	 * SCS_FinalColorLDR is the fully post-processed, tone-mapped, display-
	 * referred image -- what the camera actually sees, and what belongs in an
	 * 8-bit target.
	 */
	TEnumAsByte<ESceneCaptureSource> ColorCaptureSource = SCS_FinalColorLDR;

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
