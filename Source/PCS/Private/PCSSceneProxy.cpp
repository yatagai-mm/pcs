#include "PCSSceneProxy.h"

#include "Data/PCSFrameData.h"
#include "Engine/Engine.h"
#include "Materials/Material.h"
#include "MeshBatch.h"
#include "PointCloudSequenceComponent.h"
#include "Rendering/PCSRenderResources.h"

namespace
{
// UserData stored by FMeshBatchElement must remain alive until this view's
// collected mesh commands have been consumed.
class FPCSOneFrameVertexFactoryData final : public FOneFrameResource
{
public:
	FPCSVertexFactoryUserData Parameters;
};
} // namespace

FPCSSceneProxy::FPCSSceneProxy(const UPointCloudSequenceComponent *Component) : FPrimitiveSceneProxy(Component)
{
	check(IsInGameThread());
	PointSizePixels = FMath::Max(Component->PointSize, 0.0f);

	// TODO: Custom material support at some point? if needed
	const UMaterialInterface *Material = UMaterial::GetDefaultMaterial(MD_Surface);
	MaterialRenderProxy = Material->GetRenderProxy();
	MaterialRelevance = Material->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
	bWillEverBeLit = false;
}

FPCSSceneProxy::~FPCSSceneProxy() { ReleaseFrameResources_RenderThread(); }

// Called by MarkRenderDynamicDataDirty() from the game thread.
void FPCSSceneProxy::SetFrameData_RenderThread(FRHICommandListBase &RHICmdList, int32 InFrameIndex, TSharedPtr<const FPCSFrameData> InFrameData,
											   float InPointSizePixels)
{
	check(IsInRenderingThread());

	PointSizePixels = FMath::Max(InPointSizePixels, 0.0f);
	// Release previous frame resources here not to produce a dangling reference
	ReleaseFrameResources_RenderThread();
	FrameIndex = InFrameIndex;

	if (!InFrameData.IsValid() || InFrameData->Vertices.IsEmpty())
	{
		return;
	}

	// Copy a reference for GPU processing otherwise the frame data may be released unexpectedly.
	// GetFeatureLevel tells VertexFactory and shader about kinds of rendering feature supported by the current platform.
	FrameResources = MakeUnique<FPCSFrameRenderResources>(GetScene().GetFeatureLevel(), MoveTemp(InFrameData));
	FrameResources->InitResources(RHICmdList);
}

// Release reference to the FrameResources
void FPCSSceneProxy::ReleaseFrameResources_RenderThread()
{
	if (FrameResources.IsValid())
	{
		check(IsInRenderingThread());
		FrameResources->ReleaseResources();
		FrameResources.Reset();
	}
}

SIZE_T FPCSSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer; // Instance-agnostic static ID for this class type.
	return reinterpret_cast<size_t>(&UniquePointer);
}

// GetDynamicMeshElements is called at every rendering frame. Not necessarily a new PLY frame if the frame rate is lower than the display refresh rate.
void FPCSSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView *> &Views, const FSceneViewFamily &, uint32 VisibilityMap,
											FMeshElementCollector &Collector) const
{
	if (!FrameResources.IsValid() || !FrameResources->IsInitialized() || FrameResources->GetNumPoints() == 0 || MaterialRenderProxy == nullptr)
	{
		return;
	}

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if ((VisibilityMap & (1u << ViewIndex)) == 0)
		{
			continue;
		}

		const FSceneView *View = Views[ViewIndex];
		FPCSOneFrameVertexFactoryData &ViewData = Collector.AllocateOneFrameResource<FPCSOneFrameVertexFactoryData>();
		ViewData.Parameters.ViewRight = FVector3f(View->GetViewRight());
		ViewData.Parameters.ViewUp = FVector3f(View->GetViewUp());
		ViewData.Parameters.PointSizePixels = PointSizePixels;

		FMeshBatch &Mesh = Collector.AllocateMesh();
		Mesh.Type = PT_TriangleList;
		Mesh.VertexFactory = FrameResources->GetVertexFactory();
		Mesh.MaterialRenderProxy = MaterialRenderProxy;
		Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
		Mesh.bDisableBackfaceCulling = true;
		Mesh.bCanApplyViewModeOverrides = true;
		Mesh.CastShadow = false;
		Mesh.DepthPriorityGroup = SDPG_World;

		FMeshBatchElement &Element = Mesh.Elements[0];
		Element.PrimitiveUniformBuffer = GetUniformBuffer();
		Element.IndexBuffer = &GPCSQuadIndexBuffer;
		Element.FirstIndex = 0;
		Element.NumPrimitives = 2;
		Element.NumInstances = FrameResources->GetNumPoints();
		Element.MinVertexIndex = 0;
		Element.MaxVertexIndex = 3;
		Element.UserData = &ViewData.Parameters;

		Collector.AddMesh(ViewIndex, Mesh);
	}
}

FPrimitiveViewRelevance FPCSSceneProxy::GetViewRelevance(const FSceneView *View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bDynamicRelevance = true;
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bEditorPrimitiveRelevance = UseEditorCompositing(View);
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	return Result;
}

uint32 FPCSSceneProxy::GetMemoryFootprint() const
{
	// GetAllocatedSize() is a method in FPrimitiveSceneProxy that returns the size of any additional memory allocated by the proxy.
	return sizeof(*this) + GetAllocatedSize();
}
