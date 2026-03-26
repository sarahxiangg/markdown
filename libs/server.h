#ifndef SERVER_H
#define SERVER_H
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <pthread.h>

// fifo config
#define PIPE_FMT_C2S    "FIFO_C2S_%d"
#define PIPE_FMT_S2C    "FIFO_S2C_%d"
#define PIPE_NAME_LEN   64
#define FIFO_MODE       0600

// buffer sizes
#define INITIAL_VEC_CAP 8U
#define STDIN_BUF       16
#define USER_BUF        2048
#define LOG_LINE_BUF    256

// threads
void *client_worker(void *arg);
void *stdin_listener(void *unused);
void *version_daemon(void *arg);

// utils
void broadcast_line(const char *line);
int  lookup_role(const char *user, char *out);
void install_handler(void);

#endif