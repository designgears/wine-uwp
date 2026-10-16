/*
 * ID2D1SvgDocument
 *
 * Copyright 2026 Bobby Kircher
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* An SVG document parsed into an element tree, and a renderer for the static subset UI icon sets
 * are made of: svg, g, path, rect, circle, ellipse, line, polyline and polygon; fill and stroke
 * with their opacities, widths, caps, joins, miter limit and dashes; fill-rule, opacity,
 * transform, and clip-path through a clipPath in defs. Gradients, patterns, filters, masks, use
 * and image are not drawn (one FIXME each). The document keeps the XML it was made from, which is
 * what Serialize writes back; the element object model (GetRoot, SetRoot, the Create* methods) is
 * not implemented.
 *
 * Win2D's CanvasSvgDocument sits on this, and Shapr3D loads its toolbar and constraint icons
 * through it as the editor opens: CreateSvgDocument's E_NOTIMPL came back as an uncaught
 * hresult_error on the render thread and aborted the app. */

#include <float.h>

#include "d2d1_private.h"
#include "wincodec.h"

WINE_DEFAULT_DEBUG_CHANNEL(d2d);

/* d2d1svg.h; present in Win2D, which is how the value was checked */
static const GUID IID_ID2D1SvgElement = {0xac7b67a6, 0x183e, 0x49c1, {0xa8, 0x23, 0x0e, 0xbe, 0x40, 0xb0, 0xdb, 0x29}};

struct svg_attr
{
    WCHAR *name;
    WCHAR *value;
};

struct svg_element;

struct svg_node
{
    struct svg_node *parent;
    struct svg_element *element;   /* its ID2D1SvgElement, if one has been handed out */
    WCHAR *name;
    struct svg_attr *attrs;
    size_t attr_count;
    struct svg_node **children;
    size_t child_count, child_capacity;
};

struct d2d_svg_document
{
    ID2D1SvgDocument ID2D1SvgDocument_iface;
    LONG refcount;

    ID2D1Factory *factory;
    D2D1_SIZE_F viewport;
    BYTE *xml;
    SIZE_T xml_size;
    struct svg_node *root;
    BOOL modified;   /* Serialize writes the tree instead of the original XML */
    struct svg_node **detached;   /* removed or created nodes not in the tree, freed with it */
    size_t detached_count;
};

static const ID2D1SvgDocumentVtbl d2d_svg_document_vtbl;

static inline struct d2d_svg_document *impl_from_ID2D1SvgDocument(ID2D1SvgDocument *iface)
{
    return CONTAINING_RECORD(iface, struct d2d_svg_document, ID2D1SvgDocument_iface);
}

static void svg_node_free(struct svg_node *node)
{
    size_t i;

    if (!node) return;
    for (i = 0; i < node->attr_count; ++i)
    {
        free(node->attrs[i].name);
        free(node->attrs[i].value);
    }
    for (i = 0; i < node->child_count; ++i)
        svg_node_free(node->children[i]);
    free(node->attrs);
    free(node->children);
    free(node->name);
    free(node);
}

static const WCHAR *svg_attr(const struct svg_node *node, const WCHAR *name)
{
    size_t i;

    for (i = 0; i < node->attr_count; ++i)
        if (!wcscmp(node->attrs[i].name, name)) return node->attrs[i].value;
    return NULL;
}

/* ---------------------------------------------------------------------------------------------
 * Parsing the XML
 * ------------------------------------------------------------------------------------------- */
/* A small XML reader for SVG documents: elements, attributes, self-closing tags and the five
 * predefined and numeric entities; comments, processing instructions, CDATA and the doctype are
 * skipped, text is not kept. It used xmllite at first, which faulted in a memmove on one of
 * Shapr3D's documents, and SVG icon markup needs no more than this. Input is UTF-8, or UTF-16
 * with a byte order mark. */
static const WCHAR *find_char(const WCHAR *str, WCHAR c, size_t len)
{
    for (; len; --len, ++str) if (*str == c) return str;
    return NULL;
}

static WCHAR *strndup_w(const WCHAR *str, size_t len)
{
    WCHAR *out = malloc((len + 1) * sizeof(WCHAR));
    if (!out) return NULL;
    memcpy(out, str, len * sizeof(WCHAR));
    out[len] = 0;
    return out;
}

static WCHAR *decode_entities(const WCHAR *start, size_t len)
{
    WCHAR *out, *o;
    size_t i;

    if (!(out = malloc((len + 1) * sizeof(WCHAR)))) return NULL;
    for (i = 0, o = out; i < len; ++i)
    {
        const WCHAR *semi;
        if (start[i] != '&' || !(semi = find_char(start + i, ';', len - i)))
        {
            *o++ = start[i];
            continue;
        }
        if (!wcsncmp(start + i, L"&lt;", 4)) *o++ = '<';
        else if (!wcsncmp(start + i, L"&gt;", 4)) *o++ = '>';
        else if (!wcsncmp(start + i, L"&amp;", 5)) *o++ = '&';
        else if (!wcsncmp(start + i, L"&quot;", 6)) *o++ = '"';
        else if (!wcsncmp(start + i, L"&apos;", 6)) *o++ = '\'';
        else if (start[i + 1] == '#')
            *o++ = start[i + 2] == 'x' ? wcstoul(start + i + 3, NULL, 16) : wcstoul(start + i + 2, NULL, 10);
        else
        {
            *o++ = start[i];
            continue;
        }
        i = semi - start;
    }
    *o = 0;
    return out;
}

static BOOL is_name_char(WCHAR c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == ':' || c == '-' || c == '_' || c == '.' || c >= 0x80;
}

