#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "logger.h"
#include "protocol.h"
#include "util.h"

#if defined(__APPLE__)
#define PLATFORM_MACOS 1
#elif defined(__linux__)
#define PLATFORM_LINUX 1
#endif

// StoreAndForward: receives data from src_fd, stores locally at fmeta->filename,
// then forwards the same data to dst_fd. Returns 0 on success, -1 on error.
static int StoreAndForward(int src_fd, int dst_fd, ForwardFile *fmeta, uint32_t data_len) {
  char tmp_path[] = "/tmp/fwd_forward_XXXXXX";
  int tmp_fd = mkstemp(tmp_path);
  if (tmp_fd < 0) {
    LOG_ERROR("mkstemp error, %d", errno);
    return -1;
  }

  // Receive data from src and write to temp file
  char buffer[kForwardBufSize];
  uint32_t remaining = data_len;
  while (remaining > 0) {
    int recv_len = remaining < kForwardBufSize ? remaining : kForwardBufSize;
    int ret = read(src_fd, buffer, recv_len);
    if (ret < 0) {
      LOG_ERROR("read error, %d", errno);
      close(tmp_fd);
      unlink(tmp_path);
      return -1;
    }
    if (ret == 0) {
      break;
    }
    int write_len = write(tmp_fd, buffer, ret);
    if (write_len < 0) {
      LOG_ERROR("write temp error, %d", errno);
      close(tmp_fd);
      unlink(tmp_path);
      return -1;
    }
    remaining -= write_len;
  }

  // Store locally
  int store_fd = open((const char *)fmeta->filename, O_CREAT | O_RDWR, kDefaultFileMode);
  if (store_fd < 0) {
    LOG_ERROR("open file error, %d", errno);
    close(tmp_fd);
    unlink(tmp_path);
    return -1;
  }
  // Rewind temp file to beginning for reading
  lseek(tmp_fd, 0, SEEK_SET);
  int bytes_read;
  while ((bytes_read = read(tmp_fd, buffer, kForwardBufSize)) > 0) {
    int write_len = write(store_fd, buffer, bytes_read);
    if (write_len < 0) {
      LOG_ERROR("write store error, %d", errno);
      close(store_fd);
      close(tmp_fd);
      unlink(tmp_path);
      return -1;
    }
  }
  close(store_fd);
  LOG_INFO("stored %u bytes to %s", data_len, fmeta->filename);

  // Forward to dst: rewind and send same data
  lseek(tmp_fd, 0, SEEK_SET);
  remaining = data_len;
  while (remaining > 0) {
    int send_len = remaining < kForwardBufSize ? remaining : kForwardBufSize;
    int ret = read(tmp_fd, buffer, send_len);
    if (ret <= 0) {
      break;
    }
    int write_len = write(dst_fd, buffer, ret);
    if (write_len < 0) {
      LOG_ERROR("write dst error, %d", errno);
      close(tmp_fd);
      unlink(tmp_path);
      return -1;
    }
    remaining -= write_len;
  }
  close(tmp_fd);
  unlink(tmp_path);

  LOG_INFO("forwarded %u bytes", data_len);
  return 0;
}

