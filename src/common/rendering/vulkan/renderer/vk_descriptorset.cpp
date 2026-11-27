/*
** vk_descriptorset.cpp
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

#include "vk_descriptorset.h"
#include "vk_streambuffer.h"
#include "vk_raytrace.h"
#include "c_cvars.h"
#include "printf.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "vulkan/textures/vk_texture.h"
#include <zvulkan/vulkanbuilders.h>
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_hwbuffer.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/system/vk_buffer.h"
#include "flatvertices.h"
#include "hw_viewpointuniforms.h"
#include "hw_viewpointbuffer.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "v_2ddrawer.h"

#include "vk_postprocess.h"

#include <algorithm>

CVAR(Bool, vk_gpu_driven_bindless, false, 0)
EXTERN_CVAR(Bool, r_worldmode)

VkDescriptorSetManager::VkDescriptorSetManager(VulkanRenderDevice* fb) : fb(fb)
{
	CreateHWBufferSetLayout();
	CreateFixedSetLayout();
	CreateHWBufferPool();
	CreateFixedSetPool();
}

VkDescriptorSetManager::~VkDescriptorSetManager()
{
	while (!Materials.empty())
		RemoveMaterial(Materials.back());
}

void VkDescriptorSetManager::Init()
{
	UpdateFixedSet();
	UpdateHWBufferSet();

	TryCreateBindlessTextureSet();
	if (vk_gpu_driven_bindless)
		Printf("Vulkan: bindless texture array %s (capacity %d)\n", BindlessTextureSet ? "ready" : "unavailable", BindlessTextureCapacity);
}

bool VkDescriptorSetManager::IsBindlessEnabled() const
{
	return (vk_gpu_driven_bindless || r_worldmode) && fb->DescriptorIndexingSupported && !BindlessCreationFailed;
}

void VkDescriptorSetManager::TryCreateBindlessTextureSet()
{
	if (!IsBindlessEnabled() || BindlessTextureSet) return;
	BindlessCreationFailed = true;
	const auto& props = fb->device->PhysicalDevice.Properties.DescriptorIndexing;
	uint32_t capacity = std::min({ props.maxPerStageDescriptorUpdateAfterBindSampledImages,
		props.maxDescriptorSetUpdateAfterBindSampledImages, props.maxPerStageDescriptorUpdateAfterBindSamplers,
		props.maxDescriptorSetUpdateAfterBindSamplers, props.maxPerStageUpdateAfterBindResources,
		props.maxUpdateAfterBindDescriptorsInAllPools, 16384u });
	auto getSupport = vkGetDescriptorSetLayoutSupport;
	if (!capacity || !getSupport) return;

	VkDescriptorSetLayoutBinding binding = {};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = capacity;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
	VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlags{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
	bindingFlags.bindingCount = 1;
	bindingFlags.pBindingFlags = &flags;
	VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	info.pNext = &bindingFlags;
	info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
	info.bindingCount = 1;
	info.pBindings = &binding;
	VkDescriptorSetVariableDescriptorCountLayoutSupport variable{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT };
	VkDescriptorSetLayoutSupport support{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT };
	support.pNext = &variable;
	getSupport(fb->device->device, &info, &support);
	if (!support.supported || !variable.maxVariableDescriptorCount) return;
	capacity = std::min(capacity, variable.maxVariableDescriptorCount);
	if (capacity <= 64 || props.maxUpdateAfterBindDescriptorsInAllPools < 128) return;
	capacity = std::min(capacity - 64, props.maxUpdateAfterBindDescriptorsInAllPools / 2);

	try
	{
		CreateBindlessTextureSet(int(capacity));
		BindlessCreationFailed = false;
	}
	catch (const CVulkanError& error)
	{
		GpuWorldTextureSet.reset();
		BindlessTextureSet.reset();
		BindlessTexturePool.reset();
		BindlessTextureSetLayout.reset();
		BindlessTextureCapacity = 0;
		BindlessTextureSlots.clear();
		Printf("Vulkan: bindless texture array unavailable: %s\n", error.what());
	}
}

void VkDescriptorSetManager::CreateBindlessTextureSet(int maxTextures)
{
	// [Karto] Partially bound slots are legal only when consumers check readiness.
	VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
		VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
		VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

	DescriptorSetLayoutBuilder layout;
	layout.Flags(VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT);
	layout.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxTextures, VK_SHADER_STAGE_FRAGMENT_BIT, flags);
	layout.DebugName("VkDescriptorSetManager.BindlessTextureSetLayout");
	BindlessTextureSetLayout = layout.Create(fb->device.get());

	BindlessTexturePool = DescriptorPoolBuilder()
		.Flags(VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxTextures * 2)
		.MaxSets(2)
		.DebugName("VkDescriptorSetManager.BindlessTexturePool")
		.Create(fb->device.get());

	BindlessTextureCapacity = maxTextures;
	BindlessTextureSlots.assign(maxTextures, BindlessTextureSlot{});

	BindlessTextureSet = BindlessTexturePool->allocate(BindlessTextureSetLayout.get(), maxTextures);
	BindlessTextureSet->SetDebugName("VkDescriptorSetManager.BindlessTextureSet");
	GpuWorldTextureSet = BindlessTexturePool->allocate(BindlessTextureSetLayout.get(), maxTextures);
}

bool VkDescriptorSetManager::IsBindlessTextureReady(int index, VulkanImageView* view, VulkanSampler* sampler, VkImageLayout layout) const
{
	if (!IsBindlessEnabled() || !BindlessTextureSet || index < 0 || index >= BindlessTextureCapacity) return false;
	const auto& slot = BindlessTextureSlots[index];
	return slot.Epoch == BindlessTextureEpoch && slot.Compatible && slot.View == view && slot.Sampler == sampler && slot.Layout == layout;
}

void VkDescriptorSetManager::RegisterBindlessTexture(int index, VulkanImageView* view, VulkanSampler* sampler, VkImageLayout layout)
{
	TryCreateBindlessTextureSet();
	if (!IsBindlessEnabled() || !BindlessTextureSet || index < 0 || index >= BindlessTextureCapacity || !view || !sampler) return;
	auto& slot = BindlessTextureSlots[index];
	if (slot.Epoch == BindlessTextureEpoch)
	{
		// [Karto] A conflicting variant cannot overwrite a slot an earlier draw may already reference.
		if (slot.View != view || slot.Sampler != sampler || slot.Layout != layout) slot.Compatible = false;
		return;
	}
	WriteDescriptors()
		.AddCombinedImageSampler(BindlessTextureSet.get(), 0, index, view, sampler, layout)
		.Execute(fb->device.get());
	slot.View = view;
	slot.Sampler = sampler;
	slot.Layout = layout;
	slot.Epoch = BindlessTextureEpoch;
	slot.Compatible = true;
	if (BindlessRegisterCount++ == 0)
		Printf("Vulkan: bindless texture slot %d populated (lazy registration active)\n", index);
}

void VkDescriptorSetManager::UnregisterBindlessTexture(int index)
{
	if (index >= 0 && index < int(BindlessTextureSlots.size()))
		BindlessTextureSlots[index].Compatible = false;
}

void VkDescriptorSetManager::InvalidateBindlessTextures()
{
	GpuWorldTextures.clear();
	++GpuWorldTextureEpoch;
	// [Karto] Image teardown may bypass material-set reset; a generation invalidates all borrowed views without a scan.
	if (++BindlessTextureEpoch == 0)
	{
		for (auto& slot : BindlessTextureSlots) slot.Epoch = 0;
		BindlessTextureEpoch = 1;
	}
}

void VkDescriptorSetManager::Deinit()
{
	while (!Materials.empty())
		RemoveMaterial(Materials.back());
}

int VkDescriptorSetManager::GetGpuWorldTexture(VulkanImageView* view, VulkanSampler* sampler, VkImageLayout layout)
{
	if (!IsBindlessEnabled() || !GpuWorldTextureSet || !view || !sampler) return -1;
	auto key = std::make_tuple(view, sampler, layout);
	auto found = GpuWorldTextures.find(key);
	if (found != GpuWorldTextures.end()) return int(found->second);
	if (GpuWorldTextureCount >= uint32_t(BindlessTextureCapacity)) return -1;
	const auto index = GpuWorldTextureCount++;
	WriteDescriptors().AddCombinedImageSampler(GpuWorldTextureSet.get(), 0, index, view, sampler, layout).Execute(fb->device.get());
	GpuWorldTextures.emplace(key, index);
	return int(index);
}

EXTERN_CVAR(Bool, vk_gpu_driven_resident)

VulkanDescriptorSetLayout* VkDescriptorSetManager::GetGpuWorldSetLayout()
{
	if (!GpuWorldSetLayout) GpuWorldSetLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		.AddBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		.AddBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		.AddBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		.AddBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		.AddBinding(8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		.AddBinding(9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.DebugName("GpuIndexedWorld.SetLayout").Create(fb->device.get());
	return GpuWorldSetLayout.get();
}

std::unique_ptr<VulkanDescriptorSet> VkDescriptorSetManager::AllocateGpuWorldSet(VulkanBuffer* buffer, VulkanBuffer* references, VulkanBuffer* visibility,
	VulkanBuffer* surfaceParams, VulkanBuffer* params, VulkanBuffer* planeMatrices, VulkanBuffer* planeHeights, VulkanBuffer* vertexRefs, bool transient,
	VulkanBuffer* surfaceLights, VulkanBuffer* worldLights)
{
	auto& pool = transient ? TransientGpuWorldPool : GpuWorldPool;
	std::unique_ptr<VulkanDescriptorSet> set;
	if (pool) set = pool->tryAllocate(GetGpuWorldSetLayout());
	if (!set)
	{
		fb->GetCommands()->DrawDeleteList->Add(std::move(pool));
		pool = DescriptorPoolBuilder().AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 300).MaxSets(100)
			.DebugName("GpuIndexedWorld.Pool").Create(fb->device.get());
		set = pool->allocate(GetGpuWorldSetLayout());
	}
	auto writes = WriteDescriptors();
	writes.AddBuffer(set.get(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffer);
	if (references) writes.AddBuffer(set.get(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, references);
	if (visibility) writes.AddBuffer(set.get(), 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, visibility);
	if (surfaceParams) writes.AddBuffer(set.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, surfaceParams);
	if (params) writes.AddBuffer(set.get(), 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, params);
	if (planeMatrices) writes.AddBuffer(set.get(), 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, planeMatrices);
	if (planeHeights) writes.AddBuffer(set.get(), 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, planeHeights);
	if (vertexRefs) writes.AddBuffer(set.get(), 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vertexRefs);
	if (surfaceLights) writes.AddBuffer(set.get(), 8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, surfaceLights);
	if (worldLights) writes.AddBuffer(set.get(), 9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, worldLights);
	writes.Execute(fb->device.get());
	return set;
}

void VkDescriptorSetManager::BeginFrame()
{
	// [Karto] Resident rows borrow immutable variants across frames; recycle only at a completed frame boundary.
	if ((!vk_gpu_driven_resident && !r_worldmode) || GpuWorldTextureCount >= uint32_t(BindlessTextureCapacity))
	{
		// [Karto] Rewriting slots from index 0 would retarget textures the parked frame is still sampling.
		if (GpuWorldTextureCount > 0) fb->GetCommands()->WaitForPendingFrame();
		GpuWorldTextures.clear();
		GpuWorldTextureCount = 0;
		++GpuWorldTextureEpoch;
	}
	UpdateFixedSet();
	UpdateHWBufferSet();
}

void VkDescriptorSetManager::UpdateHWBufferSet()
{
	fb->GetCommands()->DrawDeleteList->Add(std::move(HWBufferSet));

	HWBufferSet = HWBufferDescriptorPool->tryAllocate(HWBufferSetLayout.get());
	if (!HWBufferSet)
	{
		fb->GetCommands()->WaitForCommands(false);
		HWBufferSet = HWBufferDescriptorPool->allocate(HWBufferSetLayout.get());
	}

	// [Karto] Bind the frame slot's copies: the buffer manager's binding-point pointers name the copy created last.
	auto viewpoints = fb->mViewpoints ? static_cast<VkHardwareDataBuffer*>(fb->mViewpoints->GetBuffer()) : fb->GetBufferManager()->ViewpointUBO;
	auto lights = fb->mLights ? static_cast<VkHardwareDataBuffer*>(fb->mLights->GetBuffer()) : fb->GetBufferManager()->LightBufferSSO;
	auto bones = fb->mBones ? static_cast<VkHardwareDataBuffer*>(fb->mBones->GetBuffer()) : fb->GetBufferManager()->BoneBufferSSO;
	WriteDescriptors()
		.AddBuffer(HWBufferSet.get(), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, viewpoints->mBuffer.get(), 0, sizeof(HWViewpointUniforms))
		.AddBuffer(HWBufferSet.get(), 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, fb->GetBufferManager()->MatrixBuffer->UniformBuffer->mBuffer.get(), 0, sizeof(MatricesUBO))
		.AddBuffer(HWBufferSet.get(), 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, fb->GetBufferManager()->StreamBuffer->UniformBuffer->mBuffer.get(), 0, sizeof(StreamUBO))
		.AddBuffer(HWBufferSet.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, lights->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bones->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<VkHardwareDataBuffer*>(fb->GetBufferManager()->RetainedParameters.get())->mBuffer.get())
		.AddBuffer(HWBufferSet.get(), 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<VkHardwareDataBuffer*>(fb->GetBufferManager()->SectorAppearance.get())->mBuffer.get())
		.Execute(fb->device.get());
}

void VkDescriptorSetManager::UpdateFixedSet()
{
	fb->GetCommands()->DrawDeleteList->Add(std::move(FixedSet));

	FixedSet = FixedDescriptorPool->tryAllocate(FixedSetLayout.get());
	if (!FixedSet)
	{
		fb->GetCommands()->WaitForCommands(false);
		FixedSet = FixedDescriptorPool->allocate(FixedSetLayout.get());
	}

	WriteDescriptors update;
	update.AddCombinedImageSampler(FixedSet.get(), 0, fb->GetTextureManager()->Shadowmap.View.get(), fb->GetSamplerManager()->ShadowmapSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	update.AddCombinedImageSampler(FixedSet.get(), 1, fb->GetTextureManager()->Lightmap.View.get(), fb->GetSamplerManager()->LightmapSampler.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	if (fb->RaytracingEnabled())
		update.AddAccelerationStructure(FixedSet.get(), 2, fb->GetRaytrace()->GetAccelStruct());
	update.Execute(fb->device.get());
}

void VkDescriptorSetManager::ResetHWTextureSets()
{
	for (auto mat : Materials)
		mat->DeleteDescriptors();

	auto deleteList = fb->GetCommands()->DrawDeleteList.get();
	for (auto& desc : TextureDescriptorPools)
	{
		deleteList->Add(std::move(desc));
	}
	deleteList->Add(std::move(NullTextureDescriptorSet));

	TextureDescriptorPools.clear();
	TextureDescriptorSetsLeft = 0;
	TextureDescriptorsLeft = 0;

	InvalidateBindlessTextures();
}

VulkanDescriptorSet* VkDescriptorSetManager::GetNullTextureDescriptorSet()
{
	if (!NullTextureDescriptorSet)
	{
		NullTextureDescriptorSet = AllocateTextureDescriptorSet(SHADER_MIN_REQUIRED_TEXTURE_LAYERS);

		WriteDescriptors update;
		for (int i = 0; i < SHADER_MIN_REQUIRED_TEXTURE_LAYERS; i++)
		{
			update.AddCombinedImageSampler(NullTextureDescriptorSet.get(), i, fb->GetTextureManager()->GetNullTextureView(), fb->GetSamplerManager()->Get(CLAMP_XY_NOMIP), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		update.Execute(fb->device.get());
	}

	return NullTextureDescriptorSet.get();
}

std::unique_ptr<VulkanDescriptorSet> VkDescriptorSetManager::AllocateTextureDescriptorSet(int numLayers)
{
	if (TextureDescriptorSetsLeft == 0 || TextureDescriptorsLeft < numLayers)
	{
		TextureDescriptorSetsLeft = 1000;
		TextureDescriptorsLeft = 2000;

		TextureDescriptorPools.push_back(DescriptorPoolBuilder()
			.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TextureDescriptorsLeft)
			.MaxSets(TextureDescriptorSetsLeft)
			.DebugName("VkDescriptorSetManager.TextureDescriptorPool")
			.Create(fb->device.get()));
	}

	TextureDescriptorSetsLeft--;
	TextureDescriptorsLeft -= numLayers;
	return TextureDescriptorPools.back()->allocate(GetTextureSetLayout(numLayers));
}

VulkanDescriptorSetLayout* VkDescriptorSetManager::GetTextureSetLayout(int numLayers)
{
	if (TextureSetLayouts.size() < (size_t)numLayers)
		TextureSetLayouts.resize(numLayers);

	auto& layout = TextureSetLayouts[numLayers - 1];
	if (layout)
		return layout.get();

	DescriptorSetLayoutBuilder builder;
	for (int i = 0; i < numLayers; i++)
	{
		builder.AddBinding(i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	}
	builder.DebugName("VkDescriptorSetManager.TextureSetLayout");
	layout = builder.Create(fb->device.get());
	return layout.get();
}

void VkDescriptorSetManager::AddMaterial(VkMaterial* texture)
{
	texture->it = Materials.insert(Materials.end(), texture);
}

void VkDescriptorSetManager::RemoveMaterial(VkMaterial* texture)
{
	texture->DeleteDescriptors();
	texture->fb = nullptr;
	Materials.erase(texture->it);
}

VulkanDescriptorSet* VkDescriptorSetManager::GetInput(VkPPRenderPassSetup* passSetup, const TArray<PPTextureInput>& textures, bool bindShadowMapBuffers)
{
	auto descriptors = AllocatePPDescriptorSet(passSetup->DescriptorLayout.get());
	descriptors->SetDebugName("VkPostprocess.descriptors");

	WriteDescriptors write;
	VkImageTransition imageTransition;

	for (unsigned int index = 0; index < textures.Size(); index++)
	{
		const PPTextureInput& input = textures[index];
		VulkanSampler* sampler = fb->GetSamplerManager()->Get(input.Filter, input.Wrap);
		VkTextureImage* tex = fb->GetTextureManager()->GetTexture(input.Type, input.Texture);

		write.AddCombinedImageSampler(descriptors.get(), index, tex->DepthOnlyView ? tex->DepthOnlyView.get() : tex->View.get(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		imageTransition.AddImage(tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
	}

	// Bind automatic uniforms buffer
	write.AddBuffer(descriptors.get(), AUTOMATIC_UNIFORMS_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, fb->GetPostprocess()->GetAutomaticUniformsBuffer());

	if (bindShadowMapBuffers)
	{
		write.AddBuffer(descriptors.get(), LIGHTNODES_BINDINGPOINT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetBufferManager()->LightNodes->mBuffer.get());
		write.AddBuffer(descriptors.get(), LIGHTLINES_BINDINGPOINT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetBufferManager()->LightLines->mBuffer.get());
		write.AddBuffer(descriptors.get(), LIGHTLIST_BINDINGPOINT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, fb->GetBufferManager()->LightList->mBuffer.get());
	}

	write.Execute(fb->device.get());
	imageTransition.Execute(fb->GetCommands()->GetDrawCommands());

	VulkanDescriptorSet* set = descriptors.get();
	fb->GetCommands()->DrawDeleteList->Add(std::move(descriptors));
	return set;
}

std::unique_ptr<VulkanDescriptorSet> VkDescriptorSetManager::AllocatePPDescriptorSet(VulkanDescriptorSetLayout* layout)
{
	if (PPDescriptorPool)
	{
		auto descriptors = PPDescriptorPool->tryAllocate(layout);
		if (descriptors)
			return descriptors;

		fb->GetCommands()->DrawDeleteList->Add(std::move(PPDescriptorPool));
	}

	PPDescriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 200)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 100)  // For automatic uniforms
		.MaxSets(100)
		.DebugName("PPDescriptorPool")
		.Create(fb->device.get());

	return PPDescriptorPool->allocate(layout);
}

void VkDescriptorSetManager::CreateHWBufferSetLayout()
{
	HWBufferSetLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT)
		.AddBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		.AddBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
		.DebugName("VkDescriptorSetManager.HWBufferSetLayout")
		.Create(fb->device.get());
}

void VkDescriptorSetManager::CreateFixedSetLayout()
{
	DescriptorSetLayoutBuilder builder;
	builder.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	builder.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	if (fb->RaytracingEnabled())
		builder.AddBinding(2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT);
	builder.DebugName("VkDescriptorSetManager.FixedSetLayout");
	FixedSetLayout = builder.Create(fb->device.get());
}

void VkDescriptorSetManager::CreateHWBufferPool()
{
	HWBufferDescriptorPool = DescriptorPoolBuilder()
		.AddPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * maxSets)
		.AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * maxSets)
		.MaxSets(maxSets)
		.DebugName("VkDescriptorSetManager.HWBufferDescriptorPool")
		.Create(fb->device.get());
}

void VkDescriptorSetManager::CreateFixedSetPool()
{
	DescriptorPoolBuilder poolbuilder;
	poolbuilder.AddPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * maxSets);
	if (fb->RaytracingEnabled())
		poolbuilder.AddPoolSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 * maxSets);
	poolbuilder.MaxSets(maxSets);
	poolbuilder.DebugName("VkDescriptorSetManager.FixedDescriptorPool");
	FixedDescriptorPool = poolbuilder.Create(fb->device.get());
}
