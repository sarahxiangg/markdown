#ifndef DOCUMENT_H
#define DOCUMENT_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// command types for edit operations
typedef enum {
    CMD_INSERT,
    CMD_DELETE,
    CMD_NEWLINE,
    CMD_HEADING,
    CMD_BOLD,
    CMD_ITALIC,
    CMD_BLOCKQUOTE,
    CMD_ORDERED_LIST,
    CMD_UNORDERED_LIST,
    CMD_CODE,
    CMD_HORIZONTAL_RULE,
    CMD_LINK
} CommandType;

// Node for a single character
typedef struct text_node {
    char data;
    struct text_node* next;
    struct text_node* prev;
} TextNode;

// edit operation
typedef struct operation {
    CommandType cmd_type;   // type of operation
    size_t pos;             // cursor position
    size_t len;             // len for deletion
    char* content;          // text for insert/link
    size_t level;           // heading level
    size_t start;           // inline style start
    size_t end;             // inline style end
    uint64_t time_ns;       // timestamp nanoseconds
} Operation;

// document structure
typedef struct document {
    uint64_t version;       // version counter
    uint64_t modification_time; // last modification time
    TextNode* begin;        // start of text list
    TextNode* end;          // end of text list
    size_t char_count;      // total characters
    Operation** commands;   // array of queued operations
    size_t cmd_count;       // number of commands
    size_t cmd_capacity;    // command array capacity
} document;


document* doc_init(void);
void doc_free(document* doc);
int doc_insert(document* doc, size_t pos, const char* text);
int doc_delete(document* doc, size_t pos, size_t len);
char* doc_to_string(const document* doc);

// Helper for markdown layer
int doc_find_position(const document* doc, size_t pos, TextNode** prev, TextNode** next);

#endif 