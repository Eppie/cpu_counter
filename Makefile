CXX := /opt/homebrew/opt/llvm/bin/clang++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -pedantic -I.
TARGET := cpu_counter
SOURCES := main.cpp $(wildcard demos/*.cpp)
TEST_API := test_api_compile
TEST_REGISTRY := test_registry_check

.PHONY: all clean run test test-disable live-smoke

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $(TARGET)

$(TEST_API): tests/api_compile.cpp
	$(CXX) $(CXXFLAGS) tests/api_compile.cpp -o $(TEST_API)

$(TEST_REGISTRY): tests/registry_check.cpp $(wildcard demos/*.cpp)
	$(CXX) $(CXXFLAGS) tests/registry_check.cpp $(wildcard demos/*.cpp) -o $(TEST_REGISTRY)

run: $(TARGET)
	./$(TARGET) help

test: $(TEST_API) $(TEST_REGISTRY)
	./$(TEST_API)
	./$(TEST_REGISTRY)

test-disable:
	$(CXX) $(CXXFLAGS) -DPERF_DISABLE tests/api_compile.cpp -o $(TEST_API)
	./$(TEST_API)

live-smoke: $(TARGET)
	sudo ./$(TARGET) validate --prefer-pcore --require-stable-cpu --require-active-pmu

clean:
	rm -f $(TARGET) $(TEST_API) $(TEST_REGISTRY)