int ForwardNext(int fd, ForwardRequest *req, ForwardNode *nodes,
                ForwardFile *fmeta) {
  int ret;
  ForwardRequest next_req;
  ForwardResponse res;
  uint32_t data_len = 0;
  int next_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (next_fd < 0) {
    LOG_ERROR("socket error, %d", errno);
    ret = -1;
    res.retcode = ForwardInternalError;
    goto out_response;
  }
  struct sockaddr_in dst_addr;
  memset(&dst_addr, 0, sizeof(dst_addr));
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(nodes[0].port);
  dst_addr.sin_addr.s_addr = nodes[0].ip;

  if (connect(next_fd, (struct sockaddr *)&dst_addr, sizeof(dst_addr)) < 0) {
    LOG_ERROR("connect error, %d", errno);
    ret = -1;
    res.retcode = ForwardUnreachable;
    goto out_response;
  }
  memcpy(&next_req, req, sizeof(next_req));
  next_req.length -= sizeof(ForwardNode);
  next_req.ttl -= 1;
  ret = SendSync(next_fd, &next_req, sizeof(ForwardRequest));
  if (ret) {
    LOG_ERROR("send forward req error %d", errno);
    ret = -1;
    res.retcode = ForwardInterrupt;
    goto out_response;
  }
  if (next_req.ttl) {
    ret = SendSync(next_fd, nodes + 1, sizeof(ForwardNode) * next_req.ttl);
    if (ret) {
      LOG_ERROR("send forward nodes error %d", errno);
      ret = -1;
      res.retcode = ForwardInterrupt;
      goto out_response;
    }
  }
  ret = SendSync(next_fd, fmeta, sizeof(ForwardFile) + fmeta->length);
  if (ret) {
    LOG_ERROR("send forward file error");
    ret = -1;
    res.retcode = ForwardInterrupt;
    goto out_response;
  }

  data_len = next_req.length - sizeof(ForwardRequest) -
                      sizeof(ForwardNode) * next_req.ttl -
                      sizeof(ForwardFile) - fmeta->length;
  ret = StoreAndForward(fd, next_fd, fmeta, data_len);
  if (ret) {
    LOG_ERROR("store and forward error");
    ret = -1;
    res.retcode = ForwardInterrupt;
    goto out_response;
  }

  ret = RecvSync(next_fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("recv response error");
    ret = -1;
    res.retcode = ForwardInterrupt;
  }
out_response:
  MakeResponse(&res, req, res.retcode);
  ret = SendSync(fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("send response error");
    ret = -1;
  }
out:
  if (next_fd > 0) {
    close(next_fd);
  }
  return ret;
}

