#!/usr/bin/env python3
"""Exercise the actual composition minifier, including strokes bilinear sampling skips."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'wine/dlls/dcomp/composition.c').read_text()
start = source.index('static void sample_bgra_area(')
end = source.index('\n/* OP_BLEND', start)
program = r'''
#include <math.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef unsigned char BYTE;
typedef int BOOL;
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))
''' + source[start:end] + r'''
static void near(float actual, float expected) { assert(fabsf(actual - expected) < 0.01f); }
int main(void) {
    BYTE pixels[5 * 5 * 4] = {0};
    float out[4];
    /* A one-texel vertical stroke missed by a centre-only bilinear sample. */
    for (int y = 0; y < 5; y++)
        pixels[(y * 5) * 4] = pixels[(y * 5) * 4 + 3] = 255;
    sample_bgra_area(pixels, 5, 5, 20, 2.5f, 2.5f, 5, 5, 0, out);
    near(out[0], 51); near(out[3], 51); near(out[1], 0);
    /* Magnification on one axis, reduction on the other. */
    sample_bgra_area(pixels, 5, 5, 20, 2.5f, 1.5f, 5, 0.5f, 0, out);
    near(out[0], 51); near(out[3], 51);
    /* Fractional pixel overlap, preserving premultiplied transparency. */
    sample_bgra_area(pixels, 5, 5, 20, 1.25f, 2.5f, 2.5f, 5, 0, out);
    near(out[0], 102); near(out[3], 102);
    /* Edge extension is explicit, rather than normalizing away transparent edges. */
    sample_bgra_area(pixels, 5, 5, 20, 0, 2.5f, 2, 5, 0, out);
    near(out[0], 127.5f); near(out[3], 127.5f);
    sample_bgra_area(pixels, 5, 5, 20, 0, 2.5f, 2, 5, 1, out);
    near(out[0], 255); near(out[3], 255);
    /* Unscaled samples stay exact. */
    sample_bgra_area(pixels, 5, 5, 20, 0.5f, 2.5f, 1, 1, 1, out);
    near(out[0], 255); near(out[3], 255);
    puts("PASS: thin stroke, anisotropic/fractional reduction, premultiplied edges, 1:1");
}
'''
with tempfile.TemporaryDirectory(prefix='wine-uwp-area-') as directory:
    c = Path(directory) / 'check.c'
    exe = Path(directory) / 'check'
    c.write_text(program)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', str(c), '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
