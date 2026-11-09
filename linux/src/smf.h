//
// smf.h - Minimal Standard MIDI File reader for offline rendering/testing
//

#ifndef _smf_h
#define _smf_h

#include <cstdint>
#include <string>
#include <vector>

struct TSMFEvent
{
	double nTimeSeconds;
	std::vector<uint8_t> Bytes; // Raw MIDI bytes as they would appear on the wire
};

// Loads a format 0/1 SMF and returns all channel/SysEx events merged and sorted, with absolute times (tempo map applied)
bool LoadStandardMIDIFile(const char* pPath, std::vector<TSMFEvent>& OutEvents, std::string& OutError);

#endif
