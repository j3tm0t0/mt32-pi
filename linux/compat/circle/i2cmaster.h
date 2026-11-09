//
// circle/i2cmaster.h - Linux compatibility shim for mt32-pi
//
// Declaration-only stand-in so that headers which reference CI2CMaster
// (e.g. lcd/drivers/ssd1306.h, pulled in by config.h for its enum types)
// compile. I2C displays are not built on Linux yet.
//

#ifndef _circle_i2cmaster_h
#define _circle_i2cmaster_h

#include <circle/types.h>

class CI2CMaster
{
public:
	int Read(u8 ucAddress, void* pBuffer, unsigned nCount) { return -1; }
	int Write(u8 ucAddress, const void* pBuffer, unsigned nCount) { return -1; }
};

#endif
