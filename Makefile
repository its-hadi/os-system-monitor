CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS = $(shell sdl2-config --libs) -lSDL2_image -lSDL2_ttf

SRC = src/main.cpp
OUT = os_system_monitor

all:
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) $(SRC) -o $(OUT) $(SDL_LIBS)

run: all
	./$(OUT)

clean:
	rm -f $(OUT)
