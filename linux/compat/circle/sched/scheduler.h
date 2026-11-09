//
// circle/sched/scheduler.h - Linux compatibility shim for mt32-pi
//

#ifndef _circle_sched_scheduler_h
#define _circle_sched_scheduler_h

#include <sched.h>

class CScheduler
{
public:
	static CScheduler* Get()
	{
		static CScheduler Scheduler;
		return &Scheduler;
	}

	void Yield() { sched_yield(); }
};

#endif
