#include "Utilities.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderGraphUtils.h"
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

	TSharedPtr<FJsonObject> TransformToJsonObject(const FTransform& Transform)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

		FVector	 Location = Transform.GetLocation();
		FRotator Rotation = Transform.Rotator();
		FQuat	 Quaternion = Transform.GetRotation();
		FVector	 Scale = Transform.GetScale3D();

		TArray<TSharedPtr<FJsonValue>> LocationArray;
		LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
		LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
		LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
		Obj->SetArrayField(TEXT("location"), LocationArray);

		TArray<TSharedPtr<FJsonValue>> RotationArray;
		RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
		RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
		RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
		Obj->SetArrayField(TEXT("rotation"), RotationArray);

		TArray<TSharedPtr<FJsonValue>> QuaternionArray;
		QuaternionArray.Add(MakeShared<FJsonValueNumber>(Quaternion.W));
		QuaternionArray.Add(MakeShared<FJsonValueNumber>(Quaternion.X));
		QuaternionArray.Add(MakeShared<FJsonValueNumber>(Quaternion.Y));
		QuaternionArray.Add(MakeShared<FJsonValueNumber>(Quaternion.Z));
		Obj->SetArrayField(TEXT("quaternion"), QuaternionArray);

		TArray<TSharedPtr<FJsonValue>> ScaleArray;
		ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
		ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
		ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
		Obj->SetArrayField(TEXT("scale"), ScaleArray);

		return Obj;
	}

	bool EnqueueReadback(UTextureRenderTarget2D* RenderTarget, FAsyncReadback& Out)
	{
		Out.Reset();
		if (!RenderTarget)
		{
			return false;
		}

		FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
		if (!Resource)
		{
			UE_LOG(LogTemp, Warning, TEXT("EnqueueReadback: no render target resource"));
			return false;
		}

		Out.Width = RenderTarget->SizeX;
		Out.Height = RenderTarget->SizeY;
		Out.PixelFormat = GetPixelFormatFromRenderTargetFormat(RenderTarget->RenderTargetFormat);
		Out.Readback = MakeShared<FRHIGPUTextureReadback>(TEXT("CamCaptureReadback"));

		TSharedPtr<FRHIGPUTextureReadback> ReadbackPtr = Out.Readback;
		ENQUEUE_RENDER_COMMAND(CameraCaptureUtilsEnqueueReadback)
		(
			[ReadbackPtr, Resource](FRHICommandListImmediate& RHICmdList) {
				if (FRHITexture* Texture = Resource->GetRenderTargetTexture())
				{
					// Five-argument virtual: the two-argument convenience overload
					// lands on an unimplemented() base and asserts.
					ReadbackPtr->EnqueueCopy(RHICmdList, Texture, FIntVector(0, 0, 0), 0, FIntVector(0, 0, 0));
				}
			});
		return true;
	}

	bool BeginHarvestLinearColor(FAsyncReadback& Readback)
	{
		if (!Readback.IsCopyReady() || Readback.IsHarvesting())
		{
			return false;
		}

		const EPixelFormat Format = Readback.PixelFormat;
		if (Format != PF_B8G8R8A8 && Format != PF_R8G8B8A8 && Format != PF_A32B32G32R32F && Format != PF_FloatRGBA)
		{
			UE_LOG(LogTemp, Error,
				TEXT("BeginHarvestLinearColor: unsupported pixel format %d; skipping rather than reading at the wrong stride"),
				static_cast<int32>(Format));
			Readback.Reset();
			return false;
		}

		// Everything the render command writes into is shared, so it stays alive
		// even if the caller drops its FAsyncReadback before the copy lands.
		Readback.Pixels = MakeShared<TArray<FLinearColor>>();
		Readback.Done = MakeShared<FThreadSafeBool>(false);
		Readback.Failed = MakeShared<FThreadSafeBool>(false);

		TSharedPtr<FRHIGPUTextureReadback> Rb = Readback.Readback;
		TSharedPtr<TArray<FLinearColor>>   Pixels = Readback.Pixels;
		TSharedPtr<FThreadSafeBool>		   Done = Readback.Done;
		TSharedPtr<FThreadSafeBool>		   Failed = Readback.Failed;
		const int32						   Width = Readback.Width;
		const int32						   Height = Readback.Height;

		ENQUEUE_RENDER_COMMAND(CameraCaptureUtilsHarvestLinearColor)
		(
			[Rb, Pixels, Done, Failed, Width, Height, Format](FRHICommandListImmediate& RHICmdList) {
				// Lock is only legal here; from the game thread it asserts
				// IsInRenderingThread() inside FRHICommandListImmediate::Get().
				int32 RowPitchInPixels = 0;
				int32 BufferHeight = 0;
				void* SrcData = Rb->Lock(RowPitchInPixels, &BufferHeight);
				if (!SrcData)
				{
					UE_LOG(LogTemp, Error, TEXT("BeginHarvestLinearColor: failed to lock readback"));
					Rb->Unlock();
					*Failed = true;
					*Done = true;
					return;
				}

				if (Width <= 0 || Height <= 0 || RowPitchInPixels < Width || (BufferHeight > 0 && BufferHeight < Height))
				{
					UE_LOG(LogTemp, Error,
						TEXT("BeginHarvestLinearColor: staging buffer does not hold %dx%d (pitch %d, buffer height %d)"),
						Width, Height, RowPitchInPixels, BufferHeight);
					Rb->Unlock();
					*Failed = true;
					*Done = true;
					return;
				}

				Pixels->SetNumUninitialized(Width * Height);
				FLinearColor* RESTRICT Dst = Pixels->GetData();

				switch (Format)
				{
					case PF_A32B32G32R32F:
					{
						const FLinearColor* SrcRow = static_cast<const FLinearColor*>(SrcData);
						for (int32 y = 0; y < Height; ++y)
						{
							FMemory::Memcpy(Dst, SrcRow, Width * sizeof(FLinearColor));
							Dst += Width;
							SrcRow += RowPitchInPixels;
						}
						break;
					}
					case PF_FloatRGBA:
					{
						const FFloat16Color* SrcRow = static_cast<const FFloat16Color*>(SrcData);
						for (int32 y = 0; y < Height; ++y)
						{
							for (int32 x = 0; x < Width; ++x)
							{
								Dst[x] = FLinearColor(SrcRow[x]);
							}
							Dst += Width;
							SrcRow += RowPitchInPixels;
						}
						break;
					}
					default:
					{
						const FColor* SrcRow = static_cast<const FColor*>(SrcData);
						for (int32 y = 0; y < Height; ++y)
						{
							for (int32 x = 0; x < Width; ++x)
							{
								Dst[x] = FLinearColor(SrcRow[x]);
							}
							Dst += Width;
							SrcRow += RowPitchInPixels;
						}
						break;
					}
				}

				Rb->Unlock();
				*Done = true;
			});
		return true;
	}

	TArray<float> ResampleDepthNearest(const TArray<float>& Src, int32 SrcW, int32 SrcH, int32 DstW, int32 DstH)
	{
		if (SrcW <= 0 || SrcH <= 0 || DstW <= 0 || DstH <= 0)
		{
			return TArray<float>();
		}
		if (Src.Num() != SrcW * SrcH)
		{
			UE_LOG(LogTemp, Error, TEXT("ResampleDepthNearest: source is %d values, expected %dx%d"), Src.Num(), SrcW, SrcH);
			return TArray<float>();
		}
		if (SrcW == DstW && SrcH == DstH)
		{
			return Src;
		}

		TArray<float> Out;
		Out.SetNumUninitialized(DstW * DstH);

		// Map destination pixel centres into the source grid, so the result is
		// centred rather than biased towards the origin by half a pixel.
		const double ScaleX = static_cast<double>(SrcW) / static_cast<double>(DstW);
		const double ScaleY = static_cast<double>(SrcH) / static_cast<double>(DstH);

		for (int32 y = 0; y < DstH; ++y)
		{
			const int32			  SrcY = FMath::Clamp(static_cast<int32>((y + 0.5) * ScaleY), 0, SrcH - 1);
			const float* RESTRICT SrcRow = Src.GetData() + static_cast<SIZE_T>(SrcY) * SrcW;
			float* RESTRICT		  DstRow = Out.GetData() + static_cast<SIZE_T>(y) * DstW;
			for (int32 x = 0; x < DstW; ++x)
			{
				const int32 SrcX = FMath::Clamp(static_cast<int32>((x + 0.5) * ScaleX), 0, SrcW - 1);
				DstRow[x] = SrcRow[SrcX];
			}
		}
		return Out;
	}

	bool WritePNGPixels(const FString& FilePath, TArray64<FColor> Pixels, int32 Width, int32 Height)
	{
		IImageWriteQueueModule* ImageWriteQueueModule = FModuleManager::Get().GetModulePtr<IImageWriteQueueModule>("ImageWriteQueue");
		if (!ImageWriteQueueModule)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to load ImageWriteQueue module"));
			return false;
		}

		if (Width <= 0 || Height <= 0 || Pixels.Num() != static_cast<int64>(Width) * Height)
		{
			UE_LOG(LogTemp, Error, TEXT("WritePNGPixels: %lld pixels does not match %dx%d"), Pixels.Num(), Width, Height);
			return false;
		}

		TUniquePtr<TImagePixelData<FColor>> PixelData = MakeUnique<TImagePixelData<FColor>>(
			FIntPoint(Width, Height),
			MoveTemp(Pixels));

		TUniquePtr<FImageWriteTask> ImageTask = MakeUnique<FImageWriteTask>();
		ImageTask->PixelData = MoveTemp(PixelData);
		ImageTask->Filename = FilePath;
		ImageTask->Format = EImageFormat::PNG;
		ImageTask->CompressionQuality = (int32)EImageCompressionQuality::Default;
		ImageTask->bOverwriteFile = true;

		ImageWriteQueueModule->GetWriteQueue().Enqueue(MoveTemp(ImageTask));
		return true;
	}

	bool WriteEXRPixels(const FString& FilePath, TArray64<FLinearColor> Pixels, int32 Width, int32 Height)
	{
		IImageWriteQueueModule* ImageWriteQueueModule = FModuleManager::Get().GetModulePtr<IImageWriteQueueModule>("ImageWriteQueue");
		if (!ImageWriteQueueModule)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to load ImageWriteQueue module"));
			return false;
		}

		if (Width <= 0 || Height <= 0 || Pixels.Num() != static_cast<int64>(Width) * Height)
		{
			UE_LOG(LogTemp, Error, TEXT("WriteEXRPixels: %lld pixels does not match %dx%d"), Pixels.Num(), Width, Height);
			return false;
		}

		TUniquePtr<TImagePixelData<FLinearColor>> PixelData = MakeUnique<TImagePixelData<FLinearColor>>(
			FIntPoint(Width, Height),
			MoveTemp(Pixels));

		TUniquePtr<FImageWriteTask> ImageTask = MakeUnique<FImageWriteTask>();
		ImageTask->PixelData = MoveTemp(PixelData);
		ImageTask->Filename = FilePath;
		ImageTask->Format = EImageFormat::EXR;
		ImageTask->CompressionQuality = (int32)EImageCompressionQuality::Default;
		ImageTask->bOverwriteFile = true;

		ImageWriteQueueModule->GetWriteQueue().Enqueue(MoveTemp(ImageTask));
		return true;
	}

	bool WriteEXRFile(const FString& FilePath,
		const TArray<FLinearColor>&	 RgbData,
		const TArray<FLinearColor>&	 DmvData,
		int32						 Width,
		int32						 Height,
		bool						 bIncludeDepth)
	{
		if (Width <= 0 || Height <= 0)
		{
			UE_LOG(LogTemp, Error, TEXT("WriteEXRFile: invalid dimensions %dx%d"), Width, Height);
			return false;
		}

		const int32 NumPixels = Width * Height;

		// Both planes must already be on this grid. Callers with depth at a
		// different resolution resample first (ResampleDepthNearest) or write the
		// depth plane as its own file -- this function cannot know which was meant.
		if (RgbData.Num() != NumPixels || DmvData.Num() != NumPixels)
		{
			UE_LOG(LogTemp, Error, TEXT("Image data size mismatch. Expected %dx%d, got RGB:%d DMV:%d"),
				Width, Height, RgbData.Num(), DmvData.Num());
			return false;
		}

		TArray64<FLinearColor> Pixels;
		Pixels.SetNumUninitialized(NumPixels);
		FLinearColor* RESTRICT		 Out = Pixels.GetData();
		const FLinearColor* RESTRICT Rgb = RgbData.GetData();
		const FLinearColor* RESTRICT Dmv = DmvData.GetData();

		if (bIncludeDepth)
		{
			// RGB + Depth: colour from RgbData, depth from DmvData.R into alpha.
			for (int32 i = 0; i < NumPixels; ++i)
			{
				Out[i] = FLinearColor(Rgb[i].R, Rgb[i].G, Rgb[i].B, Dmv[i].R);
			}
		}
		else
		{
			// Motion vectors: X from DmvData.G, Y from DmvData.B.
			for (int32 i = 0; i < NumPixels; ++i)
			{
				Out[i] = FLinearColor(Dmv[i].G, Dmv[i].B, 0.0f, 0.0f);
			}
		}

		return WriteEXRPixels(FilePath, MoveTemp(Pixels), Width, Height);
	}

	bool WriteMetadataFile(const FString& FilePath,
		USceneCaptureComponent2D*		  Camera,
		const FCameraIntrinsics&		  Intrinsics,
		int32							  FrameNumber,
		float							  Timestamp,
		const FString&					  ActorPath,
		const FString&					  LevelName)
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
		RootObject->SetObjectField(TEXT("world_transform"), TransformToJsonObject(CameraTransform));

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
		FString					  OutputString;
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

	void DrawFrustumFromProjectionMatrix(UWorld* World,
		const FTransform&						 CameraTransform,
		const FMatrix&							 ProjectionMatrix,
		float									 NearDistance,
		float									 FarDistance,
		const FColor&							 LineColor,
		float									 LineThickness,
		bool									 bDrawPlanes,
		const FLinearColor&						 PlaneColor)
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

	void DrawFrustumFromIntrinsics(UWorld* World,
		const FTransform&				   CameraTransform,
		const FCameraIntrinsics&		   Intrinsics,
		float							   NearDistance,
		float							   FarDistance,
		const FColor&					   LineColor,
		float							   LineThickness,
		bool							   bDrawPlanes,
		const FLinearColor&				   PlaneColor)
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
		NearCorners[0] = GetWorldPoint(0.0f, 0.0f, NearDistance);	 // Top-left
		NearCorners[1] = GetWorldPoint(Width, 0.0f, NearDistance);	 // Top-right
		NearCorners[2] = GetWorldPoint(Width, Height, NearDistance); // Bottom-right
		NearCorners[3] = GetWorldPoint(0.0f, Height, NearDistance);	 // Bottom-left

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
