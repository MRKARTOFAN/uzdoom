/*
** hw_vertexbuilder.cpp
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

#include "g_levellocals.h"
#include "hw_vertexbuilder.h"
#include "flatvertices.h"
#include "hw_renderstate.h"
#include "earcut.hpp"
#include "v_video.h"
#include "i_time.h"
#include "c_dispatch.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>

//=============================================================================
//
//
//
//=============================================================================

static void CreateVerticesForSubsector(subsector_t *sub, VertexContainer &gen, int qualifier)
{
	if (sub->numlines < 3) return;

	uint32_t startindex = gen.indices.Size();

	if ((sub->flags & SSECF_HOLE) && sub->numlines > 3)
	{
		// Hole filling "subsectors" are not necessarily convex so they require real triangulation.
		// These things are extremely rare so performance is secondary here.

		using Point = std::pair<double, double>;
		std::vector<std::vector<Point>> polygon;
		std::vector<Point> *curPoly;

		polygon.resize(1);
		curPoly = &polygon.back();
		curPoly->resize(sub->numlines);

		for (unsigned i = 0; i < sub->numlines; i++)
		{
			(*curPoly)[i] = { sub->firstline[i].v1->fX(), sub->firstline[i].v1->fY() };
		}
		auto indices = mapbox::earcut(polygon);
		for (auto vti : indices)
		{
			gen.AddIndexForVertex(sub->firstline[vti].v1, qualifier);
		}
	}
	else
	{
		int firstndx = gen.AddVertex(sub->firstline[0].v1, qualifier);
		int secondndx = gen.AddVertex(sub->firstline[1].v1, qualifier);
		for (unsigned int k = 2; k < sub->numlines; k++)
		{
			gen.AddIndex(firstndx);
			gen.AddIndex(secondndx);
			auto ndx = gen.AddVertex(sub->firstline[k].v1, qualifier);
			gen.AddIndex(ndx);
			secondndx = ndx;
		}
	}
}

//=============================================================================
//
//
//
//=============================================================================

static void TriangulateSection(FSection &sect, VertexContainer &gen, int qualifier)
{
	if (sect.segments.Size() < 3) return;

	// todo
}

//=============================================================================
//
//
//
//=============================================================================


static void CreateVerticesForSection(FSection &section, VertexContainer &gen, bool useSubsectors)
{
	section.vertexindex = gen.indices.Size();

	if (useSubsectors)
	{
		for (auto sub : section.subsectors)
		{
			CreateVerticesForSubsector(sub, gen, -1);
		}
	}
	else
	{
		TriangulateSection(section, gen, -1);
	}
	section.vertexcount = gen.indices.Size() - section.vertexindex;
}

//==========================================================================
//
// Creates the vertices for one plane in one subsector
//
//==========================================================================

static void CreateVerticesForSector(sector_t *sec, VertexContainer &gen)
{
	auto sections = sec->Level->sections.SectionsForSector(sec);
	for (auto &section :sections)
	{
		CreateVerticesForSection( section, gen, true);
	}
}


TArray<VertexContainer> BuildVertices(TArray<sector_t> &sectors)
{
	TArray<VertexContainer> verticesPerSector(sectors.Size(), true);
	for (unsigned i=0; i< sectors.Size(); i++)
	{
		CreateVerticesForSector(&sectors[i], verticesPerSector[i]);
	}
	return verticesPerSector;
}

//==========================================================================
//
// Creates the vertices for one plane in one subsector
//
//==========================================================================

//==========================================================================
//
// Find a 3D floor
//
//==========================================================================

static F3DFloor *Find3DFloor(sector_t* target, sector_t* model, int &ffloorIndex)
{
	for (unsigned i = 0; i < target->e->XFloor.ffloors.Size(); i++)
	{
		F3DFloor* ffloor = target->e->XFloor.ffloors[i];
		if (ffloor->model == model && !(ffloor->flags & FF_THISINSIDE))
		{
			ffloorIndex = i;
			return ffloor;
		}
	}
	ffloorIndex = -1;
	return NULL;
}

//==========================================================================
//
// Initialize a single vertex
//
//==========================================================================

static void SetFlatVertex(FFlatVertex& ffv, vertex_t* vt, const secplane_t& plane)
{
	ffv.x = (float)vt->fX();
	ffv.y = (float)vt->fY();
	ffv.z = (float)plane.ZatPoint(vt);
	ffv.u = (float)vt->fX() / 64.f;
	ffv.v = -(float)vt->fY() / 64.f;
	ffv.lindex = -1.0f;
}

static void SetFlatVertex(FFlatVertex& ffv, vertex_t* vt, const secplane_t& plane, float llu, float llv, int llindex)
{
	ffv.x = (float)vt->fX();
	ffv.y = (float)vt->fY();
	ffv.z = (float)plane.ZatPoint(vt);
	ffv.u = (float)vt->fX() / 64.f;
	ffv.v = -(float)vt->fY() / 64.f;
	ffv.lu = llu;
	ffv.lv = llv;
	ffv.lindex = (float)llindex;
}

//==========================================================================
//
// Creates the vertices for one plane in one subsector w/lightmap support.
// Sectors with lightmaps cannot share subsector vertices.
//
//==========================================================================

static int CreateIndexedSectorVerticesLM(FFlatVertexBuffer* fvb, sector_t* sec, const secplane_t& plane, int floor, int h, int lightmapIndex)
{
	int i, pos;
	float diff;

	auto& ibo_data = fvb->ibo_data;

	int rt = ibo_data.Size();
	if (sec->transdoor && floor) diff = -1.f;
	else diff = 0.f;

	// Allocate space
	for (i = 0, pos = 0; i < sec->subsectorcount; i++)
	{
		pos += sec->subsectors[i]->numlines;
	}

	auto& vbo_shadowdata = fvb->vbo_shadowdata;
	int vi = vbo_shadowdata.Reserve(pos);
	int idx = ibo_data.Reserve((pos - 2 * sec->subsectorcount) * 3);

	// Create the actual vertices.
	for (i = 0, pos = 0; i < sec->subsectorcount; i++)
	{
		subsector_t* sub = sec->subsectors[i];
		LightmapSurface* lightmap = &sub->lightmap[h][lightmapIndex];
		if (lightmap->Type != ST_NULL)
		{
			float* luvs = lightmap->TexCoords;
			int lindex = lightmap->LightmapNum;
			for (unsigned int j = 0; j < sub->numlines; j++)
			{
				SetFlatVertex(vbo_shadowdata[vi + pos], sub->firstline[j].v1, plane, luvs[j * 2], luvs[j * 2 + 1], lindex);
				vbo_shadowdata[vi + pos].z += diff;
				pos++;
			}
		}
		else
		{
			for (unsigned int j = 0; j < sub->numlines; j++)
			{
				SetFlatVertex(vbo_shadowdata[vi + pos], sub->firstline[j].v1, plane);
				vbo_shadowdata[vi + pos].z += diff;
				pos++;
			}
		}
	}

	// Create the indices for the subsectors
	for (i = 0, pos = 0; i < sec->subsectorcount; i++)
	{
		subsector_t* sub = sec->subsectors[i];
		int firstndx = vi + pos;
		for (unsigned int k = 2; k < sub->numlines; k++)
		{
			ibo_data[idx++] = firstndx;
			ibo_data[idx++] = firstndx + k - 1;
			ibo_data[idx++] = firstndx + k;
		}
		pos += sec->subsectors[i]->numlines;
	}

	sec->ibocount = ibo_data.Size() - rt;
	return rt;
}

static int CreateIndexedSectorVertices(FFlatVertexBuffer* fvb, sector_t* sec, const secplane_t& plane, int floor, VertexContainer& verts, int h, int lightmapIndex)
{
	if (sec->HasLightmaps && lightmapIndex != -1)
		return CreateIndexedSectorVerticesLM(fvb, sec, plane, floor, h, lightmapIndex);

	auto& vbo_shadowdata = fvb->vbo_shadowdata;
	unsigned vi = vbo_shadowdata.Reserve(verts.vertices.Size());
	float diff;

	// Create the actual vertices.
	if (sec->transdoor && floor) diff = -1.f;
	else diff = 0.f;
	for (unsigned i = 0; i < verts.vertices.Size(); i++)
	{
		SetFlatVertex(vbo_shadowdata[vi + i], verts.vertices[i].vertex, plane);
		vbo_shadowdata[vi + i].z += diff;
	}

	auto& ibo_data = fvb->ibo_data;
	unsigned rt = ibo_data.Reserve(verts.indices.Size());
	for (unsigned i = 0; i < verts.indices.Size(); i++)
	{
		ibo_data[rt + i] = vi + verts.indices[i];
	}
	return (int)rt;
}

//==========================================================================
//
//
//
//==========================================================================

static int CreateIndexedVertices(FFlatVertexBuffer* fvb, int h, sector_t* sec, const secplane_t& plane, int floor, VertexContainers& verts)
{
	auto& vbo_shadowdata = fvb->vbo_shadowdata;
	sec->vboindex[h] = vbo_shadowdata.Size();
	// First calculate the vertices for the sector itself
	for (int n = 0; n < screen->mPipelineNbr; n++)
		sec->vboheight[n][h] = sec->GetPlaneTexZ(h);
	sec->ibocount = verts[sec->Index()].indices.Size();
	sec->iboindex[h] = CreateIndexedSectorVertices(fvb, sec, plane, floor, verts[sec->Index()], h, 0);

	// Next are all sectors using this one as heightsec
	TArray<sector_t*>& fakes = sec->e->FakeFloor.Sectors;
	for (unsigned g = 0; g < fakes.Size(); g++)
	{
		sector_t* fsec = fakes[g];
		fsec->iboindex[2 + h] = CreateIndexedSectorVertices(fvb, fsec, plane, false, verts[fsec->Index()], h, -1);
	}

	// and finally all attached 3D floors
	TArray<sector_t*>& xf = sec->e->XFloor.attached;
	for (unsigned g = 0; g < xf.Size(); g++)
	{
		sector_t* fsec = xf[g];
		int ffloorIndex;
		F3DFloor* ffloor = Find3DFloor(fsec, sec, ffloorIndex);

		if (ffloor != NULL && ffloor->flags & FF_RENDERPLANES)
		{
			bool dotop = (ffloor->top.model == sec) && (ffloor->top.isceiling == h);
			bool dobottom = (ffloor->bottom.model == sec) && (ffloor->bottom.isceiling == h);

			if (dotop || dobottom)
			{
				auto ndx = CreateIndexedSectorVertices(fvb, fsec, plane, false, verts[fsec->Index()], h, ffloorIndex + 1);
				if (dotop) ffloor->top.vindex = ndx;
				if (dobottom) ffloor->bottom.vindex = ndx;
			}
		}
	}
	sec->vbocount[h] = vbo_shadowdata.Size() - sec->vboindex[h];
	return sec->iboindex[h];
}


//==========================================================================
//
//
//
//==========================================================================

static void CreateIndexedFlatVertices(FFlatVertexBuffer* fvb, TArray<sector_t>& sectors)
{
	auto verts = BuildVertices(sectors);

	int i = 0;
	/*
	for (auto &vert : verts)
	{
		Printf(PRINT_LOG, "Sector %d\n", i);
		Printf(PRINT_LOG, "%d vertices, %d indices\n", vert.vertices.Size(), vert.indices.Size());
		int j = 0;
		for (auto &v : vert.vertices)
		{
			Printf(PRINT_LOG, "    %d: (%2.3f, %2.3f)\n", j++, v.vertex->fX(), v.vertex->fY());
		}
		for (unsigned i=0;i<vert.indices.Size();i+=3)
		{
			Printf(PRINT_LOG, "     %d, %d, %d\n", vert.indices[i], vert.indices[i + 1], vert.indices[i + 2]);
		}

		i++;
	}
	*/


	for (int h = sector_t::floor; h <= sector_t::ceiling; h++)
	{
		for (auto& sec : sectors)
		{
			CreateIndexedVertices(fvb, h, &sec, sec.GetSecPlane(h), h == sector_t::floor, verts);
		}
	}

	// We need to do a final check for Vavoom water and FF_FIX sectors.
	// No new vertices are needed here. The planes come from the actual sector
	for (auto& sec : sectors)
	{
		for (auto ff : sec.e->XFloor.ffloors)
		{
			if (ff->top.model == &sec)
			{
				ff->top.vindex = sec.iboindex[ff->top.isceiling];
			}
			if (ff->bottom.model == &sec)
			{
				ff->bottom.vindex = sec.iboindex[ff->top.isceiling];
			}
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

static void UpdatePlaneVertices(FFlatVertexBuffer *fvb, sector_t* sec, int plane)
{
	int startvt = sec->vboindex[plane];
	int countvt = sec->vbocount[plane];
	secplane_t& splane = sec->GetSecPlane(plane);
	FFlatVertex* vt = &fvb->vbo_shadowdata[startvt];
	FFlatVertex* mapvt = fvb->GetBuffer(startvt);
	for (int i = 0; i < countvt; i++, vt++, mapvt++)
	{
		vt->z = (float)splane.ZatPoint(vt->x, vt->y);
		if (plane == sector_t::floor && sec->transdoor) vt->z -= 1;
		mapvt->z = vt->z;
	}

	fvb->mVertexBuffer->Upload(startvt * sizeof(FFlatVertex), countvt * sizeof(FFlatVertex));
}

//==========================================================================
//
//
//
//==========================================================================

static void CreateVertices(FFlatVertexBuffer* fvb, TArray<sector_t>& sectors)
{
	fvb->vbo_shadowdata.Resize(FFlatVertexBuffer::NUM_RESERVED);
	CreateIndexedFlatVertices(fvb, sectors);
}

//==========================================================================
//
//
//
//==========================================================================

static void CheckPlanes(FFlatVertexBuffer* fvb, sector_t* sector)
{
	if (sector->GetPlaneTexZ(sector_t::ceiling) != sector->vboheight[screen->mVertexData->GetPipelinePos()][sector_t::ceiling])
	{
		UpdatePlaneVertices(fvb, sector, sector_t::ceiling);
		sector->vboheight[screen->mVertexData->GetPipelinePos()][sector_t::ceiling] = sector->GetPlaneTexZ(sector_t::ceiling);
	}
	if (sector->GetPlaneTexZ(sector_t::floor) != sector->vboheight[screen->mVertexData->GetPipelinePos()][sector_t::floor])
	{
		UpdatePlaneVertices(fvb, sector, sector_t::floor);
		sector->vboheight[screen->mVertexData->GetPipelinePos()][sector_t::floor] = sector->GetPlaneTexZ(sector_t::floor);
	}
}

//==========================================================================
//
// checks the validity of all planes attached to this sector
// and updates them if possible.
//
//==========================================================================

void CheckUpdate(FFlatVertexBuffer* fvb, sector_t* sector)
{
	CheckPlanes(fvb, sector);
	sector_t* hs = sector->GetHeightSec();
	if (hs != NULL) CheckPlanes(fvb, hs);
	for (unsigned i = 0; i < sector->e->XFloor.ffloors.Size(); i++)
		CheckPlanes(fvb, sector->e->XFloor.ffloors[i]->model);
}

//==========================================================================
//
//
//
//==========================================================================

static void GPUBakeCensus(TArray<sector_t>& sectors);
static void GPUBakeFlats(FFlatVertexBuffer* fvb, TArray<sector_t>& sectors);

void CreateVBO(FFlatVertexBuffer* fvb, TArray<sector_t>& sectors)
{
	// [Karto] A new level can reuse sidedef addresses; retained geometry must not survive this rebuild.
	fvb->ClearWallVertexCache();
	ClearRetainedWorld();
	fvb->vbo_shadowdata.Resize(fvb->mNumReserved);
	CreateVertices(fvb, sectors);
	fvb->RetainedFlatIDBuffer.reset();
	fvb->RetainedFlatSetupNS = 0;
	fvb->RetainedFlatMetadataBytes = 0;
	fvb->mCurIndex = fvb->mIndex = fvb->vbo_shadowdata.Size();
	fvb->Copy(0, fvb->mIndex);
	fvb->mIndexBuffer->SetData(fvb->ibo_data.Size() * sizeof(uint32_t), &fvb->ibo_data[0], BufferUsageType::Static);
	GPUBakeCensus(sectors);
	GPUBakeFlats(fvb, sectors);
}

CVAR(Bool, vk_gpu_driven_census, false, 0)

static bool CensusOrdinary(sector_t* sector)
{
	// [Karto] Mirrors RetainedWorld::OrdinarySector without pulling the retained-world header into the vertex builder.
	return !sector || (!sector->floorplane.isSlope() && !sector->ceilingplane.isSlope() &&
		!sector->GetHeightSec() && !sector->Portals[0] && !sector->Portals[1] &&
		sector->GetReflect(0) == 0 && sector->GetReflect(1) == 0 &&
		!sector->e->XFloor.ffloors.Size() && !sector->e->XFloor.lightlist.Size());
}

static void GPUBakeCensus(TArray<sector_t>& sectors)
{
	if (!vk_gpu_driven_census || sectors.Size() == 0) return;
	FLevelLocals* Level = sectors[0].Level;
	if (!Level) return;
	uint64_t ordinarySectors = 0;
	for (auto& sector : sectors) if (CensusOrdinary(&sector)) ++ordinarySectors;
	uint64_t wallParts = 0, eligibleWallParts = 0;
	for (auto& line : Level->lines)
	{
		auto front = line.sidedef[0] ? line.sidedef[0]->sector : nullptr;
		auto back = line.sidedef[1] ? line.sidedef[1]->sector : nullptr;
		uint64_t parts = 0;
		if (line.sidedef[0] && line.sidedef[1])
		{
			parts = 1;	// mid
			if (front->ceilingplane.fD() > back->ceilingplane.fD()) ++parts;	// upper
			if (front->floorplane.fD() < back->floorplane.fD()) ++parts;		// lower
		}
		else if (line.sidedef[0] || line.sidedef[1]) parts = 1;
		wallParts += parts;
		if (CensusOrdinary(front) && (!back || CensusOrdinary(back))) eligibleWallParts += parts;
	}
	const uint64_t flats = ordinarySectors * 2;
	const uint64_t primitives = flats + eligibleWallParts;
	const uint64_t verts = primitives * 4;
	const uint64_t bytes = verts * sizeof(FFlatVertex) + primitives * 6 * sizeof(uint32_t);
	Printf("GPU-Driven census: sectors=%u ordinary=%llu flats=%llu wallParts=%llu eligibleWallParts=%llu -> primitives=%llu verts=%llu bytes=%llu\n",
		(unsigned)sectors.Size(), (unsigned long long)ordinarySectors, (unsigned long long)flats,
		(unsigned long long)wallParts, (unsigned long long)eligibleWallParts,
		(unsigned long long)primitives, (unsigned long long)verts, (unsigned long long)bytes);
}

CVAR(Bool, vk_gpu_driven_bake, false, 0)
CVAR(Bool, vk_gpu_driven_draw, false, 0)
CVAR(Bool, vk_gpu_driven_wallbake, false, 0)
CVAR(Bool, vk_gpu_driven_bakerow, false, 0)
bool g_bakeSweepActive = false;
uint64_t g_sweepMvCalls = 0, g_sweepBaked = 0, g_sweepSkipNoTex = 0, g_sweepSkipSplit = 0, g_sweepSkipCanCache = 0, g_sweepSkipOther = 0;
CVAR(Bool, vk_gpu_driven_debug, false, 0)

struct FGPUBakedPrimitive
{
	uint32_t VboOffset;
	uint32_t VertexCount;
	uint32_t IboOffset;
	uint32_t IndexCount;
	uint32_t TextureIndex;
	uint32_t SectorIndex;
	int32_t Plane;
	uint32_t Kind;	// 0 = flat, 1 = wall
};
static_assert(sizeof(FGPUBakedPrimitive) == 8 * sizeof(uint32_t));
static_assert(offsetof(FGPUBakedPrimitive, Kind) == 7 * sizeof(uint32_t));

static std::vector<FFlatVertex> g_bakeVerts;
static std::vector<uint32_t> g_bakeIndices;
static std::vector<FGPUBakedPrimitive> g_bakeTable;
static std::set<uint64_t> g_bakeWallKeys;
// [Karto] Draws must validate the complete live quad because scripts can change UVs without moving a sector.
struct FBakeWallEntry { uint32_t IboOffset; uint32_t IndexCount; uint32_t VboOffset; uint32_t Row; bool Pending = false; };
static std::unordered_map<uint64_t, FBakeWallEntry> g_bakeWallRow;
static bool g_bakeDirty = false;
static bool g_bakeUploaded = false;
static uint64_t g_bakeEpoch = 1;

CCMD(gpu_resident_stats)
{
	Printf("G3RESIDENTSTATS direct_draws=%llu\n", static_cast<unsigned long long>(screen->RenderState()->GpuResidentDrawCount()));
}

uint64_t GPUBakeEpoch()
{
	return g_bakeUploaded ? g_bakeEpoch : 0;
}
static size_t g_bakeUploadedVerts = 0;
static size_t g_bakeUploadedIndices = 0;

void GPUBakeRecordWall(const FFlatVertex* wallVerts, seg_t* seg, int type, uint32_t texture, uint32_t sector)
{
	if (!vk_gpu_driven_bake || !seg || !wallVerts || !texture) return;
	const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(seg)) << 8) ^ uint64_t(type & 0xff);
	if (!g_bakeWallKeys.insert(key).second)
	{
		auto& entry = g_bakeWallRow.at(key);
		if (memcmp(wallVerts, &g_bakeVerts[entry.VboOffset], 4 * sizeof(FFlatVertex)) != 0)
		{
			// [Karto] Host shadow changes cannot certify old device bytes; publish the new epoch only after this view ends.
			memcpy(&g_bakeVerts[entry.VboOffset], wallVerts, 4 * sizeof(FFlatVertex));
			entry.Pending = g_bakeDirty = true;
		}
		return;
	}
	const uint32_t base = uint32_t(g_bakeVerts.size());
	for (int i = 0; i < 4; ++i) g_bakeVerts.push_back(wallVerts[i]);
	const uint32_t ibo = uint32_t(g_bakeIndices.size());
	g_bakeIndices.push_back(base + 0); g_bakeIndices.push_back(base + 1); g_bakeIndices.push_back(base + 2);
	g_bakeIndices.push_back(base + 0); g_bakeIndices.push_back(base + 2); g_bakeIndices.push_back(base + 3);
	FGPUBakedPrimitive primitive = {};
	primitive.VboOffset = base;
	primitive.VertexCount = 4;
	primitive.IboOffset = ibo;
	primitive.IndexCount = 6;
	primitive.TextureIndex = texture;
	primitive.SectorIndex = sector;
	primitive.Plane = type;
	primitive.Kind = 1;
	g_bakeTable.push_back(primitive);
	auto* fs = seg->frontsector;
	auto* bs = seg->backsector;
	g_bakeWallRow[key] = { primitive.IboOffset, primitive.IndexCount, primitive.VboOffset, uint32_t(g_bakeTable.size() - 1) };
	if (vk_gpu_driven_debug)
		Printf("G1BAKE seg=%p type=%d sect=%u z=[%.2f %.2f %.2f %.2f] fs(%.1f,%.1f) back=%d\n",
			(void*)seg, type, sector, wallVerts[0].z, wallVerts[1].z, wallVerts[2].z, wallVerts[3].z,
			fs ? fs->CenterFloor() : 0.0, fs ? fs->CenterCeiling() : 0.0, int(bs != nullptr));
	g_bakeDirty = true;
}

