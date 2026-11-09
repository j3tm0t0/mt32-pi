//
// circle/logger.h - Linux compatibility shim for mt32-pi
//
// Implements Circle's LOGMODULE/LOG* macros and a minimal CLogger on top of
// stderr.
//

#ifndef _circle_logger_h
#define _circle_logger_h

#include <cstdarg>

enum TLogSeverity
{
	LogPanic,
	LogError,
	LogWarning,
	LogNotice,
	LogDebug
};

class CLogger
{
public:
	static CLogger* Get();

	void SetLogLevel(TLogSeverity Severity) { m_MaxSeverity = Severity; }

	void Write(const char* pSource, TLogSeverity Severity, const char* pMessage, ...) __attribute__((format(printf, 4, 5)));
	void WriteV(const char* pSource, TLogSeverity Severity, const char* pMessage, va_list Args);

	using TPanicHandler = void (*)();
	void RegisterPanicHandler(TPanicHandler Handler) { m_PanicHandler = Handler; }

private:
	TLogSeverity m_MaxSeverity = LogNotice;
	TPanicHandler m_PanicHandler = nullptr;
};

#define LOGMODULE(name) static const char From[] = name
#define LOGPANIC(...) CLogger::Get()->Write(From, LogPanic, __VA_ARGS__)
#define LOGERR(...) CLogger::Get()->Write(From, LogError, __VA_ARGS__)
#define LOGWARN(...) CLogger::Get()->Write(From, LogWarning, __VA_ARGS__)
#define LOGNOTE(...) CLogger::Get()->Write(From, LogNotice, __VA_ARGS__)
#define LOGDBG(...) CLogger::Get()->Write(From, LogDebug, __VA_ARGS__)

#endif
