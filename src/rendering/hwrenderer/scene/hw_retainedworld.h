#pragma once

#include <array>
#include <deque>
#include <tuple>
#include <vector>
#include <type_traits>
#include <unordered_map>
#include "filesystem.h"

EXTERN_CVAR(Int, r_distance_cull_type)
EXTERN_CVAR(Int, gl_distfog)
EXTERN_CVAR(Float, r_visibility)
EXTERN_CVAR(Bool, vk_retainedlighting)
EXTERN_CVAR(Bool, vk_retainedparams)
EXTERN_CVAR(Bool, vk_snapshot_fast)

namespace RetainedWorld
{
	struct SectorInputs
	{
		std::array<double, 4> Heights;
		std::array<int, 4> Properties;
		bool Transdoor;
		bool operator==(const SectorInputs& other) const
		{
			return Heights == other.Heights && Properties == other.Properties && Transdoor == other.Transdoor;
		}
	};
	struct WallAppearanceInputs
	{
		uint32_t LightColor, FadeColor;
		int WallLight;
		uint8_t Desaturation, BlendFactor;
		uint16_t FogDensity;
		std::array<float, 8> Glow;
		bool Glowing;
		bool operator==(const WallAppearanceInputs& other) const
		{
			return std::tie(LightColor, FadeColor, WallLight, Desaturation, BlendFactor, FogDensity, Glowing) ==
				std::tie(other.LightColor, other.FadeColor, other.WallLight, other.Desaturation, other.BlendFactor, other.FogDensity, other.Glowing) &&
				(!Glowing || Glow == other.Glow);
		}
	};
	struct SectorState
	{
		SectorInputs Inputs = {};
		HWRetainedSectorAppearance Appearance = {};
		bool Ordinary = false;
		uint64_t View = 0;
		uint64_t Version = 0;
		uint64_t AppearanceView = 0;
		uint64_t GlowView = 0;
		uint64_t WallAppearanceVersion = 0;
		WallAppearanceInputs WallInputs = {};
		std::array<uint64_t, 2> PlaneViews = {};
		std::array<int, 3> LightInputs = {};
		std::array<unsigned, 2> LightingSlots = { ~0u, ~0u };
		bool AppearanceValid = false;
	};
	struct SectorGeometryState
	{
		uint64_t View = 0;
		uint64_t Version = 0;
		bool Ordinary = false;
		SectorInputs Inputs = {};
	};
	struct SectorAppearanceState
	{
		HWRetainedSectorAppearance Appearance = {};
		uint64_t AppearanceView = 0;
		uint64_t GlowView = 0;
		uint64_t WallAppearanceVersion = 0;
		WallAppearanceInputs WallInputs = {};
		std::array<uint64_t, 2> PlaneViews = {};
		std::array<int, 3> LightInputs = {};
		std::array<unsigned, 2> LightingSlots = { ~0u, ~0u };
		bool AppearanceValid = false;
	};
	static_assert(sizeof(SectorGeometryState) == 80, "Retained geometry metadata layout changed");
	struct TextureGeometryInputs
	{
		std::array<float, 4> Size;
		bool Valid, WorldPanning;
		bool operator==(const TextureGeometryInputs& other) const
		{
			return Size == other.Size && Valid == other.Valid && WorldPanning == other.WorldPanning;
		}
	};
	struct TextureGeometryHash
	{
		size_t operator()(const TextureGeometryInputs& inputs) const
		{
			size_t hash = unsigned(inputs.Valid) | (unsigned(inputs.WorldPanning) << 1);
			for (float value : inputs.Size)
				hash ^= std::hash<float>()(value) + size_t(0x9e3779b9) + (hash << 6) + (hash >> 2);
			return hash;
		}
	};
	struct TextureState
	{
		FGameTexture* Texture = nullptr;
		std::array<float, 4> Size = {};
		uint64_t View = 0;
		bool Valid = false;
		bool Supported = true;
		bool WorldPanning = false;
		uint64_t GeometryVersion = 0;
	};
	struct TextureInputs
	{
		FGameTexture* Texture;
		uint64_t GeometryVersion;
		bool operator==(const TextureInputs& other) const
		{
			return GeometryVersion == other.GeometryVersion;
		}
	};
	struct SideInputs
	{
		std::array<TextureInputs, 3> Textures;
		std::array<int, 3> TextureIds = {};
		std::array<double, 4> Endpoints;
		std::array<int, 3> Skews;
		std::array<int8_t, 3> SideSkews;
		uint64_t TextureTransformVersion;
		uint64_t FrontVersion, BackVersion;
		uint32_t LineFlags, SideFlags, LevelFlags;
		int TexelLength;
		sector_t* Front;
		sector_t* Back;
		bool Forward;
		bool EmptyOnly = false;
		bool operator==(const SideInputs& other) const
		{
			if (EmptyOnly || other.EmptyOnly)
			{
				return EmptyOnly == other.EmptyOnly && FrontVersion == other.FrontVersion && BackVersion == other.BackVersion &&
					Front == other.Front && Back == other.Back && LineFlags == other.LineFlags;
			}
			return std::tie(TextureTransformVersion, FrontVersion, BackVersion, Textures, Endpoints, Skews, SideSkews,
				LineFlags, SideFlags, LevelFlags, TexelLength, Front, Back, Forward, EmptyOnly) ==
				std::tie(other.TextureTransformVersion, other.FrontVersion, other.BackVersion, other.Textures, other.Endpoints,
				other.Skews, other.SideSkews, other.LineFlags, other.SideFlags, other.LevelFlags, other.TexelLength,
				other.Front, other.Back, other.Forward, other.EmptyOnly);
		}
	};
	struct EmptyInputs
	{
		sector_t* Front;
		sector_t* Back;
		uint64_t FrontVersion, BackVersion;
		uint32_t LineFlags;
		bool operator==(const EmptyInputs& other) const
		{
			return std::tie(Front, Back, FrontVersion, BackVersion, LineFlags) ==
				std::tie(other.Front, other.Back, other.FrontVersion, other.BackVersion, other.LineFlags);
		}
	};
	struct EmptyState
	{
		EmptyInputs Inputs = {};
		bool Valid = false;
		bool Unsupported = false;
	};
	struct Entry
	{
		SideInputs Inputs;
		std::array<int, 5> LightInputs = {};
		std::array<short, 3> Light = {};
		std::array<short, 3> RelativeLight = {};
		std::array<FGameTexture*, 3> AppearanceTextures = {};
		uint64_t WallAppearanceVersion = 0;
		uint64_t WallLightingVersion = 0;
		bool Fullbright = false;
		bool ReplayValid = false;
		bool LightValid = false;
		uint64_t OwnerVersion = 0;
		uint64_t OwnedLightingContext = 0;
		ELightMode OwnedLightMode = {};
		int OwnedExtraLight = 0;
		HWMeshHelper Output;
	};
	// [Karto] Pool growth must move ownership without copying buffers already referenced by this view's draw lists.
	static_assert(std::is_nothrow_move_constructible<Entry>::value, "Retained entry buffers must move without copying");
	struct LightingInputs
	{
		FLevelLocals* Level;
		uint32_t LightColor, FadeColor;
		int Light, RelativeLight;
		ELightMode Mode;
		float Alpha;
		uint8_t Desaturation, BlendFactor;
		uint16_t FogDensity;
		bool Fullbright, Additive, FogFirst;
		bool operator==(const LightingInputs& other) const
		{
			return std::tie(Level, LightColor, FadeColor, Light, RelativeLight, Mode, Alpha,
				Desaturation, BlendFactor, FogDensity, Fullbright, Additive, FogFirst) ==
				std::tie(other.Level, other.LightColor, other.FadeColor, other.Light, other.RelativeLight, other.Mode, other.Alpha,
					other.Desaturation, other.BlendFactor, other.FogDensity, other.Fullbright, other.Additive, other.FogFirst);
		}
	};
	struct LightingHash
	{
		size_t operator()(const LightingInputs& inputs) const
		{
			size_t hash = std::hash<FLevelLocals*>()(inputs.Level);
			auto combine = [&hash](size_t value)
			{
				hash ^= value + size_t(0x9e3779b9) + (hash << 6) + (hash >> 2);
			};
			combine(inputs.LightColor);
			combine(inputs.FadeColor);
			combine(std::hash<int>()(inputs.Light));
			combine(std::hash<int>()(inputs.RelativeLight));
			combine(size_t(inputs.Mode));
			combine(std::hash<float>()(inputs.Alpha));
			combine(inputs.Desaturation | (unsigned(inputs.BlendFactor) << 8) | (unsigned(inputs.FogDensity) << 16));
			combine(unsigned(inputs.Fullbright) | (unsigned(inputs.Additive) << 1) | (unsigned(inputs.FogFirst) << 2));
			return hash;
		}
	};
	struct LightingEntry
	{
		LightingInputs Inputs;
		HWWorldLightingState State = {};
		uint64_t ContextVersion = 0;
	};
	struct LightingContext
	{
		FLevelLocals* Level;
		std::array<int, 5> FogOptions;
		float Visibility, CullDistance;
		uint32_t Flags, Flags3, CullColor, OutsideFog;
		bool BrightFog, InSkybox;
		bool operator==(const LightingContext& other) const
		{
			return std::tie(Level, FogOptions, Visibility, CullDistance, Flags, Flags3, CullColor, OutsideFog, BrightFog, InSkybox) ==
				std::tie(other.Level, other.FogOptions, other.Visibility, other.CullDistance, other.Flags, other.Flags3,
					other.CullColor, other.OutsideFog, other.BrightFog, other.InSkybox);
		}
	};
	static uint64_t View = 1;
	static uint64_t NextOwnerVersion = 0;
	static unsigned ResourceGeneration = 0;
	static std::vector<SectorState> Sectors;
	static std::vector<SectorGeometryState> SectorGeometry;
	static std::vector<SectorAppearanceState> SectorAppearances;
	static bool CompactSectors = false;
	static std::vector<TextureState> Textures;
	static std::vector<uint32_t> SideSlots;
	static std::vector<Entry> Entries;
	static std::vector<EmptyState> EmptySides;
	static std::deque<HWPreparedWall> Geometry;
	static std::deque<LightingEntry> Lighting;
	static std::unordered_map<LightingInputs, unsigned, LightingHash> LightingLookup;
	static LightingContext CurrentLightingContext = {};
	static uint64_t LightingContextView = 0;
	static uint64_t LightingContextVersion = 0;
	static uint64_t TextureGeometryVersion = 0;
	static std::unordered_map<TextureGeometryInputs, uint64_t, TextureGeometryHash> TextureGeometryLookup;
	static constexpr unsigned TextureGeometryCapacity = 4096;
	static uint64_t WallLightingView = 0;
	static uint64_t WallLightingVersion = 0;
	static FLevelLocals* WallLightingLevel = nullptr;
	static std::array<int, 7> WallLightingInputs = {};
	static constexpr unsigned Capacity = 32768;
	static constexpr unsigned LightingCapacity = 4096;
	static bool ShadersChecked = false;
	static bool ShaderSourcesCompatible = false;

