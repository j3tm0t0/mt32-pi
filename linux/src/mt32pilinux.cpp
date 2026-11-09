//
// mt32pilinux.cpp - mt32-pi application core for Linux
//

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <vector>

#include <circle/logger.h>
#include <circle/timer.h>

#include "lcd/drivers/hd44780.h"
#include "lcd/drivers/ssd1306.h"
#include "mt32pilinux.h"
#include "smf.h"

#ifndef MT32_PI_VERSION
#define MT32_PI_VERSION "<unknown>"
#endif

LOGMODULE("mt32pi");

namespace
{
	constexpr unsigned ActiveSenseTimeoutMillis = 330;
	constexpr int ControlPollTimeoutMillis      = 10;
	constexpr unsigned LCDUpdatePeriodMillis    = 16;
	const char MT32PiFullName[]                 = "mt32-pi " MT32_PI_VERSION;
}

CMT32PiLinux::CMT32PiLinux(CConfig& Config)
	: m_Config(Config),
	  m_pI2CMaster(nullptr),
	  m_pLCD(nullptr),
	  m_pControl(nullptr),
	  m_nLCDUpdateTime(0),
	  m_nMasterVolume(100),
	  m_bDeferredSoundFontSwitchFlag(false),
	  m_nDeferredSoundFontSwitchIndex(0),
	  m_nDeferredSoundFontSwitchTime(0),
	  m_pMT32Synth(nullptr),
	  m_pSoundFontSynth(nullptr),
	  m_pCurrentSynth(nullptr),
	  m_pAudioOutput(nullptr),
	  m_bRunning(false),
	  m_bAudioFailed(false),
	  m_bActiveSenseFlag(false),
	  m_nActiveSenseTime(0),
	  m_nLastMIDITime(0),
	  m_nStatPeriods(0),
	  m_nStatRenderTotalMicros(0),
	  m_nStatRenderMaxMicros(0),
	  m_nLastXRuns(0)
{
}

CMT32PiLinux::~CMT32PiLinux()
{
	m_bRunning = false;
	if (m_AudioThread.joinable())
		m_AudioThread.join();
	if (m_UIThread.joinable())
		m_UIThread.join();

	delete m_pControl;
	delete m_pLCD;
	delete m_pI2CMaster;
	delete m_pAudioOutput;
	delete m_pMT32Synth;
	delete m_pSoundFontSynth;
}

bool CMT32PiLinux::Initialize(const TOptional<TSynth>& ForcedSynth)
{
	LOGNOTE("Init mt32emu");
	InitMT32Synth();

	LOGNOTE("Init FluidSynth");
	InitSoundFontSynth();

	TSynth PreferredSynth = m_Config.SystemDefaultSynth == CConfig::TSystemDefaultSynth::SoundFont ? TSynth::SoundFont : TSynth::MT32;
	if (ForcedSynth)
		PreferredSynth = *ForcedSynth;

	CSynthBase* pSynth = PreferredSynth == TSynth::MT32 ? static_cast<CSynthBase*>(m_pMT32Synth) : static_cast<CSynthBase*>(m_pSoundFontSynth);

	if (!pSynth)
	{
		LOGERR("Preferred synth failed to initialize successfully");
		pSynth = m_pMT32Synth ? static_cast<CSynthBase*>(m_pMT32Synth) : static_cast<CSynthBase*>(m_pSoundFontSynth);
	}

	if (!pSynth)
	{
		LOGERR("No synths available; ROMs/SoundFonts not found");
		return false;
	}

	m_pCurrentSynth = pSynth;
	LOGNOTE("Active synth: %s", pSynth == m_pMT32Synth ? "MT-32" : "SoundFont");
	pSynth->ReportStatus();

	return true;
}

bool CMT32PiLinux::InitMT32Synth()
{
	m_pMT32Synth = new CMT32Synth(m_Config.AudioSampleRate, m_Config.MT32EmuGain, m_Config.MT32EmuReverbGain, m_Config.MT32EmuResamplerQuality);
	if (!m_pMT32Synth->Initialize())
	{
		LOGWARN("mt32emu init failed; no ROMs present?");
		delete m_pMT32Synth;
		m_pMT32Synth = nullptr;
		return false;
	}

	if (m_Config.MT32EmuMIDIChannels == CMT32Synth::TMIDIChannels::Alternate)
		m_pMT32Synth->SetMIDIChannels(m_Config.MT32EmuMIDIChannels);

	m_pMT32Synth->SetReversedStereo(m_Config.MT32EmuReversedStereo);
	m_pMT32Synth->SetUserInterface(&m_UserInterface);

	const CROMManager& ROMManager = m_pMT32Synth->GetROMManager();
	LOGNOTE("MT-32 ROM sets: MT-32 old %s, MT-32 new %s, CM-32L %s; using %s",
		ROMManager.HaveROMSet(TMT32ROMSet::MT32Old) ? "yes" : "no",
		ROMManager.HaveROMSet(TMT32ROMSet::MT32New) ? "yes" : "no",
		ROMManager.HaveROMSet(TMT32ROMSet::CM32L) ? "yes" : "no",
		m_pMT32Synth->GetControlROMName());
	return true;
}

