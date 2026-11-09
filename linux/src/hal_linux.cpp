//
// hal_linux.cpp - GPIO, I2C and timer shims for mt32-pi on Linux
//

#include <dirent.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <circle/gpiopin.h>
#include <circle/i2cmaster.h>
#include <circle/logger.h>
#include <circle/timer.h>
#include <circle/usertimer.h>

LOGMODULE("hal");

//
// GPIO
//
namespace
{
	std::string g_ChipSetting = "auto";

	std::mutex g_GPIOMutex;
	int g_nChipFD = -1;
	bool g_bChipResolved = false;
	std::map<unsigned, CGPIOPin*> g_Pins;

	// Simulation state
	bool g_bSimulated = false;
	std::string g_SimFile;
	std::set<unsigned> g_SimLowPins;
	u64 g_nSimLastCheck = 0;
	struct timespec g_SimMTime{};

	void ResolveChip()
	{
		if (g_bChipResolved)
			return;
		g_bChipResolved = true;

		if (g_ChipSetting.rfind("sim:", 0) == 0)
		{
			g_bSimulated = true;
			g_SimFile    = g_ChipSetting.substr(4);
			LOGNOTE("GPIO simulation; LOW pins are read from %s", g_SimFile.c_str());
			return;
		}

		auto TryOpen = [](const std::string& Path, bool bCheckLabel) -> bool {
			const int nFD = open(Path.c_str(), O_RDWR | O_CLOEXEC);
			if (nFD < 0)
				return false;

			gpiochip_info Info{};
			if (ioctl(nFD, GPIO_GET_CHIPINFO_IOCTL, &Info) != 0)
			{
				close(nFD);
				return false;
			}

			static const char* const Labels[] = {"pinctrl-rp1", "pinctrl-bcm2712", "pinctrl-bcm2711", "pinctrl-bcm2835"};
			const bool bMatch = !bCheckLabel || std::any_of(std::begin(Labels), std::end(Labels), [&](const char* p) { return !strcmp(Info.label, p); });
			if (!bMatch)
			{
				close(nFD);
				return false;
			}

			LOGNOTE("Using GPIO chip %s (%s, %u lines)", Path.c_str(), Info.label, Info.lines);
			g_nChipFD = nFD;
			return true;
		};

		if (g_ChipSetting != "auto")
		{
			if (!TryOpen(g_ChipSetting, false))
				LOGERR("Cannot open GPIO chip %s: %s", g_ChipSetting.c_str(), strerror(errno));
			return;
		}

		for (int i = 0; i < 16; ++i)
			if (TryOpen("/dev/gpiochip" + std::to_string(i), true))
				return;

		LOGERR("No Raspberry Pi GPIO chip found (set [linux] gpio_chip)");
	}

	void RefreshSimulation()
	{
		const u64 nNow = CTimer::GetClockTicks64();
		if (nNow - g_nSimLastCheck < 10000)
			return;
		g_nSimLastCheck = nNow;

		struct stat Stat;
		if (stat(g_SimFile.c_str(), &Stat) != 0)
		{
			g_SimLowPins.clear();
			return;
		}

		if (Stat.st_mtim.tv_sec == g_SimMTime.tv_sec && Stat.st_mtim.tv_nsec == g_SimMTime.tv_nsec)
			return;
		g_SimMTime = Stat.st_mtim;

		g_SimLowPins.clear();
		std::ifstream File(g_SimFile);
		unsigned nPin;
		while (File >> nPin)
			g_SimLowPins.insert(nPin);
	}
}

void CGPIOPin::SetChip(const char* pChip)
{
	std::lock_guard<std::mutex> Lock(g_GPIOMutex);
	g_ChipSetting  = (pChip && *pChip) ? pChip : "auto";
	g_bChipResolved = false;
}

CGPIOPin::CGPIOPin()
	: m_nPin(~0u),
	  m_Mode(GPIOModeUnknown),
	  m_nLineFD(-1),
	  m_nValue(LOW)
{
}

CGPIOPin::CGPIOPin(unsigned nPin, TGPIOMode Mode, CGPIOManager* pManager)
	: m_nPin(nPin),
	  m_Mode(Mode),
	  m_nLineFD(-1),
	  m_nValue(LOW)
{
	Request();
}

CGPIOPin::~CGPIOPin()
{
	Release();
}

void CGPIOPin::AssignPin(unsigned nPin)
{
	Release();
	m_nPin = nPin;
	if (m_Mode != GPIOModeUnknown)
		Request();
}

void CGPIOPin::SetMode(TGPIOMode Mode, bool bInitPin)
{
	Release();
	m_Mode = Mode;
	if (m_nPin != ~0u)
		Request();
}

