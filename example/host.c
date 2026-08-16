/*
 * host.c -- pure C host for WebAssembly modules produced by qvm2wasm,
 * built on the wasm3 interpreter (https://github.com/wasm3/wasm3), a
 * WebAssembly runtime written in plain C.
 *
 * This demonstrates how a C engine (e.g. Quake 3) can run the compiled
 * modules without any JavaScript: it implements the same `env.syscall`
 * import and the same demo syscall table as the native interpreter
 * driver (see SystemCalls in ../main.c) and the Node.js host (../run.js):
 *   -1 print_int, -2 print_string, -3 memset, -4 memcpy, -5 error
 *
 * Usage: host <file.wasm> [vmMain args...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "wasm3.h"
#include "m3_env.h"

#define MAX_VMMAIN_ARGS 13

static void fatal(const char* msg)
{
    fprintf(stderr, "host: %s\n", msg);
    exit(1);
}

static void check(M3Result result, const char* what)
{
    if (result)
    {
        fprintf(stderr, "host: %s: %s\n", what, result);
        exit(1);
    }
}

static int rangeValid(uint32_t addr, uint32_t len, uint32_t memSize)
{
    return addr > 0 && addr + len >= addr && addr + len <= memSize;
}

/* env.syscall (i32 argsPtr) -> i32
 * argsPtr points at the syscall argument block in VM memory:
 * args[0] = syscall number, args[1..] = arguments. */
m3ApiRawFunction(Syscall)
{
    m3ApiReturnType(int32_t);
    m3ApiGetArg(uint32_t, argsPtr);

    uint32_t memSize = 0;
    uint8_t* mem     = m3_GetMemory(runtime, &memSize, 0);

    if (!mem || !rangeValid(argsPtr, 4 * 4, memSize))
    {
        m3ApiReturn(0);
    }

    int32_t args[4];
    memcpy(args, mem + argsPtr, sizeof(args));

    const int32_t  id   = -1 - args[0];
    const uint32_t a1   = (uint32_t)args[1];
    const uint32_t a2   = (uint32_t)args[2];
    const uint32_t a3   = (uint32_t)args[3];

    switch (id)
    {
    case -1: /* print_int */
        printf("%d", args[1]);
        m3ApiReturn(0);
    case -2: /* print_string */
        if (a1 < memSize)
        {
            fwrite(mem + a1, 1, strnlen((const char*)(mem + a1), memSize - a1),
                   stdout);
        }
        m3ApiReturn(0);
    case -3: /* memset */
        if (rangeValid(a1, a3, memSize))
        {
            memset(mem + a1, args[2], a3);
        }
        m3ApiReturn(args[1]);
    case -4: /* memcpy */
        if (rangeValid(a1, a3, memSize) && rangeValid(a2, a3, memSize))
        {
            memmove(mem + a1, mem + a2, a3);
        }
        m3ApiReturn(args[1]);
    case -5: /* error */
        if (a1 < memSize)
        {
            fwrite(mem + a1, 1, strnlen((const char*)(mem + a1), memSize - a1),
                   stderr);
        }
        m3ApiReturn(0);
    default:
        fprintf(stderr, "Bad system call: %d\n", id);
        m3ApiReturn(0);
    }
}

static uint8_t* loadFile(const char* path, uint32_t* size)
{
    FILE* f = fopen(path, "rb");
    if (!f)
    {
        return NULL;
    }
    fseek(f, 0L, SEEK_END);
    long sz = ftell(f);
    if (sz < 1)
    {
        fclose(f);
        return NULL;
    }
    rewind(f);
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf || fread(buf, 1, sz, f) != (size_t)sz)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (uint32_t)sz;
    return buf;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fatal("usage: host <file.wasm> [vmMain args...]");
    }

    uint32_t wasmSize;
    uint8_t* wasm = loadFile(argv[1], &wasmSize);
    if (!wasm)
    {
        fatal("failed to read wasm file");
    }

    IM3Environment env = m3_NewEnvironment();
    if (!env)
    {
        fatal("m3_NewEnvironment failed");
    }
    IM3Runtime runtime = m3_NewRuntime(env, 64 * 1024, NULL);
    if (!runtime)
    {
        fatal("m3_NewRuntime failed");
    }

    IM3Module module;
    check(m3_ParseModule(env, &module, wasm, wasmSize), "parse");
    check(m3_LoadModule(runtime, module), "load");
    check(m3_LinkRawFunction(module, "env", "syscall", "i(i)", &Syscall),
          "link env.syscall");

    IM3Function vmMain;
    check(m3_FindFunction(&vmMain, runtime, "vmMain"), "find vmMain");

    int32_t     args[MAX_VMMAIN_ARGS] = { 0 };
    const void* argPtrs[MAX_VMMAIN_ARGS];
    int         i;
    for (i = 0; i < MAX_VMMAIN_ARGS; i++)
    {
        if (i + 2 < argc)
        {
            args[i] = atoi(argv[i + 2]);
        }
        argPtrs[i] = &args[i];
    }

    check(m3_Call(vmMain, MAX_VMMAIN_ARGS, argPtrs), "call vmMain");

    int32_t     ret        = 0;
    const void* retPtrs[1] = { &ret };
    check(m3_GetResults(vmMain, 1, retPtrs), "get result");

    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(env);
    free(wasm);
    return ret;
}
