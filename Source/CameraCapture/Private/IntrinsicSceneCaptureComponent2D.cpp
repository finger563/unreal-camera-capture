#include "IntrinsicSceneCaptureComponent2D.h"
#include "DrawDebugHelpers.h"

#if WITH_EDITOR
#include "UObject/UObjectGlobals.h"
#endif

UIntrinsicSceneCaptureComponent2D::UIntrinsicSceneCaptureComponent2D()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_DuringPhysics;
	
#if WITH_EDITORONLY_DATA
	// Enable ticking in editor for frustum visualization
	bTickInEditor = true;
#endif
	
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

void UIntrinsicSceneCaptureComponent2D::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Draw frustum if enabled (works in both editor and runtime)
	if (bDrawFrustum)
	{
		DrawCameraFrustum();
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
			
			// Redraw frustum if enabled since intrinsics affect the frustum shape
			if (bDrawFrustum)
			{
				DrawCameraFrustum();
			}
		}
		
		// Force immediate redraw when frustum properties change
		if (MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicSceneCaptureComponent2D, bDrawFrustum) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicSceneCaptureComponent2D, FrustumDrawDistance) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicSceneCaptureComponent2D, FrustumColor) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicSceneCaptureComponent2D, FrustumLineThickness) ||
			MemberPropertyName == GET_MEMBER_NAME_CHECKED(UIntrinsicSceneCaptureComponent2D, FOVAngle))
		{
			// Draw new frustum immediately if enabled
			if (bDrawFrustum)
			{
				DrawCameraFrustum();
			}
			
			MarkRenderStateDirty();
		}
	}
	else if (PropertyChangedEvent.Property != nullptr)
	{
		// Handle changes to properties within the IntrinsicsAsset or InlineIntrinsics struct
		FName PropertyName = PropertyChangedEvent.Property->GetFName();
		
		// Check if any intrinsics parameter changed (focal length, principal point, etc.)
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, FocalLengthX) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, FocalLengthY) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, PrincipalPointX) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, PrincipalPointY) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, ImageWidth) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, ImageHeight) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FCameraIntrinsics, bMaintainYAxis))
		{
			ApplyIntrinsics();
			
			// Redraw frustum if enabled
			if (bDrawFrustum)
			{
				DrawCameraFrustum();
			}
		}
	}
}

void UIntrinsicSceneCaptureComponent2D::OnRegister()
{
	Super::OnRegister();
	
	// Register delegate to listen for property changes on any object
	if (!OnObjectPropertyChangedHandle.IsValid())
	{
		OnObjectPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(
			this, &UIntrinsicSceneCaptureComponent2D::OnObjectPropertyChanged);
	}
	
	// Draw frustum immediately in editor
	if (GIsEditor && !GetWorld()->IsGameWorld() && bDrawFrustum)
	{
		DrawCameraFrustum();
	}
}

void UIntrinsicSceneCaptureComponent2D::OnUnregister()
{
	// Unregister delegate
	if (OnObjectPropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(OnObjectPropertyChangedHandle);
		OnObjectPropertyChangedHandle.Reset();
	}
	
	Super::OnUnregister();
}

