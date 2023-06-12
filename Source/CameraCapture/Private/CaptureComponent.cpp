// Fill out your copyright notice in the Description page of Project Settings.

#include "CaptureComponent.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/GameplayStatics.h"
#include "Serialization/BufferArchive.h"

// stl includes
#include <limits>
#include <cmath>

// Sets default values
UCaptureComponent::UCaptureComponent()
{
  PrimaryComponentTick.bCanEverTick = true;
  SetComponentTickEnabled(true);
}

// Called when the game starts or when spawned
void UCaptureComponent::BeginPlay()
{
  Super::BeginPlay();

  GetOwner()->GetComponents(DmvCameras);

  if (DmvCameras.Num()) {
    ConfigureCameras();

    if (TimerPeriod > 0.0f) {
      UE_LOG(LogTemp, Warning, TEXT("Timer period > 0, capturing every %f seconds!"), TimerPeriod);
      // if we have DmvCameras, start the rendering timer (if we're not
      // capturing every frame)
      GetOwner()->GetWorldTimerManager().SetTimer(CaptureTimerHandle,
                                                  this,
                                                  &UCaptureComponent::TimerUpdateCallback,
                                                  TimerPeriod,
                                                  true,
                                                  TimerDelay);
    } else {
      UE_LOG(LogTemp, Warning, TEXT("Timer Period <= 0, capturing every frame!"));
    }
  }
  else {
    UE_LOG(LogTemp, Warning, TEXT("UCaptureComponent:: Could not find any camera components on this actor!"));
  }
}

void UCaptureComponent::TimerUpdateCallback()
{
  UpdateTransformFile();
  CaptureData();
}

// Called every frame
void UCaptureComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
  Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

  // make sure to do deferred saving FIRST, to ensure data consistency if we
  // capture every frame
  if (DeferredCaptureReady) {
    SaveData();
    // reset the state variable here
    DeferredCaptureReady = false;
  }

  // If the timer period is set to 0 (or less), we're capturing every frame, so
  // we can want to capture in the TickComponent function. Otherwise it's called
  // within the timer callback function
  if (TimerPeriod <= 0.0f) {
    UpdateTransformFile();
    CaptureData();
  }
}

void UCaptureComponent::ConfigureCameras()
{
  for (auto camera : DmvCameras) {
    UE_LOG(LogTemp, Warning, TEXT("UCaptureComponent:: Found camera %s!"), *camera->GetFName().ToString());
    ConfigureDmvCamera(camera);
    auto rgbCamera = MakeRgbCameraFromDmvCamera(camera);
    // store the rgbCamera pointer
    RgbCameras.Add(rgbCamera);
  }
  // set the hidden actors for all the cameras
  SetHiddenActors(HiddenActors);
}

void UCaptureComponent::ConfigureDmvCamera(USceneCaptureComponent2D* camera)
{
  // we control the capture of the cameras, so turn off the
  // auto-capture
  camera->bCaptureEveryFrame = false;
  if (!camera->bCaptureEveryFrame) {
    // Make sure the cameras don't auto-capture on movement
    camera->bCaptureOnMovement = false;
    // make sure the camera rendering state is saved between frames
    // (otherwise we get a black image)
    camera->bAlwaysPersistRenderingState = true;
  }
  // where do we pull color data from? options are
  // https://docs.unrealengine.com/en-US/API/Runtime/Engine/Engine/ESceneCaptureSource/index.html
  camera->CaptureSource = CaptureSource;

  // make the DmvMaterial
  if (DmvMaterialBase) {
    auto DmvMaterial = UMaterialInstanceDynamic::Create(DmvMaterialBase, this);
    if (!DmvMaterial) {
      UE_LOG(LogTemp, Error, TEXT("Error, could not create DmvMaterial!"));
    }
    // set the post process material (DMV)
    // https://docs.unrealengine.com/en-US/API/Runtime/Engine/Components/USceneCaptureComponent2D/PostProcessSettings/index.html
    camera->PostProcessSettings.WeightedBlendables.Array.Empty();
    camera->PostProcessSettings.WeightedBlendables.Array.Add(FWeightedBlendable(1.0f, DmvMaterial));
  } else {
    UE_LOG(LogTemp, Error, TEXT("No DmvMaterial set!"));
  }

  auto depth = MakeRenderTexture(ImageWidth, ImageHeight);
  DmvTextures.Add(depth);
  camera->TextureTarget = depth;
}

