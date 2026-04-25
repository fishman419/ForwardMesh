CXX=g++
CXXFLAGS=-std=c++11 -I./src
DEPS = src/protocol.h src/util.h src/logger.h

BUILD_DIR = build
BIN_DIR = bin

OBJ_FWDD = $(BUILD_DIR)/fwdd.o $(BUILD_DIR)/util.o $(BUILD_DIR)/logger.o
OBJ_FWD = $(BUILD_DIR)/fwd.o $(BUILD_DIR)/util.o $(BUILD_DIR)/logger.o
TARGET = $(BIN_DIR)/fwdd $(BIN_DIR)/fwd

$(shell mkdir -p $(BUILD_DIR) $(BIN_DIR))

$(BUILD_DIR)/%.o: src/%.cc $(DEPS)
	$(CXX) -c -o $@ $< $(CXXFLAGS)

all: $(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

$(BIN_DIR)/fwdd: $(OBJ_FWDD)
	$(CXX) -o $@ $^ $(CXXFLAGS)

$(BIN_DIR)/fwd: $(OBJ_FWD)
	$(CXX) -o $@ $^ $(CXXFLAGS)

# ASAN build (Linux only - macOS has issues with ASAN + fork + threads)
ASAN_CXXFLAGS=-std=c++11 -I./src -fsanitize=address -g -fno-omit-frame-pointer
ASAN_BUILD_DIR = build_asan
ASAN_BIN_DIR = bin_asan

$(shell mkdir -p $(ASAN_BUILD_DIR) $(ASAN_BIN_DIR))

$(ASAN_BUILD_DIR)/%.o: src/%.cc $(DEPS)
	$(CXX) -c -o $@ $< $(ASAN_CXXFLAGS)

$(ASAN_BIN_DIR)/fwdd: $(ASAN_BUILD_DIR)/fwdd.o $(ASAN_BUILD_DIR)/util.o $(ASAN_BUILD_DIR)/logger.o
	$(CXX) -o $@ $^ $(ASAN_CXXFLAGS)

$(ASAN_BIN_DIR)/fwd: $(ASAN_BUILD_DIR)/fwd.o $(ASAN_BUILD_DIR)/util.o $(ASAN_BUILD_DIR)/logger.o
	$(CXX) -o $@ $^ $(ASAN_CXXFLAGS)

asan: $(ASAN_BIN_DIR)/fwdd $(ASAN_BIN_DIR)/fwd

asan-test: asan
	ASAN_BIN=$(ASAN_BIN_DIR) ./tests/test_asan.sh

# macOS leak check using leaks(1)
LEAKS_BUILD_DIR = build_leaks
LEAKS_BIN_DIR = bin_leaks

$(shell mkdir -p $(LEAKS_BUILD_DIR) $(LEAKS_BIN_DIR))

$(LEAKS_BUILD_DIR)/%.o: src/%.cc $(DEPS)
	$(CXX) -c -o $@ $< $(CXXFLAGS) -g

$(LEAKS_BIN_DIR)/fwdd: $(LEAKS_BUILD_DIR)/fwdd.o $(LEAKS_BUILD_DIR)/util.o $(LEAKS_BUILD_DIR)/logger.o
	$(CXX) -o $@ $^ $(CXXFLAGS) -g

$(LEAKS_BIN_DIR)/fwd: $(LEAKS_BUILD_DIR)/fwd.o $(LEAKS_BUILD_DIR)/util.o $(LEAKS_BUILD_DIR)/logger.o
	$(CXX) -o $@ $^ $(CXXFLAGS) -g

leaks-check: $(LEAKS_BIN_DIR)/fwdd $(LEAKS_BIN_DIR)/fwd
	LEAKS_BIN=$(LEAKS_BIN_DIR) ./tests/test_leaks.sh

.PHONY: all clean asan asan-test leaks-check

