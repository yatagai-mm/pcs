#if WITH_DEV_AUTOMATION_TESTS

#include "Loading/PCSPlyLoader.h"
#include "PointCloudSequenceComponent.h"

#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
TArray<uint8> MakeOptimizationFixture(int32 VertexStride, bool bIncludeFrameToWorldTransform = false)
{
	const ANSICHAR *Header =
		VertexStride == 15 ? "ply\nformat binary_little_endian 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nproperty uchar "
							 "red\nproperty uchar green\nproperty uchar blue\nend_header\n"
		: VertexStride == 16
			? "ply\nformat binary_little_endian 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty "
			  "uchar green\nproperty uchar blue\nproperty uchar alpha\nend_header\n"
			: "ply\nformat binary_little_endian 1.0\nelement vertex 3\nproperty float x\nproperty uchar red\nproperty float y\nproperty uchar green\nproperty "
			  "float z\nproperty uchar blue\nproperty uchar padding0\nproperty uchar padding1\nproperty uchar padding2\nend_header\n";
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8 *>(Header), FCStringAnsi::Strlen(Header));
	if (bIncludeFrameToWorldTransform)
	{
		const ANSICHAR *Comments = "comment frame_to_world_scale 2\ncomment frame_to_world_translation 10 20 30\n";
		constexpr int32 CommentsOffset = UE_ARRAY_COUNT("ply\nformat binary_little_endian 1.0\n") - 1;
		Bytes.Insert(reinterpret_cast<const uint8 *>(Comments), FCStringAnsi::Strlen(Comments), CommentsOffset);
	}
	const float Positions[3][3] = {{1.0f, 2.0f, 3.0f}, {-4.0f, 5.0f, -6.0f}, {7.0f, -8.0f, 9.0f}};
	const uint8 Colors[3][4] = {{10, 20, 30, 40}, {50, 60, 70, 80}, {90, 100, 110, 120}};
	for (int32 VertexIndex = 0; VertexIndex < 3; ++VertexIndex)
	{
		if (VertexStride == 15 || VertexStride == 16)
		{
			const int32 Start = Bytes.AddZeroed(VertexStride);
			FMemory::Memcpy(Bytes.GetData() + Start, Positions[VertexIndex], sizeof(Positions[VertexIndex]));
			FMemory::Memcpy(Bytes.GetData() + Start + 12, Colors[VertexIndex], VertexStride == 15 ? 3 : 4);
		}
		else
		{
			const int32 Start = Bytes.AddZeroed(18);
			FMemory::Memcpy(Bytes.GetData() + Start, &Positions[VertexIndex][0], sizeof(float));
			Bytes[Start + 4] = Colors[VertexIndex][0];
			FMemory::Memcpy(Bytes.GetData() + Start + 5, &Positions[VertexIndex][1], sizeof(float));
			Bytes[Start + 9] = Colors[VertexIndex][1];
			FMemory::Memcpy(Bytes.GetData() + Start + 10, &Positions[VertexIndex][2], sizeof(float));
			Bytes[Start + 14] = Colors[VertexIndex][2];
		}
	}
	return Bytes;
}

