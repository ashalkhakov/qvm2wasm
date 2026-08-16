#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h> // uint8_t ...
#include <assert.h>

#include "vm.h"

#define Com_Printf printf
#define Com_Memcpy memcpy
#define Com_Memset memset

void Com_Error(const char* error)
{
    fprintf(stderr, "Err: %s\n", error);
    exit(1);
}
void* Com_malloc(size_t size, vm_t* vm, int type)
{
    (void)vm; /* simple malloc, we don't care about the vm */
    (void)type; /* we don't care what the VM wants to do with the memory */
    return malloc(size); /* just allocate the memory and return it */
}
void Com_free(void* p, vm_t* vm, int type)
{
    (void)vm;
    (void)type;
    free(p);
}



static void Q_strncpyz(char* dest, const char* src, int destsize)
{
    if (!dest || !src || destsize < 1)
    {
        return;
    }
    strncpy(dest, src, destsize - 1);
    dest[destsize - 1] = 0;
}

static int LittleEndianToHost(const uint8_t b[4])
{
    return (b[0] << 0) | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
}

#define ARRAY_LEN(x) (sizeof(x) / sizeof(*(x)))
#define PAD(base, alignment) (((base) + (alignment)-1) & ~((alignment)-1))
#define PADLEN(base, alignment) (PAD((base), (alignment)) - (base))
#define PADP(base, alignment) ((void*)PAD((intptr_t)(base), (alignment)))
#define Q_ftol(v) ((long)(v))

//---------------------

static int vm_debugLevel; /**< 0: be quiet, 1: debug msgs, 2: print op codes */

#ifdef DEBUG_VM
/** Table to convert op codes to readable names */
const static char* opnames[OPCODE_TABLE_SIZE] = {
    "OP_UNDEF",  "OP_IGNORE", "OP_BREAK",  "OP_ENTER", "OP_LEAVE",
    "OP_CALL",   "OP_PUSH",   "OP_POP",    "OP_CONST", "OP_LOCAL",
    "OP_JUMP",   "OP_EQ",     "OP_NE",     "OP_LTI",   "OP_LEI",
    "OP_GTI",    "OP_GEI",    "OP_LTU",    "OP_LEU",   "OP_GTU",
    "OP_GEU",    "OP_EQF",    "OP_NEF",    "OP_LTF",   "OP_LEF",
    "OP_GTF",    "OP_GEF",    "OP_LOAD1",  "OP_LOAD2", "OP_LOAD4",
    "OP_STORE1", "OP_STORE2", "OP_STORE4", "OP_ARG",   "OP_BLOCK_COPY",
    "OP_SEX8",   "OP_SEX16",  "OP_NEGI",   "OP_ADD",   "OP_SUB",
    "OP_DIVI",   "OP_DIVU",   "OP_MODI",   "OP_MODU",  "OP_MULI",
    "OP_MULU",   "OP_BAND",   "OP_BOR",    "OP_BXOR",  "OP_BCOM",
    "OP_LSH",    "OP_RSHI",   "OP_RSHU",   "OP_NEGF",  "OP_ADDF",
    "OP_SUBF",   "OP_DIVF",   "OP_MULF",   "OP_CVIF",  "OP_CVFI",
    "OP_UNDEF",  "OP_UNDEF",  "OP_UNDEF",  "OP_UNDEF",
};
#endif

#ifdef DEBUG_VM
#include <stdio.h>           /* fopen to read symbols */
#include <stdlib.h>          /* qsort */
#define MAX_TOKEN_CHARS 1024 /**< max length of an individual token */
/* WARNING: DEBUG_VM is not thread safe */
static char com_token[MAX_TOKEN_CHARS]; /**< helper for COM_Parse */
static int  com_lines;                  /**< helper for COM_Parse */
static int  com_tokenline;              /**< helper for COM_Parse */
static int ParseHex(const char* text);  /**< helper for VM_LoadSymbols */
static void COM_StripExtension(const char* in,
                               char* out); /**< helper for VM_LoadSymbols */
static char* VM_Indent(vm_t* vm);
/** For profiling, find the symbol behind this value */
static vmSymbol_t* VM_ValueToFunctionSymbol(vm_t* vm, int value);
static const char* VM_ValueToSymbol(vm_t* vm, int value);
/** Load a .map file for the virtual machine. The .map file
 * should have the same path as the .qvm file. */
static void VM_LoadSymbols(vm_t* vm);
/** Load the file into a malloc'd buffer.
 * @param[in] filepath File to load.
 * @return file content in buffer. Call Com_free() to cleanup. */
static uint8_t* loadImage(const char* filepath, int* imageSize);
/** Print a stack trace on OP_ENTER if vm_debugLevel is > 0 */
static void VM_StackTrace(vm_t* vm, int programCounter, int programStack);
#endif

void* VM_ArgPtr(intptr_t vmAddr, vm_t* vm)
{
    if (!vmAddr)
    {
        return NULL;
    }
    if (vm == NULL)
    {
        Com_Error("Invalid VM pointer");
        return NULL;
    }

    return (void*)(vm->dataBase + (vmAddr & vm->dataMask));
}

float VM_IntToFloat(int32_t x)
{
    union {
        float    f;  /**< float IEEE 754 32-bit single */
        int32_t  i;  /**< int32 part */
        uint32_t ui; /**< unsigned int32 part */
    } fi;
    fi.i = x;
    return fi.f;
}