static BOOL is_space(WCHAR c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* The local name, without a namespace prefix: "svg:path" is "path". */
static WCHAR *element_name(const WCHAR *start, size_t len)
{
    const WCHAR *colon = find_char(start, ':', len);
    if (colon) { len -= colon + 1 - start; start = colon + 1; }
    return strndup_w(start, len);
}

static HRESULT svg_parse(struct d2d_svg_document *document)
{
    struct svg_node *stack[64], *node, *parent;
    unsigned int depth = 0;
    const WCHAR *p, *end;
    WCHAR *text;
    size_t len;

    /* the document as UTF-16: with a byte order mark, or without one, which is how Win2D's
     * LoadFromXml hands over a string - recognisable from any ASCII character's zero high byte */
    if (document->xml_size >= 2 && ((document->xml[0] == 0xff && document->xml[1] == 0xfe) ||
                                    (document->xml[0] && !document->xml[1])))
    {
        SIZE_T skip = document->xml[0] == 0xff ? 2 : 0;
        len = (document->xml_size - skip) / sizeof(WCHAR);
        if (!(text = malloc((len + 1) * sizeof(WCHAR)))) return E_OUTOFMEMORY;
        memcpy(text, document->xml + skip, len * sizeof(WCHAR));
    }
    else
    {
        const char *bytes = (const char *)document->xml;
        SIZE_T size = document->xml_size;
        if (size >= 3 && !memcmp(bytes, "\xef\xbb\xbf", 3)) { bytes += 3; size -= 3; }
        len = MultiByteToWideChar(CP_UTF8, 0, bytes, size, NULL, 0);
        if (!(text = malloc((len + 1) * sizeof(WCHAR)))) return E_OUTOFMEMORY;
        MultiByteToWideChar(CP_UTF8, 0, bytes, size, text, len);
    }
    text[len] = 0;

    for (p = text, end = text + len; p < end;)
    {
        const WCHAR *name;

        if (*p != '<')
        {
            ++p;
            continue;
        }
        if (!wcsncmp(p, L"<!--", 4))
        {
            const WCHAR *close = wcsstr(p + 4, L"-->");
            p = close ? close + 3 : end;
            continue;
        }
        if (!wcsncmp(p, L"<![CDATA[", 9))
        {
            const WCHAR *close = wcsstr(p + 9, L"]]>");
            p = close ? close + 3 : end;
            continue;
        }
        if (p[1] == '?' || p[1] == '!')
        {
            /* a doctype may carry an internal subset in brackets */
            const WCHAR *q = p + 2;
            int brackets = 0;
            for (; q < end && (*q != '>' || brackets); ++q)
            {
                if (*q == '[') ++brackets;
                else if (*q == ']') --brackets;
            }
            p = q < end ? q + 1 : end;
            continue;
        }
        if (p[1] == '/')
        {
            const WCHAR *close = wcschr(p, '>');
            if (depth) --depth;
            p = close ? close + 1 : end;
            continue;
        }

        /* a start tag */
        name = ++p;
        while (p < end && is_name_char(*p)) ++p;
        if (p == name) continue;
        if (!(node = calloc(1, sizeof(*node))) || !(node->name = element_name(name, p - name)))
        {
            free(node);
            free(text);
            return E_OUTOFMEMORY;
        }

        for (;;)
        {
            const WCHAR *attr_name, *value;
            struct svg_attr *attrs;
            WCHAR quote;
            size_t name_len;

            while (p < end && is_space(*p)) ++p;
            if (p >= end || *p == '>' || *p == '/') break;
            attr_name = p;
            while (p < end && is_name_char(*p)) ++p;
            if ((name_len = p - attr_name) == 0) { ++p; continue; }
            while (p < end && is_space(*p)) ++p;
            if (p >= end || *p != '=') continue;
            ++p;
            while (p < end && is_space(*p)) ++p;
            if (p >= end || (*p != '"' && *p != '\'')) continue;
            quote = *p++;
            value = p;
            while (p < end && *p != quote) ++p;
            if (!(attrs = realloc(node->attrs, (node->attr_count + 1) * sizeof(*attrs)))) break;
            node->attrs = attrs;
            attrs[node->attr_count].name = strndup_w(attr_name, name_len);
            attrs[node->attr_count].value = decode_entities(value, p - value);
            if (attrs[node->attr_count].name && attrs[node->attr_count].value) ++node->attr_count;
            else
            {
                free(attrs[node->attr_count].name);
                free(attrs[node->attr_count].value);
            }
            if (p < end) ++p;
        }

        if (!depth)
        {
            if (document->root) svg_node_free(node), node = NULL;
            else document->root = node;
        }
        else
        {
            parent = stack[depth - 1];
            if (parent->child_count == parent->child_capacity)
            {
                size_t capacity = max(4, parent->child_capacity * 2);
                struct svg_node **children = realloc(parent->children, capacity * sizeof(*children));
                if (!children)
                {
                    svg_node_free(node);
                    free(text);
                    return E_OUTOFMEMORY;
                }
                parent->children = children;
                parent->child_capacity = capacity;
            }
            parent->children[parent->child_count++] = node;
            node->parent = parent;
        }

        if (p < end && *p == '/')
        {
            /* self-closing */
            const WCHAR *close = wcschr(p, '>');
            p = close ? close + 1 : end;
        }
        else
        {
            if (p < end) ++p;
            if (node && depth < ARRAY_SIZE(stack)) stack[depth++] = node;
            else if (node) FIXME("elements nested deeper than %u are flattened.\n", (unsigned int)ARRAY_SIZE(stack));
        }
    }

    free(text);
    if (!document->root || wcscmp(document->root->name, L"svg")) return D2DERR_INVALID_CALL;
    return S_OK;
}

/* ---------------------------------------------------------------------------------------------
 * Values
 * ------------------------------------------------------------------------------------------- */
static const WCHAR *skip_space(const WCHAR *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') ++p;
    return p;
}

/* A number, SVG style: "-1.5e3", ".5", "1.5.5" is two numbers. Returns NULL if there is none. */
static const WCHAR *parse_number(const WCHAR *p, float *value)
{
    const WCHAR *start;
    BOOL digits = FALSE, dot = FALSE;
    WCHAR buf[64];
    size_t len;

    p = skip_space(p);
    start = p;
    if (*p == '+' || *p == '-') ++p;
    for (;; ++p)
    {
        if (*p >= '0' && *p <= '9') digits = TRUE;
        else if (*p == '.' && !dot) dot = TRUE;
        else break;
    }
    if (!digits) return NULL;
    if (*p == 'e' || *p == 'E')
    {
        const WCHAR *e = p + 1;
        if (*e == '+' || *e == '-') ++e;
        if (*e >= '0' && *e <= '9')
        {
            p = e;
            while (*p >= '0' && *p <= '9') ++p;
        }
    }
    len = min(p - start, ARRAY_SIZE(buf) - 1);
    memcpy(buf, start, len * sizeof(WCHAR));
    buf[len] = 0;
    *value = wcstod(buf, NULL);
    return p;
}

/* An arc flag is a single 0 or 1, and may be written with no separator after it. */
static const WCHAR *parse_flag(const WCHAR *p, BOOL *flag)
{
    p = skip_space(p);
    if (*p != '0' && *p != '1') return NULL;
    *flag = *p == '1';
    return p + 1;
}

static float attr_float(const struct svg_node *node, const WCHAR *name, float def)
{
    const WCHAR *value = svg_attr(node, name);
    float f;
    return value && parse_number(value, &f) ? f : def;
}

static const struct { const WCHAR *name; UINT32 rgb; } named_colours[] =
{
    {L"black", 0x000000}, {L"white", 0xffffff}, {L"red", 0xff0000}, {L"green", 0x008000},
    {L"blue", 0x0000ff}, {L"yellow", 0xffff00}, {L"gray", 0x808080}, {L"grey", 0x808080},
    {L"orange", 0xffa500}, {L"transparent", 0},
};

/* Returns FALSE for "none" (and for paints this renderer cannot do). */
static BOOL parse_paint(const WCHAR *value, D2D1_COLOR_F *colour)
{
    UINT32 rgb = 0;
    float alpha = 1.0f;
    size_t i, len;

    if (!value) return FALSE;
    value = skip_space(value);
    len = wcslen(value);
    while (len && (value[len - 1] == ' ' || value[len - 1] == '\t')) --len;

    if (!wcsncmp(value, L"none", 4)) return FALSE;
    if (!wcsncmp(value, L"url(", 4))
    {
        FIXME("paint server %s is not drawn.\n", debugstr_w(value));
        return FALSE;
    }
    if (!wcsncmp(value, L"currentColor", 12)) rgb = 0;
    else if (*value == '#')
    {
        WCHAR hex[9] = {0};
        size_t n = min(len - 1, 8);
        memcpy(hex, value + 1, n * sizeof(WCHAR));
        rgb = wcstoul(hex, NULL, 16);
        if (n == 3) rgb = ((rgb & 0xf00) * 0x1100) | ((rgb & 0xf0) * 0x110) | ((rgb & 0xf) * 0x11);
        else if (n == 8) { alpha = (rgb & 0xff) / 255.0f; rgb >>= 8; }
    }
    else if (!wcsncmp(value, L"rgb", 3))
    {
        const WCHAR *p = wcschr(value, '(');
        float c[4] = {0, 0, 0, 1};
        unsigned int k;
        for (k = 0; p && k < 4; ++k)
        {
            if (!(p = parse_number(p + (k ? 0 : 1), &c[k]))) break;
            if (*p == '%') { c[k] *= 2.55f; ++p; }
        }
        rgb = ((UINT32)min(255, max(0, c[0])) << 16) | ((UINT32)min(255, max(0, c[1])) << 8)
                | (UINT32)min(255, max(0, c[2]));
        alpha = c[3];
    }
    else
    {
        for (i = 0; i < ARRAY_SIZE(named_colours); ++i)
            if (!wcsncmp(value, named_colours[i].name, len) && wcslen(named_colours[i].name) == len)
                break;
        if (i == ARRAY_SIZE(named_colours))
        {
            FIXME("colour %s is not known, using black.\n", debugstr_w(value));
            rgb = 0;
        }
        else
        {
            rgb = named_colours[i].rgb;
            if (!rgb && !wcscmp(named_colours[i].name, L"transparent")) alpha = 0.0f;
        }
    }
    colour->r = ((rgb >> 16) & 0xff) / 255.0f;
    colour->g = ((rgb >> 8) & 0xff) / 255.0f;
    colour->b = (rgb & 0xff) / 255.0f;
    colour->a = alpha;
    return TRUE;
}

static D2D1_MATRIX_3X2_F matrix_multiply(const D2D1_MATRIX_3X2_F *a, const D2D1_MATRIX_3X2_F *b)
{
    D2D1_MATRIX_3X2_F r;
    r._11 = a->_11 * b->_11 + a->_12 * b->_21;
    r._12 = a->_11 * b->_12 + a->_12 * b->_22;
    r._21 = a->_21 * b->_11 + a->_22 * b->_21;
    r._22 = a->_21 * b->_12 + a->_22 * b->_22;
    r._31 = a->_31 * b->_11 + a->_32 * b->_21 + b->_31;
    r._32 = a->_31 * b->_12 + a->_32 * b->_22 + b->_32;
    return r;
}

static const D2D1_MATRIX_3X2_F identity = {{{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}}};

/* A transform list, applied left to right onto the parent's: "translate(1 2) rotate(45)". */
static D2D1_MATRIX_3X2_F parse_transform(const WCHAR *p)
{
    D2D1_MATRIX_3X2_F result = identity, m;
    float v[6];
    unsigned int n;

    while (p && *(p = skip_space(p)))
    {
        const WCHAR *name = p, *q;
        size_t len;

        while (*p && *p != '(') ++p;
        if (!*p) break;
        len = p - name;
        while (len && name[len - 1] == ' ') --len;
        for (n = 0, q = p + 1; n < 6 && (q = parse_number(q, &v[n])); ++n) p = q;
        if (!(p = wcschr(p, ')'))) break;
        ++p;

        m = identity;
        if (len == 6 && !wcsncmp(name, L"matrix", 6) && n == 6)
        {
            m._11 = v[0]; m._12 = v[1]; m._21 = v[2]; m._22 = v[3]; m._31 = v[4]; m._32 = v[5];
        }
        else if (len == 9 && !wcsncmp(name, L"translate", 9) && n >= 1)
        {
            m._31 = v[0]; m._32 = n > 1 ? v[1] : 0.0f;
        }
        else if (len == 5 && !wcsncmp(name, L"scale", 5) && n >= 1)
        {
            m._11 = v[0]; m._22 = n > 1 ? v[1] : v[0];
        }
        else if (len == 6 && !wcsncmp(name, L"rotate", 6) && n >= 1)
        {
            float a = v[0] * (float)M_PI / 180.0f, c = cosf(a), s = sinf(a);
            float cx = n >= 3 ? v[1] : 0.0f, cy = n >= 3 ? v[2] : 0.0f;
            m._11 = c; m._12 = s; m._21 = -s; m._22 = c;
            m._31 = cx - c * cx + s * cy;
            m._32 = cy - s * cx - c * cy;
        }
        else if (len == 5 && !wcsncmp(name, L"skewX", 5) && n >= 1)
            m._21 = tanf(v[0] * (float)M_PI / 180.0f);
        else if (len == 5 && !wcsncmp(name, L"skewY", 5) && n >= 1)
            m._12 = tanf(v[0] * (float)M_PI / 180.0f);
        else
            FIXME("transform %s is not understood.\n", debugstr_wn(name, len));
        /* each transform in the list applies to what follows it */
        result = matrix_multiply(&m, &result);
    }
    return result;
}

/* ---------------------------------------------------------------------------------------------
 * Geometry
 * ------------------------------------------------------------------------------------------- */
struct path_builder
{
    ID2D1GeometrySink *sink;
    D2D1_POINT_2F start, current, last_control;
    BOOL in_figure;
    WCHAR last_command;
};

static void builder_end(struct path_builder *b, BOOL closed)
{
    if (!b->in_figure) return;
    ID2D1GeometrySink_EndFigure(b->sink, closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    b->in_figure = FALSE;
}

static void builder_ensure_figure(struct path_builder *b)
{
    if (b->in_figure) return;
    ID2D1GeometrySink_BeginFigure(b->sink, b->current, D2D1_FIGURE_BEGIN_FILLED);
    b->start = b->current;
    b->in_figure = TRUE;
}

/* The path data grammar of SVG 1.1, all commands, absolute and relative. */
static void build_path(struct path_builder *b, const WCHAR *d)
{
    const WCHAR *p = d, *q;
    WCHAR command = 0;
    float v[7];

    while (*(p = skip_space(p)))
    {
        BOOL relative;
        unsigned int count, i;
        D2D1_POINT_2F base;

        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
            command = *p++;
        else if (!command)
            break;  /* numbers with no command */

        relative = command >= 'a';
        base = relative ? b->current : (D2D1_POINT_2F){0.0f, 0.0f};

        switch (command | 0x20)
        {
        case 'z':
            builder_end(b, TRUE);
            b->current = b->start;
            b->last_command = 'z';
            command = 0;
            continue;
        case 'm': case 'l': case 't': count = 2; break;
        case 'h': case 'v': count = 1; break;
        case 's': case 'q': count = 4; break;
        case 'c': count = 6; break;
        case 'a': count = 7; break;
        default:
            FIXME("path command %c in %s.\n", command, debugstr_w(d));
            return;
        }

        if ((command | 0x20) == 'a')
        {
            BOOL large, sweep;
            if (!(q = parse_number(p, &v[0])) || !(q = parse_number(q, &v[1])) ||
                !(q = parse_number(q, &v[2])) || !(q = parse_flag(q, &large)) ||
                !(q = parse_flag(q, &sweep)) || !(q = parse_number(q, &v[5])) || !(q = parse_number(q, &v[6])))
                break;
            v[3] = large; v[4] = sweep;
            p = q;
        }
        else
        {
            for (i = 0; i < count; ++i)
            {
                if (!(q = parse_number(p, &v[i]))) break;
                p = q;
            }
            if (i < count) break;
        }

        switch (command | 0x20)
        {
        case 'm':
            builder_end(b, FALSE);
            b->current.x = base.x + v[0];
            b->current.y = base.y + v[1];
            builder_ensure_figure(b);
            /* further coordinate pairs after a moveto are linetos */
            command = relative ? 'l' : 'L';
            b->last_command = 'm';
            continue;
        case 'l':
            builder_ensure_figure(b);
            b->current.x = base.x + v[0];
            b->current.y = base.y + v[1];
            ID2D1GeometrySink_AddLine(b->sink, b->current);
            break;
        case 'h':
            builder_ensure_figure(b);
            b->current.x = (relative ? b->current.x : 0.0f) + v[0];
            ID2D1GeometrySink_AddLine(b->sink, b->current);
            break;
        case 'v':
            builder_ensure_figure(b);
            b->current.y = (relative ? b->current.y : 0.0f) + v[0];
            ID2D1GeometrySink_AddLine(b->sink, b->current);
            break;
        case 'c':
        case 's':
        {
            D2D1_BEZIER_SEGMENT bezier;
            builder_ensure_figure(b);
            if ((command | 0x20) == 'c')
            {
                bezier.point1.x = base.x + v[0]; bezier.point1.y = base.y + v[1];
                bezier.point2.x = base.x + v[2]; bezier.point2.y = base.y + v[3];
                bezier.point3.x = base.x + v[4]; bezier.point3.y = base.y + v[5];
            }
            else
            {
                /* the first control point reflects the previous curve's second one */
                if (b->last_command == 'c' || b->last_command == 's')
                {
                    bezier.point1.x = 2.0f * b->current.x - b->last_control.x;
                    bezier.point1.y = 2.0f * b->current.y - b->last_control.y;
                }
                else bezier.point1 = b->current;
                bezier.point2.x = base.x + v[0]; bezier.point2.y = base.y + v[1];
                bezier.point3.x = base.x + v[2]; bezier.point3.y = base.y + v[3];
            }
            ID2D1GeometrySink_AddBezier(b->sink, &bezier);
            b->last_control = bezier.point2;
            b->current = bezier.point3;
            b->last_command = command | 0x20;
            continue;
        }
        case 'q':
        case 't':
        {
            D2D1_QUADRATIC_BEZIER_SEGMENT quad;
            builder_ensure_figure(b);
            if ((command | 0x20) == 'q')
            {
                quad.point1.x = base.x + v[0]; quad.point1.y = base.y + v[1];
                quad.point2.x = base.x + v[2]; quad.point2.y = base.y + v[3];
            }
            else
            {
                if (b->last_command == 'q' || b->last_command == 't')
                {
                    quad.point1.x = 2.0f * b->current.x - b->last_control.x;
                    quad.point1.y = 2.0f * b->current.y - b->last_control.y;
                }
                else quad.point1 = b->current;
                quad.point2.x = base.x + v[0]; quad.point2.y = base.y + v[1];
            }
            ID2D1GeometrySink_AddQuadraticBezier(b->sink, &quad);
            b->last_control = quad.point1;
            b->current = quad.point2;
            b->last_command = command | 0x20;
            continue;
        }
        case 'a':
        {
            D2D1_ARC_SEGMENT arc;
            builder_ensure_figure(b);
            arc.point.x = base.x + v[5];
            arc.point.y = base.y + v[6];
            arc.size.width = fabsf(v[0]);
            arc.size.height = fabsf(v[1]);
            arc.rotationAngle = v[2];
            arc.arcSize = v[3] ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;
            /* y points down in both, so SVG's positive-angle sweep is D2D's clockwise */
            arc.sweepDirection = v[4] ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
            if (!arc.size.width || !arc.size.height)
                ID2D1GeometrySink_AddLine(b->sink, arc.point);
            else
                ID2D1GeometrySink_AddArc(b->sink, &arc);
            b->current = arc.point;
            break;
        }
        }
        b->last_command = command | 0x20;
    }
    builder_end(b, FALSE);
}

static HRESULT path_geometry_create(ID2D1Factory *factory, const struct svg_node *node, BOOL evenodd,
        ID2D1Geometry **out)
{
    struct path_builder builder = {0};
    ID2D1PathGeometry *path;
    const WCHAR *d;
    HRESULT hr;

    *out = NULL;
    if (FAILED(hr = ID2D1Factory_CreatePathGeometry(factory, &path))) return hr;
    if (FAILED(hr = ID2D1PathGeometry_Open(path, &builder.sink)))
    {
        ID2D1PathGeometry_Release(path);
        return hr;
    }
    ID2D1GeometrySink_SetFillMode(builder.sink, evenodd ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);

    if (!wcscmp(node->name, L"path"))
    {
        if ((d = svg_attr(node, L"d"))) build_path(&builder, d);
    }
    else
    {
        /* line, polyline, polygon */
        BOOL closed = !wcscmp(node->name, L"polygon");
        D2D1_POINT_2F pt;

        if (!wcscmp(node->name, L"line"))
        {
            builder.current.x = attr_float(node, L"x1", 0.0f);
            builder.current.y = attr_float(node, L"y1", 0.0f);
            builder_ensure_figure(&builder);
            pt.x = attr_float(node, L"x2", 0.0f);
            pt.y = attr_float(node, L"y2", 0.0f);
            ID2D1GeometrySink_AddLine(builder.sink, pt);
        }
        else if ((d = svg_attr(node, L"points")))
        {
            const WCHAR *q = d;
            BOOL first = TRUE;
            while ((q = parse_number(q, &pt.x)) && (q = parse_number(q, &pt.y)))
            {
                if (first)
                {
                    builder.current = pt;
                    builder_ensure_figure(&builder);
                    first = FALSE;
                }
                else ID2D1GeometrySink_AddLine(builder.sink, pt);
            }
        }
        builder_end(&builder, closed);
    }

    hr = ID2D1GeometrySink_Close(builder.sink);
    ID2D1GeometrySink_Release(builder.sink);
    if (FAILED(hr))
    {
        WARN("path of %s did not build, hr %#lx.\n", debugstr_w(node->name), hr);
        ID2D1PathGeometry_Release(path);
        return hr;
    }
    *out = (ID2D1Geometry *)path;
    return S_OK;
}

/* The untransformed geometry of a shape element, or NULL if it is not one. */
static HRESULT shape_geometry(ID2D1Factory *factory, const struct svg_node *node, BOOL evenodd,
        ID2D1Geometry **out)
{
    *out = NULL;
    if (!wcscmp(node->name, L"rect"))
    {
        D2D1_ROUNDED_RECT rr;
        float x = attr_float(node, L"x", 0.0f), y = attr_float(node, L"y", 0.0f);
        float w = attr_float(node, L"width", 0.0f), h = attr_float(node, L"height", 0.0f);
        float rx = attr_float(node, L"rx", -1.0f), ry = attr_float(node, L"ry", -1.0f);

        if (w <= 0.0f || h <= 0.0f) return S_OK;
        if (rx < 0.0f) rx = ry;
        if (ry < 0.0f) ry = rx;
        if (rx < 0.0f) rx = ry = 0.0f;
        rr.rect = (D2D1_RECT_F){x, y, x + w, y + h};
        rr.radiusX = min(rx, w / 2.0f);
        rr.radiusY = min(ry, h / 2.0f);
        if (rr.radiusX > 0.0f && rr.radiusY > 0.0f)
            return ID2D1Factory_CreateRoundedRectangleGeometry(factory, &rr, (ID2D1RoundedRectangleGeometry **)out);
        return ID2D1Factory_CreateRectangleGeometry(factory, &rr.rect, (ID2D1RectangleGeometry **)out);
    }
    if (!wcscmp(node->name, L"circle") || !wcscmp(node->name, L"ellipse"))
    {
        D2D1_ELLIPSE ellipse;
        ellipse.point.x = attr_float(node, L"cx", 0.0f);
        ellipse.point.y = attr_float(node, L"cy", 0.0f);
        if (node->name[0] == 'c')
            ellipse.radiusX = ellipse.radiusY = attr_float(node, L"r", 0.0f);
        else
        {
            ellipse.radiusX = attr_float(node, L"rx", 0.0f);
            ellipse.radiusY = attr_float(node, L"ry", 0.0f);
        }
        if (ellipse.radiusX <= 0.0f || ellipse.radiusY <= 0.0f) return S_OK;
        return ID2D1Factory_CreateEllipseGeometry(factory, &ellipse, (ID2D1EllipseGeometry **)out);
    }
    if (!wcscmp(node->name, L"path") || !wcscmp(node->name, L"line") ||
        !wcscmp(node->name, L"polyline") || !wcscmp(node->name, L"polygon"))
        return path_geometry_create(factory, node, evenodd, out);
    return S_OK;
}

/* ---------------------------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------------------------- */
struct svg_shadow
{
    float dx, dy, sigma, alpha;
};

struct svg_style
{
    D2D1_COLOR_F fill, stroke;
    BOOL has_fill, has_stroke;
    const struct svg_node *fill_server, *stroke_server;   /* a gradient or pattern, from url(#id) */
    float fill_opacity, stroke_opacity, opacity, stroke_width, miter_limit;
    BOOL evenodd, clip_evenodd;
    D2D1_CAP_STYLE cap;
    D2D1_LINE_JOIN join;
    const WCHAR *dash_array;
    struct svg_shadow shadows[4];   /* from a drop-shadow filter */
    unsigned int shadow_count;
    BOOL group_shadow;              /* the shadows came from the parent group */
};

struct svg_draw
{
    ID2D1DeviceContext *context;
    ID2D1Factory *factory;
    const struct svg_node *root;
};

static const struct svg_node *find_by_id(const struct svg_node *node, const WCHAR *id)
{
    const WCHAR *value = svg_attr(node, L"id");
    const struct svg_node *found;
    size_t i;

    if (value && !wcscmp(value, id)) return node;
    for (i = 0; i < node->child_count; ++i)
        if ((found = find_by_id(node->children[i], id))) return found;
    return NULL;
}

/* The element a url(#id) or an href="#id" names. */
static const struct svg_node *reference(struct svg_draw *draw, const WCHAR *value)
{
    WCHAR id[128];
    const WCHAR *start, *end;
    size_t len;

    if (!value) return NULL;
    value = skip_space(value);
    if (!wcsncmp(value, L"url(", 4)) value = skip_space(value + 4);
    if (*value == '\'' || *value == '"') ++value;
    if (*value != '#') return NULL;
    start = ++value;
    for (end = start; *end && *end != ')' && *end != '\'' && *end != '"' && *end != ' '; ++end) ;
    len = min(end - start, ARRAY_SIZE(id) - 1);
    memcpy(id, start, len * sizeof(WCHAR));
    id[len] = 0;
    return find_by_id(draw->root, id);
}

static const WCHAR *href(const struct svg_node *node)
{
    const WCHAR *value = svg_attr(node, L"xlink:href");
    return value ? value : svg_attr(node, L"href");
}

static BOOL is_paint_server(const struct svg_node *node)
{
    return node && (!wcscmp(node->name, L"linearGradient") || !wcscmp(node->name, L"radialGradient") ||
                    !wcscmp(node->name, L"pattern"));
}

/* A fill or stroke value: a colour, "none", or url(#server) with an optional fallback colour. */
static void set_paint(struct svg_draw *draw, const WCHAR *value, BOOL *has, D2D1_COLOR_F *colour,
        const struct svg_node **server)
{
    const WCHAR *p = skip_space(value);

    *server = NULL;
    if (!wcsncmp(p, L"url(", 4))
    {
        const struct svg_node *target = reference(draw, p);
        const WCHAR *after = wcschr(p, ')');

        if (is_paint_server(target))
        {
            *server = target;
            *has = TRUE;
            return;
        }
        *has = after && parse_paint(after + 1, colour);
        return;
    }
    *has = parse_paint(value, colour);
}

/* A drop-shadow filter: each feOffset / feGaussianBlur pair followed by the feColorMatrix that sets
 * its opacity (what design tools export), or an feDropShadow. Anything else is not applied. */
static unsigned int parse_shadow_filter(const struct svg_node *filter, struct svg_shadow *shadows, unsigned int max)
{
    float dx = 0.0f, dy = 0.0f, sigma = 0.0f;
    BOOL pending = FALSE;
    unsigned int count = 0;
    size_t i;

    for (i = 0; i < filter->child_count && count < max; ++i)
    {
        const struct svg_node *fe = filter->children[i];
        const WCHAR *in = svg_attr(fe, L"in"), *values;

        if (!wcscmp(fe->name, L"feOffset"))
        {
            dx = attr_float(fe, L"dx", 0.0f);
            dy = attr_float(fe, L"dy", 0.0f);
            pending = TRUE;
        }
        else if (!wcscmp(fe->name, L"feGaussianBlur"))
        {
            sigma = attr_float(fe, L"stdDeviation", 0.0f);
            pending = TRUE;
        }
        else if (!wcscmp(fe->name, L"feColorMatrix") && pending && !(in && !wcscmp(in, L"SourceAlpha")) &&
                 (values = svg_attr(fe, L"values")))
        {
            float v[20] = {0};
            const WCHAR *q = values;
            unsigned int k;
            for (k = 0; k < 20 && (q = parse_number(q, &v[k])); ++k) ;
            if (k == 20)
            {
                shadows[count].dx = dx;
                shadows[count].dy = dy;
                shadows[count].sigma = sigma;
                shadows[count].alpha = v[18];
                ++count;
            }
            dx = dy = sigma = 0.0f;
            pending = FALSE;
        }
        else if (!wcscmp(fe->name, L"feDropShadow"))
        {
            shadows[count].dx = attr_float(fe, L"dx", 2.0f);
            shadows[count].dy = attr_float(fe, L"dy", 2.0f);
            shadows[count].sigma = attr_float(fe, L"stdDeviation", 2.0f);
            shadows[count].alpha = attr_float(fe, L"flood-opacity", 1.0f);
            ++count;
        }
    }
    if (!count) FIXME("filter %s is not a drop shadow and is not applied.\n", debugstr_w(svg_attr(filter, L"id")));
    return count;
}

static void apply_style(struct svg_draw *draw, const struct svg_node *node, struct svg_style *style)
{
    const WCHAR *value;
    float f;

    if ((value = svg_attr(node, L"fill"))) set_paint(draw, value, &style->has_fill, &style->fill, &style->fill_server);
    if ((value = svg_attr(node, L"stroke")))
        set_paint(draw, value, &style->has_stroke, &style->stroke, &style->stroke_server);
    if ((value = svg_attr(node, L"fill-opacity")) && parse_number(value, &f)) style->fill_opacity = f;
    if ((value = svg_attr(node, L"stroke-opacity")) && parse_number(value, &f)) style->stroke_opacity = f;
    if ((value = svg_attr(node, L"stroke-width")) && parse_number(value, &f)) style->stroke_width = f;
    if ((value = svg_attr(node, L"stroke-miterlimit")) && parse_number(value, &f)) style->miter_limit = f;
    if ((value = svg_attr(node, L"fill-rule"))) style->evenodd = !wcscmp(value, L"evenodd");
    if ((value = svg_attr(node, L"clip-rule"))) style->clip_evenodd = !wcscmp(value, L"evenodd");
    if ((value = svg_attr(node, L"stroke-dasharray"))) style->dash_array = wcscmp(value, L"none") ? value : NULL;
    if ((value = svg_attr(node, L"stroke-linecap")))
        style->cap = !wcscmp(value, L"round") ? D2D1_CAP_STYLE_ROUND
                : !wcscmp(value, L"square") ? D2D1_CAP_STYLE_SQUARE : D2D1_CAP_STYLE_FLAT;
    if ((value = svg_attr(node, L"stroke-linejoin")))
        style->join = !wcscmp(value, L"round") ? D2D1_LINE_JOIN_ROUND
                : !wcscmp(value, L"bevel") ? D2D1_LINE_JOIN_BEVEL : D2D1_LINE_JOIN_MITER_OR_BEVEL;
    /* opacity is not inherited, but it multiplies down the tree, which is what it looks like for
     * shapes that do not overlap */
    if ((value = svg_attr(node, L"opacity")) && parse_number(value, &f)) style->opacity *= f;
    if ((value = svg_attr(node, L"filter")))
    {
        const struct svg_node *filter = reference(draw, value);
        if (filter && !wcscmp(filter->name, L"filter"))
            style->shadow_count = parse_shadow_filter(filter, style->shadows, ARRAY_SIZE(style->shadows));
    }
    if (svg_attr(node, L"style")) FIXME("style attributes are not parsed.\n");
}

/* ---- paint servers ---- */

/* An attribute of a gradient or pattern, following href to the template it inherits from. */
static const WCHAR *server_attr(struct svg_draw *draw, const struct svg_node *node, const WCHAR *name)
{
    unsigned int depth;
    for (depth = 0; node && depth < 8; ++depth)
    {
        const WCHAR *value = svg_attr(node, name);
        if (value) return value;
        node = reference(draw, href(node));
    }
    return NULL;
}

/* A coordinate that may be a percentage: a fraction of 1 in bounding-box units. */
static float server_coord(struct svg_draw *draw, const struct svg_node *node, const WCHAR *name, float def)
{
    const WCHAR *value = server_attr(draw, node, name), *q;
    float f;
    if (!value || !(q = parse_number(value, &f))) return def;
    return *q == '%' ? f / 100.0f : f;
}

static const struct svg_node *server_with_stops(struct svg_draw *draw, const struct svg_node *node)
{
    unsigned int depth;
    size_t i;
    for (depth = 0; node && depth < 8; ++depth)
    {
        for (i = 0; i < node->child_count; ++i)
            if (!wcscmp(node->children[i]->name, L"stop")) return node;
        node = reference(draw, href(node));
    }
    return NULL;
}

static HRESULT gradient_stops(struct svg_draw *draw, const struct svg_node *gradient, ID2D1GradientStopCollection **out)
{
    const struct svg_node *owner = server_with_stops(draw, gradient);
    D2D1_GRADIENT_STOP stops[32];
    UINT32 count = 0;
    size_t i;

    for (i = 0; owner && i < owner->child_count && count < ARRAY_SIZE(stops); ++i)
    {
        const struct svg_node *stop = owner->children[i];
        const WCHAR *value, *q;
        D2D1_COLOR_F colour = {0.0f, 0.0f, 0.0f, 1.0f};
        float offset = 0.0f, opacity = 1.0f;

        if (wcscmp(stop->name, L"stop")) continue;
        if ((value = svg_attr(stop, L"offset")) && (q = parse_number(value, &offset)) && *q == '%') offset /= 100.0f;
        if ((value = svg_attr(stop, L"stop-color"))) parse_paint(value, &colour);
        if ((value = svg_attr(stop, L"stop-opacity"))) parse_number(value, &opacity);
        colour.a *= opacity;
        stops[count].position = min(1.0f, max(0.0f, offset));
        if (count && stops[count].position < stops[count - 1].position) stops[count].position = stops[count - 1].position;
        stops[count].color = colour;
        ++count;
    }
    if (!count) return E_FAIL;
    return ID2D1RenderTarget_CreateGradientStopCollection((ID2D1RenderTarget *)draw->context, stops, count, D2D1_GAMMA_2_2,
            D2D1_EXTEND_MODE_CLAMP, out);
}

/* The server's own units mapped to user space: objectBoundingBox (the default for gradients and
 * pattern content alike) scales 0..1 onto the shape's bounds. */
static D2D1_MATRIX_3X2_F server_transform(struct svg_draw *draw, const struct svg_node *server, const WCHAR *units_attr,
        const WCHAR *transform_attr, const D2D1_RECT_F *bounds, BOOL bbox_default)
{
    const WCHAR *units = server_attr(draw, server, units_attr), *value;
    D2D1_MATRIX_3X2_F m = identity, bbox = identity;

    if ((value = server_attr(draw, server, transform_attr))) m = parse_transform(value);
    if (units ? !wcscmp(units, L"objectBoundingBox") : bbox_default)
    {
        bbox._11 = bounds->right - bounds->left;
        bbox._22 = bounds->bottom - bounds->top;
        bbox._31 = bounds->left;
        bbox._32 = bounds->top;
        m = matrix_multiply(&m, &bbox);
    }
    return m;
}

static int base64_value(WCHAR c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* An <image>'s pixels, from a base64 data URI, as a D2D bitmap. External files are not loaded. */
static HRESULT image_bitmap(struct svg_draw *draw, const struct svg_node *image, ID2D1Bitmap **out)
{
    IWICImagingFactory *wic = NULL;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    const WCHAR *uri = href(image), *p;
    BYTE *bytes;
    size_t len = 0;
    unsigned int acc = 0, bits = 0;
    HRESULT hr;

    *out = NULL;
    if (!uri || wcsncmp(uri, L"data:", 5) || !(p = wcsstr(uri, L";base64,")))
    {
        FIXME("image %s is not an embedded base64 image.\n", debugstr_wn(uri, uri ? 64 : 0));
        return E_NOTIMPL;
    }
    p += 8;
    if (!(bytes = malloc(wcslen(p) * 3 / 4 + 4))) return E_OUTOFMEMORY;
    for (; *p; ++p)
    {
        int v = base64_value(*p);
        if (v < 0) continue;   /* padding and whitespace */
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            bytes[len++] = (acc >> bits) & 0xff;
        }
    }

    if (SUCCEEDED(hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
            &IID_IWICImagingFactory, (void **)&wic)) &&
        SUCCEEDED(hr = IWICImagingFactory_CreateStream(wic, &stream)) &&
        SUCCEEDED(hr = IWICStream_InitializeFromMemory(stream, bytes, len)) &&
        SUCCEEDED(hr = IWICImagingFactory_CreateDecoderFromStream(wic, (IStream *)stream, NULL,
            WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame)) &&
        SUCCEEDED(hr = IWICImagingFactory_CreateFormatConverter(wic, &converter)) &&
        SUCCEEDED(hr = IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame,
            &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)))
        hr = ID2D1DeviceContext_CreateBitmapFromWicBitmap(draw->context, (IWICBitmapSource *)converter, NULL,
                (ID2D1Bitmap1 **)out);
    if (FAILED(hr)) WARN("embedded image did not decode, hr %#lx.\n", hr);

    if (converter) IWICFormatConverter_Release(converter);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (stream) IWICStream_Release(stream);
    if (wic) IWICImagingFactory_Release(wic);
    free(bytes);
    return hr;
}

