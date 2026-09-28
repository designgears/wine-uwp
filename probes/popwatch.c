/* Host tool, not a Windows probe: build with `gcc -O2 -o popwatch probes/popwatch.c -lX11`, run with DISPLAY set.
 * Usage: popwatch [ms]  (default 400 ms of sampling after each map or resize). */
/* popwatch: log top-level window create/map/configure/unmap on a display, and after each map or configure
 * sample the window's pixels (alpha included) every ~3 ms for SAMPLE_MS, classifying opaque pixels.
 * Frames are saved raw as /tmp/popwatch/w<id>_<ms>.bgra (+ .txt with WxH depth) for viewing. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static int xerr(Display *d, XErrorEvent *e){ return 0; }

static void sample(Display *d, Window w, double until, int save)
{
    int n = 0;
    while (now() < until)
    {
        XWindowAttributes a;
        if (!XGetWindowAttributes(d, w, &a) || a.map_state != IsViewable) break;
        XImage *im = XGetImage(d, w, 0, 0, a.width, a.height, AllPlanes, ZPixmap);
        if (!im) break;
        long transparent = 0, opaque_black = 0, opaque_white = 0, opaque_other = 0, partial = 0;
        for (int y = 0; y < a.height; y++)
            for (int x = 0; x < a.width; x++)
            {
                unsigned long p = XGetPixel(im, x, y);
                unsigned int al = a.depth == 32 ? (p >> 24) & 0xff : 255, r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
                if (!al) transparent++;
                else if (al < 255) partial++;
                else if (r < 16 && g < 16 && b < 16) opaque_black++;
                else if (r > 240 && g > 240 && b > 240) opaque_white++;
                else opaque_other++;
            }
        printf("%.3f   sample w %lx %dx%d depth %d: transparent %ld partial %ld black %ld white %ld other %ld\n", now(), w,
               a.width, a.height, a.depth, transparent, partial, opaque_black, opaque_white, opaque_other);
        if (save && n < 40)
        {
            char path[256]; FILE *f;
            snprintf(path, sizeof(path), "/tmp/popwatch/w%lx_%03d.bgra", w, n);
            if ((f = fopen(path, "wb")))
            {
                for (int y = 0; y < a.height; y++)
                    for (int x = 0; x < a.width; x++)
                    {
                        unsigned long p = XGetPixel(im, x, y);
                        unsigned char px[4] = { p & 0xff, (p >> 8) & 0xff, (p >> 16) & 0xff, a.depth == 32 ? (p >> 24) & 0xff : 255 };
                        fwrite(px, 1, 4, f);
                    }
                fclose(f);
                snprintf(path, sizeof(path), "/tmp/popwatch/w%lx_%03d.txt", w, n);
                if ((f = fopen(path, "w"))) { fprintf(f, "%d %d %d %.3f\n", a.width, a.height, a.depth, now()); fclose(f); }
            }
        }
        XDestroyImage(im);
        n++;
        fflush(stdout);
        usleep(3000);
        /* stop early on new events, the caller handles them */
        if (XPending(d)) break;
    }
}

int main(int argc, char **argv)
{
    Display *d = XOpenDisplay(NULL);
    int ms = argc > 1 ? atoi(argv[1]) : 400;
    if (!d) return 1;
    XSetErrorHandler(xerr);
    XSelectInput(d, DefaultRootWindow(d), SubstructureNotifyMask);
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (;;)
    {
        XEvent e; Window w = 0; XWindowAttributes a;
        XNextEvent(d, &e);
        switch (e.type)
        {
        case CreateNotify:
            printf("%.3f create %lx %dx%d+%d+%d override %d\n", now(), e.xcreatewindow.window, e.xcreatewindow.width,
                   e.xcreatewindow.height, e.xcreatewindow.x, e.xcreatewindow.y, e.xcreatewindow.override_redirect);
            break;
        case MapNotify:
            w = e.xmap.window;
            memset(&a, 0, sizeof(a)); XGetWindowAttributes(d, w, &a);
            printf("%.3f map %lx %dx%d+%d+%d depth %d override %d\n", now(), w, a.width, a.height, a.x, a.y, a.depth, a.override_redirect);
            if (a.width > 16 && a.height > 16 && a.override_redirect) sample(d, w, now() + ms / 1000.0, 1);
            break;
        case ConfigureNotify:
            w = e.xconfigure.window;
            printf("%.3f configure %lx %dx%d+%d+%d\n", now(), w, e.xconfigure.width, e.xconfigure.height, e.xconfigure.x, e.xconfigure.y);
            memset(&a, 0, sizeof(a));
            if (XGetWindowAttributes(d, w, &a) && a.override_redirect && a.map_state == IsViewable && a.width > 16)
                sample(d, w, now() + ms / 1000.0, 1);
            break;
        case UnmapNotify: printf("%.3f unmap %lx\n", now(), e.xunmap.window); break;
        case DestroyNotify: printf("%.3f destroy %lx\n", now(), e.xdestroywindow.window); break;
        }
    }
}
