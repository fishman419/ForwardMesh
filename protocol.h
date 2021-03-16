#ifndef PROTOCOL_H_
#define PROTOCOL_H_

#include <stdint.h>

// packet format:
// |---ForwardHead---|---ForwardNode*n---|---data---|
// After forward one node:
// |---ForwardHead---|---ForwardNode*(n-1)---|---data---|
// reach Final node:
// |---ForwardHead---|---data---|

const uint32_t kDefaultPort = 40000;
const uint32_t kForwardMagic = 0xFF44AADD;
const uint8_t kForwardVersion1 = 1;
const uint8_t kMaxForwardTTL = 32;

enum ForwardRetcode {
  ForwardSuccess = 0,
  ForwardUnreachable = 1
};

enum ForwardCommand {
  ForwardPull = 0,
  ForwardPush = 1
};

struct ForwardNode {
  uint32_t ip;
  uint32_t port;
} __attribute__((packed));

struct ForwardRequest {
  uint32_t length;
  uint32_t magic;
  uint8_t version;
  uint8_t cmd;
  uint8_t ttl;
  uint8_t reserved;
  uint64_t id;
  uint8_t data[0];
} __attribute__((packed));

struct ForwardResponse {
  uint32_t length;
  uint32_t magic;
  uint8_t version;
  uint8_t cmd;
  uint8_t ttl;
  uint8_t retcode;
  uint64_t id;
  uint8_t data[0];
} __attribute__((packed));

uint32_t DataOffset(ForwardRequest *req) {
  return sizeof(ForwardRequest) + req->ttl * sizeof(ForwardNode);
}

uint32_t DataOffset(ForwardResponse *res) { return sizeof(ForwardResponse); }

#endif  // PROTOCOL_H_