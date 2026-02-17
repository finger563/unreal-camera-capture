#include "IntrinsicCameraComponent.h"
#include "Utilities.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"

#if WITH_EDITOR
	#include "UObject/UObjectGlobals.h"
#endif

UIntrinsicCameraComponent::UIntrinsicCameraComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

#if WITH_EDITORONLY_DATA
	// Enable ticking in editor for frustum visualization
	bTickInEditor = true;
#endif
}

void UIntrinsicCameraComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bUseCustomIntrinsics)
	{
		ApplyIntrinsics();
	}
}

void UIntrinsicCameraComponent::BeginDestroy()
{
	// Unregister delegate
	if (OnObjectPropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(OnObjectPropertyChangedHandle);
		OnObjectPropertyChangedHandle.Reset();
	}

	Super::BeginDestroy();
}

void UIntrinsicCameraComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Draw frustum in-game if enabled
	if (bDrawFrustumInGame)
	{
		DrawCameraFrustum();
	}

	// Draw frustum in-editor if enabled, but not when PIE is active
#if WITH_EDITOR
	if (GIsEditor && bDrawFrustumInEditor && !GetWorld()->IsPlayInEditor())
	{
		DrawCameraFrustum();
	}
#endif
}

#if WITH_EDITOR
void UIntrinsicCameraComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Apply or clear intrinsics whenever relevant properties change
	if (PropertyChangedEvent.MemberProperty != nullptr)
	{
		FName MemberPropertyName = PropertyChangedEvent.MemberProperty->GetFName();

		// Check if any intrinsics-related property changed
		if (MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicCameraComponent, bUseCustomIntrinsics) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicCameraComponent, bUseIntrinsicsAsset) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicCameraComponent, IntrinsicsAsset) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicCameraComponent, InlineIntrinsics))
		{
			ApplyIntrinsics();
		}

		// Force immediate redraw when frustum properties change
		if (MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicCameraComponent, bDrawFrustumInEditor) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicCameraComponent, FrustumDrawDistance) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicCameraComponent, FrustumNearDistance) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicCameraComponent, FrustumColor) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicCameraComponent, FrustumLineThickness) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicCameraComponent, bDrawFrustumPlanes) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicCameraComponent, FrustumPlaneColor) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicCameraComponent, FieldOfView))
		{
			MarkRenderStateDirty();
		}
	}
	else if (PropertyChangedEvent.Property != nullptr)
	{
		// Handle changes to properties within the IntrinsicsAsset or InlineIntrinsics struct
		FName PropertyName = PropertyChangedEvent.Property->GetFName();

		// Check if any intrinsics parameter changed
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, FocalLengthX) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, FocalLengthY) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, PrincipalPointX) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, PrincipalPointY) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, ImageWidth) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, ImageHeight) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, bMaintainYAxis))
		{
			ApplyIntrinsics();
		}
	}
}

void UIntrinsicCameraComponent::OnRegister()
{
	Super::OnRegister();

	// Register delegate to listen for property changes on any object
	if (!OnObjectPropertyChangedHandle.IsValid())
	{
		OnObjectPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(
			this, &UIntrinsicCameraComponent::OnObjectPropertyChanged);
	}
}

void UIntrinsicCameraComponent::OnUnregister()
{
	// Unregister delegate
	if (OnObjectPropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(OnObjectPropertyChangedHandle);
		OnObjectPropertyChangedHandle.Reset();
	}

	Super::OnUnregister();
}

void UIntrinsicCameraComponent::OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	if (Object == IntrinsicsAsset && IntrinsicsAsset != nullptr && bUseIntrinsicsAsset)
	{
		// An intrinsics property changed in our referenced asset
		ApplyIntrinsics();
	}
}
#endif

FCameraIntrinsics UIntrinsicCameraComponent::GetActiveIntrinsics() const
{
	if (bUseIntrinsicsAsset && IntrinsicsAsset)
	{
		return IntrinsicsAsset->Intrinsics;
	}
	return InlineIntrinsics;
}