bool CMT32PiLinux::InitSoundFontSynth()
{
	m_pSoundFontSynth = new CSoundFontSynth(m_Config.AudioSampleRate);
	if (!m_pSoundFontSynth->Initialize())
	{
		LOGWARN("FluidSynth init failed; no SoundFonts present?");
		delete m_pSoundFontSynth;
		m_pSoundFontSynth = nullptr;
		return false;
	}

	m_pSoundFontSynth->SetUserInterface(&m_UserInterface);
	return true;
}

//
// MIDI handling (same semantics as CMT32Pi)
//
void CMT32PiLinux::OnShortMessage(u32 nMessage)
{
	if (nMessage == 0xFE)
	{
		m_bActiveSenseFlag = true;
		return;
	}

	m_pCurrentSynth.load()->HandleMIDIShortMessage(nMessage);
}

void CMT32PiLinux::OnSysExMessage(const u8* pData, size_t nSize)
{
	if (!ParseCustomSysEx(pData, nSize))
		m_pCurrentSynth.load()->HandleMIDISysExMessage(pData, nSize);
}

void CMT32PiLinux::OnUnexpectedStatus()
{
	CMIDIParser::OnUnexpectedStatus();
}

void CMT32PiLinux::OnSysExOverflow()
{
	CMIDIParser::OnSysExOverflow();
}

bool CMT32PiLinux::ParseCustomSysEx(const u8* pData, size_t nSize)
{
	if (nSize < 4)
		return false;

	// 'Educational' manufacturer
	if (pData[1] != 0x7D)
		return false;

	const auto Command = static_cast<TCustomSysExCommand>(pData[2]);

	// Reboot (F0 7D 00 F7): exit and let the service manager restart us
	if (nSize == 4 && Command == TCustomSysExCommand::Reboot)
	{
		LOGNOTE("Reboot command received; exiting");
		m_bRunning = false;
		return true;
	}

	if (nSize != 5)
		return false;

	const u8 nParameter = pData[3];
	switch (Command)
	{
		case TCustomSysExCommand::SwitchMT32ROMSet:
		{
			const TMT32ROMSet NewROMSet = static_cast<TMT32ROMSet>(nParameter);
			if (NewROMSet < TMT32ROMSet::Any)
				SwitchMT32ROMSet(NewROMSet);
			return true;
		}

		case TCustomSysExCommand::SwitchSoundFont:
			SwitchSoundFont(nParameter);
			return true;

		case TCustomSysExCommand::SwitchSynth:
			SwitchSynth(static_cast<TSynth>(nParameter));
			return true;

		case TCustomSysExCommand::SetMT32ReversedStereo:
			if (m_pMT32Synth)
				m_pMT32Synth->SetReversedStereo(nParameter);
			return true;

		default:
			return false;
	}
}

void CMT32PiLinux::SwitchSynth(TSynth NewSynth)
{
	CSynthBase* pNewSynth = nullptr;

	if (NewSynth == TSynth::MT32)
		pNewSynth = m_pMT32Synth;
	else if (NewSynth == TSynth::SoundFont)
		pNewSynth = m_pSoundFontSynth;

	if (!pNewSynth)
	{
		LOGWARN("Synth unavailable!");
		LCDLog(TLCDLogType::Warning, "Synth unavailable!");
		return;
	}

	CSynthBase* pCurrentSynth = m_pCurrentSynth.load();
	if (pNewSynth == pCurrentSynth)
	{
		LOGNOTE("Synth already active");
		LCDLog(TLCDLogType::Warning, "Already active!");
		return;
	}

	pCurrentSynth->AllSoundOff();
	m_pCurrentSynth = pNewSynth;
	const char* pMode = NewSynth == TSynth::MT32 ? "MT-32 mode" : "SoundFont mode";
	LOGNOTE("Switching to %s", pMode);
	LCDLog(TLCDLogType::Notice, "%s", pMode);
}

