/*
** hw_bsp.cpp
**
** Main rendering loop / BSP traversal / visibility clipping
**
**---------------------------------------------------------------------------
**
** Copyright 2000-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "p_lnspec.h"
#include "p_local.h"
#include "a_sharedglobal.h"
#include "g_levellocals.h"
#include "p_effect.h"
#include "po_man.h"
#include "m_fixed.h"
#include "ctpl.h"
#include "texturemanager.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hwrenderer/scene/hw_clipper.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_portal.h"
#include "hw_clock.h"
#include "flatvertices.h"
#include "hw_vertexbuilder.h"
#include "hw_walldispatcher.h"
#include <limits>
#include <memory>
#include <vector>
#include <algorithm>

#include "p_visualthinker.h"
#include "hw_spriterecord.h"
#include "c_dispatch.h"
#include "d_framestages.h"

#ifdef ARCH_IA32
#include <immintrin.h>
#endif // ARCH_IA32

CVAR(Bool, gl_multithread, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_jobqueue_fast, true, 0)

EXTERN_CVAR(Float, r_actorspriteshadowdist)
EXTERN_CVAR(Bool, r_radarclipper)
EXTERN_CVAR(Bool, r_dithertransparency)
EXTERN_CVAR(Color, gl_cullcolor)
EXTERN_CVAR(Bool, vk_gpu_driven_bake)
EXTERN_CVAR(Bool, vk_gpu_driven_wallbake)
extern bool g_bakeSweepActive;
extern uint64_t g_sweepMvCalls, g_sweepBaked, g_sweepSkipNoTex, g_sweepSkipSplit, g_sweepSkipCanCache, g_sweepSkipOther;

CVAR(Bool, vk_gpu_driven_compat, false, 0)
static std::vector<uint8_t> g_coveragePending;

thread_local bool isWorkerThread;
HWSceneSurfaceProfile &GetSceneSurfaceProfile()
{
	return isWorkerThread ? SceneWorkerSurfaceProfile : SceneMainSurfaceProfile;
}

ctpl::thread_pool renderPool(1);
bool inited = false;

const int MAXDITHERACTORS = 20; // Maximum number of enemies that can set dither-transparency flags
AActor* RenderedTargets[MAXDITHERACTORS];
int RTnum;

void ClearDitherTargets()
{
	RTnum = 0; // Number of rendered enemies/targets
	for (int ii = 0; ii < MAXDITHERACTORS; ii++)
	  RenderedTargets[ii] = nullptr;
}

struct RenderJob
{
	enum
	{
		FlatJob,
		WallJob,
		SpriteJob,
		ParticleJob,
		PortalJob,
		TerminateJob	// inserted when all work is done so that the worker can return.
	};

	int type;
	subsector_t *sub;
	seg_t *seg;
	bool isculled;
	unsigned wallOrder;
};

static_assert(RenderJob::TerminateJob == 5);

class RenderJobQueue
{
	static constexpr int InitialCapacity = 300000;
	static constexpr int ChunkCapacity = 4096;
	struct JobChunk
	{
		RenderJob Jobs[ChunkCapacity];
		std::unique_ptr<JobChunk> Next;
	};
	RenderJob pool[InitialCapacity];
	std::unique_ptr<JobChunk> Overflow;
	JobChunk* ProducerChunk = nullptr;
	JobChunk* ConsumerChunk = nullptr;
	int ProducerOffset = 0;
	int ConsumerOffset = 0;
	std::atomic<int> readindex{};
	std::atomic<int> writeindex{};
	alignas(64) bool Fast = false;
	alignas(64) int ProducerIndex = 0;
	alignas(64) std::atomic<int> Published{};
	alignas(64) int ConsumerIndex = 0;
	int Available = 0;

	RenderJob* ProducerSlot(int index)
	{
		if (index < InitialCapacity) return &pool[index];
		// [Karto] Stable chunks grow the append pool without moving payloads still referenced by the worker.
		if (!ProducerChunk)
		{
			if (!Overflow) Overflow = std::make_unique<JobChunk>();
			ProducerChunk = Overflow.get();
		}
		else if (ProducerOffset == ChunkCapacity)
		{
			if (!ProducerChunk->Next) ProducerChunk->Next = std::make_unique<JobChunk>();
			ProducerChunk = ProducerChunk->Next.get();
			ProducerOffset = 0;
		}
		return &ProducerChunk->Jobs[ProducerOffset++];
	}

	RenderJob* ConsumerSlot(int index)
	{
		if (index < InitialCapacity) return &pool[index];
		// [Karto] The acquired published count covers chunk links as well as payloads; only the producer appends links.
		if (!ConsumerChunk) ConsumerChunk = Overflow.get();
		else if (ConsumerOffset == ChunkCapacity)
		{
			ConsumerChunk = ConsumerChunk->Next.get();
			ConsumerOffset = 0;
		}
		return &ConsumerChunk->Jobs[ConsumerOffset++];
	}

public:
	void AddJob(int type, subsector_t *sub, seg_t *seg = nullptr, bool isculled = false, unsigned wallOrder = 0)
	{
		if (SceneProfiling)
		{
			++SceneTraversalProfile.Jobs[type];
		}
		if (Fast)
		{
			const int index = ProducerIndex;
			if (index == std::numeric_limits<int>::max()) I_FatalError("Render job index limit exceeded");
			*ProducerSlot(index) = { type, sub, seg, isculled, wallOrder };
			++ProducerIndex;
			// [Karto] One producer owns each slot; release publishes its complete payload without a locked increment.
			Published.store(index + 1, std::memory_order_release);
			return;
		}

		const int index = writeindex.load(std::memory_order_relaxed);
		if (index == std::numeric_limits<int>::max()) I_FatalError("Render job index limit exceeded");
		*ProducerSlot(index) = { type, sub, seg, isculled, wallOrder };
		writeindex.store(index + 1);
	}

	RenderJob *GetJob()
	{
		if (Fast)
		{
			// [Karto] Acquire only when the consumed prefix runs out; published slots stay immutable until the worker joins.
			if (ConsumerIndex == Available) Available = Published.load(std::memory_order_acquire);
			if (ConsumerIndex < Available) return ConsumerSlot(ConsumerIndex++);
			return nullptr;
		}
		if (readindex < writeindex) return ConsumerSlot(readindex++);
		return nullptr;
	}

	void ReleaseAll()
	{
		// [Karto] RenderBSP joins the previous consumer before reset, so mode changes cannot mix queue protocols.
		Fast = gl_jobqueue_fast;
		ProducerIndex = ConsumerIndex = Available = 0;
		ProducerChunk = ConsumerChunk = nullptr;
		ProducerOffset = ConsumerOffset = 0;
		Published.store(0, std::memory_order_relaxed);
		readindex = 0;
		writeindex = 0;
	}
};

static RenderJobQueue jobQueue;	// One static queue is sufficient here. This code will never be called recursively.

void HWDrawInfo::WorkerThread()
{
	const bool profile = SceneProfiling;
	uint64_t emptyPolls = 0;
	sector_t *front, *back;
	HWWallDispatcher disp(this);

	WTTotal.Clock();
	isWorkerThread = true;	// for adding asserts in GL API code. The worker thread may never call any GL API.
	while (true)
	{
		auto job = jobQueue.GetJob();
		if (job == nullptr)
		{
			if (profile) ++emptyPolls;
#ifdef ARCH_IA32
			// The queue is empty. But yielding would be too costly here and possibly cause further delays down the line if the thread is halted.
			// So instead add a few pause instructions and retry immediately.
			_mm_pause();
			_mm_pause();
			_mm_pause();
			_mm_pause();
			_mm_pause();
			_mm_pause();
			_mm_pause();
			_mm_pause();
			_mm_pause();
			_mm_pause();
#endif // ARCH_IA32
			continue;
		}
		if (profile)
		{
			++SceneWorkerProfile.Jobs[job->type];
		}
		// Note that the main thread MUST have prepared the fake sectors that get used below!
		// This worker thread cannot prepare them itself without costly synchronization.
		switch (job->type)
		{
		case RenderJob::TerminateJob:
			if (profile)
			{
				++SceneWorkerProfile.Runs;
				SceneWorkerProfile.EmptyPolls += emptyPolls;
			}
			WTTotal.Unclock();
			return;

		case RenderJob::WallJob:
		{
			disp.SubmissionOrder = job->wallOrder;
			disp.SubmissionPart = 0;
			HWWall wall;
			SetupWall.Clock();
			wall.sub = job->sub;

			front = hw_FakeFlat(job->sub->sector, in_area, false);
			auto seg = job->seg;
			auto backsector = seg->backsector;
			if (!backsector && seg->linedef->isVisualPortal() && seg->sidedef == seg->linedef->sidedef[0]) // For one-sided portals use the portal's destination sector as backsector.
			{
				auto portal = seg->linedef->getPortal();
				backsector = portal->mDestination->frontsector;
				back = hw_FakeFlat(backsector, in_area, true);
				if (front->floorplane.isSlope() || front->ceilingplane.isSlope() || back->floorplane.isSlope() || back->ceilingplane.isSlope())
				{
					// Having a one-sided portal like this with slopes is too messy so let's ignore that case.
					back = nullptr;
				}
			}
			else if (backsector)
			{
				if (front->sectornum == backsector->sectornum || (seg->sidedef->Flags & WALLF_POLYOBJ))
				{
					back = front;
				}
				else
				{
					back = hw_FakeFlat(backsector, in_area, true);
				}
			}
			else back = nullptr;

			wall.Process(&disp, job->seg, front, back, job->isculled);
			rendered_lines++;
			SetupWall.Unclock();
			break;
		}

		case RenderJob::FlatJob:
		{
			HWFlat flat;
			SetupFlat.Clock();
			flat.section = job->sub->section;
			front = hw_FakeFlat(job->sub->render_sector, in_area, false);
			flat.ProcessSector(this, front);
			SetupFlat.Unclock();
			break;
		}

		case RenderJob::SpriteJob:
			SetupSprite.Clock();
			front = hw_FakeFlat(job->sub->sector, in_area, false);
			RenderThings(job->sub, front);
			SetupSprite.Unclock();
			break;

		case RenderJob::ParticleJob:
			SetupSprite.Clock();
			front = hw_FakeFlat(job->sub->sector, in_area, false);
			RenderParticles(job->sub, front);
			SetupSprite.Unclock();
			break;

		case RenderJob::PortalJob:
			AddSubsectorToPortal((FSectorPortalGroup *)job->seg, job->sub);
			if (profile) ++SceneTraversalProfile.PortalRegistrations;
			break;
		}

	}
}





EXTERN_CVAR(Bool, gl_render_segs)

CVAR(Bool, gl_render_things, true, 0)
CVAR(Bool, gl_render_walls, true, 0)
CVAR(Bool, gl_render_flats, true, 0)
CVAR(Bool, r_ceiling_noworld, false, 0)
EXTERN_CVAR(Bool, r_worldmode)
// [Karto] A world-mode traversal that keeps only gameplay-visible side effects (automap, drawn flags);
// file-static because AddLine/DoSubsector run on the main thread only and must not queue any production.
static bool WorldSideEffectsOnly = false;

void HWDrawInfo::UnclipSubsector(subsector_t *sub)
{
	int count = sub->numlines;
	seg_t * seg = sub->firstline;
	auto &clipper = *mClipper;

	while (count--)
	{
		angle_t startAngle = clipper.GetClipAngle(seg->v2);
		angle_t endAngle = clipper.GetClipAngle(seg->v1);

		// Back side, i.e. backface culling	- read: endAngle >= startAngle!
		if (startAngle-endAngle >= ANGLE_180)
		{
			clipper.SafeRemoveClipRange(startAngle, endAngle);
			clipper.SetBlocked(false);
		}
		seg++;
	}
}

//==========================================================================
//
// R_AddLine
// Clips the given segment
// and adds any visible pieces to the line list.
//
//==========================================================================

EXTERN_CVAR(Int, r_distance_cull_type)
EXTERN_CVAR(Int, gl_particles_style)
EXTERN_CVAR(Float, r_line_distance_cull)

bool hw_IsLineDistanceCulled(const line_t* line, const DVector2& VPos, double cullDistanceSquared)
{
	if (cullDistanceSquared <= 0.0)
		return false;

	DVector2 vec1 = (line->v1->fPos() - VPos);
	DVector2 vec2 = (line->v2->fPos() - VPos);
	double dist1 = vec1.LengthSquared();
	double dist2 = vec2.LengthSquared();
	if ((dist1 > cullDistanceSquared) && (dist2 > cullDistanceSquared))
	{
		if (vec1.dot(vec2) > 0.0)
		{
			return true;
		}
		else
		{
			DVector2 linevec = line->Delta();
			double linelensq = linevec.LengthSquared();
			if (linelensq > 0.0)
			{
				double projlen = -vec1.dot(linevec) / linelensq;
				DVector2 closest = line->v1->fPos() + linevec * projlen;
				if ((VPos- closest).LengthSquared() > cullDistanceSquared)
				{
					return true;
				}
			}
		}
	}
	return false;
}

bool HWDrawInfo::IsDistanceCulled(seg_t *line)
{
	if (Viewpoint.culldistsq <= 0.0) return false;
	DVector2 VPos = Viewpoint.Pos.XY();
	if (Viewpoint.bDoOob && r_radarclipper && !(Level->flags3 & LEVEL3_NOFOGOFWAR)) VPos = Viewpoint.OffPos.XY();
	return hw_IsLineDistanceCulled(line->linedef, VPos, Viewpoint.culldistsq);
}

void HWDrawInfo::AddLine (seg_t *seg, bool portalclip)
{
	const bool profile = SceneProfiling;
	if (profile) ++SceneTraversalProfile.Lines;
	const bool doOob = Viewpoint.bDoOob;
	bool isculled = false;

#ifdef _DEBUG
	if (seg->linedef && seg->linedef->Index() == 38)
	{
		int a = 0;
	}
#endif

	sector_t * backsector = nullptr;

	if (portalclip)
	{
		int clipres = mClipPortal->ClipSeg(seg, Viewpoint.Pos);
		if (clipres == PClip_InFront)
		{
			if (profile) ++SceneTraversalProfile.PortalRejected;
			return;
		}
	}

	auto &clipper = *mClipper;
	angle_t startAngle = clipper.GetClipAngle(seg->v2);
	angle_t endAngle = clipper.GetClipAngle(seg->v1);
	auto &clipperr = *rClipper;
	angle_t startAngleR = 0;
	angle_t endAngleR = 0;
	angle_t paddingR = 0x00200000; // Make radar clipping more aggressive (reveal less)

	if(doOob && r_radarclipper && !(Level->flags3 & LEVEL3_NOFOGOFWAR))
	{
		if (profile) ++SceneTraversalProfile.RadarLines;
		startAngleR = clipperr.PointToPseudoAngle(seg->v2->fX(), seg->v2->fY());
		endAngleR = clipperr.PointToPseudoAngle(seg->v1->fX(), seg->v1->fY());

		if (startAngleR - endAngleR >= ANGLE_180)
		{
			if (!seg->backsector) clipperr.SafeAddClipRange(startAngleR - paddingR, endAngleR + paddingR);
			else if((seg->sidedef != nullptr) && !uint8_t(seg->sidedef->Flags & WALLF_POLYOBJ) && (currentsector->sectornum != seg->backsector->sectornum))
			{
				if (in_area == area_default)
				{
					if (profile) ++SceneTraversalProfile.ViewAreaChecks;
					in_area = hw_CheckViewArea(seg->v1, seg->v2, seg->frontsector, seg->backsector);
				}
				if (profile)
				{
					++SceneTraversalProfile.FakeBackCalls;
					++SceneTraversalProfile.ClipChecks;
				}
				backsector = hw_FakeFlat(seg->backsector, in_area, true);
				if (hw_CheckClip(seg->sidedef, currentsector, backsector)) clipperr.SafeAddClipRange(startAngleR - paddingR, endAngleR + paddingR);
				backsector = nullptr;
			}
		}
	}

	// Back side, i.e. backface culling	- read: endAngle >= startAngle!
	if (startAngle-endAngle<ANGLE_180)
	{
		if (profile) ++SceneTraversalProfile.BackfaceRejected;
		return;
	}

	if (seg->sidedef == nullptr)
	{
		if (profile) ++SceneTraversalProfile.Minisegs;
		if (!(currentsubsector->flags & SSECMF_DRAWN))
		{
			if (clipper.SafeCheckRange(startAngle, endAngle) && !doOob)
			{
				currentsubsector->flags |= SSECMF_DRAWN;
				if (profile) ++SceneTraversalProfile.SubsectorDrawnWrites;
			}
			if (doOob && ((r_radarclipper && !(Level->flags3 & LEVEL3_NOFOGOFWAR)) || (r_distance_cull_type > 0)) && clipperr.SafeCheckRange(startAngleR, endAngleR))
			{
				currentsubsector->flags |= SSECMF_DRAWN;
				if (profile) ++SceneTraversalProfile.SubsectorDrawnWrites;
			}
		}
		return;
	}

	if (!clipper.SafeCheckRange(startAngle, endAngle))
	{
		if (profile) ++SceneTraversalProfile.RangeRejected;
		return;
	}

	if (!doOob || (!r_radarclipper || (Level->flags3 & LEVEL3_NOFOGOFWAR) || clipperr.SafeCheckRange(startAngleR, endAngleR)))
	{
		currentsubsector->flags |= SSECMF_DRAWN;
		if (profile) ++SceneTraversalProfile.SubsectorDrawnWrites;
	}

	uint8_t ispoly = uint8_t(seg->sidedef->Flags & WALLF_POLYOBJ);

	if (profile && r_distance_cull_type > 0) ++SceneTraversalProfile.DistanceChecks;
	if (r_distance_cull_type > 0 && IsDistanceCulled(seg))
	{
		if (profile) ++SceneTraversalProfile.DistanceCulled;
		isculled = true;
		if(doOob)
		{
			if (startAngleR == 0) startAngleR = clipperr.PointToPseudoAngle(seg->v2->fX(), seg->v2->fY());
			if (endAngleR == 0) endAngleR = clipperr.PointToPseudoAngle(seg->v1->fX(), seg->v1->fY());
			if (startAngleR - endAngleR >= ANGLE_180) clipperr.SafeAddClipRange(startAngleR - paddingR, endAngleR + paddingR);
		}
		else
		{
			clipper.SafeAddClipRange(startAngle, endAngle);
		}
		if (seg->frontsector->Colormap.FadeColor != 0) Level->cullcolor = seg->frontsector->Colormap.FadeColor; // Write can't be multithreaded
	}

	// [XA] NOTE: ideally it'd be nice to collapse these checks into one,
	// but it's possible to add & remove WALLF_BLOCKRENDERING via zscript
	// so auto-setting it on 1s lines may result in a crash if someone
	// later tries to remove the flag from a line.
	if (!seg->backsector || (seg->sidedef->Flags & WALLF_BLOCKRENDERING))
	{
		if(!doOob)
			if (!(seg->sidedef->Flags & WALLF_DITHERTRANS_MID)) clipper.SafeAddClipRange(startAngle, endAngle);
	}
	else if (!ispoly)	// Two-sided polyobjects never obstruct the view
	{
		if (currentsector->sectornum == seg->backsector->sectornum)
		{
			if (!seg->linedef->isVisualPortal())
			{
				auto tex = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::mid), true);
				if (!tex || !tex->isValid()) 
				{
					if (profile) ++SceneTraversalProfile.EmptyTextureRejected;
					// nothing to do here!
					seg->linedef->validcount=rendervalidcount;
					return;
				}
			}
			backsector=currentsector;
		}
		else
		{
			// clipping checks are only needed when the backsector is not the same as the front sector
			if (in_area == area_default)
			{
				if (profile) ++SceneTraversalProfile.ViewAreaChecks;
				in_area = hw_CheckViewArea(seg->v1, seg->v2, seg->frontsector, seg->backsector);
			}
			if (profile)
			{
				++SceneTraversalProfile.FakeBackCalls;
				++SceneTraversalProfile.ClipChecks;
			}

			backsector = hw_FakeFlat(seg->backsector, in_area, true);

			if (hw_CheckClip(seg->sidedef, currentsector, backsector))
			{
				if(!doOob && !(seg->sidedef->Flags & WALLF_DITHERTRANS_MID))
					clipper.SafeAddClipRange(startAngle, endAngle);
			}
		}
	}
	else
	{
		// Backsector for polyobj segs is always the containing sector itself
		backsector = currentsector;
	}

	seg->linedef->flags |= ML_MAPPED;
	if (profile) ++SceneTraversalProfile.MLMappedWrites;
	if (vk_gpu_driven_compat)
	{
		const unsigned revealIndex = seg->linedef->Index();
		if (g_coveragePending.size() > (revealIndex >> 3)) g_coveragePending[revealIndex >> 3] |= uint8_t(1u << (revealIndex & 7));
	}

	if (ispoly || seg->linedef->validcount!=rendervalidcount)
	{
		if (!ispoly) seg->linedef->validcount=rendervalidcount;

		if (gl_render_walls && !WorldSideEffectsOnly)
		{
			if (profile)
			{
				++SceneTraversalProfile.WallWork;
				if (ispoly) ++SceneTraversalProfile.PolyWallWork;
			}
			unsigned order = 0;
			if (OwnOpaqueWalls)
			{
				if (OwnedNextOrder == std::numeric_limits<unsigned>::max()) I_FatalError("Wall submission order limit exceeded");
				order = OwnedNextOrder++;
			}
			if (multithread)
			{
				jobQueue.AddJob(RenderJob::WallJob, seg->Subsector, seg, isculled, order);
			}
			else
			{
				HWWall wall;
				HWWallDispatcher disp(this);
				disp.SubmissionOrder = order;
				SetupWall.Clock();
				wall.sub = seg->Subsector;
				wall.Process(&disp, seg, currentsector, backsector, isculled);
				rendered_lines++;
				SetupWall.Unclock();
			}
		}
		else if (profile) ++SceneTraversalProfile.DisabledWalls;
	}
	else if (profile) ++SceneTraversalProfile.DuplicateLines;
}

//==========================================================================
//
// R_Subsector
// Determine floor/ceiling planes.
// Add sprites of things in sector.
// Draw one or more line segments.
//
//==========================================================================

void HWDrawInfo::PolySubsector(subsector_t * sub)
{
	int count = sub->numlines;
	seg_t * line = sub->firstline;

	while (count--)
	{
		if (line->linedef)
		{
			AddLine (line, mClipPortal != nullptr);
		}
		line++;
	}
}

//==========================================================================
//
// RenderBSPNode
// Renders all subsectors below a given node,
//  traversing subtree recursively.
// Just call with BSP root.
//
//==========================================================================

void HWDrawInfo::RenderPolyBSPNode (void *node)
{
	while (!((size_t)node & 1))  // Keep going until found a subsector
	{
		node_t *bsp = (node_t *)node;

		// Decide which side the view point is on.
		int side = R_PointOnSide(viewx, viewy, bsp);

		// Recursively divide front space (toward the viewer).
		RenderPolyBSPNode (bsp->children[side]);

		// Possibly divide back space (away from the viewer).
		side ^= 1;

		// It is not necessary to use the slower precise version here
		if (!mClipper->CheckBox(bsp->bbox[side]))
		{
			return;
		}

		node = bsp->children[side];
	}
	PolySubsector ((subsector_t *)((uint8_t *)node - 1));
}

//==========================================================================
//
// Unlilke the software renderer this function will only draw the walls,
// not the flats. Those are handled as a whole by the parent subsector.
//
//==========================================================================

void HWDrawInfo::AddPolyobjs(subsector_t *sub)
{
	if (sub->BSP == nullptr || sub->BSP->bDirty)
	{
		sub->BuildPolyBSP();
	}
	if (sub->BSP->Nodes.Size() == 0)
	{
		PolySubsector(&sub->BSP->Subsectors[0]);
	}
	else
	{
		RenderPolyBSPNode(&sub->BSP->Nodes.Last());
	}
}


//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::AddLines(subsector_t * sub, sector_t * sector)
{
	currentsector = sector;
	currentsubsector = sub;

	ClipWall.Clock();
	if (sub->polys != nullptr)
	{
		AddPolyobjs(sub);
	}
	else
	{
		int count = sub->numlines;
		seg_t * seg = sub->firstline;

		while (count--)
		{
			if (seg->linedef == nullptr)
			{
				if (!(sub->flags & SSECMF_DRAWN)) AddLine (seg, mClipPortal != nullptr);
			}
			else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
			{
				AddLine (seg, mClipPortal != nullptr);
			}
			seg++;
		}
	}
	ClipWall.Unclock();
}

//==========================================================================
//
// Adds lines that lie directly on the portal boundary.
// Only two-sided lines will be handled here, and no polyobjects
//
//==========================================================================

inline bool PointOnLine(const DVector2 &pos, const linebase_t *line)
{
	double v = (pos.Y - line->v1->fY()) * line->Delta().X + (line->v1->fX() - pos.X) * line->Delta().Y;
	return fabs(v) <= EQUAL_EPSILON;
}

void HWDrawInfo::AddSpecialPortalLines(subsector_t * sub, sector_t * sector, linebase_t *line)
{
	currentsector = sector;
	currentsubsector = sub;

	ClipWall.Clock();
	int count = sub->numlines;
	seg_t * seg = sub->firstline;

	while (count--)
	{
		if (seg->linedef != nullptr && seg->PartnerSeg != nullptr)
		{
			if (PointOnLine(seg->v1->fPos(), line) && PointOnLine(seg->v2->fPos(), line))
				AddLine(seg, false);
		}
		seg++;
	}
	ClipWall.Unclock();
}


//==========================================================================
//
// R_RenderThings
//
//==========================================================================

CVAR(Bool, r_worldmode_thingsplit, true, 0)
static std::vector<int> ThingStamps;

void HWDrawInfo::RenderThings(subsector_t * sub, sector_t * sector)
{
	sector_t * sec=sub->sector;
	// Handle all things in sector.
	const auto &vp = Viewpoint;
	auto listedThing = [&](AActor* thing)
	{
		if (thing->validcount == rendervalidcount) return;
		thing->validcount = rendervalidcount;

		if(Viewpoint.bDoOob && thing->Sector->isSecret() && thing->Sector->wasSecret() && !r_radarclipper) return; // This covers things that are touching non-secret sectors
		FIntCVar *cvar = thing->GetInfo()->distancecheck;
		if (cvar != nullptr && *cvar >= 0)
		{
			double dist = (thing->Pos() - (vp.bDoOob ? vp.OffPos : vp.Pos)).LengthSquared();
			double check = (double)**cvar;
			if (dist >= check * check)
			{
				return;
			}
		}
		if (r_distance_cull_type > 0)
		{
			bool renderradcheck = thing->RenderRadius() < 0.25 * vp.culldist;
			double dist2 = renderradcheck ? (thing->Pos() - (vp.bDoOob ? vp.OffPos : vp.Pos)).LengthSquared() :
				(thing->Pos() - (vp.bDoOob ? vp.OffPos : vp.Pos)).Length() - 1.414 * thing->RenderRadius();
			if (dist2 >= (renderradcheck ? vp.culldistsq : vp.culldist))
			{
				return;
			}
		}
		// If this thing is in a map section that's not in view it can't possibly be visible
		if (CurrentMapSections[thing->subsector->mapsection])
		{
			HWSprite sprite;

			// [Nash] draw sprite shadow
			if (R_ShouldDrawSpriteShadow(thing))
			{
				double dist = (thing->Pos() - vp.Pos).LengthSquared();
				double check = r_actorspriteshadowdist;
				if (dist <= check * check)
				{
					sprite.Process(this, thing, sector, in_area, false, true);
				}
			}

			sprite.Process(this, thing, sector, in_area, false);
		}
	};
	auto recordThing = [&](FSpriteRecord& r)
	{
		if (r.validcount == rendervalidcount) return;
		r.validcount = rendervalidcount;
		if (Viewpoint.bDoOob && r.Sector->isSecret() && r.Sector->wasSecret() && !r_radarclipper) return;
		FIntCVar *cvar = r.Class->ActorInfo()->distancecheck;
		if (cvar != nullptr && *cvar >= 0)
		{
			double dist = (r.Pos - (vp.bDoOob ? vp.OffPos : vp.Pos)).LengthSquared();
			double check = (double)**cvar;
			if (dist >= check * check)
			{
				return;
			}
		}
		if (r_distance_cull_type > 0)
		{
			bool renderradcheck = r.RenderRadius < 0.25 * vp.culldist;
			double dist2 = renderradcheck ? (r.Pos - (vp.bDoOob ? vp.OffPos : vp.Pos)).LengthSquared() :
				(r.Pos - (vp.bDoOob ? vp.OffPos : vp.Pos)).Length() - 1.414 * r.RenderRadius;
			if (dist2 >= (renderradcheck ? vp.culldistsq : vp.culldist))
			{
				return;
			}
		}
		if (CurrentMapSections[r.MapSection])
		{
			// [Karto] Draw only the instances this traversal reaches: drawing every published one rasterized hidden rooms.
			if (GpuSprites && r.Instance >= 0)
			{
				const int blend = WorldModeGpuSpriteBlend(r.Instance);
				if (blend >= 0)
				{
					GpuSpriteVisible[blend].push_back(r.Instance);
					return;
				}
			}
			HWSprite sprite;
			sprite.ProcessRecord(this, r, sector, in_area, false);
		}
	};
	auto portalThing = [&](AActor* thing)
	{
		FIntCVar *cvar = thing->GetInfo()->distancecheck;
		if (cvar != nullptr && *cvar >= 0)
		{
			double dist = (thing->Pos() - vp.Pos).LengthSquared();
			double check = (double)**cvar;
			if (dist >= check * check)
			{
				return;
			}
		}

		HWSprite sprite;

		// [Nash] draw sprite shadow
		if (R_ShouldDrawSpriteShadow(thing))
		{
			double dist = (thing->Pos() - vp.Pos).LengthSquared();
			double check = r_actorspriteshadowdist;
			if (dist <= check * check)
			{
				sprite.Process(this, thing, sector, in_area, true, true);
			}
		}

		sprite.Process(this, thing, sector, in_area, true);
	};

	const FSnapshotThing* listed = nullptr;
	AActor* const* portal = nullptr;
	uint32_t listedCount = 0, portalCount = 0;
	if (UseActorSnapshot && WorldModeSnapshotThings(Level, sec, listed, listedCount, portal, portalCount))
	{
		// [Karto] Written once per publish; view-dependent tests (out-of-bounds secrets, distance cull) send the frame
		// down the full path.
		const FThingSplitEntry* split;
		uint32_t gpuCount, splitCount;
		if (r_worldmode_thingsplit && GpuSprites && !Viewpoint.bDoOob && r_distance_cull_type <= 0 &&
			WorldModeThingSplit(Level, sec, split, gpuCount, splitCount))
		{
			const uint32_t instances = WorldModeGpuInstanceCount();
			if (ThingStamps.size() != instances) ThingStamps.assign(instances, INT_MIN);
			for (uint32_t i = 0; i < gpuCount; ++i)
			{
				const FThingSplitEntry& e = split[i];
				if (ThingStamps[e.Instance] == rendervalidcount) continue;
				ThingStamps[e.Instance] = rendervalidcount;
				if (!CurrentMapSections[e.MapSection]) continue;
				if (WorldModeGpuFrameFailed(e.GpuFrame))
				{
					recordThing(*listed[e.Listed].Record);
					continue;
				}
				GpuSpriteVisible[e.Blend].push_back(int32_t(e.Instance));
			}
			for (uint32_t i = gpuCount; i < splitCount; ++i)
			{
				const FSnapshotThing& t = listed[split[i].Listed];
				if (t.Record != nullptr) recordThing(*t.Record);
				else listedThing(t.Copy);
			}
			for (uint32_t i = 0; i < portalCount; ++i) portalThing(portal[i]);
			return;
		}
		for (uint32_t i = 0; i < listedCount; ++i)
		{
			if (listed[i].Record != nullptr) recordThing(*listed[i].Record);
			else listedThing(listed[i].Copy);
		}
		for (uint32_t i = 0; i < portalCount; ++i) portalThing(portal[i]);
		return;
	}
	for (auto p = sec->touching_renderthings; p != nullptr; p = p->m_snext) listedThing(p->m_thing);
	for (msecnode_t *node = sec->sectorportal_thinglist; node; node = node->m_snext) portalThing(node->m_thing);
}

void HWDrawInfo::RenderParticles(subsector_t *sub, sector_t *front)
{
	FFrameStageScope stage(FS_ParticleCollect);
	SetupSprite.Clock();
	particle_t* particles;
	int first;
	DVisualThinker* const* visuals;
	unsigned visualCount;
	WorldModeSubsectorParticles(Level, sub, particles, first, visuals, visualCount);
	for (uint32_t i = 0; i < visualCount; i++)
	{
		DVisualThinker *sp = visuals[i];
		if (!sp || sp->ObjectFlags & OF_EuthanizeMe)
			continue;
		if (r_distance_cull_type > 0)
		{
			double dist = (sp->PT.Pos - (Viewpoint.bDoOob ? Viewpoint.OffPos : Viewpoint.Pos)).LengthSquared();
			if (dist >= Viewpoint.culldistsq)
			{
				continue;
			}
		}
		if (mClipPortal)
		{
			int clipres = mClipPortal->ClipPoint(sp->PT.Pos.XY());
			if (clipres == PClip_InFront) continue;
		}

		HWSprite sprite;
		sprite.ProcessParticle(this, &sp->PT, front, sp);
	}
	if (GpuParticles)
	{
		RenderGpuParticles(particles, first, front, sub);
		SetupSprite.Unclock();
		return;
	}
	for (int i = first; i != NO_PARTICLE; i = particles[i].snext)
	{
		if (r_distance_cull_type > 0)
		{
			double dist2 = (particles[i].Pos - (Viewpoint.bDoOob ? Viewpoint.OffPos : Viewpoint.Pos)).LengthSquared();
			if (dist2 >= Viewpoint.culldistsq)
			{
				continue;
			}
		}
		if (mClipPortal)
		{
			int clipres = mClipPortal->ClipPoint(particles[i].Pos.XY());
			if (clipres == PClip_InFront) continue;
		}

		HWSprite sprite;
		sprite.ProcessParticle(this, &particles[i], front, nullptr);
	}
	SetupSprite.Unclock();
}

// [Karto] GPU particles: a class's first particle carries the draw key and the rest are indices the GPU expands; chain
// order within a key is kept. Only the main view uses it: light slices, the distance cull and portal clipping are view
// dependent.
namespace
{
	enum : uint32_t { PP_Group = 1u << 24, PP_Single = 2u << 24, PP_CountMask = (1u << 24) - 1 };
	std::vector<uint32_t> ParticleProgram;		// per subsector: ops (op | count, then count particle indices)
	std::vector<uint32_t> ParticleProgramAt;	// per subsector: start in ParticleProgram + 1, 0 = not written
	std::vector<uint32_t> ParticleProgramEnd;
	uint64_t ParticleProgramSerial = 0;
	uint64_t ParticleProgramVersion = 0;	// changes with every clear or write, for the GPU copy
	int ParticleProgramStyle = -1;
	FLevelLocals* ParticleProgramLevel = nullptr;
}

CVAR(Bool, r_particle_program, true, 0)
CVAR(Bool, r_particle_programranges, true, 0)

void GpuParticleProgramStore(FGpuParticleIndices& out)
{
	out.Program = ParticleProgram.data();
	out.ProgramCount = uint32_t(ParticleProgram.size());
	out.ProgramVersion = ParticleProgramVersion;
}

// [Karto] Appends the subsector's program to out. Every particle field ProcessParticle's key reads must be part of the class.
static void WriteParticleProgram(const particle_t* particles, int first, bool sliced, const FRenderViewpoint& vp,
	HWPortal* clipPortal, std::vector<uint32_t>& out)
{
	struct FSlot { uint64_t Class; };
	static constexpr unsigned MaxSlots = 8;
	FSlot slots[MaxSlots];
	static std::vector<uint32_t> lists[MaxSlots];
	unsigned used = 0;
	auto flush = [&]()
	{
		for (unsigned k = 0; k < used; ++k)
		{
			out.push_back(PP_Group | uint32_t(lists[k].size()));
			out.insert(out.end(), lists[k].begin(), lists[k].end());
			lists[k].clear();
		}
		used = 0;
	};
	const DVector3& viewPos = vp.bDoOob ? vp.OffPos : vp.Pos;
	for (int i = first; i != NO_PARTICLE; i = particles[i].snext)
	{
		const particle_t& p = particles[i];
		if (p.alpha <= 0) continue;
		if (r_distance_cull_type > 0 && (p.Pos - viewPos).LengthSquared() >= vp.culldistsq) continue;
		if (clipPortal && clipPortal->ClipPoint(p.Pos.XY()) == PClip_InFront) continue;
		if (sliced || ((p.flags & SPF_LOCAL_ANIM) && p.animData.ok))
		{
			// [Karto] Own key (animation frame, light slice): nothing after it may join a group opened before it, or the order within a key changes.
			flush();
			out.push_back(PP_Single | 1u);
			out.push_back(uint32_t(i));
			continue;
		}
		const bool textured = p.texture.isValid();
		const int style = textured ? 2 : gl_particles_style;
		const bool solid = style != 2 && p.alpha >= 1.0f - FLT_EPSILON;
		const uint64_t cls = uint64_t(uint32_t(p.texture.GetIndex())) | (uint64_t(uint8_t(p.style)) << 32) |
			(uint64_t((p.flags & SPF_FULLBRIGHT) ? 1 : 0) << 40) | (uint64_t((p.flags & SPF_ALLOWSHADERS) ? 1 : 0) << 41) |
			(uint64_t(solid) << 42);
		unsigned k = 0;
		while (k < used && slots[k].Class != cls) ++k;
		if (k == used)
		{
			if (used == MaxSlots) { flush(); k = 0; }
			slots[k].Class = cls;
			++used;
		}
		lists[k].push_back(uint32_t(i));
	}
	flush();
}

void HWDrawInfo::RenderGpuParticles(particle_t* particles, int first, sector_t* front, subsector_t* sub)
{
	const bool sliced = front->e->XFloor.lightlist.Size() != 0;
	const bool cacheable = r_particle_program && !sliced && r_distance_cull_type <= 0 && mClipPortal == nullptr;
	static std::vector<uint32_t> live;
	const uint32_t* op;
	const uint32_t* end;
	if (cacheable)
	{
		if (ParticleProgramSerial != GpuParticleSerial || ParticleProgramStyle != gl_particles_style || ParticleProgramLevel != Level)
		{
			ParticleProgramSerial = GpuParticleSerial;
			ParticleProgramStyle = gl_particles_style;
			ParticleProgramLevel = Level;
			ParticleProgram.clear();
			++ParticleProgramVersion;
			ParticleProgramAt.assign(Level->subsectors.Size(), 0);
			ParticleProgramEnd.assign(Level->subsectors.Size(), 0);
		}
		const unsigned ss = sub->Index();
		if (ParticleProgramAt[ss] == 0)
		{
			ParticleProgramAt[ss] = uint32_t(ParticleProgram.size()) + 1;
			WriteParticleProgram(particles, first, false, Viewpoint, nullptr, ParticleProgram);
			ParticleProgramEnd[ss] = uint32_t(ParticleProgram.size());
			++ParticleProgramVersion;
		}
		op = ParticleProgram.data() + ParticleProgramAt[ss] - 1;
		end = ParticleProgram.data() + ParticleProgramEnd[ss];
	}
	else
	{
		live.clear();
		WriteParticleProgram(particles, first, sliced, Viewpoint, mClipPortal, live);
		op = live.data();
		end = live.data() + live.size();
	}
	while (op < end)
	{
		const uint32_t count = *op & PP_CountMask;
		const bool group = (*op & ~PP_CountMask) == PP_Group;
		const uint32_t* idx = op + 1;
		op = idx + count;
		if (!group)
		{
			HWSprite sprite;
			sprite.ProcessParticle(this, &particles[idx[0]], front, nullptr);
			continue;
		}
		const unsigned before = drawlists[GLDL_PARTICLES].Size() + drawlists[GLDL_SOLIDPARTICLES].Size() + drawlists[GLDL_ADDPARTICLES].Size();
		HWSprite sprite;
		sprite.GpuParticleRange = int32_t(GpuParticleRanges.size());
		sprite.ProcessParticle(this, &particles[idx[0]], front, nullptr);
		if (drawlists[GLDL_PARTICLES].Size() + drawlists[GLDL_SOLIDPARTICLES].Size() + drawlists[GLDL_ADDPARTICLES].Size() == before + 1)
		{
			if (cacheable && r_particle_programranges)
			{
				GpuParticleRanges.push_back({ GpuParticleProgramRange | uint32_t(idx - ParticleProgram.data()), count });
				continue;
			}
			GpuParticleRanges.push_back({ uint32_t(GpuParticleIndices.size()), count });
			for (uint32_t n = 0; n < count; ++n) GpuParticleIndices.push_back(uint16_t(idx[n]));
			continue;
		}
		// [Karto] A leader the bucket passes do not draw (desaturated or dynamically lit) went to another list; its class follows it there.
		for (uint32_t n = 1; n < count; ++n)
		{
			HWSprite follower;
			follower.ProcessParticle(this, &particles[idx[n]], front, nullptr);
		}
	}
}


//==========================================================================
//
// R_Subsector
// Determine floor/ceiling planes.
// Add sprites of things in sector.
// Draw one or more line segments.
//
//==========================================================================

void HWDrawInfo::DoSubsector(subsector_t * sub)
{
	const bool profile = SceneProfiling;
	if (profile) ++SceneTraversalProfile.Subsectors;
	auto reject = [&]()
	{
		if (profile) ++SceneTraversalProfile.SubsectorRejected;
	};
	const bool doOob = Viewpoint.bDoOob;
	sector_t * sector;
	sector_t * fakesector;

#ifdef _DEBUG
	if (sub->sector->sectornum==931)
	{
		int a = 0;
	}
#endif

	sector=sub->sector;
	if (!sector)
	{
		reject();
		return;
	}

	// If the mapsections differ this subsector can't possibly be visible from the current view point
	if (!CurrentMapSections[sub->mapsection] || (sub->flags & SSECF_POLYORG))
	{
		reject();
		return;
	}

	if (ss_renderflags[sub->Index()] & SSRF_SEEN)
	{
		// This means that we have reached a subsector in a portal that has been marked 'seen'
		// from the other side of the portal. This means we must clear the clipper for the
		// range this subsector spans before going on.
		UnclipSubsector(sub);
	}
	if (mClipper->IsBlocked())
	{
		reject();
		return;
	}

	fakesector=hw_FakeFlat(sector, in_area, false);

	if (doOob && sector->isSecret() && sector->wasSecret() && !r_radarclipper)
	{
		reject();
		return;
	}

	// cull everything if subsector outside all relevant clippers
	if (doOob && (sub->polys == nullptr))
	{
	auto &clipper = *mClipper;
		auto &clipperv = *vClipper;
		auto &clipperr = *rClipper;
		int count = sub->numlines;
		seg_t * seg = sub->firstline;
		bool anglevisible = false;
		bool pitchvisible = false;
		bool radarvisible = !r_radarclipper || (Level->flags3 & LEVEL3_NOFOGOFWAR) || ((sub->flags & SSECMF_DRAWN) && !deathmatch);
		const int reflect = mCurrentPortal ? mCurrentPortal->GetMirrorSide() : 0; // -1 = ceiling, 1 = floor, 0 = none
		const double planez = (reflect < 0 ? sector->ceilingplane.ZatPoint(Viewpoint.Pos) : sector->floorplane.ZatPoint(Viewpoint.Pos));
		angle_t pitchtemp;
		angle_t pitchmin = ANGLE_90;
		angle_t pitchmax = 0;

		while (count--)
		{
			if((seg->v1 != nullptr) && (seg->v2 != nullptr))
			{
				if (!anglevisible)
				{
					angle_t startAngle = clipper.GetClipAngle(seg->v2);
					angle_t endAngle = clipper.GetClipAngle(seg->v1);
					if (startAngle-endAngle >= ANGLE_180) anglevisible |= clipper.SafeCheckRange(startAngle, endAngle);
				}
				if (!radarvisible)
				{
					angle_t startAngleR = clipperr.PointToPseudoAngle(seg->v2->fX(), seg->v2->fY());
					angle_t endAngleR = clipperr.PointToPseudoAngle(seg->v1->fX(), seg->v1->fY());
					if (startAngleR-endAngleR >= ANGLE_180) radarvisible |= clipperr.SafeCheckRange(startAngleR, endAngleR);
				}

				if (!pitchvisible)
				{
					pitchmin = clipperv.PointToPseudoPitch(seg->v1->fX(), seg->v1->fY(),
														   (reflect != 0) ?
														   2 * planez - sector->floorplane.ZatPoint(seg->v1) :
														   sector->floorplane.ZatPoint(seg->v1));
					pitchmax = clipperv.PointToPseudoPitch(seg->v1->fX(), seg->v1->fY(),
														   (reflect != 0) ?
														   2 * planez - sector->ceilingplane.ZatPoint(seg->v1) :
														   sector->ceilingplane.ZatPoint(seg->v1));
					pitchvisible |= clipperv.SafeCheckRange(pitchmin, pitchmax);
				}
				if (pitchvisible && anglevisible && radarvisible) break;
				if (!pitchvisible)
				{
					pitchtemp = clipperv.PointToPseudoPitch(seg->v2->fX(), seg->v2->fY(),
															(reflect != 0) ?
															2 * planez - sector->floorplane.ZatPoint(seg->v2) :
															sector->floorplane.ZatPoint(seg->v2));
					if (int(pitchmin) > int(pitchtemp)) pitchmin = pitchtemp;
					pitchtemp = clipperv.PointToPseudoPitch(seg->v2->fX(), seg->v2->fY(),
															(reflect != 0) ?
															2 * planez - sector->ceilingplane.ZatPoint(seg->v2) :
															sector->ceilingplane.ZatPoint(seg->v2));
					if (int(pitchmax) < int(pitchtemp)) pitchmax = pitchtemp;
					pitchvisible |= clipperv.SafeCheckRange(pitchmin, pitchmax);
				}
				if (pitchvisible && anglevisible && radarvisible) break;
			}
			seg++;
		}
		// Skip subsector if outside vertical or horizontal clippers or is in unexplored territory (fog of war)
		if (!pitchvisible || !anglevisible || (!radarvisible && r_radarclipper))
		{
			reject();
			return;
		}
	}

	if (mClipPortal)
	{
		int clipres = mClipPortal->ClipSubsector(sub);
		if (clipres == PClip_InFront)
		{
			auto line = mClipPortal->ClipLine();
			// The subsector is out of range, but we still have to check lines that lie directly on the boundary and may expose their upper or lower parts.
			if (line) AddSpecialPortalLines(sub, fakesector, line);
			reject();
			return;
		}
	}

	if (sector->validcount != rendervalidcount)
	{
		if (profile) ++SceneTraversalProfile.UniqueSectors;
		CheckUpdate(screen->mVertexData, sector);
	}
	if (profile) ++SceneTraversalProfile.SubsectorEntered;

	// [RH] Add particles
	particle_t* subParticles;
	int subFirst;
	DVisualThinker* const* subVisuals;
	unsigned subVisualCount;
	if (gl_render_things && !WorldSideEffectsOnly && WorldModeSubsectorParticles(Level, sub, subParticles, subFirst, subVisuals, subVisualCount))
	{
		if (multithread)
		{
			jobQueue.AddJob(RenderJob::ParticleJob, sub, nullptr);
		}
		else
		{
			SetupSprite.Clock();
			RenderParticles(sub, fakesector);
			SetupSprite.Unclock();
		}
	}

	AddLines(sub, fakesector);

	// BSP is traversed by subsector.
	// A sector might have been split into several
	//	subsectors during BSP building.
	// Thus we check whether it was already added.
	if (sector->validcount != rendervalidcount)
	{
		// Well, now it will be done.
		sector->validcount = rendervalidcount;
		sector->MoreFlags |= SECMF_DRAWN;
		if (SceneProfiling) ++SceneTraversalProfile.SectorDrawnWrites;

		if (gl_render_things && !WorldSideEffectsOnly && (sector->touching_renderthings || sector->sectorportal_thinglist))
		{
			if (multithread)
			{
				jobQueue.AddJob(RenderJob::SpriteJob, sub, nullptr);
			}
			else
			{
				SetupSprite.Clock();
				RenderThings(sub, fakesector);
				SetupSprite.Unclock();
			}
		}
		if (r_dithertransparency && doOob && (RTnum < MAXDITHERACTORS) && mCurrentPortal == nullptr)
		{
			// [DVR] Not parallelizable due to variables RTnum and RenderedTargets[]
			for (auto p = sector->touching_renderthings; p != nullptr; p = p->m_snext)
			{
				auto thing = p->m_thing;
				if (thing->validcount == rendervalidcount) continue; // Don't double count
				if (((thing->flags3 & MF3_ISMONSTER) && !(thing->flags & MF_CORPSE)) || (thing->flags & MF_MISSILE))
				{
					if (RTnum < MAXDITHERACTORS) RenderedTargets[RTnum++] = thing;
					else break;
				}
			}
		}
	}

	if (gl_render_flats && !WorldSideEffectsOnly)
	{
		// Subsectors with only 2 lines cannot have any area
		if (sub->numlines>2 || (sub->hacked&1))
		{
			// Exclude the case when it tries to render a sector with a heightsec
			// but undetermined heightsec state. This can only happen if the
			// subsector is obstructed but not excluded due to a large bounding box.
			// Due to the way a BSP works such a subsector can never be visible
			if (!sector->GetHeightSec() || in_area!=area_default)
			{
				if (sector != sub->render_sector)
				{
					sector = sub->render_sector;
					// the planes of this subsector are faked to belong to another sector
					// This means we need the heightsec parts and light info of the render sector, not the actual one.
					fakesector = hw_FakeFlat(sector, in_area, false);
				}

				uint8_t &srf = section_renderflags[Level->sections.SectionIndex(sub->section)];
				if (!(srf & SSRF_PROCESSED))
				{
					srf |= SSRF_PROCESSED;

					if (multithread)
					{
						jobQueue.AddJob(RenderJob::FlatJob, sub);
					}
					else
					{
						HWFlat flat;
						flat.section = sub->section;
						SetupFlat.Clock();
						flat.ProcessSector(this, fakesector);
						SetupFlat.Unclock();
					}
				}
				// mark subsector as processed - but mark for rendering only if it has an actual area.
				ss_renderflags[sub->Index()] =
					(sub->numlines > 2) ? SSRF_PROCESSED|SSRF_RENDERALL : SSRF_PROCESSED;
				if (sub->hacked & 1) AddHackedSubsector(sub);

				// This is for portal coverage.
				FSectorPortalGroup *portal;

				// AddSubsectorToPortal cannot be called here when using multithreaded processing,
				// because the wall processing code in the worker can also modify the portal state.
				// To avoid costly synchronization for every access to the portal list,
				// the call to AddSubsectorToPortal will be deferred to the worker.
				// (GetPortalGruop only accesses static sector data so this check can be done here, restricting the new job to the minimum possible extent.)
				portal = fakesector->GetPortalGroup(sector_t::ceiling);
				if (portal != nullptr)
				{
					if (multithread)
					{
						jobQueue.AddJob(RenderJob::PortalJob, sub, (seg_t *)portal);
					}
					else
					{
						AddSubsectorToPortal(portal, sub); if (SceneProfiling) ++SceneTraversalProfile.PortalRegistrations;
					}
				}

				portal = fakesector->GetPortalGroup(sector_t::floor);
				if (portal != nullptr)
				{
					if (multithread)
					{
						jobQueue.AddJob(RenderJob::PortalJob, sub, (seg_t *)portal);
					}
					else
					{
						AddSubsectorToPortal(portal, sub); if (SceneProfiling) ++SceneTraversalProfile.PortalRegistrations;
					}
				}
			}
		}
	}
}




//==========================================================================
//
// RenderBSPNode
// Renders all subsectors below a given node,
//  traversing subtree recursively.
// Just call with BSP root.
//
//==========================================================================

void HWDrawInfo::RenderBSPNode (void *node)
{
	const bool profile = SceneProfiling;
	if (Level->nodes.Size() == 0)
	{
		DoSubsector (&Level->subsectors[0]);
		return;
	}
	while (!((size_t)node & 1))  // Keep going until found a subsector
	{
		if (profile) ++SceneTraversalProfile.Nodes;
		node_t *bsp = (node_t *)node;

		// Decide which side the view point is on.
		int side = R_PointOnSide(viewx, viewy, bsp);

		// Recursively divide front space (toward the viewer).
		RenderBSPNode (bsp->children[side]);

		// Possibly divide back space (away from the viewer).
		side ^= 1;

		// It is not necessary to use the slower precise version here
		if (profile) ++SceneTraversalProfile.BoxChecks;
		if (!mClipper->CheckBox(bsp->bbox[side]))
		{
			if (!(no_renderflags[bsp->Index()] & SSRF_SEEN))
			{
				if (profile) ++SceneTraversalProfile.BoxRejected;
				return;
			}
		}
		if (Viewpoint.bDoOrtho)
		{
			if (!vClipper->CheckBoxOrthoPitch(bsp->bbox[side]))
			{
				if (!(no_renderflags[bsp->Index()] & SSRF_SEEN))
					return;
			}
		}

		node = bsp->children[side];
	}
	DoSubsector ((subsector_t *)((uint8_t *)node - 1));
}

// No need for clipping inside frustum if no fog of war (How is this faster!)
void HWDrawInfo::RenderOrthoNoFog()
{
	if (Viewpoint.bDoOrtho && ((Level->flags3 & LEVEL3_NOFOGOFWAR) || !r_radarclipper))
	{
		double vxdbl = Viewpoint.OffPos.X;
		double vydbl = Viewpoint.OffPos.Y;
		double ext = Viewpoint.camera->ViewPos->Offset.Length() ?
			3.0 * Viewpoint.camera->ViewPos->Offset.Length() * tan (Viewpoint.FieldOfView.Radians()*0.5) : 100.0;
		FBoundingBox viewbox(vxdbl, vydbl, ext);

		for (unsigned int kk = 0; kk < Level->subsectors.Size(); kk++)
		{
			if (Level->subsectors[kk].bbox.CheckOverlap(viewbox))
			{
				DoSubsector (&Level->subsectors[kk]);
			}
		}
	}
}

void HWDrawInfo::GPUBakeSweepWalls()
{
	if (!vk_gpu_driven_wallbake || !vk_gpu_driven_bake || !Level) return;
	if (BakeSweptLevel == (const void*)Level) return;
	BakeSweptLevel = Level;
	HWMeshHelper mesh;
	HWWallDispatcher disp(Level, &mesh, lightmode);
	uint64_t swept = 0;
	for (auto& seg : Level->segs)
	{
		if (!seg.sidedef || !seg.frontsector) continue;
		HWWall wall;
		wall.sub = seg.Subsector;
		sector_t* front = hw_FakeFlat(seg.frontsector, in_area, false);
		sector_t* back = nullptr;
		sector_t* backsector = seg.backsector;
		if (backsector)
		{
			if (front->sectornum == backsector->sectornum || (seg.sidedef->Flags & WALLF_POLYOBJ)) back = front;
			else back = hw_FakeFlat(backsector, in_area, true);
		}
		wall.Process(&disp, &seg, front, back, false);
		++swept;
	}
	g_sweepMvCalls = g_sweepBaked = g_sweepSkipNoTex = g_sweepSkipSplit = g_sweepSkipCanCache = g_sweepSkipOther = 0;
	g_bakeSweepActive = true;
	for (auto& w : mesh.list) w.MakeVertices(false);
	g_bakeSweepActive = false;
	Printf("Wall bake sweep: segs=%llu produced=%llu mvCalls=%llu baked=%llu skipNoTex=%llu skipSplit=%llu skipCanCache=%llu skipOther=%llu\n",
		static_cast<unsigned long long>(swept), static_cast<unsigned long long>(mesh.list.Size()),
		static_cast<unsigned long long>(g_sweepMvCalls), static_cast<unsigned long long>(g_sweepBaked),
		static_cast<unsigned long long>(g_sweepSkipNoTex), static_cast<unsigned long long>(g_sweepSkipSplit),
		static_cast<unsigned long long>(g_sweepSkipCanCache), static_cast<unsigned long long>(g_sweepSkipOther));
}

void SpriteFrameCensus(HWDrawInfo* di);
void ModelFrameCensus();

void HWDrawInfo::RenderBSP(void *node, bool drawpsprites)
{
	GPUBakeSweepWalls();
	// [Karto] World mode bakes once per level and otherwise forgets it, so stale bindless IDs never replay.
	if (r_worldmode) BakeWorldMode(); else ForgetWorldMode();
	const bool worldMode = WorldModeActive(Level);
	if (worldMode && drawpsprites && !mCurrentPortal)
	{
		FFrameStageScope stage(FS_UpdateWorldMode);
		UpdateWorldMode();
	}
	WorldSideEffectsOnly = worldMode && WorldModeSideEffectsDue(Level, drawpsprites && !mCurrentPortal);
	const bool skipWorld = r_ceiling_noworld || (worldMode && !WorldSideEffectsOnly);
	// [Karto] A native walk shares the clipper's vertex angle cache with the automap walker; never overlap them.
	if (!skipWorld) WorldModeWalkerWait();
	BeginRetainedWorldView(Level);
	OwnOpaqueWalls = CanOwnOpaqueWalls(this);
	OwnedNextOrder = 1;
	if (SceneProfiling)
	{
		if (OwnOpaqueWalls) ++SceneMainSurfaceProfile.OwnedViews;
		++SceneTraversalProfile.Views;
		if (mClipPortal != nullptr) ++SceneTraversalProfile.ClipPortalViews;
		if (gl_multithread) ++SceneTraversalProfile.ThreadedViews;
	}
	ClearDitherTargets();
	if (vk_gpu_driven_compat)
	{
		const size_t coverageBytes = (Level->lines.Size() + 7) / 8;
		g_coveragePending.assign(coverageBytes, 0);
	}
	Bsp.Clock();

	// Give the DrawInfo the viewpoint in fixed point because that's what the nodes are.
	viewx = FLOAT2FIXED(Viewpoint.Pos.X);
	viewy = FLOAT2FIXED(Viewpoint.Pos.Y);
	if (r_radarclipper && !(Level->flags3 & LEVEL3_NOFOGOFWAR) && Viewpoint.bDoOob)
	{
		viewx = FLOAT2FIXED(Viewpoint.OffPos.X);
		viewy = FLOAT2FIXED(Viewpoint.OffPos.Y);
	}

	rendervalidcount++;	// used for processing sidedefs only once by the renderer.

	multithread = gl_multithread;
	if (multithread)
	{
		jobQueue.ReleaseAll();
		std::future<void> future = renderPool.push([&](int id) { WorkerThread(); });
		if (skipWorld) {}
		else if (Viewpoint.bDoOrtho && ((Level->flags3 & LEVEL3_NOFOGOFWAR) || !r_radarclipper)) RenderOrthoNoFog();
		else RenderBSPNode(node);
		jobQueue.AddJob(RenderJob::TerminateJob, nullptr, nullptr);
		Bsp.Unclock();
		MTWait.Clock();
		future.wait();
		MTWait.Unclock();
	}
	else
	{
		if (skipWorld) {}
		else if (Viewpoint.bDoOrtho && ((Level->flags3 & LEVEL3_NOFOGOFWAR) || !r_radarclipper)) RenderOrthoNoFog();
		else RenderBSPNode(node);
		Bsp.Unclock();
	}
	WorldSideEffectsOnly = false;
	// [Karto] The save picture is a second top-level view of the same camera, without psprites; it needs the same
	// sprites and sky as the frame.
	if (worldMode && !mCurrentPortal && (drawpsprites || outer == nullptr))
	{
		if (drawpsprites)
		{
			FFrameStageScope stage(FS_Walker);
			WorldModeWalkerFrame();
		}
		{
			FFrameStageScope stage(FS_Sprites);
			CollectWorldModeSprites();
		}
		SpriteFrameCensus(this);
		ModelFrameCensus();
		{
			FFrameStageScope stage(FS_Sky);
			ReplayWorldModeSky();
		}
		ReplayWorldModeHorizons();
		ReplayWorldModeTranslucentWalls();
		CollectWorldModeMissingTextures();
	}

	// Make rendered targets set dither transparency flags on level geometry for next pass
	// Can't do this inside DoSubsector() because both Trace() and P_CheckSight() affect 'rendervalidcount' global variable
	{
		cycle_t ditherTimer;
		const bool timeDither = SceneProfiling;
		if (timeDither) { ditherTimer.ResetAndClock(); for (int ii = 0; ii < MAXDITHERACTORS; ii++) if (RenderedTargets[ii]) ++SceneTraversalProfile.DitherCandidates; }
		for (int ii = 0; ii < MAXDITHERACTORS; ii++)
		{
			if ( RenderedTargets[ii] && P_CheckSight(players[consoleplayer].mo, RenderedTargets[ii], 0) )
			{
				SetDitherTransFlags(RenderedTargets[ii]);
				if (timeDither) ++SceneTraversalProfile.DitherTargetsSet;
			}
		}
		if (timeDither) { ditherTimer.Unclock(); SceneTraversalProfile.DitherTraceNS += uint64_t(ditherTimer.Time() * 1e9); }
	}

	if (vk_gpu_driven_compat)
		screen->RenderState()->UploadCoverage(g_coveragePending.data(), g_coveragePending.size());

	// Process all the sprites on the current portal's back side which touch the portal.
	if (mCurrentPortal != nullptr) mCurrentPortal->RenderAttached(this);

	if (drawpsprites)
		PreparePlayerSprites(WorldModePublishedSector(Viewpoint.sector), in_area);
}