bool GPUBakeLookupWall(seg_t* seg, int type, const FFlatVertex* currentVerts, uint32_t* iboOffset, uint32_t* indexCount, uint32_t* row)
{
	if (!seg || !currentVerts || !vk_gpu_driven_bake || !g_bakeUploaded) return false;
	const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(seg)) << 8) ^ uint64_t(type & 0xff);
	auto it = g_bakeWallRow.find(key);
	if (it == g_bakeWallRow.end()) return false;
	const auto& entry = it->second;
	if (entry.Pending || size_t(entry.VboOffset) + 4 > g_bakeUploadedVerts ||
		size_t(entry.IboOffset) + entry.IndexCount > g_bakeUploadedIndices) return false;
	static_assert(sizeof(FFlatVertex) == 8 * sizeof(float));
	if (memcmp(currentVerts, &g_bakeVerts[entry.VboOffset], 4 * sizeof(FFlatVertex)) != 0) return false;
	*iboOffset = entry.IboOffset;
	*indexCount = entry.IndexCount;
	if (row)
	{
		if (entry.VboOffset > uint32_t(INT32_MAX) - 4) return false;
		*row = entry.Row;
	}
	return true;
}

bool GPUBakeLookupRow(seg_t* seg, int type, uint32_t* row)
{
	if (!seg || !row || !g_bakeUploaded) return false;
	const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(seg)) << 8) ^ uint64_t(type & 0xff);
	auto it = g_bakeWallRow.find(key);
	if (it == g_bakeWallRow.end()) return false;
	if (it->second.Pending) return false;
	*row = it->second.Row;
	return true;
}

