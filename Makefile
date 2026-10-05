CFLAGS ?= -O2 -Wall -Wextra
UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
BOUNDSFLAGS += -fbounds-safety
LDLIBS += -framework CoreFoundation -framework DiskArbitration -liconv
endif
SRCS = main.c util.c progress.c device.c label.c report.c plan.c fat.c exfat.c tables.c
OBJS = $(SRCS:.c=.o)
flashfmt: $(OBJS)
	$(CC) $(CFLAGS) $(BOUNDSFLAGS) -o $@ $(OBJS) $(LDLIBS)
%.o: %.c flashfmt.h flashfmt_tables.h
	$(CC) $(CFLAGS) $(BOUNDSFLAGS) -c -o $@ $<
clean:
	rm -f flashfmt $(OBJS)
.PHONY: clean
