#include <stdlib.h>
#include <assert.h>

#include "vm.h"
#include "ir.h"

const static char *kindnames[] = {
    "CFG_NONE",
    "CFG_BASIC",
    "CFG_SEQ",
    "CFG_IF",
    "CFG_WHILE",
    "CFG_LAST"
};

/** Table to convert op codes to readable names */
// FIXME: duplicated in vm.c
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
// FIXME: duplicated in vm.c
static int LittleEndianToHost(const uint8_t b[4])
{
    return (b[0] << 0) | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
}


void FreeCode(codeseg_t *seg)
{
    free(seg->insns);
}

void ExpandCode(codeseg_t *seg, uint8_t *code, int length, int instructionCount)
{
    int      op;
    int      byte_pc;
    int      int_pc;
    insn_t  *codeBase;

    seg->numInsns = instructionCount;
    seg->insns = (insn_t*)malloc(length * sizeof(insn_t));
    if (!seg->insns)
    {
        Com_Error("Instruction list malloc failed: out of memory?");
        return -1;
    }

    /* we don't need to translate the instructions, but we still need
       to find each instructions starting point for jumps */
    int_pc = byte_pc = 0;
    codeBase         = seg->insns;

    /* Copy and expand instructions to words while
     * building instruction table */
    while (int_pc < instructionCount)
    {
        op = (int)code[byte_pc];
        if (byte_pc > length)
        {
            Com_Error("ExpandCode: byte_pc > length");
            return;
        }

        codeBase[int_pc].opcode = op;
        byte_pc++;

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
            codeBase[int_pc].operand = LittleEndianToHost(&code[byte_pc]);
            byte_pc += 4;
            break;
        case OP_ARG:
            codeBase[int_pc].operand = (int)code[byte_pc];
            byte_pc++;
            break;
        default:
            if (op < 0 || op >= OP_MAX)
            {
                Com_Error("Bad VM instruction");
                return;
            }
            codeBase[int_pc].operand = 0;
            break;
        }
        int_pc++;
    }
}

static void FPrintInsn(FILE *fp, int i, insn_t *insn) {
    fprintf(fp, "%d:\t%s", i, opnames[insn->opcode]);
    switch (insn->opcode)
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
    case OP_ARG:
        fprintf(fp, " %d", insn->operand);
        break;
    default:
        break;
    }
}

void PrintCode(codeseg_t *seg) {
    printf("CODE segment:\n%d instructions\n", seg->numInsns);
    insn_t *insn = seg->insns;
    for (int i = 0; i < seg->numInsns; i++) {
        FPrintInsn(stdout, i, insn);
        printf("\n");
        insn++;
    }
}

typedef struct intlist_s {
    int                 num;
    struct intlist_s    *next;
} intlist_t;

static int intlist_length(intlist_t *list) {
    intlist_t *head = list;
    int len = 0;

    while (head != NULL) {
        len++;
        head = head->next;
    }

    return len;
}

static void intlist_append_to_end(intlist_t **list, int item) {
    if (*list == NULL) {
        intlist_t *tmp = (intlist_t*)malloc(sizeof(intlist_t));
        tmp->num = item;
        tmp->next = NULL;
        *list = tmp;
    } else {
        intlist_t *head = *list;
        while (head->next != NULL)
            head = head->next;
        intlist_t *tmp = (intlist_t*)malloc(sizeof(intlist_t));
        tmp->num = item;
        tmp->next = NULL;
        head->next = tmp;
    }
}

static intlist_t *intlist_reverse(intlist_t *list) {
    intlist_t *prev    = NULL;
    intlist_t *current = list;
    intlist_t *next;
    while (current != NULL) {
        next  = current->next;
        current->next = prev;
        prev = current;
        current = next;
    }
    return prev;
}

// NOTE: this acts like set union (no duplicates allowed)
intlist_t *intlist_insert_sorted(intlist_t *head, int num)
{
    intlist_t *current;

    // end of list?
    if (head == NULL || head->num >= num) {
        intlist_t *node = (intlist_t *)malloc(sizeof(intlist_t));
        node->num = num;
        node->next = head;
        head = node;
    }
    else
    {
        // locate the node before the point of insertion
        current = head;
        while (current->next != NULL && current->next->num < num) {
            current = current->next;
        }
        if (current->num != num) {
            intlist_t *node = (intlist_t *)malloc(sizeof(intlist_t));
            node->num = num;
            node->next = current->next;
            current->next = node;
        }
    }
    return head;
}