int32_t VM_FloatToInt(float f)
{
    union {
        float    f;  /**< float IEEE 754 32-bit single */
        int32_t  i;  /**< int32 part */
        uint32_t ui; /**< unsigned int32 part */
    } fi;
    fi.f = f;
    return fi.i;
}

int VM_MemoryRangeValid(intptr_t vmAddr, size_t len, const vm_t* vm)
{
    if (!vmAddr || !vm)
    {
        return -1;
    }
    const unsigned dest     = vmAddr;
    const unsigned dataMask = vm->dataMask;
    if ((dest & dataMask) != dest || ((dest + len) & dataMask) != dest + len)
    {
        Com_Error("Memory access out of range");
        return -1;
    }
    else
    {
        return 0;
    }
}

static void VM_BlockCopy(unsigned int dest, unsigned int src, size_t n,
                         vm_t* vm)
{
    unsigned int dataMask = vm->dataMask;

    if ((dest & dataMask) != dest || (src & dataMask) != src ||
        ((dest + n) & dataMask) != dest + n ||
        ((src + n) & dataMask) != src + n)
    {
        Com_Error("OP_BLOCK_COPY out of range");
        return;
    }

    Com_Memcpy(vm->dataBase + dest, vm->dataBase + src, n);
}


/*
==============
VM_CallInterpreted

Upon a system call, the stack will look like:

sp+32   parm1
sp+28   parm0
sp+24   return stack
sp+20   return address
sp+16   local1
sp+14   local0
sp+12   arg1
sp+8    arg0
sp+4    return stack
sp      return address

An interpreted function will immediately execute
an OP_ENTER instruction, which will subtract space for
locals from sp
==============
*/

