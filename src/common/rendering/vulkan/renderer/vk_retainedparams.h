#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <cstddef>

template<class Row>
class RetainedParameterCache
{
public:
	struct Selection
	{
		const Row* Data = nullptr;
		bool Upload = false;
		bool CrossFrameReuse = false;
		bool OwnerReuse = false;
	};

	void Init(size_t surfaces)
	{
		Slots.assign(surfaces, 0);
		Entries.clear();
	}

	Selection ReuseOwned(uint32_t surface, uint32_t frame, uint32_t context, uint64_t ownerVersion, uint64_t inheritedLight = 0)
	{
		if (surface >= Slots.size() || !frame || !context || !ownerVersion || !Slots[surface]) return {};
		auto& entry = Entries[Slots[surface] - 1];
		if (entry.OwnerVersion != ownerVersion || entry.Context != context || entry.InheritedLight != inheritedLight) return {};
		const bool crossFrame = entry.Frame != frame;
		entry.Frame = frame;
		return { &entry.Data, false, crossFrame, true };
	}

	template<class Match, class Build>
	Selection Collect(uint32_t surface, uint32_t frame, bool persistent, Match&& match, Build&& build)
	{
		return CollectContext(surface, frame, persistent, 0,
			[&](const Row& row, uint32_t) { return match(row); }, build);
	}

	template<class Match, class Build>
	Selection CollectContext(uint32_t surface, uint32_t frame, bool persistent, uint32_t context, Match&& match, Build&& build, uint64_t ownerVersion = 0, uint64_t inheritedLight = 0)
	{
		if (surface >= Slots.size() || frame == 0) return {};
		const auto slot = Slots[surface];
		if (!slot)
		{
			Entries.push_back({ build(), frame, context, ownerVersion, inheritedLight });
			Slots[surface] = uint32_t(Entries.size());
			return { &Entries.back().Data, true, false };
		}
		auto& entry = Entries[slot - 1];
		if (persistent)
		{
			auto owned = ReuseOwned(surface, frame, context, ownerVersion, inheritedLight);
			if (owned.Data) return owned;
		}
		// [Karto] Recorded draws already reference this row; a conflicting revisit must fall back rather than overwrite it.
		if (entry.Frame == frame)
		{
			if (!match(entry.Data, entry.Context)) return {};
			entry.Context = context;
			entry.OwnerVersion = ownerVersion;
			entry.InheritedLight = inheritedLight;
			return { &entry.Data, false, false };
		}
		if (persistent && match(entry.Data, entry.Context))
		{
			entry.Frame = frame;
			entry.Context = context;
			entry.OwnerVersion = ownerVersion;
			entry.InheritedLight = inheritedLight;
			return { &entry.Data, false, true };
		}
		entry.Data = build();
		entry.Frame = frame;
		entry.Context = context;
		entry.OwnerVersion = ownerVersion;
		entry.InheritedLight = inheritedLight;
		return { &entry.Data, true, false };
	}

	void ResetFrames()
	{
		for (auto& entry : Entries) entry.Frame = 0;
	}

	size_t Memory() const
	{
		return Slots.capacity() * sizeof(uint32_t) + Entries.capacity() * sizeof(Entry);
	}

	bool Matches(uint32_t surface, const Row& reference) const
	{
		return surface < Slots.size() && Slots[surface] &&
			memcmp(&Entries[Slots[surface] - 1].Data, &reference, sizeof(Row)) == 0;
	}

private:
	struct Entry
	{
		Row Data;
		uint32_t Frame;
		uint32_t Context;
		uint64_t OwnerVersion;
		uint64_t InheritedLight;
	};
	std::vector<uint32_t> Slots;
	std::vector<Entry> Entries;
};

template<class Context, size_t Capacity = 64>
class RetainedContextCache
{
public:
	uint32_t Find(const Context& context)
	{
		if (Last && memcmp(&Entries[Last - 1], &context, sizeof(Context)) == 0) return Last;
		for (size_t i = 0; i < Entries.size(); ++i)
		{
			if (memcmp(&Entries[i], &context, sizeof(Context)) == 0) return Last = uint32_t(i + 1);
		}
		if (Entries.size() == Capacity) return 0;
		Entries.push_back(context);
		return Last = uint32_t(Entries.size());
	}

	size_t Memory() const { return Entries.capacity() * sizeof(Context); }

private:
	std::vector<Context> Entries;
	uint32_t Last = 0;
};

inline uint64_t MakeRetainedInheritedLight(const HWWorldLightingState* lighting, int liveFogEnabled, int fogMode, const float* liveLight)
{
	const int fogEnabled = lighting ? lighting->FogEnabled : liveFogEnabled;
	if (fogEnabled == 2 || (fogEnabled && fogMode)) return 0;
	const float inherited[] = { lighting && fogEnabled ? lighting->LightParameters.X : liveLight[0],
		lighting && fogEnabled ? lighting->LightParameters.Y : liveLight[1] };
	uint64_t bits;
	static_assert(sizeof(inherited) == sizeof(bits));
	memcpy(&bits, inherited, sizeof(bits));
	return bits;
}