	bool OrdinarySector(sector_t* sector);

	template<bool Compact>
	auto& SectorGeometryPool()
	{
		if constexpr (Compact) return SectorGeometry;
		else return Sectors;
	}

	template<bool Compact>
	auto& SectorAppearance(unsigned index)
	{
		if constexpr (Compact) return SectorAppearances[index];
		else return Sectors[index];
	}

	template<class Target, class Source>
	void CopySectorGeometry(Target& target, const Source& source)
	{
		target.Inputs = source.Inputs;
		target.Ordinary = source.Ordinary;
		target.View = source.View;
		target.Version = source.Version;
	}

	template<class Target, class Source>
	void CopySectorAppearance(Target& target, const Source& source)
	{
		target.Appearance = source.Appearance;
		target.AppearanceView = source.AppearanceView;
		target.GlowView = source.GlowView;
		target.WallAppearanceVersion = source.WallAppearanceVersion;
		target.WallInputs = source.WallInputs;
		target.PlaneViews = source.PlaneViews;
		target.LightInputs = source.LightInputs;
		target.LightingSlots = source.LightingSlots;
		target.AppearanceValid = source.AppearanceValid;
	}

	void SwitchSectorLayout(bool compact)
	{
		if (compact == CompactSectors) return;
		// [Karto] Preserve version identities across layout switches so existing side recipes cannot acquire an ABA hit.
		if (compact)
		{
			std::vector<SectorGeometryState> geometry(Sectors.size());
			std::vector<SectorAppearanceState> appearances(Sectors.size());
			for (size_t i = 0; i < Sectors.size(); ++i)
			{
				CopySectorGeometry(geometry[i], Sectors[i]);
				CopySectorAppearance(appearances[i], Sectors[i]);
			}
			SectorGeometry.swap(geometry);
			SectorAppearances.swap(appearances);
			std::vector<SectorState>().swap(Sectors);
		}
		else
		{
			std::vector<SectorState> sectors(SectorGeometry.size());
			for (size_t i = 0; i < SectorGeometry.size(); ++i)
			{
				CopySectorGeometry(sectors[i], SectorGeometry[i]);
				CopySectorAppearance(sectors[i], SectorAppearances[i]);
			}
			Sectors.swap(sectors);
			std::vector<SectorGeometryState>().swap(SectorGeometry);
			std::vector<SectorAppearanceState>().swap(SectorAppearances);
		}
		CompactSectors = compact;
	}

