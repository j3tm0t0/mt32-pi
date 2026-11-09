//
// serialmidi.h - Serial (GPIO UART / USB-serial) MIDI input for mt32-pi on Linux
//
// Linux counterpart of the bare-metal GPIO MIDI path: opens a tty in raw
// mode at an arbitrary baud rate (31250 for MIDI, via termios2/BOTHER) and
// hands the received bytes to the shared CMIDIParser. Optionally echoes
// them back out ("software thru"), like [midi] gpio_thru on the Pi.
//

#ifndef _serialmidi_h
#define _serialmidi_h

#include <cstddef>
#include <cstdint>
#include <string>

class CSerialMIDIInput
{
public:
	CSerialMIDIInput();
	~CSerialMIDIInput();

	bool Open(const char* pDevice, unsigned nBaudRate, bool bThru);
	void Close();

	bool IsOpen() const { return m_nFD >= 0; }
	int GetFD() const { return m_nFD; }
	const char* GetDevice() const { return m_Device.c_str(); }

	// Reads available bytes (non-blocking); echoes them if thru is enabled. Returns bytes read, 0 if none, -1 on error.
	int Read(uint8_t* pBuffer, size_t nSize);

	// Returns true if the given tty is used as a kernel console (console= on the kernel command line)
	static bool IsKernelConsole(const char* pDevice);

private:
	int m_nFD;
	bool m_bThru;
	std::string m_Device;
};

#endif
