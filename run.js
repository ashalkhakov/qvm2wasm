#!/usr/bin/env node
/*
 * run.js -- host runtime for WebAssembly modules produced by qvm2wasm.
 *
 * Implements the same example syscalls as the native interpreter driver
 * (see SystemCalls in main.c):
 *   -1 print_int, -2 print_string, -3 memset, -4 memcpy, -5 error
 *
 * Usage: node run.js <file.wasm> [vmMain args...]
 */
"use strict";

const fs = require("fs");

function fatal(msg) {
    process.stderr.write("run.js: " + msg + "\n");
    process.exit(1);
}

if (process.argv.length < 3) {
    fatal("usage: node run.js <file.wasm> [vmMain args...]");
}

const wasmFile = process.argv[2];
const vmArgs = new Array(13).fill(0);
for (let i = 0; i < 13 && i + 3 < process.argv.length; i++) {
    vmArgs[i] = process.argv[i + 3] | 0;
}

let memory = null; // set after instantiation

function mem() {
    return new DataView(memory.buffer);
}

function readString(addr) {
    const bytes = new Uint8Array(memory.buffer);
    let end = addr;
    while (end < bytes.length && bytes[end] !== 0) {
        end++;
    }
    return Buffer.from(bytes.subarray(addr, end)).toString("latin1");
}

function rangeValid(addr, len) {
    return addr > 0 && len >= 0 && addr + len <= memory.buffer.byteLength;
}

/* argsPtr points at the syscall argument block in VM memory:
   args[0] = syscall number, args[1..] = arguments */
function syscall(argsPtr) {
    const m = mem();
    const arg = (i) => m.getInt32(argsPtr + 4 * i, true);
    const id = -1 - arg(0);

    switch (id) {
        case -1: /* print_int */
            process.stdout.write(String(arg(1)));
            return 0;
        case -2: /* print_string */
            process.stdout.write(readString(arg(1)));
            return 0;
        case -3: /* memset */
            if (rangeValid(arg(1), arg(3))) {
                new Uint8Array(memory.buffer).fill(arg(2) & 0xff, arg(1),
                                                   arg(1) + arg(3));
            }
            return arg(1);
        case -4: /* memcpy */
            if (rangeValid(arg(1), arg(3)) && rangeValid(arg(2), arg(3))) {
                new Uint8Array(memory.buffer).copyWithin(arg(1), arg(2),
                                                         arg(2) + arg(3));
            }
            return arg(1);
        case -5: /* error */
            process.stderr.write(readString(arg(1)));
            return 0;
        default:
            process.stderr.write("Bad system call: " + id + "\n");
            return 0;
    }
}

const bytes = fs.readFileSync(wasmFile);
WebAssembly.instantiate(bytes, { env: { syscall } })
    .then(({ instance }) => {
        memory = instance.exports.memory;
        const ret = instance.exports.vmMain(...vmArgs);
        process.exitCode = ret | 0;
    })
    .catch((e) => fatal(String(e)));
