/*
** vk_commandbuffer.h
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

#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkanobjects.h>
#include "zstring.h"

class VulkanRenderDevice;

class VkCommandBufferManager
{
public:
	VkCommandBufferManager(VulkanRenderDevice* fb);
	~VkCommandBufferManager();

	void BeginFrame();

	VulkanCommandBuffer* GetTransferCommands();
	VulkanCommandBuffer* GetDrawCommands();

	void FlushCommands(bool finish, bool lastsubmit = false, bool uploadOnly = false);

	void WaitForCommands(bool finish) { WaitForCommands(finish, false); }
	void WaitForCommands(bool finish, bool uploadOnly);

	void PushGroup(const FString& name);
	void PopGroup();
	void UpdateGpuStats();
	int BeginWorldProfile(int category, bool compute, VulkanCommandBuffer* commands = nullptr);
	void EndWorldProfile(int range, bool compute, VulkanCommandBuffer* commands = nullptr);

	class DeleteList
	{
	public:
		std::vector<std::unique_ptr<VulkanBuffer>> Buffers;
		std::vector<std::unique_ptr<VulkanSampler>> Samplers;
		std::vector<std::unique_ptr<VulkanImage>> Images;
		std::vector<std::unique_ptr<VulkanImageView>> ImageViews;
		std::vector<std::unique_ptr<VulkanFramebuffer>> Framebuffers;
		std::vector<std::unique_ptr<VulkanAccelerationStructure>> AccelStructs;
		std::vector<std::unique_ptr<VulkanDescriptorPool>> DescriptorPools;
		std::vector<std::unique_ptr<VulkanDescriptorSet>> Descriptors;
		std::vector<std::unique_ptr<VulkanShader>> Shaders;
		std::vector<std::unique_ptr<VulkanCommandBuffer>> CommandBuffers;
		size_t TotalSize = 0;

		void Add(std::unique_ptr<VulkanBuffer> obj) { if (obj) { TotalSize += obj->size; Buffers.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanSampler> obj) { if (obj) { Samplers.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanImage> obj) { if (obj) { Images.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanImageView> obj) { if (obj) { ImageViews.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanFramebuffer> obj) { if (obj) { Framebuffers.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanAccelerationStructure> obj) { if (obj) { AccelStructs.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanDescriptorPool> obj) { if (obj) { DescriptorPools.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanDescriptorSet> obj) { if (obj) { Descriptors.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanCommandBuffer> obj) { if (obj) { CommandBuffers.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanShader> obj) { if (obj) { Shaders.push_back(std::move(obj)); } }
	};

	std::unique_ptr<DeleteList> TransferDeleteList = std::make_unique<DeleteList>();
	std::unique_ptr<DeleteList> DrawDeleteList = std::make_unique<DeleteList>();

	void DeleteFrameObjects(bool uploadOnly = false);

	int GetFrameSlot() const { return mBank; }
	void WaitForPendingFrame();
	uint64_t GetCompletedFrame() const { return mCompletedSerial; }
	uint64_t GetCurrentFrame() const { return mFrameSerial; }

private:
	bool CanLeaveFrameInFlight() const;
	bool RetirePendingFrame();
	void FlushCommands(VulkanCommandBuffer** commands, size_t count, bool finish, bool lastsubmit);
	int BeginGPUProfileRange(VulkanCommandBuffer *commands, int category, VkPipelineStageFlagBits stage);
	void EndGPUProfileRange(VulkanCommandBuffer *commands, int range, VkPipelineStageFlagBits stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	void ReadGPUProfile();

	VulkanRenderDevice* fb = nullptr;

	std::unique_ptr<VulkanCommandPool> mCommandPool;

	std::unique_ptr<VulkanCommandBuffer> mTransferCommands;
	std::unique_ptr<VulkanCommandBuffer> mDrawCommands;

	enum { maxConcurrentSubmitCount = 8, maxFramesInFlight = 2 };
	std::unique_ptr<VulkanSemaphore> mSubmitSemaphore[maxFramesInFlight][maxConcurrentSubmitCount];
	std::unique_ptr<VulkanFence> mSubmitFence[maxFramesInFlight][maxConcurrentSubmitCount];
	int mBank = 0;
	struct PendingFrame
	{
		int Fences = 0;
		int Bank = 0;
		uint64_t Serial = 0;
		std::unique_ptr<DeleteList> Transfer, Draw;
	} mPending;
	uint64_t mFrameSerial = 1, mCompletedSerial = 0;
	uint64_t mInFlightFrames = 0, mSerialFrames = 0, mPendingWaitNS = 0, mPendingWaits = 0, mForcedPendingWaits = 0;
	const char* mLastSerialReason = "none";

public:
	void PrintFramesInFlightStats() const;

private:
	VkFence mSubmitWaitFences[maxConcurrentSubmitCount];
	int mNextSubmit = 0;
	bool mPreviousSubmitChained = false;
	uint64_t mReuseWaitNS = 0;
	uint64_t mReuseWaitCount = 0;

	struct GPUProfileRange
	{
		int Category;
		uint32_t Start;
		uint32_t End;
		bool Closed = false;
		FString Name;
		int Depth = -1;
	};

	static constexpr uint32_t MaxGPUProfileQueries = 512;
	std::unique_ptr<VulkanQueryPool> mGPUProfilePool;
	std::vector<GPUProfileRange> mGPUProfileRanges;
	int mTransferProfileRange = -1;
	bool mTransferProfileWritten = false;
	std::unique_ptr<VulkanQueryPool> mTransferProfilePool;
	std::vector<int> mGPUProfileGroups;
	uint32_t mGPUProfileNextQuery = 0;
	uint32_t mGPUProfileSkippedDepth = 0;
	uint64_t mGPUProfileMask = 0;
	int mGPUProfileDrawRange = -1;
	bool mGPUProfileActive = false;

	struct TimestampQuery
	{
		FString name;
		uint32_t startIndex;
		uint32_t endIndex;
	};

	static constexpr uint32_t MaxOverdrawQueries = 64;
	std::unique_ptr<VulkanQueryPool> mOverdrawPool;
	std::vector<FString> mOverdrawNames;
	std::vector<int> mOverdrawStack;
	std::vector<uint64_t> mOverdrawBarriers;
	uint64_t mOverdrawBarrierStart = 0, mOverdrawGroupBarrierStart = 0, mOverdrawFrameBarriers = 0, mOverdrawFrameBarrierItems = 0, mOverdrawItemStart = 0;
	uint64_t mOverdrawFrame = 0;
	int mOverdrawOpen = 0, mOverdrawNested = 0;
	bool mOverdrawActive = false, mOverdrawPending = false;

	enum { MaxTimestampQueries = 100 };
	std::unique_ptr<VulkanQueryPool> mTimestampQueryPool;
	int mNextTimestampQuery = 0;
	std::vector<size_t> mGroupStack;
	std::vector<TimestampQuery> timeElapsedQueries;
};
