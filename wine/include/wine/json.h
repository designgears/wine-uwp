/*
 * A small JSON reader
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

/*
 * For the modules that read web services' answers (the Store, Xbox Live). json_parse returns a
 * tree, NULL if the text is not JSON; json_get and json_at never return NULL, so lookups chain:
 *
 *     json_str( json_get( json_get( json_at( json_get( root, "Products" ), 0 ), "Sku" ), "Id" ) )
 *
 * is "" when any step is missing. Strings are UTF-8. A number keeps its text, so json_str of one
 * is the number as the service wrote it.
 */

#ifndef __WINE_WINE_JSON_H
#define __WINE_WINE_JSON_H

#include <stdlib.h>
#include <string.h>

enum json_type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct json
{
    enum json_type type;
    int boolean;
    double number;
    char *string;               /* strings, and the text of numbers */
    unsigned int count;         /* array items or object members */
    struct json *items;
    char **keys;                /* object member names */
};

static const struct json json_none;

struct json_parser
{
    const char *p, *end;
};

static inline void json_skip_space( struct json_parser *jp )
{
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')) jp->p++;
}

static inline int json_add_char( char **buf, size_t *len, size_t *size, char c )
{
    char *grown;

    if (*len + 2 > *size)
    {
        *size = *size ? *size * 2 : 32;
        if (!(grown = realloc( *buf, *size ))) return 0;
        *buf = grown;
    }
    (*buf)[(*len)++] = c;
    (*buf)[*len] = 0;
    return 1;
}

static inline int json_add_utf8( char **buf, size_t *len, size_t *size, unsigned int cp )
{
    if (cp < 0x80) return json_add_char( buf, len, size, cp );
    if (cp < 0x800)
        return json_add_char( buf, len, size, 0xc0 | cp >> 6 ) && json_add_char( buf, len, size, 0x80 | (cp & 0x3f) );
    if (cp < 0x10000)
        return json_add_char( buf, len, size, 0xe0 | cp >> 12 ) &&
               json_add_char( buf, len, size, 0x80 | (cp >> 6 & 0x3f) ) &&
               json_add_char( buf, len, size, 0x80 | (cp & 0x3f) );
    return json_add_char( buf, len, size, 0xf0 | cp >> 18 ) &&
           json_add_char( buf, len, size, 0x80 | (cp >> 12 & 0x3f) ) &&
           json_add_char( buf, len, size, 0x80 | (cp >> 6 & 0x3f) ) &&
           json_add_char( buf, len, size, 0x80 | (cp & 0x3f) );
}

static inline unsigned int json_hex4( const char *p )
{
    char digits[5];
    memcpy( digits, p, 4 );
    digits[4] = 0;
    return strtoul( digits, NULL, 16 );
}

static inline int json_parse_string( struct json_parser *jp, char **out )
{
    size_t len = 0, size = 0;
    char *buf = NULL;
    unsigned int cp;
    int ok = 1;

    if (jp->p >= jp->end || *jp->p != '"') return 0;
    jp->p++;
    if (!json_add_char( &buf, &len, &size, 0 )) return 0;
    len = 0;
    while (ok && jp->p < jp->end && *jp->p != '"')
    {
        if (*jp->p != '\\' || jp->p + 1 >= jp->end)
        {
            ok = json_add_char( &buf, &len, &size, *jp->p++ );
            continue;
        }
        jp->p++;
        switch (*jp->p)
        {
        case 'n': ok = json_add_char( &buf, &len, &size, '\n' ); break;
        case 't': ok = json_add_char( &buf, &len, &size, '\t' ); break;
        case 'r': ok = json_add_char( &buf, &len, &size, '\r' ); break;
        case 'b': ok = json_add_char( &buf, &len, &size, '\b' ); break;
        case 'f': ok = json_add_char( &buf, &len, &size, '\f' ); break;
        case 'u':
            if (jp->end - jp->p < 5)
            {
                ok = 0;
                break;
            }
            cp = json_hex4( jp->p + 1 );
            jp->p += 4;
            if (cp >= 0xd800 && cp < 0xdc00 && jp->end - jp->p > 6 && jp->p[1] == '\\' && jp->p[2] == 'u')
            {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (json_hex4( jp->p + 3 ) - 0xdc00);
                jp->p += 6;
            }
            ok = json_add_utf8( &buf, &len, &size, cp );
            break;
        default: ok = json_add_char( &buf, &len, &size, *jp->p ); break;
        }
        jp->p++;
    }
    if (!ok || jp->p >= jp->end)
    {
        free( buf );
        return 0;
    }
    jp->p++;
    *out = buf;
    return 1;
}

static inline void json_free( struct json *value );

