OBJDIR := obj
BINDIR := bin

CC := gcc
CFLAGS := -Wall -Wextra -pedantic
DFLAGS := -MD -MP
LDFLAGS := -lm -lrt

SRCS := $(wildcard *.c)
OBJS := $(SRCS:%.c=$(OBJDIR)/%.o)

# Create the build directories if they are not there already
$(shell mkdir -p $(OBJDIR)/examples $(BINDIR))

.PHONY: all posix clean

all: posix

# Include any .d files generated on prior builds (via DFLAGS) this results in
# files getting recompiled if their headers change
include $(shell find $(OBJDIR) -name '*.d')

# Rule to build .o and .d files
$(OBJDIR)/%.o: %.c
	$(CC) -I. $(CFLAGS) $(DFLAGS) -c $< -o $@

# Rules to build the posix example
posix: $(BINDIR)/posix
$(BINDIR)/posix: $(OBJDIR)/examples/posix.o $(OBJS)
	$(CC) $(CFLAGS) $< $(OBJS) -o $@ $(LDFLAGS)

clean:
	rm -rf $(OBJDIR) $(BINDIR)
