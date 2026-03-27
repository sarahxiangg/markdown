// markdown document management with versioning and command queuing
#include "../libs/markdown.h"
#include "../libs/document.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

// configuration constants for initial sizes and buffer
#define CMD_ARRAY_INIT 10
#define DEL_SPAN_INIT 8
#define BUFFER_SIZE 16

// structure to track deleted spans for cursor adjustments
typedef struct {
    size_t start_pos;
    size_t end_pos;
} DelSpan;

// allocate memory safely with zero initialization
static void* alloc_safe(size_t count, size_t size) {
    void* ptr = calloc(count, size);
    if (!ptr) return NULL;
    return ptr;
}

// create linked list from string for efficient edits
static int make_text_nodes(const char* str, TextNode** start, TextNode** finish) {
    if (!str || !*str) return RET_FAIL;
    TextNode* initial = malloc(sizeof(TextNode));
    if (!initial) return RET_FAIL;
    initial->data = *str;
    initial->prev = NULL;
    TextNode* curr = initial;
    size_t idx = 1;
    do {
        if (!str[idx]) break;
        TextNode* new_node = malloc(sizeof(TextNode));
        if (!new_node) return RET_FAIL;
        new_node->data = str[idx];
        new_node->prev = curr;
        curr->next = new_node;
        curr = new_node;
        idx++;
    } while (1);
    curr->next = NULL;
    *start = initial;
    *finish = curr;
    return RET_OK;
}

// ensure command array has space, resizing if needed
static int ensure_cmd_space(document* doc) {
    if (!doc->commands) {
        doc->commands = alloc_safe(CMD_ARRAY_INIT, sizeof(Operation*));
        if (!doc->commands) return RET_FAIL;
        doc->cmd_capacity = CMD_ARRAY_INIT;
    }
    if (doc->cmd_count >= doc->cmd_capacity) {
        size_t new_size = doc->cmd_capacity * 2;
        Operation** temp = realloc(doc->commands, new_size * sizeof(Operation*));
        if (!temp) return RET_FAIL;
        doc->commands = temp;
        doc->cmd_capacity = new_size;
    }
    return RET_OK;
}

// swap two operations for sorting
static void swap_operations(Operation** a, Operation** b) {
    Operation* temp = *a;
    *a = *b;
    *b = temp;
}

// sort commands by timestamp to ensure order of operations
static void sort_commands(document* doc) {
    if (doc->cmd_count < 2) return;
    size_t i = 0;
    do {
        if (i >= doc->cmd_count - 1) break;
        size_t j = 0;
        do {
            if (j >= doc->cmd_count - i - 1) break;
            if (doc->commands[j] && doc->commands[j+1] &&
                doc->commands[j]->time_ns > doc->commands[j+1]->time_ns) {
                swap_operations(&doc->commands[j], &doc->commands[j+1]);
            }
            j++;
        } while (1);
        i++;
    } while (1);
}

// initialize document with default values
document* doc_init(void) {
    document* doc = alloc_safe(1, sizeof(document));
    if (!doc) return NULL;
    doc->cmd_capacity = CMD_ARRAY_INIT;
    doc->modification_time = (uint64_t)time(NULL);
    return doc;
}

// free all document resources
void doc_free(document* doc) {
    if (!doc) return;
    TextNode* node = doc->begin;
    while (node) {
        TextNode* next = node->next;
        free(node);
        node = next;
    }
    size_t i = 0;
    while (i < doc->cmd_count) {
        if (doc->commands[i]) {
            free(doc->commands[i]->content);
            free(doc->commands[i]);
        }
        i++;
    }
    free(doc->commands);
    free(doc);
}

// find position in text, returns 0 (start), 1 (middle), 2 (end), or error
int doc_find_position(const document* doc, size_t pos, TextNode** prev, TextNode** next) {
    if (!doc || pos > doc->char_count) return RET_BAD_POS;
    if (pos == 0) {
        if (prev) *prev = NULL;
        if (next) *next = doc->begin;
        return 0; // start of document
    }
    if (pos == doc->char_count) {
        if (prev) *prev = doc->end;
        if (next) *next = NULL;
        return 2; // end of document
    }
    TextNode* curr = doc->begin;
    size_t count = 0;
    while (count < pos && curr) {
        curr = curr->next;
        count++;
    }
    if (!curr) return RET_BAD_POS;
    if (prev) *prev = curr->prev;
    if (next) *next = curr;
    return 1; // middle of document
}

