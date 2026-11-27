/*
** d_ticworkercheck.cpp
**
** [Karto] Reports main-thread use of shared systems while the worker ticks (d_ticworkercheck.h).
*/

#include <map>
#include <array>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

#include "printf.h"
#include "d_ticworkercheck.h"
#include "d_ticworker.h"
#include "v_text.h"
#include "i_system.h"

bool TicWorkerCheckArmed = false;
thread_local bool TicWorkerThisThread = false;

namespace
{
	const char* const SystemNames[TWS_Count] = { "object_alloc", "sound_start", "print", "name_insert", "texture_add", "play_rng" };

	std::map<std::pair<int, std::array<void*, 6>>, uint64_t> Violations;
}

static std::string Symbol(void* address)
{
#ifdef _WIN32
	static bool initialised = false;
	if (!initialised)
	{
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		SymInitialize(GetCurrentProcess(), nullptr, TRUE);
		initialised = true;
	}
	alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
	auto info = reinterpret_cast<SYMBOL_INFO*>(buffer);
	info->SizeOfStruct = sizeof(SYMBOL_INFO);
	info->MaxNameLen = 255;
	DWORD64 displacement = 0;
	if (address && SymFromAddr(GetCurrentProcess(), DWORD64(address), &displacement, info)) return info->Name;
#endif
	return address ? "?" : "";
}

void TicWorkerCheckNoteSlow(ETicWorkerSystem system)
{
	if (!TicWorkerBusy || TicWorkerOnThread()) return;
	std::array<void*, 6> stack{};
#ifdef _WIN32
	void* frames[8] = {};
	const USHORT n = CaptureStackBackTrace(2, 8, frames, nullptr);
	for (USHORT i = 0; i < n && i < stack.size(); i++) stack[i] = frames[i];
#endif
	if (Violations[{ int(system), stack }]++ != 0) return;
	std::string chain;
	for (void* frame : stack)
	{
		if (!frame) break;
		if (!chain.empty()) chain += " < ";
		chain += Symbol(frame);
	}
	const bool armed = TicWorkerCheckArmed;
	TicWorkerCheckArmed = false;
	Printf(TEXTCOLOR_RED "TICWORKERCHECK main-thread %s while the worker ticks: %s\n", SystemNames[system], chain.c_str());
	TicWorkerCheckArmed = armed;
	// [Karto] A DObject made here links into the GC list the worker is appending to: corrupt state, not a warning.
	if (system == TWS_ObjectAlloc) I_FatalError("TICWORKERCHECK object allocation on the main thread while the worker ticks: %s", chain.c_str());
}