/* The transform from an <image>'s pixels to its user space: its x, y, width and height. */
static D2D1_MATRIX_3X2_F image_transform(const struct svg_node *image, ID2D1Bitmap *bitmap)
{
    D2D1_MATRIX_3X2_F m = identity;
    D2D1_SIZE_U size = ID2D1Bitmap_GetPixelSize(bitmap);
    float w = attr_float(image, L"width", (float)size.width), h = attr_float(image, L"height", (float)size.height);

    if (size.width && size.height)
    {
        m._11 = w / size.width;
        m._22 = h / size.height;
    }
    m._31 = attr_float(image, L"x", 0.0f);
    m._32 = attr_float(image, L"y", 0.0f);
    return m;
}

/* A pattern's picture: the <image> it holds, directly or through <use>, and the transform of that
 * image within the pattern's content units. */
static const struct svg_node *pattern_image(struct svg_draw *draw, const struct svg_node *pattern, D2D1_MATRIX_3X2_F *m)
{
    unsigned int depth;
    size_t i;

    for (depth = 0; pattern && depth < 8; ++depth)
    {
        for (i = 0; i < pattern->child_count; ++i)
        {
            const struct svg_node *child = pattern->children[i], *target = child;
            D2D1_MATRIX_3X2_F local = identity;
            const WCHAR *value;

            if (!wcscmp(child->name, L"use"))
            {
                target = reference(draw, href(child));
                if ((value = svg_attr(child, L"transform"))) local = parse_transform(value);
                local._31 += attr_float(child, L"x", 0.0f);
                local._32 += attr_float(child, L"y", 0.0f);
            }
            else if ((value = svg_attr(child, L"transform"))) local = parse_transform(value);
            if (target && !wcscmp(target->name, L"image"))
            {
                *m = local;
                return target;
            }
        }
        pattern = reference(draw, href(pattern));
    }
    FIXME("pattern %s holds no image; other pattern content is not drawn.\n", debugstr_w(svg_attr(pattern, L"id")));
    return NULL;
}