static int VM_CallInterpreted(vm_t* vm, int* args)
{
    uint8_t  stack[OPSTACK_SIZE + 15];
    int*     opStack;
    uint8_t  opStackOfs;
    int      programCounter;
    int      programStack;
    int      stackOnEntry;
    uint8_t* image;
    int*     codeImage;
    int      v1;
    int      dataMask;
    int      arg;
#ifdef DEBUG_VM
    vmSymbol_t* profileSymbol;
#endif

    /* interpret the code */
    vm->currentlyInterpreting = 1;

    /* we might be called recursively, so this might not be the very top */
    programStack = stackOnEntry = vm->programStack;

#ifdef DEBUG_VM
    profileSymbol = VM_ValueToFunctionSymbol(vm, 0);
    /* uncomment this for debugging breakpoints */
    vm->breakFunction = 0;
#endif

    image          = vm->dataBase;
    codeImage      = (int*)vm->codeBase;
    dataMask       = vm->dataMask;
    programCounter = 0;
    programStack -= (8 + 4 * MAX_VMMAIN_ARGS);

    for (arg = 0; arg < MAX_VMMAIN_ARGS; arg++)
    {
        *(int*)&image[programStack + 8 + arg * 4] = args[arg];
    }

    *(int*)&image[programStack + 4] = 0; /* return stack */
    *(int*)&image[programStack] = -1;    /* will terminate the loop on return */

    /* leave a free spot at start of stack so
       that as long as opStack is valid, opStack-1 will
       not corrupt anything */
    opStack    = PADP(stack, 16);
    *opStack   = 0x0000BEEF;
    opStackOfs = 0;

    /* main interpreter loop, will exit when a LEAVE instruction
       grabs the -1 program counter */

    int opcode, r0, r1;
#define r2 codeImage[programCounter]
#define DISPATCH2() goto nextInstruction2
#define DISPATCH() goto nextInstruction

    while (1)
    {
    nextInstruction:
        r0 = opStack[opStackOfs];
        r1 = opStack[(uint8_t)(opStackOfs - 1)];
    nextInstruction2:
        opcode = codeImage[programCounter++];

#ifdef DEBUG_VM
        if ((unsigned)programCounter >= vm->codeLength)
        {
            Com_Error("VM pc out of range");
            return -1;
        }

        if (programStack <= vm->stackBottom)
        {
            Com_Error("VM stack overflow");
            return -1;
        }

        if (programStack & 3)
        {
            Com_Error("VM program stack misaligned");
            return -1;
        }

        if (vm_debugLevel > 1)
        {
            Com_Printf("%s%i %s\n", VM_Indent(vm), opStackOfs,
                       opnames[opcode & OPCODE_TABLE_MASK]);
        }
        profileSymbol->profileCount++;
#endif /* DEBUG_VM */
        switch (opcode)
        {
#ifdef DEBUG_VM
        default: /* fall through */
#endif
        case OP_UNDEF:
            Com_Error("Bad VM instruction");
            return -1;
        case OP_IGNORE:
            DISPATCH2();
        case OP_BREAK:
            vm->breakCount++;
            DISPATCH2();
        case OP_CONST:
            opStackOfs++;
            r1 = r0;
            r0 = opStack[opStackOfs] = r2;

            programCounter += 1;
            DISPATCH2();
        case OP_LOCAL:
            opStackOfs++;
            r1 = r0;
            r0 = opStack[opStackOfs] = r2 + programStack;

            programCounter += 1;
            DISPATCH2();
        case OP_LOAD4:
#ifdef DEBUG_VM
            if (opStack[opStackOfs] & 3)
            {
                Com_Error("OP_LOAD4 misaligned");
                return -1;
            }
#endif
            r0 = opStack[opStackOfs] = *(int*)&image[r0 & dataMask];
            DISPATCH2();
        case OP_LOAD2:
            r0 = opStack[opStackOfs] = *(unsigned short*)&image[r0 & dataMask];
            DISPATCH2();
        case OP_LOAD1:
            r0 = opStack[opStackOfs] = image[r0 & dataMask];
            DISPATCH2();

        case OP_STORE4:
            *(int*)&image[r1 & dataMask] = r0;
            opStackOfs -= 2;
            DISPATCH();
        case OP_STORE2:
            *(short*)&image[r1 & dataMask] = r0;
            opStackOfs -= 2;
            DISPATCH();
        case OP_STORE1:
            image[r1 & dataMask] = r0;
            opStackOfs -= 2;
            DISPATCH();
        case OP_ARG:
            /* single byte offset from programStack */
            *(int*)&image[(codeImage[programCounter] + programStack) &
                          dataMask] = r0;
            opStackOfs--;
            programCounter += 1;
            DISPATCH();
        case OP_BLOCK_COPY:
            VM_BlockCopy(r1, r0, r2, vm);
            programCounter += 1;
            opStackOfs -= 2;
            DISPATCH();
        case OP_CALL:
            /* save current program counter */
            *(int*)&image[programStack] = programCounter;

            /* jump to the location on the stack */
            programCounter = r0;
            opStackOfs--;
            if (programCounter < 0) /* system call */
            {
                int r;
#ifdef DEBUG_VM
                if (vm_debugLevel)
                {
                    Com_Printf("%s%i---> systemcall(%i)\n", VM_Indent(vm),
                               opStackOfs, -1 - programCounter);
                }
#endif
                /* save the stack to allow recursive VM entry */
                vm->programStack = programStack - 4;
#ifdef DEBUG_VM
                int stomped = *(int*)&image[programStack + 4];
#endif
                *(int*)&image[programStack + 4] = -1 - programCounter;

                /* the vm has ints on the stack, we expect
                   pointers so we might have to convert it */
                if (sizeof(intptr_t) != sizeof(int))
                {
                    intptr_t argarr[MAX_VMSYSCALL_ARGS];
                    int*     imagePtr = (int*)&image[programStack];
                    int      i;
                    for (i = 0; i < (int)ARRAY_LEN(argarr); ++i)
                    {
                        argarr[i] = *(++imagePtr);
                    }
                    r = vm->systemCall(vm, argarr);
                }
                else
                {
                    r = vm->systemCall(vm, (intptr_t*)&image[programStack + 4]);
                }

#ifdef DEBUG_VM
                /* this is just our stack frame pointer, only needed
                   for debugging */
                *(int*)&image[programStack + 4] = stomped;
#endif

                /* save return value */
                opStackOfs++;
                opStack[opStackOfs] = r;
                programCounter      = *(int*)&image[programStack];
#ifdef DEBUG_VM
                if (vm_debugLevel)
                {
                    Com_Printf("%s%i<--- %s\n", VM_Indent(vm), opStackOfs,
                               VM_ValueToSymbol(vm, programCounter));
                }
#endif
            }
            else if ((unsigned)programCounter >= (unsigned)vm->instructionCount)
            {
                Com_Error("VM program counter out of range in OP_CALL");
                return -1;
            }
            else
            {
                programCounter = vm->instructionPointers[programCounter];
            }
            DISPATCH();
        /* push and pop are only needed for discarded or bad function return
           values */
        case OP_PUSH:
            opStackOfs++;
            DISPATCH();
        case OP_POP:
            opStackOfs--;
            DISPATCH();
        case OP_ENTER:
            /* get size of stack frame */
            v1 = r2;

            programCounter += 1;
            programStack -= v1;
#ifdef DEBUG_VM
            profileSymbol = VM_ValueToFunctionSymbol(vm, programCounter);
            /* save old stack frame for debugging traces */
            *(int*)&image[programStack + 4] = programStack + v1;
            if (vm_debugLevel)
            {
                Com_Printf("%s%i---> %s\n", VM_Indent(vm), opStackOfs,
                           VM_ValueToSymbol(vm, programCounter - 5));
                if (vm->breakFunction &&
                    programCounter - 5 == vm->breakFunction)
                {
                    /* this is to allow setting breakpoints here in the
                     * debugger */
                    vm->breakCount++;
                    VM_StackTrace(vm, programCounter, programStack);
                }
            }
#endif
            DISPATCH();
        case OP_LEAVE:
            /* remove our stack frame */
            v1 = r2;

            programStack += v1;

            /* grab the saved program counter */
            programCounter = *(int*)&image[programStack];
#ifdef DEBUG_VM
            profileSymbol = VM_ValueToFunctionSymbol(vm, programCounter);
            if (vm_debugLevel)
            {
                Com_Printf("%s%i<--- %s\n", VM_Indent(vm), opStackOfs,
                           VM_ValueToSymbol(vm, programCounter));
            }
#endif
            /* check for leaving the VM */
            if (programCounter == -1)
            {
                goto done;
            }
            else if ((unsigned)programCounter >= (unsigned)vm->codeLength)
            {
                Com_Error("VM program counter out of range in OP_LEAVE");
                return -1;
            }
            DISPATCH();

        /*
           ===================================================================
           BRANCHES
           ===================================================================
           */

        case OP_JUMP:
            if ((unsigned)r0 >= (unsigned)vm->instructionCount)
            {
                Com_Error("VM program counter out of range in OP_JUMP");
                return -1;
            }

            programCounter = vm->instructionPointers[r0];

            opStackOfs--;
            DISPATCH();
        case OP_EQ:
            opStackOfs -= 2;
            if (r1 == r0)
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_NE:
            opStackOfs -= 2;
            if (r1 != r0)
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_LTI:
            opStackOfs -= 2;
            if (r1 < r0)
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_LEI:
            opStackOfs -= 2;
            if (r1 <= r0)
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_GTI:
            opStackOfs -= 2;
            if (r1 > r0)
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_GEI:
            opStackOfs -= 2;
            if (r1 >= r0)
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_LTU:
            opStackOfs -= 2;
            if (((unsigned)r1) < ((unsigned)r0))
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_LEU:
            opStackOfs -= 2;
            if (((unsigned)r1) <= ((unsigned)r0))
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_GTU:
            opStackOfs -= 2;
            if (((unsigned)r1) > ((unsigned)r0))
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_GEU:
            opStackOfs -= 2;
            if (((unsigned)r1) >= ((unsigned)r0))
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_EQF:
            opStackOfs -= 2;

            if (((float*)opStack)[(uint8_t)(opStackOfs + 1)] ==
                ((float*)opStack)[(uint8_t)(opStackOfs + 2)])
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_NEF:
            opStackOfs -= 2;

            if (((float*)opStack)[(uint8_t)(opStackOfs + 1)] !=
                ((float*)opStack)[(uint8_t)(opStackOfs + 2)])
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_LTF:
            opStackOfs -= 2;

            if (((float*)opStack)[(uint8_t)(opStackOfs + 1)] <
                ((float*)opStack)[(uint8_t)(opStackOfs + 2)])
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_LEF:
            opStackOfs -= 2;

            if (((float*)opStack)[(uint8_t)((uint8_t)(opStackOfs + 1))] <=
                ((float*)opStack)[(uint8_t)((uint8_t)(opStackOfs + 2))])
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_GTF:
            opStackOfs -= 2;

            if (((float*)opStack)[(uint8_t)(opStackOfs + 1)] >
                ((float*)opStack)[(uint8_t)(opStackOfs + 2)])
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }
        case OP_GEF:
            opStackOfs -= 2;

            if (((float*)opStack)[(uint8_t)(opStackOfs + 1)] >=
                ((float*)opStack)[(uint8_t)(opStackOfs + 2)])
            {
                programCounter = r2; /* vm->instructionPointers[r2]; */
                DISPATCH();
            }
            else
            {
                programCounter += 1;
                DISPATCH();
            }

        /*===================================================================*/

        case OP_NEGI:
            opStack[opStackOfs] = -r0;
            DISPATCH();
        case OP_ADD:
            opStackOfs--;
            opStack[opStackOfs] = r1 + r0;
            DISPATCH();
        case OP_SUB:
            opStackOfs--;
            opStack[opStackOfs] = r1 - r0;
            DISPATCH();
        case OP_DIVI:
            opStackOfs--;
            opStack[opStackOfs] = r1 / r0;
            DISPATCH();
        case OP_DIVU:
            opStackOfs--;
            opStack[opStackOfs] = ((unsigned)r1) / ((unsigned)r0);
            DISPATCH();
        case OP_MODI:
            opStackOfs--;
            opStack[opStackOfs] = r1 % r0;
            DISPATCH();
        case OP_MODU:
            opStackOfs--;
            opStack[opStackOfs] = ((unsigned)r1) % ((unsigned)r0);
            DISPATCH();
        case OP_MULI:
            opStackOfs--;
            opStack[opStackOfs] = r1 * r0;
            DISPATCH();
        case OP_MULU:
            opStackOfs--;
            opStack[opStackOfs] = ((unsigned)r1) * ((unsigned)r0);
            DISPATCH();
        case OP_BAND:
            opStackOfs--;
            opStack[opStackOfs] = ((unsigned)r1) & ((unsigned)r0);
            DISPATCH();
        case OP_BOR:
            opStackOfs--;
            opStack[opStackOfs] = ((unsigned)r1) | ((unsigned)r0);
            DISPATCH();
        case OP_BXOR:
            opStackOfs--;
            opStack[opStackOfs] = ((unsigned)r1) ^ ((unsigned)r0);
            DISPATCH();
        case OP_BCOM:
            opStack[opStackOfs] = ~((unsigned)r0);
            DISPATCH();
        case OP_LSH:
            opStackOfs--;
            opStack[opStackOfs] = r1 << r0;
            DISPATCH();
        case OP_RSHI:
            opStackOfs--;
            opStack[opStackOfs] = r1 >> r0;
            DISPATCH();
        case OP_RSHU:
            opStackOfs--;
            opStack[opStackOfs] = ((unsigned)r1) >> r0;
            DISPATCH();
        case OP_NEGF:
            ((float*)opStack)[opStackOfs] = -((float*)opStack)[opStackOfs];
            DISPATCH();
        case OP_ADDF:
            opStackOfs--;
            ((float*)opStack)[opStackOfs] =
                ((float*)opStack)[opStackOfs] +
                ((float*)opStack)[(uint8_t)(opStackOfs + 1)];
            DISPATCH();
        case OP_SUBF:
            opStackOfs--;
            ((float*)opStack)[opStackOfs] =
                ((float*)opStack)[opStackOfs] -
                ((float*)opStack)[(uint8_t)(opStackOfs + 1)];
            DISPATCH();
        case OP_DIVF:
            opStackOfs--;
            ((float*)opStack)[opStackOfs] =
                ((float*)opStack)[opStackOfs] /
                ((float*)opStack)[(uint8_t)(opStackOfs + 1)];
            DISPATCH();
        case OP_MULF:
            opStackOfs--;
            ((float*)opStack)[opStackOfs] =
                ((float*)opStack)[opStackOfs] *
                ((float*)opStack)[(uint8_t)(opStackOfs + 1)];
            DISPATCH();
        case OP_CVIF:
            ((float*)opStack)[opStackOfs] = (float)opStack[opStackOfs];
            DISPATCH();
        case OP_CVFI:
            opStack[opStackOfs] = Q_ftol(((float*)opStack)[opStackOfs]);
            DISPATCH();
        case OP_SEX8:
            opStack[opStackOfs] = (int8_t)opStack[opStackOfs];
            DISPATCH();
        case OP_SEX16:
            opStack[opStackOfs] = (int16_t)opStack[opStackOfs];
            DISPATCH();
        }
    }

