// Tests for the case where depth is captured at a different resolution than colour.
//
// A camera with separate depth intrinsics (RealSense-style: 1280x720 colour,
// 848x480 depth) produces three planes in two different grids inside one
// FCaptureData. Everything here pins the arithmetic that used to assume one grid:
//
//  - the depth planes are indexed by DepthWidth/DepthHeight, not Width/Height,
//    so a mismatch is no longer "wrong size, write zeros";
//  - the resample that puts depth into the combined file's alpha channel is
//    nearest-neighbour, lands on pixel centres, and never invents a distance;
//  - a readback is interpreted with the stride its pixel format actually has.
//
// The pixel-format half is checked as arithmetic rather than by driving a GPU
// readback: the defect was that RGBA16f (PF_FloatRGBA, 8 bytes/pixel) was read
// as FLinearColor (16 bytes/pixel), so what matters is that the sizes these
// tests assert are the sizes the harvest code switches on.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "CameraCaptureSubsystem.h"
#include "Utilities.h"
#include "Engine/TextureRenderTarget2D.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/** A depth plane whose every value is distinguishable, so a resample can be
	 *  checked value-by-value rather than only by size. */
	TArray<float> MakeRamp(int32 W, int32 H)
	{
		TArray<float> Out;
		Out.SetNumUninitialized(W * H);
		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x = 0; x < W; ++x)
			{
				Out[y * W + x] = static_cast<float>(y * W + x);
			}
		}
		return Out;
	}
} // namespace

