#include "Utilities.h"
#include "Components/SceneCaptureComponent2D.h"
#include "ImageWriteQueue.h"
#include "ImageWriteTask.h"
#include "ImagePixelData.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"

namespace CameraCaptureUtils
{

bool WriteEXRFile(const FString&				FilePath,
				  const TArray<FLinearColor>& RgbData,
				  const TArray<FLinearColor>& DmvData,
				  int32							Width,
				  int32							Height,
				  bool							bIncludeDepth)
{
	IImageWriteQueueModule* ImageWriteQueueModule = FModuleManager::Get().GetModulePtr<IImageWriteQueueModule>("ImageWriteQueue");
	if (!ImageWriteQueueModule)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load ImageWriteQueue module"));
		return false;
	}

	if (RgbData.Num() != Width * Height || DmvData.Num() != Width * Height)
	{
		UE_LOG(LogTemp, Error, TEXT("Image data size mismatch. Expected %dx%d, got RGB:%d DMV:%d"),
			   Width, Height, RgbData.Num(), DmvData.Num());
		return false;
	}

	TUniquePtr<TImagePixelData<FLinearColor>> PixelData = MakeUnique<TImagePixelData<FLinearColor>>(
		FIntPoint(Width, Height),
		TArray64<FLinearColor>());

	PixelData->Pixels.Reserve(Width * Height);

	if (bIncludeDepth)
	{
		// RGB + Depth format: RGB from RgbData, depth from DmvData.R
		for (int32 i = 0; i < Width * Height; ++i)
		{
			FLinearColor Pixel;
			Pixel.R = RgbData[i].R;
			Pixel.G = RgbData[i].G;
			Pixel.B = RgbData[i].B;
			Pixel.A = DmvData[i].R; // Depth in alpha channel
			PixelData->Pixels.Add(Pixel);
		}
	}
	else
	{
		// Motion vector format: X from DmvData.G, Y from DmvData.B
		for (int32 i = 0; i < Width * Height; ++i)
		{
			FLinearColor Pixel;
			Pixel.R = DmvData[i].G; // Motion X
			Pixel.G = DmvData[i].B; // Motion Y
			Pixel.B = 0.0f;
			Pixel.A = 0.0f;
			PixelData->Pixels.Add(Pixel);
		}
	}

	TUniquePtr<FImageWriteTask> ImageTask = MakeUnique<FImageWriteTask>();
	ImageTask->PixelData = MoveTemp(PixelData);
	ImageTask->Filename = FilePath;
	ImageTask->Format = EImageFormat::EXR;
	ImageTask->CompressionQuality = (int32)EImageCompressionQuality::Default;
	ImageTask->bOverwriteFile = true;

	TFuture<bool> CompletionFuture = ImageWriteQueueModule->GetWriteQueue().Enqueue(MoveTemp(ImageTask));

	return true;
}

