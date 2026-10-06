# NoSleep 构建脚本（MinGW-w64）。编译参数只在这里维护，build.ps1 与 CI 都调用本文件。
#
#   Windows（MSYS2 / MinGW）：make
#   Linux 交叉编译：          make CROSS=x86_64-w64-mingw32-
#   视警告为错误（CI 使用）：  make EXTRA_CFLAGS=-Werror

CROSS        ?=
CC           := $(CROSS)gcc
WINDRES      := $(CROSS)windres

TARGET       := nosleep_pure.exe
CFLAGS       ?= -Os
WARNINGS     := -Wall -Wextra
EXTRA_CFLAGS ?=
LDFLAGS      := -s -static -mwindows -municode
LDLIBS       := -lcomctl32 -lshell32

all: $(TARGET)

$(TARGET): nosleep_win32.c nosleep.res
	$(CC) $(CFLAGS) $(WARNINGS) $(EXTRA_CFLAGS) -o $@ nosleep_win32.c nosleep.res $(LDFLAGS) $(LDLIBS)

nosleep.res: nosleep.rc nosleep.manifest nosleep.ico
	$(WINDRES) -i nosleep.rc -o $@ -O coff

clean:
	$(RM) $(TARGET) nosleep.res

.PHONY: all clean
