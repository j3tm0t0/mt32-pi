//
// mt32pilinux.h - mt32-pi application core for Linux
//
// Linux counterpart of CMT32Pi (src/mt32pi.cpp). It reuses the portable
// synth, parser, config and ROM/SoundFont management code unchanged and
// replaces the Circle-specific wiring:
//
//   Circle (bare metal)                 Linux
//   ----------------------------------  ----------------------------------
//   Core 0: MainTask (busy loop)        Control thread: poll() on ALSA seq
//   Core 1: UITask (LCD/MiSTer)         (not yet ported)
//   Core 2: AudioTask (busy poll)       Audio thread: SCHED_FIFO + blocking
//                                       ALSA writes
//

#ifndef _mt32pilinux_h
#define _mt32pilinux_h

#include <atomic>
#include <string>
#include <thread>

#include <circle/types.h>

#include <circle/i2cmaster.h>

#include "config.h"
#include "control/control.h"
#include "event.h"
#include "lcd/lcd.h"
#include "lcd/ui.h"
#include "midiparser.h"
#include "optional.h"
#include "synth/mt32synth.h"
#include "synth/soundfontsynth.h"
#include "synth/synth.h"

#include "alsamidi.h"
#include "audiooutput.h"
#include "serialmidi.h"

class CMT32PiLinux : public CMIDIParser
{
public:
	CMT32PiLinux(CConfig& Config);
	~CMT32PiLinux();

	// Creates synths; must be called before Run() or RenderFile()
	bool Initialize(const TOptional<TSynth>& ForcedSynth);

	// Real-time mode: ALSA MIDI in -> synth -> ALSA PCM out, until stopped.
	// If pPlayFile is given, that Standard MIDI File is also fed in real time (looped if bLoop).
	int Run(const char* pMIDISource, unsigned nStatsIntervalSecs, const char* pPlayFile = nullptr, bool bLoop = false);

	// Offline mode: render a Standard MIDI File into a WAV file as fast as possible
	int RenderFile(const char* pMIDIPath, const char* pWAVPath, double nTailSeconds);

	void Stop() { m_bRunning = false; }

private:
	enum class TCustomSysExCommand : u8
	{
		Reboot                = 0x00,
		SwitchMT32ROMSet      = 0x01,
		SwitchSoundFont       = 0x02,
		SwitchSynth           = 0x03,
		SetMT32ReversedStereo = 0x04,
	};

	// CMIDIParser
	virtual void OnShortMessage(u32 nMessage) override;
	virtual void OnSysExMessage(const u8* pData, size_t nSize) override;
	virtual void OnUnexpectedStatus() override;
	virtual void OnSysExOverflow() override;

	bool InitMT32Synth();
	bool InitSoundFontSynth();

	bool ParseCustomSysEx(const u8* pData, size_t nSize);
	void SwitchSynth(TSynth Synth);
	void SwitchMT32ROMSet(TMT32ROMSet ROMSet);
	void SwitchSoundFont(size_t nIndex);
	void NextMT32ROMSet();
	void DeferSwitchSoundFont(size_t nIndex);
	void SetMasterVolume(s32 nVolume);

	// Front panel (LCD/OLED, buttons/encoder) - live mode only
	enum class TLCDLogType
	{
		Startup,
		Error,
		Warning,
		Notice,
		Spinner,
	};

	void InitFrontPanel();
	void UIThread();
	void ProcessEventQueue();
	void ProcessButtonEvent(const TButtonEvent& Event);
	void LCDLog(TLCDLogType Type, const char* pFormat, ...) __attribute__((format(printf, 3, 4)));

	void AudioThread();
	void ConfigureRealtime(const char* pThreadName, int nPriority, int nCPU);
	void UpdateControl();
	void ReportStats();

	void RenderPeriod(float* pBuffer, size_t nFrames);

	CConfig& m_Config;

	CUserInterface m_UserInterface;
	CI2CMaster* m_pI2CMaster;
	CLCD* m_pLCD;
	CControl* m_pControl;
	TEventQueue m_EventQueue;
	std::thread m_UIThread;
	unsigned m_nLCDUpdateTime;

	u8 m_nMasterVolume;

	// Deferred SoundFont switch (lets the user step through names before loading)
	bool m_bDeferredSoundFontSwitchFlag;
	size_t m_nDeferredSoundFontSwitchIndex;
	unsigned m_nDeferredSoundFontSwitchTime;

	CMT32Synth* m_pMT32Synth;
	CSoundFontSynth* m_pSoundFontSynth;
	std::atomic<CSynthBase*> m_pCurrentSynth;

	CALSAMIDIInput m_MIDIInput;
	CSerialMIDIInput m_SerialMIDI;
	CAudioOutput* m_pAudioOutput;
	std::thread m_AudioThread;

	std::atomic<bool> m_bRunning;
	std::atomic<bool> m_bAudioFailed;

	// Active sensing
	bool m_bActiveSenseFlag;
	unsigned m_nActiveSenseTime;
	unsigned m_nLastMIDITime;

	// Audio statistics (written by audio thread, read by control thread)
	std::atomic<unsigned> m_nStatPeriods;
	std::atomic<u64> m_nStatRenderTotalMicros;
	std::atomic<unsigned> m_nStatRenderMaxMicros;
	unsigned m_nLastXRuns;
};

#endif
