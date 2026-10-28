// Tests for the segmentation class mask's plain-data side: how labels are read off tags, what the class
// table writes, and that a label mask survives the PNG round trip value for value. The GPU half (the
// stencil pass itself) needs a scene and is checked in PIE; see README "Segmentation".

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "CameraCaptureSegmentation.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Utilities.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamCapSegClassFromTags,
	"CameraCapture.Segmentation.ClassFromTags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCamCapSegClassFromTags::RunTest(const FString& Parameters)
{
	const FString Prefix(TEXT("seg:"));
	TestEqual(TEXT("first class tag wins"),
		UCameraCaptureSegmentationSubsystem::ClassFromTags({ FName(TEXT("Other")), FName(TEXT("seg:Bench")), FName(TEXT("seg:Tree")) }, Prefix),
		FName(TEXT("Bench")));
	TestEqual(TEXT("prefix is case-insensitive"),
		UCameraCaptureSegmentationSubsystem::ClassFromTags({ FName(TEXT("SEG:Hazard.AFrame")) }, Prefix), FName(TEXT("Hazard.AFrame")));
	TestTrue(TEXT("no class tag is None"), UCameraCaptureSegmentationSubsystem::ClassFromTags({ FName(TEXT("RammsProcPiece")) }, Prefix).IsNone());
	TestTrue(TEXT("a bare prefix names no class"), UCameraCaptureSegmentationSubsystem::ClassFromTags({ FName(TEXT("seg:")) }, Prefix).IsNone());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamCapSegTableCsv,
	"CameraCapture.Segmentation.TableCsv",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCamCapSegTableCsv::RunTest(const FString& Parameters)
{
	FCameraCaptureSegTable T;
	TestEqual(TEXT("256 fine values"), T.FineNames.Num(), 256);
	TestEqual(TEXT("value 0 is unlabeled"), T.FineNames[0], FName(TEXT("unlabeled")));
	TestEqual(TEXT("coarse 0 is unlabeled"), T.CoarseNames[0], FName(TEXT("unlabeled")));

	T.CoarseNames.Add(FName(TEXT("street_furniture")));
	T.FineNames[7] = FName(TEXT("Bench"));
	T.FineToCoarse[7] = 1;
	T.Reserved.Add(200);

	const FString Csv = T.ToCsv();
	TArray<FString> Lines;
	Csv.ParseIntoArrayLines(Lines);
	TestEqual(TEXT("header, unlabeled, one class, one reserved"), Lines.Num(), 4);
	TestEqual(TEXT("header"), Lines[0], FString(TEXT("value,name,coarse_id,coarse_name,reserved")));
	TestEqual(TEXT("unlabeled row"), Lines[1], FString(TEXT("0,unlabeled,0,unlabeled,0")));
	TestEqual(TEXT("class row"), Lines[2], FString(TEXT("7,Bench,1,street_furniture,0")));
	TestEqual(TEXT("reserved row"), Lines[3], FString(TEXT("200,,0,,1")));
	TestTrue(TEXT("json lists the class"), T.ToJson().Contains(TEXT("\"Bench\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamCapSegMaskPngRoundTrip,
	"CameraCapture.Segmentation.MaskPngRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCamCapSegMaskPngRoundTrip::RunTest(const FString& Parameters)
{
	// Every value 0-255, in an odd-sized image, so a channel swap, a gamma or a stride error all show.
	const int32	  W = 37, H = 11;
	TArray<uint8> Mask;
	Mask.SetNumUninitialized(W * H);
	for (int32 i = 0; i < W * H; ++i)
	{
		Mask[i] = static_cast<uint8>((i * 7) % 256);
	}
	const FString Path = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("seg_mask_roundtrip.png"));
	TestTrue(TEXT("written"), CameraCaptureUtils::WriteGrayPNG(Path, Mask.GetData(), Mask.Num(), W, H, 8));

	TArray64<uint8> Compressed;
	TestTrue(TEXT("read back"), FFileHelper::LoadFileToArray(Compressed, *Path));
	IImageWrapperModule&	   Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName(TEXT("ImageWrapper")));
	TSharedPtr<IImageWrapper> Png = Module.CreateImageWrapper(EImageFormat::PNG);
	TArray64<uint8>			   Raw;
	const bool				   bDecoded = Png.IsValid() && Png->SetCompressed(Compressed.GetData(), Compressed.Num()) && Png->GetRaw(ERGBFormat::Gray, 8, Raw);
	TestTrue(TEXT("decoded as 8-bit gray"), bDecoded);
	if (bDecoded)
	{
		TestEqual(TEXT("size"), Png->GetWidth() * Png->GetHeight(), static_cast<int64>(W * H));
		TestEqual(TEXT("bytes"), Raw.Num(), static_cast<int64>(W * H));
		bool bSame = Raw.Num() == W * H;
		for (int32 i = 0; bSame && i < W * H; ++i)
		{
			bSame = Raw[i] == Mask[i];
		}
		TestTrue(TEXT("every value comes back unchanged"), bSame);
	}
	IFileManager::Get().Delete(*Path);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
