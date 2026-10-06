#pragma once

#include "Components/PrimitiveComponent.h"
#include "CoreMinimal.h"
#include "PCSStreamInput.h"
#include "UObject/SoftObjectPath.h"

#include "PointCloudSequenceComponent.generated.h"

struct FPCSFrameData;
class UMaterialInterface;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPCSFrameChangedSignature, int32, PreviousFrameIndex, int32, CurrentFrameIndex);

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FPCSPlaybackFinishedSignature);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPCSStreamFrameActivatedSignature, int64, FrameId, double, PresentationTime);

// Game-thread-facing playback component for a point-cloud frame sequence.
//
// This class owns playback state, discovers the source sequence, and coordinates
// asynchronous PLY loading. Render-thread state and GPU resources remain in the
// scene proxy.
UCLASS(BlueprintType, ClassGroup = (PCS), meta = (BlueprintSpawnableComponent, DisplayName = "Point Cloud Sequence"))
class PCS_API UPointCloudSequenceComponent final : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UPointCloudSequenceComponent();

	virtual FPrimitiveSceneProxy *CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform &LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface *> &OutMaterials, bool bGetDebugMaterials = false) const override;
	virtual void SendRenderDynamicData_Concurrent() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent &PropertyChangedEvent) override;
#endif

	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Rendering")
	void SetConvertSRGBToLinear(bool bEnabled);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginDestroy() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;

	// Game thread only. Replaces the source and clears the display. The returned
	// handle accepts decoded frames from any thread. Call Play() to start.
	// FrameCount stays zero; FrameRate, bLoop and Seek do not apply to streams.
	TSharedRef<FPCSStreamInput, ESPMode::ThreadSafe> OpenStream(const FPCSStreamConfig &Config = FPCSStreamConfig());

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Source")
	bool IsStreaming() const { return bStreamMode; }

	// Last frame activated toward the renderer, not a GPU presentation timestamp.
	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Playback")
	int64 GetCurrentStreamFrameId() const { return CurrentStreamFrameId; }

	UPROPERTY(BlueprintAssignable, Category = "Point Cloud Sequence|Events")
	FPCSStreamFrameActivatedSignature OnStreamFrameActivated;

	// Starts or resumes timeline advancement. This request is retained while the sequence is still loading.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Playback")
	void Play();

	// Pauses timeline advancement without changing the selected frame.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Playback")
	void Pause();

	// Files: returns to frame zero. Streams: closes input and keeps the last image;
	// call OpenStream() to start another stream.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Playback")
	void Stop();

	// Selects a file frame immediately. No-op for streams.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Playback")
	void SeekFrame(int32 FrameIndex);

	// Selects a file timeline position in seconds. No-op for streams.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Playback")
	void SeekTime(double TimeSeconds);

	// Changes the source directory, rescans it, and resets the current playback state.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Source")
	void SetSequenceDirectory(const FString &Directory);

	// Changes both source inputs and rescans the directory. Capture group 1 of
	// FileNameRegex must contain the integer sequence number.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Source")
	void SetSequenceSource(const FString &Directory, const FString &FileNameRegex);

	// Rescans the configured directory and requests the first matching frame.
	UFUNCTION(BlueprintCallable, Category = "Point Cloud Sequence|Source")
	bool RefreshSequence();

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Source")
	FString GetSequenceDirectory() const;

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Source")
	FString GetFrameFileNameRegex() const { return FrameFileNameRegex; }

	// Publishes the number of frames discovered by a loader.
	// This must be called on the game thread after asynchronous discovery completes.
	void SetFrameCount(int32 InFrameCount);

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Playback")
	bool IsPlaying() const { return bPlaying; }

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Playback")
	int32 GetCurrentFrame() const { return CurrentFrameIndex; }

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Playback")
	int32 GetFrameCount() const { return FrameCount; }

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Playback")
	int32 GetLoadedFrame() const { return LoadedFrameIndex; }

	// Returns the sequence index stored in the first parsed look-ahead slot, or INDEX_NONE when the buffer is empty.
	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Playback")
	int32 GetBufferedFrame() const;

	UFUNCTION(BlueprintPure, Category = "Point Cloud Sequence|Playback")
	double GetPlaybackTime() const { return PlaybackTimeSeconds; }

	// Emitted on the game thread whenever the selected frame changes.
	UPROPERTY(BlueprintAssignable, Category = "Point Cloud Sequence|Events")
	FPCSFrameChangedSignature OnFrameChanged;

	// Emitted on the game thread when non-looping file playback ends, or a stream
	// drains after SignalEndOfStream() and the final duration elapses.
	UPROPERTY(BlueprintAssignable, Category = "Point Cloud Sequence|Events")
	FPCSPlaybackFinishedSignature OnPlaybackFinished;

	// Directory containing the PLY frame sequence.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Point Cloud Sequence|Source")
	FDirectoryPath SequenceDirectory;

	// Full-file-name regular expression. Capture group 1 is parsed as the sequence
	// number used to order matching files.
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

	// Interpret PLY RGB as sRGB and decode it to linear RGB before shading.
	// Disable for PLY files that already contain linear RGB. Alpha is unchanged.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Point Cloud Sequence|Rendering", meta = (DisplayName = "Convert sRGB to Linear"))
	bool bConvertSRGBToLinear = true;

	// Surface material used to shade every point quad. The PCS default material
	// is unlit and forwards each PLY vertex color to Emissive Color.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Point Cloud Sequence|Rendering")
	TObjectPtr<UMaterialInterface> PointMaterial;

