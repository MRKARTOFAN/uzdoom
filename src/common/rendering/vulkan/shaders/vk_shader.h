/*
** vk_shader.h
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

#include <memory>
#include <cstddef>

#include "vectors.h"
#include "matrix.h"
#include "name.h"
#include "hw_renderstate.h"
#include "zvulkan/vulkanbuilders.h"
#include <list>
#include "shaderuniforms.h"

#define SHADER_MIN_REQUIRED_TEXTURE_LAYERS 11

class VulkanRenderDevice;
class VulkanDevice;
class VulkanShader;
class VkPPShader;
class PPShader;

struct MatricesUBO
{
	VSMatrix ModelMatrix;
	VSMatrix NormalModelMatrix;
	VSMatrix TextureMatrix;
};

#define MAX_STREAM_DATA ((int)(65536 / sizeof(StreamData)))

struct StreamUBO
{
	StreamData data[MAX_STREAM_DATA];
};

struct PushConstants
{
	int uTextureMode;
	float uAlphaThreshold;
	FVector2 uClipSplit;

	// Lighting + Fog
	float uLightLevel;
	float uFogDensity;
	float uLightFactor;
	float uLightDist;
	int uFogEnabled;

	// dynamic lights
	int uLightIndex;

	// Blinn glossiness and specular level
	FVector2 uSpecularMaterial;

	// bone animation
	int uBoneIndexBase;

	int uDataIndex;
	int uAppearanceIndex, padding2, padding3;
};

class VkShaderProgram
{
public:
	std::unique_ptr<VulkanShader> vert;
	std::unique_ptr<VulkanShader> frag;
};

struct alignas(16) RetainedSurfaceData
{
	StreamData Stream;
	VSMatrix TextureTransform;
	FVector4 Light;
	FVector4 ClipSpecular;
	int TextureMode;
	int FogEnabled;
	int LightIndex;
	float AlphaThreshold;
	int AppearanceIndex;
};
// [Karto] Fixed std430 offsets in the GLSL table; layout drift must fail the build rather than corrupt surfaces.
static_assert(sizeof(StreamData) == 384, "Retained GLSL StreamData layout changed");
static_assert(sizeof(RetainedSurfaceData) == 512 && alignof(RetainedSurfaceData) == 16, "Retained GLSL row stride changed");
static_assert(offsetof(RetainedSurfaceData, Stream) == 0, "Retained GLSL stream offset changed");
static_assert(offsetof(RetainedSurfaceData, TextureTransform) == 384, "Retained GLSL texture transform offset changed");
static_assert(offsetof(RetainedSurfaceData, Light) == 448, "Retained GLSL light offset changed");
static_assert(offsetof(RetainedSurfaceData, ClipSpecular) == 464, "Retained GLSL clip/specular offset changed");
static_assert(offsetof(RetainedSurfaceData, TextureMode) == 480, "Retained GLSL texture mode offset changed");
static_assert(offsetof(RetainedSurfaceData, FogEnabled) == 484, "Retained GLSL fog offset changed");
static_assert(offsetof(RetainedSurfaceData, LightIndex) == 488, "Retained GLSL light index offset changed");
static_assert(offsetof(RetainedSurfaceData, AlphaThreshold) == 492, "Retained GLSL alpha offset changed");

struct alignas(16) SectorAppearanceRow
{
	FVector4PalEntry FogColor;
	FVector4 VertexColor;
	float Desaturation;
	float LightDist;
	float LightFactor;
	float FogDensity;
	float LightLevel;
	int FogEnabled;
	int Padding[2];
};
static_assert(sizeof(SectorAppearanceRow) == 64, "Shared sector appearance row stride changed");
static constexpr unsigned kSharedAppearanceCapacity = 4096;
static_assert(offsetof(SectorAppearanceRow, FogColor) == 0, "Shared sector appearance fog offset changed");
static_assert(offsetof(SectorAppearanceRow, VertexColor) == 16, "Shared sector appearance vertex-colour offset changed");
static_assert(offsetof(SectorAppearanceRow, Desaturation) == 32, "Shared sector appearance desaturation offset changed");
static_assert(offsetof(SectorAppearanceRow, FogEnabled) == 52, "Shared sector appearance fog-enabled offset changed");

struct alignas(16) GpuWorldSurfaceData
{
	RetainedSurfaceData Surface;
	uint32_t TextureIDs[4];
};
static_assert(sizeof(GpuWorldSurfaceData) == 528, "Indexed world GLSL row stride changed");
static_assert(offsetof(GpuWorldSurfaceData, TextureIDs) == 512, "Indexed world texture offset changed");

class VkShaderManager
{
public:
	VkShaderManager(VulkanRenderDevice* fb);
	~VkShaderManager();

	void Deinit();

	VkShaderProgram *GetEffect(int effect, EPassType passType);
	VkShaderProgram *Get(unsigned int eff, bool alphateston, EPassType passType);
	VkShaderProgram* GetRetained(EPassType passType, bool independentTime);
	VkShaderProgram* GetGpuWorld(EPassType passType, bool alphaTest, int variant = 1);
	VkShaderProgram* GetModelInstances(EPassType passType, bool alphaTest);
	bool CompileNextShader();
	void RebuildSpecialized(int features);
	int GetSpecializedFeatures() const { return SpecFeatures; }
	bool IsCompileDone() const { return compileIndex == -1; }

	VkPPShader* GetVkShader(PPShader* shader);

	void AddVkPPShader(VkPPShader* shader);
	void RemoveVkPPShader(VkPPShader* shader);

private:
	std::unique_ptr<VulkanShader> LoadVertShader(FString shadername, const char *vert_lump, const char *defines, AllShaderIndex type);
	std::unique_ptr<VulkanShader> LoadFragShader(FString shadername, const char *frag_lump, const char *material_lump, const char *light_lump, const char *defines, bool alphatest, bool gbufferpass, AllShaderIndex type);

	FString GetTargetGlslVersion();
	FString LoadPublicShaderLump(const char *lumpname);
	FString LoadPrivateShaderLump(const char *lumpname);

	static ShaderIncludeResult OnInclude(FString headerName, FString includerName, size_t depth);

	VulkanRenderDevice* fb = nullptr;

	std::vector<VkShaderProgram> mMaterialShaders[MAX_PASS_TYPES];
	std::vector<VkShaderProgram> mMaterialShadersNAT[MAX_PASS_TYPES];
	std::vector<VkShaderProgram> mEffectShaders[MAX_PASS_TYPES];
	VkShaderProgram RetainedShaders[MAX_PASS_TYPES][2];
	VkShaderProgram GpuWorldShaders[MAX_PASS_TYPES][2];
	VkShaderProgram GpuResidentWorldShaders[MAX_PASS_TYPES][2];
	VkShaderProgram WorldModeShaders[MAX_PASS_TYPES][2];
	VkShaderProgram WorldModeVisibilityShaders[MAX_PASS_TYPES][2];
	VkShaderProgram WorldModeSpriteShaders[MAX_PASS_TYPES][2];
	VkShaderProgram ModelInstanceShaders[MAX_PASS_TYPES][2];
	VkShaderProgram WorldModeStampShaders[MAX_PASS_TYPES][2];
	uint8_t compilePass = 0, compileState = 0;
	int compileIndex = 0;
	int SpecFeatures = 63;
	bool Specializing = false;
	VkShaderProgram CompileDefaultMaterial(int pass, bool alphatest, bool modelInstances = false);

	std::list<VkPPShader*> PPShaders;
	friend class VkPPShader;
};
