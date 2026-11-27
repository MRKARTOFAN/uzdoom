// [Karto] Keep in step with HWDrawInfo::RenderBSPNode/DoSubsector/AddLine (hw_bsp.cpp): this mirrors their clipping,
// minus production, portals, radar and out-of-bounds views.

#include "g_levellocals.h"
#include "r_utility.h"
#include "texturemanager.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_clipper.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "i_time.h"
#include "printf.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

CVAR(Bool, r_worldmode_automap, true, 0)
CVAR(Bool, r_worldmode_occlusion, true, 0)
EXTERN_CVAR(Float, r_worldmode_occlusion_slack)
CVAR(Int, r_worldmode_occlusion_adaptive, 1000, 0)

namespace
{
	struct FWalkerSnapshot
	{
		FLevelLocals* Level = nullptr;
		int StartTime = 0;
		FRenderViewpoint Viewpoint;
		BitArray MapSections;
		area_t InArea = area_default;
		bool Automap = false, Around = false;
	};

	struct FWalkerResult
	{
		FLevelLocals* Level = nullptr;
		int StartTime = 0;
		std::vector<int> Lines, Subsectors, Sectors;
		std::vector<int> AroundSectors;
		DVector2 AroundFrom;
		bool Automap = false, Around = false;
		uint64_t NS = 0, AroundNS = 0;
	};

	class FWorldModeWalker
	{
	public:
		~FWorldModeWalker() { Stop(); }

		// Returns false while a pass is queued or running; the caller simply tries again next tic.
		bool Submit(FWalkerSnapshot&& snapshot)
		{
			std::unique_lock<std::mutex> lock(Mutex);
			if (!Thread.joinable()) Thread = std::thread([this] { Run(); });
			if (HasJob || Busy) return false;
			Job = std::move(snapshot);
			HasJob = true;
			Wake.notify_one();
			return true;
		}

		bool TakeResult(FWalkerResult& out)
		{
			std::lock_guard<std::mutex> lock(Mutex);
			if (!HasResult) return false;
			out = std::move(Result);
			HasResult = false;
			return true;
		}

		// [Karto] The clipper's angle cache lives on shared vertices; a native walk must never overlap a walker pass.
		void WaitIdle()
		{
			std::unique_lock<std::mutex> lock(Mutex);
			Done.wait(lock, [this] { return !HasJob && !Busy; });
		}

		void Stop()
		{
			{
				std::lock_guard<std::mutex> lock(Mutex);
				if (!Thread.joinable()) return;
				Quit = true;
				Wake.notify_one();
			}
			Thread.join();
			std::lock_guard<std::mutex> lock(Mutex);
			Quit = HasJob = Busy = HasResult = false;
			Result = {};
		}

		uint64_t Passes = 0, TotalNS = 0, LastNS = 0, AppliedWrites = 0, LastLines = 0;
		uint64_t AroundPasses = 0, AroundTotalNS = 0, LastAroundNS = 0, LastAroundSectors = 0;

	private:
		std::thread Thread;
		std::mutex Mutex;
		std::condition_variable Wake, Done;
		bool Quit = false, HasJob = false, Busy = false, HasResult = false;
		FWalkerSnapshot Job, Current;
		FWalkerResult Result, Work;

		Clipper Clip;
		std::vector<uint8_t> LineSeen, SubSeen, SectorSeen;
		sector_t FakeFront, FakeBack;
		sector_t* CurrentSector = nullptr;
		subsector_t* CurrentSub = nullptr;
		area_t InArea = area_default;
		fixed_t ViewX = 0, ViewY = 0;
		bool AroundMode = false;

		void Run()
		{
			hw_FakeFlatPrivate = true;
			std::unique_lock<std::mutex> lock(Mutex);
			for (;;)
			{
				Wake.wait(lock, [this] { return Quit || HasJob; });
				if (Quit) return;
				Current = std::move(Job);
				HasJob = false;
				Busy = true;
				lock.unlock();
				const uint64_t started = I_nsTime();
				Work.Automap = Current.Automap;
				Work.Around = Current.Around;
				if (Current.Automap) Walk();
				const uint64_t aroundStarted = I_nsTime();
				Work.NS = aroundStarted - started;
				if (Current.Around)
				{
					Work.AroundFrom = Current.Viewpoint.Pos.XY();
					WalkAround();
				}
				Work.AroundNS = I_nsTime() - aroundStarted;
				lock.lock();
				Result = std::move(Work);
				Work = {};
				HasResult = true;
				Busy = false;
				if (Result.Automap)
				{
					++Passes;
					LastNS = Result.NS;
					TotalNS += Result.NS;
				}
				if (Result.Around)
				{
					++AroundPasses;
					LastAroundNS = Result.AroundNS;
					AroundTotalNS += Result.AroundNS;
					LastAroundSectors = Result.AroundSectors.size();
				}
				Done.notify_all();
			}
		}

