#include "PointCloudSequenceComponent.h"

#include "Algo/Sort.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Loading/PCSPlyLoader.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Paths.h"
#include "PCSSceneProxy.h"
#include "RenderingThread.h"
#include "Tasks/Task.h"

DEFINE_LOG_CATEGORY_STATIC(LogPCSComponent, Log, All);

namespace
{
struct FPCSDiscoveredFrame
{
	int64 SequenceNumber = 0;
	FString FilePath;
};
} // namespace

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

void UPointCloudSequenceComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();

	FPCSSceneProxy *PCSProxy = static_cast<FPCSSceneProxy *>(SceneProxy);
	if (PCSProxy == nullptr)
	{
		return;
	}

	const int32 FrameIndex = LoadedFrameIndex;
	TSharedPtr<const FPCSFrameData, ESPMode::ThreadSafe> FrameData = CurrentFrameData;

	ENQUEUE_RENDER_COMMAND(PCSSetFrameData)([PCSProxy, FrameIndex, FrameData = MoveTemp(FrameData)](FRHICommandListImmediate &) mutable
											{ PCSProxy->SetFrameData_RenderThread(FrameIndex, MoveTemp(FrameData)); });
}

void UPointCloudSequenceComponent::BeginPlay()
{
	Super::BeginPlay();
	check(IsInGameThread());
	RefreshSequence();

	if (bAutoPlay)
	{
		Play();
	}
}

void UPointCloudSequenceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	check(IsInGameThread());
	bPlaying = false;
	InvalidatePendingLoads();
	CurrentFrameData.Reset();
	LoadedFrameIndex = INDEX_NONE;

	Super::EndPlay(EndPlayReason);
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

void UPointCloudSequenceComponent::SetSequenceDirectory(const FString &Directory) { SetSequenceSource(Directory, FrameFileNameRegex); }

void UPointCloudSequenceComponent::SetSequenceSource(const FString &Directory, const FString &FileNameRegex)
{
	check(IsInGameThread());

	bPlaying = false;
	PlaybackTimeSeconds = 0.0;
	CurrentFrameIndex = 0;
	SequenceDirectory.Path = Directory;
	FrameFileNameRegex = FileNameRegex;
	RefreshSequence();
}

bool UPointCloudSequenceComponent::RefreshSequence()
{
	check(IsInGameThread());

	InvalidatePendingLoads();
	SequenceFilePaths.Reset();
	FrameCount = 0;
	CurrentFrameIndex = 0;
	PlaybackTimeSeconds = 0.0;
	LoadedFrameIndex = INDEX_NONE;
	CurrentFrameData.Reset();
	MarkRenderDynamicDataDirty();

	if (SequenceDirectory.Path.IsEmpty() || FrameFileNameRegex.IsEmpty())
	{
		return false;
	}

	const FString Directory = FPaths::ConvertRelativePathToFull(SequenceDirectory.Path);
	if (!IFileManager::Get().DirectoryExists(*Directory))
	{
		UE_LOG(LogPCSComponent, Warning, TEXT("Point-cloud sequence directory does not exist: %s"), *Directory);
		return false;
	}

	TArray<FString> FileNames;
	IFileManager::Get().FindFiles(FileNames, *FPaths::Combine(Directory, TEXT("*")), true, false);

	const FRegexPattern Pattern(FrameFileNameRegex);
	TArray<FPCSDiscoveredFrame> DiscoveredFrames;
	DiscoveredFrames.Reserve(FileNames.Num());

	for (const FString &FileName : FileNames)
	{
		FRegexMatcher Matcher(Pattern, FileName);
		if (!Matcher.FindNext() || Matcher.GetMatchBeginning() != 0 || Matcher.GetMatchEnding() != FileName.Len())
		{
			continue;
		}

		int64 SequenceNumber = 0;
		const FString SequenceNumberText = Matcher.GetCaptureGroup(1);
		if (SequenceNumberText.IsEmpty() || !LexTryParseString(SequenceNumber, *SequenceNumberText))
		{
			UE_LOG(LogPCSComponent, Warning, TEXT("Regex capture group 1 is not an integer for file '%s': '%s'"), *FileName, *SequenceNumberText);
			continue;
		}

		FPCSDiscoveredFrame &Frame = DiscoveredFrames.AddDefaulted_GetRef();
		Frame.SequenceNumber = SequenceNumber;
		Frame.FilePath = FPaths::Combine(Directory, FileName);
	}

	Algo::Sort(DiscoveredFrames,
			   [](const FPCSDiscoveredFrame &Left, const FPCSDiscoveredFrame &Right)
			   {
				   if (Left.SequenceNumber != Right.SequenceNumber)
				   {
					   return Left.SequenceNumber < Right.SequenceNumber;
				   }
				   return Left.FilePath < Right.FilePath;
			   });

	TOptional<int64> PreviousSequenceNumber;
	for (FPCSDiscoveredFrame &Frame : DiscoveredFrames)
	{
		if (PreviousSequenceNumber.IsSet() && PreviousSequenceNumber.GetValue() == Frame.SequenceNumber)
		{
			UE_LOG(LogPCSComponent, Warning, TEXT("Ignoring duplicate point-cloud sequence number %lld: %s"), Frame.SequenceNumber, *Frame.FilePath);
			continue;
		}

		PreviousSequenceNumber = Frame.SequenceNumber;
		SequenceFilePaths.Add(MoveTemp(Frame.FilePath));
	}

	FrameCount = SequenceFilePaths.Num();
	if (FrameCount == 0)
	{
		UE_LOG(LogPCSComponent, Warning, TEXT("No files in '%s' matched regex '%s'."), *Directory, *FrameFileNameRegex);
		return false;
	}

	UE_LOG(LogPCSComponent, Log, TEXT("Discovered %d point-cloud frames in %s"), FrameCount, *Directory);
	RequestFrameLoad(0);
	return true;
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
		RequestFrameLoad(ClampedFrameIndex);
		return;
	}

	const int32 PreviousFrameIndex = CurrentFrameIndex;
	CurrentFrameIndex = ClampedFrameIndex;

	RequestFrameLoad(CurrentFrameIndex);
	OnFrameChanged.Broadcast(PreviousFrameIndex, CurrentFrameIndex);
}

