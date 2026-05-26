# Makefile — Mini CI Grading Server

CC      = gcc
CLANG   = clang
CFLAGS  = -O2 -Wall -g -Wno-stringop-truncation -Wno-unused-result
BPF_ARCH = x86

all: vmlinux.h monitor.skel.h server_bin client_bin tui_bin

server_bin: server/server.c server/server_ctx.c monitor.skel.h
	$(CC) $(CFLAGS) -I. -o $@ server/server.c server/server_ctx.c -lbpf -lelf -lz -lncursesw -lpthread -lseccomp -lcap

monitor.bpf.o: server/monitor.bpf.c vmlinux.h
	$(CLANG) -O2 -g -target bpf -D__TARGET_ARCH_$(BPF_ARCH) -I. -c $< -o $@

monitor.skel.h: monitor.bpf.o
	bpftool gen skeleton $< > $@ || ./bpftool gen skeleton $< > $@

vmlinux.h:
	@echo "Generating vmlinux.h..."
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h || \
	(wget -qO bpftool.tar.gz https://github.com/libbpf/bpftool/releases/download/v7.4.0/bpftool-v7.4.0-amd64.tar.gz && \
	tar -xzf bpftool.tar.gz && \
	./bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h)

client_bin: client/client.c
	$(CC) $(CFLAGS) -o $@ $< -lncursesw

tui_bin: minici-tui.c
	$(CC) $(CFLAGS) -o minici-tui minici-tui.c -lncursesw -lpthread

clean:
	rm -f server_bin client_bin minici-tui monitor.bpf.o monitor.skel.h vmlinux.h bpftool*
