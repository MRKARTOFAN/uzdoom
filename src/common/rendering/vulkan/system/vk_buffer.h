/*
** vk_buffer.h
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

#pragma once

#include "zvulkan/vulkanobjects.h"
#include <list>

class VulkanRenderDevice;
class VkHardwareBuffer;
class VkHardwareDataBuffer;
class VkStreamBuffer;
class IIndexBuffer;
class IVertexBuffer;
class IDataBuffer;

class VkBufferManager
{
public:
	VkBufferManager(VulkanRenderDevice* fb);
	~VkBufferManager();

	void Init();
	void Deinit();

	IVertexBuffer* CreateVertexBuffer();
	IIndexBuffer* CreateIndexBuffer();
	IDataBuffer* CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize);

	void AddBuffer(VkHardwareBuffer* buffer);
	void RemoveBuffer(VkHardwareBuffer* buffer);

	struct StreamPair
	{
		std::unique_ptr<VulkanBuffer> Buffer, Staging;
		VkBufferUsageFlags Usage = 0;
		uint64_t Frame = 0;
	};
	bool AcquireStream(VkBufferUsageFlags usage, size_t bytes, StreamPair& out);
	void RetireStream(StreamPair&& pair);

	VkHardwareDataBuffer* ViewpointUBO = nullptr;
	VkHardwareDataBuffer* LightBufferSSO = nullptr;
	VkHardwareDataBuffer* LightNodes = nullptr;
	VkHardwareDataBuffer* LightLines = nullptr;
	VkHardwareDataBuffer* LightList = nullptr;
	VkHardwareDataBuffer* BoneBufferSSO = nullptr;

	std::unique_ptr<VkStreamBuffer> MatrixBuffer;
	std::unique_ptr<VkStreamBuffer> StreamBuffer;

	std::unique_ptr<IIndexBuffer> FanToTrisIndexBuffer;
	IIndexBuffer* GetQuadBatchIndexBuffer();
	std::unique_ptr<IDataBuffer> RetainedParameters;
	std::unique_ptr<IIndexBuffer> RetainedIndices;
	std::unique_ptr<IDataBuffer> SectorAppearance;
	std::unique_ptr<IDataBuffer> Coverage;
	std::unique_ptr<IDataBuffer> StaticGeometry;
	std::unique_ptr<IDataBuffer> StaticIndices;
	std::unique_ptr<IDataBuffer> PrimitiveTable;

	std::unique_ptr<IDataBuffer> PrimitiveGrid;
	std::unique_ptr<IDataBuffer> PrimitiveRefs;
	std::unique_ptr<IDataBuffer> GpuCullVisible;
	std::unique_ptr<IDataBuffer> GpuCullArgs;
	std::unique_ptr<IDataBuffer> GpuCullCommands;
	std::unique_ptr<IDataBuffer> GpuCullDrawMaterials;
	std::unique_ptr<IDataBuffer> GpuCullAdmission;
	std::unique_ptr<IDataBuffer> GpuWorldAppearance;
	std::unique_ptr<IDataBuffer> GpuResidentAppearance;
	std::unique_ptr<IDataBuffer> GpuResidentReferences;
	std::unique_ptr<IDataBuffer> PrimitiveBounds;
	std::unique_ptr<IDataBuffer> PrimitiveMaterials;
	std::unique_ptr<IIndexBuffer> GpuQuadIndices;

	std::unique_ptr<IVertexBuffer> StaticGeometryVB;
	std::unique_ptr<IVertexBuffer> StaticSurfaceID;
	std::unique_ptr<IIndexBuffer> StaticIndicesIB;
	void UploadStaticBakeGeometry(const void* verts, size_t vertBytes, const void* indices, size_t indexBytes);

private:
	std::unique_ptr<IIndexBuffer> QuadBatchIndexBuffer;
	void CreateFanToTrisIndexBuffer();

	VulkanRenderDevice* fb = nullptr;

	std::list<VkHardwareBuffer*> Buffers;
	std::vector<StreamPair> StreamPool;
};

class VkStreamBuffer
{
public:
	VkStreamBuffer(VkBufferManager* buffers, size_t structSize, size_t count);
	~VkStreamBuffer();

	uint32_t NextStreamDataBlock();
	// [Karto] The region is chosen at the first block after a reset: EndFrame resets before the frame slot advances.
	void Reset() { mRegionPending = true; }

	VkHardwareDataBuffer* UniformBuffer = nullptr;

private:
	uint32_t mBlockSize = 0;
	uint32_t mStreamDataOffset = 0;
	uint32_t mRegionSize = 0, mRegionStart = 0;
	bool mRegionPending = true;
};
