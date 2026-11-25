#ifndef UTIL_H_
#define UTIL_H_

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "protocol.h"

// Endianness helpers (host <-> big-endian network byte order)
uint64_t HostToNet64(uint64_t val);
uint64_t NetToHost64(uint64_t val);

// IEEE 802.3 CRC32 helpers
uint32_t Crc32Update(uint32_t crc, const void *data, size_t len);
int ComputeFileCrc32(int fd, uint64_t len, uint32_t *out_crc);

// Low-level synchronous I/O & single-pass streaming/tee relay
int SendSync(int fd, const void *buffer, size_t len);
int RecvSync(int fd, void *buffer, size_t len);
int RelayStream(int fd_src, int fd_dst, int fd_tee, uint64_t len,
                uint32_t *out_crc);
int SetSocketTimeout(int fd, int timeout_sec);

// Wire serialization helpers (inputs/outputs in host byte order; ip stays NBO)
int SendRequestHeader(int fd, const ForwardRequest &host_req);
int RecvRequestHeader(int fd, ForwardRequest *host_req);
int SendResponseHeader(int fd, const ForwardResponse &host_res);
int RecvResponseHeader(int fd, ForwardResponse *host_res);
int SendNodes(int fd, const ForwardNode *host_nodes, size_t count);
int RecvNodes(int fd, ForwardNode *host_nodes, size_t count);
int SendFileMeta(int fd, const char *filename, uint32_t fname_with_nul_len);
int RecvFileMeta(int fd, ForwardFile **out_fmeta);

void MakeResponse(ForwardResponse *res, const ForwardRequest *req,
                  uint8_t retcode);
bool IsSafeRelativePath(const char *path);
std::string ExtractBasename(const char *path);

#endif  // UTIL_H_
