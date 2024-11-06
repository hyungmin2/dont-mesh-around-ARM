CC:= g++
# HOSTNAME := $(shell hostname|awk '{print toupper($$0)'})
CFLAGS:= -std=c++20 -O3 -D_POSIX_SOURCE -D_GNU_SOURCE #-D$(HOSTNAME)
CFLAGSO1:= -O1 -D_POSIX_SOURCE -D_GNU_SOURCE #-D$(HOSTNAME)
LIBS:= -lpthread -lrt

all: obj bin out transmitter transmitter-rand-bits receiver-no-ev setup-sem cleanup-sem

transmitter: obj/transmitter.o ../util/util.o ../util/pfn_util.o
	$(CC) -o bin/$@ $^ $(LIBS)

transmitter-rand-bits: obj/transmitter-rand-bits.o ../util/util.o ../util/pfn_util.o
	$(CC) -o bin/$@ $^ $(LIBS)

receiver-no-ev: obj/receiver-no-ev.o ../util/util.o ../util/pfn_util.o
	$(CC) -o bin/$@ $^ $(LIBS)

setup-sem: obj/setup-sem.o
	$(CC) -o bin/$@ $^ $(LIBS)

cleanup-sem: obj/cleanup-sem.o
	$(CC) -o bin/$@ $^ $(LIBS)

obj/transmitter-rand-bits.o: transmitter.cpp
	$(CC) -c $(CFLAGS) -DRANDOM_PATTERN -o $@ $<

obj/%.o: %.cpp
	$(CC) -c $(CFLAGS) -o $@ $<

obj:
	mkdir -p $@

bin:
	mkdir -p $@

out:
	mkdir -p $@

clean:
	rm -rf bin obj

.PHONY: all clean
