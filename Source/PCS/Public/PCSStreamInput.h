#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "PCSFrameData.h"

// A complete decoded frame. PTS and duration are in seconds in the source's
// media timeline, not wall-clock time. IDs are non-negative application IDs.
struct PCS_API FPCSTimedFrame
{
	int64 FrameId = 0;
	double PresentationTime = 0.0;
	double Duration = 0.0;
	TSharedPtr<const FPCSFrameData, ESPMode::ThreadSafe> Data;
};

struct PCS_API FPCSStreamConfig
{
	int32 MaxBufferedFrames = 8;
	uint64 MaxBufferedBytes = 512ull * 1024 * 1024;
};

enum class EPCSSubmitResult : uint8
{
	Accepted,
	Backpressure,
	DroppedLate,
	Duplicate,
	Closed,
	Invalid
};

// Thread-safe producer endpoint, independent of UObjects and transport libraries.
// The component owns playback; producers may retain this handle across shutdown.
// Accepted frames must remain immutable (including through mutable aliases).
// Limits cover queued CPU vertex allocations, excluding the displayed frame,
// producer-owned data, and render commands/GPU resources.
class PCS_API FPCSStreamInput final
{
public:
	explicit FPCSStreamInput(const FPCSStreamConfig &InConfig);

	// Non-blocking with respect to queue capacity. A short mutex protects state.
	// Rejected frames are not retained. Nonempty frames require finite local bounds
	// enclosing finite vertices; the producer is responsible for vertex validation.
	EPCSSubmitResult TrySubmit(const FPCSTimedFrame &Frame);

	// Call only after every producer has finished submitting. Drains before finish.
	void SignalEndOfStream();
	// Discards queued data and permanently rejects input. Safe to call repeatedly.
	void Close();
	bool IsClosed() const;
	int32 GetBufferedFrameCount() const;
	uint64 GetBufferedBytes() const;

private:
	friend class UPointCloudSequenceComponent;
	struct FTickResult
	{
		FPCSTimedFrame Frame;
		double PlaybackTime = 0.0;
		bool bFinished = false;
	};
	FTickResult Advance(double DeltaSeconds);

	mutable FCriticalSection Mutex;
	FPCSStreamConfig Config;
	TArray<FPCSTimedFrame> Frames;
	uint64 BufferedBytes = 0;
	double PlaybackTime = 0.0;
	double LastPresentedTime = 0.0;
	double EndTime = 0.0;
	int64 LastPresentedId = INDEX_NONE;
	bool bStarted = false;
	bool bEndOfStream = false;
	bool bClosed = false;
};
