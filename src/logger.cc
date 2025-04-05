#include "logger.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

FILE* log_file = NULL;

int log_init(const char* filename) {
  if (log_file) {
    fclose(log_file);
  }

  log_file = fopen(filename, "a");
  if (!log_file) {
    perror("Failed to open log file");
    return errno;
  }

  setvbuf(log_file, NULL, _IOLBF, 0);
  return 0;
}

void log_close() {
  if (log_file) {
    fclose(log_file);
    log_file = NULL;
  }
}

void log_write(LogLevel level, const char* format, ...) {
  if (!log_file) return;

  time_t now;
  time(&now);
  struct tm* tm_info = localtime(&now);

  char time_str[20];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

  const char* level_str;
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

  fprintf(log_file, "[%s] [%s] ", time_str, level_str);

  va_list args;
  va_start(args, format);
  vfprintf(log_file, format, args);
  va_end(args);

  fprintf(log_file, "\n");
}