CVAR(Bool, vk_gpu_driven_grid, false, 0)
CVAR(Bool, vk_gpu_driven_cull, false, 0)
CVAR(Bool, vk_gpu_driven_geometry, false, 0)
CVAR(Bool, vk_gpu_driven_candidate, false, 0)
CVAR(Bool, vk_gpu_driven_verify, false, 0)

static std::vector<uint32_t> g_gridCellStart;	// CSR offsets, size cells+1
static std::vector<uint32_t> g_gridRefs;		// primitive indices per cell
static int g_gridCellsX = 0;
static int g_gridCellsY = 0;
static std::vector<float> g_bounds;
static std::vector<FGpuMaterialRow> g_materials;
static std::vector<uint32_t> g_drawAdmission;
static bool g_bakeGridEnabled = false;

static void GPUBakeBuildGrid()
{
	g_gridCellStart.clear();
	g_gridRefs.clear();
	g_gridCellsX = g_gridCellsY = 0;
	if (g_bakeTable.empty()) return;

	constexpr float cellSize = 128.0f;
	float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
	auto primBounds = [&](const FGPUBakedPrimitive& p, float& x0, float& y0, float& x1, float& y1)
	{
		x0 = y0 = 1e30f; x1 = y1 = -1e30f;
		for (uint32_t v = 0; v < p.VertexCount && (p.VboOffset + v) < g_bakeVerts.size(); ++v)
		{
			const FFlatVertex& fv = g_bakeVerts[p.VboOffset + v];
			x0 = std::min(x0, fv.x); x1 = std::max(x1, fv.x);
			y0 = std::min(y0, fv.y); y1 = std::max(y1, fv.y);
		}
	};
	g_bounds.assign(g_bakeTable.size() * 8, 0.0f);
	for (size_t bi = 0; bi < g_bakeTable.size(); ++bi)
	{
		const FGPUBakedPrimitive& p = g_bakeTable[bi];
		float x0, y0, x1, y1;
		primBounds(p, x0, y0, x1, y1);
		if (x0 > x1) continue;
		float z0 = 1e30f, z1 = -1e30f;
		for (uint32_t v = 0; v < p.VertexCount && (p.VboOffset + v) < g_bakeVerts.size(); ++v)
		{
			z0 = std::min(z0, g_bakeVerts[p.VboOffset + v].z);
			z1 = std::max(z1, g_bakeVerts[p.VboOffset + v].z);
		}
		g_bounds[bi * 8 + 0] = x0; g_bounds[bi * 8 + 1] = z0; g_bounds[bi * 8 + 2] = y0;
		g_bounds[bi * 8 + 4] = x1; g_bounds[bi * 8 + 5] = z1; g_bounds[bi * 8 + 6] = y1;
		minX = std::min(minX, x0); maxX = std::max(maxX, x1);
		minY = std::min(minY, y0); maxY = std::max(maxY, y1);
	}
	if (screen && screen->RenderState())
		screen->RenderState()->UploadPrimitiveBounds(g_bounds.data(), g_bounds.size() * sizeof(float));

	g_materials.resize(g_bakeTable.size());
	for (size_t i = 0; i < g_bakeTable.size(); ++i)
	{
		g_materials[i].TextureIndex = g_bakeTable[i].TextureIndex;
		g_materials[i].SectorIndex = g_bakeTable[i].SectorIndex;
		g_materials[i].Plane = uint32_t(g_bakeTable[i].Plane);
		g_materials[i].Flags = 0;
	}
	if (screen && screen->RenderState())
		screen->RenderState()->UploadPrimitiveMaterials(g_materials.data(), g_materials.size() * sizeof(FGpuMaterialRow));

	if (!vk_gpu_driven_grid || minX > maxX) return;

	constexpr size_t maxCells = 1024 * 1024;
	constexpr uint32_t maxRefs = 16 * 1024 * 1024;
	const double cellsX = (double(maxX) - minX) / cellSize + 1;
	const double cellsY = (double(maxY) - minY) / cellSize + 1;
	if (!std::isfinite(cellsX) || !std::isfinite(cellsY) || cellsX < 1 || cellsY < 1 || cellsX > maxCells || cellsY > maxCells) return;
	const int cx = int(cellsX);
	const int cy = int(cellsY);
	const size_t cells = size_t(cx) * size_t(cy);
	if (cells > maxCells) return;
	std::vector<uint32_t> counts(cells, 0);

	auto forEachCell = [&](const FGPUBakedPrimitive& p, auto&& fn)
	{
		float x0, y0, x1, y1;
		primBounds(p, x0, y0, x1, y1);
		if (x0 > x1) return true;
		int a = std::clamp(int((x0 - minX) / cellSize), 0, cx - 1);
		int b = std::clamp(int((x1 - minX) / cellSize), 0, cx - 1);
		int c = std::clamp(int((y0 - minY) / cellSize), 0, cy - 1);
		int d = std::clamp(int((y1 - minY) / cellSize), 0, cy - 1);
		for (int y = c; y <= d; ++y)
			for (int x = a; x <= b; ++x)
				if (!fn(size_t(y) * size_t(cx) + size_t(x))) return false;
		return true;
	};

	uint32_t refs = 0;
	for (auto& p : g_bakeTable)
		if (!forEachCell(p, [&](size_t c) { if (refs == maxRefs) return false; ++refs; ++counts[c]; return true; })) return;
	std::vector<uint32_t> start(cells + 1, 0);
	for (size_t i = 0; i < cells; ++i) start[i + 1] = start[i] + counts[i];
	g_gridCellStart = start;
	g_gridRefs.assign(start[cells], 0);
	std::vector<uint32_t> cursor = start;
	for (uint32_t i = 0; i < g_bakeTable.size(); ++i)
		forEachCell(g_bakeTable[i], [&](size_t c) { g_gridRefs[cursor[c]++] = i; return true; });
	g_gridCellsX = cx;
	g_gridCellsY = cy;

	if (screen && screen->RenderState())
	{
		screen->RenderState()->UploadPrimitiveGrid(g_gridCellStart.data(), g_gridCellStart.size() * sizeof(uint32_t),
			g_gridRefs.data(), g_gridRefs.size() * sizeof(uint32_t));
	}

	uint32_t maxCell = 0;
	for (size_t i = 0; i < cells; ++i) maxCell = std::max(maxCell, counts[i]);
	Printf("GPU-Driven grid: cells=%dx%d refs=%zu prims=%zu maxCell=%u\n",
		cx, cy, g_gridRefs.size(), g_bakeTable.size(), maxCell);
}