void CMT32PiLinux::SwitchMT32ROMSet(TMT32ROMSet ROMSet)
{
	if (!m_pMT32Synth)
		return;

	LOGNOTE("Switching to ROM set %d", static_cast<u8>(ROMSet));
	if (m_pMT32Synth->SwitchROMSet(ROMSet))
	{
		LOGNOTE("Now using %s", m_pMT32Synth->GetControlROMName());
		if (m_pCurrentSynth.load() == m_pMT32Synth)
			m_pMT32Synth->ReportStatus();
	}
	else
		LOGNOTE("ROM set %d not switched (already active or unavailable)", static_cast<u8>(ROMSet));
}

void CMT32PiLinux::SwitchSoundFont(size_t nIndex)
{
	if (!m_pSoundFontSynth)
		return;

	LOGNOTE("Switching to SoundFont %zu", nIndex);
	if (m_pSoundFontSynth->SwitchSoundFont(nIndex) && m_pCurrentSynth.load() == m_pSoundFontSynth)
		m_pSoundFontSynth->ReportStatus();
}

//
// Audio
//
void CMT32PiLinux::RenderPeriod(float* pBuffer, size_t nFrames)
{
	m_pCurrentSynth.load()->Render(pBuffer, nFrames);

	if (m_Config.AudioReversedStereo)
	{
		for (size_t i = 0; i < nFrames * 2; i += 2)
			std::swap(pBuffer[i], pBuffer[i + 1]);
	}
}

void CMT32PiLinux::ConfigureRealtime(const char* pThreadName, int nPriority, int nCPU)
{
	pthread_setname_np(pthread_self(), pThreadName);

	if (nCPU >= 0)
	{
		cpu_set_t CPUSet;
		CPU_ZERO(&CPUSet);
		CPU_SET(nCPU, &CPUSet);
		const int nResult = pthread_setaffinity_np(pthread_self(), sizeof(CPUSet), &CPUSet);
		if (nResult != 0)
			LOGWARN("Cannot pin %s thread to CPU %d: %s", pThreadName, nCPU, strerror(nResult));
		else
			LOGNOTE("%s thread pinned to CPU %d", pThreadName, nCPU);
	}

	if (nPriority <= 0)
	{
		LOGNOTE("%s thread uses normal scheduling", pThreadName);
		return;
	}

	sched_param Param{};
	Param.sched_priority = std::min(nPriority, sched_get_priority_max(SCHED_FIFO));
	const int nResult = pthread_setschedparam(pthread_self(), SCHED_FIFO, &Param);
	if (nResult != 0)
		LOGWARN("Cannot set SCHED_FIFO priority %d for %s thread: %s (grant rtprio via systemd LimitRTPRIO or limits.conf)", Param.sched_priority, pThreadName, strerror(nResult));
	else
		LOGNOTE("%s thread running SCHED_FIFO priority %d", pThreadName, Param.sched_priority);
}

void CMT32PiLinux::AudioThread()
{
	ConfigureRealtime("mt32pi-audio", m_Config.LinuxRealtimePriority, m_Config.LinuxAudioCPU);

	const size_t nFrames = m_pAudioOutput->GetPeriodFrames();
	std::vector<float> Buffer(nFrames * 2);

	while (m_bRunning)
	{
		const u64 nStart = CTimer::GetClockTicks64();
		RenderPeriod(Buffer.data(), nFrames);
		const unsigned nRenderMicros = static_cast<unsigned>(CTimer::GetClockTicks64() - nStart);

		m_nStatPeriods.fetch_add(1, std::memory_order_relaxed);
		m_nStatRenderTotalMicros.fetch_add(nRenderMicros, std::memory_order_relaxed);
		unsigned nMax = m_nStatRenderMaxMicros.load(std::memory_order_relaxed);
		while (nRenderMicros > nMax && !m_nStatRenderMaxMicros.compare_exchange_weak(nMax, nRenderMicros, std::memory_order_relaxed))
			;

		if (!m_pAudioOutput->Write(Buffer.data(), nFrames))
		{
			LOGERR("Audio output failed");
			m_bAudioFailed = true;
			m_bRunning     = false;
			break;
		}
	}
}

