//
// smf.cpp - Minimal Standard MIDI File reader for offline rendering/testing
//

#include <algorithm>
#include <cstdio>
#include <map>

#include "smf.h"

namespace
{
	struct TRawEvent
	{
		uint64_t nTick;
		size_t nOrder; // Preserve file order for simultaneous events
		bool bTempo;
		uint32_t nTempo;
		std::vector<uint8_t> Bytes;
	};

	uint32_t ReadBE(const uint8_t* p, size_t n)
	{
		uint32_t nValue = 0;
		for (size_t i = 0; i < n; ++i)
			nValue = (nValue << 8) | p[i];
		return nValue;
	}

	bool ReadVLQ(const std::vector<uint8_t>& Data, size_t& nPos, size_t nEnd, uint32_t& nOut)
	{
		nOut = 0;
		for (int i = 0; i < 4; ++i)
		{
			if (nPos >= nEnd)
				return false;
			const uint8_t nByte = Data[nPos++];
			nOut = (nOut << 7) | (nByte & 0x7F);
			if (!(nByte & 0x80))
				return true;
		}
		return false;
	}

	size_t DataBytesForStatus(uint8_t nStatus)
	{
		switch (nStatus & 0xF0)
		{
			case 0xC0:
			case 0xD0:
				return 1;
			default:
				return 2;
		}
	}
}

bool LoadStandardMIDIFile(const char* pPath, std::vector<TSMFEvent>& OutEvents, std::string& OutError)
{
	FILE* pFile = fopen(pPath, "rb");
	if (!pFile)
	{
		OutError = "cannot open file";
		return false;
	}

	std::vector<uint8_t> Data;
	uint8_t Chunk[65536];
	size_t nRead;
	while ((nRead = fread(Chunk, 1, sizeof(Chunk), pFile)) > 0)
		Data.insert(Data.end(), Chunk, Chunk + nRead);
	fclose(pFile);

	if (Data.size() < 14 || std::string(Data.begin(), Data.begin() + 4) != "MThd")
	{
		OutError = "not a Standard MIDI File";
		return false;
	}

	const uint32_t nHeaderLength = ReadBE(&Data[4], 4);
	const uint16_t nFormat       = ReadBE(&Data[8], 2);
	const uint16_t nTracks       = ReadBE(&Data[10], 2);
	const uint16_t nDivision     = ReadBE(&Data[12], 2);

	if (nFormat > 1)
	{
		OutError = "only SMF format 0 and 1 are supported";
		return false;
	}

	if (nDivision & 0x8000)
	{
		OutError = "SMPTE time division is not supported";
		return false;
	}

	std::vector<TRawEvent> RawEvents;
	size_t nPos   = 8 + nHeaderLength;
	size_t nOrder = 0;

	for (uint16_t nTrack = 0; nTrack < nTracks && nPos + 8 <= Data.size(); ++nTrack)
	{
		const std::string ChunkID(Data.begin() + nPos, Data.begin() + nPos + 4);
		const uint32_t nChunkLength = ReadBE(&Data[nPos + 4], 4);
		nPos += 8;
		const size_t nEnd = std::min(Data.size(), nPos + nChunkLength);

		if (ChunkID != "MTrk")
		{
			nPos = nEnd;
			--nTrack;
			continue;
		}

		uint64_t nTick = 0;
		uint8_t nRunningStatus = 0;

		while (nPos < nEnd)
		{
			uint32_t nDelta;
			if (!ReadVLQ(Data, nPos, nEnd, nDelta))
				break;
			nTick += nDelta;

			if (nPos >= nEnd)
				break;

			uint8_t nStatus = Data[nPos];
			if (nStatus & 0x80)
				++nPos;
			else
				nStatus = nRunningStatus;

			if (nStatus == 0xFF)
			{
				// Meta event
				if (nPos >= nEnd)
					break;
				const uint8_t nType = Data[nPos++];
				uint32_t nLength;
				if (!ReadVLQ(Data, nPos, nEnd, nLength) || nPos + nLength > nEnd)
					break;

				if (nType == 0x51 && nLength == 3)
					RawEvents.push_back({nTick, nOrder++, true, ReadBE(&Data[nPos], 3), {}});
				nPos += nLength;

				if (nType == 0x2F)
					break;
			}
			else if (nStatus == 0xF0 || nStatus == 0xF7)
			{
				uint32_t nLength;
				if (!ReadVLQ(Data, nPos, nEnd, nLength) || nPos + nLength > nEnd)
					break;

				TRawEvent Event{nTick, nOrder++, false, 0, {}};
				if (nStatus == 0xF0)
					Event.Bytes.push_back(0xF0);
				Event.Bytes.insert(Event.Bytes.end(), Data.begin() + nPos, Data.begin() + nPos + nLength);
				RawEvents.push_back(std::move(Event));
				nPos += nLength;
				nRunningStatus = 0;
			}
			else if (nStatus >= 0x80 && nStatus < 0xF0)
			{
				const size_t nDataBytes = DataBytesForStatus(nStatus);
				if (nPos + nDataBytes > nEnd)
					break;

				TRawEvent Event{nTick, nOrder++, false, 0, {nStatus}};
				Event.Bytes.insert(Event.Bytes.end(), Data.begin() + nPos, Data.begin() + nPos + nDataBytes);
				RawEvents.push_back(std::move(Event));
				nPos += nDataBytes;
				nRunningStatus = nStatus;
			}
			else
			{
				// Corrupt data / unexpected system message
				break;
			}
		}

		nPos = nEnd;
	}

	std::stable_sort(RawEvents.begin(), RawEvents.end(), [](const TRawEvent& A, const TRawEvent& B) {
		return A.nTick != B.nTick ? A.nTick < B.nTick : A.nOrder < B.nOrder;
	});

	// Apply tempo map
	uint32_t nTempo    = 500000; // 120 BPM
	uint64_t nLastTick = 0;
	double nSeconds    = 0.0;

	OutEvents.clear();
	for (const TRawEvent& Event : RawEvents)
	{
		nSeconds += static_cast<double>(Event.nTick - nLastTick) * nTempo / (1000000.0 * nDivision);
		nLastTick = Event.nTick;

		if (Event.bTempo)
			nTempo = Event.nTempo;
		else
			OutEvents.push_back({nSeconds, Event.Bytes});
	}

	return true;
}
