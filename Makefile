CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
CFLAGS := -Wall -O3 -pthread
TARGET := ps5_web_manager.elf

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)

.PHONY: all clean
