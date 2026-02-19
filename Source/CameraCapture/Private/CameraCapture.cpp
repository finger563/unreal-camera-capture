// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "CameraCapture.h"
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FCameraCaptureModule"

void FCameraCaptureModule::StartupModule()
{
	// Register the plugin's Shaders/ directory as a virtual shader include path.
	// This lets Custom HLSL nodes in materials use: #include "/CameraCapture/Private/LensDistortion.usf"
	FString PluginShaderDir = FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("CameraCapture"), TEXT("Shaders"));
	FPaths::CollapseRelativeDirectories(PluginShaderDir);

	if (FPaths::DirectoryExists(PluginShaderDir))
	{
		AddShaderSourceDirectoryMapping(TEXT("/CameraCapture"), PluginShaderDir);
		UE_LOG(LogTemp, Log, TEXT("CameraCapture: Registered shader source directory: %s"), *PluginShaderDir);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("CameraCapture: Shader directory not found at: %s"), *PluginShaderDir);
	}
}

void FCameraCaptureModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FCameraCaptureModule, CameraCapture)
