#ifndef UTIL_H_
#define UTIL_H_

#include <stdint.h>

#include "protocol.h"

int SendSync(int fd, void *buffer, int len);
int RecvSync(int fd, void *buffer, int len);
int ForwardSync(int fd_src, int fd_dst, int len);
void MakeResponse(ForwardResponse *res, ForwardRequest *req, uint8_t retcode);

#endif  // UTIL_H_