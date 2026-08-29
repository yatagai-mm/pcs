#include "PointCloudSequenceComponent.h"

#include "Math/UnrealMathUtility.h"
#include "PCSSceneProxy.h"

UPointCloudSequenceComponent::UPointCloudSequenceComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
}

FPrimitiveSceneProxy *UPointCloudSequenceComponent::CreateSceneProxy()
{
	check(IsInGameThread());
	return new FPCSSceneProxy(this);
}

void UPointCloudSequenceComponent::BeginPlay()
{
	Super::BeginPlay();
	check(IsInGameThread());

	if (bAutoPlay)
	{
		Play();
	}
}

void UPointCloudSequenceComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	check(IsInGameThread());

	if (!bPlaying || FrameCount <= 0 || PlaybackRate <= 0.0f)
	{
		return;
	}

	AdvancePlayback(DeltaTime);
}

void UPointCloudSequenceComponent::Play()
{
	check(IsInGameThread());
	bPlaying = true;
}

void UPointCloudSequenceComponent::Pause()
{
	check(IsInGameThread());
	bPlaying = false;
}

void UPointCloudSequenceComponent::Stop()
{
	check(IsInGameThread());

	bPlaying = false;
	PlaybackTimeSeconds = 0.0;
	SetCurrentFrameInternal(0);
}

void UPointCloudSequenceComponent::SeekFrame(int32 FrameIndex)
{
	check(IsInGameThread());

	const int32 ClampedFrameIndex = ClampFrameIndex(FrameIndex);
	// Increase precision by making dividend double
	PlaybackTimeSeconds = static_cast<double>(ClampedFrameIndex) / GetSafeFrameRate();
	SetCurrentFrameInternal(ClampedFrameIndex);
}

void UPointCloudSequenceComponent::SeekTime(double TimeSeconds)
{
	check(IsInGameThread());

	double ClampedTime = FMath::Max(0.0, TimeSeconds);
	if (FrameCount > 0)
	{
		// Increase precision by making dividend double
		const double LastFrameTime = static_cast<double>(FrameCount - 1) / GetSafeFrameRate();
		ClampedTime = FMath::Min(ClampedTime, LastFrameTime);
	}

	PlaybackTimeSeconds = ClampedTime;
	const int32 DesiredFrame = FMath::FloorToInt(PlaybackTimeSeconds * GetSafeFrameRate());
	SetCurrentFrameInternal(ClampFrameIndex(DesiredFrame));
}

void UPointCloudSequenceComponent::SetSequenceDirectory(const FString &Directory)
{
	check(IsInGameThread());

	if (SequenceDirectory.Path == Directory)
	{
		return;
	}

	Stop();
	FrameCount = 0;
	SequenceDirectory.Path = Directory;
}

FString UPointCloudSequenceComponent::GetSequenceDirectory() const
{
	check(IsInGameThread());
	return SequenceDirectory.Path;
}

void UPointCloudSequenceComponent::SetFrameCount(int32 InFrameCount)
{
	check(IsInGameThread());

	FrameCount = FMath::Max(0, InFrameCount);
	if (FrameCount == 0)
	{
		PlaybackTimeSeconds = 0.0;
		SetCurrentFrameInternal(0);
		return;
	}

	SeekFrame(CurrentFrameIndex);
}

void UPointCloudSequenceComponent::AdvancePlayback(float DeltaSeconds)
{
	check(IsInGameThread());

	const double Duration = GetSequenceDuration();
	if (Duration <= 0.0)
	{
		return;
	}

	double NewTime = PlaybackTimeSeconds + DeltaSeconds * PlaybackRate;
	bool bReachedEnd = false;

	if (bLoop)
	{
		NewTime = FMath::Fmod(NewTime, Duration);
		if (NewTime < 0.0)
		{
			NewTime += Duration;
		}
	}
	else if (NewTime >= Duration)
	{
		NewTime = Duration;
		bPlaying = false;
		bReachedEnd = true;
	}

	PlaybackTimeSeconds = NewTime;
	const int32 DesiredFrame = FMath::FloorToInt(PlaybackTimeSeconds * GetSafeFrameRate());
	SetCurrentFrameInternal(ClampFrameIndex(DesiredFrame));

	if (bReachedEnd)
	{
		OnPlaybackFinished.Broadcast();
	}
}

void UPointCloudSequenceComponent::SetCurrentFrameInternal(int32 NewFrameIndex)
{
	check(IsInGameThread());

	const int32 ClampedFrameIndex = ClampFrameIndex(NewFrameIndex);
	if (CurrentFrameIndex == ClampedFrameIndex)
	{
		return;
	}

	const int32 PreviousFrameIndex = CurrentFrameIndex;
	CurrentFrameIndex = ClampedFrameIndex;

	// A later render-thread implementation will consume the selected frame here.
	MarkRenderDynamicDataDirty();
	OnFrameChanged.Broadcast(PreviousFrameIndex, CurrentFrameIndex);
}

int32 UPointCloudSequenceComponent::ClampFrameIndex(int32 FrameIndex) const
{
	if (FrameCount <= 0)
	{
		return FMath::Max(0, FrameIndex);
	}

	return FMath::Clamp(FrameIndex, 0, FrameCount - 1);
}

float UPointCloudSequenceComponent::GetSafeFrameRate() const { return FMath::Max(FrameRate, 1.0f); }

double UPointCloudSequenceComponent::GetSequenceDuration() const
{
	if (FrameCount <= 0)
	{
		return 0.0;
	}

	return static_cast<double>(FrameCount) / GetSafeFrameRate();
}