bool WriteMetadataFile(const FString&				FilePath,
					   USceneCaptureComponent2D*	Camera,
					   const FCameraIntrinsics&		Intrinsics,
					   int32						FrameNumber,
					   float						Timestamp,
					   const FString&				ActorPath,
					   const FString&				LevelName)
{
	if (!Camera)
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid camera component for metadata"));
		return false;
	}

	TSharedPtr<FJsonObject> RootObject = MakeShareable(new FJsonObject);

	// Frame information
	RootObject->SetNumberField(TEXT("frame_number"), FrameNumber);
	RootObject->SetNumberField(TEXT("timestamp"), Timestamp);

	// Camera identifier
	FString CameraId = Camera->GetOwner() ? Camera->GetOwner()->GetName() : TEXT("Unknown");
	RootObject->SetStringField(TEXT("camera_id"), CameraId);

	// World transform
	FTransform CameraTransform = Camera->GetComponentTransform();
	FVector Location = CameraTransform.GetLocation() / 100.0f; // Convert cm to meters
	FRotator Rotation = CameraTransform.Rotator();
	FQuat Quaternion = CameraTransform.GetRotation();
	FVector Scale = CameraTransform.GetScale3D();

	TSharedPtr<FJsonObject> TransformObject = MakeShareable(new FJsonObject);

	TArray<TSharedPtr<FJsonValue>> LocationArray;
	LocationArray.Add(MakeShareable(new FJsonValueNumber(Location.X)));
	LocationArray.Add(MakeShareable(new FJsonValueNumber(Location.Y)));
	LocationArray.Add(MakeShareable(new FJsonValueNumber(Location.Z)));
	TransformObject->SetArrayField(TEXT("location"), LocationArray);

	TArray<TSharedPtr<FJsonValue>> RotationArray;
	RotationArray.Add(MakeShareable(new FJsonValueNumber(Rotation.Pitch)));
	RotationArray.Add(MakeShareable(new FJsonValueNumber(Rotation.Yaw)));
	RotationArray.Add(MakeShareable(new FJsonValueNumber(Rotation.Roll)));
	TransformObject->SetArrayField(TEXT("rotation"), RotationArray);

	TArray<TSharedPtr<FJsonValue>> QuaternionArray;
	QuaternionArray.Add(MakeShareable(new FJsonValueNumber(Quaternion.W)));
	QuaternionArray.Add(MakeShareable(new FJsonValueNumber(Quaternion.X)));
	QuaternionArray.Add(MakeShareable(new FJsonValueNumber(Quaternion.Y)));
	QuaternionArray.Add(MakeShareable(new FJsonValueNumber(Quaternion.Z)));
	TransformObject->SetArrayField(TEXT("quaternion"), QuaternionArray);

	TArray<TSharedPtr<FJsonValue>> ScaleArray;
	ScaleArray.Add(MakeShareable(new FJsonValueNumber(Scale.X)));
	ScaleArray.Add(MakeShareable(new FJsonValueNumber(Scale.Y)));
	ScaleArray.Add(MakeShareable(new FJsonValueNumber(Scale.Z)));
	TransformObject->SetArrayField(TEXT("scale"), ScaleArray);

	RootObject->SetObjectField(TEXT("world_transform"), TransformObject);

	// Camera intrinsics
	TSharedPtr<FJsonObject> IntrinsicsObject = MakeShareable(new FJsonObject);
	IntrinsicsObject->SetNumberField(TEXT("focal_length_x"), Intrinsics.FocalLengthX);
	IntrinsicsObject->SetNumberField(TEXT("focal_length_y"), Intrinsics.FocalLengthY);
	IntrinsicsObject->SetNumberField(TEXT("principal_point_x"), Intrinsics.PrincipalPointX);
	IntrinsicsObject->SetNumberField(TEXT("principal_point_y"), Intrinsics.PrincipalPointY);
	IntrinsicsObject->SetNumberField(TEXT("image_width"), Intrinsics.ImageWidth);
	IntrinsicsObject->SetNumberField(TEXT("image_height"), Intrinsics.ImageHeight);
	IntrinsicsObject->SetBoolField(TEXT("maintain_y_axis"), Intrinsics.bMaintainYAxis);
	RootObject->SetObjectField(TEXT("intrinsics"), IntrinsicsObject);

	// Context information
	if (!ActorPath.IsEmpty())
	{
		RootObject->SetStringField(TEXT("actor_path"), ActorPath);
	}
	if (!LevelName.IsEmpty())
	{
		RootObject->SetStringField(TEXT("level_name"), LevelName);
	}

	// Serialize to string
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	if (!FJsonSerializer::Serialize(RootObject.ToSharedRef(), Writer))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to serialize metadata JSON"));
		return false;
	}

	// Write to file
	if (!FFileHelper::SaveStringToFile(OutputString, *FilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to write metadata file: %s"), *FilePath);
		return false;
	}

	return true;
}