inline RetainedSurfaceData MakeRetainedWallContext(const StreamData& stream, const VSMatrix& transform, const PushConstants& push)
{
	RetainedSurfaceData context = { stream, transform, {},
		{ push.uClipSplit.X, push.uClipSplit.Y, push.uSpecularMaterial.X, push.uSpecularMaterial.Y },
		push.uTextureMode, 0, 0, push.uAlphaThreshold };
	context.Stream.uVertexNormal = {};
	context.Stream.uVertexColor = {};
	context.Stream.uFogColor = FVector4PalEntry{};
	context.Stream.uDesaturationFactor = 0;
	context.Stream.timer = 0;
	return context;
}

inline bool MatchesRetainedWallSurface(const RetainedSurfaceData& cached, const StreamData& stream,
	const PushConstants& push, const HWWorldLightingState* lighting)
{
	const auto& vertex = lighting ? lighting->VertexColor : stream.uVertexColor;
	const auto& fog = lighting ? lighting->FogColor : stream.uFogColor;
	const auto& desaturation = lighting ? lighting->Desaturation : stream.uDesaturationFactor;
	const FVector4 light = { push.uLightDist, push.uLightFactor, push.uFogDensity, push.uLightLevel };
	return memcmp(&cached.Stream.uVertexNormal, &stream.uVertexNormal, sizeof(stream.uVertexNormal)) == 0 &&
		memcmp(&cached.Stream.uVertexColor, &vertex, sizeof(vertex)) == 0 &&
		memcmp(&cached.Stream.uFogColor, &fog, sizeof(fog)) == 0 &&
		memcmp(&cached.Stream.uDesaturationFactor, &desaturation, sizeof(desaturation)) == 0 &&
		memcmp(&cached.Light, &light, sizeof(light)) == 0 &&
		cached.FogEnabled == push.uFogEnabled && cached.LightIndex == push.uLightIndex;
}

inline bool MatchesRetainedParameters(const RetainedSurfaceData& cached, const StreamData& stream,
	const VSMatrix& textureTransform, const PushConstants& push, bool independentTime,
	const HWWorldLightingState* lighting = nullptr)
{
	constexpr size_t timerOffset = offsetof(StreamData, timer);
	constexpr size_t afterTimer = timerOffset + sizeof(stream.timer);
	bool sameStream;
	if (lighting)
	{
		constexpr size_t fogOffset = offsetof(StreamData, uFogColor);
		constexpr size_t afterDesaturation = offsetof(StreamData, uDesaturationFactor) + sizeof(stream.uDesaturationFactor);
		constexpr size_t vertexOffset = offsetof(StreamData, uVertexColor);
		constexpr size_t afterVertex = vertexOffset + sizeof(stream.uVertexColor);
		const auto* saved = reinterpret_cast<const char*>(&cached.Stream);
		const auto* live = reinterpret_cast<const char*>(&stream);
		sameStream = memcmp(saved, live, fogOffset) == 0 &&
			memcmp(&cached.Stream.uFogColor, &lighting->FogColor, sizeof(lighting->FogColor)) == 0 &&
			memcmp(&cached.Stream.uDesaturationFactor, &lighting->Desaturation, sizeof(lighting->Desaturation)) == 0 &&
			memcmp(&cached.Stream.uVertexColor, &lighting->VertexColor, sizeof(lighting->VertexColor)) == 0 &&
			memcmp(saved + afterVertex, live + afterVertex, sizeof(StreamData) - afterVertex) == 0 &&
			(independentTime ? memcmp(saved + afterDesaturation, live + afterDesaturation, timerOffset - afterDesaturation) == 0 &&
				memcmp(saved + afterTimer, live + afterTimer, vertexOffset - afterTimer) == 0 :
				memcmp(saved + afterDesaturation, live + afterDesaturation, vertexOffset - afterDesaturation) == 0);
	}
	else
	{
		sameStream = independentTime ? memcmp(&cached.Stream, &stream, timerOffset) == 0 &&
			memcmp(reinterpret_cast<const char*>(&cached.Stream) + afterTimer,
				reinterpret_cast<const char*>(&stream) + afterTimer, sizeof(StreamData) - afterTimer) == 0 :
			memcmp(&cached.Stream, &stream, sizeof(StreamData)) == 0;
	}
	const FVector4 light = { push.uLightDist, push.uLightFactor, push.uFogDensity, push.uLightLevel };
	const FVector4 clipSpecular = { push.uClipSplit.X, push.uClipSplit.Y, push.uSpecularMaterial.X, push.uSpecularMaterial.Y };
	return sameStream && memcmp(&cached.TextureTransform, &textureTransform, sizeof(VSMatrix)) == 0 &&
		memcmp(&cached.Light, &light, sizeof(light)) == 0 && memcmp(&cached.ClipSpecular, &clipSpecular, sizeof(clipSpecular)) == 0 &&
		cached.TextureMode == push.uTextureMode && cached.FogEnabled == push.uFogEnabled &&
		cached.LightIndex == push.uLightIndex && memcmp(&cached.AlphaThreshold, &push.uAlphaThreshold, sizeof(float)) == 0;
}
