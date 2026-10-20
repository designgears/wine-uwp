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

#include "wine/unixlib.h"

/* renames a directory whatever handles are open below it: an app updating itself has its own stage
 * directory open (the process's working directory), which no Windows rename allows */
struct rename_params
{
    const char *src;        /* Unix paths */
    const char *dst;
};

/* copies a tree by reflink only (FICLONE): a plain copy of a 100 GB game would cost more than the download it
 * saves. Fails as a whole when the filesystem cannot share extents. */
struct clone_params
{
    const char *src;        /* Unix paths; dst exists and is empty */
    const char *dst;
};

enum appxinstall_funcs
{
    unix_rename_dir,
    unix_clone_tree,
    unix_funcs_count,
};
