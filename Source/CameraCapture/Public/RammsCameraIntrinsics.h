#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "RammsCameraIntrinsics.generated.h"

/**
 * Camera intrinsic parameters for precise camera calibration
 * Can be used as inline parameters or saved as reusable data assets
 */
USTRUCT(BlueprintType)
struct CAMERACAPTURE_API FRammsCameraIntrinsics
{
	GENERATED_BODY()

	/** Focal length in X direction (pixels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics")
	float FocalLengthX = 500.0f;

	/** Focal length in Y direction (pixels) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics")
	float FocalLengthY = 500.0f;

	/** Principal point X coordinate (pixels) - typically ImageWidth/2 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics")
	float PrincipalPointX = 320.0f;

	/** Principal point Y coordinate (pixels) - typically ImageHeight/2 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics")
	float PrincipalPointY = 240.0f;

	/** Image width in pixels */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics")
	int32 ImageWidth = 640;

	/** Image height in pixels */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics")
	int32 ImageHeight = 480;

	/** Whether to use Maintain Y-Axis FOV calculation instead of custom projection matrix */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics")
	bool bMaintainYAxis = false;

	/** Preset name for documentation purposes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics")
	FString PresetName = TEXT("Custom");

	FRammsCameraIntrinsics() = default;
};

/**
 * Data asset for reusable camera intrinsic profiles
 * Create these as assets for specific camera models (e.g., RealSense D435)
 */
UCLASS(BlueprintType, Blueprintable)
class CAMERACAPTURE_API URammsCameraIntrinsicsAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics")
	FRammsCameraIntrinsics Intrinsics;
};
