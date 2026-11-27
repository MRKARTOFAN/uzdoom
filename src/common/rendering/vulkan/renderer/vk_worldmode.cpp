// [Karto] Replacement world mode backend: captures native draws into exact shading rows at bake time and replays
// the resident world with indirect draws.

#include "vk_renderstate.h"
#include "c_cvars.h"

EXTERN_CVAR(Bool, r_worldmode_gpuvis_inpass)
#include "zvulkan/vulkanbuilders.h"
#include "i_time.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/system/vk_buffer.h"
#include "vulkan/system/vk_hwbuffer.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_descriptorset.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "flatvertices.h"
#include "cmdlib.h"
#include "printf.h"
#include "c_dispatch.h"
#include <algorithm>
#include <map>
#include <cstring>

struct VkRenderState::WorldModeResources
{
	// [Karto] Commands are rewritten every frame while earlier frames may still read them; ring slots avoid a wait.
	static constexpr uint32_t Ring = 4;
	std::unique_ptr<VkHardwareVertexBuffer> Vertices;
	std::unique_ptr<VkHardwareVertexBuffer> SurfaceIDs;
	std::unique_ptr<VkHardwareIndexBuffer> Indices;
	std::unique_ptr<VkHardwareDataBuffer> Rows;
	std::unique_ptr<VkHardwareDataBuffer> Commands;
	std::unique_ptr<VkHardwareDataBuffer> SurfaceRows;
	std::unique_ptr<VkHardwareDataBuffer> SurfaceParams, Params, PlaneMatrices, PlaneHeights, VertexRefs;
	std::unique_ptr<VkHardwareDataBuffer> Visibility;
	std::unique_ptr<VkHardwareDataBuffer> SurfaceLights;
	std::unique_ptr<VkHardwareDataBuffer> WorldLights;
	static constexpr int ReadbackCount = 2;
	std::unique_ptr<VulkanBuffer> Readback[ReadbackCount];
	const uint32_t* ReadbackData[ReadbackCount] = {};
	uint32_t ReadbackStamp[ReadbackCount] = {};
	uint64_t ReadbackFrame[ReadbackCount] = {};
	std::unique_ptr<VulkanDescriptorSet> Set;
	std::vector<uint32_t> FirstIndex, IndexCount;
	uint32_t Clusters = 0, OpaqueClusters = 0, RingSlot = 0;
	// [Karto] Pool clusters sit after the baked ones in every ring slot, so a slot's stride covers their capacity.
	std::vector<uint32_t> PoolFirst, PoolCount;
	uint32_t PoolOpaque = 0, PoolTotal = 0, PoolCapacity = 0, CommandStride = 0;
	size_t Surfaces = 0;
	uint32_t NextStamp = 0, DrawnStamp = 0;
	uint32_t InPassStamp = 0;
	uint64_t InPassFrame = UINT64_MAX;
	uint64_t TextureEpoch = 0;
	bool Ready = false, Visible = false;
	uint64_t Draws = 0, StaleSkips = 0, Frames = 0, VisibilityDraws = 0;
	uint64_t DrawnFrame = UINT64_MAX;	// cull frame whose opaque world was actually drawn
	size_t ResidentBytes = 0;

	// [Karto] At device shutdown the resources are destroyed directly, not through ReleaseWorldMode: unmap the readbacks so
	// VMA does not free a mapped allocation.
	~WorldModeResources()
	{
		for (int i = 0; i < ReadbackCount; ++i)
			if (Readback[i] && ReadbackData[i]) Readback[i]->Unmap();
	}
};

static uint64_t WorldModeWorldDraws = 0, WorldModeVisibilityDraws = 0, WorldModeSkipNotReady = 0, WorldModeSkipNoCull = 0,
	WorldModeSkipStale = 0, WorldModeSkipVisibilityNoWorld = 0;
static uint64_t WorldModeCopyRewrites = 0, WorldModeCopyFlushes = 0;

bool VkRenderState::BeginWorldCapture(FWorldCaptureSink* sink)
{
	auto descriptors = fb->GetDescriptorSetManager();
	descriptors->TryCreateBindlessTextureSet();
	if (!sink || !descriptors->IsBindlessEnabled() || !descriptors->GetGpuWorldTextureSet()) return false;
	if (!mWorldMode) mWorldMode = std::make_shared<WorldModeResources>();
	auto& res = *mWorldMode;
	if (!res.Vertices)
	{
		// [Karto] Rows record useVertexData for the buffer they replay from, so the format must exist before capture.
		res.Vertices = std::make_unique<VkHardwareVertexBuffer>(fb);
		res.SurfaceIDs = std::make_unique<VkHardwareVertexBuffer>(fb);
		static const FVertexBufferAttribute format[] = {
			{ 0, VATTR_VERTEX, VFmt_Float3, (int)myoffsetof(FFlatVertex, x) },
			{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(FFlatVertex, u) },
			{ 0, VATTR_LIGHTMAP, VFmt_Float3, (int)myoffsetof(FFlatVertex, lu) },
			{ 1, VATTR_SURFACEID, VFmt_Float, 0, sizeof(float) },
		};
		res.Vertices->SetFormat(2, 4, sizeof(FFlatVertex), format);
		static const FVertexBufferAttribute idformat[] = { { 0, VATTR_SURFACEID, VFmt_Float, 0 } };
		res.SurfaceIDs->SetFormat(1, 1, sizeof(float), idformat);
	}
	mWorldCapture = sink;
	screen->mVertexData->BeginCaptureScratch();
	return true;
}

void VkRenderState::EndWorldCapture()
{
	if (mWorldCapture) screen->mVertexData->EndCaptureScratch();
	mWorldCapture = nullptr;
}

bool VkRenderState::CaptureWorldDraw(int dt, int index, int count, bool indexed)
{
	// [Karto] Every intercepted draw is swallowed: a bake frame must never submit the world it is capturing.
	auto sink = mWorldCapture;
	auto vertices = screen->mVertexData;
	if (!mMaterial.mMaterial || !mTextureEnabled || mModelMatrixEnabled || mBoneIndexBase >= 0 || mSpecialEffect != EFF_NONE)
		sink->Refuse(FWorldCaptureSink::RefuseState);
	else if (indexed ? dt != DT_Triangles : dt != DT_TriangleFan)
		sink->Refuse(FWorldCaptureSink::RefusePrimitive);
	else if (indexed ? mIndexBuffer != vertices->mIndexBuffer : mVertexBuffer != vertices->mVertexBuffer)
		sink->Refuse(FWorldCaptureSink::RefuseVertexSource);
	else
	{
		uint32_t textures[4];
		if (!static_cast<VkMaterial*>(mMaterial.mMaterial)->GetGpuWorldTextures(mMaterial, textures))
		{
			sink->Refuse(FWorldCaptureSink::RefuseMaterial);
			return true;
		}
		auto savedVB = mVertexBuffer;
		const auto savedStream = mStreamData;
		mVertexBuffer = mWorldMode->Vertices.get();
		GpuWorldSurfaceData row = {};
		FillGpuWorldRow(row, textures);
		mVertexBuffer = savedVB;
		mStreamData = savedStream;
		// [Karto] Dynamic lights are not carried into baked rows; a baked light index would point into a stale per-frame buffer.
		row.Surface.LightIndex = -1;
		FWorldCaptureDraw draw;
		draw.Type = dt;
		draw.Index = index;
		draw.Count = count;
		draw.Indexed = indexed;
		draw.AlphaTest = mAlphaThreshold >= 0.f;
		draw.Row = &row;
		draw.RowBytes = sizeof(row);
		draw.ScaleFlags = mMaterial.mMaterial->GetScaleFlags();
		draw.ClampMode = mMaterial.mClampMode;
		draw.Translation = mMaterial.mTranslation;
		draw.TextureIDOffset = offsetof(GpuWorldSurfaceData, TextureIDs);
		sink->Capture(draw);
	}
	return true;
}

