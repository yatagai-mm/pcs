#pragma once

#include "Components/PrimitiveComponent.h"
#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

#include "PointCloudSequenceComponent.generated.h"

struct FPCSFrameData;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPCSFrameChangedSignature, int32, PreviousFrameIndex, int32, CurrentFrameIndex);

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FPCSPlaybackFinishedSignature);

/**
 * Game-thread-facing playback component for a point-cloud frame sequence.
 *
 * This class owns playback state, discovers the source sequence, and coordinates
 * asynchronous PLY loading. Render-thread state and GPU resources remain in the
 * scene proxy.
 */
UCLASS(BlueprintType, ClassGroup = (PCS), meta = (BlueprintSpawnableComponent, DisplayName = "Point Cloud Sequence"))
class PCS_API UPointCloudSequenceComponent final : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UPointCloudSequenceComponent();

	virtual FPrimitiveSceneProxy *CreateSceneProxy() override;
	virtual void SendRenderDynamicData_Concurrent() override;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;

	// Starts or resumes timeline advancement. This request is retained while the sequence is still loading.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Playback")
	void Play();

	// Pauses timeline advancement without changing the selected frame.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Playback")
	void Pause();

	// Stops playback and returns to frame zero.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Playback")
	void Stop();

	// Selects a frame immediately. Out-of-range values are clamped when the frame count is known.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Playback")
	void SeekFrame(int32 FrameIndex);

	// Selects a timeline position in seconds.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Playback")
	void SeekTime(double TimeSeconds);

	// Changes the source directory, rescans it, and resets the current playback state.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Source")
	void SetSequenceDirectory(const FString &Directory);

	/**
	 * Changes both source inputs and rescans the directory. Capture group 1 of
	 * FileNameRegex must contain the integer sequence number.
	 */
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Source")
	void SetSequenceSource(const FString &Directory, const FString &FileNameRegex);

	/** Rescans the configured directory and requests the first matching frame. */
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Source")
	bool RefreshSequence();

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Source")
	FString GetSequenceDirectory() const;

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Source")
	FString GetFrameFileNameRegex() const { return FrameFileNameRegex; }

	/**
	 * Publishes the number of frames discovered by a loader.
	 * This must be called on the game thread after asynchronous discovery completes.
	 */
	void SetFrameCount(int32 InFrameCount);

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Playback")
	bool IsPlaying() const { return bPlaying; }

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Playback")
	int32 GetCurrentFrame() const { return CurrentFrameIndex; }

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Playback")
	int32 GetFrameCount() const { return FrameCount; }

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Playback")
	int32 GetLoadedFrame() const { return LoadedFrameIndex; }

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Playback")
	double GetPlaybackTime() const { return PlaybackTimeSeconds; }

	// Emitted on the game thread whenever the selected frame changes.
	UPROPERTY(BlueprintAssignable, Category = "Point Cloud Sequence|Events")
	FPCSFrameChangedSignature OnFrameChanged;

	// Emitted on the game thread when non-looping playback reaches the end.
	UPROPERTY(BlueprintAssignable, Category = "Point Cloud Sequence|Events")
	FPCSPlaybackFinishedSignature OnPlaybackFinished;

	// Directory containing the PLY frame sequence.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Point Cloud Sequence|Source")
	FDirectoryPath SequenceDirectory;

	/**
	 * Full-file-name regular expression. Capture group 1 is parsed as the sequence
	 * number used to order matching files.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Point Cloud Sequence|Source")
	FString FrameFileNameRegex = TEXT("^frame_(\\d+)\\.ply$");

	// Sequence sampling rate.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Point Cloud Sequence|Playback", meta = (ClampMin = "1.0", UIMin = "1.0"))
	float FrameRate = 30.0f;

	// Timeline speed multiplier. Zero freezes timeline advancement.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Point Cloud Sequence|Playback", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float PlaybackRate = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Point Cloud Sequence|Playback")
	bool bLoop = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Point Cloud Sequence|Playback")
	bool bAutoPlay = true;

	// Requested rendered point size. The renderer will consume this value later.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Point Cloud Sequence|Rendering", meta = (ClampMin = "0.1", UIMin = "0.1"))
	float PointSize = 1.0f;

private:
	void AdvancePlayback(float DeltaSeconds);
	void SetCurrentFrameInternal(int32 NewFrameIndex);
	void RequestFrameLoad(int32 FrameIndex);
	void LaunchPendingFrameLoad();
	void HandleFrameLoadCompleted(uint64 RequestId, uint64 RequestGeneration, int32 FrameIndex, struct FPCSPlyLoadResult &&Result);
	void InvalidatePendingLoads();

	/**
	 * Clamps the given frame index to the valid range of [0, FrameCount - 1].
	 * SeekFrame is BlueprintCallable, so this method is necessary to ensure that the frame index is always valid.
	 */
	int32 ClampFrameIndex(int32 FrameIndex) const;

	/**
	 * Returns the "safe" frame rate.
	 * This method is necessary because FrameRate can be edited anywhere including the editor or the blueprint.
	 */
	float GetSafeFrameRate() const;

	// Use double for returned value to avoid overflow when FrameCount is large and FrameRate is small.
	double GetSequenceDuration() const;

	UPROPERTY(Transient, VisibleInstanceOnly, Category = "Point Cloud Sequence|Playback")
	int32 CurrentFrameIndex = 0;

	UPROPERTY(Transient, VisibleInstanceOnly, Category = "Point Cloud Sequence|Playback")
	int32 FrameCount = 0;

	// The current playback time in seconds. Double is used to reduce error accumulation.
	UPROPERTY(Transient, VisibleInstanceOnly, Category = "Point Cloud Sequence|Playback")
	double PlaybackTimeSeconds = 0.0;

	UPROPERTY(Transient, VisibleInstanceOnly, Category = "Point Cloud Sequence|Playback")
	bool bPlaying = false;

	TArray<FString> SequenceFilePaths;
	TSharedPtr<const FPCSFrameData, ESPMode::ThreadSafe> CurrentFrameData;

	int32 LoadedFrameIndex = INDEX_NONE;
	int32 LoadingFrameIndex = INDEX_NONE;
	int32 PendingLoadFrameIndex = INDEX_NONE; // A single buffer for the next frame. New frame will replace this one

	uint64 SequenceGeneration = 0;
	uint64 NextLoadRequestId = 0;
	uint64 ActiveLoadRequestId = 0;
};