// insert text at specified position
int doc_insert(document* doc, size_t pos, const char* text) {
    if (!doc || !text || pos > doc->char_count) return RET_BAD_POS;
    TextNode* head, *tail;
    if (make_text_nodes(text, &head, &tail) != RET_OK) return RET_FAIL;
    size_t len = strlen(text);
    TextNode* prev, *next;
    int result = doc_find_position(doc, pos, &prev, &next);
    if (result == RET_BAD_POS) return RET_BAD_POS;
    // insert at start
    if (result == 0) { 
        tail->next = doc->begin;
        if (doc->begin) doc->begin->prev = tail;
        doc->begin = head;
        if (!doc->end) doc->end = tail;
    // insert at end
    } else if (result == 2) { 
        if (doc->end) doc->end->next = head;
        head->prev = doc->end;
        doc->end = tail;
        if (!doc->begin) doc->begin = head;
    // insert in middle
    } else { 
        prev->next = head;
        head->prev = prev;
        tail->next = next;
        next->prev = tail;
    }
    doc->char_count += len;
    doc->modification_time = (uint64_t)time(NULL);
    return RET_OK;
}

// delete text from specified position
int doc_delete(document* doc, size_t pos, size_t len) {
    if (!doc || pos >= doc->char_count) return RET_BAD_POS;
    if (pos + len > doc->char_count) len = doc->char_count - pos; // adjust len if beyond doc size
    TextNode* start_prev, *start_next, *end_prev, *end_next;
    if (doc_find_position(doc, pos, &start_prev, &start_next) == RET_BAD_POS) return RET_BAD_POS;
    if (doc_find_position(doc, pos + len, &end_prev, &end_next) == RET_BAD_POS) return RET_BAD_POS;
    TextNode* curr = start_next;
    // free nodes in range
    while (curr != end_next) { 
        TextNode* temp = curr->next;
        free(curr);
        curr = temp;
    }
    if (!start_prev) doc->begin = end_next; // update begin if deleting from start
    else start_prev->next = end_next;
    if (end_next) end_next->prev = start_prev; // update end if deleting to end
    else doc->end = start_prev;
    doc->char_count -= len;
    doc->modification_time = (uint64_t)time(NULL);
    return RET_OK;
}

// convert document to string
char* doc_to_string(const document* doc) {
    if (!doc || doc->char_count == 0) {
        char* empty = malloc(1);
        if (empty) *empty = '\0';
        return empty;
    }
    char* result = malloc(doc->char_count + 1);
    if (!result) return NULL;
    TextNode* curr = doc->begin;
    size_t i = 0;
    while (curr) {
        result[i++] = curr->data;
        curr = curr->next;
    }
    result[i] = '\0';
    return result;
}

// check if position is at line start
static bool is_line_begin(document* doc, size_t pos) {
    TextNode* prev, *next;
    int result = doc_find_position(doc, pos, &prev, &next);
    return result == 0 || (prev && prev->data == '\n');
}

// apply inline style to range
static int apply_style(document* doc, size_t start, size_t end, const char* marker) {
    if (start >= end || end > doc->char_count) return RET_BAD_POS;
    if (doc_insert(doc, end, marker) != RET_OK) return RET_FAIL; // add end marker first to avoid shifting
    if (doc_insert(doc, start, marker) != RET_OK) return RET_FAIL;
    return RET_OK;
}

// create heading prefix based on level
static int create_heading_prefix(char* buffer, size_t level) {
    if (level < 1 || level > 3) return RET_BAD_POS;
    size_t i = 0;
    do {
        if (i >= level) break;
        buffer[i++] = '#';
    } while (1);
    buffer[i++] = ' ';
    buffer[i] = '\0';
    return RET_OK;
}