void UIntrinsicCameraComponent::ApplyIntrinsics()
{
	if (!bUseCustomIntrinsics)
	{
		// Clear custom intrinsics mode when disabled
		bUsingCustomIntrinsics = false;
		return;
	}

	FCameraIntrinsics Intrinsics = GetActiveIntrinsics();

	// Validate intrinsics to prevent divide-by-zero
	if (Intrinsics.ImageWidth < 1 || Intrinsics.ImageHeight < 1)
	{
		UE_LOG(LogTemp, Error, TEXT("IntrinsicCameraComponent [%s]: Invalid image dimensions (%dx%d). Intrinsics not applied."),
			*GetName(), Intrinsics.ImageWidth, Intrinsics.ImageHeight);
		bUsingCustomIntrinsics = false;
		return;
	}

	if (Intrinsics.bMaintainYAxis)
	{
		// Adjust FOV to maintain Y-axis (vertical FOV) like standard gameplay camera
		float IntrinsicsAspectRatio = static_cast<float>(Intrinsics.ImageWidth) / static_cast<float>(Intrinsics.ImageHeight);

		// Assume the current FOV is for a 16:9 aspect ratio (standard)
		float ReferenceAspect = 16.0f / 9.0f;

		// Derive vertical FOV from current horizontal FOV
		float HalfHFOVRad = FMath::DegreesToRadians(FieldOfView * 0.5f);
		float HalfVFOVRad = FMath::Atan(FMath::Tan(HalfHFOVRad) / ReferenceAspect);

		// Recalculate horizontal FOV for the actual aspect ratio
		float NewHalfHFOVRad = FMath::Atan(IntrinsicsAspectRatio * FMath::Tan(HalfVFOVRad));
		FieldOfView = FMath::RadiansToDegrees(NewHalfHFOVRad * 2.0f);

		bUsingCustomIntrinsics = false;

		UE_LOG(LogTemp, Log, TEXT("Applied Maintain Y-Axis to %s: New HFOV=%.2f deg (Aspect=%.3f)"),
			*GetName(), FieldOfView, IntrinsicsAspectRatio);
	}
	else
	{
		// Use custom intrinsics mode (will apply in GetCameraView)
		bUsingCustomIntrinsics = true;

		UE_LOG(LogTemp, Log, TEXT("Applied custom intrinsics to %s (fx=%.2f, fy=%.2f, cx=%.2f, cy=%.2f, %dx%d)"),
			*GetName(), Intrinsics.FocalLengthX, Intrinsics.FocalLengthY,
			Intrinsics.PrincipalPointX, Intrinsics.PrincipalPointY,
			Intrinsics.ImageWidth, Intrinsics.ImageHeight);
	}
}

FMatrix UIntrinsicCameraComponent::BuildProjectionMatrixFromIntrinsics(const FCameraIntrinsics& Intrinsics)
{
	// Validate dimensions to prevent divide-by-zero
	if (Intrinsics.ImageWidth < 1 || Intrinsics.ImageHeight < 1)
	{
		UE_LOG(LogTemp, Error, TEXT("BuildProjectionMatrixFromIntrinsics: Invalid dimensions (%dx%d), returning identity matrix"),
			Intrinsics.ImageWidth, Intrinsics.ImageHeight);
		return FMatrix::Identity;
	}

	float Width = static_cast<float>(Intrinsics.ImageWidth);
	float Height = static_cast<float>(Intrinsics.ImageHeight);

	// Convert from pixel-based intrinsics to normalized coordinates
	float fx = Intrinsics.FocalLengthX / Width;
	float fy = Intrinsics.FocalLengthY / Height;
	float cx = (Intrinsics.PrincipalPointX - Width * 0.5f) / Width;
	float cy = (Intrinsics.PrincipalPointY - Height * 0.5f) / Height;

	// Use GNearClippingPlane for consistency
	float NearClip = GNearClippingPlane;

	// Build custom projection matrix from camera intrinsics
	FMatrix ProjectionMatrix = FMatrix::Identity;

	// Scale factors from pixel space to normalized device coordinates
	ProjectionMatrix.M[0][0] = 2.0f * fx;
	ProjectionMatrix.M[1][1] = 2.0f * fy;
	ProjectionMatrix.M[2][0] = 2.0f * cx;
	ProjectionMatrix.M[2][1] = -2.0f * cy; // Flip Y for UE coordinate system

	// Standard perspective projection depth terms (reversed-Z with infinite far plane)
	ProjectionMatrix.M[2][2] = 0.0f;
	ProjectionMatrix.M[2][3] = 1.0f;
	ProjectionMatrix.M[3][2] = NearClip;
	ProjectionMatrix.M[3][3] = 0.0f;

	return ProjectionMatrix;
}

