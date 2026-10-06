#pragma once

#include "CoreMinimal.h"
#include "CameraIntrinsics.h"
#include "RHIGPUReadback.h"
#include "PixelFormat.h"
#include "Dom/JsonObject.h"

// Forward declarations
class USceneCaptureComponent2D;

template <typename ObjClass>
static FORCEINLINE ObjClass* LoadObjFromPath(const FName& Path)
{
	if (Path == NAME_None)
		return nullptr;

	return Cast<ObjClass>(StaticLoadObject(ObjClass::StaticClass(), nullptr, *Path.ToString()));
}

static FORCEINLINE UMaterial* LoadMaterialFromPath(const FName& Path)
{
	if (Path == NAME_None)
		return nullptr;

	return LoadObjFromPath<UMaterial>(Path);
}

/**
 * Shared utilities for camera capture functionality
 */
namespace CameraCaptureUtils
{
	/**
	 * Convert an FTransform to a JSON object with location, rotation, quaternion, and scale.
	 * Location is in Unreal units (cm), rotation in degrees [pitch, yaw, roll],
	 * quaternion as [w, x, y, z], and scale as [x, y, z].
	 */
	TSharedPtr<FJsonObject> TransformToJsonObject(const FTransform& Transform);

	/**
	 * A GPU readback in flight, plus somewhere for its pixels to land.
	 *
	 * The alternative, UTextureRenderTarget2D::ReadLinearColorPixels, ends in
	 * FlushRenderingCommands() -- it blocks the game thread until the GPU has
	 * finished. Enqueueing a copy and collecting it a frame or two later costs
	 * latency instead of a stall.
	 *
	 * Collecting it is a two-step affair because FRHIGPUTextureReadback::Lock
	 * goes through FRHICommandListImmediate::Get(), which checks
	 * IsInRenderingThread(): poll IsReady() on the game thread, then
	 * BeginHarvestLinearColor to do the actual copy on the render thread, then
	 * wait for IsHarvested(). Locking straight from the game thread asserts.
	 */
	struct FAsyncReadback
	{
		TSharedPtr<FRHIGPUTextureReadback> Readback;
		int32							   Width = 0;
		int32							   Height = 0;
		/** The source format, so the harvest knows the real stride rather than
		 *  guessing one. PF_Unknown means "do not interpret". */
		EPixelFormat PixelFormat = PF_Unknown;

		/** Filled by the render thread; shared so it outlives this struct. */
		TSharedPtr<TArray<FLinearColor>> Pixels;
		/** Set by the render thread when Pixels is complete, or on failure. */
		TSharedPtr<FThreadSafeBool> Done;
		/** Set alongside Done when the copy could not be made. */
		TSharedPtr<FThreadSafeBool> Failed;

		bool IsPending() const { return Readback.IsValid(); }
		bool IsCopyReady() const { return Readback.IsValid() && Readback->IsReady(); }
		bool IsHarvesting() const { return Done.IsValid(); }
		bool IsHarvested() const { return Done.IsValid() && *Done; }
		bool HasFailed() const { return Failed.IsValid() && *Failed; }
		void Reset()
		{
			Readback.Reset();
			Pixels.Reset();
			Done.Reset();
			Failed.Reset();
		}
	};

	/** Start a copy of RenderTarget into Out. Returns false if it could not be
	 *  started, in which case Out is left not pending. */
	bool EnqueueReadback(UTextureRenderTarget2D* RenderTarget, FAsyncReadback& Out);

	/**
	 * Kick the lock-and-copy onto the render thread. Call once IsCopyReady();
	 * poll IsHarvested() afterwards and read Pixels.
	 *
	 * Handles RGBA8, RGBA16f and RGBA32f sources and refuses anything else
	 * rather than reading at the wrong stride (HasFailed then reports it).
	 */
	bool BeginHarvestLinearColor(FAsyncReadback& Readback);

	/**
	 * Nearest-neighbour resample of a single-channel plane (depth).
	 *
	 * Nearest rather than bilinear on purpose: depth is not a continuous signal
	 * across an object boundary, and averaging across one invents a surface that
	 * is in front of the far object and behind the near one. A resampled depth
	 * map should contain only distances the scene actually had.
	 *
	 * Returns Src unchanged when the sizes already agree, and an empty array if
	 * any dimension is non-positive or Src is not SrcW*SrcH long.
	 */
	TArray<float> ResampleDepthNearest(const TArray<float>& Src, int32 SrcW, int32 SrcH, int32 DstW, int32 DstH);

