CFLAGS=-D__LINUX__ -c -Wall -O2 
LIBS = 
CXX = g++

all: demo 

demo: main.o AnimatedGIF.o
	$(CXX) main.o AnimatedGIF.o $(LIBS) -o demo 

main.o: main.cpp
	$(CXX) $(CFLAGS) main.cpp

AnimatedGIF.o: ../src/AnimatedGIF.cpp
	$(CXX) $(CFLAGS) ../src/AnimatedGIF.cpp

clean:
	rm -rf *.o demo
