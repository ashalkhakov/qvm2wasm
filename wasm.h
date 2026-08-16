#ifndef __WASM_H__
#define __WASM_H__

#include <stdint.h>

/**
 * Compile a Quake 3 .qvm bytecode image into a WebAssembly binary module.
 *
 * The produced module:
 *  - imports one host function: (import "env" "syscall" (func (param i32) (result i32)))
 *    The i32 parameter is the address (in the exported linear memory) of the
 *    syscall argument block: args[0] is the syscall number (-1 - callnum),
 *    args[1..] are the call arguments (32-bit little endian each).
 *  - exports "vmMain" (13 x i32 params -> i32) and "memory".
 *
 * @param bytecode  Contents of the .qvm file.
 * @param length    Size of the .qvm file in bytes.
 * @param wasmOut   On success, *wasmOut points to a malloc'd buffer with the
 *                  wasm module. Caller must free().
 * @param wasmLen   On success, *wasmLen is the module size in bytes.
 * @return 0 on success, -1 on error (message printed to stderr).
 */
int QVM2WASM_Compile(const uint8_t* bytecode, int length, uint8_t** wasmOut,
                     int* wasmLen);

#endif /* !__WASM_H__ */
