# SPDX-License-Identifier: GPL-2.0
CC	?= cc
CFLAGS	?= -O2
CFLAGS	+= -Wall -Wextra -Wno-unused-parameter -std=gnu11
USB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
USB_LIBS   := $(shell pkg-config --libs libusb-1.0)

SRCS = src/main.c src/fx2.c src/linux_shim.c src/tbs5530.c src/m88rs6060.c src/sat.c \
       src/stream.c src/ts.c src/service.c src/http.c src/teletext.c
OBJS = $(SRCS:.c=.o)

tbs5530: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(USB_LIBS)

src/%.o: src/%.c src/*.h
	$(CC) $(CFLAGS) $(USB_CFLAGS) -c -o $@ $<

# m88rs6060.c is kept as TBS wrote it; silence the warnings that come with that
src/m88rs6060.o: CFLAGS += -Wno-sign-compare -Wno-unused-variable -Wno-missing-field-initializers

clean:
	rm -f tbs5530 $(OBJS)

.PHONY: clean
