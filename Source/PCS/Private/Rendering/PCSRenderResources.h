// QuadIndexBuffer and PointVertexBuffer are shared among all points in a frame, only require 44B combined among all frames.

#pragma once

#include "CoreMinimal.h"
#include "Data/PCSFrameData.h"
#include "RenderResource.h"
#include "Rendering/PCSVertexFactory.h"

// QuadVertexBuffer gives GPU the centered offsets of the quad corners. This will be shared among all points.
class FPCSQuadVertexBuffer : public FVertexBuffer
{
public:
	virtual void InitRHI(FRHICommandListBase &RHICmdList) override;
	virtual FString GetFriendlyName() const override { return TEXT("FPCSQuadVertexBuffer"); }
};

// QuadIndexBuffer gives GPU the indices of the quad corners to form two triangles. This will be shared among all points.
class FPCSQuadIndexBuffer : public FIndexBuffer
{
public:
	virtual void InitRHI(FRHICommandListBase &RHICmdList) override;
};

// PointeVertexBuffer gives GPU the centered offsets of the point and its color. This will be unique for each point in the frame.
class FPCSPointVertexBuffer final : public FVertexBuffer
{
public:
	explicit FPCSPointVertexBuffer(TSharedPtr<const FPCSFrameData> InFrameData);

	virtual void InitRHI(FRHICommandListBase &RHICmdList) override;
	virtual FString GetFriendlyName() const override { return TEXT("FPCSPointVertexBuffer"); }

	uint32 GetNumPoints() const { return NumPoints; }

private:
	TSharedPtr<const FPCSFrameData> FrameData;
	uint32 NumPoints = 0;
};

extern TGlobalResource<FPCSQuadVertexBuffer> GPCSQuadVertexBuffer;
extern TGlobalResource<FPCSQuadIndexBuffer> GPCSQuadIndexBuffer;

// Render-thread-owned resources for one active point-cloud frame. The point
// buffer is frame-specific, while its quad geometry is shared globally.
class FPCSFrameRenderResources final
{
public:
	FPCSFrameRenderResources(ERHIFeatureLevel::Type FeatureLevel, TSharedPtr<const FPCSFrameData> InFrameData);
	~FPCSFrameRenderResources();

	void InitResources(FRHICommandListBase &RHICmdList);
	void ReleaseResources();

	const FPCSVertexFactory *GetVertexFactory() const { return &VertexFactory; }
	uint32 GetNumPoints() const { return PointVertexBuffer.GetNumPoints(); }
	bool IsInitialized() const { return PointVertexBuffer.IsInitialized() && VertexFactory.IsInitialized(); }

private:
	FPCSPointVertexBuffer PointVertexBuffer;
	FPCSVertexFactory VertexFactory;
};
