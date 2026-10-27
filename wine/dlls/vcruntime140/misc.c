/*
 * Copyright 2015 Martin Storsjo
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

#include <string.h>
#include "windows.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(vcruntime);

int* CDECL __processing_throw(void);
void** CDECL __current_exception(void);

#define CXX_EXCEPTION 0xe06d7363
#define TYPE_FLAG_WINRT 16

/*********************************************************************
 *              __GetPlatformExceptionInfo
 *
 * C++/CX's catch-all asks what the exception being handled is: for a WinRT (Platform::Exception^) throw, the
 * WINRTEXCEPTIONINFO the object carries in the word before its vtable pointer's target; NULL otherwise, with
 * *bad_alloc set for std::bad_alloc.
 */
void * CDECL __GetPlatformExceptionInfo(int *bad_alloc)
{
    EXCEPTION_RECORD *rec = *(EXCEPTION_RECORD **)__current_exception();
    const BYTE *base = NULL;
    const UINT *type;
    ULONG_PTR obj;

    *bad_alloc = 0;
    if (!rec || rec->ExceptionCode != CXX_EXCEPTION || rec->NumberParameters < 3) return NULL;
    obj = rec->ExceptionInformation[1];
    if (!(type = (const UINT *)rec->ExceptionInformation[2])) return NULL;
    if (rec->NumberParameters >= 4) base = (const BYTE *)rec->ExceptionInformation[3];
    if (type[0] & TYPE_FLAG_WINRT)
    {
        ULONG_PTR *info = obj ? *(ULONG_PTR **)obj : NULL;
        TRACE("WinRT exception object %#Ix\n", obj);
        return info ? (void *)info[-1] : NULL;
    }
    else
    {
        /* the most derived type is the first catchable one; type_info's mangled name follows its vtable and name */
        const char *mangled = NULL;

        if (base)   /* 64-bit: image-relative offsets */
        {
            const UINT *table = (const UINT *)(base + type[3]);
            const UINT *info = table[0] ? (const UINT *)(base + table[1]) : NULL;
            if (info) mangled = (const char *)(base + info[1]) + 2 * sizeof(void *);
        }
        else
        {
            const struct { UINT flags; void *dtor, *handler; const UINT *table; } *t = (const void *)type;
            const struct { UINT flags; const void *type_info; } *info = t->table[0] ? *(void **)(t->table + 1) : NULL;
            if (info) mangled = (const char *)info->type_info + 2 * sizeof(void *);
        }
        if (mangled && !strcmp( mangled, ".?AVbad_alloc@std@@" )) *bad_alloc = 1;
    }
    return NULL;
}

/*********************************************************************
 *              __telemetry_main_invoke_trigger
 */
void CDECL __telemetry_main_invoke_trigger(HINSTANCE hinst)
{
    FIXME("(%p)\n", hinst);
}

/*********************************************************************
 *              __telemetry_main_return_trigger
 */
void CDECL __telemetry_main_return_trigger(HINSTANCE hinst)
{
    FIXME("(%p)\n", hinst);
}

/*********************************************************************
 *              __vcrtInitializeCriticalSectionEx
 */
BOOL CDECL __vcrt_InitializeCriticalSectionEx(
        CRITICAL_SECTION *cs, DWORD spin_count, DWORD flags)
{
    TRACE("(%p %lx %lx)\n", cs, spin_count, flags);
    return InitializeCriticalSectionEx(cs, spin_count, flags);
}

int __cdecl __uncaught_exceptions(void)
{
    return *__processing_throw();
}
