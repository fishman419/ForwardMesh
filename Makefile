CXX=g++
CXXFLAGS=-std=c++11 -I.
DEPS = protocol.h util.h logger.h

BUILD_DIR = build
BIN_DIR = bin

OBJ_FWDD = $(BUILD_DIR)/fwdd.o $(BUILD_DIR)/util.o $(BUILD_DIR)/logger.o
OBJ_FWD = $(BUILD_DIR)/fwd.o $(BUILD_DIR)/util.o $(BUILD_DIR)/logger.o
TARGET = $(BIN_DIR)/fwdd $(BIN_DIR)/fwd

$(shell mkdir -p $(BUILD_DIR) $(BIN_DIR))

$(BUILD_DIR)/%.o: %.cc $(DEPS)
	$(CXX) -c -o $@ $< $(CXXFLAGS)

all: $(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

$(BIN_DIR)/fwdd: $(OBJ_FWDD)
	$(CXX) -o $@ $^ $(CXXFLAGS)

$(BIN_DIR)/fwd: $(OBJ_FWD)
	$(CXX) -o $@ $^ $(CXXFLAGS)

.PHONY: all clean

