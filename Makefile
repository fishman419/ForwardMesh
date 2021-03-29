CXX=g++
CXXFLAGS=-std=c++11 -I.
DEPS = protocol.h util.h
OBJ_FWDD = fwdd.o util.o
OBJ_FWD = fwd.o util.o
TARGET = fwdd fwd

%.o: %.c $(DEPS)
	$(CXX) -c -o $@ $< $(CXXFLAGS)

all: $(TARGET)

clean:
	rm *.o
	rm $(TARGET)

fwdd: $(OBJ_FWDD)
	$(CXX) -o $@ $^ $(CXXFLAGS)

fwd: $(OBJ_FWD)
	$(CXX) -o $@ $^ $(CXXFLAGS)

