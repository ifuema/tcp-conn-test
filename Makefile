CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=

BINDIR  := .
SRC     := src

all: $(BINDIR)/server $(BINDIR)/client

$(BINDIR)/server: $(SRC)/server.c $(SRC)/ev.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/server.c $(LDFLAGS)

$(BINDIR)/client: $(SRC)/client.c $(SRC)/ev.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/client.c $(LDFLAGS)

clean:
	rm -f $(BINDIR)/server $(BINDIR)/client

.PHONY: all clean
