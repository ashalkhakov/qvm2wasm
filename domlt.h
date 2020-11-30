#include <stdio.h>

typedef struct intlist_s {
    int                 n;
    struct intlist_s   *next;
} intlist_t;

// directed graph represented as adjacency list for each node
typedef struct adjdigraph_s {
    int         numVerts;   // number of vertices
    intlist_t   **succ;     // outgoing edges for each vertex
    int         r;          // the starting node
} adjdigraph_t;

void adjdigraph_make(adjdigraph_t *g, int numVerts, int r);
void adjdigraph_insert_edge(adjdigraph_t *g, int v, int w);
void adjdigraph_free(adjdigraph_t *g);
void adjdigraph_dot(adjdigraph_t *g, FILE *fp);

// input:
//  - a flowgraph
// output:
//  - an array d of size g->numVerts,
//    containing for each vertex v != r, d[v] = idom(v)
int *DominatorTree(adjdigraph_t *g);
