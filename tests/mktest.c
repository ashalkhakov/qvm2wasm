/*
 * mktest.c -- generates a .qvm bytecode image that exercises the QVM
 * instruction set. Used to test the qvm2wasm translator against the
 * interpreter (both must produce identical output).
 *
 * Usage: mktest <output.qvm>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "vm.h"

#define MAX_INSNS 4096
#define MAX_LABELS 256
#define MAX_PATCHES 512

#define DATA_SIZE 64  /* words for the jump table etc. */
#define LIT_SIZE 256  /* string constants */
#define LIT_BASE DATA_SIZE
#define BSS_BASE (DATA_SIZE + LIT_SIZE)
#define BSS_SIZE 0x10000

static struct
{
    uint8_t op;
    int32_t arg;
} insns[MAX_INSNS];
static int numInsns;

static int labelPos[MAX_LABELS]; /* label id -> instruction index */
static int nextLabel;

static struct
{
    int isData;  /* patch a data word instead of an instruction operand */
    int where;   /* instruction index or data word offset */
    int labelId;
} patches[MAX_PATCHES];
static int numPatches;

static uint8_t litSeg[LIT_SIZE];
static int     litUsed;

static int32_t dataSeg[DATA_SIZE / 4];

static void die(const char* msg)
{
    fprintf(stderr, "mktest: %s\n", msg);
    exit(1);
}

/* ---- tiny assembler ---------------------------------------------------- */

static int op_has_i4(int op)
{
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
        return 1;
    default:
        return 0;
    }
}

static void I(int op)
{
    if (numInsns >= MAX_INSNS)
        die("too many instructions");
    insns[numInsns].op  = op;
    insns[numInsns].arg = 0;
    numInsns++;
}

static void I4(int op, int32_t v)
{
    I(op);
    insns[numInsns - 1].arg = v;
}

static int newLabel(void)
{
    if (nextLabel >= MAX_LABELS)
        die("too many labels");
    labelPos[nextLabel] = -1;
    return nextLabel++;
}

static void label(int id)
{
    labelPos[id] = numInsns;
}

/* branch/const whose operand is a label (instruction index) */
static void I4L(int op, int labelId)
{
    I4(op, 0);
    patches[numPatches].isData  = 0;
    patches[numPatches].where   = numInsns - 1;
    patches[numPatches].labelId = labelId;
    if (++numPatches > MAX_PATCHES)
        die("too many patches");
}

/* data word holding a label (for jump tables) */
static void dataLabel(int wordOfs, int labelId)
{
    patches[numPatches].isData  = 1;
    patches[numPatches].where   = wordOfs;
    patches[numPatches].labelId = labelId;
    if (++numPatches > MAX_PATCHES)
        die("too many patches");
}

static int str(const char* s) /* returns VM address of the string */
{
    int addr = LIT_BASE + litUsed;
    int n    = (int)strlen(s) + 1;
    if (litUsed + n > LIT_SIZE)
        die("lit segment full");
    memcpy(litSeg + litUsed, s, n);
    litUsed += n;
    return addr;
}

static int32_t f2i(float f)
{
    int32_t v;
    memcpy(&v, &f, 4);
    return v;
}

/* ---- helpers generating common sequences ------------------------------- */

/* syscall numbers as CONST operands (see SystemCalls in main.c) */
#define SYS_PRINT_INT -1
#define SYS_PRINT_STR -2
#define SYS_MEMSET -3
#define SYS_MEMCPY -4
#define SYS_ERROR -5

/* print the int on top of the opstack (consumes it) */
static void printTop(void)
{
    I4(OP_ARG, 8);
    I4(OP_CONST, SYS_PRINT_INT);
    I(OP_CALL);
    I(OP_POP);
}

static void printStr(int addr)
{
    I4(OP_CONST, addr);
    I4(OP_ARG, 8);
    I4(OP_CONST, SYS_PRINT_STR);
    I(OP_CALL);
    I(OP_POP);
}

static int nlAddr;

static void printTopLn(void)
{
    printTop();
    printStr(nlAddr);
}

/* print result of <CONST a> <CONST b> <op> */
static void testAlu(int op, int32_t a, int32_t b)
{
    I4(OP_CONST, a);
    I4(OP_CONST, b);
    I(op);
    printTopLn();
}

/* print 1 if branch taken, 0 if not, for "a <op> b" */
static void testBranch(int op, int32_t a, int32_t b)
{
    int taken = newLabel();
    int next  = newLabel();
    I4(OP_CONST, a);
    I4(OP_CONST, b);
    I4L(op, taken);
    I4(OP_CONST, 0);
    printTopLn();
    I4L(OP_CONST, next); /* unconditional jump via computed OP_JUMP */
    I(OP_JUMP);
    label(taken);
    I4(OP_CONST, 1);
    printTopLn();
    label(next);
}

