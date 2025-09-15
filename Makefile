CC=gcc
CFLAGS=-Wall -Wextra -O2 -g
LIBS=-lreadline -lncurses

SRC=mainl.c parse.c
OBJ=$(SRC:.c=.o)

yash: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(OBJ) yash
