//
//  cdplusprotocol.cpp
//  xlxd
//
//  Created by Jean-Luc Deltombe (LX3JL) on 01/11/2015.
//  Copyright © 2015 Jean-Luc Deltombe (LX3JL). All rights reserved.
//  Copyright © 2020 Thomas A. Early, N7TAE
//
// ----------------------------------------------------------------------------
//    This file is part of xlxd.
//
//    xlxd is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    xlxd is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with Foobar.  If not, see <http://www.gnu.org/licenses/>.
// ----------------------------------------------------------------------------

#include "main.h"
#include <string.h>
#include "cdplusclient.h"
#include "cdplusprotocol.h"
#include "creflector.h"
#include "cgatekeeper.h"


////////////////////////////////////////////////////////////////////////////////////////
// operation

bool CDplusProtocol::Initialize(const char *type, const int ptype, const uint16 port, const bool has_ipv4, const bool has_ipv6)
{
	// base class
	if (! CProtocol::Initialize(type, ptype, port, has_ipv4, has_ipv6))
		return false;

	// update time
	m_LastKeepaliveTime.Now();

	// done
	return true;
}



////////////////////////////////////////////////////////////////////////////////////////
// task

void CDplusProtocol::Task(void)
{
	CBuffer             Buffer;
	CIp                 Ip;
	CCallsign           Callsign;
	std::unique_ptr<CDvHeaderPacket>    Header;
	std::unique_ptr<CDvFramePacket>     Frame;
	std::unique_ptr<CDvLastFramePacket> LastFrame;

	// handle incoming packets
#if DSTAR_IPV6==true
#if DSTAR_IPV4==true
	if ( ReceiveDS(Buffer, Ip, 20) )
#else
	if ( Receive6(Buffer, Ip, 20) )
#endif
#else
	if ( Receive4(Buffer, Ip, 20) )
#endif
	{
		// crack the packet
		if ( IsValidDvFramePacket(Buffer, Frame) )
		{
			OnDvFramePacketIn(Frame, &Ip);
		}
		else if ( IsValidDvHeaderPacket(Buffer, Header) )
		{
			// is muted?
			if ( g_GateKeeper.MayTransmit(Header->GetMyCallsign(), Ip, PROTOCOL_DPLUS, Header->GetRpt2Module()) )
			{
				// handle it
				OnDvHeaderPacketIn(Header, Ip);
			}
		}
		else if ( IsValidDvLastFramePacket(Buffer, LastFrame) )
		{
			OnDvLastFramePacketIn(LastFrame, &Ip);
		}
		else if ( IsValidConnectPacket(Buffer) )
		{
			std::cout << "DPlus connect request packet from " << Ip << std::endl;

			// acknowledge the request
			Send(Buffer, Ip);
		}
		else if ( IsValidLoginPacket(Buffer, &Callsign) )
		{
			std::cout << "DPlus login packet from " << Callsign << " at " << Ip << std::endl;

			// callsign authorized?
			if ( g_GateKeeper.MayLink(Callsign, Ip, PROTOCOL_DPLUS) )
			{
				// acknowledge the request
				EncodeLoginAckPacket(&Buffer);
				Send(Buffer, Ip);

				// create the client and append
				g_Reflector.GetClients()->AddClient(std::make_shared<CDplusClient>(Callsign, Ip));
				g_Reflector.ReleaseClients();
			}
			else
			{
				// deny the request
				EncodeLoginNackPacket(&Buffer);
				Send(Buffer, Ip);
			}

		}
		else if ( IsValidDisconnectPacket(Buffer) )
		{
			std::cout << "DPlus disconnect packet from " << Ip << std::endl;

			// find client
			CClients *clients = g_Reflector.GetClients();
			std::shared_ptr<CClient>client = clients->FindClient(Ip, PROTOCOL_DPLUS);
			if ( client != nullptr )
			{
				// remove it
				clients->RemoveClient(client);
				// and acknowledge the disconnect
				EncodeDisconnectPacket(&Buffer);
				Send(Buffer, Ip);
			}
			g_Reflector.ReleaseClients();
		}
		else if ( IsValidKeepAlivePacket(Buffer) )
		{
			//std::cout << "DPlus keepalive packet from " << Ip << std::endl;

			// find all clients with that callsign & ip and keep them alive
			CClients *clients = g_Reflector.GetClients();
			auto it = clients->begin();
			std::shared_ptr<CClient>client = nullptr;
			while ( (client = clients->FindNextClient(Ip, PROTOCOL_DPLUS, it)) != nullptr )
			{
				client->Alive();
			}
			g_Reflector.ReleaseClients();
		}
		else
		{
			std::cout << "DPlus packet (" << Buffer.size() << ")" << std::endl;
		}
	}

	// handle end of streaming timeout
	CheckStreamsTimeout();

	// handle queue from reflector
	HandleQueue();

	// keep client alive
	if ( m_LastKeepaliveTime.DurationSinceNow() > DPLUS_KEEPALIVE_PERIOD )
	{
		//
		HandleKeepalives();

		// update time
		m_LastKeepaliveTime.Now();
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// streams helpers

void CDplusProtocol::OnDvHeaderPacketIn(std::unique_ptr<CDvHeaderPacket> &Header, const CIp &Ip)
{
	// find the stream
	CPacketStream *stream = GetStream(Header->GetStreamId());
	if ( stream )
	{
		// stream already open
		// skip packet, but tickle the stream
		stream->Tickle();
	}
	else
	{
		// no stream open yet, open a new one
		CCallsign my(Header->GetMyCallsign());
		CCallsign rpt1(Header->GetRpt1Callsign());
		CCallsign rpt2(Header->GetRpt2Callsign());

		// first, check module is valid
		if ( g_Reflector.IsValidModule(rpt2.GetModule()) )
		{
			// find this client
			std::shared_ptr<CClient>client = g_Reflector.GetClients()->FindClient(Ip, PROTOCOL_DPLUS);
			if ( client )
			{
				// now we know if it's a dextra dongle or a genuine dplus node
				if ( Header->GetRpt2Callsign().HasSameCallsignWithWildcard(CCallsign("XRF*"))  )
				{
					client->SetDextraDongle();
				}
				// now we know its module, let's update it
				if ( !client->HasModule() )
				{
					client->SetModule(rpt1.GetModule());
				}
				// get client callsign
				rpt1 = client->GetCallsign();
				// and try to open the stream
				if ( (stream = g_Reflector.OpenStream(Header, client)) != nullptr )
				{
					// keep the handle
					m_Streams.push_back(stream);
				}
			}
			// release
			g_Reflector.ReleaseClients();

			// update last heard
			g_Reflector.GetUsers()->Hearing(my, rpt1, rpt2);
			g_Reflector.ReleaseUsers();
		}
		else
		{
			std::cout << "DPlus node " << rpt1 << " link attempt on non-existing module" << std::endl;
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// queue helper

void CDplusProtocol::HandleQueue(void)
{
	m_Queue.Lock();
	while ( !m_Queue.empty() )
	{
		// get the packet
		auto packet = m_Queue.front();
		m_Queue.pop();

		// get our sender's id
		int iModId = g_Reflector.GetModuleIndex(packet->GetModuleId());

		// check if it's header and update cache
		if ( packet->IsDvHeader() )
		{
			// this relies on queue feeder setting valid module id
			m_StreamsCache[iModId].m_dvHeader = CDvHeaderPacket((const CDvHeaderPacket &)*packet);
			m_StreamsCache[iModId].m_iSeqCounter = 0;
		}

		// encode it
		CBuffer buffer;
		if ( EncodeDvPacket(*packet, &buffer) )
		{
			// and push it to all our clients who are not streaming in
			// note that for dplus protocol, all stream of all modules are push to all clients
			// it's client who decide which stream he's interrrested in
			CClients *clients = g_Reflector.GetClients();
			auto it = clients->begin();
			std::shared_ptr<CClient>client = nullptr;
			while ( (client = clients->FindNextClient(PROTOCOL_DPLUS, it)) != nullptr )
			{
				// is this client busy ?
				if ( !client->IsAMaster() )
				{
					// check if client is a dextra dongle
					// then replace RPT2 with XRF instead of REF
					// if the client type is not yet known, send bothheaders
					if ( packet->IsDvHeader() )
					{
						// sending header in Dplus is client specific
						SendDvHeader((CDvHeaderPacket *)packet.get(), (CDplusClient *)client.get());
					}
					else if ( packet->IsDvFrame() )
					{
						// and send the DV frame
						Send(buffer, client->GetIp());

						// is it time to insert a DVheader copy ?
						if ( (m_StreamsCache[iModId].m_iSeqCounter++ % 21) == 20 )
						{
							// yes, clone it
							CDvHeaderPacket packet2(m_StreamsCache[iModId].m_dvHeader);
							// and send it
							SendDvHeader(&packet2, (CDplusClient *)client.get());
						}
					}
					else
					{
						// otherwise, send the original packet
						Send(buffer, client->GetIp());
					}
				}
			}
			g_Reflector.ReleaseClients();
		}
	}
	m_Queue.Unlock();
}

void CDplusProtocol::SendDvHeader(CDvHeaderPacket *packet, CDplusClient *client)
{
	// encode it
	CBuffer buffer;
	if ( EncodeDvPacket(*packet, &buffer) )
	{
		if ( (client->IsDextraDongle() || !client->HasModule()) )
		{
			// clone the packet and patch it
			CDvHeaderPacket packet2(*((CDvHeaderPacket *)packet));
			CCallsign rpt2 = packet2.GetRpt2Callsign();
			rpt2.PatchCallsign(0, (const uint8 *)"XRF", 3);
			packet2.SetRpt2Callsign(rpt2);

			// encode it
			CBuffer buffer2;
			if ( EncodeDvPacket(packet2, &buffer2) )
			{
				// and send it
				Send(buffer2, client->GetIp());
			}

			// client type known ?
			if ( !client->HasModule() )
			{
				// no, send also the genuine packet
				Send(buffer, client->GetIp());
			}
		}
		else
		{
			// otherwise, send the original packet
			Send(buffer, client->GetIp());
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////
// keepalive helpers

void CDplusProtocol::HandleKeepalives(void)
{
	// send keepalives
	CBuffer keepalive;
	EncodeKeepAlivePacket(&keepalive);

	// iterate on clients
	CClients *clients = g_Reflector.GetClients();
	auto it = clients->begin();
	std::shared_ptr<CClient>client = nullptr;
	while ( (client = clients->FindNextClient(PROTOCOL_DPLUS, it)) != nullptr )
	{
		// send keepalive
		//std::cout << "Sending DPlus packet @ " << client->GetIp() << std::endl;
		Send(keepalive, client->GetIp());

		// is this client busy ?
		if ( client->IsAMaster() )
		{
			// yes, just tickle it
			client->Alive();
		}
		// check it's still with us
		else if ( !client->IsAlive() )
		{
			// no, disconnect
			CBuffer disconnect;
			EncodeDisconnectPacket(&disconnect);
			Send(disconnect, client->GetIp());

			// and remove it
			std::cout << "DPlus client " << client->GetCallsign() << " keepalive timeout" << std::endl;
			clients->RemoveClient(client);
		}
	}
	g_Reflector.ReleaseClients();
}

////////////////////////////////////////////////////////////////////////////////////////
// packet decoding helpers

bool CDplusProtocol::IsValidConnectPacket(const CBuffer &Buffer)
{
	uint8 tag[] = { 0x05,0x00,0x18,0x00,0x01 };
	return (Buffer == CBuffer(tag, sizeof(tag)));
}

bool CDplusProtocol::IsValidLoginPacket(const CBuffer &Buffer, CCallsign *Callsign)
{
	uint8 Tag[] = { 0x1C,0xC0,0x04,0x00 };
	bool valid = false;

	if ( (Buffer.size() == 28) &&(::memcmp(Buffer.data(), Tag, sizeof(Tag)) == 0) )
	{
		Callsign->SetCallsign(&(Buffer.data()[4]), 8);
		valid = Callsign->IsValid();
	}
	return valid;
}

bool CDplusProtocol::IsValidDisconnectPacket(const CBuffer &Buffer)
{
	uint8 tag[] = { 0x05,0x00,0x18,0x00,0x00 };
	return (Buffer == CBuffer(tag, sizeof(tag)));
}

bool CDplusProtocol::IsValidKeepAlivePacket(const CBuffer &Buffer)
{
	uint8 tag[] = { 0x03,0x60,0x00 };
	return (Buffer == CBuffer(tag, sizeof(tag)));
}

bool CDplusProtocol::IsValidDvHeaderPacket(const CBuffer &Buffer, std::unique_ptr<CDvHeaderPacket> &header)
{
	if ( 58==Buffer.size() && 0x3AU==Buffer.data()[0] && 0x80U==Buffer.data()[1] && 0==Buffer.Compare((uint8 *)"DSVT", 2, 4) && 0x10U==Buffer.data()[6] && 0x20U==Buffer.data()[10] )
	{
		// create packet
		header = std::unique_ptr<CDvHeaderPacket>(new CDvHeaderPacket((struct dstar_header *)&(Buffer.data()[17]), *((uint16 *)&(Buffer.data()[14])), 0x80));
		// check validity of packet
		if ( header && header->IsValid() )
			return true;
	}
	return false;
}

bool CDplusProtocol::IsValidDvFramePacket(const CBuffer &Buffer, std::unique_ptr<CDvFramePacket> &dvframe)
{
	if ( 29==Buffer.size() && 0x1DU==Buffer.data()[0] && 0x80U==Buffer.data()[1] && 0==Buffer.Compare((uint8 *)"DSVT", 2, 4) && 0x20U==Buffer.data()[6] && 0x20U==Buffer.data()[10] )
	{
		// create packet
		dvframe = std::unique_ptr<CDvFramePacket>(new CDvFramePacket((struct dstar_dvframe *)&(Buffer.data()[17]), *((uint16 *)&(Buffer.data()[14])), Buffer.data()[16]));
		// check validity of packet
		if ( dvframe && dvframe->IsValid() )
			return true;
	}
	return false;
}

bool CDplusProtocol::IsValidDvLastFramePacket(const CBuffer &Buffer, std::unique_ptr<CDvLastFramePacket> &dvframe)
{
	if ( 32==Buffer.size() && 0==Buffer.Compare((uint8 *)"DSVT", 2, 4) && 0x20U==Buffer.data()[0] && 0x80U==Buffer.data()[1] && 0x20U==Buffer.data()[6] && 0x20U==Buffer.data()[10] )
	{
		// create packet
		dvframe = std::unique_ptr<CDvLastFramePacket>(new CDvLastFramePacket((struct dstar_dvframe *)&(Buffer.data()[17]), *((uint16 *)&(Buffer.data()[14])), Buffer.data()[16]));
		// check validity of packet
		if ( dvframe && dvframe->IsValid() )
			return true;
	}
	return false;
}


////////////////////////////////////////////////////////////////////////////////////////
// packet encoding helpers

void CDplusProtocol::EncodeKeepAlivePacket(CBuffer *Buffer)
{
	uint8 tag[] = { 0x03,0x60,0x00 };
	Buffer->Set(tag, sizeof(tag));
}

void CDplusProtocol::EncodeLoginAckPacket(CBuffer *Buffer)
{
	uint8 tag[] = { 0x08,0xC0,0x04,0x00,'O','K','R','W' };
	Buffer->Set(tag, sizeof(tag));
}

void CDplusProtocol::EncodeLoginNackPacket(CBuffer *Buffer)
{
	uint8 tag[] = { 0x08,0xC0,0x04,0x00,'B','U','S','Y' };
	Buffer->Set(tag, sizeof(tag));
}

void CDplusProtocol::EncodeDisconnectPacket(CBuffer *Buffer)
{
	uint8 tag[] = { 0x05,0x00,0x18,0x00,0x00 };
	Buffer->Set(tag, sizeof(tag));
}


bool CDplusProtocol::EncodeDvHeaderPacket(const CDvHeaderPacket &Packet, CBuffer *Buffer) const
{
	uint8 tag[]	= { 0x3A,0x80,0x44,0x53,0x56,0x54,0x10,0x00,0x00,0x00,0x20,0x00,0x01,0x02 };
	struct dstar_header DstarHeader;

	Packet.ConvertToDstarStruct(&DstarHeader);

	Buffer->Set(tag, sizeof(tag));
	Buffer->Append(Packet.GetStreamId());
	Buffer->Append((uint8)0x80);
	Buffer->Append((uint8 *)&DstarHeader, sizeof(struct dstar_header));

	return true;
}

bool CDplusProtocol::EncodeDvFramePacket(const CDvFramePacket &Packet, CBuffer *Buffer) const
{
	uint8 tag[] = { 0x1D,0x80,0x44,0x53,0x56,0x54,0x20,0x00,0x00,0x00,0x20,0x00,0x01,0x02 };

	Buffer->Set(tag, sizeof(tag));
	Buffer->Append(Packet.GetStreamId());
	Buffer->Append((uint8)(Packet.GetPacketId() % 21));
	Buffer->Append((uint8 *)Packet.GetAmbe(), AMBE_SIZE);
	Buffer->Append((uint8 *)Packet.GetDvData(), DVDATA_SIZE);

	return true;

}

bool CDplusProtocol::EncodeDvLastFramePacket(const CDvLastFramePacket &Packet, CBuffer *Buffer) const
{
	uint8 tag1[] = { 0x20,0x80,0x44,0x53,0x56,0x54,0x20,0x00,0x81,0x00,0x20,0x00,0x01,0x02 };
	uint8 tag2[] = { 0x55,0xC8,0x7A,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x55,0x25,0x1A,0xC6 };

	Buffer->Set(tag1, sizeof(tag1));
	Buffer->Append(Packet.GetStreamId());
	Buffer->Append((uint8)((Packet.GetPacketId() % 21) | 0x40));
	Buffer->Append(tag2, sizeof(tag2));

	return true;
}
