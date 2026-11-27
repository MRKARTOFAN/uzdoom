/*
** hw_drawlist.cpp
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

#include "actor.h"
#include "flatvertices.h"
#include "g_levellocals.h"
#include "hw_clock.h"
#include "hw_drawinfo.h"
#include "hw_renderstate.h"
#include "hw_vertexbuilder.h"
#include "hw_walldispatcher.h"
#include "hwrenderer/scene/hw_drawlist.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_spriterecord.h"
#include "hwrenderer/scene/hw_lighting.h"
#include "r_sky.h"
#include "r_utility.h"
#include "c_dispatch.h"
#include "printf.h"
#include "d_framestages.h"
#include "doomstat.h"
#include "d_net.h"

FMemArena RenderDataAllocator(1024*1024);	// Use large blocks to reduce allocation time.

CVAR(Bool, r_sprite_solidruns, true, 0)
EXTERN_CVAR(Bool, vk_worldruns)
EXTERN_CVAR(Int, vk_retainedworld)
EXTERN_CVAR(Bool, vk_drawreuse)
EXTERN_CVAR(Bool, gl_seamless)
EXTERN_CVAR(Bool, vk_gpu_driven_resident)
EXTERN_CVAR(Bool, vk_gpu_driven_persistent)
EXTERN_CVAR(Bool, vk_gpu_driven_bakerow)

void ResetRenderDataAllocator()
{
	RenderDataAllocator.FreeAll();
}

//==========================================================================
//
//
//
//==========================================================================
class StaticSortNodeArray : public TDeletingArray<SortNode*>
{
	unsigned usecount;
public:
	unsigned Size() { return usecount; }
	void Clear() { usecount=0; }
	void Release(int start) { usecount=start; }
	SortNode * GetNew();
};


SortNode * StaticSortNodeArray::GetNew()
{
	if (usecount==TArray<SortNode*>::Size())
	{
		Push(new SortNode);
	}
	return operator[](usecount++);
}


static StaticSortNodeArray SortNodes;

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::Reset()
{
	OwnedWalls.Clear();
	FallbackWalls.Clear();
	VisibleWalls.Clear();
	UseOwnedOrder = false;
	if (sorted) SortNodes.Release(SortNodeStart);
	sorted=NULL;
	walls.Clear();
	flats.Clear();
	sprites.Clear();
	drawitems.Clear();
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::UnlinkFromChain()
{
	if (parent) parent->next=next;
	if (next) next->parent=parent;
	parent=next=NULL;
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::Link(SortNode * hook)
{
	if (hook)
	{
		parent=hook->parent;
		hook->parent=this;
	}
	next=hook;
	if (parent) parent->next=this;
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::AddToEqual(SortNode *child)
{
	child->UnlinkFromChain();
	child->equal=equal;
	equal=child;
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::AddToLeft(SortNode * child)
{
	child->UnlinkFromChain();
	child->Link(left);
	left=child;
}

//==========================================================================
//
//
//
//==========================================================================
inline void SortNode::AddToRight(SortNode * child)
{
	child->UnlinkFromChain();
	child->Link(right);
	right=child;
}


//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::MakeSortList()
{
	SortNode * p, * n, * c;
	unsigned i;

	SortNodeStart=SortNodes.Size();
	p=NULL;
	n=SortNodes.GetNew();
	for(i=0;i<drawitems.Size();i++)
	{
		n->itemindex=(int)i;
		n->left=n->equal=n->right=NULL;
		n->parent=p;
		p=n;
		if (i!=drawitems.Size()-1)
		{
			c=SortNodes.GetNew();
			n->next=c;
			n=c;
		}
		else
		{
			n->next=NULL;
		}
	}
}


//==========================================================================
//
//
//
//==========================================================================
SortNode * HWDrawList::FindSortPlane(SortNode * head)
{
	while (head->next && drawitems[head->itemindex].rendertype!=DrawType_FLAT)
		head=head->next;
	if (drawitems[head->itemindex].rendertype==DrawType_FLAT) return head;
	return NULL;
}


//==========================================================================
//
//
//
//==========================================================================
SortNode * HWDrawList::FindSortWall(SortNode * head)
{
	float farthest = -FLT_MAX;
	float nearest = FLT_MAX;
	SortNode * best = NULL;
	SortNode * node = head;
	float bestdist = FLT_MAX;

	while (node)
	{
		HWDrawItem * it = &drawitems[node->itemindex];
		if (it->rendertype == DrawType_WALL)
		{
			float d = walls[it->index]->ViewDistance;
			if (d > farthest) farthest = d;
			if (d < nearest) nearest = d;
		}
		node = node->next;
	}
	if (farthest == INT_MIN) return NULL;
	node = head;
	farthest = (farthest + nearest) / 2;
	while (node)
	{
		HWDrawItem * it = &drawitems[node->itemindex];
		if (it->rendertype == DrawType_WALL)
		{
			float di = fabsf(walls[it->index]->ViewDistance - farthest);
			if (!best || di < bestdist)
			{
				best = node;
				bestdist = di;
			}
		}
		node = node->next;
	}
	return best;
}

//==========================================================================
//
// Note: sloped planes are a huge problem...
//
//==========================================================================
void HWDrawList::SortPlaneIntoPlane(SortNode * head,SortNode * sort)
{
	HWFlat * fh= flats[drawitems[head->itemindex].index];
	HWFlat * fs= flats[drawitems[sort->itemindex].index];

	if (fh->z==fs->z)
		head->AddToEqual(sort);
	else if ( (fh->z<fs->z && fh->ceiling) || (fh->z>fs->z && !fh->ceiling))
		head->AddToLeft(sort);
	else
		head->AddToRight(sort);
}


//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::SortWallIntoPlane(HWDrawInfo* di, SortNode * head, SortNode * sort)
{
	HWFlat * fh = flats[drawitems[head->itemindex].index];
	HWWall * ws = walls[drawitems[sort->itemindex].index];

	bool ceiling = fh->z > SortZ;

	if ((ws->ztop[0] > fh->z || ws->ztop[1] > fh->z) && (ws->zbottom[0] < fh->z || ws->zbottom[1] < fh->z))
	{
		// We have to split this wall!

		HWWall *w = NewWall();
		*w = *ws;

		// Splitting is done in the shader with clip planes, if available
		if (screen->hwcaps & RFL_NO_CLIP_PLANES)
		{
			ws->vertcount = 0;	// invalidate current vertices.
			float newtexv = ws->tcs[HWWall::UPLFT].v + ((ws->tcs[HWWall::LOLFT].v - ws->tcs[HWWall::UPLFT].v) / (ws->zbottom[0] - ws->ztop[0])) * (fh->z - ws->ztop[0]);
			float newlmv = ws->lightuv[HWWall::UPLFT].v + ((ws->lightuv[HWWall::LOLFT].v - ws->lightuv[HWWall::UPLFT].v) / (ws->zbottom[0] - ws->ztop[0])) * (fh->z - ws->ztop[0]);

			// I make the very big assumption here that translucent walls in sloped sectors
			// and 3D-floors never coexist in the same level - If that were the case this
			// code would become extremely more complicated.
			if (!ceiling)
			{
				ws->ztop[1] = w->zbottom[1] = ws->ztop[0] = w->zbottom[0] = fh->z;
				ws->tcs[HWWall::UPRGT].v = w->tcs[HWWall::LORGT].v = ws->tcs[HWWall::UPLFT].v = w->tcs[HWWall::LOLFT].v = newtexv;
				ws->lightuv[HWWall::UPRGT].v = w->lightuv[HWWall::LORGT].v = ws->lightuv[HWWall::UPLFT].v = w->lightuv[HWWall::LOLFT].v = newlmv;
			}
			else
			{
				w->ztop[1] = ws->zbottom[1] = w->ztop[0] = ws->zbottom[0] = fh->z;
				w->tcs[HWWall::UPLFT].v = ws->tcs[HWWall::LOLFT].v = w->tcs[HWWall::UPRGT].v = ws->tcs[HWWall::LORGT].v = newtexv;
				w->lightuv[HWWall::UPLFT].v = ws->lightuv[HWWall::LOLFT].v = w->lightuv[HWWall::UPRGT].v = ws->lightuv[HWWall::LORGT].v = newlmv;
			}
			w->MakeVertices(false);
			ws->MakeVertices(false);
		}

		SortNode * sort2 = SortNodes.GetNew();
		memset(sort2, 0, sizeof(SortNode));
		sort2->itemindex = drawitems.Size() - 1;

		head->AddToLeft(sort);
		head->AddToRight(sort2);
	}
	else if ((ws->zbottom[0] < fh->z && !ceiling) || (ws->ztop[0] > fh->z && ceiling))	// completely on the left side
	{
		head->AddToLeft(sort);
	}
	else
	{
		head->AddToRight(sort);
	}

}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::SortSpriteIntoPlane(SortNode * head, SortNode * sort)
{
	HWFlat * fh = flats[drawitems[head->itemindex].index];
	HWSprite * ss = sprites[drawitems[sort->itemindex].index];

	bool ceiling = fh->z > SortZ;

	auto hiz = ss->z1 > ss->z2 ? ss->z1 : ss->z2;
	auto loz = ss->z1 < ss->z2 ? ss->z1 : ss->z2;

	if ((hiz > fh->z && loz < fh->z) || ss->modelframe)
	{
		// We have to split this sprite
		HWSprite *s = NewSprite();
		*s = *ss;

		// Splitting is done in the shader with clip planes, if available.
		// The fallback here only really works for non-y-billboarded sprites.
		if (screen->hwcaps & RFL_NO_CLIP_PLANES)
		{
			float newtexv = ss->vt + ((ss->vb - ss->vt) / (ss->z2 - ss->z1))*(fh->z - ss->z1);

			if (!ceiling)
			{
				ss->z1 = s->z2 = fh->z;
				ss->vt = s->vb = newtexv;
			}
			else
			{
				s->z1 = ss->z2 = fh->z;
				s->vt = ss->vb = newtexv;
			}
		}

		SortNode * sort2 = SortNodes.GetNew();
		memset(sort2, 0, sizeof(SortNode));
		sort2->itemindex = drawitems.Size() - 1;

		head->AddToLeft(sort);
		head->AddToRight(sort2);
	}
	else if ((ss->z2<fh->z && !ceiling) || (ss->z1>fh->z && ceiling))	// completely on the left side
	{
		head->AddToLeft(sort);
	}
	else
	{
		head->AddToRight(sort);
	}
}

//==========================================================================
//
//
//
//==========================================================================
#define MIN_EQ (0.0005f)

// Lines start-end and fdiv must intersect.
inline double CalcIntersectionVertex(HWWall *w1, HWWall * w2)
{
	float ax = w1->glseg.x1, ay = w1->glseg.y1;
	float bx = w1->glseg.x2, by = w1->glseg.y2;
	float cx = w2->glseg.x1, cy = w2->glseg.y1;
	float dx = w2->glseg.x2, dy = w2->glseg.y2;
	return ((ay - cy)*(dx - cx) - (ax - cx)*(dy - cy)) / ((bx - ax)*(dy - cy) - (by - ay)*(dx - cx));
}

void HWDrawList::SortWallIntoWall(HWDrawInfo *di, SortNode * head,SortNode * sort)
{
	HWWall * wh= walls[drawitems[head->itemindex].index];
	HWWall * ws= walls[drawitems[sort->itemindex].index];
	float v1=wh->PointOnSide(ws->glseg.x1,ws->glseg.y1);
	float v2=wh->PointOnSide(ws->glseg.x2,ws->glseg.y2);

	if (fabs(v1)<MIN_EQ && fabs(v2)<MIN_EQ)
	{
		if (ws->type==RENDERWALL_FOGBOUNDARY && wh->type!=RENDERWALL_FOGBOUNDARY)
		{
			head->AddToRight(sort);
		}
		else if (ws->type!=RENDERWALL_FOGBOUNDARY && wh->type==RENDERWALL_FOGBOUNDARY)
		{
			head->AddToLeft(sort);
		}
		else
		{
			head->AddToEqual(sort);
		}
	}
	else if (v1<MIN_EQ && v2<MIN_EQ)
	{
		head->AddToLeft(sort);
	}
	else if (v1>-MIN_EQ && v2>-MIN_EQ)
	{
		head->AddToRight(sort);
	}
	else
	{
		double r = CalcIntersectionVertex(ws, wh);

		float ix=(float)(ws->glseg.x1+r*(ws->glseg.x2-ws->glseg.x1));
		float iy=(float)(ws->glseg.y1+r*(ws->glseg.y2-ws->glseg.y1));
		float iu=(float)(ws->tcs[HWWall::UPLFT].u + r * (ws->tcs[HWWall::UPRGT].u - ws->tcs[HWWall::UPLFT].u));
		float ilmu=(float)(ws->lightuv[HWWall::UPLFT].u + r * (ws->lightuv[HWWall::UPRGT].u - ws->lightuv[HWWall::UPLFT].u));
		float izt=(float)(ws->ztop[0]+r*(ws->ztop[1]-ws->ztop[0]));
		float izb=(float)(ws->zbottom[0]+r*(ws->zbottom[1]-ws->zbottom[0]));

		ws->vertcount = 0;	// invalidate current vertices.
		HWWall *w= NewWall();
		*w = *ws;

		w->glseg.x1=ws->glseg.x2=ix;
		w->glseg.y1=ws->glseg.y2=iy;
		w->glseg.fracleft = ws->glseg.fracright = ws->glseg.fracleft + r*(ws->glseg.fracright - ws->glseg.fracleft);
		w->ztop[0]=ws->ztop[1]=izt;
		w->zbottom[0]=ws->zbottom[1]=izb;
		w->tcs[HWWall::LOLFT].u = w->tcs[HWWall::UPLFT].u = ws->tcs[HWWall::LORGT].u = ws->tcs[HWWall::UPRGT].u = iu;
		w->lightuv[HWWall::LOLFT].u = w->lightuv[HWWall::UPLFT].u = ws->lightuv[HWWall::LORGT].u = ws->lightuv[HWWall::UPRGT].u = iu;
		ws->MakeVertices(false);
		w->MakeVertices(false);

		SortNode * sort2=SortNodes.GetNew();
		memset(sort2,0,sizeof(SortNode));
		sort2->itemindex=drawitems.Size()-1;

		if (v1>0)
		{
			head->AddToLeft(sort2);
			head->AddToRight(sort);
		}
		else
		{
			head->AddToLeft(sort);
			head->AddToRight(sort2);
		}
	}
}


//==========================================================================
//
//
//
//==========================================================================
EXTERN_CVAR(Int, gl_billboard_mode)
EXTERN_CVAR(Bool, gl_billboard_faces_camera)
EXTERN_CVAR(Bool, hw_force_cambbpref)
EXTERN_CVAR(Bool, gl_billboard_particles)
EXTERN_CVAR(Int, gl_particles_style)

inline double CalcIntersectionVertex(HWSprite *s, HWWall * w2)
{
	float ax = s->x1, ay = s->y1;
	float bx = s->x2, by = s->y2;
	float cx = w2->glseg.x1, cy = w2->glseg.y1;
	float dx = w2->glseg.x2, dy = w2->glseg.y2;
	return ((ay - cy)*(dx - cx) - (ax - cx)*(dy - cy)) / ((bx - ax)*(dy - cy) - (by - ay)*(dx - cx));
}

void HWDrawList::SortSpriteIntoWall(HWDrawInfo *di, SortNode * head,SortNode * sort)
{
	HWWall *wh= walls[drawitems[head->itemindex].index];
	HWSprite * ss= sprites[drawitems[sort->itemindex].index];

	float v1 = wh->PointOnSide(ss->x1, ss->y1);
	float v2 = wh->PointOnSide(ss->x2, ss->y2);

	if (fabs(v1)<MIN_EQ && fabs(v2)<MIN_EQ)
	{
		if (wh->type==RENDERWALL_FOGBOUNDARY)
		{
			head->AddToLeft(sort);
		}
		else
		{
			head->AddToEqual(sort);
		}
	}
	else if (v1<MIN_EQ && v2<MIN_EQ)
	{
		head->AddToLeft(sort);
	}
	else if (v1>-MIN_EQ && v2>-MIN_EQ)
	{
		head->AddToRight(sort);
	}
	else
	{
		const ActorRenderFlags ssflags = ss->actor ? ss->actor->renderflags : ss->record ? ss->record->renderflags : ActorRenderFlags::FromInt(0);
		const bool drawWithXYBillboard = ((ss->particle && gl_billboard_particles) || (!(ssflags & RF_FORCEYBILLBOARD)
			&& (gl_billboard_mode == 1 || (ssflags & RF_FORCEXYBILLBOARD))));

		const bool drawBillboardFacingCamera = hw_force_cambbpref ? gl_billboard_faces_camera :
			(gl_billboard_faces_camera && (ss->actor && !(ss->actor->renderflags2 & RF2_BILLBOARDNOFACECAMERA)))
			|| (ss->actor && ss->actor->renderflags2 & RF2_BILLBOARDFACECAMERA);

		// [Nash] has +ROLLSPRITE
		const bool rotated = !!(ssflags & (RF_ROLLSPRITE | RF_WALLSPRITE | RF_FLATSPRITE));

		// cannot sort them at the moment. This requires more complex splitting.
		if (drawWithXYBillboard || drawBillboardFacingCamera || rotated)
		{
			float v1 = wh->PointOnSide(ss->x, ss->y);
			if (v1 < 0)
			{
				head->AddToLeft(sort);
			}
			else
			{
				head->AddToRight(sort);
			}
			return;
		}
		double r=CalcIntersectionVertex(ss, wh);

		float ix=(float)(ss->x1 + r * (ss->x2-ss->x1));
		float iy=(float)(ss->y1 + r * (ss->y2-ss->y1));
		float iu=(float)(ss->ul + r * (ss->ur-ss->ul));

		HWSprite *s = NewSprite();
		*s = *ss;

		s->x1=ss->x2=ix;
		s->y1=ss->y2=iy;
		s->ul=ss->ur=iu;

		SortNode * sort2=SortNodes.GetNew();
		memset(sort2,0,sizeof(SortNode));
		sort2->itemindex=drawitems.Size()-1;

		if (v1>0)
		{
			head->AddToLeft(sort2);
			head->AddToRight(sort);
		}
		else
		{
			head->AddToLeft(sort);
			head->AddToRight(sort2);
		}
		if (screen->BuffersArePersistent())
		{
			s->vertexindex = ss->vertexindex = -1;
		}
		else
		{
			s->CreateVertices(di);
			ss->CreateVertices(di);
		}

	}
}


//==========================================================================
//
//
//
//==========================================================================

inline int HWDrawList::CompareSprites(SortNode * a,SortNode * b)
{
	HWSprite * s1= sprites[drawitems[a->itemindex].index];
	HWSprite * s2= sprites[drawitems[b->itemindex].index];

	if (s1->depth < s2->depth) return 1;
	if (s1->depth > s2->depth) return -1;
	return reverseSort? s2->index-s1->index : s1->index-s2->index;
}

//==========================================================================
//
//
//
//==========================================================================
SortNode * HWDrawList::SortSpriteList(SortNode * head)
{
	SortNode * n;
	int count;
	unsigned i;

	static TArray<SortNode*> sortspritelist;

	SortNode * parent=head->parent;

	sortspritelist.Clear();
	for(count=0,n=head;n;n=n->next) sortspritelist.Push(n);
	std::stable_sort(sortspritelist.begin(), sortspritelist.end(), [this](SortNode *a, SortNode *b)
	{
		return CompareSprites(a, b) < 0;
	});

	for(i=0;i<sortspritelist.Size();i++)
	{
		sortspritelist[i]->next=NULL;
		if (parent) parent->equal=sortspritelist[i];
		parent=sortspritelist[i];
	}
	return sortspritelist[0];
}

//==========================================================================
//
//
//
//==========================================================================
SortNode * HWDrawList::DoSort(HWDrawInfo *di, SortNode * head)
{
	SortNode * node, * sn, * next;

	sn=FindSortPlane(head);
	if (sn)
	{
		if (sn==head) head=head->next;
		sn->UnlinkFromChain();
		node=head;
		head=sn;
		while (node)
		{
			next=node->next;
			switch(drawitems[node->itemindex].rendertype)
			{
			case DrawType_FLAT:
				SortPlaneIntoPlane(head,node);
				break;

			case DrawType_WALL:
				SortWallIntoPlane(di,head,node);
				break;

			case DrawType_SPRITE:
				SortSpriteIntoPlane(head,node);
				break;
			}
			node=next;
		}
	}
	else
	{
		sn=FindSortWall(head);
		if (sn)
		{
			if (sn==head) head=head->next;
			sn->UnlinkFromChain();
			node=head;
			head=sn;
			while (node)
			{
				next=node->next;
				switch(drawitems[node->itemindex].rendertype)
				{
				case DrawType_WALL:
					SortWallIntoWall(di, head,node);
					break;

				case DrawType_SPRITE:
					SortSpriteIntoWall(di, head, node);
					break;

				case DrawType_FLAT: break;
				}
				node=next;
			}
		}
		else
		{
			return SortSpriteList(head);
		}
	}
	if (head->left) head->left=DoSort(di, head->left);
	if (head->right) head->right=DoSort(di, head->right);
	return sn;
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::Sort(HWDrawInfo *di)
{
	reverseSort = !!(di->Level->i_compatflags & COMPATF_SPRITESORT);
	SortZ = di->Viewpoint.Pos.Z;
	MakeSortList();
	sorted = DoSort(di, SortNodes[SortNodeStart]);
}

//==========================================================================
//
// Sorting the drawitems first by texture and then by light level
//
//==========================================================================

void HWDrawList::PrepareOwnedWalls(HWDrawInfo* di)
{
	HWSceneScopeTimer timer(SceneMainSurfaceProfile.OwnedVisibleAssemblyNS);
	UseOwnedOrder = true;
	VisibleWalls.Clear();
	{
		HWSceneScopeTimer orderTimer(SceneMainSurfaceProfile.OwnedOrderNS);
		unsigned fallback = 0, owned = 0;
		while (fallback < FallbackWalls.Size() && owned < OwnedWalls.Size())
		{
			const auto& ordinary = FallbackWalls[fallback];
			const auto& selected = OwnedWalls[owned];
			const unsigned order = ordinary.Order ? ordinary.Order : di->OwnedNextOrder;
			if (order < selected.Order || (order == selected.Order && ordinary.Part <= selected.Part))
				VisibleWalls.Push(FallbackWalls[fallback++]);
			else VisibleWalls.Push(OwnedWalls[owned++]);
		}
		while (fallback < FallbackWalls.Size()) VisibleWalls.Push(FallbackWalls[fallback++]);
		while (owned < OwnedWalls.Size()) VisibleWalls.Push(OwnedWalls[owned++]);
	}
	if (SceneProfiling)
	{
		const uint64_t bytes = (uint64_t(OwnedWalls.Max()) + FallbackWalls.Max() + VisibleWalls.Max()) * sizeof(HWOwnedWallDraw);
		SceneMainSurfaceProfile.OwnedSubmissionBytes = std::max(SceneMainSurfaceProfile.OwnedSubmissionBytes, bytes);
	}
}

// [Karto] Solid sprites are opaque and depth-tested, so order does not change the image: group equal texture and
// lighting for long runs. The draw-item index breaks ties to keep the order stable per frame.
void HWDrawList::SortSolidSprites()
{
	if (drawitems.Size() < 2) return;
	std::sort(drawitems.begin(), drawitems.end(), [this](const HWDrawItem& a, const HWDrawItem& b) -> bool
	{
		const HWSprite* s1 = sprites[a.index];
		const HWSprite* s2 = sprites[b.index];
		if (s1->texture != s2->texture) return s1->texture < s2->texture;
		if (s1->translation != s2->translation) return s1->translation.index() < s2->translation.index();
		if (s1->lightlevel != s2->lightlevel) return s1->lightlevel < s2->lightlevel;
		if (s1->fullbright != s2->fullbright) return s1->fullbright < s2->fullbright;
		if (s1->Colormap.LightColor.d != s2->Colormap.LightColor.d) return s1->Colormap.LightColor.d < s2->Colormap.LightColor.d;
		if (s1->Colormap.FadeColor.d != s2->Colormap.FadeColor.d) return s1->Colormap.FadeColor.d < s2->Colormap.FadeColor.d;
		return a.index < b.index;
	});
}

// [Karto] Additive sprites with depth writes off sum the same in any order, so they are grouped by draw key; the
// draw-item index breaks ties to keep the order stable per frame.
void HWDrawList::DrawAdditiveSprites(HWDrawInfo *di, FRenderState &state)
{
	if (drawitems.Size() == 0) return;
	std::sort(drawitems.begin(), drawitems.end(), [this](const HWDrawItem& a, const HWDrawItem& b) -> bool
	{
		const HWSprite* s1 = sprites[a.index];
		const HWSprite* s2 = sprites[b.index];
		if (s1->texture != s2->texture) return s1->texture < s2->texture;
		if (s1->RenderStyle.AsDWORD != s2->RenderStyle.AsDWORD) return s1->RenderStyle.AsDWORD < s2->RenderStyle.AsDWORD;
		if (s1->translation != s2->translation) return s1->translation.index() < s2->translation.index();
		if (s1->lightlevel != s2->lightlevel) return s1->lightlevel < s2->lightlevel;
		if (s1->fullbright != s2->fullbright) return s1->fullbright < s2->fullbright;
		if (s1->trans != s2->trans) return s1->trans < s2->trans;
		if (s1->Colormap.LightColor.d != s2->Colormap.LightColor.d) return s1->Colormap.LightColor.d < s2->Colormap.LightColor.d;
		if (s1->Colormap.FadeColor.d != s2->Colormap.FadeColor.d) return s1->Colormap.FadeColor.d < s2->Colormap.FadeColor.d;
		return a.index < b.index;
	});
	HWSpriteRunKey run, next;
	bool open = false;
	for (auto& item : drawitems)
	{
		HWSprite* s = sprites[item.index];
		next.Set(di, *s);
		if (open && !next.dynlight && next == run)
		{
			s->DrawRunRepeat(di, state);
			continue;
		}
		if (open) HWSprite::FinishAdditiveRun(state);
		run = next;
		s->DrawSprite(di, state, true, true);
		open = true;
	}
	HWSprite::FinishAdditiveRun(state);
}

// [Karto] polyoffset is known only once CreateVertices ran, so a sprite clipped into the floor still joins; its depth
// bias is cleared right after its quad.
void HWDrawList::DrawSolidSprites(HWDrawInfo *di, FRenderState &state)
{
	HWSpriteRunKey run, next;
	bool open = false;
	const bool persistent = screen->BuffersArePersistent();
	for (auto& item : drawitems)
	{
		HWSprite* s = sprites[item.index];
		const bool runnable = persistent && (s->record || s->actor) && s->particle == nullptr && s->modelframe == nullptr &&
			s->lightlist == nullptr && s->topclip == LARGE_VALUE && s->bottomclip == -LARGE_VALUE &&
			s->RenderStyle.BlendOp != STYLEOP_RevSub && s->RenderStyle.BlendOp != STYLEOP_Sub;
		if (runnable)
		{
			next.Set(di, *s);
			if (!next.dynlight)
			{
				if (open && next == run)
				{
					s->DrawRunRepeat(di, state);
					if (s->polyoffset) state.ClearDepthBias();
					continue;
				}
				if (open) HWSprite::FinishSolidRun(state);
				run = next;
				s->DrawSprite(di, state, false, true);
				if (s->polyoffset) state.ClearDepthBias();
				open = true;
				continue;
			}
		}
		if (open)
		{
			HWSprite::FinishSolidRun(state);
			open = false;
		}
		s->DrawSprite(di, state, false);
	}
	if (open) HWSprite::FinishSolidRun(state);
}

// [Karto] A bucket's first particle sets the state for the rest. That matches the shader only while nothing applies
// between, so desaturation and per-particle dynamic light go elsewhere (AddSprite).
namespace
{
	struct FParticleVertex
	{
		float x, z, y, u, v, lu, lv, lindex;
		float r, g, b, a;
	};

	struct FParticleKey
	{
		FGameTexture* texture;
		FColormap Colormap;
		uint32_t style, sectorAdd;
		int lightlevel, OverrideShader;
		uint8_t foglevel, hw_styleflags;
		bool fullbright;

		void Set(const HWSprite& s)
		{
			memset(this, 0, sizeof(*this));
			texture = s.texture;
			Colormap = s.Colormap;
			style = s.RenderStyle.AsDWORD;
			sectorAdd = s.particle->subsector->sector->AdditiveColors[sector_t::sprites].d;
			lightlevel = s.lightlevel;
			OverrideShader = s.OverrideShader;
			foglevel = s.foglevel;
			hw_styleflags = s.hw_styleflags;
			fullbright = s.fullbright;
		}
		bool operator==(const FParticleKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
		bool operator<(const FParticleKey& o) const { return memcmp(this, &o, sizeof(*this)) < 0; }
	};

	IVertexBuffer* ParticleBuffer = nullptr;
	TArray<FParticleVertex> ParticleVertices;
	IVertexBuffer* GpuParticleBuffer = nullptr;
	TArray<uint32_t> GpuParticleEntries;
	TArray<float> GpuParticleRangeData;
	uint64_t GpuParticleExpanded, GpuParticleFailed;
}

CVAR(Bool, r_particle_group, true, 0)
CVAR(Bool, r_particle_group_verify, false, 0)
CVAR(Bool, r_particle_gpu_lightpass, true, 0)
EXTERN_CVAR(Bool, r_particle_lit)
static constexpr float LitParticleOffset = 0.4f;
EXTERN_CVAR(Bool, gl_light_particles)
static uint64_t ParticleVerifyPasses, ParticleVerifyMismatches;

static uint64_t ParticlePasses, ParticleItems, ParticleRuns, ParticleKeyChanges, ParticleMaxItems, ParticleMaxRuns;

CCMD(particle_bucket_stats)
{
	const double p = ParticlePasses ? double(ParticlePasses) : 1.0;
	Printf("PARTICLEBUCKETS passes=%llu particles_per_pass=%.1f runs_per_pass=%.2f key_changes_per_pass=%.1f max_particles=%llu max_runs=%llu\n",
		(unsigned long long)ParticlePasses, ParticleItems / p, ParticleRuns / p, ParticleKeyChanges / p, (unsigned long long)ParticleMaxItems,
		(unsigned long long)ParticleMaxRuns);
	Printf("PARTICLEGPU expanded=%llu failed=%llu\n", (unsigned long long)GpuParticleExpanded, (unsigned long long)GpuParticleFailed);
	Printf("PARTICLEGROUP enabled=%d verify_passes=%llu verify_mismatches=%llu\n", int(r_particle_group),
		(unsigned long long)ParticleVerifyPasses, (unsigned long long)ParticleVerifyMismatches);
	if (argv.argc() == 2 && stricmp(argv[1], "reset") == 0)
		ParticlePasses = ParticleItems = ParticleRuns = ParticleKeyChanges = ParticleMaxItems = ParticleMaxRuns = 0;
}

void HWDrawList::DrawParticleBuckets(HWDrawInfo *di, FRenderState &state)
{
	if (drawitems.Size() == 0) return;
	FFrameStageScope keyStage(FS_ParticleKeys);
	TArray<FParticleKey> keys(drawitems.Size(), true);
	for (unsigned i = 0; i < drawitems.Size(); ++i) keys[i].Set(*sprites[drawitems[i].index]);
	TArray<unsigned> order(drawitems.Size(), true);
	TArray<unsigned> group;
	bool grouped = false;
	if (r_particle_group)
	{
		static constexpr unsigned MaxGroups = 32;
		unsigned firsts[MaxGroups], counts[MaxGroups] = {}, ranks[MaxGroups], starts[MaxGroups];
		unsigned groups = 0;
		group.Resize(keys.Size());
		grouped = true;
		for (unsigned i = 0; i < keys.Size() && grouped; ++i)
		{
			if (i > 0 && keys[i] == keys[i - 1])
			{
				group[i] = group[i - 1];
			}
			else
			{
				unsigned g = 0;
				while (g < groups && !(keys[firsts[g]] == keys[i])) ++g;
				if (g == groups)
				{
					if (groups == MaxGroups)
					{
						grouped = false;
						break;
					}
					firsts[groups++] = i;
				}
				group[i] = g;
				++ParticleKeyChanges;
			}
			++counts[group[i]];
		}
		if (grouped)
		{
			for (unsigned g = 0; g < groups; ++g) ranks[g] = g;
			std::sort(ranks, ranks + groups, [&](unsigned a, unsigned b) { return keys[firsts[a]] < keys[firsts[b]]; });
			unsigned at = 0;
			for (unsigned r = 0; r < groups; ++r)
			{
				starts[ranks[r]] = at;
				at += counts[ranks[r]];
			}
			for (unsigned i = 0; i < keys.Size(); ++i) order[starts[group[i]]++] = i;
		}
	}
	if (!grouped || r_particle_group_verify)
	{
		TArray<unsigned> sorted(drawitems.Size(), true);
		for (unsigned i = 0; i < sorted.Size(); ++i) sorted[i] = i;
		std::sort(sorted.begin(), sorted.end(), [&](unsigned a, unsigned b) { return keys[a] < keys[b] || (keys[a] == keys[b] && a < b); });
		if (grouped)
		{
			++ParticleVerifyPasses;
			bool same = true;
			for (unsigned k = 0; k < sorted.Size() && same; ++k)
				same = sorted[k] == order[k] && (k == 0 || (group[order[k]] == group[order[k - 1]]) == (keys[order[k]] == keys[order[k - 1]]));
			ParticleVerifyMismatches += !same;
		}
		else order = std::move(sorted);
	}
	auto sameKey = [&](unsigned a, unsigned b) { return grouped ? group[a] == group[b] : keys[a] == keys[b]; };

	if (keyStage.Active) FrameStageEnd(FS_ParticleKeys);
	keyStage.Active = false;
	struct FSegment { bool Gpu; unsigned First, Count; };
	struct FRun { HWSprite* first; unsigned segment, segments; };
	TArray<FRun> runs;
	TArray<FSegment> segments;
	auto addSegment = [&](bool gpu, unsigned first, unsigned count)
	{
		auto& run = runs.Last();
		if (run.segments > 0)
		{
			auto& last = segments.Last();
			if (last.Gpu == gpu && last.First + last.Count == first) { last.Count += count; return; }
		}
		segments.Push({ gpu, first, count });
		++run.segments;
	};
	FFrameStageScope buildStage(FS_ParticleBuild);
	ParticleVertices.Clear();
	const auto& vp = di->Viewpoint;
	const int rel = getExtraLight();
	const bool gpu = di->GpuParticles;
	GpuParticleEntries.Clear();
	GpuParticleRangeData.Clear();
	const bool lit = r_particle_lit && gl_light_particles && di->Level->HasDynamicLights && !di->isFullbrightScene();
	FGpuLightPack lightPack;
	const bool gpuLit = lit && gpu && r_particle_gpu_lightpass && WorldModeGpuLightPack(di->Level, lightPack);
	const bool perParticle = lit && !gpuLit;
	FGpuParticleIndices indices;
	GpuParticleProgramStore(indices);
	indices.Frame = di->GpuParticleIndices.data();
	indices.FrameCount = uint32_t(di->GpuParticleIndices.size());
	auto indexOf = [&](uint32_t first, uint32_t n) -> uint32_t
	{
		return (first & HWDrawInfo::GpuParticleProgramRange) ? indices.Program[(first & ~HWDrawInfo::GpuParticleProgramRange) + n] :
			di->GpuParticleIndices[first + n];
	};
	uint32_t entryCount = 0;
	struct FParticleLight { uint64_t Serial; float Rgb[3]; };
	static std::vector<FParticleLight> lightCache(65536, { 0, { 0, 0, 0 } });
	auto particleLight = [&](const particle_t* base, uint32_t index, float* out)
	{
		auto& cached = lightCache[index];
		if (cached.Serial != di->GpuParticleSerial + 1)
		{
			di->GetDynSpriteLight(nullptr, const_cast<particle_t*>(base + index), cached.Rgb);
			cached.Serial = di->GpuParticleSerial + 1;
		}
		memcpy(out, cached.Rgb, sizeof(cached.Rgb));
	};
	auto emitRange = [&](HWSprite* s, bool skipFirst)
	{
		const auto& range = di->GpuParticleRanges[s->GpuParticleRange];
		const unsigned from = skipFirst ? 1 : 0;
		if (range.Count <= from) return;
		const uint32_t slot = GpuParticleRangeData.Size() / 8;
		PalEntry light = PalEntry(255, 255, 255, 255);
		if (!di->isFullbrightScene())
		{
			const int level = CalcLightLevel(di->lightmode, s->lightlevel, s->fullbright ? 0 : rel, false, s->Colormap.BlendFactor);
			light = CalcLightColor(di->lightmode, level, s->Colormap.LightColor, s->Colormap.BlendFactor);
		}
		const PalEntry special = s->particle->subsector->sector->SpecialColors[sector_t::sprites];
		const float scale = 1.0f / 255.0f;
		const float data[8] = { light.r * scale, light.g * scale, light.b * scale, s->fullbright ? 1.0f : 0.0f,
			float(special.r), float(special.g), float(special.b), float(special.a) };
		for (float f : data) GpuParticleRangeData.Push(f);
		const unsigned at = entryCount;
		entryCount += range.Count - from;
		addSegment(true, at * 6, (range.Count - from) * 6);
		if (!perParticle)
		{
			const uint32_t record[4] = { range.First + from, range.Count - from, at, slot };
			memcpy(&GpuParticleEntries[GpuParticleEntries.Reserve(4)], record, sizeof(record));
			return;
		}
		const particle_t* base = s->particle - indexOf(range.First, 0);
		const bool litRange = !s->fullbright;
		uint32_t* out = &GpuParticleEntries[GpuParticleEntries.Reserve((range.Count - from) * 5)];
		for (unsigned n = from; n < range.Count; ++n)
		{
			const uint32_t index = indexOf(range.First, n);
			*out++ = index;
			*out++ = slot;
			float dyn[3] = { 0, 0, 0 };
			if (litRange) particleLight(base, index, dyn);
			memcpy(out, dyn, sizeof(dyn));
			out += 3;
		}
	};
	for (unsigned k = 0; k < order.Size(); ++k)
	{
		HWSprite* s = sprites[drawitems[order[k]].index];
		const bool ranged = gpu && s->GpuParticleRange >= 0;
		if (k == 0 || !sameKey(order[k], order[k - 1]))
		{
			runs.Push({ s, segments.Size(), 0 });
			if (ranged) emitRange(s, true);
			continue;
		}
		if (ranged)
		{
			emitRange(s, false);
			continue;
		}
		FVector3 v[4];
		s->CalculateVertices(di, v, const_cast<DVector3*>(&vp.Pos));
		const sector_t* cursec = s->particle->subsector->sector;
		const PalEntry object = s->fullbright ? s->ThingColor : s->ThingColor.Modulate(cursec->SpecialColors[sector_t::sprites]);
		PalEntry light = PalEntry(255, 255, 255, 255);
		if (!di->isFullbrightScene())
		{
			const int level = CalcLightLevel(di->lightmode, s->lightlevel, s->fullbright ? 0 : rel, false, s->Colormap.BlendFactor);
			light = CalcLightColor(di->lightmode, level, s->Colormap.LightColor, s->Colormap.BlendFactor);
		}
		const float scale = 1.0f / 255.0f;
		float lr = light.r * scale, lg = light.g * scale, lb = light.b * scale;
		if (lit && !s->fullbright)
		{
			// [Karto] r_particle_lit: the shader's clamp(colour + dynamic light, 0, 1.4), applied before the particle colour.
			float dyn[3] = { 0, 0, 0 };
			di->GetDynSpriteLight(nullptr, s->particle, dyn);
			lr = std::clamp(lr + dyn[0], 0.0f, 1.4f);
			lg = std::clamp(lg + dyn[1], 0.0f, 1.4f);
			lb = std::clamp(lb + dyn[2], 0.0f, 1.4f);
		}
		// [Karto] The shader caps the vertex colour at 1 before adding dynamic light, so followers carry a uniform dynamic light
		// of LitParticleOffset and the vertex colour the rest; clamp(min(c - 0.4, 1) + 0.4, 0, 1.4) returns c up to 1.4.
		const float offset = lit ? LitParticleOffset : 0.0f;
		const float r = lr * (object.r * scale) - offset, g = lg * (object.g * scale) - offset;
		const float b = lb * (object.b * scale) - offset, a = s->trans * (object.a * scale);
		const float u[4] = { s->ul, s->ur, s->ul, s->ur }, t[4] = { s->vt, s->vt, s->vb, s->vb };
		static const int corners[6] = { 0, 1, 2, 2, 1, 3 };
		const unsigned at = ParticleVertices.Size();
		for (int c : corners)
			ParticleVertices.Push({ v[c][0], v[c][1], v[c][2], u[c], t[c], 0, 0, -1.0f, r, g, b, a });
		addSegment(false, at, 6);
	}

	if (buildStage.Active) FrameStageEnd(FS_ParticleBuild);
	buildStage.Active = false;
	FFrameStageScope uploadStage(FS_ParticleUpload);
	static const FVertexBufferAttribute format[] = {
		{ 0, VATTR_VERTEX, VFmt_Float3, (int)myoffsetof(FParticleVertex, x) },
		{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(FParticleVertex, u) },
		{ 0, VATTR_LIGHTMAP, VFmt_Float3, (int)myoffsetof(FParticleVertex, lu) },
		{ 0, VATTR_COLOR, VFmt_Float4, (int)myoffsetof(FParticleVertex, r) },
	};
	if (ParticleVertices.Size() > 0)
	{
		if (ParticleBuffer == nullptr)
		{
			ParticleBuffer = screen->CreateVertexBuffer();
			ParticleBuffer->SetFormat(1, 4, sizeof(FParticleVertex), format);
		}
		// [Karto] One upload per view; SetData keeps earlier buffers alive for draws already recorded from them.
		ParticleBuffer->SetData(ParticleVertices.Size() * sizeof(FParticleVertex), ParticleVertices.Data(), BufferUsageType::Stream);
	}
	bool expanded = false;
	if (GpuParticleEntries.Size() > 0)
	{
		if (GpuParticleBuffer == nullptr)
		{
			GpuParticleBuffer = screen->CreateVertexBuffer();
			GpuParticleBuffer->SetFormat(1, 4, sizeof(FParticleVertex), format);
		}
		const float* data;
		uint32_t count;
		uint64_t serial;
		if (WorldModeGpuParticleData(di->Level, data, count, serial))
		{
			FGpuParticleParams params = {};
			params.ViewVector[0] = float(vp.ViewVector.X);
			params.ViewVector[1] = float(vp.ViewVector.Y);
			params.TimeFrac = float(Net_ModifyParticleFrac(nullptr, vp.TicFrac));
			params.PixelStretch = di->Level->pixelstretch;
			params.Yaw = vp.HWAngles.Yaw.Degrees();
			params.Pitch = vp.HWAngles.Pitch.Degrees();
			params.ViewPos[0] = float(vp.Pos.X);
			params.ViewPos[1] = float(vp.Pos.Y);
			params.Flags = (paused ? 1u : 0u) | (di->Level->isFrozen() ? 2u : 0u) | ((gl_billboard_particles || gl_billboard_mode == 1) ? 4u : 0u) |
				(gl_billboard_faces_camera ? 8u : 0u) | (hw_force_cambbpref ? 16u : 0u);
			params.Style = gl_particles_style;
			params.EntryCount = entryCount;
			params.Pad = gpuLit ? 2u : lit ? 1u : 0u;
			params.RecordCount = perParticle ? 0 : GpuParticleEntries.Size() / 4;
			expanded = state.ExpandParticles(GpuParticleBuffer, data, count, serial, GpuParticleEntries.Data(), GpuParticleRangeData.Data(),
				GpuParticleRangeData.Size() / 8, params, indices, gpuLit ? &lightPack : nullptr);
		}
		++(expanded ? GpuParticleExpanded : GpuParticleFailed);
	}

	if (uploadStage.Active) FrameStageEnd(FS_ParticleUpload);
	uploadStage.Active = false;
	FFrameStageScope drawStage(FS_ParticleDraw);
	++ParticlePasses;
	ParticleItems += order.Size();
	ParticleRuns += runs.Size();
	ParticleMaxItems = max<uint64_t>(ParticleMaxItems, order.Size());
	ParticleMaxRuns = max<uint64_t>(ParticleMaxRuns, runs.Size());
	for (auto& run : runs)
	{
		run.first->DrawSprite(di, state, true, true);
		if (run.segments)
		{
			state.SetObjectColor(0xffffffff);
			if (lit) state.SetDynLight(LitParticleOffset, LitParticleOffset, LitParticleOffset);
			else state.SetDynLight(0, 0, 0);
			for (unsigned n = run.segment; n < run.segment + run.segments; ++n)
			{
				const auto& seg = segments[n];
				if (seg.Gpu && !expanded) continue;
				state.SetVertexBuffer(seg.Gpu ? GpuParticleBuffer : ParticleBuffer, 0, 0);
				state.Draw(DT_Triangles, seg.First, seg.Count);
			}
			state.SetVertexBuffer(screen->mVertexData);
		}
		HWSprite::FinishAdditiveRun(state);
	}
}

void HWDrawList::SortWalls()
{
	HWSceneScopeTimer timer(SceneMainSurfaceProfile.SortWallsNS);
	if (UseOwnedOrder)
	{
		if (VisibleWalls.Size() > 1) std::sort(VisibleWalls.begin(), VisibleWalls.end(), [](const HWOwnedWallDraw& a, const HWOwnedWallDraw& b)
		{
			return a.Wall->texture != b.Wall->texture ? a.Wall->texture < b.Wall->texture :
				(a.Wall->flags & 3) < (b.Wall->flags & 3);
		});
		return;
	}
	if (drawitems.Size() > 1)
	{
		std::sort(drawitems.begin(), drawitems.end(), [this](const HWDrawItem &a, const HWDrawItem &b) -> bool
		{
			HWWall * w1 = walls[a.index];
			HWWall * w2 = walls[b.index];

			if (w1->texture != w2->texture) return w1->texture < w2->texture;
			return (w1->flags & 3) < (w2->flags & 3);

		});
	}
}

void HWDrawList::SortFlats()
{
	HWSceneScopeTimer timer(SceneMainSurfaceProfile.SortFlatsNS);
	if (drawitems.Size() > 1)
	{
		std::sort(drawitems.begin(), drawitems.end(), [this](const HWDrawItem &a, const HWDrawItem &b)
		{
			HWFlat * w1 = flats[a.index];
			HWFlat* w2 = flats[b.index];
			return w1->texture < w2->texture;
		});
	}
}


//==========================================================================
//
//
//
//==========================================================================

HWWall *HWDrawList::NewWall(HWWall* retained)
{
	auto wall = retained ? retained : (HWWall*)RenderDataAllocator.Alloc(sizeof(HWWall));
	drawitems.Push(HWDrawItem(DrawType_WALL, walls.Push(wall)));
	return wall;
}

//==========================================================================
//
//
//
//==========================================================================
HWFlat *HWDrawList::NewFlat()
{
	auto flat = (HWFlat*)RenderDataAllocator.Alloc(sizeof(HWFlat));
	drawitems.Push(HWDrawItem(DrawType_FLAT,flats.Push(flat)));
	return flat;
}

//==========================================================================
//
//
//
//==========================================================================
HWSprite *HWDrawList::NewSprite()
{
	auto sprite = (HWSprite*)RenderDataAllocator.Alloc(sizeof(HWSprite));
	drawitems.Push(HWDrawItem(DrawType_SPRITE, sprites.Push(sprite)));
	return sprite;
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::DoDraw(HWDrawInfo *di, FRenderState &state, bool translucent, int i)
{
	switch(drawitems[i].rendertype)
	{
	case DrawType_FLAT:
		{
			HWFlat * f= flats[drawitems[i].index];
			RenderFlat.Clock();
			f->DrawFlat(di, state, translucent);
			RenderFlat.Unclock();
		}
		break;

	case DrawType_WALL:
		{
			HWWall * w= walls[drawitems[i].index];
			HWWallDispatcher dis(di);
			RenderWall.Clock();
			w->DrawWall(&dis, state, translucent);
			RenderWall.Unclock();
		}
		break;

	case DrawType_SPRITE:
		{
			HWSprite * s= sprites[drawitems[i].index];
			RenderSprite.Clock();
			s->DrawSprite(di, state, translucent);
			RenderSprite.Unclock();
		}
		break;
	}
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::Draw(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	for (unsigned i = 0; i < drawitems.Size(); i++)
	{
		DoDraw(di, state, translucent, i);
	}
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::PrepareGpuDrivenWalls()
{
	GPUBakeBeginView();
	for (auto& item : drawitems)
	{
		auto wall = walls[item.index];
		if (!wall->CanGpuDriveWall(true)) continue;
		FFlatVertex current[4];
		FFlatVertex* cursor = current;
		wall->CreateVertices(cursor, false);
		GPUBakeAdmitWall(wall->seg, wall->type, current);
	}
}

// [Karto] Slot order follows the original sorted list, including gaps, so coplanar ties and fallback boundaries remain ordered.
bool HWDrawList::DrawGpuDrivenWalls(HWDrawInfo* di, FRenderState& state)
{
	if (di->outer || di->mCurrentPortal || di->mClipPortal || di->Viewpoint.bDoOob || di->Viewpoint.bDoOrtho ||
		UseOwnedOrder || !screen->IsVulkan() || drawitems.Size() == 0 || drawitems.Size() > 1024 * 1024) return false;
	TArray<uint32_t> rows;
	rows.Resize(drawitems.Size());
	uint32_t eligible = 0;
	{
		HWSceneScopeTimer candidateTimer(SceneMainSurfaceProfile.GpuCandidateBuildNS);
		for (unsigned i = 0; i < drawitems.Size(); ++i)
		{
			rows[i] = UINT32_MAX;
			auto wall = walls[drawitems[i].index];
			if (vk_gpu_driven_resident && wall->GpuAppearanceRevision && wall->GpuResidentEpoch && wall->GpuResidentEpoch == GPUBakeEpoch())
			{
				rows[i] = wall->GpuResidentRow;
				++eligible;
				continue;
			}
			if (!wall->CanGpuDriveWall(true)) continue;
			if (vk_gpu_driven_persistent && wall->GpuResidentEpoch && wall->GpuResidentEpoch == GPUBakeEpoch())
			{
				rows[i] = wall->GpuResidentRow;
				++eligible;
				continue;
			}
			if (vk_gpu_driven_bakerow)
			{
				uint32_t row;
				if (GPUBakeLookupRow(wall->seg, wall->type, &row))
				{
					rows[i] = row;
					++eligible;
					continue;
				}
			}
			FFlatVertex current[4];
			auto cursor = current;
			wall->CreateVertices(cursor, false);
			uint32_t offset, count;
			if (GPUBakeLookupWall(wall->seg, wall->type, current, &offset, &count, &rows[i])) ++eligible;
		}
	}
	if (!eligible) return false;
	VSMatrix vpm = di->VPUniforms.ProjectionMatrix;
	vpm.multMatrix(di->VPUniforms.ViewMatrix);
	const float* matrix = vpm.get();
	float planes[24];
	for (int p = 0; p < 6; ++p)
		for (int k = 0; k < 4; ++k)
			planes[p * 4 + k] = matrix[k * 4 + 3] + ((p & 1) ? -1.f : 1.f) * matrix[k * 4 + (p >> 1)];
	GPUBakeRunCull(planes, true, rows.Data(), rows.Size());
	if (!state.BeginGpuDrivenWalls()) return false;
	HWWallDispatcher dis(di);
	RenderWall.Clock();
	for (unsigned i = 0; i < drawitems.Size(); ++i)
	{
		auto wall = walls[drawitems[i].index];
		if (rows[i] != UINT32_MAX && vk_gpu_driven_resident &&
			state.DrawGpuResidentWall(i, wall->GpuAppearanceId, wall->GpuAppearanceRevision))
		{
			vertexcount += 4;
			continue;
		}
		state.EndGpuResidentBatch();
		state.SetGpuResidentCapture(wall->GpuAppearanceId, wall->GpuAppearanceRevision);
		wall->GpuBakeRow = rows[i];
		wall->DrawWall(&dis, state, false, rows[i] != UINT32_MAX ? i : UINT32_MAX);
		state.SetGpuResidentCapture(UINT32_MAX, 0);
	}
	state.EndGpuDrivenWalls();
	RenderWall.Unclock();
	return true;
}

void HWDrawList::DrawWalls(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	HWWallDispatcher dis(di);
	RenderWall.Clock();
	if (UseOwnedOrder)
	{
		HWPreparedWallRun run;
		for (const auto& item : VisibleWalls)
		{
			if (item.Version || (vk_worldruns && vk_drawreuse && !translucent))
			{
				if (item.Wall->DrawPreparedWall(&dis, state, run, item.Version)) continue;
				if (SceneProfiling)
				{
					++SceneMainSurfaceProfile.PreparedDrawFallbacks;
					if (item.Version) ++SceneMainSurfaceProfile.OwnedDrawFallbacks;
				}
			}
			state.EndRetainedWallRun();
			if (run.Buffer)
			{
				state.SetVertexBuffer(screen->mVertexData);
				run = {};
			}
			item.Wall->DrawWall(&dis, state, translucent);
		}
		state.EndRetainedWallRun();
		if (run.Buffer) state.SetVertexBuffer(screen->mVertexData);
		RenderWall.Unclock();
		return;
	}
	if (vk_worldruns && vk_retainedworld == 2 && vk_drawreuse && !translucent && !gl_seamless &&
		screen->IsVulkan() && screen->BuffersArePersistent() && !di->Level->HasDynamicLights &&
		!di->mCurrentPortal && !di->Viewpoint.bDoOob && !di->Viewpoint.bDoOrtho && !sysCallbacks.PreBindTexture)
	{
		HWPreparedWallRun run;
		for (auto& item : drawitems)
		{
			auto wall = walls[item.index];
			if (wall->DrawPreparedWall(&dis, state, run)) continue;
			state.EndRetainedWallRun();
			if (run.Buffer)
			{
				state.SetVertexBuffer(screen->mVertexData);
				run = {};
			}
			if (SceneProfiling) ++SceneMainSurfaceProfile.PreparedDrawFallbacks;
			wall->DrawWall(&dis, state, translucent);
		}
		state.EndRetainedWallRun();
		if (run.Buffer) state.SetVertexBuffer(screen->mVertexData);
		RenderWall.Unclock();
		return;
	}
	for (auto &item : drawitems)
	{
		walls[item.index]->DrawWall(&dis, state, translucent);
	}
	RenderWall.Unclock();
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::DrawFlats(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	RenderFlat.Clock();
	for (unsigned i = 0; i<drawitems.Size(); i++)
	{
		flats[drawitems[i].index]->DrawFlat(di, state, translucent);
	}
	RenderFlat.Unclock();
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::DrawSorted(HWDrawInfo *di, FRenderState &state, SortNode * head)
{
	float clipsplit[2];
	int relation = 0;
	float z = 0.f;

	state.GetClipSplit(clipsplit);

	if (drawitems[head->itemindex].rendertype == DrawType_FLAT)
	{
		z = flats[drawitems[head->itemindex].index]->z;
		relation = z > di->Viewpoint.Pos.Z ? 1 : -1;
	}


	// left is further away, i.e. for stuff above viewz its z coordinate higher, for stuff below viewz its z coordinate is lower
	if (head->left)
	{
		if (relation == -1)
		{
			state.SetClipSplit(clipsplit[0], z);	// render below: set flat as top clip plane
		}
		else if (relation == 1)
		{
			state.SetClipSplit(z, clipsplit[1]);	// render above: set flat as bottom clip plane
		}
		DrawSorted(di, state, head->left);
		state.SetClipSplit(clipsplit);
	}
	DoDraw(di, state, true, head->itemindex);
	if (head->equal)
	{
		SortNode * ehead = head->equal;
		while (ehead)
		{
			DoDraw(di, state, true, ehead->itemindex);
			ehead = ehead->equal;
		}
	}
	// right is closer, i.e. for stuff above viewz its z coordinate is lower, for stuff below viewz its z coordinate is higher
	if (head->right)
	{
		if (relation == 1)
		{
			state.SetClipSplit(clipsplit[0], z);	// render below: set flat as top clip plane
		}
		else if (relation == -1)
		{
			state.SetClipSplit(z, clipsplit[1]);	// render above: set flat as bottom clip plane
		}
		DrawSorted(di, state, head->right);
		state.SetClipSplit(clipsplit);
	}
}

//==========================================================================
//
//
//
//==========================================================================
void HWDrawList::DrawSorted(HWDrawInfo *di, FRenderState &state)
{
	if (drawitems.Size() == 0) return;

	if (!sorted)
	{
		screen->mVertexData->Map();
		Sort(di);
		screen->mVertexData->Unmap();
	}
	state.ClearClipSplit();
	state.EnableClipDistance(1, true);
	state.EnableClipDistance(2, true);
	DrawSorted(di, state, sorted);
	state.EnableClipDistance(1, false);
	state.EnableClipDistance(2, false);
	state.ClearClipSplit();
}