static cfgnodelist_t *cfgnodelist_push(cfgnodelist_t *list, cfgnode_t *data) {
    cfgnodelist_t* n = (cfgnodelist_t *)malloc(sizeof(cfgnodelist_t));
    n->node  = data;
    n->next = list;
    return n;
}
static cfgbranchlist_t *cfgbranchlist_push(cfgbranchlist_t *list, cfgbranch_t *data) {
    cfgbranchlist_t *n = (cfgbranchlist_t *)malloc(sizeof(cfgbranchlist_t));
    n->branch = data;
    n->next = list;
    return n;
}

// b -> c
void cfgnode_attach(cfgnode_t *b, cfgbranch_t *c) {
    assert(b != NULL);
    assert(c != NULL);
    b->outgoing = cfgbranchlist_push(b->outgoing, c);
    c->node->incoming = cfgnodelist_push(c->node->incoming, b);
}

void FreeCFG(cfg_t *cfg) {
    cfgnodelist_t *n = cfg->nodes;
    cfgnodelist_t *next;
    while (n != NULL) {
        next = n->next;

        cfgnodelist_t *inc = n->node->incoming;
        cfgnodelist_t *inc_next;
        while (inc != NULL) {
            inc_next = inc->next;
            free(inc);
            inc = inc_next;
        }
        n->node->incoming = NULL;

        cfgnodelist_t *out = n->node->outgoing;
        cfgnodelist_t *out_next;
        while (out != NULL) {
            out_next = out->next;
            free(out);
            out = out_next;
        }
        n->node->outgoing = NULL;

        free(n->node);
        free(n);

        n = next;
    }
    cfg->startNode = NULL;
    cfg->endNode = NULL;
    cfg->nodes = NULL;
}

// FIXME: debug printing to something? svg?
void EmitCFG(cfg_t *cfg, FILE *fp, insn_t *insns, int length) {
    cfgnodelist_t *outl;
    cfgnode_t *block;
    cfgnodelist_t *n;

    n = cfg->nodes;
    fprintf(fp, "digraph G\n{\n");
    while (n != NULL) {
        block = n->node;
        fprintf(fp, "    n%i [ shape = \"box\"\n"
                    "             fontname = \"Monospace\"\n"
                    "             label = \"<%d(OpStackOut:%d)>\\l", block->num, block->num, block->opStackOutputDepth);

        for (int i = block->start; i < block->start + block->length; i++) {
            FPrintInsn(fp, i, &insns[i]);
            fprintf(fp, "\\l");
        }
        outl = block->outgoing;
        fprintf(fp, "\" ];\n");

        while (outl != NULL) {
            cfgnode_t *o = outl->node;
            fprintf(fp, "      n%i -> n%i;\n", block->num, o->num);
            outl = outl->next;
        }

        n = n->next;
    }
    fprintf(fp, "}\n");
}

static cfgbranch_t *branch_make_conditional(int opcode, int local, int constant, cfgnode_t *node) {
    cfgbranch_t *br = malloc(sizeof(cfgbranch_t));
    br->opcode = opcode;
    br->local = local;
    br->constant = constant;
    br->node = node;
    return br;
}
static cfgbranch_t *branch_make(cfgnode_t *node) {
    return branch_make_conditional(OP_NONE, 0, 0, node);
}

cfgnode_t *find_target(int numBlocks, cfgnode_t **blocks, int num) {
    for (int j = 0; j < numBlocks; j++) {
        if (blocks[j]->start == num) {
            return blocks[j];
        }
    }
    return NULL;
}

