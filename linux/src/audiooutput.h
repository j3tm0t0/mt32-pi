//
// audiooutput.h - Audio output backends for mt32-pi on Linux
//

#ifndef _audiooutput_h
#define _audiooutput_h

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class CAudioOutput
{
public:
	virtual ~CAudioOutput() = default;

	// Writes interleaved stereo float frames; blocks until accepted by the device
	virtual bool Write(const float* pFrames, size_t nFrames) = 0;

	// Number of buffer underruns recovered so far
	virtual unsigned GetXRunCount() const { return 0; }

	virtual unsigned GetSampleRate() const = 0;
	virtual size_t GetPeriodFrames() const = 0;
	virtual size_t GetBufferFrames() const = 0;
};

// ALSA PCM playback (blocking writes, interleaved stereo)
class CALSAAudioOutput : public CAudioOutput
{
public:
	CALSAAudioOutput();
	virtual ~CALSAAudioOutput() override;

	bool Open(const char* pDevice, unsigned nSampleRate, size_t nPeriodFrames, unsigned nPeriods);

	virtual bool Write(const float* pFrames, size_t nFrames) override;
	virtual unsigned GetXRunCount() const override { return m_nXRuns; }
	virtual unsigned GetSampleRate() const override { return m_nSampleRate; }
	virtual size_t GetPeriodFrames() const override { return m_nPeriodFrames; }
	virtual size_t GetBufferFrames() const override { return m_nBufferFrames; }

	const char* GetFormatName() const;

private:
	enum class TFormat
	{
		Float,
		S32,
		S16,
	};

	bool Recover(int nError);

	void* m_pPCM;
	TFormat m_Format;
	unsigned m_nSampleRate;
	size_t m_nPeriodFrames;
	size_t m_nBufferFrames;
	volatile unsigned m_nXRuns;
	std::vector<uint8_t> m_ConversionBuffer;
};

// 16-bit PCM WAV file writer (used for offline rendering)
class CWAVFileOutput : public CAudioOutput
{
public:
	CWAVFileOutput(unsigned nSampleRate, size_t nPeriodFrames);
	virtual ~CWAVFileOutput() override;

	bool Open(const char* pPath);
	void Close();

	virtual bool Write(const float* pFrames, size_t nFrames) override;
	virtual unsigned GetSampleRate() const override { return m_nSampleRate; }
	virtual size_t GetPeriodFrames() const override { return m_nPeriodFrames; }
	virtual size_t GetBufferFrames() const override { return m_nPeriodFrames; }

	// Peak absolute sample value written so far (0..1); handy for checking that something was rendered
	float GetPeak() const { return m_nPeak; }
	uint64_t GetFramesWritten() const { return m_nFramesWritten; }

private:
	void WriteHeader();

	FILE* m_pFile;
	unsigned m_nSampleRate;
	size_t m_nPeriodFrames;
	uint64_t m_nFramesWritten;
	float m_nPeak;
	std::vector<int16_t> m_ConversionBuffer;
};

#endif
