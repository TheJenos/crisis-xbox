// Multiplayer: Xbox LIVE's sessions and secure network, emulated between
// browsers. The page (web-src/mp.js) carries session listings and packets
// between players (BroadcastChannel between tabs, or Firebase + WebRTC).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// LIVE=1: the profile is signed in to Xbox LIVE (multiplayer on).
bool MpEnabled();

// This player: MP_ID (24-bit hex, from the page; 1 if not given) and MP_NAME.
uint32_t MpId();
uint32_t MpOwnIp();                 // 10.x.y.z from the ID (host byte order)
uint64_t MpXuid(bool online);       // offline 0xE0..., online 0x0009...
std::string MpGamertag();

// A player's XNADDR (36 bytes) from their address: everything in it follows
// from the ID, so any player's can be made from their IP alone.
void MpXnAddr(uint32_t ip, uint8_t* out36);
uint32_t MpIpOfXnAddr(uint32_t guestXnAddr);

// App 0xFB (XGI: sessions, contexts, properties) messages. Returns the
// result; *handled false for messages it doesn't know.
uint32_t MpXgiMessage(uint32_t message, uint32_t buffer, uint32_t length, bool* handled);

// A datagram for another player (net.cpp): handed to the page.
void MpSendPacket(uint32_t fromIp, uint16_t fromPort, uint32_t toIp, uint16_t toPort, const uint8_t* data, size_t size);

// Whether this player is in any session: packets from other players are
// dropped otherwise (the console's secure network only lets in players who
// share a session; a late packet from a previous game crashed the menus).
bool MpInSession();

// The key (XNKID) of the session this player is in, for XNetInAddrToXnAddr.
void MpCurrentSessionKey(uint8_t* out8);
void MpSessionKeyFor(uint32_t ip, uint8_t* out8); // the session we share with the player at ip
