CC = gcc
CFLAGS = -g -Wall -std=c17
TARGETS = Proj1

.PHONY: all clean

all: $(TARGETS)

clean:
	rm -f *.o $(TARGETS)