done:
    vm->currentlyInterpreting = 0;

    if (opStackOfs != 1 || *opStack != 0x0000BEEF)
    {
        Com_Error("Interpreter stack error");
    }

    vm->programStack = stackOnEntry;

    /* return the result of the bytecode computations */
    return opStack[opStackOfs];
}

intptr_t VM_Call(vm_t* vm, int command, ...)
{
    intptr_t r;
    int      args[MAX_VMMAIN_ARGS];
    va_list  ap;
    int      i;

    if (vm == NULL)
    {
        Com_Error("VM_Call with NULL vm");
        return -1;
    }
    if (vm->codeLength < 1)
    {
        Com_Error("VM not loaded");
        return -1;
    }

    /* FIXME this is not nice. we should check the actual number of arguments */
    args[0] = command;
    va_start(ap, command);
    for (i = 1; i < (int)ARRAY_LEN(args); i++)
    {
        args[i] = va_arg(ap, int);
    }
    va_end(ap);

    ++vm->callLevel;
    r = VM_CallInterpreted(vm, args);
    --vm->callLevel;

    return r;
}

static int VM_PrepareInterpreter(vm_t* vm, const vmHeader_t* header)
{
    int      op;
    int      byte_pc;
    int      int_pc;
    uint8_t* code;
    int      instruction;
    int*     codeBase;

    vm->codeBase = (uint8_t*)Com_malloc(
        vm->codeLength * 4, vm, 0); /* we're now int aligned */
    if (!vm->codeBase)
    {
        Com_Error("Data pointer malloc failed: out of memory?");
        return -1;
    }

    Com_Memcpy(vm->codeBase, (uint8_t*)header + header->codeOffset,
               vm->codeLength);

    /* we don't need to translate the instructions, but we still need
       to find each instructions starting point for jumps */
    int_pc = byte_pc = 0;
    instruction      = 0;
    code             = (uint8_t*)header + header->codeOffset;
    codeBase         = (int*)vm->codeBase;

    /* Copy and expand instructions to words while
     * building instruction table */
    while (instruction < header->instructionCount)
    {
        vm->instructionPointers[instruction] = int_pc;
        instruction++;

        op               = (int)code[byte_pc];
        codeBase[int_pc] = op;
        if (byte_pc > header->codeLength)
        {
            Com_Error("VM_PrepareInterpreter: pc > header->codeLength");
            return -1;
        }

        byte_pc++;
        int_pc++;

        /* these are the only opcodes that aren't a single byte */
        switch (op)
        {
        case OP_ENTER:
        case OP_CONST:
        case OP_LOCAL:
        case OP_LEAVE:
        case OP_EQ:
        case OP_NE:
        case OP_LTI:
        case OP_LEI:
        case OP_GTI:
        case OP_GEI:
        case OP_LTU:
        case OP_LEU:
        case OP_GTU:
        case OP_GEU:
        case OP_EQF:
        case OP_NEF:
        case OP_LTF:
        case OP_LEF:
        case OP_GTF:
        case OP_GEF:
        case OP_BLOCK_COPY:
            codeBase[int_pc] = LittleEndianToHost(&code[byte_pc]);
            byte_pc += 4;
            int_pc++;
            break;
        case OP_ARG:
            codeBase[int_pc] = (int)code[byte_pc];
            byte_pc++;
            int_pc++;
            break;
        default:
            if (op < 0 || op >= OP_MAX)
            {
                Com_Error("Bad VM instruction");
                return -1;
            }
            break;
        }
    }
    int_pc      = 0;
    instruction = 0;

    /* Now that the code has been expanded to int-sized opcodes, we'll translate
       instruction index
       into an index into codeBase[], which contains opcodes and operands. */
    while (instruction < header->instructionCount)
    {
        op = codeBase[int_pc];
        instruction++;
        int_pc++;

        switch (op)
        {
        /* These ops need to translate addresses in jumps from instruction index
           to int index */
        case OP_EQ:
        case OP_NE:
        case OP_LTI:
        case OP_LEI:
        case OP_GTI:
        case OP_GEI:
        case OP_LTU:
        case OP_LEU:
        case OP_GTU:
        case OP_GEU:
        case OP_EQF:
        case OP_NEF:
        case OP_LTF:
        case OP_LEF:
        case OP_GTF:
        case OP_GEF:
            if (codeBase[int_pc] < 0 || codeBase[int_pc] > vm->instructionCount)
            {
                Com_Error("VM_PrepareInterpreter: Jump to invalid "
                          "instruction number");
                return -1;
            }

            /* codeBase[pc] is the instruction index. Convert that into a
               offset into
               the int-aligned codeBase[] by the lookup table. */
            codeBase[int_pc] = vm->instructionPointers[codeBase[int_pc]];
            int_pc++;
            break;

        /* These opcodes have an operand that isn't an instruction index */
        case OP_ENTER:
        case OP_CONST:
        case OP_LOCAL:
        case OP_LEAVE:
        case OP_BLOCK_COPY:
        case OP_ARG:
            int_pc++;
            break;

        default:
            break;
        }
    }
    return 0;
}