// insert heading at position
static int add_heading(document* doc, size_t level, size_t pos) {
    if (pos > doc->char_count) return RET_BAD_POS;
    char buffer[BUFFER_SIZE] = {0};
    if (create_heading_prefix(buffer, level) != RET_OK) return RET_BAD_POS;
    if (!is_line_begin(doc, pos) && doc_insert(doc, pos, "\n") != RET_OK) return RET_FAIL; // ensure new line
    return doc_insert(doc, pos + (!is_line_begin(doc, pos)), buffer);
}

// insert blockquote at position
static int add_blockquote(document* doc, size_t pos) {
    if (pos > doc->char_count) return RET_BAD_POS;
    const char* prefix = "> ";
    if (!is_line_begin(doc, pos) && doc_insert(doc, pos, "\n") != RET_OK) return RET_FAIL; // ensure new line
    return doc_insert(doc, pos + (!is_line_begin(doc, pos)), prefix);
}

// insert unordered list item
static int add_unordered_item(document* doc, size_t pos) {
    if (pos > doc->char_count) return RET_BAD_POS;
    const char* prefix = "- ";
    if (!is_line_begin(doc, pos) && doc_insert(doc, pos, "\n") != RET_OK) return RET_FAIL; // ensure new line
    return doc_insert(doc, pos + (!is_line_begin(doc, pos)), prefix);
}

// insert ordered list item and renumber subsequent items
static int add_ordered_item(document* doc, size_t pos) {
    if (pos > doc->char_count) return RET_BAD_POS;
    size_t prev_num = 0;
    TextNode* scan;
    doc_find_position(doc, pos, &scan, NULL);
    // find previous number
    while (scan) { 
        if (scan->data == '\n' && scan->prev && scan->prev->data == '\n') break;
        if (scan->data == '.' && scan->prev && isdigit((unsigned char)scan->prev->data) &&
            scan->next && scan->next->data == ' ') {
            prev_num = (size_t)(scan->prev->data - '0');
            break;
        }
        scan = scan->prev;
    }
    char prefix[BUFFER_SIZE];
    snprintf(prefix, sizeof(prefix), "%zu. ", prev_num + 1);
    bool need_nl = !is_line_begin(doc, pos);
    if (need_nl && doc_insert(doc, pos, "\n") != RET_OK) return RET_FAIL;
    size_t ins_pos = pos + (need_nl ? 1 : 0);
    if (doc_insert(doc, ins_pos, prefix) != RET_OK) return RET_FAIL;
    size_t next_num = prev_num + 2;
    size_t text_pos = ins_pos + strlen(prefix);
    TextNode* walker;
    doc_find_position(doc, text_pos, NULL, &walker);
    // renumber subsequent items
    while (walker) { 
        if (walker->data == '\n' && walker->next && walker->next->data == '\n') break;
        if (isdigit((unsigned char)walker->data) && walker->next && walker->next->data == '.') {
            walker->data = (char)('0' + (next_num % 10));
            next_num++;
        }
        while (walker && walker->data != '\n') walker = walker->next;
        if (walker) walker = walker->next;
        else break;
        if (!(walker && isdigit((unsigned char)walker->data) && walker->next && walker->next->data == '.')) break;
    }
    return RET_OK;
}

// insert horizontal rule with newline
static int add_separator(document* doc, size_t pos) {
    if (pos > doc->char_count) return RET_BAD_POS;
    if (!is_line_begin(doc, pos) && doc_insert(doc, pos, "\n") != RET_OK) return RET_FAIL; // ensure new line
    return doc_insert(doc, pos + (!is_line_begin(doc, pos)), "---\n");
}

// insert link with url
static int add_link(document* doc, size_t start, size_t end, const char* url) {
    if (!url || start >= end || end > doc->char_count) return RET_BAD_POS;
    size_t url_len = strlen(url);
    char* suffix = malloc(url_len + 5);
    if (!suffix) return RET_FAIL;
    snprintf(suffix, url_len + 5, "](%s)", url);
    // insert link start
    if (doc_insert(doc, start, "[") != RET_OK) { 
        free(suffix);
        return RET_FAIL;
    }
    end++;
    // insert link end with url
    int result = doc_insert(doc, end, suffix); 
    free(suffix);
    return result;
}