	uint64_t GetSectorMemory()
	{
		return Sectors.capacity() * sizeof(SectorState) + SectorGeometry.capacity() * sizeof(SectorGeometryState) +
			SectorAppearances.capacity() * sizeof(SectorAppearanceState);
	}

	Entry* FindSideEntry(unsigned side)
	{
		const auto slot = SideSlots[side];
		return slot ? &Entries[slot - 1] : nullptr;
	}

	void StoreSideEntry(unsigned side, const SideInputs& inputs, HWMeshHelper&& output)
	{
		Entry replacement;
		replacement.Inputs = inputs;
		replacement.Output = std::move(output);
		if (auto entry = FindSideEntry(side))
		{
			*entry = std::move(replacement);
		}
		else
		{
			Entries.push_back(std::move(replacement));
			SideSlots[side] = uint32_t(Entries.size());
		}
	}

	void ClearSideEntries()
	{
		Entries.clear();
		SideSlots.clear();
	}

	uint64_t GetSideEntryMemory()
	{
		uint64_t bytes = SideSlots.capacity() * sizeof(uint32_t) + Entries.capacity() * sizeof(Entry);
		for (auto& entry : Entries)
		{
			bytes += entry.Output.list.Size() * sizeof(HWWall) +
				(entry.Output.upper.Size() + entry.Output.lower.Size()) * sizeof(HWMissing);
		}
		return bytes;
	}

	uint64_t ValidateWallLightingContext(FLevelLocals* level)
	{
		if (WallLightingView != View || WallLightingLevel != level)
		{
			const std::array<int, 7> inputs = { int(level->flags), int(level->flags2), int(level->flags3),
				level->WallHorizLight, level->WallVertLight, int(r_fakecontrast), int(r_extralight) };
			if (!WallLightingVersion || WallLightingLevel != level || WallLightingInputs != inputs)
			{
				WallLightingInputs = inputs;
				WallLightingLevel = level;
				++WallLightingVersion;
			}
			WallLightingView = View;
		}
		return WallLightingVersion;
	}

	uint64_t ValidateLightingContext(FLevelLocals* level)
	{
		if (LightingContextView != View || CurrentLightingContext.Level != level)
		{
			const LightingContext context = { level,
				{ int(gl_fogmode), int(r_distance_cull_type), int(gl_distfog), level->fogdensity, level->outsidefogdensity },
				float(r_visibility), level->culldist, level->flags, level->flags3, level->cullcolor.d,
				uint32_t(level->info->outsidefog), level->brightfog, bool(portalState.inskybox) };
			if (!LightingContextVersion || !(context == CurrentLightingContext))
			{
				CurrentLightingContext = context;
				++LightingContextVersion;
			}
			LightingContextView = View;
		}
		return LightingContextVersion;
	}

	template<bool Compact = false>
	auto& ValidateSector(sector_t* sector)
	{
		if (SceneProfiling) ++GetSceneSurfaceProfile().RetainedSectorRequests;
		auto& pool = SectorGeometryPool<Compact>();
		if (pool.empty())
		{
			pool.resize(sector->Level->sectors.Size());
			if constexpr (Compact) SectorAppearances.resize(pool.size());
		}
		if (SceneProfiling)
		{
			auto& profile = GetSceneSurfaceProfile();
			profile.RetainedSectorBytes = GetSectorMemory();
			profile.RetainedSectorHotBytes = pool.capacity() * sizeof(pool[0]);
			profile.RetainedSectorColdBytes = SectorAppearances.capacity() * sizeof(SectorAppearanceState);
			profile.RetainedSectorLayout = Compact;
		}
		auto& state = pool[sector->Index()];
		if (state.View != View)
		{
			if (SceneProfiling) ++GetSceneSurfaceProfile().RetainedSectorRefreshes;
			state.Ordinary = OrdinarySector(sector);
			SectorInputs inputs = {
				{ sector->floorplane.fD(), sector->ceilingplane.fD(), sector->GetPlaneTexZ(0), sector->GetPlaneTexZ(1) },
				{ sector->GetTexture(0) == skyflatnum, sector->GetTexture(1) == skyflatnum, sector->special, int(sector->MoreFlags) },
				sector->transdoor != 0
			};
			if (!(inputs == state.Inputs) || !state.Version)
			{
				state.Inputs = inputs;
				++state.Version;
			}
			state.View = View;
		}
		return state;
	}

	template<bool Compact = false>
	HWRetainedSectorAppearance& ValidateAppearance(sector_t* sector)
	{
		if (SceneProfiling) ++GetSceneSurfaceProfile().RetainedAppearanceRequests;
		ValidateSector<Compact>(sector);
		auto& state = SectorAppearance<Compact>(sector->Index());
		if (state.AppearanceView != View)
		{
			if (SceneProfiling) ++GetSceneSurfaceProfile().RetainedAppearanceRefreshes;
			state.Appearance.Colormap = sector->Colormap;
			state.Appearance.WallLight = hw_ClampLight(sector->lightlevel);
			std::array<int, 3> lightInputs = { sector->GetFloorLight(), sector->GetCeilingLight(), int(r_extralight) };
			if (!state.AppearanceValid || state.LightInputs != lightInputs)
			{
				state.Appearance.PlaneLight[0] = RescaleLightLevel(lightInputs[0]);
				state.Appearance.PlaneLight[1] = RescaleLightLevel(lightInputs[1]);
				state.LightInputs = lightInputs;
			}
			state.AppearanceValid = true;
			state.AppearanceView = View;
		}
		return state.Appearance;
	}

	template<bool Compact = false>
	HWRetainedSectorAppearance& ValidateWallAppearance(sector_t* sector)
	{
		if (SceneProfiling) ++GetSceneSurfaceProfile().RetainedGlowRequests;
		auto& appearance = ValidateAppearance<Compact>(sector);
		auto& state = SectorAppearance<Compact>(sector->Index());
		if (state.GlowView != View)
		{
			if (SceneProfiling) ++GetSceneSurfaceProfile().RetainedGlowRefreshes;
			appearance.Glowing = sector->GetWallGlow(appearance.TopGlow, appearance.BottomGlow);
			WallAppearanceInputs inputs = { appearance.Colormap.LightColor.d, appearance.Colormap.FadeColor.d,
				appearance.WallLight, appearance.Colormap.Desaturation, appearance.Colormap.BlendFactor, appearance.Colormap.FogDensity,
				{}, appearance.Glowing };
			if (inputs.Glowing)
			{
				memcpy(inputs.Glow.data(), appearance.TopGlow, sizeof(appearance.TopGlow));
				memcpy(inputs.Glow.data() + 4, appearance.BottomGlow, sizeof(appearance.BottomGlow));
			}
			if (!state.WallAppearanceVersion || !(state.WallInputs == inputs))
			{
				state.WallInputs = inputs;
				++state.WallAppearanceVersion;
			}
			state.GlowView = View;
		}
		return appearance;
	}

