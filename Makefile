CLANG    ?= clang
CXX      ?= g++
BPFTOOL  ?= bpftool
LIBBPF_A ?= /tmp/bpftool/src/libbpf/libbpf.a
LIBBPF_INC ?= /tmp/bpftool/src/libbpf/include

all: engine/engine tracer/kslat

engine/engine: engine/engine.cpp engine/order_book.hpp engine/spsc.hpp engine/net.hpp engine/feeds.hpp
	$(CXX) -O2 -std=c++20 -march=native -pthread -Wall -o $@ engine/engine.cpp -lssl -lcrypto

tracer/vmlinux.h:
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

tracer/kslat.bpf.o: tracer/kslat.bpf.c tracer/kslat.h tracer/vmlinux.h
	$(CLANG) -O2 -g -target bpf -D__TARGET_ARCH_x86 -I$(LIBBPF_INC) -c $< -o $@

tracer/kslat.skel.h: tracer/kslat.bpf.o
	$(BPFTOOL) gen skeleton $< > $@

tracer/kslat: tracer/kslat.c tracer/kslat.skel.h tracer/kslat.h
	$(CC) -O2 -Wall -I$(LIBBPF_INC) -Itracer -o $@ tracer/kslat.c $(LIBBPF_A) -lelf -lz

clean:
	rm -f engine/engine tracer/kslat tracer/*.o tracer/kslat.skel.h

.PHONY: all clean