void BuildCFG(cfg_t *cfg, int funNumber, insn_t *insns, int length, int offset) {
    // Identify leaders: first statement of a basic block
    // In program order, construct a block by appending subsequent statements up to, but not including, the next leader

    // Identifying leaders
    // - first statement
    // - explicit target of any conditional or unconditional branch
    // - implicit target of any branch

    intlist_t *leaders = intlist_insert_sorted(NULL, offset);
    // NOTE: qvm functions always contain a LEAVE statement as the last one
    // so the last LEAVE acts as exit node, and all other LEAVEs preceding it
    // will be treated as jumps to the exit node
    leaders = intlist_insert_sorted(leaders, offset+length-1);

    for (int i = offset; i < offset+length; i++) {
        int r0 = insns[i].operand;

        switch (insns[i].opcode) {
        case OP_JUMP:
            // ha, this might be dynamic!
            // - see Duff's device!
            // - in that case there will have to be some value on the opStack
            assert(i > offset); // if it isn't, we got a dangling JUMP with no destination!
            if (insns[i-1].opcode == OP_CONST) {
                assert(insns[i-1].opcode == OP_CONST);
                r0 = insns[i-1].operand;
                if ((unsigned)r0 >= (unsigned)(offset+length))
                {
                    Com_Error("VM program counter out of range in jump");
                    return;
                }
                leaders = intlist_insert_sorted(leaders, r0);
            } else if (insns[i-1].opcode == OP_LOAD4) {
                // alright it's a branch table

                if (i-15 < offset) {
                    // there should be at least 15 instructions preceding it!
                    assert(0);
                }

                // OP_LOCAL <LOC>
                assert(insns[i-15].opcode == OP_LOCAL);
                int loc = insns[i-15].operand;
                // OP_LOAD4
                assert(insns[i-14].opcode == OP_LOAD4);
                // OP_CONST <LB>
                assert(insns[i-13].opcode == OP_CONST);
                int lb = insns[i-13].operand;
                // OP_LTI <label-default>
                assert(insns[i-12].opcode == OP_LTI);
                int dl = insns[i-12].operand;
                // OP_LOCAL <LOC>
                assert(insns[i-11].opcode == OP_LOCAL);
                assert(insns[i-11].operand == loc);
                // OP_LOAD4
                assert(insns[i-10].opcode == OP_LOAD4);
                // OP_CONST <UB>
                assert(insns[i-9].opcode == OP_CONST);
                int ub = insns[i-9].operand;
                // OP_GTI <label-default>
                assert(insns[i-8].opcode == OP_GTI);
                assert(insns[i-8].operand == dl);
                // OP_LOCAL <LOC> // the local variable being compared with
                assert(insns[i-7].opcode == OP_LOCAL);
                assert(insns[i-7].operand == loc);
                // OP_LOAD4
                assert(insns[i-6].opcode == OP_LOAD4);
                // OP_CONST <OFS> // the hard-coded offset to the table
                assert(insns[i-5].opcode == OP_CONST);
                int ofs = insns[i-5].operand;
                // OP_LSH
                assert(insns[i-4].opcode == OP_LSH);
                // OP_CONST <LS>  // the left-shift amount
                assert(insns[i-3].opcode == OP_CONST);
                int ls = insns[i-3].operand;
                // OP_ADD
                assert(insns[i-2].opcode == OP_ADD);
                // OP_LOAD4
                assert(insns[i-1].opcode == OP_LOAD4);
                // OP_JUMP <- you are here

                // since this code is only ever generated by LCC, but is
                // inexpressible in C, I think we can rely on this code being
                // as it is.
                int first = ofs + (lb << ls);
                int num = ub - lb + 1;
                // the table is like this:
                // if <local> = <v> then goto <L>
                // - where v is always the same local

                // insert all destinations into the leaders
                //leaders = intlist_insert_sorted(leaders, r0);
                assert(0); // not implemented yet, sorry
            }
            break;
        case OP_LEAVE:
            // we treat a leave as a branching instruction
            // unless it's the last one, which we treat as exit node
            if (i+1 < offset+length) {
                // next statement is a leader
                leaders = intlist_insert_sorted(leaders, i+1);
            }
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
        case OP_EQF:
        case OP_NEF:
        case OP_LTF:
        case OP_LEF:
        case OP_GTF:
        case OP_GEF:
            if ((unsigned)r0 >= (unsigned)(offset+length))
            {
                Com_Error("VM program counter out of range in jump");
                return;
            }
            leaders = intlist_insert_sorted(leaders, r0);
            if ((unsigned)(i+1) >= (unsigned)(offset+length))
            {
                Com_Error("VM program counter out of range in jump");
                return;
            }
            leaders = intlist_insert_sorted(leaders, i+1);
        default:
            break;
        }
    }

    intlist_t *leader = leaders;
    cfgnode_t *node;
    int numBlocks = intlist_length(leaders);
    assert(numBlocks > 1); // ENTER + LEAVE at the very minimum
    cfgnode_t **blocks = (cfgnode_t **)malloc(numBlocks * sizeof(cfgnode_t*));

    // create all basic blocks
    for (int i = 0; i < numBlocks; i++) {
        intlist_t *next = leader->next;

        int beg = leader->num;
        int end = ((next != NULL) ? next->num : offset+length);
        int len = end - beg;
        node = (cfgnode_t *)malloc(sizeof(cfgnode_t));
        node->num = i;
        node->start = beg;
        node->length = len;
        node->outgoing = NULL;
        node->incoming = NULL;
        node->opStackOutputDepth = -1; // unset

        blocks[i] = node;
        leader = next;
    }

    cfgnode_t *entry = blocks[0];
    cfgnode_t *exit = blocks[numBlocks-1];
    assert(entry != exit);

    // create all edges
    for (int i = 0; i < numBlocks; i++) {
        cfgnode_t *block = blocks[i];
        cfgnode_t *target;
        cfgbranch_t *branch;

        int lastInstr = block->start + block->length - 1;
        if ((unsigned)lastInstr >= (unsigned)(offset+length)) {
            Com_Error("Last instruction out of range!");
            return;
        }
        insn_t *lastInsn = &insns[lastInstr];
        int r0 = lastInsn->operand;

        switch (lastInsn->opcode) {
        case OP_JUMP:
            assert(lastInstr > offset); // if it isn't, we got a dangling JUMP with no destination!
            assert(insns[lastInstr-1].opcode == OP_CONST);
            r0 = insns[lastInstr-1].operand;

            // find which block the jumped instruction is leader of
            target = find_target(numBlocks, blocks, r0);

            assert(target != NULL);
            branch = cfgbranch_make(target);
            cfgnode_attach(block, branch);
            break;
        case OP_LEAVE:
            // NOTE: OP_LEAVE is also usually last instruction in a function
            // in which case we don't want to have it go to itself
            if (block != exit) {
                branch = cfgbranch_make(exit);
                cfgnode_attach(block, branch);
            }
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
        case OP_EQF:
        case OP_NEF:
        case OP_LTF:
        case OP_LEF:
        case OP_GTF:
        case OP_GEF:
            // here we have two targets! the explicit, or the fallthrough
            target = find_target(numBlocks, blocks, r0);
            assert(target != NULL);
            branch = cfgbranch_make(target);
            cfgnode_attach(block, branch);
            // now handle the usual case: link to next block
            target = i+1 > numBlocks ? exit : blocks[i+1];
            branch = cfgbranch_make(target);
            cfgnode_attach(block, branch);
            break;
        default:
            // just link to next block
            target = i+1 > numBlocks ? exit : blocks[i+1];
            cfgnode_attach(block, target);
            break;
        }
    }

    // now, move all nodes from the temporary array to the list
    // and get rid of the array

    cfg->nodes = NULL;
    // go from end of array and cons onto the list
    // (the list must have the correct order)
    for (int i = numBlocks - 1; i >= 0; i--) {
        cfg->nodes = cfgnodelist_push(cfg->nodes, blocks[i]);
    }
    free(blocks);

    cfg->startNode = entry;
    cfg->endNode = exit;
    cfg->funNumber = funNumber;
}

