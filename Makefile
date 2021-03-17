CXX=g++
CXXFLAGS=-I.
DEPS = protocol.h
OBJ_FWDD = fwdd.o
OBJ_FWD = fwd.o

%.o: %.c $(DEPS)
	$(CXX) -c -o $@ $< $(CXXFLAGS)

all: fwdd fwd

fwdd: $(OBJ_FWDD)
	$(CXX) -o $@ $^ $(CXXFLAGS)

fwd: $(OBJ_FWD)
	$(CXX) -o $@ $^ $(CXXFLAGS)
