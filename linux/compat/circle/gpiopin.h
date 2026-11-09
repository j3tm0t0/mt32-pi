//
// circle/gpiopin.h - Linux compatibility shim for mt32-pi
//
// CGPIOPin backed by the Linux GPIO character device (uAPI v2, kernel 5.10+).
// This works the same on the Pi 4 (pinctrl-bcm2711) and the Pi 5 (RP1), where
// direct register access is no longer possible. No libgpiod dependency.
//
// Chip selection (CGPIOPin::SetChip, from [linux] gpio_chip):
//   "auto"         first chip labelled pinctrl-rp1/bcm2712/bcm2711/bcm2835
//   "/dev/gpiochipN"
//   "sim:FILE"     simulation for testing: FILE lists the BCM pin numbers that
//                  currently read LOW (e.g. pressed buttons); others read HIGH
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

class CGPIOManager;

class CGPIOPin
{
public:
	CGPIOPin();
	CGPIOPin(unsigned nPin, TGPIOMode Mode, CGPIOManager* pManager = nullptr);
	~CGPIOPin();

	CGPIOPin(const CGPIOPin&) = delete;
	CGPIOPin& operator=(const CGPIOPin&) = delete;

	void AssignPin(unsigned nPin);
	void SetMode(TGPIOMode Mode, bool bInitPin = true);

	unsigned Read() const;
	void Write(unsigned nValue);
	void Invert() { Write(m_nValue ^ 1); }

	// Levels of all requested pins as a bitmask indexed by BCM pin number
	static u32 ReadAll();

	static void SetChip(const char* pChip);

private:
	void Request();
	void Release();

	unsigned m_nPin;
	TGPIOMode m_Mode;
	int m_nLineFD;
	unsigned m_nValue;
};

#endif