void GPUBakeFinalize()
{
	if (!vk_gpu_driven_bake || !screen || !screen->RenderState()) return;
	if (!g_bakeDirty)
	{
		if (g_bakeGridEnabled != bool(vk_gpu_driven_grid))
		{
			GPUBakeBuildGrid();
			g_bakeGridEnabled = bool(vk_gpu_driven_grid);
		}
		return;
	}
	g_bakeDirty = false;
	screen->RenderState()->UploadStaticBake(g_bakeVerts.data(), g_bakeVerts.size() * sizeof(FFlatVertex),
		g_bakeIndices.data(), g_bakeIndices.size() * sizeof(uint32_t),
		g_bakeTable.data(), g_bakeTable.size() * sizeof(FGPUBakedPrimitive));
	g_bakeUploadedVerts = g_bakeVerts.size();
	g_bakeUploadedIndices = g_bakeIndices.size();
	g_bakeUploaded = true;
	for (auto& entry : g_bakeWallRow) entry.second.Pending = false;
	++g_bakeEpoch;
	GPUBakeBuildGrid();
	g_bakeGridEnabled = bool(vk_gpu_driven_grid);
	static size_t lastLogged = 0;
	if (g_bakeTable.size() != lastLogged)
	{
		lastLogged = g_bakeTable.size();
		Printf("GPU-Driven bake: primitives=%u verts=%llu vertBytes=%llu indexBytes=%llu tableBytes=%llu\n",
			(unsigned)g_bakeTable.size(), (unsigned long long)g_bakeVerts.size(),
			(unsigned long long)(g_bakeVerts.size() * sizeof(FFlatVertex)),
			(unsigned long long)(g_bakeIndices.size() * sizeof(uint32_t)),
			(unsigned long long)(g_bakeTable.size() * sizeof(FGPUBakedPrimitive)));
	}
}

