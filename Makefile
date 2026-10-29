# make | make install | make CC=musl-gcc LDFLAGS=-static

CC ?= cc
CSTD = -std=c11
OPTFLAGS ?= -Os -ffunction-sections -fdata-sections
CFLAGS ?= -Wall -Wextra
LDFLAGS ?=

TINY = -flto -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident
TINY_LINK = -Wl,--gc-sections -Wl,--build-id=none -s

wiretui: wiretui.c termbox2.h
	$(CC) $(CSTD) $(OPTFLAGS) $(TINY) $(CFLAGS) $(LDFLAGS) $(TINY_LINK) -o $@ wiretui.c

release: wiretui

# PREFIX: /usr/local as root, else ~/.local
install: wiretui
	@prefix="$(PREFIX)"; \
	if [ -z "$$prefix" ]; then \
	    if [ "$$(id -u)" = 0 ]; then prefix=/usr/local; else prefix="$$HOME/.local"; fi; \
	fi; \
	mkdir -p "$(DESTDIR)$$prefix/bin" && \
	install -m 755 wiretui "$(DESTDIR)$$prefix/bin/wiretui" && \
	echo "installed $(DESTDIR)$$prefix/bin/wiretui"

clean:
	rm -f wiretui
.PHONY: release install clean
