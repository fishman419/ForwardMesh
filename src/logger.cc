#include "logger.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mutex>

namespace {
FILE *g_log_file = NULL;
std::mutex g_log_mutex;
}  // namespace

int LogInit(const char *filename) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_log_file) {
    fclose(g_log_file);
    g_log_file = NULL;
  }

  g_log_file = fopen(filename, "a");
  if (!g_log_file) {
    perror("Failed to open log file");
    return errno ? errno : -1;
  }

  setvbuf(g_log_file, NULL, _IOLBF, 0);
  return 0;
}

void LogClose() {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (g_log_file) {
    fclose(g_log_file);
    g_log_file = NULL;
  }
}

void LogWrite(LogLevel level, const char *format, ...) {
  std::lock_guard<std::mutex> lock(g_log_mutex);
  if (!g_log_file) return;

  time_t now = time(NULL);
  struct tm tm_info;
  localtime_r(&now, &tm_info);

  char time_str[20];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_info);

  const char *level_str;
  switch (level) {
    case LOG_DEBUG:
      level_str = "DEBUG";
      break;
    case LOG_INFO:
      level_str = "INFO";
      break;
    case LOG_WARNING:
      level_str = "WARNING";
      break;
    case LOG_ERROR:
      level_str = "ERROR";
      break;
    default:
      level_str = "UNKNOWN";
      break;
  }

  fprintf(g_log_file, "[%s] [%s] ", time_str, level_str);

  va_list args;
  va_start(args, format);
  vfprintf(g_log_file, format, args);
  va_end(args);

  fprintf(g_log_file, "\n");
  fflush(g_log_file);
}
