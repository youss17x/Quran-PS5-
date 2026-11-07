# Quran PS5 — desktop build (Linux), for development and testing.
# The console build is ps5/build-ps5.py (see BUILD.md).
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PKGS     = sdl2 libcurl

quran: src/main.c src/quran_data.h src/quran_layout.h
	$(CC) $(CFLAGS) $$(pkg-config --cflags $(PKGS)) -o $@ src/main.c $$(pkg-config --libs $(PKGS)) -lm

run: quran
	./quran

# regenerate the data tables and Arabic labels (needs Pillow built with libraqm)
data:
	python3 tools/gen_data.py

clean:
	rm -f quran
	rm -rf ps5/build ps5/QRAN00001 ps5/QRAN00001-*.zip

.PHONY: run data clean
