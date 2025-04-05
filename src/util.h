#ifndef UTIL_H_
#define UTIL_H_

int send_sync(int fd, void *buffer, int len);
int recv_sync(int fd, void *buffer, int len);
int forward_sync(int fd_src, int fd_dst, int len);

#endif  // UTIL_H_