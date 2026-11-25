#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARNING, LOG_ERROR } LogLevel;

int LogInit(const char *filename);
void LogClose();
void LogWrite(LogLevel level, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_DEBUG(...) LogWrite(LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...) LogWrite(LOG_INFO, __VA_ARGS__)
#define LOG_WARNING(...) LogWrite(LOG_WARNING, __VA_ARGS__)
#define LOG_ERROR(...) LogWrite(LOG_ERROR, __VA_ARGS__)

#endif  // LOGGER_H
