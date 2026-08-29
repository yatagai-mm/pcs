#pragma once

#include "CoreMinimal.h"

// One point decoded from a point-cloud frame. Positions use the units stored in the PLY file.
struct FPCSPoint
{
	FVector3f Position = FVector3f::ZeroVector;
	FColor Color = FColor::White;
};

// Immutable CPU representation of one decoded point-cloud frame.
struct FPCSFrameData
{
	TArray<FPCSPoint> Points;
	FBox3f Bounds = FBox3f(ForceInit);
};
