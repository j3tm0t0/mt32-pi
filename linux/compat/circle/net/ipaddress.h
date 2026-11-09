//
// circle/net/ipaddress.h - Linux compatibility shim for mt32-pi
//
// Only needed so that the [network] section of the shared config definition
// still parses; networking itself is left to the operating system on Linux.
//

#ifndef _circle_net_ipaddress_h
#define _circle_net_ipaddress_h

#include <circle/types.h>
#include <cstring>

class CIPAddress
{
public:
	CIPAddress() : m_nAddress(0) {}
	CIPAddress(u32 nAddress) : m_nAddress(nAddress) {}

	void Set(const u8* pAddress) { memcpy(&m_nAddress, pAddress, sizeof(m_nAddress)); }
	u32 Get() const { return m_nAddress; }

private:
	u32 m_nAddress;
};

#endif
