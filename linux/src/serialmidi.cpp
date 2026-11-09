//
// serialmidi.cpp - Serial (GPIO UART / USB-serial) MIDI input for mt32-pi on Linux
//

// termios2 (for arbitrary baud rates such as 31250) lives in <asm/termbits.h>, which conflicts with <termios.h>;
// this file therefore must not include <termios.h>.
#include <asm/termbits.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <circle/logger.h>

#include "serialmidi.h"

LOGMODULE("serialmidi");

namespace
{
	// Resolves symlinks like /dev/serial0 -> /dev/ttyAMA0 and returns the tty name ("ttyAMA0")
	std::string TTYName(const char* pDevice)
	{
		char Resolved[PATH_MAX];
		const char* pPath = realpath(pDevice, Resolved) ? Resolved : pDevice;
		const char* pSlash = strrchr(pPath, '/');
		return pSlash ? pSlash + 1 : pPath;
	}
}

CSerialMIDIInput::CSerialMIDIInput()
	: m_nFD(-1),
	  m_bThru(false)
{
}

CSerialMIDIInput::~CSerialMIDIInput()
{
	Close();
}

bool CSerialMIDIInput::IsKernelConsole(const char* pDevice)
{
	std::ifstream CmdLine("/proc/cmdline");
	std::string Line;
	if (!std::getline(CmdLine, Line))
		return false;

	const std::string TTY = TTYName(pDevice);
	std::istringstream Tokens(Line);
	std::string Token;
	while (Tokens >> Token)
	{
		if (Token.rfind("console=", 0) != 0)
			continue;

		std::string Console = Token.substr(8);
		Console = Console.substr(0, Console.find(','));

		// console=serial0 refers to the /dev/serial0 alias
		const std::string ConsoleTTY = TTYName(("/dev/" + Console).c_str());
		if (ConsoleTTY == TTY || Console == TTY)
			return true;
	}

	return false;
}

bool CSerialMIDIInput::Open(const char* pDevice, unsigned nBaudRate, bool bThru)
{
	Close();

	const int nFD = open(pDevice, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (nFD < 0)
	{
		LOGWARN("Cannot open serial MIDI device '%s': %s", pDevice, strerror(errno));
		return false;
	}

	// Exclusive access; avoid another process (e.g. getty) interleaving reads
	ioctl(nFD, TIOCEXCL);

	struct termios2 TIO;
	if (ioctl(nFD, TCGETS2, &TIO) != 0)
	{
		LOGWARN("'%s' is not a tty: %s", pDevice, strerror(errno));
		close(nFD);
		return false;
	}

	// Raw 8N1, receiver on, ignore modem control lines, custom baud rate
	TIO.c_iflag = IGNBRK;
	TIO.c_oflag = 0;
	TIO.c_lflag = 0;
	TIO.c_cflag = CS8 | CREAD | CLOCAL | BOTHER;
	TIO.c_cflag |= BOTHER << IBSHIFT;
	TIO.c_ispeed = nBaudRate;
	TIO.c_ospeed = nBaudRate;
	TIO.c_cc[VMIN]  = 0;
	TIO.c_cc[VTIME] = 0;

	if (ioctl(nFD, TCSETS2, &TIO) != 0)
	{
		LOGWARN("Cannot configure '%s' for %u baud: %s", pDevice, nBaudRate, strerror(errno));
		close(nFD);
		return false;
	}

	// Discard anything received before we were ready
	ioctl(nFD, TCFLSH, TCIFLUSH);

	// Report the rate the driver actually chose
	if (ioctl(nFD, TCGETS2, &TIO) == 0 && TIO.c_ispeed != nBaudRate)
		LOGWARN("'%s' runs at %u baud instead of %u", pDevice, TIO.c_ispeed, nBaudRate);

	m_nFD    = nFD;
	m_bThru  = bThru;
	m_Device = pDevice;

	LOGNOTE("Serial MIDI on %s (%s), %u baud%s", pDevice, TTYName(pDevice).c_str(), nBaudRate, bThru ? ", thru enabled" : "");
	return true;
}

void CSerialMIDIInput::Close()
{
	if (m_nFD >= 0)
	{
		close(m_nFD);
		m_nFD = -1;
	}
}

int CSerialMIDIInput::Read(uint8_t* pBuffer, size_t nSize)
{
	if (m_nFD < 0)
		return -1;

	const ssize_t nRead = read(m_nFD, pBuffer, nSize);
	if (nRead < 0)
	{
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return 0;

		LOGERR("Serial MIDI read error on %s: %s", m_Device.c_str(), strerror(errno));
		return -1;
	}

	if (nRead > 0 && m_bThru)
	{
		const ssize_t nWritten = write(m_nFD, pBuffer, static_cast<size_t>(nRead));
		if (nWritten != nRead)
			LOGERR("Received %zd bytes, but only sent %zd bytes", nRead, nWritten);
	}

	return static_cast<int>(nRead);
}
