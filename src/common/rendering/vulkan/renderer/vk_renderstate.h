/*
** vk_renderstate.h
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

#include "vulkan/system/vk_hwbuffer.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_streambuffer.h"
#include "vulkan/renderer/vk_retainedparams.h"

#include "name.h"

#include <unordered_map>
#include <utility>

#include "hw_renderstate.h"
#include "hw_material.h"

class VulkanRenderDevice;
class VkRenderPassSetup;
class VkTextureImage;

class VkRenderState : public FRenderState
{
public:
	VkRenderState(VulkanRenderDevice* fb);
	virtual ~VkRenderState() = default;

	// Draw commands
	void ClearScreen() override;
	void Draw(int dt, int index, int count, bool apply = true) override;
	void DrawIndexed(int dt, int index, int count, bool apply = true) override;
	void DrawModelIndexed(int index, int count) override;
	void DrawIndirectFans(const FIndirectFan* fans, int count) override;
	void DrawOpaqueQuad(int index) override;
	void DrawBatchedQuad(int index) override;
	void DrawBatchedQuadSameState(int index) override;
	void DrawRetainedQuad(int index) override;
	void PrepareRetainedWallRun(bool changed, bool owned = false) override;
	bool SetRetainedWorldOwnership(bool enabled) override;
	void DrawOwnedRetainedQuad(int index, uint64_t version) override;
	void EndRetainedWallRun() override;
	void DrawRetainedTriangles(int index, int count, unsigned surface) override;
	bool DrawBakedPrimitive(uint32_t iboOffset, uint32_t indexCount) override;
	bool DrawGpuDrivenGeometry(uint32_t drawCount) override;
	bool BeginWorldCapture(FWorldCaptureSink* sink) override;
	void EndWorldCapture() override;
	bool UploadWorldMode(const FWorldModeUpload& data) override;
	void ReleaseWorldMode() override;
	bool SetWorldModeVisibility(const uint8_t* visible, uint32_t count) override;
	bool SetWorldModePoolClusters(const uint32_t* firstIndex, const uint32_t* indexCount, uint32_t opaque, uint32_t total) override;
	bool WriteWorldMode(const FWorldModeWrite* writes, size_t count) override;
	bool GetWorldModeTextureIDs(const FMaterialState& state, uint32_t ids[4]) override;
	uint32_t AddSpriteRow() override;
	uint64_t GetGpuTextureEpoch() override;
	bool SupportsSpriteInstances() override;
	bool SupportsParticleExpansion() override;
	bool ExpandParticles(IVertexBuffer* target, const float* particles, uint32_t particleCount, uint64_t serial,
		const uint32_t* entries, const float* ranges, uint32_t rangeCount, const FGpuParticleParams& params, const FGpuParticleIndices& indices,
		const FGpuLightPack* lights) override;
	bool ComputeSpriteLights(const uint32_t* instances, uint32_t instanceCount, uint32_t instanceWords, uint64_t instanceSerial,
		const FGpuLightPack& pack) override;
	bool DrawSpriteInstances(const uint32_t* instances, uint32_t instanceCount, uint32_t instanceWords, uint64_t instanceSerial,
		const uint32_t* visible, uint32_t count, const uint32_t* frames, uint32_t frameWords, uint64_t frameVersion, const float* params,
		uint32_t paramVec4s) override;
	void DrawWorldMode(bool alphaTest) override;
	bool DrawWorldModeVisibility() override;
	bool GetWorldModeVisibility(const uint32_t*& stamps, size_t& count, uint32_t& stamp) override;
	uint32_t PeekWorldModeStamp() override;
	bool GetWorldModeLightPatch(FWorldModeLightPatch& patch) override;
	bool IsWorldModeStale() override;
	bool BeginGpuDrivenWalls() override;
	void CaptureGpuStaticMaterial(FGpuStaticMaterial& out) override;
	bool DrawGpuDrivenWall(uint32_t slot, uint32_t bakeRow = UINT32_MAX) override;
	bool HasGpuResidentAppearance(uint32_t id, uint64_t revision) const override;
	void SetGpuResidentCapture(uint32_t id, uint64_t revision) override;
	bool DrawGpuResidentWall(uint32_t slot, uint32_t id, uint64_t revision) override;
	void EndGpuResidentBatch() override;
	uint64_t GpuResidentDrawCount() const override { return mGpuResidentDraws; }
	void EndGpuDrivenWalls() override;
	uint32_t GpuDrivenGeometryCount() const override { return mCullPending && mCullForDraw ? mCullPrims : 0; }
	void BeginGPUProfile(const char* name) override;
	void EndGPUProfile() override;

	// Immediate render state change commands. These only change infrequently and should not clutter the render state.
	bool SetDepthClamp(bool on) override;
	void SetDepthMask(bool on) override;
	void SetDepthFunc(int func) override;
	void SetDepthRange(float min, float max) override;
	void SetColorMask(bool r, bool g, bool b, bool a) override;
	void SetStencil(int offs, int op, int flags = -1) override;
	void SetCulling(int mode) override;
	void EnableClipDistance(int num, bool state) override;
	void Clear(int targets) override;
	void EnableStencil(bool on) override;
	void SetScissor(int x, int y, int w, int h) override;
	void SetViewport(int x, int y, int w, int h) override;
	void EnableDepthTest(bool on) override;
	void EnableMultisampling(bool on) override;
	void EnableLineSmooth(bool on) override;
	void EnableDrawBuffers(int count, bool apply) override;

	void BeginFrame();
	void SetRenderTarget(VkTextureImage *image, VulkanImageView *depthStencilView, int width, int height, VkFormat Format, VkSampleCountFlagBits samples);
	void Bind(int bindingpoint, uint32_t offset);
	void EndRenderPass();
	void EndFrame();

protected:
	bool mGpuOwnedWorldDrawing = false;
	bool mRetainedDrawing = false;
	bool mRetainedIndependentTime = false;
	bool mOwnedRetainedView = false;
	float mRetainedTimer = 0;
	uint32_t mRetainedFrame = 0;
	RetainedParameterCache<RetainedSurfaceData> mRetainedParameters;
	RetainedContextCache<RetainedSurfaceData> mRetainedContexts;
	uint32_t mRetainedWallContext = 0;
	bool mRetainedWallRun = false;
	RetainedSurfaceData mRetainedWallSnapshot = {};
	bool mRetainedBuffersReady = false;
	uint32_t mRetainedViewOffset = 0xffffffff;
	uint32_t mRetainedViewCount = 0;
	uint32_t mRetainedBase = 0;
	uint32_t mRetainedSurfaceIndex = 0;
	uint32_t mRetainedIndexCursor = 0;
	uint32_t mRetainedBatchStart = 0;
	uint32_t mRetainedBatchCount = 0;
	FMaterialState mRetainedMaterial = {};
	VSMatrix mRetainedModelMatrix;
	bool mRetainedModelEnabled = false;
	uint64_t mRetainedOwnerVersion = 0;
	bool GatherRetained(const uint32_t* indices, uint32_t count, uint32_t surface, uint64_t ownerVersion = 0);
	bool SelectSharedAppearance(const FSharedAppearanceKey& key) override;
	void StoreSharedAppearance(const FSharedAppearanceKey& key) override;
	void UploadCoverage(const void* data, size_t bytes) override;
	size_t mCoverageBytes = 0;
	void UploadStaticBake(const void* verts, size_t vertBytes, const void* indices, size_t indexBytes, const void* table, size_t tableBytes) override;
	void UploadPrimitiveGrid(const void* offsets, size_t offsetBytes, const void* refs, size_t refBytes) override;
	void UploadPrimitiveBounds(const void* bounds, size_t bytes) override;
	void UploadPrimitiveMaterials(const void* rows, size_t bytes) override;
	void UploadGpuCullAdmission(const void* rows, size_t bytes) override;
	void SetGpuCullPlanes(const float* planes24, uint32_t primCount, uint32_t expectedVisible, bool verify, bool forDraw, uint32_t commandCount = 0) override;
	bool mCullForDraw = false;
	uint32_t mCullCommandCount = 0;
	std::vector<uint32_t> mCullExpectedCommands;
	uint32_t mGpuWallBatchStart = 0;
	uint32_t mGpuWallBatchCount = 0;
	bool mGpuWallsActive = false;
	int mGpuIndexedDrawing = 0;
	bool mGpuIndexedView = false;
	bool mGpuAppearanceUsedThisFrame = false;
	std::unique_ptr<VulkanDescriptorSet> mGpuWorldSet;
	std::vector<GpuWorldSurfaceData> mGpuWorldResident;
	std::vector<uint64_t> mGpuWorldResidentRevision;
	std::vector<HWWorldLightingState> mGpuWorldResidentLighting;
	std::vector<uint64_t> mGpuWorldResidentTextureEpoch;
	std::unique_ptr<VulkanDescriptorSet> mGpuResidentSet;
	bool mGpuResidentUsedThisFrame = false;
	bool mGpuResidentView = false;
	uint32_t mGpuResidentCapture = UINT32_MAX;
	uint64_t mGpuResidentCaptureRevision = 0;
	uint32_t mGpuResidentLast = UINT32_MAX;
	uint64_t mGpuResidentDraws = 0;
	static constexpr uint32_t GpuResidentCapacity = 32768 * 3;
	bool DrawGpuIndexedWall(uint32_t slot, uint32_t bakeRow);
	void FillGpuWorldRow(GpuWorldSurfaceData& row, const uint32_t textures[4]);
	bool CaptureWorldDraw(int dt, int index, int count, bool indexed);
	void IssueWorldModeDraws(uint32_t first, uint32_t count, int variant, int dataIndex);
	void RecordWorldModeReadback();
	struct WorldModeResources;
	FWorldCaptureSink* mWorldCapture = nullptr;
	std::shared_ptr<WorldModeResources> mWorldMode;
	// [Karto] SetData retires the previous buffers through the delete list, so frames in flight keep theirs.
	std::vector<GpuWorldSurfaceData> mSpriteRows;
	std::unique_ptr<VkHardwareDataBuffer> mSpriteRowBuffer, mSpriteInstanceBuffer, mSpriteFrameBuffer, mSpriteParamBuffer, mSpriteVisibleBuffer;
	uint64_t mSpriteInstanceSerial = 0, mSpriteFrameVersion = 0;
	uint32_t mSpriteFrameWords = 0;
	bool mLightReady = false, mLightFailed = false;
	uint32_t mLightPackSerial = 0;
	uint64_t mSpriteLightSerial = 0, mParticleLightSerial = 0;
	std::unique_ptr<VulkanShader> mLightShader;
	std::unique_ptr<VulkanDescriptorSetLayout> mLightSetLayout;
	std::unique_ptr<VulkanPipelineLayout> mLightPipelineLayout;
	std::unique_ptr<VulkanPipeline> mLightPipeline;
	std::unique_ptr<VulkanDescriptorPool> mSpriteLightPool, mParticleLightPool;
	std::unique_ptr<VulkanDescriptorSet> mSpriteLightSet, mParticleLightSet;
	std::unique_ptr<VkHardwareDataBuffer> mLightPack;
	std::unique_ptr<VulkanBuffer> mSpriteLightOut, mParticleLightOut;
	void EnsureLightResources();
	bool DispatchLights(VulkanBuffer* items, uint32_t count, uint32_t words, uint32_t mode, const FGpuLightPack& pack,
		std::unique_ptr<VulkanBuffer>& out, std::unique_ptr<VulkanDescriptorPool>& pool, std::unique_ptr<VulkanDescriptorSet>& set,
		VkPipelineStageFlags reader);
	std::unique_ptr<VulkanDescriptorSet> mSpriteSet;
	std::unique_ptr<VulkanBuffer> mSpriteQuadIndices;
	bool mParticleReady = false, mParticleFailed = false;
	uint64_t mParticleSerial = 0;
	std::unique_ptr<VulkanShader> mParticleShader;
	std::unique_ptr<VulkanDescriptorSetLayout> mParticleSetLayout;
	std::unique_ptr<VulkanPipelineLayout> mParticlePipelineLayout;
	std::unique_ptr<VulkanPipeline> mParticlePipeline;
	std::unique_ptr<VulkanDescriptorPool> mParticlePool;
	std::unique_ptr<VulkanDescriptorSet> mParticleSet;
	std::unique_ptr<VkHardwareDataBuffer> mParticleData, mParticleEntries, mParticleRanges, mParticleProgram, mParticleFrameIndices;
	uint64_t mParticleProgramVersion = 0;
	struct FParticleOutput { std::unique_ptr<VulkanBuffer> Buffer; uint64_t Frame; };
	std::vector<FParticleOutput> mParticleOutputs;
	void EnsureParticleResources();
	void ApplyGpuIndexedSlot(uint32_t slot, int variant, VulkanDescriptorSet* appearanceSet);
	FMaterialState mGpuWallMaterial = {};
	bool mGpuCullUsedThisFrame = false;
	bool mGpuAdmissionUsedThisFrame = false;
	bool mCullDrawReady = false;
	float mCullPlanes[24] = {};
	uint32_t mCullPrims = 0;
	uint32_t mCullExpected = 0;
	uint32_t mCullLastExpected = 0;
	uint32_t mCullLastPrims = 0;
	bool mCullVerify = false;
	bool mCullLastVerified = false;
	bool mGpuCullFailed = false;
	bool mCullPending = false;
	bool mGpuCullReady = false;
	std::unique_ptr<VulkanShader> mGpuCullShader;
	std::unique_ptr<VulkanDescriptorSetLayout> mGpuCullSetLayout;
	std::unique_ptr<VulkanDescriptorPool> mGpuCullPool;
	std::unique_ptr<VulkanDescriptorSet> mGpuCullSet;
	std::unique_ptr<VulkanPipelineLayout> mGpuCullPipelineLayout;
	std::unique_ptr<VulkanPipeline> mGpuCullPipeline;
	void EnsureGpuCullResources();
	void VerifyGpuCullResult();
	void RecordGpuCull();
	static uint64_t HashSharedAppearance(const FSharedAppearanceKey& key);
	std::unordered_map<uint64_t, std::pair<FSharedAppearanceKey, unsigned>> mSharedAppearances;
	unsigned mSharedAppearanceCount = 0;
	bool CollectRetainedParameters();
	bool CanReuseRetainedState(const VkPipelineKey& key) const;
	VkPipelineKey MakePipelineKey(int dt) const;
	PushConstants MakePushConstants();
	void FlushQuadBatch();
	void AdvanceSubmissionBudget();
	void Apply(int dt, bool countSubmission = true);
	void ApplyRenderPass(int dt);
	void ApplyStencilRef();
	void ApplyDepthBias();
	void ApplyScissor();
	void ApplyViewport();
	void ApplyStreamData();
	void ApplyMatrices();
	void ApplyPushConstants();
	void ApplyHWBufferSet();
	void ApplyVertexBuffers();
	void ApplyMaterial();

	void BeginRenderPass(VulkanCommandBuffer *cmdbuffer);
	void WaitForStreamBuffers();

	VulkanRenderDevice* fb = nullptr;

	bool mDepthClamp = true;
	VulkanCommandBuffer *mCommandBuffer = nullptr;
	VkPipelineKey mPipelineKey = {};
	VkRenderPassSetup *mPassSetup = nullptr;
	int mClearTargets = 0;
	bool mNeedApply = true;

	int mScissorX = 0, mScissorY = 0, mScissorWidth = -1, mScissorHeight = -1;
	int mViewportX = 0, mViewportY = 0, mViewportWidth = -1, mViewportHeight = -1;
	float mViewportDepthMin = 0.0f, mViewportDepthMax = 1.0f;
	bool mScissorChanged = true;
	bool mViewportChanged = true;

	bool mDepthTest = false;
	bool mDepthWrite = false;
	bool mStencilTest = false;

	bool mStencilRefChanged = false;
	int mStencilRef = 0;
	int mStencilOp = 0;
	int mDepthFunc = 0;
	int mColorMask = 15;
	int mCullMode = 0;

	PushConstants mPushConstants = {};
	PushConstants mLastPushConstants = {};
	bool mPushConstantsValid = false;
	int mLastPushLayers = -1;
	VulkanDescriptorSet* mLastMaterialSet = nullptr;
	int mLastMaterialLayers = -1;
	int mLastBufferLayers = -1;
	int mQuadBatchIndex = 0;
	int mQuadBatchCount = 0;
	// [Karto] FlushQuadBatch draws the pending instanced model draw, so any state change ends it in order.
	int mModelBatchIndex = 0, mModelBatchIndices = 0, mModelBatchBase = 0, mModelBatchCount = 0;
	bool mModelInstancing = false;
	// [Karto] One host-visible indirect command buffer, split by frame parity so a parked frame never reads a slot being rewritten.
	std::unique_ptr<VkHardwareBuffer> mIndirectFanBuffer;
	uint64_t mIndirectFanFrame = 0;
	size_t mIndirectFanOffset = 0;

	uint32_t mLastViewpointOffset = 0xffffffff;
	uint32_t mLastMatricesOffset = 0xffffffff;
	uint32_t mLastStreamDataOffset = 0xffffffff;
	uint32_t mViewpointOffset = 0;

	VkStreamBufferWriter mStreamBufferWriter;
	VkMatrixBufferWriter mMatrixBufferWriter;

	int mLastVertexOffsets[2] = { 0, 0 };
	IVertexBuffer *mLastVertexBuffer = nullptr;
	IVertexBuffer* mLastSecondaryVertexBuffer = nullptr;
	IIndexBuffer *mLastIndexBuffer = nullptr;

	bool mLastModelMatrixEnabled = true;
	bool mLastTextureMatrixEnabled = true;

	int mSubmissionWork = 0;

	struct RenderTarget
	{
		VkTextureImage *Image = nullptr;
		VulkanImageView *DepthStencil = nullptr;
		int Width = 0;
		int Height = 0;
		VkFormat Format = VK_FORMAT_R16G16B16A16_SFLOAT;
		VkSampleCountFlagBits Samples = VK_SAMPLE_COUNT_1_BIT;
		int DrawBuffers = 1;
	} mRenderTarget;
};

class VkRenderStateMolten : public VkRenderState
{
public:
	using VkRenderState::VkRenderState;

	void Draw(int dt, int index, int count, bool apply = true) override;
};
