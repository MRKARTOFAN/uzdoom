/*
** d_ticworker.cpp
**
** [Karto] The tic worker thread (d_ticworker.h). The mutex and condition variable give the release/acquire ordering that
** publishes the worker's writes to the main thread and the main thread's prologue to the worker.
*/

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

#include "d_ticworker.h"
#include "d_ticworkercheck.h"

std::atomic<bool> TicWorkerBusy{ false };

namespace
{
	std::mutex Lock;
	std::condition_variable Wake, Done;
	// [Karto] Never destroyed: a joinable std::thread destroyed at exit() calls std::terminate, and the engine exits
	// from many places; the OS ends the thread with the process.
	std::thread* Thread = nullptr;
	std::thread::id ThreadId;
	void (*Job)() = nullptr;
	bool Finished = false;
	bool Quit = false;
	std::exception_ptr Failure;

	void WorkerMain()
	{
		TicWorkerThisThread = true;
		for (;;)
		{
			void (*job)();
			{
				std::unique_lock<std::mutex> guard(Lock);
				Wake.wait(guard, [] { return Job != nullptr || Quit; });
				if (Quit) return;
				job = Job;
			}
			std::exception_ptr failure;
			try
			{
				job();
			}
			catch (...)
			{
				// [Karto] Errors must surface on the main thread, which owns error handling, not end this thread.
				failure = std::current_exception();
			}
			{
				std::lock_guard<std::mutex> guard(Lock);
				Job = nullptr;
				Failure = failure;
				Finished = true;
			}
			Done.notify_all();
		}
	}
}

bool TicWorkerOnThread()
{
	return Thread != nullptr && std::this_thread::get_id() == ThreadId;
}

void TicWorkerStart(void (*job)())
{
	if (Thread == nullptr)
	{
		Thread = new std::thread(WorkerMain);
		ThreadId = Thread->get_id();
	}
	{
		std::lock_guard<std::mutex> guard(Lock);
		Job = job;
		Finished = false;
		Failure = nullptr;
	}
	TicWorkerBusy = true;
	Wake.notify_one();
}

static bool Collect(std::unique_lock<std::mutex>& guard)
{
	std::exception_ptr failure = Failure;
	Failure = nullptr;
	Finished = false;
	guard.unlock();
	TicWorkerBusy = false;
	if (failure) std::rethrow_exception(failure);
	return true;
}

bool TicWorkerPoll()
{
	std::unique_lock<std::mutex> guard(Lock);
	if (!Finished) return false;
	return Collect(guard);
}

void TicWorkerWait()
{
	std::unique_lock<std::mutex> guard(Lock);
	if (!TicWorkerBusy) return;
	Done.wait(guard, [] { return Finished; });
	Collect(guard);
}

void TicWorkerWaitFinished()
{
	std::unique_lock<std::mutex> guard(Lock);
	if (!TicWorkerBusy) return;
	Done.wait(guard, [] { return Finished; });
}

bool TicWorkerWaitFinishedFor(double ms)
{
	std::unique_lock<std::mutex> guard(Lock);
	if (!TicWorkerBusy) return true;
	return Done.wait_for(guard, std::chrono::duration<double, std::milli>(ms), [] { return Finished; });
}

void TicWorkerShutdown()
{
	if (Thread == nullptr) return;
	{
		std::lock_guard<std::mutex> guard(Lock);
		Quit = true;
	}
	Wake.notify_one();
	Thread->join();
	delete Thread;
	Thread = nullptr;
}
