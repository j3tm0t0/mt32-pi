//
// circle/string.h - Linux compatibility shim for mt32-pi
//
// Minimal CString holding a single heap pointer, like Circle's CString.
//
// NOTE: This must stay trivially relocatable. Utility::Swap() (used by
// Utility::QSort() to sort the SoundFont list) swaps objects with memcpy().
// std::string is NOT safe for that (libstdc++'s small-string buffer points
// into the object itself), so it is deliberately not used here.
//

#ifndef _circle_string_h
#define _circle_string_h

#include <cstdarg>
#include <cstddef>

class CString
{
public:
	CString() : m_pBuffer(nullptr) {}
	CString(const char* pString);
	CString(const CString& Other) : CString(Other.m_pBuffer) {}
	CString(CString&& Other) : m_pBuffer(Other.m_pBuffer) { Other.m_pBuffer = nullptr; }
	~CString();

	CString& operator=(const CString& Other);
	CString& operator=(CString&& Other);
	const char* operator=(const char* pString);

	operator const char*() const { return m_pBuffer ? m_pBuffer : ""; }

	size_t GetLength() const;
	void Append(const char* pString);
	int Compare(const char* pString) const;

	void Format(const char* pFormat, ...) __attribute__((format(printf, 2, 3)));
	void FormatV(const char* pFormat, va_list Args);

private:
	void Assign(const char* pString);

	char* m_pBuffer;
};

#endif
