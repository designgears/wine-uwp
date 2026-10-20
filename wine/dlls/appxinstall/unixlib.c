/*
 * appxinstall: the Unix side
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

#if 0
#pragma makedep unix
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "unixlib.h"

static NTSTATUS errno_status( int err )
{
    switch (err)
    {
    case ENOENT: return STATUS_OBJECT_NAME_NOT_FOUND;
    case EEXIST:
    case ENOTEMPTY: return STATUS_OBJECT_NAME_COLLISION;
    case EACCES:
    case EPERM: return STATUS_ACCESS_DENIED;
    case EXDEV: return STATUS_NOT_SAME_DEVICE;
    default: return STATUS_UNSUCCESSFUL;
    }
}

static NTSTATUS rename_dir( void *args )
{
    const struct rename_params *params = args;

    return rename( params->src, params->dst ) ? errno_status( errno ) : STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    rename_dir,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

typedef ULONG PTR32;

static NTSTATUS wow64_rename_dir( void *args )
{
    struct { PTR32 src, dst; } const *params32 = args;
    struct rename_params params = { ULongToPtr( params32->src ), ULongToPtr( params32->dst ) };

    return rename_dir( &params );
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_rename_dir,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif /* _WIN64 */
