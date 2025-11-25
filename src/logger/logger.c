#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <strings.h>

// Logger configuration
typedef struct {
    LogLevel level;
    FILE *file;
    const char *filepath;
    int initialized;
} Logger;

// Global logger instance
static Logger logger = {
    .level = LOG_INFO,  // Default to INFO level
    .file = NULL,
    .filepath = "application.log",
    .initialized = 0
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

// Parse log level from string
static LogLevel parse_log_level(const char *str) {
    if (!str || str[0] == '\0') {
        return LOG_INFO;
    }
    
    // Case-insensitive comparison
    if (strcasecmp(str, "debug") == 0 || strcmp(str, "0") == 0) {
        return LOG_DEBUG;
    } else if (strcasecmp(str, "info") == 0 || strcmp(str, "1") == 0) {
        return LOG_INFO;
    } else if (strcasecmp(str, "warn") == 0 || strcasecmp(str, "warning") == 0 || strcmp(str, "2") == 0) {
        return LOG_WARN;
    } else if (strcasecmp(str, "error") == 0 || strcmp(str, "3") == 0) {
        return LOG_ERROR;
    } else if (strcasecmp(str, "quiet") == 0 || strcasecmp(str, "none") == 0 || strcasecmp(str, "off") == 0) {
        return LOG_ERROR + 1;  // Suppress all logs
    }
    
    return LOG_INFO;
}

// Initialize logger
int log_init(const char *filepath, LogLevel level) {
    logger.level = level;
    logger.filepath = filepath;
    
    // Check for environment variable override
    const char *env_level = getenv("HYPRSWITCHER_LOG");
    if (env_level && env_level[0] != '\0') {
        logger.level = parse_log_level(env_level);
    }
    
    // Open log file if path provided
    if (filepath && filepath[0] != '\0') {
        logger.file = fopen(filepath, "a");
        if (!logger.file) {
            fprintf(stderr, "Failed to open log file: %s\n", filepath);
            // Continue without file logging
        }
    }
    
    logger.initialized = 1;
    
    return 0;
}

// Close logger
void log_close(void) {
    if (logger.file) {
        fflush(logger.file);
        fclose(logger.file);
        logger.file = NULL;
    }
    logger.initialized = 0;
}

// Set log level
void log_set_level(LogLevel level) {
    logger.level = level;
}

// Get current log level
LogLevel log_get_level(void) {
    return logger.level;
}

// Check if a log level is enabled
int log_level_enabled(LogLevel level) {
    return (level >= logger.level);
}

// Get current timestamp
static void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

// Core logging function
void log_message(LogLevel level, const char *file, int line, const char *fmt, ...) {
    // Skip if level is below threshold
    if (level < logger.level) {
        return;
    }
    
    // Safety check for level bounds
    if (level < LOG_DEBUG || level > LOG_ERROR) {
        return;
    }
    
    char timestamp[24];
    get_timestamp(timestamp, sizeof(timestamp));
    
    // Extract filename from path (remove directory prefix)
    const char *filename = file;
    const char *last_slash = strrchr(file, '/');
    if (last_slash) {
        filename = last_slash + 1;
    }

    va_list args;

    // Log to console with colors
    printf("%s[%s] [%s] [%s:%d] ",
           level_colors[level],
           timestamp,
           level_strings[level],
           filename,
           line);

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);

    printf("%s\n", COLOR_RESET);
    fflush(stdout);

    // Log to file (no colors)
    if (logger.file) {
        fprintf(logger.file, "[%s] [%s] [%s:%d] ",
                timestamp,
                level_strings[level],
                filename,
                line);

        va_start(args, fmt);
        vfprintf(logger.file, fmt, args);
        va_end(args);

        fprintf(logger.file, "\n");
        fflush(logger.file);
    }
}