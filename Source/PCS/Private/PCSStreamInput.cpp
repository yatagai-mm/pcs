#include "PCSStreamInput.h"
#include "Algo/BinarySearch.h"

#include "Misc/ScopeLock.h"

FPCSStreamInput::FPCSStreamInput(const FPCSStreamConfig &InConfig) : Config(InConfig)
{
	Config.MaxBufferedFrames = FMath::Max(1, Config.MaxBufferedFrames);
	Config.MaxBufferedBytes = FMath::Max<uint64>(sizeof(FPCSPointVertex), Config.MaxBufferedBytes);
}

EPCSSubmitResult FPCSStreamInput::TrySubmit(const FPCSTimedFrame &Frame)
{
	FScopeLock Lock(&Mutex);
	if (bClosed || bEndOfStream)
	{
		return EPCSSubmitResult::Closed;
	}
	if (!Frame.Data.IsValid() || Frame.FrameId < 0 || !FMath::IsFinite(Frame.PresentationTime) || Frame.PresentationTime < 0.0 ||
		!FMath::IsFinite(Frame.Duration) || Frame.Duration <= 0.0 || !FMath::IsFinite(Frame.PresentationTime + Frame.Duration) ||
		Frame.PresentationTime + Frame.Duration <= Frame.PresentationTime ||
		static_cast<uint64>(Frame.Data->Vertices.Num()) * sizeof(FPCSPointVertex) > MAX_uint32)
	{
		return EPCSSubmitResult::Invalid;
	}
	const FBox3f &Bounds = Frame.Data->Bounds;
	if (!Frame.Data->Vertices.IsEmpty() &&
		(!Bounds.IsValid || !FMath::IsFinite(Bounds.Min.X) || !FMath::IsFinite(Bounds.Min.Y) || !FMath::IsFinite(Bounds.Min.Z) ||
		 !FMath::IsFinite(Bounds.Max.X) || !FMath::IsFinite(Bounds.Max.Y) || !FMath::IsFinite(Bounds.Max.Z) || Bounds.Min.X > Bounds.Max.X ||
		 Bounds.Min.Y > Bounds.Max.Y || Bounds.Min.Z > Bounds.Max.Z))
	{
		return EPCSSubmitResult::Invalid;
	}
	if (LastPresentedId != INDEX_NONE && Frame.PresentationTime <= LastPresentedTime)
	{
		return EPCSSubmitResult::DroppedLate;
	}
	if (Frame.FrameId == LastPresentedId ||
		Frames.ContainsByPredicate([&Frame](const FPCSTimedFrame &Queued)
								   { return Queued.FrameId == Frame.FrameId || Queued.PresentationTime == Frame.PresentationTime; }))
	{
		return EPCSSubmitResult::Duplicate;
	}
	const uint64 Bytes = Frame.Data->Vertices.GetAllocatedSize();
	if (Bytes > Config.MaxBufferedBytes)
	{
		return EPCSSubmitResult::Invalid;
	}
	if (Frames.Num() >= Config.MaxBufferedFrames || Bytes > Config.MaxBufferedBytes - BufferedBytes)
	{
		return EPCSSubmitResult::Backpressure;
	}
	const int32 Index = Algo::LowerBoundBy(Frames, Frame.PresentationTime, &FPCSTimedFrame::PresentationTime);
	Frames.Insert(Frame, Index);
	BufferedBytes += Bytes;
	EndTime = FMath::Max(EndTime, Frame.PresentationTime + Frame.Duration);
	return EPCSSubmitResult::Accepted;
}

void FPCSStreamInput::SignalEndOfStream()
{
	FScopeLock Lock(&Mutex);
	bEndOfStream = true;
}

void FPCSStreamInput::Close()
{
	FScopeLock Lock(&Mutex);
	bClosed = true;
	Frames.Reset();
	BufferedBytes = 0;
}

bool FPCSStreamInput::IsClosed() const
{
	FScopeLock Lock(&Mutex);
	return bClosed;
}

int32 FPCSStreamInput::GetBufferedFrameCount() const
{
	FScopeLock Lock(&Mutex);
	return Frames.Num();
}

uint64 FPCSStreamInput::GetBufferedBytes() const
{
	FScopeLock Lock(&Mutex);
	return BufferedBytes;
}

FPCSStreamInput::FTickResult FPCSStreamInput::Advance(double DeltaSeconds)
{
	FScopeLock Lock(&Mutex);
	FTickResult Result;
	Result.PlaybackTime = PlaybackTime;
	if (bClosed)
	{
		return Result;
	}
	if (!bStarted && !Frames.IsEmpty())
	{
		// Anchor on the earliest queued PTS, never on arrival time or frame count.
		PlaybackTime = Frames[0].PresentationTime;
		bStarted = true;
	}
	else if (bStarted)
	{
		PlaybackTime += DeltaSeconds;
	}
	int32 DueCount = 0;
	while (DueCount < Frames.Num() && Frames[DueCount].PresentationTime <= PlaybackTime)
	{
		BufferedBytes -= Frames[DueCount].Data->Vertices.GetAllocatedSize();
		++DueCount;
	}
	if (DueCount > 0)
	{
		Result.Frame = MoveTemp(Frames[DueCount - 1]);
		LastPresentedTime = Result.Frame.PresentationTime;
		LastPresentedId = Result.Frame.FrameId;
		Frames.RemoveAt(0, DueCount, EAllowShrinking::No);
	}
	if (bEndOfStream && Frames.IsEmpty() && (!bStarted || PlaybackTime >= EndTime))
	{
		// Clamp this tick's overshoot, but never rewind if EOS arrived after an
		// underflow had already advanced the clock beyond the final duration.
		PlaybackTime = FMath::Max(Result.PlaybackTime, EndTime);
		bClosed = true;
		Result.bFinished = true;
	}
	Result.PlaybackTime = PlaybackTime;
	return Result;
}
