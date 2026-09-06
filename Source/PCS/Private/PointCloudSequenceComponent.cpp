#include "PointCloudSequenceComponent.h"

#include "Algo/Sort.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Loading/PCSPlyLoader.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
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
	BufferedFrames.Reserve(FrameBufferSize);
	PointMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/PCS/Materials/M_PCSDefault.M_PCSDefault"));

	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);
}

FPrimitiveSceneProxy *UPointCloudSequenceComponent::CreateSceneProxy()
{
	check(IsInGameThread());
	return new FPCSSceneProxy(this);
}

void UPointCloudSequenceComponent::GetUsedMaterials(TArray<UMaterialInterface *> &OutMaterials, bool bGetDebugMaterials) const
{
	Super::GetUsedMaterials(OutMaterials, bGetDebugMaterials);

	// FPrimitiveSceneProxy validates every submitted mesh material against this
	// list before it lets the render thread consume the mesh batch.
	if (PointMaterial != nullptr)
	{
		OutMaterials.AddUnique(PointMaterial);
	}
	// The proxy falls back to this material when a user-supplied material was not
	// compiled for point clouds, so it must also be declared to the renderer.
	OutMaterials.AddUnique(UMaterial::GetDefaultMaterial(MD_Surface));
}

FBoxSphereBounds UPointCloudSequenceComponent::CalcBounds(const FTransform &LocalToWorld) const
{
	if (!CurrentFrameData.IsValid() || !CurrentFrameData->Bounds.IsValid)
	{
		return Super::CalcBounds(LocalToWorld);
	}

	// The SceneProxy is culled before its vertex shader runs. Supplying the
	// decoded point-center bounds here prevents a zero-sized component at the
	// actor origin from hiding a cloud located elsewhere in local space.
	const FBox LocalBounds(FVector(CurrentFrameData->Bounds.Min), FVector(CurrentFrameData->Bounds.Max));
	return FBoxSphereBounds(LocalBounds).TransformBy(LocalToWorld);
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
	TSharedPtr<const FPCSFrameData> FrameData = CurrentFrameData;
	const float PointSizePixels = PointSize;

	ENQUEUE_RENDER_COMMAND(PCSSetFrameData)(
		[PCSProxy, FrameIndex, FrameData = MoveTemp(FrameData), PointSizePixels](FRHICommandListImmediate &RHICmdList) mutable
		{ PCSProxy->SetFrameData_RenderThread(RHICmdList, FrameIndex, MoveTemp(FrameData), PointSizePixels); });
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
	BufferedFrames.Reset();
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
	BufferedFrames.Reset();
	UpdateBounds();
	MarkRenderTransformDirty();
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

int32 UPointCloudSequenceComponent::GetBufferedFrame() const
{
	check(IsInGameThread());
	return BufferedFrames.IsEmpty() ? INDEX_NONE : BufferedFrames[0].FrameIndex;
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
	SetCurrentFrameInternal(DesiredFrame);

	if (bReachedEnd)
	{
		OnPlaybackFinished.Broadcast();
	}
}

void UPointCloudSequenceComponent::SetCurrentFrameInternal(int32 NewFrameIndex)
{
	check(IsInGameThread());

	const int32 ClampedFrameIndex = ClampFrameIndex(NewFrameIndex);
	const bool bFrameChanged = CurrentFrameIndex != ClampedFrameIndex;
	const int32 PreviousFrameIndex = CurrentFrameIndex;
	CurrentFrameIndex = ClampedFrameIndex;

	// A seek can make parsed look-ahead frames irrelevant before their
	// presentation times arrive. Keep the selected frame long enough to
	// activate it below, plus frames in the new look-ahead window.
	for (int32 BufferIndex = BufferedFrames.Num() - 1; BufferIndex >= 0; --BufferIndex)
	{
		const int32 BufferedFrameIndex = BufferedFrames[BufferIndex].FrameIndex;
		if (BufferedFrameIndex != CurrentFrameIndex && !IsFrameInBufferWindow(BufferedFrameIndex))
		{
			BufferedFrames.RemoveAt(BufferIndex);
		}
	}

	if (!TryActivateBufferedFrame(CurrentFrameIndex) && LoadedFrameIndex != CurrentFrameIndex)
	{
		// If the current frame is not already loaded nor buffered (fps seek or parsing delay), request it immediately.
		// This does not guarantee that the frame will be loaded before the next tick, but it will be prioritized over any prefetching.
		RequestFrameLoad(CurrentFrameIndex);
	}
	else
	{
		RequestNextFrameLoad();
	}

	if (bFrameChanged)
	{
		OnFrameChanged.Broadcast(PreviousFrameIndex, CurrentFrameIndex);
	}
}

void UPointCloudSequenceComponent::RequestFrameLoad(int32 FrameIndex)
{
	check(IsInGameThread());

	if (!SequenceFilePaths.IsValidIndex(FrameIndex))
	{
		return;
	}

	if (LoadedFrameIndex == FrameIndex || IsFrameBuffered(FrameIndex))
	{
		// If the requested frame is already loaded or buffered, cancel any pending load for that frame.
		if (PendingLoadFrameIndex == FrameIndex)
		{
			PendingLoadFrameIndex = INDEX_NONE;
		}
		return;
	}

	if (LoadingFrameIndex == FrameIndex && ActiveLoadGeneration == SequenceGeneration)
	{
		// If already loading the requested frame and the sequence has not changed, do nothing.
		PendingLoadFrameIndex = INDEX_NONE;
		return;
	}

	PendingLoadFrameIndex = FrameIndex;
	if (ActiveLoadRequestId == 0)
	{
		LaunchPendingFrameLoad();
	}
}

void UPointCloudSequenceComponent::RequestNextFrameLoad()
{
	check(IsInGameThread());

	if (ActiveLoadRequestId != 0 || PendingLoadFrameIndex != INDEX_NONE)
	{
		return;
	}

	// The playback target always has priority over prefetching.
	if (LoadedFrameIndex != CurrentFrameIndex)
	{
		RequestFrameLoad(CurrentFrameIndex);
		return;
	}

	if (BufferedFrames.Num() >= FrameBufferSize)
	{
		return;
	}

	for (int32 Offset = 1; Offset <= FrameBufferSize; ++Offset)
	{
		const int32 FrameIndex = GetFollowingFrameIndex(CurrentFrameIndex, Offset);
		if (FrameIndex == INDEX_NONE)
		{
			return;
		}

		if (!IsFrameBuffered(FrameIndex))
		{
			RequestFrameLoad(FrameIndex);
			return;
		}
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

	PendingLoadFrameIndex = INDEX_NONE;
	ActiveLoadRequestId = RequestId;
	ActiveLoadGeneration = RequestGeneration;
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
	ActiveLoadGeneration = 0;
	LoadingFrameIndex = INDEX_NONE;

	bool bLoadSucceeded = false;
	if (RequestGeneration == SequenceGeneration)
	{
		if (Result.IsSuccess())
		{
			bLoadSucceeded = true;
			if (FrameIndex == CurrentFrameIndex)
			{
				// Immediately activate the frame if it's the current playback target.
				ActivateFrame(FrameIndex, MoveTemp(Result.FrameData));
			}
			else if (IsFrameInBufferWindow(FrameIndex) && BufferedFrames.Num() < FrameBufferSize && !IsFrameBuffered(FrameIndex))
			{
				// Buffer pre-parsed frames only if BufferedFrames has room and the frame is still within the look-ahead window.
				FPCSBufferedFrame &BufferedFrame = BufferedFrames.AddDefaulted_GetRef();
				BufferedFrame.FrameIndex = FrameIndex;
				BufferedFrame.FrameData = MoveTemp(Result.FrameData); // Only store reference to the frame data
			}
		}
		else
		{
			UE_LOG(LogPCSComponent, Error, TEXT("Failed to load point-cloud frame %d: %s"), FrameIndex, *Result.ErrorMessage);
		}
	}

	if (PendingLoadFrameIndex != INDEX_NONE)
	{
		// Start the next load
		LaunchPendingFrameLoad();
	}
	else if (bLoadSucceeded)
	{
		RequestNextFrameLoad();
	}
}

void UPointCloudSequenceComponent::ActivateFrame(int32 FrameIndex, TSharedPtr<const FPCSFrameData> FrameData)
{
	check(IsInGameThread());
	check(FrameData.IsValid());

	CurrentFrameData = MoveTemp(FrameData);
	LoadedFrameIndex = FrameIndex;
	UE_LOG(LogPCSComponent, Verbose, TEXT("Activated point-cloud frame %d with %d points."), FrameIndex, CurrentFrameData->Vertices.Num());
	UpdateBounds();
	MarkRenderTransformDirty();
	// Notify the render thread of the new frame
	MarkRenderDynamicDataDirty();
}

bool UPointCloudSequenceComponent::TryActivateBufferedFrame(int32 FrameIndex)
{
	check(IsInGameThread());

	const int32 BufferIndex =
		BufferedFrames.IndexOfByPredicate([FrameIndex](const FPCSBufferedFrame &BufferedFrame) { return BufferedFrame.FrameIndex == FrameIndex; });
	if (BufferIndex == INDEX_NONE || !BufferedFrames[BufferIndex].FrameData.IsValid())
	{
		return false;
	}

	TSharedPtr<const FPCSFrameData> FrameData = MoveTemp(BufferedFrames[BufferIndex].FrameData);
	BufferedFrames.RemoveAt(BufferIndex);
	ActivateFrame(FrameIndex, MoveTemp(FrameData));
	return true;
}

bool UPointCloudSequenceComponent::IsFrameBuffered(int32 FrameIndex) const
{
	return BufferedFrames.ContainsByPredicate([FrameIndex](const FPCSBufferedFrame &BufferedFrame) { return BufferedFrame.FrameIndex == FrameIndex; });
}

bool UPointCloudSequenceComponent::IsFrameInBufferWindow(int32 FrameIndex) const
{
	for (int32 Offset = 1; Offset <= FrameBufferSize; ++Offset)
	{
		const int32 Candidate = GetFollowingFrameIndex(CurrentFrameIndex, Offset);
		if (Candidate == INDEX_NONE)
		{
			return false;
		}

		if (Candidate == FrameIndex)
		{
			return true;
		}
	}

	return false;
}

void UPointCloudSequenceComponent::InvalidatePendingLoads()
{
	check(IsInGameThread());
	++SequenceGeneration;
	PendingLoadFrameIndex = INDEX_NONE;
}

int32 UPointCloudSequenceComponent::GetFollowingFrameIndex(int32 FrameIndex, int32 Offset) const
{
	if (!SequenceFilePaths.IsValidIndex(FrameIndex) || FrameCount <= 1 || Offset <= 0)
	{
		return INDEX_NONE;
	}

	const int32 Candidate = FrameIndex + Offset;
	if (Candidate < FrameCount)
	{
		return Candidate;
	}

	if (!bLoop)
	{
		return INDEX_NONE;
	}

	const int32 WrappedFrameIndex = Candidate % FrameCount;
	return WrappedFrameIndex == FrameIndex ? INDEX_NONE : WrappedFrameIndex;
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
