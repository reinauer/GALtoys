CC ?= cc
CFLAGS ?= -Wall -Wextra -O2

TARGET = jedi
SRCS = jedi.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)
