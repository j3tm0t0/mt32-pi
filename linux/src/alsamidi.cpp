//
// alsamidi.cpp - ALSA sequencer MIDI input for mt32-pi on Linux
//

#include <alsa/asoundlib.h>

#include <circle/logger.h>

#include "alsamidi.h"

LOGMODULE("midi");

namespace
{
	constexpr size_t DecoderBufferSize = 1024;
}

CALSAMIDIInput::CALSAMIDIInput()
	: m_pSeq(nullptr),
	  m_pDecoder(nullptr),
	  m_nClientID(-1),
	  m_nPortID(-1),
	  m_bAutoConnect(false)
{
}

CALSAMIDIInput::~CALSAMIDIInput()
{
	if (m_pDecoder)
		snd_midi_event_free(static_cast<snd_midi_event_t*>(m_pDecoder));
	if (m_pSeq)
		snd_seq_close(static_cast<snd_seq_t*>(m_pSeq));
}

bool CALSAMIDIInput::Open(const char* pClientName, bool bAutoConnect)
{
	snd_seq_t* pSeq;
	int nResult = snd_seq_open(&pSeq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK);
	if (nResult < 0)
	{
		LOGERR("Cannot open ALSA sequencer: %s", snd_strerror(nResult));
		return false;
	}

	m_pSeq = pSeq;
	snd_seq_set_client_name(pSeq, pClientName);
	m_nClientID = snd_seq_client_id(pSeq);

	m_nPortID = snd_seq_create_simple_port(pSeq, "MIDI In",
		SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
		SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTHESIZER | SND_SEQ_PORT_TYPE_APPLICATION);
	if (m_nPortID < 0)
	{
		LOGERR("Cannot create sequencer port: %s", snd_strerror(m_nPortID));
		return false;
	}

	// Enlarge input buffer so that big SysEx dumps don't get dropped while a SoundFont is loading
	snd_seq_set_client_pool_input(pSeq, 2000);
	snd_seq_set_input_buffer_size(pSeq, 64 * 1024);

	snd_midi_event_t* pDecoder;
	if ((nResult = snd_midi_event_new(DecoderBufferSize, &pDecoder)) < 0)
	{
		LOGERR("Cannot create MIDI event decoder: %s", snd_strerror(nResult));
		return false;
	}

	// Emit full status bytes; CMIDIParser handles running status itself
	snd_midi_event_no_status(pDecoder, 1);
	m_pDecoder = pDecoder;

	LOGNOTE("ALSA sequencer port %d:%d (%s:MIDI In)", m_nClientID, m_nPortID, pClientName);

	m_bAutoConnect = bAutoConnect;
	if (bAutoConnect)
	{
		// Get notified about new ports (USB hotplug etc.)
		snd_seq_connect_from(pSeq, m_nPortID, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
		ConnectAll();
	}

	return true;
}

std::vector<pollfd> CALSAMIDIInput::GetPollFDs() const
{
	snd_seq_t* pSeq = static_cast<snd_seq_t*>(m_pSeq);
	const int nFDs = snd_seq_poll_descriptors_count(pSeq, POLLIN);
	std::vector<pollfd> FDs(nFDs);
	snd_seq_poll_descriptors(pSeq, FDs.data(), nFDs, POLLIN);
	return FDs;
}

void CALSAMIDIInput::TryConnect(int nClient, int nPort, unsigned nCaps, unsigned nType, const char* pName)
{
	snd_seq_t* pSeq = static_cast<snd_seq_t*>(m_pSeq);

	if (nClient == m_nClientID || nClient == SND_SEQ_CLIENT_SYSTEM)
		return;

	// Must be a readable, subscribable MIDI source
	if ((nCaps & (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ)) != (SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ))
		return;
	if (nCaps & SND_SEQ_PORT_CAP_NO_EXPORT)
		return;

	// Only hardware ports (USB/serial MIDI) and MIDI-generic application ports (rtpmidid, ttymidi...)
	if (!(nType & (SND_SEQ_PORT_TYPE_HARDWARE | SND_SEQ_PORT_TYPE_MIDI_GENERIC)))
		return;

	// Skip "Midi Through"
	if (pName && strstr(pName, "Midi Through"))
		return;

	const int nResult = snd_seq_connect_from(pSeq, m_nPortID, nClient, nPort);
	if (nResult == 0)
		LOGNOTE("Connected MIDI source %d:%d (%s)", nClient, nPort, pName ? pName : "?");
	else if (nResult != -EBUSY)
		LOGWARN("Failed to connect %d:%d: %s", nClient, nPort, snd_strerror(nResult));
}

void CALSAMIDIInput::ConnectAll()
{
	snd_seq_t* pSeq = static_cast<snd_seq_t*>(m_pSeq);

	snd_seq_client_info_t* pClientInfo;
	snd_seq_port_info_t* pPortInfo;
	snd_seq_client_info_alloca(&pClientInfo);
	snd_seq_port_info_alloca(&pPortInfo);

	snd_seq_client_info_set_client(pClientInfo, -1);
	while (snd_seq_query_next_client(pSeq, pClientInfo) >= 0)
	{
		const int nClient = snd_seq_client_info_get_client(pClientInfo);
		snd_seq_port_info_set_client(pPortInfo, nClient);
		snd_seq_port_info_set_port(pPortInfo, -1);

		while (snd_seq_query_next_port(pSeq, pPortInfo) >= 0)
		{
			TryConnect(nClient,
				snd_seq_port_info_get_port(pPortInfo),
				snd_seq_port_info_get_capability(pPortInfo),
				snd_seq_port_info_get_type(pPortInfo),
				snd_seq_client_info_get_name(pClientInfo));
		}
	}
}

bool CALSAMIDIInput::Connect(const char* pSource)
{
	snd_seq_t* pSeq = static_cast<snd_seq_t*>(m_pSeq);
	snd_seq_addr_t Addr;

	int nResult = snd_seq_parse_address(pSeq, &Addr, pSource);
	if (nResult < 0)
	{
		LOGERR("Invalid MIDI source '%s': %s", pSource, snd_strerror(nResult));
		return false;
	}

	nResult = snd_seq_connect_from(pSeq, m_nPortID, Addr.client, Addr.port);
	if (nResult < 0)
	{
		LOGERR("Cannot connect '%s': %s", pSource, snd_strerror(nResult));
		return false;
	}

	LOGNOTE("Connected MIDI source %d:%d", Addr.client, Addr.port);
	return true;
}

size_t CALSAMIDIInput::Process(const TByteHandler& Handler)
{
	snd_seq_t* pSeq = static_cast<snd_seq_t*>(m_pSeq);
	snd_midi_event_t* pDecoder = static_cast<snd_midi_event_t*>(m_pDecoder);

	size_t nEvents = 0;
	uint8_t Buffer[DecoderBufferSize];

	while (true)
	{
		snd_seq_event_t* pEvent = nullptr;
		const int nResult = snd_seq_event_input(pSeq, &pEvent);

		if (nResult == -EAGAIN || nResult == -EWOULDBLOCK)
			break;

		if (nResult == -ENOSPC)
		{
			LOGWARN("ALSA sequencer input overrun; events lost");
			continue;
		}

		if (nResult < 0 || !pEvent)
			break;

		++nEvents;

		switch (pEvent->type)
		{
			case SND_SEQ_EVENT_PORT_START:
				if (m_bAutoConnect)
				{
					snd_seq_port_info_t* pPortInfo;
					snd_seq_client_info_t* pClientInfo;
					snd_seq_port_info_alloca(&pPortInfo);
					snd_seq_client_info_alloca(&pClientInfo);

					const int nClient = pEvent->data.addr.client;
					const int nPort   = pEvent->data.addr.port;
					if (snd_seq_get_any_port_info(pSeq, nClient, nPort, pPortInfo) == 0 &&
					    snd_seq_get_any_client_info(pSeq, nClient, pClientInfo) == 0)
					{
						TryConnect(nClient, nPort,
							snd_seq_port_info_get_capability(pPortInfo),
							snd_seq_port_info_get_type(pPortInfo),
							snd_seq_client_info_get_name(pClientInfo));
					}
				}
				continue;

			case SND_SEQ_EVENT_PORT_SUBSCRIBED:
			case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
			case SND_SEQ_EVENT_PORT_EXIT:
			case SND_SEQ_EVENT_PORT_CHANGE:
			case SND_SEQ_EVENT_CLIENT_START:
			case SND_SEQ_EVENT_CLIENT_EXIT:
			case SND_SEQ_EVENT_CLIENT_CHANGE:
				continue;

			default:
				break;
		}

		const long nBytes = snd_midi_event_decode(pDecoder, Buffer, sizeof(Buffer), pEvent);
		if (nBytes > 0)
			Handler(Buffer, static_cast<size_t>(nBytes));
		else if (nBytes == -ENOMEM && pEvent->type == SND_SEQ_EVENT_SYSEX)
		{
			// SysEx chunk larger than the decoder buffer; pass raw payload through
			Handler(static_cast<const uint8_t*>(pEvent->data.ext.ptr), pEvent->data.ext.len);
		}
	}

	return nEvents;
}
