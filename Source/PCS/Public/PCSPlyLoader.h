#pragma once

#include "CoreMinimal.h"

#include "PCSFrameData.h"

// Result of loading one PLY frame. Exactly one of FrameData and ErrorMessage is populated.
struct PCS_API FPCSPlyLoadResult
{
	TSharedPtr<const FPCSFrameData> FrameData;
	FString ErrorMessage;

	// Returns true if the PLY frame was successfully loaded and FrameData is valid.
	// Otherwise, returns false and ErrorMessage contains a description of the failure.
	[[nodiscard]]
	bool IsSuccess() const
	{
		return FrameData.IsValid();
	}
};

// Synchronous PLY decoder.
//
// The loader does not access UObjects. Call it on a worker for playback/sending;
// it performs blocking file I/O. Independent calls may run concurrently.
// This supports scalar vertex properties in binary little-endian PLY 1.0 files.
// The returned data contains decoded positions, colors and local-space bounds.
// Optional frame_to_world comments are applied; no actor transform is applied.
class PCS_API FPCSPlyLoader final
{
public:
	// User of this method should make use of return value.
	[[nodiscard]] static FPCSPlyLoadResult LoadFromFile(const FString &FilePath);
};