static const vmHeader_t* VM_LoadQVM(vm_t* vm, const uint8_t* bytecode,
                                    int length)
{
    int dataLength;
    int i;
    const union {
        const vmHeader_t* h;
        const uint8_t*    v;
    } header = {.v = bytecode };

    Com_Printf("Loading vm file %s...\n", vm->name);

    if (!header.h || !bytecode || length <= (int)sizeof(vmHeader_t) ||
        length > VM_MAX_IMAGE_SIZE)
    {
        Com_Printf("Failed.\n");
        return NULL;
    }

    if (LittleLong(header.h->vmMagic) == VM_MAGIC)
    {
        /* byte swap the header */
        for (i = 0; i < (int)(sizeof(vmHeader_t)) / 4; i++)
        {
            ((int*)header.h)[i] = LittleLong(((int*)header.h)[i]);
        }

        /* validate */
        if (header.h->bssLength < 0 || header.h->dataLength < 0 ||
            header.h->litLength < 0 || header.h->codeLength <= 0 ||
            header.h->codeOffset < 0 || header.h->dataOffset < 0 ||
            header.h->instructionCount <= 0 ||
            header.h->bssLength > VM_MAX_BSS_LENGTH ||
            header.h->codeOffset + header.h->codeLength > length ||
            header.h->dataOffset + header.h->dataLength + header.h->litLength >
                length)
        {
            Com_Printf("Warning: %s has bad header\n", vm->name);
            return NULL;
        }
    }
    else
    {
        Com_Printf("Warning: Invalid magic number in header of \"%s\". "
                   "Read: 0x%x, expected: 0x%x\n",
                   vm->name, LittleLong(header.h->vmMagic), VM_MAGIC);
        return NULL;
    }


    /* round up to next power of 2 so all data operations can
       be mask protected */
    dataLength =
        header.h->dataLength + header.h->litLength + header.h->bssLength;
    for (i = 0; dataLength > (1 << i); i++)
    {
    }
    dataLength = 1 << i;

    /* allocate zero filled space for initialized and uninitialized data
     leave some space beyond data mask so we can secure all mask operations */
    vm->dataAlloc = dataLength + 4;
    vm->dataBase  = (uint8_t*)Com_malloc(vm->dataAlloc, vm, 0);
    vm->dataMask  = dataLength - 1;
    if (vm->dataBase == NULL)
    {
        Com_Error("Data malloc failed: out of memory?\n");
        return NULL;
    }
    /* make sure data section is always initialized with 0
     * (bss would be enough) */
    Com_Memset(vm->dataBase, 0, vm->dataAlloc);

    /* copy the intialized data */
    Com_Memcpy(vm->dataBase, header.v + header.h->dataOffset,
               header.h->dataLength + header.h->litLength);

    


    /* byte swap the longs */
    for (i = 0; i < header.h->dataLength; i += sizeof(int))
    {
        int x = LittleLong(*(int*)(vm->dataBase + i));
        *(int*)(vm->dataBase + i) = x;
    }


    return header.h;
}


