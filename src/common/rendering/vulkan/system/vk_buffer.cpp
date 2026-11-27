/*
** vk_buffer.cpp
**
** Vulkan backend
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Copyright 2016-2020 Magnus Norddahl
**
** SPDX-License-Identifier: Zlib
**
**---------------------------------------------------------------------------
**
*/

#include "vk_buffer.h"
#include "vk_hwbuffer.h"
#include "vk_renderdevice.h"
#include "vk_commandbuffer.h"
#include "vulkan/renderer/vk_streambuffer.h"
#include "vulkan/shaders/vk_shader.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "hwrenderer/data/buffers.h"
#include "hwrenderer/data/flatvertices.h"
#include "templates.h"
#include "cmdlib.h"

#include <vector>
#include <algorithm>

VkBufferManager::VkBufferManager(VulkanRenderDevice* fb) : fb(fb)
{
}

VkBufferManager::~VkBufferManager()
{
}

void VkBufferManager::Init()
{
	MatrixBuffer.reset(new VkStreamBuffer(this, sizeof(MatricesUBO), 50000));
	StreamBuffer.reset(new VkStreamBuffer(this, sizeof(StreamUBO), 300));

	CreateFanToTrisIndexBuffer();
	RetainedParameters.reset(CreateDataBuffer(-1, true, false));
	RetainedParameters->SetData(16, nullptr, BufferUsageType::Persistent);
	RetainedIndices.reset(CreateIndexBuffer());
	RetainedIndices->SetData(16, nullptr, BufferUsageType::Persistent);
	SectorAppearance.reset(CreateDataBuffer(-1, true, false));
	SectorAppearance->SetData(size_t(kSharedAppearanceCapacity) * 64, nullptr, BufferUsageType::Persistent);
	Coverage.reset(CreateDataBuffer(-1, true, false));
	Coverage->SetData(16, nullptr, BufferUsageType::Persistent);
	StaticGeometry.reset(CreateDataBuffer(-1, true, false));
	StaticGeometry->SetData(16, nullptr, BufferUsageType::Persistent);
	StaticIndices.reset(CreateDataBuffer(-1, true, false));
	StaticIndices->SetData(16, nullptr, BufferUsageType::Persistent);
	PrimitiveTable.reset(CreateDataBuffer(-1, true, false));
	PrimitiveTable->SetData(16, nullptr, BufferUsageType::Persistent);
	PrimitiveGrid.reset(CreateDataBuffer(-1, true, false));
	PrimitiveGrid->SetData(16, nullptr, BufferUsageType::Persistent);
	PrimitiveRefs.reset(CreateDataBuffer(-1, true, false));
	PrimitiveRefs->SetData(16, nullptr, BufferUsageType::Persistent);
	GpuCullVisible.reset(CreateDataBuffer(-1, true, true));
	GpuCullVisible->SetData(16, nullptr, BufferUsageType::Persistent);
	GpuCullArgs.reset(CreateDataBuffer(-1, true, true));
	static_cast<VkHardwareDataBuffer*>(GpuCullArgs.get())->mBufferType |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
	GpuCullArgs->SetData(5 * sizeof(uint32_t), nullptr, BufferUsageType::Persistent);
	GpuCullCommands.reset(CreateDataBuffer(-1, true, true));
	static_cast<VkHardwareDataBuffer*>(GpuCullCommands.get())->mBufferType |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
	GpuCullCommands->SetData(16, nullptr, BufferUsageType::Persistent);
	GpuCullDrawMaterials.reset(CreateDataBuffer(-1, true, true));
	GpuCullDrawMaterials->SetData(16, nullptr, BufferUsageType::Persistent);
	GpuWorldAppearance.reset(CreateDataBuffer(-1, true, false));
	GpuWorldAppearance->SetData(16, nullptr, BufferUsageType::Persistent);
	GpuResidentAppearance.reset(CreateDataBuffer(-1, true, false));
	GpuResidentReferences.reset(CreateDataBuffer(-1, true, false));
	GpuCullAdmission.reset(CreateDataBuffer(-1, true, false));
	GpuCullAdmission->SetData(16, nullptr, BufferUsageType::Persistent);
	PrimitiveBounds.reset(CreateDataBuffer(-1, true, false));
	PrimitiveBounds->SetData(16, nullptr, BufferUsageType::Persistent);
	PrimitiveMaterials.reset(CreateDataBuffer(-1, true, false));
	PrimitiveMaterials->SetData(16, nullptr, BufferUsageType::Persistent);
	GpuQuadIndices.reset(CreateIndexBuffer());
	{
		const uint32_t quad[6] = { 0, 1, 2, 0, 2, 3 };
		GpuQuadIndices->SetData(sizeof(quad), quad, BufferUsageType::Static);
	}
}

