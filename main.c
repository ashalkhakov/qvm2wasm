#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"
#include "wasm.h"

intptr_t SystemCalls(vm_t* vm, intptr_t* args)
{
    const int id = -1 - args[0];

    switch (id)
    {
    case -1: /* print_int */
        return printf("%d", (int)args[1]);
    case -2: /* print_string */
        return printf("%s", (const char*)VMA(1, vm));

    case -3: /* MEMSET */
        if (VM_MemoryRangeValid(args[1] /*addr*/, args[3] /*len*/, vm) == 0)
        {
            memset(VMA(1, vm), args[2], args[3]);
        }
        return args[1];

    case -4: /* MEMCPY */
        if (VM_MemoryRangeValid(args[1] /*addr*/, args[3] /*len*/, vm) == 0 &&
            VM_MemoryRangeValid(args[2] /*addr*/, args[3] /*len*/, vm) == 0)
        {
            memcpy(VMA(1, vm), VMA(2, vm), args[3]);
        }
        return args[1];

    case -5: /* ERROR */
        return fprintf(stderr, "%s", (const char*)VMA(1, vm));

    default:
        fprintf(stderr, "Bad system call: %i\n", id);
    }
    return 0;
}

uint8_t* loadImage(const char* filepath, int* size)
{
    FILE*    f;            /* bytecode input file */
    uint8_t* image = NULL; /* bytecode buffer */
    int      sz;           /* bytecode file size */

    *size = 0;
    f     = fopen(filepath, "rb");
    if (!f)
    {
        fprintf(stderr, "Failed to open file %s.\n", filepath);
        return NULL;
    }
    /* calculate file size */
    fseek(f, 0L, SEEK_END);
    sz = ftell(f);
    if (sz < 1)
    {
        fclose(f);
        return NULL;
    }
    rewind(f);

    image = (uint8_t*)malloc(sz);
    if (!image)
    {
        fclose(f);
        return NULL;
    }

    if (fread(image, 1, sz, f) != (size_t)sz)
    {
        free(image);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *size = sz;
    return image;
}

static void usage(void)
{
    printf("qvm2wasm - Quake 3 QVM bytecode to WebAssembly translator\n"
           "\n"
           "Usage:\n"
           "  qvm2wasm <file.qvm> [-o <file.wasm>]   compile to WebAssembly\n"
           "  qvm2wasm -r <file.qvm> [args...]       run in the interpreter\n"
           "\n"
           "The default output file is the input with a .wasm extension.\n"
           "Run the compiled module with: node run.js <file.wasm> [args...]\n");
}

static int runInterpreter(const char* filepath, int argc, char** argv)
{
    vm_t     vm;
    int      imageSize;
    int      args[MAX_VMMAIN_ARGS] = { 0 };
    int      i;
    int      retVal = -1;
    uint8_t* image  = loadImage(filepath, &imageSize);

    if (!image)
    {
        return -1;
    }
    for (i = 0; i < argc && i < MAX_VMMAIN_ARGS; i++)
    {
        args[i] = atoi(argv[i]);
    }
    if (VM_Create(&vm, filepath, image, imageSize, SystemCalls) == 0)
    {
        retVal = VM_Call(&vm, args[0], args[1], args[2], args[3], args[4],
                         args[5], args[6], args[7], args[8], args[9], args[10],
                         args[11], args[12]);
        VM_Free(&vm);
    }
    free(image);
    return retVal;
}

static int compileToWasm(const char* inPath, const char* outPath)
{
    int      imageSize;
    uint8_t* image = loadImage(inPath, &imageSize);
    uint8_t* wasm  = NULL;
    int      wasmLen;
    char     defaultOut[1024];

    if (!image)
    {
        return -1;
    }

    if (!outPath)
    {
        /* replace the extension with .wasm */
        const char* dot = strrchr(inPath, '.');
        size_t      n   = dot ? (size_t)(dot - inPath) : strlen(inPath);
        if (n > sizeof(defaultOut) - 6)
        {
            n = sizeof(defaultOut) - 6;
        }
        memcpy(defaultOut, inPath, n);
        memcpy(defaultOut + n, ".wasm", 6);
        outPath = defaultOut;
    }

    if (QVM2WASM_Compile(image, imageSize, &wasm, &wasmLen) != 0)
    {
        free(image);
        return -1;
    }
    free(image);

    FILE* f = fopen(outPath, "wb");
    if (!f)
    {
        fprintf(stderr, "Failed to open output file %s.\n", outPath);
        free(wasm);
        return -1;
    }
    if (fwrite(wasm, 1, wasmLen, f) != (size_t)wasmLen)
    {
        fprintf(stderr, "Failed to write %s.\n", outPath);
        fclose(f);
        free(wasm);
        return -1;
    }
    fclose(f);
    free(wasm);
    printf("Wrote %s (%d bytes)\n", outPath, wasmLen);
    return 0;
}

int main(int argc, char** argv)
{
    const char* inPath  = NULL;
    const char* outPath = NULL;
    int         run     = 0;
    int         i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-r") == 0)
        {
            run = 1;
        }
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
        {
            outPath = argv[++i];
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            usage();
            return 0;
        }
        else if (!inPath)
        {
            inPath = argv[i];
        }
        else if (run)
        {
            /* remaining args are vmMain arguments */
            break;
        }
        else
        {
            usage();
            return 1;
        }
    }

    if (!inPath)
    {
        usage();
        return 1;
    }

    if (run)
    {
        return runInterpreter(inPath, argc - i, argv + i);
    }
    return compileToWasm(inPath, outPath);
}