void DrawFrustumFromProjectionMatrix(UWorld*					World,
									 const FTransform&			CameraTransform,
									 const FMatrix&				ProjectionMatrix,
									 float						NearDistance,
									 float						FarDistance,
									 const FColor&				LineColor,
									 float						LineThickness,
									 bool						bDrawPlanes,
									 const FLinearColor&		PlaneColor)
{
	if (!World)
	{
		return;
	}

	// Invert the projection matrix to get view-space corners
	FMatrix InvProjectionMatrix = ProjectionMatrix.Inverse();

	// Define the 4 corners of the far plane in normalized device coordinates (NDC)
	// NDC: X[-1,1], Y[-1,1], Z[0,1] (reversed-Z)
	FVector4 NDCCorners[4] = {
		FVector4(-1.0f, -1.0f, 0.0f, 1.0f), // Bottom-left
		FVector4(1.0f, -1.0f, 0.0f, 1.0f),	// Bottom-right
		FVector4(1.0f, 1.0f, 0.0f, 1.0f),	// Top-right
		FVector4(-1.0f, 1.0f, 0.0f, 1.0f)	// Top-left
	};

	// Transform corners from NDC to view space, then normalize to get direction vectors
	FVector ViewSpaceDirs[4];
	for (int32 i = 0; i < 4; i++)
	{
		FVector4 ViewSpace4 = InvProjectionMatrix.TransformFVector4(NDCCorners[i]);

		// Perspective divide
		if (FMath::Abs(ViewSpace4.W) > SMALL_NUMBER)
		{
			ViewSpaceDirs[i] = FVector(ViewSpace4.X, ViewSpace4.Y, ViewSpace4.Z) / ViewSpace4.W;
		}
		else
		{
			ViewSpaceDirs[i] = FVector(ViewSpace4.X, ViewSpace4.Y, ViewSpace4.Z);
		}
		ViewSpaceDirs[i] = ViewSpaceDirs[i].GetSafeNormal();
	}

	const float NearDist = FMath::Max(1.0f, NearDistance);
	const float FarDist = FMath::Max(NearDist + 1.0f, FarDistance);

	// Transform view-space corners to world space
	// UE SceneCaptureComponent2D: Camera looks down +X (Forward), Y=Right, Z=Up in local space
	// But view space from projection matrix: X=right, Y=up, Z=back (OpenGL convention)
	// We need to convert: ViewSpace.X->LocalY, ViewSpace.Y->LocalZ, ViewSpace.Z->LocalX (negated)
	FVector NearWorld[4];
	FVector FarWorld[4];
	FVector CameraLocation = CameraTransform.GetLocation();
	
	for (int32 i = 0; i < 4; i++)
	{
		const FVector ViewDir = ViewSpaceDirs[i];
		FVector		  LocalNear;
		FVector		  LocalFar;
		LocalNear.X = ViewDir.Z * NearDist; // View back (Z) -> Local forward (X)
		LocalNear.Y = ViewDir.X * NearDist; // View right (X) -> Local right (Y)
		LocalNear.Z = ViewDir.Y * NearDist; // View up (Y) -> Local up (Z)

		LocalFar.X = ViewDir.Z * FarDist;
		LocalFar.Y = ViewDir.X * FarDist;
		LocalFar.Z = ViewDir.Y * FarDist;

		NearWorld[i] = CameraTransform.TransformPosition(LocalNear);
		FarWorld[i] = CameraTransform.TransformPosition(LocalFar);
	}

	// Draw lines from camera origin to each corner
	for (int32 i = 0; i < 4; i++)
	{
		DrawDebugLine(World, CameraLocation, FarWorld[i], LineColor, false, -1.0f, 0, LineThickness);
	}

	// Draw lines connecting the corners (near/far plane rectangles)
	for (int32 i = 0; i < 4; i++)
	{
		int32 NextIdx = (i + 1) % 4;
		DrawDebugLine(World, FarWorld[i], FarWorld[NextIdx], LineColor, false, -1.0f, 0, LineThickness);
		DrawDebugLine(World, NearWorld[i], NearWorld[NextIdx], LineColor, false, -1.0f, 0, LineThickness);
		DrawDebugLine(World, NearWorld[i], FarWorld[i], LineColor, false, -1.0f, 0, LineThickness);
	}

	// Draw frustum planes if enabled
	if (bDrawPlanes)
	{
		const FColor PlaneColorSolid = PlaneColor.ToFColor(true);

		auto DrawQuad = [&](const FVector& A, const FVector& B, const FVector& C, const FVector& D) {
			TArray<FVector> Verts;
			Verts.Reserve(4);
			Verts.Add(A);
			Verts.Add(B);
			Verts.Add(C);
			Verts.Add(D);

			TArray<int32> Indices;
			Indices.Reserve(6);
			Indices.Add(0);
			Indices.Add(1);
			Indices.Add(2);
			Indices.Add(0);
			Indices.Add(2);
			Indices.Add(3);

			DrawDebugMesh(World, Verts, Indices, PlaneColorSolid, false, -1.0f, 0);
		};

		// Near and far planes
		DrawQuad(NearWorld[0], NearWorld[1], NearWorld[2], NearWorld[3]);
		DrawQuad(FarWorld[0], FarWorld[1], FarWorld[2], FarWorld[3]);

		// Side planes
		DrawQuad(NearWorld[0], NearWorld[3], FarWorld[3], FarWorld[0]); // Left
		DrawQuad(NearWorld[1], NearWorld[2], FarWorld[2], FarWorld[1]); // Right
		DrawQuad(NearWorld[0], NearWorld[1], FarWorld[1], FarWorld[0]); // Bottom
		DrawQuad(NearWorld[3], NearWorld[2], FarWorld[2], FarWorld[3]); // Top
	}
}