USceneCaptureComponent2D* UCaptureComponent::MakeRgbCameraFromDmvCamera(USceneCaptureComponent2D* camera)
{
  // NOTE: we don't have to do all the configuration for the camera that we did
  // for the dmv camera (e.g. post process material) since we're just going to
  // copy the dmv camera's config to the rgb camera's config

  // create the color camera - initialize it with the same
  // settings as the dmv camera and attach it to the dmv camera
  auto name = camera->GetFName().ToString() + FString("_rgb");
  UE_LOG(LogTemp, Warning, TEXT("Making RGB camera '%s' from DMV camera."), *name);
  auto rgbCamera = NewObject<USceneCaptureComponent2D>(this,
                                                       USceneCaptureComponent2D::StaticClass(),
                                                       FName(*name),
                                                       EObjectFlags::RF_NoFlags, // flags
                                                       camera // tmplate object for initializing
                                                       );
  auto hitResult = FHitResult();
  rgbCamera->K2_SetRelativeLocationAndRotation(FVector(0,0,0), FRotator(0,0,0), false, hitResult, true);
  auto rule = EAttachmentRule::KeepRelative;
  // rules: location, rotation, scale, weld bodies when attaching
  auto attachmentRules = FAttachmentTransformRules(rule, rule, rule, true);
  rgbCamera->AttachToComponent(camera, attachmentRules);

  // make sure there are no post process materials / effects on the
  // rgb camera (since we're just copying the dmv camera's config)
  rgbCamera->PostProcessSettings.WeightedBlendables.Array.Empty();

  // make the rgb texture
  auto rgb = MakeRenderTexture(ImageWidth, ImageHeight);
  RgbTextures.Add(rgb);
  rgbCamera->TextureTarget = rgb;
  return rgbCamera;
}

UTextureRenderTarget2D* UCaptureComponent::MakeRenderTexture(int width, int height)
{
  auto renderTexture = NewObject<UTextureRenderTarget2D>();
  renderTexture->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA32f;
  renderTexture->ResizeTarget(width, height);
  renderTexture->UpdateResource();
  return renderTexture;
}

void UCaptureComponent::InitializeFiles()
{
  if (!HasInitializedFiles) {
    InitOutput();
    WriteConfigFile();
    WriteTransformHeader();
  }
  HasInitializedFiles = true;
}

void UCaptureComponent::StartCapturing()
{
  ShouldCaptureData = true;
}

void UCaptureComponent::StopCapturing()
{
  ShouldCaptureData = false;
}

bool UCaptureComponent::IsCapturing()
{
  return ShouldCaptureData;
}

bool UCaptureComponent::ToggleCapturing()
{
  if (ShouldCaptureData) {
    StopCapturing();
  } else {
    StartCapturing();
  }
  return IsCapturing();
}

void UCaptureComponent::StartSavingData()
{
  // set the ShouldSaveData variable to true to resume / start saving
  // rendering and transform data
  ShouldSaveData = true;
  // Ensure that we create the necessary folders and files if
  // necessary
  InitializeFiles();
}

void UCaptureComponent::StopSavingData()
{
  // set the ShouldSaveData state variable to false to stop saving rendering
  // & transform data
  ShouldSaveData = false;
}

bool UCaptureComponent::IsSavingData()
{
  return ShouldSaveData;
}

bool UCaptureComponent::ToggleSavingData()
{
  if (ShouldSaveData) {
    StopSavingData();
  } else {
    StartSavingData();
  }
  return IsSavingData();
}

TArray<AActor*> UCaptureComponent::GetHiddenActors()
{
  return HiddenActors;
}

void UCaptureComponent::SetHiddenActors(TArray<AActor*> actors)
{
  HiddenActors = actors;
  // make sure to update the hidden actors for all the cameras
  for (auto camera : DmvCameras) {
    camera->HiddenActors = HiddenActors;
  }
  for (auto camera : RgbCameras) {
    camera->HiddenActors = HiddenActors;
  }
}