// ---------------------------------------------------------------------------
// ResampleDepthNearest
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamCapResampleIsIdentityAtTheSameSize,
	"CameraCapture.Depth.ResampleIsIdentityAtTheSameSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCamCapResampleIsIdentityAtTheSameSize::RunTest(const FString& Parameters)
{
	const TArray<float> Src = MakeRamp(8, 4);
	const TArray<float> Out = CameraCaptureUtils::ResampleDepthNearest(Src, 8, 4, 8, 4);

	TestEqual(TEXT("same size returns the same number of values"), Out.Num(), Src.Num());
	for (int32 i = 0; i < Src.Num(); ++i)
	{
		if (Out[i] != Src[i])
		{
			AddError(FString::Printf(TEXT("value %d changed: %f -> %f"), i, Src[i], Out[i]));
			break;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamCapResampleOnlyEverReturnsSourceValues,
	"CameraCapture.Depth.ResampleOnlyEverReturnsSourceValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCamCapResampleOnlyEverReturnsSourceValues::RunTest(const FString& Parameters)
{
	// The point of nearest over bilinear: a depth map must not contain a distance
	// the scene never had. Two surfaces, far apart, and nothing in between.
	const int32	  SrcW = 4, SrcH = 4;
	TArray<float> Src;
	Src.Init(0.0f, SrcW * SrcH);
	for (int32 i = 0; i < SrcW * SrcH; ++i)
	{
		Src[i] = (i % SrcW < 2) ? 100.0f : 5000.0f;
	}

	// Up and down, neither an integer ratio.
	const int32 Sizes[][2] = { { 7, 7 }, { 3, 2 }, { 13, 5 }, { 1, 1 } };
	for (const auto& Size : Sizes)
	{
		const TArray<float> Out = CameraCaptureUtils::ResampleDepthNearest(Src, SrcW, SrcH, Size[0], Size[1]);
		TestEqual(FString::Printf(TEXT("%dx%d output size"), Size[0], Size[1]), Out.Num(), Size[0] * Size[1]);
		for (int32 i = 0; i < Out.Num(); ++i)
		{
			if (Out[i] != 100.0f && Out[i] != 5000.0f)
			{
				AddError(FString::Printf(TEXT("%dx%d invented depth %f at %d — bilinear would do this, nearest must not"),
					Size[0], Size[1], Out[i], i));
				break;
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamCapResampleSamplesPixelCentres,
	"CameraCapture.Depth.ResampleSamplesPixelCentres",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCamCapResampleSamplesPixelCentres::RunTest(const FString& Parameters)
{
	// Halving a 4x4 must take one sample from each 2x2 block, not four copies of
	// the top-left corner — the bug a floor() without the +0.5 would produce.
	const TArray<float> Src = MakeRamp(4, 4); // values 0..15
	const TArray<float> Out = CameraCaptureUtils::ResampleDepthNearest(Src, 4, 4, 2, 2);

	TestEqual(TEXT("output size"), Out.Num(), 4);
	// Destination centres land at source (1,1), (3,1), (1,3), (3,3) => 5, 7, 13, 15
	TestEqual(TEXT("top-left block"), Out[0], 5.0f);
	TestEqual(TEXT("top-right block"), Out[1], 7.0f);
	TestEqual(TEXT("bottom-left block"), Out[2], 13.0f);
	TestEqual(TEXT("bottom-right block"), Out[3], 15.0f);

	// All four distinct: a corner-biased resample would repeat values.
	TSet<float> Distinct(Out);
	TestEqual(TEXT("each block contributed its own sample"), Distinct.Num(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamCapResampleRejectsBadInput,
	"CameraCapture.Depth.ResampleRejectsBadInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCamCapResampleRejectsBadInput::RunTest(const FString& Parameters)
{
	const TArray<float> Src = MakeRamp(4, 4);

	TestEqual(TEXT("zero destination width yields nothing"),
		CameraCaptureUtils::ResampleDepthNearest(Src, 4, 4, 0, 4).Num(), 0);
	TestEqual(TEXT("negative source height yields nothing"),
		CameraCaptureUtils::ResampleDepthNearest(Src, 4, -1, 4, 4).Num(), 0);

	// A source whose length disagrees with its claimed dimensions is the exact
	// shape of the original defect; it must be refused, not read past.
	AddExpectedError(TEXT("ResampleDepthNearest: source is"), EAutomationExpectedErrorFlags::Contains, 1);
	TestEqual(TEXT("length disagreeing with dimensions yields nothing"),
		CameraCaptureUtils::ResampleDepthNearest(Src, 8, 8, 4, 4).Num(), 0);
	return true;
}

// ---------------------------------------------------------------------------
// FCaptureData geometry
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamCapMismatchIsDetectedAndOnlyWhenReal,
	"CameraCapture.Depth.MismatchIsDetectedAndOnlyWhenReal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCamCapMismatchIsDetectedAndOnlyWhenReal::RunTest(const FString& Parameters)
{
	FCaptureData Data;
	Data.Width = 1280;
	Data.Height = 720;

	// No depth captured at all: not a mismatch, so callers do not resample.
	Data.DepthWidth = 0;
	Data.DepthHeight = 0;
	TestFalse(TEXT("absent depth is not a mismatch"), Data.HasMismatchedDepthResolution());

	// Same grid: not a mismatch.
	Data.DepthWidth = 1280;
	Data.DepthHeight = 720;
	TestFalse(TEXT("matching depth is not a mismatch"), Data.HasMismatchedDepthResolution());

	// RealSense D435-ish: same aspect family, different size.
	Data.DepthWidth = 848;
	Data.DepthHeight = 480;
	TestTrue(TEXT("different depth size is a mismatch"), Data.HasMismatchedDepthResolution());

	// Differing aspect ratio, which is the case the report called out.
	Data.Width = 640;
	Data.Height = 480; // 4:3
	Data.DepthWidth = 640;
	Data.DepthHeight = 360; // 16:9
	TestTrue(TEXT("differing aspect ratio is a mismatch"), Data.HasMismatchedDepthResolution());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamCapDepthPlanesAreSizedByTheirOwnGrid,
	"CameraCapture.Depth.DepthPlanesAreSizedByTheirOwnGrid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCamCapDepthPlanesAreSizedByTheirOwnGrid::RunTest(const FString& Parameters)
{
	// The regression this guards: depth sized correctly for its own grid used to
	// be judged against the colour pixel count and discarded.
	FCaptureData Data;
	Data.Width = 640;
	Data.Height = 480;
	Data.DepthWidth = 320;
	Data.DepthHeight = 240;

	Data.ImageData.SetNumZeroed(Data.Width * Data.Height);
	Data.DepthData.Init(1234.0f, Data.DepthWidth * Data.DepthHeight);
	Data.MotionVectorData.SetNumZeroed(Data.DepthWidth * Data.DepthHeight);

	TestNotEqual(TEXT("the two grids really do differ"),
		Data.DepthData.Num(), Data.ImageData.Num());
	TestEqual(TEXT("depth is complete on its own grid"),
		Data.DepthData.Num(), Data.DepthWidth * Data.DepthHeight);
	TestEqual(TEXT("motion is complete on the depth grid"),
		Data.MotionVectorData.Num(), Data.DepthWidth * Data.DepthHeight);

	// And it reaches the colour grid intact, carrying real distances.
	const TArray<float> OnColour = CameraCaptureUtils::ResampleDepthNearest(
		Data.DepthData, Data.DepthWidth, Data.DepthHeight, Data.Width, Data.Height);
	TestEqual(TEXT("resampled depth fills the colour grid"), OnColour.Num(), Data.Width * Data.Height);
	for (int32 i = 0; i < OnColour.Num(); ++i)
	{
		if (OnColour[i] != 1234.0f)
		{
			AddError(FString::Printf(TEXT("resampled depth lost its value at %d: %f"), i, OnColour[i]));
			break;
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Readback stride
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamCapFloatFormatsHaveDifferentStrides,
	"CameraCapture.Depth.FloatFormatsHaveDifferentStrides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCamCapFloatFormatsHaveDifferentStrides::RunTest(const FString& Parameters)
{
	// The crash: both of these were treated as "is float" and read as
	// FLinearColor. They are not the same width, so that walked twice the
	// staging buffer for one of them.
	TestEqual(TEXT("RGBA32f is FLinearColor-shaped"),
		static_cast<int32>(GPixelFormats[PF_A32B32G32R32F].BlockBytes), static_cast<int32>(sizeof(FLinearColor)));
	TestEqual(TEXT("RGBA16f is FFloat16Color-shaped"),
		static_cast<int32>(GPixelFormats[PF_FloatRGBA].BlockBytes), static_cast<int32>(sizeof(FFloat16Color)));
	TestTrue(TEXT("and the two strides differ, so one cast cannot serve both"),
		GPixelFormats[PF_A32B32G32R32F].BlockBytes != GPixelFormats[PF_FloatRGBA].BlockBytes);

	// 8-bit colour is FColor-shaped, which the memcpy path relies on.
	TestEqual(TEXT("RGBA8 is FColor-shaped"),
		static_cast<int32>(GPixelFormats[PF_B8G8R8A8].BlockBytes), static_cast<int32>(sizeof(FColor)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCamCapRenderTargetFormatsMapAsTheHarvestExpects,
	"CameraCapture.Depth.RenderTargetFormatsMapAsTheHarvestExpects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCamCapRenderTargetFormatsMapAsTheHarvestExpects::RunTest(const FString& Parameters)
{
	// The harvest switches on the pixel format derived from the render target
	// format. Pin that mapping, because the old code inferred it by hand.
	TestEqual(TEXT("RTF_RGBA8"), static_cast<int32>(GetPixelFormatFromRenderTargetFormat(RTF_RGBA8)),
		static_cast<int32>(PF_B8G8R8A8));
	TestEqual(TEXT("RTF_RGBA16f"), static_cast<int32>(GetPixelFormatFromRenderTargetFormat(RTF_RGBA16f)),
		static_cast<int32>(PF_FloatRGBA));
	TestEqual(TEXT("RTF_RGBA32f"), static_cast<int32>(GetPixelFormatFromRenderTargetFormat(RTF_RGBA32f)),
		static_cast<int32>(PF_A32B32G32R32F));

	// A single-channel float target is a plausible thing to point a depth camera
	// at, and it is NOT readable as any of the four-channel types — the harvest
	// has to reject it rather than pick the closest.
	const EPixelFormat R32 = GetPixelFormatFromRenderTargetFormat(RTF_R32f);
	TestTrue(TEXT("R32f is not one of the four-channel formats the harvest reads"),
		R32 != PF_B8G8R8A8 && R32 != PF_R8G8B8A8 && R32 != PF_FloatRGBA && R32 != PF_A32B32G32R32F);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
