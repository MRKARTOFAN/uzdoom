/*
** vk_descriptorset.h
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
#include <map>
#include <tuple>
#include "tarray.h"

class VulkanRenderDevice;
class VkMaterial;
class PPTextureInput;
class VkPPRenderPassSetup;

class VkDescriptorSetManager
{
public:
	VkDescriptorSetManager(VulkanRenderDevice* fb);
	~VkDescriptorSetManager();

	void Init();
	void Deinit();
	void BeginFrame();
	void UpdateFixedSet();
	void UpdateHWBufferSet();
	void ResetHWTextureSets();

	VulkanDescriptorSetLayout* GetHWBufferSetLayout() { return HWBufferSetLayout.get(); }
	VulkanDescriptorSetLayout* GetFixedSetLayout() { return FixedSetLayout.get(); }
	VulkanDescriptorSetLayout* GetTextureSetLayout(int numLayers);

	VulkanDescriptorSet* GetHWBufferDescriptorSet() { return HWBufferSet.get(); }
	VulkanDescriptorSet* GetFixedDescriptorSet() { return FixedSet.get(); }
	VulkanDescriptorSet* GetNullTextureDescriptorSet();

	std::unique_ptr<VulkanDescriptorSet> AllocateTextureDescriptorSet(int numLayers);

	void TryCreateBindlessTextureSet();
	bool IsBindlessEnabled() const;
	VulkanDescriptorSetLayout* GetBindlessTextureSetLayout() const { return IsBindlessEnabled() ? BindlessTextureSetLayout.get() : nullptr; }
	VulkanDescriptorSet* GetBindlessTextureSet() const { return IsBindlessEnabled() ? BindlessTextureSet.get() : nullptr; }
	int GetBindlessTextureCapacity() const { return IsBindlessEnabled() ? BindlessTextureCapacity : 0; }
	bool IsBindlessTextureReady(int index, VulkanImageView* view, VulkanSampler* sampler, VkImageLayout layout) const;
	void RegisterBindlessTexture(int index, VulkanImageView* view, VulkanSampler* sampler, VkImageLayout layout);
	void UnregisterBindlessTexture(int index);
	void InvalidateBindlessTextures();
	VulkanDescriptorSet* GetGpuWorldTextureSet() const { return GpuWorldTextureSet.get(); }
	VulkanDescriptorSetLayout* GetGpuWorldSetLayout();
	std::unique_ptr<VulkanDescriptorSet> AllocateGpuWorldSet(VulkanBuffer* buffer, VulkanBuffer* references = nullptr, VulkanBuffer* visibility = nullptr,
		VulkanBuffer* surfaceParams = nullptr, VulkanBuffer* params = nullptr, VulkanBuffer* planeMatrices = nullptr,
		VulkanBuffer* planeHeights = nullptr, VulkanBuffer* vertexRefs = nullptr, bool transient = false, VulkanBuffer* surfaceLights = nullptr,
		VulkanBuffer* worldLights = nullptr);
	int GetGpuWorldTexture(VulkanImageView* view, VulkanSampler* sampler, VkImageLayout layout);
	uint64_t GetGpuWorldTextureEpoch() const { return GpuWorldTextureEpoch; }

	VulkanDescriptorSet* GetInput(VkPPRenderPassSetup* passSetup, const TArray<PPTextureInput>& textures, bool bindShadowMapBuffers);

	void AddMaterial(VkMaterial* texture);
	void RemoveMaterial(VkMaterial* texture);

private:
	void CreateHWBufferSetLayout();
	void CreateFixedSetLayout();
	void CreateHWBufferPool();
	void CreateFixedSetPool();
	void CreateBindlessTextureSet(int maxTextures);

	std::unique_ptr<VulkanDescriptorSet> AllocatePPDescriptorSet(VulkanDescriptorSetLayout* layout);

	VulkanRenderDevice* fb = nullptr;

	std::unique_ptr<VulkanDescriptorSetLayout> HWBufferSetLayout;
	std::unique_ptr<VulkanDescriptorSetLayout> FixedSetLayout;
	std::vector<std::unique_ptr<VulkanDescriptorSetLayout>> TextureSetLayouts;

	std::unique_ptr<VulkanDescriptorPool> HWBufferDescriptorPool;
	std::unique_ptr<VulkanDescriptorPool> FixedDescriptorPool;

	std::unique_ptr<VulkanDescriptorPool> PPDescriptorPool;

	int TextureDescriptorSetsLeft = 0;
	int TextureDescriptorsLeft = 0;
	std::vector<std::unique_ptr<VulkanDescriptorPool>> TextureDescriptorPools;

	std::unique_ptr<VulkanDescriptorSet> HWBufferSet;
	std::unique_ptr<VulkanDescriptorSet> FixedSet;
	std::unique_ptr<VulkanDescriptorSet> NullTextureDescriptorSet;

	std::unique_ptr<VulkanDescriptorSetLayout> BindlessTextureSetLayout;
	std::unique_ptr<VulkanDescriptorPool> BindlessTexturePool;
	std::unique_ptr<VulkanDescriptorSet> BindlessTextureSet;
	int BindlessTextureCapacity = 0;
	struct BindlessTextureSlot
	{
		VulkanImageView* View = nullptr;
		VulkanSampler* Sampler = nullptr;
		VkImageLayout Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		uint64_t Epoch = 0;
		bool Compatible = false;
	};
	std::vector<BindlessTextureSlot> BindlessTextureSlots;
	uint64_t BindlessTextureEpoch = 1;
	std::unique_ptr<VulkanDescriptorSet> GpuWorldTextureSet;
	std::unique_ptr<VulkanDescriptorSetLayout> GpuWorldSetLayout;
	std::unique_ptr<VulkanDescriptorPool> GpuWorldPool;
	// [Karto] Per-frame sets (GPU sprites) use their own pool: retiring a full pool frees every set in it, and the shared
	// pool also holds world mode's map-lifetime set.
	std::unique_ptr<VulkanDescriptorPool> TransientGpuWorldPool;
	std::map<std::tuple<VulkanImageView*, VulkanSampler*, VkImageLayout>, uint32_t> GpuWorldTextures;
	uint32_t GpuWorldTextureCount = 0;
	uint64_t GpuWorldTextureEpoch = 1;
	bool BindlessCreationFailed = false;
	int BindlessRegisterCount = 0;

	std::list<VkMaterial*> Materials;

	static const int maxSets = 10;
};