	template<bool Compact = false>
	void ValidatePlaneAppearance(sector_t* sector, int plane)
	{
		if (SceneProfiling) ++GetSceneSurfaceProfile().RetainedPlaneRequests;
		auto& state = SectorAppearance<Compact>(sector->Index());
		if (state.PlaneViews[plane] != View)
		{
			if (SceneProfiling) ++GetSceneSurfaceProfile().RetainedPlaneRefreshes;
			state.Appearance.Planes[plane].GetFromSector(sector, plane);
			state.PlaneViews[plane] = View;
		}
	}

	const TextureState& ValidateTexture(FGameTexture* texture)
	{
		if (SceneProfiling) ++GetSceneSurfaceProfile().RetainedTextureRequests;
		static const TextureState missing;
		static const TextureState unsupported = { nullptr, {}, 0, false, false, false };
		if (!texture) return missing;
		auto index = texture->GetID().GetIndex();
		if (index < 0 || index >= TexMan.NumTextures()) return unsupported;
		if (Textures.size() < size_t(TexMan.NumTextures())) Textures.resize(TexMan.NumTextures());
		if (SceneProfiling) GetSceneSurfaceProfile().RetainedTextureBytes = Textures.capacity() * sizeof(TextureState);
		auto& state = Textures[index];
		if (state.View != View || state.Texture != texture)
		{
			if (SceneProfiling) ++GetSceneSurfaceProfile().RetainedTextureRefreshes;
			const bool valid = texture->isValid();
			const std::array<float, 4> size = { valid ? texture->GetDisplayWidth() : 0, valid ? texture->GetDisplayHeight() : 0,
				valid ? texture->GetScaleX() : 0, valid ? texture->GetScaleY() : 0 };
			const bool worldPanning = valid && texture->useWorldPanning();
			if (!state.GeometryVersion || state.Valid != valid || state.Size != size || state.WorldPanning != worldPanning)
			{
				const TextureGeometryInputs inputs = { size, valid, worldPanning };
				auto found = TextureGeometryLookup.find(inputs);
				if (found != TextureGeometryLookup.end()) state.GeometryVersion = found->second;
				else
				{
					if (TextureGeometryLookup.size() == TextureGeometryCapacity) return unsupported;
					state.GeometryVersion = ++TextureGeometryVersion;
					TextureGeometryLookup.emplace(inputs, state.GeometryVersion);
				}
			}
			state.Texture = texture;
			state.Valid = valid;
			state.Supported = !state.Valid || (!texture->isMasked() && !texture->GetTranslucency() &&
				texture->GetShaderIndex() == 0 && !texture->isHardwareCanvas());
			state.Size = size;
			state.WorldPanning = worldPanning;
			state.View = View;
		}
		return state;
	}

	bool OrdinarySector(sector_t* sector)
	{
		return !sector || (!sector->floorplane.isSlope() && !sector->ceilingplane.isSlope() &&
			!sector->GetHeightSec() && !sector->Portals[0] && !sector->Portals[1] &&
			sector->GetReflect(0) == 0 && sector->GetReflect(1) == 0 &&
			!sector->e->XFloor.ffloors.Size() && !sector->e->XFloor.lightlist.Size());
	}

	bool Fallback(unsigned reason)
	{
		if (SceneProfiling) ++GetSceneSurfaceProfile().RetainedFallbackReasons[reason];
		return false;
	}

	template<bool Compact, bool Fast>
	inline auto& SnapshotSector(sector_t* sector)
	{
		if constexpr (Fast)
		{
			auto& pool = SectorGeometryPool<Compact>();
			if (!pool.empty())
			{
				auto& state = pool[sector->Index()];
				if (state.View == View)
				{
					if (SceneProfiling)
					{
						auto& profile = GetSceneSurfaceProfile();
						++profile.RetainedSectorRequests;
						++profile.RetainedSnapshotSectorShortcuts;
						profile.RetainedSectorBytes = GetSectorMemory();
						profile.RetainedSectorHotBytes = pool.capacity() * sizeof(pool[0]);
						profile.RetainedSectorColdBytes = SectorAppearances.capacity() * sizeof(SectorAppearanceState);
						profile.RetainedSectorLayout = Compact;
					}
					return state;
				}
			}
		}
		return ValidateSector<Compact>(sector);
	}

	template<bool Fast>
	inline const TextureState& SnapshotTexture(FGameTexture* texture)
	{
		if constexpr (Fast)
		{
			if (!texture)
			{
				static const TextureState missing;
				if (SceneProfiling)
				{
					++GetSceneSurfaceProfile().RetainedTextureRequests;
					++GetSceneSurfaceProfile().RetainedSnapshotTextureShortcuts;
				}
				return missing;
			}
			const auto index = texture->GetID().GetIndex();
			if (index >= 0 && index < TexMan.NumTextures() && size_t(index) < Textures.size())
			{
				const auto& state = Textures[index];
				if (state.View == View && state.Texture == texture)
				{
					if (SceneProfiling)
					{
						auto& profile = GetSceneSurfaceProfile();
						++profile.RetainedTextureRequests;
						++profile.RetainedSnapshotTextureShortcuts;
						profile.RetainedTextureBytes = Textures.capacity() * sizeof(TextureState);
					}
					return state;
				}
			}
		}
		return ValidateTexture(texture);
	}

	bool MatchesSideInputs(const SideInputs& cached, const SideInputs& materials, seg_t* seg,
		sector_t* front, sector_t* back, uint32_t levelFlags)
	{
		auto side = seg->sidedef;
		auto line = seg->linedef;
		if (cached.EmptyOnly || materials.EmptyOnly || cached.TextureTransformVersion != side->TextureTransformVersion ||
			cached.FrontVersion != materials.FrontVersion || cached.BackVersion != materials.BackVersion ||
			cached.Front != front || cached.Back != back || cached.LineFlags != line->flags ||
			cached.SideFlags != (side->Flags & (WALLF_POLYOBJ | WALLF_CLIP_MIDTEX | WALLF_WRAP_MIDTEX)) ||
			cached.LevelFlags != levelFlags || cached.TexelLength != side->TexelLength ||
			cached.Forward != (side == line->sidedef[0])) return false;
		for (int part = 0; part < 3; ++part)
		{
			if (cached.Textures[part].GeometryVersion != materials.Textures[part].GeometryVersion ||
				cached.SideSkews[part] != materials.SideSkews[part]) return false;
		}
		// [Karto] Script-writable vertices and skews still need live equality, including floating-point NaN and signed-zero semantics.
		return cached.Endpoints[0] == line->v1->fX() && cached.Endpoints[1] == line->v1->fY() &&
			cached.Endpoints[2] == line->v2->fX() && cached.Endpoints[3] == line->v2->fY() &&
			cached.Skews[0] == int(topskew) && cached.Skews[1] == int(midskew) && cached.Skews[2] == int(bottomskew);
	}