void CGPIOPin::Request()
{
	std::lock_guard<std::mutex> Lock(g_GPIOMutex);
	ResolveChip();
	g_Pins[m_nPin] = this;

	if (g_bSimulated || g_nChipFD < 0)
		return;

	gpio_v2_line_request Request{};
	Request.offsets[0] = m_nPin;
	Request.num_lines  = 1;
	snprintf(Request.consumer, sizeof(Request.consumer), "mt32-pi");

	switch (m_Mode)
	{
		case GPIOModeOutput:
			Request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT;
			Request.config.num_attrs = 1;
			Request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
			Request.config.attrs[0].attr.values = m_nValue;
			Request.config.attrs[0].mask = 1;
			break;
		case GPIOModeInputPullUp:
			Request.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
			break;
		case GPIOModeInputPullDown:
			Request.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
			break;
		default:
			Request.config.flags = GPIO_V2_LINE_FLAG_INPUT;
			break;
	}

	if (ioctl(g_nChipFD, GPIO_V2_GET_LINE_IOCTL, &Request) != 0)
	{
		LOGERR("Cannot request GPIO %u: %s", m_nPin, strerror(errno));
		return;
	}

	m_nLineFD = Request.fd;
}

void CGPIOPin::Release()
{
	std::lock_guard<std::mutex> Lock(g_GPIOMutex);
	auto It = g_Pins.find(m_nPin);
	if (It != g_Pins.end() && It->second == this)
		g_Pins.erase(It);

	if (m_nLineFD >= 0)
	{
		close(m_nLineFD);
		m_nLineFD = -1;
	}
}

unsigned CGPIOPin::Read() const
{
	if (g_bSimulated)
	{
		std::lock_guard<std::mutex> Lock(g_GPIOMutex);
		RefreshSimulation();
		return g_SimLowPins.count(m_nPin) ? LOW : HIGH;
	}

	if (m_nLineFD < 0)
		return m_Mode == GPIOModeInputPullUp ? HIGH : LOW;

	gpio_v2_line_values Values{};
	Values.mask = 1;
	if (ioctl(m_nLineFD, GPIO_V2_LINE_GET_VALUES_IOCTL, &Values) != 0)
		return m_Mode == GPIOModeInputPullUp ? HIGH : LOW;

	return (Values.bits & 1) ? HIGH : LOW;
}

void CGPIOPin::Write(unsigned nValue)
{
	m_nValue = nValue ? HIGH : LOW;
	if (m_nLineFD < 0)
		return;

	gpio_v2_line_values Values{};
	Values.mask = 1;
	Values.bits = m_nValue;
	ioctl(m_nLineFD, GPIO_V2_LINE_SET_VALUES_IOCTL, &Values);
}

u32 CGPIOPin::ReadAll()
{
	std::map<unsigned, CGPIOPin*> Pins;
	{
		std::lock_guard<std::mutex> Lock(g_GPIOMutex);
		Pins = g_Pins;
	}

	u32 nState = 0;
	for (const auto& Pin : Pins)
		if (Pin.first < 32 && Pin.second->Read() == HIGH)
			nState |= 1u << Pin.first;

	return nState;
}

//
// I2C
//
CI2CMaster::CI2CMaster(const char* pDevice)
	: m_Device(pDevice ? pDevice : ""),
	  m_nFD(-1),
	  m_nCurrentAddress(-1),
	  m_bSimulated(false),
	  m_nSimFrames(0)
{
}

CI2CMaster::~CI2CMaster()
{
	if (m_nFD >= 0)
		close(m_nFD);
}

bool CI2CMaster::Initialize()
{
	if (m_Device.rfind("sim:", 0) == 0)
	{
		m_bSimulated = true;
		m_SimPath    = m_Device.substr(4);
		m_SimRAM.assign(132 * 8, 0);
		LOGNOTE("I2C OLED simulation; frames are written to %s", m_SimPath.c_str());
		return true;
	}

	m_nFD = open(m_Device.c_str(), O_RDWR | O_CLOEXEC);
	if (m_nFD < 0)
	{
		LOGWARN("Cannot open I2C bus %s: %s (enable with dtparam=i2c_arm=on)", m_Device.c_str(), strerror(errno));
		return false;
	}

	LOGNOTE("I2C bus %s", m_Device.c_str());
	return true;
}

bool CI2CMaster::SelectAddress(u8 ucAddress)
{
	if (m_nCurrentAddress == ucAddress)
		return true;

	if (ioctl(m_nFD, I2C_SLAVE, ucAddress) != 0)
		return false;

	m_nCurrentAddress = ucAddress;
	return true;
}