#if 0
#define QVM_OPCODE_MAX_ARITY 3

typedef struct qvmopcodeinfo_s {
    opcode_t        opcode;

    int             arity_input;
    qvmtype_t       input[QVM_OPCODE_MAX_ARITY];
    int             arity_output;
    qvmtype_t       output[QVM_OPCODE_MAX_ARITY];
} qvmopcodeinfo_t;

/*
ok seems like this table is a tad too simple.
- maintain stack of types
- go over instructions, for each inst:
    - record depth of stack after this instruction

we need number and type of each temporary in a function. or perhaps in a block, since these should not really leak.
- every block needs its own temporaries, but it might return some too! in case the opStack is non-empty on block exit
- and when we see that the block returns different temporaries in different exits, we need to insert "phi" functions
  at its successors
    - erm no. every block which has multiple predecessors will need a phi function.
      - if the predecessors yield values.
    - where control "merges"

block info:
- input opStack shape (e.g. [])
- output opStack shape (e.g. I4 :: [])
- whether or not it has a phi function.


from https://www.csd.uwo.ca/~moreno//CS447/Lectures/IntermediateCode.html/node3.html
you see:

E -> E1 + E2 = E.place:=newtemp; code:=generate(E.place,':=',E1.place,'+',E2.place); E.code:=E1.code || E2.code || code;

but we have this:

[E2, E1, ...] // opStack at execution of instruction
ADD           // the instruction being exected
------------
[E1+E2, ...]  // opStack after executing the instruction

so at this point, we want to have E2 and E1 be the two temporaries for us.
- track temporaries using our own opStack
- all the branches are explicit, so you don't have to think about them.

https://web.stanford.edu/class/archive/cs/cs143/cs143.1128/handouts/240%20TAC%20Examples.pdf
other rules?
[E, ...]     // ALL functions return something. even if it's garbage.
OP_LEAVE n   // return n // our own three-address code with a modifier... wrong? nope it's ok.
----------
[E, ...]     // and we actually retain stack!

---what about function calls in the TAC?
https://web.stanford.edu/class/archive/cs/cs143/cs143.1128/handouts/240%20TAC%20Examples.pdf
- PushParam t4 // push as many as you want to a stack!
- result = Call myfunc // call your function
- PopParams 4; // pop as many as you just pushed (but in qvm, this is done by the callee, in our case myfunc!)
*/