// queue command for processing
static int add_command(document* doc, CommandType type, size_t pos, size_t len,
                       const char* content, size_t start, size_t end, size_t level) {
    if (ensure_cmd_space(doc) != RET_OK) return RET_FAIL;
    Operation* cmd = alloc_safe(1, sizeof(Operation));
    if (!cmd) return RET_FAIL;
    cmd->cmd_type = type;
    cmd->pos = pos;
    cmd->len = len;
    cmd->start = start;
    cmd->end = end;
    cmd->level = level;
    if (content) cmd->content = strdup(content);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    // timestamp in nanoseconds
    cmd->time_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec; 
    doc->commands[doc->cmd_count++] = cmd;
    return RET_OK;
}

// adjust position for deletions
static size_t adjust_pos(size_t pos, DelSpan* spans, size_t count) {
    size_t i = 0;
    while (i < count) {
        // clamp to deletion start
        if (pos >= spans[i].start_pos && pos < spans[i].end_pos) return spans[i].start_pos; 
        i++;
    }
    return pos;
}

// adjust range for deletions
static void adjust_range(Operation* cmd, DelSpan* spans, size_t count) {
    size_t i = 0;
    while (i < count) {
        DelSpan* span = &spans[i];
         // range fully deleted
        if (cmd->start >= span->start_pos && cmd->end <= span->end_pos) {
            cmd->start = cmd->end = span->end_pos;
            return;
        }
        // adjust start
        if (cmd->start >= span->start_pos && cmd->start < span->end_pos) cmd->start = span->end_pos; 
         // adjust end
        if (cmd->end > span->start_pos && cmd->end <= span->end_pos) cmd->end = span->start_pos;
        i++;
    }
}

// process queued commands and increment version
void markdown_increment_version(document* doc) {
    if (!doc) return;
    sort_commands(doc);
    DelSpan* spans = alloc_safe(DEL_SPAN_INIT, sizeof(DelSpan));
    size_t span_count = 0, span_capacity = DEL_SPAN_INIT;
    size_t i = 0;
    while (i < doc->cmd_count) {
        Operation* cmd = doc->commands[i];
        if (!cmd) {
            i++;
            continue;
        }
        switch (cmd->cmd_type) {
            case CMD_INSERT:
                cmd->pos = adjust_pos(cmd->pos, spans, span_count);
                doc_insert(doc, cmd->pos, cmd->content);
                break;
            case CMD_DELETE:
                doc_delete(doc, cmd->pos, cmd->len);
                // resize array if needed
                if (span_count == span_capacity) { 
                    span_capacity *= 2;
                    DelSpan* temp = realloc(spans, span_capacity * sizeof(DelSpan));
                    if (!temp) break;
                    spans = temp;
                }
                spans[span_count].start_pos = cmd->pos;
                spans[span_count].end_pos = cmd->pos + cmd->len;
                span_count++;
                break;
            // handling the different commands
            case CMD_NEWLINE:
                cmd->pos = adjust_pos(cmd->pos, spans, span_count);
                doc_insert(doc, cmd->pos, "\n");
                break;
            case CMD_HEADING:
                cmd->pos = adjust_pos(cmd->pos, spans, span_count);
                add_heading(doc, cmd->level, cmd->pos);
                break;
            case CMD_BOLD:
                adjust_range(cmd, spans, span_count);
                if (cmd->start < cmd->end) apply_style(doc, cmd->start, cmd->end, "**");
                break;
            case CMD_ITALIC:
                adjust_range(cmd, spans, span_count);
                if (cmd->start < cmd->end) apply_style(doc, cmd->start, cmd->end, "*");
                break;
            case CMD_BLOCKQUOTE:
                cmd->pos = adjust_pos(cmd->pos, spans, span_count);
                add_blockquote(doc, cmd->pos);
                break;
            case CMD_ORDERED_LIST:
                cmd->pos = adjust_pos(cmd->pos, spans, span_count);
                add_ordered_item(doc, cmd->pos);
                break;
            case CMD_UNORDERED_LIST:
                cmd->pos = adjust_pos(cmd->pos, spans, span_count);
                add_unordered_item(doc, cmd->pos);
                break;
            case CMD_CODE:
                adjust_range(cmd, spans, span_count);
                if (cmd->start < cmd->end) apply_style(doc, cmd->start, cmd->end, "`");
                break;
            case CMD_HORIZONTAL_RULE:
                cmd->pos = adjust_pos(cmd->pos, spans, span_count);
                add_separator(doc, cmd->pos);
                break;
            case CMD_LINK:
                adjust_range(cmd, spans, span_count);
                if (cmd->start < cmd->end) add_link(doc, cmd->start, cmd->end, cmd->content);
                break;
            default:
                break;
        }
        free(cmd->content);
        free(cmd);
        doc->commands[i] = NULL;
        i++;
    }
    free(spans);
    doc->cmd_count = 0;
    doc->version++;
    doc->modification_time = (uint64_t)time(NULL);
}