static inline void json_clear( struct json *value )
{
    unsigned int i;

    for (i = 0; i < value->count; i++)
    {
        json_clear( &value->items[i] );
        if (value->keys) free( value->keys[i] );
    }
    free( value->items );
    free( value->keys );
    free( value->string );
    memset( value, 0, sizeof(*value) );
}

/* Adds an item (and for an object, its key) to a container being parsed. */
static inline struct json *json_grow( struct json *value, char *key )
{
    struct json *items;
    char **keys;

    if (!(items = realloc( value->items, (value->count + 1) * sizeof(*items) ))) return NULL;
    value->items = items;
    if (value->type == JSON_OBJECT)
    {
        if (!(keys = realloc( value->keys, (value->count + 1) * sizeof(*keys) ))) return NULL;
        value->keys = keys;
        keys[value->count] = key;
    }
    memset( &items[value->count], 0, sizeof(*items) );
    return &items[value->count++];
}

static inline int json_parse_value( struct json_parser *jp, struct json *value, int depth )
{
    struct json *item;
    char *key, *end;

    if (depth > 64) return 0;
    json_skip_space( jp );
    if (jp->p >= jp->end) return 0;

    if (*jp->p == '{' || *jp->p == '[')
    {
        char close = *jp->p == '{' ? '}' : ']';

        value->type = *jp->p == '{' ? JSON_OBJECT : JSON_ARRAY;
        jp->p++;
        json_skip_space( jp );
        if (jp->p < jp->end && *jp->p == close)
        {
            jp->p++;
            return 1;
        }
        for (;;)
        {
            key = NULL;
            if (value->type == JSON_OBJECT)
            {
                json_skip_space( jp );
                if (!json_parse_string( jp, &key )) return 0;
                json_skip_space( jp );
                if (jp->p >= jp->end || *jp->p != ':')
                {
                    free( key );
                    return 0;
                }
                jp->p++;
            }
            if (!(item = json_grow( value, key )))
            {
                free( key );
                return 0;
            }
            if (!json_parse_value( jp, item, depth + 1 )) return 0;
            json_skip_space( jp );
            if (jp->p < jp->end && *jp->p == ',')
            {
                jp->p++;
                continue;
            }
            if (jp->p < jp->end && *jp->p == close)
            {
                jp->p++;
                return 1;
            }
            return 0;
        }
    }
    if (*jp->p == '"')
    {
        value->type = JSON_STRING;
        return json_parse_string( jp, &value->string );
    }
    if (jp->end - jp->p >= 4 && !strncmp( jp->p, "true", 4 ))
    {
        value->type = JSON_BOOL;
        value->boolean = 1;
        jp->p += 4;
        return 1;
    }
    if (jp->end - jp->p >= 5 && !strncmp( jp->p, "false", 5 ))
    {
        value->type = JSON_BOOL;
        jp->p += 5;
        return 1;
    }
    if (jp->end - jp->p >= 4 && !strncmp( jp->p, "null", 4 ))
    {
        jp->p += 4;
        return 1;
    }
    value->number = strtod( jp->p, &end );
    if (end == jp->p || end > jp->end) return 0;
    value->type = JSON_NUMBER;
    if (!(value->string = malloc( end - jp->p + 1 ))) return 0;
    memcpy( value->string, jp->p, end - jp->p );
    value->string[end - jp->p] = 0;
    jp->p = end;
    return 1;
}

/* text need not be NUL-terminated; len is its length. */
static inline struct json *json_parse( const char *text, size_t len )
{
    struct json_parser jp = { text, text + len };
    struct json *value;

    if (!text || !(value = calloc( 1, sizeof(*value) ))) return NULL;
    if (!json_parse_value( &jp, value, 0 ))
    {
        json_free( value );
        return NULL;
    }
    return value;
}

static inline void json_free( struct json *value )
{
    if (!value) return;
    json_clear( value );
    free( value );
}

static inline const struct json *json_get( const struct json *value, const char *key )
{
    unsigned int i;

    if (value && value->type == JSON_OBJECT)
        for (i = 0; i < value->count; i++) if (!strcmp( value->keys[i], key )) return &value->items[i];
    return &json_none;
}

static inline const struct json *json_at( const struct json *value, unsigned int index )
{
    if (value && value->type == JSON_ARRAY && index < value->count) return &value->items[index];
    return &json_none;
}

static inline unsigned int json_count( const struct json *value )
{
    return value && (value->type == JSON_ARRAY || value->type == JSON_OBJECT) ? value->count : 0;
}

/* A string's text, or a number's; "" otherwise. */
static inline const char *json_str( const struct json *value )
{
    return value && (value->type == JSON_STRING || value->type == JSON_NUMBER) && value->string ? value->string : "";
}

#endif /* __WINE_WINE_JSON_H */
