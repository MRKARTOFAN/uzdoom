#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <unordered_map>

class FWallVertexCache
{
public:
	using Quad = std::array<uint8_t, 128>;
	enum class Status { Hit, Miss, Full };
	struct Match
	{
		Status State;
		unsigned Index;
	};

	explicit FWallVertexCache(unsigned capacity = 32768) : Capacity(capacity) {}

	Match Acquire(const void *side, uint8_t part, const Quad &quad)
	{
		Key key = { side, part };
		auto found = Entries.find(key);
		if (found != Entries.end() && found->second.Vertices == quad)
		{
			return { Status::Hit, found->second.Index };
		}
		if (Used == Capacity) return { Status::Full, 0 };
		// [Karto] Changed geometry gets a fresh slot; in-flight draws may still reference the old vertices.
		unsigned index = Used++ * 4;
		Entries.insert_or_assign(key, Entry { quad, index });
		return { Status::Miss, index };
	}

	void Clear()
	{
		Entries.clear();
		Used = 0;
	}

	unsigned GetCapacity() const { return Capacity; }
	unsigned GetUsed() const { return Used; }

private:
	struct Key
	{
		const void *Side;
		uint8_t Part;
		bool operator==(const Key &other) const { return Side == other.Side && Part == other.Part; }
	};
	struct KeyHash
	{
		size_t operator()(const Key &key) const
		{
			return (reinterpret_cast<uintptr_t>(key.Side) >> 3) ^ (size_t(key.Part) * 0x9e3779b9u);
		}
	};
	struct Entry
	{
		Quad Vertices;
		unsigned Index;
	};
	std::unordered_map<Key, Entry, KeyHash> Entries;
	unsigned Capacity;
	unsigned Used = 0;
};