void UIntrinsicSceneCaptureComponent2D::OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	// Check if the changed object is our IntrinsicsAsset
	if (Object == IntrinsicsAsset && IntrinsicsAsset != nullptr && bUseIntrinsicsAsset)
	{
		// An intrinsics property changed in our referenced asset
		ApplyIntrinsics();
		
		// Redraw frustum if enabled
		if (bDrawFrustum)
		{
			DrawCameraFrustum();
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

void UIntrinsicSceneCaptureComponent2D::DrawCameraFrustum()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Determine if we're in editor mode
	bool bIsEditorWorld = GIsEditor && !World->IsGameWorld();
	
	// Get the camera's world transform
	FTransform CameraTransform = GetComponentTransform();
	FVector CameraLocation = CameraTransform.GetLocation();
	FRotator CameraRotation = CameraTransform.Rotator();

	// Determine which projection matrix to use
	FMatrix ProjectionMatrix;
	bool bHasValidProjection = false;

	if (bUseCustomIntrinsics && bUseCustomProjectionMatrix)
	{
		// Use the custom projection matrix from intrinsics
		ProjectionMatrix = CustomProjectionMatrix;
		bHasValidProjection = true;
	}
	else if (!bUseCustomIntrinsics || (bUseCustomIntrinsics && GetActiveIntrinsics().bMaintainYAxis))
	{
		// Use FOV-based projection
		float AspectRatio = 1.777f; // Default 16:9
		if (TextureTarget)
		{
			AspectRatio = static_cast<float>(TextureTarget->SizeX) / static_cast<float>(TextureTarget->SizeY);
		}
		else if (bUseCustomIntrinsics)
		{
			FCameraIntrinsics Intrinsics = GetActiveIntrinsics();
			AspectRatio = static_cast<float>(Intrinsics.ImageWidth) / static_cast<float>(Intrinsics.ImageHeight);
		}

		// Build standard perspective projection from FOV
		float HalfFOVRad = FMath::DegreesToRadians(FOVAngle * 0.5f);
		ProjectionMatrix = FReversedZPerspectiveMatrix(HalfFOVRad, AspectRatio, 1.0f, GNearClippingPlane);
		bHasValidProjection = true;
	}

	if (!bHasValidProjection)
	{
		return;
	}

	// Invert the projection matrix to get view-space corners
	FMatrix InvProjectionMatrix = ProjectionMatrix.Inverse();

	// Define the 4 corners of the far plane in normalized device coordinates (NDC)
	// NDC: X[-1,1], Y[-1,1], Z[0,1] (reversed-Z)
	FVector4 NDCCorners[4] = {
		FVector4(-1.0f, -1.0f, 0.0f, 1.0f), // Bottom-left
		FVector4( 1.0f, -1.0f, 0.0f, 1.0f), // Bottom-right
		FVector4( 1.0f,  1.0f, 0.0f, 1.0f), // Top-right
		FVector4(-1.0f,  1.0f, 0.0f, 1.0f)  // Top-left
	};

	// Transform corners from NDC to view space, then scale to desired draw distance
	FVector ViewSpaceCorners[4];
	for (int32 i = 0; i < 4; i++)
	{
		FVector4 ViewSpace4 = InvProjectionMatrix.TransformFVector4(NDCCorners[i]);
		
		// Perspective divide
		if (FMath::Abs(ViewSpace4.W) > SMALL_NUMBER)
		{
			ViewSpaceCorners[i] = FVector(ViewSpace4.X, ViewSpace4.Y, ViewSpace4.Z) / ViewSpace4.W;
		}
		else
		{
			ViewSpaceCorners[i] = FVector(ViewSpace4.X, ViewSpace4.Y, ViewSpace4.Z);
		}

		// Normalize and scale to frustum draw distance
		// Note: Projection matrix gives us view space where camera looks down +X axis
		// Scale the direction from origin to the desired distance
		ViewSpaceCorners[i] = ViewSpaceCorners[i].GetSafeNormal() * FrustumDrawDistance;
	}

	// Transform view-space corners to world space
	// UE SceneCaptureComponent2D: Camera looks down +X (Forward), Y=Right, Z=Up in local space
	// But view space from projection matrix: X=right, Y=up, Z=back (OpenGL convention)
	// We need to convert: ViewSpace.X->LocalY, ViewSpace.Y->LocalZ, ViewSpace.Z->LocalX (negated)
	FVector WorldCorners[4];
	for (int32 i = 0; i < 4; i++)
	{
		FVector LocalSpace;
		LocalSpace.X = ViewSpaceCorners[i].Z;  // View back (Z) -> Local forward (X) - flip to point forward
		LocalSpace.Y = ViewSpaceCorners[i].X;  // View right (X) -> Local right (Y)
		LocalSpace.Z = ViewSpaceCorners[i].Y;  // View up (Y) -> Local up (Z)
		
		WorldCorners[i] = CameraTransform.TransformPosition(LocalSpace);
	}

	// Use short-lifetime non-persistent lines that auto-expire
	// This allows multiple cameras to draw simultaneously without interfering
	float LifeTime = 0.1f; // Short lifetime, refreshed every tick
	bool bPersistent = false;

	// Draw lines from camera origin to each corner
	for (int32 i = 0; i < 4; i++)
	{
		DrawDebugLine(World, CameraLocation, WorldCorners[i], FrustumColor, bPersistent, LifeTime, 0, FrustumLineThickness);
	}

	// Draw lines connecting the corners (far plane rectangle)
	for (int32 i = 0; i < 4; i++)
	{
		int32 NextIdx = (i + 1) % 4;
		DrawDebugLine(World, WorldCorners[i], WorldCorners[NextIdx], FrustumColor, bPersistent, LifeTime, 0, FrustumLineThickness);
	}

	// Optionally draw a small cross at the camera origin for reference
	DrawDebugCrosshairs(World, CameraLocation, CameraRotation, 10.0f, FrustumColor, bPersistent, LifeTime, 0);
}