static ID2D1Brush *server_brush(struct svg_draw *draw, const struct svg_node *server, ID2D1Geometry *geometry, float opacity)
{
    D2D1_BRUSH_PROPERTIES props;
    D2D1_RECT_F bounds;
    ID2D1Brush *brush = NULL;

    if (FAILED(ID2D1Geometry_GetBounds(geometry, NULL, &bounds))) return NULL;
    props.opacity = opacity;

    if (!wcscmp(server->name, L"linearGradient") || !wcscmp(server->name, L"radialGradient"))
    {
        ID2D1GradientStopCollection *stops;
        if (FAILED(gradient_stops(draw, server, &stops))) return NULL;
        props.transform = server_transform(draw, server, L"gradientUnits", L"gradientTransform", &bounds, TRUE);
        if (server->name[0] == 'l')
        {
            D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES linear;
            linear.startPoint.x = server_coord(draw, server, L"x1", 0.0f);
            linear.startPoint.y = server_coord(draw, server, L"y1", 0.0f);
            linear.endPoint.x = server_coord(draw, server, L"x2", 1.0f);
            linear.endPoint.y = server_coord(draw, server, L"y2", 0.0f);
            ID2D1DeviceContext_CreateLinearGradientBrush(draw->context, &linear, &props, stops,
                    (ID2D1LinearGradientBrush **)&brush);
        }
        else
        {
            D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES radial;
            float cx = server_coord(draw, server, L"cx", 0.5f), cy = server_coord(draw, server, L"cy", 0.5f);
            radial.center.x = cx;
            radial.center.y = cy;
            radial.gradientOriginOffset.x = server_coord(draw, server, L"fx", cx) - cx;
            radial.gradientOriginOffset.y = server_coord(draw, server, L"fy", cy) - cy;
            radial.radiusX = radial.radiusY = server_coord(draw, server, L"r", 0.5f);
            ID2D1DeviceContext_CreateRadialGradientBrush(draw->context, &radial, &props, stops,
                    (ID2D1RadialGradientBrush **)&brush);
        }
        ID2D1GradientStopCollection_Release(stops);
        return brush;
    }

    /* pattern */
    {
        D2D1_BITMAP_BRUSH_PROPERTIES bitmap_props = { D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                      D2D1_BITMAP_INTERPOLATION_MODE_LINEAR };
        D2D1_MATRIX_3X2_F in_pattern, content, pixels;
        const struct svg_node *image;
        ID2D1Bitmap *bitmap;

        if (!(image = pattern_image(draw, server, &in_pattern))) return NULL;
        if (FAILED(image_bitmap(draw, image, &bitmap))) return NULL;
        /* patternContentUnits defaults to userSpaceOnUse; a viewBox is not handled */
        content = server_transform(draw, server, L"patternContentUnits", L"patternTransform", &bounds, FALSE);
        pixels = image_transform(image, bitmap);
        pixels = matrix_multiply(&pixels, &in_pattern);
        props.transform = matrix_multiply(&pixels, &content);
        ID2D1RenderTarget_CreateBitmapBrush((ID2D1RenderTarget *)draw->context, bitmap, &bitmap_props, &props, (ID2D1BitmapBrush **)&brush);
        ID2D1Bitmap_Release(bitmap);
        return brush;
    }
}

static ID2D1Brush *paint_brush(struct svg_draw *draw, const D2D1_COLOR_F *colour, const struct svg_node *server,
        ID2D1Geometry *geometry, float opacity)
{
    ID2D1SolidColorBrush *brush;
    D2D1_COLOR_F c;

    if (server) return server_brush(draw, server, geometry, opacity);
    c = *colour;
    c.a *= opacity;
    if (c.a <= 0.0f || FAILED(ID2D1DeviceContext_CreateSolidColorBrush(draw->context, &c, NULL, &brush))) return NULL;
    return (ID2D1Brush *)brush;
}

/* A drop shadow: the shape's silhouette, offset and darkened. D2D's blur is not usable here, so
 * the blur is approximated with strokes of growing width and falling opacity around a core fill. */
static void draw_shadows(struct svg_draw *draw, ID2D1Geometry *geometry, const struct svg_style *style)
{
    unsigned int i, k;

    for (i = 0; i < style->shadow_count; ++i)
    {
        const struct svg_shadow *shadow = &style->shadows[i];
        D2D1_MATRIX_3X2_F offset = identity;
        ID2D1TransformedGeometry *moved;
        ID2D1SolidColorBrush *brush;
        D2D1_COLOR_F black = {0.0f, 0.0f, 0.0f, 0.0f};
        float alpha = shadow->alpha * style->opacity;

        if (alpha <= 0.0f) continue;
        offset._31 = shadow->dx;
        offset._32 = shadow->dy;
        if (FAILED(ID2D1Factory_CreateTransformedGeometry(draw->factory, geometry, &offset, &moved))) continue;
        if (shadow->sigma <= 0.25f)
        {
            black.a = alpha;
            if (SUCCEEDED(ID2D1DeviceContext_CreateSolidColorBrush(draw->context, &black, NULL, &brush)))
            {
                ID2D1DeviceContext_FillGeometry(draw->context, (ID2D1Geometry *)moved, (ID2D1Brush *)brush, NULL);
                ID2D1SolidColorBrush_Release(brush);
            }
        }
        else
        {
            black.a = alpha * 0.5f;
            if (SUCCEEDED(ID2D1DeviceContext_CreateSolidColorBrush(draw->context, &black, NULL, &brush)))
            {
                ID2D1DeviceContext_FillGeometry(draw->context, (ID2D1Geometry *)moved, (ID2D1Brush *)brush, NULL);
                /* four rings reaching out about two standard deviations, each fainter */
                for (k = 4; k > 0; --k)
                {
                    black.a = alpha * 0.5f / 4.0f;
                    ID2D1SolidColorBrush_SetColor(brush, &black);
                    ID2D1DeviceContext_DrawGeometry(draw->context, (ID2D1Geometry *)moved, (ID2D1Brush *)brush,
                            shadow->sigma * k, NULL);
                }
                ID2D1SolidColorBrush_Release(brush);
            }
        }
        ID2D1TransformedGeometry_Release(moved);
    }
}