// markdown api functions with version checking
#define CHECK_VERSION(doc, v) do { if ((v) != (doc)->version) return RET_OUTDATED_VER; } while (0)

document* markdown_init(void) { return doc_init(); }
void markdown_free(document* doc) { doc_free(doc); }

int markdown_insert(document* doc, uint64_t version, size_t pos, const char* content) {
    CHECK_VERSION(doc, version);
    return add_command(doc, CMD_INSERT, pos, 0, content, 0, 0, 0);
}

int markdown_delete(document* doc, uint64_t version, size_t pos, size_t len) {
    CHECK_VERSION(doc, version);
    return add_command(doc, CMD_DELETE, pos, len, NULL, 0, 0, 0);
}

int markdown_newline(document* doc, uint64_t version, size_t pos) {
    CHECK_VERSION(doc, version);
    return add_command(doc, CMD_NEWLINE, pos, 0, NULL, 0, 0, 0);
}

int markdown_heading(document* doc, uint64_t version, size_t level, size_t pos) {
    CHECK_VERSION(doc, version);
    return add_command(doc, CMD_HEADING, pos, 0, NULL, 0, 0, level);
}

int markdown_bold(document* doc, uint64_t version, size_t start, size_t end) {
    CHECK_VERSION(doc, version);
    return add_command(doc, CMD_BOLD, 0, 0, NULL, start, end, 0);
}

int markdown_italic(document* doc, uint64_t version, size_t start, size_t end) {
    CHECK_VERSION(doc, version);
    return add_command(doc, CMD_ITALIC, 0, 0, NULL, start, end, 0);
}

int markdown_blockquote(document* doc, uint64_t version, size_t pos) {
    CHECK_VERSION(doc, version);
    return add_command(doc, CMD_BLOCKQUOTE, pos, 0, NULL, 0, 0, 0);
}

int markdown_ordered_list(document* doc, uint64_t version, size_t pos) {
    CHECK_VERSION(doc, version);
    return add_command(doc, CMD_ORDERED_LIST, pos, 0, NULL, 0, 0, 0);
}

int markdown_unordered_list(document* doc, uint64_t version, size_t pos) {
    CHECK_VERSION(doc, version);
    return add_command(doc, CMD_UNORDERED_LIST, pos, 0, NULL, 0, 0, 0);
}

int markdown_code(document* doc, uint64_t version, size_t start, size_t end) {
    CHECK_VERSION(doc, version);
    return add_command(doc, CMD_CODE, 0, 0, NULL, start, end, 0);
}

int markdown_horizontal_rule(document* doc, uint64_t version, size_t pos) {
    CHECK_VERSION(doc, version);
    return add_command(doc, CMD_HORIZONTAL_RULE, pos, 0, NULL, 0, 0, 0);
}

int markdown_link(document* doc, uint64_t version, size_t start, size_t end, const char* url) {
    CHECK_VERSION(doc, version);
    return add_command(doc, CMD_LINK, 0, 0, url, start, end, 0);
}

void markdown_print(const document* doc, FILE* out) {
    if (!doc || !out) return;
    char* text = doc_to_string(doc);
    if (text) {
        fputs(text, out);
        free(text);
    }
}

char* markdown_flatten(const document* doc) {
    return doc_to_string(doc);
}