void UCaptureComponent::InitOutput()
{
  if (SaveLocation.IsEmpty()) {
    SaveLocation = FPaths::Combine(*FPaths::ProjectDir(), *FString("camera_data"));
    FString displayPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*SaveLocation);
    UE_LOG(LogTemp, Error,
           TEXT("No output directory provided, creating 'camera_data' folder in game directory: %s"),
           *displayPath);
  }
  if (!IFileManager::Get().DirectoryExists(*SaveLocation)) {
    IFileManager::Get().MakeDirectory(*SaveLocation);
  }
  ConfigFile = FPaths::Combine(*SaveLocation, *FString("camera_config.csv"));
  TransformFile = FPaths::Combine(*SaveLocation, *FString("transformations.csv"));
}

void UCaptureComponent::WriteConfigFile()
{
  FString configString("name,width,height,focalLength,fov,nearClipPlane,farClipPlane,tx,ty,tz,qw,qx,qy,qz\n");
  for (auto camera : DmvCameras) {
    auto transform = camera->GetComponentTransform().GetRelativeTransform(GetOwner()->GetTransform());
    // for distances here we divide by 100.0 since unreal is in cm and we want to convert to m
    auto t = transform.GetTranslation() / 100.0f;
    auto q = transform.GetRotation();
    float focalLength = 0.0f; // TODO: we cannot get focal length here!
    // Note: all cameras share the same frustum configuration (near/far planes)
    float nearPlane = GNearClippingPlane / 100.0f;
    // there is no far clip plane in UE4, according to
    // https://forums.unrealengine.com/development-discussion/rendering/1580676-far-clip-plane
    float farPlane = std::numeric_limits<float>::infinity();
    FString cameraString = FString::Printf(
                                           TEXT("%s,%d,%d,%.2f,%.2f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n"),
                                           *camera->GetFName().ToString(),
                                           ImageWidth, ImageHeight,
                                           focalLength,
                                           camera->FOVAngle,
                                           nearPlane, farPlane,
                                           t.X, t.Y, t.Z,
                                           q.W, q.X, q.Y, q.Z
                                           );
    configString += cameraString;
  }
  bool didWrite = FFileHelper::SaveStringToFile(configString, *ConfigFile);
  FString displayPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*ConfigFile);
  if (!didWrite) {
    UE_LOG(LogTemp, Error, TEXT("Error: could not write config file %s"), *displayPath);
  } else {
    UE_LOG(LogTemp, Warning, TEXT("Wrote config file %s"), *displayPath);
  }
}

void UCaptureComponent::WriteTransformHeader()
{
  FString transformString("i,time,tx,ty,tz,qw,qx,qy,qz\n");
  bool didWrite = FFileHelper::SaveStringToFile(transformString, *TransformFile);
  FString displayPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*TransformFile);
  if (!didWrite) {
    UE_LOG(LogTemp, Error, TEXT("Error: could not write transform file %s"), *displayPath);
  } else {
    UE_LOG(LogTemp, Warning, TEXT("Writing transform file %s"), *displayPath);
  }
}

void UCaptureComponent::UpdateTransformFile()
{
  if (!ShouldSaveData) {
    return;
  }
  auto transform = GetOwner()->GetTransform();
  auto t = transform.GetTranslation() / 100.0f; // unreal is in cm, convert to m
  auto q = transform.GetRotation();
  float time = GetWorld()->GetTimeSeconds(); // other option is UGameplayStatics::GetRealTimeSeconds()
  FString transformString = FString::Printf(
                                            TEXT("%d,%f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n"),
                                            ImageIndex,
                                            time,
                                            t.X, t.Y, t.Z,
                                            q.W, q.X, q.Y, q.Z
                                            );
  bool didWrite = FFileHelper::SaveStringToFile(
                                                transformString,
                                                *TransformFile,
                                                FFileHelper::EEncodingOptions::AutoDetect,
                                                &IFileManager::Get(),
                                                FILEWRITE_Append
                                                );
  if (!didWrite) {
    UE_LOG(LogTemp, Error, TEXT("Error: could not append to transform file %s"), *TransformFile);
  }
}

void UCaptureComponent::RunAsyncImageSaveTask(TArray<FLinearColor> Image, FString ImageName, int width, int height)
{
  (new FAutoDeleteAsyncTask<AsyncSaveImageToDiskTask>(Image, ImageName, width, height))->StartBackgroundTask();
}

