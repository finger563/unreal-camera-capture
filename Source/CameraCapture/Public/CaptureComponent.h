// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Utilities.h"
#include "CaptureComponent.generated.h"

class USceneCaptureComponent2D;
class URammsSceneCaptureComponent2D;
class UTextureRenderTarget2D;
class UMaterialInterface;

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent) )
class UCaptureComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    // Sets default values for this component's properties
    UCaptureComponent();

    // Period of data generation timer (seconds). If 0, no timer is used and data is generated every frame.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture")
    float TimerPeriod = 0.5f;

    // Delay before the timer starts (seconds). If 0, the timer starts immediately.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture")
    float TimerDelay = 0.5f;

    // What source do we use for RGB SceneCaptureComponent2D data?
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture")
    TEnumAsByte<ESceneCaptureSource> CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;

    // Reference to objects to not render in data collection cameras
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Capture")
    TArray<AActor*> HiddenActors;

    // Get the list of actors to not render in data collection cameras
    UFUNCTION(BlueprintCallable, Category = "Capture")
    TArray<AActor*> GetHiddenActors();

    // Updates the list of actors to not render in data collection cameras
    UFUNCTION(BlueprintCallable, Category = "Capture")
    void SetHiddenActors(TArray<AActor*> actors);

    // Get this path by right clicking on the asset in UE content browser and
    // selecting "Copy Reference"
    FString MaterialPath = "/Script/Engine.Material'/CameraCapture/M_DmvCapture.M_DmvCapture'";

    // Material base that will be used to render the DMV data - it should render
    // the depth data into the red channel, the motion vector into the green and
    // blue channels, and set the alpha channel to 1.0
    UPROPERTY(EditDefaultsOnly, Category = "Capture")
    UMaterialInterface* DmvMaterialBase = LoadMaterialFromPath(FName(*MaterialPath));

    // Start / resume capturing data
    UFUNCTION(BlueprintCallable, Category = "Capture")
    void StartCapturing();

    // Stop / pause capturing data
    UFUNCTION(BlueprintCallable, Category = "Capture")
    void StopCapturing();

    // Is the component currently capturing data?
    UFUNCTION(BlueprintCallable, Category = "Capture")
    bool IsCapturing();

    // Toggle whether the component should be capturing data or not. Returns the new state (true = capturing)
    UFUNCTION(BlueprintCallable, Category = "Capture")
    bool ToggleCapturing();

    // Start / resume saving data to disk (creating the files if they don't
    // exist)
    UFUNCTION(BlueprintCallable, Category = "Serialization")
    void StartSavingData();

    // Stop / pause saving data to disk
    UFUNCTION(BlueprintCallable, Category = "Serialization")
    void StopSavingData();

    // Is the component currently serializing / saving data?
    UFUNCTION(BlueprintCallable, Category = "Serialization")
    bool IsSavingData();

    // Toggle whether the component should be serializing or not. Returns the new state (true = saving)
    UFUNCTION(BlueprintCallable, Category = "Serialization")
    bool ToggleSavingData();

    // Location (folder) where we should save the data. Default (empty) will
    // create a "camera_data" folder in the project root directory.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Serialization")
    FString SaveLocation = "";

    // Array of child camera components for DMV data
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture")
    TArray<USceneCaptureComponent2D*> DmvCameras;

    // Array of child camera components for RGB data
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture")
    TArray<USceneCaptureComponent2D*> RgbCameras;

    // Array of render targets for DMV data
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture")
    TArray<UTextureRenderTarget2D*> DmvTextures;

    // Array of render targets for RGB data
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capture")
    TArray<UTextureRenderTarget2D*> RgbTextures;

protected:

    virtual void BeginPlay() override;

    // Camera / capture initialization functions
    void ConfigureCameras();
    void ConfigureDmvCamera(USceneCaptureComponent2D* camera);
    void ConfigureRgbCamera(USceneCaptureComponent2D* camera);
    USceneCaptureComponent2D* CopyAndAttachCamera(USceneCaptureComponent2D* camera, FString name_suffix);
    UTextureRenderTarget2D* MakeRenderTexture(int width, int height);

    // Serialization functions
    void InitializeFiles();
    void TimerUpdateCallback();
    void InitOutput();
    void WriteConfigFile();
    void WriteTransformHeader();
    void UpdateTransformFile();

    // Data capture functions
    void CaptureData();
    void SaveData();

    // Creates an async task that will save the captured image to disk
    void RunAsyncImageSaveTask(TArray<FLinearColor> Image, FString ImageName, int width, int height);

    // Timer for handling state update and rendering
    FTimerHandle CaptureTimerHandle;

    int ImageIndex = 0;
    bool ShouldCaptureData = true;
    bool ShouldSaveData = false;
    bool DeferredCaptureReady = false;
    bool HasInitializedFiles = false;

    FString TransformFile;
    FString ConfigFile;

public:

    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
};

class AsyncSaveImageToDiskTask : public FNonAbandonableTask{
public:
    AsyncSaveImageToDiskTask(TArray<FLinearColor> Image, FString ImageName, int _width, int _height);
    ~AsyncSaveImageToDiskTask();

    FORCEINLINE TStatId GetStatId() const{
        RETURN_QUICK_DECLARE_CYCLE_STAT(AsyncSaveImageToDiskTask, STATGROUP_ThreadPoolAsyncTasks);
    }

protected:
    TArray<FLinearColor> ImageCopy;
    FString FileName = "";
    int width;
    int height;

public:
    void DoWork();
};
