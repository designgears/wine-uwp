/*
 * appxinstall: internal interfaces
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

#ifndef __APPXINSTALL_PRIVATE_H
#define __APPXINSTALL_PRIVATE_H

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winternl.h"
#include "bcrypt.h"
#include "wine/debug.h"
#include "wine/appxinstall.h"

/* crypto.c */
struct xts;
HRESULT sha256( const void *data, ULONG size, BYTE digest[32] );
HRESULT xts_create( const BYTE key[32], struct xts **xts );
void xts_destroy( struct xts *xts );
HRESULT xts_decrypt_unit( struct xts *xts, BYTE *data, ULONG size, const BYTE tweak[16] );
HRESULT xts_self_test(void);

/* source.c: a package's bytes, from a file or an http(s) URL read with range requests */
struct source;
HRESULT source_open( const WCHAR *path, const struct appx_install_params *params, struct source **source );
HRESULT source_open_range( struct source *parent, UINT64 offset, UINT64 size, struct source **source );
UINT64 source_size( const struct source *source );
HRESULT source_read( struct source *source, UINT64 offset, void *buffer, ULONG size );
void source_close( struct source *source );
BOOL is_url( const WCHAR *path );

/* zip.c */
struct zip;
struct zip_entry
{
    WCHAR *name;            /* as stored: '/'-separated, percent-encoded in packages */
    UINT64 compressed, size, header;
    UINT32 crc;
    UINT16 method;          /* 0 stored, 8 deflate */
};
HRESULT zip_open( struct source *source, struct zip **zip );
unsigned int zip_count( const struct zip *zip );
const struct zip_entry *zip_entry( const struct zip *zip, unsigned int index );
const struct zip_entry *zip_find( const struct zip *zip, const WCHAR *name );
HRESULT zip_data_offset( const struct zip *zip, const struct zip_entry *entry, UINT64 *offset );
HRESULT zip_extract( const struct zip *zip, const struct zip_entry *entry,
                     BOOL (*write)( void *context, const BYTE *data, ULONG size ), void *context );
HRESULT zip_read( const struct zip *zip, const struct zip_entry *entry, BYTE **data, ULONG *size );
void zip_close( struct zip *zip );

/* xml.c */
struct xml_attr { WCHAR *name, *value; };
struct xml_node
{
    WCHAR *name;            /* local name, no namespace */
    struct xml_attr *attrs;
    unsigned int attr_count;
    WCHAR *text;
    struct xml_node *parent, *children, *next;
};
HRESULT xml_parse( const BYTE *data, ULONG size, struct xml_node **root );
void xml_free( struct xml_node *root );
const WCHAR *xml_attr( const struct xml_node *node, const WCHAR *name );
const struct xml_node *xml_find( const struct xml_node *scope, const WCHAR *name );
const struct xml_node *xml_next( const struct xml_node *scope, const struct xml_node *prev, const WCHAR *name );

/* util.c */
int hex_value( char c );
BOOL path_equal( const WCHAR *a, const WCHAR *b );
WCHAR *path_join( const WCHAR *dir, const WCHAR *name );
WCHAR *strdupW( const WCHAR *s );
BOOL create_directories( const WCHAR *dir );
char *utf8_dup( const WCHAR *s );
BOOL find_content_key( const char *keys, const BYTE kid[16], BYTE key[32] );

/* xvd.c: GDK packages */
struct xvd;
struct xvd_region
{
    UINT32 id, first_segment;
    UINT16 key;             /* content key slot; 0xffff (>= 192) plain */
    UINT64 first_page, pages;
    WCHAR **tags, **languages;
    unsigned int tag_count, language_count;
};
HRESULT xvd_open( struct source *source, const struct appx_install_params *params, struct xvd **xvd );
unsigned int xvd_count( const struct xvd *xvd );
const WCHAR *xvd_name( const struct xvd *xvd, unsigned int index );
UINT64 xvd_size( const struct xvd *xvd, unsigned int index );
UINT32 xvd_member_region( const struct xvd *xvd, unsigned int index );
const BYTE *xvd_content_id( const struct xvd *xvd );
unsigned int xvd_region_count( const struct xvd *xvd );
const struct xvd_region *xvd_region( const struct xvd *xvd, unsigned int index );
HRESULT xvd_extract( struct xvd *xvd, unsigned int index,
                     BOOL (*write)( void *context, const BYTE *data, ULONG size ), void *context );
void xvd_close( struct xvd *xvd );

/* eappx.c: encrypted packages and bundles */
struct eappx;
HRESULT eappx_open( struct source *source, const struct appx_install_params *params, struct eappx **eappx );
unsigned int eappx_count( const struct eappx *eappx );
const WCHAR *eappx_name( const struct eappx *eappx, unsigned int index );
UINT64 eappx_size( const struct eappx *eappx, unsigned int index );
HRESULT eappx_extract( struct eappx *eappx, unsigned int index,
                       BOOL (*write)( void *context, const BYTE *data, ULONG size ), void *context );
void eappx_close( struct eappx *eappx );

/* package.c */
struct package;
WCHAR *unescape_name( const WCHAR *name );
HRESULT package_open( const WCHAR *path, const struct appx_install_params *params, const WCHAR *temp_dir,
                      struct package **pkg );
unsigned int package_count( const struct package *pkg );
const WCHAR *package_name( const struct package *pkg, unsigned int index );
UINT64 package_size( const struct package *pkg, unsigned int index );
BOOL package_is_dir( const struct package *pkg, unsigned int index );
int package_find( const struct package *pkg, const WCHAR *name );
HRESULT package_extract( const struct package *pkg, unsigned int index,
                         BOOL (*write)( void *context, const BYTE *data, ULONG size ), void *context );
HRESULT package_read( const struct package *pkg, unsigned int index, BYTE **data, ULONG *size );
void package_close( struct package *pkg );
struct xvd *package_xvd( const struct package *pkg );   /* NULL unless a GDK package */

/* manifest.c */
struct dependency { WCHAR *name, *min_version, *publisher; };
struct manifest
{
    WCHAR *name, *publisher, *version, *arch, *resource_id, *app_id;
    WCHAR *exe;             /* '/'-separated */
    WCHAR *title, *publisher_name;  /* NULL when only an ms-resource: reference */
    WCHAR *logo;            /* '/'-separated, NULL if none */
    struct dependency *deps;
    unsigned int dep_count;
    WCHAR **protocols;      /* lowercased, sorted, unique */
    unsigned int protocol_count;
};
HRESULT publisher_id( const WCHAR *publisher, WCHAR id[14] );
HRESULT manifest_read( const struct package *pkg, struct manifest **manifest );
void manifest_free( struct manifest *manifest );
int pick_logo( const struct package *pkg, const WCHAR *logo );

/* install.c */
void report( const struct appx_install_params *params, enum appx_event event, UINT64 done, UINT64 total,
             const WCHAR *format, ... );
BOOL wait_while_paused( const struct appx_install_params *params );   /* FALSE: cancelled */

#endif /* __APPXINSTALL_PRIVATE_H */
