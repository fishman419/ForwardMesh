#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERROR } LogLevel;

extern FILE* log_file;

int log_init(const char* filename);
void log_close();
void log_write(LogLevel level, const char* format, ...);

#define LOG_DEBUG(...) log_write(LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...) log_write(LOG_INFO, __VA_ARGS__)
#define LOG_WARNING(...) log_write(LOG_WARNING, __VA_ARGS__)
#define LOG_ERROR(...) log_write(LOG_ERROR, __VA_ARGS__)

#endif  // LOGGER_H