const static qvmopcodeinfo_t qvmopcodeinfo[] = {
    {OP_UNDEF, 0, {}, 0, {}},
    {OP_IGNORE, 0, {}, 0, {}},
    {OP_BREAK, 0, {}, 0, {}},
    {OP_ENTER, 0, {}, 0, {}},
    {OP_LEAVE, 0, {}, 0, {}},
    //{OP_CALL,  // Call subroutine. FIXME: this is variadic! i.e. depends on the subroutine!
    //OP_PUSH,  /* Push to stack. */ FIXME: this puts 1 ANY TYPE value on top of stack
    //OP_POP,   /* Discard top-of-stack. */ // FIXME: this pops 1 ANY TYPE value from top of stack
    //OP_CONST, /* Load constant to stack. */ FIXME: this depends on what constant we got. i.e. it's polymorphic.
    //OP_LOCAL, /* Get local variable. */ // FIXME: this depends on what the local variable's type is there
    {OP_JUMP, 0, {}, 0, {}}, /* Unconditional jump. */

    {OP_EQ, 2, {QT_I4, QT_I4}, 1, {QT_I4}}, /* Compare integers, jump if equal. */
    {OP_NE, 2, {QT_I4, QT_I4}, 1, {QT_I4}}, /* Compare integers, jump if not equal. */

    OP_LTI, /* Compare integers, jump if less-than. */
    //OP_LEI, /* Compare integers, jump if less-than-or-equal. */
    //OP_GTI, /* Compare integers, jump if greater-than. */
    //OP_GEI, /* Compare integers, jump if greater-than-or-equal. */

    //OP_LTU, /* Compare unsigned integers, jump if less-than */
    //OP_LEU, /* Compare unsigned integers, jump if less-than-or-equal */
    //OP_GTU, /* Compare unsigned integers, jump if greater-than */
    //OP_GEU, /* Compare unsigned integers, jump if greater-than-or-equal */

    //OP_EQF, /* Compare floats, jump if equal */
    //OP_NEF, /* Compare floats, jump if not-equal */

    //OP_LTF, /* Compare floats, jump if less-than */
    //OP_LEF, /* Compare floats, jump if less-than-or-equal */
    //OP_GTF, /* Compare floats, jump if greater-than */
    //OP_GEF, /* Compare floats, jump if greater-than-or-equal */

    // P4 -> U1
    OP_LOAD1,  /* Load 1-byte from memory */
    // P4 -> U2
    OP_LOAD2,  /* Load 2-bytes from memory */
    // P4 -> U4
    OP_LOAD4,  /* Load 4-bytes from memory */
    // U1,P4->void
    OP_STORE1, /* Store 1-byte to memory */
    // U2,P4->void
    OP_STORE2, /* Store 2-byte to memory */
    // U4,P4->void
    OP_STORE4, /* *(stack[top-1]) = stack[top] */
    // ?
    OP_ARG,    /* Marshal argument */

    // ?
    OP_BLOCK_COPY, /* memcpy */

    /*-------------------*/

    // U1->S1
    OP_SEX8,  /* Sign-Extend 8-bit */
    // U2->S2
    OP_SEX16, /* Sign-Extend 16-bit */

    // I4->I4
    OP_NEGI, /* Negate integer. */
    // I4,I4->I4
    OP_ADD,  /* Add integers (two's complement). */
    OP_SUB,  /* Subtract integers (two's complement). */
    OP_DIVI, /* Divide signed integers. */
    OP_DIVU, /* Divide unsigned integers. */
    OP_MODI, /* Modulus (signed). */
    OP_MODU, /* Modulus (unsigned). */
    OP_MULI, /* Multiply signed integers. */
    OP_MULU, /* Multiply unsigned integers. */

    // I4,I4->I4 (or U4 too! i.e. two valid types)
    OP_BAND, /* Bitwise AND */
    OP_BOR,  /* Bitwise OR */
    OP_BXOR, /* Bitwise eXclusive-OR */
    OP_BCOM, /* Bitwise COMplement */

    // I4,I4->I4
    OP_LSH,  /* Left-shift */
    OP_RSHI, /* Right-shift (algebraic; preserve sign) */
    OP_RSHU, /* Right-shift (bitwise; ignore sign) */

    // F4->F4
    OP_NEGF, /* Negate float */
    // F4,F4->F4
    OP_ADDF, /* Add floats */
    OP_SUBF, /* Subtract floats */
    OP_DIVF, /* Divide floats */
    OP_MULF, /* Multiply floats */

    // F4->I4
    OP_CVIF, /* Convert to integer from float */
    // I4->F4
    OP_CVFI, /* Convert to float from integer */

    OP_MAX /* Make this the last item */
};
#endif