// [Karto] Shared by the shaded world draw and the visibility replay: identical inputs make the replay's depth match the first
// pass.
void VkRenderState::IssueWorldModeDraws(uint32_t first, uint32_t count, int variant, int dataIndex)
{
	auto& res = *mWorldMode;
	if (!count) return;
	EndRetainedWallRun(); FlushQuadBatch();
	auto vertexBuffer = mVertexBuffer; auto secondary = mSecondaryVertexBuffer; auto indexBuffer = mIndexBuffer;
	const int offsets[2] = { mVertexOffsets[0], mVertexOffsets[1] };
	SetVertexBuffer(res.Vertices.get(), 0, 0, res.SurfaceIDs.get());
	SetIndexBuffer(res.Indices.get());
	mGpuOwnedWorldDrawing = true;
	ApplyStreamData();
	ApplyGpuIndexedSlot(0, variant, res.Set.get());
	mGpuWallBatchCount = 0;
	PushConstants push = {};
	push.uDataIndex = dataIndex;
	mCommandBuffer->pushConstants(fb->GetRenderPassManager()->GetPipelineLayout(mPipelineKey.NumTextureLayers, true),
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	const uint32_t limit = std::max(1u, fb->device->PhysicalDevice.Properties.Properties.limits.maxDrawIndirectCount);
	const size_t base = (size_t(res.RingSlot) * res.CommandStride + first) * sizeof(VkDrawIndexedIndirectCommand);
	for (uint32_t done = 0; done < count;)
	{
		const uint32_t draws = std::min(limit, count - done);
		mCommandBuffer->drawIndexedIndirect(res.Commands->mBuffer->buffer, base + size_t(done) * sizeof(VkDrawIndexedIndirectCommand),
			draws, sizeof(VkDrawIndexedIndirectCommand));
		++res.Draws;
		done += draws;
	}
	mGpuOwnedWorldDrawing = false;
	// [Karto] The world draw bound its own sets and buffers; cached native bindings no longer describe the command buffer.
	mLastMaterialSet = nullptr;
	mLastMaterialLayers = mLastBufferLayers = -1;
	mLastMatricesOffset = mLastStreamDataOffset = 0xffffffff;
	mMaterial.mChanged = true;
	mPushConstantsValid = false;
	mNeedApply = true;
	SetVertexBuffer(vertexBuffer, offsets[0], offsets[1], secondary);
	SetIndexBuffer(indexBuffer);
}

bool VkRenderState::DrawWorldModeVisibility()
{
	if (!mWorldMode || !mWorldMode->Ready || !mWorldMode->Visible || !mWorldMode->Visibility) return false;
	// [Karto] Without this frame's world depth every in-frustum surface would pass LEqual and pollute the automap flags.
	if (mWorldMode->DrawnFrame != mWorldMode->Frames) { ++WorldModeSkipVisibilityNoWorld; return false; }
	auto& res = *mWorldMode;
	if (!fb->device->EnabledFeatures.Features.fragmentStoresAndAtomics) return false;
	if (res.InPassStamp != 0 && res.InPassFrame == res.Frames)
	{
		const uint32_t stamp = res.InPassStamp;
		res.InPassStamp = 0;
		IssueWorldModeDraws(res.OpaqueClusters, res.Clusters - res.OpaqueClusters, 4, int(stamp));
		IssueWorldModeDraws(res.Clusters + res.PoolOpaque, res.PoolTotal - res.PoolOpaque, 4, int(stamp));
		res.DrawnStamp = stamp;
		++res.VisibilityDraws;
		++WorldModeVisibilityDraws;
		return true;
	}
	if (++res.NextStamp == 0) res.NextStamp = 1;
	IssueWorldModeDraws(0, res.Clusters + res.PoolTotal, 4, int(res.NextStamp));
	res.DrawnStamp = res.NextStamp;
	++res.VisibilityDraws;
	++WorldModeVisibilityDraws;
	return true;
}

void VkRenderState::RecordWorldModeReadback()
{
	if (!mWorldMode || !mWorldMode->DrawnStamp || !mWorldMode->Readback[0]) return;
	auto& res = *mWorldMode;
	const uint64_t frame = fb->GetCommands()->GetCurrentFrame();
	const int slot = int(frame % WorldModeResources::ReadbackCount);
	auto commands = fb->GetCommands()->GetDrawCommands();
	VkMemoryBarrier before{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	commands->pipelineBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
	commands->copyBuffer(res.Visibility->mBuffer.get(), res.Readback[slot].get(), 0, 0, res.Surfaces * sizeof(uint32_t));
	VkMemoryBarrier after{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	after.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
	commands->pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
	res.ReadbackStamp[slot] = res.DrawnStamp;
	res.ReadbackFrame[slot] = frame;
	res.DrawnStamp = 0;
}

bool VkRenderState::GetWorldModeVisibility(const uint32_t*& stamps, size_t& count, uint32_t& stamp)
{
	if (!mWorldMode || !mWorldMode->Ready) return false;
	auto& res = *mWorldMode;
	const uint64_t completed = fb->GetCommands()->GetCompletedFrame();
	int best = -1;
	for (int i = 0; i < WorldModeResources::ReadbackCount; ++i)
	{
		if (res.ReadbackStamp[i] && res.ReadbackData[i] && res.ReadbackFrame[i] <= completed && (best < 0 || res.ReadbackFrame[i] > res.ReadbackFrame[best]))
			best = i;
	}
	if (best < 0) return false;
	stamps = res.ReadbackData[best];
	count = res.Surfaces;
	stamp = res.ReadbackStamp[best];
	return true;
}

// [Karto] The readback buffers stay mapped for their whole life; VMA asserts when freeing a mapped allocation, so unmap
// before handing it to the delete list.
static void RetireWorldModeReadback(VulkanRenderDevice* fb, std::unique_ptr<VulkanBuffer>& readback, const uint32_t*& data)
{
	if (!readback) return;
	if (data) readback->Unmap();
	data = nullptr;
	fb->GetCommands()->DrawDeleteList->Add(std::move(readback));
}

bool VkRenderState::UploadWorldMode(const FWorldModeUpload& data)
{
	if (!mWorldMode || !mWorldMode->Vertices || data.RowBytes != sizeof(GpuWorldSurfaceData) || !data.ClusterCount ||
		!data.SurfaceRows || !data.SurfaceCount || !data.SurfaceParams || !data.Params || !data.PlaneMatrices || !data.PlaneCount || !data.PlaneHeights || !data.VertexRefs) return false;
	auto& res = *mWorldMode;
	auto descriptors = fb->GetDescriptorSetManager();
	const auto& limits = fb->device->PhysicalDevice.Properties.Properties.limits;
	const size_t rowBytes = data.RowCount * data.RowBytes;
	if (rowBytes > limits.maxStorageBufferRange)
	{
		Printf("World mode: %zu row bytes exceed maxStorageBufferRange %u\n", rowBytes, limits.maxStorageBufferRange);
		return false;
	}
	res.Vertices->SetData(data.VertexCount * sizeof(FFlatVertex), data.Vertices, BufferUsageType::Static);
	res.SurfaceIDs->SetData(data.VertexCount * sizeof(float), data.SurfaceIDs, BufferUsageType::Static);
	if (!res.Indices) res.Indices = std::make_unique<VkHardwareIndexBuffer>(fb);
	res.Indices->SetData(data.IndexCount * sizeof(uint32_t), data.Indices, BufferUsageType::Static);
	if (!res.Rows) res.Rows = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
	res.Rows->SetData(rowBytes, data.Rows, BufferUsageType::Static);
	if (!res.Commands)
	{
		res.Commands = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
		res.Commands->mBufferType |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
	}
	res.CommandStride = data.ClusterCount + data.PoolClusterCapacity;
	res.Commands->SetData(size_t(res.CommandStride) * sizeof(VkDrawIndexedIndirectCommand) * WorldModeResources::Ring, nullptr, BufferUsageType::Persistent);
	if (!res.SurfaceRows) res.SurfaceRows = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
	res.SurfaceRows->SetData(data.SurfaceCount * sizeof(uint32_t), data.SurfaceRows, BufferUsageType::Static);
	if (!res.SurfaceParams) res.SurfaceParams = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
	res.SurfaceParams->SetData(data.SurfaceCount * sizeof(uint32_t), data.SurfaceParams, BufferUsageType::Static);
	if (!res.Params) res.Params = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
	const float noParams[4] = {};
	res.Params->SetData(std::max<size_t>(data.ParamCount, 1) * 4 * sizeof(float), data.ParamCount ? data.Params : noParams, BufferUsageType::Static);
	if (!res.PlaneMatrices) res.PlaneMatrices = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
	res.PlaneMatrices->SetData(data.PlaneCount * 2 * 16 * sizeof(float), data.PlaneMatrices, BufferUsageType::Static);
	if (!res.PlaneHeights) res.PlaneHeights = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
	res.PlaneHeights->SetData(data.PlaneCount * 12 * sizeof(float), data.PlaneHeights, BufferUsageType::Static);
	if (!res.VertexRefs) res.VertexRefs = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
	res.VertexRefs->SetData(data.VertexCount * 4 * sizeof(uint32_t), data.VertexRefs, BufferUsageType::Static);
	if (!res.Visibility)
	{
		res.Visibility = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
		res.Visibility->mBufferType |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	}
	// [Karto] Zeroed stamps: stamp 0 is never issued, so an untouched surface can never read as visible.
	const std::vector<uint32_t> zeros(data.SurfaceCount, 0);
	res.Visibility->SetData(data.SurfaceCount * sizeof(uint32_t), zeros.data(), BufferUsageType::Static);
	if (!res.SurfaceLights) res.SurfaceLights = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
	const std::vector<int32_t> unlit(data.SurfaceCount, -1);
	res.SurfaceLights->SetData(data.SurfaceCount * sizeof(int32_t), unlit.data(), BufferUsageType::Static);
	if (!res.WorldLights)
	{
		res.WorldLights = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
		const std::vector<float> none(size_t(WorldModeLightCapacity) * 4, 0.f);
		res.WorldLights->SetData(none.size() * sizeof(float), none.data(), BufferUsageType::Static);
	}
	for (int i = 0; i < WorldModeResources::ReadbackCount; ++i)
	{
		RetireWorldModeReadback(fb, res.Readback[i], res.ReadbackData[i]);
		res.Readback[i] = BufferBuilder()
			.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
			.Size(data.SurfaceCount * sizeof(uint32_t))
			.DebugName("WorldModeVisibilityReadback")
			.Create(fb->device.get());
		res.ReadbackData[i] = static_cast<const uint32_t*>(res.Readback[i]->Map(0, data.SurfaceCount * sizeof(uint32_t)));
		res.ReadbackStamp[i] = 0;
		res.ReadbackFrame[i] = 0;
	}
	res.Surfaces = data.SurfaceCount;
	res.DrawnStamp = 0;
	if (res.Set) fb->GetCommands()->DrawDeleteList->Add(std::move(res.Set));
	res.Set = descriptors->AllocateGpuWorldSet(res.Rows->mBuffer.get(), res.SurfaceRows->mBuffer.get(), res.Visibility->mBuffer.get(),
		res.SurfaceParams->mBuffer.get(), res.Params->mBuffer.get(), res.PlaneMatrices->mBuffer.get(), res.PlaneHeights->mBuffer.get(),
		res.VertexRefs->mBuffer.get(), false, res.SurfaceLights->mBuffer.get(), res.WorldLights->mBuffer.get());
	res.FirstIndex.assign(data.ClusterFirstIndex, data.ClusterFirstIndex + data.ClusterCount);
	res.IndexCount.assign(data.ClusterIndexCount, data.ClusterIndexCount + data.ClusterCount);
	res.Clusters = data.ClusterCount;
	res.OpaqueClusters = data.OpaqueClusters;
	res.PoolCapacity = data.PoolClusterCapacity;
	res.PoolFirst.clear();
	res.PoolCount.clear();
	res.PoolOpaque = res.PoolTotal = 0;
	res.TextureEpoch = descriptors->GetGpuWorldTextureEpoch();
	res.ResidentBytes = data.VertexCount * (sizeof(FFlatVertex) + sizeof(float)) + data.IndexCount * sizeof(uint32_t) + rowBytes +
		data.SurfaceCount * sizeof(uint32_t) * 2;
	res.Ready = true;
	res.Visible = false;
	return true;
}

void VkRenderState::ReleaseWorldMode()
{
	if (!mWorldMode) return;
	if (mWorldMode->Set) fb->GetCommands()->DrawDeleteList->Add(std::move(mWorldMode->Set));
	for (int i = 0; i < WorldModeResources::ReadbackCount; ++i)
		RetireWorldModeReadback(fb, mWorldMode->Readback[i], mWorldMode->ReadbackData[i]);
	mWorldMode.reset();
}

bool VkRenderState::SetWorldModeVisibility(const uint8_t* visible, uint32_t count)
{
	if (!mWorldMode || !mWorldMode->Ready || count != mWorldMode->Clusters + mWorldMode->PoolTotal) return false;
	auto& res = *mWorldMode;
	res.RingSlot = uint32_t(res.Frames++ % WorldModeResources::Ring);
	auto commands = static_cast<VkDrawIndexedIndirectCommand*>(res.Commands->Memory()) + size_t(res.RingSlot) * res.CommandStride;
	for (uint32_t i = 0; i < res.Clusters; ++i)
		commands[i] = { res.IndexCount[i], visible[i] ? 1u : 0u, res.FirstIndex[i], 0, 0 };
	for (uint32_t i = 0; i < res.PoolTotal; ++i)
		commands[res.Clusters + i] = { res.PoolCount[i], visible[res.Clusters + i] ? 1u : 0u, res.PoolFirst[i], 0, 0 };
	res.Visible = true;
	return true;
}

bool VkRenderState::SetWorldModePoolClusters(const uint32_t* firstIndex, const uint32_t* indexCount, uint32_t opaque, uint32_t total)
{
	if (!mWorldMode || !mWorldMode->Ready || total > mWorldMode->PoolCapacity || opaque > total) return false;
	auto& res = *mWorldMode;
	res.PoolFirst.assign(firstIndex, firstIndex + total);
	res.PoolCount.assign(indexCount, indexCount + total);
	res.PoolOpaque = opaque;
	res.PoolTotal = total;
	return true;
}

bool VkRenderState::WriteWorldMode(const FWorldModeWrite* writes, size_t count)
{
	if (!mWorldMode || !mWorldMode->Ready || !count) return false;
	auto& res = *mWorldMode;
	VulkanBuffer* targets[FWorldModeWrite::TargetCount] = { res.Vertices->mBuffer.get(), res.SurfaceIDs->mBuffer.get(),
		res.Indices->mBuffer.get(), res.Rows->mBuffer.get(), res.SurfaceParams->mBuffer.get(), res.Params->mBuffer.get(),
		res.PlaneMatrices->mBuffer.get(), res.PlaneHeights->mBuffer.get(),
		res.VertexRefs->mBuffer.get(), res.SurfaceLights->mBuffer.get(), res.WorldLights->mBuffer.get() };
	size_t total = 0;
	for (size_t i = 0; i < count; ++i)
	{
		const auto& w = writes[i];
		if (w.Target < 0 || w.Target >= FWorldModeWrite::TargetCount || !w.Bytes || w.Offset + w.Bytes > targets[w.Target]->size) return false;
		total += w.Bytes;
	}
	// [Karto] A fresh staging buffer per frame: the parked frame may still read the device buffers, and only a queue-ordered
	// copy keeps both frames consistent.
	auto staging = BufferBuilder()
		.Usage(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
		.Size(total)
		.DebugName("WorldModeChangeStaging")
		.Create(fb->device.get());
	auto bytes = static_cast<uint8_t*>(staging->Map(0, total));
	auto commands = fb->GetCommands()->GetTransferCommands();
	// [Karto] The first scope covers earlier submissions on the queue, so the copies cannot overwrite data a previous frame
	// still reads.
	VkMemoryBarrier before{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	before.srcAccessMask = 0;
	before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	const VkPipelineStageFlags readers = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
		VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	commands->pipelineBarrier(readers | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
	// [Karto] One copy command per target with all its regions. Regions of one copy must not overlap (VUID-vkCmdCopyBuffer-
	// pRegions-12482): their order is undefined. A rewrite of the same range replaces the earlier region's staging
	// bytes; a partial overlap flushes that target's copy behind a write-after-write barrier.
	std::vector<VkBufferCopy> regions[FWorldModeWrite::TargetCount];
	std::map<VkDeviceSize, size_t> ranges[FWorldModeWrite::TargetCount];
	auto flush = [&](int t)
	{
		commands->copyBuffer(staging->buffer, targets[t]->buffer, uint32_t(regions[t].size()), regions[t].data());
		VkMemoryBarrier waw{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		waw.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		waw.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		commands->pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &waw, 0, nullptr, 0, nullptr);
		regions[t].clear();
		ranges[t].clear();
		++WorldModeCopyFlushes;
	};
	size_t cursor = 0;
	for (size_t i = 0; i < count; ++i)
	{
		const auto& w = writes[i];
		auto& map = ranges[w.Target];
		auto next = map.lower_bound(w.Offset);
		if (next != map.end() && next->first == w.Offset && regions[w.Target][next->second].size == w.Bytes)
		{
			memcpy(bytes + regions[w.Target][next->second].srcOffset, w.Data, w.Bytes);
			++WorldModeCopyRewrites;
			continue;
		}
		const bool overlapsNext = next != map.end() && next->first < w.Offset + w.Bytes;
		const bool overlapsPrev = next != map.begin() && [&] { const auto& r = regions[w.Target][std::prev(next)->second]; return r.dstOffset + r.size > w.Offset; }();
		if (overlapsNext || overlapsPrev) flush(w.Target);
		memcpy(bytes + cursor, w.Data, w.Bytes);
		ranges[w.Target][w.Offset] = regions[w.Target].size();
		regions[w.Target].push_back({ cursor, w.Offset, w.Bytes });
		cursor += w.Bytes;
	}
	for (int t = 0; t < FWorldModeWrite::TargetCount; ++t)
		if (!regions[t].empty()) commands->copyBuffer(staging->buffer, targets[t]->buffer, uint32_t(regions[t].size()), regions[t].data());
	staging->Unmap();
	VkMemoryBarrier after{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	after.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
	commands->pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, readers, 0, 1, &after, 0, nullptr, 0, nullptr);
	fb->GetCommands()->TransferDeleteList->Add(std::move(staging));
	return true;
}

bool VkRenderState::GetWorldModeTextureIDs(const FMaterialState& state, uint32_t ids[4])
{
	return state.mMaterial && static_cast<VkMaterial*>(state.mMaterial)->GetGpuWorldTextures(state, ids);
}

uint64_t VkRenderState::GetGpuTextureEpoch()
{
	return fb->GetDescriptorSetManager()->GetGpuWorldTextureEpoch();
}

bool VkRenderState::SupportsSpriteInstances()
{
	auto descriptors = fb->GetDescriptorSetManager();
	descriptors->TryCreateBindlessTextureSet();
	return descriptors->IsBindlessEnabled() && descriptors->GetGpuWorldTextureSet() != nullptr;
}

uint32_t VkRenderState::AddSpriteRow()
{
	if (!mMaterial.mMaterial) return ~0u;
	GpuWorldSurfaceData row;
	const uint32_t none[4] = {};
	FillGpuWorldRow(row, none);
	mSpriteRows.push_back(row);
	return uint32_t(mSpriteRows.size() - 1);
}

bool VkRenderState::DrawSpriteInstances(const uint32_t* instances, uint32_t instanceCount, uint32_t instanceWords, uint64_t instanceSerial,
	const uint32_t* visible, uint32_t count, const uint32_t* frames, uint32_t frameWords, uint64_t frameVersion, const float* params,
	uint32_t paramVec4s)
{
	auto descriptors = fb->GetDescriptorSetManager();
	descriptors->TryCreateBindlessTextureSet();
	if (!count || mSpriteRows.empty() || !descriptors->IsBindlessEnabled() || !descriptors->GetGpuWorldTextureSet())
	{
		mSpriteRows.clear();
		return false;
	}
	auto upload = [&](std::unique_ptr<VkHardwareDataBuffer>& buffer, const void* data, size_t bytes)
	{
		if (!buffer) buffer = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
		buffer->SetData(bytes, data, BufferUsageType::Stream);
	};
	upload(mSpriteRowBuffer, mSpriteRows.data(), mSpriteRows.size() * sizeof(GpuWorldSurfaceData));
	if (!mSpriteInstanceBuffer || instanceSerial == 0 || instanceSerial != mSpriteInstanceSerial)
	{
		upload(mSpriteInstanceBuffer, instances, size_t(instanceCount) * instanceWords * sizeof(uint32_t));
		mSpriteInstanceSerial = instanceSerial;
	}
	if (!mSpriteFrameBuffer || frameVersion != mSpriteFrameVersion || frameWords != mSpriteFrameWords)
	{
		upload(mSpriteFrameBuffer, frames, size_t(frameWords) * sizeof(uint32_t));
		mSpriteFrameVersion = frameVersion;
		mSpriteFrameWords = frameWords;
	}
	upload(mSpriteVisibleBuffer, visible, size_t(count) * 4 * sizeof(uint32_t));
	upload(mSpriteParamBuffer, params, size_t(paramVec4s) * 4 * sizeof(float));
	mSpriteRows.clear();
	if (mSpriteSet) fb->GetCommands()->DrawDeleteList->Add(std::move(mSpriteSet));
	// [Karto] Sprite variant bindings: 0 rows, 3 frame table, 4 parameters, 5 instances, 6 visible entries; 7 is this
	// publish's GPU light colours when the light pass made them, else any buffer.
	VulkanBuffer* lightOut = mSpriteLightOut && mSpriteLightSerial == instanceSerial && instanceSerial != 0 ? mSpriteLightOut.get() :
		mSpriteVisibleBuffer->mBuffer.get();
	mSpriteSet = descriptors->AllocateGpuWorldSet(mSpriteRowBuffer->mBuffer.get(), nullptr, nullptr,
		mSpriteFrameBuffer->mBuffer.get(), mSpriteParamBuffer->mBuffer.get(), mSpriteInstanceBuffer->mBuffer.get(), mSpriteVisibleBuffer->mBuffer.get(),
		lightOut, true);
	EndRetainedWallRun();
	FlushQuadBatch();
	ApplyStreamData();
	ApplyGpuIndexedSlot(0, 5, mSpriteSet.get());
	mGpuWallBatchCount = 0;
	PushConstants push = {};
	mCommandBuffer->pushConstants(fb->GetRenderPassManager()->GetPipelineLayout(mPipelineKey.NumTextureLayers, true),
		VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
	if (params[13] != 0.f)
	{
		if (!mSpriteQuadIndices)
		{
			mSpriteQuadIndices = BufferBuilder()
				.Usage(VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU)
				.Size(6 * sizeof(uint32_t))
				.DebugName("GpuSpriteQuadIndices")
				.Create(fb->device.get());
			static const uint32_t quad[6] = { 0, 1, 2, 2, 1, 3 };
			memcpy(mSpriteQuadIndices->Map(0, sizeof(quad)), quad, sizeof(quad));
			mSpriteQuadIndices->Unmap();
		}
		mCommandBuffer->bindIndexBuffer(mSpriteQuadIndices->buffer, 0, VK_INDEX_TYPE_UINT32);
		mCommandBuffer->drawIndexed(6, count, 0, 0, 0);
		mLastIndexBuffer = nullptr;
	}
	else mCommandBuffer->draw(6, count, 0, 0);
	mLastMaterialSet = nullptr;
	mLastMaterialLayers = mLastBufferLayers = -1;
	mLastMatricesOffset = mLastStreamDataOffset = 0xffffffff;
	mMaterial.mChanged = true;
	mPushConstantsValid = false;
	mNeedApply = true;
	return true;
}

void VkRenderState::DrawWorldMode(bool alphaTest)
{
	if (!mWorldMode || !mWorldMode->Ready) { ++WorldModeSkipNotReady; return; }
	auto& res = *mWorldMode;
	if (!res.Visible) { ++WorldModeSkipNoCull; return; }
	if (res.TextureEpoch != fb->GetDescriptorSetManager()->GetGpuWorldTextureEpoch())
	{
		// [Karto] Recycled bindless slots would sample the wrong images; the renderer side rebakes on the next frame.
		++res.StaleSkips;
		++WorldModeSkipStale;
		return;
	}
	const uint32_t first = alphaTest ? res.OpaqueClusters : 0;
	const uint32_t count = alphaTest ? res.Clusters - res.OpaqueClusters : res.OpaqueClusters;
	// [Karto] The opaque pass writes this frame's visibility stamps itself; the replay covers only the alpha-tested clusters,
	// whose discard rules out early depth tests.
	int variant = 3, stamp = 0;
	if (!alphaTest && r_worldmode_gpuvis_inpass && res.Visibility && fb->device->EnabledFeatures.Features.fragmentStoresAndAtomics &&
		fb->device->EnabledFeatures.Features.shaderClipDistance)
	{
		if (++res.NextStamp == 0) res.NextStamp = 1;
		stamp = int(res.NextStamp);
		variant = 6;
		res.InPassStamp = res.NextStamp;
		res.InPassFrame = res.Frames;
	}
	IssueWorldModeDraws(first, count, variant, stamp);
	const uint32_t poolFirst = res.Clusters + (alphaTest ? res.PoolOpaque : 0);
	const uint32_t poolCount = alphaTest ? res.PoolTotal - res.PoolOpaque : res.PoolOpaque;
	IssueWorldModeDraws(poolFirst, poolCount, variant, stamp);
	if (count || poolCount) ++WorldModeWorldDraws;
	if (!alphaTest) res.DrawnFrame = res.Frames;
}

bool VkRenderState::GetWorldModeLightPatch(FWorldModeLightPatch& patch)
{
	ResolveWorldLighting();
	const auto push = MakePushConstants();
	const float light[4] = { push.uLightDist, push.uLightFactor, push.uFogDensity, push.uLightLevel };
	constexpr size_t surface = offsetof(GpuWorldSurfaceData, Surface), stream = surface + offsetof(RetainedSurfaceData, Stream);
	patch.Count = 0;
	size_t at = 0;
	auto add = [&](size_t offset, const void* source, size_t bytes)
	{
		patch.Range[patch.Count] = { uint32_t(offset), uint32_t(bytes) };
		memcpy(patch.Data + at, source, bytes);
		at += bytes;
		++patch.Count;
	};
	// [Karto] Without fog, SetFog leaves distance and factor from whatever was drawn before; keep the baked ones.
	if (push.uFogEnabled > 0) add(surface + offsetof(RetainedSurfaceData, Light), light, sizeof(light));
	else add(surface + offsetof(RetainedSurfaceData, Light) + 2 * sizeof(float), light + 2, 2 * sizeof(float));
	add(surface + offsetof(RetainedSurfaceData, FogEnabled), &push.uFogEnabled, sizeof(push.uFogEnabled));
	add(stream + offsetof(StreamData, uVertexColor), &mStreamData.uVertexColor, sizeof(mStreamData.uVertexColor));
	add(stream + offsetof(StreamData, uDesaturationFactor), &mStreamData.uDesaturationFactor, sizeof(mStreamData.uDesaturationFactor));
	add(stream + offsetof(StreamData, uFogColor), &mStreamData.uFogColor, sizeof(mStreamData.uFogColor));
	return true;
}

uint32_t VkRenderState::PeekWorldModeStamp()
{
	if (!mWorldMode) return 0;
	const uint32_t next = mWorldMode->NextStamp + 1;
	return next ? next : 1;
}

bool VkRenderState::IsWorldModeStale()
{
	return mWorldMode && mWorldMode->Ready && mWorldMode->TextureEpoch != fb->GetDescriptorSetManager()->GetGpuWorldTextureEpoch();
}

CCMD(worldmode_backend_stats)
{
	Printf("WORLDMODEBACKEND api_draws=%llu visibility_draws=%llu skip_not_ready=%llu skip_no_cull=%llu stale_epoch_skips=%llu skip_visibility_no_world=%llu\n",
		(unsigned long long)WorldModeWorldDraws, (unsigned long long)WorldModeVisibilityDraws, (unsigned long long)WorldModeSkipNotReady,
		(unsigned long long)WorldModeSkipNoCull, (unsigned long long)WorldModeSkipStale, (unsigned long long)WorldModeSkipVisibilityNoWorld);
	Printf("WORLDMODECOPY rewrites=%llu flushes=%llu\n", (unsigned long long)WorldModeCopyRewrites, (unsigned long long)WorldModeCopyFlushes);
	if (screen && screen->IsVulkan()) static_cast<VulkanRenderDevice*>(screen)->GetCommands()->PrintFramesInFlightStats();
}

static const char* LightPassSource = R"(#version 450
layout(local_size_x = 64) in;
layout(push_constant) uniform PC { uint Count; uint Words; uint Mode; uint Pad; } pc;
struct GPUNode { vec2 aabb_min; vec2 aabb_max; int left; int right; int line_index; int padding; };
struct GPULine { vec2 pos; vec2 delta; };
layout(std430, set = 0, binding = 0) readonly buffer Instances { uint I[]; };
layout(std430, set = 0, binding = 1) readonly buffer Pack { uint L[]; };
layout(std430, set = 0, binding = 2) readonly buffer Nodes { GPUNode nodes[]; };
layout(std430, set = 0, binding = 3) readonly buffer Lines { GPULine lines[]; };
layout(std430, set = 0, binding = 4) writeonly buffer Lights { vec4 O[]; };

bool overlapRayAABB(vec2 ray_start2d, vec2 ray_end2d, vec2 aabb_min2d, vec2 aabb_max2d)
{
	vec3 ray_start = vec3(ray_start2d, 0.0);
	vec3 ray_end = vec3(ray_end2d, 0.0);
	vec3 aabb_min = vec3(aabb_min2d, -1.0);
	vec3 aabb_max = vec3(aabb_max2d, 1.0);
	vec3 c = (ray_start + ray_end) * 0.5f;
	vec3 w = ray_end - c;
	vec3 h = (aabb_max - aabb_min) * 0.5f;
	c -= (aabb_max + aabb_min) * 0.5f;
	vec3 v = abs(w);
	if (abs(c.x) > v.x + h.x || abs(c.y) > v.y + h.y || abs(c.z) > v.z + h.z) return false;
	if (abs(c.y * w.z - c.z * w.y) > h.y * v.z + h.z * v.y || abs(c.x * w.z - c.z * w.x) > h.x * v.z + h.z * v.x ||
		abs(c.x * w.y - c.y * w.x) > h.x * v.y + h.y * v.x) return false;
	return true;
}

float intersectRayLine(vec2 ray_start, vec2 ray_end, int line_index, vec2 raydelta, float rayd, float raydist2)
{
	const float epsilon = 0.0000001;
	GPULine line = lines[line_index];
	vec2 raynormal = vec2(raydelta.y, -raydelta.x);
	float den = dot(raynormal, line.delta);
	if (abs(den) > epsilon)
	{
		float t_line = (rayd - dot(raynormal, line.pos)) / den;
		if (t_line >= 0.0 && t_line <= 1.0)
		{
			vec2 linehitdelta = line.pos + line.delta * t_line - ray_start;
			float t = dot(raydelta, linehitdelta) / raydist2;
			return t > 0.0 ? t : 1.0;
		}
	}
	return 1.0;
}

float rayTest(vec2 ray_start, vec2 ray_end)
{
	vec2 raydelta = ray_end - ray_start;
	float raydist2 = dot(raydelta, raydelta);
	vec2 raynormal = vec2(raydelta.y, -raydelta.x);
	float rayd = dot(raynormal, ray_start);
	if (raydist2 < 1.0) return 1.0;
	float t = 1.0;
	int stack[32];
	int stack_pos = 1;
	stack[0] = int(L[7]) - 1;
	while (stack_pos > 0)
	{
		int node_index = stack[stack_pos - 1];
		if (!overlapRayAABB(ray_start, ray_end, nodes[node_index].aabb_min, nodes[node_index].aabb_max)) stack_pos--;
		else if (nodes[node_index].line_index != -1)
		{
			t = min(intersectRayLine(ray_start, ray_end, nodes[node_index].line_index, raydelta, rayd, raydist2), t);
			stack_pos--;
		}
		else if (stack_pos == 32) stack_pos--;
		else
		{
			stack[stack_pos - 1] = nodes[node_index].left;
			stack[stack_pos] = nodes[node_index].right;
			stack_pos++;
		}
	}
	return t;
}

float F(uint i) { return uintBitsToFloat(L[i]); }

void main()
{
	uint i = gl_GlobalInvocationID.x;
	if (i >= pc.Count) return;
	uint b = i * pc.Words;
	vec3 o = vec3(0.0);
	bool particle = pc.Mode != 0u;
	if (particle ? (I[b + 9u] & 262144u) == 0u : (I[b + 23u] & 65536u) == 0u)
	{
		uint px = particle ? b : b + 3u, pz = particle ? b + 2u : b + 34u;
		float x = uintBitsToFloat(I[px]), y = uintBitsToFloat(I[px + 1u]), z = uintBitsToFloat(I[pz]);
		int rec = particle ? -1 : int(I[b + 25u]);
		int cx = int(floor(x / 256.0)) - int(L[0]), cy = int(floor(y / 256.0)) - int(L[1]);
		if (cx >= 0 && cy >= 0 && cx < int(L[2]) && cy < int(L[3]))
		{
			uint c = L[4] + uint(cy * int(L[2]) + cx) * 2u;
			uint start = L[c], n = L[c + 1u];
			for (uint k = 0u; k < n; ++k)
			{
				uint l = L[6] + L[start + k] * 16u;
				uint flags = L[l + 7u];
				int owner = int(L[l + 13u]);
				bool own = particle ? (flags & 16u) != 0u : owner == rec;
				if ((flags & 2u) != 0u && own) continue;
				if ((flags & 4u) != 0u && !own) continue;
				vec3 lp = vec3(F(l), F(l + 1u), F(l + 2u));
				vec3 d = vec3(x - lp.x, y - lp.y, z - lp.z);
				float dist = d.x * d.x + d.y * d.y + d.z * d.z;
				float radius = F(l + 3u);
				if (dist < radius)
				{
					// [Karto] The pack holds radius squared for the range test; the falloff is linear in distance, as on the CPU.
					float frac = 1.0 - sqrt(dist / radius);
					if ((flags & 1u) != 0u)
					{
						d *= -1.0 / sqrt(dist);
						float cosDir = d.x * F(l + 8u) + d.y * F(l + 9u) + d.z * F(l + 10u);
						float t = clamp((cosDir - F(l + 11u)) / (F(l + 12u) - F(l + 11u)), 0.0, 1.0);
						frac *= t * t * (3.0 - 2.0 * t);
					}
					if (frac > 0.0 && ((flags & 8u) == 0u || rayTest(lp.xy, vec2(x, y)) >= 1.0))
						o += vec3(F(l + 4u), F(l + 5u), F(l + 6u)) * frac;
				}
			}
		}
	}
	O[i] = vec4(o, 0.0);
}
)";

void VkRenderState::EnsureLightResources()
{
	if (mLightReady || mLightFailed) return;
	try
	{
		auto device = fb->device.get();
		mLightShader = ShaderBuilder()
			.Type(ShaderType::Compute)
			.AddSource("light_pass", LightPassSource)
			.DebugName("LightPass")
			.Create("light_pass", device);
		DescriptorSetLayoutBuilder layout;
		for (int i = 0; i < 5; ++i) layout.AddBinding(i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT);
		mLightSetLayout = layout.DebugName("LightPassSetLayout").Create(device);
		mLightPipelineLayout = PipelineLayoutBuilder()
			.AddSetLayout(mLightSetLayout.get())
			.AddPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(uint32_t))
			.DebugName("LightPassPipelineLayout")
			.Create(device);
		mLightPipeline = ComputePipelineBuilder()
			.Layout(mLightPipelineLayout.get())
			.ComputeShader(mLightShader.get())
			.DebugName("LightPassPipeline")
			.Create(device);
		mLightReady = true;
	}
	catch (const CVulkanError& error)
	{
		mLightFailed = true;
		Printf("GPU lights unavailable: %s\n", error.what());
	}
}

// [Karto] Recorded on the transfer list ahead of every draw of the frame. The output is rewritten in place after a barrier on
// earlier frames' reads, because a new buffer per publish stalled the main thread in VMA.
bool VkRenderState::DispatchLights(VulkanBuffer* items, uint32_t count, uint32_t words, uint32_t mode, const FGpuLightPack& pack,
	std::unique_ptr<VulkanBuffer>& out, std::unique_ptr<VulkanDescriptorPool>& pool, std::unique_ptr<VulkanDescriptorSet>& set,
	VkPipelineStageFlags reader)
{
	EnsureLightResources();
	if (!mLightReady || count == 0 || pack.Count < 8) return false;
	VulkanBuffer* nodes = nullptr;
	VulkanBuffer* lines = nullptr;
	if (pack.Shadows)
	{
		auto* n = static_cast<VkHardwareDataBuffer*>(screen->mShadowMap.mNodesBuffer);
		auto* l = static_cast<VkHardwareDataBuffer*>(screen->mShadowMap.mLinesBuffer);
		if (n == nullptr || l == nullptr || !n->mBuffer || !l->mBuffer) return false;
		nodes = n->mBuffer.get();
		lines = l->mBuffer.get();
	}
	try
	{
		auto commands = fb->GetCommands();
		if (!mLightPack || mLightPackSerial != pack.Serial)
		{
			if (!mLightPack) mLightPack = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
			mLightPack->SetData(size_t(pack.Count) * sizeof(uint32_t), pack.Words, BufferUsageType::Stream);
			mLightPackSerial = pack.Serial;
		}
		const size_t bytes = size_t(count) * 4 * sizeof(float);
		const bool reused = out && out->size >= bytes;
		if (!reused)
		{
			if (out) commands->DrawDeleteList->Add(std::move(out));
			out = BufferBuilder()
				.Usage(VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY)
				.Size((bytes + bytes / 4 + 0xffff) & ~size_t(0xffff))
				.DebugName("GpuLights")
				.Create(fb->device.get());
		}
		if (!pack.Shadows) nodes = lines = mLightPack->mBuffer.get();
		if (set) commands->DrawDeleteList->Add(std::move(set));
		if (pool) commands->DrawDeleteList->Add(std::move(pool));
		pool = DescriptorPoolBuilder().AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5).MaxSets(1).Create(fb->device.get());
		set = pool->allocate(mLightSetLayout.get());
		WriteDescriptors()
			.AddBuffer(set.get(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, items)
			.AddBuffer(set.get(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mLightPack->mBuffer.get())
			.AddBuffer(set.get(), 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nodes)
			.AddBuffer(set.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, lines)
			.AddBuffer(set.get(), 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out.get())
			.Execute(fb->device.get());
		const uint32_t pc[4] = { count, words, mode, 0 };
		auto cmd = commands->GetTransferCommands();
		if (reused)
		{
			PipelineBarrier()
				.AddBuffer(out.get(), VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT)
				.Execute(cmd, reader, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		}
		cmd->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, mLightPipeline.get());
		cmd->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, mLightPipelineLayout.get(), 0, set.get());
		cmd->pushConstants(mLightPipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
		cmd->dispatch((count + 63) / 64, 1, 1);
		PipelineBarrier()
			.AddBuffer(out.get(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT)
			.Execute(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, reader);
		return true;
	}
	catch (const CVulkanError& error)
	{
		Printf("GPU lights failed: %s\n", error.what());
		return false;
	}
}

bool VkRenderState::ComputeSpriteLights(const uint32_t* instances, uint32_t instanceCount, uint32_t instanceWords, uint64_t instanceSerial,
	const FGpuLightPack& pack)
{
	mSpriteLightSerial = 0;
	if (instanceCount == 0) return false;
	try
	{
		if (!mSpriteInstanceBuffer || instanceSerial != mSpriteInstanceSerial)
		{
			if (!mSpriteInstanceBuffer) mSpriteInstanceBuffer = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
			mSpriteInstanceBuffer->SetData(size_t(instanceCount) * instanceWords * sizeof(uint32_t), instances, BufferUsageType::Stream);
			mSpriteInstanceSerial = instanceSerial;
		}
	}
	catch (const CVulkanError& error)
	{
		Printf("GPU sprite lights failed: %s\n", error.what());
		return false;
	}
	if (!DispatchLights(mSpriteInstanceBuffer->mBuffer.get(), instanceCount, instanceWords, 0, pack, mSpriteLightOut, mSpriteLightPool,
		mSpriteLightSet, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT)) return false;
	mSpriteLightSerial = instanceSerial;
	return true;
}

// [Karto] The SPF_ values are pinned by static_asserts next to WorldModeGpuParticleData.
static const char* ParticleExpandSource = R"(#version 450
layout(local_size_x = 64) in;
layout(push_constant) uniform PC { vec2 ViewVector; float TimeFrac; float PixelStretch; float Yaw; float Pitch; vec2 ViewPos;
	uint Flags; int Style; uint EntryCount; uint Pad; uint RecordCount; uint Spare; } pc;
layout(std430, set = 0, binding = 0) readonly buffer Particles { float P[]; };
layout(std430, set = 0, binding = 1) readonly buffer Entries { uint E[]; };
layout(std430, set = 0, binding = 2) readonly buffer Ranges { float R[]; };
layout(std430, set = 0, binding = 3) writeonly buffer Vertices { float V[]; };
layout(std430, set = 0, binding = 4) readonly buffer Lights { vec4 PL[]; };
layout(std430, set = 0, binding = 5) readonly buffer Program { uint PP[]; };
layout(std430, set = 0, binding = 6) readonly buffer FrameIndices { uint FI[]; };
const uint SPF_NOTIMEFREEZE = 32u, SPF_ROLL = 64u, SPF_NO_XY_BILLBOARD = 256u, SPF_FACECAMERA = 2048u,
	SPF_NOFACECAMERA = 4096u, SPF_ROLLCENTER = 8192u;

struct M34 { vec4 r0, r1, r2; };
M34 Ident() { return M34(vec4(1, 0, 0, 0), vec4(0, 1, 0, 0), vec4(0, 0, 1, 0)); }
void Translate(inout M34 m, vec3 t)
{
	m.r0.w += dot(m.r0.xyz, t);
	m.r1.w += dot(m.r1.xyz, t);
	m.r2.w += dot(m.r2.xyz, t);
}
void Scale(inout M34 m, vec3 s) { m.r0.xyz *= s; m.r1.xyz *= s; m.r2.xyz *= s; }
void Rotate(inout M34 m, vec3 axis, float angle)
{
	axis = normalize(axis);
	float c = cos(radians(angle)), s = sin(radians(angle)), t = 1.0 - c;
	float sx = s * axis.x, sy = s * axis.y, sz = s * axis.z;
	float tx = t * axis.x, txx = tx * axis.x, u = tx * axis.y, v = tx * axis.z;
	float ty = t * axis.y, tyy = ty * axis.y, w = ty * axis.z;
	vec3 a0 = vec3(txx + c, u - sz, v + sy);
	vec3 a1 = vec3(u + sz, tyy + c, w - sx);
	vec3 a2 = vec3(v - sy, w + sx, (t - txx - tyy) + c);
	m.r0.xyz = m.r0.x * a0 + m.r0.y * a1 + m.r0.z * a2;
	m.r1.xyz = m.r1.x * a0 + m.r1.y * a1 + m.r1.z * a2;
	m.r2.xyz = m.r2.x * a0 + m.r2.y * a1 + m.r2.z * a2;
}
vec3 Apply(M34 m, vec3 p) { return vec3(dot(m.r0.xyz, p) + m.r0.w, dot(m.r1.xyz, p) + m.r1.w, dot(m.r2.xyz, p) + m.r2.w); }

void main()
{
	uint e = gl_GlobalInvocationID.x;
	if (e >= pc.EntryCount) return;
	// [Karto] Pad 1: five words per entry, the dynamic light after the slot. Otherwise entries are range records found by
	// their first entry; Pad 2 takes the light from the light pass.
	uint index, slot;
	if (pc.Pad == 1u)
	{
		index = E[e * 5u];
		slot = E[e * 5u + 1u] * 8u;
	}
	else
	{
		uint lo = 0u, hi = pc.RecordCount - 1u;
		while (lo < hi)
		{
			uint mid = (lo + hi + 1u) >> 1;
			if (E[mid * 4u + 2u] <= e) lo = mid;
			else hi = mid - 1u;
		}
		uint first = E[lo * 4u], at = (first & 0x7fffffffu) + (e - E[lo * 4u + 2u]);
		index = (first & 0x80000000u) != 0u ? PP[at] : (FI[at >> 1] >> ((at & 1u) * 16u)) & 0xffffu;
		slot = E[lo * 4u + 3u] * 8u;
	}
	uint b = index * 12u;
	uint color = floatBitsToUint(P[b + 8u]), flags = floatBitsToUint(P[b + 9u]);
	bool textured = (color & 0x1000000u) != 0u;
	float tf = pc.TimeFrac;
	if ((pc.Flags & 1u) != 0u || ((pc.Flags & 2u) != 0u && (flags & SPF_NOTIMEFREEZE) == 0u)) tf = 0.0;
	float x = P[b] + P[b + 4u] * tf, y = P[b + 1u] + P[b + 5u] * tf, z = P[b + 2u] + P[b + 6u] * tf;

	int style = textured ? 2 : pc.Style;
	float factor = style == 1 ? 1.3 / 7.0 : style == 2 ? 2.5 / 7.0 : 1.0 / 7.0;
	float ps = pc.PixelStretch;
	float scalefac = P[b + 3u] * factor;
	scalefac /= sqrt(ps);
	float vvx = pc.ViewVector.x * scalefac * ps, vvy = pc.ViewVector.y * scalefac;
	float x1 = x + vvy, x2 = x - vvy, y1 = y - vvx, y2 = y + vvx, z1 = z - scalefac, z2 = z + scalefac;

	vec3 corner[4] = vec3[4](vec3(x1, z1, y1), vec3(x2, z1, y2), vec3(x1, z2, y1), vec3(x2, z2, y2));
	vec3 center = vec3((x1 + x2) * 0.5, (y1 + y2) * 0.5, (z1 + z2) * 0.5);
	bool xy = (pc.Flags & 4u) != 0u && (flags & SPF_NO_XY_BILLBOARD) == 0u;
	bool face = (pc.Flags & 8u) != 0u;
	if ((pc.Flags & 16u) == 0u && textured)
	{
		if ((flags & SPF_NOFACECAMERA) != 0u) face = false;
		else if ((flags & SPF_FACECAMERA) != 0u) face = true;
	}
	bool doRoll = (flags & SPF_ROLL) != 0u;
	bool useOffsets = (flags & SPF_ROLLCENTER) == 0u;
	vec3 off = vec3(x - center.x, z - center.z, y - center.y);
	float rollDeg = P[b + 10u] + P[b + 11u] * tf;
	float angleDeg = 270.0 - pc.Yaw;
	vec3 pitchAxis = vec3(-sin(radians(angleDeg)), 0.0, cos(radians(angleDeg)));
	vec3 rollAxis = vec3(cos(radians(angleDeg)), 0.0, sin(radians(angleDeg)));
	vec3 c3 = vec3(center.x, center.z, center.y);
	if (xy || face)
	{
		M34 m = Ident();
		Translate(m, c3);
		Scale(m, vec3(1.0, 1.0 / ps, 1.0));
		if (face)
		{
			float absAngle = atan(-(center.y - pc.ViewPos.y), center.x - pc.ViewPos.x) * (180.0 / 3.14159265358979323846);
			Rotate(m, vec3(0.0, 1.0, 0.0), (270.0 - pc.Yaw) + absAngle);
		}
		if (doRoll)
		{
			if (useOffsets) Translate(m, off);
			if (xy) Rotate(m, pitchAxis, -pc.Pitch);
			Rotate(m, rollAxis, rollDeg);
			if (useOffsets) Translate(m, -off);
		}
		else if (xy) Rotate(m, pitchAxis, -pc.Pitch);
		Scale(m, vec3(1.0, ps, 1.0));
		Translate(m, -c3);
		for (int k = 0; k < 4; ++k) corner[k] = Apply(m, corner[k]);
	}
	else if (doRoll)
	{
		M34 m = Ident();
		Translate(m, c3);
		if (useOffsets) Translate(m, off);
		Scale(m, vec3(1.0, 1.0 / ps, 1.0));
		Rotate(m, rollAxis, rollDeg);
		Scale(m, vec3(1.0, ps, 1.0));
		if (useOffsets) Translate(m, -off);
		Translate(m, -c3);
		for (int k = 0; k < 4; ++k) corner[k] = Apply(m, corner[k]);
	}

	uvec3 thing = uvec3((color >> 16) & 255u, (color >> 8) & 255u, color & 255u);
	uvec4 special = uvec4(R[slot + 4u], R[slot + 5u], R[slot + 6u], R[slot + 7u]);
	uvec4 object;
	if (R[slot + 3u] != 0.0) object = uvec4(thing, 255u);
	else if (all(equal(thing, uvec3(255u)))) object = special;
	else if (all(equal(special.rgb, uvec3(255u)))) object = uvec4(thing, 255u);
	else object = uvec4((thing * special.rgb) / 255u, special.a);
	const float sc = 1.0 / 255.0;
	vec3 light = vec3(R[slot], R[slot + 1u], R[slot + 2u]);
	if (pc.Pad == 1u) light = clamp(light + vec3(uintBitsToFloat(E[e * 5u + 2u]), uintBitsToFloat(E[e * 5u + 3u]), uintBitsToFloat(E[e * 5u + 4u])), 0.0, 1.4);
	else if (pc.Pad == 2u) light = clamp(light + (R[slot + 3u] != 0.0 ? vec3(0.0) : PL[index].xyz), 0.0, 1.4);
	vec4 col = vec4(light * (vec3(object.rgb) * sc), P[b + 7u] * (float(object.a) * sc));
	// [Karto] The followers' uniform dynamic light of 0.4 restores what the shader's cap at 1 would cut.
	if (pc.Pad != 0u) col.rgb -= vec3(0.4);

	const int order[6] = int[6](0, 1, 2, 2, 1, 3);
	const vec2 uv[4] = vec2[4](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0));
	uint o = e * 72u;
	for (int k = 0; k < 6; ++k)
	{
		int c = order[k];
		V[o] = corner[c].x; V[o + 1u] = corner[c].y; V[o + 2u] = corner[c].z;
		V[o + 3u] = uv[c].x; V[o + 4u] = uv[c].y;
		V[o + 5u] = 0.0; V[o + 6u] = 0.0; V[o + 7u] = -1.0;
		V[o + 8u] = col.r; V[o + 9u] = col.g; V[o + 10u] = col.b; V[o + 11u] = col.a;
		o += 12u;
	}
}
)";

void VkRenderState::EnsureParticleResources()
{
	if (mParticleReady || mParticleFailed) return;
	try
	{
		auto device = fb->device.get();
		mParticleShader = ShaderBuilder()
			.Type(ShaderType::Compute)
			.AddSource("particle_expand", ParticleExpandSource)
			.DebugName("ParticleExpand")
			.Create("particle_expand", device);
		mParticleSetLayout = DescriptorSetLayoutBuilder()
			.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
			.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
			.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
			.AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
			.AddBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
			.AddBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
			.AddBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
			.DebugName("ParticleExpandSetLayout")
			.Create(device);
		mParticlePipelineLayout = PipelineLayoutBuilder()
			.AddSetLayout(mParticleSetLayout.get())
			.AddPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FGpuParticleParams))
			.DebugName("ParticleExpandPipelineLayout")
			.Create(device);
		mParticlePipeline = ComputePipelineBuilder()
			.Layout(mParticlePipelineLayout.get())
			.ComputeShader(mParticleShader.get())
			.DebugName("ParticleExpandPipeline")
			.Create(device);
		mParticleReady = true;
	}
	catch (const CVulkanError& error)
	{
		mParticleFailed = true;
		Printf("GPU particles unavailable: %s\n", error.what());
	}
}

static uint64_t ParticleExpandNS[4], ParticleExpandMaxNS[4], ParticleExpandCalls, ParticleDataUploads, ParticleOutputAllocs, ParticleLightPasses;
static uint32_t ParticleLastEntries, ParticleLastCount;

CCMD(particle_gpu_timing)
{
	static const char* names[4] = { "data", "frame_uploads", "output_alloc", "record" };
	const double calls = ParticleExpandCalls ? double(ParticleExpandCalls) : 1.0;
	Printf("PARTICLEGPUTIMING calls=%llu data_uploads=%llu output_allocs=%llu light_passes=%llu last_entries=%u last_particles=%u\n",
		(unsigned long long)ParticleExpandCalls, (unsigned long long)ParticleDataUploads, (unsigned long long)ParticleOutputAllocs,
		(unsigned long long)ParticleLightPasses, ParticleLastEntries, ParticleLastCount);
	for (int i = 0; i < 4; ++i)
		Printf("PARTICLEGPUTIMING part=%s mean_us=%.1f max_us=%.1f\n", names[i], ParticleExpandNS[i] / calls / 1000.0, ParticleExpandMaxNS[i] / 1000.0);
}

bool VkRenderState::SupportsParticleExpansion()
{
	EnsureParticleResources();
	return mParticleReady;
}

bool VkRenderState::ExpandParticles(IVertexBuffer* target, const float* particles, uint32_t particleCount, uint64_t serial,
	const uint32_t* entries, const float* ranges, uint32_t rangeCount, const FGpuParticleParams& params, const FGpuParticleIndices& indices,
	const FGpuLightPack* lights)
{
	if (!mParticleReady || params.EntryCount == 0 || particleCount == 0) return false;
	if (params.Pad != 1 && params.RecordCount == 0) return false;
	if (params.Pad == 2 && lights == nullptr) return false;
	try
	{
		auto commands = fb->GetCommands();
		uint64_t mark = I_nsTime();
		auto lap = [&](int part)
		{
			const uint64_t now = I_nsTime(), ns = now - mark;
			ParticleExpandNS[part] += ns;
			ParticleExpandMaxNS[part] = std::max(ParticleExpandMaxNS[part], ns);
			mark = now;
		};
		++ParticleExpandCalls;
		ParticleLastEntries = params.EntryCount;
		ParticleLastCount = particleCount;
		auto upload = [&](std::unique_ptr<VkHardwareDataBuffer>& buffer, const void* data, size_t bytes)
		{
			if (!buffer) buffer = std::make_unique<VkHardwareDataBuffer>(fb, -1, true, false);
			buffer->SetData(bytes, data, BufferUsageType::Stream);
		};
		if (!mParticleData || serial != mParticleSerial)
		{
			upload(mParticleData, particles, size_t(particleCount) * 12 * sizeof(float));
			mParticleSerial = serial;
			++ParticleDataUploads;
		}
		lap(0);
		if (params.Pad == 2 && mParticleLightSerial != serial)
		{
			if (!DispatchLights(mParticleData->mBuffer.get(), particleCount, 12, 1, *lights, mParticleLightOut, mParticleLightPool,
				mParticleLightSet, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)) return false;
			mParticleLightSerial = serial;
			++ParticleLightPasses;
		}
		upload(mParticleEntries, entries, (params.Pad == 1 ? size_t(params.EntryCount) * 5 : size_t(params.RecordCount) * 4) * sizeof(uint32_t));
		if (!mParticleProgram || mParticleProgramVersion != indices.ProgramVersion)
		{
			static const uint32_t none = 0;
			upload(mParticleProgram, indices.ProgramCount ? indices.Program : &none, size_t(std::max(indices.ProgramCount, 1u)) * sizeof(uint32_t));
			mParticleProgramVersion = indices.ProgramVersion;
		}
		{
			static std::vector<uint32_t> words;
			words.assign((size_t(indices.FrameCount) + 1) / 2 + 1, 0u);
			if (indices.FrameCount) memcpy(words.data(), indices.Frame, size_t(indices.FrameCount) * sizeof(uint16_t));
			upload(mParticleFrameIndices, words.data(), words.size() * sizeof(uint32_t));
		}
		upload(mParticleRanges, ranges, size_t(std::max(rangeCount, 1u)) * 8 * sizeof(float));
		lap(1);

		// [Karto] A different output per expansion: draws already recorded and frames in flight keep reading theirs. Outputs
		// return to a pool, reused once the GPU has completed the frame that drew from them.
		auto vb = static_cast<VkHardwareVertexBuffer*>(target);
		const size_t bytes = size_t(params.EntryCount) * 6 * 12 * sizeof(float);
		const uint64_t frame = commands->GetCurrentFrame(), completed = commands->GetCompletedFrame();
		if (vb->mBuffer) mParticleOutputs.push_back({ std::move(vb->mBuffer), frame });
		for (size_t i = 0; i < mParticleOutputs.size(); ++i)
		{
			auto& out = mParticleOutputs[i];
			if (out.Frame > completed) continue;
			if (out.Buffer->size >= bytes && !vb->mBuffer)
			{
				vb->mBuffer = std::move(out.Buffer);
			}
			else if (out.Buffer->size >= bytes && mParticleOutputs.size() <= 4)
			{
				continue;
			}
			else
			{
				commands->DrawDeleteList->Add(std::move(out.Buffer));
			}
			mParticleOutputs.erase(mParticleOutputs.begin() + i--);
		}
		if (!vb->mBuffer)
		{
			const size_t capacity = (bytes + bytes / 4 + 0xfffff) & ~size_t(0xfffff);
			vb->mBuffer = BufferBuilder()
				.Usage(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY)
				.Size(capacity)
				.DebugName("GpuParticleVertices")
				.Create(fb->device.get());
			++ParticleOutputAllocs;
		}
		lap(2);

		if (mParticleSet) commands->DrawDeleteList->Add(std::move(mParticleSet));
		if (mParticlePool) commands->DrawDeleteList->Add(std::move(mParticlePool));
		mParticlePool = DescriptorPoolBuilder().AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7).MaxSets(1).Create(fb->device.get());
		mParticleSet = mParticlePool->allocate(mParticleSetLayout.get());
		WriteDescriptors()
			.AddBuffer(mParticleSet.get(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mParticleData->mBuffer.get())
			.AddBuffer(mParticleSet.get(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mParticleEntries->mBuffer.get())
			.AddBuffer(mParticleSet.get(), 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mParticleRanges->mBuffer.get())
			.AddBuffer(mParticleSet.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vb->mBuffer.get())
			.AddBuffer(mParticleSet.get(), 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, params.Pad == 2 ? mParticleLightOut.get() : mParticleEntries->mBuffer.get())
			.AddBuffer(mParticleSet.get(), 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mParticleProgram->mBuffer.get())
			.AddBuffer(mParticleSet.get(), 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mParticleFrameIndices->mBuffer.get())
			.Execute(fb->device.get());

		auto cmd = commands->GetTransferCommands();
		cmd->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, mParticlePipeline.get());
		cmd->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, mParticlePipelineLayout.get(), 0, mParticleSet.get());
		cmd->pushConstants(mParticlePipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
		cmd->dispatch((params.EntryCount + 63) / 64, 1, 1);
		// [Karto] The draws read the output as vertex input later in the same submit (the draw list follows the transfer list).
		PipelineBarrier()
			.AddBuffer(vb->mBuffer.get(), VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT)
			.Execute(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
		lap(3);
		return true;
	}
	catch (const CVulkanError& error)
	{
		mParticleFailed = true;
		mParticleReady = false;
		Printf("GPU particles disabled: %s\n", error.what());
		return false;
	}
}
