/*
** vk_commandbuffer.cpp
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

#include "vk_commandbuffer.h"
#include "vk_renderdevice.h"
#include "zvulkan/vulkanswapchain.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "hw_clock.h"
#include "v_video.h"
#include "i_time.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "cmdlib.h"
#include "printf.h"
#include <algorithm>
#include <vector>
#include <map>
#include <string>

CVAR(Bool, gl_profile_gpu_sections, false, 0)

namespace
{
	struct FGpuSection
	{
		double Sum = 0;
		int Count = 0;
		void Add(double ns) { Sum += ns; ++Count; }
	};
	std::map<std::string, FGpuSection> GpuSections;
}

CCMD(gpu_sections)
{
	const int frames = GpuSections.count("frame") ? GpuSections["frame"].Count : 0;
	for (const auto &entry : GpuSections)
		Printf("GPUSECTION %.4f ms per frame (%d of %d frames) %s\n", entry.second.Sum / max(frames, 1) / 1e6, entry.second.Count, frames, entry.first.c_str());
	GpuSections.clear();
}

extern int rendered_commandbuffers;
int current_rendered_commandbuffers;

extern bool gpuStatActive;
extern bool keepGpuStatActive;
extern FString gpuStatOutput;

VkCommandBufferManager::VkCommandBufferManager(VulkanRenderDevice* fb) : fb(fb)
{
	mCommandPool = CommandPoolBuilder()
		.QueueFamily(fb->device->GraphicsFamily)
		.DebugName("mCommandPool")
		.Create(fb->device.get());

	for (auto& bank : mSubmitSemaphore)
		for (auto& semaphore : bank)
			semaphore.reset(new VulkanSemaphore(fb->device.get()));

	for (auto& bank : mSubmitFence)
		for (auto& fence : bank)
			fence.reset(new VulkanFence(fb->device.get()));

	for (int i = 0; i < maxConcurrentSubmitCount; i++)
		mSubmitWaitFences[i] = mSubmitFence[mBank][i]->fence;

	if (fb->device->GraphicsTimeQueries)
	{
		mTimestampQueryPool = QueryPoolBuilder()
			.QueryType(VK_QUERY_TYPE_TIMESTAMP, MaxTimestampQueries)
			.Create(fb->device.get());

		GetDrawCommands()->resetQueryPool(mTimestampQueryPool.get(), 0, MaxTimestampQueries);
	}
}

VkCommandBufferManager::~VkCommandBufferManager()
{
}

VulkanCommandBuffer* VkCommandBufferManager::GetTransferCommands()
{
	if (!mTransferCommands)
	{
		mTransferCommands = mCommandPool->createBuffer();
		mTransferCommands->SetDebugName("VulkanRenderDevice.mTransferCommands");
		mTransferCommands->begin();
		if (mGPUProfileActive && gl_profile_gpu_sections)
		{
			if (!mTransferProfilePool) mTransferProfilePool = QueryPoolBuilder().QueryType(VK_QUERY_TYPE_TIMESTAMP, 2).Create(fb->device.get());
			mTransferCommands->resetQueryPool(mTransferProfilePool.get(), 0, 2);
			mTransferCommands->writeTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mTransferProfilePool.get(), 0);
			mTransferProfileRange = 0;
		}
	}
	return mTransferCommands.get();
}

VulkanCommandBuffer* VkCommandBufferManager::GetDrawCommands()
{
	if (!mDrawCommands)
	{
		mDrawCommands = mCommandPool->createBuffer();
		mDrawCommands->SetDebugName("VulkanRenderDevice.mDrawCommands");
		mDrawCommands->begin();
		if (mGPUProfileActive) mGPUProfileDrawRange = BeginGPUProfileRange(mDrawCommands.get(), GPUCommands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
	}
	return mDrawCommands.get();
}

static bool OverdrawRequested;

CCMD(vk_overdraw)
{
	OverdrawRequested = true;
}

void VkCommandBufferManager::BeginFrame()
{
	if (mOverdrawActive)
	{
		mOverdrawActive = false;
		mOverdrawPending = true;
		mOverdrawFrameBarriers = VulkanCommandBuffer::BarrierCalls - mOverdrawBarrierStart;
		mOverdrawFrameBarrierItems = VulkanCommandBuffer::BarrierItems - mOverdrawItemStart;
	}
	if (mOverdrawPending && mCompletedSerial >= mOverdrawFrame)
	{
		mOverdrawPending = false;
		const size_t n = mOverdrawNames.size();
		constexpr size_t stats = 4;
		std::vector<uint64_t> values(n * stats + stats, 0);
		const double pixels = double(screen->GetWidth()) * screen->GetHeight();
		if (n > 0 && mOverdrawPool->getResults(0, uint32_t(n), n * stats * sizeof(uint64_t), values.data(), stats * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT))
		{
			Printf("VKOVERDRAW frame=%llu pixels=%.0f nested_skipped=%d barrier_calls=%llu barrier_items=%llu\n", (unsigned long long)mOverdrawFrame, pixels,
				mOverdrawNested, (unsigned long long)mOverdrawFrameBarriers, (unsigned long long)mOverdrawFrameBarrierItems);
			for (size_t i = 0; i < n; ++i)
			{
				const uint64_t* v = &values[i * stats];
				Printf("VKOVERDRAW %s fragments=%llu per_pixel=%.3f vertices=%llu clip_in=%llu clip_out=%llu barriers=%llu\n", mOverdrawNames[i].GetChars(),
					(unsigned long long)v[3], v[3] / pixels, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
					(unsigned long long)(i < mOverdrawBarriers.size() ? mOverdrawBarriers[i] : 0));
			}
		}
		else Printf("VKOVERDRAW no results (%zu groups)\n", n);
	}
	if (OverdrawRequested && !mOverdrawPending)
	{
		OverdrawRequested = false;
		if (!fb->device->EnabledFeatures.Features.pipelineStatisticsQuery) Printf("VKOVERDRAW unsupported\n");
		else
		{
			if (!mOverdrawPool)
				mOverdrawPool = QueryPoolBuilder().QueryType(VK_QUERY_TYPE_PIPELINE_STATISTICS, MaxOverdrawQueries,
					VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
					VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT).Create(fb->device.get());
			GetDrawCommands()->resetQueryPool(mOverdrawPool.get(), 0, MaxOverdrawQueries);
			mOverdrawNames.clear();
			mOverdrawStack.clear();
			mOverdrawBarriers.clear();
			mOverdrawBarrierStart = VulkanCommandBuffer::BarrierCalls;
			mOverdrawItemStart = VulkanCommandBuffer::BarrierItems;
			mOverdrawOpen = mOverdrawNested = 0;
			mOverdrawFrame = mFrameSerial;
			mOverdrawActive = true;
		}
	}
	mGPUProfileActive = false;
	mGPUProfileDrawRange = -1;
	mGPUProfileNextQuery = 0;
	mGPUProfileSkippedDepth = 0;
	mGPUProfileRanges.clear();
	mTransferProfileRange = -1;
	mGPUProfileGroups.clear();
	GPUProfile = {};
	GPUProfile.Enabled = IsGPUProfilingEnabled();
	if (GPUProfile.Enabled && fb->device->GraphicsTimeQueries)
	{
		const auto bits = fb->device->PhysicalDevice.QueueFamilies[fb->device->GraphicsFamily].timestampValidBits;
		if (bits > 0)
		{
			mGPUProfileMask = bits == 64 ? std::numeric_limits<uint64_t>::max() : (uint64_t(1) << bits) - 1;
			if (!mGPUProfilePool)
			{
				mGPUProfilePool = QueryPoolBuilder().QueryType(VK_QUERY_TYPE_TIMESTAMP, MaxGPUProfileQueries).Create(fb->device.get());
				mGPUProfileRanges.reserve(MaxGPUProfileQueries / 2);
				mGPUProfileGroups.reserve(MaxGPUProfileQueries / 2);
			}
			GetDrawCommands()->resetQueryPool(mGPUProfilePool.get(), 0, MaxGPUProfileQueries);
			mGPUProfileActive = true;
			mGPUProfileDrawRange = BeginGPUProfileRange(mDrawCommands.get(), GPUCommands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
		}
	}
	mReuseWaitNS = 0;
	mReuseWaitCount = 0;
	if (mNextTimestampQuery > 0)
	{
		GetDrawCommands()->resetQueryPool(mTimestampQueryPool.get(), 0, mNextTimestampQuery);
		mNextTimestampQuery = 0;
	}
}

void VkCommandBufferManager::FlushCommands(VulkanCommandBuffer** commands, size_t count, bool finish, bool lastsubmit)
{
	int currentIndex = mNextSubmit % maxConcurrentSubmitCount;

	if (mNextSubmit >= maxConcurrentSubmitCount)
	{
		const bool profile = IsFinishProfilingEnabled();
		uint64_t start = profile ? I_nsTime() : 0;
		vkWaitForFences(fb->device->device, 1, &mSubmitFence[mBank][currentIndex]->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
		if (profile)
		{
			mReuseWaitNS += I_nsTime() - start;
			++mReuseWaitCount;
		}
		vkResetFences(fb->device->device, 1, &mSubmitFence[mBank][currentIndex]->fence);
	}

	QueueSubmit submit;

	for (size_t i = 0; i < count; i++)
		submit.AddCommandBuffer(commands[i]);

	if (mNextSubmit > 0 && mPreviousSubmitChained)
		submit.AddWait(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mSubmitSemaphore[mBank][(mNextSubmit - 1) % maxConcurrentSubmitCount].get());

	auto framebuffers = fb->GetFramebufferManager();
	if (finish && framebuffers->PresentImageIndex != -1)
	{
		submit.AddWait(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, framebuffers->GetImageAvailableSemaphore());
		submit.AddSignal(framebuffers->RenderFinishedSemaphores[framebuffers->PresentImageIndex].get());
	}

	mPreviousSubmitChained = !lastsubmit;
	if (mPreviousSubmitChained)
		submit.AddSignal(mSubmitSemaphore[mBank][currentIndex].get());

	submit.Execute(fb->device.get(), fb->device->GraphicsQueue, mSubmitFence[mBank][currentIndex].get());
	mNextSubmit++;
}

void VkCommandBufferManager::FlushCommands(bool finish, bool lastsubmit, bool uploadOnly)
{
	if (!uploadOnly)
		fb->GetRenderState()->EndRenderPass();

	if ((!uploadOnly && mDrawCommands) || mTransferCommands)
	{
		VulkanCommandBuffer* commands[2];
		size_t count = 0;

		if (mTransferCommands)
		{
			if (mTransferProfileRange >= 0) { mTransferCommands->writeTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mTransferProfilePool.get(), 1); mTransferProfileWritten = true; }
			mTransferProfileRange = -1;
			mTransferCommands->end();
			commands[count++] = mTransferCommands.get();
			TransferDeleteList->Add(std::move(mTransferCommands));
		}

		if (!uploadOnly && mDrawCommands)
		{
			if (mGPUProfileActive) EndGPUProfileRange(mDrawCommands.get(), mGPUProfileDrawRange);
			mGPUProfileDrawRange = -1;
			mDrawCommands->end();
			commands[count++] = mDrawCommands.get();
			DrawDeleteList->Add(std::move(mDrawCommands));
		}

		FlushCommands(commands, count, finish, lastsubmit);

		current_rendered_commandbuffers += (int)count;
	}
}

void VkCommandBufferManager::WaitForCommands(bool finish, bool uploadOnly)
{
	const bool profile = finish && IsFinishProfilingEnabled();
	uint64_t ticks[7] = {};
	if (finish)
	{
		FinishProfileAvailable = false;
		Finish.Reset();
		Finish.Clock();

		if (profile) ticks[0] = I_nsTime();
		fb->GetFramebufferManager()->AcquireImage();
		if (profile) ticks[1] = I_nsTime();
	}

	FlushCommands(finish, true, uploadOnly);
	if (profile) ticks[2] = I_nsTime();

	if (finish)
	{
		if (!fb->GetVSync())
			fb->FPSLimit();
		if (profile) ticks[3] = I_nsTime();
		fb->GetFramebufferManager()->QueuePresent();
		if (profile) ticks[4] = I_nsTime();
	}

	int numWaitFences = min(mNextSubmit, (int)maxConcurrentSubmitCount);

	const bool leaveInFlight = finish && !uploadOnly && CanLeaveFrameInFlight();
	if (leaveInFlight)
	{
		RetirePendingFrame();
		if (profile) ticks[5] = I_nsTime();
		mPending.Fences = numWaitFences;
		mPending.Bank = mBank;
		mPending.Serial = mFrameSerial;
		mPending.Transfer = std::move(TransferDeleteList);
		mPending.Draw = std::move(DrawDeleteList);
		TransferDeleteList = std::make_unique<DeleteList>();
		DrawDeleteList = std::make_unique<DeleteList>();
		mBank ^= 1;
		++mInFlightFrames;
	}
	else
	{
		// [Karto] Retire the parked frame first: its fences must be reset and its objects freed before any bank reuse.
		RetirePendingFrame();
		// [Karto] This frame's submits went to the current bank; the other bank's fences are already reset and would
		// never signal.
		for (int i = 0; i < numWaitFences; ++i) mSubmitWaitFences[i] = mSubmitFence[mBank][i]->fence;
		if (numWaitFences > 0)
		{
			vkWaitForFences(fb->device->device, numWaitFences, mSubmitWaitFences, VK_TRUE, std::numeric_limits<uint64_t>::max());
		}
		if (profile) ticks[5] = I_nsTime();
		if (finish && mGPUProfileActive) ReadGPUProfile();
		if (numWaitFences > 0)
		{
			vkResetFences(fb->device->device, numWaitFences, mSubmitWaitFences);
		}
		// [Karto] A mid-frame wait completes only what was submitted so far; work recorded later in this frame is not
		// covered.
		if (finish)
		{
			mCompletedSerial = mFrameSerial;
			++mSerialFrames;
		}
	}

	int submits = mNextSubmit;
	if (!leaveInFlight) DeleteFrameObjects(uploadOnly);
	mNextSubmit = 0;
	mPreviousSubmitChained = false;
	if (profile) ticks[6] = I_nsTime();

	if (finish)
	{
		Finish.Unclock();
		if (profile) RecordFinishProfile(ticks, submits, numWaitFences, mReuseWaitNS, mReuseWaitCount, fb->IsFullscreen(), fb->GetVSync());
		rendered_commandbuffers = current_rendered_commandbuffers;
		current_rendered_commandbuffers = 0;
		++mFrameSerial;
	}
}

void VkCommandBufferManager::DeleteFrameObjects(bool uploadOnly)
{
	TransferDeleteList = std::make_unique<DeleteList>();
	if (!uploadOnly)
	{
		DrawDeleteList = std::make_unique<DeleteList>();
		mPending.Transfer.reset();
		mPending.Draw.reset();
	}
}

CUSTOM_CVAR(Int, vk_frames_in_flight, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1) self = 1;
	else if (self > 2) self = 2;
}

// [Karto] The frozen in-renderer GPU-world stack and the wall vertex cache reuse host-mapped storage on the assumption that
// the previous frame has finished; enabling any of them keeps the frame end serial.
static const char* FrozenFrameFeature()
{
	static std::vector<FBoolCVar*> frozen;
	static bool collected = false;
	if (!collected)
	{
		collected = true;
		CVarMap::Iterator it(cvarMap);
		CVarMap::Pair* pair;
		while (it.NextPair(pair))
		{
			auto var = pair->Value;
			const char* name = var->GetName();
			if (var->GetRealType() == CVAR_Bool && (strnicmp(name, "vk_gpu_driven", 13) == 0 || strnicmp(name, "vk_retained", 11) == 0 || stricmp(name, "gl_wallcache") == 0))
				frozen.push_back(static_cast<FBoolCVar*>(var));
		}
	}
	for (auto var : frozen)
		if (**var) return var->GetName();
	return nullptr;
}

bool VkCommandBufferManager::CanLeaveFrameInFlight() const
{
	auto self = const_cast<VkCommandBufferManager*>(this);
	if (vk_frames_in_flight < 2) { self->mLastSerialReason = "depth1"; return false; }
	// [Karto] Query results are read right after the frame-end fence; a parked frame would make them unavailable.
	if (mGPUProfileActive || gpuStatActive || keepGpuStatActive || mNextTimestampQuery > 0) { self->mLastSerialReason = "gpu_queries"; return false; }
	if (auto name = FrozenFrameFeature()) { self->mLastSerialReason = name; return false; }
	return true;
}

void VkCommandBufferManager::WaitForPendingFrame()
{
	if (RetirePendingFrame()) ++mForcedPendingWaits;
}

bool VkCommandBufferManager::RetirePendingFrame()
{
	if (!mPending.Transfer && !mPending.Draw && mPending.Fences == 0) return false;
	const uint64_t start = I_nsTime();
	if (mPending.Fences > 0)
	{
		VkFence fences[maxConcurrentSubmitCount];
		for (int i = 0; i < mPending.Fences; ++i) fences[i] = mSubmitFence[mPending.Bank][i]->fence;
		vkWaitForFences(fb->device->device, mPending.Fences, fences, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(fb->device->device, mPending.Fences, fences);
	}
	mPending.Fences = 0;
	mPending.Transfer.reset();
	mPending.Draw.reset();
	mCompletedSerial = std::max(mCompletedSerial, mPending.Serial);
	mPendingWaitNS += I_nsTime() - start;
	++mPendingWaits;
	return true;
}

CCMD(vk_frames_in_flight_stats)
{
	if (screen && screen->IsVulkan()) static_cast<VulkanRenderDevice*>(screen)->GetCommands()->PrintFramesInFlightStats();
}

void VkCommandBufferManager::PrintFramesInFlightStats() const
{
	Printf("FRAMESINFLIGHT depth=%d slot=%d frame=%llu completed=%llu in_flight_frames=%llu serial_frames=%llu last_serial_reason=%s pending_waits=%llu avg_pending_wait_us=%.1f forced_waits=%llu\n",
		int(vk_frames_in_flight), mBank, (unsigned long long)mFrameSerial, (unsigned long long)mCompletedSerial,
		(unsigned long long)mInFlightFrames, (unsigned long long)mSerialFrames, mLastSerialReason, (unsigned long long)mPendingWaits,
		mPendingWaits ? double(mPendingWaitNS) / mPendingWaits / 1000.0 : 0.0, (unsigned long long)mForcedPendingWaits);
}

void VkCommandBufferManager::PushGroup(const FString& name)
{
	if (mOverdrawActive)
	{
		// [Karto] Statistics queries of one type cannot nest: only the outermost open group is measured.
		if (mOverdrawOpen == 0 && mOverdrawNames.size() < MaxOverdrawQueries)
		{
			GetDrawCommands()->beginQuery(mOverdrawPool.get(), uint32_t(mOverdrawNames.size()), 0);
			mOverdrawStack.push_back(int(mOverdrawNames.size()));
			mOverdrawNames.push_back(name);
			mOverdrawBarriers.push_back(0);
			mOverdrawGroupBarrierStart = VulkanCommandBuffer::BarrierCalls;
			mOverdrawOpen = 1;
		}
		else
		{
			mOverdrawStack.push_back(-1);
			++mOverdrawNested;
		}
	}
	if (mGPUProfileActive)
	{
		if (mGPUProfileGroups.size() < MaxGPUProfileQueries / 2)
		{
			static const char *names[] = { "bloom", "blur", "lens", "fxaa", "exposure", "colormap", "tonemap", "ssao", "shadowmap" };
			int category = GPUCustom;
			for (int i = 0; i < 9; ++i) if (name.Compare(names[i]) == 0) category = GPUBloom + i;
			if (name.Compare("world_opaque") == 0) category = GPUWorldOpaque;
			if (name.Compare("world_masked") == 0) category = GPUWorldMasked;
			if (name.Compare("world_translucent") == 0) category = GPUWorldTranslucent;
			int range = BeginGPUProfileRange(GetDrawCommands(), category, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
			if (range >= 0)
			{
				mGPUProfileRanges[range].Name = name;
				mGPUProfileRanges[range].Depth = int(mGPUProfileGroups.size());
			}
			mGPUProfileGroups.push_back(range);
		}
		else
		{
			++mGPUProfileSkippedDepth;
			++GPUProfile.Dropped;
		}
	}
	if (!gpuStatActive)
		return;

	if (mNextTimestampQuery < MaxTimestampQueries && fb->device->GraphicsTimeQueries)
	{
		TimestampQuery q;
		q.name = name;
		q.startIndex = mNextTimestampQuery++;
		q.endIndex = 0;
		GetDrawCommands()->writeTimestamp(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, mTimestampQueryPool.get(), q.startIndex);
		mGroupStack.push_back(timeElapsedQueries.size());
		timeElapsedQueries.push_back(q);
	}
}

void VkCommandBufferManager::PopGroup()
{
	if (mOverdrawActive && !mOverdrawStack.empty())
	{
		const int query = mOverdrawStack.back();
		mOverdrawStack.pop_back();
		if (query >= 0)
		{
			GetDrawCommands()->endQuery(mOverdrawPool.get(), uint32_t(query));
			mOverdrawBarriers[query] = VulkanCommandBuffer::BarrierCalls - mOverdrawGroupBarrierStart;
			mOverdrawOpen = 0;
		}
	}
	if (mGPUProfileActive)
	{
		if (mGPUProfileSkippedDepth > 0) --mGPUProfileSkippedDepth;
		else if (!mGPUProfileGroups.empty())
		{
			EndGPUProfileRange(GetDrawCommands(), mGPUProfileGroups.back());
			mGPUProfileGroups.pop_back();
		}
	}
	if (!gpuStatActive || mGroupStack.empty())
		return;

	TimestampQuery& q = timeElapsedQueries[mGroupStack.back()];
	mGroupStack.pop_back();

	if (mNextTimestampQuery < MaxTimestampQueries && fb->device->GraphicsTimeQueries)
	{
		q.endIndex = mNextTimestampQuery++;
		GetDrawCommands()->writeTimestamp(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, mTimestampQueryPool.get(), q.endIndex);
	}
}
int VkCommandBufferManager::BeginWorldProfile(int category, bool compute, VulkanCommandBuffer* commands)
{
	if (!mGPUProfileActive) return -1;
	return BeginGPUProfileRange(commands ? commands : GetDrawCommands(), category, compute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
}

void VkCommandBufferManager::EndWorldProfile(int range, bool compute, VulkanCommandBuffer* commands)
{
	if (range >= 0) EndGPUProfileRange(commands ? commands : GetDrawCommands(), range, compute ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

int VkCommandBufferManager::BeginGPUProfileRange(VulkanCommandBuffer *commands, int category, VkPipelineStageFlagBits stage)
{
	if (mGPUProfileNextQuery + 2 > MaxGPUProfileQueries)
	{
		++GPUProfile.Dropped;
		return -1;
	}
	uint32_t start = mGPUProfileNextQuery;
	mGPUProfileNextQuery += 2;
	mGPUProfileRanges.push_back({ category, start, start + 1 });
	commands->writeTimestamp(stage, mGPUProfilePool.get(), start);
	return int(mGPUProfileRanges.size()) - 1;
}

void VkCommandBufferManager::EndGPUProfileRange(VulkanCommandBuffer *commands, int range, VkPipelineStageFlagBits stage)
{
	if (range < 0) return;
	auto &entry = mGPUProfileRanges[range];
	commands->writeTimestamp(stage, mGPUProfilePool.get(), entry.End);
	entry.Closed = true;
}

void VkCommandBufferManager::ReadGPUProfile()
{
	uint64_t results[MaxGPUProfileQueries][2] = {};
	uint64_t start = I_nsTime();
	bool available = mGPUProfileNextQuery > 0 && mGPUProfilePool->getResults(0, mGPUProfileNextQuery,
		sizeof(results[0]) * mGPUProfileNextQuery, results, sizeof(results[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
	GPUProfile.ReadNS = I_nsTime() - start;
	mGPUProfileActive = false;
	if (!available || !mGPUProfileGroups.empty() || mGPUProfileSkippedDepth > 0) return;
	for (const auto &range : mGPUProfileRanges)
	{
		if (!range.Closed || !results[range.Start][1] || !results[range.End][1]) return;
	}
	GPUProfile.Available = true;
	double period = fb->device->PhysicalDevice.Properties.Properties.limits.timestampPeriod;
	int first = -1, last = -1;
	for (const auto &range : mGPUProfileRanges)
	{
		GPUProfile.DurationNS[range.Category] += uint64_t(((results[range.End][0] - results[range.Start][0]) & mGPUProfileMask) * period);
		++GPUProfile.Samples[range.Category];
		if (range.Category == GPUCommands)
		{
			if (first < 0) first = int(range.Start);
			last = int(range.End);
		}
	}
	if (first >= 0)
	{
		GPUProfile.DurationNS[GPUFrameSpan] = uint64_t(((results[last][0] - results[first][0]) & mGPUProfileMask) * period);
		GPUProfile.Samples[GPUFrameSpan] = 1;
	}
	if (gl_profile_gpu_sections && first >= 0)
	{
		auto span = [&](uint32_t a, uint32_t b) { return double((results[b][0] - results[a][0]) & mGPUProfileMask) * period; };
		struct FLevel { uint32_t End; uint32_t Cursor; FString Previous; FString Prefix; };
		std::vector<FLevel> levels = { { uint32_t(last), uint32_t(first), "start", "" } };
		auto closeTo = [&](size_t depth)
		{
			while (levels.size() > depth + 1)
			{
				FLevel done = levels.back();
				levels.pop_back();
				FString key;
				key.Format("%sgap %s > end", done.Prefix.GetChars(), done.Previous.GetChars());
				GpuSections[key.GetChars()].Add(span(done.Cursor, done.End));
			}
		};
		for (const auto &range : mGPUProfileRanges)
		{
			if (range.Depth < 0) continue;
			closeTo(size_t(range.Depth));
			FLevel &parent = levels.back();
			FString key;
			key.Format("%s%s", parent.Prefix.GetChars(), range.Name.GetChars());
			GpuSections[key.GetChars()].Add(span(range.Start, range.End));
			key.Format("%sgap %s > %s", parent.Prefix.GetChars(), parent.Previous.GetChars(), range.Name.GetChars());
			GpuSections[key.GetChars()].Add(span(parent.Cursor, range.Start));
			parent.Cursor = range.End;
			parent.Previous = range.Name;
			FString prefix;
			prefix.Format("%s%s/", parent.Prefix.GetChars(), range.Name.GetChars());
			levels.push_back({ range.End, range.Start, "start", prefix });
		}
		closeTo(0);
		FString key;
		key.Format("gap %s > end", levels[0].Previous.GetChars());
		GpuSections[key.GetChars()].Add(span(levels[0].Cursor, levels[0].End));
		GpuSections["frame"].Add(span(uint32_t(first), uint32_t(last)));
		uint64_t uploads[2][2] = {};
		if (mTransferProfileWritten && mTransferProfilePool->getResults(0, 2, sizeof(uploads), uploads, sizeof(uploads[0]),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) && uploads[0][1] && uploads[1][1])
			GpuSections["uploads (last buffer)"].Add(double((uploads[1][0] - uploads[0][0]) & mGPUProfileMask) * period);
		mTransferProfileWritten = false;
	}
}

void VkCommandBufferManager::UpdateGpuStats()
{
	uint64_t timestamps[MaxTimestampQueries];
	if (mNextTimestampQuery > 0)
		mTimestampQueryPool->getResults(0, mNextTimestampQuery, sizeof(uint64_t) * mNextTimestampQuery, timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

	double timestampPeriod = fb->device->PhysicalDevice.Properties.Properties.limits.timestampPeriod;

	gpuStatOutput = "";
	for (auto& q : timeElapsedQueries)
	{
		if (q.endIndex <= q.startIndex)
			continue;

		int64_t timeElapsed = max(static_cast<int64_t>(timestamps[q.endIndex] - timestamps[q.startIndex]), (int64_t)0);
		double timeNS = timeElapsed * timestampPeriod;

		FString out;
		out.Format("%s=%04.2f ms\n", q.name.GetChars(), timeNS / 1000000.0f);
		gpuStatOutput += out;
	}
	timeElapsedQueries.clear();
	mGroupStack.clear();

	gpuStatActive = keepGpuStatActive;
	keepGpuStatActive = false;
}
