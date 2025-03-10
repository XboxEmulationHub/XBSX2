// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once
#include <atomic>
#include <chrono>
#ifdef _WIN32
#include <winsock2.h>
#elif defined(__POSIX__)
#define INVALID_SOCKET -1
#include <sys/socket.h>
#endif

#include "UDP_BaseSession.h"

namespace Sessions
{
	class UDP_Session : public UDP_BaseSession
	{
	private:
		std::atomic<bool> open{false};

#ifdef _WIN32
		SOCKET client = INVALID_SOCKET;
#elif defined(__POSIX__)
		int client = INVALID_SOCKET;
#endif

		u16 srcPort = 0;
		u16 destPort = 0;
		// UDP_Session flags
		const bool isBroadcast;
		const bool isMulticast;
		const bool isFixedPort;

		std::atomic<std::chrono::steady_clock::time_point> deathClockStart;
		const static std::chrono::duration<std::chrono::steady_clock::rep, std::chrono::steady_clock::period> MAX_IDLE;

	public:
		// Normal Port
		UDP_Session(ConnectionKey parKey, PacketReader::IP::IP_Address parAdapterIP);
		// Fixed Port
#ifdef _WIN32
		UDP_Session(ConnectionKey parKey, PacketReader::IP::IP_Address parAdapterIP, bool parIsBroadcast, bool parIsMulticast, SOCKET parClient);
#elif defined(__POSIX__)
		UDP_Session(ConnectionKey parKey, PacketReader::IP::IP_Address parAdapterIP, bool parIsBroadcast, bool parIsMulticast, int parClient);
#endif

		virtual std::optional<ReceivedPayload> Recv();
		virtual bool WillRecive(PacketReader::IP::IP_Address parDestIP);
		virtual bool Send(PacketReader::IP::IP_Payload* payload);
		virtual void Reset();

		virtual ~UDP_Session();
	};
} // namespace Sessions
