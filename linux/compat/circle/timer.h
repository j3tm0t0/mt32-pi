//
// circle/timer.h - Linux compatibility shim for mt32-pi
//
// Circle exposes two clocks that mt32-pi relies on:
//   - CTimer::Get()->GetTicks(): HZ (100 Hz) system ticks
//   - CTimer::GetClockTicks():   1 MHz free-running counter (wraps at 32 bits)
// Both are derived from CLOCK_MONOTONIC here.
//

#ifndef _circle_timer_h
#define _circle_timer_h

#include <circle/types.h>

#define HZ 100
#define MSEC2HZ(msec) ((msec) * HZ / 1000)

class CTimer
{
public:
	static CTimer* Get();

	// 100 Hz ticks since program start
	unsigned GetTicks() const;

	// Seconds since program start
	unsigned GetUptime() const { return GetTicks() / HZ; }

	// 1 MHz ticks since program start (wraps)
	static unsigned GetClockTicks();
	static u64 GetClockTicks64();

	static void SimpleMsDelay(unsigned nMilliSeconds);
	static void SimpleusDelay(unsigned nMicroSeconds);
	void MsDelay(unsigned nMilliSeconds) { SimpleMsDelay(nMilliSeconds); }
	void usDelay(unsigned nMicroSeconds) { SimpleusDelay(nMicroSeconds); }
};

#endif