bool GPUBakeWallVerts(seg_t* seg, int type, FFlatVertex* out)
{
	if (!seg) return false;
	const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(seg)) << 8) ^ uint64_t(type & 0xff);
	auto it = g_bakeWallRow.find(key);
	if (it == g_bakeWallRow.end()) return false;
	const uint32_t base = it->second.VboOffset;
	if (size_t(base) + 4 > g_bakeVerts.size()) return false;
	for (int i = 0; i < 4; ++i) out[i] = g_bakeVerts[base + i];
	return true;
}

// [Karto] CPU twin of the compute shader's AABB-vs-frustum test; keep the plane convention identical.
static bool AabbInFrustum(const float* planes, const float* b)
{
	for (int i = 0; i < 6; ++i)
	{
		const float* pl = planes + i * 4;
		const float px = pl[0] >= 0.0f ? b[4] : b[0];
		const float py = pl[1] >= 0.0f ? b[5] : b[1];
		const float pz = pl[2] >= 0.0f ? b[6] : b[2];
		if (pl[0] * px + pl[1] * py + pl[2] * pz + pl[3] < 0.0f) return false;
	}
	return true;
}

void GPUBakeBeginView()
{
	g_drawAdmission.assign(g_bounds.size() / 8, 0);
}

void GPUBakeAdmitWall(seg_t* seg, int type, const FFlatVertex* currentVerts)
{
	uint32_t offset = 0, count = 0;
	if (!GPUBakeLookupWall(seg, type, currentVerts, &offset, &count)) return;
	const uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(seg)) << 8) ^ uint64_t(type & 0xff);
	const auto& entry = g_bakeWallRow.at(key);
	if (entry.Row < g_drawAdmission.size() && entry.VboOffset <= uint32_t(INT32_MAX) - 4) g_drawAdmission[entry.Row] = 1;
}