	template<bool Compact, bool Fast>
	bool SnapshotStorage(HWWallDispatcher* di, seg_t* seg, sector_t* front, sector_t* back, bool culled,
		SideInputs& inputs, Entry*& found, bool& hit)
	{
		if (!di->di) return Fallback(0);
		if (!RetainedWorldShadersCompatible()) return Fallback(5);
		if (di->di->mCurrentPortal || di->di->Viewpoint.bDoOob || di->di->Viewpoint.bDoOrtho ||
			!screen->IsVulkan() || !screen->BuffersArePersistent() || culled || gl_seamless) return Fallback(1);
		if (front != seg->frontsector || back != seg->backsector ||
			seg->sidedef->AttachedDecals || seg->sidedef->lightmap || (seg->sidedef->Flags & WALLF_POLYOBJ) ||
			seg->linedef->isVisualPortal() || seg->linedef->GetTransferredPortal() ||
			seg->linedef->special == Line_Horizon || seg->linedef->special == Line_Mirror) return Fallback(2);
		if (SceneProfiling && back) ++GetSceneSurfaceProfile().RetainedFogChecks;
		if (back && CheckFog(di->Level, front, back, di->lightmode)) return Fallback(3);
		auto& frontState = SnapshotSector<Compact, Fast>(front);
		auto backState = back ? &SnapshotSector<Compact, Fast>(back) : nullptr;
		inputs.FrontVersion = frontState.Version;
		inputs.BackVersion = backState ? backState->Version : 0;
		if (!frontState.Ordinary || (backState && !backState->Ordinary)) return Fallback(4);
		auto side = seg->sidedef;
		if (backState && (seg->linedef->flags & ML_TWOSIDED) && !(seg->linedef->flags & ML_3DMIDTEX) &&
			frontState.Inputs.Heights[0] == backState->Inputs.Heights[0] &&
			frontState.Inputs.Heights[1] == backState->Inputs.Heights[1] &&
			!frontState.Inputs.Properties[0] && !frontState.Inputs.Properties[1] &&
			!backState->Inputs.Properties[0] && !backState->Inputs.Properties[1])
		{
			auto middle = TexMan.GetGameTexture(side->GetTexture(side_t::mid), true);
			if (!middle || !middle->isValid())
			{
				inputs.EmptyOnly = true;
				inputs.Front = front;
				inputs.Back = back;
				inputs.FrontVersion = frontState.Version;
				inputs.BackVersion = backState->Version;
				inputs.LineFlags = seg->linedef->flags;
				return true;
			}
		}
		found = SideSlots.empty() ? nullptr : FindSideEntry(side->Index());
		const std::array<int, 3> rawTextures = { side->GetTexture(0).GetIndex(), side->GetTexture(1).GetIndex(), side->GetTexture(2).GetIndex() };
		const bool reuseTextures = vk_retainedworld_directkey && found && !found->Inputs.EmptyOnly &&
			found->Inputs.TextureIds == rawTextures;
		if (reuseTextures)
		{
			inputs.Textures = found->Inputs.Textures;
			for (int part = 0; part < 3; ++part)
				inputs.SideSkews[part] = side->textures[part].skew;
		}
		else
		{
			for (int part = 0; part < 3; ++part)
			{
				auto texture = TexMan.GetGameTexture(side->GetTexture(part), true);
				const auto& material = SnapshotTexture<Fast>(texture);
				if (!material.Supported || (material.Valid && back && part == side_t::mid)) return Fallback(5);
				inputs.Textures[part] = { texture, material.GeometryVersion };
				inputs.SideSkews[part] = side->textures[part].skew;
			}
		}
		inputs.TextureIds = rawTextures;
		if (vk_retainedworld_directkey && found && MatchesSideInputs(found->Inputs, inputs, seg, front, back, di->Level->flags3))
		{
			hit = true;
			return true;
		}
		inputs.TextureTransformVersion = side->TextureTransformVersion;
		inputs.Endpoints = { seg->linedef->v1->fX(), seg->linedef->v1->fY(), seg->linedef->v2->fX(), seg->linedef->v2->fY() };
		inputs.Skews = { int(topskew), int(midskew), int(bottomskew) };
		inputs.LineFlags = seg->linedef->flags;
		inputs.SideFlags = side->Flags & (WALLF_POLYOBJ | WALLF_CLIP_MIDTEX | WALLF_WRAP_MIDTEX);
		inputs.LevelFlags = di->Level->flags3;
		inputs.TexelLength = side->TexelLength;
		inputs.Front = front;
		inputs.Back = back;
		inputs.Forward = side == seg->linedef->sidedef[0];
		hit = found && found->Inputs == inputs;
		return true;
	}

	bool Snapshot(HWWallDispatcher* di, seg_t* seg, sector_t* front, sector_t* back, bool culled,
		SideInputs& inputs, Entry*& found, bool& hit)
	{
		if (vk_snapshot_fast)
		{
			if (CompactSectors) return SnapshotStorage<true, true>(di, seg, front, back, culled, inputs, found, hit);
			return SnapshotStorage<false, true>(di, seg, front, back, culled, inputs, found, hit);
		}
		if (CompactSectors) return SnapshotStorage<true, false>(di, seg, front, back, culled, inputs, found, hit);
		return SnapshotStorage<false, false>(di, seg, front, back, culled, inputs, found, hit);
	}