	/**
	 * Enqueue an already-composed pixel buffer as an EXR.
	 *
	 * This is the one place that talks to ImageWriteQueue. Callers that already
	 * have the final pixels hand them over directly instead of building a
	 * separate RGB and DMV array for a helper to interleave -- the old path cost
	 * three full-frame buffers per file to emit one.
	 *
	 * Takes Pixels by value so a caller can MoveTemp into it; the buffer is then
	 * moved again into the write task and never copied.
	 */
	/**
	 * Enqueue an 8-bit colour buffer as a PNG.
	 *
	 * The capture targets are RGBA8, so this writes colour at the depth it was
	 * captured at instead of expanding every pixel to four 32-bit floats.
	 */
	bool WritePNGPixels(const FString& FilePath, TArray64<FColor> Pixels, int32 Width, int32 Height);

	bool WriteEXRPixels(const FString& FilePath, TArray64<FLinearColor> Pixels, int32 Width, int32 Height);

	/**
	 * Write image data to EXR file using ImageWriteQueue
	 * @param FilePath - Output file path
	 * @param RgbData - RGB image data
	 * @param DmvData - Depth/Motion/Velocity data (depth in R, motion X in G, motion Y in B)
	 * @param Width - Image width
	 * @param Height - Image height
	 * @param bIncludeDepth - If true, stores RGB+Depth (depth in alpha). If false, stores motion vectors (X in R, Y in G)
	 * @return true if write task was successfully queued
	 */
	bool WriteEXRFile(const FString& FilePath,
		const TArray<FLinearColor>&	 RgbData,
		const TArray<FLinearColor>&	 DmvData,
		int32						 Width,
		int32						 Height,
		bool						 bIncludeDepth);

	/**
	 * Write metadata JSON file with camera transform and intrinsics
	 * @param FilePath - Output JSON file path
	 * @param Camera - Scene capture component
	 * @param Intrinsics - Camera intrinsics
	 * @param FrameNumber - Frame number for this capture
	 * @param Timestamp - World time in seconds
	 * @param ActorPath - Full path to owning actor (optional)
	 * @param LevelName - Level name (optional)
	 * @return true if file was successfully written
	 */
	bool WriteMetadataFile(const FString& FilePath,
		USceneCaptureComponent2D*		  Camera,
		const FCameraIntrinsics&		  Intrinsics,
		int32							  FrameNumber,
		float							  Timestamp,
		const FString&					  ActorPath = TEXT(""),
		const FString&					  LevelName = TEXT(""));

	/**
	 * Draw a camera frustum for visualization using projection matrix
	 * @param World - World to draw in
	 * @param CameraTransform - Camera world transform (scale will be ignored)
	 * @param ProjectionMatrix - Projection matrix to visualize
	 * @param NearDistance - Near plane distance (in cm)
	 * @param FarDistance - Far plane distance (in cm)
	 * @param LineColor - Color for frustum lines
	 * @param LineThickness - Thickness of frustum lines
	 * @param bDrawPlanes - Whether to draw filled frustum planes
	 * @param PlaneColor - Color for frustum planes (with alpha)
	 */
	void DrawFrustumFromProjectionMatrix(UWorld* World,
		const FTransform&						 CameraTransform,
		const FMatrix&							 ProjectionMatrix,
		float									 NearDistance,
		float									 FarDistance,
		const FColor&							 LineColor,
		float									 LineThickness,
		bool									 bDrawPlanes,
		const FLinearColor&						 PlaneColor);

	/**
	 * Draw a camera frustum for visualization using intrinsics
	 * @param World - World to draw in
	 * @param CameraTransform - Camera world transform (scale will be ignored)
	 * @param Intrinsics - Camera intrinsics
	 * @param NearDistance - Near plane distance (in cm)
	 * @param FarDistance - Far plane distance (in cm)
	 * @param LineColor - Color for frustum lines
	 * @param LineThickness - Thickness of frustum lines
	 * @param bDrawPlanes - Whether to draw filled frustum planes
	 * @param PlaneColor - Color for frustum planes (with alpha)
	 */
	void DrawFrustumFromIntrinsics(UWorld* World,
		const FTransform&				   CameraTransform,
		const FCameraIntrinsics&		   Intrinsics,
		float							   NearDistance,
		float							   FarDistance,
		const FColor&					   LineColor,
		float							   LineThickness,
		bool							   bDrawPlanes,
		const FLinearColor&				   PlaneColor);
} // namespace CameraCaptureUtils
