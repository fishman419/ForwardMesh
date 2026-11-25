#ifndef UTIL_H_
#define UTIL_H_

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "protocol.h"

int SendSync(int fd, const void *buffer, size_t len);
int RecvSync(int fd, void *buffer, size_t len);
int ForwardSync(int fd_src, int fd_dst, size_t len);
int SetSocketTimeout(int fd, int timeout_sec);
void MakeResponse(ForwardResponse *res, const ForwardRequest *req,
                  uint8_t retcode);
bool IsSafeRelativePath(const char *path);
std::string ExtractBasename(const char *path);

#endif  // UTIL_H_
