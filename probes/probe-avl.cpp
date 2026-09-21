// Exercise ntdll's RtlGenericTableAvl implementation: random inserts/deletes, order, lookup, balance.
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>
typedef enum _RTL_GENERIC_COMPARE_RESULTS { GenericLessThan, GenericGreaterThan, GenericEqual } RTL_GENERIC_COMPARE_RESULTS;
typedef struct _RTL_BALANCED_LINKS { struct _RTL_BALANCED_LINKS *Parent, *LeftChild, *RightChild; CHAR Balance; UCHAR Reserved[3]; } RTL_BALANCED_LINKS;
struct _RTL_AVL_TABLE;
typedef RTL_GENERIC_COMPARE_RESULTS (WINAPI *PRTL_AVL_COMPARE_ROUTINE)(struct _RTL_AVL_TABLE *, void *, void *);
typedef void *(WINAPI *PRTL_AVL_ALLOCATE_ROUTINE)(struct _RTL_AVL_TABLE *, LONG);
typedef void (WINAPI *PRTL_AVL_FREE_ROUTINE)(struct _RTL_AVL_TABLE *, void *);
typedef struct _RTL_AVL_TABLE { RTL_BALANCED_LINKS BalancedRoot; void *OrderedPointer; ULONG WhichOrderedElement; ULONG NumberGenericTableElements; ULONG DepthOfTree; RTL_BALANCED_LINKS *RestartKey; ULONG DeleteCount; PRTL_AVL_COMPARE_ROUTINE CompareRoutine; PRTL_AVL_ALLOCATE_ROUTINE AllocateRoutine; PRTL_AVL_FREE_ROUTINE FreeRoutine; void *TableContext; } RTL_AVL_TABLE;
extern "C" {
void WINAPI RtlInitializeGenericTableAvl(RTL_AVL_TABLE*, PRTL_AVL_COMPARE_ROUTINE, PRTL_AVL_ALLOCATE_ROUTINE, PRTL_AVL_FREE_ROUTINE, void*);
void *WINAPI RtlInsertElementGenericTableAvl(RTL_AVL_TABLE*, void*, ULONG, BOOLEAN*);
void *WINAPI RtlLookupElementGenericTableAvl(RTL_AVL_TABLE*, void*);
BOOLEAN WINAPI RtlDeleteElementGenericTableAvl(RTL_AVL_TABLE*, void*);
void *WINAPI RtlEnumerateGenericTableWithoutSplayingAvl(RTL_AVL_TABLE*, void**);
ULONG WINAPI RtlNumberGenericTableElementsAvl(RTL_AVL_TABLE*);
}
static RTL_GENERIC_COMPARE_RESULTS WINAPI cmp(RTL_AVL_TABLE*, void *a, void *b) { int x = *(int*)a, y = *(int*)b; return x < y ? GenericLessThan : x > y ? GenericGreaterThan : GenericEqual; }
static void *WINAPI alloc(RTL_AVL_TABLE*, LONG n) { return malloc(n); }
static void WINAPI dealloc(RTL_AVL_TABLE*, void *p) { free(p); }
static int height(RTL_BALANCED_LINKS *n, int *bad) {
    if (!n) return 0;
    int l = height(n->LeftChild, bad), r = height(n->RightChild, bad);
    if (r - l != n->Balance || abs(r - l) > 1) (*bad)++;
    if (n->LeftChild && n->LeftChild->Parent != n) (*bad)++;
    if (n->RightChild && n->RightChild->Parent != n) (*bad)++;
    return 1 + (l > r ? l : r);
}
static int check(RTL_AVL_TABLE *t, const char *what, int expect) {
    int bad = 0, count = 0, last = -1; void *key = NULL; void *e;
    height(t->BalancedRoot.RightChild, &bad);
    while ((e = RtlEnumerateGenericTableWithoutSplayingAvl(t, &key))) { int v = *(int*)e; if (v <= last) bad++; last = v; count++; }
    if (count != expect || RtlNumberGenericTableElementsAvl(t) != (ULONG)expect) bad++;
    printf("%s: count %d (expect %d), problems %d\n", what, count, expect, bad);
    return bad;
}
int main() {
    RTL_AVL_TABLE t; RtlInitializeGenericTableAvl(&t, cmp, alloc, dealloc, NULL);
    int present[5000] = {0}, n = 0, bad = 0; srand(1234);
    for (int i = 0; i < 20000; i++) { int v = rand() % 5000; BOOLEAN nw; int *p = (int*)RtlInsertElementGenericTableAvl(&t, &v, sizeof v, &nw); if (!p || *p != v) bad++; if (nw != !present[v]) bad++; if (!present[v]) { present[v] = 1; n++; } }
    bad += check(&t, "after inserts", n);
    for (int i = 0; i < 20000; i++) { int v = rand() % 5000; BOOLEAN r = RtlDeleteElementGenericTableAvl(&t, &v); if (r != !!present[v]) bad++; if (present[v]) { present[v] = 0; n--; } }
    bad += check(&t, "after deletes", n);
    for (int v = 0; v < 5000; v++) { void *e = RtlLookupElementGenericTableAvl(&t, &v); if (!!e != !!present[v]) bad++; }
    for (int v = 0; v < 5000; v++) if (present[v]) { RtlDeleteElementGenericTableAvl(&t, &v); n--; }
    bad += check(&t, "after clearing", 0);
    printf("avl probe: %s (%d problems)\n", bad ? "FAIL" : "OK", bad);
    return bad;
}
