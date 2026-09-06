CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -pthread

.PHONY: test sanitize tsan clean

test: build/test_mpmc
	./build/test_mpmc

build/test_mpmc: test_mpmc.cpp mpmc_queue.hpp
	mkdir -p build
	$(CXX) $(CXXFLAGS) test_mpmc.cpp -o $@

sanitize:
	$(MAKE) clean
	$(MAKE) CXXFLAGS='$(CXXFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' test

tsan:
	$(MAKE) clean
	$(MAKE) CXXFLAGS='$(CXXFLAGS) -O1 -g -fno-omit-frame-pointer -fsanitize=thread' test

clean:
	rm -rf build
