/*
** d_framestages.cpp
**
** [Karto] Test-only per-stage frame timers (frame_stages on | print | off). Stage sums are kept per frame so percentiles are exact.
*/

#include <algorithm>
#include <chrono>
#include <vector>

#include "c_dispatch.h"
#include "printf.h"
#include "d_framestages.h"

bool FrameStagesArmed = false;

static const char* const StageNames[FS_Count] = { "tic", "publish", "display", "updateworldmode", "walker", "sprites",
	"sky", "createscene", "renderscene", "solidsprites", "sortedlist", "addpass", "translucent", "update", "gc", "net",
	"predict", "clientlogic", "sounds", "csthinkers", "recreatelights", "statusbar", "solidsort", "alphapass", "particlepass",
	"particlecollect", "particlekeys", "particlebuild", "particleupload", "particledraw" };

namespace
{
	using Clock = std::chrono::steady_clock;

	struct FStageState
	{
		Clock::time_point Start;
		int Depth = 0;
		int Entries = 0;
		double Ms = 0;
	};

	FStageState Current[FS_Count];
	Clock::time_point FrameStart;
	bool FrameOpen = false;
	std::vector<float> Rows[2];
}

void FrameStageBegin(EFrameStage stage)
{
	auto& s = Current[stage];
	if (s.Depth++ == 0) s.Start = Clock::now();
	s.Entries++;
}

void FrameStageEnd(EFrameStage stage)
{
	auto& s = Current[stage];
	if (--s.Depth == 0) s.Ms += std::chrono::duration<double, std::milli>(Clock::now() - s.Start).count();
}

void FrameStagesFrameStart()
{
	if (!FrameStagesArmed) return;
	for (auto& s : Current) s = FStageState();
	FrameStart = Clock::now();
	FrameOpen = true;
}

void FrameStagesFrameEnd()
{
	if (!FrameStagesArmed || !FrameOpen) return;
	FrameOpen = false;
	const double total = std::chrono::duration<double, std::milli>(Clock::now() - FrameStart).count();
	auto& rows = Rows[Current[FS_Tic].Entries > 0 ? 1 : 0];
	for (auto& s : Current) rows.push_back(float(s.Ms));
	rows.push_back(float(total));
}

static void PrintClass(int cls)
{
	const auto& rows = Rows[cls];
	const size_t width = FS_Count + 1;
	const size_t frames = rows.size() / width;
	Printf("FRAMESTAGES class=%s frames=%zu\n", cls ? "tic" : "notic", frames);
	if (frames == 0) return;
	std::vector<float> column(frames);
	auto report = [&](const char* name, auto value)
	{
		double sum = 0;
		for (size_t f = 0; f < frames; f++)
		{
			column[f] = value(f);
			sum += column[f];
		}
		std::sort(column.begin(), column.end());
		Printf("FRAMESTAGE class=%s stage=%s mean=%.4f median=%.4f p95=%.4f max=%.4f\n", cls ? "tic" : "notic", name, sum / frames,
			column[frames / 2], column[std::min(frames - 1, size_t(frames * 0.95))], column[frames - 1]);
	};
	for (int s = 0; s < FS_Count; s++) report(StageNames[s], [&](size_t f) { return rows[f * width + s]; });
	report("frame", [&](size_t f) { return rows[f * width + FS_Count]; });
	report("remainder", [&](size_t f)
	{
		const float* r = &rows[f * width];
		return r[FS_Count] - r[FS_Tic] - r[FS_Publish] - r[FS_Display];
	});
}

CCMD(frame_stages)
{
	if (argv.argc() == 2 && stricmp(argv[1], "on") == 0)
	{
		Rows[0].clear();
		Rows[1].clear();
		FrameOpen = false;
		FrameStagesArmed = true;
		Printf("FRAMESTAGES armed\n");
	}
	else if (argv.argc() == 2 && stricmp(argv[1], "off") == 0)
	{
		FrameStagesArmed = false;
		Printf("FRAMESTAGES disarmed\n");
	}
	else if (argv.argc() == 2 && stricmp(argv[1], "print") == 0)
	{
		PrintClass(0);
		PrintClass(1);
	}
	else Printf("Usage: frame_stages on|off|print\n");
}
