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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#ifdef __linux__
#include <linux/fs.h>
#endif
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
    case EOPNOTSUPP:
    case EINVAL: return STATUS_NOT_SUPPORTED;
    default: return STATUS_UNSUCCESSFUL;
    }
}

static NTSTATUS rename_dir( void *args )
{
    const struct rename_params *params = args;

    return rename( params->src, params->dst ) ? errno_status( errno ) : STATUS_SUCCESS;
}

static int clone_file( const char *src, const char *dst, mode_t mode )
{
#ifdef FICLONE
    int in, out, ret = -1;

    if ((in = open( src, O_RDONLY )) < 0) return -1;
    if ((out = open( dst, O_WRONLY | O_CREAT | O_EXCL, mode & 07777 )) >= 0)
    {
        ret = ioctl( out, FICLONE, in );
        close( out );
        if (ret) unlink( dst );
    }
    close( in );
    return ret;
#else
    errno = EOPNOTSUPP;
    return -1;
#endif
}

static int clone_dir( const char *src, const char *dst )
{
    char from[PATH_MAX], to[PATH_MAX], target[PATH_MAX];
    struct dirent *de;
    struct stat st;
    int ret = 0;
    ssize_t n;
    DIR *dir;

    if (!(dir = opendir( src ))) return -1;
    while (!ret && (de = readdir( dir )))
    {
        if (!strcmp( de->d_name, "." ) || !strcmp( de->d_name, ".." )) continue;
        if (snprintf( from, sizeof(from), "%s/%s", src, de->d_name ) >= sizeof(from) ||
            snprintf( to, sizeof(to), "%s/%s", dst, de->d_name ) >= sizeof(to) || lstat( from, &st ))
        {
            ret = -1;
            break;
        }
        if (S_ISDIR( st.st_mode ))
            ret = (mkdir( to, st.st_mode & 07777 ) && errno != EEXIST) ? -1 : clone_dir( from, to );
        else if (S_ISLNK( st.st_mode ))
            ret = ((n = readlink( from, target, sizeof(target) - 1 )) < 0) ? -1 : (target[n] = 0, symlink( target, to ));
        else if (S_ISREG( st.st_mode ))
            ret = clone_file( from, to, st.st_mode );
    }
    closedir( dir );
    return ret;
}

static NTSTATUS clone_tree( void *args )
{
    const struct clone_params *params = args;

    return clone_dir( params->src, params->dst ) ? errno_status( errno ) : STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    rename_dir,
    clone_tree,
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

static NTSTATUS wow64_clone_tree( void *args )
{
    struct { PTR32 src, dst; } const *params32 = args;
    struct clone_params params = { ULongToPtr( params32->src ), ULongToPtr( params32->dst ) };

    return clone_tree( &params );
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_rename_dir,
    wow64_clone_tree,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif /* _WIN64 */
