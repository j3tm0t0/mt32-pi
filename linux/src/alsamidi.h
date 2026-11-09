//
// alsamidi.h - ALSA sequencer MIDI input for mt32-pi on Linux
//
// Creates a writable sequencer port ("mt32-pi:MIDI In") and converts incoming
// sequencer events back into a raw MIDI byte stream, so that the existing
// CMIDIParser path is used unchanged. USB MIDI interfaces, rtpmidid,
// ttymidi, aconnect/aplaymidi etc. can all feed it.
//
// With auto-connect enabled, every hardware/application MIDI output that
// appears (including hot-plugged USB devices) is subscribed automatically.
//

#ifndef _alsamidi_h
#define _alsamidi_h

#include <poll.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

class CALSAMIDIInput
{
public:
	using TByteHandler = std::function<void(const uint8_t* pData, size_t nSize)>;

	CALSAMIDIInput();
	~CALSAMIDIInput();

	bool Open(const char* pClientName, bool bAutoConnect);

	// File descriptors to poll() for input
	std::vector<pollfd> GetPollFDs() const;

	// Drains all pending events, invoking the handler with raw MIDI bytes; returns number of events processed
	size_t Process(const TByteHandler& Handler);

	// Connects to a specific source given as "client:port" or a client name
	bool Connect(const char* pSource);

	int GetClientID() const { return m_nClientID; }
	int GetPortID() const { return m_nPortID; }

private:
	void ConnectAll();
	void TryConnect(int nClient, int nPort, unsigned nCaps, unsigned nType, const char* pName);

	void* m_pSeq;
	void* m_pDecoder;
	int m_nClientID;
	int m_nPortID;
	bool m_bAutoConnect;
};

#endif
