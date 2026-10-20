/*
 * appxinstall: installs packaged Windows apps and games into a wine-uwp data directory
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

#ifndef __WINE_APPXINSTALL_H
#define __WINE_APPXINSTALL_H

#include <windef.h>

/* What the installer reports while it works. */
enum appx_event
{
    APPX_EVENT_STAGE,       /* a step begins: text names it ("Reading the package", "Paused", ...) */
    APPX_EVENT_PROGRESS,    /* done of total bytes of package content are in place */
    APPX_EVENT_MESSAGE,     /* a line worth logging: text */
    APPX_EVENT_INSTALLED,   /* finished: text is the stage name */
};

typedef void (CALLBACK *appx_callback)( void *context, enum appx_event event, UINT64 done, UINT64 total,
                                         const WCHAR *text );

struct appx_install_params
{
    const WCHAR *package;       /* a file path or http(s) URL: .appx/.msix, a bundle, .appinstaller, EAppx, .msixvc */
    const WCHAR *data_dir;      /* the install goes to DATA_DIR\stage\NAME */
    const WCHAR *windows_tree;  /* a harvested Windows tree: frameworks, mrt100.dll */
    const WCHAR *stage;         /* NAME; NULL: the last dotted part of the package name, lowercased */
    const WCHAR *tags;          /* GDK: install-feature tags to keep, ';'-separated; NULL keeps all */
    const WCHAR *languages;     /* GDK: language regions to keep, ';'-separated; NULL keeps all */
    const char *content_keys;   /* encrypted packages: "kid:key;..." in hex; NULL if none */
    const WCHAR *launch_defaults; /* launch-defaults.env, copied into each app's launch.env; NULL if none */
    const WCHAR *launcher;      /* wineuwp-launch.exe, copied into the stage; NULL: Wine's, from system32 */
    const WCHAR *d3d_runtime;   /* GDK games: DXVK's dxgi/d3d11 and vkd3d-proton's d3d12/d3d12core; NULL if none */
    volatile LONG *pause;       /* non-zero: wait between chunks */
    volatile LONG *cancel;      /* non-zero: stop, leave the installed copy as it was */
    appx_callback callback;
    void *context;
};

HRESULT CDECL appx_install( const struct appx_install_params *params );

/* A JSON description of a package (name, title, publisher, version, protocols, frameworks and whether the Windows
 * tree or DATA_DIR\stage\frameworks has them), as `install-app.py --info` printed it. icon, when given, receives the
 * package's logo. windows_tree, data_dir and content_keys (an encrypted package's, as for appx_install) may be
 * NULL. Free *json with appx_free(). */
HRESULT CDECL appx_info( const WCHAR *package, const WCHAR *windows_tree, const WCHAR *data_dir, const WCHAR *icon,
                         const char *content_keys, char **json );

/* The version of the main bundle an .appinstaller (a file or URL) serves, for update checks. Free *version with
 * appx_free(). */
HRESULT CDECL appx_latest( const WCHAR *appinstaller, WCHAR **version );

/* frees what appx_info and appx_latest return */
void CDECL appx_free( void *ptr );

#endif /* __WINE_APPXINSTALL_H */