static void draw_shape(struct svg_draw *draw, ID2D1Geometry *geometry, const struct svg_style *style)
{
    ID2D1Brush *brush;

    if (style->shadow_count) draw_shadows(draw, geometry, style);
    if (style->has_fill &&
        (brush = paint_brush(draw, &style->fill, style->fill_server, geometry, style->fill_opacity * style->opacity)))
    {
        ID2D1DeviceContext_FillGeometry(draw->context, geometry, brush, NULL);
        ID2D1Brush_Release(brush);
    }
    if (style->has_stroke && style->stroke_width > 0.0f)
    {
        D2D1_STROKE_STYLE_PROPERTIES props;
        ID2D1StrokeStyle *stroke_style = NULL;
        float dashes[32];
        UINT32 dash_count = 0;

        props.startCap = props.endCap = props.dashCap = style->cap;
        props.lineJoin = style->join;
        props.miterLimit = style->miter_limit;
        props.dashStyle = D2D1_DASH_STYLE_SOLID;
        props.dashOffset = 0.0f;
        if (style->dash_array)
        {
            const WCHAR *q = style->dash_array;
            float f;
            /* D2D's dashes are in multiples of the stroke width */
            while (dash_count < ARRAY_SIZE(dashes) && (q = parse_number(q, &f)))
                dashes[dash_count++] = f / style->stroke_width;
            if (dash_count) props.dashStyle = D2D1_DASH_STYLE_CUSTOM;
        }
        ID2D1Factory_CreateStrokeStyle(draw->factory, &props, dash_count ? dashes : NULL, dash_count, &stroke_style);

        if ((brush = paint_brush(draw, &style->stroke, style->stroke_server, geometry,
                style->stroke_opacity * style->opacity)))
        {
            ID2D1DeviceContext_DrawGeometry(draw->context, geometry, brush, style->stroke_width, stroke_style);
            ID2D1Brush_Release(brush);
        }
        if (stroke_style) ID2D1StrokeStyle_Release(stroke_style);
    }
}

/* The union of a clipPath's or a mask's shapes, in the coordinate space of the element that uses
 * it. A mask is taken as a clip by its shapes' outlines: masks in icon sets are solid shapes, often
 * an alpha mask cutting a hole (fill-rule evenodd), which is exactly a clip. */
static HRESULT clip_geometry(struct svg_draw *draw, const struct svg_node *clip, ID2D1Geometry **out)
{
    ID2D1Geometry *geometries[64];
    UINT32 count = 0, i;
    size_t k;
    HRESULT hr;

    *out = NULL;
    for (k = 0; k < clip->child_count && count < ARRAY_SIZE(geometries); ++k)
    {
        const struct svg_node *child = clip->children[k];
        const WCHAR *rule = svg_attr(child, L"clip-rule"), *transform;
        ID2D1Geometry *geometry;

        if (!rule) rule = svg_attr(child, L"fill-rule");
        if (FAILED(shape_geometry(draw->factory, child, rule && !wcscmp(rule, L"evenodd"), &geometry)) || !geometry)
            continue;
        if ((transform = svg_attr(child, L"transform")))
        {
            D2D1_MATRIX_3X2_F m = parse_transform(transform);
            ID2D1TransformedGeometry *transformed;
            if (SUCCEEDED(ID2D1Factory_CreateTransformedGeometry(draw->factory, geometry, &m, &transformed)))
            {
                ID2D1Geometry_Release(geometry);
                geometry = (ID2D1Geometry *)transformed;
            }
        }
        geometries[count++] = geometry;
    }
    if (!count) return S_FALSE;
    /* a group's fill mode overrides its members', which would fill an evenodd hole */
    if (count == 1)
    {
        *out = geometries[0];
        return S_OK;
    }
    hr = ID2D1Factory_CreateGeometryGroup(draw->factory, D2D1_FILL_MODE_WINDING, geometries, count,
            (ID2D1GeometryGroup **)out);
    for (i = 0; i < count; ++i) ID2D1Geometry_Release(geometries[i]);
    return hr;
}

/* Clip-paths and masks. Layers are not implemented in this d2d, and neither is geometry combining,
 * so the clipped content is drawn into an offscreen bitmap the size of the target and then painted
 * through the clip geometry with a bitmap brush. */
struct svg_clip
{
    ID2D1Geometry *geometry;
    ID2D1Image *saved_target;
    ID2D1Bitmap1 *bitmap;
};

static BOOL invert_matrix(const D2D1_MATRIX_3X2_F *m, D2D1_MATRIX_3X2_F *out)
{
    float det = m->_11 * m->_22 - m->_12 * m->_21;
    if (fabsf(det) < 1e-12f) return FALSE;
    out->_11 = m->_22 / det;
    out->_12 = -m->_12 / det;
    out->_21 = -m->_21 / det;
    out->_22 = m->_11 / det;
    out->_31 = (m->_21 * m->_32 - m->_22 * m->_31) / det;
    out->_32 = (m->_12 * m->_31 - m->_11 * m->_32) / det;
    return TRUE;
}

static BOOL begin_clip(struct svg_draw *draw, const struct svg_node *clip_node, struct svg_clip *clip)
{
    D2D1_BITMAP_PROPERTIES1 props = {{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED}};
    D2D1_COLOR_F clear = {0};
    D2D1_SIZE_U size;
    ID2D1Bitmap *target;

    memset(clip, 0, sizeof(*clip));
    if (!clip_node) return FALSE;
    if (clip_geometry(draw, clip_node, &clip->geometry) != S_OK)
    {
        FIXME("clip %s has no geometry.\n", debugstr_w(svg_attr(clip_node, L"id")));
        return FALSE;
    }
    TRACE("clipping to %s.\n", debugstr_w(svg_attr(clip_node, L"id")));
    ID2D1DeviceContext_GetTarget(draw->context, &clip->saved_target);
    if (!clip->saved_target || FAILED(ID2D1Image_QueryInterface(clip->saved_target, &IID_ID2D1Bitmap, (void **)&target)))
        goto fail;
    size = ID2D1Bitmap_GetPixelSize(target);
    ID2D1Bitmap_GetDpi(target, &props.dpiX, &props.dpiY);
    ID2D1Bitmap_Release(target);
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
    if (FAILED(ID2D1DeviceContext_CreateBitmap(draw->context, size, NULL, 0, &props, &clip->bitmap))) goto fail;
    ID2D1DeviceContext_SetTarget(draw->context, (ID2D1Image *)clip->bitmap);
    ID2D1DeviceContext_Clear(draw->context, &clear);
    return TRUE;

fail:
    FIXME("could not set up a clip; drawing unclipped.\n");
    if (clip->saved_target) ID2D1Image_Release(clip->saved_target);
    ID2D1Geometry_Release(clip->geometry);
    memset(clip, 0, sizeof(*clip));
    return FALSE;
}

static void end_clip(struct svg_draw *draw, struct svg_clip *clip, const D2D1_MATRIX_3X2_F *transform)
{
    D2D1_BITMAP_BRUSH_PROPERTIES1 bitmap_props = { D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                   D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR };
    D2D1_BRUSH_PROPERTIES props = {1.0f};
    ID2D1BitmapBrush1 *brush;

    if (!clip->geometry) return;
    ID2D1DeviceContext_SetTarget(draw->context, clip->saved_target);
    /* the bitmap is in device space; the geometry is filled in the element's user space */
    if (invert_matrix(transform, &props.transform) &&
        SUCCEEDED(ID2D1DeviceContext_CreateBitmapBrush(draw->context, (ID2D1Bitmap *)clip->bitmap, &bitmap_props,
            &props, &brush)))
    {
        ID2D1DeviceContext_SetTransform(draw->context, transform);
        ID2D1DeviceContext_FillGeometry(draw->context, clip->geometry, (ID2D1Brush *)brush, NULL);
        ID2D1BitmapBrush1_Release(brush);
    }
    ID2D1Bitmap1_Release(clip->bitmap);
    ID2D1Image_Release(clip->saved_target);
    ID2D1Geometry_Release(clip->geometry);
}

static void draw_image(struct svg_draw *draw, const struct svg_node *image, const struct svg_style *style)
{
    D2D1_MATRIX_3X2_F saved, m;
    ID2D1Bitmap *bitmap;
    D2D1_SIZE_U size;
    D2D1_RECT_F dest;

    if (FAILED(image_bitmap(draw, image, &bitmap))) return;
    size = ID2D1Bitmap_GetPixelSize(bitmap);
    m = image_transform(image, bitmap);
    ID2D1DeviceContext_GetTransform(draw->context, &saved);
    m = matrix_multiply(&m, &saved);
    ID2D1DeviceContext_SetTransform(draw->context, &m);
    dest = (D2D1_RECT_F){0.0f, 0.0f, (float)size.width, (float)size.height};
    ID2D1DeviceContext_DrawBitmap(draw->context, bitmap, &dest, style->opacity,
            D2D1_INTERPOLATION_MODE_LINEAR, NULL, NULL);
    ID2D1DeviceContext_SetTransform(draw->context, &saved);
    ID2D1Bitmap_Release(bitmap);
}

static BOOL is_ancestor_node(const struct svg_node *maybe, const struct svg_node *node)
{
    for (; node; node = node->parent) if (node == maybe) return TRUE;
    return FALSE;
}

static void draw_node(struct svg_draw *draw, const struct svg_node *node, struct svg_style style,
        const D2D1_MATRIX_3X2_F *parent_transform)
{
    D2D1_MATRIX_3X2_F transform = *parent_transform, local;
    struct svg_clip clip, mask;
    const WCHAR *value;
    size_t i;

    /* never drawn directly */
    if (!wcscmp(node->name, L"defs") || !wcscmp(node->name, L"clipPath") || !wcscmp(node->name, L"mask") ||
        !wcscmp(node->name, L"linearGradient") || !wcscmp(node->name, L"radialGradient") ||
        !wcscmp(node->name, L"pattern") || !wcscmp(node->name, L"filter") || !wcscmp(node->name, L"metadata") ||
        !wcscmp(node->name, L"title") || !wcscmp(node->name, L"desc") || !wcscmp(node->name, L"style") ||
        !wcscmp(node->name, L"symbol"))
        return;
    if (!wcscmp(node->name, L"text"))
    {
        FIXME("<text> is not drawn.\n");
        return;
    }
    if ((value = svg_attr(node, L"display")) && !wcscmp(value, L"none")) return;

    /* a filter applies to its own element only, or to a group's children through the group */
    if (!style.group_shadow) style.shadow_count = 0;
    style.group_shadow = FALSE;
    apply_style(draw, node, &style);
    if ((value = svg_attr(node, L"transform")) && node != draw->root)
    {
        local = parse_transform(value);
        transform = matrix_multiply(&local, &transform);
    }
    if (!wcscmp(node->name, L"use"))
    {
        /* the referenced element, drawn at x, y */
        const struct svg_node *target = reference(draw, href(node));
        D2D1_MATRIX_3X2_F shift = identity;
        shift._31 = attr_float(node, L"x", 0.0f);
        shift._32 = attr_float(node, L"y", 0.0f);
        transform = matrix_multiply(&shift, &transform);
        if (target && target != node && !is_ancestor_node(target, node))
        {
            ID2D1DeviceContext_SetTransform(draw->context, &transform);
            if (!wcscmp(target->name, L"image")) draw_image(draw, target, &style);
            else draw_node(draw, target, style, &transform);
        }
        ID2D1DeviceContext_SetTransform(draw->context, parent_transform);
        return;
    }

    ID2D1DeviceContext_SetTransform(draw->context, &transform);
    begin_clip(draw, reference(draw, svg_attr(node, L"clip-path")), &clip);
    begin_clip(draw, reference(draw, svg_attr(node, L"mask")), &mask);
    ID2D1DeviceContext_SetTransform(draw->context, &transform);

    if (!wcscmp(node->name, L"svg") || !wcscmp(node->name, L"g") || !wcscmp(node->name, L"a") ||
        !wcscmp(node->name, L"switch"))
    {
        struct svg_style children = style;
        /* a group's shadow falls from the group; drawn per child, which is right for the single
         * shape a shadowed group holds in exported icons */
        children.group_shadow = style.shadow_count != 0;
        for (i = 0; i < node->child_count; ++i)
            draw_node(draw, node->children[i], children, &transform);
        ID2D1DeviceContext_SetTransform(draw->context, &transform);
    }
    else if (!wcscmp(node->name, L"image"))
        draw_image(draw, node, &style);
    else
    {
        ID2D1Geometry *geometry;
        if (SUCCEEDED(shape_geometry(draw->factory, node, style.evenodd, &geometry)) && geometry)
        {
            draw_shape(draw, geometry, &style);
            ID2D1Geometry_Release(geometry);
        }
    }

    end_clip(draw, &mask, &transform);
    end_clip(draw, &clip, &transform);
}

