#pragma once

#include "CoreMinimal.h"
#include "VertexFactory.h"

class FVertexBuffer;

// Per-view values consumed while the vertex shader expands one point into a
// camera-facing quad. A separate value is built for every visible view so that
// stereo rendering does not accidentally reuse the first eye's camera axes.
struct FPCSVertexFactoryUserData
{
	FVector3f ViewRight = FVector3f::RightVector;
	FVector3f ViewUp = FVector3f::UpVector;
	float PointSizePixels = 1.0f;
};

class FPCSVertexFactory final : public FVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FPCSVertexFactory);

public:
	// QuadVertexBuffer and PointVertexBuffer must remain alive for the lifetime of this factory
	FPCSVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const FVertexBuffer *InQuadVertexBuffer, const FVertexBuffer *InPointVertexBuffer);

	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters &Parameters);
	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters &Parameters, FShaderCompilerEnvironment &OutEnvironment);

	virtual void InitRHI(FRHICommandListBase &RHICmdList) override;
	virtual bool RendersPrimitivesAsCameraFacingSprites() const override { return true; }

private:
	const FVertexBuffer *QuadVertexBuffer = nullptr;
	const FVertexBuffer *PointVertexBuffer = nullptr;
};
