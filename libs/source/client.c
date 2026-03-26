#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "../libs/client.h"
#include "../libs/markdown.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

// client program for a document editing system

volatile sig_atomic_t g_client_quit = 0;

static char *g_log_path = NULL;
static char *g_doc_text = NULL;
static char *g_role = NULL;
static size_t g_doc_len = 0;
static uint64_t g_version = 0;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

// signal used for server-client handshake
void block_realtime(sigset_t *set) {
    sigemptyset(set);
    sigaddset(set, SIGRTMIN + 1);
    if (sigprocmask(SIG_BLOCK, set, NULL) == -1) {
        die("sigprocmask");
    }
}

// read initial data from server -> role, version, and document
// returns 0 on success, 1 if unauthorized, -1 on error
int read_initial_document(FILE *in) {
    char buf[64];

    if (!fgets(buf, sizeof buf, in)) return -1;
    if (strcmp(buf, "Reject UNAUTHORISED\n") == 0) return 1;
    buf[strcspn(buf, "\n")] = 0;
    g_role = strdup(buf);

    if (!fgets(buf, sizeof buf, in)) return -1;
    sscanf(buf, "%lu", &g_version);

    if (!fgets(buf, sizeof buf, in)) return -1;
    sscanf(buf, "%zu", &g_doc_len);

    g_doc_text = calloc(g_doc_len + 1, 1);
    if (!g_doc_text) return -1;
    if (g_doc_len && fread(g_doc_text, 1, g_doc_len, in) != g_doc_len)
        return -1;

    return 0;
}

// listener thread -> reads updates from server and logs them
void *listener(void *arg) {
    FILE *in = arg;
    FILE *log = fopen(g_log_path, "w");
    if (!log) die("open log");

    char line[2048];
    int inside_version = 0;

    while (!g_client_quit && fgets(line, sizeof line, in)) {
        if (strncmp(line, "VERSION", 7) == 0) {
            inside_version = 1;
            fputs(line, log);
        } else if (inside_version) {
            fputs(line, log);
            if (strncmp(line, "END", 3) == 0) {
                fflush(log);
                inside_version = 0;
            }
        }
    }

    fclose(log);
    return NULL;
}

int main(int argc, char **argv) {
    // parse command-line arguments
    if (argc != 3) {
        fprintf(stderr, "usage: %s <server-pid> <username>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // convert server pid and store username
    pid_t server_pid = strtol(argv[1], NULL, 10);
    const char *user = argv[2];

    // generate unique log file name using timestamp
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int suffix = (int)((ts.tv_nsec ^ ts.tv_sec) & 0xFFFF);
    asprintf(&g_log_path, "client_log_%s_%d.txt", user, suffix);
    if (!g_log_path) die("asprintf");

    // set up signal blocking
    sigset_t set;
    block_realtime(&set);

    // signal server to initiate connection
    if (kill(server_pid, SIGRTMIN) == -1) die("signal-server");

    // wait for server response signal
    int sig;
    if (sigwait(&set, &sig) != 0 || sig != SIGRTMIN + 1) die("sigwait");

    // create named pipes for communication
    char fifo_c2s[64], fifo_s2c[64];
    snprintf(fifo_c2s, sizeof fifo_c2s, "FIFO_C2S_%d", getpid());
    snprintf(fifo_s2c, sizeof fifo_s2c, "FIFO_S2C_%d", getpid());

    // open pipes
    int wfd = open(fifo_c2s, O_WRONLY);
    if (wfd == -1) die("open c2s");
    int rfd = open(fifo_s2c, O_RDONLY);
    if (rfd == -1) die("open s2c");

    // create file streams for pipes
    FILE *out = fdopen(wfd, "w");
    FILE *in = fdopen(rfd, "r");

    // send username to server
    fprintf(out, "%s\n", user);
    fflush(out);

    // read initial document data
    int status = read_initial_document(in);
    if (status == 1) {
        fprintf(stderr, "Server refused access.\n");
        return EXIT_FAILURE;
    } else if (status != 0) {
        die("initial payload");
    }

    // start listener thread
    pthread_t th;
    pthread_create(&th, NULL, listener, in);

    // process user input
    char line[CLI_LINE_MAX];
    while (!g_client_quit && fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;

        if (strcmp(line, "DOC?") == 0) {
            puts(g_doc_text);
        } else if (strcmp(line, "PERM?") == 0) {
            puts(g_role);
        } else if (strcmp(line, "LOG?") == 0) {
            FILE *log = fopen(g_log_path, "r");
            if (!log) continue;
            char l[2048];
            while (fgets(l, sizeof l, log)) fputs(l, stdout);
            fclose(log);
        } else {
            fprintf(out, "%s\n", line);
            fflush(out);
            if (strcmp(line, "DISCONNECT") == 0) {
                g_client_quit = 1;
                break;
            }
        }
    }

    // cleanup
    pthread_join(th, NULL);
    free(g_doc_text);
    free(g_log_path);
    free(g_role);
    return 0;
}