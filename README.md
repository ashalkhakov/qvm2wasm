# qvm2wasm

A Quake 3 virtual machine (`.qvm`) to WebAssembly compiler, so Quake 3 VMs
can run on WASM infrastructure (browsers, node, any wasm runtime).

Dependencies are kept to the bare minimum: the compiler is plain C with no
libraries beyond libc, and it emits a binary `.wasm` module directly — no
wabt, LLVM, or emscripten needed. Node.js (or any wasm host) is only needed
to *run* the produced module.

## Building

```sh
make
```

This produces the `qvm2wasm` binary.

## Usage

```sh
# compile bytecode.qvm to bytecode.wasm
./qvm2wasm bytecode.qvm

# choose the output file name
./qvm2wasm bytecode.qvm -o out.wasm

# run the .qvm in the bundled reference interpreter instead (for comparison)
./qvm2wasm -r bytecode.qvm [vmMain args...]

# run the compiled module with the bundled Node.js host
node run.js bytecode.wasm [vmMain args...]
```

## Testing

```sh
make test
```

This generates a test `.qvm` exercising the whole instruction set
(`tests/mktest.c`), runs it both in the reference interpreter and as
WebAssembly under node, and verifies the outputs are identical.

## Produced module interface

The generated module:

* exports `vmMain` (`(i32 x13) -> i32`, same signature as Q3's `vmMain`)
  and `memory` (the VM's data+lit+bss and program stack);
* imports one host function `env.syscall (i32) -> i32`. The parameter is
  the address in `memory` of the syscall argument block: `args[0]` is the
  syscall number (`-1 - trap`), `args[1..]` are the call arguments, each a
  32-bit little-endian word. This mirrors the q3vm interpreter's syscall
  convention, so an engine embedding these modules implements its
  `g_syscalls.asm` traps in the `syscall` import (see `run.js` for a small
  example host implementing the demo syscalls used by `main.c`).

## How the translation works

* The QVM `data`+`lit`+`bss` segments are placed at offset 0 of one linear
  memory, rounded up to a power of two so every load/store is masked —
  the same sandboxing scheme as the q3vm interpreter. The program stack
  lives at the top of that region; the stack pointer is a mutable wasm
  global.
* Every QVM function (an `OP_ENTER` up to the next `OP_ENTER`) becomes a
  wasm function of type `() -> i32`. The QVM operand stack is mapped onto
  wasm locals using a static stack-depth analysis.
* Control flow uses a dispatch loop (`loop` + nested `block`s + `br_table`
  indexed by a label local), which supports arbitrary jumps, including
  computed `OP_JUMP` used for switch jump tables.
* `OP_CALL` calls through a `funcref` table indexed by QVM instruction
  index, so calls through function pointers work; negative targets are
  routed to the imported host `syscall`.

## Repository layout

* `wasm.c`, `wasm.h` — the QVM → WASM translator
* `vm.c`, `vm.h` — the reference q3vm interpreter (used by `-r`)
* `main.c` — command line driver
* `run.js` — minimal Node.js host for the produced modules
* `tests/mktest.c` — test bytecode generator
* `ir.c`, `domlt.c` — unfinished CFG/dominator-tree experiments, not part
  of the build