private:
	friend class FPCSSceneProxy;

	// Every frame index below is an ordinal position in SequenceFilePaths after
	// sorting by the regex capture value. It is not necessarily the number written in the file name.
	struct FPCSBufferedFrame
	{
		// Parsed look-ahead frame waiting for its presentation time. It is not displayed yet.
		int32 FrameIndex = INDEX_NONE;
		TSharedPtr<const FPCSFrameData> FrameData;
	};

	void AdvancePlayback(float DeltaSeconds);
	void AdvanceStreamPlayback(double DeltaSeconds);
	void CloseStreamInput();
	void SetCurrentFrameInternal(int32 NewFrameIndex);
	void RequestFrameLoad(int32 FrameIndex);
	void RequestNextFrameLoad();
	void LaunchPendingFrameLoad();
	void HandleFrameLoadCompleted(uint64 RequestId, uint64 RequestGeneration, int32 FrameIndex, struct FPCSPlyLoadResult &&Result);
	void ActivateFrame(int32 FrameIndex, TSharedPtr<const FPCSFrameData> FrameData);
	bool TryActivateBufferedFrame(int32 FrameIndex);
	bool IsFrameBuffered(int32 FrameIndex) const;
	bool IsFrameInBufferWindow(int32 FrameIndex) const;
	void InvalidatePendingLoads();
	int32 GetFollowingFrameIndex(int32 FrameIndex, int32 Offset) const;

	// Clamps the given frame index to the valid range of [0, FrameCount - 1].
	// SeekFrame is BlueprintCallable, so this method is necessary to ensure that the frame index is always valid.
	int32 ClampFrameIndex(int32 FrameIndex) const;

	// Returns the "safe" frame rate.
	// This method is necessary because FrameRate can be edited anywhere including the editor or the blueprint.
	float GetSafeFrameRate() const;

	// Use double for returned value to avoid overflow when FrameCount is large and FrameRate is small.
	double GetSequenceDuration() const;

	// Frame selected by the playback clock. It may be ahead of the frame currently displayed while loading catches up.
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
	TSharedPtr<const FPCSFrameData> CurrentFrameData;
	TSharedPtr<FPCSStreamInput, ESPMode::ThreadSafe> StreamInput;
	bool bStreamMode = false;
	int64 CurrentStreamFrameId = INDEX_NONE;

	// Parsed future frames waiting for their presentation time.
	// FrameBufferSize limits the number of elements; it is currently one.
	TArray<FPCSBufferedFrame> BufferedFrames;
	int32 FrameBufferSize = 1;

	// Frame belonging to CurrentFrameData. It has been activated and sent toward
	// the scene proxy. INDEX_NONE means no frame is ready yet.
	int32 LoadedFrameIndex = INDEX_NONE;

	// Frame currently being parsed by the active worker task.
	// INDEX_NONE means that no PLY parse is running.
	int32 LoadingFrameIndex = INDEX_NONE;

	// Latest frame to load after the active worker task finishes.
	// Newer playback requests replace this value. INDEX_NONE means no request is queued.
	int32 PendingLoadFrameIndex = INDEX_NONE;

	// Generation counter for target sequence. Incremented whenever the sequence is refreshed or the directory changes.
	uint64 SequenceGeneration = 0;
	uint64 NextLoadRequestId = 0;
	uint64 ActiveLoadRequestId = 0;
	// Active SequenceGeneration number. Used to ignore load requests from previous generations.
	uint64 ActiveLoadGeneration = 0;
};
