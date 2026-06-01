CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I./include
LDFLAGS = -lsqlite3 -lssl -lcrypto -lpthread
SRC     = src/main.c src/util.c src/db.c src/http.c src/admin.c src/tor.c src/index.c
BIN     = kewld
CLI_DIR = cli
CLI_BIN = kewld-cli

.PHONY: all daemon cli clean

all: daemon cli

daemon: $(SRC) include/kewld.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

cli:
	cd $(CLI_DIR) && go build -o ../$(CLI_BIN) .

clean:
	rm -f $(BIN) $(CLI_BIN)
