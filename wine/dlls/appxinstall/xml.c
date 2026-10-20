/*
 * appxinstall: a small element tree over xmllite
 *
 * Package manifests are read by element local name, ignoring namespaces: the schemas add a namespace per Windows
 * release (uap, uap10, desktop6, ...) for what is the same element to us.
 *
 * Copyright 2026 wine-uwp contributors
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

#define COBJMACROS
#include <stdlib.h>
#include <string.h>

#include "appxinstall_private.h"
#include "objbase.h"
#include "shlwapi.h"
#include "initguid.h"
#include "xmllite.h"

static WCHAR *strndupW( const WCHAR *s, UINT len )
{
    WCHAR *r = malloc( (len + 1) * sizeof(WCHAR) );
    if (r) { memcpy( r, s, len * sizeof(WCHAR) ); r[len] = 0; }
    return r;
}

static void append_child( struct xml_node *parent, struct xml_node *child )
{
    struct xml_node **p = &parent->children;
    while (*p) p = &(*p)->next;
    *p = child;
    child->parent = parent;
}

void xml_free( struct xml_node *node )
{
    unsigned int i;

    while (node)
    {
        struct xml_node *next = node->next;
        xml_free( node->children );
        for (i = 0; i < node->attr_count; i++)
        {
            free( node->attrs[i].name );
            free( node->attrs[i].value );
        }
        free( node->attrs );
        free( node->name );
        free( node->text );
        free( node );
        node = next;
    }
}

/* parses data (UTF-8 or UTF-16 with a BOM, as xmllite detects) into a tree under a nameless root */
HRESULT xml_parse( const BYTE *data, ULONG size, struct xml_node **out )
{
    struct xml_node *root, *current;
    IXmlReader *reader = NULL;
    IStream *stream;
    XmlNodeType type;
    HRESULT hr;

    if (!(root = calloc( 1, sizeof(*root) ))) return E_OUTOFMEMORY;
    if (!(stream = SHCreateMemStream( data, size ))) { free( root ); return E_OUTOFMEMORY; }
    if (FAILED(hr = CreateXmlReader( &IID_IXmlReader, (void **)&reader, NULL )) ||
        FAILED(hr = IXmlReader_SetProperty( reader, XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit )) ||
        FAILED(hr = IXmlReader_SetInput( reader, (IUnknown *)stream )))
        goto done;

    current = root;
    while ((hr = IXmlReader_Read( reader, &type )) == S_OK)
    {
        const WCHAR *s;
        UINT len;

        if (type == XmlNodeType_Element)
        {
            struct xml_node *node;
            BOOL empty = IXmlReader_IsEmptyElement( reader );

            if (!(node = calloc( 1, sizeof(*node) ))) { hr = E_OUTOFMEMORY; break; }
            IXmlReader_GetLocalName( reader, &s, &len );
            node->name = strndupW( s, len );
            append_child( current, node );
            if ((hr = IXmlReader_MoveToFirstAttribute( reader )) == S_OK)
            {
                do
                {
                    struct xml_attr *attrs = realloc( node->attrs, (node->attr_count + 1) * sizeof(*attrs) );
                    if (!attrs) { hr = E_OUTOFMEMORY; break; }
                    node->attrs = attrs;
                    IXmlReader_GetLocalName( reader, &s, &len );
                    attrs[node->attr_count].name = strndupW( s, len );
                    IXmlReader_GetValue( reader, &s, &len );
                    attrs[node->attr_count].value = strndupW( s, len );
                    node->attr_count++;
                } while ((hr = IXmlReader_MoveToNextAttribute( reader )) == S_OK);
                IXmlReader_MoveToElement( reader );
            }
            if (FAILED(hr)) break;
            if (!empty) current = node;
        }
        else if (type == XmlNodeType_EndElement)
        {
            if (current->parent) current = current->parent;
        }
        else if (type == XmlNodeType_Text || type == XmlNodeType_CDATA)
        {
            UINT old = current->text ? wcslen( current->text ) : 0;
            WCHAR *text;

            IXmlReader_GetValue( reader, &s, &len );
            if (!(text = realloc( current->text, (old + len + 1) * sizeof(WCHAR) ))) { hr = E_OUTOFMEMORY; break; }
            memcpy( text + old, s, len * sizeof(WCHAR) );
            text[old + len] = 0;
            current->text = text;
        }
    }
    if (hr == S_FALSE) hr = S_OK;

done:
    if (reader) IXmlReader_Release( reader );
    IStream_Release( stream );
    if (FAILED(hr))
    {
        xml_free( root );
        return hr;
    }
    *out = root;
    return S_OK;
}

const WCHAR *xml_attr( const struct xml_node *node, const WCHAR *name )
{
    unsigned int i;

    if (!node) return NULL;
    for (i = 0; i < node->attr_count; i++)
        if (!wcscmp( node->attrs[i].name, name )) return node->attrs[i].value;
    return NULL;
}

/* The next element named name below scope, in document order, after prev (NULL: the first). Like ElementTree's
 * iter(): every descendant, not only children. */
const struct xml_node *xml_next( const struct xml_node *scope, const struct xml_node *prev, const WCHAR *name )
{
    const struct xml_node *node = prev;

    for (;;)
    {
        if (!node) node = scope->children;
        else if (node->children) node = node->children;
        else
        {
            while (node != scope && !node->next) node = node->parent;
            if (node == scope) return NULL;
            node = node->next;
        }
        if (!node) return NULL;
        if (!wcscmp( node->name, name )) return node;
    }
}

const struct xml_node *xml_find( const struct xml_node *scope, const WCHAR *name )
{
    return scope ? xml_next( scope, NULL, name ) : NULL;
}