#define MAX_TEMPS 256

typedef struct tac_s {
    int             opcode;
    int             arg1, arg2;
    int             result;
    int             instrNum; // qvm instruction number
} tac_t;

// NOTE: similar thing to EvalStack_r
// https://github.com/ioquake/ioq3/blob/master/code/qcommon/vm_sparc.c#L753
// - opStack starts at 0 at entry to function!
// - computes stack depth
// - checks types of stack operands
// bytecode instructions are categorized thus:
// - opArgIF: requires integer or float
// - opArg2IF: requires second argument, integer or float
// - opRetIF: returns integer or float
// this will actually help it figure out e.g. if PUSH is a float push or an int push!
//
// also it saves the most recent CONST instruction
// during compilation
// in compile_one_insn
// - this will help to emit immediate constants
// - will also handle CALL and JUMP with a dynamic integer on stack!
//   - dunno about JUMP but CALL might be for calling through a function pointer
//
//
// well it seems to me that OP_ARG is used for transferring stuff from opStack
// into the call stack!
// - CALL is not a variadic thing :)
// - CALL will grow opStack by 1, always
//
// after running the below, I see that:
// - a basic block will only end with something on opStack if it's going to LEAVE anyway
// - seems like otherwise, there will be local variables used to put stuff in them at the end
//
// t = a > 0 ? E1 : E2;
// -->
// both E1 and E2 will get saved to a local at the end.

void EvalStackNode(codeseg_t *seg, cfgnode_t *node, int depth) {

    for (int i = node->start; i < node->start + node->length; i++) {
        switch (seg->insns[i].opcode) {
        case OP_UNDEF:
            // bad instruction! shouldn't happen!
            assert(0);
            break;
        case OP_IGNORE:
            // don't touch the stack
            // completely ignored
            break;
        case OP_BREAK:
            // don't touch the stack
            // completely ignored
            break;
        case OP_ENTER:
        case OP_LEAVE:
            break;
        case OP_CALL:   // Call subroutine.
            // takes 1 (address of routine to call), returns 1 (return value).
            // no change
            break;

        case OP_PUSH:  // Push to stack.
            depth++;
            break;
        case OP_POP:   // Discard top-of-stack.
            depth--;
            break;
        case OP_CONST: // Load constant to stack.
        case OP_LOCAL: // Get local variable. // FIXME: this depends on what the local variable's type is there
            // LOCAL: get address of local variable at offset given as immediate
            depth++;
            break;

        case OP_JUMP: // Unconditional jump.
            depth--;
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
        case OP_EQF:
        case OP_NEF:
        case OP_LTF:
        case OP_LEF:
        case OP_GTF:
        case OP_GEF:
            depth -= 2; // consumes two, puts back zero
            break;

        case OP_LOAD1:  /* Load 1-byte from memory */
        case OP_LOAD2:  /* Load 2-bytes from memory */
        case OP_LOAD4:  /* Load 4-bytes from memory */
            // no change (takes 1, yields 1)
            break;

        case OP_STORE1: /* Store 1-byte to memory */
        case OP_STORE2: /* Store 2-byte to memory */
        case OP_STORE4: /* *(stack[top-1]) = stack[top] */
            depth -= 2;
            break;

        case OP_ARG:
            depth--; // pops value off opStack and pushes into call stack
            break;

        case OP_BLOCK_COPY:
            depth -= 2;
            break;

        case OP_SEX8:  /* Sign-Extend 8-bit */
        case OP_SEX16: /* Sign-Extend 16-bit */
        case OP_NEGI: /* Negate integer. */
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
        case OP_BCOM:
        case OP_LSH:
        case OP_RSHI:
        case OP_RSHU:
            depth--;
            break;
        case OP_NEGF:
            break;
        case OP_ADDF:
        case OP_SUBF:
        case OP_DIVF:
        case OP_MULF:
            depth--;
            break;
        case OP_CVIF:
        case OP_CVFI:
            break;
        default:
            assert(0);
            break;
        }
    }

    node->opStackOutputDepth = depth;
}

