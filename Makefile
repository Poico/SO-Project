CC = gcc
CFLAGS = -g -Wall -std=c17
TARGET=build/Proj1

.PHONY: all run clean

all: $(TARGET)

run: $(TARGET)
	./$(TARGET)

$(TARGET): Proj1.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $^ -o $@

clean:
	$(RM) -r build