void GPUBakeRunCull(const float* planes24, bool forDraw, const uint32_t* rows, uint32_t count)
{
	if (!vk_gpu_driven_cull || !g_bakeUploaded || g_bounds.empty() || !screen || !screen->RenderState()) return;
	const uint32_t prims = uint32_t(g_bounds.size() / 8);
	uint32_t expected = 0;
	const bool verify = vk_gpu_driven_verify || vk_gpu_driven_debug;
	if (verify)
	{
		if (rows)
		{
			for (uint32_t i = 0; i < count; ++i)
				if (rows[i] < prims && AabbInFrustum(planes24, &g_bounds[size_t(rows[i]) * 8])) expected++;
		}
		else for (uint32_t i = 0; i < prims; ++i)
			if (AabbInFrustum(planes24, &g_bounds[size_t(i) * 8])) expected++;
	}

	if (rows)
		screen->RenderState()->UploadGpuCullAdmission(rows, size_t(count) * sizeof(uint32_t));
	else if (forDraw)
	{
		if (g_drawAdmission.size() != prims) return;
		screen->RenderState()->UploadGpuCullAdmission(g_drawAdmission.data(), g_drawAdmission.size() * sizeof(uint32_t));
	}
	screen->RenderState()->SetGpuCullPlanes(planes24, prims, expected, verify, forDraw, rows ? count : 0);
}

