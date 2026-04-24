#ifndef UTIL_H_
#define UTIL_H_

int SendSync(int fd, void *buffer, int len);
int RecvSync(int fd, void *buffer, int len);
int ForwardSync(int fd_src, int fd_dst, int len);

#endif  // UTIL_H_