void CMT32PiLinux::ReportStats()
{
	const unsigned nPeriods = m_nStatPeriods.exchange(0);
	const u64 nTotal        = m_nStatRenderTotalMicros.exchange(0);
	const unsigned nMax     = m_nStatRenderMaxMicros.exchange(0);
	const unsigned nXRuns   = m_pAudioOutput->GetXRunCount();

	const double nPeriodMicros = 1e6 * m_pAudioOutput->GetPeriodFrames() / m_pAudioOutput->GetSampleRate();
	const double nAvg          = nPeriods ? static_cast<double>(nTotal) / nPeriods : 0.0;

	LOGNOTE("periods=%u render avg=%.0fus max=%uus (budget %.0fus, load avg %.1f%% peak %.1f%%) xruns=%u (+%u)",
		nPeriods, nAvg, nMax, nPeriodMicros,
		100.0 * nAvg / nPeriodMicros, 100.0 * nMax / nPeriodMicros,
		nXRuns, nXRuns - m_nLastXRuns);

	m_nLastXRuns = nXRuns;
}

void CMT32PiLinux::UpdateControl()
{
	const unsigned nTicks = CTimer::Get()->GetTicks();

	// Active sensing timeout
	if (m_bActiveSenseFlag && (nTicks - m_nLastMIDITime) >= MSEC2HZ(ActiveSenseTimeoutMillis))
	{
		m_pCurrentSynth.load()->AllSoundOff();
		m_bActiveSenseFlag = false;
		LOGNOTE("Active sense timeout - turning notes off");
	}

	// Deferred SoundFont switch
	if (m_bDeferredSoundFontSwitchFlag)
	{
		// Delay switch if scrolling a long SoundFont name
		if (m_UserInterface.IsScrolling())
			m_nDeferredSoundFontSwitchTime = nTicks;
		else if ((nTicks - m_nDeferredSoundFontSwitchTime) >= static_cast<unsigned>(m_Config.ControlSwitchTimeout) * HZ)
		{
			SwitchSoundFont(m_nDeferredSoundFontSwitchIndex);
			m_bDeferredSoundFontSwitchFlag = false;
		}
	}
}