int CI2CMaster::Read(u8 ucAddress, void* pBuffer, unsigned nCount)
{
	std::lock_guard<std::mutex> Lock(m_Mutex);
	if (m_bSimulated)
		return -1;

	if (m_nFD < 0 || !SelectAddress(ucAddress))
		return -1;

	const ssize_t nResult = read(m_nFD, pBuffer, nCount);
	return nResult < 0 ? -1 : static_cast<int>(nResult);
}

int CI2CMaster::Write(u8 ucAddress, const void* pBuffer, unsigned nCount)
{
	std::lock_guard<std::mutex> Lock(m_Mutex);
	if (m_bSimulated)
	{
		SimulateWrite(static_cast<const u8*>(pBuffer), nCount);
		return static_cast<int>(nCount);
	}

	if (m_nFD < 0 || !SelectAddress(ucAddress))
		return -1;

	const ssize_t nResult = write(m_nFD, pBuffer, nCount);
	return nResult < 0 ? -1 : static_cast<int>(nResult);
}

void CI2CMaster::SimulateWrite(const u8* pData, unsigned nCount)
{
	// SSD1306 in horizontal addressing mode: a 0x40 control byte followed by the whole GDDRAM, page by page
	if (nCount < 2 || pData[0] != 0x40)
		return;

	const unsigned nBytes  = nCount - 1;
	const unsigned nWidth  = 128;
	const unsigned nHeight = nBytes / nWidth * 8;
	if (nHeight != 32 && nHeight != 64)
		return;

	FILE* pFile = fopen((m_SimPath + ".tmp").c_str(), "wb");
	if (!pFile)
		return;

	fprintf(pFile, "P1\n# mt32-pi simulated OLED frame %u\n%u %u\n", ++m_nSimFrames, nWidth, nHeight);
	for (unsigned y = 0; y < nHeight; ++y)
	{
		for (unsigned x = 0; x < nWidth; ++x)
		{
			const u8 nByte = pData[1 + (y / 8) * nWidth + x];
			fputc((nByte >> (y % 8)) & 1 ? '1' : '0', pFile);
		}
		fputc('\n', pFile);
	}

	fclose(pFile);
	rename((m_SimPath + ".tmp").c_str(), m_SimPath.c_str());
}

//
// User timer
//
CUserTimer::CUserTimer(CInterruptSystem* pInterruptSystem, TUserTimerHandler* pHandler, void* pParam, bool bUseFIQ)
	: m_pHandler(pHandler),
	  m_pParam(pParam),
	  m_bArmed(false),
	  m_bQuit(false),
	  m_nDeadlineMicros(0)
{
}

CUserTimer::~CUserTimer()
{
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		m_bQuit = true;
	}
	m_Condition.notify_all();
	if (m_Thread.joinable())
		m_Thread.join();
}

bool CUserTimer::Initialize()
{
	m_Thread = std::thread(&CUserTimer::Thread, this);
	return true;
}

void CUserTimer::Stop()
{
	std::lock_guard<std::mutex> Lock(m_Mutex);
	m_bArmed = false;
}

void CUserTimer::Start(unsigned nDelayMicros)
{
	{
		std::lock_guard<std::mutex> Lock(m_Mutex);
		// Re-arming from the handler keeps a steady period relative to the previous deadline
		const u64 nNow = CTimer::GetClockTicks64();
		const u64 nBase = (m_bArmed || m_nDeadlineMicros == 0 || nNow - m_nDeadlineMicros > nDelayMicros) ? nNow : m_nDeadlineMicros;
		m_nDeadlineMicros = nBase + nDelayMicros;
		m_bArmed = true;
	}
	m_Condition.notify_all();
}

void CUserTimer::Thread()
{
	pthread_setname_np(pthread_self(), "mt32pi-timer");

	std::unique_lock<std::mutex> Lock(m_Mutex);
	while (!m_bQuit)
	{
		if (!m_bArmed)
		{
			m_Condition.wait(Lock);
			continue;
		}

		const u64 nNow = CTimer::GetClockTicks64();
		if (nNow < m_nDeadlineMicros)
		{
			// Sleep until the absolute deadline (CTimer ticks are CLOCK_MONOTONIC microseconds since start)
			struct timespec Wake;
			clock_gettime(CLOCK_MONOTONIC, &Wake);
			const u64 nWait = m_nDeadlineMicros - nNow;
			Wake.tv_sec += nWait / 1000000;
			Wake.tv_nsec += (nWait % 1000000) * 1000;
			if (Wake.tv_nsec >= 1000000000)
			{
				Wake.tv_sec += 1;
				Wake.tv_nsec -= 1000000000;
			}
			Lock.unlock();
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &Wake, nullptr);
			Lock.lock();
			continue;
		}

		// One-shot: disarm, then call the handler (which normally re-arms)
		m_bArmed = false;
		Lock.unlock();
		m_pHandler(this, m_pParam);
		Lock.lock();
	}
}