		// [Karto] No frustum seed: what the occluders leave open from here in any direction, so turning within a tic
		// cannot reveal a culled sector; the per-frame frustum test still handles direction.
		void WalkAround()
		{
			auto level = Current.Level;
			Work.Level = level;
			Work.StartTime = Current.StartTime;
			SectorSeen.assign(level->sectors.Size(), 0);
			Clip.Clear();
			Clip.SetViewpoint(Current.Viewpoint);
			ViewX = FLOAT2FIXED(Current.Viewpoint.Pos.X);
			ViewY = FLOAT2FIXED(Current.Viewpoint.Pos.Y);
			InArea = Current.InArea;
			AroundMode = true;
			if (level->nodes.Size() == 0) Subsector(&level->subsectors[0]);
			else Node(level->HeadNode());
			AroundMode = false;
		}

		void Walk()
		{
			auto level = Current.Level;
			Work.Level = level;
			Work.StartTime = Current.StartTime;
			LineSeen.assign(level->lines.Size(), 0);
			SubSeen.assign(level->subsectors.Size(), 0);
			SectorSeen.assign(level->sectors.Size(), 0);
			Clip.Clear();
			Clip.SetViewpoint(Current.Viewpoint);
			// Same frustum seed as HWDrawInfo::CreateScene.
			const angle_t yaw = Current.Viewpoint.Angles.Yaw.BAMs();
			Clip.SafeAddClipRangeRealAngles(yaw + Current.Viewpoint.FrustAngle, yaw - Current.Viewpoint.FrustAngle);
			ViewX = FLOAT2FIXED(Current.Viewpoint.Pos.X);
			ViewY = FLOAT2FIXED(Current.Viewpoint.Pos.Y);
			InArea = Current.InArea;
			if (level->nodes.Size() == 0) Subsector(&level->subsectors[0]);
			else Node(level->HeadNode());
		}

		void Node(void* node)
		{
			while (!((size_t)node & 1))
			{
				node_t* bsp = (node_t*)node;
				int side = R_PointOnSide(ViewX, ViewY, bsp);
				Node(bsp->children[side]);
				side ^= 1;
				if (!Clip.CheckBox(bsp->bbox[side])) return;
				node = bsp->children[side];
			}
			Subsector((subsector_t*)((uint8_t*)node - 1));
		}

		bool SubDrawn(subsector_t* sub) const
		{
			// The live flag is read only as a shortcut; a stale read just repeats an idempotent mark.
			return SubSeen[sub->Index()] || (sub->flags & SSECMF_DRAWN);
		}

		void MarkSub(subsector_t* sub)
		{
			auto& seen = SubSeen[sub->Index()];
			if (!seen) { seen = 1; Work.Subsectors.push_back(sub->Index()); }
		}

		void Subsector(subsector_t* sub)
		{
			sector_t* sector = sub->sector;
			if (!sector) return;
			if (!Current.MapSections[sub->mapsection] || (sub->flags & SSECF_POLYORG)) return;
			if (Clip.IsBlocked()) return;
			CurrentSector = hw_FakeFlat(sector, InArea, false, &FakeFront);
			CurrentSub = sub;
			int count = sub->numlines;
			seg_t* seg = sub->firstline;
			while (count--)
			{
				if (seg->linedef == nullptr) { if (!AroundMode && !SubDrawn(sub)) Line(seg); }
				else if (!(seg->sidedef->Flags & WALLF_POLYOBJ)) Line(seg);
				seg++;
			}
			auto& sectorSeen = SectorSeen[sector->Index()];
			if (!sectorSeen) { sectorSeen = 1; (AroundMode ? Work.AroundSectors : Work.Sectors).push_back(sector->Index()); }
		}

