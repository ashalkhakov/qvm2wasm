# qvm2wasm: Quake 3 QVM bytecode to WebAssembly translator
#
# Targets:
#   make            - build the qvm2wasm compiler
#   make test       - end-to-end test: run a generated .qvm in the
#                     interpreter and as WebAssembly (node) and diff
#   make clean

TARGET  = qvm2wasm
CC      = gcc
CFLAGS  = -g -Wall

# NOTE: ir.c and domlt.c are unfinished CFG/dominator experiments and are
# not part of the build.
SOURCES = main.c vm.c wasm.c
OBJECTS = $(SOURCES:.c=.o)
HEADERS = vm.h wasm.h

.PHONY: default all clean test

default: $(TARGET)
all: default

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -Wall -o $@

tests/mktest: tests/mktest.c vm.h
	$(CC) $(CFLAGS) -I. tests/mktest.c -o tests/mktest

test: $(TARGET) tests/mktest
	./tests/mktest tests/test.qvm
	./$(TARGET) -r tests/test.qvm 100 7 | sed -n '/=== BEGIN ===/,/=== END ===/p' > tests/out.interp.txt
	./$(TARGET) tests/test.qvm -o tests/test.wasm
	node run.js tests/test.wasm 100 7 | sed -n '/=== BEGIN ===/,/=== END ===/p' > tests/out.wasm.txt
	diff -u tests/out.interp.txt tests/out.wasm.txt
	@echo "PASS: interpreter and WebAssembly output match"

clean:
	-rm -f *.o
	-rm -f $(TARGET)
	-rm -f tests/mktest tests/test.qvm tests/test.wasm tests/out.*.txt
