CC := gcc
CFLAGS := -Wall -Wextra -pthread -Ilibs

SRC := source
COMMON := $(SRC)/markdown.c
SERVER := $(SRC)/server.c
CLIENT := $(SRC)/client.c

OBJ := markdown.o

all: server client

server: source/server.o $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@

client: source/client.o $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@

%.o: $(SRC)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

source/%.o: $(SRC)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f server client *.o source/*.o