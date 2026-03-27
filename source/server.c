#define _POSIX_C_SOURCE 200809L

#include "../libs/server.h"
#include "../libs/markdown.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// constants for buffer sizes and initial capacities
enum {
    FIFO_NAME_SIZE = 64,    // size for fifo path names
    ROLE_SIZE      = 16,    // size for role strings (r/w)
    COMMAND_SIZE   = 256,   // max length of client command
    ARRAY_INIT_CAP = 8      // initial capacity for dynamic arrays
};

// global state for document and synchronization
static document* shared_doc;                    // shared document instance
static pthread_mutex_t doc_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile sig_atomic_t should_exit = 0;   // flag for server shutdown
static pthread_mutex_t exit_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t exit_cond = PTHREAD_COND_INITIALIZER;

static volatile sig_atomic_t doc_changed = 0;   // flag for document edits
static pthread_mutex_t change_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t* client_threads = NULL;       // array of client threads
static pthread_mutex_t* client_locks = NULL;   // per-client mutexes
static int* client_write_fds = NULL;           // file descriptors for client pipes
static size_t client_count = 0;                // current number of clients
static size_t client_capacity = 0;             // capacity of client arrays
static volatile sig_atomic_t active_clients = 0; // number of connected clients

static FILE* version_log;                      // file for version history
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// allocate memory with error checking
void* safe_alloc(size_t count, size_t size) {
    void* ptr = calloc(count, size);
    if (!ptr) {
        perror("memory allocation failed");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

// resize dynamic array with error checking
void resize_array(void** array, size_t* capacity, size_t element_size) {
    *capacity = (*capacity == 0) ? ARRAY_INIT_CAP : *capacity * 2;
    *array = realloc(*array, *capacity * element_size);
    if (!*array) {
        perror("array resize failed");
        exit(EXIT_FAILURE);
    }
}

// send message to all connected clients
void notify_clients(const char* message) {
    pthread_mutex_lock(&doc_mutex);
    for (size_t i = 0; i < client_count; ++i) {
        if (client_write_fds[i] == -1) continue; // skip inactive clients
        FILE* stream = fdopen(client_write_fds[i], "w");
        if (stream) {
            pthread_mutex_lock(&client_locks[i]);
            fputs(message, stream);
            fflush(stream);
            pthread_mutex_unlock(&client_locks[i]);
        }
    }
    pthread_mutex_unlock(&doc_mutex);
}

// check user permissions from roles.txt
int get_user_role(const char* username, char* role) {
    FILE* file = fopen("roles.txt", "r");
    if (!file) return -1;

    char line[USER_BUF];
    while (fgets(line, sizeof(line), file)) {
        char* name = strtok(line, " \n");
        char* perm = strtok(NULL, " \n");
        if (name && perm && strcmp(name, username) == 0) {
            strncpy(role, perm, ROLE_SIZE);
            fclose(file);
            return 0;
        }
    }
    fclose(file);
    return -1;
}

// open a named pipe with error handling
int setup_fifo(const char* path, int flags) {
    int fd = open(path, flags);
    if (fd == -1) {
        perror("failed to open fifo");
        pthread_exit(NULL);
    }
    return fd;
}

// map command string to commandtype
CommandType get_command_type(const char* action) {
    if (strcmp(action, "INSERT") == 0) return CMD_INSERT;
    if (strcmp(action, "DEL") == 0) return CMD_DELETE;
    if (strcmp(action, "NEWLINE") == 0) return CMD_NEWLINE;
    if (strcmp(action, "HEADING") == 0) return CMD_HEADING;
    if (strcmp(action, "BOLD") == 0) return CMD_BOLD;
    if (strcmp(action, "ITALIC") == 0) return CMD_ITALIC;
    if (strcmp(action, "BLOCKQUOTE") == 0) return CMD_BLOCKQUOTE;
    if (strcmp(action, "ORDERED_LIST") == 0) return CMD_ORDERED_LIST;
    if (strcmp(action, "UNORDERED_LIST") == 0) return CMD_UNORDERED_LIST;
    if (strcmp(action, "CODE") == 0) return CMD_CODE;
    if (strcmp(action, "HORIZONTAL_RULE") == 0) return CMD_HORIZONTAL_RULE;
    if (strcmp(action, "LINK") == 0) return CMD_LINK;
    return -1; // invalid command
}

// handle client communication
void* handle_client(void* arg) {
    int client_pid = *(int*)arg;
    free(arg);

    // build fifo paths
    char client_to_server[FIFO_NAME_SIZE];
    char server_to_client[FIFO_NAME_SIZE];
    snprintf(client_to_server, sizeof(client_to_server), PIPE_FMT_C2S, client_pid);
    snprintf(server_to_client, sizeof(server_to_client), PIPE_FMT_S2C, client_pid);

    // create fifos
    unlink(client_to_server);
    unlink(server_to_client);
    if (mkfifo(client_to_server, FIFO_MODE) || mkfifo(server_to_client, FIFO_MODE)) {
        perror("mkfifo failed");
        pthread_exit(NULL);
    }

    // notify client that fifos are ready
    kill(client_pid, SIGRTMIN + 1);

    // open fifo endpoints
    FILE* input = fdopen(setup_fifo(client_to_server, O_RDONLY), "r");
    FILE* output = fdopen(setup_fifo(server_to_client, O_WRONLY), "w");

    // add client to global arrays
    pthread_mutex_lock(&doc_mutex);
    if (client_count >= client_capacity) {
        resize_array((void**)&client_threads, &client_capacity, sizeof(pthread_t));
        resize_array((void**)&client_locks, &client_capacity, sizeof(pthread_mutex_t));
        resize_array((void**)&client_write_fds, &client_capacity, sizeof(int));
    }
    size_t client_id = client_count;
    client_threads[client_id] = pthread_self();
    pthread_mutex_init(&client_locks[client_id], NULL);
    client_write_fds[client_id] = dup(fileno(output));
    ++client_count;
    ++active_clients;
    pthread_mutex_unlock(&doc_mutex);

    // get username
    char username[USER_BUF] = {0};
    if (!fgets(username, sizeof(username), input)) goto cleanup;
    username[strcspn(username, "\n")] = 0;

    // check permissions
    char role[ROLE_SIZE] = "read";
    if (get_user_role(username, role) != 0) {
        fputs("Reject UNAUTHORISED\n", output);
        fflush(output);
        sleep(1);  // wait 1 second before cleanup as per specification
        goto cleanup;
    }

    // send initial document
    pthread_mutex_lock(&doc_mutex);
    char* doc_text = markdown_flatten(shared_doc);
    uint64_t version = shared_doc->version;
    size_t doc_length = strlen(doc_text);
    pthread_mutex_unlock(&doc_mutex);

    fprintf(output, "%s\n%lu\n%zu\n", role, version, doc_length);
    fwrite(doc_text, 1, doc_length, output);
    fflush(output);
    free(doc_text);

    // command loop
    char command[COMMAND_SIZE];
    while (fgets(command, sizeof(command), input)) {
        command[strcspn(command, "\n")] = 0;
        if (strcmp(command, "DISCONNECT") == 0) break;

        if (strcmp(role, "write") != 0) {
            // unauthorized commands are skipped; response will be in broadcast
            continue;
        }

        char action[16];
        sscanf(command, "%15s", action);
        CommandType cmd_type = get_command_type(action);
        int result = RET_FAIL;

        pthread_mutex_lock(&doc_mutex);
        switch (cmd_type) {
            case CMD_INSERT: {
                size_t pos;
                char* text = strchr(command, ' ') + 1;
                sscanf(text, "%zu", &pos);
                text = strchr(text, ' ') + 1;
                result = markdown_insert(shared_doc, shared_doc->version, pos, text);
                break;
            }
            case CMD_DELETE: {
                size_t pos, len;
                sscanf(command + 4, "%zu %zu", &pos, &len);
                result = markdown_delete(shared_doc, shared_doc->version, pos, len);
                break;
            }
            case CMD_NEWLINE: {
                size_t pos;
                sscanf(command + 8, "%zu", &pos);
                result = markdown_newline(shared_doc, shared_doc->version, pos);
                break;
            }
            case CMD_HEADING: {
                size_t level, pos;
                sscanf(command + 8, "%zu %zu", &level, &pos);
                result = markdown_heading(shared_doc, shared_doc->version, level, pos);
                break;
            }
            case CMD_BOLD: {
                size_t start, end;
                sscanf(command + 5, "%zu %zu", &start, &end);
                result = markdown_bold(shared_doc, shared_doc->version, start, end);
                break;
            }
            case CMD_ITALIC: {
                size_t start, end;
                sscanf(command + 7, "%zu %zu", &start, &end);
                result = markdown_italic(shared_doc, shared_doc->version, start, end);
                break;
            }
            case CMD_BLOCKQUOTE: {
                size_t pos;
                sscanf(command + 11, "%zu", &pos);
                result = markdown_blockquote(shared_doc, shared_doc->version, pos);
                break;
            }
            case CMD_ORDERED_LIST: {
                size_t pos;
                sscanf(command + 13, "%zu", &pos);
                result = markdown_ordered_list(shared_doc, shared_doc->version, pos);
                break;
            }
            case CMD_UNORDERED_LIST: {
                size_t pos;
                sscanf(command + 15, "%zu", &pos);
                result = markdown_unordered_list(shared_doc, shared_doc->version, pos);
                break;
            }
            case CMD_CODE: {
                size_t start, end;
                sscanf(command + 5, "%zu %zu", &start, &end);
                result = markdown_code(shared_doc, shared_doc->version, start, end);
                break;
            }
            case CMD_HORIZONTAL_RULE: {
                size_t pos;
                sscanf(command + 16, "%zu", &pos);
                result = markdown_horizontal_rule(shared_doc, shared_doc->version, pos);
                break;
            }
            case CMD_LINK: {
                size_t start, end;
                char url[USER_BUF];
                sscanf(command + 5, "%zu %zu %s", &start, &end, url);
                result = markdown_link(shared_doc, shared_doc->version, start, end, url);
                break;
            }
            default:
                result = RET_FAIL;
        }
        pthread_mutex_unlock(&doc_mutex);

        if (result == RET_OK) {
            pthread_mutex_lock(&change_mutex);
            doc_changed = 1;
            pthread_mutex_unlock(&change_mutex);
        }
    }

cleanup:
    fclose(output);
    fclose(input);
    close(client_write_fds[client_id]);
    unlink(client_to_server);
    unlink(server_to_client);

    pthread_mutex_lock(&doc_mutex);
    client_write_fds[client_id] = -1;
    pthread_mutex_unlock(&doc_mutex);

    --active_clients;
    return NULL;
}

// process stdin commands
void* process_stdin(void* arg) {
    (void)arg;
    char input[32];
    while (fgets(input, sizeof(input), stdin)) {
        if (strcmp(input, "QUIT\n") == 0) {
            if (active_clients == 0) {
                pthread_mutex_lock(&exit_mutex);
                should_exit = 1;
                pthread_cond_signal(&exit_cond);
                pthread_mutex_unlock(&exit_mutex);
            } else {
                printf("QUIT rejected — %d clients active\n", (int)active_clients);
            }
            continue;
        } else if (strcmp(input, "DOC?\n") == 0) {
            pthread_mutex_lock(&doc_mutex);
            markdown_print(shared_doc, stdout);
            pthread_mutex_unlock(&doc_mutex);
        } else if (strcmp(input, "LOG?\n") == 0) {
            system("cat log.txt");
        }
    }
    return NULL;
}

// periodically update document version
void* update_version(void* arg) {
    int sleep_interval = *(int*)arg;
    free(arg);

    while (!should_exit) {
        sleep(sleep_interval);
        if (!doc_changed) continue;

        pthread_mutex_lock(&doc_mutex);
        markdown_increment_version(shared_doc);
        // clear the command queue
        for (size_t i = 0; i < shared_doc->cmd_count; ++i) {
            free(shared_doc->commands[i]->content);
            free(shared_doc->commands[i]);
        }
        free(shared_doc->commands);
        shared_doc->commands = NULL;
        shared_doc->cmd_count = 0;
        pthread_mutex_unlock(&doc_mutex);

        pthread_mutex_lock(&log_mutex);
        fprintf(version_log, "VERSION %lu\n", shared_doc->version);
        fflush(version_log);
        notify_clients("VERSION UPDATED\n");
        doc_changed = 0;
        pthread_mutex_unlock(&log_mutex);
    }
    return NULL;
}

// handle client connection signal
void handle_signal(int sig, siginfo_t* info, void* ctx) {
    (void)sig;
    (void)ctx;
    int* client_pid = safe_alloc(1, sizeof(int));
    *client_pid = info->si_pid;
    pthread_t thread;
    pthread_create(&thread, NULL, handle_client, client_pid);
}

// set up signal handler
void configure_signal_handler(void) {
    struct sigaction action = {
        .sa_flags = SA_SIGINFO,
        .sa_sigaction = handle_signal
    };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGRTMIN, &action, NULL) == -1) {
        perror("signal handler setup failed");
        exit(EXIT_FAILURE);
    }
}

// main server logic
int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <interval>\n", argv[0]);
        return 1;
    }

    printf("Server PID: %d\n", getpid());  // print pid immediately upon start

    // initialize document and log file
    shared_doc = markdown_init();
    version_log = fopen("log.txt", "w");

    // set up signal handling
    configure_signal_handler();

    // create threads for stdin and version updates
    pthread_t stdin_thread, version_thread;
    pthread_create(&stdin_thread, NULL, process_stdin, NULL);
    int* interval = safe_alloc(1, sizeof(int));
    *interval = atoi(argv[1]);
    pthread_create(&version_thread, NULL, update_version, interval);

    // wait for shutdown signal
    pthread_mutex_lock(&exit_mutex);
    while (!should_exit) {
        pthread_cond_wait(&exit_cond, &exit_mutex);
    }
    pthread_mutex_unlock(&exit_mutex);

    // final document save
    markdown_increment_version(shared_doc);
    FILE* output = fopen("doc.md", "w");
    markdown_print(shared_doc, output);
    fseek(output, -1, SEEK_END);
    if (fgetc(output) == '\n') {
        ftruncate(fileno(output), ftell(output));
    }
    fclose(output);

    // clean up resources
    fclose(version_log);
    markdown_free(shared_doc);
    return 0;
}