void UCaptureComponent::CaptureData()
{
  // if we're not capturing data, just return
  if (!ShouldCaptureData)
    return;

  // Start deferred capture of the scene (DMV)
  for (auto camera : DmvCameras) {
    camera->CaptureSceneDeferred();
  }
  // Start deferred capture of the scene (RGB)
  for (auto camera : RgbCameras) {
    camera->CaptureSceneDeferred();
  }

  DeferredCaptureReady = true;
}

void UCaptureComponent::SaveData() {
  if (!ShouldSaveData) {
    return;
  }
  FString suffix = "_" + FString::FromInt(ImageIndex) + ".raw";
  for (int i=0; i<DmvCameras.Num(); i++) {
    // get the camera (we use DmvCameras because we didn't touch their names)
    auto camera = DmvCameras[i];

    // get the dmv render texture data
    auto dmvRT = DmvTextures[i];
    if (!dmvRT || !dmvRT->GetResource()) {
      UE_LOG(LogTemp, Error, TEXT("Bad DmvTexture || resource. Continuing"));
      // continue, skipping this RT, we can't get its data
      continue;
    }
    // get the rgb render texture data
    auto rgbRT = RgbTextures[i];
    if (!rgbRT || !rgbRT->GetResource()) {
      UE_LOG(LogTemp, Error, TEXT("Bad RgbTexture || resource. Continuing"));
      // continue, skipping this RT, we can't get its data
      continue;
    }

    // copy the dmv and rgb image data from GPU to CPU
    TArray<FLinearColor> dmvData;
    TArray<FLinearColor> colorData;
    dmvData.SetNum(dmvRT->SizeX * dmvRT->SizeY);
    colorData.SetNum(rgbRT->SizeX * rgbRT->SizeY);
    dmvRT->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(dmvData);
    rgbRT->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(colorData);

    // setup the output
    FString prefix = FPaths::Combine(*SaveLocation, *camera->GetFName().ToString());
    FString dmvFileName = prefix + "_depth_motion" + suffix;
    FString rgbFileName = prefix + suffix;

    // now spawn tasks to save the image data to files
    RunAsyncImageSaveTask(dmvData, dmvFileName, dmvRT->SizeX, dmvRT->SizeY);
    RunAsyncImageSaveTask(colorData, rgbFileName, dmvRT->SizeX, rgbRT->SizeY);
  }
  // now update the state variable for which image we're on
  ImageIndex++;
}

AsyncSaveImageToDiskTask::AsyncSaveImageToDiskTask(TArray<FLinearColor> Image, FString ImageName, int _width, int _height)
{
  ImageCopy = Image;
  FileName = ImageName;
  width = _width;
  height = _height;
}

AsyncSaveImageToDiskTask::~AsyncSaveImageToDiskTask()
{
}

void AsyncSaveImageToDiskTask::DoWork()
{
  const int numChannels = 4;
  const int numBytesPerChannel = sizeof(float);

  FText PathError;
  FPaths::ValidatePath(FileName, &PathError);
  if (PathError.IsEmpty() && !FileName.IsEmpty()) {
    auto Ar = IFileManager::Get().CreateFileWriter(*FileName);
    if (Ar) {
      FBufferArchive rawBytes;
      size_t bufferSize = ImageCopy.Num() * numBytesPerChannel * numChannels;
      rawBytes.Init(0, bufferSize);
      for (int y = height - 1; y >= 0; y--) {
        for (int x = 0; x < width; x++) {
          int index = x + y * width;
          int offset = index * (numChannels * numBytesPerChannel);
          float _floats[numChannels] = {
            ImageCopy[index].R, // depth
            ImageCopy[index].G, // motion x
            ImageCopy[index].B, // motion y
            ImageCopy[index].A, // unused
          };
          memcpy(&rawBytes.GetData()[offset], &_floats, numBytesPerChannel * numChannels);
        }
      }
      Ar->Serialize(rawBytes.GetData(), rawBytes.Num());
      delete Ar;
    } else {
      UE_LOG(LogTemp, Error, TEXT("Bad Ar!"));
    }
  } else {
    UE_LOG(LogTemp, Error, TEXT("Invalid file path provdied: %s!"), *PathError.ToString());
  }
}

