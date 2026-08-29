#include "PCSSceneProxy.h"

#include "PointCloudSequenceComponent.h"

FPCSSceneProxy::FPCSSceneProxy(const UPointCloudSequenceComponent *Component) : FPrimitiveSceneProxy(Component)
{
	check(IsInGameThread());
	bWillEverBeLit = false;
}

SIZE_T FPCSSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer; // Instance-agnostic static ID for this class type.
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FPCSSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView *> &, const FSceneViewFamily &, uint32, FMeshElementCollector &) const
{
	// The frame GPU buffers and their FMeshBatch submission will be added here.
}

FPrimitiveViewRelevance FPCSSceneProxy::GetViewRelevance(const FSceneView *View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bDynamicRelevance = true;
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bEditorPrimitiveRelevance = UseEditorCompositing(View);
	return Result;
}

uint32 FPCSSceneProxy::GetMemoryFootprint() const
{
	// GetAllocatedSize() is a method in FPrimitiveSceneProxy that returns the size of any additional memory allocated by the proxy.
	return sizeof(*this) + GetAllocatedSize();
}
