#pragma once

#include "PrimitiveSceneProxy.h"

struct FPCSFrameData;
class FPCSFrameRenderResources;
class FMaterialRenderProxy;
class UPointCloudSequenceComponent;

// Render-thread representation of UPointCloudSequenceComponent.
//
// The proxy must not read the component after construction because the
// component belongs to the game thread.
//
// The users of this plugin are also not expected to access this proxy directly.
class FPCSSceneProxy final : public FPrimitiveSceneProxy
{
public:
	explicit FPCSSceneProxy(const UPointCloudSequenceComponent *Component);
	virtual ~FPCSSceneProxy() override;
	virtual void CreateRenderThreadResources(FRHICommandListBase &RHICmdList) override;

	void SetFrameData_RenderThread(FRHICommandListBase &RHICmdList, int32 InFrameIndex, TSharedPtr<const FPCSFrameData> InFrameData, float InPointSizePixels);

	// Get an identifier for this class type
	virtual SIZE_T GetTypeHash() const override;

	virtual void GetDynamicMeshElements(const TArray<const FSceneView *> &Views, const FSceneViewFamily &ViewFamily, uint32 VisibilityMap,
										FMeshElementCollector &Collector) const override;

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView *View) const override;
	virtual uint32 GetMemoryFootprint() const override;

private:
	void ReleaseFrameResources_RenderThread();

	// Pointer reference for GPU processingto the frame data received from the game thread
	TUniquePtr<FPCSFrameRenderResources> FrameResources;
	// Snapshot copied while the proxy is constructed on the game thread. UE can
	// recreate a proxy after a bounds change, so the replacement must be able to
	// restore the already active frame without waiting for another PLY change.
	TSharedPtr<const FPCSFrameData> InitialFrameData;
	const FMaterialRenderProxy *MaterialRenderProxy = nullptr;
	FMaterialRelevance MaterialRelevance;
	int32 FrameIndex = INDEX_NONE;
	float PointSizePixels = 1.0f;
};