void VkBufferManager::Deinit()
{
	StreamPool.clear();
	while (!Buffers.empty())
		RemoveBuffer(Buffers.back());
}

bool VkBufferManager::AcquireStream(VkBufferUsageFlags usage, size_t bytes, StreamPair& out)
{
	const uint64_t completed = fb->GetCommands()->GetCompletedFrame();
	size_t best = StreamPool.size();
	for (size_t i = 0; i < StreamPool.size(); ++i)
	{
		const auto& p = StreamPool[i];
		if (p.Usage != usage || p.Frame > completed || p.Buffer->size < bytes) continue;
		if (best == StreamPool.size() || p.Buffer->size < StreamPool[best].Buffer->size) best = i;
	}
	if (best == StreamPool.size()) return false;
	out = std::move(StreamPool[best]);
	StreamPool[best] = std::move(StreamPool.back());
	StreamPool.pop_back();
	return true;
}

void VkBufferManager::RetireStream(StreamPair&& pair)
{
	pair.Frame = fb->GetCommands()->GetCurrentFrame();
	StreamPool.push_back(std::move(pair));
	while (StreamPool.size() > 64)
	{
		auto& old = StreamPool.front();
		fb->GetCommands()->DrawDeleteList->Add(std::move(old.Buffer));
		fb->GetCommands()->TransferDeleteList->Add(std::move(old.Staging));
		StreamPool.erase(StreamPool.begin());
	}
}

void VkBufferManager::AddBuffer(VkHardwareBuffer* buffer)
{
	buffer->it = Buffers.insert(Buffers.end(), buffer);
}

void VkBufferManager::RemoveBuffer(VkHardwareBuffer* buffer)
{
	buffer->Reset();
	buffer->fb = nullptr;
	Buffers.erase(buffer->it);

	for (VkHardwareDataBuffer** knownbuf : { &ViewpointUBO, &LightBufferSSO, &LightNodes, &LightLines, &LightList, &BoneBufferSSO })
	{
		if (buffer == *knownbuf) *knownbuf = nullptr;
	}
}

IVertexBuffer* VkBufferManager::CreateVertexBuffer()
{
	return new VkHardwareVertexBuffer(fb);
}

IIndexBuffer* VkBufferManager::CreateIndexBuffer()
{
	return new VkHardwareIndexBuffer(fb);
}

void VkBufferManager::UploadStaticBakeGeometry(const void* verts, size_t vertBytes, const void* indices, size_t indexBytes)
{
	const size_t vertexCount = vertBytes / sizeof(FFlatVertex);
	if (!StaticGeometryVB)
	{
		StaticGeometryVB.reset(CreateVertexBuffer());
		StaticSurfaceID.reset(CreateVertexBuffer());
		StaticIndicesIB.reset(CreateIndexBuffer());

		static const FVertexBufferAttribute format[] = {
			{ 0, VATTR_VERTEX, VFmt_Float3, (int)myoffsetof(FFlatVertex, x) },
			{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(FFlatVertex, u) },
			{ 0, VATTR_LIGHTMAP, VFmt_Float3, (int)myoffsetof(FFlatVertex, lu) },
			{ 1, VATTR_SURFACEID, VFmt_Float, 0, sizeof(float) },
		};
		StaticGeometryVB->SetFormat(2, 4, sizeof(FFlatVertex), format);

		static const FVertexBufferAttribute idformat[] = { { 0, VATTR_SURFACEID, VFmt_Float, 0 } };
		StaticSurfaceID->SetFormat(1, 1, sizeof(float), idformat);
	}

	StaticGeometryVB->SetData(vertBytes, verts, BufferUsageType::Static);
	std::vector<float> surfaceIDs(vertexCount, 0.0f);
	StaticSurfaceID->SetData(vertexCount * sizeof(float), surfaceIDs.data(), BufferUsageType::Static);
	StaticIndicesIB->SetData(indexBytes, indices, BufferUsageType::Static);
}

