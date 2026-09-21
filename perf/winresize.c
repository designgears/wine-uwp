#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
/* winresize <title> <width> <height>: resize a top-level window the way a user drag would end */
int main(int argc, char **argv)
{
    HWND hwnd;
    if (argc < 4) return 2;
    if (!(hwnd = FindWindowA(NULL, argv[1]))) { printf("no window %s\n", argv[1]); return 1; }
    SetWindowPos(hwnd, NULL, 0, 0, atoi(argv[2]), atoi(argv[3]), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    printf("resized %p to %sx%s\n", hwnd, argv[2], argv[3]);
    return 0;
}
