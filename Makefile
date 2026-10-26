CXX ?= g++
CXXFLAGS ?= -O3 -std=c++17 -march=native -faligned-new -pthread \
             -DPCRE2_CODE_UNIT_WIDTH=8 -Isrc -Ilogspline
LDLIBS = -lpcre2-8 -ljemalloc -pthread

all: logminify decompress_logminify

logminify: src/logminify.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

decompress_logminify: src/decompress_logminify.cpp
	$(CXX) -O3 -std=c++17 -pthread -Isrc $< -o $@

clean:
	rm -f logminify decompress_logminify

.PHONY: all clean
