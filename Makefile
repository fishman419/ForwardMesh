CXX=g++
CXXFLAGS=-std=c++11 -I.
DEPS = protocol.h util.h
OBJ_FWDD = fwdd.o util.o
OBJ_FWD = fwd.o util.o

%.o: %.c $(DEPS)
	$(CXX) -c -o $@ $< $(CXXFLAGS)

all: fwdd fwd

fwdd: $(OBJ_FWDD)
	$(CXX) -o $@ $^ $(CXXFLAGS)

fwd: $(OBJ_FWD)
	$(CXX) -o $@ $^ $(CXXFLAGS)
