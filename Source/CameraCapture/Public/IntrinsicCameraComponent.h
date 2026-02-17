#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraComponent.h"
#include "CameraIntrinsics.h"
#include "IntrinsicCameraComponent.generated.h"

/**
 * Camera component with support for custom camera intrinsics
 * Use this instead of base UCameraComponent for precise camera calibration in player cameras
 */
UCLASS(ClassGroup = Camera, meta = (BlueprintSpawnableComponent))
class CAMERACAPTURE_API UIntrinsicCameraComponent : public UCameraComponent
{
	GENERATED_BODY()

public:
	UIntrinsicCameraComponent();

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

	/** Apply the camera intrinsics to this camera component */
	UFUNCTION(BlueprintCallable, Category = "Camera Intrinsics")
	void ApplyIntrinsics();

	/** Build a custom projection matrix from camera intrinsics (C++ only) */
	static FMatrix BuildProjectionMatrixFromIntrinsics(const FCameraIntrinsics& Intrinsics);

	/** Whether to draw the camera frustum for visualization */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Visualization", meta = (DisplayName = "Draw Frustum In Game", ToolTip = "Enable to visualize the camera's field of view frustum in game"))
	bool bDrawFrustumInGame = false;

	/** Whether to draw the camera frustum in the editor */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Visualization", meta = (DisplayName = "Draw Frustum In Editor", ToolTip = "Enable to visualize the camera's field of view frustum in the editor."))
	bool bDrawFrustumInEditor = true;

	/** Distance to draw the far plane of the frustum (in cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Visualization", meta = (EditCondition = "bDrawFrustumInGame | bDrawFrustumInEditor", ClampMin = "10.0", DisplayName = "Frustum Draw Distance", ToolTip = "Distance to the far plane of the visualized frustum (in cm)"))
	float FrustumDrawDistance = 500.0f;

	/** Distance to draw the near plane of the frustum (in cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Visualization", meta = (EditCondition = "bDrawFrustumInGame | bDrawFrustumInEditor", ClampMin = "1.0", DisplayName = "Frustum Near Distance", ToolTip = "Distance to the near plane of the visualized frustum (in cm)"))
	float FrustumNearDistance = 10.0f;

	/** Color of the frustum lines */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Visualization", meta = (EditCondition = "bDrawFrustumInGame | bDrawFrustumInEditor", DisplayName = "Frustum Color", ToolTip = "Color of the frustum visualization lines"))
	FColor FrustumColor = FColor::Cyan;

	/** Thickness of frustum lines */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Visualization", meta = (EditCondition = "bDrawFrustumInGame | bDrawFrustumInEditor", ClampMin = "0.0", DisplayName = "Frustum Line Thickness", ToolTip = "Thickness of the frustum visualization lines"))
	float FrustumLineThickness = 0.2f;

	/** Whether to draw frustum planes (filled quads) for overlap visualization */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Visualization", meta = (EditCondition = "bDrawFrustumInGame | bDrawFrustumInEditor", DisplayName = "Draw Frustum Planes", ToolTip = "Draw filled frustum planes to help visualize overlap."))
	bool bDrawFrustumPlanes = true;

	/** Color of the frustum planes (alpha supported) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug Visualization", meta = (EditCondition = "bDrawFrustumInGame | bDrawFrustumInEditor", DisplayName = "Frustum Plane Color", ToolTip = "Color of the frustum planes (alpha supported)."))
	FLinearColor FrustumPlaneColor = FLinearColor(0.0f, 1.0f, 1.0f, 0.03f);

	// Override to provide custom projection via OffCenterProjectionOffset and FOV
	virtual void GetCameraView(float DeltaTime, FMinimalViewInfo& DesiredView) override;

protected:
	/** Whether we're currently using custom intrinsics for projection */
	bool bUsingCustomIntrinsics = false;
	virtual void BeginPlay() override;
	virtual void BeginDestroy() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Draw the camera frustum for visualization */
	void DrawCameraFrustum();

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

	/** Handle when any object property changes (used to detect asset changes) */
	void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);

	/** Delegate handle for object property changed */
	FDelegateHandle OnObjectPropertyChangedHandle;
#endif
};
