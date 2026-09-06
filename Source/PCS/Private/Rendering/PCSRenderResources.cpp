#include "Rendering/PCSRenderResources.h"

#include "RHICommandList.h"

TGlobalResource<FPCSQuadVertexBuffer> GPCSQuadVertexBuffer;
TGlobalResource<FPCSQuadIndexBuffer> GPCSQuadIndexBuffer;

void FPCSQuadVertexBuffer::InitRHI(FRHICommandListBase &RHICmdList)
{
	// Signed, dimensionless offsets from a point center; the vertex shader turns them into World offsets.
	const FVector2f Corners[] = {
		FVector2f(-0.5f, -0.5f),
		FVector2f(0.5f, -0.5f),
		FVector2f(0.5f, 0.5f),
		FVector2f(-0.5f, 0.5f),
	};

	const FRHIBufferCreateDesc CreateDesc = FRHIBufferCreateDesc::CreateVertex<FVector2f>(TEXT("PCS.QuadVertices"), UE_ARRAY_COUNT(Corners))
												.AddUsage(EBufferUsageFlags::Static)
												.SetInitialState(ERHIAccess::VertexOrIndexBuffer)
												.SetInitActionInitializer();

	TRHIBufferInitializer<FVector2f> Initializer = RHICmdList.CreateBufferInitializer(CreateDesc);
	Initializer.WriteArray(MakeConstArrayView(Corners));
	VertexBufferRHI = Initializer.Finalize();
}

void FPCSQuadIndexBuffer::InitRHI(FRHICommandListBase &RHICmdList)
{
	const uint16 Indices[] = {0, 1, 2, 0, 2, 3};

	const FRHIBufferCreateDesc CreateDesc = FRHIBufferCreateDesc::CreateIndex<uint16>(TEXT("PCS.QuadIndices"), UE_ARRAY_COUNT(Indices))
												.AddUsage(EBufferUsageFlags::Static)
												.SetInitialState(ERHIAccess::VertexOrIndexBuffer)
												.SetInitActionInitializer();

	TRHIBufferInitializer<uint16> Initializer = RHICmdList.CreateBufferInitializer(CreateDesc);
	Initializer.WriteArray(MakeConstArrayView(Indices));
	IndexBufferRHI = Initializer.Finalize();
}

FPCSPointVertexBuffer::FPCSPointVertexBuffer(TSharedPtr<const FPCSFrameData> InFrameData) : FrameData(MoveTemp(InFrameData))
{
	check(FrameData.IsValid());
	checkf(static_cast<uint64>(FrameData->Vertices.Num()) * sizeof(FPCSPointVertex) <= MAX_uint32,
		   TEXT("A PCS frame exceeds the maximum size of one RHI vertex buffer."));
	NumPoints = static_cast<uint32>(FrameData->Vertices.Num());
}

void FPCSPointVertexBuffer::InitRHI(FRHICommandListBase &RHICmdList)
{
	check(FrameData.IsValid());
	check(NumPoints > 0);

	const FRHIBufferCreateDesc CreateDesc = FRHIBufferCreateDesc::CreateVertex<FPCSPointVertex>(TEXT("PCS.PointFrame"), NumPoints)
												// Buffer can be immutable here because FrameResources is recreated for every frame
												.AddUsage(EBufferUsageFlags::Static)
												.SetInitialState(ERHIAccess::VertexOrIndexBuffer)
												.SetInitActionInitializer();

	TRHIBufferInitializer<FPCSPointVertex> Initializer = RHICmdList.CreateBufferInitializer(CreateDesc);
	Initializer.WriteArray(MakeConstArrayView(FrameData->Vertices));
	VertexBufferRHI = Initializer.Finalize();

	// The immutable CPU frame crossed from the game thread only to seed this
	// upload. Once the RHI owns a copy, the render resource no longer retains it.
	FrameData.Reset();
}

FPCSFrameRenderResources::FPCSFrameRenderResources(ERHIFeatureLevel::Type FeatureLevel, TSharedPtr<const FPCSFrameData> InFrameData)
	: PointVertexBuffer(MoveTemp(InFrameData)), VertexFactory(FeatureLevel, &GPCSQuadVertexBuffer, &PointVertexBuffer)
{
}

FPCSFrameRenderResources::~FPCSFrameRenderResources()
{
	check(!PointVertexBuffer.IsInitialized());
	check(!VertexFactory.IsInitialized());
}

void FPCSFrameRenderResources::InitResources(FRHICommandListBase &RHICmdList)
{
	check(IsInRenderingThread());
	// These ultimately call InitRHI() on both buffers
	PointVertexBuffer.InitResource(RHICmdList);
	VertexFactory.InitResource(RHICmdList);
}

void FPCSFrameRenderResources::ReleaseResources()
{
	check(IsInRenderingThread());

	// Release the declaration first because it references streams backed by the
	// frame vertex buffer.
	VertexFactory.ReleaseResource();
	PointVertexBuffer.ReleaseResource();
}
