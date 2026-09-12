CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?=

BINDIR  := .
SRC     := src

# Windows 交叉编译器, 可覆盖:
#   macOS/Linux 交叉编译: make windows WINCC="zig cc -target x86_64-windows-gnu"
#   MSYS2/MinGW 里直接 make 即可(原生编译, 自动走 _WIN32 分支)
WINCC   ?= x86_64-w64-mingw32-gcc
WINDIR  := build-windows
WINLIBS := -lws2_32 -lmswsock

# Windows 下产物带 .exe (在 MSYS2/MinGW 里 $(OS)=Windows_NT)
ifeq ($(OS),Windows_NT)
EXE     := .exe
LIBS    := $(WINLIBS)
else
EXE     :=
LIBS    := $(LDFLAGS)
endif

all: $(BINDIR)/server$(EXE) $(BINDIR)/client$(EXE)

$(BINDIR)/server$(EXE): $(SRC)/server.c $(SRC)/ev.h $(SRC)/wincompat.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/server.c $(LIBS)

$(BINDIR)/client$(EXE): $(SRC)/client.c $(SRC)/ev.h $(SRC)/wincompat.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/client.c $(LIBS)

# 交叉编译 Windows 版本, 产出 build-windows/{server,client}.exe
windows:
	mkdir -p $(WINDIR)
	$(WINCC) $(CFLAGS) -o $(WINDIR)/server.exe $(SRC)/server.c $(WINLIBS)
	$(WINCC) $(CFLAGS) -o $(WINDIR)/client.exe $(SRC)/client.c $(WINLIBS)

clean:
	rm -f $(BINDIR)/server$(EXE) $(BINDIR)/client$(EXE)
	rm -rf $(WINDIR)

.PHONY: all windows clean