/* ---- the test program -------------------------------------------------- */

static void generate(void)
{
    int F0 = newLabel(); /* vmMain */
    int F1 = newLabel(); /* fact */
    int F2 = newLabel(); /* double */

    nlAddr = str("\n");

    /* ---------------- vmMain ---------------- */
    label(F0);
    I4(OP_ENTER, 64);
    /* frame: outgoing args at +8..+20, locals at +32..+60,
       vmMain args at +64+8 (command), +64+12 (arg1), ... */

    printStr(str("=== BEGIN ===\n"));

    /* vmMain arguments */
    printStr(str("args:\n"));
    I4(OP_LOCAL, 64 + 8);
    I(OP_LOAD4);
    printTopLn();
    I4(OP_LOCAL, 64 + 12);
    I(OP_LOAD4);
    printTopLn();

    /* integer alu: command <op> arg1 */
    printStr(str("int alu:\n"));
    {
        static const int ops[] = { OP_ADD,  OP_SUB,  OP_MULI, OP_DIVI,
                                   OP_MODI, OP_BAND, OP_BOR,  OP_BXOR,
                                   OP_LSH,  OP_RSHI };
        int i;
        for (i = 0; i < (int)(sizeof(ops) / sizeof(ops[0])); i++)
        {
            I4(OP_LOCAL, 64 + 8);
            I(OP_LOAD4);
            I4(OP_LOCAL, 64 + 12);
            I(OP_LOAD4);
            I(ops[i]);
            printTopLn();
        }
    }
    /* unary ops */
    I4(OP_LOCAL, 64 + 8);
    I(OP_LOAD4);
    I(OP_NEGI);
    printTopLn();
    I4(OP_LOCAL, 64 + 8);
    I(OP_LOAD4);
    I(OP_BCOM);
    printTopLn();

    /* unsigned ops */
    printStr(str("unsigned:\n"));
    testAlu(OP_DIVU, (int32_t)0x80000000u, 3);
    testAlu(OP_MODU, (int32_t)0x80000000u, 7);
    testAlu(OP_RSHU, -64, 3);
    testAlu(OP_RSHI, -64, 3);
    testAlu(OP_MULU, (int32_t)0xFFFFFFFFu, 2);

    /* floats: (2.5*4.0 + 1.25 - 0.75) / 3.0 = 3.5 -> 3 */
    printStr(str("float:\n"));
    I4(OP_CONST, f2i(2.5f));
    I4(OP_CONST, f2i(4.0f));
    I(OP_MULF);
    I4(OP_CONST, f2i(1.25f));
    I(OP_ADDF);
    I4(OP_CONST, f2i(0.75f));
    I(OP_SUBF);
    I4(OP_CONST, f2i(3.0f));
    I(OP_DIVF);
    I(OP_CVFI);
    printTopLn();
    /* int -> float -> int: 7 * 2.0 = 14 */
    I4(OP_CONST, 7);
    I(OP_CVIF);
    I4(OP_CONST, f2i(2.0f));
    I(OP_MULF);
    I(OP_CVFI);
    printTopLn();
    /* negf: trunc(-5.5 * 1.5) = -8 */
    I4(OP_CONST, f2i(5.5f));
    I(OP_NEGF);
    I4(OP_CONST, f2i(1.5f));
    I(OP_MULF);
    I(OP_CVFI);
    printTopLn();

    /* loop: sum 1..10 = 55 (backward conditional branch) */
    printStr(str("loop:\n"));
    {
        int loop = newLabel();
        int end  = newLabel();
        I4(OP_LOCAL, 32); /* acc = 0 */
        I4(OP_CONST, 0);
        I(OP_STORE4);
        I4(OP_LOCAL, 36); /* i = 1 */
        I4(OP_CONST, 1);
        I(OP_STORE4);
        label(loop);
        I4(OP_LOCAL, 36);
        I(OP_LOAD4);
        I4(OP_CONST, 10);
        I4L(OP_GTI, end); /* if i > 10 break */
        I4(OP_LOCAL, 32); /* acc += i */
        I4(OP_LOCAL, 32);
        I(OP_LOAD4);
        I4(OP_LOCAL, 36);
        I(OP_LOAD4);
        I(OP_ADD);
        I(OP_STORE4);
        I4(OP_LOCAL, 36); /* i++ */
        I4(OP_LOCAL, 36);
        I(OP_LOAD4);
        I4(OP_CONST, 1);
        I(OP_ADD);
        I(OP_STORE4);
        I4L(OP_CONST, loop);
        I(OP_JUMP);
        label(end);
        I4(OP_LOCAL, 32);
        I(OP_LOAD4);
        printTopLn();
    }

    /* conditional branch battery */
    printStr(str("branches:\n"));
    testBranch(OP_EQ, 3, 3);
    testBranch(OP_NE, 3, 3);
    testBranch(OP_LTI, -2, 1);
    testBranch(OP_LEI, 5, 5);
    testBranch(OP_GTI, -1, -2);
    testBranch(OP_GEI, 2, 3);
    testBranch(OP_LTU, -2, 1); /* 0xFFFFFFFE < 1 unsigned: no */
    testBranch(OP_LEU, 1, 1);
    testBranch(OP_GTU, (int32_t)0x80000000u, 1);
    testBranch(OP_GEU, 2, 3);
    testBranch(OP_EQF, f2i(1.5f), f2i(1.5f));
    testBranch(OP_NEF, f2i(1.5f), f2i(1.5f));
    testBranch(OP_LTF, f2i(-1.5f), f2i(2.0f));
    testBranch(OP_LEF, f2i(2.0f), f2i(2.0f));
    testBranch(OP_GTF, f2i(3.0f), f2i(2.0f));
    testBranch(OP_GEF, f2i(1.0f), f2i(2.0f));

    /* recursion: fact(6) = 720 */
    printStr(str("fact:\n"));
    I4(OP_CONST, 6);
    I4(OP_ARG, 8);
    I4L(OP_CONST, F1);
    I(OP_CALL);
    printTopLn();

    /* indirect call through a function pointer in a local: double(21) */
    printStr(str("fptr:\n"));
    I4(OP_LOCAL, 40);
    I4L(OP_CONST, F2);
    I(OP_STORE4);
    I4(OP_CONST, 21);
    I4(OP_ARG, 8);
    I4(OP_LOCAL, 40);
    I(OP_LOAD4);
    I(OP_CALL);
    printTopLn();

    /* switch via jump table in the data segment: case (arg1 % 3) */
    printStr(str("switch:\n"));
    {
        int c0 = newLabel(), c1 = newLabel(), c2 = newLabel();
        int end = newLabel();
        dataSeg[0] = 0; /* patched below */
        dataLabel(0, c0);
        dataLabel(1, c1);
        dataLabel(2, c2);
        I4(OP_LOCAL, 64 + 12); /* arg1 % 3 */
        I(OP_LOAD4);
        I4(OP_CONST, 3);
        I(OP_MODI);
        I4(OP_CONST, 2); /* *4 */
        I(OP_LSH);
        I4(OP_CONST, 0); /* + jump table base (data address 0) */
        I(OP_ADD);
        I(OP_LOAD4);
        I(OP_JUMP);
        label(c0);
        I4(OP_CONST, 100);
        printTopLn();
        I4L(OP_CONST, end);
        I(OP_JUMP);
        label(c1);
        I4(OP_CONST, 200);
        printTopLn();
        I4L(OP_CONST, end);
        I(OP_JUMP);
        label(c2);
        I4(OP_CONST, 300);
        printTopLn();
        label(end);
    }

    /* sized loads/stores and sign extension */
    printStr(str("mem:\n"));
    I4(OP_LOCAL, 44);
    I4(OP_CONST, 0x12345678);
    I(OP_STORE4);
    I4(OP_LOCAL, 44);
    I(OP_LOAD4);
    printTopLn();
    I4(OP_LOCAL, 44);
    I4(OP_CONST, 0xBEEF);
    I(OP_STORE2);
    I4(OP_LOCAL, 44);
    I(OP_LOAD2);
    printTopLn(); /* 48879 */
    I4(OP_LOCAL, 44);
    I(OP_LOAD2);
    I(OP_SEX16);
    printTopLn(); /* -16657 */
    I4(OP_LOCAL, 44);
    I4(OP_CONST, 0xF0);
    I(OP_STORE1);
    I4(OP_LOCAL, 44);
    I(OP_LOAD1);
    printTopLn(); /* 240 */
    I4(OP_LOCAL, 44);
    I(OP_LOAD1);
    I(OP_SEX8);
    printTopLn(); /* -16 */

    /* OP_BLOCK_COPY: copy a string from lit into bss and print it */
    {
        int src = str("copied\n");
        I4(OP_CONST, BSS_BASE);
        I4(OP_CONST, src);
        I4(OP_BLOCK_COPY, 8);
        printStr(BSS_BASE);
    }

    /* memset / memcpy syscalls */
    I4(OP_CONST, BSS_BASE + 64);
    I4(OP_ARG, 8);
    I4(OP_CONST, 'B');
    I4(OP_ARG, 12);
    I4(OP_CONST, 5);
    I4(OP_ARG, 16);
    I4(OP_CONST, SYS_MEMSET);
    I(OP_CALL);
    I(OP_POP);
    printStr(BSS_BASE + 64); /* "BBBBB" (bss is zero initialized) */
    printStr(nlAddr);

    {
        int src = str("memcpy'd\n");
        I4(OP_CONST, BSS_BASE + 128);
        I4(OP_ARG, 8);
        I4(OP_CONST, src);
        I4(OP_ARG, 12);
        I4(OP_CONST, 10);
        I4(OP_ARG, 16);
        I4(OP_CONST, SYS_MEMCPY);
        I(OP_CALL);
        I(OP_POP);
        printStr(BSS_BASE + 128);
    }

    printStr(str("=== END ===\n"));
    I4(OP_CONST, 42); /* vmMain return value */
    I4(OP_LEAVE, 64);

    /* ---------------- fact(n) ---------------- */
    label(F1);
    I4(OP_ENTER, 64);
    {
        int base = newLabel();
        I4(OP_LOCAL, 64 + 8); /* if n <= 1 return 1 */
        I(OP_LOAD4);
        I4(OP_CONST, 1);
        I4L(OP_LEI, base);
        I4(OP_LOCAL, 64 + 8); /* fact(n - 1) */
        I(OP_LOAD4);
        I4(OP_CONST, 1);
        I(OP_SUB);
        I4(OP_ARG, 8);
        I4L(OP_CONST, F1);
        I(OP_CALL);
        I4(OP_LOCAL, 64 + 8); /* * n */
        I(OP_LOAD4);
        I(OP_MULI);
        I4(OP_LEAVE, 64);
        label(base);
        I4(OP_CONST, 1);
        I4(OP_LEAVE, 64);
    }

    /* ---------------- double(n) ---------------- */
    label(F2);
    I4(OP_ENTER, 64);
    I4(OP_LOCAL, 64 + 8);
    I(OP_LOAD4);
    I4(OP_CONST, 2);
    I(OP_MULI);
    I4(OP_LEAVE, 64);
}

