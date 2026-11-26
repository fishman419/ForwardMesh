CXX ?= g++
CXXFLAGS = -std=c++11 -Wall -Wextra -pthread -O2 -I./src
DEPS = src/protocol.h src/util.h src/logger.h src/mesh.h

BUILD_DIR = build
BIN_DIR = bin

OBJ_FWDD = $(BUILD_DIR)/fwdd.o $(BUILD_DIR)/mesh.o $(BUILD_DIR)/util.o $(BUILD_DIR)/logger.o
OBJ_FWD = $(BUILD_DIR)/fwd.o $(BUILD_DIR)/util.o $(BUILD_DIR)/logger.o
OBJ_UNIT = $(BUILD_DIR)/test_mesh_unit.o $(BUILD_DIR)/mesh.o $(BUILD_DIR)/util.o $(BUILD_DIR)/logger.o
TARGET = $(BIN_DIR)/fwdd $(BIN_DIR)/fwd

all: $(TARGET)

$(BUILD_DIR)/%.o: src/%.cc $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) -c -o $@ $< $(CXXFLAGS)

$(BIN_DIR)/fwdd: $(OBJ_FWDD)
	@mkdir -p $(BIN_DIR)
	$(CXX) -o $@ $^ $(CXXFLAGS)

$(BIN_DIR)/fwd: $(OBJ_FWD)
	@mkdir -p $(BIN_DIR)
	$(CXX) -o $@ $^ $(CXXFLAGS)

$(BUILD_DIR)/test_mesh_unit.o: tests/test_mesh_unit.cc $(DEPS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) -c -o $@ $< $(CXXFLAGS)

$(BIN_DIR)/test_mesh_unit: $(OBJ_UNIT)
	@mkdir -p $(BIN_DIR)
	$(CXX) -o $@ $^ $(CXXFLAGS)

unit-test: $(BIN_DIR)/test_mesh_unit
	./$(BIN_DIR)/test_mesh_unit

test: all unit-test
	./tests/test_basic.sh
	./tests/test_mesh.sh

# AddressSanitizer build & test (runs in foreground -F mode)
ASAN_CXXFLAGS = -std=c++11 -Wall -Wextra -pthread -I./src -fsanitize=address -g -O1 -fno-omit-frame-pointer
ASAN_BUILD_DIR = build_asan
ASAN_BIN_DIR = bin_asan

$(ASAN_BUILD_DIR)/%.o: src/%.cc $(DEPS)
	@mkdir -p $(ASAN_BUILD_DIR)
	$(CXX) -c -o $@ $< $(ASAN_CXXFLAGS)

$(ASAN_BIN_DIR)/fwdd: $(ASAN_BUILD_DIR)/fwdd.o $(ASAN_BUILD_DIR)/mesh.o $(ASAN_BUILD_DIR)/util.o $(ASAN_BUILD_DIR)/logger.o
	@mkdir -p $(ASAN_BIN_DIR)
	$(CXX) -o $@ $^ $(ASAN_CXXFLAGS)

$(ASAN_BIN_DIR)/fwd: $(ASAN_BUILD_DIR)/fwd.o $(ASAN_BUILD_DIR)/util.o $(ASAN_BUILD_DIR)/logger.o
	@mkdir -p $(ASAN_BIN_DIR)
	$(CXX) -o $@ $^ $(ASAN_CXXFLAGS)

asan: $(ASAN_BIN_DIR)/fwdd $(ASAN_BIN_DIR)/fwd

asan-test: asan
	ASAN_BIN=$(ASAN_BIN_DIR) ./tests/test_asan.sh

# macOS leak check using leaks(1)
LEAKS_CXXFLAGS = -std=c++11 -Wall -Wextra -pthread -I./src -g -O0
LEAKS_BUILD_DIR = build_leaks
LEAKS_BIN_DIR = bin_leaks

$(LEAKS_BUILD_DIR)/%.o: src/%.cc $(DEPS)
	@mkdir -p $(LEAKS_BUILD_DIR)
	$(CXX) -c -o $@ $< $(LEAKS_CXXFLAGS)

$(LEAKS_BIN_DIR)/fwdd: $(LEAKS_BUILD_DIR)/fwdd.o $(LEAKS_BUILD_DIR)/mesh.o $(LEAKS_BUILD_DIR)/util.o $(LEAKS_BUILD_DIR)/logger.o
	@mkdir -p $(LEAKS_BIN_DIR)
	$(CXX) -o $@ $^ $(LEAKS_CXXFLAGS)

$(LEAKS_BIN_DIR)/fwd: $(LEAKS_BUILD_DIR)/fwd.o $(LEAKS_BUILD_DIR)/util.o $(LEAKS_BUILD_DIR)/logger.o
	@mkdir -p $(LEAKS_BIN_DIR)
	$(CXX) -o $@ $^ $(LEAKS_CXXFLAGS)

leaks-check: $(LEAKS_BIN_DIR)/fwdd $(LEAKS_BIN_DIR)/fwd
	LEAKS_BIN=$(LEAKS_BIN_DIR) ./tests/test_leaks.sh

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) $(ASAN_BUILD_DIR) $(ASAN_BIN_DIR) $(LEAKS_BUILD_DIR) $(LEAKS_BIN_DIR)

.PHONY: all unit-test test clean asan asan-test leaks-check
