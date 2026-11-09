//
// audiooutput.cpp - Audio output backends for mt32-pi on Linux
//

#include <alsa/asoundlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <circle/logger.h>

#include "audiooutput.h"

LOGMODULE("audio");

namespace
{
	inline float ClampSample(float nSample)
	{
		return nSample > 1.0f ? 1.0f : (nSample < -1.0f ? -1.0f : nSample);
	}
}

//
// ALSA
//
CALSAAudioOutput::CALSAAudioOutput()
	: m_pPCM(nullptr),
	  m_Format(TFormat::Float),
	  m_nSampleRate(0),
	  m_nPeriodFrames(0),
	  m_nBufferFrames(0),
	  m_nXRuns(0)
{
}

CALSAAudioOutput::~CALSAAudioOutput()
{
	if (m_pPCM)
	{
		snd_pcm_t* pPCM = static_cast<snd_pcm_t*>(m_pPCM);
		snd_pcm_drop(pPCM);
		snd_pcm_close(pPCM);
	}
}

const char* CALSAAudioOutput::GetFormatName() const
{
	switch (m_Format)
	{
		case TFormat::Float: return "FLOAT_LE";
		case TFormat::S32:   return "S32_LE";
		case TFormat::S16:   return "S16_LE";
	}
	return "?";
}

