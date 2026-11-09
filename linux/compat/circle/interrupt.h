//
// circle/interrupt.h - Linux compatibility shim for mt32-pi
//

#ifndef _circle_interrupt_h
#define _circle_interrupt_h

class CInterruptSystem
{
public:
	static CInterruptSystem* Get() { return nullptr; }
};

#endif