		void Line(seg_t* seg)
		{
			const angle_t startAngle = Clip.GetClipAngle(seg->v2);
			const angle_t endAngle = Clip.GetClipAngle(seg->v1);
			if (startAngle - endAngle < ANGLE_180) return;	// back side
			if (seg->sidedef == nullptr)
			{
				if (!AroundMode && !SubDrawn(CurrentSub) && Clip.SafeCheckRange(startAngle, endAngle)) MarkSub(CurrentSub);
				return;
			}
			if (!Clip.SafeCheckRange(startAngle, endAngle)) return;
			if (!AroundMode) MarkSub(CurrentSub);
			if (!seg->backsector || (seg->sidedef->Flags & WALLF_BLOCKRENDERING))
			{
				if (!(seg->sidedef->Flags & WALLF_DITHERTRANS_MID)) Clip.SafeAddClipRange(startAngle, endAngle);
			}
			else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
			{
				if (CurrentSector->sectornum == seg->backsector->sectornum)
				{
					if (!seg->linedef->isVisualPortal())
					{
						auto tex = TexMan.GetGameTexture(seg->sidedef->GetTexture(side_t::mid), true);
						if (!tex || !tex->isValid()) return;	// native returns before mapping such lines
					}
				}
				else
				{
					if (InArea == area_default) InArea = hw_CheckViewArea(seg->v1, seg->v2, seg->frontsector, seg->backsector);
					sector_t* back = hw_FakeFlat(seg->backsector, InArea, true, &FakeBack);
					if (hw_CheckClip(seg->sidedef, CurrentSector, back) && !(seg->sidedef->Flags & WALLF_DITHERTRANS_MID))
						Clip.SafeAddClipRange(startAngle, endAngle);
				}
			}
			if (AroundMode) return;
			const int line = seg->linedef->Index();
			if (!LineSeen[line]) { LineSeen[line] = 1; Work.Lines.push_back(line); }
		}
	};

	FWorldModeWalker Walker;
	int LastSubmittedTic = -1;
	FLevelLocals* LastLevel = nullptr;
	std::vector<uint8_t> OpenSector;
	FLevelLocals* OpenLevel = nullptr;
	int OpenStartTime = 0;
	DVector2 OpenFrom;
	std::vector<uint8_t> WalkSector;
	FLevelLocals* WalkLevel = nullptr;
	int WalkStartTime = 0;
	int LastAroundTic = -1;
	DVector2 LastAroundFrom;
	constexpr int AdaptiveWindow = 10, AdaptiveSwitch = 3, AdaptiveIdleTics = 8;
	constexpr double AdaptiveBand = 0.05;
	double AroundTimes[AdaptiveWindow] = {};
	int AroundTimeCount = 0, AroundTimeNext = 0, AdaptiveAbove = 0, AdaptiveBelow = 0;
	bool AdaptiveOn = true;
	double AdaptiveMedian = 0.;
	uint64_t AdaptiveSwitches = 0;

	bool OcclusionWanted()
	{
		return r_worldmode_occlusion && (r_worldmode_occlusion_adaptive <= 0 || AdaptiveOn);
	}

	void AdaptiveSample(double us)
	{
		AroundTimes[AroundTimeNext] = us;
		AroundTimeNext = (AroundTimeNext + 1) % AdaptiveWindow;
		AroundTimeCount = std::min(AroundTimeCount + 1, AdaptiveWindow);
		double sorted[AdaptiveWindow];
		std::copy(AroundTimes, AroundTimes + AroundTimeCount, sorted);
		std::sort(sorted, sorted + AroundTimeCount);
		AdaptiveMedian = sorted[AroundTimeCount / 2];
		const double threshold = r_worldmode_occlusion_adaptive;
		if (threshold <= 0) return;
		if (AdaptiveMedian < threshold * (1 - AdaptiveBand)) { ++AdaptiveBelow; AdaptiveAbove = 0; }
		else if (AdaptiveMedian > threshold * (1 + AdaptiveBand)) { ++AdaptiveAbove; AdaptiveBelow = 0; }
		if (!AdaptiveOn && AdaptiveBelow >= AdaptiveSwitch) { AdaptiveOn = true; ++AdaptiveSwitches; }
		else if (AdaptiveOn && AdaptiveAbove >= AdaptiveSwitch) { AdaptiveOn = false; ++AdaptiveSwitches; }
	}

	void AdaptiveReset()
	{
		AroundTimeCount = AroundTimeNext = AdaptiveAbove = AdaptiveBelow = 0;
		AdaptiveOn = true;
		AdaptiveMedian = 0.;
	}
}

bool WorldModeWalkSectors(FLevelLocals* level, const uint8_t*& entered)
{
	if (WalkLevel != level || WalkStartTime != level->starttime || WalkSector.size() != level->sectors.Size()) return false;
	entered = WalkSector.data();
	return true;
}

bool WorldModeOpenSectors(FLevelLocals* level, const uint8_t*& open, DVector2& from)
{
	if (!OcclusionWanted() || OpenLevel != level || OpenStartTime != level->starttime || OpenSector.size() != level->sectors.Size()) return false;
	open = OpenSector.data();
	from = OpenFrom;
	return true;
}

void WorldModeWalkerWait()
{
	Walker.WaitIdle();
}

