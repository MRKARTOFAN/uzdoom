/*
** hw_vertexbuilder.h
**
** Creates vertex meshes for sector planes
**
**---------------------------------------------------------------------------
**
** Copyright 2015-2018 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once
#include "tarray.h"
#include "r_defs.h"
struct vertex_t;

struct FQualifiedVertex
{
	vertex_t *vertex;       // index into vertex array
	int qualifier;          // some index to prevent vertices in different groups from being merged together.
};

template<> struct THashTraits<FQualifiedVertex>
{
	hash_t Hash(const FQualifiedVertex &key)
	{
		return (int)(((intptr_t)key.vertex) >> 4) ^ (key.qualifier << 16);
	}
	int Compare(const FQualifiedVertex &left, const FQualifiedVertex &right) { return left.vertex != right.vertex || left.qualifier != right.qualifier; }
};

//=============================================================================
//
// One sector's vertex data.
//
//=============================================================================

struct VertexContainer
{
	TArray<FQualifiedVertex> vertices;
	TMap<FQualifiedVertex, uint32_t> vertexmap;
	bool perSubsector = false;

	TArray<uint32_t> indices;

	uint32_t AddVertex(FQualifiedVertex *vert)
	{
		auto check = vertexmap.CheckKey(*vert);
		if (check != nullptr) return *check;
		auto index = vertices.Push(*vert);
		vertexmap[*vert] = index;
		return index;
	}

	uint32_t AddVertex(vertex_t *vert, int qualifier)
	{
		FQualifiedVertex vertx = { vert, qualifier};
		return AddVertex(&vertx);
	}

	uint32_t AddIndexForVertex(FQualifiedVertex *vert)
	{
		return indices.Push(AddVertex(vert));
	}

	uint32_t AddIndexForVertex(vertex_t *vert, int qualifier)
	{
		return indices.Push(AddVertex(vert, qualifier));
	}

	uint32_t AddIndex(uint32_t indx)
	{
		return indices.Push(indx);
	}
};

using VertexContainers = TArray<VertexContainer>;

VertexContainers BuildVertices(TArray<sector_t> &sectors);

class FFlatVertexBuffer;
void CheckUpdate(FFlatVertexBuffer* fvb, sector_t* sector);
void CreateVBO(FFlatVertexBuffer* fvb, TArray<sector_t>& sectors);

struct FFlatVertex;
// [Karto] While true, wall vertices go to a local buffer so a whole-map bake sweep cannot overrun the frame allocation.
extern bool g_bakeSweepActive;
extern uint64_t g_sweepMvCalls, g_sweepBaked, g_sweepSkipNoTex, g_sweepSkipSplit, g_sweepSkipCanCache, g_sweepSkipOther;
void GPUBakeRecordWall(const FFlatVertex* verts, seg_t* seg, int type, uint32_t texture, uint32_t sector);
void GPUBakeFinalize();
uint64_t GPUBakeEpoch();
void GPUClearPersistentWalls();
bool GPUBakeLookupWall(seg_t* seg, int type, const FFlatVertex* currentVerts, uint32_t* iboOffset, uint32_t* indexCount, uint32_t* row = nullptr);
bool GPUBakeLookupRow(seg_t* seg, int type, uint32_t* row);
bool GPUBakeWallVerts(seg_t* seg, int type, FFlatVertex* out);
void GPUBakeRunCull(const float* planes24, bool forDraw = false, const uint32_t* rows = nullptr, uint32_t count = 0);
void GPUBakeBeginView();
void GPUBakeAdmitWall(seg_t* seg, int type, const FFlatVertex* currentVerts);

struct FGpuDrawCommand
{
	uint32_t IndexCount;		// always 6 (canonical quad)
	uint32_t InstanceCount;	// 1
	uint32_t FirstIndex;		// 0 into the canonical index buffer
	int32_t VertexOffset;		// baked VboOffset
	uint32_t FirstInstance;	// 0
};
static_assert(sizeof(FGpuDrawCommand) == 5 * sizeof(uint32_t), "must match VkDrawIndexedIndirectCommand");

struct FGpuMaterialRow
{
	uint32_t TextureIndex;	// FTextureID index (bindless slot)
	uint32_t SectorIndex;
	uint32_t Plane;			// wall part / side
	uint32_t Flags;			// reserved: colour/texture-mode/epoch
};
static_assert(sizeof(FGpuMaterialRow) == 4 * sizeof(uint32_t), "material row ABI");
