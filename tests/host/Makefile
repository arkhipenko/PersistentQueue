# Host tests for PersistentQueue: the library against a model of the arduino-esp32 FS API.
# make                      - build and run for the SPIFFS and the LittleFS model, and for the IDF 3.x core
# make CXX=clang++ STD=c++20

CXX ?= g++
STD ?= gnu++17

SRC      = ../../src/PersistentQueue.cpp
CXXFLAGS = -std=$(STD) -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
           -I stubs -I ../../src -DPQ_REPO_DIR=\"../..\"
DEPS     = test_persistentqueue.cpp $(SRC) ../../src/PersistentQueue.h $(wildcard stubs/*.h)

all: run

test_spiffs: $(DEPS)
	$(CXX) $(CXXFLAGS) -DPQ_USES_SPIFFS -o $@ test_persistentqueue.cpp $(SRC)

test_littlefs: $(DEPS)
	$(CXX) $(CXXFLAGS) -DPQ_USES_LITTLEFS -o $@ test_persistentqueue.cpp $(SRC)

test_idf3: $(DEPS)
	$(CXX) $(CXXFLAGS) -DPQ_USES_SPIFFS -DPQ_TEST_IDF3 -o $@ test_persistentqueue.cpp $(SRC)

run: test_spiffs test_littlefs test_idf3
	./test_spiffs
	./test_littlefs
	./test_idf3

clean:
	rm -f test_spiffs test_littlefs test_idf3

.PHONY: all run clean