void EvalStackR(codeseg_t *seg, cfgnode_t *node, int depth) {
    if (node->opStackOutputDepth >= 0)
        return; // already visited

    EvalStackNode(seg, node, depth);
    cfgnodelist_t *sl = node->outgoing;
    while (sl != NULL) {
        EvalStackR(seg, sl->node, node->opStackOutputDepth);
        sl = sl->next;
    }
}

void EvalStack(codeseg_t *seg, cfg_t *cfg) {
    cfgnode_t       *block = cfg->startNode;

    // NOTE: other nodes are unreachable! we can delete them!
    EvalStackR(seg, cfg->startNode, 0);
}

// how can we move stuff to 3-address code now?
// we have stack depth
// we can have temporaries now!
// and for inserting phi-functions, we can do this:
// - if incoming edges into this block have outStack>0:
//   create newlocal = phi(localAtStack[0])

#if 0
// go over all instructions and annotate the opStack on entry/exit from each node
void AnnotateOpStack(codeseg_t *seg, cfgnode_t *node) {
    uint8_t     stack[OPSTACK_SIZE + 15];
    int*        opStack;
    uint8_t     opStackOfs;
    int         numTemps = 0;
    // TODO: why not generate tac opcodes as well?
    qvmtype_t   tempTypes[MAX_TEMPS];

    // just leave something at the end, so opStack[-1] is OK
    opStack    = PADP(stack, 16);
    *opStack   = 0x0000BEEF;
    opStackOfs = 0;

    int         r0, r1;
    int         ret;

    for (int i = node->start; i < node->start + node->length; i++) {
        insn_t *insn = &seg->insns[i];

        switch (insn->opcode) {
        case OP_UNDEF:
            // bad instruction! shouldn't happen!
            assert(0);
            break;
        case OP_IGNORE:
            // don't touch the stack
            // completely ignored
            break;
        case OP_BREAK:
            // don't touch the stack
            // completely ignored
            break;
/*
    {OP_ENTER, 0, {}, 0, {}},
    {OP_LEAVE, 0, {}, 0, {}},
    //{OP_CALL,  // Call subroutine. FIXME: this is variadic! i.e. depends on the subroutine!
    //OP_PUSH,  // Push to stack.  FIXME: this puts 1 ANY TYPE value on top of stack
    //OP_POP,   // Discard top-of-stack. // FIXME: this pops 1 ANY TYPE value from top of stack
*/
        // Load constant to stack. // FIXME: this depends on what constant we got. i.e. it's polymorphic!
        case OP_CONST:
            assert(numTemps < MAX_TEMPS);
            ret = numTemps++;
            tempTypes[ret] = QT_A4;

            opStackOfs++;
            r1 = r0;
            r0 = opStack[opStackOfs] = ret; // r2

            tac_t tac;
            tac.opcode = insn->opcode;
            tac.arg1 = insn->operand; // everywhere it's a temporary but here it's a constant???
            tac.arg2 = 0;
            tac.result = ret;
            // save it somewhere! we'll need it!
            // snoc into a list?

            break;

        case OP_LOCAL: // Get local variable. // FIXME: this depends on what the local variable's type is there
            assert(numTemps < MAX_TEMPS);
            ret = numTemps++;
            tempTypes[ret] = QT_A4;

            opStackOfs++;
            r1 = r0;
            r0 = opStack[opStackOfs] = ret; // r2

            tac_t tac;
            tac.opcode = insn->opcode;
            tac.arg1 = insn->operand; // everywhere it's a temporary but here it's what? a constant?
            tac.arg2 = 0;
            tac.result = ret;
            // save it somewhere! we'll need it!
            // snoc into a list?

            break;

        case OP_JUMP: // Unconditional jump.
            r0 = opStack[opStackOfs];
            r1 = opStack[(uint8_t)(opStackOfs - 1)];

            tac_t tac;
            tac.opcode = insn->opcode;
            tac.arg1 = r0; // everywhere it's a temporary but here it's a label? what???
            tac.arg2 = 0;
            tac.result = 0;

            opStackOfs--;
            break;
        case OP_EQ:
        case OP_NE:
            r0 = opStack[opStackOfs];
            r1 = opStack[(uint8_t)(opStackOfs - 1)];

            assert(tempTypes[r0] == QT_I4);
            assert(tempTypes[r1] == QT_I4);

            assert(numTemps < MAX_TEMPS);
            ret = numTemps++;
            tempTypes[ret] = QT_I4;

            tac_t tac;
            tac.opcode = insn->opcode;
            tac.arg1 = r0;
            tac.arg2 = r1;
            tac.result = ret;
            // save it somewhere! we'll need it!
            // snoc into a list?

            opStackOfs--;
            opStack[opStackOfs] = ret;
            break;
/*
    // OP_LTI(L) maps to:
    // res = r0 < r1
    // ifZ res goto L
    // or we could just, you know, added our own instruction:
    // - if r1<r2 goto L
    // erm I don't like it. there should be a temporary there!
    // we could also resolve L to be the number of the basic block.
    // nr blocks = nr labels (well if we don't remove deadcode/unreachable code...)

    OP_LTI, // Compare integers, jump if less-than.
    //OP_LEI, // Compare integers, jump if less-than-or-equal.
    //OP_GTI, // Compare integers, jump if greater-than.
    //OP_GEI, // Compare integers, jump if greater-than-or-equal.

    //OP_LTU, // Compare unsigned integers, jump if less-than
    //OP_LEU, // Compare unsigned integers, jump if less-than-or-equal
    //OP_GTU, // Compare unsigned integers, jump if greater-than
    //OP_GEU, // Compare unsigned integers, jump if greater-than-or-equal

    //OP_EQF, // Compare floats, jump if equal
    //OP_NEF, // Compare floats, jump if not-equal

    //OP_LTF, // Compare floats, jump if less-than
    //OP_LEF, // Compare floats, jump if less-than-or-equal
    //OP_GTF, // Compare floats, jump if greater-than
    //OP_GEF, // Compare floats, jump if greater-than-or-equal
*/
        default:
            break;
        }
    }
    return NULL;
}
#endif

