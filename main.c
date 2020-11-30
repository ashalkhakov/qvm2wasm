#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "vm.h"
#include "domlt.h"

intptr_t SystemCalls(vm_t* vm, intptr_t* args)
{
    const int id = -1 - args[0];

    switch (id)
    {
    case -1: /* print_int */
        return printf("%d", args[1]);
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

void test_dominators1() {
    // from wikipedia
    adjdigraph_t g;
    adjdigraph_make(&g, 6, 0);
    adjdigraph_insert_edge(&g, 0, 1);
    adjdigraph_insert_edge(&g, 1, 2);
    adjdigraph_insert_edge(&g, 1, 3);
    adjdigraph_insert_edge(&g, 1, 5);
    adjdigraph_insert_edge(&g, 2, 4);
    adjdigraph_insert_edge(&g, 3, 4);
    adjdigraph_insert_edge(&g, 4, 1);

    char fpPath[1024];
    snprintf(fpPath, sizeof(fpPath)-1, "/home/artyom/projects/qvm2wasm/example/test_dominators1.dot");
    FILE *fp = fopen(fpPath, "w");
    if (fp != NULL) {
        adjdigraph_dot(&g, fp);
        fflush(fp);
        fclose(fp);
    }

    int *idom = DominatorTree(&g);
    assert(idom[0] == -1); // the entry node has no immediate dominator
    assert(idom[1] == 0);
    assert(idom[2] == 1);
    assert(idom[3] == 1);
    assert(idom[4] == 1);
    assert(idom[5] == 1);

    adjdigraph_free(&g);
    free(idom);

}

void test_dominators2() {
    // from the paper

    /*
    nodes:
    R
    A,B,C
    D,E,F,G
    L,H,I,J
    K
    */
    #define R 0
    #define A 1
    #define B 2
    #define C 3
    #define D 4
    #define E 5
    #define F 6
    #define G 7
    #define L 8
    #define H 9
    #define I 10
    #define J 11
    #define K 12 
    #define gE(x,y) adjdigraph_insert_edge(&g, (x), (y));

    adjdigraph_t g;
    adjdigraph_make(&g, 13, 0);

    // edges
    gE(R, A)
    gE(R, B)
    gE(R, C)
    gE(A, D)
    gE(B, A)
    gE(B, D)
    gE(B, E)
    gE(C, F)
    gE(C, G)
    gE(D, L)
    gE(E, H)
    gE(F, I)
    gE(G, I)
    gE(G, J)
    gE(H, K)
    gE(H, E)
    gE(I, K)
    gE(J, I)
    gE(K, R)
    gE(K, I)

    int *idom = DominatorTree(&g);

    #define CHK(x,y) assert(idom[(x)] == (y));
    CHK(R,-1)
    CHK(A,R)
    CHK(B,R)
    CHK(C,R)
    CHK(D,R)
    CHK(E,R)
    CHK(H,R)
    CHK(I,R) // doesn't go deep enough.
    CHK(K,R)
    CHK(F,C)
    CHK(G,C)
    CHK(J,G)
    CHK(L,D)

    #undef R
    #undef A
    #undef B
    #undef C
    #undef D
    #undef E
    #undef F
    #undef G
    #undef L
    #undef H
    #undef I
    #undef J
    #undef K 
    #undef gE
    #undef CHK
}

int main()
{
    vm_t vm;
    int  retVal = -1;
    int  imageSize;

    /*if (argc < 2)
    {
        printf("No virtual machine supplied. Example: q3vm bytecode.qvm\n");
        return retVal;
    }*/

    // test dominators
    //test_dominators1();
    //test_dominators2();

    /* load virtual machine image from file */
    char*    filepath = "/home/artyom/projects/q3vm/example/bytecode.qvm";//argv[1];
    uint8_t* image    = loadImage(filepath, &imageSize);
    if (!image)
    {
        return -1;
    }

    /* set-up virtual machine */
    if (VM_Create(&vm, filepath, image, imageSize, SystemCalls) == 0)
    {
        /* call virtual machine vmMain() with integer argument (here 0) */
        retVal = VM_Call(&vm, 0);
    }
    /* output profile information in DEBUG_VM build: */
    VM_VmProfile_f(&vm);
    VM_Free(&vm);
    free(image); /* we can release the memory now */

    return retVal;
}
