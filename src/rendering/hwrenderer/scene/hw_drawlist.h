/*
** hw_drawlist.h
**
** The main container type for draw items.
**
**---------------------------------------------------------------------------
**
** Copyright 2002-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include "memarena.h"
#include <cstdint>

extern FMemArena RenderDataAllocator;
void ResetRenderDataAllocator();
struct HWDrawInfo;
class HWWall;
class HWFlat;
class HWSprite;
class FRenderState;

//==========================================================================
//
// Intermediate struct to link one draw item into a draw list
//
// unfortunately this struct must not contain pointers because
// the arrays may be reallocated!
//
//==========================================================================

enum HWDrawItemType
{
	DrawType_WALL,
	DrawType_FLAT,
	DrawType_SPRITE,
};

struct HWDrawItem
{
	HWDrawItemType rendertype;
	int index;

	HWDrawItem() = default; // we need this for dynamic arrays.
	HWDrawItem(HWDrawItemType _rendertype,int _index) : rendertype(_rendertype),index(_index) {}
};

struct SortNode
{
	int itemindex;
	SortNode * parent;
	SortNode * next;		// unsorted successor
	SortNode * left;		// left side of this node
	SortNode * equal;		// equal to this node
	SortNode * right;		// right side of this node


	void UnlinkFromChain();
	void Link(SortNode * hook);
	void AddToEqual(SortNode * newnode);
	void AddToLeft (SortNode * newnode);
	void AddToRight(SortNode * newnode);
};

//==========================================================================
//
// One draw list. This contains all info for one type of rendering data
//
//==========================================================================

struct HWOwnedWallDraw
{
	HWWall* Wall = nullptr;
	uint64_t Version = 0;
	unsigned Order = 0;
	unsigned Part = 0;
};

struct HWDrawList
{
	//private:
	TArray<HWWall*> walls;
	TArray<HWFlat*> flats;
	TArray<HWSprite*> sprites;
	TArray<HWDrawItem> drawitems;
	TArray<HWOwnedWallDraw> OwnedWalls;
	TArray<HWOwnedWallDraw> FallbackWalls;
	TArray<HWOwnedWallDraw> VisibleWalls;
	bool UseOwnedOrder = false;
	int SortNodeStart;
	float SortZ;
	SortNode * sorted;
	bool reverseSort;

public:
	HWDrawList()
	{
		next=NULL;
		SortNodeStart=-1;
		sorted=NULL;
	}

	~HWDrawList()
	{
		Reset();
	}

	unsigned int Size()
	{
		return drawitems.Size();
	}

	HWWall *NewWall(HWWall* retained = nullptr);
	HWFlat *NewFlat();
	HWSprite *NewSprite();
	void Reset();
	void PrepareOwnedWalls(HWDrawInfo* di);
	void SortWalls();
	void SortSolidSprites();
	void DrawAdditiveSprites(HWDrawInfo *di, FRenderState &state);
	void DrawParticleBuckets(HWDrawInfo *di, FRenderState &state);
	void DrawSolidSprites(HWDrawInfo *di, FRenderState &state);
	void SortFlats();


	void MakeSortList();
	SortNode * FindSortPlane(SortNode * head);
	SortNode * FindSortWall(SortNode * head);
	void SortPlaneIntoPlane(SortNode * head,SortNode * sort);
	void SortWallIntoPlane(HWDrawInfo* di, SortNode * head,SortNode * sort);
	void SortSpriteIntoPlane(SortNode * head,SortNode * sort);
	void SortWallIntoWall(HWDrawInfo *di, SortNode * head,SortNode * sort);
	void SortSpriteIntoWall(HWDrawInfo *di, SortNode * head,SortNode * sort);
	int CompareSprites(SortNode * a,SortNode * b);
	SortNode * SortSpriteList(SortNode * head);
	SortNode * DoSort(HWDrawInfo *di, SortNode * head);
	void Sort(HWDrawInfo *di);

	void DoDraw(HWDrawInfo *di, FRenderState &state, bool translucent, int i);
	void Draw(HWDrawInfo *di, FRenderState &state, bool translucent);
	void PrepareGpuDrivenWalls();
	bool DrawGpuDrivenWalls(HWDrawInfo* di, FRenderState& state);
	void DrawWalls(HWDrawInfo *di, FRenderState &state, bool translucent);
	void DrawFlats(HWDrawInfo *di, FRenderState &state, bool translucent);

	void DrawSorted(HWDrawInfo *di, FRenderState &state, SortNode * head);
	void DrawSorted(HWDrawInfo *di, FRenderState &state);

	HWDrawList * next;
} ;
