# 📝 Collaborative Markdown Editor

A multithreaded client-server markdown editing system built in C. This project uses POSIX threads, signals, and named FIFOs to support multiple clients editing a shared markdown document with version control, permission handling, and queued operations.

## Overview

This project implements a lightweight collaborative editor where multiple clients can connect to a central server and interact with a shared markdown document.

The server manages:
- client connections
- document state
- version updates
- command processing
- role-based access control

Clients can:
- connect with a username
- view the current document
- check their permissions
- send editing commands
- inspect version logs
- disconnect cleanly

## 🧠 Key Features

- **Multithreaded server** for handling multiple clients concurrently
- **Named FIFO communication** between client and server
- **Signal-based handshake** for client connection setup
- **Role-based permissions** using `roles.txt`
- **Versioned markdown document model**
- **Queued edit commands** sorted by timestamp before version updates
- Support for markdown operations such as:
  - insert
  - delete
  - newline
  - headings
  - bold
  - italic
  - blockquotes
  - ordered lists
  - unordered lists
  - inline code
  - horizontal rules
  - links

## 🏗️ Project Structure

```text
.
├── server.c      # multithreaded server, client handling, version updates
├── client.c      # client connection logic, command input, log viewing
├── markdown.c    # document model and markdown command processing
├── roles.txt     # username -> permission mapping
├── log.txt       # server version log output
└── doc.md        # final persisted markdown document
````

## ⚙️ How It Works

### 🖥️ Server

The server:

* initializes the shared markdown document
* waits for client connection signals
* creates per-client FIFOs
* authenticates users through `roles.txt`
* accepts commands from write-enabled clients
* batches edits into a command queue
* periodically increments the document version
* saves the final markdown document on shutdown

### 👤 Client

Each client:

* signals the server to request a connection
* waits for the FIFO handshake
* sends its username
* receives:

  * permission level
  * current document version
  * initial document contents
* can then issue commands or query local state

### 🧾 Markdown Engine

The markdown layer:

* stores document text as a linked list of characters
* queues operations with timestamps
* sorts queued commands before applying them
* adjusts positions and ranges to account for deletions
* applies markdown formatting safely across version boundaries

## 🚀 Build

Compile with GCC and pthread support:

```bash
gcc -Wall -Wextra -pedantic -pthread server.c markdown.c -o
```