static void GPUBakeFlats(FFlatVertexBuffer* fvb, TArray<sector_t>& sectors)
{
	GPUClearPersistentWalls();
	++g_bakeEpoch;
	g_bakeVerts.clear(); g_bakeIndices.clear(); g_bakeTable.clear(); g_bakeWallKeys.clear(); g_bakeWallRow.clear(); g_bakeDirty = false; g_bakeUploaded = false;
	g_bounds.clear(); g_materials.clear(); g_drawAdmission.clear();
	g_bakeUploadedVerts = g_bakeUploadedIndices = 0;
	g_bakeGridEnabled = false;
	if (!vk_gpu_driven_bake || sectors.Size() == 0 || !screen->RenderState()) return;
	g_bakeVerts.assign(fvb->vbo_shadowdata.Data(), fvb->vbo_shadowdata.Data() + fvb->mIndex);
	g_bakeIndices.assign(fvb->ibo_data.Data(), fvb->ibo_data.Data() + fvb->ibo_data.Size());
	// Flats: reference the existing static ranges.
	for (auto& sector : sectors)
	{
		if (!CensusOrdinary(&sector) || sector.ibocount <= 0) continue;
		for (int plane = 0; plane < 2; ++plane)
		{
			FGPUBakedPrimitive primitive = {};
			primitive.VboOffset = uint32_t(sector.vboindex[plane]);
			primitive.VertexCount = uint32_t(sector.vbocount[plane]);
			primitive.IboOffset = uint32_t(sector.iboindex[plane]);
			primitive.IndexCount = uint32_t(sector.ibocount);
			primitive.TextureIndex = uint32_t(sector.GetTexture(plane).GetIndex());
			primitive.SectorIndex = uint32_t(sector.Index());
			primitive.Plane = plane;
			g_bakeTable.push_back(primitive);
		}
	}
	// Wall primitive rows are appended by GPUBakeRecordWall during vertex generation.
	g_bakeDirty = true;
}

