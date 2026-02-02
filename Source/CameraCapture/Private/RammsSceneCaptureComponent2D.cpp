#include "RammsSceneCaptureComponent2D.h"

URammsSceneCaptureComponent2D::URammsSceneCaptureComponent2D()
{
	PrimaryComponentTick.bCanEverTick = false;
	
	// We control capture timing via CaptureComponent, so disable auto-capture
	bCaptureEveryFrame = false;
	bCaptureOnMovement = false;
    // but we want to ensure that motion vectors and such are still good so turn on persist rendering state
    bAlwaysPersistRenderingState = true;
}

void URammsSceneCaptureComponent2D::BeginPlay()
{
	Super::BeginPlay();

	if (bUseCustomIntrinsics)
	{
		ApplyIntrinsics();
	}
}

#if WITH_EDITOR
void URammsSceneCaptureComponent2D::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Apply intrinsics when properties change in editor
	if (bUseCustomIntrinsics && PropertyChangedEvent.Property != nullptr)
	{
		FName PropertyName = PropertyChangedEvent.Property->GetFName();
		if (PropertyName == GET_MEMBER_NAME_CHECKED(URammsSceneCaptureComponent2D, bUseCustomIntrinsics) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(URammsSceneCaptureComponent2D, bUseIntrinsicsAsset) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(URammsSceneCaptureComponent2D, IntrinsicsAsset) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(URammsSceneCaptureComponent2D, InlineIntrinsics))
		{
			ApplyIntrinsics();
		}
	}
}
#endif

FRammsCameraIntrinsics URammsSceneCaptureComponent2D::GetActiveIntrinsics() const
{
	if (bUseIntrinsicsAsset && IntrinsicsAsset)
	{
		return IntrinsicsAsset->Intrinsics;
	}
	return InlineIntrinsics;
}

void URammsSceneCaptureComponent2D::ApplyIntrinsics()
{
	if (!bUseCustomIntrinsics)
	{
		bUseCustomProjectionMatrix = false;
		return;
	}

	FRammsCameraIntrinsics Intrinsics = GetActiveIntrinsics();

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

FMatrix URammsSceneCaptureComponent2D::BuildProjectionMatrixFromIntrinsics(const FRammsCameraIntrinsics& Intrinsics)
{
	float Width = static_cast<float>(Intrinsics.ImageWidth);
	float Height = static_cast<float>(Intrinsics.ImageHeight);
	
	// Convert from pixel-based intrinsics to normalized coordinates
	float fx = Intrinsics.FocalLengthX / Width;
	float fy = Intrinsics.FocalLengthY / Height;
	float cx = (Intrinsics.PrincipalPointX - Width * 0.5f) / Width;
	float cy = (Intrinsics.PrincipalPointY - Height * 0.5f) / Height;
	
	// Use reasonable near/far clip planes
	float NearClip = 10.0f; // 10cm in UE units
	float FarClip = 1000000.0f; // 10km in UE units
	
	// Build custom projection matrix from camera intrinsics
	FMatrix ProjectionMatrix = FMatrix::Identity;
	
	// Scale factors from pixel space to normalized device coordinates
	ProjectionMatrix.M[0][0] = 2.0f * fx;
	ProjectionMatrix.M[1][1] = 2.0f * fy;
	ProjectionMatrix.M[2][0] = 2.0f * cx;
	ProjectionMatrix.M[2][1] = -2.0f * cy; // Flip Y for UE coordinate system
	
	// Standard perspective projection depth terms (reversed-Z)
	ProjectionMatrix.M[2][2] = 0.0f;
	ProjectionMatrix.M[2][3] = 1.0f;
	ProjectionMatrix.M[3][2] = NearClip;
	ProjectionMatrix.M[3][3] = 0.0f;
	
	return ProjectionMatrix;
}
