CXX := /opt/homebrew/opt/llvm/bin/clang++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -pedantic
TARGET := cpu_counter
SOURCES := main.cpp

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