void RunOptimizationFixtureTest(FAutomationTestBase &Test, int32 VertexStride)
{
	const FString Path = FPaths::Combine(FPaths::ProjectIntermediateDir(),
										 FString::Printf(TEXT("pcs_loader_%d_%s.ply"), VertexStride, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const TArray<uint8> Bytes = MakeOptimizationFixture(VertexStride);
	Test.TestTrue(TEXT("Write optimization fixture"), FFileHelper::SaveArrayToFile(Bytes, *Path));
	const FPCSPlyLoadResult Result = FPCSPlyLoader::LoadFromFile(Path);
	Test.TestTrue(TEXT("Optimization fixture loads"), Result.IsSuccess());
	if (Result.IsSuccess())
	{
		Test.TestEqual(TEXT("Optimization fixture vertex count"), Result.FrameData->Vertices.Num(), 3);
		Test.TestEqual(TEXT("Optimization fixture red channel"), Result.FrameData->Vertices[1].Color.R, static_cast<uint8>(50));
		Test.TestTrue(TEXT("Optimization fixture position"), Result.FrameData->Vertices[2].Position.Equals(FVector3f(7.0f, -8.0f, 9.0f), 0.000001f));
	}
	IFileManager::Get().Delete(*Path);
}

void RunFrameToWorldTransformFixtureTest(FAutomationTestBase &Test, int32 VertexStride)
{
	const FString Path = FPaths::Combine(FPaths::ProjectIntermediateDir(),
										 FString::Printf(TEXT("pcs_frame_to_world_%d_%s.ply"), VertexStride, *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const TArray<uint8> Bytes = MakeOptimizationFixture(VertexStride, true);
	Test.TestTrue(TEXT("Write frame-to-world fixture"), FFileHelper::SaveArrayToFile(Bytes, *Path));
	const FPCSPlyLoadResult Result = FPCSPlyLoader::LoadFromFile(Path);
	Test.TestTrue(TEXT("Frame-to-world fixture loads"), Result.IsSuccess());
	if (Result.IsSuccess())
	{
		Test.TestTrue(TEXT("Frame-to-world transform applies to vertices"),
					  Result.FrameData->Vertices[2].Position.Equals(FVector3f(24.0f, 4.0f, 48.0f), 0.000001f));
		Test.TestTrue(TEXT("Frame-to-world transform applies to bounds minimum"), Result.FrameData->Bounds.Min.Equals(FVector3f(2.0f, 4.0f, 18.0f), 0.000001f));
		Test.TestTrue(TEXT("Frame-to-world transform applies to bounds maximum"),
					  Result.FrameData->Bounds.Max.Equals(FVector3f(24.0f, 30.0f, 48.0f), 0.000001f));
	}
	IFileManager::Get().Delete(*Path);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCSPlyLoaderPackedLayoutOptimizationTest, "PCS.Loading.PlyLoader.PackedLayoutOptimization",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPCSPlyLoaderPackedLayoutOptimizationTest::RunTest(const FString &Parameters)
{
	RunOptimizationFixtureTest(*this, 15);
	RunOptimizationFixtureTest(*this, 16);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCSPlyLoaderGenericLayoutOptimizationTest, "PCS.Loading.PlyLoader.GenericLayoutFallback",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPCSPlyLoaderGenericLayoutOptimizationTest::RunTest(const FString &Parameters)
{
	RunOptimizationFixtureTest(*this, 18);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCSPlyLoaderFrameToWorldTransformTest, "PCS.Loading.PlyLoader.FrameToWorldTransform",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPCSPlyLoaderFrameToWorldTransformTest::RunTest(const FString &Parameters)
{
	// Exercise both optimized packed layouts and the generic reordered-property path.
	RunFrameToWorldTransformFixtureTest(*this, 15);
	RunFrameToWorldTransformFixtureTest(*this, 16);
	RunFrameToWorldTransformFixtureTest(*this, 18);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCSPlyLoaderGenericChunkBoundsTest, "PCS.Loading.PlyLoader.GenericChunkBounds",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPCSPlyLoaderGenericChunkBoundsTest::RunTest(const FString &Parameters)
{
	// Reordered properties force generic decoding. The payload exceeds 4 MiB,
	// with extrema in different chunks to catch accidental bounds replacement.
	constexpr int32 VertexCount = 250001;
	constexpr int32 Stride = 18;
	const ANSICHAR *Header =
		"ply\nformat binary_little_endian 1.0\nelement vertex 250001\nproperty float x\nproperty uchar red\nproperty float y\nproperty uchar green\n"
		"property float z\nproperty uchar blue\nproperty uchar padding0\nproperty uchar padding1\nproperty uchar padding2\nend_header\n";
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8 *>(Header), FCStringAnsi::Strlen(Header));
	const int32 DataOffset = Bytes.Num();
	Bytes.AddZeroed(VertexCount * Stride);
	const float First[3] = {-100.0f, -200.0f, -300.0f};
	const float Last[3] = {400.0f, 500.0f, 600.0f};
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		FMemory::Memcpy(Bytes.GetData() + DataOffset + Axis * 5, &First[Axis], sizeof(float));
		FMemory::Memcpy(Bytes.GetData() + DataOffset + (VertexCount - 1) * Stride + Axis * 5, &Last[Axis], sizeof(float));
	}
	const FString Path =
		FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("pcs_generic_bounds_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".ply"));
	if (!TestTrue(TEXT("Write multichunk generic fixture"), FFileHelper::SaveArrayToFile(Bytes, *Path)))
	{
		return false;
	}
	const FPCSPlyLoadResult Result = FPCSPlyLoader::LoadFromFile(Path);
	IFileManager::Get().Delete(*Path);
	if (!TestTrue(TEXT("Multichunk generic fixture loads"), Result.IsSuccess()))
	{
		return false;
	}
	TestEqual(TEXT("All generic vertices decoded"), Result.FrameData->Vertices.Num(), VertexCount);
	TestTrue(TEXT("Bounds remain valid"), Result.FrameData->Bounds.IsValid != 0);
	TestTrue(TEXT("Minimum retained from first chunk"), Result.FrameData->Bounds.Min == FVector3f(-100.0f, -200.0f, -300.0f));
	TestTrue(TEXT("Maximum retained from last chunk"), Result.FrameData->Bounds.Max == FVector3f(400.0f, 500.0f, 600.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCSPlyLoaderBinaryLittleEndianTest, "PCS.Loading.PlyLoader.BinaryLittleEndian",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPCSPlyLoaderBinaryLittleEndianTest::RunTest(const FString &Parameters)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PCS"));
	if (!TestTrue(TEXT("PCS plugin is discoverable"), Plugin.IsValid()))
	{
		return false;
	}

	// I could remove Project module completely by not using GetBaseDir but nah
	const FString FixturePath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Static/frame_000038_first_64.ply"));
	const FPCSPlyLoadResult Result = FPCSPlyLoader::LoadFromFile(FixturePath);
	if (!TestTrue(TEXT("Fixture loads successfully"), Result.IsSuccess()))
	{
		AddError(Result.ErrorMessage);
		return false;
	}

	TestEqual(TEXT("Vertex count"), Result.FrameData->Vertices.Num(), 64);
	if (Result.FrameData->Vertices.Num() != 64)
	{
		return false;
	}

	const FPCSPointVertex &FirstPoint = Result.FrameData->Vertices[0];
	TestTrue(TEXT("First position"), FirstPoint.Position.Equals(FVector3f(-95.4737091f, -6.77048254f, 0.00000845129125f), 0.000001f));
	TestEqual(TEXT("First red channel"), FirstPoint.Color.R, static_cast<uint8>(145));
	TestEqual(TEXT("First green channel"), FirstPoint.Color.G, static_cast<uint8>(125));
	TestEqual(TEXT("First blue channel"), FirstPoint.Color.B, static_cast<uint8>(106));
	TestEqual(TEXT("Default alpha channel"), FirstPoint.Color.A, static_cast<uint8>(255));

	const FPCSPointVertex &LastPoint = Result.FrameData->Vertices.Last();
	TestTrue(TEXT("Last position"), LastPoint.Position.Equals(FVector3f(-95.1511917f, -5.90523434f, -0.00000672730448f), 0.000001f));
	TestTrue(TEXT("Bounds minimum"), Result.FrameData->Bounds.Min.Equals(FVector3f(-95.4944534f, -7.42234516f, -0.0000512590668f), 0.000001f));
	TestTrue(TEXT("Bounds maximum"), Result.FrameData->Bounds.Max.Equals(FVector3f(-95.1511917f, -5.90523434f, 0.0000603428052f), 0.000001f));

	return true;
}

class FPCSWaitForFolderFrameLoadCommand final : public IAutomationLatentCommand
{
public:
	FPCSWaitForFolderFrameLoadCommand(TStrongObjectPtr<UPointCloudSequenceComponent> &&InComponent, TStrongObjectPtr<UWorld> &&InWorld,
									  FAutomationTestBase *InTest, FString InTemporaryDirectory)
		: Component(MoveTemp(InComponent)), World(MoveTemp(InWorld)), Test(InTest), TemporaryDirectory(MoveTemp(InTemporaryDirectory)),
		  StartTime(FPlatformTime::Seconds())
	{
	}

	virtual bool Update() override
	{
		if (Component->GetLoadedFrame() == 0 && Component->GetBufferedFrame() == 1)
		{
			Test->TestEqual(TEXT("Asynchronously loaded current frame"), Component->GetLoadedFrame(), 0);
			Test->TestEqual(TEXT("Immediately prefetched next frame"), Component->GetBufferedFrame(), 1);
			Component->TickComponent(0.01f, LEVELTICK_All, nullptr);
			Test->TestEqual(TEXT("Playback clock resumes after the initial load"), Component->GetPlaybackTime(), static_cast<double>(0.01f));
			Test->TestTrue(TEXT("Playback remains active after initial load"), Component->IsPlaying());
			Finish();
			return true;
		}

		if (FPlatformTime::Seconds() - StartTime >= 5.0)
		{
			Test->AddError(TEXT("Timed out waiting for the component's current and prefetched frame loads."));
			Finish();
			return true;
		}

		return false;
	}

private:
	void Finish()
	{
		Component->UnregisterComponent();
		Component.Reset();
		World->DestroyWorld(false);
		World.Reset();
		IFileManager::Get().DeleteDirectory(*TemporaryDirectory, false, true);
	}

	TStrongObjectPtr<UPointCloudSequenceComponent> Component;
	TStrongObjectPtr<UWorld> World;
	FAutomationTestBase *Test = nullptr;
	FString TemporaryDirectory;
	double StartTime = 0.0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCSComponentFolderSequenceTest, "PCS.Component.FolderSequence",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPCSComponentFolderSequenceTest::RunTest(const FString &Parameters)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PCS"));
	if (!TestTrue(TEXT("PCS plugin is discoverable"), Plugin.IsValid()))
	{
		return false;
	}

	const FString FixturePath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Static/frame_000038_first_64.ply"));
	const FString TemporaryDirectory =
		FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("PCSAutomation"), FGuid::NewGuid().ToString(EGuidFormats::Digits));

	if (!TestTrue(TEXT("Create temporary sequence directory"), IFileManager::Get().MakeDirectory(*TemporaryDirectory, true)))
	{
		return false;
	}

	const FString FirstFramePath = FPaths::Combine(TemporaryDirectory, TEXT("frame_000000.ply"));
	const FString SecondFramePath = FPaths::Combine(TemporaryDirectory, TEXT("frame_000001.ply"));
	if (!TestEqual(TEXT("Copy first fixture frame"), IFileManager::Get().Copy(*FirstFramePath, *FixturePath), COPY_OK) ||
		!TestEqual(TEXT("Copy second fixture frame"), IFileManager::Get().Copy(*SecondFramePath, *FixturePath), COPY_OK))
	{
		IFileManager::Get().DeleteDirectory(*TemporaryDirectory, false, true);
		return false;
	}

	const FString FileNameRegex = TEXT("^frame_(\\d+)\\.ply$");
	TStrongObjectPtr<UWorld> World(UWorld::CreateWorld(EWorldType::Game, false));
	TStrongObjectPtr<UPointCloudSequenceComponent> Component(NewObject<UPointCloudSequenceComponent>(GetTransientPackage()));
	Component->RegisterComponentWithWorld(World.Get());

	Component->SetSequenceSource(TemporaryDirectory, FileNameRegex);
	TestEqual(TEXT("Matching frame count"), Component->GetFrameCount(), 2);
	TestEqual(TEXT("Configured directory"), Component->GetSequenceDirectory(), TemporaryDirectory);
	TestEqual(TEXT("Configured regex"), Component->GetFrameFileNameRegex(), FileNameRegex);

	Component->FrameRate = 30.0f;
	Component->Play();
	// The completion callback runs on the game thread, so no result can be
	// activated during this synchronous tick, even if the worker has finished.
	Component->TickComponent(0.05f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("Initial load keeps playback clock at zero"), Component->GetPlaybackTime(), 0.0);
	TestEqual(TEXT("Initial load keeps the first frame selected"), Component->GetCurrentFrame(), 0);
	TestTrue(TEXT("Play remains pending during the initial load"), Component->IsPlaying());

	FAutomationTestFramework::Get().EnqueueLatentCommand(
		MakeShared<FPCSWaitForFolderFrameLoadCommand>(MoveTemp(Component), MoveTemp(World), this, TemporaryDirectory));
	return true;
}

#endif