void UIntrinsicCameraComponent::GetCameraView(float DeltaTime, FMinimalViewInfo& DesiredView)
{
	Super::GetCameraView(DeltaTime, DesiredView);

	// Apply custom projection using OffCenterProjectionOffset if enabled
	if (bUsingCustomIntrinsics && bUseCustomIntrinsics)
	{
		FCameraIntrinsics Intrinsics = GetActiveIntrinsics();
		
		// Calculate offset from principal point
		// Convert from pixel coordinates to normalized offset (-1 to 1)
		float Width = static_cast<float>(Intrinsics.ImageWidth);
		float Height = static_cast<float>(Intrinsics.ImageHeight);
		
		float cx = Intrinsics.PrincipalPointX;
		float cy = Intrinsics.PrincipalPointY;
		
		// Calculate normalized offset from center
		// OffCenterProjectionOffset is in proportion of screen dimensions
		float OffsetX = (cx - Width * 0.5f) / Width;
		float OffsetY = (cy - Height * 0.5f) / Height;
		
		DesiredView.OffCenterProjectionOffset = FVector2D(OffsetX, OffsetY);
		
		// Also adjust the FOV based on focal length
		// FOV = 2 * atan(width / (2 * focal_length))
		if (Intrinsics.FocalLengthX > 0.0f)
		{
			float HalfHFOVRad = FMath::Atan(Width / (2.0f * Intrinsics.FocalLengthX));
			DesiredView.FOV = FMath::RadiansToDegrees(HalfHFOVRad * 2.0f);
		}
	}
}

void UIntrinsicCameraComponent::DrawCameraFrustum()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FTransform CameraTransform = GetComponentTransform();
	
	// Ignore component scale for frustum drawing (so frustum is always world-scale)
	CameraTransform.SetScale3D(FVector::OneVector);

	// Get active intrinsics for frustum calculation
	FCameraIntrinsics Intrinsics = bUseCustomIntrinsics ? GetActiveIntrinsics() : FCameraIntrinsics();
	
	// If not using custom intrinsics, derive from FOV
	if (!bUseCustomIntrinsics)
	{
		// Assume 1920x1080 for default aspect ratio
		Intrinsics.ImageWidth = 1920;
		Intrinsics.ImageHeight = 1080;
		
		// Calculate focal length from horizontal FOV
		float HalfHFOVRad = FMath::DegreesToRadians(FieldOfView * 0.5f);
		Intrinsics.FocalLengthX = Intrinsics.ImageWidth / (2.0f * FMath::Tan(HalfHFOVRad));
		Intrinsics.FocalLengthY = Intrinsics.FocalLengthX; // Assume square pixels
		Intrinsics.PrincipalPointX = Intrinsics.ImageWidth * 0.5f;
		Intrinsics.PrincipalPointY = Intrinsics.ImageHeight * 0.5f;
	}

	// Use shared utility function for drawing
	CameraCaptureUtils::DrawFrustumFromIntrinsics(
		World,
		CameraTransform,
		Intrinsics,
		FrustumNearDistance,
		FrustumDrawDistance,
		FrustumColor,
		FrustumLineThickness,
		bDrawFrustumPlanes,
		FrustumPlaneColor);
}
