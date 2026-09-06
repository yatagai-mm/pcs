#include "Rendering/PCSVertexFactory.h"

#include "Data/PCSFrameData.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "MaterialDomain.h"
#include "MeshBatch.h"
#include "MeshDrawShaderBindings.h"
#include "MeshMaterialShader.h"
#include "ShaderCompilerCore.h"

class FPCSVertexFactoryShaderParameters final : public FVertexFactoryShaderParameters
{
	DECLARE_TYPE_LAYOUT(FPCSVertexFactoryShaderParameters, NonVirtual);

public:
	void Bind(const FShaderParameterMap &ParameterMap)
	{
		PointSizePixels.Bind(ParameterMap, TEXT("PCSPointSizePixels"));
		ViewRight.Bind(ParameterMap, TEXT("PCSViewRight"));
		ViewUp.Bind(ParameterMap, TEXT("PCSViewUp"));
	}

	// GetElementShaderBindings is called when the render thread converts a FMeshBatchElement into a FMeshDrawCommand
	void GetElementShaderBindings(const FSceneInterface *, const FSceneView *, const FMeshMaterialShader *, EVertexInputStreamType, ERHIFeatureLevel::Type,
								  const FVertexFactory *, const FMeshBatchElement &BatchElement, FMeshDrawSingleShaderBindings &ShaderBindings,
								  FVertexInputStreamArray &) const
	{
		const FPCSVertexFactoryUserData *UserData = static_cast<const FPCSVertexFactoryUserData *>(BatchElement.UserData);
		check(UserData != nullptr);

		if (PointSizePixels.IsBound())
		{
			ShaderBindings.Add(PointSizePixels, UserData->PointSizePixels);
		}
		if (ViewRight.IsBound())
		{
			ShaderBindings.Add(ViewRight, UserData->ViewRight);
		}
		if (ViewUp.IsBound())
		{
			ShaderBindings.Add(ViewUp, UserData->ViewUp);
		}
	}

private:
	LAYOUT_FIELD(FShaderParameter, PointSizePixels);
	LAYOUT_FIELD(FShaderParameter, ViewRight);
	LAYOUT_FIELD(FShaderParameter, ViewUp);
};

IMPLEMENT_TYPE_LAYOUT(FPCSVertexFactoryShaderParameters);

FPCSVertexFactory::FPCSVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const FVertexBuffer *InQuadVertexBuffer, const FVertexBuffer *InPointVertexBuffer)
	: FVertexFactory(InFeatureLevel), QuadVertexBuffer(InQuadVertexBuffer), PointVertexBuffer(InPointVertexBuffer)
{
}

bool FPCSVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters &Parameters)
{
	// Until PCS owns a dedicated material usage flag, limit shader permutations to default materials instead of compiling this vertex factory for
	// every surface material in the project.
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) && Parameters.MaterialParameters.MaterialDomain == MD_Surface &&
		   (Parameters.MaterialParameters.bIsDefaultMaterial || Parameters.MaterialParameters.bIsSpecialEngineMaterial);
}

void FPCSVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters &, FShaderCompilerEnvironment &OutEnvironment)
{
	OutEnvironment.SetDefine(TEXT("PCS_VERTEX_FACTORY"), 1);
}

void FPCSVertexFactory::InitRHI(FRHICommandListBase &)
{
	check(QuadVertexBuffer != nullptr);
	check(PointVertexBuffer != nullptr);

	FVertexDeclarationElementList Elements;

	// ATTRIBUTE0 advances for each of the four shared quad vertices.
	Elements.Add(AccessStreamComponent(FVertexStreamComponent(QuadVertexBuffer, 0, sizeof(FVector2f), VET_Float2), 0));

	// ATTRIBUTE1 and ATTRIBUTE2 advance once per point instance. Consequently, all four quad vertices read the same point position and color without
	// duplicating the point record four times in GPU memory.
	Elements.Add(AccessStreamComponent(FVertexStreamComponent(PointVertexBuffer, STRUCT_OFFSET(FPCSPointVertex, Position), sizeof(FPCSPointVertex), VET_Float3,
															  EVertexStreamUsage::Instancing),
									   1));
	Elements.Add(AccessStreamComponent(
		FVertexStreamComponent(PointVertexBuffer, STRUCT_OFFSET(FPCSPointVertex, Color), sizeof(FPCSPointVertex), VET_Color, EVertexStreamUsage::Instancing),
		2));

	InitDeclaration(Elements);
}

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FPCSVertexFactory, SF_Vertex, FPCSVertexFactoryShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE(FPCSVertexFactory, "/Plugin/PCS/Private/PCSVertexFactory.ush",
							  EVertexFactoryFlags::UsedWithMaterials | EVertexFactoryFlags::SupportsDynamicLighting);
