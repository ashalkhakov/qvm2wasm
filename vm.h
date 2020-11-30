#ifndef __QVM_H__
#define __QVM_H__

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/******************************************************************************
 * DEFINES
 ******************************************************************************/

#define DEBUG_VM /**< ifdef: enable debug functions and additional checks */

/** File start magic number for .qvm files (4 bytes, little endian) */
#define VM_MAGIC 0x12721444

/** Don't change stack size: Hardcoded in q3asm and reserved at end of BSS */
#define VM_PROGRAM_STACK_SIZE 0x10000

/** Max. number of bytes in .qvm */
#define VM_MAX_IMAGE_SIZE 0x400000

/**< Maximum length of a pathname, 64 to be Q3 compatible */
#define VM_MAX_QPATH 64

#endif /* __QVM_H__ */

/** File header of a bytecode .qvm file. Can be directly mapped to the start of
 *  the file. This is always little endian encoded in the file. */
typedef struct
{
    int32_t vmMagic;          /**< Bytecode image shall start with VM_MAGIC */
    int32_t instructionCount; /**< Number of instructions in .qvm */
    int32_t codeOffset;       /**< Byte offset in .qvm file of .code segment */
    int32_t codeLength;       /**< Bytes in code segment */
    int32_t dataOffset;       /**< Byte offset in .qvm file of .data segment */
    int32_t dataLength;       /**< Bytes in .data segment */
    int32_t litLength; /**< Bytes in strings segment (after .data segment) */
    int32_t bssLength; /**< How many bytes should be used for .bss segment */
} vmHeader_t;

/** For debugging (DEBUG_VM): symbol list */
typedef struct vmSymbol_s
{
    struct vmSymbol_s* next; /**< Linked list of symbols */

    int symValue; /**< Value of symbol that we want to have the ASCII name for
                     */
    int  profileCount; /**< For the runtime profiler. +1 for each call. */
    char symName[1];   /**< Variable sized symbol name. Space is reserved by
                          malloc at load time. */
} vmSymbol_t;

/** Main struct (think of a kind of a main class) to keep all information of
 * the virtual machine together. Has pointer to the bytecode, the stack and
 * everything. Call VM_Create(...) to initialize this struct. Call VM_Free(...)
 * to cleanup this struct and free the memory. */
typedef struct vm_s
{
    /* DO NOT MOVE OR CHANGE THESE WITHOUT CHANGING THE VM_OFFSET_* DEFINES
       USED BY THE ASM CODE (IF WE ADD THE Q3 JIT COMPILER IN THE FUTURE) */

    int programStack; /**< Stack pointer into .data segment. */

    /** Function pointer to callback function for native functions called by
     * the bytecode. The function is identified by an integer id that
     * corresponds to the bytecode function ids defined in g_syscalls.asm.
     * Note however that parms equals to (-1 - function_id).
     * So -1 in g_syscalls.asm equals to 0 in the systemCall parms argument,
     *    -2 in g_syscalls.asm equals to 1 in the systemCall parms argument,
     *    -3 in g_syscalls.asm equals to 2 in the systemCall parms argument
     * and so on. You can convert it back to -1, -2, -3, but the 0 based
     * index might help a lookup table. */
    intptr_t (*systemCall)(struct vm_s* vm, intptr_t* parms);

    /*------------------------------------*/

    char  name[VM_MAX_QPATH]; /** File name of the bytecode */
    void* searchPath;         /**< unused */

    /* for dynamic libs (unused in Q3VM) */
    void* unused_dllHandle;                          /**< unused */
    intptr_t (*unused_entryPoint)(int callNum, ...); /**< unused */
    void (*unused_destroy)(struct vm_s* self);       /**< unused */

    int currentlyInterpreting; /**< Is the vm currently running? */

    int      compiled;   /**< Is a JIT active? Otherwise interpreted */
    uint8_t* codeBase;   /**< Bytecode code segment */
    int      entryOfs;   /**< unused */
    int      codeLength; /**< Number of bytes in code segment */

    intptr_t* instructionPointers;
    int       instructionCount; /**< Number of instructions for VM */

    uint8_t* dataBase;  /**< Start of .data memory segment */
    int      dataMask;  /**< VM mask to protect access to dataBase */
    int      dataAlloc; /**< Number of bytes allocated for dataBase */

    int stackBottom; /**< If programStack < stackBottom, error */

    /*------------------------------------*/

    /* DEBUG_VM */
    int         numSymbols; /**< Number of symbols from VM_LoadSymbols */
    vmSymbol_t* symbols;    /**< By VM_LoadSymbols: names for debugging */

    int callLevel;     /**< Counts recursive VM_Call */
    int breakFunction; /**< For debugging: break at this function */
    int breakCount;    /**< Used for breakpoints, triggered by OP_BREAK */

} vm_t;

int VM_Create(vm_t* vm, const char* module, const uint8_t* bytecode, int length,
              intptr_t (*systemCalls)(vm_t*, intptr_t*));

void VM_Free(vm_t* vm);

intptr_t VM_Call(vm_t* vm, int command, ...);

void* VM_ArgPtr(intptr_t vmAddr, vm_t* vm);