/* ---- serialization ------------------------------------------------------ */

static void putLE32(FILE* f, int32_t v)
{
    uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                     (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
    fwrite(b, 1, 4, f);
}

int main(int argc, char** argv)
{
    int i;

    if (argc != 2)
    {
        fprintf(stderr, "usage: mktest <output.qvm>\n");
        return 1;
    }

    generate();

    /* resolve label patches */
    for (i = 0; i < numPatches; i++)
    {
        int pos = labelPos[patches[i].labelId];
        if (pos < 0)
            die("undefined label");
        if (patches[i].isData)
            dataSeg[patches[i].where] = pos;
        else
            insns[patches[i].where].arg = pos;
    }

    /* compute code length */
    int codeLength = 0;
    for (i = 0; i < numInsns; i++)
    {
        codeLength += 1;
        if (op_has_i4(insns[i].op))
            codeLength += 4;
        else if (insns[i].op == OP_ARG)
            codeLength += 1;
    }

    FILE* f = fopen(argv[1], "wb");
    if (!f)
        die("cannot open output file");

    /* header */
    putLE32(f, VM_MAGIC);
    putLE32(f, numInsns);
    putLE32(f, 32); /* codeOffset */
    putLE32(f, codeLength);
    putLE32(f, 32 + codeLength); /* dataOffset */
    putLE32(f, DATA_SIZE);
    putLE32(f, LIT_SIZE);
    putLE32(f, BSS_SIZE);

    /* code */
    for (i = 0; i < numInsns; i++)
    {
        fputc(insns[i].op, f);
        if (op_has_i4(insns[i].op))
            putLE32(f, insns[i].arg);
        else if (insns[i].op == OP_ARG)
            fputc(insns[i].arg & 0xFF, f);
    }

    /* data + lit */
    for (i = 0; i < DATA_SIZE / 4; i++)
        putLE32(f, dataSeg[i]);
    fwrite(litSeg, 1, LIT_SIZE, f);

    fclose(f);
    printf("wrote %s: %d instructions, %d code bytes\n", argv[1], numInsns,
           codeLength);
    return 0;
}
