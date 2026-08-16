/*
 * wasm.c -- Quake 3 QVM bytecode to WebAssembly translator.
 *
 * Self contained: emits a binary .wasm module directly (no external
 * dependencies, no wabt/LLVM needed).
 *
 * Translation model
 * -----------------
 * - The QVM data+lit+bss segments live at offset 0 of one wasm linear
 *   memory. The size is rounded up to a power of two so that every load
 *   and store can be masked (same sandboxing scheme as the q3vm
 *   interpreter). The program stack lives at the top of that region,
 *   exactly like in q3vm (q3asm reserves it at the end of BSS).
 * - The program stack pointer is a mutable wasm global.
 * - Every QVM function (an OP_ENTER up to the next OP_ENTER) becomes one
 *   wasm function of type () -> i32.
 * - The QVM operand stack is modeled with wasm locals. The stack depth at
 *   every instruction is computed statically (q3asm/LCC output always has
 *   consistent depths), so opstack slot N simply becomes local N+1.
 * - Control flow inside a function uses the classic dispatch-loop pattern:
 *   a loop containing nested blocks, entered through a br_table indexed by
 *   a "label" local that holds the relative instruction index. This
 *   supports arbitrary jumps including the computed OP_JUMP used for
 *   switch jump tables.
 * - OP_CALL uses call_indirect through a funcref table indexed by
 *   instruction index (entries exist at each OP_ENTER), so indirect calls
 *   through function pointers work. Negative targets become calls to the
 *   imported host syscall function.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"
#include "wasm.h"

/* ---------------------------------------------------------------------- */
/* growable byte buffer                                                    */

typedef struct buf_s
{
    uint8_t* data;
    int      len;
    int      cap;
} buf_t;

static int buf_init(buf_t* b)
{
    b->cap  = 4096;
    b->len  = 0;
    b->data = (uint8_t*)malloc(b->cap);
    return b->data ? 0 : -1;
}

static void buf_free(buf_t* b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int buf_reserve(buf_t* b, int extra)
{
    if (b->len + extra > b->cap)
    {
        int newCap = b->cap;
        while (b->len + extra > newCap)
        {
            newCap *= 2;
        }
        uint8_t* p = (uint8_t*)realloc(b->data, newCap);
        if (!p)
        {
            return -1;
        }
        b->data = p;
        b->cap  = newCap;
    }
    return 0;
}

static void emit_byte(buf_t* b, uint8_t v)
{
    if (buf_reserve(b, 1) == 0)
    {
        b->data[b->len++] = v;
    }
}

static void emit_bytes(buf_t* b, const uint8_t* p, int n)
{
    if (n > 0 && buf_reserve(b, n) == 0)
    {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
}

/* unsigned LEB128 */
static void emit_u32(buf_t* b, uint32_t v)
{
    do
    {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        if (v)
        {
            byte |= 0x80;
        }
        emit_byte(b, byte);
    } while (v);
}

/* signed LEB128 */
static void emit_s32(buf_t* b, int32_t v)
{
    int more = 1;
    while (more)
    {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40)))
        {
            more = 0;
        }
        else
        {
            byte |= 0x80;
        }
        emit_byte(b, byte);
    }
}

/* ---------------------------------------------------------------------- */
/* wasm opcodes we use                                                     */

#define W_UNREACHABLE 0x00
#define W_LOOP 0x03
#define W_IF 0x04
#define W_ELSE 0x05
#define W_END 0x0B
#define W_BR 0x0C
#define W_BR_IF 0x0D
#define W_BR_TABLE 0x0E
#define W_RETURN 0x0F
#define W_CALL 0x10
#define W_CALL_INDIRECT 0x11
#define W_BLOCK 0x02
#define W_LOCAL_GET 0x20
#define W_LOCAL_SET 0x21
#define W_GLOBAL_GET 0x23
#define W_GLOBAL_SET 0x24
#define W_I32_LOAD 0x28
#define W_I32_LOAD8_U 0x2D
#define W_I32_LOAD16_U 0x2F
#define W_I32_STORE 0x36
#define W_I32_STORE8 0x3A
#define W_I32_STORE16 0x3B
#define W_I32_CONST 0x41
#define W_I32_EQ 0x46
#define W_I32_NE 0x47
#define W_I32_LT_S 0x48
#define W_I32_LT_U 0x49
#define W_I32_GT_S 0x4A
#define W_I32_GT_U 0x4B
#define W_I32_LE_S 0x4C
#define W_I32_LE_U 0x4D
#define W_I32_GE_S 0x4E
#define W_I32_GE_U 0x4F
#define W_F32_EQ 0x5B
#define W_F32_NE 0x5C
#define W_F32_LT 0x5D
#define W_F32_GT 0x5E
#define W_F32_LE 0x5F
#define W_F32_GE 0x60
#define W_I32_ADD 0x6A
#define W_I32_SUB 0x6B
#define W_I32_MUL 0x6C
#define W_I32_DIV_S 0x6D
#define W_I32_DIV_U 0x6E
#define W_I32_REM_S 0x6F
#define W_I32_REM_U 0x70
#define W_I32_AND 0x71
#define W_I32_OR 0x72
#define W_I32_XOR 0x73
#define W_I32_SHL 0x74
#define W_I32_SHR_S 0x75
#define W_I32_SHR_U 0x76
#define W_F32_NEG 0x8C
#define W_F32_ADD 0x92
#define W_F32_SUB 0x93
#define W_F32_MUL 0x94
#define W_F32_DIV 0x95
#define W_F32_CONVERT_I32_S 0xB2
#define W_I32_REINTERPRET_F32 0xBC
#define W_F32_REINTERPRET_I32 0xBE
#define W_I32_EXTEND8_S 0xC0
#define W_I32_EXTEND16_S 0xC1
#define W_FC_PREFIX 0xFC /* i32.trunc_sat_f32_s = FC 00, memory.copy = FC 0A */

