/*
 * xcap: capture an X screen with one XGetImage of the root window, as a PPM on stdout.
 *
 *   gcc -O2 -o xcap probes/xcap.c -lX11
 *   DISPLAY=:97 ./xcap > s.ppm && magick s.ppm s.png
 *
 * This is what is on the screen. `xwd -root` is not, whenever windows use a colormap other than the root's (Wine's
 * do): it then takes its multi-visual path, which merges every window with the same visual and colormap into one region
 * and reads that whole region back from the first such window it found. A Wine popup over a Wine main window (the
 * Xbox app's purchase dialog, XAML's windowed menus) is then read out of the main window, where it is obscured, and
 * comes back black although the screen shows it.
 */
#include <stdio.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

int main( void )
{
    Display *display = XOpenDisplay( NULL );
    XWindowAttributes attr;
    XImage *image;
    Window root;
    int x, y;

    if (!display) return 1;
    root = DefaultRootWindow( display );
    XGetWindowAttributes( display, root, &attr );
    if (!(image = XGetImage( display, root, 0, 0, attr.width, attr.height, AllPlanes, ZPixmap ))) return 1;
    printf( "P6\n%d %d\n255\n", attr.width, attr.height );
    for (y = 0; y < attr.height; y++)
        for (x = 0; x < attr.width; x++)
        {
            unsigned long pixel = XGetPixel( image, x, y );
            putchar( (pixel & image->red_mask) >> 16 );
            putchar( (pixel & image->green_mask) >> 8 );
            putchar( pixel & image->blue_mask );
        }
    XDestroyImage( image );
    XCloseDisplay( display );
    return 0;
}
