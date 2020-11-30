#ifndef __IR_H__
#define __IR_H__

#include <stdint.h>

// expanded instruction
typedef struct insn_s {
    uint8_t    opcode;     // qvm opcode
    int        operand;    // operand, if applicable (not applicable to OP_JUMP!)
} insn_t;

typedef struct codeseg_s {
    int     numInsns;
    insn_t  *insns;
} codeseg_t;

void FreeCode(codeseg_t *seg);
void ExpandCode(codeseg_t *seg, uint8_t *code, int length, int instructionCount);
void PrintCode(codeseg_t *seg);
void Sweep(codeseg_t *seg);

typedef struct cfgnodelist_s cfgnodelist_t;
typedef struct cfgbranchlist_s cfgbranchlist_t;
typedef struct cfgnode_s cfgnode_t;

/*
how to do a better design?
- when we contract edges and join nodes, what does it give us?
  - more info in the node?
  - AST ?
    - allow AST To be embedded inside a node?
    - what about the other way around? can an AST node contain a subgraph? yes.
        - no; T1-T2 analysis always works on ONE node. it's local; and the node is "joined" to another node.
        - yeeeeehaaaaaw.

type ast =
| Span of (int, int) // start+length NOTE: no control flow here!!!
| If of (ast, ast)
| Loop of asts // "branch" moves back to start of this
| Block of asts // "branch" short-circuits this
| Branch of (int) // level NOTE: this is like a de Bruijn index

and asts = list ast

we can build ast by successive T1/T2 ops.

Apr 23: graph dominators again... https://tanujkhattar.wordpress.com/2016/01/11/dominator-tree-of-a-directed-graph/
NOTE: This is tight on defs, good! http://www.cs.uoi.gr/~loukas/index.files/domsurvey.pdf

naive approach:
G = (N, E, s)
dom(i) where i in N is the set of dominators of i

-- this function does dfs and marks all visited nodes as reachable,
-- returns set of visited nodes (aka reachable)
init_r := dfs_reachable(s) -- mark all vertices reachable from s

dom(s) = {s}
for each node w in N:
  remove w from G (i.e. remove the node and all its in-edges/out-edges)
  new_r := dfs_reachable(s)
  dom(w) := { i in init_r | i not in new_r } // all the vertices which were earlier visited but not now

faster approach:

num(i) for each node i
- integer number
- corresponds to "arrival time" of the node in dfs from s
  - i.e. the dfs step will "stamp" every node uniquely

sdom(w) for each node w (semi-dominator)
- undefined for s
- min {v | exists v:path. v=[v0,v1,...,vk] -> forall 1<=i<=k-1 -> vi > w}
  - FIXME: doesn't this operate on "num"s defined above???
    - oh, they probably define ">" via num()
  - take minimum of a set of paths, each path being a list of nodes such
    that for two consecutive nodes in this list, they are connected by an edge in E
*/

typedef enum qvmtype_s {
    QT_N = 0, // not used

    QT_B,     // octet sequence
    QT_F4,    // 32-bit float
    QT_P4,    // 32-bit pointer (unsigned int)
    QT_I4,    // 32-bit int
    QT_I2,    // 16-bit int
    QT_I1,    // 8-bit int
    QT_U4,    // 32-bit unsigned int
    QT_U2,    // 16-bit unsigned int
    QT_U1,    // 8-bit unsigned int

    QT_A4,    // anything (32-bit)

    QT_ALL_TYPES
} qvmtype_t;

/*typedef struct qvmtypestack_s {
    qvmtype_t       qtype;
    qvmtypestack_t *next;
} qvmtypestack_t;*/

typedef enum tackind_s {
  TAC_ASSGN = 0,
  TAC_BINOP, // ops?
  TAC_UNOP, // ops?
  TAC_BR,   // unconditional branch
  TAC_BR_COND
} tackind_t;

typedef struct tacinsn_s {
    int   qvm_insn;   // points back to the qvm instruction
    int   kind;       // kind of instruction
    int   arg1, arg2; // argument temporaries
    int   result;     // result temporary
} tacinsn_t;

typedef struct cfgbranch_s {
    int opcode;   // OP_NOP means unconditional
    int local;    // local variable used for comparison (only if opcode is present)
    int constant; // constant used for comparison (only if opcode is present)
    cfgnode_t *node; // the node to jump to
} cfgbranch_t;

struct cfgnode_s {
    int             num;        // id of the node (always increasing)

    int             start;      // start instruction (qvm)
    int             length;     // full length of block (qvm)

    // NOTE: initially -1
    int             opStackOutputDepth; // opStack on exiting the node

    int             numInsns;       // number of instructions
    tacinsn_t       *insns;         // three-address code instructions

    cfgnodelist_t   *incoming;  // incoming
    cfgbranchlist_t *outgoing;  // outgoing

};

struct cfgnodelist_s {
    cfgnodelist_t *next;     // At beginning for list_append()
    cfgnode_t *node;
};

struct cfgbranchlist_s {
    cfgbranchlist_t *next;
    cfgbranch_t *branch;
};

typedef struct cfg_s {
    int         funNumber;  // function id
    int         numTemps;   // total number of temporaries in CFG
    cfgnode_t   *startNode; // NOTE: this is same as [nodes]!
    cfgnode_t   *endNode;   // NOTE: this is last node in [nodes]!
    cfgnodelist_t *nodes;
} cfg_t;

#endif /* !__IR_H__ */
