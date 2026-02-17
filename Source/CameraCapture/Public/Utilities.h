#pragma once

#include "CoreMinimal.h"
#include "CameraIntrinsics.h"

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
	 * Write image data to EXR file using ImageWriteQueue
	 * @param FilePath - Output file path
	 * @param RgbData - RGB image data
	 * @param DmvData - Depth/Motion/Velocity data (depth in R, motion X in G, motion Y in B)
	 * @param Width - Image width
	 * @param Height - Image height
	 * @param bIncludeDepth - If true, stores RGB+Depth (depth in alpha). If false, stores motion vectors (X in R, Y in G)
	 * @return true if write task was successfully queued
	 */
	bool WriteEXRFile(const FString&				FilePath,
					  const TArray<FLinearColor>& RgbData,
					  const TArray<FLinearColor>& DmvData,
					  int32							Width,
					  int32							Height,
					  bool							bIncludeDepth);

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
	bool WriteMetadataFile(const FString&				FilePath,
						   USceneCaptureComponent2D*	Camera,
						   const FCameraIntrinsics&		Intrinsics,
						   int32						FrameNumber,
						   float						Timestamp,
						   const FString&				ActorPath = TEXT(""),
						   const FString&				LevelName = TEXT(""));
}

