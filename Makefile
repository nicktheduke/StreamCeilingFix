# Cross-compile with MinGW-w64 (Linux, WSL or MSYS2) and test under Wine.
#   make           build build/StreamCeilingFix.dll
#   make test      build the UCRT test harness and run it under Wine (normal + locked-log runs)
#   make package   build/StreamCeilingFix-<version>.zip, ready for MO2 / Vortex
VERSION ?= 1.0.1
CC      ?= x86_64-w64-mingw32-gcc
ifeq ($(CC),cc)
CC := x86_64-w64-mingw32-gcc
endif
WINE    ?= wine
CFLAGS  := -O2 -Wall -Wextra
DLL     := build/StreamCeilingFix.dll

all: $(DLL)

$(DLL): src/StreamCeilingFix.c | build
	$(CC) $(CFLAGS) -s -shared -o $@ $< -lshell32 -static-libgcc

build:
	mkdir -p build

# The harness must import its stdio functions from the Universal CRT, like SkyrimSE.exe does.
build/ucrt.specs: | build
	$(CC) -dumpspecs | sed 's/-lmsvcrt/-lucrt/g' > $@

build/harness.exe: tests/harness.c build/ucrt.specs | build
	$(CC) -specs=build/ucrt.specs -O1 -o $@ $< -lshell32

test: $(DLL) build/harness.exe
	cd build && WINEDEBUG=-all $(WINE) harness.exe
	cd build && WINEDEBUG=-all $(WINE) harness.exe lock

package: $(DLL)
	rm -rf build/pkg && mkdir -p build/pkg/SKSE/Plugins
	cp $(DLL) build/pkg/SKSE/Plugins/
	cp "dist/StreamCeilingFix - README.txt" build/pkg/
	cd build/pkg && rm -f ../StreamCeilingFix-$(VERSION).zip && zip -r -X ../StreamCeilingFix-$(VERSION).zip SKSE "StreamCeilingFix - README.txt"

clean:
	rm -rf build

.PHONY: all test package clean