int PullForward(int fd, ForwardRequest *req, ForwardNode *nodes,
                ForwardFile *fmeta) {
  int ret;
  ForwardRequest next_req;
  ForwardResponse res;
  int next_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (next_fd < 0) {
    LOG_ERROR("socket error, %d", errno);
    ret = -1;
    res.retcode = ForwardInternalError;
    goto out_pull_response;
  }
  struct sockaddr_in dst_addr;
  memset(&dst_addr, 0, sizeof(dst_addr));
  dst_addr.sin_family = AF_INET;
  dst_addr.sin_port = htons(nodes[0].port);
  dst_addr.sin_addr.s_addr = nodes[0].ip;

  if (connect(next_fd, (struct sockaddr *)&dst_addr, sizeof(dst_addr)) < 0) {
    LOG_ERROR("connect error, %d", errno);
    ret = -1;
    res.retcode = ForwardUnreachable;
    goto out_pull_response;
  }

  memcpy(&next_req, req, sizeof(next_req));
  next_req.length -= sizeof(ForwardNode);
  next_req.ttl -= 1;
  ret = SendSync(next_fd, &next_req, sizeof(next_req));
  if (ret) {
    LOG_ERROR("send pull req error %d", errno);
    ret = -1;
    res.retcode = ForwardInterrupt;
    goto out_pull_response;
  }
  if (next_req.ttl) {
    ret = SendSync(next_fd, nodes + 1, sizeof(ForwardNode) * next_req.ttl);
    if (ret) {
      LOG_ERROR("send pull nodes error %d", errno);
      ret = -1;
      res.retcode = ForwardInterrupt;
      goto out_pull_response;
    }
  }
  ret = SendSync(next_fd, fmeta, sizeof(ForwardFile) + fmeta->length);
  if (ret) {
    LOG_ERROR("send pull fmeta error");
    ret = -1;
    res.retcode = ForwardInterrupt;
    goto out_pull_response;
  }

  ForwardResponse pull_res;
  ret = RecvSync(next_fd, &pull_res, sizeof(pull_res));
  if (ret) {
    LOG_ERROR("recv pull response error");
    ret = -1;
    res.retcode = ForwardInterrupt;
    goto out_pull_response;
  }

  SendSync(fd, &pull_res, sizeof(pull_res));

  if (pull_res.cmd == ForwardPush) {
    ForwardFile pull_fmeta;
    ret = RecvSync(next_fd, &pull_fmeta, sizeof(pull_fmeta));
    if (ret) {
      LOG_ERROR("recv pull fmeta error");
      res.retcode = ForwardInterrupt;
      goto out_pull_response;
    }
    uint32_t fmeta_total_len = sizeof(ForwardFile) + pull_fmeta.length;
    ForwardFile *pull_fmeta_buf = (ForwardFile *)malloc(fmeta_total_len);
    if (!pull_fmeta_buf) {
      LOG_ERROR("malloc error");
      res.retcode = ForwardInternalError;
      goto out_pull_response;
    }
    memcpy(pull_fmeta_buf, &pull_fmeta, sizeof(ForwardFile));
    ret = RecvSync(next_fd, pull_fmeta_buf->filename, pull_fmeta.length);
    if (ret) {
      LOG_ERROR("recv filename error");
      free(pull_fmeta_buf);
      res.retcode = ForwardInterrupt;
      goto out_pull_response;
    }
    SendSync(fd, pull_fmeta_buf, fmeta_total_len);

    uint32_t f_length = pull_res.length - sizeof(ForwardResponse) - fmeta_total_len;
    ret = StoreAndForward(next_fd, fd, pull_fmeta_buf, f_length);
    free(pull_fmeta_buf);
    if (ret) {
      LOG_ERROR("store and forward error");
      res.retcode = ForwardInterrupt;
      goto out_pull_response;
    }
  }

  MakeResponse(&res, req, pull_res.retcode);
  ret = SendSync(fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("send response error");
    ret = -1;
  }
  close(next_fd);
  return ret;

out_pull_response:
  MakeResponse(&res, req, res.retcode);
  ret = SendSync(fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("send response error");
    ret = -1;
  }
out_pull:
  if (next_fd > 0) {
    close(next_fd);
  }
  return ret;
}

int StoreLocal(int fd, ForwardRequest *req, ForwardFile *fmeta) {
  ForwardResponse res;
  int ret = 0;
  int w_fd =
      open((const char *)fmeta->filename, O_CREAT | O_RDWR, kDefaultFileMode);
  if (w_fd < 0) {
    LOG_ERROR("open error, %d", errno);
    return -1;
  }
  ret = ForwardSync(fd, w_fd,
                     req->length - sizeof(ForwardRequest) -
                         sizeof(ForwardFile) - fmeta->length);
  if (ret) {
    LOG_ERROR("forward data error");
    ret = -1;
    goto out;
  }
  close(w_fd);
  MakeResponse(&res, req, ForwardSuccess);
  ret = SendSync(fd, &res, sizeof(res));
  if (ret) {
    LOG_ERROR("send response error");
    ret = -1;
    goto out;
  }
  LOG_INFO("store file(%s) to local", fmeta->filename);
out:
  if (w_fd > 0) {
    close(w_fd);
  }
  return ret;
}

