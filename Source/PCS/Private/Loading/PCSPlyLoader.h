#pragma once

#include "CoreMinimal.h"

#include "Data/PCSFrameData.h"

// Result of loading one PLY frame. Exactly one of FrameData and ErrorMessage is populated.
struct FPCSPlyLoadResult
{
	TSharedPtr<const FPCSFrameData, ESPMode::ThreadSafe> FrameData;
	FString ErrorMessage;

	/**
	 * Returns true if the PLY frame was successfully loaded and FrameData is valid.
	 * Otherwise, returns false and ErrorMessage contains a description of the failure.
	 */
	[[nodiscard]]
	bool IsSuccess() const
	{
		return FrameData.IsValid();
	}
};

/**
 * Synchronous PLY decoder.
 *
 * The loader does not access UObjects and is intended to be called from a
 * worker task (inside UE::Tasks::Launch bracket).
 * This supports scalar vertex properties in binary little-endian PLY 1.0 files.
 */
class FPCSPlyLoader final
{
public:
	// User of this method should make use of return value.
	[[nodiscard]] static FPCSPlyLoadResult LoadFromFile(const FString &FilePath);
};
