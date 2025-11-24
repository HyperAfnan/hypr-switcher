#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

// Logger configuration
typedef struct {
    LogLevel level;
    FILE *file;
    const char *filepath;
} Logger;

// Global logger instance
static Logger logger = {
    .level = LOG_DEBUG,
    .file = NULL,
    .filepath = "application.log"
};

// Color codes for console output
#define COLOR_RESET   "\033[0m"
#define COLOR_DEBUG   "\033[36m"  // Cyan
#define COLOR_INFO    "\033[32m"  // Green
#define COLOR_WARN    "\033[33m"  // Yellow
#define COLOR_ERROR   "\033[31m"  // Red

// Log level strings
static const char* level_strings[] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};

// Log level colors
static const char* level_colors[] = {
    COLOR_DEBUG, COLOR_INFO, COLOR_WARN, COLOR_ERROR
};

// Initialize logger
int log_init(const char *filepath, LogLevel level) {
    logger.level = level;
    logger.filepath = filepath;

    logger.file = fopen(filepath, "a");
    if (!logger.file) {
        fprintf(stderr, "Failed to open log file: %s\n", filepath);
        return -1;
    }

    return 0;
}

// Close logger
void log_close(void) {
    if (logger.file) {
        fclose(logger.file);
        logger.file = NULL;
    }
}

// Set log level
void log_set_level(LogLevel level) {
    logger.level = level;
}

// Get current timestamp
static void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

// Core logging function
void log_message(LogLevel level, const char *file, int line, const char *fmt, ...) {
    if (level < logger.level) {
        return;
    }

    char timestamp[20];
    get_timestamp(timestamp, sizeof(timestamp));

    va_list args;

    // Log to console with colors
    printf("%s[%s] [%s] [%s:%d] ",
           level_colors[level],
           timestamp,
           level_strings[level],
           file,
           line);

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("%s\n", COLOR_RESET);

    // Log to file (no colors)
    if (logger.file) {
        fprintf(logger.file, "[%s] [%s] [%s:%d] ",
                timestamp,
                level_strings[level],
                file,
                line);

        va_start(args, fmt);
        vfprintf(logger.file, fmt, args);
        va_end(args);

        fprintf(logger.file, "\n");
        fflush(logger.file);  // Ensure immediate write
    }
}
