#pragma once

#include "CoreMinimal.h"

// One point in the exact interleaved layout uploaded to the GPU.
// Positions remain in the units stored in the PLY file.
struct FPCSPointVertex
{
	FVector3f Position = FVector3f::ZeroVector;
	FColor Color = FColor::White;
};

static_assert(sizeof(FPCSPointVertex) == 16, "FPCSPointVertex must have a 16-byte GPU vertex stride.");
static_assert(STRUCT_OFFSET(FPCSPointVertex, Color) == 12, "FPCSPointVertex::Color must begin at byte offset 12.");

// Immutable, GPU-upload-ready CPU representation of one decoded point-cloud frame.
struct FPCSFrameData
{
	TArray<FPCSPointVertex> Vertices;
	FBox3f Bounds = FBox3f(ForceInit);
};
