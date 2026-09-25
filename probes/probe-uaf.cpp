// probe-uaf: write into a freed heap block, then churn the heap so it leaves the delayed-free ring.
// Run with WINEDEBUG=warn+heap; ntdll should report "written after free" for the block.
#include <windows.h>
#include <stdio.h>

int main()
{
    HANDLE heap = GetProcessHeap();
    void **p = (void **)HeapAlloc(heap, 0, 256);
    printf("block %p\n", p);
    HeapFree(heap, 0, p);
    p[3] = (void *)&main;   // the use after free
    for (int i = 0; i < 20000; i++) HeapFree(heap, 0, HeapAlloc(heap, 0, 64 + (i % 7) * 16));
    printf("done\n");
    return 0;
}
