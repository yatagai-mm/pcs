#if WITH_DEV_AUTOMATION_TESTS

#include "Loading/PCSPlyLoader.h"
#include "PointCloudSequenceComponent.h"

#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

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
	const FString FixturePath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tests/Fixtures/frame_000038_first_64.ply"));
	const FPCSPlyLoadResult Result = FPCSPlyLoader::LoadFromFile(FixturePath);
	if (!TestTrue(TEXT("Fixture loads successfully"), Result.IsSuccess()))
	{
		AddError(Result.ErrorMessage);
		return false;
	}

	TestEqual(TEXT("Vertex count"), Result.FrameData->Points.Num(), 64);
	if (Result.FrameData->Points.Num() != 64)
	{
		return false;
	}

	const FPCSPoint &FirstPoint = Result.FrameData->Points[0];
	TestTrue(TEXT("First position"), FirstPoint.Position.Equals(FVector3f(-95.4737091f, -6.77048254f, 0.00000845129125f), 0.000001f));
	TestEqual(TEXT("First red channel"), FirstPoint.Color.R, static_cast<uint8>(145));
	TestEqual(TEXT("First green channel"), FirstPoint.Color.G, static_cast<uint8>(125));
	TestEqual(TEXT("First blue channel"), FirstPoint.Color.B, static_cast<uint8>(106));
	TestEqual(TEXT("Default alpha channel"), FirstPoint.Color.A, static_cast<uint8>(255));

	const FPCSPoint &LastPoint = Result.FrameData->Points.Last();
	TestTrue(TEXT("Last position"), LastPoint.Position.Equals(FVector3f(-95.1511917f, -5.90523434f, -0.00000672730448f), 0.000001f));
	TestTrue(TEXT("Bounds minimum"), Result.FrameData->Bounds.Min.Equals(FVector3f(-95.4944534f, -7.42234516f, -0.0000512590668f), 0.000001f));
	TestTrue(TEXT("Bounds maximum"), Result.FrameData->Bounds.Max.Equals(FVector3f(-95.1511917f, -5.90523434f, 0.0000603428052f), 0.000001f));

	return true;
}

class FPCSWaitForFolderFrameLoadCommand final : public IAutomationLatentCommand
{
public:
	FPCSWaitForFolderFrameLoadCommand(TStrongObjectPtr<UPointCloudSequenceComponent> &&InComponent, FAutomationTestBase *InTest)
		: Component(MoveTemp(InComponent)), Test(InTest), StartTime(FPlatformTime::Seconds())
	{
	}

	virtual bool Update() override
	{
		if (Component->GetLoadedFrame() == 0)
		{
			Test->TestEqual(TEXT("Asynchronously loaded frame"), Component->GetLoadedFrame(), 0);
			Component.Reset();
			return true;
		}

		if (FPlatformTime::Seconds() - StartTime >= 5.0)
		{
			Test->AddError(TEXT("Timed out waiting for the component's asynchronous frame load."));
			Component.Reset();
			return true;
		}

		return false;
	}

private:
	TStrongObjectPtr<UPointCloudSequenceComponent> Component;
	FAutomationTestBase *Test = nullptr;
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

	const FString FixtureDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tests/Fixtures"));
	const FString FileNameRegex = TEXT("^frame_(\\d+)_first_64\\.ply$");
	TStrongObjectPtr<UPointCloudSequenceComponent> Component(NewObject<UPointCloudSequenceComponent>(GetTransientPackage()));

	Component->SetSequenceSource(FixtureDirectory, FileNameRegex);
	TestEqual(TEXT("Matching frame count"), Component->GetFrameCount(), 1);
	TestEqual(TEXT("Configured directory"), Component->GetSequenceDirectory(), FixtureDirectory);
	TestEqual(TEXT("Configured regex"), Component->GetFrameFileNameRegex(), FileNameRegex);

	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FPCSWaitForFolderFrameLoadCommand>(MoveTemp(Component), this));
	return true;
}

#endif
