#include "Utilities.h"
#include "Components/SceneCaptureComponent2D.h"
#include "ImageWriteQueue.h"
#include "ImageWriteTask.h"
#include "ImagePixelData.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"

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

} // namespace CameraCaptureUtils