int CMT32PiLinux::Run(const char* pMIDISource, unsigned nStatsIntervalSecs, const char* pPlayFile, bool bLoop)
{
	// Optional SMF playback through the live path (useful without MIDI hardware and for load testing)
	std::vector<TSMFEvent> PlayEvents;
	if (pPlayFile && *pPlayFile)
	{
		std::string Error;
		if (!LoadStandardMIDIFile(pPlayFile, PlayEvents, Error))
		{
			LOGERR("Cannot load '%s': %s", pPlayFile, Error.c_str());
			return 1;
		}
		LOGNOTE("Playing '%s' (%zu events)%s", pPlayFile, PlayEvents.size(), bLoop ? ", looped" : "");
	}

	// Lock memory to avoid page faults in the audio path, but only if it cannot starve allocations
	if (m_Config.LinuxLockMemory)
	{
		rlimit Limit;
		if (getrlimit(RLIMIT_MEMLOCK, &Limit) == 0 && Limit.rlim_cur == RLIM_INFINITY)
		{
			if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
				LOGNOTE("Memory locked");
			else
				LOGWARN("mlockall failed: %s", strerror(errno));
		}
		else
			LOGNOTE("RLIMIT_MEMLOCK is limited; not locking memory (set LimitMEMLOCK=infinity to enable)");
	}

	CALSAAudioOutput* pALSA = new CALSAAudioOutput();
	m_pAudioOutput = pALSA;
	if (!pALSA->Open(m_Config.LinuxALSADevice, m_Config.AudioSampleRate, m_Config.AudioChunkSize, m_Config.LinuxALSAPeriods))
		return 1;

	const double nLatencyMillis = 1000.0 * pALSA->GetBufferFrames() / pALSA->GetSampleRate();
	LOGNOTE("Output buffer latency: %.1f ms", nLatencyMillis);

	const bool bMIDIAvailable = m_MIDIInput.Open("mt32-pi", m_Config.LinuxMIDIAutoConnect);
	if (!bMIDIAvailable)
		LOGWARN("ALSA sequencer unavailable; no USB/ALSA MIDI input");
	else if (pMIDISource && *pMIDISource)
		m_MIDIInput.Connect(pMIDISource);

	// GPIO UART MIDI, like the bare-metal default; skipped if the port is the kernel console
	const char* pSerialDevice = m_Config.LinuxSerialDevice;
	if (*pSerialDevice && strcmp(pSerialDevice, "none") != 0)
	{
		if (access(pSerialDevice, F_OK) != 0)
			LOGNOTE("Serial MIDI device %s not present (enable_uart=1 / dtoverlay needed?)", pSerialDevice);
		else if (CSerialMIDIInput::IsKernelConsole(pSerialDevice))
			LOGWARN("%s is the kernel console; not using it for MIDI (remove console=serial0 from cmdline.txt)", pSerialDevice);
		else
			m_SerialMIDI.Open(pSerialDevice, m_Config.MIDIGPIOBaudRate, m_Config.MIDIGPIOThru);
	}

	if (!bMIDIAvailable && !m_SerialMIDI.IsOpen())
		LOGWARN("Running without MIDI input");

	InitFrontPanel();

	m_bRunning    = true;
	m_AudioThread = std::thread(&CMT32PiLinux::AudioThread, this);
	if (m_pLCD)
		m_UIThread = std::thread(&CMT32PiLinux::UIThread, this);

	pthread_setname_np(pthread_self(), "mt32pi-control");

	std::vector<pollfd> FDs;
	if (bMIDIAvailable)
		FDs = m_MIDIInput.GetPollFDs();
	const size_t nSeqFDs = FDs.size();
	if (m_SerialMIDI.IsOpen())
		FDs.push_back({m_SerialMIDI.GetFD(), POLLIN, 0});

	unsigned nLastStatsTicks = CTimer::Get()->GetTicks();
	const auto MIDIHandler   = [this](const uint8_t* pData, size_t nSize) {
		ParseMIDIBytes(pData, nSize);
		m_nLastMIDITime = CTimer::Get()->GetTicks();
	};

	size_t nNextPlayEvent = 0;
	u64 nPlayStart        = CTimer::GetClockTicks64();

	while (m_bRunning)
	{
		// Wake up in time for the next file event
		int nTimeout = ControlPollTimeoutMillis;
		if (nNextPlayEvent < PlayEvents.size())
		{
			const u64 nDue = nPlayStart + static_cast<u64>(PlayEvents[nNextPlayEvent].nTimeSeconds * 1e6);
			const u64 nNow = CTimer::GetClockTicks64();
			nTimeout = nDue <= nNow ? 0 : std::min<int>(nTimeout, static_cast<int>((nDue - nNow + 999) / 1000));
		}

		if (!FDs.empty())
		{
			const int nReady = poll(FDs.data(), FDs.size(), nTimeout);
			if (nReady > 0)
			{
				bool bSeqReady = false;
				for (size_t i = 0; i < nSeqFDs; ++i)
					bSeqReady |= (FDs[i].revents & POLLIN) != 0;
				if (bSeqReady)
					m_MIDIInput.Process(MIDIHandler);

				if (m_SerialMIDI.IsOpen() && (FDs.back().revents & (POLLIN | POLLERR | POLLHUP)))
				{
					uint8_t Buffer[1024];
					int nRead;
					while ((nRead = m_SerialMIDI.Read(Buffer, sizeof(Buffer))) > 0)
						MIDIHandler(Buffer, static_cast<size_t>(nRead));

					if (nRead < 0)
					{
						// Device gone (e.g. USB-serial unplugged); stop polling it
						m_SerialMIDI.Close();
						FDs.pop_back();
					}
				}
			}
		}
		else if (nTimeout > 0)
			poll(nullptr, 0, nTimeout);

		if (!PlayEvents.empty())
		{
			const u64 nElapsed = CTimer::GetClockTicks64() - nPlayStart;
			while (nNextPlayEvent < PlayEvents.size() && static_cast<u64>(PlayEvents[nNextPlayEvent].nTimeSeconds * 1e6) <= nElapsed)
			{
				const TSMFEvent& Event = PlayEvents[nNextPlayEvent++];
				MIDIHandler(Event.Bytes.data(), Event.Bytes.size());
			}

			if (nNextPlayEvent == PlayEvents.size())
			{
				// Let the last notes ring out before looping/finishing
				if (nElapsed >= static_cast<u64>((PlayEvents.back().nTimeSeconds + 2.0) * 1e6))
				{
					if (bLoop)
					{
						nNextPlayEvent = 0;
						nPlayStart     = CTimer::GetClockTicks64();
					}
					else
					{
						LOGNOTE("Playback finished");
						PlayEvents.clear();
						if (FDs.empty())
							m_bRunning = false;
					}
				}
			}
		}

		if (m_pControl)
			m_pControl->Update();
		ProcessEventQueue();
		UpdateControl();

		if (nStatsIntervalSecs)
		{
			const unsigned nTicks = CTimer::Get()->GetTicks();
			if (nTicks - nLastStatsTicks >= nStatsIntervalSecs * HZ)
			{
				ReportStats();
				nLastStatsTicks = nTicks;
			}
		}
	}

	LOGNOTE("Shutting down");
	if (m_AudioThread.joinable())
		m_AudioThread.join();
	if (m_UIThread.joinable())
		m_UIThread.join();

	if (nStatsIntervalSecs)
		ReportStats();

	return m_bAudioFailed ? 1 : 0;
}

