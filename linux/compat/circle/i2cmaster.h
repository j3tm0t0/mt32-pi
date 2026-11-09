//
// circle/i2cmaster.h - Linux compatibility shim for mt32-pi
//
// CI2CMaster backed by Linux i2c-dev (/dev/i2c-N). The LCD and MiSTer
// drivers use it unchanged.
//
// A device of the form "sim:FILE.pbm" emulates an SSD1306/SH1106 OLED for
// testing without hardware: each framebuffer update is written to FILE.pbm.
//

#ifndef _circle_i2cmaster_h
#define _circle_i2cmaster_h

#include <circle/types.h>

#include <mutex>
#include <string>
#include <vector>

class CI2CMaster
{
public:
	CI2CMaster(const char* pDevice);
	~CI2CMaster();

	bool Initialize();
	void SetClock(unsigned nClockSpeed) {}

	// Return number of bytes transferred or a negative value on error, like Circle
	int Read(u8 ucAddress, void* pBuffer, unsigned nCount);
	int Write(u8 ucAddress, const void* pBuffer, unsigned nCount);

	const char* GetDevice() const { return m_Device.c_str(); }

private:
	bool SelectAddress(u8 ucAddress);
	void SimulateWrite(const u8* pData, unsigned nCount);

	std::string m_Device;
	int m_nFD;
	int m_nCurrentAddress;
	std::mutex m_Mutex;

	// OLED simulation
	bool m_bSimulated;
	std::string m_SimPath;
	std::vector<u8> m_SimRAM;
	unsigned m_nSimFrames;
};

#endif