bool CALSAAudioOutput::Open(const char* pDevice, unsigned nSampleRate, size_t nPeriodFrames, unsigned nPeriods)
{
	snd_pcm_t* pPCM;
	int nResult = snd_pcm_open(&pPCM, pDevice, SND_PCM_STREAM_PLAYBACK, 0);
	if (nResult < 0)
	{
		LOGERR("Cannot open ALSA device '%s': %s", pDevice, snd_strerror(nResult));
		return false;
	}

	m_pPCM = pPCM;

	snd_pcm_hw_params_t* pHWParams;
	snd_pcm_hw_params_alloca(&pHWParams);
	snd_pcm_hw_params_any(pPCM, pHWParams);

	if ((nResult = snd_pcm_hw_params_set_access(pPCM, pHWParams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
	{
		LOGERR("Interleaved access not supported: %s", snd_strerror(nResult));
		return false;
	}

	// Prefer formats that avoid conversion/precision loss; fall back to 16-bit
	const std::pair<snd_pcm_format_t, TFormat> Formats[] = {
		{SND_PCM_FORMAT_FLOAT_LE, TFormat::Float},
		{SND_PCM_FORMAT_S32_LE, TFormat::S32},
		{SND_PCM_FORMAT_S16_LE, TFormat::S16},
	};

	bool bFormatSet = false;
	for (const auto& Format : Formats)
	{
		if (snd_pcm_hw_params_test_format(pPCM, pHWParams, Format.first) == 0 &&
		    snd_pcm_hw_params_set_format(pPCM, pHWParams, Format.first) == 0)
		{
			m_Format   = Format.second;
			bFormatSet = true;
			break;
		}
	}

	if (!bFormatSet)
	{
		LOGERR("No supported sample format");
		return false;
	}

	if ((nResult = snd_pcm_hw_params_set_channels(pPCM, pHWParams, 2)) < 0)
	{
		LOGERR("Stereo not supported: %s", snd_strerror(nResult));
		return false;
	}

	unsigned nRate = nSampleRate;
	if ((nResult = snd_pcm_hw_params_set_rate_near(pPCM, pHWParams, &nRate, nullptr)) < 0)
	{
		LOGERR("Cannot set sample rate: %s", snd_strerror(nResult));
		return false;
	}

	// The synths are already running at the configured rate; a different device rate would detune everything
	if (nRate != nSampleRate)
	{
		LOGERR("Device '%s' does not support %u Hz (offers %u Hz); use a plughw: device or set [audio] sample_rate = %u", pDevice, nSampleRate, nRate, nRate);
		return false;
	}

	snd_pcm_uframes_t nPeriod = nPeriodFrames;
	snd_pcm_hw_params_set_period_size_near(pPCM, pHWParams, &nPeriod, nullptr);

	snd_pcm_uframes_t nBuffer = nPeriod * std::max(2u, nPeriods);
	snd_pcm_hw_params_set_buffer_size_near(pPCM, pHWParams, &nBuffer);

	if ((nResult = snd_pcm_hw_params(pPCM, pHWParams)) < 0)
	{
		LOGERR("Cannot apply hardware parameters: %s", snd_strerror(nResult));
		return false;
	}

	snd_pcm_hw_params_get_period_size(pHWParams, &nPeriod, nullptr);
	snd_pcm_hw_params_get_buffer_size(pHWParams, &nBuffer);

	// Start playback once the whole buffer is full; wake up once per period
	snd_pcm_sw_params_t* pSWParams;
	snd_pcm_sw_params_alloca(&pSWParams);
	snd_pcm_sw_params_current(pPCM, pSWParams);
	snd_pcm_sw_params_set_start_threshold(pPCM, pSWParams, nBuffer - nBuffer % nPeriod);
	snd_pcm_sw_params_set_avail_min(pPCM, pSWParams, nPeriod);
	if ((nResult = snd_pcm_sw_params(pPCM, pSWParams)) < 0)
	{
		LOGERR("Cannot apply software parameters: %s", snd_strerror(nResult));
		return false;
	}

	m_nSampleRate   = nRate;
	m_nPeriodFrames = nPeriod;
	m_nBufferFrames = nBuffer;

	LOGNOTE("ALSA '%s': %u Hz, %s, period %lu frames, buffer %lu frames (%.1f ms)",
		pDevice, nRate, GetFormatName(),
		static_cast<unsigned long>(nPeriod), static_cast<unsigned long>(nBuffer),
		1000.0 * nBuffer / nRate);

	return true;
}

bool CALSAAudioOutput::Recover(int nError)
{
	snd_pcm_t* pPCM = static_cast<snd_pcm_t*>(m_pPCM);

	if (nError == -EPIPE)
		m_nXRuns = m_nXRuns + 1;

	const int nResult = snd_pcm_recover(pPCM, nError, 1);
	if (nResult < 0)
	{
		LOGERR("Unrecoverable ALSA error: %s", snd_strerror(nError));
		return false;
	}

	return true;
}

bool CALSAAudioOutput::Write(const float* pFrames, size_t nFrames)
{
	snd_pcm_t* pPCM = static_cast<snd_pcm_t*>(m_pPCM);
	const size_t nSamples = nFrames * 2;
	const void* pData = pFrames;

	switch (m_Format)
	{
		case TFormat::Float:
			break;

		case TFormat::S32:
		{
			m_ConversionBuffer.resize(nSamples * sizeof(int32_t));
			int32_t* pOut = reinterpret_cast<int32_t*>(m_ConversionBuffer.data());
			for (size_t i = 0; i < nSamples; ++i)
				pOut[i] = static_cast<int32_t>(lrintf(ClampSample(pFrames[i]) * 2147483392.0f));
			pData = pOut;
			break;
		}

		case TFormat::S16:
		{
			m_ConversionBuffer.resize(nSamples * sizeof(int16_t));
			int16_t* pOut = reinterpret_cast<int16_t*>(m_ConversionBuffer.data());
			for (size_t i = 0; i < nSamples; ++i)
				pOut[i] = static_cast<int16_t>(lrintf(ClampSample(pFrames[i]) * 32767.0f));
			pData = pOut;
			break;
		}
	}

	const size_t nBytesPerFrame = m_Format == TFormat::S16 ? 4 : 8;
	const uint8_t* pBytes = static_cast<const uint8_t*>(pData);

	while (nFrames > 0)
	{
		const snd_pcm_sframes_t nWritten = snd_pcm_writei(pPCM, pBytes, nFrames);
		if (nWritten < 0)
		{
			if (nWritten == -EAGAIN)
				continue;
			if (!Recover(static_cast<int>(nWritten)))
				return false;
			continue;
		}

		pBytes += nWritten * nBytesPerFrame;
		nFrames -= nWritten;
	}

	return true;
}

//
// WAV file
//
CWAVFileOutput::CWAVFileOutput(unsigned nSampleRate, size_t nPeriodFrames)
	: m_pFile(nullptr),
	  m_nSampleRate(nSampleRate),
	  m_nPeriodFrames(nPeriodFrames),
	  m_nFramesWritten(0),
	  m_nPeak(0.0f)
{
}

CWAVFileOutput::~CWAVFileOutput()
{
	Close();
}

bool CWAVFileOutput::Open(const char* pPath)
{
	m_pFile = fopen(pPath, "wb");
	if (!m_pFile)
	{
		LOGERR("Cannot create '%s'", pPath);
		return false;
	}

	WriteHeader();
	return true;
}

void CWAVFileOutput::Close()
{
	if (!m_pFile)
		return;

	// Patch sizes in header
	fseek(m_pFile, 0, SEEK_SET);
	WriteHeader();
	fclose(m_pFile);
	m_pFile = nullptr;
}

void CWAVFileOutput::WriteHeader()
{
	const uint32_t nDataBytes = static_cast<uint32_t>(m_nFramesWritten * 4);
	const uint32_t nRIFFSize  = 36 + nDataBytes;
	const uint16_t nFormat = 1, nChannels = 2, nBlockAlign = 4, nBits = 16;
	const uint32_t nFmtSize = 16, nByteRate = m_nSampleRate * 4;

	fwrite("RIFF", 1, 4, m_pFile);
	fwrite(&nRIFFSize, 4, 1, m_pFile);
	fwrite("WAVEfmt ", 1, 8, m_pFile);
	fwrite(&nFmtSize, 4, 1, m_pFile);
	fwrite(&nFormat, 2, 1, m_pFile);
	fwrite(&nChannels, 2, 1, m_pFile);
	fwrite(&m_nSampleRate, 4, 1, m_pFile);
	fwrite(&nByteRate, 4, 1, m_pFile);
	fwrite(&nBlockAlign, 2, 1, m_pFile);
	fwrite(&nBits, 2, 1, m_pFile);
	fwrite("data", 1, 4, m_pFile);
	fwrite(&nDataBytes, 4, 1, m_pFile);
}

bool CWAVFileOutput::Write(const float* pFrames, size_t nFrames)
{
	const size_t nSamples = nFrames * 2;
	m_ConversionBuffer.resize(nSamples);

	for (size_t i = 0; i < nSamples; ++i)
	{
		const float nSample = ClampSample(pFrames[i]);
		m_nPeak = std::max(m_nPeak, std::fabs(nSample));
		m_ConversionBuffer[i] = static_cast<int16_t>(lrintf(nSample * 32767.0f));
	}

	if (fwrite(m_ConversionBuffer.data(), sizeof(int16_t), nSamples, m_pFile) != nSamples)
		return false;

	m_nFramesWritten += nFrames;
	return true;
}