#define W_BLOCKTYPE_VOID 0x40
#define W_VALTYPE_I32 0x7F

/* section ids */
#define SEC_TYPE 1
#define SEC_IMPORT 2
#define SEC_FUNC 3
#define SEC_TABLE 4
#define SEC_MEMORY 5
#define SEC_GLOBAL 6
#define SEC_EXPORT 7
#define SEC_ELEM 9
#define SEC_CODE 10
#define SEC_DATA 11

/* ---------------------------------------------------------------------- */
/* expanded bytecode                                                       */

typedef struct qfunc_s
{
    int start;    /* first instruction (the OP_ENTER) */
    int end;      /* one past last instruction */
    int maxDepth; /* max opstack depth (in slots) */
} qfunc_t;

typedef struct ctx_s
{
    const vmHeader_t* header;
    const uint8_t*    fileData;

    int  insnCount;
    int* ops;      /* opcode per instruction */
    int* operands; /* operand per instruction (0 if none) */

    int      numFuncs;
    qfunc_t* funcs;
    int*     funcOfInsn; /* function index per instruction */

    int* depth;    /* static opstack depth before each instruction */
    int* leader;   /* -1, or region index within the function */
    int* expected; /* expected depth at branch targets, -1 if unknown */

    int dataMask;
    int totalMem;
} ctx_t;