int PullFileToClient(int fd, ForwardRequest *req, ForwardFile *fmeta) {
  ForwardResponse res;
  int ret = 0;
  int r_fd = open((const char *)fmeta->filename, O_RDONLY);
  if (r_fd < 0) {
    LOG_ERROR("open file error, %d", errno);
    MakeResponse(&res, req, ForwardInternalError);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }

  struct stat s;
  if (fstat(r_fd, &s) < 0) {
    LOG_ERROR("stat file error, %d", errno);
    close(r_fd);
    MakeResponse(&res, req, ForwardInternalError);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }

  uint64_t f_size = s.st_size;
  size_t fname_len = strlen((const char *)fmeta->filename);
  uint32_t fmeta_length = sizeof(ForwardFile) + fname_len + 1;

  ForwardResponse resp_res;
  resp_res.length = sizeof(ForwardResponse) + fmeta_length + f_size;
  resp_res.magic = kForwardMagic;
  resp_res.version = kForwardVersion1;
  resp_res.cmd = ForwardPush;
  resp_res.ttl = 0;
  resp_res.retcode = ForwardSuccess;
  resp_res.id = req->id;

  SendSync(fd, &resp_res, sizeof(resp_res));

  ForwardFile *resp_fmeta =
      (ForwardFile *)malloc(fmeta_length);
  if (!resp_fmeta) {
    LOG_ERROR("malloc error");
    close(r_fd);
    MakeResponse(&res, req, ForwardInternalError);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }

  resp_fmeta->length = fname_len + 1;
  memcpy(resp_fmeta->filename, fmeta->filename, fname_len);
  resp_fmeta->filename[fname_len] = '\0';
  SendSync(fd, resp_fmeta, fmeta_length);
  free(resp_fmeta);

  ret = ForwardSync(r_fd, fd, f_size);
  close(r_fd);

  if (ret) {
    LOG_ERROR("forward file error");
    MakeResponse(&res, req, ForwardInterrupt);
    SendSync(fd, &res, sizeof(res));
    return -1;
  }

  MakeResponse(&res, req, ForwardSuccess);
  SendSync(fd, &res, sizeof(res));
  LOG_INFO("pull file(%s) to client success", fmeta->filename);
  return 0;
}

