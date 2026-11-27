/*
** d_decoupleprobe.cpp
**
** [Karto] Test-only tic-ahead detector for the tic/render decoupling: see decouple_tickahead below.
*/

#include "c_dispatch.h"
#include "g_levellocals.h"
#include "printf.h"
#include "doomstat.h"
#include "p_tick.h"
#include "i_interface.h"

bool DecouplePublishHeld = false;
void P_RunClientSideLogic();

CCMD(decouple_tickahead)
{
	if (argv.argc() > 1 && !stricmp(argv[1], "release"))
	{
		DecouplePublishHeld = false;
		return;
	}
	if (gamestate != GS_LEVEL || !paused)
	{
		Printf("decouple_tickahead: needs a paused level\n");
		return;
	}
	DecouplePublishHeld = true;
	const int wasPaused = paused;
	paused = 0;
	const bool clientOnly = argv.argc() > 1 && !stricmp(argv[1], "client");
	const bool playOnly = argv.argc() > 1 && !stricmp(argv[1], "play");
	if (!clientOnly) P_Ticker();
	if (!playOnly) P_RunClientSideLogic();
	paused = wasPaused;
	Printf("DECOUPLETICKAHEAD maptime=%d\n", primaryLevel->maptime);
}
