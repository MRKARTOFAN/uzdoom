/*
** vk_renderstate.cpp
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

#include "vk_renderstate.h"
#include <algorithm>
#include "vulkan/system/vk_renderdevice.h"
#include "printf.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/system/vk_buffer.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_descriptorset.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_hwtexture.h"

#include "hw_skydome.h"
#include "hw_viewpointuniforms.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "c_dispatch.h"
#include "hw_cvars.h"
#include "hw_clock.h"
#include "flatvertices.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "hwrenderer/data/shaderuniforms.h"

CVAR(Int, vk_submit_size, 1000, 0);
CVAR(Bool, vk_drawreuse, true, 0);
static uint64_t ModelInstanceDraws, ModelInstances, ModelInstanceFrames;
CVAR(Bool, vk_wallbatch, false, 0);
CVAR(Bool, vk_retainedparams, false, 0);
CVAR(Bool, vk_retainedlighting, false, 0);
CVAR(Bool, vk_worldcontext, false, 0);
CVAR(Bool, vk_sharedappearance, false, 0);
EXTERN_CVAR(Bool, vk_gpu_driven_cull);
EXTERN_CVAR(Bool, vk_gpu_driven_geometry);
EXTERN_CVAR(Bool, vk_gpu_driven_candidate);
EXTERN_CVAR(Bool, vk_gpu_driven_debug);
EXTERN_CVAR(Bool, vk_retainedworld_verify)
EXTERN_CVAR(Bool, vk_ownedstate)
EXTERN_CVAR(Bool, vk_gpu_driven_compat)
EXTERN_CVAR(Bool, r_skipmats)
EXTERN_CVAR(Bool, r_flatindirect)

VkRenderState::VkRenderState(VulkanRenderDevice* fb) : fb(fb), mStreamBufferWriter(fb), mMatrixBufferWriter(fb)
{
	Reset();
}

void VkRenderState::ClearScreen()
{
	screen->mViewpoints->Set2D(*this, SCREENWIDTH, SCREENHEIGHT);
	SetColor(0, 0, 0);
	FlushQuadBatch();
	Apply(DT_TriangleStrip);
	if (SceneProfiling) ++SceneMainSurfaceProfile.GPUDraws;
	mCommandBuffer->draw(4, 1, FFlatVertexBuffer::FULLSCREEN_INDEX, 0);
}

void VkRenderState::Draw(int dt, int index, int count, bool apply)
{
	if (mWorldCapture && CaptureWorldDraw(dt, index, count, false)) return;
	if (SceneProfiling && mRetainedBatchCount) ++SceneMainSurfaceProfile.RetainedHelperBoundaries;
	EndRetainedWallRun();
	FlushQuadBatch();
	if (apply || mNeedApply)
		Apply(dt);

	if (SceneProfiling) ++SceneMainSurfaceProfile.GPUDraws;
	mCommandBuffer->draw(count, 1, index, 0);
}

void VkRenderState::DrawIndexed(int dt, int index, int count, bool apply)
{
	if (mWorldCapture && CaptureWorldDraw(dt, index, count, true)) return;
	if (SceneProfiling && mRetainedBatchCount) ++SceneMainSurfaceProfile.RetainedHelperBoundaries;
	EndRetainedWallRun();
	FlushQuadBatch();
	if (apply || mNeedApply)
		Apply(dt);

	if (SceneProfiling) ++SceneMainSurfaceProfile.GPUDraws;
	mCommandBuffer->drawIndexed(count, 1, index, 0, 0);
}

void VkRenderState::DrawRetainedQuad(int index)
{
	// [Karto] A bake must see every wall as an ordinary draw, never as a retained batch entry.
	if (mWorldCapture) { Draw(DT_TriangleFan, index, 4); return; }
	const uint32_t indices[] = { uint32_t(index), uint32_t(index + 1), uint32_t(index + 2), uint32_t(index), uint32_t(index + 2), uint32_t(index + 3) };
	if (index < 0 || index / 4 >= 32768 || !GatherRetained(indices, 6, index / 4))
		Draw(DT_TriangleFan, index, 4);
	else if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedWallQuads;
}

void VkRenderState::DrawOwnedRetainedQuad(int index, uint64_t version)
{
	if (mWorldCapture) { Draw(DT_TriangleFan, index, 4); return; }
	const uint32_t indices[] = { uint32_t(index), uint32_t(index + 1), uint32_t(index + 2), uint32_t(index), uint32_t(index + 2), uint32_t(index + 3) };
	if (index < 0 || index / 4 >= 32768 || !GatherRetained(indices, 6, index / 4, version))
		Draw(DT_TriangleFan, index, 4);
	else if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedWallQuads;
}

void VkRenderState::PrepareRetainedWallRun(bool changed, bool owned)
{
	if (!owned && (!vk_worldcontext || (!vk_retainedparams && !mOwnedRetainedView)))
	{
		EndRetainedWallRun();
		return;
	}
	if (mRetainedWallRun && !changed) return;
	HWSceneScopeTimer timer(SceneMainSurfaceProfile.RetainedContextNS);
	if (owned)
	{
		mRetainedDrawing = true;
		ApplyStreamData();
		mRetainedDrawing = false;
	}
	mStreamData.useVertexData = fb->GetRenderPassManager()->GetVertexFormat(
		static_cast<VkHardwareVertexBuffer*>(mVertexBuffer)->VertexFormat)->UseVertexData;
	static const VSMatrix identity(1);
	mRetainedWallSnapshot = MakeRetainedWallContext(mStreamData,
		mTextureMatrixEnabled ? mTextureMatrix : identity, MakePushConstants());
	mRetainedWallContext = mRetainedContexts.Find(mRetainedWallSnapshot);
	mRetainedWallRun = true;
	if (SceneProfiling)
	{
		++SceneMainSurfaceProfile.RetainedContextSnapshots;
		if (!mRetainedWallContext) ++SceneMainSurfaceProfile.RetainedContextFull;
		SceneMainSurfaceProfile.RetainedContextBytes = mRetainedContexts.Memory();
	}
}

bool VkRenderState::SetRetainedWorldOwnership(bool enabled)
{
	const bool previous = mOwnedRetainedView;
	if (enabled != previous) EndRetainedWallRun();
	mOwnedRetainedView = enabled;
	return previous;
}

void VkRenderState::EndRetainedWallRun()
{
	if (!mRetainedWallRun) return;
	mRetainedWallRun = false;
	mRetainedWallContext = 0;
}

void VkRenderState::DrawRetainedTriangles(int index, int count, unsigned surface)
{
	if (mWorldCapture) { DrawIndexed(DT_Triangles, index, count); return; }
	auto& indices = screen->mVertexData->ibo_data;
	if (index < 0 || count <= 0 || uint64_t(index) + count > indices.Size() ||
		!GatherRetained(&indices[index], count, surface))
		DrawIndexed(DT_Triangles, index, count);
	else if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedFlatDraws;
}

uint64_t VkRenderState::HashSharedAppearance(const FSharedAppearanceKey& key)
{
	const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&key);
	uint64_t hash = 1469598103934665603ull;
	for (size_t i = 0; i < sizeof(key); ++i)
	{
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

bool VkRenderState::SelectSharedAppearance(const FSharedAppearanceKey& key)
{
	if (!vk_sharedappearance) return false;
	auto found = mSharedAppearances.find(HashSharedAppearance(key));
	if (found != mSharedAppearances.end() && found->second.first == key)
	{
		SetAppearanceIndex(found->second.second);
		return true;
	}
	return false;
}

void VkRenderState::StoreSharedAppearance(const FSharedAppearanceKey& key)
{
	if (!vk_sharedappearance) return;
	const unsigned index = mSharedAppearanceCount + 1;
	if (index >= kSharedAppearanceCapacity)
	{
		SetAppearanceIndex(0);
		return;
	}
	const PushConstants push = MakePushConstants();
	SectorAppearanceRow row = {};
	row.FogColor = mStreamData.uFogColor;
	row.VertexColor = mStreamData.uVertexColor;
	row.Desaturation = mStreamData.uDesaturationFactor;
	row.LightDist = push.uLightDist;
	row.LightFactor = push.uLightFactor;
	row.FogDensity = push.uFogDensity;
	row.LightLevel = push.uLightLevel;
	row.FogEnabled = push.uFogEnabled;
	static_cast<SectorAppearanceRow*>(fb->GetBufferManager()->SectorAppearance->Memory())[index] = row;
	mSharedAppearances[HashSharedAppearance(key)] = { key, index };
	++mSharedAppearanceCount;
	SetAppearanceIndex(index);
}

void VkRenderState::UploadCoverage(const void* data, size_t bytes)
{
	if (!vk_gpu_driven_compat || !data || !bytes) return;
	auto buffer = fb->GetBufferManager()->Coverage.get();
	if (bytes > mCoverageBytes)
	{
		buffer->SetData(bytes, nullptr, BufferUsageType::Persistent);
		mCoverageBytes = bytes;
	}
	if (buffer->Memory()) memcpy(buffer->Memory(), data, bytes);
}

void VkRenderState::UploadStaticBake(const void* verts, size_t vertBytes, const void* indices, size_t indexBytes, const void* table, size_t tableBytes)
{
	// [Karto] Replacing buffers behind stable wrapper pointers must invalidate cached bindings before another view can draw.
	EndRenderPass();
	auto buffers = fb->GetBufferManager();
	if (verts && vertBytes) buffers->StaticGeometry->SetData(vertBytes, verts, BufferUsageType::Persistent);
	if (indices && indexBytes) buffers->StaticIndices->SetData(indexBytes, indices, BufferUsageType::Persistent);
	if (table && tableBytes) buffers->PrimitiveTable->SetData(tableBytes, table, BufferUsageType::Persistent);
	if (verts && vertBytes && indices && indexBytes) buffers->UploadStaticBakeGeometry(verts, vertBytes, indices, indexBytes);
}

void VkRenderState::UploadPrimitiveGrid(const void* offsets, size_t offsetBytes, const void* refs, size_t refBytes)
{
	auto buffers = fb->GetBufferManager();
	if (offsets && offsetBytes) buffers->PrimitiveGrid->SetData(offsetBytes, offsets, BufferUsageType::Persistent);
	if (refs && refBytes) buffers->PrimitiveRefs->SetData(refBytes, refs, BufferUsageType::Persistent);
}

void VkRenderState::EnsureGpuCullResources()
{
	if (mGpuCullReady) return;
	auto device = fb->device.get();
	const auto& limits = device->PhysicalDevice.Properties.Properties.limits;
	if (limits.maxPerStageDescriptorStorageBuffers < 8 || limits.maxDescriptorSetStorageBuffers < 8 ||
		limits.maxPerStageResources < 8 || limits.maxComputeWorkGroupInvocations < 64 ||
		limits.maxComputeWorkGroupSize[0] < 64 || limits.maxDrawIndirectCount == 0)
		VulkanError("GPU-Driven cull exceeds device limits");

	mGpuCullShader = ShaderBuilder()
		.Type(ShaderType::Compute)
		.AddSource("gpu_cull", R"(#version 450
layout(local_size_x = 64) in;
layout(push_constant) uniform PC { vec4 Planes[6]; uint PrimCount; uint ForDraw; uint CommandCount; } pc;
layout(std430, set = 0, binding = 0) buffer Visible { uint Indices[]; };
layout(std430, set = 0, binding = 1) buffer Args { uint IndexCount; uint InstanceCount; uint FirstIndex; int VertexOffset; uint FirstInstance; };
layout(std430, set = 0, binding = 2) readonly buffer Bounds { vec4 MinMax[]; };
layout(std430, set = 0, binding = 3) readonly buffer PrimTable { uint Data[]; };
layout(std430, set = 0, binding = 4) writeonly buffer Commands { uint Cmd[]; };
layout(std430, set = 0, binding = 5) readonly buffer PrimMats { uint Mat[]; };
layout(std430, set = 0, binding = 6) writeonly buffer DrawMats { uint DMat[]; };
layout(std430, set = 0, binding = 7) readonly buffer Admission { uint Allowed[]; };
void main()
{
	uint i = gl_GlobalInvocationID.x;
	if (pc.ForDraw >= 2u)
	{
		if (i >= pc.CommandCount) return;
		uint row = Allowed[i];
		uint base = i * 5u;
		Cmd[base] = 0u; Cmd[base + 1u] = 0u; Cmd[base + 2u] = 0u; Cmd[base + 3u] = 0u; Cmd[base + 4u] = 0u;
		if (row >= pc.PrimCount || Data[row * 8u + 7u] != 1u || Data[row * 8u + 1u] != 4u || Data[row * 8u + 3u] != 6u) return;
		Cmd[base] = 6u;
		Cmd[base + 3u] = Data[row * 8u];
		vec3 mn = MinMax[row * 2u].xyz;
		vec3 mx = MinMax[row * 2u + 1u].xyz;
		for (int p = 0; p < 6; ++p)
		{
			vec3 n = pc.Planes[p].xyz;
			vec3 q = mix(mn, mx, greaterThanEqual(n, vec3(0.0)));
			if (dot(n, q) + pc.Planes[p].w < 0.0) return;
		}
		Cmd[base + 1u] = 1u;
		if (pc.ForDraw == 2u)
		{
			Indices[atomicAdd(InstanceCount, 1u)] = row;
		}
		return;
	}
	if (i >= pc.PrimCount) return;
	uint base = i * 5u;
	bool wall = Data[i * 8u + 7u] == 1u && Data[i * 8u + 1u] == 4u;
	Cmd[base + 0u] = wall ? 6u : 0u;
	Cmd[base + 1u] = 0u;
	Cmd[base + 2u] = 0u;
	Cmd[base + 3u] = wall ? Data[i * 8u + 0u] : 0u;
	Cmd[base + 4u] = 0u;
	vec3 mn = MinMax[i * 2].xyz;
	vec3 mx = MinMax[i * 2 + 1].xyz;
	bool vis = true;
	for (int p = 0; p < 6; ++p)
	{
		vec3 n = pc.Planes[p].xyz;
		vec3 q = vec3(n.x >= 0.0 ? mx.x : mn.x, n.y >= 0.0 ? mx.y : mn.y, n.z >= 0.0 ? mx.z : mn.z);
		if (dot(n, q) + pc.Planes[p].w < 0.0) { vis = false; break; }
	}
	if (vis)
	{
		uint slot = atomicAdd(InstanceCount, 1u);
		Indices[slot] = i;
		if (wall && pc.ForDraw != 0u && Allowed[i] != 0u) Cmd[base + 1u] = 1u;
		DMat[i * 4u + 0u] = Mat[i * 4u + 0u];
		DMat[i * 4u + 1u] = Mat[i * 4u + 1u];
		DMat[i * 4u + 2u] = Mat[i * 4u + 2u];
		DMat[i * 4u + 3u] = Mat[i * 4u + 3u];
	}
}
)")
		.DebugName("GpuCull")
		.Create("gpu_cull", device);

	mGpuCullSetLayout = DescriptorSetLayoutBuilder()
		.AddBinding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.AddBinding(7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT)
		.DebugName("GpuCullSetLayout")
		.Create(device);

	mGpuCullPipelineLayout = PipelineLayoutBuilder()
		.AddSetLayout(mGpuCullSetLayout.get())
		.AddPushConstantRange(VK_SHADER_STAGE_COMPUTE_BIT, 0, 6 * sizeof(float) * 4 + 3 * sizeof(uint32_t))
		.DebugName("GpuCullPipelineLayout")
		.Create(device);

	mGpuCullPipeline = ComputePipelineBuilder()
		.Layout(mGpuCullPipelineLayout.get())
		.ComputeShader(mGpuCullShader.get())
		.DebugName("GpuCullPipeline")
		.Create(device);

	mGpuCullReady = true;
}

void VkRenderState::UploadPrimitiveBounds(const void* bounds, size_t bytes)
{
	if (bounds && bytes) fb->GetBufferManager()->PrimitiveBounds->SetData(bytes, bounds, BufferUsageType::Persistent);
}

void VkRenderState::UploadPrimitiveMaterials(const void* rows, size_t bytes)
{
	if (rows && bytes) fb->GetBufferManager()->PrimitiveMaterials->SetData(bytes, rows, BufferUsageType::Persistent);
}

// [Karto] Admission storage must outlive recorded consumers, including earlier views in this frame.
void VkRenderState::UploadGpuCullAdmission(const void* rows, size_t bytes)
{
	try
	{
		if (rows && bytes)
		{
			auto buffer = fb->GetBufferManager()->GpuCullAdmission.get();
			// [Karto] BeginFrame follows the final completion fence; only its first upload may reuse mapped storage.
			if (mGpuAdmissionUsedThisFrame || buffer->Size() < bytes || !buffer->Memory())
				buffer->SetData((bytes + 4095) & ~size_t(4095), nullptr, BufferUsageType::Persistent);
			memcpy(buffer->Memory(), rows, bytes);
			mGpuAdmissionUsedThisFrame = true;
		}
	}
	catch (const CVulkanError& error)
	{
		mGpuCullFailed = true;
		Printf("GPU-Driven cull unavailable: %s\n", error.what());
	}
}

void VkRenderState::SetGpuCullPlanes(const float* planes24, uint32_t primCount, uint32_t expectedVisible, bool verify, bool forDraw, uint32_t commandCount)
{
	mCullDrawReady = false;
	mCullPending = false;
	if (!planes24 || primCount == 0) return;
	memcpy(mCullPlanes, planes24, sizeof(mCullPlanes));
	mCullPrims = primCount;
	mCullExpected = expectedVisible;
	mCullVerify = verify;
	mCullForDraw = forDraw;
	mCullCommandCount = commandCount;
	mCullPending = true;
}

// [Karto] The previous buffer bindings are restored so the surrounding wall run keeps drawing from its own vertex stream.
bool VkRenderState::DrawBakedPrimitive(uint32_t iboOffset, uint32_t indexCount)
{
	auto buffers = fb->GetBufferManager();
	if (!buffers->StaticGeometryVB || !buffers->StaticIndicesIB || !indexCount ||
		size_t(iboOffset) + indexCount > buffers->StaticIndicesIB->Size() / sizeof(uint32_t)) return false;
	IVertexBuffer* savedVB = mVertexBuffer;
	IVertexBuffer* savedSecondary = mSecondaryVertexBuffer;
	const int savedOffsets[2] = { mVertexOffsets[0], mVertexOffsets[1] };
	IIndexBuffer* savedIB = mIndexBuffer;
	SetVertexBuffer(buffers->StaticGeometryVB.get(), 0, 0, buffers->StaticSurfaceID.get());
	SetIndexBuffer(buffers->StaticIndicesIB.get());
	DrawIndexed(DT_Triangles, iboOffset, indexCount);
	mVertexBuffer = savedVB;
	mSecondaryVertexBuffer = savedSecondary;
	mVertexOffsets[0] = savedOffsets[0];
	mVertexOffsets[1] = savedOffsets[1];
	mIndexBuffer = savedIB;
	return true;
}

EXTERN_CVAR(Bool, vk_gpu_driven_bindless)
CVAR(Bool, vk_gpu_driven_indexed, false, 0)
CVAR(Bool, vk_gpu_driven_resident, false, 0)

bool VkRenderState::BeginGpuDrivenWalls()
{
	mGpuIndexedView = false;
	mGpuResidentView = false;
	mGpuResidentLast = UINT32_MAX;
	if (vk_gpu_driven_indexed && vk_gpu_driven_bindless && fb->ShaderDrawParametersSupported && mCullCommandCount)
	{
		auto descriptors = fb->GetDescriptorSetManager();
		const auto& limits = fb->device->PhysicalDevice.Properties.Properties.limits;
		const size_t bytes = size_t(mCullCommandCount) * sizeof(GpuWorldSurfaceData);
		if (bytes <= limits.maxStorageBufferRange && limits.maxPerStageDescriptorStorageBuffers >= 5 && limits.maxDescriptorSetStorageBuffers >= 5 && limits.maxBoundDescriptorSets >= 4)
		{
			try
			{
				descriptors->TryCreateBindlessTextureSet();
				if (descriptors->GetGpuWorldTextureSet() && descriptors->IsBindlessEnabled())
				{
					auto buffer = fb->GetBufferManager()->GpuWorldAppearance.get();
					if (mGpuAppearanceUsedThisFrame || buffer->Size() < bytes || !buffer->Memory()) buffer->SetData(bytes, nullptr, BufferUsageType::Persistent);
					fb->GetCommands()->DrawDeleteList->Add(std::move(mGpuWorldSet));
					mGpuWorldSet = descriptors->AllocateGpuWorldSet(static_cast<VkHardwareDataBuffer*>(buffer)->mBuffer.get());
					mGpuAppearanceUsedThisFrame = true;
					mGpuIndexedView = true;
					const size_t residentBytes = size_t(GpuResidentCapacity) * sizeof(GpuWorldSurfaceData);
					if (vk_gpu_driven_resident && residentBytes <= limits.maxStorageBufferRange &&
						limits.maxPerStageDescriptorStorageBuffers >= 6 && limits.maxDescriptorSetStorageBuffers >= 6)
					{
						auto resident = fb->GetBufferManager()->GpuResidentAppearance.get();
						auto refs = fb->GetBufferManager()->GpuResidentReferences.get();
						// [Karto] Recorded earlier views must keep immutable rows even when a later view refreshes the same
						// identity.
						if (!resident->Memory() || mGpuResidentUsedThisFrame)
						{
							resident->SetData(residentBytes, nullptr, BufferUsageType::Persistent);
							if (!mGpuWorldResident.empty()) memcpy(resident->Memory(), mGpuWorldResident.data(), mGpuWorldResident.size() * sizeof(GpuWorldSurfaceData));
						}
						const size_t refBytes = size_t(mCullCommandCount) * sizeof(uint32_t);
						if (!refs->Memory() || refs->Size() < refBytes || mGpuResidentUsedThisFrame) refs->SetData(refBytes, nullptr, BufferUsageType::Persistent);
						fb->GetCommands()->DrawDeleteList->Add(std::move(mGpuResidentSet));
						mGpuResidentSet = descriptors->AllocateGpuWorldSet(static_cast<VkHardwareDataBuffer*>(resident)->mBuffer.get(), static_cast<VkHardwareDataBuffer*>(refs)->mBuffer.get());
						mGpuResidentView = mGpuResidentUsedThisFrame = true;
					}
				}
			}
			catch (const CVulkanError& error) { Printf("Gpu indexed world unavailable: %s\n", error.what()); }
		}
	}
	if (!vk_gpu_driven_candidate || !vk_gpu_driven_geometry || !vk_gpu_driven_cull ||
		!mCullPending || !mCullForDraw || !mCullCommandCount) return false;
	EndRenderPass();
	RecordGpuCull();
	mGpuWallsActive = mCullDrawReady;
	if (mGpuWallsActive)
	{
		static bool logged = false;
		if (!logged) { logged = true; Printf("G37USE slots=%u current_view=1 shaded=1 replacement=1\n", mCullCommandCount); }
	}
	return mGpuWallsActive;
}

void VkRenderState::CaptureGpuStaticMaterial(FGpuStaticMaterial& out)
{
	// [Karto] Refreshed every frame the wall is drawn so scripted texture transforms and mode changes are picked up, not
	// frozen.
	out.Valid = false;
	if (!mMaterial.mMaterial || mSpecialEffect != EFF_NONE || mMaterial.mOverrideShader > 0) return;
	if (!static_cast<VkMaterial*>(mMaterial.mMaterial)->GetGpuWorldTextures(mMaterial, out.TextureIDs)) return;
	static const VSMatrix identity(1);
	memcpy(out.TextureMatrix, (mTextureMatrixEnabled ? mTextureMatrix : identity).get(), sizeof(out.TextureMatrix));
	const auto push = MakePushConstants();
	out.LightDistFactor[0] = push.uLightDist;
	out.LightDistFactor[1] = push.uLightFactor;
	out.Specular[0] = push.uSpecularMaterial.X;
	out.Specular[1] = push.uSpecularMaterial.Y;
	out.AlphaThreshold = push.uAlphaThreshold;
	out.TextureMode = push.uTextureMode;
	out.FogEnabled = push.uFogEnabled;
	out.ShaderSpeed = static_cast<float>(mMaterial.mMaterial->Source()->GetShaderSpeed());
	out.Valid = true;
}

bool VkRenderState::DrawGpuDrivenWall(uint32_t slot, uint32_t bakeRow)
{
	if (!mGpuWallsActive || slot >= mCullCommandCount || mSpecialEffect != EFF_NONE ||
		!mMaterial.mMaterial || mMaterial.mMaterial->GetShaderIndex() != 0 || mMaterial.mOverrideShader > 0) return false;
	if (mGpuIndexedView) return DrawGpuIndexedWall(slot, bakeRow);
	auto buffers = fb->GetBufferManager();
	if (!buffers->StaticGeometryVB || !buffers->StaticSurfaceID || !buffers->GpuQuadIndices) return false;
	if (mGpuWallBatchCount && slot != mGpuWallBatchStart + mGpuWallBatchCount) FlushQuadBatch();
	auto savedVB = mVertexBuffer;
	auto savedSecondary = mSecondaryVertexBuffer;
	const int offsets[2] = { mVertexOffsets[0], mVertexOffsets[1] };
	auto savedIB = mIndexBuffer;
	SetVertexBuffer(buffers->StaticGeometryVB.get(), 0, 0, buffers->StaticSurfaceID.get());
	SetIndexBuffer(buffers->GpuQuadIndices.get());
	AdvanceSubmissionBudget();
	ApplyStreamData();
	ApplyMatrices();
	const auto key = MakePipelineKey(DT_Triangles);
	const auto push = MakePushConstants();
	const bool reuse = vk_drawreuse && mGpuWallBatchCount && mCommandBuffer && key == mPipelineKey &&
		!mScissorChanged && !mViewportChanged && !mStencilRefChanged && !mBias.mChanged &&
		mViewpointOffset == mLastViewpointOffset && mMatrixBufferWriter.Offset() == mLastMatricesOffset &&
		mStreamBufferWriter.StreamDataOffset() == mLastStreamDataOffset &&
		mLastBufferLayers == key.NumTextureLayers && mLastPushLayers == key.NumTextureLayers &&
		mPushConstantsValid && memcmp(&push, &mLastPushConstants, sizeof(push)) == 0 &&
		mVertexBuffer == mLastVertexBuffer && mSecondaryVertexBuffer == mLastSecondaryVertexBuffer &&
		mIndexBuffer == mLastIndexBuffer && mVertexOffsets[0] == mLastVertexOffsets[0] && mVertexOffsets[1] == mLastVertexOffsets[1] &&
		mMaterial.mMaterial == mGpuWallMaterial.mMaterial && mMaterial.mClampMode == mGpuWallMaterial.mClampMode &&
		mMaterial.mTranslation == mGpuWallMaterial.mTranslation && mMaterial.mOverrideShader == mGpuWallMaterial.mOverrideShader;
	if (!reuse)
	{
		ApplyRenderPass(DT_Triangles);
		ApplyScissor();
		ApplyViewport();
		ApplyStencilRef();
		ApplyDepthBias();
		ApplyPushConstants();
		ApplyVertexBuffers();
		ApplyHWBufferSet();
		ApplyMaterial();
	}
	else mMaterial.mChanged = false;
	mNeedApply = false;
	mGpuWallMaterial = mMaterial;
	if (!mGpuWallBatchCount) mGpuWallBatchStart = slot;
	++mGpuWallBatchCount;
	mVertexBuffer = savedVB;
	mSecondaryVertexBuffer = savedSecondary;
	mVertexOffsets[0] = offsets[0];
	mVertexOffsets[1] = offsets[1];
	mIndexBuffer = savedIB;
	return true;
}

// [Karto] Shared by indexed walls and world-mode capture so a captured row is byte-for-byte the native draw's shading.
void VkRenderState::FillGpuWorldRow(GpuWorldSurfaceData& row, const uint32_t textures[4])
{
	static const VSMatrix identity(1);
	ResolveWorldLighting();
	mStreamData.useVertexData = fb->GetRenderPassManager()->GetVertexFormat(static_cast<VkHardwareVertexBuffer*>(mVertexBuffer)->VertexFormat)->UseVertexData;
	mStreamData.timer = static_cast<float>((double)(screen->FrameTime - firstFrame) * (double)mMaterial.mMaterial->Source()->GetShaderSpeed() / 1000.);
	const auto push = MakePushConstants();
	NoteShaderFeatures(mStreamData, push.uLightIndex);
	row.Surface = { mStreamData, mTextureMatrixEnabled ? mTextureMatrix : identity,
		{ push.uLightDist, push.uLightFactor, push.uFogDensity, push.uLightLevel },
		{ push.uClipSplit.X, push.uClipSplit.Y, push.uSpecularMaterial.X, push.uSpecularMaterial.Y },
		push.uTextureMode, push.uFogEnabled, push.uLightIndex, push.uAlphaThreshold, 0 };
	memcpy(row.TextureIDs, textures, sizeof(row.TextureIDs));
}

bool VkRenderState::DrawGpuIndexedWall(uint32_t slot, uint32_t bakeRow)
{
	if (!mTextureEnabled || mModelMatrixEnabled || mBoneIndexBase >= 0 || mRetainedDrawing || !mGpuWorldSet) return false;
	auto buffers = fb->GetBufferManager();
	if (!buffers->StaticGeometryVB || !buffers->StaticSurfaceID || !buffers->GpuQuadIndices) return false;
	uint32_t textures[4];
	if (!static_cast<VkMaterial*>(mMaterial.mMaterial)->GetGpuWorldTextures(mMaterial, textures)) return false;
	if (mGpuWallBatchCount && slot != mGpuWallBatchStart + mGpuWallBatchCount) FlushQuadBatch();
	auto savedVB = mVertexBuffer;
	auto savedSecondary = mSecondaryVertexBuffer;
	const int offsets[2] = { mVertexOffsets[0], mVertexOffsets[1] };
	auto savedIB = mIndexBuffer;
	SetVertexBuffer(buffers->StaticGeometryVB.get(), 0, 0, buffers->StaticSurfaceID.get());
	SetIndexBuffer(buffers->GpuQuadIndices.get());
	{
		HWSceneScopeTimer rowTimer(SceneMainSurfaceProfile.GpuAppearanceRowNS);
		GpuWorldSurfaceData row = {};
		FillGpuWorldRow(row, textures);
		memcpy(static_cast<uint8_t*>(buffers->GpuWorldAppearance->Memory()) + size_t(slot) * sizeof(row), &row, sizeof(row));
		if (mGpuResidentView && mGpuResidentCapture < GpuResidentCapacity && mGpuResidentCaptureRevision)
		{
			const uint32_t id = mGpuResidentCapture;
			if (mGpuWorldResident.size() <= id)
			{
				mGpuWorldResident.resize(size_t(id) + 1);
				mGpuWorldResidentRevision.resize(size_t(id) + 1);
				mGpuWorldResidentLighting.resize(size_t(id) + 1);
				mGpuWorldResidentTextureEpoch.resize(size_t(id) + 1);
			}
			mGpuWorldResident[id] = row;
			mGpuWorldResidentRevision[id] = mGpuResidentCaptureRevision;
			mGpuWorldResidentLighting[id] = CaptureWorldLighting();
			mGpuWorldResidentTextureEpoch[id] = fb->GetDescriptorSetManager()->GetGpuWorldTextureEpoch();
			memcpy(static_cast<uint8_t*>(buffers->GpuResidentAppearance->Memory()) + size_t(id) * sizeof(row), &row, sizeof(row));
		}
	}
	ApplyGpuIndexedSlot(slot, 1, mGpuWorldSet.get());
	mVertexBuffer = savedVB;
	mSecondaryVertexBuffer = savedSecondary;
	mVertexOffsets[0] = offsets[0]; mVertexOffsets[1] = offsets[1];
	mIndexBuffer = savedIB;
	return true;
}

void VkRenderState::ApplyGpuIndexedSlot(uint32_t slot, int variant, VulkanDescriptorSet* appearanceSet)
{
	mGpuIndexedDrawing = variant;
	AdvanceSubmissionBudget();
	const auto key = MakePipelineKey(DT_Triangles);
	const bool reuse = mGpuWallBatchCount && mCommandBuffer && key == mPipelineKey &&
		!mScissorChanged && !mViewportChanged && !mStencilRefChanged && !mBias.mChanged && mViewpointOffset == mLastViewpointOffset;
	if (!reuse)
	{
		FlushQuadBatch();
		const bool textureMatrix = mTextureMatrixEnabled;
		mTextureMatrixEnabled = false;
		ApplyMatrices();
		mTextureMatrixEnabled = textureMatrix;
		ApplyRenderPass(DT_Triangles);
		ApplyScissor(); ApplyViewport(); ApplyStencilRef(); ApplyDepthBias();
		ApplyVertexBuffers();
		auto descriptors = fb->GetDescriptorSetManager();
		auto layout = fb->GetRenderPassManager()->GetPipelineLayout(key.NumTextureLayers, true);
		uint32_t dynamicOffsets[3] = { mViewpointOffset, mMatrixBufferWriter.Offset(), mStreamBufferWriter.StreamDataOffset() };
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, descriptors->GetFixedDescriptorSet());
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, descriptors->GetHWBufferDescriptorSet(), 3, dynamicOffsets);
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, descriptors->GetGpuWorldTextureSet());
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 3, appearanceSet);
		mLastViewpointOffset = mViewpointOffset;
		if (SceneProfiling) SceneMainSurfaceProfile.DescriptorBinds += 4;
	}
	if (!mGpuWallBatchCount) mGpuWallBatchStart = slot;
	++mGpuWallBatchCount;
	mGpuIndexedDrawing = false;
	mNeedApply = true;
}

bool VkRenderState::HasGpuResidentAppearance(uint32_t id, uint64_t revision) const
{
	return revision && id < mGpuWorldResidentRevision.size() && mGpuWorldResidentRevision[id] == revision &&
		mGpuWorldResidentTextureEpoch[id] == fb->GetDescriptorSetManager()->GetGpuWorldTextureEpoch();
}

void VkRenderState::SetGpuResidentCapture(uint32_t id, uint64_t revision)
{
	mGpuResidentCapture = id;
	mGpuResidentCaptureRevision = revision;
}

bool VkRenderState::DrawGpuResidentWall(uint32_t slot, uint32_t id, uint64_t revision)
{
	if (!mGpuResidentView || !mGpuWallsActive || slot >= mCullCommandCount || !mTextureEnabled ||
		mModelMatrixEnabled || mTextureMatrixEnabled || mBoneIndexBase >= 0 || mRetainedDrawing ||
		mSpecialEffect != EFF_NONE || !HasGpuResidentAppearance(id, revision)) return false;
	const auto& row = mGpuWorldResident[id].Surface;
	if (row.TextureMode != GetTextureModeAndFlags(TM_NORMAL) || row.AlphaThreshold != mAlphaThreshold ||
		row.ClipSpecular.X != mClipSplit[0] || row.ClipSpecular.Y != mClipSplit[1] ||
		row.Stream.uInterpolationFactor != mStreamData.uInterpolationFactor || row.Stream.uDetailParms != mStreamData.uDetailParms) return false;
	const auto& lighting = mGpuWorldResidentLighting[id];
	// [Karto] Disabled fog still inherits distance/factor used by software lighting; a changed predecessor refreshes the row.
	const auto inherited = mGpuResidentLast == UINT32_MAX ? CaptureWorldLighting() : mGpuWorldResidentLighting[mGpuResidentLast];
	if (!lighting.FogEnabled && (lighting.LightParameters.X != inherited.LightParameters.X || lighting.LightParameters.Y != inherited.LightParameters.Y)) return false;
	auto buffers = fb->GetBufferManager();
	if (!buffers->StaticGeometryVB || !buffers->StaticSurfaceID || !buffers->GpuQuadIndices) return false;
	if (mGpuWallBatchCount && slot != mGpuWallBatchStart + mGpuWallBatchCount) FlushQuadBatch();
	auto savedVB = mVertexBuffer;
	auto savedSecondary = mSecondaryVertexBuffer;
	const int offsets[2] = { mVertexOffsets[0], mVertexOffsets[1] };
	auto savedIB = mIndexBuffer;
	SetVertexBuffer(buffers->StaticGeometryVB.get(), 0, 0, buffers->StaticSurfaceID.get());
	SetIndexBuffer(buffers->GpuQuadIndices.get());
	static_cast<uint32_t*>(buffers->GpuResidentReferences->Memory())[slot] = id;
	ApplyGpuIndexedSlot(slot, 2, mGpuResidentSet.get());
	mGpuResidentLast = id;
	mVertexBuffer = savedVB;
	mSecondaryVertexBuffer = savedSecondary;
	mVertexOffsets[0] = offsets[0]; mVertexOffsets[1] = offsets[1];
	mIndexBuffer = savedIB;
	++mGpuResidentDraws;
	return true;
}

void VkRenderState::EndGpuResidentBatch()
{
	if (mGpuResidentLast != UINT32_MAX)
	{
		FlushQuadBatch();
		const auto& lighting = mGpuWorldResidentLighting[mGpuResidentLast];
		ApplyWorldLighting(lighting);
		mLightParms[0] = lighting.LightParameters.X;
		mLightParms[1] = lighting.LightParameters.Y;
		mLightIndex = mGpuWorldResident[mGpuResidentLast].Surface.LightIndex;
		mGpuResidentLast = UINT32_MAX;
	}
}

void VkRenderState::EndGpuDrivenWalls()
{
	EndGpuResidentBatch();
	FlushQuadBatch();
	mGpuWallsActive = false;
}

bool VkRenderState::DrawGpuDrivenGeometry(uint32_t drawCount)
{
	if (!vk_gpu_driven_geometry || !vk_gpu_driven_cull || drawCount == 0 ||
		!mCullPending || !mCullForDraw || drawCount != mCullPrims) return false;
	EndRenderPass();
	RecordGpuCull();
	if (!mCullDrawReady) return false;
	auto buffers = fb->GetBufferManager();
	if (!buffers->StaticGeometryVB || !buffers->StaticSurfaceID || !buffers->GpuQuadIndices || !buffers->GpuCullCommands) return false;
	if (buffers->GpuCullCommands->Size() < size_t(drawCount) * 5 * sizeof(uint32_t)) return false;
	IVertexBuffer* savedVB = mVertexBuffer;
	IVertexBuffer* savedSecondary = mSecondaryVertexBuffer;
	const int savedOffsets[2] = { mVertexOffsets[0], mVertexOffsets[1] };
	IIndexBuffer* savedIB = mIndexBuffer;
	SetVertexBuffer(buffers->StaticGeometryVB.get(), 0, 0, buffers->StaticSurfaceID.get());
	SetIndexBuffer(buffers->GpuQuadIndices.get());
	Apply(DT_Triangles);
	const uint32_t limit = fb->device->PhysicalDevice.Properties.Properties.limits.maxDrawIndirectCount;
	for (uint32_t first = 0; first < drawCount;)
	{
		const uint32_t count = std::min(limit, drawCount - first);
		if (SceneProfiling) ++SceneMainSurfaceProfile.GPUDraws;
		mCommandBuffer->drawIndexedIndirect(static_cast<VkHardwareDataBuffer*>(buffers->GpuCullCommands.get())->mBuffer->buffer,
			size_t(first) * 5 * sizeof(uint32_t), count, 5 * sizeof(uint32_t));
		first += count;
	}
	{
		static bool logged = false;
		if (!logged) { logged = true; Printf("G3DRAW commandSlots=%u current_view=1\n", drawCount); }
	}
	mVertexBuffer = savedVB;
	mSecondaryVertexBuffer = savedSecondary;
	mVertexOffsets[0] = savedOffsets[0];
	mVertexOffsets[1] = savedOffsets[1];
	mIndexBuffer = savedIB;
	return true;
}

bool VkRenderState::GatherRetained(const uint32_t* indices, uint32_t count, uint32_t surface, uint64_t ownerVersion)
{
	HWSceneScopeTimer gatherTimer(SceneMainSurfaceProfile.RetainedGatherNS);
	constexpr uint32_t surfaces = 65536;
	constexpr uint32_t views = 4;
	constexpr uint32_t indexCapacity = surfaces * views * 6;
	static_assert(size_t(surfaces) * views * sizeof(RetainedSurfaceData) <= 128 * 1024 * 1024);
	auto buffers = fb->GetBufferManager();
	if (!mTextureEnabled || mSpecialEffect != EFF_NONE || mMaterial.mOverrideShader > 0 || !mMaterial.mMaterial ||
		mMaterial.mMaterial->GetShaderIndex() != 0 || mMaterial.mMaterial->Source()->isHardwareCanvas() ||
		surface >= surfaces || count > indexCapacity - mRetainedIndexCursor ||
		(mViewpointOffset != mRetainedViewOffset && mRetainedViewCount == views))
	{
		if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedGPUFallbacks;
		return false;
	}
	if (!mRetainedBuffersReady)
	{
		FlushQuadBatch();
		buffers->RetainedParameters->SetData(size_t(surfaces) * views * sizeof(RetainedSurfaceData), nullptr, BufferUsageType::Persistent);
		buffers->RetainedIndices->SetData(size_t(indexCapacity) * sizeof(uint32_t), nullptr, BufferUsageType::Persistent);
		fb->GetDescriptorSetManager()->UpdateHWBufferSet();
		mLastBufferLayers = -1;
		mRetainedParameters.Init(surfaces * views);
		mRetainedBuffersReady = true;
	}
	if (mViewpointOffset != mRetainedViewOffset)
	{
		FlushQuadBatch();
		mRetainedBase = mRetainedViewCount++ * surfaces;
		mRetainedViewOffset = mViewpointOffset;
	}
	mRetainedSurfaceIndex = mRetainedBase + surface;
	auto oldIndexBuffer = mIndexBuffer;
	mIndexBuffer = buffers->RetainedIndices.get();
	mRetainedDrawing = true;
	mRetainedIndependentTime = vk_retainedparams || mOwnedRetainedView || ownerVersion != 0;
	if (SceneProfiling && mOwnedRetainedView) ++SceneMainSurfaceProfile.RetainedOwnedTimerDraws;
	mRetainedOwnerVersion = ownerVersion;
	if (!ownerVersion || !mRetainedWallContext) ApplyStreamData();
	if (!CollectRetainedParameters())
	{
		mRetainedDrawing = false;
		mIndexBuffer = oldIndexBuffer;
		if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedGPUFallbacks;
		return false;
	}
	{
		HWSceneScopeTimer stateTimer(SceneMainSurfaceProfile.RetainedStateNS);
		AdvanceSubmissionBudget();
		const auto pipelineKey = MakePipelineKey(DT_Triangles);
		if (CanReuseRetainedState(pipelineKey))
		{
			if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedStateReuses;
		}
		else
		{
			if (SceneProfiling)
			{
				if (!mRetainedBatchCount) ++SceneMainSurfaceProfile.RetainedStateEmptyBatch;
				else
				{
					++SceneMainSurfaceProfile.RetainedStateBoundaries;
					if (mMaterial.mMaterial != mRetainedMaterial.mMaterial || mMaterial.mClampMode != mRetainedMaterial.mClampMode ||
						mMaterial.mTranslation != mRetainedMaterial.mTranslation || mMaterial.mOverrideShader != mRetainedMaterial.mOverrideShader)
						++SceneMainSurfaceProfile.RetainedMaterialChanges;
					if (mVertexBuffer != mLastVertexBuffer || mSecondaryVertexBuffer != mLastSecondaryVertexBuffer ||
						mIndexBuffer != mLastIndexBuffer || mVertexOffsets[0] != mLastVertexOffsets[0] || mVertexOffsets[1] != mLastVertexOffsets[1])
						++SceneMainSurfaceProfile.RetainedBufferChanges;
					auto compatibleKey = pipelineKey;
					compatibleKey.RetainedWorld = mPipelineKey.RetainedWorld;
					if (((pipelineKey.RetainedWorld == 1 && mPipelineKey.RetainedWorld == 2) ||
						(pipelineKey.RetainedWorld == 2 && mPipelineKey.RetainedWorld == 1)) && CanReuseRetainedState(compatibleKey))
						++SceneMainSurfaceProfile.RetainedVariantOnlyBreaks;
				}
			}
			FlushQuadBatch();
			Apply(DT_Triangles, false);
			mRetainedMaterial = mMaterial;
			mRetainedModelMatrix = mModelMatrix;
			mRetainedModelEnabled = mModelMatrixEnabled;
			mRetainedTimer = mStreamData.timer;
			if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedStateApplies;
		}
	}
	mRetainedDrawing = false;
	mIndexBuffer = oldIndexBuffer;
	if (!mRetainedBatchCount) mRetainedBatchStart = mRetainedIndexCursor;
	// [Karto] Batching must preserve topology and coplanar tie order.
	{
		HWSceneScopeTimer indexTimer(SceneMainSurfaceProfile.RetainedIndexNS);
		memcpy(static_cast<uint32_t*>(buffers->RetainedIndices->Memory()) + mRetainedIndexCursor, indices, count * sizeof(uint32_t));
	}
	mRetainedIndexCursor += count;
	mRetainedBatchCount += count;
	if (SceneProfiling)
	{
		SceneMainSurfaceProfile.RetainedParameterCPUBytes = mRetainedParameters.Memory();
		SceneMainSurfaceProfile.RetainedTriangles += count / 3;
		SceneMainSurfaceProfile.RetainedBufferBytes = size_t(surfaces) * views * sizeof(RetainedSurfaceData) + size_t(indexCapacity) * sizeof(uint32_t);
	}
	return true;
}

void VkRenderState::BeginGPUProfile(const char* name)
{
	FlushQuadBatch();
	fb->GetCommands()->PushGroup(name);
}

void VkRenderState::EndGPUProfile()
{
	FlushQuadBatch();
	fb->GetCommands()->PopGroup();
}

void VkRenderState::DrawModelIndexed(int index, int count)
{
	if (mWorldCapture || mRetainedDrawing || mGpuIndexedDrawing || !mModelMatrixEnabled || mBoneIndexBase >= 0 ||
		mTextureMatrixEnabled || !vk_drawreuse || mSpecialEffect != EFF_NONE || mMaterial.mOverrideShader > 0 ||
		!mMaterial.mMaterial || mMaterial.mMaterial->GetShaderIndex() != 0 || mMaterial.mMaterial->Source()->isHardwareCanvas())
	{
		DrawIndexed(DT_Triangles, index, count);
		return;
	}
	VSMatrix matrices[2] = { mModelMatrix, {} };
	matrices[1].computeNormalMatrix(mModelMatrix);
	const int base = screen->mBones->UploadMatrices(matrices, 2);
	if (base < 0)
	{
		DrawIndexed(DT_Triangles, index, count);
		return;
	}
	EndRetainedWallRun();
	if (mModelBatchCount && (index != mModelBatchIndex || count != mModelBatchIndices || base != mModelBatchBase + 2 * mModelBatchCount))
		FlushQuadBatch();
	mModelInstancing = true;
	mModelMatrixEnabled = false;
	mBoneIndexBase = mModelBatchCount ? mModelBatchBase : base;
	const int pending = mModelBatchCount;
	Apply(DT_Triangles);
	if (pending && !mModelBatchCount)
	{
		mBoneIndexBase = base;
		Apply(DT_Triangles);
	}
	mModelInstancing = false;
	mModelMatrixEnabled = true;
	mBoneIndexBase = -1;
	if (!mModelBatchCount)
	{
		mModelBatchBase = base;
		mModelBatchIndex = index;
		mModelBatchIndices = count;
	}
	++mModelBatchCount;
}

void VkRenderState::DrawIndirectFans(const FIndirectFan* fans, int count)
{
	constexpr size_t RegionBytes = 64 * 1024;
	constexpr size_t CapacityBytes = RegionBytes * 2;
	if (count <= 0) return;
	auto fallback = [&](int first, int last)
	{
		for (int i = first; i < last; ++i)
		{
			SetLightIndex(fans[i].LightIndex);
			Draw(DT_TriangleFan, fans[i].VertexIndex, fans[i].VertexCount);
		}
	};
	if (mWorldCapture || !r_flatindirect)
	{
		fallback(0, count);
		return;
	}
	if (!mIndirectFanBuffer)
	{
		mIndirectFanBuffer = std::make_unique<VkHardwareBuffer>(fb);
		mIndirectFanBuffer->mBufferType = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		mIndirectFanBuffer->SetData(CapacityBytes, nullptr, BufferUsageType::Persistent);
		if (mIndirectFanBuffer->Memory() == nullptr)
		{
			mIndirectFanBuffer.reset();
			fallback(0, count);
			return;
		}
	}
	const uint64_t frame = fb->GetCommands()->GetCurrentFrame();
	if (frame != mIndirectFanFrame)
	{
		mIndirectFanFrame = frame;
		mIndirectFanOffset = size_t(frame & 1) * RegionBytes;
	}
	const size_t regionEnd = size_t(frame & 1) * RegionBytes + RegionBytes;
	uint8_t* base = static_cast<uint8_t*>(mIndirectFanBuffer->Memory());
	for (int i = 0; i < count;)
	{
		const int light = fans[i].LightIndex;
		int end = i + 1;
		while (end < count && fans[end].LightIndex == light) ++end;
		constexpr int MinBatch = 8;
		if (end - i < MinBatch)
		{
			fallback(i, end);
			i = end;
			continue;
		}
		const size_t bytes = size_t(end - i) * sizeof(VkDrawIndirectCommand);
		if (mIndirectFanOffset + bytes > regionEnd)
		{
			fallback(i, end);
			i = end;
			continue;
		}
		auto* commands = reinterpret_cast<VkDrawIndirectCommand*>(base + mIndirectFanOffset);
		for (int k = i; k < end; ++k)
		{
			commands[k - i].vertexCount = fans[k].VertexCount;
			commands[k - i].instanceCount = 1;
			commands[k - i].firstVertex = fans[k].VertexIndex;
			commands[k - i].firstInstance = 0;
		}
		SetLightIndex(light);
		EndRetainedWallRun();
		FlushQuadBatch();
		Apply(DT_TriangleFan);
		if (SceneProfiling) SceneMainSurfaceProfile.GPUDraws += (end - i);
		mCommandBuffer->drawIndirect(mIndirectFanBuffer->mBuffer->buffer, mIndirectFanOffset, uint32_t(end - i), sizeof(VkDrawIndirectCommand));
		mIndirectFanOffset += bytes;
		i = end;
	}
}

void VkRenderState::DrawOpaqueQuad(int index)
{
	if (!vk_wallbatch)
	{
		Draw(DT_TriangleFan, index, 4);
		return;
	}
	DrawBatchedQuad(index);
}

void VkRenderState::DrawBatchedQuad(int index)
{
	if (!vk_drawreuse || mSpecialEffect != EFF_NONE || mMaterial.mOverrideShader > 0 ||
		!mMaterial.mMaterial || mMaterial.mMaterial->GetShaderIndex() != 0 ||
		mMaterial.mMaterial->Source()->isHardwareCanvas())
	{
		Draw(DT_TriangleFan, index, 4);
		return;
	}

	if (mQuadBatchCount && (index != mQuadBatchIndex + mQuadBatchCount * 4 || mQuadBatchCount == 256))
		FlushQuadBatch();

	auto oldIndexBuffer = mIndexBuffer;
	mIndexBuffer = fb->GetBufferManager()->GetQuadBatchIndexBuffer();
	Apply(DT_Triangles);
	mIndexBuffer = oldIndexBuffer;

	if (!mQuadBatchCount) mQuadBatchIndex = index;
	++mQuadBatchCount;
}

void VkRenderState::DrawBatchedQuadSameState(int index)
{
	if (mQuadBatchCount && index == mQuadBatchIndex + mQuadBatchCount * 4 && mQuadBatchCount < 256)
	{
		++mQuadBatchCount;
		return;
	}
	DrawBatchedQuad(index);
}

void VkRenderState::FlushQuadBatch()
{
	if (mModelBatchCount)
	{
		mCommandBuffer->drawIndexed(mModelBatchIndices, mModelBatchCount, mModelBatchIndex, 0, 0);
		if (SceneProfiling) ++SceneMainSurfaceProfile.GPUDraws;
		++ModelInstanceDraws;
		ModelInstances += mModelBatchCount;
		mModelBatchCount = 0;
	}
	if (mGpuWallBatchCount)
	{
		const uint32_t limit = fb->device->PhysicalDevice.Properties.Properties.limits.maxDrawIndirectCount;
		auto commands = static_cast<VkHardwareDataBuffer*>(fb->GetBufferManager()->GpuCullCommands.get());
		for (uint32_t first = 0; first < mGpuWallBatchCount;)
		{
			const uint32_t count = std::min(limit, mGpuWallBatchCount - first);
			if (mPipelineKey.GpuIndexedWorld)
			{
				// [Karto] DrawID restarts at each split call, so the base must advance with the ordered command offset.
				PushConstants push = {};
				push.uDataIndex = int(mGpuWallBatchStart + first);
				mCommandBuffer->pushConstants(fb->GetRenderPassManager()->GetPipelineLayout(mPipelineKey.NumTextureLayers, true),
					VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
				if (SceneProfiling) ++SceneMainSurfaceProfile.PushWrites;
				static bool logged = false;
				if (!logged) { logged = true; Printf("G3INDEXEDUSE slots=%u appearance=1 bindless=1 replacement=1\n", count); }
			}
			mCommandBuffer->drawIndexedIndirect(commands->mBuffer->buffer, size_t(mGpuWallBatchStart + first) * 20, count, 20);
			if (SceneProfiling) ++SceneMainSurfaceProfile.GPUDraws;
			if (mPipelineKey.GpuIndexedWorld == 2)
			{
				static bool residentLogged = false;
				if (!residentLogged) { residentLogged = true; Printf("G3RESIDENTUSE slots=%u stable_appearance=1 compact_refs=1 native_draw_bypass=1\n", count); }
			}
			if (vk_gpu_driven_debug)
			{
				static unsigned logs = 0;
				if (logs++ < 12) Printf("G37DRAW slots=%u first=%u shaded=1 replacement=1\n", count, mGpuWallBatchStart + first);
			}
			first += count;
		}
		mGpuWallBatchCount = 0;
		if (mPipelineKey.GpuIndexedWorld)
		{
			mLastBufferLayers = mLastMaterialLayers = mLastPushLayers = -1;
			mLastMaterialSet = nullptr;
			mPushConstantsValid = false;
			mMaterial.mChanged = true;
		}
	}
	if (mRetainedBatchCount)
	{
		mCommandBuffer->drawIndexed(mRetainedBatchCount, 1, mRetainedBatchStart, 0, 0);
		if (SceneProfiling)
		{
			++SceneMainSurfaceProfile.GPUDraws;
			++SceneMainSurfaceProfile.RetainedBatchCalls;
		}
		mRetainedBatchCount = 0;
	}
	if (!mQuadBatchCount) return;
	mCommandBuffer->drawIndexed(mQuadBatchCount * 6, 1, 0, mQuadBatchIndex, 0);
	if (SceneProfiling)
	{
		++SceneMainSurfaceProfile.GPUDraws;
		++SceneMainSurfaceProfile.QuadBatchCalls;
		SceneMainSurfaceProfile.QuadBatchQuads += mQuadBatchCount;
	}
	mQuadBatchCount = 0;
}

bool VkRenderState::SetDepthClamp(bool on)
{
	bool lastValue = mDepthClamp;
	mDepthClamp = on;
	mNeedApply = true;
	return lastValue;
}

void VkRenderState::SetDepthMask(bool on)
{
	mDepthWrite = on;
	mNeedApply = true;
}

void VkRenderState::SetDepthFunc(int func)
{
	mDepthFunc = func;
	mNeedApply = true;
}

void VkRenderState::SetDepthRange(float min, float max)
{
	mViewportDepthMin = min;
	mViewportDepthMax = max;
	mViewportChanged = true;
	mNeedApply = true;
}

void VkRenderState::SetColorMask(bool r, bool g, bool b, bool a)
{
	int rr = r, gg = g, bb = b, aa = a;
	mColorMask = (aa << 3) | (bb << 2) | (gg << 1) | rr;
	mNeedApply = true;
}

void VkRenderState::SetStencil(int offs, int op, int flags)
{
	mStencilRef = screen->stencilValue + offs;
	mStencilRefChanged = true;
	mStencilOp = op;

	if (flags != -1)
	{
		bool cmon = !(flags & SF_ColorMaskOff);
		SetColorMask(cmon, cmon, cmon, cmon); // don't write to the graphics buffer
		mDepthWrite = !(flags & SF_DepthMaskOff);
	}

	mNeedApply = true;
}

void VkRenderState::SetCulling(int mode)
{
	mCullMode = mode;
	mNeedApply = true;
}

void VkRenderState::EnableClipDistance(int num, bool state)
{
}

void VkRenderState::Clear(int targets)
{
	mClearTargets = targets;
	EndRenderPass();
}

void VkRenderState::EnableStencil(bool on)
{
	mStencilTest = on;
	mNeedApply = true;
}

void VkRenderState::SetScissor(int x, int y, int w, int h)
{
	mScissorX = x;
	mScissorY = y;
	mScissorWidth = w;
	mScissorHeight = h;
	mScissorChanged = true;
	mNeedApply = true;
}

void VkRenderState::SetViewport(int x, int y, int w, int h)
{
	mViewportX = x;
	mViewportY = y;
	mViewportWidth = w;
	mViewportHeight = h;
	mViewportChanged = true;
	mNeedApply = true;
}

void VkRenderState::EnableDepthTest(bool on)
{
	mDepthTest = on;
	mNeedApply = true;
}

void VkRenderState::EnableMultisampling(bool on)
{
}

void VkRenderState::EnableLineSmooth(bool on)
{
}

void VkRenderState::AdvanceSubmissionBudget()
{
	++mSubmissionWork;
	if (mSubmissionWork >= vk_submit_size)
	{
		fb->GetCommands()->FlushCommands(false);
		mSubmissionWork = 0;
	}
}

void VkRenderState::Apply(int dt, bool countSubmission)
{
	drawcalls.Clock();
	if (countSubmission) AdvanceSubmissionBudget();

	ApplyStreamData();
	ApplyMatrices();
	ApplyRenderPass(dt);
	ApplyScissor();
	ApplyViewport();
	ApplyStencilRef();
	ApplyDepthBias();
	ApplyPushConstants();
	ApplyVertexBuffers();
	ApplyHWBufferSet();
	ApplyMaterial();
	mNeedApply = false;

	drawcalls.Unclock();
}

void VkRenderState::ApplyDepthBias()
{
	if (mBias.mChanged)
	{
		FlushQuadBatch();
		mCommandBuffer->setDepthBias(mBias.mUnits, 0.0f, mBias.mFactor);
		mBias.mChanged = false;
	}
}

VkPipelineKey VkRenderState::MakePipelineKey(int dt) const
{
	VkPipelineKey pipelineKey;
	pipelineKey.DrawType = dt;
	pipelineKey.GpuIndexedWorld = mGpuIndexedDrawing;
	pipelineKey.RetainedWorld = mRetainedDrawing ? (mRetainedIndependentTime ? 2 : 1) : 0;
	pipelineKey.ModelInstances = mModelInstancing;
	pipelineKey.VertexFormat = static_cast<VkHardwareVertexBuffer*>(mVertexBuffer)->VertexFormat;
	pipelineKey.RenderStyle = mRenderStyle;
	pipelineKey.DepthTest = mDepthTest;
	pipelineKey.DepthWrite = mDepthTest && mDepthWrite;
	pipelineKey.DepthFunc = mDepthFunc;
	pipelineKey.DepthClamp = mDepthClamp;
	pipelineKey.DepthBias = !(mBias.mFactor == 0 && mBias.mUnits == 0);
	pipelineKey.StencilTest = mStencilTest;
	pipelineKey.StencilPassOp = mStencilOp;
	pipelineKey.ColorMask = mColorMask;
	pipelineKey.CullMode = mCullMode;
	pipelineKey.NumTextureLayers = mMaterial.mMaterial ? mMaterial.mMaterial->NumLayers() : 0;
	pipelineKey.NumTextureLayers = max(pipelineKey.NumTextureLayers, SHADER_MIN_REQUIRED_TEXTURE_LAYERS);// Always force minimum 8 textures as the shader requires it
	if (mSpecialEffect > EFF_NONE)
	{
		pipelineKey.SpecialEffect = mSpecialEffect;
		pipelineKey.EffectState = 0;
		pipelineKey.AlphaTest = false;
	}
	else
	{
		int effectState = mMaterial.mOverrideShader >= 0 ? mMaterial.mOverrideShader : (mMaterial.mMaterial ? mMaterial.mMaterial->GetShaderIndex() : 0);
		pipelineKey.SpecialEffect = EFF_NONE;
		pipelineKey.EffectState = mTextureEnabled ? effectState : SHADER_NoTexture;
		if (r_skipmats && pipelineKey.EffectState >= 3 && pipelineKey.EffectState <= 4)
			pipelineKey.EffectState = 0;
		pipelineKey.AlphaTest = mAlphaThreshold >= 0.f;
	}

	if (mGpuIndexedDrawing) pipelineKey.NumTextureLayers = SHADER_MIN_REQUIRED_TEXTURE_LAYERS;
	if (mGpuIndexedDrawing >= 2 || mGpuOwnedWorldDrawing)
	{
		pipelineKey.EffectState = 0;
		pipelineKey.SpecialEffect = EFF_NONE;
	}
	return pipelineKey;
}

bool VkRenderState::CanReuseRetainedState(const VkPipelineKey& key) const
{
	return vk_drawreuse && mRetainedBatchCount && mCommandBuffer && key == mPipelineKey &&
		(!mRetainedIndependentTime || memcmp(&mRetainedTimer, &mStreamData.timer, sizeof(float)) == 0) &&
		!mScissorChanged && !mViewportChanged && !mStencilRefChanged && !mBias.mChanged &&
		mViewpointOffset == mLastViewpointOffset && mVertexBuffer == mLastVertexBuffer &&
		mSecondaryVertexBuffer == mLastSecondaryVertexBuffer &&
		mIndexBuffer == mLastIndexBuffer && mVertexOffsets[0] == mLastVertexOffsets[0] &&
		mVertexOffsets[1] == mLastVertexOffsets[1] &&
		mMaterial.mMaterial == mRetainedMaterial.mMaterial &&
		mMaterial.mClampMode == mRetainedMaterial.mClampMode &&
		mMaterial.mTranslation == mRetainedMaterial.mTranslation &&
		mMaterial.mOverrideShader == mRetainedMaterial.mOverrideShader &&
		mModelMatrixEnabled == mRetainedModelEnabled &&
		(!mModelMatrixEnabled || memcmp(&mModelMatrix, &mRetainedModelMatrix, sizeof(VSMatrix)) == 0);
}

void VkRenderState::ApplyRenderPass(int dt)
{
	auto pipelineKey = MakePipelineKey(dt);
	bool inRenderPass = mCommandBuffer;
	bool changingPipeline = (!inRenderPass) || (pipelineKey != mPipelineKey);

	if (!inRenderPass)
	{
		mCommandBuffer = fb->GetCommands()->GetDrawCommands();
		mScissorChanged = true;
		mViewportChanged = true;
		mStencilRefChanged = true;
		mBias.mChanged = true;

		BeginRenderPass(mCommandBuffer);
	}

	if (changingPipeline)
	{
		if (SceneProfiling && inRenderPass &&
			((pipelineKey.RetainedWorld == 1 && mPipelineKey.RetainedWorld == 2) ||
			(pipelineKey.RetainedWorld == 2 && mPipelineKey.RetainedWorld == 1)))
			++SceneMainSurfaceProfile.RetainedVariantSwitches;
		FlushQuadBatch();
		if (SceneProfiling) ++SceneMainSurfaceProfile.PipelineBinds;
		mCommandBuffer->bindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, mPassSetup->GetPipeline(pipelineKey));
		mPipelineKey = pipelineKey;
	}
}

void VkRenderState::ApplyStencilRef()
{
	if (mStencilRefChanged)
	{
		FlushQuadBatch();
		mCommandBuffer->setStencilReference(VK_STENCIL_FRONT_AND_BACK, mStencilRef);
		mStencilRefChanged = false;
	}
}

void VkRenderState::ApplyScissor()
{
	if (mScissorChanged)
	{
		VkRect2D scissor;
		if (mScissorWidth >= 0)
		{
			int x0 = clamp(mScissorX, 0, mRenderTarget.Width);
			int y0 = clamp(mScissorY, 0, mRenderTarget.Height);
			int x1 = clamp(mScissorX + mScissorWidth, 0, mRenderTarget.Width);
			int y1 = clamp(mScissorY + mScissorHeight, 0, mRenderTarget.Height);

			scissor.offset.x = x0;
			scissor.offset.y = y0;
			scissor.extent.width = x1 - x0;
			scissor.extent.height = y1 - y0;
		}
		else
		{
			scissor.offset.x = 0;
			scissor.offset.y = 0;
			scissor.extent.width = mRenderTarget.Width;
			scissor.extent.height = mRenderTarget.Height;
		}
		FlushQuadBatch();
		mCommandBuffer->setScissor(0, 1, &scissor);
		mScissorChanged = false;
	}
}

void VkRenderState::ApplyViewport()
{
	if (mViewportChanged)
	{
		VkViewport viewport;
		if (mViewportWidth >= 0)
		{
			viewport.x = (float)mViewportX;
			viewport.y = (float)mViewportY;
			viewport.width = (float)mViewportWidth;
			viewport.height = (float)mViewportHeight;
		}
		else
		{
			viewport.x = 0.0f;
			viewport.y = 0.0f;
			viewport.width = (float)mRenderTarget.Width;
			viewport.height = (float)mRenderTarget.Height;
		}
		viewport.minDepth = mViewportDepthMin;
		viewport.maxDepth = mViewportDepthMax;
		FlushQuadBatch();
		mCommandBuffer->setViewport(0, 1, &viewport);
		mViewportChanged = false;
	}
}

void VkRenderState::ApplyStreamData()
{
	auto passManager = fb->GetRenderPassManager();

	mStreamData.useVertexData = passManager->GetVertexFormat(static_cast<VkHardwareVertexBuffer*>(mVertexBuffer)->VertexFormat)->UseVertexData;

	if (mMaterial.mMaterial && mMaterial.mMaterial->Source())
		mStreamData.timer = static_cast<float>((double)(screen->FrameTime - firstFrame) * (double)mMaterial.mMaterial->Source()->GetShaderSpeed() / 1000.);
	else
		mStreamData.timer = 0.0f;
	if (mRetainedDrawing) return;
	ResolveWorldLighting();

	NoteShaderFeatures(mStreamData, mLightIndex);
	const bool reuse = vk_drawreuse && mSpecialEffect == EFF_NONE && mMaterial.mOverrideShader <= 0 &&
		mMaterial.mMaterial && mMaterial.mMaterial->GetShaderIndex() == 0 && !mMaterial.mMaterial->Source()->isHardwareCanvas();
	if (!mStreamBufferWriter.Write(mStreamData, reuse))
	{
		WaitForStreamBuffers();
		mStreamBufferWriter.Write(mStreamData);
	}
}

PushConstants VkRenderState::MakePushConstants()
{
	auto next = mPushConstants;
	int fogset = 0;
	const auto* lighting = mDeferredWorldLighting;
	const auto fogEnabled = lighting ? lighting->FogEnabled : mFogEnabled;
	if (fogEnabled)
	{
		if (fogEnabled == 2)
		{
			fogset = -3;	// 2D rendering with 'foggy' overlay.
		}
		else if ((GetFogColor() & 0xffffff) == 0)
		{
			fogset = gl_fogmode;
		}
		else
		{
			fogset = -gl_fogmode;
		}
	}

	int tempTM = TM_NORMAL;
	if (mMaterial.mMaterial && mMaterial.mMaterial->Source()->isHardwareCanvas() && !mMaterial.mMaterial->Source()->GetTranslucency())
		tempTM = TM_OPAQUE;

	next.uFogEnabled = fogset;
	next.uTextureMode = GetTextureModeAndFlags(tempTM);
	next.uLightDist = lighting && fogEnabled ? lighting->LightParameters.X : mLightParms[0];
	next.uLightFactor = lighting && fogEnabled ? lighting->LightParameters.Y : mLightParms[1];
	next.uFogDensity = lighting ? lighting->LightParameters.Z : mLightParms[2];
	next.uLightLevel = lighting ? lighting->LightParameters.W : mLightParms[3];
	next.uAlphaThreshold = mAlphaThreshold;
	next.uClipSplit = { mClipSplit[0], mClipSplit[1] };

	if (mMaterial.mMaterial)
	{
		auto source = mMaterial.mMaterial->Source();
		next.uSpecularMaterial = { source->GetGlossiness(), source->GetSpecularLevel() };
	}

	next.uLightIndex = mLightIndex;
	next.uBoneIndexBase = mBoneIndexBase;
	next.uDataIndex = mStreamBufferWriter.DataIndex();
	next.uAppearanceIndex = mAppearanceIndex;
	return next;
}

bool VkRenderState::CollectRetainedParameters()
{
	HWSceneScopeTimer timer(SceneMainSurfaceProfile.RetainedCollectNS);
	const auto context = mRetainedWallRun && mRetainedIndependentTime ? mRetainedWallContext : 0;
	if (vk_ownedstate && mRetainedOwnerVersion && context && !vk_retainedworld_verify)
	{
		const auto inherited = MakeRetainedInheritedLight(mDeferredWorldLighting, mFogEnabled, gl_fogmode, mLightParms);
		const auto selected = mRetainedParameters.ReuseOwned(mRetainedSurfaceIndex, mRetainedFrame,
			context, mRetainedOwnerVersion, inherited);
		if (selected.Data)
		{
			if (SceneProfiling)
			{
				++SceneMainSurfaceProfile.OwnedParameterReuses;
				++SceneMainSurfaceProfile.RetainedParameterReuses;
				if (selected.CrossFrameReuse) ++SceneMainSurfaceProfile.RetainedParameterCrossFrameReuses;
				if (mDeferredWorldLighting) ++SceneMainSurfaceProfile.RetainedParameterDeferred;
			}
			return true;
		}
	}
	const auto push = MakePushConstants();
	static const VSMatrix identity(1);
	const auto& transform = mTextureMatrixEnabled ? mTextureMatrix : identity;
	if (context && vk_retainedworld_verify)
	{
		auto reference = MakeRetainedWallContext(mStreamData, transform, push);
		if (memcmp(&reference, &mRetainedWallSnapshot, sizeof(reference)) != 0)
			I_FatalError("Retained wall context changed inside a run");
	}
	auto match = [&](const RetainedSurfaceData& cached, uint32_t cachedContext)
	{
		if (context && context == cachedContext)
		{
			if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedContextShortcuts;
			return MatchesRetainedWallSurface(cached, mStreamData, push, mDeferredWorldLighting);
		}
		if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedContextChecks;
		return MatchesRetainedParameters(cached, mStreamData, transform, push, mRetainedIndependentTime, mDeferredWorldLighting);
	};
	auto build = [&]()
	{
		RetainedSurfaceData next = { mStreamData, transform,
			{ push.uLightDist, push.uLightFactor, push.uFogDensity, push.uLightLevel },
			{ push.uClipSplit.X, push.uClipSplit.Y, push.uSpecularMaterial.X, push.uSpecularMaterial.Y },
			push.uTextureMode, push.uFogEnabled, push.uLightIndex, push.uAlphaThreshold };
		CopyWorldLighting(next.Stream);
		NoteShaderFeatures(next.Stream, push.uLightIndex);
		if (mRetainedIndependentTime) next.Stream.timer = 0;
		return next;
	};
	uint64_t inheritedLight = 0;
	if (mRetainedOwnerVersion && !push.uFogEnabled)
	{
		const float inherited[] = { push.uLightDist, push.uLightFactor };
		static_assert(sizeof(inherited) == sizeof(inheritedLight));
		memcpy(&inheritedLight, inherited, sizeof(inheritedLight));
	}
	const bool persistent = vk_retainedparams || mRetainedOwnerVersion != 0;
	const auto selected = mRetainedParameters.CollectContext(mRetainedSurfaceIndex, mRetainedFrame, persistent,
		context, match, build, mRetainedOwnerVersion, inheritedLight);
	if (!selected.Data) return false;
	if (SceneProfiling && selected.OwnerReuse) ++SceneMainSurfaceProfile.OwnedParameterReuses;
	if (selected.Upload)
	{
		static_cast<RetainedSurfaceData*>(fb->GetBufferManager()->RetainedParameters->Memory())[mRetainedSurfaceIndex] = *selected.Data;
	}
	else if (vk_retainedworld_verify)
	{
		auto reference = build();
		if (mRetainedIndependentTime) reference.Stream.timer = selected.Data->Stream.timer;
		if (!mRetainedParameters.Matches(mRetainedSurfaceIndex, reference))
			I_FatalError("Retained parameter reference differs at surface %u", mRetainedSurfaceIndex);
	}
	if (SceneProfiling)
	{
		if (mDeferredWorldLighting) ++SceneMainSurfaceProfile.RetainedParameterDeferred;
		if (selected.Upload)
		{
			++SceneMainSurfaceProfile.RetainedParameterUploads;
			SceneMainSurfaceProfile.RetainedParameterBytes += sizeof(RetainedSurfaceData);
		}
		else
		{
			++SceneMainSurfaceProfile.RetainedParameterReuses;
			if (selected.CrossFrameReuse) ++SceneMainSurfaceProfile.RetainedParameterCrossFrameReuses;
		}
		if (vk_retainedworld_verify && !selected.Upload) ++SceneMainSurfaceProfile.RetainedParameterVerified;
	}
	return true;
}

void VkRenderState::ApplyPushConstants()
{
	if (!vk_ownedstate || !mOwnedRetainedView || !mRetainedDrawing) mPushConstants = MakePushConstants();
	if (mRetainedDrawing)
	{
		mPushConstants = {};
		mPushConstants.uDataIndex = mRetainedBase;
		if (mRetainedIndependentTime) mPushConstants.uLightDist = mStreamData.timer;
	}

	auto passManager = fb->GetRenderPassManager();
	if (!vk_drawreuse || !mPushConstantsValid || mLastPushLayers != mPipelineKey.NumTextureLayers ||
		memcmp(&mLastPushConstants, &mPushConstants, sizeof(mPushConstants)) != 0)
	{
		FlushQuadBatch();
		mCommandBuffer->pushConstants(passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(PushConstants), &mPushConstants);
		if (vk_drawreuse) mLastPushConstants = mPushConstants;
		mLastPushLayers = mPipelineKey.NumTextureLayers;
		mPushConstantsValid = vk_drawreuse;
		if (SceneProfiling) ++SceneMainSurfaceProfile.PushWrites;
	}
}

void VkRenderState::ApplyMatrices()
{
	if (mRetainedDrawing)
	{
		VSMatrix identity(1);
		if (!mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, identity, false))
		{
			WaitForStreamBuffers();
			mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, identity, false);
		}
		return;
	}
	if (!mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, mTextureMatrix, mTextureMatrixEnabled))
	{
		WaitForStreamBuffers();
		mMatrixBufferWriter.Write(mModelMatrix, mModelMatrixEnabled, mTextureMatrix, mTextureMatrixEnabled);
	}
}

void VkRenderState::ApplyVertexBuffers()
{
	if ((mVertexBuffer != mLastVertexBuffer || mSecondaryVertexBuffer != mLastSecondaryVertexBuffer ||
		mVertexOffsets[0] != mLastVertexOffsets[0] || mVertexOffsets[1] != mLastVertexOffsets[1]) && mVertexBuffer)
	{
		auto vkbuf = static_cast<VkHardwareVertexBuffer*>(mVertexBuffer);
		const VkVertexFormat *format = fb->GetRenderPassManager()->GetVertexFormat(vkbuf->VertexFormat);
		auto secondary = static_cast<VkHardwareVertexBuffer*>(mSecondaryVertexBuffer ? mSecondaryVertexBuffer : mVertexBuffer);
		VkBuffer vertexBuffers[2] = { vkbuf->mBuffer->buffer, secondary->mBuffer->buffer };
		const size_t secondStride = format->NumBindingPoints > 1 ? format->BindingStrides[1] : format->Stride;
		VkDeviceSize offsets[] = { mVertexOffsets[0] * format->BindingStrides[0], mVertexOffsets[1] * secondStride };
		FlushQuadBatch();
		if (SceneProfiling) ++SceneMainSurfaceProfile.BufferBinds;
		mCommandBuffer->bindVertexBuffers(0, 2, vertexBuffers, offsets);
		mLastVertexBuffer = mVertexBuffer;
		mLastSecondaryVertexBuffer = mSecondaryVertexBuffer;
		mLastVertexOffsets[0] = mVertexOffsets[0];
		mLastVertexOffsets[1] = mVertexOffsets[1];
	}

	if (mIndexBuffer != mLastIndexBuffer && mIndexBuffer)
	{
		FlushQuadBatch();
		if (SceneProfiling) ++SceneMainSurfaceProfile.BufferBinds;
		mCommandBuffer->bindIndexBuffer(static_cast<VkHardwareIndexBuffer*>(mIndexBuffer)->mBuffer->buffer, 0, VK_INDEX_TYPE_UINT32);
		mLastIndexBuffer = mIndexBuffer;
	}
}

void VkRenderState::ApplyMaterial()
{
	if (mMaterial.mChanged)
	{
		auto passManager = fb->GetRenderPassManager();
		auto descriptors = fb->GetDescriptorSetManager();

		if (mMaterial.mMaterial && mMaterial.mMaterial->Source()->isHardwareCanvas())
		{
			FlushQuadBatch();
			static_cast<FCanvasTexture*>(mMaterial.mMaterial->Source()->GetTexture())->NeedUpdate();
		}

		VulkanDescriptorSet* descriptorset = mMaterial.mMaterial ? static_cast<VkMaterial*>(mMaterial.mMaterial)->GetDescriptorSet(mMaterial) : descriptors->GetNullTextureDescriptorSet();

		if (!vk_drawreuse || descriptorset != mLastMaterialSet || mLastMaterialLayers != mPipelineKey.NumTextureLayers)
		{
			FlushQuadBatch();
			if (!vk_drawreuse)
				mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), 0, descriptors->GetFixedDescriptorSet());
			mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), 2, descriptorset);
			mLastMaterialSet = descriptorset;
			mLastMaterialLayers = mPipelineKey.NumTextureLayers;
			if (SceneProfiling) SceneMainSurfaceProfile.DescriptorBinds += vk_drawreuse ? 1 : 2;
		}
		mMaterial.mChanged = false;
	}
}

void VkRenderState::ApplyHWBufferSet()
{
	uint32_t matrixOffset = mMatrixBufferWriter.Offset();
	uint32_t streamDataOffset = mStreamBufferWriter.StreamDataOffset();
	if (mViewpointOffset != mLastViewpointOffset || matrixOffset != mLastMatricesOffset || streamDataOffset != mLastStreamDataOffset || mLastBufferLayers != mPipelineKey.NumTextureLayers)
	{
		auto passManager = fb->GetRenderPassManager();
		auto descriptors = fb->GetDescriptorSetManager();

		uint32_t offsets[3] = { mViewpointOffset, matrixOffset, streamDataOffset };
		FlushQuadBatch();
		if (SceneProfiling) SceneMainSurfaceProfile.DescriptorBinds += 2;
		mLastBufferLayers = mPipelineKey.NumTextureLayers;
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), 0, fb->GetDescriptorSetManager()->GetFixedDescriptorSet());
		mCommandBuffer->bindDescriptorSet(VK_PIPELINE_BIND_POINT_GRAPHICS, passManager->GetPipelineLayout(mPipelineKey.NumTextureLayers), 1, descriptors->GetHWBufferDescriptorSet(), 3, offsets);

		mLastViewpointOffset = mViewpointOffset;
		mLastMatricesOffset = matrixOffset;
		mLastStreamDataOffset = streamDataOffset;
	}
}

void VkRenderState::WaitForStreamBuffers()
{
	FlushQuadBatch();
	if (SceneProfiling) ++SceneMainSurfaceProfile.StreamWaits;
	fb->WaitForCommands(false);
	mSubmissionWork = 0;
	mStreamBufferWriter.Reset();
	mMatrixBufferWriter.Reset();
}

void VkRenderState::Bind(int bindingpoint, uint32_t offset)
{
	if (bindingpoint == VIEWPOINT_BINDINGPOINT)
	{
		mViewpointOffset = offset;
		mNeedApply = true;
	}
}

void VkRenderState::BeginFrame()
{
	++ModelInstanceFrames;
	VerifyGpuCullResult();
	mCullPending = mCullDrawReady = false;
	mGpuCullUsedThisFrame = mGpuAdmissionUsedThisFrame = false;
	mGpuWallsActive = mGpuIndexedDrawing = mGpuIndexedView = false;
	mGpuAppearanceUsedThisFrame = false;
	mGpuResidentUsedThisFrame = mGpuResidentView = false;
	mGpuResidentLast = mGpuResidentCapture = UINT32_MAX;
	mGpuResidentCaptureRevision = 0;
	EndRetainedWallRun();
	if (++mRetainedFrame == 0)
	{
		mRetainedParameters.ResetFrames();
		++mRetainedFrame;
	}
	mRetainedViewOffset = 0xffffffff;
	mRetainedViewCount = 0;
	mRetainedIndexCursor = 0;
	mMaterial.Reset();
	mSubmissionWork = 0;
}

void VkRenderState::VerifyGpuCullResult()
{
	auto buffers = fb->GetBufferManager();
	if (mCullLastVerified && buffers->GpuCullArgs->Memory())
	{
		const uint32_t prev = reinterpret_cast<const uint32_t*>(buffers->GpuCullArgs->Memory())[1];
		static uint32_t cullLogCounter = 0;
		if (prev != mCullLastExpected || (cullLogCounter++ % 120) == 0)
			Printf("GPU-Driven cull: prims=%u gpu_visible=%u cpu_visible=%u %s\n", mCullLastPrims, prev, mCullLastExpected, prev == mCullLastExpected ? "OK" : "MISMATCH");
	}
	if (mCullLastVerified && !mCullExpectedCommands.empty())
	{
		const auto actual = static_cast<const uint32_t*>(buffers->GpuCullCommands->Memory());
		const bool matched = actual && memcmp(actual, mCullExpectedCommands.data(), mCullExpectedCommands.size() * sizeof(uint32_t)) == 0;
		static unsigned logs = 0;
		if (!matched || logs++ % 120 == 0)
			Printf("G37COMMANDS checked=%u active=%u %s\n", unsigned(mCullExpectedCommands.size() / 5), mCullLastExpected, matched ? "OK" : "MISMATCH");
	}
	mCullExpectedCommands.clear();
	mCullLastVerified = false;
}

void VkRenderState::RecordGpuCull()
{
	mCullDrawReady = false;
	if (!mCullPending) return;
	mCullLastVerified = false;
	mCullExpectedCommands.clear();
	mCullPending = false;
	if (!vk_gpu_driven_cull || mGpuCullFailed) return;
	try
	{
		EnsureGpuCullResources();
	}
	catch (const CVulkanError& error)
	{
		mGpuCullFailed = true;
		Printf("GPU-Driven cull unavailable: %s\n", error.what());
		return;
	}
	auto buffers = fb->GetBufferManager();
	auto visible = static_cast<VkHardwareDataBuffer*>(buffers->GpuCullVisible.get());
	auto args = static_cast<VkHardwareDataBuffer*>(buffers->GpuCullArgs.get());
	auto bounds = static_cast<VkHardwareDataBuffer*>(buffers->PrimitiveBounds.get());
	auto primTable = static_cast<VkHardwareDataBuffer*>(buffers->PrimitiveTable.get());
	auto commands = static_cast<VkHardwareDataBuffer*>(buffers->GpuCullCommands.get());
	auto materials = static_cast<VkHardwareDataBuffer*>(buffers->PrimitiveMaterials.get());
	auto drawMats = static_cast<VkHardwareDataBuffer*>(buffers->GpuCullDrawMaterials.get());
	auto admission = static_cast<VkHardwareDataBuffer*>(buffers->GpuCullAdmission.get());
	if (!visible->mBuffer || !args->mBuffer || !bounds->mBuffer || !primTable->mBuffer || !commands->mBuffer ||
		!materials->mBuffer || !drawMats->mBuffer) return;

	// [Karto] Portal/view dispatches can share a command buffer; replace and retire outputs instead of rewriting live epochs.
	const uint32_t dispatchCount = mCullCommandCount ? mCullCommandCount : mCullPrims;
	const size_t visibleBytes = size_t(dispatchCount) * sizeof(uint32_t);
	if ((uint64_t(dispatchCount) + 63) / 64 > fb->device->PhysicalDevice.Properties.Properties.limits.maxComputeWorkGroupCount[0] ||
		mCullPrims > 1024 * 1024 || dispatchCount > 1024 * 1024) return;
	try
	{
		auto reserve = [&](IDataBuffer* buffer, size_t bytes)
		{
			if (mGpuCullUsedThisFrame || buffer->Size() < bytes || !buffer->Memory())
				buffer->SetData((bytes + 4095) & ~size_t(4095), nullptr, BufferUsageType::Persistent);
		};
		reserve(buffers->GpuCullVisible.get(), visibleBytes);
		reserve(buffers->GpuCullArgs.get(), 5 * sizeof(uint32_t));
		reserve(buffers->GpuCullCommands.get(), size_t(dispatchCount) * 5 * sizeof(uint32_t));
		reserve(buffers->GpuCullDrawMaterials.get(), size_t(mCullPrims) * 4 * sizeof(uint32_t));
		if (mGpuCullUsedThisFrame || !mGpuCullSet)
		{
			fb->GetCommands()->DrawDeleteList->Add(std::move(mGpuCullSet));
			fb->GetCommands()->DrawDeleteList->Add(std::move(mGpuCullPool));
			mGpuCullPool = DescriptorPoolBuilder().AddPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8).MaxSets(1).Create(fb->device.get());
			mGpuCullSet = mGpuCullPool->allocate(mGpuCullSetLayout.get());
		}
		mGpuCullUsedThisFrame = true;
	}
	catch (const CVulkanError& error)
	{
		mGpuCullFailed = true;
		Printf("GPU-Driven cull unavailable: %s\n", error.what());
		return;
	}
	if (!buffers->GpuCullArgs->Memory() || bounds->Size() < size_t(mCullPrims) * 8 * sizeof(float) ||
		buffers->PrimitiveTable->Size() < size_t(mCullPrims) * 8 * sizeof(uint32_t) ||
		buffers->PrimitiveMaterials->Size() < size_t(mCullPrims) * 4 * sizeof(uint32_t) ||
		(mCullForDraw && admission->Size() < visibleBytes)) return;

	if (mCullVerify && mCullCommandCount)
	{
		const auto rows = static_cast<const uint32_t*>(admission->Memory());
		const auto table = static_cast<const uint32_t*>(primTable->Memory());
		const auto boxes = static_cast<const float*>(bounds->Memory());
		mCullExpectedCommands.assign(size_t(mCullCommandCount) * 5, 0);
		for (uint32_t i = 0; i < mCullCommandCount; ++i)
		{
			const auto row = rows[i];
			if (row >= mCullPrims || table[row * 8 + 7] != 1 || table[row * 8 + 1] != 4 || table[row * 8 + 3] != 6) continue;
			auto command = mCullExpectedCommands.data() + size_t(i) * 5;
			command[0] = 6;
			command[3] = table[row * 8];
			bool visible = true;
			for (unsigned p = 0; p < 6; ++p)
			{
				const auto plane = mCullPlanes + p * 4;
				const auto box = boxes + size_t(row) * 8;
				float distance = plane[3];
				for (unsigned axis = 0; axis < 3; ++axis) distance += plane[axis] * box[axis + (plane[axis] >= 0 ? 4 : 0)];
				if (distance < 0) { visible = false; break; }
			}
			command[1] = visible ? 1 : 0;
		}
	}

	WriteDescriptors()
		.AddBuffer(mGpuCullSet.get(), 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, visible->mBuffer.get())
		.AddBuffer(mGpuCullSet.get(), 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, args->mBuffer.get())
		.AddBuffer(mGpuCullSet.get(), 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bounds->mBuffer.get())
		.AddBuffer(mGpuCullSet.get(), 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, primTable->mBuffer.get())
		.AddBuffer(mGpuCullSet.get(), 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, commands->mBuffer.get())
		.AddBuffer(mGpuCullSet.get(), 5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, materials->mBuffer.get())
		.AddBuffer(mGpuCullSet.get(), 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, drawMats->mBuffer.get())
		.AddBuffer(mGpuCullSet.get(), 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, admission->mBuffer.get())
		.Execute(fb->device.get());

	auto cmd = fb->GetCommands()->GetDrawCommands();
	cmd->fillBuffer(args->mBuffer->buffer, 0, 5 * sizeof(uint32_t), 0);
	{
		VkBufferMemoryBarrier bb{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
		bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		bb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bb.buffer = args->mBuffer->buffer;
		bb.offset = 0;
		bb.size = VK_WHOLE_SIZE;
		cmd->pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
	}

	struct { float planes[24]; uint32_t primCount; uint32_t forDraw; uint32_t commandCount; } pc = {};
	memcpy(pc.planes, mCullPlanes, sizeof(pc.planes));
	pc.primCount = mCullPrims;
	pc.forDraw = mCullCommandCount ? (mCullVerify ? 2 : 3) : (mCullForDraw ? 1 : 0);
	pc.commandCount = mCullCommandCount;
	cmd->bindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, mGpuCullPipeline.get());
	cmd->bindDescriptorSet(VK_PIPELINE_BIND_POINT_COMPUTE, mGpuCullPipelineLayout.get(), 0, mGpuCullSet.get());
	cmd->pushConstants(mGpuCullPipelineLayout.get(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	cmd->dispatch((dispatchCount + 63) / 64, 1, 1);
	{
		VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT;
		cmd->pipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
	}
	mCullLastExpected = mCullExpected;
	mCullLastPrims = mCullPrims;
	mCullLastVerified = mCullVerify;
	mCullDrawReady = mCullForDraw;
}

void VkRenderState::EndRenderPass()
{
	EndRetainedWallRun();
	if (mCommandBuffer)
	{
		FlushQuadBatch();
		mCommandBuffer->endRenderPass();
		mPushConstantsValid = false;
		mLastMaterialSet = nullptr;
		mLastMaterialLayers = -1;
		mLastBufferLayers = -1;
		mCommandBuffer = nullptr;
		mPipelineKey = {};

		mLastViewpointOffset = 0xffffffff;
		mLastVertexBuffer = nullptr;
		mLastSecondaryVertexBuffer = nullptr;
		mLastIndexBuffer = nullptr;
		mLastModelMatrixEnabled = true;
		mLastTextureMatrixEnabled = true;
	}
}

void VkRenderState::EndFrame()
{
	EndRenderPass();
	// [Karto] The visibility copy must sit outside a render pass; the frame-end fence completes it before the next frame
	// reads it.
	RecordWorldModeReadback();
	RecordGpuCull();
	mMatrixBufferWriter.Reset();
	mStreamBufferWriter.Reset();
}

void VkRenderState::EnableDrawBuffers(int count, bool apply)
{
	if (mRenderTarget.DrawBuffers != count)
	{
		EndRenderPass();
		mRenderTarget.DrawBuffers = count;
	}
}

void VkRenderState::SetRenderTarget(VkTextureImage *image, VulkanImageView *depthStencilView, int width, int height, VkFormat format, VkSampleCountFlagBits samples)
{
	EndRenderPass();

	mRenderTarget.Image = image;
	mRenderTarget.DepthStencil = depthStencilView;
	mRenderTarget.Width = width;
	mRenderTarget.Height = height;
	mRenderTarget.Format = format;
	mRenderTarget.Samples = samples;
}

void VkRenderState::BeginRenderPass(VulkanCommandBuffer *cmdbuffer)
{
	VkRenderPassKey key = {};
	key.DrawBufferFormat = mRenderTarget.Format;
	key.Samples = mRenderTarget.Samples;
	key.DrawBuffers = mRenderTarget.DrawBuffers;
	key.DepthStencil = !!mRenderTarget.DepthStencil;

	mPassSetup = fb->GetRenderPassManager()->GetRenderPass(key);

	auto &framebuffer = mRenderTarget.Image->RSFramebuffers[key];
	if (!framebuffer)
	{
		auto buffers = fb->GetBuffers();
		FramebufferBuilder builder;
		builder.RenderPass(mPassSetup->GetRenderPass(0));
		builder.Size(mRenderTarget.Width, mRenderTarget.Height);
		builder.AddAttachment(mRenderTarget.Image->View.get());
		if (key.DrawBuffers > 1)
			builder.AddAttachment(buffers->SceneFog.View.get());
		if (key.DrawBuffers > 2)
			builder.AddAttachment(buffers->SceneNormal.View.get());
		if (key.DepthStencil)
			builder.AddAttachment(mRenderTarget.DepthStencil);
		builder.DebugName("VkRenderPassSetup.Framebuffer");
		framebuffer = builder.Create(fb->device.get());
	}

	// Only clear depth+stencil if the render target actually has that
	if (!mRenderTarget.DepthStencil)
		mClearTargets &= ~(CT_Depth | CT_Stencil);

	RenderPassBegin beginInfo;
	beginInfo.RenderPass(mPassSetup->GetRenderPass(mClearTargets));
	beginInfo.RenderArea(0, 0, mRenderTarget.Width, mRenderTarget.Height);
	beginInfo.Framebuffer(framebuffer.get());
	beginInfo.AddClearColor(screen->mSceneClearColor[0], screen->mSceneClearColor[1], screen->mSceneClearColor[2], screen->mSceneClearColor[3]);
	if (key.DrawBuffers > 1)
		beginInfo.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	if (key.DrawBuffers > 2)
		beginInfo.AddClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	beginInfo.AddClearDepthStencil(1.0f, 0);
	beginInfo.Execute(cmdbuffer);

	mMaterial.mChanged = true;
	mClearTargets = 0;
}

/////////////////////////////////////////////////////////////////////////////

void VkRenderStateMolten::Draw(int dt, int index, int count, bool apply)
{
	if (dt == DT_TriangleFan)
	{
		IIndexBuffer *oldIndexBuffer = mIndexBuffer;
		mIndexBuffer = fb->GetBufferManager()->FanToTrisIndexBuffer.get();

		if (apply || mNeedApply)
			Apply(DT_Triangles);
		else
			ApplyVertexBuffers();

		mCommandBuffer->drawIndexed((count - 2) * 3, 1, 0, index, 0);

		mIndexBuffer = oldIndexBuffer;
	}
	else
	{
		if (apply || mNeedApply)
			Apply(dt);

		if (SceneProfiling) ++SceneMainSurfaceProfile.GPUDraws;
	mCommandBuffer->draw(count, 1, index, 0);
	}
}

CCMD(model_instance_stats)
{
	const double f = ModelInstanceFrames ? double(ModelInstanceFrames) : 1.0;
	Printf("MODELINSTANCES frames=%llu draws=%.1f instances=%.1f\n", (unsigned long long)ModelInstanceFrames, ModelInstanceDraws / f,
		ModelInstances / f);
	ModelInstanceDraws = ModelInstances = ModelInstanceFrames = 0;
}
