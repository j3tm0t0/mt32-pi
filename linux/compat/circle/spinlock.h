//
// circle/spinlock.h - Linux compatibility shim for mt32-pi
//
// CSpinLock is mapped to a priority-inheriting pthread mutex. The synth lock
// is shared between the real-time audio thread and the MIDI/control thread,
// so priority inheritance avoids priority inversion when the audio thread
// runs with SCHED_FIFO.
//

#ifndef _circle_spinlock_h
#define _circle_spinlock_h

#include <pthread.h>

#define TASK_LEVEL 0
#define IRQ_LEVEL 1
#define FIQ_LEVEL 2

class CSpinLock
{
public:
	CSpinLock(unsigned nTargetLevel = IRQ_LEVEL)
	{
		pthread_mutexattr_t Attr;
		pthread_mutexattr_init(&Attr);
		pthread_mutexattr_setprotocol(&Attr, PTHREAD_PRIO_INHERIT);
		pthread_mutex_init(&m_Mutex, &Attr);
		pthread_mutexattr_destroy(&Attr);
	}

	~CSpinLock() { pthread_mutex_destroy(&m_Mutex); }

	CSpinLock(const CSpinLock&) = delete;
	CSpinLock& operator=(const CSpinLock&) = delete;

	void Acquire() { pthread_mutex_lock(&m_Mutex); }
	void Release() { pthread_mutex_unlock(&m_Mutex); }

private:
	pthread_mutex_t m_Mutex;
};

#endif
