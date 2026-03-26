#ifndef CLIENT_H
#define CLIENT_H

#define _POSIX_C_SOURCE 200809L 

#include <stdio.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

// max length of a single command from stdin
enum { CLI_LINE_MAX = 256 }; 

// global flag used to signal client shutdown
extern volatile sig_atomic_t g_client_quit;

// block SIGRTMIN+1 for synchronous use with sigwait()
void block_realtime(sigset_t *set);

// read role, version, length, and document content from server
int read_initial_document(FILE *in);

// background listener thread for receiving server updates and logging
void *listener(void *arg);

#endif 