	void ProcessEmpty(HWWall& wall, HWWallDispatcher* di, seg_t* seg, sector_t* front, sector_t* back, const SideInputs& inputs)
	{
		if (EmptySides.empty()) EmptySides.resize(di->Level->sides.Size());
		auto& entry = EmptySides[seg->sidedef->Index()];
		const EmptyInputs current = { front, back, inputs.FrontVersion, inputs.BackVersion, inputs.LineFlags };
		bool same = entry.Inputs == current;
		bool hit = same && entry.Valid;
		if (same && entry.Unsupported)
		{
			if (SceneProfiling) ++GetSceneSurfaceProfile().PreparedFallbacks;
			wall.PrepareWall(di, seg, front, back, false);
			return;
		}
		if (!hit || vk_retainedworld_verify)
		{
			HWMeshHelper reference;
			di->PreparedCapture = &reference;
			di->SuppressSubmit = true;
			wall.PrepareWall(di, seg, front, back, false);
			di->PreparedCapture = nullptr;
			di->SuppressSubmit = false;
			if (reference.list.Size() || reference.upper.Size() || reference.lower.Size())
			{
				if (hit) I_FatalError("Retained empty wall reference differs on side %d", seg->sidedef->Index());
				entry = { current, false, true };
				if (SceneProfiling) ++GetSceneSurfaceProfile().PreparedFallbacks;
				wall.PrepareWall(di, seg, front, back, false);
				return;
			}
			entry = { current, true, false };
		}
		if (SceneProfiling)
		{
			if (hit) ++GetSceneSurfaceProfile().PreparedHits;
			else ++GetSceneSurfaceProfile().PreparedRebuilds;
			if (hit && vk_retainedworld_verify) ++GetSceneSurfaceProfile().PreparedVerified;
		}
	}

	template<bool Compact>
	bool UpdateEntryAppearanceStorage(HWWallDispatcher* di, sector_t* front, seg_t* seg, subsector_t* sub, Entry& entry, const SideInputs* inputs = nullptr)
	{
		if (!entry.Output.list.Size() && !entry.Output.upper.Size() && !entry.Output.lower.Size()) return false;
		auto& appearance = ValidateWallAppearance<Compact>(front);
		auto side = seg->sidedef;
		const auto appearanceVersion = SectorAppearance<Compact>(front->Index()).WallAppearanceVersion;
		const auto lightingVersion = ValidateWallLightingContext(di->Level);
		const std::array<int, 5> lightInputs = { int(side->Flags), side->Light, side->TierLights[0], side->TierLights[1], side->TierLights[2] };
		const bool lightChanged = !entry.LightValid || entry.LightInputs != lightInputs ||
			entry.WallAppearanceVersion != appearanceVersion || entry.WallLightingVersion != lightingVersion;
		if (lightChanged)
		{
			const bool foggy = !appearance.Colormap.FadeColor.isBlack() || (di->Level->flags & LEVEL_HASFADETABLE);
			for (int part = 0; part < 3; ++part)
			{
				int rel = 0;
				entry.Light[part] = RescaleLightLevel(side->GetLightLevel(foggy, appearance.WallLight, part, false, &rel));
				entry.RelativeLight[part] = CalcRelLight(entry.Light[part], appearance.WallLight, rel);
			}
			entry.LightInputs = lightInputs;
			entry.LightValid = true;
		}
		const std::array<FGameTexture*, 3> textures = inputs ?
			std::array<FGameTexture*, 3> { inputs->Textures[0].Texture, inputs->Textures[1].Texture, inputs->Textures[2].Texture } :
			std::array<FGameTexture*, 3> { TexMan.GetGameTexture(side->GetTexture(0), true),
				TexMan.GetGameTexture(side->GetTexture(1), true), TexMan.GetGameTexture(side->GetTexture(2), true) };
		const bool fullbright = di->isFullbrightScene();
		const bool appearanceChanged = !entry.ReplayValid || lightChanged || entry.Fullbright != fullbright || entry.AppearanceTextures != textures;
		if (SceneProfiling)
		{
			if (appearanceChanged) ++GetSceneSurfaceProfile().PreparedAppearanceUpdates;
			else ++GetSceneSurfaceProfile().PreparedAppearanceReuses;
			if (inputs) GetSceneSurfaceProfile().PreparedTextureReuses += 3;
		}
		for (auto& wall : entry.Output.list)
		{
			if (wall.seg != seg || wall.sub != sub)
			{
				wall.seg = seg;
				wall.sub = sub;
			}
			if (appearanceChanged)
			{
				wall.Colormap = appearance.Colormap;
				const int part = wall.type == RENDERWALL_TOP ? side_t::top : wall.type == RENDERWALL_BOTTOM ? side_t::bottom : side_t::mid;
				wall.texture = textures[part];
				wall.lightlevel = entry.Light[part];
				wall.rellight = entry.RelativeLight[part];
				wall.flags &= ~HWWall::HWF_GLOW;
				if (appearance.Glowing)
				{
					wall.flags |= HWWall::HWF_GLOW;
					memcpy(wall.topglowcolor, appearance.TopGlow, sizeof(wall.topglowcolor));
					memcpy(wall.bottomglowcolor, appearance.BottomGlow, sizeof(wall.bottomglowcolor));
				}
				if (fullbright) wall.Colormap.Clear();
				if (fullbright || (wall.Colormap.LightColor.isWhite() && wall.lightlevel == 255)) wall.flags &= ~HWWall::HWF_GLOW;
			}
			// [Karto] A previous draw may have uploaded lights; disabling them must not reuse its old buffer index.
			wall.dynlightindex = -1;
		}
		if (appearanceChanged)
		{
			entry.WallAppearanceVersion = appearanceVersion;
			entry.WallLightingVersion = lightingVersion;
			entry.AppearanceTextures = textures;
			entry.Fullbright = fullbright;
			entry.ReplayValid = true;
			// [Karto] Other views and the legacy replay path may update this recipe before ownership resumes.
			entry.OwnerVersion = 0;
		}
		return appearanceChanged;
	}

	template<bool Compact>
	void ReplayEntryStorage(HWWallDispatcher* di, const HWWall& source, Entry& entry, const SideInputs* inputs = nullptr)
	{
		if (!entry.Output.list.Size() && !entry.Output.upper.Size() && !entry.Output.lower.Size()) return;
		UpdateEntryAppearanceStorage<Compact>(di, source.frontsector, source.seg, source.sub, entry, inputs);
		for (auto& wall : entry.Output.list)
		{
			if (SceneProfiling)
			{
				++GetSceneSurfaceProfile().Submitted;
				++GetSceneSurfaceProfile().OpaqueRegular;
			}
			di->AddWall(&wall, true);
		}
		// [Karto] Missing-texture recipes use the current seg and subsector, never the seg that populated the cache.
		if (source.seg->PartnerSeg && !(source.seg->PartnerSeg->Subsector->hacked & 4))
		{
			for (auto missing : entry.Output.upper) di->AddUpperMissingTexture(source.seg->sidedef, source.sub, missing.plane);
			for (auto missing : entry.Output.lower) di->AddLowerMissingTexture(source.seg->sidedef, source.sub, missing.plane);
		}
	}

	void ReplayEntry(HWWallDispatcher* di, const HWWall& source, Entry& entry, const SideInputs* inputs = nullptr)
	{
		if (CompactSectors) ReplayEntryStorage<true>(di, source, entry, inputs);
		else ReplayEntryStorage<false>(di, source, entry, inputs);
	}

