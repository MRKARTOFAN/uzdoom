/*
** d_ticworker.h
**
** [Karto] Runs the world tick (P_Ticker) on one persistent thread while the main thread keeps drawing the last publish;
** everything else of a tic stays on the main thread in serial order.
*/

#pragma once

#include <atomic>

// True while a handed-over tic runs or waits to be collected; the main thread must not touch play state then.
extern std::atomic<bool> TicWorkerBusy;

bool TicWorkerOnThread();
void TicWorkerStart(void (*job)());
// Non-blocking: true once the job finished (rethrows the job's exception on the calling thread).
bool TicWorkerPoll();
void TicWorkerWait();
// Blocks until the handed-over job finished, without collecting it (TicWorkerPoll then returns true).
void TicWorkerWaitFinished();
// As TicWorkerWaitFinished, but gives up after the given time; true when the job finished.
bool TicWorkerWaitFinishedFor(double ms);
void TicWorkerShutdown();
