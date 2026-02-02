#include "IntrinsicSceneCaptureComponent2D.h"

UIntrinsicSceneCaptureComponent2D::UIntrinsicSceneCaptureComponent2D()
{
	PrimaryComponentTick.bCanEverTick = false;
	
	// We control capture timing via CaptureComponent, so disable auto-capture
	bCaptureEveryFrame = false;
	bCaptureOnMovement = false;
	// but we want to ensure that motion vectors and such are still good so turn on persist rendering state
	bAlwaysPersistRenderingState = true;
}

void UIntrinsicSceneCaptureComponent2D::BeginPlay()
{
	Super::BeginPlay();

	if (bUseCustomIntrinsics)
	{
		ApplyIntrinsics();
	}
}

#if WITH_EDITOR
void UIntrinsicSceneCaptureComponent2D::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Apply or clear intrinsics whenever relevant properties change
	if (PropertyChangedEvent.MemberProperty != nullptr)
	{
		FName MemberPropertyName = PropertyChangedEvent.MemberProperty->GetFName();
		
		// Check if any intrinsics-related property changed
		if (MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicSceneCaptureComponent2D, bUseCustomIntrinsics) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicSceneCaptureComponent2D, bUseIntrinsicsAsset) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicSceneCaptureComponent2D, IntrinsicsAsset) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicSceneCaptureComponent2D, InlineIntrinsics))
		{
			ApplyIntrinsics();
		}
	}
}
#endif

FCameraIntrinsics UIntrinsicSceneCaptureComponent2D::GetActiveIntrinsics() const
{
	if (bUseIntrinsicsAsset && IntrinsicsAsset)
	{
		return IntrinsicsAsset->Intrinsics;
	}
	return InlineIntrinsics;
}

void UIntrinsicSceneCaptureComponent2D::ApplyIntrinsics()
{
	if (!bUseCustomIntrinsics)
	{
		// Clear custom projection matrix when disabled
		bUseCustomProjectionMatrix = false;
		return;
	}

	FCameraIntrinsics Intrinsics = GetActiveIntrinsics();

	// Validate intrinsics to prevent divide-by-zero
	if (Intrinsics.ImageWidth < 1 || Intrinsics.ImageHeight < 1)
	{
		UE_LOG(LogTemp, Error, TEXT("IntrinsicSceneCaptureComponent2D [%s]: Invalid image dimensions (%dx%d). Intrinsics not applied."),
			*GetName(), Intrinsics.ImageWidth, Intrinsics.ImageHeight);
		bUseCustomProjectionMatrix = false;
		return;
	}

	if (Intrinsics.bMaintainYAxis)
	{
		// Adjust FOV to maintain Y-axis (vertical FOV) like gameplay camera
		float AspectRatio = static_cast<float>(Intrinsics.ImageWidth) / static_cast<float>(Intrinsics.ImageHeight);
		
		// Assume the current FOV is for a 16:9 aspect ratio (standard)
		float ReferenceAspect = 16.0f / 9.0f;
		
		// Derive vertical FOV from current horizontal FOV
		float HalfHFOVRad = FMath::DegreesToRadians(FOVAngle * 0.5f);
		float HalfVFOVRad = FMath::Atan(FMath::Tan(HalfHFOVRad) / ReferenceAspect);
		
		// Recalculate horizontal FOV for the actual aspect ratio
		float NewHalfHFOVRad = FMath::Atan(AspectRatio * FMath::Tan(HalfVFOVRad));
		FOVAngle = FMath::RadiansToDegrees(NewHalfHFOVRad * 2.0f);
		
		bUseCustomProjectionMatrix = false;

		UE_LOG(LogTemp, Log, TEXT("Applied Maintain Y-Axis to %s: New HFOV=%.2f deg (Aspect=%.3f)"),
			*GetName(), FOVAngle, AspectRatio);
	}
	else
	{
		// Build custom projection matrix from intrinsics
		CustomProjectionMatrix = BuildProjectionMatrixFromIntrinsics(Intrinsics);
		bUseCustomProjectionMatrix = true;

		UE_LOG(LogTemp, Log, TEXT("Applied custom projection matrix to %s (fx=%.2f, fy=%.2f, cx=%.2f, cy=%.2f, %dx%d)"),
			*GetName(), Intrinsics.FocalLengthX, Intrinsics.FocalLengthY, 
			Intrinsics.PrincipalPointX, Intrinsics.PrincipalPointY,
			Intrinsics.ImageWidth, Intrinsics.ImageHeight);
	}
}

FMatrix UIntrinsicSceneCaptureComponent2D::BuildProjectionMatrixFromIntrinsics(const FCameraIntrinsics& Intrinsics)
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
	
	// Use GNearClippingPlane for consistency with config export
	// Note: Using infinite far plane (reversed-Z projection)
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