// now start sweeping!
// - every ENTER is an entry into function (goes until next ENTER or EOF)
void Sweep(codeseg_t *seg) {
    int         instr = 0;
    int         numInsns = seg->numInsns;
    insn_t      *insns = seg->insns;
    int         start, length;
    int         funNumber = 0;
    cfg_t       cfg;
    char        fpPath[1024];

    memset(&cfg, 0, sizeof(cfg));

    while (instr < numInsns) {
        // first instruction is always ENTER and we maintain that below
        start = instr;
        instr++;
        // look for the next ENTER
        while (instr < numInsns && insns[instr].opcode != OP_ENTER) {
            instr++;
        }
        length = instr - start;

        // start..start+length is a function
        // build its CFG
        //printf("function %d: starts at %d, length %d\n", funNumber, start, length);
        BuildCFG(&cfg, funNumber, seg->insns, length, start);
        // annotate stack depth
        EvalStack(seg, &cfg);
        // TODO: now simplify the graph
        // - via T1-T2 analysis
        // - via controlled node splitting
        snprintf(fpPath, sizeof(fpPath)-1, "/home/artyom/projects/qvm2wasm/example/bytecode_%d.dot", funNumber);
        FILE *fp = fopen(fpPath, "w");
        if (fp != NULL) {
            EmitCFG(&cfg, fp, seg->insns, numInsns);
            fflush(fp);
            fclose(fp);
        }
        FreeCFG(&cfg);
        funNumber++;
    }
}
