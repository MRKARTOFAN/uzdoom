/*
** d_ticworkercheck.h
**
** [Karto] Main-thread use of shared systems while the worker ticks races with it; the hooks report it when
** r_tic_worker_check is on.
*/

#pragma once

enum ETicWorkerSystem
{
	TWS_ObjectAlloc,
	TWS_SoundStart,
	TWS_Print,
	TWS_NameInsert,
	TWS_TextureAdd,
	TWS_PlayRng,
	TWS_Count
};

extern bool TicWorkerCheckArmed;
extern thread_local bool TicWorkerThisThread;

void TicWorkerCheckNoteSlow(ETicWorkerSystem system);

inline void TicWorkerCheckNote(ETicWorkerSystem system)
{
	if (TicWorkerCheckArmed && !TicWorkerThisThread) TicWorkerCheckNoteSlow(system);
}