int VM_Create(vm_t* vm, const char* name, const uint8_t* bytecode, int length,
              intptr_t (*systemCalls)(vm_t*, intptr_t*))
{


    if (vm == NULL)
    {
        Com_Error("Invalid vm pointer");
        return -1;
    }
    if (!systemCalls)
    {
        Com_Error("No systemcalls provided");
        return -1;
    }

    Com_Memset(vm, 0, sizeof(vm_t));
    Q_strncpyz(vm->name, name, sizeof(vm->name));
    const vmHeader_t* header = VM_LoadQVM(vm, bytecode, length);
    if (!header)
    {
        Com_Error("Failed to load bytecode");
        VM_Free(vm);
        return -1;
    }

    vm->systemCall = systemCalls;

    /* allocate space for the jump targets, which will be filled in by the
       compile/prep functions */
    vm->instructionCount    = header->instructionCount;
    vm->instructionPointers = (intptr_t*)Com_malloc(
        vm->instructionCount * sizeof(*vm->instructionPointers), vm,
        0);
    if (!vm->instructionPointers)
    {
        Com_Error("Instr. pointer malloc failed: out of memory?");
        VM_Free(vm);
        return -1;
    }

    vm->codeLength = header->codeLength;

    vm->compiled = 0; /* no JIT */
    if (!vm->compiled)
    {
        if (VM_PrepareInterpreter(vm, header) != 0)
        {
            VM_Free(vm);
            return -1;
        }
    }

#ifdef DEBUG_VM
    /* load the map file */
    VM_LoadSymbols(vm);
#endif

    /* the stack is implicitly at the end of the image */
    vm->programStack = vm->dataMask + 1;
    vm->stackBottom  = vm->programStack - VM_PROGRAM_STACK_SIZE;

#ifdef DEBUG_VM
    Com_Printf("VM:\n");
    Com_Printf(".code length: %6i bytes\n", header->codeLength);
    Com_Printf(".data length: %6i bytes\n", header->dataLength);
    Com_Printf(".lit  length: %6i bytes\n", header->litLength);
    Com_Printf(".bss  length: %6i bytes\n", header->bssLength);
    Com_Printf("Stack size:   %6i bytes\n", VM_PROGRAM_STACK_SIZE);
    Com_Printf("Allocated memory: %6i bytes\n", vm->dataAlloc);
    Com_Printf("Instruction count: %i\n", header->instructionCount);

#endif

    return 0;
}