void DrawFrustumFromIntrinsics(UWorld*					World,
							   const FTransform&		CameraTransform,
							   const FCameraIntrinsics&	Intrinsics,
							   float					NearDistance,
							   float					FarDistance,
							   const FColor&			LineColor,
							   float					LineThickness,
							   bool						bDrawPlanes,
							   const FLinearColor&		PlaneColor)
{
	if (!World)
	{
		return;
	}

	float Width = static_cast<float>(Intrinsics.ImageWidth);
	float Height = static_cast<float>(Intrinsics.ImageHeight);

	// Calculate frustum corners at near and far plane
	// Using intrinsics: (x - cx) / fx = X/Z  =>  X = Z * (x - cx) / fx
	
	auto GetWorldPoint = [&](float x, float y, float depth) -> FVector {
		float X = depth * (x - Intrinsics.PrincipalPointX) / Intrinsics.FocalLengthX;
		float Y = depth * (y - Intrinsics.PrincipalPointY) / Intrinsics.FocalLengthY;
		float Z = depth;
		
		// Convert from camera space to world space
		FVector CameraSpacePoint(Z, X, -Y); // UE camera: +X forward, +Y right, +Z up
		return CameraTransform.TransformPosition(CameraSpacePoint);
	};

	// Near plane corners (in pixels: top-left, top-right, bottom-right, bottom-left)
	FVector NearCorners[4];
	NearCorners[0] = GetWorldPoint(0.0f, 0.0f, NearDistance);           // Top-left
	NearCorners[1] = GetWorldPoint(Width, 0.0f, NearDistance);          // Top-right
	NearCorners[2] = GetWorldPoint(Width, Height, NearDistance);        // Bottom-right
	NearCorners[3] = GetWorldPoint(0.0f, Height, NearDistance);         // Bottom-left

	// Far plane corners
	FVector FarCorners[4];
	FarCorners[0] = GetWorldPoint(0.0f, 0.0f, FarDistance);
	FarCorners[1] = GetWorldPoint(Width, 0.0f, FarDistance);
	FarCorners[2] = GetWorldPoint(Width, Height, FarDistance);
	FarCorners[3] = GetWorldPoint(0.0f, Height, FarDistance);

	FVector CameraLocation = CameraTransform.GetLocation();

	// Draw frustum lines
	// Near plane rectangle
	for (int32 i = 0; i < 4; ++i)
	{
		DrawDebugLine(World, NearCorners[i], NearCorners[(i + 1) % 4], LineColor, false, -1.0f, 0, LineThickness);
	}

	// Far plane rectangle
	for (int32 i = 0; i < 4; ++i)
	{
		DrawDebugLine(World, FarCorners[i], FarCorners[(i + 1) % 4], LineColor, false, -1.0f, 0, LineThickness);
	}

	// Connecting lines from near to far
	for (int32 i = 0; i < 4; ++i)
	{
		DrawDebugLine(World, NearCorners[i], FarCorners[i], LineColor, false, -1.0f, 0, LineThickness);
	}

	// Draw frustum planes if enabled
	if (bDrawPlanes)
	{
		const FColor PlaneColorSolid = PlaneColor.ToFColor(true);

		auto DrawQuad = [&](const FVector& A, const FVector& B, const FVector& C, const FVector& D) {
			TArray<FVector> Verts;
			Verts.Reserve(4);
			Verts.Add(A);
			Verts.Add(B);
			Verts.Add(C);
			Verts.Add(D);

			TArray<int32> Indices;
			Indices.Reserve(6);
			Indices.Add(0);
			Indices.Add(1);
			Indices.Add(2);
			Indices.Add(0);
			Indices.Add(2);
			Indices.Add(3);

			DrawDebugMesh(World, Verts, Indices, PlaneColorSolid, false, -1.0f, 0);
		};

		// Draw all 6 frustum faces
		// Near and far planes
		DrawQuad(NearCorners[0], NearCorners[1], NearCorners[2], NearCorners[3]);
		DrawQuad(FarCorners[0], FarCorners[1], FarCorners[2], FarCorners[3]);

		// Side planes
		DrawQuad(NearCorners[0], NearCorners[3], FarCorners[3], FarCorners[0]); // Left
		DrawQuad(NearCorners[1], NearCorners[2], FarCorners[2], FarCorners[1]); // Right
		DrawQuad(NearCorners[0], NearCorners[1], FarCorners[1], FarCorners[0]); // Bottom
		DrawQuad(NearCorners[3], NearCorners[2], FarCorners[2], FarCorners[3]); // Top
	}
}

} // namespace CameraCaptureUtils