int ForwardLoop(int port) {
  int ret;
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    LOG_ERROR("socket error, %d", errno);
    return -1;
  }
  int optval = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) {
    LOG_ERROR("setsockopt error, %d", errno);
    return -1;
  }
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  if (bind(sockfd, (struct sockaddr *)&address, sizeof(address))) {
    LOG_ERROR("bind error, %d", errno);
    return -1;
  }
  if (listen(sockfd, kDefaultBacklog)) {
    LOG_ERROR("listen error, %d", errno);
    return -1;
  }
  while (1) {
    struct sockaddr_in src_addr;
    socklen_t addr_len = sizeof(src_addr);
    char ip_str[64];
    ForwardRequest req;
    ForwardNode *fnodes = NULL;
    ForwardFile *fmeta = NULL;
    int fd = accept(sockfd, (struct sockaddr *)&src_addr, &addr_len);
    if (fd < 0) {
      LOG_ERROR("accept error, %d", errno);
      continue;
    }
    inet_ntop(AF_INET, &src_addr.sin_addr, ip_str, sizeof(ip_str));
    LOG_DEBUG("accept %d, ip: %s, port: %d", fd, ip_str,
              ntohs(src_addr.sin_port));
    ret = RecvSync(fd, &req, sizeof(uint32_t));
    if (ret) {
      LOG_ERROR("recv req size");
      goto cleanup_fd;
    }
    // ForwardRequest
    ret = RecvSync(fd, (char *)&req + sizeof(uint32_t),
                    sizeof(ForwardRequest) - sizeof(uint32_t));
    if (ret) {
      LOG_ERROR("recv req error");
      goto cleanup_fd;
    }
    LOG_DEBUG(
        "[header]length %d, magic %d, version %d, cmd %d, ttl %d, id "
        "%llu",
        req.length, req.magic, req.version, req.cmd, req.ttl, req.id);
    // ForwardNode
    if (req.ttl) {
      fnodes = (ForwardNode *)malloc(sizeof(ForwardNode) * req.ttl);
      if (!fnodes) {
        LOG_ERROR("malloc fnodes error");
        goto cleanup_fd;
      }
      ret = RecvSync(fd, fnodes, sizeof(ForwardNode) * req.ttl);
      if (ret) {
        LOG_ERROR("recv nodes error");
        goto cleanup_fnodes;
      }
      for (uint32_t i = 0; i < req.ttl; ++i) {
        LOG_DEBUG("[node%u]ip %u, port %u", i, fnodes[i].ip, fnodes[i].port);
      }
    }
    // ForwardFile
    uint32_t f_length;
    ret = RecvSync(fd, &f_length, sizeof(uint32_t));
    if (ret) {
      LOG_ERROR("recv file size error");
      goto cleanup_fnodes;
    }
    fmeta = (ForwardFile *)malloc(sizeof(ForwardFile) + f_length);
    if (!fmeta) {
      LOG_ERROR("malloc fmeta error");
      goto cleanup_fnodes;
    }
    fmeta->length = f_length;
    ret = RecvSync(fd, fmeta->filename, f_length);
    if (ret) {
      LOG_ERROR("recv file name error");
      goto cleanup_fmeta;
    }
// Handle request based on command
    if (req.cmd == ForwardPull) {
      if (req.ttl > 0) {
        ret = PullForward(fd, &req, fnodes, fmeta);
      } else {
        ret = PullFileToClient(fd, &req, fmeta);
      }
    } else {
      if (req.ttl > 0) {
        ret = ForwardNext(fd, &req, fnodes, fmeta);
      } else {
        ret = StoreLocal(fd, &req, fmeta);
      }
    }

  cleanup_fmeta:
    free(fmeta);
    fmeta = NULL;
  cleanup_fnodes:
    free(fnodes);
    fnodes = NULL;
  cleanup_fd:
    close(fd);
    continue;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int opt;
  int port = kDefaultPort;
  char *dir = NULL;

  while ((opt = getopt(argc, argv, "p:d:h")) != -1) {
    switch (opt) {
      case 'p':
        port = atoi(optarg);
        break;
      case 'd':
        dir = strdup(optarg);
        struct stat s;
        if (stat(dir, &s)) {
          LOG_ERROR("stat error, dir %s, %d", dir, errno);
          return -1;
        }
        if (!S_ISDIR(s.st_mode)) {
          LOG_ERROR("input dir is not directory, dir %s, mode %d", dir,
                    s.st_mode);
          return -1;
        }
        break;
      case 'h':
        printf("Usage: %s [-p port] [-d dir]\n", argv[0]);
        printf("  -p port  : specify port (default %d)\n", kDefaultPort);
        printf("  -d dir   : specify working directory\n");
        printf("  -h       : show this help\n");
        return 0;
      default:
        printf("Usage: %s [-p port] [-d dir]\n", argv[0]);
        printf("  -p port  : specify port (default %d)\n", kDefaultPort);
        printf("  -d dir   : specify working directory\n");
        printf("  -h       : show this help\n");
        return -1;
    }
  }
  if (!dir) {
    dir = strdup("/");
  }
  char log_path[PATH_MAX];
  snprintf(log_path, sizeof(log_path), "%s/forward_server.log", dir);
  int ret = LogInit(log_path);
  if (ret) {
    return -1;
  }
  atexit(LogClose);

  LOG_INFO("Forward daemon started, working directory: %s", dir);
  pid_t pid = fork();
  if (pid < 0) {
    LOG_ERROR("fork error, %d", errno);
    return -1;
  }
  // parent
  if (pid > 0) {
    // printf("parent exit, child pid: %d\n", pid);
    return 0;
  }
  if (setsid() < 0) {
    LOG_ERROR("setsid error, %d", errno);
    return -1;
  }
  signal(SIGCHLD, SIG_IGN);
  signal(SIGHUP, SIG_IGN);
  pid = fork();
  if (pid < 0) {
    LOG_ERROR("fork error, %d", errno);
    return -1;
  }
  if (pid > 0) {
    return 0;
  }
  umask(0);
  chdir(dir);
  free(dir);
  return ForwardLoop(port);
}
