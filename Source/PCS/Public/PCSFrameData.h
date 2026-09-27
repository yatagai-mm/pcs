#pragma once

#include "CoreMinimal.h"

// One point in the exact interleaved layout uploaded to the GPU.
// Positions are in component-local space, before the component transform.
struct PCS_API FPCSPointVertex
{
	FVector3f Position = FVector3f::ZeroVector;
	FColor Color = FColor::White;
};

static_assert(sizeof(FPCSPointVertex) == 16, "FPCSPointVertex must have a 16-byte GPU vertex stride.");
static_assert(STRUCT_OFFSET(FPCSPointVertex, Color) == 12, "FPCSPointVertex::Color must begin at byte offset 12.");

// GPU-upload-ready CPU representation of one decoded point-cloud frame.
// Populate Vertices and matching local-space Bounds before sharing with consumers,
// then treat the frame as immutable for as long as any consumer retains it.
struct PCS_API FPCSFrameData
{
	TArray<FPCSPointVertex> Vertices;
	FBox3f Bounds = FBox3f(ForceInit);
};
