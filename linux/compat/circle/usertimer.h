//
// circle/usertimer.h - Linux compatibility shim for mt32-pi
//
// Circle's CUserTimer is a one-shot hardware timer interrupt that the handler
// re-arms with Start(). Here it is a thread sleeping on CLOCK_MONOTONIC; the
// handler runs in that thread (used for the 1 kHz button/encoder polling).
//

#ifndef _circle_usertimer_h
#define _circle_usertimer_h

#include <circle/interrupt.h>
#include <circle/types.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class CUserTimer;
typedef void TUserTimerHandler(CUserTimer* pUserTimer, void* pParam);

class CUserTimer
{
public:
	CUserTimer(CInterruptSystem* pInterruptSystem, TUserTimerHandler* pHandler, void* pParam = nullptr, bool bUseFIQ = false);
	~CUserTimer();

	bool Initialize();
	void Stop();
	void Start(unsigned nDelayMicros);

private:
	void Thread();

	TUserTimerHandler* m_pHandler;
	void* m_pParam;

	std::thread m_Thread;
	std::mutex m_Mutex;
	std::condition_variable m_Condition;
	bool m_bArmed;
	bool m_bQuit;
	u64 m_nDeadlineMicros;
};

#endif