static void d2d_svg_document_draw_direct(ID2D1DeviceContext *context, struct d2d_svg_document *document)
{
    D2D1_MATRIX_3X2_F saved, viewbox_transform = identity, transform;
    struct svg_style style;
    struct svg_draw draw;
    const WCHAR *value;
    float vb[4], width, height;

    if (!document->root) return;

    /* the viewBox is fitted into the viewport: xMidYMid meet, the default preserveAspectRatio */
    width = document->viewport.width;
    height = document->viewport.height;
    if ((value = svg_attr(document->root, L"viewBox")) && (value = parse_number(value, &vb[0]))
            && (value = parse_number(value, &vb[1])) && (value = parse_number(value, &vb[2]))
            && parse_number(value, &vb[3]) && vb[2] > 0.0f && vb[3] > 0.0f)
    {
        const WCHAR *par = svg_attr(document->root, L"preserveAspectRatio");
        float sx = width / vb[2], sy = height / vb[3];

        if (par && !wcsncmp(par, L"none", 4))
        {
            viewbox_transform._11 = sx;
            viewbox_transform._22 = sy;
            viewbox_transform._31 = -vb[0] * sx;
            viewbox_transform._32 = -vb[1] * sy;
        }
        else
        {
            float s = min(sx, sy);
            viewbox_transform._11 = viewbox_transform._22 = s;
            viewbox_transform._31 = -vb[0] * s + (width - vb[2] * s) / 2.0f;
            viewbox_transform._32 = -vb[1] * s + (height - vb[3] * s) / 2.0f;
        }
    }

    memset(&style, 0, sizeof(style));
    style.has_fill = TRUE;   /* the initial fill is black, the initial stroke none */
    style.fill.a = 1.0f;
    style.fill_opacity = style.stroke_opacity = style.opacity = 1.0f;
    style.stroke_width = 1.0f;
    style.miter_limit = 4.0f;
    style.cap = D2D1_CAP_STYLE_FLAT;
    style.join = D2D1_LINE_JOIN_MITER_OR_BEVEL;

    ID2D1DeviceContext_GetTransform(context, &saved);
    transform = matrix_multiply(&viewbox_transform, &saved);

    draw.context = context;
    draw.factory = document->factory;
    draw.root = document->root;
    draw_node(&draw, document->root, style, &transform);

    ID2D1DeviceContext_SetTransform(context, &saved);
}

static void d2d_svg_draw_callback(struct d2d_device_context *context, void *data)
{
    d2d_svg_document_draw_direct((ID2D1DeviceContext *)&context->ID2D1DeviceContext6_iface, data);
}

void d2d_svg_document_draw(ID2D1DeviceContext *context, ID2D1SvgDocument *iface)
{
    struct d2d_svg_document *document;

    if (!iface || iface->lpVtbl != &d2d_svg_document_vtbl)
    {
        WARN("not our document %p.\n", iface);
        return;
    }
    document = impl_from_ID2D1SvgDocument(iface);
    if (document->root)
        d2d_device_context_draw_antialiased(CONTAINING_RECORD(context, struct d2d_device_context, ID2D1DeviceContext6_iface),
                d2d_svg_draw_callback, document);
}

/* ---------------------------------------------------------------------------------------------
 * ID2D1SvgElement: a read-only view of a parsed node. Win2D wraps the root in its own
 * CanvasSvgNamedElement as soon as a document is loaded, so GetRoot has to hand one out; the
 * structural queries are answered from the tree, the mutators and the typed attribute accessors
 * are not implemented. One object per node, so identity comparisons hold.
 *
 * Slots after ID2D1Resource (d2d1svg.h): 4 GetDocument, 5 GetTagName, 6 GetTagNameLength,
 * 7 IsTextContent, 8 GetParent, 9 HasChildren, 10 GetFirstChild, 11 GetLastChild,
 * 12 GetPreviousChild, 13 GetNextChild, 14 InsertChildBefore, 15 AppendChild, 16 ReplaceChild,
 * 17 RemoveChild, 18 CreateChild, 19 IsAttributeSpecified, 20 GetSpecifiedAttributeCount,
 * 21 GetSpecifiedAttributeName, 22 GetSpecifiedAttributeNameLength, 23 RemoveAttribute,
 * 24 SetTextValue, 25 GetTextValue, 26 GetTextValueLength, 27-29 SetAttributeValue (three
 * overloads), 30-32 GetAttributeValue (three), 33 GetAttributeValueLength. The overloads' order
 * within their group is MSVC's and unverified, so those slots are traced thunks naming their slot.
 * ------------------------------------------------------------------------------------------- */
struct svg_element
{
    const void **vtbl;
    LONG refcount;
    struct d2d_svg_document *document;
    struct svg_node *node;
};

static const void *svg_element_vtbl[34];
static HRESULT svg_element_get(struct d2d_svg_document *document, struct svg_node *node, ID2D1SvgElement **out);

static struct svg_element *element_from(void *iface)
{
    struct svg_element *element = iface;
    return element && element->vtbl == svg_element_vtbl ? element : NULL;
}

static HRESULT STDMETHODCALLTYPE svg_element_QueryInterface(void *iface, REFIID iid, void **out)
{
    struct svg_element *element = iface;

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1SvgElement) || IsEqualGUID(iid, &IID_ID2D1Resource) ||
        IsEqualGUID(iid, &IID_IUnknown))
    {
        InterlockedIncrement(&element->refcount);
        *out = iface;
        return S_OK;
    }
    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE svg_element_AddRef(void *iface)
{
    return InterlockedIncrement(&((struct svg_element *)iface)->refcount);
}

static ULONG STDMETHODCALLTYPE svg_element_Release(void *iface)
{
    struct svg_element *element = iface;
    ULONG refcount = InterlockedDecrement(&element->refcount);

    if (!refcount)
    {
        struct d2d_svg_document *document = element->document;
        element->node->element = NULL;
        free(element);
        ID2D1SvgDocument_Release(&document->ID2D1SvgDocument_iface);
    }
    return refcount;
}

static void STDMETHODCALLTYPE svg_element_GetFactory(void *iface, ID2D1Factory **factory)
{
    ID2D1Factory_AddRef(*factory = ((struct svg_element *)iface)->document->factory);
}

static void STDMETHODCALLTYPE svg_element_GetDocument(void *iface, ID2D1SvgDocument **document)
{
    struct svg_element *element = iface;
    ID2D1SvgDocument_AddRef(*document = &element->document->ID2D1SvgDocument_iface);
}

