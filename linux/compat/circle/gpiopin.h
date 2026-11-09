//
// circle/gpiopin.h - Linux compatibility shim for mt32-pi
//
// Declaration-only stand-in so that headers which embed CGPIOPin members
// (e.g. control/rotaryencoder.h, pulled in by config.h for its enum types)
// compile. GPIO-based controls are not built on Linux yet.
//

#ifndef _circle_gpiopin_h
#define _circle_gpiopin_h

#include <circle/types.h>

#define LOW 0
#define HIGH 1

enum TGPIOMode
{
	GPIOModeInput,
	GPIOModeOutput,
	GPIOModeInputPullUp,
	GPIOModeInputPullDown,
	GPIOModeUnknown
};

class CGPIOPin
{
public:
	CGPIOPin() = default;
	CGPIOPin(unsigned nPin, TGPIOMode Mode, void* pManager = nullptr) {}

	unsigned Read() const { return LOW; }
	void Write(unsigned nValue) {}
};

#endif
