CC=gcc

all: fawrap.so

fawrap.so: fawrap.c
	$(CC) -Wall -shared -fPIC fawrap.c -o fawrap.so -ldl

clean:
	rm -f fawrap.so