static int32_t le32(const uint8_t b[4])
{
    return (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                     ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
}

static int err(const char* msg)
{
    fprintf(stderr, "qvm2wasm: %s\n", msg);
    return -1;
}

static int op_has_i4_operand(int op)
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

static int op_is_cond_branch(int op)
{
    return op >= OP_EQ && op <= OP_GEF;
}

/* opstack pops/pushes per opcode */
static void op_stack_effect(int op, int* pops, int* pushes)
{
    switch (op)
    {
    case OP_CONST:
    case OP_LOCAL:
    case OP_PUSH:
        *pops   = 0;
        *pushes = 1;
        break;
    case OP_POP:
    case OP_ARG:
    case OP_JUMP:
        *pops   = 1;
        *pushes = 0;
        break;
    case OP_LEAVE: /* consumes nothing; return value read from top */
        *pops   = 0;
        *pushes = 0;
        break;
    case OP_CALL: /* pops target, pushes result */
        *pops   = 1;
        *pushes = 1;
        break;
    case OP_LOAD1:
    case OP_LOAD2:
    case OP_LOAD4:
    case OP_SEX8:
    case OP_SEX16:
    case OP_NEGI:
    case OP_BCOM:
    case OP_NEGF:
    case OP_CVIF:
    case OP_CVFI:
        *pops   = 1;
        *pushes = 1;
        break;
    case OP_STORE1:
    case OP_STORE2:
    case OP_STORE4:
    case OP_BLOCK_COPY:
        *pops   = 2;
        *pushes = 0;
        break;
    case OP_ADD:
    case OP_SUB:
    case OP_DIVI:
    case OP_DIVU:
    case OP_MODI:
    case OP_MODU:
    case OP_MULI:
    case OP_MULU:
    case OP_BAND:
    case OP_BOR:
    case OP_BXOR:
    case OP_LSH:
    case OP_RSHI:
    case OP_RSHU:
    case OP_ADDF:
    case OP_SUBF:
    case OP_DIVF:
    case OP_MULF:
        *pops   = 2;
        *pushes = 1;
        break;
    default:
        if (op_is_cond_branch(op))
        {
            *pops   = 2;
            *pushes = 0;
        }
        else
        {
            *pops   = 0;
            *pushes = 0;
        }
        break;
    }
}

/* ---------------------------------------------------------------------- */
/* pass 1: decode the byte stream into (opcode, operand) pairs             */

static int expand_code(ctx_t* c)
{
    const uint8_t* code   = c->fileData + c->header->codeOffset;
    int            length = c->header->codeLength;
    int            pc     = 0;
    int            i;

    c->ops      = (int*)calloc(c->insnCount, sizeof(int));
    c->operands = (int*)calloc(c->insnCount, sizeof(int));
    if (!c->ops || !c->operands)
    {
        return err("out of memory");
    }

    for (i = 0; i < c->insnCount; i++)
    {
        if (pc >= length)
        {
            return err("code segment truncated");
        }
        int op    = code[pc++];
        c->ops[i] = op;
        if (op < 0 || op >= OP_MAX)
        {
            return err("bad opcode in code segment");
        }
        if (op_has_i4_operand(op))
        {
            if (pc + 4 > length)
            {
                return err("code segment truncated (operand)");
            }
            c->operands[i] = (int)((uint32_t)code[pc] |
                                   ((uint32_t)code[pc + 1] << 8) |
                                   ((uint32_t)code[pc + 2] << 16) |
                                   ((uint32_t)code[pc + 3] << 24));
            pc += 4;
        }
        else if (op == OP_ARG)
        {
            if (pc + 1 > length)
            {
                return err("code segment truncated (arg operand)");
            }
            c->operands[i] = code[pc++];
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------- */
/* pass 2: split into functions                                            */

static int find_functions(ctx_t* c)
{
    int i, f;

    if (c->ops[0] != OP_ENTER)
    {
        return err("bytecode does not start with OP_ENTER");
    }

    c->numFuncs = 0;
    for (i = 0; i < c->insnCount; i++)
    {
        if (c->ops[i] == OP_ENTER)
        {
            c->numFuncs++;
        }
    }

    c->funcs      = (qfunc_t*)calloc(c->numFuncs, sizeof(qfunc_t));
    c->funcOfInsn = (int*)calloc(c->insnCount, sizeof(int));
    if (!c->funcs || !c->funcOfInsn)
    {
        return err("out of memory");
    }

    f = -1;
    for (i = 0; i < c->insnCount; i++)
    {
        if (c->ops[i] == OP_ENTER)
        {
            if (f >= 0)
            {
                c->funcs[f].end = i;
            }
            f++;
            c->funcs[f].start = i;
        }
        c->funcOfInsn[i] = f;
    }
    c->funcs[f].end = c->insnCount;
    return 0;
}

/* ---------------------------------------------------------------------- */
/* pass 3: per function, compute static opstack depths and leaders         */

static int analyze_function(ctx_t* c, qfunc_t* fn)
{
    int i;
    int cur       = 0;
    int reachable = 1;
    int hasJump   = 0;

    fn->maxDepth = 0;

    /* collect branch targets first so backward/forward targets both work */
    for (i = fn->start; i < fn->end; i++)
    {
        if (op_is_cond_branch(c->ops[i]) || c->ops[i] == OP_JUMP)
        {
            if (c->ops[i] == OP_JUMP)
            {
                hasJump = 1;
                continue;
            }
            int t = c->operands[i];
            if (t < fn->start || t >= fn->end)
            {
                return err("branch target outside of function");
            }
        }
    }

    /* forward scan: propagate depths, record expectations at targets */
    cur = 0;
    for (i = fn->start; i < fn->end; i++)
    {
        int op = c->ops[i];
        int pops, pushes;

        if (c->expected[i] >= 0)
        {
            if (reachable && cur != c->expected[i])
            {
                return err("inconsistent opstack depth at branch target");
            }
            cur       = c->expected[i];
            reachable = 1;
        }
        else if (!reachable)
        {
            /* dead code or a jump-table target: those always start with an
               empty opstack in q3asm generated code */
            cur       = 0;
            reachable = 1;
        }

        c->depth[i] = cur;
        if (cur > fn->maxDepth)
        {
            fn->maxDepth = cur;
        }

        op_stack_effect(op, &pops, &pushes);
        if (cur < pops)
        {
            return err("opstack underflow");
        }
        cur += pushes - pops;
        if (cur > fn->maxDepth)
        {
            fn->maxDepth = cur;
        }
        if (cur > OPSTACK_SIZE / 4)
        {
            return err("opstack overflow");
        }

        if (op_is_cond_branch(op))
        {
            int t = c->operands[i];
            if (c->expected[t] < 0)
            {
                c->expected[t] = cur;
                if (t <= i && c->depth[t] != cur)
                {
                    return err("inconsistent opstack depth at branch target");
                }
            }
            else if (c->expected[t] != cur)
            {
                return err("inconsistent opstack depth at branch target");
            }
        }
        else if (op == OP_JUMP || op == OP_LEAVE)
        {
            reachable = 0;
        }
    }

    /* leaders: the entry, all explicit branch targets and (if the function
       contains computed jumps) every instruction with an empty opstack */
    int region       = 0;
    c->leader[fn->start] = region++;
    for (i = fn->start + 1; i < fn->end; i++)
    {
        if (c->expected[i] >= 0 || (hasJump && c->depth[i] == 0))
        {
            c->leader[i] = region++;
        }
    }
    return region; /* number of leaders */
}

/* ---------------------------------------------------------------------- */
/* code emission helpers                                                   */

static void e_local_get(buf_t* b, int idx)
{
    emit_byte(b, W_LOCAL_GET);
    emit_u32(b, idx);
}

static void e_local_set(buf_t* b, int idx)
{
    emit_byte(b, W_LOCAL_SET);
    emit_u32(b, idx);
}

static void e_const(buf_t* b, int32_t v)
{
    emit_byte(b, W_I32_CONST);
    emit_s32(b, v);
}

static void e_memarg(buf_t* b)
{
    emit_u32(b, 0); /* alignment hint: 1 byte (QVM does unaligned access) */
    emit_u32(b, 0); /* offset */
}

/* global 0 is the program stack pointer */
static void e_ps_get(buf_t* b)
{
    emit_byte(b, W_GLOBAL_GET);
    emit_u32(b, 0);
}

static void e_ps_set(buf_t* b)
{
    emit_byte(b, W_GLOBAL_SET);
    emit_u32(b, 0);
}

static void e_mask(buf_t* b, const ctx_t* c)
{
    e_const(b, c->dataMask);
    emit_byte(b, W_I32_AND);
}

/* opstack slot n lives in local n+1 (local 0 is the dispatch label) */
#define SLOT(n) ((n) + 1)
#define LBL_LOCAL 0

static void e_reinterp_f32(buf_t* b)
{
    emit_byte(b, W_F32_REINTERPRET_I32);
}

static void e_reinterp_i32(buf_t* b)
{
    emit_byte(b, W_I32_REINTERPRET_F32);
}

/* ---------------------------------------------------------------------- */
/* per function body emission                                              */

/* wasm function indices: 0 = imported syscall, 1..numFuncs = QVM functions,
   numFuncs+1 = the exported vmMain wrapper */
#define SYSCALL_FUNCIDX 0

static int emit_function_body(ctx_t* c, int funcIdx, buf_t* body)
{
    qfunc_t* fn = &c->funcs[funcIdx];
    int      i, k;

    int numLeaders = 0;
    for (i = fn->start; i < fn->end; i++)
    {
        if (c->leader[i] >= 0)
        {
            numLeaders++;
        }
    }

    /* locals: label + opstack slots, all i32 */
    int numLocals = 1 + fn->maxDepth;
    emit_u32(body, 1); /* one local group */
    emit_u32(body, numLocals);
    emit_byte(body, W_VALTYPE_I32);

    /* loop L
         block BAD
           block B_{n-1} ... block B_0
             local.get $lbl
             br_table <insn -> leader block> default=BAD
           end B_0
           <region 0>
           end B_1
           <region 1>
           ...
         end BAD  (also reached by falling off the last region)
         unreachable
       end L
       unreachable */

    emit_byte(body, W_LOOP);
    emit_byte(body, W_BLOCKTYPE_VOID);
    emit_byte(body, W_BLOCK); /* BAD */
    emit_byte(body, W_BLOCKTYPE_VOID);
    for (k = 0; k < numLeaders; k++)
    {
        emit_byte(body, W_BLOCK);
        emit_byte(body, W_BLOCKTYPE_VOID);
    }

    e_local_get(body, LBL_LOCAL);
    emit_byte(body, W_BR_TABLE);
    emit_u32(body, fn->end - fn->start); /* number of entries */
    for (i = fn->start; i < fn->end; i++)
    {
        if (c->leader[i] >= 0)
        {
            emit_u32(body, c->leader[i]); /* B_k is at label depth k */
        }
        else
        {
            emit_u32(body, numLeaders); /* not a jump target -> BAD */
        }
    }
    emit_u32(body, numLeaders); /* default -> BAD */
    emit_byte(body, W_END);     /* closes B_0: region 0 starts here */

    int curRegion = 0;
    for (i = fn->start; i < fn->end; i++)
    {
        if (i != fn->start && c->leader[i] >= 0)
        {
            emit_byte(body, W_END); /* close B_{curRegion+1} */
            curRegion++;
        }

        int op = c->ops[i];
        int v  = c->operands[i];
        int d  = c->depth[i];
        /* label depth of the dispatch loop from inside region curRegion */
        int loopDepth = numLeaders - curRegion;

        switch (op)
        {
        case OP_IGNORE:
        case OP_BREAK:
            break;

        case OP_ENTER:
            e_ps_get(body);
            e_const(body, v);
            emit_byte(body, W_I32_SUB);
            e_ps_set(body);
            break;

        case OP_LEAVE:
            e_ps_get(body);
            e_const(body, v);
            emit_byte(body, W_I32_ADD);
            e_ps_set(body);
            if (d > 0)
            {
                e_local_get(body, SLOT(d - 1));
            }
            else
            {
                e_const(body, 0);
            }
            emit_byte(body, W_RETURN);
            break;

        case OP_CALL:
            /* target in slot d-1; negative -> host syscall */
            e_local_get(body, SLOT(d - 1));
            e_const(body, 0);
            emit_byte(body, W_I32_LT_S);
            emit_byte(body, W_IF);
            emit_byte(body, W_VALTYPE_I32);
            /*   mem[PS+4] = -1 - target */
            e_ps_get(body);
            e_const(body, 4);
            emit_byte(body, W_I32_ADD);
            e_mask(body, c);
            e_const(body, -1);
            e_local_get(body, SLOT(d - 1));
            emit_byte(body, W_I32_SUB);
            emit_byte(body, W_I32_STORE);
            e_memarg(body);
            /*   result = syscall(PS+4) */
            e_ps_get(body);
            e_const(body, 4);
            emit_byte(body, W_I32_ADD);
            emit_byte(body, W_CALL);
            emit_u32(body, SYSCALL_FUNCIDX);
            emit_byte(body, W_ELSE);
            /*   result = table[target]() */
            e_local_get(body, SLOT(d - 1));
            emit_byte(body, W_CALL_INDIRECT);
            emit_u32(body, 0); /* type 0: () -> i32 */
            emit_u32(body, 0); /* table 0 */
            emit_byte(body, W_END);
            e_local_set(body, SLOT(d - 1));
            break;

        case OP_PUSH:
            e_const(body, 0);
            e_local_set(body, SLOT(d));
            break;

        case OP_POP:
            break;

        case OP_CONST:
            e_const(body, v);
            e_local_set(body, SLOT(d));
            break;

        case OP_LOCAL:
            e_ps_get(body);
            e_const(body, v);
            emit_byte(body, W_I32_ADD);
            e_local_set(body, SLOT(d));
            break;

        case OP_JUMP:
            e_local_get(body, SLOT(d - 1));
            e_const(body, fn->start);
            emit_byte(body, W_I32_SUB);
            e_local_set(body, LBL_LOCAL);
            emit_byte(body, W_BR);
            emit_u32(body, loopDepth);
            break;

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
        {
            static const uint8_t cmp[] = {
                W_I32_EQ,   W_I32_NE,   W_I32_LT_S, W_I32_LE_S, W_I32_GT_S,
                W_I32_GE_S, W_I32_LT_U, W_I32_LE_U, W_I32_GT_U, W_I32_GE_U
            };
            e_const(body, v - fn->start);
            e_local_set(body, LBL_LOCAL);
            e_local_get(body, SLOT(d - 2));
            e_local_get(body, SLOT(d - 1));
            emit_byte(body, cmp[op - OP_EQ]);
            emit_byte(body, W_BR_IF);
            emit_u32(body, loopDepth);
            break;
        }

        case OP_EQF:
        case OP_NEF:
        case OP_LTF:
        case OP_LEF:
        case OP_GTF:
        case OP_GEF:
        {
            static const uint8_t cmp[] = { W_F32_EQ, W_F32_NE, W_F32_LT,
                                           W_F32_LE, W_F32_GT, W_F32_GE };
            e_const(body, v - fn->start);
            e_local_set(body, LBL_LOCAL);
            e_local_get(body, SLOT(d - 2));
            e_reinterp_f32(body);
            e_local_get(body, SLOT(d - 1));
            e_reinterp_f32(body);
            emit_byte(body, cmp[op - OP_EQF]);
            emit_byte(body, W_BR_IF);
            emit_u32(body, loopDepth);
            break;
        }

        case OP_LOAD1:
        case OP_LOAD2:
        case OP_LOAD4:
            e_local_get(body, SLOT(d - 1));
            e_mask(body, c);
            emit_byte(body, op == OP_LOAD1   ? W_I32_LOAD8_U
                            : op == OP_LOAD2 ? W_I32_LOAD16_U
                                             : W_I32_LOAD);
            e_memarg(body);
            e_local_set(body, SLOT(d - 1));
            break;

        case OP_STORE1:
        case OP_STORE2:
        case OP_STORE4:
            e_local_get(body, SLOT(d - 2));
            e_mask(body, c);
            e_local_get(body, SLOT(d - 1));
            emit_byte(body, op == OP_STORE1   ? W_I32_STORE8
                            : op == OP_STORE2 ? W_I32_STORE16
                                              : W_I32_STORE);
            e_memarg(body);
            break;

        case OP_ARG:
            /* mem[(PS + v) & mask] = top */
            e_ps_get(body);
            e_const(body, v);
            emit_byte(body, W_I32_ADD);
            e_mask(body, c);
            e_local_get(body, SLOT(d - 1));
            emit_byte(body, W_I32_STORE);
            e_memarg(body);
            break;

        case OP_BLOCK_COPY:
            /* memcpy(dest = slot d-2, src = slot d-1, n = operand) */
            e_local_get(body, SLOT(d - 2));
            e_mask(body, c);
            e_local_get(body, SLOT(d - 1));
            e_mask(body, c);
            e_const(body, v);
            emit_byte(body, W_FC_PREFIX);
            emit_u32(body, 0x0A); /* memory.copy */
            emit_byte(body, 0x00);
            emit_byte(body, 0x00);
            break;

        case OP_SEX8:
        case OP_SEX16:
            e_local_get(body, SLOT(d - 1));
            emit_byte(body, op == OP_SEX8 ? W_I32_EXTEND8_S
                                          : W_I32_EXTEND16_S);
            e_local_set(body, SLOT(d - 1));
            break;

        case OP_NEGI:
            e_const(body, 0);
            e_local_get(body, SLOT(d - 1));
            emit_byte(body, W_I32_SUB);
            e_local_set(body, SLOT(d - 1));
            break;

        case OP_BCOM:
            e_local_get(body, SLOT(d - 1));
            e_const(body, -1);
            emit_byte(body, W_I32_XOR);
            e_local_set(body, SLOT(d - 1));
            break;

        case OP_ADD:
        case OP_SUB:
        case OP_DIVI:
        case OP_DIVU:
        case OP_MODI:
        case OP_MODU:
        case OP_MULI:
        case OP_MULU:
        case OP_BAND:
        case OP_BOR:
        case OP_BXOR:
        case OP_LSH:
        case OP_RSHI:
        case OP_RSHU:
        {
            static const uint8_t alu[] = {
                W_I32_ADD,   W_I32_SUB,   W_I32_DIV_S, W_I32_DIV_U,
                W_I32_REM_S, W_I32_REM_U, W_I32_MUL,   W_I32_MUL,
                W_I32_AND,   W_I32_OR,    W_I32_XOR,   0 /* BCOM */,
                W_I32_SHL,   W_I32_SHR_S, W_I32_SHR_U
            };
            e_local_get(body, SLOT(d - 2));
            e_local_get(body, SLOT(d - 1));
            emit_byte(body, alu[op - OP_ADD]);
            e_local_set(body, SLOT(d - 2));
            break;
        }

        case OP_NEGF:
            e_local_get(body, SLOT(d - 1));
            e_reinterp_f32(body);
            emit_byte(body, W_F32_NEG);
            e_reinterp_i32(body);
            e_local_set(body, SLOT(d - 1));
            break;

        case OP_ADDF:
        case OP_SUBF:
        case OP_DIVF:
        case OP_MULF:
        {
            static const uint8_t falu[] = { W_F32_ADD, W_F32_SUB, W_F32_DIV,
                                            W_F32_MUL };
            e_local_get(body, SLOT(d - 2));
            e_reinterp_f32(body);
            e_local_get(body, SLOT(d - 1));
            e_reinterp_f32(body);
            emit_byte(body, falu[op - OP_ADDF]);
            e_reinterp_i32(body);
            e_local_set(body, SLOT(d - 2));
            break;
        }

        case OP_CVIF:
            e_local_get(body, SLOT(d - 1));
            emit_byte(body, W_F32_CONVERT_I32_S);
            e_reinterp_i32(body);
            e_local_set(body, SLOT(d - 1));
            break;

        case OP_CVFI:
            e_local_get(body, SLOT(d - 1));
            e_reinterp_f32(body);
            emit_byte(body, W_FC_PREFIX);
            emit_u32(body, 0x00); /* i32.trunc_sat_f32_s */
            e_local_set(body, SLOT(d - 1));
            break;

        default:
            emit_byte(body, W_UNREACHABLE);
            break;
        }
    }

    emit_byte(body, W_END); /* BAD */
    emit_byte(body, W_UNREACHABLE);
    emit_byte(body, W_END); /* loop */
    emit_byte(body, W_UNREACHABLE);
    emit_byte(body, W_END); /* function */
    return 0;
}

/* the exported vmMain wrapper: sets up the stack frame like
   VM_CallInterpreted and calls the QVM function at instruction 0 */
static void emit_vmmain_body(ctx_t* c, buf_t* body)
{
    int i;
    int frame = 8 + 4 * MAX_VMMAIN_ARGS;

    emit_u32(body, 0); /* no locals */

    /* PS -= frame */
    e_ps_get(body);
    e_const(body, frame);
    emit_byte(body, W_I32_SUB);
    e_ps_set(body);

    /* mem[PS] = -1 (return address sentinel), mem[PS+4] = 0 */
    e_ps_get(body);
    e_mask(body, c);
    e_const(body, -1);
    emit_byte(body, W_I32_STORE);
    e_memarg(body);
    e_ps_get(body);
    e_const(body, 4);
    emit_byte(body, W_I32_ADD);
    e_mask(body, c);
    e_const(body, 0);
    emit_byte(body, W_I32_STORE);
    e_memarg(body);

    /* mem[PS + 8 + 4*i] = arg_i */
    for (i = 0; i < MAX_VMMAIN_ARGS; i++)
    {
        e_ps_get(body);
        e_const(body, 8 + 4 * i);
        emit_byte(body, W_I32_ADD);
        e_mask(body, c);
        e_local_get(body, i);
        emit_byte(body, W_I32_STORE);
        e_memarg(body);
    }

    /* result = f0() */
    emit_byte(body, W_CALL);
    emit_u32(body, 1); /* first QVM function */

    /* PS += frame */
    e_ps_get(body);
    e_const(body, frame);
    emit_byte(body, W_I32_ADD);
    e_ps_set(body);

    emit_byte(body, W_END);
}

/* ---------------------------------------------------------------------- */
/* module assembly                                                         */

static void emit_section(buf_t* out, int id, const buf_t* content)
{
    emit_byte(out, id);
    emit_u32(out, content->len);
    emit_bytes(out, content->data, content->len);
}

static int emit_module(ctx_t* c, buf_t* out)
{
    static const uint8_t moduleHeader[] = { 0x00, 0x61, 0x73, 0x6D,
                                            0x01, 0x00, 0x00, 0x00 };
    buf_t sec, body;
    int   i;
    int   vmMainFuncIdx = 1 + c->numFuncs;

    emit_bytes(out, moduleHeader, sizeof(moduleHeader));

    if (buf_init(&sec) != 0 || buf_init(&body) != 0)
    {
        return err("out of memory");
    }

    /* --- type section: 0 = ()->i32, 1 = (i32)->i32, 2 = vmMain --- */
    sec.len = 0;
    emit_u32(&sec, 3);
    emit_byte(&sec, 0x60); /* func type */
    emit_u32(&sec, 0);
    emit_u32(&sec, 1);
    emit_byte(&sec, W_VALTYPE_I32);
    emit_byte(&sec, 0x60);
    emit_u32(&sec, 1);
    emit_byte(&sec, W_VALTYPE_I32);
    emit_u32(&sec, 1);
    emit_byte(&sec, W_VALTYPE_I32);
    emit_byte(&sec, 0x60);
    emit_u32(&sec, MAX_VMMAIN_ARGS);
    for (i = 0; i < MAX_VMMAIN_ARGS; i++)
    {
        emit_byte(&sec, W_VALTYPE_I32);
    }
    emit_u32(&sec, 1);
    emit_byte(&sec, W_VALTYPE_I32);
    emit_section(out, SEC_TYPE, &sec);

    /* --- import section: env.syscall --- */
    sec.len = 0;
    emit_u32(&sec, 1);
    emit_u32(&sec, 3);
    emit_bytes(&sec, (const uint8_t*)"env", 3);
    emit_u32(&sec, 7);
    emit_bytes(&sec, (const uint8_t*)"syscall", 7);
    emit_byte(&sec, 0x00); /* func import */
    emit_u32(&sec, 1);     /* type 1: (i32)->i32 */
    emit_section(out, SEC_IMPORT, &sec);

    /* --- function section --- */
    sec.len = 0;
    emit_u32(&sec, c->numFuncs + 1);
    for (i = 0; i < c->numFuncs; i++)
    {
        emit_u32(&sec, 0); /* type 0: ()->i32 */
    }
    emit_u32(&sec, 2); /* vmMain wrapper: type 2 */
    emit_section(out, SEC_FUNC, &sec);

    /* --- table section: funcref table indexed by instruction --- */
    sec.len = 0;
    emit_u32(&sec, 1);
    emit_byte(&sec, 0x70); /* funcref */
    emit_byte(&sec, 0x01); /* min and max */
    emit_u32(&sec, c->insnCount);
    emit_u32(&sec, c->insnCount);
    emit_section(out, SEC_TABLE, &sec);

    /* --- memory section --- */
    sec.len = 0;
    emit_u32(&sec, 1);
    emit_byte(&sec, 0x01); /* min and max */
    emit_u32(&sec, (c->totalMem + 0xFFFF) / 0x10000);
    emit_u32(&sec, (c->totalMem + 0xFFFF) / 0x10000);
    emit_section(out, SEC_MEMORY, &sec);

    /* --- global section: program stack pointer --- */
    sec.len = 0;
    emit_u32(&sec, 1);
    emit_byte(&sec, W_VALTYPE_I32);
    emit_byte(&sec, 0x01); /* mutable */
    e_const(&sec, c->totalMem);
    emit_byte(&sec, W_END);
    emit_section(out, SEC_GLOBAL, &sec);

    /* --- export section --- */
    sec.len = 0;
    emit_u32(&sec, 2);
    emit_u32(&sec, 6);
    emit_bytes(&sec, (const uint8_t*)"vmMain", 6);
    emit_byte(&sec, 0x00); /* func */
    emit_u32(&sec, vmMainFuncIdx);
    emit_u32(&sec, 6);
    emit_bytes(&sec, (const uint8_t*)"memory", 6);
    emit_byte(&sec, 0x02); /* memory */
    emit_u32(&sec, 0);
    emit_section(out, SEC_EXPORT, &sec);

    /* --- element section: table[startInsn(f)] = f --- */
    sec.len = 0;
    emit_u32(&sec, c->numFuncs);
    for (i = 0; i < c->numFuncs; i++)
    {
        emit_u32(&sec, 0); /* active segment, table 0, funcidx vector */
        e_const(&sec, c->funcs[i].start);
        emit_byte(&sec, W_END);
        emit_u32(&sec, 1);
        emit_u32(&sec, 1 + i);
    }
    emit_section(out, SEC_ELEM, &sec);

    /* --- code section --- */
    sec.len = 0;
    emit_u32(&sec, c->numFuncs + 1);
    for (i = 0; i < c->numFuncs; i++)
    {
        body.len = 0;
        if (emit_function_body(c, i, &body) != 0)
        {
            buf_free(&sec);
            buf_free(&body);
            return -1;
        }
        emit_u32(&sec, body.len);
        emit_bytes(&sec, body.data, body.len);
    }
    body.len = 0;
    emit_vmmain_body(c, &body);
    emit_u32(&sec, body.len);
    emit_bytes(&sec, body.data, body.len);
    emit_section(out, SEC_CODE, &sec);

    /* --- data section: data + lit segments at offset 0 --- */
    int initLen = c->header->dataLength + c->header->litLength;
    if (initLen > 0)
    {
        sec.len = 0;
        emit_u32(&sec, 1);
        emit_u32(&sec, 0); /* active segment, memory 0 */
        e_const(&sec, 0);
        emit_byte(&sec, W_END);
        emit_u32(&sec, initLen);
        emit_bytes(&sec, c->fileData + c->header->dataOffset, initLen);
        emit_section(out, SEC_DATA, &sec);
    }

    buf_free(&sec);
    buf_free(&body);
    return 0;
}

/* ---------------------------------------------------------------------- */

static void ctx_free(ctx_t* c)
{
    free(c->ops);
    free(c->operands);
    free(c->funcs);
    free(c->funcOfInsn);
    free(c->depth);
    free(c->leader);
    free(c->expected);
}

int QVM2WASM_Compile(const uint8_t* bytecode, int length, uint8_t** wasmOut,
                     int* wasmLen)
{
    ctx_t      c;
    vmHeader_t header;
    int        i;

    *wasmOut = NULL;
    *wasmLen = 0;
    memset(&c, 0, sizeof(c));

    if (!bytecode || length <= (int)sizeof(vmHeader_t) ||
        length > VM_MAX_IMAGE_SIZE)
    {
        return err("invalid .qvm image");
    }

    /* read the little endian header */
    for (i = 0; i < (int)(sizeof(header) / 4); i++)
    {
        ((int32_t*)&header)[i] = le32(bytecode + 4 * i);
    }

    if (header.vmMagic != VM_MAGIC)
    {
        return err("bad magic number (not a .qvm file?)");
    }
    if (header.bssLength < 0 || header.dataLength < 0 ||
        header.litLength < 0 || header.codeLength <= 0 ||
        header.codeOffset < 0 || header.dataOffset < 0 ||
        header.instructionCount <= 0 ||
        header.bssLength > VM_MAX_BSS_LENGTH ||
        header.codeOffset + header.codeLength > length ||
        header.dataOffset + header.dataLength + header.litLength > length)
    {
        return err("bad .qvm header");
    }

    c.header    = &header;
    c.fileData  = bytecode;
    c.insnCount = header.instructionCount;

    /* round data+lit+bss up to a power of two for mask based sandboxing,
       just like the q3vm interpreter; the program stack lives at the top */
    int dataLen = header.dataLength + header.litLength + header.bssLength;
    for (i = 0; dataLen > (1 << i); i++)
    {
    }
    c.totalMem = 1 << i;
    if (c.totalMem < VM_PROGRAM_STACK_SIZE)
    {
        c.totalMem = VM_PROGRAM_STACK_SIZE;
    }
    c.dataMask = c.totalMem - 1;

    if (expand_code(&c) != 0 || find_functions(&c) != 0)
    {
        ctx_free(&c);
        return -1;
    }

    c.depth    = (int*)calloc(c.insnCount, sizeof(int));
    c.leader   = (int*)malloc(c.insnCount * sizeof(int));
    c.expected = (int*)malloc(c.insnCount * sizeof(int));
    if (!c.depth || !c.leader || !c.expected)
    {
        ctx_free(&c);
        return err("out of memory");
    }
    for (i = 0; i < c.insnCount; i++)
    {
        c.leader[i]   = -1;
        c.expected[i] = -1;
    }

    for (i = 0; i < c.numFuncs; i++)
    {
        if (analyze_function(&c, &c.funcs[i]) < 0)
        {
            ctx_free(&c);
            return -1;
        }
    }

    buf_t out;
    if (buf_init(&out) != 0)
    {
        ctx_free(&c);
        return err("out of memory");
    }
    if (emit_module(&c, &out) != 0)
    {
        buf_free(&out);
        ctx_free(&c);
        return -1;
    }

    ctx_free(&c);
    *wasmOut = out.data;
    *wasmLen = out.len;
    return 0;
}