void VM_Free(vm_t* vm)
{
    if (!vm)
    {
        return;
    }
    if (vm->callLevel)
    {
        Com_Error("VM_Free on running vm");
        return;
    }

    if (vm->codeBase)
    {
        Com_free(vm->codeBase, vm, 0);
        vm->codeBase = NULL;
    }

    if (vm->dataBase)
    {
        Com_free(vm->dataBase, vm, 0);
        vm->dataBase = NULL;
    }

    if (vm->instructionPointers)
    {
        Com_free(vm->instructionPointers, vm, 0);
        vm->instructionPointers = NULL;
    }

#ifdef DEBUG_VM
    vmSymbol_t* sym = vm->symbols;
    while (sym)
    {
        vmSymbol_t* next = sym->next;
        Com_free(sym, NULL, 0);
        sym = next;
    }
#endif

    Com_Memset(vm, 0, sizeof(*vm));
}

/* DEBUG FUNCTIONS */
/* --------------- */

void VM_Debug(int level)
{
    vm_debugLevel = level;
}

#ifdef DEBUG_VM
static char* VM_Indent(vm_t* vm)
{
    static char* string = "                                        ";
    if (vm->callLevel > 20)
    {
        return string;
    }
    return string + 2 * (20 - vm->callLevel);
}

static const char* VM_ValueToSymbol(vm_t* vm, int value)
{
    vmSymbol_t* sym;
    static char text[MAX_TOKEN_CHARS];

    sym = vm->symbols;
    if (!sym)
    {
        return "NO SYMBOLS";
    }

    /* find the symbol */
    while (sym->next && sym->next->symValue <= value)
    {
        sym = sym->next;
    }

    if (value == sym->symValue)
    {
        return sym->symName;
    }

    snprintf(text, sizeof(text), "%s+%i", sym->symName, value - sym->symValue);

    return text;
}

static vmSymbol_t* VM_ValueToFunctionSymbol(vm_t* vm, int value)
{
    vmSymbol_t*       sym;
    static vmSymbol_t nullSym;

    sym = vm->symbols;
    if (!sym)
    {
        return &nullSym;
    }

    while (sym->next && sym->next->symValue <= value)
    {
        sym = sym->next;
    }

    return sym;
}

static int ParseHex(const char* text)
{
    int value;
    int c;

    value = 0;
    while ((c = *text++) != 0)
    {
        if (c >= '0' && c <= '9')
        {
            value = value * 16 + c - '0';
            continue;
        }
        if (c >= 'a' && c <= 'f')
        {
            value = value * 16 + 10 + c - 'a';
            continue;
        }
        if (c >= 'A' && c <= 'F')
        {
            value = value * 16 + 10 + c - 'A';
            continue;
        }
    }

    return value;
}