float VM_IntToFloat(int32_t x);

int32_t VM_FloatToInt(float f);

int VM_MemoryRangeValid(intptr_t vmAddr, size_t len, const vm_t* vm);

/** Translate from virtual machine memory to real machine memory. */
#define VMA(x, vm) VM_ArgPtr(args[x], vm)

/** Get argument in syscall and interpret it bit by bit as IEEE 754 float */
#define VMF(x) VM_IntToFloat(args[x])

void VM_VmProfile_f(const vm_t* vm);

void VM_Debug(int level);

/** Virtual machine op stack size in bytes */
#define OPSTACK_SIZE 1024

/** Max number of arguments to pass from a vm to engine's syscall handler
 * function for the vm.
 * syscall number + 15 arguments */
#define MAX_VMSYSCALL_ARGS 16

/** Max number of arguments to pass from engine to vm's vmMain function.
 * command number + 12 arguments */
#define MAX_VMMAIN_ARGS 13

/** Macro to read 32-bit little endian value (from the .qvm file) and convert it
 * to the host byte order */
#define LittleLong(x) LittleEndianToHost((const uint8_t*)&(x))

/** Max. number of op codes in op codes table */
#define OPCODE_TABLE_SIZE 64
/** Mask for a valid opcode (so no one can escape the sandbox) */
#define OPCODE_TABLE_MASK (OPCODE_TABLE_SIZE - 1)

/** Max. size of BSS section */
#define VM_MAX_BSS_LENGTH 10485760

/** Enum for the virtual machine op codes */
typedef enum {
    OP_UNDEF, /* Error: VM halt */

    OP_IGNORE, /* No operation */

    OP_BREAK, /* vm->breakCount++ */

    OP_ENTER, /* Begin subroutine. */
    OP_LEAVE, /* End subroutine. */
    OP_CALL,  /* Call subroutine. */
    OP_PUSH,  /* Push to stack. */
    OP_POP,   /* Discard top-of-stack. */

    OP_CONST, /* Load constant to stack. */
    OP_LOCAL, /* Get local variable. */

    OP_JUMP, /* Unconditional jump. */

    /*-------------------*/

    OP_EQ, /* Compare integers, jump if equal. */
    OP_NE, /* Compare integers, jump if not equal. */

    OP_LTI, /* Compare integers, jump if less-than. */
    OP_LEI, /* Compare integers, jump if less-than-or-equal. */
    OP_GTI, /* Compare integers, jump if greater-than. */
    OP_GEI, /* Compare integers, jump if greater-than-or-equal. */

    OP_LTU, /* Compare unsigned integers, jump if less-than */
    OP_LEU, /* Compare unsigned integers, jump if less-than-or-equal */
    OP_GTU, /* Compare unsigned integers, jump if greater-than */
    OP_GEU, /* Compare unsigned integers, jump if greater-than-or-equal */

    OP_EQF, /* Compare floats, jump if equal */
    OP_NEF, /* Compare floats, jump if not-equal */

    OP_LTF, /* Compare floats, jump if less-than */
    OP_LEF, /* Compare floats, jump if less-than-or-equal */
    OP_GTF, /* Compare floats, jump if greater-than */
    OP_GEF, /* Compare floats, jump if greater-than-or-equal */

    /*-------------------*/

    OP_LOAD1,  /* Load 1-byte from memory */
    OP_LOAD2,  /* Load 2-bytes from memory */
    OP_LOAD4,  /* Load 4-bytes from memory */
    OP_STORE1, /* Store 1-byte to memory */
    OP_STORE2, /* Store 2-byte to memory */
    OP_STORE4, /* *(stack[top-1]) = stack[top] */
    OP_ARG,    /* Marshal argument */

    OP_BLOCK_COPY, /* memcpy */

    /*-------------------*/

    OP_SEX8,  /* Sign-Extend 8-bit */
    OP_SEX16, /* Sign-Extend 16-bit */

    OP_NEGI, /* Negate integer. */
    OP_ADD,  /* Add integers (two's complement). */
    OP_SUB,  /* Subtract integers (two's complement). */
    OP_DIVI, /* Divide signed integers. */
    OP_DIVU, /* Divide unsigned integers. */
    OP_MODI, /* Modulus (signed). */
    OP_MODU, /* Modulus (unsigned). */
    OP_MULI, /* Multiply signed integers. */
    OP_MULU, /* Multiply unsigned integers. */

    OP_BAND, /* Bitwise AND */
    OP_BOR,  /* Bitwise OR */
    OP_BXOR, /* Bitwise eXclusive-OR */
    OP_BCOM, /* Bitwise COMplement */

    OP_LSH,  /* Left-shift */
    OP_RSHI, /* Right-shift (algebraic; preserve sign) */
    OP_RSHU, /* Right-shift (bitwise; ignore sign) */

    OP_NEGF, /* Negate float */
    OP_ADDF, /* Add floats */
    OP_SUBF, /* Subtract floats */
    OP_DIVF, /* Divide floats */
    OP_MULF, /* Multiply floats */

    OP_CVIF, /* Convert to integer from float */
    OP_CVFI, /* Convert to float from integer */

    OP_MAX /* Make this the last item */
} opcode_t;
