#ifndef PROTOCOL_H_
#define PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

// Wire packet format (all multi-byte integer fields are big-endian / network
// byte order on the wire):
// |---ForwardHead (28B)---|---ForwardNode*n---|---ForwardFile---|---data---|
// After forwarding one hop:
// |---ForwardHead
// (28B)---|---ForwardNode*(n-1)---|---ForwardFile---|---data---| At final node:
// |---ForwardHead (28B)---|---ForwardFile---|---data---|

const uint32_t kDefaultPort = 40000;
const uint32_t kForwardMagic = 0xFF44AADD;
const uint8_t kForwardVersion = 2;
const uint8_t kMaxForwardTTL = 32;
const int kDefaultBacklog = 128;
const int kDefaultFileMode = 0644;
const size_t kForwardBufSize = 65536;
const uint32_t kMaxFilenameLen = 4096;
const int kDefaultSocketTimeoutSec = 30;

// ForwardRequest::flags bitmask
const uint8_t kFlagNone = 0x00;
const uint8_t kFlagStoreOnHop = 0x01;

enum ForwardRetcode {
  ForwardSuccess = 0,
  ForwardInterrupt = 1,
  ForwardUnreachable = 2,
  ForwardInternalError = 3,
  ForwardInvalidRequest = 4,
  ForwardFileNotFound = 5,
  ForwardChecksumMismatch = 6,
  ForwardRetcodeMax,
};

enum ForwardCommand { ForwardPull = 0, ForwardPush = 1 };

struct ForwardNode {
  uint32_t ip;    // IPv4 address in network byte order
  uint32_t port;  // Port number (encoded in network byte order on wire)
} __attribute__((packed));

struct ForwardFile {
  uint32_t length;  // Filename buffer length including '\0'
  uint8_t filename[0];
} __attribute__((packed));

struct ForwardRequest {
  uint64_t length;  // Total packet bytes including header, nodes, meta, data
  uint32_t magic;   // kForwardMagic
  uint8_t version;  // kForwardVersion (2)
  uint8_t cmd;      // ForwardPull or ForwardPush
  uint8_t ttl;      // Remaining hops after the target of this packet
  uint8_t flags;    // Bitmask (e.g. kFlagStoreOnHop)
  uint64_t id;      // Traceable request ID
  uint32_t crc32;   // IEEE 802.3 CRC32 of payload data (0 when no payload)
  uint8_t data[0];
} __attribute__((packed));

struct ForwardResponse {
  uint64_t length;  // Total response bytes including header (+ meta + data)
  uint32_t magic;   // kForwardMagic
  uint8_t version;  // kForwardVersion (2)
  uint8_t cmd;      // ForwardPull or ForwardPush
  uint8_t ttl;      // Echoed TTL
  uint8_t retcode;  // ForwardRetcode
  uint64_t id;      // Echoed request ID
  uint32_t crc32;   // IEEE 802.3 CRC32 of payload data (for Pull responses)
  uint8_t data[0];
} __attribute__((packed));

#endif  // PROTOCOL_H_