static void COM_StripExtension(const char* in, char* out)
{
    while (*in && *in != '.')
    {
        *out++ = *in++;
    }
    *out = 0;
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

static char* SkipWhitespace(char* data, int* hasNewLines)
{
    int c;

    while ((c = *data) <= ' ')
    {
        if (!c)
        {
            return NULL;
        }
        if (c == '\n')
        {
            com_lines++;
            *hasNewLines = 1;
        }
        data++;
    }

    return data;
}

static char* COM_Parse(char** data_p)
{
    int   c           = 0, len;
    int   hasNewLines = 0;
    char* data;
    int   allowLineBreaks = 1;

    data          = *data_p;
    len           = 0;
    com_token[0]  = 0;
    com_tokenline = 0;

    /* make sure incoming data is valid */
    if (!data)
    {
        *data_p = NULL;
        return com_token;
    }

    while (1)
    {
        /* skip whitespace */
        data = SkipWhitespace(data, &hasNewLines);
        if (!data)
        {
            *data_p = NULL;
            return com_token;
        }
        if (hasNewLines && !allowLineBreaks)
        {
            *data_p = data;
            return com_token;
        }

        c = *data;

        /* skip double slash comments */
        if (c == '/' && data[1] == '/')
        {
            data += 2;
            while (*data && *data != '\n')
            {
                data++;
            }
        }
        /* skip comments */
        else if (c == '/' && data[1] == '*')
        {
            data += 2;
            while (*data && (*data != '*' || data[1] != '/'))
            {
                if (*data == '\n')
                {
                    com_lines++;
                }
                data++;
            }
            if (*data)
            {
                data += 2;
            }
        }
        else
        {
            break;
        }
    }

    /* token starts on this line */
    com_tokenline = com_lines;

    /* handle quoted strings */
    if (c == '\"')
    {
        data++;
        while (1)
        {
            c = *data++;
            if (c == '\"' || !c)
            {
                com_token[len] = 0;
                *data_p        = (char*)data;
                return com_token;
            }
            if (c == '\n')
            {
                com_lines++;
            }
            if (len < MAX_TOKEN_CHARS - 1)
            {
                com_token[len] = c;
                len++;
            }
        }
    }

    /* parse a regular word */
    do
    {
        if (len < MAX_TOKEN_CHARS - 1)
        {
            com_token[len] = c;
            len++;
        }
        data++;
        c = *data;
    } while (c > 32);

    com_token[len] = 0;

    *data_p = (char*)data;
    return com_token;
}

static void VM_LoadSymbols(vm_t* vm)
{
    union {
        char* c;
        void* v;
    } mapfile;
    char *       text_p, *token;
    char         name[VM_MAX_QPATH];
    char         symbols[VM_MAX_QPATH];
    vmSymbol_t **prev, *sym;
    int          count;
    int          value;
    int          chars;
    int          segment;
    int          numInstructions;
    int          imageSize;

    COM_StripExtension(vm->name, name);
    snprintf(symbols, sizeof(symbols), "%s.map", name);
    Com_Printf("Loading symbol file: %s...\n", symbols);
    mapfile.v = loadImage(symbols, &imageSize);

    if (!mapfile.c)
    {
        Com_Printf("Couldn't load symbol file: %s\n", symbols);
        return;
    }

    numInstructions = vm->instructionCount;

    /* parse the symbols */
    text_p = mapfile.c;
    prev   = &vm->symbols;
    count  = 0;

    while (1)
    {
        token = COM_Parse(&text_p);
        if (!token[0])
        {
            break;
        }
        segment = ParseHex(token);
        if (segment)
        {
            COM_Parse(&text_p);
            COM_Parse(&text_p);
            continue; /* only load code segment values */
        }

        token = COM_Parse(&text_p);
        if (!token[0])
        {
            Com_Printf("WARNING: incomplete line at end of file\n");
            break;
        }
        value = ParseHex(token);

        token = COM_Parse(&text_p);
        if (!token[0])
        {
            Com_Printf("WARNING: incomplete line at end of file\n");
            break;
        }
        chars = strlen(token);
        sym   = Com_malloc(sizeof(*sym) + chars, NULL, 0);
        *prev = sym;
        if (!sym)
        {
            Com_Error("Sym. pointer malloc failed: out of memory?");
            break;
        }
        Com_Memset(sym, 0, sizeof(*sym) + chars);
        prev      = &sym->next;
        sym->next = NULL;

        /* convert value from an instruction number to a code offset */
        if (value >= 0 && value < numInstructions)
        {
            value = vm->instructionPointers[value];
        }

        sym->symValue = value;
        Q_strncpyz(sym->symName, token, chars + 1);

        count++;
    }

    vm->numSymbols = count;
    Com_Printf("%i symbols parsed from %s\n", count, symbols);
    Com_free(mapfile.v, NULL, 0);
}

static void VM_StackTrace(vm_t* vm, int programCounter, int programStack)
{
    int count;

    count = 0;
    do
    {
        Com_Printf("%s\n", VM_ValueToSymbol(vm, programCounter));
        programStack   = *(int*)&vm->dataBase[programStack + 4];
        programCounter = *(int*)&vm->dataBase[programStack];
    } while (programCounter != -1 && ++count < 32);
}

static int VM_ProfileSort(const void* a, const void* b)
{
    vmSymbol_t *sa, *sb;

    sa = *(vmSymbol_t**)a;
    sb = *(vmSymbol_t**)b;

    if (sa->profileCount < sb->profileCount)
    {
        return -1;
    }
    if (sa->profileCount > sb->profileCount)
    {
        return 1;
    }
    return 0;
}

void VM_VmProfile_f(const vm_t* vm)
{
    vmSymbol_t **sorted, *sym;
    int          i;
    float        total;

    if (!vm)
    {
        return;
    }

    if (vm->numSymbols < 1)
    {
        return;
    }

    sorted = Com_malloc(vm->numSymbols * sizeof(*sorted), NULL, 0);
    if (!sorted)
    {
        Com_Error("Symbol pointer malloc failed: out of memory?");
        return;
    }
    sorted[0] = vm->symbols;
    total     = (float)sorted[0]->profileCount;
    for (i = 1; i < vm->numSymbols; i++)
    {
        sorted[i] = sorted[i - 1]->next;
        total += sorted[i]->profileCount;
    }

    qsort(sorted, vm->numSymbols, sizeof(*sorted), VM_ProfileSort);

    for (i = 0; i < vm->numSymbols; i++)
    {
        int perc;

        sym = sorted[i];

        perc = (int)(100 * (float)sym->profileCount / total);
        Com_Printf("%2i%% %9i %s\n", perc, sym->profileCount, sym->symName);
        sym->profileCount = 0;
    }

    Com_Printf("    %9.0f total\n", total);

    Com_free(sorted, NULL, 0);
}
#else
void VM_VmProfile_f(const vm_t* vm)
{
    (void)vm;
}
#endif
