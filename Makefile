CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I./include
LDFLAGS = -lsqlite3 -lssl -lcrypto -lpthread
SRC     = src/main.c src/util.c src/db.c src/http.c src/tor.c src/index.c
BIN     = 2kewld
CLI_DIR = cli
CLI_BIN = 2kewld-cli

.PHONY: all daemon cli clean

all: daemon cli

daemon: $(SRC) include/2kewld.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

cli:
	cd $(CLI_DIR) && go build -o ../$(CLI_BIN) .

clean:
	rm -f $(BIN) $(CLI_BIN)