void UPointCloudSequenceComponent::RequestFrameLoad(int32 FrameIndex)
{
	check(IsInGameThread());

	if (!SequenceFilePaths.IsValidIndex(FrameIndex))
	{
		return;
	}

	PendingLoadFrameIndex = FrameIndex;
	if (LoadedFrameIndex == FrameIndex)
	{
		return;
	}

	if (ActiveLoadRequestId == 0)
	{
		LaunchPendingFrameLoad();
	}
}

void UPointCloudSequenceComponent::LaunchPendingFrameLoad()
{
	check(IsInGameThread());

	if (ActiveLoadRequestId != 0 || !SequenceFilePaths.IsValidIndex(PendingLoadFrameIndex))
	{
		return;
	}

	const int32 FrameIndex = PendingLoadFrameIndex;
	const FString FilePath = SequenceFilePaths[FrameIndex];
	const uint64 RequestGeneration = SequenceGeneration;
	const uint64 RequestId = ++NextLoadRequestId;

	ActiveLoadRequestId = RequestId;
	LoadingFrameIndex = FrameIndex;

	TWeakObjectPtr<UPointCloudSequenceComponent> WeakThis(this);
	UE::Tasks::TTask<FPCSPlyLoadResult> LoadTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [FilePath]() { return FPCSPlyLoader::LoadFromFile(FilePath); });

	UE::Tasks::Launch(
		UE_SOURCE_LOCATION,
		[WeakThis, LoadTask, RequestId, RequestGeneration, FrameIndex]() mutable
		{
			FPCSPlyLoadResult Result = MoveTemp(LoadTask.GetResult());
			if (WeakThis.IsValid())
			{
				WeakThis->HandleFrameLoadCompleted(RequestId, RequestGeneration, FrameIndex, MoveTemp(Result));
			}
		},
		UE::Tasks::Prerequisites(LoadTask), UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}

void UPointCloudSequenceComponent::HandleFrameLoadCompleted(uint64 RequestId, uint64 RequestGeneration, int32 FrameIndex, FPCSPlyLoadResult &&Result)
{
	check(IsInGameThread());

	if (RequestId != ActiveLoadRequestId)
	{
		return;
	}

	ActiveLoadRequestId = 0;
	LoadingFrameIndex = INDEX_NONE;

	if (RequestGeneration == SequenceGeneration && PendingLoadFrameIndex == FrameIndex)
	{
		if (Result.IsSuccess())
		{
			CurrentFrameData = MoveTemp(Result.FrameData);
			LoadedFrameIndex = FrameIndex;
			MarkRenderDynamicDataDirty(); // Notify the render thread about the new frame
		}
		else
		{
			UE_LOG(LogPCSComponent, Error, TEXT("Failed to load point-cloud frame %d: %s"), FrameIndex, *Result.ErrorMessage);
			PendingLoadFrameIndex = INDEX_NONE;
		}
	}

	if (PendingLoadFrameIndex != INDEX_NONE && PendingLoadFrameIndex != LoadedFrameIndex)
	{
		LaunchPendingFrameLoad();
	}
}

void UPointCloudSequenceComponent::InvalidatePendingLoads()
{
	check(IsInGameThread());
	++SequenceGeneration;
	PendingLoadFrameIndex = INDEX_NONE;
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