int CMT32PiLinux::RenderFile(const char* pMIDIPath, const char* pWAVPath, double nTailSeconds)
{
	std::vector<TSMFEvent> Events;
	std::string Error;
	if (!LoadStandardMIDIFile(pMIDIPath, Events, Error))
	{
		LOGERR("Cannot load '%s': %s", pMIDIPath, Error.c_str());
		return 1;
	}

	const unsigned nSampleRate = m_Config.AudioSampleRate;
	const size_t nPeriodFrames = m_Config.AudioChunkSize;

	CWAVFileOutput* pWAV = new CWAVFileOutput(nSampleRate, nPeriodFrames);
	m_pAudioOutput       = pWAV;
	if (!pWAV->Open(pWAVPath))
		return 1;

	const double nDuration = (Events.empty() ? 0.0 : Events.back().nTimeSeconds) + nTailSeconds;
	const u64 nTotalFrames = static_cast<u64>(nDuration * nSampleRate);
	LOGNOTE("Rendering %zu events, %.1f s of audio", Events.size(), nDuration);

	std::vector<float> Buffer(nPeriodFrames * 2);
	size_t nNextEvent = 0;
	u64 nFrame        = 0;
	u64 nRenderMicros = 0;
	unsigned nMaxPeriodMicros = 0;
	m_bRunning = true;

	const u64 nStart = CTimer::GetClockTicks64();
	while (nFrame < nTotalFrames && m_bRunning)
	{
		// Feed all events due before the start of this period, through the same parser as live input
		while (nNextEvent < Events.size() && static_cast<u64>(Events[nNextEvent].nTimeSeconds * nSampleRate) <= nFrame)
		{
			const TSMFEvent& Event = Events[nNextEvent++];
			ParseMIDIBytes(Event.Bytes.data(), Event.Bytes.size());
		}

		const size_t nFrames = static_cast<size_t>(std::min<u64>(nPeriodFrames, nTotalFrames - nFrame));

		const u64 nPeriodStart = CTimer::GetClockTicks64();
		RenderPeriod(Buffer.data(), nFrames);
		const unsigned nPeriodMicros = static_cast<unsigned>(CTimer::GetClockTicks64() - nPeriodStart);
		nRenderMicros += nPeriodMicros;
		nMaxPeriodMicros = std::max(nMaxPeriodMicros, nPeriodMicros);

		if (!pWAV->Write(Buffer.data(), nFrames))
		{
			LOGERR("Write error");
			return 1;
		}

		nFrame += nFrames;
	}
	const double nWallSeconds = (CTimer::GetClockTicks64() - nStart) / 1e6;

	pWAV->Close();

	const double nBudgetMicros = 1e6 * nPeriodFrames / nSampleRate;
	LOGNOTE("Wrote %s: %.1f s audio in %.2f s (%.1fx realtime), peak %.3f",
		pWAVPath, static_cast<double>(nFrame) / nSampleRate, nWallSeconds,
		nWallSeconds > 0 ? (static_cast<double>(nFrame) / nSampleRate) / nWallSeconds : 0.0,
		pWAV->GetPeak());
	LOGNOTE("Render per %zu-frame period: avg %.0fus, max %uus (budget %.0fus)",
		nPeriodFrames, nFrame ? nRenderMicros / (static_cast<double>(nFrame) / nPeriodFrames) : 0.0,
		nMaxPeriodMicros, nBudgetMicros);

	return pWAV->GetPeak() > 0.0f ? 0 : 2;
}

