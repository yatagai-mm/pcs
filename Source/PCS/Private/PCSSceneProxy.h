#pragma once

#include "PrimitiveSceneProxy.h"

struct FPCSFrameData;
class UPointCloudSequenceComponent;

/**
 * Render-thread representation of UPointCloudSequenceComponent.
 *
 * The proxy must not read the component after construction because the
 * component belongs to the game thread.
 *
 * The users of this plugin are also not expected to access this proxy directly.
 */
class FPCSSceneProxy final : public FPrimitiveSceneProxy
{
public:
	explicit FPCSSceneProxy(const UPointCloudSequenceComponent *Component);

	void SetFrameData_RenderThread(int32 InFrameIndex, TSharedPtr<const FPCSFrameData> InFrameData);

	// Get an identifier for this class type
	virtual SIZE_T GetTypeHash() const override;

	virtual void GetDynamicMeshElements(const TArray<const FSceneView *> &Views, const FSceneViewFamily &ViewFamily, uint32 VisibilityMap,
										FMeshElementCollector &Collector) const override;

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView *View) const override;
	virtual uint32 GetMemoryFootprint() const override;

private:
	TSharedPtr<const FPCSFrameData> FrameData;
	int32 FrameIndex = INDEX_NONE;
};
