#if WITH_DEV_AUTOMATION_TESTS

#include "PCSStreamInput.h"
#include "PointCloudSequenceComponent.h"

#include "Async/Async.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

namespace
{
struct FPCSStreamTestWorld
{
	TStrongObjectPtr<UWorld> World{UWorld::CreateWorld(EWorldType::Game, false)};
	TStrongObjectPtr<UPointCloudSequenceComponent> Component{NewObject<UPointCloudSequenceComponent>()};

	FPCSStreamTestWorld() { Component->RegisterComponentWithWorld(World.Get()); }
	~FPCSStreamTestWorld()
	{
		Component->DestroyComponent();
		World->DestroyWorld(false);
	}
};

FPCSTimedFrame MakeStreamFrame(int64 Id, double Time, double Duration = 0.25)
{
	TSharedRef<FPCSFrameData> Data = MakeShared<FPCSFrameData>();
	FPCSPointVertex Point;
	Point.Position = FVector3f(static_cast<float>(Id), 0.0f, 0.0f);
	Data->Vertices.Add(Point);
	Data->Bounds += Point.Position;
	return {Id, Time, Duration, Data};
}

void TickStream(UPointCloudSequenceComponent &Component, float Delta = 0.0f) { Component.TickComponent(Delta, LEVELTICK_All, nullptr); }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCSStreamTimelineTest, "PCS.Stream.PTSTimeline", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPCSStreamTimelineTest::RunTest(const FString &Parameters)
{
	FPCSStreamTestWorld Fixture;
	auto &Component = Fixture.Component;
	const auto Input = Component->OpenStream();
	Component->FrameRate = 1.0f; // Deliberately unrelated to incoming PTS.
	Component->bLoop = true;
	Component->Play();
	TickStream(*Component, 10.0f);
	TestEqual(TEXT("No frames: clock waits"), Component->GetPlaybackTime(), 0.0);
	TestTrue(TEXT("Play stays pending"), Component->IsPlaying());
	TestTrue(TEXT("Accept later frame first"), Input->TrySubmit(MakeStreamFrame(3, 100.75)) == EPCSSubmitResult::Accepted);
	Input->TrySubmit(MakeStreamFrame(1, 100.0));
	Input->TrySubmit(MakeStreamFrame(2, 100.25));
	TickStream(*Component, 5.0f);
	TestEqual(TEXT("Anchor to earliest PTS, ignore startup delay"), Component->GetPlaybackTime(), 100.0);
	TestEqual(TEXT("Arrival order does not determine presentation"), Component->GetCurrentStreamFrameId(), int64(1));
	TestEqual(TEXT("Activated data updates render bounds"), Component->CalcBounds(FTransform::Identity).Origin.X, 1.0);
	TestEqual(TEXT("Streams need no total frame count"), Component->GetFrameCount(), 0);
	TestEqual(TEXT("File index is not a stream ID"), Component->GetLoadedFrame(), INDEX_NONE);
	TickStream(*Component, 0.125f);
	TestEqual(TEXT("Future frame not displayed early"), Component->GetCurrentStreamFrameId(), int64(1));
	TickStream(*Component, 0.625f);
	TestEqual(TEXT("Skip obsolete due frames"), Component->GetCurrentStreamFrameId(), int64(3));
	TestEqual(TEXT("Consumed data releases queue budget"), Input->GetBufferedBytes(), uint64(0));
	TestTrue(TEXT("Old arrival cannot rewind display"), Input->TrySubmit(MakeStreamFrame(2, 100.25)) == EPCSSubmitResult::DroppedLate);
	TickStream(*Component, 0.25f);
	TestEqual(TEXT("Underflow holds last frame"), Component->GetCurrentStreamFrameId(), int64(3));
	TestEqual(TEXT("Underflow clock keeps advancing"), Component->GetPlaybackTime(), 101.0);
	TestTrue(TEXT("No implicit end at empty queue"), Component->IsPlaying());
	Input->TrySubmit(MakeStreamFrame(4, 101.25));
	TickStream(*Component, 0.25f);
	TestEqual(TEXT("Recovers after underflow"), Component->GetCurrentStreamFrameId(), int64(4));
	TickStream(*Component, 1.0f);
	const double TimeBeforeEnd = Component->GetPlaybackTime();
	Input->SignalEndOfStream();
	TickStream(*Component);
	TestEqual(TEXT("Late EOS cannot rewind the clock"), Component->GetPlaybackTime(), TimeBeforeEnd);
	TestFalse(TEXT("Late EOS completes"), Component->IsPlaying());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCSStreamControlsTest, "PCS.Stream.ControlsAndEnd", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPCSStreamControlsTest::RunTest(const FString &Parameters)
{
	FPCSStreamTestWorld Fixture;
	auto &Component = Fixture.Component;
	const auto Input = Component->OpenStream();
	Input->TrySubmit(MakeStreamFrame(10, 20.0));
	Input->TrySubmit(MakeStreamFrame(11, 20.5, 0.5));
	Component->Play();
	TickStream(*Component);
	Component->Pause();
	TickStream(*Component, 2.0f);
	TestEqual(TEXT("Pause freezes PTS clock"), Component->GetPlaybackTime(), 20.0);
	Component->SeekTime(0.0);
	Component->SeekFrame(100);
	Component->SetFrameCount(100);
	TestEqual(TEXT("File controls do not alter stream time"), Component->GetPlaybackTime(), 20.0);
	TestEqual(TEXT("File controls do not give stream a total"), Component->GetFrameCount(), 0);
	Component->Play();
	Component->PlaybackRate = 0.0f;
	TickStream(*Component, 1.0f);
	TestEqual(TEXT("Zero rate freezes clock"), Component->GetPlaybackTime(), 20.0);
	Component->PlaybackRate = 2.0f;
	TickStream(*Component, 0.25f);
	TestEqual(TEXT("Playback rate scales PTS advancement"), Component->GetCurrentStreamFrameId(), int64(11));
	Input->SignalEndOfStream();
	TestTrue(TEXT("EOS rejects further data"), Input->TrySubmit(MakeStreamFrame(12, 21.0)) == EPCSSubmitResult::Closed);
	TickStream(*Component, 0.125f);
	TestTrue(TEXT("Wait for final duration"), Component->IsPlaying());
	TickStream(*Component, 0.125f);
	TestFalse(TEXT("EOS finishes despite bLoop"), Component->IsPlaying());
	TestEqual(TEXT("EOS clamps to final end PTS"), Component->GetPlaybackTime(), 21.0);
	Component->Play();
	TestFalse(TEXT("Completed stream cannot restart"), Component->IsPlaying());
	const auto Empty = Component->OpenStream();
	Empty->SignalEndOfStream();
	Component->Play();
	TickStream(*Component);
	TestFalse(TEXT("Empty stream can finish"), Component->IsPlaying());
	const auto Replacement = Component->OpenStream();
	Replacement->TrySubmit(MakeStreamFrame(1, 0.0));
	Component->Play();
	TickStream(*Component);
	Component->Stop();
	TestEqual(TEXT("Stop retains last image ID"), Component->GetCurrentStreamFrameId(), int64(1));
	TestTrue(TEXT("Stop permanently closes input"), Replacement->IsClosed());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCSStreamCapacityTest, "PCS.Stream.CapacityAndValidation",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPCSStreamCapacityTest::RunTest(const FString &Parameters)
{
	const FPCSTimedFrame First = MakeStreamFrame(1, 0.0);
	const uint64 Bytes = First.Data->Vertices.GetAllocatedSize();
	FPCSStreamConfig Config;
	Config.MaxBufferedFrames = 2;
	Config.MaxBufferedBytes = Bytes;
	FPCSStreamInput Input(Config);
	TestTrue(TEXT("First fits"), Input.TrySubmit(First) == EPCSSubmitResult::Accepted);
	TestTrue(TEXT("Duplicate PTS rejected"), Input.TrySubmit(MakeStreamFrame(9, 0.0)) == EPCSSubmitResult::Duplicate);
	TestTrue(TEXT("Duplicate queued ID rejected"), Input.TrySubmit(MakeStreamFrame(1, 0.5)) == EPCSSubmitResult::Duplicate);
	TestTrue(TEXT("Byte budget enforced"), Input.TrySubmit(MakeStreamFrame(2, 0.5)) == EPCSSubmitResult::Backpressure);
	TestEqual(TEXT("Rejected frame not retained"), Input.GetBufferedFrameCount(), 1);
	TestEqual(TEXT("Allocated vertex bytes counted"), Input.GetBufferedBytes(), Bytes);
	FPCSTimedFrame Invalid = First;
	Invalid.PresentationTime = std::numeric_limits<double>::quiet_NaN();
	TestTrue(TEXT("Reject NaN PTS"), Input.TrySubmit(Invalid) == EPCSSubmitResult::Invalid);
	Invalid = First;
	Invalid.Duration = 0.0;
	TestTrue(TEXT("Reject zero duration"), Input.TrySubmit(Invalid) == EPCSSubmitResult::Invalid);
	Invalid = First;
	Invalid.Data.Reset();
	TestTrue(TEXT("Reject null data"), Input.TrySubmit(Invalid) == EPCSSubmitResult::Invalid);
	Config.MaxBufferedFrames = 1;
	Config.MaxBufferedBytes = 1024;
	FPCSStreamInput CountLimited(Config);
	CountLimited.TrySubmit(First);
	TestTrue(TEXT("Frame count budget enforced"), CountLimited.TrySubmit(MakeStreamFrame(2, 0.5)) == EPCSSubmitResult::Backpressure);
	Input.Close();
	TestEqual(TEXT("Close releases bytes"), Input.GetBufferedBytes(), uint64(0));
	TestTrue(TEXT("Closed handle rejects input"), Input.TrySubmit(First) == EPCSSubmitResult::Closed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPCSStreamSourceLifecycleTest, "PCS.Stream.SourceLifecycle",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPCSStreamSourceLifecycleTest::RunTest(const FString &Parameters)
{
	FPCSStreamTestWorld Fixture;
	auto &Component = Fixture.Component;
	const auto Old = Component->OpenStream();
	Old->TrySubmit(MakeStreamFrame(1, 100.0));
	const auto Current = Component->OpenStream();
	TestTrue(TEXT("Switch closes old producers"), Old->TrySubmit(MakeStreamFrame(2, 101.0)) == EPCSSubmitResult::Closed);
	TestEqual(TEXT("Switch frees old queue"), Old->GetBufferedBytes(), uint64(0));
	const EPCSSubmitResult Submitted = Async(EAsyncExecution::ThreadPool, [Current]() { return Current->TrySubmit(MakeStreamFrame(42, 0.0)); }).Get();
	TestTrue(TEXT("Worker can submit decoded data"), Submitted == EPCSSubmitResult::Accepted);
	Component->Play();
	TickStream(*Component);
	TestEqual(TEXT("Only replacement frame activated"), Component->GetCurrentStreamFrameId(), int64(42));
	Component->SetSequenceDirectory(TEXT(""));
	TestFalse(TEXT("File source exits stream mode"), Component->IsStreaming());
	TestTrue(TEXT("File source closes previous endpoint"), Current->IsClosed());
	TestEqual(TEXT("File source clears stream ID"), Component->GetCurrentStreamFrameId(), int64(INDEX_NONE));
	const auto Destroyed = Component->OpenStream();
	Component->DestroyComponent();
	TestTrue(TEXT("DestroyComponent closes producer immediately"), Destroyed->IsClosed());
	return true;
}

#endif