void WorldModeWalkerStop()
{
	Walker.Stop();
	LastSubmittedTic = -1;
	LastLevel = nullptr;
	OpenSector.clear();
	OpenLevel = nullptr;
}

bool WorldModeAutomapFromWalker()
{
	return r_worldmode_automap;
}

void HWDrawInfo::WorldModeWalkerFrame()
{
	if (!r_worldmode_automap && !r_worldmode_occlusion) return;
	FWalkerResult result;
	if (Walker.TakeResult(result) && result.Level == Level && result.StartTime == Level->starttime)
	{
		if (result.Around)
		{
			OpenSector.assign(Level->sectors.Size(), 0);
			for (int index : result.AroundSectors) OpenSector[index] = 1;
			OpenLevel = Level;
			OpenStartTime = Level->starttime;
			OpenFrom = result.AroundFrom;
			AdaptiveSample(result.AroundNS / 1000.);
		}
		// [Karto] Only the main thread writes level flags; game code writes line.flags too, so a worker write could lose it.
		for (int index : result.Lines)
		{
			if (!(Level->lines[index].flags & ML_MAPPED)) { WorldModeMarkLine(Level, index); ++Walker.AppliedWrites; }
		}
		for (int index : result.Subsectors)
		{
			if (!(Level->subsectors[index].flags & SSECMF_DRAWN)) { WorldModeMarkSubsector(Level, index); ++Walker.AppliedWrites; }
		}
		for (int index : result.Sectors)
		{
			if (!(Level->sectors[index].MoreFlags & SECMF_DRAWN)) { WorldModeMarkSector(Level, index); ++Walker.AppliedWrites; }
		}
		if (result.Automap)
		{
			Walker.LastLines = result.Lines.size();
			WalkSector.assign(Level->sectors.Size(), 0);
			for (int index : result.Sectors) WalkSector[index] = 1;
			WalkLevel = Level;
			WalkStartTime = Level->starttime;
		}
	}
	if (LastLevel != Level) { LastLevel = Level; LastSubmittedTic = -1; LastAroundTic = -1; AdaptiveReset(); }
	// [Karto] Scheduled by the tic the frame draws, not the live maptime a worker may be writing.
	const int frameTic = WorldModeFrameTic(Level);
	const bool automapDue = r_worldmode_automap && frameTic != LastSubmittedTic;
	const double half = 0.5 * r_worldmode_occlusion_slack;
	const bool aroundDue = r_worldmode_occlusion && (OcclusionWanted() ?
		(frameTic != LastAroundTic || (Viewpoint.Pos.XY() - LastAroundFrom).LengthSquared() > half * half) :
		(LastAroundTic < 0 || frameTic - LastAroundTic >= AdaptiveIdleTics));
	if (!automapDue && !aroundDue) return;
	FWalkerSnapshot snapshot;
	snapshot.Level = Level;
	snapshot.StartTime = Level->starttime;
	snapshot.Viewpoint = Viewpoint;
	snapshot.MapSections = CurrentMapSections;
	snapshot.InArea = in_area;
	snapshot.Automap = automapDue;
	snapshot.Around = aroundDue;
	if (!Walker.Submit(std::move(snapshot))) return;
	if (automapDue) LastSubmittedTic = frameTic;
	if (aroundDue)
	{
		LastAroundTic = frameTic;
		LastAroundFrom = Viewpoint.Pos.XY();
	}
}

CCMD(worldmode_walker_stats)
{
	Printf("WORLDMODEWALK enabled=%d passes=%llu last_pass_us=%.1f avg_pass_us=%.1f last_lines=%llu applied_writes=%llu\n",
		int(r_worldmode_automap), (unsigned long long)Walker.Passes, Walker.LastNS / 1000.0,
		Walker.Passes ? double(Walker.TotalNS) / Walker.Passes / 1000.0 : 0.0, (unsigned long long)Walker.LastLines,
		(unsigned long long)Walker.AppliedWrites);
	Printf("WORLDMODEAROUND enabled=%d passes=%llu last_pass_us=%.1f avg_pass_us=%.1f last_sectors=%llu adaptive_us=%d active=%d median_us=%.1f switches=%llu\n",
		int(r_worldmode_occlusion), (unsigned long long)Walker.AroundPasses, Walker.LastAroundNS / 1000.0,
		Walker.AroundPasses ? double(Walker.AroundTotalNS) / Walker.AroundPasses / 1000.0 : 0.0, (unsigned long long)Walker.LastAroundSectors,
		int(r_worldmode_occlusion_adaptive), int(OcclusionWanted()), AdaptiveMedian, (unsigned long long)AdaptiveSwitches);
}