	void Replay(HWWallDispatcher* di, const HWWall& source)
	{
		ReplayEntry(di, source, *FindSideEntry(source.seg->sidedef->Index()));
	}

	void ReplayViewEffects(HWWallDispatcher* di, HWWall& wall, seg_t* seg, sector_t* front, sector_t* back)
	{
		wall.seg = seg;
		wall.frontsector = front;
		wall.backsector = back;
		if (front->GetTexture(0) != skyflatnum && front->GetTexture(1) != skyflatnum) return;
		bool oneSided = !back || !(seg->linedef->flags & (ML_TWOSIDED | ML_3DMIDTEX));
		bool ceilingSky = front->GetTexture(1) == skyflatnum && (oneSided || back->GetTexture(1) != skyflatnum ||
			(back->ceilingplane.ZatPoint(0., 0.) <= back->floorplane.ZatPoint(0., 0.) && !back->transdoor));
		if (!ceilingSky && front->GetTexture(0) != skyflatnum) return;
		wall.flags = 0;
		wall.vertcount = 0;
		wall.Colormap = front->Colormap;
		bool forward = seg->sidedef == seg->linedef->sidedef[0];
		auto v1 = forward ? seg->linedef->v1 : seg->linedef->v2;
		auto v2 = forward ? seg->linedef->v2 : seg->linedef->v1;
		wall.vertexes[0] = v1;
		wall.vertexes[1] = v2;
		wall.glseg = { float(v1->fX()), float(v2->fX()), float(v1->fY()), float(v2->fY()), 0, 1 };
		wall.zfloor[0] = wall.zfloor[1] = front->floorplane.ZatPoint(v1);
		wall.zceil[0] = wall.zceil[1] = front->ceilingplane.ZatPoint(v1);
		if (oneSided) wall.SkyNormal(di, front, v1, v2);
		else
		{
			wall.SkyTop(di, seg, front, back, v1, v2);
			wall.SkyBottom(di, seg, front, back, v1, v2);
		}
	}
}

bool ApplyRetainedWorldLighting(HWDrawInfo* di, FRenderState& state, unsigned& slot,
	int light, int relativeLight, const FColormap& colormap, float alpha, bool additive, bool fogFirst,
	HWPreparedWall* owned, uint64_t ownerVersion)
{
	HWSceneScopeTimer totalTimer(SceneMainSurfaceProfile.RetainedLightingTotalNS);
	if (!di || !vk_retainedworld || di->mCurrentPortal || di->Viewpoint.bDoOob || di->Viewpoint.bDoOrtho)
		return false;
	// [Karto] A reused entry skips SetFog/SetColor, whose arguments world mode's flash rows replay.
	if (hw_LightTrace)
	{
		hw_LightTrace->Color = { di->lightmode, light, relativeLight, di->isFullbrightScene(), false, colormap, alpha };
		hw_LightTrace->Fog = { di->lightmode, light, relativeLight, di->isFullbrightScene(), true, additive, colormap };
		hw_LightTrace->HasColor = hw_LightTrace->HasFog = true;
		hw_LightTrace->FogLast = !fogFirst;
	}
	uint64_t contextVersion = 0;
	if (owned && ownerVersion && !vk_retainedworld_verify)
	{
		cycle_t timer;
		if (SceneProfiling) timer.ResetAndClock();
		contextVersion = RetainedWorld::ValidateLightingContext(di->Level);
		if (owned->OwnedLightingVersion == ownerVersion && owned->OwnedLightingContext == contextVersion &&
			slot < RetainedWorld::Lighting.size())
		{
			const auto& entry = RetainedWorld::Lighting[slot];
			if (entry.ContextVersion == contextVersion && entry.Inputs.Mode == di->lightmode &&
				entry.Inputs.Fullbright == di->isFullbrightScene())
			{
				if (vk_retainedlighting && vk_retainedparams && vk_retainedworld == 2 && screen->IsVulkan())
				{
					state.DeferWorldLighting(entry.State);
					if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedLightingDeferred;
				}
				else state.ApplyWorldLighting(entry.State);
				if (SceneProfiling)
				{
					++SceneMainSurfaceProfile.RetainedAppearanceReuses;
					SceneMainSurfaceProfile.RetainedLightingKeys = RetainedWorld::Lighting.size();
					timer.Unclock();
					SceneMainSurfaceProfile.RetainedAppearanceNS += uint64_t(timer.Time() * 1e9);
				}
				return true;
			}
		}
	}
	const RetainedWorld::LightingInputs inputs = { di->Level, colormap.LightColor.d, colormap.FadeColor.d,
		light, relativeLight, di->lightmode, alpha, colormap.Desaturation, colormap.BlendFactor, colormap.FogDensity,
		di->isFullbrightScene(), additive, fogFirst };
	if (slot >= RetainedWorld::Lighting.size() || !(RetainedWorld::Lighting[slot].Inputs == inputs))
	{
		auto found = RetainedWorld::LightingLookup.find(inputs);
		if (found != RetainedWorld::LightingLookup.end()) slot = found->second;
		else
		{
			if (RetainedWorld::Lighting.size() == RetainedWorld::LightingCapacity)
			{
				if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedAppearanceFallbacks;
				return false;
			}
			slot = unsigned(RetainedWorld::Lighting.size());
			RetainedWorld::Lighting.push_back({ inputs });
			RetainedWorld::LightingLookup.emplace(inputs, slot);
		}
	}
	cycle_t timer;
	if (SceneProfiling) timer.ResetAndClock();
	auto& entry = RetainedWorld::Lighting[slot];
	auto prepare = [&]()
	{
		// [Karto] Flats and walls call these in different orders; software bright-fog lighting makes that order observable.
		if (fogFirst) SetFog(state, di->Level, di->lightmode, light, relativeLight, inputs.Fullbright, &colormap, additive);
		SetColor(state, di->Level, di->lightmode, light, relativeLight, inputs.Fullbright, colormap, alpha);
		if (!fogFirst) SetFog(state, di->Level, di->lightmode, light, relativeLight, inputs.Fullbright, &colormap, additive);
	};
	if (!contextVersion) contextVersion = RetainedWorld::ValidateLightingContext(di->Level);
	if (entry.ContextVersion != contextVersion)
	{
		prepare();
		entry.State = state.CaptureWorldLighting();
		entry.ContextVersion = contextVersion;
		if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedAppearanceBuilds;
	}
	else
	{
		const bool deferred = vk_retainedlighting && vk_retainedparams && vk_retainedworld == 2 && screen->IsVulkan();
		if (deferred)
		{
			state.DeferWorldLighting(entry.State);
			if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedLightingDeferred;
		}
		else state.ApplyWorldLighting(entry.State);
		if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedAppearanceReuses;
		if (vk_retainedworld_verify)
		{
			prepare();
			if (!(entry.State == state.CaptureWorldLighting()))
				I_FatalError("Retained lighting reference differs at slot %u", slot);
			if (SceneProfiling) ++SceneMainSurfaceProfile.RetainedAppearanceVerified;
			if (deferred) state.DeferWorldLighting(entry.State);
		}
	}
	if (SceneProfiling)
	{
		SceneMainSurfaceProfile.RetainedLightingKeys = RetainedWorld::Lighting.size();
		timer.Unclock();
		SceneMainSurfaceProfile.RetainedAppearanceNS += uint64_t(timer.Time() * 1e9);
	}
	if (owned && ownerVersion)
	{
		owned->OwnedLightingVersion = ownerVersion;
		owned->OwnedLightingContext = contextVersion;
	}
	return true;
}