void FFlatVertexBuffer::EnsureRetainedFlatIDs(TArray<sector_t>& sectors)
{
	if (RetainedFlatIDBuffer || !screen->IsVulkan() || !screen->BuffersArePersistent() ||
		sectors.Size() >= 16384 || !mIndex) return;
	const uint64_t started = I_nsTime();
	std::vector<float> ids(mIndex, 0.f);
	for (auto& sector : sectors)
	{
		for (int plane = 0; plane < 2; ++plane)
		{
			const float id = float(32768 + sector.Index() * 2 + plane);
			for (int vertex = sector.vboindex[plane]; vertex < sector.vboindex[plane] + sector.vbocount[plane]; ++vertex)
				ids[vertex] = id;
		}
	}
	auto buffer = std::unique_ptr<IVertexBuffer>(screen->CreateVertexBuffer());
	buffer->SetData(ids.size() * sizeof(float), ids.data(), BufferUsageType::Static);
	const FVertexBufferAttribute format[] = { { 0, VATTR_SURFACEID, VFmt_Float, 0 } };
	buffer->SetFormat(1, 1, sizeof(float), format);
	RetainedFlatMetadataBytes = ids.size() * sizeof(float);
	RetainedFlatIDBuffer = std::move(buffer);
	RetainedFlatSetupNS = I_nsTime() - started;
}
