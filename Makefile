CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pthread -Iinclude -g
LDFLAGS = -lncurses -lrt -pthread

SRC_LP = src/main.c src/parser.c src/metrics.c src/buffer.c src/worker.c src/ui.c
OBJ_LP = $(SRC_LP:.c=.o)

.PHONY: all clean run gen

all: logpulse loggen

logpulse: $(OBJ_LP)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

loggen: src/loggen.c
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

gen: loggen
	./loggen -r 0 -n 1000000 access_test.log

run: logpulse
	./logpulse -m lote -t 4 access_test.log

clean:
	rm -f src/*.o logpulse loggen *.log *.csv
