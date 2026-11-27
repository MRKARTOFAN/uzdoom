/*
** vk_shader.cpp
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

#include "vk_shader.h"
#include "vk_ppshader.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/system/vk_renderdevice.h"
#include "hw_shaderpatcher.h"
#include "filesystem.h"
#include "engineerrors.h"
#include "version.h"
#include "cmdlib.h"
#include "hw_viewpointuniforms.h"

EXTERN_CVAR(Bool, vk_sharedappearance)

ShaderIncludeResult VkShaderManager::OnInclude(FString headerName, FString includerName, size_t depth)
{
	if (depth > 8)
		I_Error("Too much include recursion!");

	FString includeguardname;
	includeguardname << "_HEADERGUARD_" << headerName.GetChars();
	includeguardname.ReplaceChars("/\\.", '_');

	FString code;
	code << "#ifndef " << includeguardname.GetChars() << "\n";
	code << "#define " << includeguardname.GetChars() << "\n";
	code << "#line 1\n";

	int lumpNum = fileSystem.CheckNumForFullName(headerName.GetChars(), 0); // look in engine pk3 first

	if(lumpNum == -1)
	{
		lumpNum = fileSystem.CheckNumForFullName(headerName.GetChars());
	}

	if(lumpNum >= 0)
	{
		code << GetStringFromLump(lumpNum, false);
	}

	code << "\n#endif\n";

	return ShaderIncludeResult(headerName.GetChars(), code.GetChars());
}

bool VkShaderManager::CompileNextShader()
{
	const char *mainvp = "shaders/glsl/main.vp";
	const char *mainfp = "shaders/glsl/main.fp";
	int i = compileIndex;

	if (compileState == 0)
	{
		// regular material shaders

		assert(i < MATERIAL_SHADER_COUNT);

		if (i == SHADER_Default)
			mMaterialShaders[compilePass].push_back(CompileDefaultMaterial(compilePass, true));
		else
		{
		VkShaderProgram prog;
		FString defines = defaultshaders[i].Defines;
		prog.vert = LoadVertShader(defaultshaders[i].ShaderName, mainvp, defines.GetChars(), static_cast<AllShaderIndex>(i));
		prog.frag = LoadFragShader(defaultshaders[i].ShaderName, mainfp, defaultshaders[i].gettexelfunc, defaultshaders[i].lightfunc, defines.GetChars(), true, compilePass == GBUFFER_PASS, static_cast<AllShaderIndex>(i));
		mMaterialShaders[compilePass].push_back(std::move(prog));
		}

		compileIndex++;
		if (defaultshaders[compileIndex].ShaderName == nullptr)
		{
			compileIndex = 0;
			compileState++;
		}
	}
	else if (compileState == 1)
	{
		// NAT material shaders

		assert(i < MATERIAL_SHADER_COUNT);

		if (i == SHADER_Default)
			mMaterialShadersNAT[compilePass].push_back(CompileDefaultMaterial(compilePass, false));
		else
		{
		VkShaderProgram natprog;
		FString natdefines = defaultshaders[i].Defines;
		natprog.vert = LoadVertShader(defaultshaders[i].ShaderName, mainvp, natdefines.GetChars(), static_cast<AllShaderIndex>(i));
		natprog.frag = LoadFragShader(defaultshaders[i].ShaderName, mainfp, defaultshaders[i].gettexelfunc, defaultshaders[i].lightfunc, natdefines.GetChars(), false, compilePass == GBUFFER_PASS, static_cast<AllShaderIndex>(i));
		mMaterialShadersNAT[compilePass].push_back(std::move(natprog));
		}

		compileIndex++;
		if (compileIndex == SHADER_NoTexture)
		{
			compileIndex = 0;
			compileState++;
			if (usershaders.Size() == 0) compileState++;
		}
	}
	else if (compileState == 2)
	{
		// user shaders
		auto &shader = usershaders[i];

		assert(shader.shaderType < MATERIAL_SHADER_COUNT);

		const FString& name = ExtractFileBase(shader.shader.GetChars());
		FString defines = defaultshaders[shader.shaderType].Defines + shader.defines;

		VkShaderProgram prog;
		prog.vert = LoadVertShader(name, mainvp, defines.GetChars(), static_cast<AllShaderIndex>(shader.shaderType));
		prog.frag = LoadFragShader(name, mainfp, shader.shader.GetChars(), defaultshaders[shader.shaderType].lightfunc, defines.GetChars(), true, compilePass == GBUFFER_PASS, static_cast<AllShaderIndex>(shader.shaderType));
		mMaterialShaders[compilePass].push_back(std::move(prog));

		compileIndex++;
		if (compileIndex >= (int)usershaders.Size())
		{
			compileIndex = 0;
			compileState++;
		}
	}
	else if (compileState == 3)
	{
		// Effect shaders

		assert(i < EFFECT_SHADER_COUNT);

		VkShaderProgram prog;
		prog.vert = LoadVertShader(effectshaders[i].ShaderName, effectshaders[i].vp, effectshaders[i].defines, static_cast<AllShaderIndex>(i + FIRST_EFFECT_SHADER));
		prog.frag = LoadFragShader(effectshaders[i].ShaderName, effectshaders[i].fp1, effectshaders[i].fp2, effectshaders[i].fp3, effectshaders[i].defines, true, compilePass == GBUFFER_PASS, static_cast<AllShaderIndex>(i + FIRST_EFFECT_SHADER));
		mEffectShaders[compilePass].push_back(std::move(prog));

		compileIndex++;
		if (compileIndex >= MAX_EFFECTS)
		{
			compileIndex = 0;
			compilePass++;
			if (compilePass == MAX_PASS_TYPES)
			{
				compileIndex = -1; // we're done.
				return true;
			}
			compileState = 0;
		}
	}
	return false;
}

VkShaderProgram VkShaderManager::CompileDefaultMaterial(int pass, bool alphatest, bool modelInstances)
{
	const auto &desc = defaultshaders[SHADER_Default];
	FString defines = desc.Defines;
	defines << "\n#define SHARED_APPEARANCE\n";
	if (modelInstances) defines << "#define MODEL_INSTANCES\n";
	VkShaderProgram prog;
	Specializing = true;
	prog.vert = LoadVertShader(desc.ShaderName, "shaders/glsl/main.vp", defines.GetChars(), static_cast<AllShaderIndex>(SHADER_Default));
	prog.frag = LoadFragShader(desc.ShaderName, "shaders/glsl/main.fp", desc.gettexelfunc, desc.lightfunc, defines.GetChars(), alphatest, pass == GBUFFER_PASS, static_cast<AllShaderIndex>(SHADER_Default));
	Specializing = false;
	return prog;
}

void VkShaderManager::RebuildSpecialized(int features)
{
	SpecFeatures = features;
	for (int pass = 0; pass < MAX_PASS_TYPES; pass++)
	{
		if (mMaterialShaders[pass].size() > SHADER_Default) mMaterialShaders[pass][SHADER_Default] = CompileDefaultMaterial(pass, true);
		if (mMaterialShadersNAT[pass].size() > SHADER_Default) mMaterialShadersNAT[pass][SHADER_Default] = CompileDefaultMaterial(pass, false);
		for (int a = 0; a < 2; a++)
		{
			RetainedShaders[pass][a] = {};
			GpuWorldShaders[pass][a] = {};
			GpuResidentWorldShaders[pass][a] = {};
			WorldModeShaders[pass][a] = {};
			WorldModeVisibilityShaders[pass][a] = {};
			WorldModeSpriteShaders[pass][a] = {};
			ModelInstanceShaders[pass][a] = {};
			WorldModeStampShaders[pass][a] = {};
		}
	}
}

VkShaderManager::VkShaderManager(VulkanRenderDevice* fb) : fb(fb)
{
	//CompileNextShader();
}

VkShaderManager::~VkShaderManager()
{
}

void VkShaderManager::Deinit()
{
	while (!PPShaders.empty())
		RemoveVkPPShader(PPShaders.back());
}

VkShaderProgram *VkShaderManager::GetEffect(int effect, EPassType passType)
{
	if (compileIndex == -1 && effect >= 0 && effect < MAX_EFFECTS && mEffectShaders[passType][effect].frag)
	{
		return &mEffectShaders[passType][effect];
	}
	return nullptr;
}

VkShaderProgram *VkShaderManager::Get(unsigned int eff, bool alphateston, EPassType passType)
{
	if (compileIndex != -1)
		return &mMaterialShaders[0][0];
	// indices 0-2 match the warping modes, 3 no texture, the following are custom
	if (!alphateston && eff < SHADER_NoTexture)
	{
		return &mMaterialShadersNAT[passType][eff];	// Non-alphatest shaders are only created for default, warp1+2. The rest won't get used anyway
	}
	else if (eff < (unsigned int)mMaterialShaders[passType].size())
	{
		return &mMaterialShaders[passType][eff];
	}
	return nullptr;
}

static const char *shaderBindings = R"(

	layout(set = 0, binding = 0) uniform sampler2D ShadowMap;
	layout(set = 0, binding = 1) uniform sampler2DArray LightMap;
	#ifdef SUPPORTS_RAYTRACING
	layout(set = 0, binding = 2) uniform accelerationStructureEXT TopLevelAS;
	#endif

	layout(set = 1, binding = 1, std140) uniform readonly MatricesUBO {
		mat4 ModelMatrix;
		mat4 NormalModelMatrix;
		mat4 TextureMatrix;
	};

	struct StreamData
	{
		vec4 uObjectColor;
		vec4 uObjectColor2;
		vec4 uDynLightColor;
		vec4 uAddColor;
		vec4 uTextureAddColor;
		vec4 uTextureModulateColor;
		vec4 uTextureBlendColor;
		vec4 uFogColor;
		float uDesaturationFactor;
		float uInterpolationFactor;
		float timer; // timer data for material shaders
		int useVertexData;
		vec4 uVertexColor;
		vec4 uVertexNormal;

		vec4 uGlowTopPlane;
		vec4 uGlowTopColor;
		vec4 uGlowBottomPlane;
		vec4 uGlowBottomColor;

		vec4 uGradientTopPlane;
		vec4 uGradientBottomPlane;

		vec4 uSplitTopPlane;
		vec4 uSplitBottomPlane;

		vec4 uDetailParms;
		vec4 uNpotEmulation;
		vec4 padding1, padding2, padding3;
	};

	layout(set = 1, binding = 2, std140) uniform readonly StreamUBO {
		StreamData data[MAX_STREAM_DATA];
	};

	// light buffers
	layout(set = 1, binding = 3, std430) buffer readonly LightBufferSSO
	{
	    vec4 lights[];
	};

	// bone matrix buffers
	layout(set = 1, binding = 4, std430) buffer readonly BoneBufferSSO
	{
	    mat4 bones[];
	};

	// textures
	layout(set = 2, binding = 0) uniform sampler2D tex;
	layout(set = 2, binding = 1) uniform sampler2D texture2;
	layout(set = 2, binding = 2) uniform sampler2D texture3;
	layout(set = 2, binding = 3) uniform sampler2D texture4;
	layout(set = 2, binding = 4) uniform sampler2D texture5;
	layout(set = 2, binding = 5) uniform sampler2D texture6;
	layout(set = 2, binding = 6) uniform sampler2D texture7;
	layout(set = 2, binding = 7) uniform sampler2D texture8;
	layout(set = 2, binding = 8) uniform sampler2D texture9;
	layout(set = 2, binding = 9) uniform sampler2D texture10;
	layout(set = 2, binding = 10) uniform sampler2D texture11;
	layout(set = 2, binding = 11) uniform sampler2D texture12;

	// This must match the PushConstants struct
	layout(push_constant) uniform PushConstants
	{
		int uTextureMode;
		float uAlphaThreshold;
		vec2 uClipSplit;

		// Lighting + Fog
		float uLightLevelRaw;
		float uFogDensityRaw;
		float uLightFactorRaw;
		float uLightDistRaw;
		int uFogEnabledRaw;

		// dynamic lights
		int uLightIndex;

		// Blinn glossiness and specular level
		vec2 uSpecularMaterial;

		// bone animation
		int uBoneIndexBase;

		int uDataIndex;
		int uAppearanceIndex, padding3;
	};

	// material types
	#if defined(SPECULAR)
	#define normaltexture texture2
	#define speculartexture texture3
	#define brighttexture texture4
	#define detailtexture texture5
	#define glowtexture texture6
	#elif defined(PBR)
	#define normaltexture texture2
	#define metallictexture texture3
	#define roughnesstexture texture4
	#define aotexture texture5
	#define brighttexture texture6
	#define detailtexture texture7
	#define glowtexture texture8
	#else
	#define brighttexture texture2
	#define detailtexture texture3
	#define glowtexture texture4
	#endif

	#define uObjectColor data[uDataIndex].uObjectColor
	#define uObjectColor2 data[uDataIndex].uObjectColor2
	#define uDynLightColor data[uDataIndex].uDynLightColor
	#define uAddColor data[uDataIndex].uAddColor
	#define uTextureBlendColor data[uDataIndex].uTextureBlendColor
	#define uTextureModulateColor data[uDataIndex].uTextureModulateColor
	#define uTextureAddColor data[uDataIndex].uTextureAddColor
	#define uFogColor data[uDataIndex].uFogColor
	#define uDesaturationFactor data[uDataIndex].uDesaturationFactor
	#define uInterpolationFactor data[uDataIndex].uInterpolationFactor
	#define timer data[uDataIndex].timer
	#define useVertexData data[uDataIndex].useVertexData
	#define uVertexColor data[uDataIndex].uVertexColor
	#define uVertexNormal data[uDataIndex].uVertexNormal
	#define uGlowTopPlane data[uDataIndex].uGlowTopPlane
	#define uGlowTopColor data[uDataIndex].uGlowTopColor
	#define uGlowBottomPlane data[uDataIndex].uGlowBottomPlane
	#define uGlowBottomColor data[uDataIndex].uGlowBottomColor
	#define uGradientTopPlane data[uDataIndex].uGradientTopPlane
	#define uGradientBottomPlane data[uDataIndex].uGradientBottomPlane
	#define uSplitTopPlane data[uDataIndex].uSplitTopPlane
	#define uSplitBottomPlane data[uDataIndex].uSplitBottomPlane
	#define uDetailParms data[uDataIndex].uDetailParms
	#define uNpotEmulation data[uDataIndex].uNpotEmulation

	#define SUPPORTS_SHADOWMAPS
	#define VULKAN_COORDINATE_SYSTEM
	#define HAS_UNIFORM_VERTEX_DATA

	// GLSL spec 4.60, 8.15. Noise Functions
	// https://www.khronos.org/registry/OpenGL/specs/gl/GLSLangSpec.4.60.pdf
	//  "The noise functions noise1, noise2, noise3, and noise4 have been deprecated starting with version 4.4 of GLSL.
	//   When not generating SPIR-V they are defined to return the value 0.0 or a vector whose components are all 0.0.
	//   When generating SPIR-V the noise functions are not declared and may not be used."
	// However, we need to support mods with custom shaders created for OpenGL renderer
	float noise1(float) { return 0; }
	vec2 noise2(vec2) { return vec2(0); }
	vec3 noise3(vec3) { return vec3(0); }
	vec4 noise4(vec4) { return vec4(0); }
)";

static FString GetShaderBindings(const char* defines, bool vertex)
{
	FString bindings = shaderBindings;
	if (vertex && strstr(defines, "MODEL_INSTANCES"))
	{
		// [Karto] After the declarations, so the matrix block keeps its members; a function because main.vp's main() has a
		// local named bones that would hide the buffer from a macro.
		bindings << "\nmat4 ModelInstanceMatrix(int which) { return bones[uBoneIndexBase + gl_InstanceIndex * 2 + which]; }\n"
			"#define ModelMatrix ModelInstanceMatrix(0)\n"
			"#define NormalModelMatrix ModelInstanceMatrix(1)\n";
	}
	const bool gpuWorld = strstr(defines, "GPU_INDEXED_WORLD") != nullptr;
	if (!strstr(defines, "RETAINED_WORLD") && !gpuWorld)
	{
		if (strstr(defines, "SHARED_APPEARANCE"))
		{
			FString shared = R"(
layout(set = 1, binding = 6, std430) readonly buffer SectorAppearanceBuffer { vec4 appearanceData[]; };
#define uLightDist (uAppearanceIndex == 0 ? uLightDistRaw : appearanceData[uAppearanceIndex * 4 + 2].y)
#define uLightFactor (uAppearanceIndex == 0 ? uLightFactorRaw : appearanceData[uAppearanceIndex * 4 + 2].z)
#define uFogDensity (uAppearanceIndex == 0 ? uFogDensityRaw : appearanceData[uAppearanceIndex * 4 + 2].w)
#define uLightLevel (uAppearanceIndex == 0 ? uLightLevelRaw : appearanceData[uAppearanceIndex * 4 + 3].x)
#define uFogEnabled (uAppearanceIndex == 0 ? uFogEnabledRaw : int(appearanceData[uAppearanceIndex * 4 + 3].y))
)";
			bindings.Substitute("\t// material types", (shared + "\t// material types").GetChars());
			bindings.Substitute("#define uFogColor data[uDataIndex].uFogColor", "#define uFogColor (uAppearanceIndex == 0 ? data[uDataIndex].uFogColor : appearanceData[uAppearanceIndex * 4 + 0])");
			bindings.Substitute("#define uVertexColor data[uDataIndex].uVertexColor", "#define uVertexColor (uAppearanceIndex == 0 ? data[uDataIndex].uVertexColor : appearanceData[uAppearanceIndex * 4 + 1])");
			bindings.Substitute("#define uDesaturationFactor data[uDataIndex].uDesaturationFactor", "#define uDesaturationFactor (uAppearanceIndex == 0 ? data[uDataIndex].uDesaturationFactor : appearanceData[uAppearanceIndex * 4 + 2].x)");
		}
		else
		{
			bindings.Substitute("\t// material types", "\n#define uLightLevel uLightLevelRaw\n#define uFogDensity uFogDensityRaw\n#define uLightFactor uLightFactorRaw\n#define uLightDist uLightDistRaw\n#define uFogEnabled uFogEnabledRaw\n\t// material types");
		}
		return bindings;
	}
	bindings.Substitute("\t};\n\n\t// material types", "\t} retainedPush;\n\n\t// material types");
	bindings.Substitute("mat4 TextureMatrix;", "mat4 retainedUnusedTextureMatrix;");
	bindings.Substitute("data[uDataIndex]", "retainedData[retainedSurfaceID].Stream");
	if (strstr(defines, "RETAINED_INDEPENDENT_TIME"))
	{
		bindings.Substitute("float uLightDist;", "float retainedTimer;");
		bindings.Substitute("#define timer retainedData[retainedSurfaceID].Stream.timer", "#define timer retainedPush.retainedTimer");
	}
	FString table = R"(
	struct RetainedSurfaceData
	{
		StreamData Stream;
		mat4 TextureTransform;
		vec4 Light;
		vec4 ClipSpecular;
		int TextureMode;
		int FogEnabled;
		int LightIndex;
		float AlphaThreshold;
		int AppearanceIndex;
	};
	layout(set = 1, binding = 5, std430) readonly buffer RetainedSurfaceBuffer
	{
		RetainedSurfaceData retainedData[];
	};
	layout(set = 1, binding = 6, std430) readonly buffer SectorAppearanceBuffer
	{
		vec4 appearanceData[];
	};
	#define retainedAppRow (retainedData[retainedSurfaceID].AppearanceIndex * 4)
	#define TextureMatrix retainedData[retainedSurfaceID].TextureTransform
	#define uTextureMode retainedData[retainedSurfaceID].TextureMode
	#define uAlphaThreshold retainedData[retainedSurfaceID].AlphaThreshold
	#define uClipSplit retainedData[retainedSurfaceID].ClipSpecular.xy
	#define uLightDist retainedData[retainedSurfaceID].Light.x
	#define uLightFactor retainedData[retainedSurfaceID].Light.y
	#define uFogDensity retainedData[retainedSurfaceID].Light.z
	#define uLightLevel retainedData[retainedSurfaceID].Light.w
	#define uFogEnabled retainedData[retainedSurfaceID].FogEnabled
	#define uLightIndex retainedData[retainedSurfaceID].LightIndex
	#define uSpecularMaterial retainedData[retainedSurfaceID].ClipSpecular.zw
	#define uBoneIndexBase -1
	)";
	if (gpuWorld)
	{
		table.Substitute("layout(set = 1, binding = 5, std430) readonly buffer RetainedSurfaceBuffer\n\t{\n\t\tRetainedSurfaceData retainedData[];\n\t};",
			"struct GpuWorldSurfaceData { RetainedSurfaceData Surface; uvec4 TextureIDs; };\nlayout(set = 3, binding = 0, std430) readonly buffer GpuWorldBuffer { GpuWorldSurfaceData gpuWorldData[]; };");
		bindings.Substitute("retainedData[retainedSurfaceID]", "gpuWorldData[retainedSurfaceID].Surface");
		table.Substitute("retainedData[retainedSurfaceID]", "gpuWorldData[retainedSurfaceID].Surface");
		const char* names[] = { "tex", "texture2", "texture3", "texture4", "texture5", "texture6", "texture7", "texture8", "texture9", "texture10", "texture11", "texture12" };
		const char* components[] = { "x", "y", "z", "w" };
		for (int i = 0; i < 12; ++i)
		{
			FString declaration, replacement;
			declaration.Format("layout(set = 2, binding = %d) uniform sampler2D %s;", i, names[i]);
			if (!vertex) replacement.Format("#define %s gpuWorldTextures[nonuniformEXT(vGpuWorldTextureIDs.%s)]", names[i], components[i < 4 ? i : 3]);
			bindings.Substitute(declaration.GetChars(), replacement.GetChars());
		}
		if (!vertex) table << "\nlayout(set = 2, binding = 0) uniform sampler2D gpuWorldTextures[];\n";
	}
	// [Karto] Location 16, not 31: with gl_Position and clip distances, 31 exceeds AMD's maxVertexOutputComponents.
	if (!vertex && gpuWorld && strstr(defines, "WORLD_MODE"))
	{
		table.Substitute("#define uLightIndex gpuWorldData[retainedSurfaceID].Surface.LightIndex",
			"layout(location = 16) flat in int vWorldLight;\n\t#define uLightIndex WorldLightList(vWorldLight)");
		bindings.Substitute("layout(set = 1, binding = 3, std430) buffer readonly LightBufferSSO",
			"layout(set = 3, binding = 9, std430) buffer readonly WorldModeLightBuffer");
	}
	if (!vertex && gpuWorld && strstr(defines, "GPU_SPRITE_INSTANCES"))
	{
		bindings.Substitute("#define uDynLightColor gpuWorldData[retainedSurfaceID].Surface.Stream.uDynLightColor",
			"layout(location = 17) flat in vec3 vSpriteDynLight;\n\t#define uDynLightColor (gpuWorldData[retainedSurfaceID].Surface.Stream.uDynLightColor + vec4(vSpriteDynLight, 0.0))");
	}
	if (vertex)
	{
		if (gpuWorld)
		{
			table << "\nlayout(location = 14) flat out uvec4 vGpuWorldTextureIDs;\nlayout(location = 15) flat out uint vRetainedSurfaceID;\nuint retainedSurfaceID;\n";
			if (strstr(defines, "GPU_RESIDENT_WORLD"))
				table << "layout(set = 3, binding = 1, std430) readonly buffer GpuResidentReferenceBuffer { uint gpuResidentRefs[]; };\n";
			if (strstr(defines, "WORLD_MODE"))
				table << "layout(location = 9) in float aSurfaceID;\nlayout(set = 3, binding = 1, std430) readonly buffer WorldModeSurfaceRows { uint worldSurfaceRows[]; };\n"
					"layout(set = 3, binding = 3, std430) readonly buffer WorldModeSurfaceParams { uint worldSurfaceParams[]; };\n"
					"layout(set = 3, binding = 4, std430) readonly buffer WorldModeParams { vec4 worldParams[]; };\n"
					"layout(set = 3, binding = 5, std430) readonly buffer WorldModePlaneMatrices { mat4 worldPlaneMatrices[]; };\n"
					"layout(set = 3, binding = 6, std430) readonly buffer WorldModePlaneHeights { vec4 worldPlaneHeights[]; };\n"
					"layout(set = 3, binding = 7, std430) readonly buffer WorldModeVertexRefs { uvec4 worldVertexRefs[]; };\n"
					"layout(set = 3, binding = 8, std430) readonly buffer WorldModeSurfaceLights { int worldSurfaceLights[]; };\n"
					"layout(location = 16) flat out int vWorldLight;\n";
			if (strstr(defines, "WORLD_MODE_VISIBILITY") || strstr(defines, "WORLD_MODE_STAMP")) table << "layout(location = 30) flat out uint vWorldSurface;\n";
			if (strstr(defines, "GPU_SPRITE_INSTANCES")) table << R"(
// [Karto] Vertex shader port of SpriteInstanceGeometry, kept equal to HWSprite::ProcessRecord on the CPU.
layout(set = 3, binding = 4, std430) readonly buffer SpriteParamBuffer { uint spriteData[]; };
layout(set = 3, binding = 3, std430) readonly buffer SpriteFrameBuffer { uint spriteFrames[]; };
layout(set = 3, binding = 5, std430) readonly buffer SpriteInstanceBuffer { uint spriteInst[]; };
layout(set = 3, binding = 6, std430) readonly buffer SpriteVisibleBuffer { uint spriteVisible[]; };
layout(set = 3, binding = 7, std430) readonly buffer SpriteLightBuffer { vec4 spriteLights[]; };
vec4 spriteParam(uint k) { return vec4(uintBitsToFloat(spriteData[k * 4u]), uintBitsToFloat(spriteData[k * 4u + 1u]), uintBitsToFloat(spriteData[k * 4u + 2u]), uintBitsToFloat(spriteData[k * 4u + 3u])); }
vec4 spritePos;
vec2 spriteUV;
uint spriteRow;
uvec4 spriteTextureIDs;
layout(location = 17) flat out vec3 vSpriteDynLight;
float spriteF(uint i) { return uintBitsToFloat(spriteInst[i]); }
float spriteFrameF(uint i) { return uintBitsToFloat(spriteFrames[i]); }
vec3 spriteRotate(vec3 k, float deg, vec3 d) { float a = radians(deg), c = cos(a), s = sin(a); return d * c + cross(k, d) * s + k * (dot(k, d) * (1.0 - c)); }
void SpriteCompute()
{
	uint vis = uint(gl_InstanceIndex) * 4u;
	uint b = spriteVisible[vis] * 40u;
	if (spriteParam(5u).w != 0.0) vSpriteDynLight = spriteLights[spriteVisible[vis]].xyz;
	else vSpriteDynLight = vec3(uintBitsToFloat(spriteVisible[vis + 1u]), uintBitsToFloat(spriteVisible[vis + 2u]), uintBitsToFloat(spriteVisible[vis + 3u]));
	vec4 p0 = spriteParam(0u);	// view x, y, z, tic fraction
	vec4 p1 = spriteParam(1u);	// view vector x, y, bob fraction, frame tic
	vec4 p2 = spriteParam(2u);	// ortho flag, ortho yaw, gl_spriteclip, gl_sclipthreshold
	vec4 p3 = spriteParam(3u);	// gl_sclipfactor, indexed draw, pixel stretch, roll axis yaw
	uint flags = spriteInst[b + 23u];
	float t = p0.w;
	vec3 prev = vec3(spriteF(b + 0u), spriteF(b + 1u), spriteF(b + 2u));
	vec3 cur = vec3(spriteF(b + 3u), spriteF(b + 4u), spriteF(b + 5u));
	vec3 pos = (flags & 1u) != 0u ? cur : prev * (1.0 - t) + cur * t;
	spriteRow = spriteInst[b + 27u];
	spritePos = vec4(0.0, 0.0, 0.0, 1.0);
	spriteUV = vec2(0.0);
	spriteTextureIDs = uvec4(0u);
	float height = spriteF(b + 12u);
	if ((flags & 2048u) != 0u && abs(pos.x - p0.x) < 2.0 && abs(pos.y - p0.y) < 2.0 && p0.z >= pos.z - 2.0 && p0.z <= pos.z + height + 2.0)
		return;	// too close to the camera: collapse the quad, as Process skips the sprite
	float x = pos.x + spriteF(b + 6u), y = pos.y + spriteF(b + 7u);
	float z = pos.z + spriteF(b + 8u) - spriteF(b + 11u);
	if ((flags & 32u) != 0u)
	{
		float bt = (flags & 64u) != 0u ? t : p1.z;
		z += sin(radians((spriteF(b + 13u) + p1.w * spriteF(b + 14u) + bt) * (180.0 / 32.0))) * 8.0 * spriteF(b + 15u);
	}
	float ang = p2.x != 0.0 ? p2.y : degrees(atan(pos.y - p0.y, pos.x - p0.x));
	float sprang = (flags & 2u) != 0u ? spriteF(b + 16u) : ang - spriteF(b + 16u);
	uint fb = spriteInst[b + 26u] * 196u;
	uint header = spriteFrames[fb];
	if ((header & 131072u) != 0u) return;	// no bindless IDs for this frame: the CPU draws it (WorldModeGpuSpriteFailed)
	bool flipagain = (flags & 8u) != 0u;
	float a = (header & 65536u) != 0u ? (flipagain ? -sprang + 360.0 + 202.5 : sprang + 202.5) : (flipagain ? -sprang + 360.0 + 191.25 : sprang + 191.25);
	a -= 360.0 * floor(a / 360.0);
	uint rot = uint(a * (16.0 / 360.0)) & 15u;
	uint rb = fb + 4u + rot * 12u;
	spriteTextureIDs = uvec4(spriteFrames[rb], spriteFrames[rb + 1u], spriteFrames[rb + 2u], spriteFrames[rb + 3u]);
	bool mirror = ((header >> rot) & 1u) != 0u;
	float UL = spriteFrameF(rb + 4u), UR = spriteFrameF(rb + 5u), vt = spriteFrameF(rb + 6u), vb = spriteFrameF(rb + 7u);
	float left = spriteFrameF(rb + 8u), top = spriteFrameF(rb + 9u), width = spriteFrameF(rb + 10u), rheight = spriteFrameF(rb + 11u);
	if ((flags & 16u) != 0u) { float s = vt; vt = vb; vb = s; }
	bool xflip = ((flags & 4u) != 0u) != ((flags & 8u) != 0u);
	float ul, ur;
	if (mirror != xflip) { left = -width - left; ul = UL; ur = UR; }
	else { ul = UR; ur = UL; }
	float sx = spriteF(b + 9u), sy = spriteF(b + 10u);
	left *= sx; width *= sx; top *= sy; rheight *= sy;
	if ((flags & 16384u) != 0u) { float mult = 1.0 / sqrt(p3.z); left *= mult * p3.z; width *= mult * p3.z; top *= mult; rheight *= mult; }
	float rightfac = -left, leftfac = rightfac - width;
	float z1 = z - top;
	float z2 = z1 - rheight;
	float spriteheight = sy * rheight;
	float spriteclip = p2.z, threshold = p2.w, factor = p3.x;
	if (spriteheight > 0.0 && spriteclip > 0.0)
	{
		bool clipthing = (flags & 128u) != 0u;
		bool smarterclip = !clipthing && spriteclip == 3.0;
		bool corpse = (flags & 256u) != 0u, monster = (flags & 512u) != 0u, nogravity = (flags & 1024u) != 0u;
		if ((clipthing || spriteclip > 1.0) && (flags & 32u) == 0u)
		{
			float btm = spriteF(b + 17u);
			float ctop = (spriteF(b + 21u) + spriteF(b + 18u) * pos.x + spriteF(b + 19u) * pos.y) * spriteF(b + 20u);
			float diffb = (z2 + 1.0) - btm;
			float difft = (z1 - 1.0) - ctop;
			if (diffb >= 0.0) diffb = 0.0;
			if (smarterclip)
			{
				if (corpse || spriteheight > abs(diffb))
					diffb *= clamp(abs(diffb) * factor / (spriteheight + 1.0), 0.5, 1.0);
				if (diffb == 0.0)
				{
					if (difft <= 0.0) difft = 0.0;
					if (difft >= threshold && (!monster || nogravity || corpse || difft > threshold)) difft = 0.0;
					if (spriteheight > abs(difft)) difft *= clamp(abs(difft) * factor / (spriteheight + 1.0), 0.5, 1.0);
					z2 -= difft;
					z1 -= difft;
				}
			}
			if (diffb <= -threshold && (!monster || nogravity || corpse || diffb < -1.8 * threshold)) diffb = 0.0;
			z2 -= diffb;
			z1 -= diffb;
		}
	}
	float spbias = 1.0;
	vec3 view = vec3(p0.x, p0.z, p0.y);
	if (spriteclip == -1.0)
	{
		vec4 a6 = spriteParam(6u), a7 = spriteParam(7u);	// w: anamorphic min bias (clamped), no-limit flag
		float minbias = a6.w;
		if (a7.w == 0.0) minbias = max(minbias, 1.0 - spriteF(b + 35u) / sqrt((p0.x - pos.x) * (p0.x - pos.x) + (p0.y - pos.y) * (p0.y - pos.y)));
		float btm = spriteF(b + 17u);
		float ctop = (spriteF(b + 21u) + spriteF(b + 18u) * pos.x + spriteF(b + 19u) * pos.y) * spriteF(b + 20u);
		float vbtm = (spriteF(b + 39u) + spriteF(b + 36u) * p0.x + spriteF(b + 37u) * p0.y) * spriteF(b + 38u);
		float vtop = (spriteF(b + 21u) + spriteF(b + 18u) * p0.x + spriteF(b + 19u) * p0.y) * spriteF(b + 20u);
		float bi = z2 < p0.z && vbtm < p0.z ? min((btm - p0.z) / (z2 - p0.z), (vbtm - p0.z) / (z2 - p0.z)) : 1.0;
		float ti = z1 > p0.z && vtop > p0.z ? min((ctop - p0.z) / (z1 - p0.z), (vtop - p0.z) / (z1 - p0.z)) : 1.0;
		if ((flags & 131072u) != 0u) { bi = 1.0; ti = 1.0; }
		spbias = clamp(min(bi, ti), minbias, 1.0);
	}
	uint corner = uint(gl_VertexIndex) % 6u;
	uint c = corner == 0u ? 0u : corner == 1u ? 1u : corner == 2u ? 2u : corner == 3u ? 2u : corner == 4u ? 1u : 3u;
	if (p3.y != 0.0) c = uint(gl_VertexIndex) & 3u;	// indexed draw (r_sprite_indexed): the index is the corner
	bool right = c == 1u || c == 3u, bottom = c >= 2u;
	float fac = right ? rightfac : leftfac;
	spritePos = vec4(x - p1.y * fac, bottom ? z2 : z1, y + p1.x * fac, 1.0);
	spritePos.xyz = spritePos.xyz * spbias + view * (1.0 - spbias);
	bool roll = (flags & 4096u) != 0u, xyb = (flags & 32768u) != 0u;
	vec2 off = vec2(spriteF(b + 32u), spriteF(b + 33u));
	if (roll || xyb || off != vec2(0.0))
	{
		vec4 p4 = spriteParam(4u), p5 = spriteParam(5u), p6 = spriteParam(6u), p7 = spriteParam(7u);
		float ar = radians(p3.w), ps = p3.z;
		vec3 rollAxis = vec3(cos(ar), 0.0, sin(ar)), pitchAxis = vec3(-sin(ar), 0.0, cos(ar));
		vec3 centre = vec3(x - p1.y * (leftfac + rightfac) * 0.5, (z1 + z2) * 0.5, y + p1.x * (leftfac + rightfac) * 0.5) * spbias + view * (1.0 - spbias);
		vec3 origin = vec3(x, z, y);
		vec3 res = xyb ? p6.xyz * off.x + p7.xyz * off.y : p4.xyz * off.x + p5.xyz * off.y;
		float rollDeg = spriteF(b + 31u);
		if (xyb)
		{
			vec3 d = spritePos.xyz - centre;
			d.y *= ps;
			if (roll)
			{
				vec3 o = (flags & 8192u) != 0u ? vec3(0.0) : origin - centre;
				d = spriteRotate(pitchAxis, -p4.w, spriteRotate(rollAxis, rollDeg, d - o)) + o;
			}
			else d = spriteRotate(pitchAxis, -p4.w, d);
			d += res;
			d.y /= ps;
			spritePos.xyz = centre + d;
		}
		else if (roll)
		{
			vec3 pivot = (flags & 8192u) != 0u ? centre : origin;
			vec3 d = spritePos.xyz - pivot;
			d.y *= ps;
			d = spriteRotate(rollAxis, rollDeg, d);
			d.y /= ps;
			spritePos.xyz = pivot + d + res;
		}
		else spritePos.xyz += res;
	}
	spriteUV = vec2(right ? ur : ul, bottom ? vb : vt);
}
)";
		}
		else table << "\nlayout(location = 9) in float aSurfaceID;\nlayout(location = 15) flat out uint vRetainedSurfaceID;\nuint retainedSurfaceID;\n";
	}
	else
	{
		table << "\nlayout(location = 15) flat in uint vRetainedSurfaceID;\n#define retainedSurfaceID vRetainedSurfaceID\n";
		if (gpuWorld) table << "\nlayout(location = 14) flat in uvec4 vGpuWorldTextureIDs;\n";
	}
	bindings.Substitute("\t// material types", table.GetChars());
	if (strstr(defines, "SHARED_APPEARANCE"))
	{
		bindings.Substitute("#define uFogColor retainedData[retainedSurfaceID].Stream.uFogColor",
			"#define uFogColor appearanceData[retainedAppRow + 0]");
		bindings.Substitute("#define uVertexColor retainedData[retainedSurfaceID].Stream.uVertexColor",
			"#define uVertexColor appearanceData[retainedAppRow + 1]");
		bindings.Substitute("#define uDesaturationFactor retainedData[retainedSurfaceID].Stream.uDesaturationFactor",
			"#define uDesaturationFactor appearanceData[retainedAppRow + 2].x");
		bindings.Substitute("#define uLightDist retainedData[retainedSurfaceID].Light.x",
			"#define uLightDist appearanceData[retainedAppRow + 2].y");
		bindings.Substitute("#define uLightFactor retainedData[retainedSurfaceID].Light.y",
			"#define uLightFactor appearanceData[retainedAppRow + 2].z");
		bindings.Substitute("#define uFogDensity retainedData[retainedSurfaceID].Light.z",
			"#define uFogDensity appearanceData[retainedAppRow + 2].w");
		bindings.Substitute("#define uLightLevel retainedData[retainedSurfaceID].Light.w",
			"#define uLightLevel appearanceData[retainedAppRow + 3].x");
		bindings.Substitute("#define uFogEnabled retainedData[retainedSurfaceID].FogEnabled",
			"#define uFogEnabled int(appearanceData[retainedAppRow + 3].y)");
	}
	// [Karto] World mode rows are baked without the weapon flash, so the frame's flash is added here.
	if (gpuWorld && strstr(defines, "WORLD_MODE"))
		bindings.Substitute("#define uLightLevel ", "#define uLightLevel (uLightLevelBaked >= 0.0 ? min(uLightLevelBaked + uExtraLight, 1.0) : uLightLevelBaked)\n#define uLightLevelBaked ");
	return bindings;
}

VkShaderProgram* VkShaderManager::GetGpuWorld(EPassType passType, bool alphaTest, int variant)
{
	auto& program = variant == 6 ? WorldModeStampShaders[passType][alphaTest ? 1 : 0] :
		variant == 5 ? WorldModeSpriteShaders[passType][alphaTest ? 1 : 0] :
		variant == 4 ? WorldModeVisibilityShaders[passType][alphaTest ? 1 : 0] :
		variant == 3 ? WorldModeShaders[passType][alphaTest ? 1 : 0] :
		variant == 2 ? GpuResidentWorldShaders[passType][alphaTest ? 1 : 0] : GpuWorldShaders[passType][alphaTest ? 1 : 0];
	if (!program.vert)
	{
		FString defines = defaultshaders[0].Defines;
		defines << "\n#define GPU_INDEXED_WORLD\n";
		if (variant == 2) defines << "#define GPU_RESIDENT_WORLD\n";
		if (variant == 3 || variant == 4 || variant == 6) defines << "#define WORLD_MODE\n";
		if (variant == 6) defines << "#define WORLD_MODE_STAMP\n";
		if (variant == 4) defines << "#define WORLD_MODE_VISIBILITY\n";
		if (variant == 5) defines << "#define GPU_SPRITE_INSTANCES\n";
		Specializing = true;
		program.vert = LoadVertShader("GpuIndexedWorld", "shaders/glsl/main.vp", defines.GetChars(), static_cast<AllShaderIndex>(0));
		if (variant == 4)
		{
			// [Karto] Early depth testing is mandatory: a late test would let hidden fragments stamp their surface.
			FString code = GetTargetGlslVersion();
			code << "layout(early_fragment_tests) in;\n";
			code << "layout(location = 30) flat in uint vWorldSurface;\n";
			code << "layout(set = 3, binding = 2, std430) buffer WorldModeVisible { uint worldVisible[]; };\n";
			code.AppendFormat("layout(push_constant) uniform WorldModeVisibilityPush { layout(offset = %d) int uDataIndex; } wmPush;\n", int(offsetof(PushConstants, uDataIndex)));
			code << "void main() { uint stamp = uint(wmPush.uDataIndex); if (worldVisible[vWorldSurface] != stamp) worldVisible[vWorldSurface] = stamp; }\n";
			program.frag = ShaderBuilder()
				.Type(ShaderType::Fragment)
				.AddSource("WorldModeVisibility", code.GetChars())
				.DebugName("WorldModeVisibility")
				.Create("WorldModeVisibility", fb->device.get());
		}
		else
		program.frag = LoadFragShader("GpuIndexedWorld", "shaders/glsl/main.fp", defaultshaders[0].gettexelfunc,
			defaultshaders[0].lightfunc, defines.GetChars(), alphaTest, passType == GBUFFER_PASS, static_cast<AllShaderIndex>(0));
		Specializing = false;
	}
	return &program;
}

VkShaderProgram* VkShaderManager::GetModelInstances(EPassType passType, bool alphaTest)
{
	auto& program = ModelInstanceShaders[passType][alphaTest ? 1 : 0];
	if (!program.vert) program = CompileDefaultMaterial(passType, alphaTest, true);
	return &program;
}

VkShaderProgram* VkShaderManager::GetRetained(EPassType passType, bool independentTime)
{
	auto& program = RetainedShaders[passType][independentTime ? 1 : 0];
	if (!program.vert)
	{
		FString defines = defaultshaders[0].Defines;
		defines << "\n#define RETAINED_WORLD\n";
		if (independentTime) defines << "\n#define RETAINED_INDEPENDENT_TIME\n";
		Specializing = true;
		program.vert = LoadVertShader("RetainedWorld", "shaders/glsl/main.vp", defines.GetChars(), static_cast<AllShaderIndex>(0));
		program.frag = LoadFragShader("RetainedWorld", "shaders/glsl/main.fp", defaultshaders[0].gettexelfunc,
			defaultshaders[0].lightfunc, defines.GetChars(), false, passType == GBUFFER_PASS, static_cast<AllShaderIndex>(0));
		Specializing = false;
	}
	return &program;
}

std::unique_ptr<VulkanShader> VkShaderManager::LoadVertShader(FString shadername, const char *vert_lump, const char *defines, AllShaderIndex type)
{
	FString code = GetTargetGlslVersion();
	code << "#extension GL_GOOGLE_include_directive : enable\n";
	const bool gpuWorld = strstr(defines, "GPU_INDEXED_WORLD") != nullptr;
	if (gpuWorld)
	{
		if (fb->device->Instance->ApiVersion == VK_API_VERSION_1_2) code << "#define GPU_WORLD_DRAW_ID gl_DrawID\n";
		else code << "#extension GL_ARB_shader_draw_parameters : require\n#define GPU_WORLD_DRAW_ID gl_DrawIDARB\n";
	}
	code << defines;
	code << "\n#define MAX_STREAM_DATA " << std::to_string(MAX_STREAM_DATA).c_str() << "\n";
#ifdef NPOT_EMULATION
	code << "#define NPOT_EMULATION\n";
#endif
	code << GetShaderBindings(defines, true);
	code << "layout(set = 1, binding = 0, std140) uniform readonly ViewpointUBO" << ShaderInputsOutputs::GenerateStruct<HWViewpointUniforms>() << ";\n";
	code << ShaderInputsOutputs::GenerateInputsOutputs(true, false, type, false, fb->device->EnabledFeatures.Features.shaderClipDistance);
	if (!fb->device->EnabledFeatures.Features.shaderClipDistance) code << "#define NO_CLIPDISTANCE_SUPPORT\n";
	code << "#line 1\n";
	FString source = LoadPrivateShaderLump(vert_lump);
	if (gpuWorld) source.Substitute("void main()\n{", "void main()\n{\n\tretainedSurfaceID = uint(retainedPush.uDataIndex) + uint(GPU_WORLD_DRAW_ID);\n\tvRetainedSurfaceID = retainedSurfaceID;");
	if (strstr(defines, "WORLD_MODE_VISIBILITY") || strstr(defines, "WORLD_MODE_STAMP"))
		source.Substitute("retainedSurfaceID = uint(retainedPush.uDataIndex) + uint(GPU_WORLD_DRAW_ID);",
			"retainedSurfaceID = worldSurfaceRows[uint(aSurfaceID)];\n\tvWorldSurface = uint(aSurfaceID);");
	else if (strstr(defines, "WORLD_MODE"))
		source.Substitute("retainedSurfaceID = uint(retainedPush.uDataIndex) + uint(GPU_WORLD_DRAW_ID);", "retainedSurfaceID = worldSurfaceRows[uint(aSurfaceID)];");
	if (strstr(defines, "WORLD_MODE"))
		source.Substitute("void main()\n{", "void main()\n{\n\tvWorldLight = worldSurfaceLights[uint(aSurfaceID)];");
	if (strstr(defines, "WORLD_MODE"))
	{
		source.Substitute("parmTexCoord = aTexCoord;", "parmTexCoord = aTexCoord;\n\tuint worldParam = worldSurfaceParams[uint(aSurfaceID)];\n"
			"\tif (worldParam < 0x80000000u) parmTexCoord += worldParams[worldParam].xy - (1.0 - worldParams[0].x) * worldParams[worldParam].zw;");
		// [Karto] precise keeps the blend the CPU's unfused prev + (cur - prev) * frac.
		source.Substitute("vTexCoord = TextureMatrix * vec4(parmTexCoord, 0.0, 1.0);",
			"precise mat4 worldMatrix = TextureMatrix;\n"
			"\tif (worldParam != 0xffffffffu && worldParam >= 0x80000000u) {\n"
			"\t\tuint wm = (worldParam & 0x7fffffffu) * 2u;\n"
			"\t\tworldMatrix = worldPlaneMatrices[wm + 1u] + (worldPlaneMatrices[wm] - worldPlaneMatrices[wm + 1u]) * worldParams[0].x;\n"
			"\t}\n"
			"\tvTexCoord = worldMatrix * vec4(parmTexCoord, 0.0, 1.0);");
		// [Karto] Evaluated as secplane_t::ZatPoint does; prev + (cur - prev) * frac stays exact when both records are equal.
		source.Substitute("parmPosition = bones.Position;", "parmPosition = bones.Position;\n"
			"\tif (worldParam != 0xffffffffu && worldParam >= 0x80000000u) {\n"
			"\t\tuint wp = (worldParam & 0x7fffffffu) * 3u;\n"
			"\t\tvec4 wh = worldPlaneHeights[wp + 1u] + (worldPlaneHeights[wp] - worldPlaneHeights[wp + 1u]) * worldParams[0].x;\n"
			"\t\tparmPosition.y = (wh.w + wh.x * parmPosition.x + wh.y * parmPosition.z) * wh.z;\n"
			"\t}\n"
			"\tuvec4 wr = worldVertexRefs[gl_VertexIndex];\n"
			"\tif (wr.x != 0xffffffffu) {\n"
			"\t\tvec4 wv = worldPlaneHeights[wr.x * 3u + 1u] + (worldPlaneHeights[wr.x * 3u] - worldPlaneHeights[wr.x * 3u + 1u]) * worldParams[0].x;\n"
			"\t\tfloat wz = (wv.w + wv.x * parmPosition.x + wv.y * parmPosition.z) * wv.z;\n"
			"\t\tvec2 wt = worldPlaneHeights[wr.y * 3u + 2u].xy;\n"
			"\t\tfloat wa = wt.y + (wt.x - wt.y) * worldParams[0].x;\n"
			"\t\tparmTexCoord.y += uintBitsToFloat(wr.z) * ((wa - uintBitsToFloat(wr.w)) - (wz - parmPosition.y));\n"
			"\t\tparmPosition.y = wz;\n"
			"\t}");
	}
	if (strstr(defines, "GPU_RESIDENT_WORLD"))
		source.Substitute("retainedSurfaceID = uint(retainedPush.uDataIndex) + uint(GPU_WORLD_DRAW_ID);",
			"retainedSurfaceID = gpuResidentRefs[uint(retainedPush.uDataIndex) + uint(GPU_WORLD_DRAW_ID)];");
	if (gpuWorld) source.Substitute("vRetainedSurfaceID = retainedSurfaceID;", "vRetainedSurfaceID = retainedSurfaceID;\n\tvGpuWorldTextureIDs = gpuWorldData[retainedSurfaceID].TextureIDs;");
	if (strstr(defines, "GPU_SPRITE_INSTANCES"))
	{
		source.Substitute("retainedSurfaceID = uint(retainedPush.uDataIndex) + uint(GPU_WORLD_DRAW_ID);", "SpriteCompute();\n\tretainedSurfaceID = spriteRow;");
		source.Substitute("vGpuWorldTextureIDs = gpuWorldData[retainedSurfaceID].TextureIDs;", "vGpuWorldTextureIDs = spriteTextureIDs;");
		source.Substitute("parmTexCoord = aTexCoord;", "parmTexCoord = spriteUV;");
		source.Substitute("parmPosition = bones.Position;", "parmPosition = spritePos;");
	}
	code << source.GetChars() << "\n";

	return ShaderBuilder()
		.Type(ShaderType::Vertex)
		.AddSource(shadername.GetChars(), code.GetChars())
		.DebugName(shadername.GetChars())
		.OnIncludeLocal(OnInclude)
		.OnIncludeSystem(OnInclude)
		.Create(shadername.GetChars(), fb->device.get());
}

std::unique_ptr<VulkanShader> VkShaderManager::LoadFragShader(FString shadername, const char *frag_lump, const char *material_lump, const char *light_lump, const char *defines, bool alphatest, bool gbufferpass, AllShaderIndex type)
{
	FString pre_placeholder = GetTargetGlslVersion();
	pre_placeholder << "#extension GL_GOOGLE_include_directive : enable\n";
	if (strstr(defines, "GPU_INDEXED_WORLD")) pre_placeholder << "#extension GL_EXT_nonuniform_qualifier : require\n";
	if (fb->RaytracingEnabled())
		pre_placeholder << "\n#define SUPPORTS_RAYTRACING\n";

	FString code = defines;
	code << "\n#define MAX_STREAM_DATA " << std::to_string(MAX_STREAM_DATA).c_str() << "\n";
#ifdef NPOT_EMULATION
	code << "#define NPOT_EMULATION\n";
#endif
	code << GetShaderBindings(defines, false);
	code << "layout(set = 1, binding = 0, std140) uniform readonly ViewpointUBO" << ShaderInputsOutputs::GenerateStruct<HWViewpointUniforms>() << ";\n";
	code << ShaderInputsOutputs::GenerateInputsOutputs(true, true, type, gbufferpass, fb->device->EnabledFeatures.Features.shaderClipDistance);
	FString placeholder = "\n";

	if (!fb->device->EnabledFeatures.Features.shaderClipDistance) code << "#define NO_CLIPDISTANCE_SUPPORT\n";
	if (!alphatest) code << "#define NO_ALPHATEST\n";
	if (gbufferpass) code << "#define GBUFFER_PASS\n";
	if (Specializing)
	{
		if (!(SpecFeatures & SF_Desaturation)) code << "#define SPEC_NO_DESAT\n";
		if (!(SpecFeatures & SF_TextureManipulation)) code << "#define SPEC_NO_TEXMANIP\n";
		if (!(SpecFeatures & SF_Doom64Colors)) code << "#define SPEC_NO_D64\n";
		if (!(SpecFeatures & SF_Glow)) code << "#define SPEC_NO_GLOW\n";
		if (!(SpecFeatures & SF_Lightmap)) code << "#define SPEC_NO_LIGHTMAP\n";
		if (!(SpecFeatures & SF_DynamicLights)) code << "#define SPEC_NO_DYNLIGHT\n";
	}

	code << "\n#line 1\n";
	code << LoadPrivateShaderLump(frag_lump).GetChars() << "\n";

	if (material_lump)
	{
		if (material_lump[0] != '#')
		{
			FString pp_code = LoadPublicShaderLump(material_lump);

			if (pp_code.IndexOf("ProcessMaterial") < 0 && pp_code.IndexOf("SetupMaterial") < 0)
			{
				// this looks like an old custom hardware shader.
				// add ProcessMaterial function that calls the older ProcessTexel function

				if (pp_code.IndexOf("GetTexCoord") >= 0)
				{
					code << "\n" << LoadPrivateShaderLump("shaders/glsl/func_defaultmat2.fp").GetChars() << "\n";
				}
				else
				{
					code << "\n" << LoadPrivateShaderLump("shaders/glsl/func_defaultmat.fp").GetChars() << "\n";
					if (pp_code.IndexOf("ProcessTexel") < 0)
					{
						// this looks like an even older custom hardware shader.
						// We need to replace the ProcessTexel call to make it work.

						placeholder << "#define NO_PROCESS_TEXEL\n";
					}
				}

				if (pp_code.IndexOf("ProcessLight") >= 0)
				{
					// The ProcessLight signatured changed. Forward to the old one.
					placeholder << "#define OLD_PROCESSLIGHT\n";
				}
			}

			code << "\n#line 1\n";
			code << RemoveLegacyUserUniforms(pp_code).GetChars();
			code.Substitute("gl_TexCoord[0]", "vTexCoord");	// fix old custom shaders.

			if (pp_code.IndexOf("ProcessLight") < 0)
			{
				code << "\n" << LoadPrivateShaderLump("shaders/glsl/func_defaultlight.fp").GetChars() << "\n";
			}

			// ProcessMaterial must be considered broken because it requires the user to fill in data they possibly cannot know all about.
			if (pp_code.IndexOf("ProcessMaterial") >= 0 && pp_code.IndexOf("SetupMaterial") < 0)
			{
				// This reactivates the old logic and disables all features that cannot be supported with that method.
				placeholder << "#define LEGACY_USER_SHADER\n";
			}
		}
		else
		{
			// material_lump is not a lump name but the source itself (from generated shaders)
			code << (material_lump + 1) << "\n";
		}
	}

	code = pre_placeholder + placeholder + code;

	if (light_lump)
	{
		if (strstr(defines, "GPU_INDEXED_WORLD") && strstr(defines, "WORLD_MODE"))
		{
			code << "\nint WorldLightList(int index)\n{\n"
				"\tif (index < 0) return index;\n"
				"\tvec4 head = lights[index];\n"
				"\tif (head.x >= 0.0) return index;\n"
				"\tvec4 grid = lights[index + 1];\n"
				"\tint tx = int(floor((pixelpos.x - head.y) / grid.y)), ty = int(floor((pixelpos.z - head.z) / grid.y));\n"
				"\tif (tx < 0 || ty < 0 || tx >= int(head.w) || ty >= int(grid.x)) return -1;\n"
				"\tint t = ty * int(head.w) + tx;\n"
				"\treturn int(lights[index + 2 + (t >> 2)][t & 3]);\n}\n";
			code << "\n#define WorldLightSlot(entry) int(lights[entry].x)\n";
			FString light = LoadPrivateShaderLump(light_lump);
			light.Substitute("i+=4", "i+=1");
			light.Substitute("lights[i", "lights[WorldLightSlot(i)");
			code << "\n#line 1\n" << light.GetChars();
		}
		else
		{
			code << "\n#line 1\n";
			code << LoadPrivateShaderLump(light_lump).GetChars();
		}
	}

	if (strstr(defines, "WORLD_MODE_STAMP"))
	{
		code.Substitute("void main()", "void WorldModeShadedMain()");
		code << "\nlayout(early_fragment_tests) in;\n";
		code << "layout(location = 30) flat in uint vWorldSurface;\n";
		code << "layout(set = 3, binding = 2, std430) buffer WorldModeVisible { uint worldVisible[]; };\n";
		code << "void main()\n{\n\tWorldModeShadedMain();\n\tuint stamp = uint(retainedPush.uDataIndex);\n"
			"\tif (stamp != 0u && worldVisible[vWorldSurface] != stamp) worldVisible[vWorldSurface] = stamp;\n}\n";
	}

	return ShaderBuilder()
		.Type(ShaderType::Fragment)
		.AddSource(shadername.GetChars(), code.GetChars())
		.DebugName(shadername.GetChars())
		.OnIncludeLocal(OnInclude)
		.OnIncludeSystem(OnInclude)
		.Create(shadername.GetChars(), fb->device.get());
}

FString VkShaderManager::GetTargetGlslVersion()
{
	if (fb->device->Instance->ApiVersion == VK_API_VERSION_1_2)
	{
		return "#version 460\n#extension GL_EXT_ray_query : enable\n";
	}
	else
	{
		return "#version 450 core\n";
	}
}

FString VkShaderManager::LoadPublicShaderLump(const char *lumpname)
{
	int lump = fileSystem.CheckNumForFullName(lumpname, 0);
	if (lump == -1) lump = fileSystem.CheckNumForFullName(lumpname);
	if (lump == -1) I_Error("Unable to load '%s'", lumpname);
	return GetStringFromLump(lump);
}

FString VkShaderManager::LoadPrivateShaderLump(const char *lumpname)
{
	int lump = fileSystem.CheckNumForFullName(lumpname, 0);
	if (lump == -1) I_Error("Unable to load '%s'", lumpname);
	return GetStringFromLump(lump);
}

VkPPShader* VkShaderManager::GetVkShader(PPShader* shader)
{
	if (!shader->Backend)
		shader->Backend = std::make_unique<VkPPShader>(fb, shader);
	return static_cast<VkPPShader*>(shader->Backend.get());
}

void VkShaderManager::AddVkPPShader(VkPPShader* shader)
{
	shader->it = PPShaders.insert(PPShaders.end(), shader);
}

void VkShaderManager::RemoveVkPPShader(VkPPShader* shader)
{
	shader->Reset();
	shader->fb = nullptr;
	PPShaders.erase(shader->it);
}
