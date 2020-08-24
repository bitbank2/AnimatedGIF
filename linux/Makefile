CFLAGS=-D__LINUX__ -c -Wall -O2 
LIBS = 

all: demo 

demo: main.o gif.o
	$(CC) main.o gif.o $(LIBS) -o demo 

main.o: main.c
	$(CC) $(CFLAGS) main.c

gif.o: ../src/gif.c
	$(CC) $(CFLAGS) ../src/gif.c

clean:
	rm -rf *.o demo