IDataBuffer* VkBufferManager::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	auto buffer = new VkHardwareDataBuffer(fb, bindingpoint, ssbo, needsresize);

	switch (bindingpoint)
	{
	case LIGHTBUF_BINDINGPOINT: LightBufferSSO = buffer; break;
	case VIEWPOINT_BINDINGPOINT: ViewpointUBO = buffer; break;
	case LIGHTNODES_BINDINGPOINT: LightNodes = buffer; break;
	case LIGHTLINES_BINDINGPOINT: LightLines = buffer; break;
	case LIGHTLIST_BINDINGPOINT: LightList = buffer; break;
	case BONEBUF_BINDINGPOINT: BoneBufferSSO = buffer; break;
	case POSTPROCESS_BINDINGPOINT: break;
	default: break;
	}

	return buffer;
}

IIndexBuffer* VkBufferManager::GetQuadBatchIndexBuffer()
{
	if (!QuadBatchIndexBuffer)
	{
		uint32_t indices[256 * 6];
		for (uint32_t i = 0; i < 256; ++i)
		{
			const uint32_t quad[] = { i * 4, i * 4 + 1, i * 4 + 2, i * 4, i * 4 + 2, i * 4 + 3 };
			memcpy(indices + i * 6, quad, sizeof(quad));
		}
		QuadBatchIndexBuffer.reset(CreateIndexBuffer());
		QuadBatchIndexBuffer->SetData(sizeof(indices), indices, BufferUsageType::Static);
	}
	return QuadBatchIndexBuffer.get();
}

void VkBufferManager::CreateFanToTrisIndexBuffer()
{
	TArray<uint32_t> data;
	for (int i = 2; i < 1000; i++)
	{
		data.Push(0);
		data.Push(i - 1);
		data.Push(i);
	}

	FanToTrisIndexBuffer.reset(CreateIndexBuffer());
	FanToTrisIndexBuffer->SetData(sizeof(uint32_t) * data.Size(), data.Data(), BufferUsageType::Static);
}

/////////////////////////////////////////////////////////////////////////////

VkStreamBuffer::VkStreamBuffer(VkBufferManager* buffers, size_t structSize, size_t count)
{
	mBlockSize = static_cast<uint32_t>((structSize + screen->uniformblockalignment - 1) / screen->uniformblockalignment * screen->uniformblockalignment);

	UniformBuffer = (VkHardwareDataBuffer*)buffers->CreateDataBuffer(-1, false, false);
	// [Karto] One full-capacity region per frame slot, so a parked frame keeps its uniforms while the next one writes.
	mRegionSize = mBlockSize * uint32_t(count);
	UniformBuffer->SetData(size_t(mRegionSize) * 2, nullptr, BufferUsageType::Persistent);
}

VkStreamBuffer::~VkStreamBuffer()
{
	delete UniformBuffer;
}

uint32_t VkStreamBuffer::NextStreamDataBlock()
{
	if (mRegionPending)
	{
		mRegionPending = false;
		mRegionStart = uint32_t(std::max(screen->mPipelineFrameSlot, 0) % 2) * mRegionSize;
		mStreamDataOffset = mRegionStart;
	}
	mStreamDataOffset += mBlockSize;
	if (mStreamDataOffset + (size_t)mBlockSize >= size_t(mRegionStart) + mRegionSize)
	{
		mStreamDataOffset = mRegionStart;
		return 0xffffffff;
	}
	return mStreamDataOffset;
}
