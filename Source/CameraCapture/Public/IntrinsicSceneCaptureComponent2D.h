#pragma once

#include "CoreMinimal.h"
#include "Components/SceneCaptureComponent2D.h"
#include "CameraIntrinsics.h"
#include "IntrinsicSceneCaptureComponent2D.generated.h"

/**
 * Scene capture component with support for custom camera intrinsics
 * Use this instead of base USceneCaptureComponent2D for precise camera calibration
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class CAMERACAPTURE_API UIntrinsicSceneCaptureComponent2D : public USceneCaptureComponent2D
{
	GENERATED_BODY()

public:
	UIntrinsicSceneCaptureComponent2D();

	/** Whether to use custom camera intrinsics */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics")
	bool bUseCustomIntrinsics = false;

	/** Whether to use an intrinsics asset or inline parameters */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics", meta = (EditCondition = "bUseCustomIntrinsics"))
	bool bUseIntrinsicsAsset = false;

	/** Reference to reusable camera intrinsics asset (e.g., RealSense D435 preset) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics", meta = (EditCondition = "bUseCustomIntrinsics && bUseIntrinsicsAsset"))
	UCameraIntrinsicsAsset* IntrinsicsAsset = nullptr;

	/** Inline camera intrinsics parameters */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Intrinsics", meta = (EditCondition = "bUseCustomIntrinsics && !bUseIntrinsicsAsset"))
	FCameraIntrinsics InlineIntrinsics;

	/** Get the active intrinsics (from asset or inline) */
	UFUNCTION(BlueprintCallable, Category = "Camera Intrinsics")
	FCameraIntrinsics GetActiveIntrinsics() const;

	/** Apply the camera intrinsics to this scene capture component */
	UFUNCTION(BlueprintCallable, Category = "Camera Intrinsics")
	void ApplyIntrinsics();

	/** Build a custom projection matrix from camera intrinsics (C++ only) */
	static FMatrix BuildProjectionMatrixFromIntrinsics(const FCameraIntrinsics& Intrinsics);

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
