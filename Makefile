CXX      ?= g++
CXXFLAGS ?= -O3 -std=c++17 -march=native -Wall -Wextra

all: server vecsearch
server: server.cpp VectorEngine.hpp
	$(CXX) $(CXXFLAGS) server.cpp -o server
vecsearch: main.cpp VectorEngine.hpp
	$(CXX) $(CXXFLAGS) main.cpp -o vecsearch
clean:
	rm -f server vecsearch
