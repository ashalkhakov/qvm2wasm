# Makefile for the example QVM and its WebAssembly hosts.
#
# The example is a classic Quake 3 style module (g_main.c + bg_lib.c +
# g_syscalls.asm) compiled with the real LCC toolchain (q3lcc/q3asm from
# ioquake3), so the resulting bytecode.qvm is exactly what the Quake 3
# engine consumes. A prebuilt bytecode.qvm is checked in, so q3lcc/q3asm
# are only needed to rebuild it (target: qvm).
#
# Targets:
#   make test     - compile bytecode.qvm to wasm and run it in the
#                   reference interpreter, under Node.js (../run.js) and
#                   under the pure C wasm3 host (host.c), then diff the
#                   three outputs
#   make host     - build the pure C host (downloads wasm3 sources)
#   make qvm      - rebuild bytecode.qvm (needs q3lcc/q3asm in PATH,
#                   build them from ioquake3's code/tools)
#   make clean

QVM2WASM = ../qvm2wasm

# --- rebuilding the bytecode with the real LCC toolchain ----------------

LCC    = q3lcc
Q3ASM  = q3asm
CFLAGS_QVM = -DQ3_VM -S -Wf-target=bytecode -Wf-g

QVM_SRCS = g_main.c bg_lib.c
QVM_ASMS = $(addprefix build/,$(QVM_SRCS:%.c=%.asm))

qvm: $(QVM_ASMS) g_syscalls.asm bytecode.q3asm
	$(Q3ASM) -vq3 -f bytecode

build/%.asm: %.c bg_lib.h
	@mkdir -p build
	$(LCC) $(CFLAGS_QVM) -o $@ $<

# --- the pure C wasm3 host ----------------------------------------------

WASM3_VERSION = 0.5.0
WASM3_DIR     = wasm3-$(WASM3_VERSION)
WASM3_URL     = https://github.com/wasm3/wasm3/archive/refs/tags/v$(WASM3_VERSION).tar.gz

$(WASM3_DIR)/source/wasm3.h:
	curl -sL $(WASM3_URL) | tar xz

host: host.c $(WASM3_DIR)/source/wasm3.h
	$(CC) -O2 -w -I$(WASM3_DIR)/source -o $@ host.c $(WASM3_DIR)/source/*.c -lm

# --- end-to-end test ------------------------------------------------------

$(QVM2WASM):
	$(MAKE) -C .. qvm2wasm

bytecode.wasm: bytecode.qvm $(QVM2WASM)
	$(QVM2WASM) bytecode.qvm -o bytecode.wasm

test: bytecode.wasm host
	$(QVM2WASM) -r bytecode.qvm 0 | sed '1,/^Instruction count:/d' > out.interp.txt
	node ../run.js bytecode.wasm 0 > out.node.txt
	./host bytecode.wasm 0 > out.host.txt
	diff -u out.interp.txt out.node.txt
	diff -u out.interp.txt out.host.txt
	@echo "PASS: interpreter, Node.js and C (wasm3) host output match"

clean:
	-rm -rf build bytecode.wasm bytecode.map host out.*.txt $(WASM3_DIR)

.PHONY: qvm test clean