//
// Front panel: LCD/OLED and buttons/encoder (same behaviour as CMT32Pi)
//
void CMT32PiLinux::InitFrontPanel()
{
	CGPIOPin::SetChip(m_Config.LinuxGPIOChip);

	const bool bNeedI2C = m_Config.LCDType == CConfig::TLCDType::HD44780I2C ||
	                      m_Config.LCDType == CConfig::TLCDType::SH1106I2C ||
	                      m_Config.LCDType == CConfig::TLCDType::SSD1306I2C;
	if (bNeedI2C)
	{
		m_pI2CMaster = new CI2CMaster(m_Config.LinuxI2CDevice);
		if (!m_pI2CMaster->Initialize())
		{
			delete m_pI2CMaster;
			m_pI2CMaster = nullptr;
		}
	}

	switch (m_Config.LCDType)
	{
		case CConfig::TLCDType::HD44780FourBit:
			m_pLCD = new CHD44780FourBit(m_Config.LCDWidth, m_Config.LCDHeight);
			break;

		case CConfig::TLCDType::HD44780I2C:
			if (m_pI2CMaster)
				m_pLCD = new CHD44780I2C(m_pI2CMaster, m_Config.LCDI2CLCDAddress, m_Config.LCDWidth, m_Config.LCDHeight);
			break;

		case CConfig::TLCDType::SH1106I2C:
			if (m_pI2CMaster)
				m_pLCD = new CSH1106(m_pI2CMaster, m_Config.LCDI2CLCDAddress, m_Config.LCDWidth, m_Config.LCDHeight, m_Config.LCDRotation);
			break;

		case CConfig::TLCDType::SSD1306I2C:
			if (m_pI2CMaster)
				m_pLCD = new CSSD1306(m_pI2CMaster, m_Config.LCDI2CLCDAddress, m_Config.LCDWidth, m_Config.LCDHeight, m_Config.LCDRotation, m_Config.LCDMirror);
			break;

		default:
			break;
	}

	if (m_pLCD)
	{
		if (m_pLCD->Initialize())
		{
			LOGNOTE("LCD initialized (%ux%u)", m_pLCD->Width(), m_pLCD->Height());
			if (m_pLCD->GetType() == CLCD::TType::Graphical && !m_Config.SystemVerbose)
				m_pLCD->DrawImage(TImage::MT32PiLogo, true);
			else
				m_pLCD->Print(MT32PiFullName, CUserInterface::CenterMessageOffset(*m_pLCD, MT32PiFullName), 0, false, true);
		}
		else
		{
			LOGWARN("LCD init failed; invalid dimensions?");
			delete m_pLCD;
			m_pLCD = nullptr;
		}
	}

	if (m_Config.ControlScheme == CConfig::TControlScheme::SimpleButtons)
		m_pControl = new CControlSimpleButtons(m_EventQueue);
	else if (m_Config.ControlScheme == CConfig::TControlScheme::SimpleEncoder)
		m_pControl = new CControlSimpleEncoder(m_EventQueue, m_Config.ControlEncoderType, m_Config.ControlEncoderReversed);

	if (m_pControl)
	{
		if (m_pControl->Initialize())
		{
			// The debounce history starts zeroed, which reads as "all buttons pressed" until it has filled up
			// (16 polls at nominally 1 kHz; slower with coarse timers, e.g. in VMs). Bare metal never notices
			// because its main loop starts much later.
			CTimer::SimpleMsDelay(100);
			LOGNOTE("Controls initialized (%s)", m_Config.ControlScheme == CConfig::TControlScheme::SimpleButtons ? "simple_buttons" : "simple_encoder");
		}
		else
		{
			LOGWARN("Control init failed");
			delete m_pControl;
			m_pControl = nullptr;
		}
	}
}

void CMT32PiLinux::UIThread()
{
	pthread_setname_np(pthread_self(), "mt32pi-ui");

	// Let the splash screen show briefly, then display current MT-32 ROM version/SoundFont
	CTimer::SimpleMsDelay(1000);
	m_pLCD->Clear();
	m_pCurrentSynth.load()->ReportStatus();

	while (m_bRunning)
	{
		const unsigned nTicks = CTimer::GetClockTicks();
		if ((nTicks - m_nLCDUpdateTime) >= Utility::MillisToTicks(LCDUpdatePeriodMillis))
		{
			m_UserInterface.Update(*m_pLCD, *m_pCurrentSynth.load(), nTicks);
			m_nLCDUpdateTime = nTicks;
		}

		CTimer::SimpleMsDelay(2);
	}

	m_pLCD->Clear();
}

void CMT32PiLinux::ProcessEventQueue()
{
	TEvent Buffer[EventQueueSize];
	const size_t nEvents = m_EventQueue.Dequeue(Buffer, EventQueueSize);

	for (size_t i = 0; i < nEvents; ++i)
	{
		const TEvent& Event = Buffer[i];

		switch (Event.Type)
		{
			case TEventType::Button:
				ProcessButtonEvent(Event.Button);
				break;

			case TEventType::SwitchSynth:
				SwitchSynth(Event.SwitchSynth.Synth);
				break;

			case TEventType::SwitchMT32ROMSet:
				SwitchMT32ROMSet(Event.SwitchMT32ROMSet.ROMSet);
				break;

			case TEventType::SwitchSoundFont:
				DeferSwitchSoundFont(Event.SwitchSoundFont.Index);
				break;

			case TEventType::AllSoundOff:
				if (m_pMT32Synth)
					m_pMT32Synth->AllSoundOff();
				if (m_pSoundFontSynth)
					m_pSoundFontSynth->AllSoundOff();
				break;

			case TEventType::DisplayImage:
				m_UserInterface.DisplayImage(Event.DisplayImage.Image);
				break;

			case TEventType::Encoder:
				SetMasterVolume(m_nMasterVolume + Event.Encoder.nDelta);
				break;
		}
	}
}