static HRESULT copy_string(const WCHAR *str, WCHAR *out, UINT32 count)
{
    size_t len = wcslen(str);
    if (!out || count <= len) return E_INVALIDARG;
    memcpy(out, str, (len + 1) * sizeof(WCHAR));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE svg_element_GetTagName(void *iface, WCHAR *name, UINT32 count)
{
    return copy_string(((struct svg_element *)iface)->node->name, name, count);
}

static UINT32 STDMETHODCALLTYPE svg_element_GetTagNameLength(void *iface)
{
    return wcslen(((struct svg_element *)iface)->node->name);
}

static BOOL STDMETHODCALLTYPE svg_element_IsTextContent(void *iface)
{
    return FALSE;   /* text nodes are not kept */
}

static void STDMETHODCALLTYPE svg_element_GetParent(void *iface, ID2D1SvgElement **parent)
{
    struct svg_element *element = iface;
    *parent = NULL;
    if (element->node->parent) svg_element_get(element->document, element->node->parent, parent);
}

static BOOL STDMETHODCALLTYPE svg_element_HasChildren(void *iface)
{
    return !!((struct svg_element *)iface)->node->child_count;
}

static void STDMETHODCALLTYPE svg_element_GetFirstChild(void *iface, ID2D1SvgElement **child)
{
    struct svg_element *element = iface;
    *child = NULL;
    if (element->node->child_count) svg_element_get(element->document, element->node->children[0], child);
}

static void STDMETHODCALLTYPE svg_element_GetLastChild(void *iface, ID2D1SvgElement **child)
{
    struct svg_element *element = iface;
    size_t count = element->node->child_count;
    *child = NULL;
    if (count) svg_element_get(element->document, element->node->children[count - 1], child);
}

static HRESULT sibling(struct svg_element *element, void *reference, int offset, ID2D1SvgElement **out)
{
    struct svg_element *ref = element_from(reference);
    size_t i;

    *out = NULL;
    if (!ref || ref->node->parent != element->node) return E_INVALIDARG;
    for (i = 0; i < element->node->child_count; ++i)
        if (element->node->children[i] == ref->node) break;
    if ((offset < 0 && !i) || i + offset >= element->node->child_count) return S_OK;
    return svg_element_get(element->document, element->node->children[i + offset], out);
}

static HRESULT STDMETHODCALLTYPE svg_element_GetPreviousChild(void *iface, void *reference, ID2D1SvgElement **out)
{
    return sibling(iface, reference, -1, out);
}

static HRESULT STDMETHODCALLTYPE svg_element_GetNextChild(void *iface, void *reference, ID2D1SvgElement **out)
{
    return sibling(iface, reference, 1, out);
}

static BOOL STDMETHODCALLTYPE svg_element_IsAttributeSpecified(void *iface, const WCHAR *name, BOOL *inherited)
{
    if (inherited) *inherited = FALSE;
    return name && svg_attr(((struct svg_element *)iface)->node, name) != NULL;
}

static UINT32 STDMETHODCALLTYPE svg_element_GetSpecifiedAttributeCount(void *iface)
{
    return ((struct svg_element *)iface)->node->attr_count;
}

static HRESULT STDMETHODCALLTYPE svg_element_GetSpecifiedAttributeName(void *iface, UINT32 index, WCHAR *name,
        UINT32 count, BOOL *inherited)
{
    struct svg_node *node = ((struct svg_element *)iface)->node;
    if (inherited) *inherited = FALSE;
    if (index >= node->attr_count) return E_INVALIDARG;
    return copy_string(node->attrs[index].name, name, count);
}

static HRESULT STDMETHODCALLTYPE svg_element_GetSpecifiedAttributeNameLength(void *iface, UINT32 index,
        UINT32 *length, BOOL *inherited)
{
    struct svg_node *node = ((struct svg_element *)iface)->node;
    if (inherited) *inherited = FALSE;
    if (index >= node->attr_count) return E_INVALIDARG;
    *length = wcslen(node->attrs[index].name);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE svg_element_GetTextValue(void *iface, WCHAR *value, UINT32 count)
{
    return copy_string(L"", value, count);
}

static UINT32 STDMETHODCALLTYPE svg_element_GetTextValueLength(void *iface)
{
    return 0;
}

#define SVG_ELEMENT_STUB(slot, what) \
    static HRESULT STDMETHODCALLTYPE svg_element_slot##slot(void *iface) \
    { \
        FIXME("iface %p: ID2D1SvgElement slot %u (%s) is not implemented.\n", iface, slot, what); \
        return E_NOTIMPL; \
    }
SVG_ELEMENT_STUB(24, "SetTextValue")
SVG_ELEMENT_STUB(29, "SetAttributeValue(ID2D1SvgAttribute)")
SVG_ELEMENT_STUB(32, "GetAttributeValue(riid)")
#undef SVG_ELEMENT_STUB

/* ---- mutation ---- */

static void set_subtree_document(struct svg_node *node, struct d2d_svg_document *document)
{
    struct svg_element *element = node->element;
    size_t i;

    if (element && element->document != document)
    {
        struct d2d_svg_document *old = element->document;
        ID2D1SvgDocument_AddRef(&document->ID2D1SvgDocument_iface);
        element->document = document;
        ID2D1SvgDocument_Release(&old->ID2D1SvgDocument_iface);
    }
    for (i = 0; i < node->child_count; ++i) set_subtree_document(node->children[i], document);
}

static void undetach(struct d2d_svg_document *document, struct svg_node *node)
{
    size_t i;
    for (i = 0; i < document->detached_count; ++i)
    {
        if (document->detached[i] != node) continue;
        document->detached[i] = document->detached[--document->detached_count];
        return;
    }
}

static BOOL detach(struct d2d_svg_document *document, struct svg_node *node)
{
    struct svg_node **list;

    if (!(list = realloc(document->detached, (document->detached_count + 1) * sizeof(*list)))) return FALSE;
    document->detached = list;
    list[document->detached_count++] = node;
    return TRUE;
}

/* Takes a node out of wherever it is - a parent, another document's root, a detached list. */
static void unlink_node(struct svg_element *element)
{
    struct svg_node *node = element->node, *parent = node->parent;
    size_t i;

    if (parent)
    {
        for (i = 0; i < parent->child_count; ++i)
            if (parent->children[i] == node) break;
        if (i < parent->child_count)
        {
            memmove(&parent->children[i], &parent->children[i + 1], (parent->child_count - i - 1) * sizeof(node));
            --parent->child_count;
        }
        node->parent = NULL;
    }
    else if (element->document->root == node)
        element->document->root = NULL;
    else
        undetach(element->document, node);
}

static BOOL is_ancestor(const struct svg_node *node, const struct svg_node *of)
{
    for (; of; of = of->parent) if (of == node) return TRUE;
    return FALSE;
}

static HRESULT insert_child(struct svg_element *parent, void *new_child, void *reference)
{
    struct svg_element *child = element_from(new_child), *ref = reference ? element_from(reference) : NULL;
    struct svg_node *node = parent->node, **children;
    size_t index, i;

    if (!child || (reference && (!ref || ref->node->parent != node))) return E_INVALIDARG;
    if (is_ancestor(child->node, node)) return E_INVALIDARG;   /* a node cannot contain itself */
    if (ref == child) return S_OK;
    /* the position is found after unlinking: the child may already be one of this node's, and
     * taking it out shifts everything after it */
    unlink_node(child);
    index = node->child_count;
    if (ref)
        for (i = 0; i < node->child_count; ++i)
            if (node->children[i] == ref->node) index = i;
    if (node->child_count == node->child_capacity)
    {
        size_t capacity = max(4, node->child_capacity * 2);
        if (!(children = realloc(node->children, capacity * sizeof(*children))))
        {
            detach(child->document, child->node);
            return E_OUTOFMEMORY;
        }
        node->children = children;
        node->child_capacity = capacity;
    }
    memmove(&node->children[index + 1], &node->children[index], (node->child_count - index) * sizeof(*children));
    node->children[index] = child->node;
    ++node->child_count;
    child->node->parent = node;
    set_subtree_document(child->node, parent->document);
    parent->document->modified = TRUE;
    return S_OK;
}

/* slot 14 */
static HRESULT STDMETHODCALLTYPE svg_element_InsertChildBefore(void *iface, void *new_child, void *reference)
{
    TRACE("iface %p, new_child %p, reference %p.\n", iface, new_child, reference);
    return insert_child(iface, new_child, reference);
}

/* slot 15 */
static HRESULT STDMETHODCALLTYPE svg_element_AppendChild(void *iface, void *new_child)
{
    TRACE("iface %p, new_child %p.\n", iface, new_child);
    return insert_child(iface, new_child, NULL);
}

/* slot 17 */
static HRESULT STDMETHODCALLTYPE svg_element_RemoveChild(void *iface, void *old_child)
{
    struct svg_element *parent = iface, *child = element_from(old_child);

    TRACE("iface %p, old_child %p.\n", iface, old_child);

    if (!child || child->node->parent != parent->node) return E_INVALIDARG;
    unlink_node(child);
    if (!detach(parent->document, child->node)) return E_OUTOFMEMORY;
    parent->document->modified = TRUE;
    return S_OK;
}

/* slot 16 */
static HRESULT STDMETHODCALLTYPE svg_element_ReplaceChild(void *iface, void *new_child, void *old_child)
{
    HRESULT hr;

    TRACE("iface %p, new_child %p, old_child %p.\n", iface, new_child, old_child);

    if (new_child == old_child) return S_OK;
    if (FAILED(hr = insert_child(iface, new_child, old_child))) return hr;
    return svg_element_RemoveChild(iface, old_child);
}

/* slot 18 */
static HRESULT STDMETHODCALLTYPE svg_element_CreateChild(void *iface, const WCHAR *tag, ID2D1SvgElement **out)
{
    struct svg_element *parent = iface, *child;
    struct svg_node *node;
    HRESULT hr;

    TRACE("iface %p, tag %s, out %p.\n", iface, debugstr_w(tag), out);

    *out = NULL;
    if (!tag || !*tag) return E_INVALIDARG;
    if (!(node = calloc(1, sizeof(*node))) || !(node->name = wcsdup(tag)))
    {
        free(node);
        return E_OUTOFMEMORY;
    }
    if (!detach(parent->document, node))
    {
        svg_node_free(node);
        return E_OUTOFMEMORY;
    }
    if (FAILED(hr = svg_element_get(parent->document, node, out))) return hr;
    child = element_from(*out);
    if (FAILED(hr = insert_child(parent, child, NULL)))
    {
        svg_element_Release(*out);
        *out = NULL;
    }
    return hr;
}

/* slot 23 */
static HRESULT STDMETHODCALLTYPE svg_element_RemoveAttribute(void *iface, const WCHAR *name)
{
    struct svg_element *element = iface;
    struct svg_node *node = element->node;
    size_t i;

    TRACE("iface %p, name %s.\n", iface, debugstr_w(name));

    for (i = 0; name && i < node->attr_count; ++i)
    {
        if (wcscmp(node->attrs[i].name, name)) continue;
        free(node->attrs[i].name);
        free(node->attrs[i].value);
        node->attrs[i] = node->attrs[--node->attr_count];
        element->document->modified = TRUE;
        return S_OK;
    }
    return S_OK;
}

/* D2D1_SVG_ATTRIBUTE_POD_TYPE and _STRING_TYPE, from d2d1svg.h */
enum { POD_FLOAT, POD_COLOR, POD_FILL_MODE, POD_DISPLAY, POD_OVERFLOW, POD_LINE_CAP, POD_LINE_JOIN,
       POD_VISIBILITY, POD_MATRIX, POD_UNIT_TYPE, POD_EXTEND_MODE, POD_PRESERVE_ASPECT_RATIO,
       POD_VIEWBOX, POD_LENGTH };

static HRESULT set_attr(struct svg_element *element, const WCHAR *name, const WCHAR *value)
{
    struct svg_node *node = element->node;
    struct svg_attr *attrs;
    WCHAR *copy;
    size_t i;

    if (!name || !value) return E_INVALIDARG;
    if (!(copy = wcsdup(value))) return E_OUTOFMEMORY;
    element->document->modified = TRUE;
    for (i = 0; i < node->attr_count; ++i)
    {
        if (wcscmp(node->attrs[i].name, name)) continue;
        free(node->attrs[i].value);
        node->attrs[i].value = copy;
        return S_OK;
    }
    if (!(attrs = realloc(node->attrs, (node->attr_count + 1) * sizeof(*attrs))) ||
        !(attrs[node->attr_count].name = wcsdup(name)))
    {
        if (attrs) node->attrs = attrs;
        free(copy);
        return E_OUTOFMEMORY;
    }
    node->attrs = attrs;
    attrs[node->attr_count++].value = copy;
    return S_OK;
}

/* slot 27: SetAttributeValue(name, D2D1_SVG_ATTRIBUTE_STRING_TYPE, value) - SVG text or an id */
static HRESULT STDMETHODCALLTYPE svg_element_SetAttributeValueString(void *iface, const WCHAR *name,
        UINT32 type, const WCHAR *value)
{
    TRACE("iface %p, name %s, type %u, value %s.\n", iface, debugstr_w(name), type, debugstr_w(value));
    return set_attr(iface, name, value);
}

/* slot 28: SetAttributeValue(name, D2D1_SVG_ATTRIBUTE_POD_TYPE, value, size). Stored as the SVG
 * text the renderer reads. Shapr3D recolours its icons for the theme this way. */
static HRESULT STDMETHODCALLTYPE svg_element_SetAttributeValuePod(void *iface, const WCHAR *name, UINT32 type,
        const void *value, UINT32 size)
{
    WCHAR text[128];

    TRACE("iface %p, name %s, type %u, value %p, size %u.\n", iface, debugstr_w(name), type, value, size);

    if (!value) return E_INVALIDARG;
    switch (type)
    {
    case POD_FLOAT:
        if (size != sizeof(float)) return E_INVALIDARG;
        swprintf(text, ARRAY_SIZE(text), L"%g", *(const float *)value);
        break;
    case POD_COLOR:
    {
        const D2D1_COLOR_F *c = value;
        if (size != sizeof(*c)) return E_INVALIDARG;
        swprintf(text, ARRAY_SIZE(text), L"rgb(%d,%d,%d,%g)", (int)(c->r * 255.0f + 0.5f),
                (int)(c->g * 255.0f + 0.5f), (int)(c->b * 255.0f + 0.5f), c->a);
        break;
    }
    case POD_FILL_MODE:
        if (size != sizeof(UINT32)) return E_INVALIDARG;
        wcscpy(text, *(const UINT32 *)value == D2D1_FILL_MODE_ALTERNATE ? L"evenodd" : L"nonzero");
        break;
    case POD_DISPLAY:
        if (size != sizeof(UINT32)) return E_INVALIDARG;
        wcscpy(text, *(const UINT32 *)value ? L"none" : L"inline");
        break;
    case POD_LINE_CAP:
        if (size != sizeof(UINT32)) return E_INVALIDARG;
        wcscpy(text, *(const UINT32 *)value == D2D1_CAP_STYLE_ROUND ? L"round"
                : *(const UINT32 *)value == D2D1_CAP_STYLE_SQUARE ? L"square" : L"butt");
        break;
    case POD_LINE_JOIN:
        if (size != sizeof(UINT32)) return E_INVALIDARG;
        wcscpy(text, *(const UINT32 *)value == D2D1_LINE_JOIN_ROUND ? L"round"
                : *(const UINT32 *)value == D2D1_LINE_JOIN_BEVEL ? L"bevel" : L"miter");
        break;
    case POD_MATRIX:
    {
        const D2D1_MATRIX_3X2_F *m = value;
        if (size != sizeof(*m)) return E_INVALIDARG;
        swprintf(text, ARRAY_SIZE(text), L"matrix(%g %g %g %g %g %g)", m->_11, m->_12, m->_21, m->_22, m->_31, m->_32);
        break;
    }
    case POD_VIEWBOX:
    {
        const D2D1_RECT_F *r = value;   /* D2D1_SVG_VIEWBOX is x, y, width, height */
        if (size != sizeof(*r)) return E_INVALIDARG;
        swprintf(text, ARRAY_SIZE(text), L"%g %g %g %g", r->left, r->top, r->right, r->bottom);
        break;
    }
    case POD_LENGTH:
    {
        const D2D1_SVG_LENGTH *l = value;
        if (size != sizeof(*l)) return E_INVALIDARG;
        swprintf(text, ARRAY_SIZE(text), l->units == D2D1_SVG_LENGTH_UNITS_PERCENTAGE ? L"%g%%" : L"%g", l->value);
        break;
    }
    default:
        FIXME("attribute %s of POD type %u is not supported.\n", debugstr_w(name), type);
        return E_NOTIMPL;
    }
    return set_attr(iface, name, text);
}

/* slot 30: GetAttributeValue(name, D2D1_SVG_ATTRIBUTE_STRING_TYPE, value, count) */
static HRESULT STDMETHODCALLTYPE svg_element_GetAttributeValueString(void *iface, const WCHAR *name, UINT32 type,
        WCHAR *value, UINT32 count)
{
    const WCHAR *str = name ? svg_attr(((struct svg_element *)iface)->node, name) : NULL;
    TRACE("iface %p, name %s, type %u.\n", iface, debugstr_w(name), type);
    if (!str)
    {
        WARN("attribute %s is not specified; answering with an empty value.\n", debugstr_w(name));
        str = L"";
    }
    return copy_string(str, value, count);
}

/* slot 31: GetAttributeValue(name, D2D1_SVG_ATTRIBUTE_POD_TYPE, value, size) */
static HRESULT STDMETHODCALLTYPE svg_element_GetAttributeValuePod(void *iface, const WCHAR *name, UINT32 type,
        void *value, UINT32 size)
{
    const WCHAR *str = name ? svg_attr(((struct svg_element *)iface)->node, name) : NULL;

    TRACE("iface %p, name %s, type %u, size %u.\n", iface, debugstr_w(name), type, size);

    if (!value) return E_INVALIDARG;
    /* an attribute that is not specified reads as its initial value */
    if (!str)
    {
        WARN("attribute %s is not specified; answering with its initial value.\n", debugstr_w(name));
        switch (type)
        {
        case POD_COLOR:
        {
            D2D1_COLOR_F black = {0.0f, 0.0f, 0.0f, 1.0f};
            if (size != sizeof(black)) return E_INVALIDARG;
            memcpy(value, &black, sizeof(black));
            return S_OK;
        }
        case POD_MATRIX:
            if (size != sizeof(identity)) return E_INVALIDARG;
            memcpy(value, &identity, sizeof(identity));
            return S_OK;
        default:
            memset(value, 0, size);   /* 0, nonzero, inline, butt, a 0x0 viewBox, a length of 0 */
            return S_OK;
        }
    }
    switch (type)
    {
    case POD_FLOAT:
        if (size != sizeof(float) || !parse_number(str, value)) return E_INVALIDARG;
        return S_OK;
    case POD_COLOR:
    {
        D2D1_COLOR_F c = {0.0f, 0.0f, 0.0f, 1.0f};
        if (size != sizeof(c)) return E_INVALIDARG;
        parse_paint(str, &c);
        memcpy(value, &c, sizeof(c));
        return S_OK;
    }
    case POD_MATRIX:
    {
        D2D1_MATRIX_3X2_F m = parse_transform(str);
        if (size != sizeof(m)) return E_INVALIDARG;
        memcpy(value, &m, sizeof(m));
        return S_OK;
    }
    case POD_VIEWBOX:
    {
        float v[4];   /* D2D1_SVG_VIEWBOX: x, y, width, height */
        const WCHAR *q = str;
        unsigned int k;
        if (size != sizeof(v)) return E_INVALIDARG;
        for (k = 0; k < 4; ++k)
            if (!(q = parse_number(q, &v[k]))) return E_INVALIDARG;
        memcpy(value, v, sizeof(v));
        return S_OK;
    }
    case POD_LENGTH:
    {
        D2D1_SVG_LENGTH l;
        const WCHAR *q;
        if (size != sizeof(l) || !(q = parse_number(str, &l.value))) return E_INVALIDARG;
        l.units = *q == '%' ? D2D1_SVG_LENGTH_UNITS_PERCENTAGE : D2D1_SVG_LENGTH_UNITS_NUMBER;
        memcpy(value, &l, sizeof(l));
        return S_OK;
    }
    case POD_FILL_MODE:
    case POD_DISPLAY:
    case POD_LINE_CAP:
    case POD_LINE_JOIN:
    {
        UINT32 v;
        if (size != sizeof(v)) return E_INVALIDARG;
        if (type == POD_FILL_MODE) v = !wcscmp(str, L"evenodd") ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING;
        else if (type == POD_DISPLAY) v = !wcscmp(str, L"none");
        else if (type == POD_LINE_CAP)
            v = !wcscmp(str, L"round") ? D2D1_CAP_STYLE_ROUND : !wcscmp(str, L"square") ? D2D1_CAP_STYLE_SQUARE
                    : D2D1_CAP_STYLE_FLAT;
        else
            v = !wcscmp(str, L"round") ? D2D1_LINE_JOIN_ROUND : !wcscmp(str, L"bevel") ? D2D1_LINE_JOIN_BEVEL
                    : D2D1_LINE_JOIN_MITER;
        memcpy(value, &v, sizeof(v));
        return S_OK;
    }
    default:
        FIXME("reading attribute %s as POD type %u is not supported.\n", debugstr_w(name), type);
        return E_NOTIMPL;
    }
}

/* slot 33: GetAttributeValueLength(name, D2D1_SVG_ATTRIBUTE_STRING_TYPE, length) */
static HRESULT STDMETHODCALLTYPE svg_element_GetAttributeValueLength(void *iface, const WCHAR *name, UINT32 type,
        UINT32 *length)
{
    const WCHAR *str = name ? svg_attr(((struct svg_element *)iface)->node, name) : NULL;
    *length = str ? wcslen(str) : 0;
    return S_OK;
}

static const void *svg_element_vtbl[34] =
{
    svg_element_QueryInterface, svg_element_AddRef, svg_element_Release, svg_element_GetFactory,
    svg_element_GetDocument, svg_element_GetTagName, svg_element_GetTagNameLength, svg_element_IsTextContent,
    svg_element_GetParent, svg_element_HasChildren, svg_element_GetFirstChild, svg_element_GetLastChild,
    svg_element_GetPreviousChild, svg_element_GetNextChild, svg_element_InsertChildBefore,
    svg_element_AppendChild, svg_element_ReplaceChild, svg_element_RemoveChild, svg_element_CreateChild,
    svg_element_IsAttributeSpecified,
    svg_element_GetSpecifiedAttributeCount, svg_element_GetSpecifiedAttributeName,
    svg_element_GetSpecifiedAttributeNameLength, svg_element_RemoveAttribute, svg_element_slot24,
    svg_element_GetTextValue, svg_element_GetTextValueLength, svg_element_SetAttributeValueString,
    svg_element_SetAttributeValuePod, svg_element_slot29, svg_element_GetAttributeValueString,
    svg_element_GetAttributeValuePod, svg_element_slot32, svg_element_GetAttributeValueLength,
};

static HRESULT svg_element_get(struct d2d_svg_document *document, struct svg_node *node, ID2D1SvgElement **out)
{
    struct svg_element *element;

    if ((element = node->element))
    {
        InterlockedIncrement(&element->refcount);
        *out = (ID2D1SvgElement *)element;
        return S_OK;
    }
    if (!(element = calloc(1, sizeof(*element))))
    {
        *out = NULL;
        return E_OUTOFMEMORY;
    }
    element->vtbl = svg_element_vtbl;
    element->refcount = 1;
    element->document = document;
    element->node = node;
    ID2D1SvgDocument_AddRef(&document->ID2D1SvgDocument_iface);
    node->element = element;
    *out = (ID2D1SvgElement *)element;
    return S_OK;
}

/* ---------------------------------------------------------------------------------------------
 * ID2D1SvgDocument
 * ------------------------------------------------------------------------------------------- */
static HRESULT STDMETHODCALLTYPE d2d_svg_document_QueryInterface(ID2D1SvgDocument *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_ID2D1SvgDocument) || IsEqualGUID(iid, &IID_ID2D1Resource) ||
        IsEqualGUID(iid, &IID_IUnknown))
    {
        ID2D1SvgDocument_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d2d_svg_document_AddRef(ID2D1SvgDocument *iface)
{
    struct d2d_svg_document *document = impl_from_ID2D1SvgDocument(iface);
    ULONG refcount = InterlockedIncrement(&document->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE d2d_svg_document_Release(ID2D1SvgDocument *iface)
{
    struct d2d_svg_document *document = impl_from_ID2D1SvgDocument(iface);
    ULONG refcount = InterlockedDecrement(&document->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);
    if (!refcount)
    {
        size_t i;
        for (i = 0; i < document->detached_count; ++i) svg_node_free(document->detached[i]);
        free(document->detached);
        svg_node_free(document->root);
        free(document->xml);
        ID2D1Factory_Release(document->factory);
        free(document);
    }
    return refcount;
}

static void STDMETHODCALLTYPE d2d_svg_document_GetFactory(ID2D1SvgDocument *iface, ID2D1Factory **factory)
{
    struct d2d_svg_document *document = impl_from_ID2D1SvgDocument(iface);

    TRACE("iface %p, factory %p.\n", iface, factory);
    ID2D1Factory_AddRef(*factory = document->factory);
}

static HRESULT STDMETHODCALLTYPE d2d_svg_document_SetViewportSize(ID2D1SvgDocument *iface, D2D1_SIZE_F size)
{
    TRACE("iface %p, size %s.\n", iface, wine_dbg_sprintf("(%.8e, %.8e)", size.width, size.height));
    impl_from_ID2D1SvgDocument(iface)->viewport = size;
    return S_OK;
}

static D2D1_SIZE_F * STDMETHODCALLTYPE d2d_svg_document_GetViewportSize(ID2D1SvgDocument *iface, D2D1_SIZE_F *size)
{
    TRACE("iface %p, size %p.\n", iface, size);
    *size = impl_from_ID2D1SvgDocument(iface)->viewport;
    return size;
}

static HRESULT STDMETHODCALLTYPE d2d_svg_document_SetRoot(ID2D1SvgDocument *iface, ID2D1SvgElement *root)
{
    FIXME("iface %p, root %p stub!\n", iface, root);
    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE d2d_svg_document_GetRoot(ID2D1SvgDocument *iface, ID2D1SvgElement **root)
{
    struct d2d_svg_document *document = impl_from_ID2D1SvgDocument(iface);

    TRACE("iface %p, root %p.\n", iface, root);

    *root = NULL;
    if (document->root) svg_element_get(document, document->root, root);
}

static HRESULT STDMETHODCALLTYPE d2d_svg_document_FindElementById(ID2D1SvgDocument *iface, const WCHAR *id,
        ID2D1SvgElement **element)
{
    FIXME("iface %p, id %s, element %p stub!\n", iface, debugstr_w(id), element);
    *element = NULL;
    return E_NOTIMPL;
}

static void write_escaped(IStream *stream, const WCHAR *str)
{
    char buf[512];
    int len = 0;
    ULONG written;

    for (; *str; ++str)
    {
        const char *entity = NULL;
        if (*str == '&') entity = "&amp;";
        else if (*str == '<') entity = "&lt;";
        else if (*str == '>') entity = "&gt;";
        else if (*str == '"') entity = "&quot;";
        if (len > (int)sizeof(buf) - 8)
        {
            IStream_Write(stream, buf, len, &written);
            len = 0;
        }
        if (entity)
        {
            memcpy(buf + len, entity, strlen(entity));
            len += strlen(entity);
        }
        else len += WideCharToMultiByte(CP_UTF8, 0, str, 1, buf + len, sizeof(buf) - len, NULL, NULL);
    }
    if (len) IStream_Write(stream, buf, len, &written);
}

static void write_node(IStream *stream, const struct svg_node *node)
{
    ULONG written;
    size_t i;

    IStream_Write(stream, "<", 1, &written);
    write_escaped(stream, node->name);
    for (i = 0; i < node->attr_count; ++i)
    {
        IStream_Write(stream, " ", 1, &written);
        write_escaped(stream, node->attrs[i].name);
        IStream_Write(stream, "=\"", 2, &written);
        write_escaped(stream, node->attrs[i].value);
        IStream_Write(stream, "\"", 1, &written);
    }
    if (!node->child_count)
    {
        IStream_Write(stream, "/>", 2, &written);
        return;
    }
    IStream_Write(stream, ">", 1, &written);
    for (i = 0; i < node->child_count; ++i) write_node(stream, node->children[i]);
    IStream_Write(stream, "</", 2, &written);
    write_escaped(stream, node->name);
    IStream_Write(stream, ">", 1, &written);
}

/* Writes back the XML the document was made from, or the tree once it has been changed. */
static HRESULT STDMETHODCALLTYPE d2d_svg_document_Serialize(ID2D1SvgDocument *iface, IStream *stream,
        ID2D1SvgElement *subtree)
{
    struct d2d_svg_document *document = impl_from_ID2D1SvgDocument(iface);
    ULONG written;

    TRACE("iface %p, stream %p, subtree %p.\n", iface, stream, subtree);

    if (!stream) return E_INVALIDARG;
    /* the root is the whole document, which is the XML it was made from */
    if (subtree && (!element_from(subtree) || element_from(subtree)->node != document->root))
    {
        FIXME("serializing a subtree below the root is not implemented.\n");
        return E_NOTIMPL;
    }
    if (document->modified && document->root)
    {
        write_node(stream, document->root);
        return S_OK;
    }
    return IStream_Write(stream, document->xml, document->xml_size, &written);
}

static HRESULT STDMETHODCALLTYPE d2d_svg_document_Deserialize(ID2D1SvgDocument *iface, IStream *stream,
        ID2D1SvgElement **subtree)
{
    FIXME("iface %p, stream %p, subtree %p stub!\n", iface, stream, subtree);
    *subtree = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_svg_document_CreatePaint(ID2D1SvgDocument *iface, D2D1_SVG_PAINT_TYPE type,
        const D2D1_COLOR_F *colour, const WCHAR *id, ID2D1SvgPaint **paint)
{
    FIXME("iface %p, type %d, colour %p, id %s, paint %p stub!\n", iface, type, colour, debugstr_w(id), paint);
    *paint = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_svg_document_CreateStrokeDashArray(ID2D1SvgDocument *iface,
        const D2D1_SVG_LENGTH *dashes, UINT32 count, ID2D1SvgStrokeDashArray **array)
{
    FIXME("iface %p, dashes %p, count %u, array %p stub!\n", iface, dashes, count, array);
    *array = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_svg_document_CreatePointCollection(ID2D1SvgDocument *iface,
        const D2D1_POINT_2F *points, UINT32 count, ID2D1SvgPointCollection **collection)
{
    FIXME("iface %p, points %p, count %u, collection %p stub!\n", iface, points, count, collection);
    *collection = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE d2d_svg_document_CreatePathData(ID2D1SvgDocument *iface, const FLOAT *data,
        UINT32 data_count, const D2D1_SVG_PATH_COMMAND *commands, UINT32 commands_count, ID2D1SvgPathData **path)
{
    FIXME("iface %p, data %p, data_count %u, commands %p, commands_count %u, path %p stub!\n",
            iface, data, data_count, commands, commands_count, path);
    *path = NULL;
    return E_NOTIMPL;
}

static const ID2D1SvgDocumentVtbl d2d_svg_document_vtbl =
{
    d2d_svg_document_QueryInterface,
    d2d_svg_document_AddRef,
    d2d_svg_document_Release,
    d2d_svg_document_GetFactory,
    d2d_svg_document_SetViewportSize,
    d2d_svg_document_GetViewportSize,
    d2d_svg_document_SetRoot,
    d2d_svg_document_GetRoot,
    d2d_svg_document_FindElementById,
    d2d_svg_document_Serialize,
    d2d_svg_document_Deserialize,
    d2d_svg_document_CreatePaint,
    d2d_svg_document_CreateStrokeDashArray,
    d2d_svg_document_CreatePointCollection,
    d2d_svg_document_CreatePathData,
};

HRESULT d2d_svg_document_create(ID2D1Factory *factory, IStream *stream, D2D1_SIZE_F viewport,
        ID2D1SvgDocument **out)
{
    struct d2d_svg_document *document;
    SIZE_T capacity = 4096;
    ULONG read;
    HRESULT hr;

    *out = NULL;
    if (!(document = calloc(1, sizeof(*document)))) return E_OUTOFMEMORY;
    document->ID2D1SvgDocument_iface.lpVtbl = &d2d_svg_document_vtbl;
    document->refcount = 1;
    ID2D1Factory_AddRef(document->factory = factory);
    document->viewport = viewport;

    if (stream)
    {
        /* the whole stream, from its current position */
        for (;;)
        {
            BYTE *grown;
            if (document->xml_size == capacity || !document->xml)
            {
                if (document->xml) capacity *= 2;
                if (!(grown = realloc(document->xml, capacity)))
                {
                    ID2D1SvgDocument_Release(&document->ID2D1SvgDocument_iface);
                    return E_OUTOFMEMORY;
                }
                document->xml = grown;
            }
            if (FAILED(hr = IStream_Read(stream, document->xml + document->xml_size,
                    capacity - document->xml_size, &read)) || !read)
                break;
            document->xml_size += read;
        }
        if (FAILED(hr = svg_parse(document)))
        {
            WARN("the document did not parse, hr %#lx.\n", hr);
            ID2D1SvgDocument_Release(&document->ID2D1SvgDocument_iface);
            return hr;
        }
    }

    TRACE("created document %p, %Iu bytes of XML, viewport %s.\n", document, document->xml_size,
            wine_dbg_sprintf("(%.8e, %.8e)", viewport.width, viewport.height));
    *out = &document->ID2D1SvgDocument_iface;
    return S_OK;
}