template<bool Compact>
bool ApplyRetainedSectorLightingStorage(HWDrawInfo* di, FRenderState& state, sector_t* sector, int plane,
	int light, int relativeLight, const FColormap& colormap, float alpha)
{
	if (!sector || plane < 0 || plane > 1) return false;
	auto& geometry = RetainedWorld::ValidateSector<Compact>(sector);
	return geometry.Ordinary && ApplyRetainedWorldLighting(di, state, RetainedWorld::SectorAppearance<Compact>(sector->Index()).LightingSlots[plane],
		light, relativeLight, colormap, alpha, false, false);
}

bool ApplyRetainedSectorLighting(HWDrawInfo* di, FRenderState& state, sector_t* sector, int plane,
	int light, int relativeLight, const FColormap& colormap, float alpha)
{
	if (RetainedWorld::CompactSectors) return ApplyRetainedSectorLightingStorage<true>(di, state, sector, plane, light, relativeLight, colormap, alpha);
	return ApplyRetainedSectorLightingStorage<false>(di, state, sector, plane, light, relativeLight, colormap, alpha);
}

bool RetainedWorldShadersCompatible()
{
	if (!RetainedWorld::ShadersChecked)
	{
		RetainedWorld::ShaderSourcesCompatible = true;
		for (int entry = fileSystem.GetFirstEntry(0); entry <= fileSystem.GetLastEntry(0); ++entry)
		{
			const char* name = fileSystem.GetFileFullName(entry);
			if (name && strnicmp(name, "shaders/glsl/", 13) == 0)
			{
				const int resolved = fileSystem.CheckNumForFullName(name);
				if (resolved < 0 || fileSystem.GetFileContainer(resolved) != 0)
					RetainedWorld::ShaderSourcesCompatible = false;
			}
		}
		RetainedWorld::ShadersChecked = true;
	}
	return RetainedWorld::ShaderSourcesCompatible;
}

template<bool Compact>
const HWRetainedSectorAppearance* QueryRetainedSectorAppearanceStorage(sector_t* sector, int plane)
{
	if (!vk_retainedworld || !sector || sector != &sector->Level->sectors[sector->Index()]) return nullptr;
	auto& state = RetainedWorld::ValidateSector<Compact>(sector);
	if (!state.Ordinary) return nullptr;
	auto& appearance = RetainedWorld::ValidateAppearance<Compact>(sector);
	if (plane >= 0 && plane < 2) RetainedWorld::ValidatePlaneAppearance<Compact>(sector, plane);
	return &appearance;
}

const HWRetainedSectorAppearance* QueryRetainedSectorAppearance(sector_t* sector, int plane)
{
	if (RetainedWorld::CompactSectors) return QueryRetainedSectorAppearanceStorage<true>(sector, plane);
	return QueryRetainedSectorAppearanceStorage<false>(sector, plane);
}

uint64_t GetRetainedWorldMemory()
{
	uint64_t bytes = RetainedWorld::GetSideEntryMemory() +
		RetainedWorld::EmptySides.capacity() * sizeof(RetainedWorld::EmptyState) +
		RetainedWorld::GetSectorMemory() +
		RetainedWorld::Geometry.size() * sizeof(HWPreparedWall) + RetainedWorld::Textures.capacity() * sizeof(RetainedWorld::TextureState) +
		RetainedWorld::Lighting.size() * sizeof(RetainedWorld::LightingEntry) +
		RetainedWorld::LightingLookup.size() * (sizeof(RetainedWorld::LightingInputs) + sizeof(unsigned) + 2 * sizeof(void*)) +
		RetainedWorld::LightingLookup.bucket_count() * sizeof(void*) +
		RetainedWorld::TextureGeometryLookup.size() * (sizeof(RetainedWorld::TextureGeometryInputs) + sizeof(uint64_t) + 2 * sizeof(void*)) +
		RetainedWorld::TextureGeometryLookup.bucket_count() * sizeof(void*);
	return bytes;
}

void BeginRetainedWorldView(FLevelLocals* level)
{
	if (RetainedWorld::ResourceGeneration != TexMan.GetResourceGeneration())
	{
		ClearRetainedWorld();
		screen->mVertexData->ClearWallVertexCache();
		RetainedWorld::ResourceGeneration = TexMan.GetResourceGeneration();
	}
	RetainedWorld::SwitchSectorLayout(vk_sectorlayout && vk_retainedworld);
	++RetainedWorld::View;
	if (vk_retainedworld == 2 && RetainedWorldShadersCompatible())
		screen->mVertexData->EnsureRetainedFlatIDs(level->sectors);
}

void ClearRetainedWorld()
{
	// [Karto] A deferred record must be consumed before its level/resource-owned storage is destroyed.
	if (screen && screen->RenderState()) screen->RenderState()->ResolveWorldLighting();
	RetainedWorld::ShadersChecked = false;
	RetainedWorld::ClearSideEntries();
	RetainedWorld::EmptySides.clear();
	RetainedWorld::Sectors.clear();
	RetainedWorld::SectorGeometry.clear();
	RetainedWorld::SectorAppearances.clear();
	RetainedWorld::Textures.clear();
		RetainedWorld::TextureGeometryVersion = 0;
		RetainedWorld::TextureGeometryLookup.clear();
		RetainedWorld::WallLightingView = 0;
		RetainedWorld::WallLightingVersion = 0;
		RetainedWorld::WallLightingLevel = nullptr;
	RetainedWorld::Geometry.clear();
	RetainedWorld::Lighting.clear();
	RetainedWorld::LightingLookup.clear();
	RetainedWorld::CurrentLightingContext = {};
	RetainedWorld::LightingContextView = 0;
	RetainedWorld::LightingContextVersion = 0;
	++RetainedWorld::View;
}