void CMT32PiLinux::ProcessButtonEvent(const TButtonEvent& Event)
{
	if (Event.Button == TButton::EncoderButton)
	{
		LCDLog(TLCDLogType::Notice, "Enc. button %s", Event.bPressed ? "PRESSED" : "RELEASED");
		return;
	}

	if (!Event.bPressed)
		return;

	LOGDBG("Button %d%s", static_cast<int>(Event.Button), Event.bRepeat ? " (repeat)" : "");

	if (Event.Button == TButton::Button1 && !Event.bRepeat)
	{
		// Swap synths
		SwitchSynth(m_pCurrentSynth.load() == m_pMT32Synth ? TSynth::SoundFont : TSynth::MT32);
	}
	else if (Event.Button == TButton::Button2 && !Event.bRepeat)
	{
		if (m_pCurrentSynth.load() == m_pMT32Synth)
			NextMT32ROMSet();
		else if (m_pSoundFontSynth)
		{
			// Next SoundFont
			const size_t nSoundFonts = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontCount();

			if (!nSoundFonts)
				LCDLog(TLCDLogType::Error, "No SoundFonts!");
			else
			{
				size_t nNextSoundFont;
				if (m_bDeferredSoundFontSwitchFlag)
					nNextSoundFont = (m_nDeferredSoundFontSwitchIndex + 1) % nSoundFonts;
				else
				{
					const size_t nCurrentSoundFont = m_pSoundFontSynth->GetSoundFontIndex();
					nNextSoundFont = nCurrentSoundFont > nSoundFonts ? 0 : (nCurrentSoundFont + 1) % nSoundFonts;
				}

				DeferSwitchSoundFont(nNextSoundFont);
			}
		}
	}
	else if (Event.Button == TButton::Button3)
		SetMasterVolume(m_nMasterVolume - 1);
	else if (Event.Button == TButton::Button4)
		SetMasterVolume(m_nMasterVolume + 1);
}

void CMT32PiLinux::NextMT32ROMSet()
{
	if (!m_pMT32Synth)
		return;

	LOGNOTE("Switching to next ROM set");
	if (m_pMT32Synth->NextROMSet())
	{
		LOGNOTE("Now using %s", m_pMT32Synth->GetControlROMName());
		if (m_pCurrentSynth.load() == m_pMT32Synth)
			m_pMT32Synth->ReportStatus();
	}
}

void CMT32PiLinux::DeferSwitchSoundFont(size_t nIndex)
{
	if (!m_pSoundFontSynth)
		return;

	const char* pName = m_pSoundFontSynth->GetSoundFontManager().GetSoundFontName(nIndex);
	LCDLog(TLCDLogType::Notice, "SF %zu: %s", nIndex, pName ? pName : "- N/A -");
	m_nDeferredSoundFontSwitchIndex = nIndex;
	m_nDeferredSoundFontSwitchTime  = CTimer::Get()->GetTicks();
	m_bDeferredSoundFontSwitchFlag  = true;
}

void CMT32PiLinux::SetMasterVolume(s32 nVolume)
{
	m_nMasterVolume = Utility::Clamp(nVolume, 0, 100);

	if (m_pMT32Synth)
		m_pMT32Synth->SetMasterVolume(m_nMasterVolume);
	if (m_pSoundFontSynth)
		m_pSoundFontSynth->SetMasterVolume(m_nMasterVolume);

	LOGDBG("Volume: %d", m_nMasterVolume);
	if (m_pCurrentSynth.load() == m_pSoundFontSynth)
		LCDLog(TLCDLogType::Notice, "Volume: %d", m_nMasterVolume);
}

void CMT32PiLinux::LCDLog(TLCDLogType Type, const char* pFormat, ...)
{
	if (!m_pLCD)
		return;

	char Buffer[256];
	va_list Args;
	va_start(Args, pFormat);
	vsnprintf(Buffer, sizeof(Buffer), pFormat, Args);
	va_end(Args);

	if (Type == TLCDLogType::Startup)
	{
		if (m_pLCD->GetType() == CLCD::TType::Graphical && !m_Config.SystemVerbose)
			return;
		m_pLCD->Print(Buffer, CUserInterface::CenterMessageOffset(*m_pLCD, Buffer), 1, true, true);
	}
	else
		m_UserInterface.ShowSystemMessage(Buffer, Type == TLCDLogType::Spinner);
}
