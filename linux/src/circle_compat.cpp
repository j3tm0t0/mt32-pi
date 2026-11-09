//
// circle_compat.cpp - Implementations behind the Circle compatibility shims
//

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#include <circle/logger.h>
#include <circle/string.h>
#include <circle/timer.h>

namespace
{
	const char* const SeverityNames[] = {"PANIC", "ERROR", "WARN", "NOTE", "DEBUG"};

	struct timespec StartTime()
	{
		static const struct timespec Start = [] {
			struct timespec Now;
			clock_gettime(CLOCK_MONOTONIC, &Now);
			return Now;
		}();
		return Start;
	}

	u64 MicrosSinceStart()
	{
		struct timespec Now;
		clock_gettime(CLOCK_MONOTONIC, &Now);
		const struct timespec Start = StartTime();
		return static_cast<u64>(Now.tv_sec - Start.tv_sec) * 1000000ULL + (Now.tv_nsec - Start.tv_nsec) / 1000;
	}

	std::mutex LogMutex;

	// Initialise the epoch at load time
	const struct timespec ForceStart = StartTime();
}

//
// CLogger
//
CLogger* CLogger::Get()
{
	static CLogger Logger;
	return &Logger;
}

void CLogger::Write(const char* pSource, TLogSeverity Severity, const char* pMessage, ...)
{
	va_list Args;
	va_start(Args, pMessage);
	WriteV(pSource, Severity, pMessage, Args);
	va_end(Args);
}

void CLogger::WriteV(const char* pSource, TLogSeverity Severity, const char* pMessage, va_list Args)
{
	if (Severity > m_MaxSeverity)
		return;

	char Buffer[1024];
	vsnprintf(Buffer, sizeof(Buffer), pMessage, Args);

	// Strip trailing newline (FluidSynth messages sometimes have one)
	for (size_t nLen = strlen(Buffer); nLen && (Buffer[nLen - 1] == '\n' || Buffer[nLen - 1] == '\r'); --nLen)
		Buffer[nLen - 1] = '\0';

	const u64 nMicros = MicrosSinceStart();
	{
		std::lock_guard<std::mutex> Lock(LogMutex);
		fprintf(stderr, "%5llu.%03llu %-5s %s: %s\n",
			static_cast<unsigned long long>(nMicros / 1000000),
			static_cast<unsigned long long>((nMicros / 1000) % 1000),
			SeverityNames[Severity], pSource, Buffer);
	}

	if (Severity == LogPanic)
	{
		if (m_PanicHandler)
			m_PanicHandler();
		abort();
	}
}

//
// CTimer
//
CTimer* CTimer::Get()
{
	static CTimer Timer;
	return &Timer;
}

unsigned CTimer::GetTicks() const
{
	return static_cast<unsigned>(MicrosSinceStart() / (1000000 / HZ));
}

unsigned CTimer::GetClockTicks()
{
	return static_cast<unsigned>(MicrosSinceStart());
}

u64 CTimer::GetClockTicks64()
{
	return MicrosSinceStart();
}

void CTimer::SimpleMsDelay(unsigned nMilliSeconds)
{
	SimpleusDelay(nMilliSeconds * 1000);
}

void CTimer::SimpleusDelay(unsigned nMicroSeconds)
{
	struct timespec Delay;
	Delay.tv_sec  = nMicroSeconds / 1000000;
	Delay.tv_nsec = (nMicroSeconds % 1000000) * 1000;
	while (nanosleep(&Delay, &Delay) != 0)
		;
}

//
// CString
//
CString::CString(const char* pString)
	: m_pBuffer(nullptr)
{
	Assign(pString);
}

CString::~CString()
{
	free(m_pBuffer);
}

void CString::Assign(const char* pString)
{
	char* pNew = (pString && *pString) ? strdup(pString) : nullptr;
	free(m_pBuffer);
	m_pBuffer = pNew;
}

CString& CString::operator=(const CString& Other)
{
	if (this != &Other)
		Assign(Other.m_pBuffer);
	return *this;
}

CString& CString::operator=(CString&& Other)
{
	if (this != &Other)
	{
		free(m_pBuffer);
		m_pBuffer       = Other.m_pBuffer;
		Other.m_pBuffer = nullptr;
	}
	return *this;
}

const char* CString::operator=(const char* pString)
{
	Assign(pString);
	return *this;
}

size_t CString::GetLength() const
{
	return m_pBuffer ? strlen(m_pBuffer) : 0;
}

void CString::Append(const char* pString)
{
	if (!pString || !*pString)
		return;

	const size_t nOld = GetLength();
	const size_t nAdd = strlen(pString);
	char* pNew = static_cast<char*>(realloc(m_pBuffer, nOld + nAdd + 1));
	if (!pNew)
		abort();
	memcpy(pNew + nOld, pString, nAdd + 1);
	m_pBuffer = pNew;
}

int CString::Compare(const char* pString) const
{
	return strcmp(*this, pString ? pString : "");
}

void CString::Format(const char* pFormat, ...)
{
	va_list Args;
	va_start(Args, pFormat);
	FormatV(pFormat, Args);
	va_end(Args);
}

void CString::FormatV(const char* pFormat, va_list Args)
{
	char* pNew = nullptr;
	if (vasprintf(&pNew, pFormat, Args) < 0)
		abort();
	free(m_pBuffer);
	m_pBuffer = pNew;
}
