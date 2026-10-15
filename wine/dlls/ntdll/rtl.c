/*
 * NT basis DLL
 *
 * This file contains the Rtl* API functions. These should be implementable.
 *
 * Copyright 1996-1998 Marcus Meissner
 * Copyright 1999      Alex Korobka
 * Copyright 2003      Thomas Mertes
 * Crc32 code Copyright 1986 Gary S. Brown (Public domain)
 * Copyright 2025      Zhiyi Zhang for CodeWeavers
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

#include <stdarg.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ntuser.h"
#include "zlib.h"
#include "wine/debug.h"
#include "wine/exception.h"
#include "ntdll_misc.h"
#include "ddk/ntifs.h"

WINE_DEFAULT_DEBUG_CHANNEL(ntdll);
WINE_DECLARE_DEBUG_CHANNEL(debugstr);


static LONG WINAPI debug_exception_handler( EXCEPTION_POINTERS *eptr )
{
    EXCEPTION_RECORD *rec = eptr->ExceptionRecord;
    return (rec->ExceptionCode == DBG_PRINTEXCEPTION_C) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

/******************************************************************************
 *	DbgPrint	[NTDLL.@]
 */
NTSTATUS WINAPIV DbgPrint(LPCSTR fmt, ...)
{
    NTSTATUS ret;
    va_list args;

    va_start(args, fmt);
    ret = vDbgPrintEx(0, DPFLTR_ERROR_LEVEL, fmt, args);
    va_end(args);
    return ret;
}


/******************************************************************************
 *	DbgPrintEx	[NTDLL.@]
 */
NTSTATUS WINAPIV DbgPrintEx(ULONG iComponentId, ULONG Level, LPCSTR fmt, ...)
{
    NTSTATUS ret;
    va_list args;

    va_start(args, fmt);
    ret = vDbgPrintEx(iComponentId, Level, fmt, args);
    va_end(args);
    return ret;
}

/******************************************************************************
 *	vDbgPrintEx	[NTDLL.@]
 */
NTSTATUS WINAPI vDbgPrintEx( ULONG id, ULONG level, LPCSTR fmt, va_list args )
{
    return vDbgPrintExWithPrefix( "", id, level, fmt, args );
}

/******************************************************************************
 *	vDbgPrintExWithPrefix  [NTDLL.@]
 */
NTSTATUS WINAPI vDbgPrintExWithPrefix( LPCSTR prefix, ULONG id, ULONG level, LPCSTR fmt, va_list args )
{
    ULONG level_mask = level <= 31 ? (1 << level) : level;
    SIZE_T len = strlen( prefix );
    char buf[1024], *end;

    strcpy( buf, prefix );
    len += _vsnprintf( buf + len, sizeof(buf) - len, fmt, args );
    end = buf + len - 1;

    WARN_(debugstr)(*end == '\n' ? "%08lx:%08lx: %s" : "%08lx:%08lx: %s\n", id, level_mask, buf);

    if (level_mask & (1 << DPFLTR_ERROR_LEVEL) && NtCurrentTeb()->Peb->BeingDebugged)
    {
        __TRY
        {
            EXCEPTION_RECORD record;
            record.ExceptionCode    = DBG_PRINTEXCEPTION_C;
            record.ExceptionFlags   = 0;
            record.ExceptionRecord  = NULL;
            record.ExceptionAddress = RtlRaiseException;
            record.NumberParameters = 2;
            record.ExceptionInformation[1] = (ULONG_PTR)buf;
            record.ExceptionInformation[0] = strlen( buf ) + 1;
            RtlRaiseException( &record );
        }
        __EXCEPT(debug_exception_handler)
        {
        }
        __ENDTRY
    }

    return STATUS_SUCCESS;
}


static struct ntuser_client_procs_table user_procs;

#define DEFINE_USER_FUNC(name,ptr) \
    LRESULT WINAPI name( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) { return (ptr)( hwnd, msg, wp, lp ); }

#define USER_FUNC(name,proc) \
    DEFINE_USER_FUNC( Ntdll##name##_A, user_procs.A[proc][0] ) \
    DEFINE_USER_FUNC( Ntdll##name##_W, user_procs.W[proc][0] )
ALL_NTUSER_CLIENT_PROCS
#undef USER_FUNC

static const struct ntuser_client_procs_table user_funcs =
{
#define USER_FUNC(name,proc) .A[proc] = { Ntdll##name##_A }, .W[proc] = { Ntdll##name##_W },
    ALL_NTUSER_CLIENT_PROCS
#undef USER_FUNC
};

static BOOL user_procs_init_done;

/***********************************************************************
 *	     RtlInitializeNtUserPfn   (NTDLL.@)
 */
NTSTATUS WINAPI RtlInitializeNtUserPfn( const void *client_procsA, ULONG procsA_size,
                                        const void *client_procsW, ULONG procsW_size,
                                        const void *client_workers, ULONG workers_size )
{
    if (procsA_size % sizeof(ntuser_client_func_ptr) || procsA_size > sizeof(user_procs.A) ||
        procsW_size % sizeof(ntuser_client_func_ptr) || procsW_size > sizeof(user_procs.W) ||
        workers_size % sizeof(ntuser_client_func_ptr) || workers_size > sizeof(user_procs.workers))
        return STATUS_INVALID_PARAMETER;

    if (user_procs_init_done) return STATUS_INVALID_PARAMETER;
    memcpy( user_procs.A, client_procsA, procsA_size );
    memcpy( user_procs.W, client_procsW, procsW_size );
    memcpy( user_procs.workers, client_workers, workers_size );
    user_procs_init_done = TRUE;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *	     RtlRetrieveNtUserPfn   (NTDLL.@)
 */
NTSTATUS WINAPI RtlRetrieveNtUserPfn( const void **client_procsA,
                                      const void **client_procsW,
                                      const void **client_workers )
{
    if (!user_procs_init_done) return STATUS_INVALID_PARAMETER;
    *client_procsA  = user_funcs.A;
    *client_procsW  = user_funcs.W;
    *client_workers = user_funcs.workers;
    return STATUS_SUCCESS;
}

/***********************************************************************
 *	     RtlResetNtUserPfn   (NTDLL.@)
 */
NTSTATUS WINAPI RtlResetNtUserPfn(void)
{
    if (!user_procs_init_done) return STATUS_INVALID_PARAMETER;
    user_procs_init_done = FALSE;
    memset( &user_procs, 0, sizeof(user_procs) );
    return STATUS_SUCCESS;
}

/* data is a place holder to align stored data on a 8 byte boundary */
struct rtl_generic_table_entry
{
    RTL_SPLAY_LINKS splay_links;
    LIST_ENTRY list_entry;
    LONGLONG data;
};

static void *get_data_from_splay_links(RTL_SPLAY_LINKS *links)
{
    return (unsigned char *)links + FIELD_OFFSET(struct rtl_generic_table_entry, data);
}

static void *get_data_from_list_entry(LIST_ENTRY *list_entry)
{
    return (unsigned char *)list_entry + FIELD_OFFSET(struct rtl_generic_table_entry, data)
        - FIELD_OFFSET(struct rtl_generic_table_entry, list_entry);
}

static LIST_ENTRY *get_list_entry_from_splay_links(RTL_SPLAY_LINKS *links)
{
    return (LIST_ENTRY *)((unsigned char *)links + FIELD_OFFSET(struct rtl_generic_table_entry, list_entry));
}

static RTL_SPLAY_LINKS *rtl_splay_find(RTL_GENERIC_TABLE *table, void *value)
{
    RTL_GENERIC_COMPARE_RESULTS result;
    RTL_SPLAY_LINKS *child;

    child = table->TableRoot;
    while (child)
    {
        result = table->CompareRoutine(table, get_data_from_splay_links(child), value);
        if (result == GenericLessThan)
            child = child->RightChild;
        else if (result == GenericGreaterThan)
            child = child->LeftChild;
        else
            return child;
    }
    return NULL;
}

static void rtl_splay_replace(RTL_SPLAY_LINKS *x, RTL_SPLAY_LINKS *y, RTL_SPLAY_LINKS **root)
{
    if (RtlIsRoot(x))
        *root = y;
    else if (RtlIsLeftChild(x))
        x->Parent->LeftChild = y;
    else
        x->Parent->RightChild = y;

    if (y)
        y->Parent = RtlIsRoot(x) ? y : x->Parent;
}

static void rtl_splay_left_rotate(RTL_SPLAY_LINKS *x)
{
    RTL_SPLAY_LINKS *y = x->RightChild;

    if (y)
    {
        x->RightChild = y->LeftChild;
        if (y->LeftChild)
            y->LeftChild->Parent = x;

        y->Parent = RtlIsRoot(x) ? y : x->Parent;
    }

    if (!RtlIsRoot(x))
    {
        if (RtlIsLeftChild(x))
            x->Parent->LeftChild = y;
        else
            x->Parent->RightChild = y;
    }

    if (y)
        y->LeftChild = x;
    x->Parent = y;
}

static void rtl_splay_right_rotate(RTL_SPLAY_LINKS *x)
{
    RTL_SPLAY_LINKS *y = x->LeftChild;

    if (y)
    {
        x->LeftChild = y->RightChild;
        if (y->RightChild)
            y->RightChild->Parent = x;

        y->Parent = RtlIsRoot(x) ? y : x->Parent;
    }

    if (!RtlIsRoot(x))
    {
        if (RtlIsLeftChild(x))
            x->Parent->LeftChild = y;
        else
            x->Parent->RightChild = y;
    }

    if (y)
        y->RightChild = x;
    x->Parent = y;
}

/******************************************************************************
 *  RtlSubtreePredecessor           [NTDLL.@]
 */
RTL_SPLAY_LINKS * WINAPI RtlSubtreePredecessor(RTL_SPLAY_LINKS *links)
{
    RTL_SPLAY_LINKS *child;

    TRACE("(%p)\n", links);

    child = RtlLeftChild(links);
    if (!child)
        return NULL;

    while (RtlRightChild(child))
        child = RtlRightChild(child);

    return child;
}

/******************************************************************************
 *  RtlSubtreeSuccessor           [NTDLL.@]
 */
RTL_SPLAY_LINKS * WINAPI RtlSubtreeSuccessor(RTL_SPLAY_LINKS *links)
{
    RTL_SPLAY_LINKS *child;

    TRACE("(%p)\n", links);

    child = RtlRightChild(links);
    if (!child)
        return NULL;

    while (RtlLeftChild(child))
        child = RtlLeftChild(child);

    return child;
}

/******************************************************************************
 *  RtlRealPredecessor           [NTDLL.@]
 */
RTL_SPLAY_LINKS * WINAPI RtlRealPredecessor(RTL_SPLAY_LINKS *links)
{
    PRTL_SPLAY_LINKS child;

    TRACE("(%p)\n", links);

    child = RtlLeftChild(links);
    if (child)
    {
        while (RtlRightChild(child))
            child = RtlRightChild(child);
        return child;
    }

    child = links;
    while (RtlIsLeftChild(child))
        child = RtlParent(child);

    if (RtlIsRightChild(child))
        return RtlParent(child);

    return NULL;
}

/******************************************************************************
 *  RtlRealSuccessor           [NTDLL.@]
 */
RTL_SPLAY_LINKS * WINAPI RtlRealSuccessor(RTL_SPLAY_LINKS *links)
{
    PRTL_SPLAY_LINKS child;

    TRACE("(%p)\n", links);

    child = RtlRightChild(links);
    if (child)
    {
        while (RtlLeftChild(child))
            child = RtlLeftChild(child);
        return child;
    }

    child = links;
    while (RtlIsRightChild(child))
        child = RtlParent(child);

    if (RtlIsLeftChild(child))
        return RtlParent(child);

    return NULL;
}

/******************************************************************************
 *  RtlSplay           [NTDLL.@]
 */
RTL_SPLAY_LINKS * WINAPI RtlSplay(RTL_SPLAY_LINKS *links)
{
    TRACE("(%p)\n", links);

    while (!RtlIsRoot(links))
    {
        if (RtlIsRoot(links->Parent))
        {
            /* Zig */
            if (RtlIsLeftChild(links))
                rtl_splay_right_rotate(links->Parent);
            /* Zag */
            else
                rtl_splay_left_rotate(links->Parent);
        }
        /* Zig-Zig */
        else if (RtlIsLeftChild(links->Parent) && RtlIsLeftChild(links))
        {
            rtl_splay_right_rotate(links->Parent->Parent);
            rtl_splay_right_rotate(links->Parent);
        }
        /* Zag-Zag */
        else if (RtlIsRightChild(links->Parent) && RtlIsRightChild(links))
        {
            rtl_splay_left_rotate(links->Parent->Parent);
            rtl_splay_left_rotate(links->Parent);
        }
        /* Zig-Zag */
        else if (RtlIsRightChild(links->Parent) && RtlIsLeftChild(links))
        {
            rtl_splay_right_rotate(links->Parent);
            rtl_splay_left_rotate(links->Parent);
        }
        /* Zag-Zig */
        else
        {
            rtl_splay_left_rotate(links->Parent);
            rtl_splay_right_rotate(links->Parent);
        }
    }

    return links;
}

/******************************************************************************
 *  RtlDeleteNoSplay           [NTDLL.@]
 */
void WINAPI RtlDeleteNoSplay(RTL_SPLAY_LINKS *links, RTL_SPLAY_LINKS **root)
{
    TRACE("(%p, %p)\n", links, root);

    if (RtlIsRoot(links) && !RtlLeftChild(links) && !RtlRightChild(links))
    {
        *root = NULL;
    }
    else  if (!links->LeftChild)
    {
        rtl_splay_replace(links, links->RightChild, root);
    }
    else if (!links->RightChild)
    {
        rtl_splay_replace(links, links->LeftChild, root);
    }
    else
    {
        RTL_SPLAY_LINKS *predecessor = RtlSubtreePredecessor(links);
        if (predecessor->Parent != links)
        {
            rtl_splay_replace(predecessor, predecessor->LeftChild, root);
            RtlInsertAsLeftChild(predecessor, links->LeftChild);
        }
        rtl_splay_replace(links, predecessor, root);
        RtlInsertAsRightChild(predecessor, links->RightChild);
    }
}

/******************************************************************************
 *  RtlDelete           [NTDLL.@]
 */
RTL_SPLAY_LINKS * WINAPI RtlDelete(RTL_SPLAY_LINKS *links)
{
    RTL_SPLAY_LINKS *root, *to_splay;

    TRACE("(%p)\n", links);

    if (RtlIsRoot(links) && !RtlLeftChild(links) && !RtlRightChild(links))
    {
        return NULL;
    }
    else if (!links->LeftChild)
    {
        rtl_splay_replace(links, links->RightChild, &root);
        if (RtlIsRoot(links))
            return links->RightChild;

        to_splay = links->Parent;
    }
    else if (!links->RightChild)
    {
        rtl_splay_replace(links, links->LeftChild, &root);
        if (RtlIsRoot(links))
            return links->LeftChild;

        to_splay = links->Parent;
    }
    else
    {
        RTL_SPLAY_LINKS *predecessor = RtlSubtreePredecessor(links);
        if (predecessor->Parent != links)
        {
            rtl_splay_replace(predecessor, predecessor->LeftChild, &root);
            RtlInsertAsLeftChild(predecessor, links->LeftChild);
            /* Delete operation first swap the value of node to delete and that of the predecessor
             * and then delete the predecessor instead. Finally, the parent of the actual deleted
             * node, which is the predecessor, is splayed afterwards */
            to_splay = predecessor->Parent;
        }
        else
        {
            /* links is the parent of predecessor. So after swapping, the parent of links is in
             * fact the predecessor. So predecessor gets splayed */
            to_splay = predecessor;
        }
        rtl_splay_replace(links, predecessor, &root);
        RtlInsertAsRightChild(predecessor, links->RightChild);
    }

    return RtlSplay(to_splay);
}

/******************************************************************************
 *  RtlInitializeGenericTable           [NTDLL.@]
 */
void WINAPI RtlInitializeGenericTable(RTL_GENERIC_TABLE *table, PRTL_GENERIC_COMPARE_ROUTINE compare,
                                      PRTL_GENERIC_ALLOCATE_ROUTINE allocate, PRTL_GENERIC_FREE_ROUTINE free,
                                      void *context)
{
    TRACE("(%p, %p, %p, %p, %p)\n", table, compare, allocate, free, context);

    table->TableRoot = NULL;
    table->InsertOrderList.Flink = &table->InsertOrderList;
    table->InsertOrderList.Blink = &table->InsertOrderList;
    table->OrderedPointer = &table->InsertOrderList;
    table->NumberGenericTableElements = 0;
    table->WhichOrderedElement = 0;
    table->CompareRoutine = compare;
    table->AllocateRoutine = allocate;
    table->FreeRoutine = free;
    table->TableContext = context;
}

/******************************************************************************
 *  RtlIsGenericTableEmpty           [NTDLL.@]
 */
BOOLEAN WINAPI RtlIsGenericTableEmpty(RTL_GENERIC_TABLE *table)
{
    TRACE("(%p)\n", table);
    return !table->TableRoot;
}

/***********************************************************************
 *           RtlInsertElementGenericTable  (NTDLL.@)
 */
void * WINAPI RtlInsertElementGenericTable(RTL_GENERIC_TABLE *table, void *value, CLONG size, BOOLEAN *new_element)
{
    RTL_SPLAY_LINKS *child, *parent = NULL;
    RTL_GENERIC_COMPARE_RESULTS result;
    void *buffer;

    TRACE("(%p, %p, %lu, %p)\n", table, value, size, new_element);

    child = table->TableRoot;
    while (child)
    {
        buffer = get_data_from_splay_links(child);
        result = table->CompareRoutine(table, buffer, value);
        parent = child;
        if (result == GenericLessThan)
        {
            child = child->RightChild;
        }
        else if (result == GenericGreaterThan)
        {
            child = child->LeftChild;
        }
        else
        {
            if (new_element)
                *new_element = FALSE;
            return buffer;
        }
    }

    /* data should be stored on a 8 byte boundary */
    child = (RTL_SPLAY_LINKS *)table->AllocateRoutine(table, size + FIELD_OFFSET(struct rtl_generic_table_entry, data));
    RtlInitializeSplayLinks(child);
    InsertTailList(&table->InsertOrderList, get_list_entry_from_splay_links(child));
    buffer = get_data_from_splay_links(child);
    memcpy(buffer, value, size);

    if (parent)
    {
        buffer = get_data_from_splay_links(parent);
        result = table->CompareRoutine(table, buffer, value);
        if (result == GenericLessThan)
            RtlInsertAsRightChild(parent, child);
        else
            RtlInsertAsLeftChild(parent, child);
    }

    if (new_element)
        *new_element = TRUE;
    table->TableRoot = RtlSplay(child);
    table->NumberGenericTableElements++;
    return get_data_from_splay_links(child);
}

BOOLEAN WINAPI RtlDeleteElementGenericTable(RTL_GENERIC_TABLE *table, void *value)
{
    RTL_SPLAY_LINKS *child;

    TRACE("(%p, %p)\n", table, value);

    child = rtl_splay_find(table, value);
    if (!child)
        return FALSE;

    table->TableRoot = RtlDelete(child);
    RemoveEntryList(get_list_entry_from_splay_links(child));
    table->NumberGenericTableElements--;
    table->OrderedPointer = &table->InsertOrderList;
    table->WhichOrderedElement = 0;
    table->FreeRoutine(table, child);
    return TRUE;
}

/******************************************************************************
 *  RtlEnumerateGenericTableWithoutSplaying           [NTDLL.@]
 */
void * WINAPI RtlEnumerateGenericTableWithoutSplaying(RTL_GENERIC_TABLE *table, PVOID *previous)
{
    RTL_SPLAY_LINKS *child;

    TRACE("(%p, %p)\n", table, previous);

    if (RtlIsGenericTableEmpty(table))
        return NULL;

    if (!*previous)
    {
        /* Find the smallest element */
        child = table->TableRoot;
        while (RtlLeftChild(child))
            child = RtlLeftChild(child);
    }
    else
    {
        /* Find the successor of the previous element */
        child = RtlRealSuccessor(*previous);
        if (!child)
            return NULL;
    }

    *previous = child;
    return get_data_from_splay_links(child);
}

/******************************************************************************
 *  RtlEnumerateGenericTable           [NTDLL.@]
 *
 * RtlEnumerateGenericTable() uses TableRoot to keep track of enumeration status according to tests.
 * This also means that other functions that change TableRoot should not be used during enumerations.
 * Otherwise, RtlEnumerateGenericTable() won't be able to find the correct next element when restart
 * is FALSE. This is also the case on Windows.
 */
void * WINAPI RtlEnumerateGenericTable(RTL_GENERIC_TABLE *table, BOOLEAN restart)
{
    RTL_SPLAY_LINKS *child;

    TRACE("(%p, %d)\n", table, restart);

    if (RtlIsGenericTableEmpty(table))
        return NULL;

    if (restart)
    {
        /* Find the smallest element */
        child = table->TableRoot;
        while (RtlLeftChild(child))
            child = RtlLeftChild(child);
    }
    else
    {
        /* Find the successor of the root */
        child = RtlRealSuccessor(table->TableRoot);
        if (!child)
            return NULL;
    }

    table->TableRoot = RtlSplay(child);
    return get_data_from_splay_links(child);
}

/******************************************************************************
 *  RtlNumberGenericTableElements           [NTDLL.@]
 */
ULONG WINAPI RtlNumberGenericTableElements(RTL_GENERIC_TABLE *table)
{
    TRACE("(%p)\n", table);
    return table->NumberGenericTableElements;
}

/******************************************************************************
 *  RtlGetElementGenericTable           [NTDLL.@]
 */
void * WINAPI RtlGetElementGenericTable(RTL_GENERIC_TABLE *table, ULONG index)
{
    ULONG count, ordered_element_index;
    LIST_ENTRY *list_entry;
    BOOL forward;

    TRACE("(%p, %lu)\n", table, index);

    if (index >= table->NumberGenericTableElements)
        return NULL;

    /* No OrderedPointer, use list header InsertOrderList */
    if (table->WhichOrderedElement == 0)
    {
        /*
         * |  lower half  | upper half |
         * ^       ^      ^            ^
         * head, 0 |      |            tail, table->NumberGenericTableElements - 1
         *         |      (table->NumberGenericTableElements - 1) / 2
         *         index closer to head
         */
        if (index <= (table->NumberGenericTableElements - 1) / 2)
        {
            list_entry = table->InsertOrderList.Flink;
            count = index;
            forward = TRUE;
        }
        /*
         * |  lower half  | upper half |
         * ^              ^       ^    ^
         * head, 0        |       |    tail, table->NumberGenericTableElements - 1
         *                |       index closer to tail
         *                (table->NumberGenericTableElements - 1) / 2
         */
        else
        {
            list_entry = table->InsertOrderList.Blink;
            count = table->NumberGenericTableElements - index - 1;
            forward = FALSE;
        }
    }
    /* Has OrderedPointer, decide to use list header or OrderedPointer */
    else
    {
        ordered_element_index = table->WhichOrderedElement - 1;

        /*
         * | -------------- | ---------- |
         * ^         ^      ^            ^
         * head, 0   |      |            table->NumberGenericTableElements - 1
         *           |      ordered_element_index
         *           index, 0 <= index <= ordered_element_index
         */
        if (index <= ordered_element_index)
        {
            /*
             * | ------------------- |
             * ^          ^          ^
             * | <--d1--> | <--d2--> |
             * head, 0    |          ordered_element_index
             *            index
             *
             */
            /* d1 <= d2, index is closer to head, forward from head */
            if (index <= ordered_element_index - index)
            {
                list_entry = table->InsertOrderList.Flink;
                count = index;
                forward = TRUE;
            }
            /* d1 > d2, index is closer to ordered_element_index, backward from ordered_element_index */
            else
            {
                list_entry = table->OrderedPointer;
                count = ordered_element_index - index;
                forward = FALSE;
            }
        }
        /*
         * | ------ | ---------- |
         * ^        ^  ^         ^
         * head, 0  |  |         tail, table->NumberGenericTableElements - 1
         *          |  index, ordered_element_index < index <= table->NumberGenericTableElements - 1
         *          ordered_element_index
         */
        else
        {
            /*
             * | --------------------------------|
             * ^                      ^          ^
             * | <--------d1--------> | <--d2--> |
             * ordered_element_index  |          tail, table->NumberGenericTableElements - 1
             *                        index
             *
             */
            /* d1 <= d2, index is closer to ordered_element_index, forward from ordered_element_index */
            if (index - ordered_element_index <= table->NumberGenericTableElements - index - 1)
            {
                list_entry = table->OrderedPointer;
                count = index - ordered_element_index;
                forward = TRUE;
            }
            /* d1 > d2, index is closer to tail, backward from tail */
            else
            {
                list_entry = table->InsertOrderList.Blink;
                count = table->NumberGenericTableElements - index - 1;
                forward = FALSE;
            }
        }
    }

    if (forward)
    {
        while (count--)
            list_entry = list_entry->Flink;
    }
    else
    {
        while (count--)
            list_entry = list_entry->Blink;
    }

    table->OrderedPointer = list_entry;
    table->WhichOrderedElement = index + 1;
    return get_data_from_list_entry(list_entry);
}

/******************************************************************************
 *  RtlLookupElementGenericTable           [NTDLL.@]
 */
void * WINAPI RtlLookupElementGenericTable(RTL_GENERIC_TABLE *table, void *value)
{
    RTL_SPLAY_LINKS *child;

    TRACE("(%p, %p)\n", table, value);

    child = rtl_splay_find(table, value);
    if (!child)
        return FALSE;

    table->TableRoot = RtlSplay(child);
    return get_data_from_splay_links(child);
}

/******************************************************************************
 *  RtlAssert                           [NTDLL.@]
 *
 * Fail a debug assertion.
 *
 * RETURNS
 *  Nothing. This call does not return control to its caller.
 *
 * NOTES
 * Not implemented in non-debug versions.
 */
void WINAPI RtlAssert(void *assertion, void *filename, ULONG linenumber, char *message)
{
    FIXME("(%s, %s, %lu, %s): stub\n", debugstr_a((char*)assertion), debugstr_a((char*)filename),
        linenumber, debugstr_a(message));
}

/*********************************************************************
 *                  RtlComputeCrc32   [NTDLL.@]
 *
 * Calculate the CRC32 checksum of a block of bytes
 *
 * PARAMS
 *  dwInitial [I] Initial CRC value
 *  pData     [I] Data block
 *  iLen      [I] Length of the byte block
 *
 * RETURNS
 *  The cumulative CRC32 of dwInitial and iLen bytes of the pData block.
 */
DWORD WINAPI RtlComputeCrc32(DWORD dwInitial, const BYTE *pData, INT iLen)
{
    return crc32( dwInitial, pData, iLen );
}


/*************************************************************************
 * RtlUlonglongByteSwap    [NTDLL.@]
 *
 * Swap the bytes of an unsigned long long value.
 *
 * PARAMS
 *  i [I] Value to swap bytes of
 *
 * RETURNS
 *  The value with its bytes swapped.
 */
#ifdef __i386__
__ASM_FASTCALL_FUNC(RtlUlonglongByteSwap, 8,
                    "movl 4(%esp),%edx\n\t"
                    "bswap %edx\n\t"
                    "movl 8(%esp),%eax\n\t"
                    "bswap %eax\n\t"
                    "ret $8")
#endif

/*************************************************************************
 * RtlUlongByteSwap    [NTDLL.@]
 *
 * Swap the bytes of an unsigned int value.
 *
 * NOTES
 *  ix86 version takes argument in %ecx. Other systems use the inline version.
 */
#ifdef __i386__
__ASM_FASTCALL_FUNC(RtlUlongByteSwap, 4,
                    "movl %ecx,%eax\n\t"
                    "bswap %eax\n\t"
                    "ret")
#endif

/*************************************************************************
 * RtlUshortByteSwap    [NTDLL.@]
 *
 * Swap the bytes of an unsigned short value.
 *
 * NOTES
 *  i386 version takes argument in %cx. Other systems use the inline version.
 */
#ifdef __i386__
__ASM_FASTCALL_FUNC(RtlUshortByteSwap, 4,
                    "movb %ch,%al\n\t"
                    "movb %cl,%ah\n\t"
                    "ret")
#endif


/*************************************************************************
 * RtlUniform   [NTDLL.@]
 *
 * Generates a uniform random number
 *
 * PARAMS
 *  seed [O] The seed of the Random function
 *
 * RETURNS
 *  It returns a random number uniformly distributed over [0..MAXLONG-1].
 *
 * NOTES
 *  Generates a uniform random number using a linear congruential generator.
 *
 * DIFFERENCES
 *  The native documentation states that the random number is
 *  uniformly distributed over [0..MAXLONG]. In reality the native
 *  function and our function return a random number uniformly
 *  distributed over [0..MAXLONG-1].
 */
ULONG WINAPI RtlUniform( ULONG *seed )
{
    /* See the tests for details. */
    return (*seed = ((ULONGLONG)*seed * 0x7fffffed + 0x7fffffc3) % 0x7fffffff);
}


/*************************************************************************
 * RtlRandom   [NTDLL.@]
 *
 * Generates a random number
 *
 * PARAMS
 *  seed [O] The seed of the Random function
 *
 * RETURNS
 *  It returns a random number distributed over [0..MAXLONG-1].
 */
ULONG WINAPI RtlRandom (PULONG seed)
{
    static ULONG saved_value[128] =
    { /*   0 */ 0x4c8bc0aa, 0x4c022957, 0x2232827a, 0x2f1e7626, 0x7f8bdafb, 0x5c37d02a, 0x0ab48f72, 0x2f0c4ffa,
      /*   8 */ 0x290e1954, 0x6b635f23, 0x5d3885c0, 0x74b49ff8, 0x5155fa54, 0x6214ad3f, 0x111e9c29, 0x242a3a09,
      /*  16 */ 0x75932ae1, 0x40ac432e, 0x54f7ba7a, 0x585ccbd5, 0x6df5c727, 0x0374dad1, 0x7112b3f1, 0x735fc311,
      /*  24 */ 0x404331a9, 0x74d97781, 0x64495118, 0x323e04be, 0x5974b425, 0x4862e393, 0x62389c1d, 0x28a68b82,
      /*  32 */ 0x0f95da37, 0x7a50bbc6, 0x09b0091c, 0x22cdb7b4, 0x4faaed26, 0x66417ccd, 0x189e4bfa, 0x1ce4e8dd,
      /*  40 */ 0x5274c742, 0x3bdcf4dc, 0x2d94e907, 0x32eac016, 0x26d33ca3, 0x60415a8a, 0x31f57880, 0x68c8aa52,
      /*  48 */ 0x23eb16da, 0x6204f4a1, 0x373927c1, 0x0d24eb7c, 0x06dd7379, 0x2b3be507, 0x0f9c55b1, 0x2c7925eb,
      /*  56 */ 0x36d67c9a, 0x42f831d9, 0x5e3961cb, 0x65d637a8, 0x24bb3820, 0x4d08e33d, 0x2188754f, 0x147e409e,
      /*  64 */ 0x6a9620a0, 0x62e26657, 0x7bd8ce81, 0x11da0abb, 0x5f9e7b50, 0x23e444b6, 0x25920c78, 0x5fc894f0,
      /*  72 */ 0x5e338cbb, 0x404237fd, 0x1d60f80f, 0x320a1743, 0x76013d2b, 0x070294ee, 0x695e243b, 0x56b177fd,
      /*  80 */ 0x752492e1, 0x6decd52f, 0x125f5219, 0x139d2e78, 0x1898d11e, 0x2f7ee785, 0x4db405d8, 0x1a028a35,
      /*  88 */ 0x63f6f323, 0x1f6d0078, 0x307cfd67, 0x3f32a78a, 0x6980796c, 0x462b3d83, 0x34b639f2, 0x53fce379,
      /*  96 */ 0x74ba50f4, 0x1abc2c4b, 0x5eeaeb8d, 0x335a7a0d, 0x3973dd20, 0x0462d66b, 0x159813ff, 0x1e4643fd,
      /* 104 */ 0x06bc5c62, 0x3115e3fc, 0x09101613, 0x47af2515, 0x4f11ec54, 0x78b99911, 0x3db8dd44, 0x1ec10b9b,
      /* 112 */ 0x5b5506ca, 0x773ce092, 0x567be81a, 0x5475b975, 0x7a2cde1a, 0x494536f5, 0x34737bb4, 0x76d9750b,
      /* 120 */ 0x2a1f6232, 0x2e49644d, 0x7dddcbe7, 0x500cebdb, 0x619dab9e, 0x48c626fe, 0x1cda3193, 0x52dabe9d };
    ULONG rand;
    int pos;
    ULONG result;

    rand = (*seed * 0x7fffffed + 0x7fffffc3) % 0x7fffffff;
    *seed = (rand * 0x7fffffed + 0x7fffffc3) % 0x7fffffff;
    pos = *seed & 0x7f;
    result = saved_value[pos];
    saved_value[pos] = rand;
    return(result);
}


/*************************************************************************
 * RtlRandomEx   [NTDLL.@]
 */
ULONG WINAPI RtlRandomEx( ULONG *seed )
{
    WARN( "semi-stub: should use a different algorithm\n" );
    return RtlRandom( seed );
}

/***********************************************************************
 * get_process_cookie (internal)
 */
static ULONG get_process_cookie(void)
{
    static ULONG process_cookie;

    if (!process_cookie)
    {
        ULONG cookie;

        NtQueryInformationProcess( GetCurrentProcess(), ProcessCookie, &cookie, sizeof(cookie), NULL );
        InterlockedCompareExchange( (LONG volatile *)&process_cookie, cookie, 0 );
    }
    return process_cookie;
}

#define BIT_COUNT_IN_POINTER (sizeof(void *) * 8)

/*************************************************************************
 * RtlEncodePointer   [NTDLL.@]
 */
PVOID WINAPI RtlEncodePointer( PVOID ptr )
{
    ULONG cookie = get_process_cookie();
    ULONG rotate = cookie % BIT_COUNT_IN_POINTER;
    ULONG_PTR ptrval = (ULONG_PTR)ptr ^ cookie;

    return (void *)((ptrval >> rotate) | (ptrval << ((BIT_COUNT_IN_POINTER - rotate) % BIT_COUNT_IN_POINTER)));
}

/*************************************************************************
 * RtlDecodePointer   [NTDLL.@]
 */
PVOID WINAPI RtlDecodePointer( PVOID ptr )
{
    ULONG cookie = get_process_cookie();
    ULONG rotate = cookie % BIT_COUNT_IN_POINTER;
    ULONG_PTR ptrval = (ULONG_PTR)ptr;

    ptrval = (ptrval << rotate) | (ptrval >> ((BIT_COUNT_IN_POINTER - rotate) % BIT_COUNT_IN_POINTER));
    return (void *)(ptrval ^ cookie);
}

/******************************************************************************
 *  RtlGetCompressionWorkSpaceSize		[NTDLL.@]
 */
NTSTATUS WINAPI RtlGetCompressionWorkSpaceSize(USHORT format, PULONG compress_workspace,
                                               PULONG decompress_workspace)
{
    FIXME("0x%04x, %p, %p: semi-stub\n", format, compress_workspace, decompress_workspace);

    switch (format & COMPRESSION_FORMAT_MASK)
    {
        case COMPRESSION_FORMAT_LZNT1:
            if (compress_workspace)
            {
                /* FIXME: The current implementation of RtlCompressBuffer does not use a
                 * workspace buffer, but Windows applications might expect a nonzero value. */
                *compress_workspace = 16;
            }
            if (decompress_workspace)
                *decompress_workspace = 0x1000;
            return STATUS_SUCCESS;

        case COMPRESSION_FORMAT_NONE:
        case COMPRESSION_FORMAT_DEFAULT:
            return STATUS_INVALID_PARAMETER;

        default:
            FIXME("format %u not implemented\n", format);
            return STATUS_UNSUPPORTED_COMPRESSION;
    }
}

/* compress data using LZNT1, currently only a stub */
static NTSTATUS lznt1_compress(UCHAR *src, ULONG src_size, UCHAR *dst, ULONG dst_size,
                               ULONG chunk_size, ULONG *final_size, UCHAR *workspace)
{
    UCHAR *src_cur = src, *src_end = src + src_size;
    UCHAR *dst_cur = dst, *dst_end = dst + dst_size;
    ULONG block_size;

    while (src_cur < src_end)
    {
        /* determine size of current chunk */
        block_size = min(0x1000, src_end - src_cur);
        if (dst_cur + sizeof(WORD) + block_size > dst_end)
            return STATUS_BUFFER_TOO_SMALL;

        /* write (uncompressed) chunk header */
        *(WORD *)dst_cur = 0x3000 | (block_size - 1);
        dst_cur += sizeof(WORD);

        /* write chunk content */
        memcpy(dst_cur, src_cur, block_size);
        dst_cur += block_size;
        src_cur += block_size;
    }

    if (final_size)
        *final_size = dst_cur - dst;

    return STATUS_SUCCESS;
}

/******************************************************************************
 *  RtlCompressBuffer		[NTDLL.@]
 */
NTSTATUS WINAPI RtlCompressBuffer(USHORT format, PUCHAR uncompressed, ULONG uncompressed_size,
                                  PUCHAR compressed, ULONG compressed_size, ULONG chunk_size,
                                  PULONG final_size, PVOID workspace)
{
    FIXME("0x%04x, %p, %lu, %p, %lu, %lu, %p, %p: semi-stub\n", format, uncompressed,
          uncompressed_size, compressed, compressed_size, chunk_size, final_size, workspace);

    switch (format & COMPRESSION_FORMAT_MASK)
    {
        case COMPRESSION_FORMAT_LZNT1:
            return lznt1_compress(uncompressed, uncompressed_size, compressed,
                                  compressed_size, chunk_size, final_size, workspace);

        case COMPRESSION_FORMAT_NONE:
        case COMPRESSION_FORMAT_DEFAULT:
            return STATUS_INVALID_PARAMETER;

        default:
            FIXME("format %u not implemented\n", format);
            return STATUS_UNSUPPORTED_COMPRESSION;
    }
}

/* decompress a single LZNT1 chunk */
static UCHAR *lznt1_decompress_chunk(UCHAR *dst, ULONG dst_size, UCHAR *src, ULONG src_size)
{
    UCHAR *src_cur = src, *src_end = src + src_size;
    UCHAR *dst_cur = dst, *dst_end = dst + dst_size;
    ULONG displacement_bits, length_bits;
    ULONG code_displacement, code_length;
    WORD flags, code;

    while (src_cur < src_end && dst_cur < dst_end)
    {
        flags = 0x8000 | *src_cur++;
        while ((flags & 0xff00) && src_cur < src_end)
        {
            if (flags & 1)
            {
                /* backwards reference */
                if (src_cur + sizeof(WORD) > src_end)
                    return NULL;

                code = *(WORD *)src_cur;
                src_cur += sizeof(WORD);

                /* find length / displacement bits */
                for (displacement_bits = 12; displacement_bits > 4; displacement_bits--)
                    if ((1 << (displacement_bits - 1)) < dst_cur - dst) break;

                length_bits       = 16 - displacement_bits;
                code_length       = (code & ((1 << length_bits) - 1)) + 3;
                code_displacement = (code >> length_bits) + 1;

                if (dst_cur < dst + code_displacement)
                    return NULL;

                /* copy bytes of chunk - we can't use memcpy() since source and dest can
                 * be overlapping, and the same bytes can be repeated over and over again */
                while (code_length--)
                {
                    if (dst_cur >= dst_end) return dst_cur;
                    *dst_cur = *(dst_cur - code_displacement);
                    dst_cur++;
                }
            }
            else
            {
                /* uncompressed data */
                if (dst_cur >= dst_end) return dst_cur;
                *dst_cur++ = *src_cur++;
            }
            flags >>= 1;
        }
    }

    return dst_cur;
}

/* decompress data encoded with LZNT1 */
static NTSTATUS lznt1_decompress(UCHAR *dst, ULONG dst_size, UCHAR *src, ULONG src_size,
                                 ULONG offset, ULONG *final_size, UCHAR *workspace)
{
    UCHAR *src_cur = src, *src_end = src + src_size;
    UCHAR *dst_cur = dst, *dst_end = dst + dst_size;
    ULONG chunk_size, block_size;
    WORD chunk_header;
    UCHAR *ptr;

    if (src_cur + sizeof(WORD) > src_end)
        return STATUS_BAD_COMPRESSION_BUFFER;

    /* skip over chunks with a distance >= 0x1000 to the destination offset */
    while (offset >= 0x1000 && src_cur + sizeof(WORD) <= src_end)
    {
        chunk_header = *(WORD *)src_cur;
        src_cur += sizeof(WORD);
        if (!chunk_header) goto out;
        chunk_size = (chunk_header & 0xfff) + 1;

        if (src_cur + chunk_size > src_end)
            return STATUS_BAD_COMPRESSION_BUFFER;

        src_cur += chunk_size;
        offset  -= 0x1000;
    }

    /* handle partially included chunk */
    if (offset && src_cur + sizeof(WORD) <= src_end)
    {
        chunk_header = *(WORD *)src_cur;
        src_cur += sizeof(WORD);
        if (!chunk_header) goto out;
        chunk_size = (chunk_header & 0xfff) + 1;

        if (src_cur + chunk_size > src_end)
            return STATUS_BAD_COMPRESSION_BUFFER;

        if (dst_cur >= dst_end)
            goto out;

        if (chunk_header & 0x8000)
        {
            /* compressed chunk */
            if (!workspace) return STATUS_ACCESS_VIOLATION;
            ptr = lznt1_decompress_chunk(workspace, 0x1000, src_cur, chunk_size);
            if (!ptr) return STATUS_BAD_COMPRESSION_BUFFER;
            if (ptr - workspace > offset)
            {
                block_size = min((ptr - workspace) - offset, dst_end - dst_cur);
                memcpy(dst_cur, workspace + offset, block_size);
                dst_cur += block_size;
            }
        }
        else
        {
            /* uncompressed chunk */
            if (chunk_size > offset)
            {
                block_size = min(chunk_size - offset, dst_end - dst_cur);
                memcpy(dst_cur, src_cur + offset, block_size);
                dst_cur += block_size;
            }
        }

        src_cur += chunk_size;
    }

    /* handle remaining chunks */
    while (src_cur + sizeof(WORD) <= src_end)
    {
        chunk_header = *(WORD *)src_cur;
        src_cur += sizeof(WORD);
        if (!chunk_header) goto out;
        chunk_size = (chunk_header & 0xfff) + 1;

        if (src_cur + chunk_size > src_end)
            return STATUS_BAD_COMPRESSION_BUFFER;

        /* fill space with padding when the previous chunk was decompressed
         * to less than 4096 bytes. no padding is needed for the last chunk
         * or when the next chunk is truncated */
        block_size = ((dst_cur - dst) + offset) & 0xfff;
        if (block_size)
        {
            block_size = 0x1000 - block_size;
            if (dst_cur + block_size >= dst_end)
                goto out;
            memset(dst_cur, 0, block_size);
            dst_cur += block_size;
        }

        if (dst_cur >= dst_end)
            goto out;

        if (chunk_header & 0x8000)
        {
            /* compressed chunk */
            dst_cur = lznt1_decompress_chunk(dst_cur, dst_end - dst_cur, src_cur, chunk_size);
            if (!dst_cur) return STATUS_BAD_COMPRESSION_BUFFER;
        }
        else
        {
            /* uncompressed chunk */
            block_size = min(chunk_size, dst_end - dst_cur);
            memcpy(dst_cur, src_cur, block_size);
            dst_cur += block_size;
        }

        src_cur += chunk_size;
    }

out:
    if (final_size)
        *final_size = dst_cur - dst;

    return STATUS_SUCCESS;

}

/******************************************************************************
 *  RtlDecompressFragment	[NTDLL.@]
 */
NTSTATUS WINAPI RtlDecompressFragment(USHORT format, PUCHAR uncompressed, ULONG uncompressed_size,
                               PUCHAR compressed, ULONG compressed_size, ULONG offset,
                               PULONG final_size, PVOID workspace)
{
    TRACE("0x%04x, %p, %lu, %p, %lu, %lu, %p, %p\n", format, uncompressed,
          uncompressed_size, compressed, compressed_size, offset, final_size, workspace);

    switch (format & COMPRESSION_FORMAT_MASK)
    {
        case COMPRESSION_FORMAT_LZNT1:
            return lznt1_decompress(uncompressed, uncompressed_size, compressed,
                                    compressed_size, offset, final_size, workspace);

        case COMPRESSION_FORMAT_NONE:
        case COMPRESSION_FORMAT_DEFAULT:
            return STATUS_INVALID_PARAMETER;

        default:
            FIXME("format %u not implemented\n", format);
            return STATUS_UNSUPPORTED_COMPRESSION;
    }
}


/******************************************************************************
 *  RtlDecompressBuffer		[NTDLL.@]
 */
NTSTATUS WINAPI RtlDecompressBuffer(USHORT format, PUCHAR uncompressed, ULONG uncompressed_size,
                                    PUCHAR compressed, ULONG compressed_size, PULONG final_size)
{
    TRACE("0x%04x, %p, %lu, %p, %lu, %p\n", format, uncompressed,
        uncompressed_size, compressed, compressed_size, final_size);

    return RtlDecompressFragment(format, uncompressed, uncompressed_size,
                                 compressed, compressed_size, 0, final_size, NULL);
}

/******************************************************************************
 * RtlGetCurrentTransaction [NTDLL.@]
 */
HANDLE WINAPI RtlGetCurrentTransaction(void)
{
    FIXME("() :stub\n");
    return NULL;
}

/******************************************************************************
 * RtlSetCurrentTransaction [NTDLL.@]
 */
BOOL WINAPI RtlSetCurrentTransaction(HANDLE new_transaction)
{
    FIXME("(%p) :stub\n", new_transaction);
    return FALSE;
}

/**********************************************************************
 *           RtlGetCurrentProcessorNumberEx [NTDLL.@]
 */
void WINAPI RtlGetCurrentProcessorNumberEx(PROCESSOR_NUMBER *processor)
{
    static int warn_once;

    if (!warn_once++)
        FIXME("(%p) :semi-stub\n", processor);

    processor->Group = 0;
    processor->Number = NtGetCurrentProcessorNumber();
    processor->Reserved = 0;
}

static RTL_BALANCED_NODE *rtl_node_parent( RTL_BALANCED_NODE *node )
{
    return (RTL_BALANCED_NODE *)(node->ParentValue & ~(ULONG_PTR)RTL_BALANCED_NODE_RESERVED_PARENT_MASK);
}

static void rtl_set_node_parent( RTL_BALANCED_NODE *node, RTL_BALANCED_NODE *parent )
{
    node->ParentValue = (ULONG_PTR)parent | (node->ParentValue & RTL_BALANCED_NODE_RESERVED_PARENT_MASK);
}

static void rtl_rotate( RTL_RB_TREE *tree, RTL_BALANCED_NODE *n, int right )
{
    RTL_BALANCED_NODE *child = n->Children[!right];
    RTL_BALANCED_NODE *parent = rtl_node_parent( n );

    if (!parent)                tree->root = child;
    else if (parent->Left == n) parent->Left = child;
    else                        parent->Right = child;

    n->Children[!right] = child->Children[right];
    if (n->Children[!right]) rtl_set_node_parent( n->Children[!right], n );
    child->Children[right] = n;
    rtl_set_node_parent( child, parent );
    rtl_set_node_parent( n, child );
}

static void rtl_flip_color( RTL_BALANCED_NODE *node )
{
    node->Red = !node->Red;
    node->Left->Red = !node->Left->Red;
    node->Right->Red = !node->Right->Red;
}

/*********************************************************************
 *           RtlRbInsertNodeEx [NTDLL.@]
 */
void WINAPI RtlRbInsertNodeEx( RTL_RB_TREE *tree, RTL_BALANCED_NODE *parent, BOOLEAN right, RTL_BALANCED_NODE *node )
{
    RTL_BALANCED_NODE *grandparent;

    TRACE( "tree %p, parent %p, right %d, node %p.\n", tree, parent, right, node );

    node->ParentValue = (ULONG_PTR)parent;
    node->Left = NULL;
    node->Right = NULL;

    if (!parent)
    {
        tree->root = tree->min = node;
        return;
    }
    if (right > 1)
    {
        ERR( "right %d.\n", right );
        return;
    }
    if (parent->Children[right])
    {
        ERR( "parent %p, right %d, child %p.\n", parent, right, parent->Children[right] );
        return;
    }

    node->Red = 1;
    parent->Children[right] = node;
    if (tree->min == parent && parent->Left == node) tree->min = node;
    grandparent = rtl_node_parent( parent );
    while (parent->Red)
    {
        right = (parent == grandparent->Right);
        if (grandparent->Children[!right] && grandparent->Children[!right]->Red)
        {
            node = grandparent;
            rtl_flip_color( node );
            if (!(parent = rtl_node_parent( node ))) break;
            grandparent = rtl_node_parent( parent );
            continue;
        }
        if (node == parent->Children[!right])
        {
            node = parent;
            rtl_rotate( tree, node, right );
            parent = rtl_node_parent( node );
            grandparent = rtl_node_parent( parent );
        }
        parent->Red = 0;
        grandparent->Red = 1;
        rtl_rotate( tree, grandparent, !right );
    }
    tree->root->Red = 0;
}

/*********************************************************************
 *           RtlRbRemoveNode [NTDLL.@]
 */
void WINAPI RtlRbRemoveNode( RTL_RB_TREE *tree, RTL_BALANCED_NODE *node )
{
    RTL_BALANCED_NODE *iter = NULL, *child, *parent, *w;
    BOOL need_fixup;
    int right;

    TRACE( "tree %p, node %p.\n", tree, node );

    if (node->Right && (node->Left || tree->min == node))
    {
        for (iter = node->Right; iter->Left; iter = iter->Left)
            ;
        if (tree->min == node) tree->min = iter;
    }
    else if (tree->min == node) tree->min = rtl_node_parent( node );
    if (!iter || !node->Left) iter = node;

    child = iter->Left ? iter->Left : iter->Right;

    if (!(parent = rtl_node_parent( iter ))) tree->root = child;
    else if (iter == parent->Left)           parent->Left = child;
    else                                     parent->Right = child;

    if (child) rtl_set_node_parent( child, parent );

    need_fixup = !iter->Red;

    if (node != iter)
    {
        *iter = *node;
        if (!(w = rtl_node_parent( iter ))) tree->root = iter;
        else if (node == w->Left)           w->Left = iter;
        else                                w->Right = iter;

        if (iter->Right) rtl_set_node_parent( iter->Right, iter );
        if (iter->Left)  rtl_set_node_parent( iter->Left, iter );
        if (parent == node) parent = iter;
    }

    if (!need_fixup)
    {
        if (tree->root) tree->root->Red = 0;
        return;
    }

    while (parent && !(child && child->Red))
    {
        right = (child == parent->Right);
        w = parent->Children[!right];
        if (w->Red)
        {
            w->Red = 0;
            parent->Red = 1;
            rtl_rotate( tree, parent, right );
            w = parent->Children[!right];
        }
        if ((w->Left && w->Left->Red) || (w->Right && w->Right->Red))
        {
            if (!(w->Children[!right] && w->Children[!right]->Red))
            {
                w->Children[right]->Red = 0;
                w->Red = 1;
                rtl_rotate( tree, w, !right );
                w = parent->Children[!right];
            }
            w->Red = parent->Red;
            parent->Red = 0;
            if (w->Children[!right]) w->Children[!right]->Red = 0;
            rtl_rotate( tree, parent, right );
            child = NULL;
            break;
        }
        w->Red = 1;
        child = parent;
        parent = rtl_node_parent( child );
    }
    if (child) child->Red = 0;
    if (tree->root) tree->root->Red = 0;
}

/***********************************************************************
 * AVL generic tables
 *
 * Layout follows Windows: BalancedRoot is a sentinel whose RightChild is the tree root, every
 * element is an RTL_BALANCED_LINKS immediately followed by the caller's data, and Balance holds
 * height(right) - height(left).
 */
static inline void *avl_data( RTL_BALANCED_LINKS *node ) { return node + 1; }
static inline RTL_BALANCED_LINKS *avl_node( void *data ) { return (RTL_BALANCED_LINKS *)data - 1; }

static RTL_BALANCED_LINKS *avl_leftmost( RTL_BALANCED_LINKS *node )
{
    while (node->LeftChild) node = node->LeftChild;
    return node;
}

static RTL_BALANCED_LINKS *avl_successor( RTL_AVL_TABLE *table, RTL_BALANCED_LINKS *node )
{
    RTL_BALANCED_LINKS *parent;

    if (node->RightChild) return avl_leftmost( node->RightChild );
    for (parent = node->Parent; parent != &table->BalancedRoot; node = parent, parent = node->Parent)
        if (parent->LeftChild == node) return parent;
    return NULL;
}

static RTL_BALANCED_LINKS *avl_predecessor( RTL_AVL_TABLE *table, RTL_BALANCED_LINKS *node )
{
    RTL_BALANCED_LINKS *parent;

    if (node->LeftChild)
    {
        node = node->LeftChild;
        while (node->RightChild) node = node->RightChild;
        return node;
    }
    for (parent = node->Parent; parent != &table->BalancedRoot; node = parent, parent = node->Parent)
        if (parent->RightChild == node) return parent;
    return NULL;
}

static void avl_replace_child( RTL_BALANCED_LINKS *parent, RTL_BALANCED_LINKS *old, RTL_BALANCED_LINKS *new )
{
    if (parent->LeftChild == old) parent->LeftChild = new;
    else parent->RightChild = new;
    if (new) new->Parent = parent;
}

static void avl_rotate_left( RTL_BALANCED_LINKS *x )
{
    RTL_BALANCED_LINKS *y = x->RightChild;

    x->RightChild = y->LeftChild;
    if (y->LeftChild) y->LeftChild->Parent = x;
    avl_replace_child( x->Parent, x, y );
    y->LeftChild = x;
    x->Parent = y;
}

static void avl_rotate_right( RTL_BALANCED_LINKS *x )
{
    RTL_BALANCED_LINKS *y = x->LeftChild;

    x->LeftChild = y->RightChild;
    if (y->RightChild) y->RightChild->Parent = x;
    avl_replace_child( x->Parent, x, y );
    y->RightChild = x;
    x->Parent = y;
}

/* rebalance at p (Balance is +2 or -2); returns the new subtree root and whether its height shrank */
static RTL_BALANCED_LINKS *avl_rebalance( RTL_BALANCED_LINKS *p, BOOL *shrunk )
{
    RTL_BALANCED_LINKS *n, *m;

    if (p->Balance == 2)
    {
        n = p->RightChild;
        if (n->Balance >= 0)
        {
            avl_rotate_left( p );
            if (n->Balance == 0) { p->Balance = 1; n->Balance = -1; *shrunk = FALSE; }
            else { p->Balance = 0; n->Balance = 0; *shrunk = TRUE; }
            return n;
        }
        m = n->LeftChild;
        avl_rotate_right( n );
        avl_rotate_left( p );
        if (m->Balance == 1) { p->Balance = -1; n->Balance = 0; }
        else if (m->Balance == -1) { p->Balance = 0; n->Balance = 1; }
        else { p->Balance = 0; n->Balance = 0; }
        m->Balance = 0;
        *shrunk = TRUE;
        return m;
    }
    n = p->LeftChild;
    if (n->Balance <= 0)
    {
        avl_rotate_right( p );
        if (n->Balance == 0) { p->Balance = -1; n->Balance = 1; *shrunk = FALSE; }
        else { p->Balance = 0; n->Balance = 0; *shrunk = TRUE; }
        return n;
    }
    m = n->RightChild;
    avl_rotate_left( n );
    avl_rotate_right( p );
    if (m->Balance == -1) { p->Balance = 1; n->Balance = 0; }
    else if (m->Balance == 1) { p->Balance = 0; n->Balance = -1; }
    else { p->Balance = 0; n->Balance = 0; }
    m->Balance = 0;
    *shrunk = TRUE;
    return m;
}

static void avl_insert_fixup( RTL_AVL_TABLE *table, RTL_BALANCED_LINKS *node )
{
    RTL_BALANCED_LINKS *p;
    BOOL shrunk;

    for (p = node->Parent; p != &table->BalancedRoot; node = p, p = node->Parent)
    {
        if (p->LeftChild == node) p->Balance--; else p->Balance++;
        if (p->Balance == 0) break;
        if (p->Balance == 2 || p->Balance == -2)
        {
            avl_rebalance( p, &shrunk );
            break;
        }
    }
}

/* the subtree on side 'left_side' of p just lost one level of height */
static void avl_delete_fixup( RTL_AVL_TABLE *table, RTL_BALANCED_LINKS *p, BOOL left_side )
{
    BOOL shrunk;

    while (p != &table->BalancedRoot)
    {
        RTL_BALANCED_LINKS *parent;

        if (left_side) p->Balance++; else p->Balance--;
        if (p->Balance == 1 || p->Balance == -1) break;
        if (p->Balance == 2 || p->Balance == -2)
        {
            p = avl_rebalance( p, &shrunk );
            if (!shrunk) break;
        }
        parent = p->Parent;
        left_side = (parent->LeftChild == p);
        p = parent;
    }
}

static RTL_BALANCED_LINKS *avl_find( RTL_AVL_TABLE *table, void *buffer, RTL_BALANCED_LINKS **parent_out,
                                     RTL_GENERIC_COMPARE_RESULTS *result_out )
{
    RTL_BALANCED_LINKS *node = table->BalancedRoot.RightChild, *parent = &table->BalancedRoot;
    RTL_GENERIC_COMPARE_RESULTS result = GenericLessThan;   /* empty tree: nothing matched */

    while (node)
    {
        result = table->CompareRoutine( table, buffer, avl_data( node ) );
        if (result == GenericEqual) break;
        parent = node;
        node = (result == GenericLessThan) ? node->LeftChild : node->RightChild;
    }
    if (parent_out) *parent_out = parent;
    if (result_out) *result_out = result;
    return node;
}

/***********************************************************************
 *           RtlInitializeGenericTableAvl  (NTDLL.@)
 */
void WINAPI RtlInitializeGenericTableAvl(PRTL_AVL_TABLE table, PRTL_AVL_COMPARE_ROUTINE compare,
                                         PRTL_AVL_ALLOCATE_ROUTINE allocate, PRTL_AVL_FREE_ROUTINE free, void *context)
{
    TRACE("%p %p %p %p %p\n", table, compare, allocate, free, context);

    memset( table, 0, sizeof(*table) );
    table->BalancedRoot.Parent = &table->BalancedRoot;
    table->CompareRoutine = compare;
    table->AllocateRoutine = allocate;
    table->FreeRoutine = free;
    table->TableContext = context;
}

/***********************************************************************
 *           RtlInsertElementGenericTableFullAvl  (NTDLL.@)
 */
void * WINAPI RtlInsertElementGenericTableFullAvl(PRTL_AVL_TABLE table, void *buffer, ULONG size, BOOLEAN *new_element,
                                                  void *node_or_parent, RTL_GENERIC_COMPARE_RESULTS search_result)
{
    RTL_BALANCED_LINKS *parent = node_or_parent, *node;

    TRACE("%p %p %lu %p %p %u\n", table, buffer, size, new_element, node_or_parent, search_result);

    if (search_result == GenericEqual && parent != &table->BalancedRoot)
    {
        if (new_element) *new_element = FALSE;
        return avl_data( parent );
    }
    if (!(node = table->AllocateRoutine( table, sizeof(*node) + size )))
    {
        if (new_element) *new_element = FALSE;
        return NULL;
    }
    node->Parent = parent;
    node->LeftChild = node->RightChild = NULL;
    node->Balance = 0;
    memcpy( avl_data( node ), buffer, size );
    if (parent == &table->BalancedRoot) parent->RightChild = node;
    else if (search_result == GenericLessThan) parent->LeftChild = node;
    else parent->RightChild = node;
    table->NumberGenericTableElements++;
    table->WhichOrderedElement = 0;
    table->OrderedPointer = NULL;
    avl_insert_fixup( table, node );
    if (new_element) *new_element = TRUE;
    return avl_data( node );
}

/***********************************************************************
 *           RtlInsertElementGenericTableAvl  (NTDLL.@)
 */
void * WINAPI RtlInsertElementGenericTableAvl(PRTL_AVL_TABLE table, void *buffer, ULONG size, BOOLEAN *new_element)
{
    RTL_BALANCED_LINKS *parent, *node;
    RTL_GENERIC_COMPARE_RESULTS result;

    TRACE("%p %p %lu %p\n", table, buffer, size, new_element);

    node = avl_find( table, buffer, &parent, &result );
    return RtlInsertElementGenericTableFullAvl( table, buffer, size, new_element, node ? node : parent, node ? GenericEqual : result );
}

/******************************************************************************
 *           RtlLookupElementGenericTableFullAvl  (NTDLL.@)
 */
void * WINAPI RtlLookupElementGenericTableFullAvl(PRTL_AVL_TABLE table, void *buffer, void **node_or_parent,
                                                  RTL_GENERIC_COMPARE_RESULTS *search_result)
{
    RTL_BALANCED_LINKS *parent, *node;
    RTL_GENERIC_COMPARE_RESULTS result;

    TRACE("%p %p %p %p\n", table, buffer, node_or_parent, search_result);

    node = avl_find( table, buffer, &parent, &result );
    if (node_or_parent) *node_or_parent = node ? node : parent;
    if (search_result) *search_result = node ? GenericEqual : result;
    return node ? avl_data( node ) : NULL;
}

/******************************************************************************
 *           RtlLookupElementGenericTableAvl  (NTDLL.@)
 */
void * WINAPI RtlLookupElementGenericTableAvl(PRTL_AVL_TABLE table, void *buffer)
{
    RTL_BALANCED_LINKS *node;

    TRACE("%p %p\n", table, buffer);

    node = avl_find( table, buffer, NULL, NULL );
    return node ? avl_data( node ) : NULL;
}

/******************************************************************************
 *           RtlLookupFirstMatchingElementGenericTableAvl  (NTDLL.@)
 */
void * WINAPI RtlLookupFirstMatchingElementGenericTableAvl(PRTL_AVL_TABLE table, void *buffer, void **restart_key)
{
    RTL_BALANCED_LINKS *node, *prev;

    TRACE("%p %p %p\n", table, buffer, restart_key);

    if (!(node = avl_find( table, buffer, NULL, NULL )))
    {
        if (restart_key) *restart_key = NULL;
        return NULL;
    }
    while ((prev = avl_predecessor( table, node )) && table->CompareRoutine( table, buffer, avl_data( prev ) ) == GenericEqual)
        node = prev;
    if (restart_key) *restart_key = node;
    return avl_data( node );
}

/***********************************************************************
 *           RtlDeleteElementGenericTableAvl  (NTDLL.@)
 */
BOOLEAN WINAPI RtlDeleteElementGenericTableAvl(PRTL_AVL_TABLE table, void *buffer)
{
    RTL_BALANCED_LINKS *node, *fixup, *child;
    BOOL left_side;

    TRACE("%p %p\n", table, buffer);

    if (!(node = avl_find( table, buffer, NULL, NULL ))) return FALSE;

    if (node->LeftChild && node->RightChild)
    {
        /* relink the in-order successor into node's place (data pointers must stay stable) */
        RTL_BALANCED_LINKS *succ = avl_leftmost( node->RightChild );

        if (succ->Parent != node)
        {
            fixup = succ->Parent;
            left_side = TRUE;
            avl_replace_child( succ->Parent, succ, succ->RightChild );
            succ->RightChild = node->RightChild;
            succ->RightChild->Parent = succ;
        }
        else
        {
            fixup = succ;
            left_side = FALSE;
        }
        succ->LeftChild = node->LeftChild;
        succ->LeftChild->Parent = succ;
        succ->Balance = node->Balance;
        avl_replace_child( node->Parent, node, succ );
    }
    else
    {
        child = node->LeftChild ? node->LeftChild : node->RightChild;
        fixup = node->Parent;
        left_side = (fixup->LeftChild == node);
        avl_replace_child( fixup, node, child );
    }
    avl_delete_fixup( table, fixup, left_side );

    if (table->RestartKey == node) table->RestartKey = NULL;
    table->NumberGenericTableElements--;
    table->DeleteCount++;
    table->WhichOrderedElement = 0;
    table->OrderedPointer = NULL;
    table->FreeRoutine( table, node );
    return TRUE;
}

/******************************************************************************
 *           RtlEnumerateGenericTableWithoutSplayingAvl  (NTDLL.@)
 */
void * WINAPI RtlEnumerateGenericTableWithoutSplayingAvl(RTL_AVL_TABLE *table, PVOID *restart_key)
{
    RTL_BALANCED_LINKS *node;

    TRACE("%p %p\n", table, restart_key);

    if (!table->BalancedRoot.RightChild) return NULL;
    if (!*restart_key) node = avl_leftmost( table->BalancedRoot.RightChild );
    else node = avl_successor( table, *restart_key );
    *restart_key = node;
    return node ? avl_data( node ) : NULL;
}

/******************************************************************************
 *           RtlEnumerateGenericTableAvl  (NTDLL.@)
 */
void * WINAPI RtlEnumerateGenericTableAvl(RTL_AVL_TABLE *table, BOOLEAN restart)
{
    void *key = restart ? NULL : table->RestartKey;
    void *ret = RtlEnumerateGenericTableWithoutSplayingAvl( table, &key );

    table->RestartKey = key;
    return ret;
}

/******************************************************************************
 *  RtlNumberGenericTableElementsAvl  (NTDLL.@)
 */
ULONG WINAPI RtlNumberGenericTableElementsAvl(RTL_AVL_TABLE *table)
{
    TRACE("%p\n", table);
    return table->NumberGenericTableElements;
}

/******************************************************************************
 *  RtlIsGenericTableEmptyAvl  (NTDLL.@)
 */
BOOLEAN WINAPI RtlIsGenericTableEmptyAvl(RTL_AVL_TABLE *table)
{
    TRACE("%p\n", table);
    return table->NumberGenericTableElements == 0;
}

/******************************************************************************
 *  RtlGetElementGenericTableAvl  (NTDLL.@)
 */
void * WINAPI RtlGetElementGenericTableAvl(RTL_AVL_TABLE *table, ULONG index)
{
    RTL_BALANCED_LINKS *node;
    ULONG i;

    TRACE("%p %lu\n", table, index);

    if (index >= table->NumberGenericTableElements) return NULL;
    node = avl_leftmost( table->BalancedRoot.RightChild );
    for (i = 0; i < index; i++) node = avl_successor( table, node );
    return avl_data( node );
}

/* Package identity for hosted UWP apps, see kernelbase/version.c for the variables. */
static NTSTATUS get_package_env( const WCHAR *var, WCHAR *buffer, SIZE_T *size )
{
    UNICODE_STRING name, value;
    WCHAR tmp[1024];
    NTSTATUS status;
    SIZE_T len;

    RtlInitUnicodeString( &name, var );
    value.Buffer = tmp;
    value.MaximumLength = sizeof(tmp);
    if ((status = RtlQueryEnvironmentVariable_U( NULL, &name, &value ))) return STATUS_NOT_FOUND;
    len = value.Length / sizeof(WCHAR) + 1;
    if (!size) return STATUS_SUCCESS;
    if (!buffer || *size < len)
    {
        *size = len;
        return STATUS_BUFFER_TOO_SMALL;
    }
    *size = len;
    memcpy( buffer, tmp, len * sizeof(WCHAR) );
    return STATUS_SUCCESS;
}

/*********************************************************************
 *           RtlQueryPackageIdentity [NTDLL.@]
 */
NTSTATUS WINAPI RtlQueryTokenHostIdAsUlong64( HANDLE token, ULONG64 *host_id )
{
    TRACE( "%p %p\n", token, host_id );
    if (host_id) *host_id = 0;
    return STATUS_NOT_FOUND;
}

NTSTATUS WINAPI RtlQueryPackageIdentity(HANDLE token, WCHAR *fullname, SIZE_T *fullname_size,
                                        WCHAR *appid, SIZE_T *appid_size, BOOLEAN *packaged)
{
    NTSTATUS status;

    TRACE("(%p, %p, %p, %p, %p, %p)\n", token, fullname, fullname_size, appid, appid_size, packaged);

    if ((status = get_package_env( L"WINE_UWP_PACKAGE_FULL_NAME", fullname, fullname_size ))) return status;
    if (appid_size && (status = get_package_env( L"WINE_UWP_AUMID", appid, appid_size ))) return status;
    if (packaged) *packaged = TRUE;
    return STATUS_SUCCESS;
}

#define PSM_ACTIVATION_TOKEN_PACKAGED_APPLICATION 0x01
#define PSM_ACTIVATION_TOKEN_DEVELOPMENT_APP      0x10
#define PACKAGE_ORIGIN_DEVELOPER_UNSIGNED 4

/* PS_PKG_CLAIM: two 32-bit fields, eight bytes in total */
struct ps_pkg_claim
{
    ULONG flags;
    ULONG origin;
};

/*********************************************************************
 *           RtlQueryPackageClaims [NTDLL.@]
 *
 * Queried by Windows.UI.Xaml.dll during Application init: it fail-fasts unless
 * this succeeds with the PACKAGED_APPLICATION flag set.
 */
NTSTATUS WINAPI RtlQueryPackageClaims(HANDLE token, WCHAR *fullname, SIZE_T *fullname_size,
                                      WCHAR *appid, SIZE_T *appid_size, GUID *dynamic_id,
                                      struct ps_pkg_claim *pkg_claim, ULONG64 *attributes_present)
{
    NTSTATUS status;

    TRACE("(%p, %p, %p, %p, %p, %p, %p, %p)\n", token, fullname, fullname_size,
          appid, appid_size, dynamic_id, pkg_claim, attributes_present);

    if ((status = get_package_env( L"WINE_UWP_PACKAGE_FULL_NAME", fullname, fullname_size ))) return status;
    if (appid_size && (status = get_package_env( L"WINE_UWP_AUMID", appid, appid_size ))) return status;
    if (dynamic_id) memset( dynamic_id, 0, sizeof(*dynamic_id) );
    if (pkg_claim)
    {
        pkg_claim->flags = PSM_ACTIVATION_TOKEN_PACKAGED_APPLICATION | PSM_ACTIVATION_TOKEN_DEVELOPMENT_APP;
        pkg_claim->origin = PACKAGE_ORIGIN_DEVELOPER_UNSIGNED;
    }
    if (attributes_present) *attributes_present = 0;
    return STATUS_SUCCESS;
}

/*********************************************************************
 *           RtlQueryProcessPlaceholderCompatibilityMode [NTDLL.@]
 */
char WINAPI RtlQueryProcessPlaceholderCompatibilityMode(void)
{
    FIXME("stub\n");
    return PHCM_APPLICATION_DEFAULT;
}

/*********************************************************************
 *           RtlGetDeviceFamilyInfoEnum [NTDLL.@]
 */
static void arm_uwp_breaks( void );

void WINAPI RtlGetDeviceFamilyInfoEnum(ULONGLONG *version, DWORD *family, DWORD *form)
{
    TRACE("%p %p %p\n", version, family, form);

    arm_uwp_breaks();   /* debugging aid: reaches threads that never call RtlIsMultiSessionSku */

    /* the packed OS version, as AnalyticsInfo.VersionInfo.DeviceFamilyVersion reports it. It was 0,
     * and the Xbox app then judged the OS too old to install Gaming Services. */
    if (version)
    {
        RTL_OSVERSIONINFOEXW info = { sizeof(info) };
        ULONGLONG revision = 0;

        RtlGetVersion( &info );
        {
            UNICODE_STRING name;
            OBJECT_ATTRIBUTES attr;
            HANDLE key;
            char buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(DWORD)];
            DWORD len;

            RtlInitUnicodeString( &name, L"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion" );
            InitializeObjectAttributes( &attr, &name, OBJ_CASE_INSENSITIVE, 0, NULL );
            if (!NtOpenKey( &key, KEY_QUERY_VALUE, &attr ))
            {
                RtlInitUnicodeString( &name, L"UBR" );
                if (!NtQueryValueKey( key, &name, KeyValuePartialInformation, buf, sizeof(buf), &len ) &&
                    ((KEY_VALUE_PARTIAL_INFORMATION *)buf)->Type == REG_DWORD)
                    revision = *(DWORD *)((KEY_VALUE_PARTIAL_INFORMATION *)buf)->Data;
                NtClose( key );
            }
        }
        *version = ((ULONGLONG)info.dwMajorVersion << 48) | ((ULONGLONG)info.dwMinorVersion << 32) |
                   ((ULONGLONG)(info.dwBuildNumber & 0xffff) << 16) | (revision & 0xffff);
    }
    if (family)
    {
        WCHAR buffer[16];
        SIZE_T size = sizeof(buffer);

        /* WINE_UWP_DEVICE_FAMILY overrides the reported family; the desktop family makes the
         * app model expect shell services (an activation watcher, a view manager) that a Wine
         * prefix has no counterpart for. */
        *family = get_package_env( L"WINE_UWP_DEVICE_FAMILY", buffer, &size )
                ? DEVICEFAMILYINFOENUM_DESKTOP : wcstoul( buffer, NULL, 10 );
    }
    if (form) *form = DEVICEFAMILYDEVICEFORM_UNKNOWN;
}

/*********************************************************************
 *           RtlConvertDeviceFamilyInfoToString [NTDLL.@]
 */
DWORD WINAPI RtlConvertDeviceFamilyInfoToString(DWORD *device_family_size, DWORD *device_form_size,
                                                WCHAR *device_family, WCHAR *device_form)
{
    static const WCHAR *windows_desktop = L"Windows.Desktop";
    static const WCHAR *unknown_form = L"Unknown";
    DWORD family_length, form_length;

    TRACE("%p %p %p %p\n", device_family_size, device_form_size, device_family, device_form);

    family_length = (wcslen(windows_desktop) + 1) * sizeof(WCHAR);
    form_length = (wcslen(unknown_form) + 1) * sizeof(WCHAR);

    if (*device_family_size < family_length || *device_form_size < form_length)
    {
        *device_family_size = family_length;
        *device_form_size = form_length;
        return STATUS_BUFFER_TOO_SMALL;
    }

    memcpy(device_family, windows_desktop, family_length);
    memcpy(device_form, unknown_form, form_length);
    return STATUS_SUCCESS;
}

/*********************************************************************
 * Windows Notification Facility. There is no WNF publisher here; subscriptions succeed and never fire,
 * queries report no data. Windows.UI.Xaml subscribes for theme and settings changes.
 */
NTSTATUS WINAPI RtlSubscribeWnfStateChangeNotification( void **subscription, ULONGLONG state_name, ULONG change_stamp,
                                                        void *callback, void *context, void *type_id, ULONG serialization_group, ULONG flags )
{
    static int once;
    if (!once++) FIXME( "%p %s %lu %p %p %p %lu %#lx: stub\n", subscription, wine_dbgstr_longlong(state_name), change_stamp,
                        callback, context, type_id, serialization_group, flags );
    if (subscription) *subscription = (void *)(ULONG_PTR)0x574e46;  /* 'WNF' */
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI RtlUnsubscribeWnfStateChangeNotification( void *subscription )
{
    TRACE( "%p\n", subscription );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI RtlUnsubscribeWnfNotificationWaitForCompletion( void *subscription )
{
    TRACE( "%p\n", subscription );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI RtlQueryWnfStateData( ULONG *change_stamp, ULONGLONG state_name, void *callback, void *context, void *type_id )
{
    static int once;
    if (!once++) FIXME( "%p %s %p %p %p: stub, reporting empty state\n", change_stamp, wine_dbgstr_longlong(state_name), callback, context, type_id );
    if (change_stamp) *change_stamp = 0;
    /* STATUS_OBJECT_NAME_NOT_FOUND here becomes hr 0xd0000034 in native WIL helpers, which
     * then raises it as a language exception through RaiseRoOriginateOnWilExceptions and
     * kills the XAML render walk (ProjectedShadowManager material props). An empty state is
     * the "no override configured" answer callers expect. */
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI RtlPublishWnfStateData( ULONGLONG state_name, const void *type_id, const void *buffer, ULONG length, const void *scope )
{
    static int once;
    if (!once++) FIXME( "%s %p %p %lu %p: stub\n", wine_dbgstr_longlong(state_name), type_id, buffer, length, scope );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtQueryWnfStateData( const ULONGLONG *state_name, const void *type_id, const void *scope,
                                     ULONG *change_stamp, void *buffer, ULONG *size )
{
    static int once;
    if (!once++) FIXME( "%p %p %p %p %p %p: stub, reporting empty state\n", state_name, type_id, scope, change_stamp, buffer, size );
    if (change_stamp) *change_stamp = 0;
    if (size) *size = 0;
    return STATUS_SUCCESS;
}

/* Debugging aid: arm execute breakpoints in another module for this thread, so a failure path deep
 * inside a native DLL reports its arguments. WINE_UWP_BREAK_MOD names the module and
 * WINE_UWP_BREAK_OFF is a comma separated list of up to four hex offsets. */
/* Debugging aid: WINE_UWP_BREAK_WATCH=<hex> turns a breakpoint into "single-step from here until
 * rax holds this value, then say where". It is the only practical way to find which instruction
 * inside a large native function first produces a failing HRESULT. */
#define UWP_STEP_HISTORY 256

static ULONG_PTR uwp_step_history[UWP_STEP_HISTORY];
static unsigned int uwp_step_pos, uwp_step_count;
static ULONG_PTR uwp_step_watch, uwp_step_entry_rsp, uwp_step_stop;
static BOOL uwp_stepping;

static BOOL uwp_start_stepping( CONTEXT *ctx )
{
    WCHAR buffer[32];
    SIZE_T size = sizeof(buffer);

    if (uwp_stepping) return TRUE;
    if (get_package_env( L"WINE_UWP_BREAK_WATCH", buffer, &size )) return FALSE;
    uwp_step_watch = wcstoul( buffer, NULL, 16 );
    uwp_step_count = 0;
    uwp_step_pos = 0;
    uwp_stepping = TRUE;
    uwp_step_entry_rsp = ctx->Rsp;
    ctx->EFlags |= 0x100;   /* trap flag */
    ERR( "stepping from %p until rax is %#Ix\n", (void *)ctx->Rip, uwp_step_watch );
    return TRUE;
}

/* returns TRUE when the single step was consumed here */
static BOOL uwp_step( CONTEXT *ctx )
{
    if (!uwp_stepping) return FALSE;

    uwp_step_history[uwp_step_pos++ % UWP_STEP_HISTORY] = ctx->Rip;
    /* stop when the watched function has returned, so each call is stepped on its own */
    if (ctx->Rax == uwp_step_watch || (uwp_step_stop && ctx->Rip == uwp_step_stop) ||
        ctx->Rsp > uwp_step_entry_rsp || ++uwp_step_count > 400000)
    {
        unsigned int i;

        if (ctx->Rax == uwp_step_watch || (uwp_step_stop && ctx->Rip == uwp_step_stop) ||
            ctx->Rsp > uwp_step_entry_rsp)
        {
            ERR( "step done at %p after %u steps, rax %#Ix rsp %#Ix (entry rsp %#Ix):\n",
                 (void *)ctx->Rip, uwp_step_count, (ULONG_PTR)ctx->Rax,
                 (ULONG_PTR)ctx->Rsp, (ULONG_PTR)uwp_step_entry_rsp );
            for (i = 0; i < UWP_STEP_HISTORY && i < uwp_step_pos; i++)
                ERR( "  step[-%u] %p\n", i, (void *)uwp_step_history[(uwp_step_pos - 1 - i) % UWP_STEP_HISTORY] );
        }
        else ERR( "step limit %u at %p rax %#Ix\n", uwp_step_count, (void *)ctx->Rip, (ULONG_PTR)ctx->Rax );
        uwp_stepping = FALSE;
        ctx->EFlags &= ~0x100;
    }
    else ctx->EFlags |= 0x100;

    ctx->Dr6 = 0;
    return TRUE;
}

static LONG CALLBACK uwp_break_handler( EXCEPTION_POINTERS *info )
{
    if (info->ExceptionRecord->ExceptionCode == STATUS_SINGLE_STEP)
    {
        CONTEXT *ctx = info->ContextRecord;

        if (uwp_step( ctx )) return EXCEPTION_CONTINUE_EXECUTION;
        if (uwp_start_stepping( ctx ))
        {
            ctx->Dr6 = 0;
            ctx->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        {
            /* one and two levels of indirection through rdx: enough to read a pointer field and
             * the object it points at, which is how most of these call sites are shaped */
            void *deref1 = NULL, *deref2 = NULL;
            MEMORY_BASIC_INFORMATION mbi;
            SIZE_T size;
            if (ctx->Rdx > 0x10000 &&
                !NtQueryVirtualMemory( GetCurrentProcess(), (void *)ctx->Rdx, MemoryBasicInformation, &mbi, sizeof(mbi), &size ) &&
                mbi.State == MEM_COMMIT)
            {
                deref1 = *(void **)ctx->Rdx;
                if ((ULONG_PTR)deref1 > 0x10000 &&
                    !NtQueryVirtualMemory( GetCurrentProcess(), deref1, MemoryBasicInformation, &mbi, sizeof(mbi), &size ) &&
                    mbi.State == MEM_COMMIT)
                    deref2 = *(void **)deref1;
            }
            ERR( "break %p [rdx] %p [[rdx]] %p\n", (void *)ctx->Rip, deref1, deref2 );
        }
        {
            DWORD state108 = 0;
            void *rcx_deref = NULL, *handle40 = NULL;
            MEMORY_BASIC_INFORMATION mbi;
            SIZE_T size;
            if (ctx->Rcx > 0x10000 &&
                !NtQueryVirtualMemory( GetCurrentProcess(), (void *)ctx->Rcx, MemoryBasicInformation,
                                       &mbi, sizeof(mbi), &size ) &&
                mbi.State == MEM_COMMIT && mbi.RegionSize >= 0x110)
            {
                state108 = *(DWORD *)((char *)ctx->Rcx + 0x108);
                rcx_deref = *(void **)ctx->Rcx;
                handle40 = *(void **)((char *)ctx->Rcx + 0x40);
            }
            ERR( "break %p [rcx] %p [rcx+0x40] %p [rcx+0x108] %#x\n",
                 (void *)ctx->Rip, rcx_deref, handle40, state108 );
        }
        ERR( "break %p rcx %p rdx %p r8 %p r9 %p rax %p rdi %p rsi %p rbx %p rsp %p [rsp] %p\n",
             (void *)ctx->Rip, (void *)ctx->Rcx, (void *)ctx->Rdx, (void *)ctx->R8, (void *)ctx->R9,
             (void *)ctx->Rax, (void *)ctx->Rdi, (void *)ctx->Rsi, (void *)ctx->Rbx, (void *)ctx->Rsp,
             *(void **)ctx->Rsp );
        ctx->Dr6 = 0;
        ctx->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void arm_uwp_breaks( void )
{
    static LONG handler_added;
    UNICODE_STRING name;
    WCHAR modname[64], offsets[128], *p, *next;
    CONTEXT ctx;
    HMODULE module;
    unsigned int n = 0;
    SIZE_T size;

    static LONG configured = -1;   /* WINE_UWP_BREAK_MOD set? The environment does not change. */

    /* this runs from hot paths (RtlIsMultiSessionSku, every LdrLoadDll): without breakpoints configured,
     * return before the thread context and environment round trips */
    if (!configured) return;
    if (configured < 0)
    {
        size = sizeof(modname);
        configured = !get_package_env( L"WINE_UWP_BREAK_MOD", modname, &size );
        if (!configured) return;
    }
    memset( &ctx, 0, sizeof(ctx) );
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!NtGetContextThread( GetCurrentThread(), &ctx ) && ctx.Dr7) return;  /* already armed here */
    size = sizeof(modname);
    if (get_package_env( L"WINE_UWP_BREAK_MOD", modname, &size )) return;
    size = sizeof(offsets);
    if (get_package_env( L"WINE_UWP_BREAK_OFF", offsets, &size )) return;
    RtlInitUnicodeString( &name, modname );
    module = NULL;
    {
        LIST_ENTRY *head = &NtCurrentTeb()->Peb->LdrData->InMemoryOrderModuleList, *entry;
        for (entry = head->Flink; entry != head; entry = entry->Flink)
        {
            LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
            if (!RtlCompareUnicodeString( &mod->BaseDllName, &name, TRUE ))
            {
                module = mod->DllBase;
                break;
            }
        }
    }
    if (!module) { ERR( "module %s not loaded yet\n", debugstr_us(&name) ); return; }
    if (!InterlockedExchange( &handler_added, 1 ))
        RtlAddVectoredExceptionHandler( 1, uwp_break_handler );

    {
        WCHAR stop[32];
        SIZE_T stop_size = sizeof(stop);
        if (!get_package_env( L"WINE_UWP_BREAK_STOPRVA", stop, &stop_size ))
            uwp_step_stop = (ULONG_PTR)module + wcstoul( stop, NULL, 16 );
    }

    memset( &ctx, 0, sizeof(ctx) );
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    for (p = offsets; *p && n < 4; p = next)
    {
        ULONG_PTR addr = (ULONG_PTR)module + wcstoul( p, &next, 16 );
        if (next == p) break;
        while (*next == ',') next++;
        switch (n)
        {
        case 0: ctx.Dr0 = addr; break;
        case 1: ctx.Dr1 = addr; break;
        case 2: ctx.Dr2 = addr; break;
        case 3: ctx.Dr3 = addr; break;
        }
        ctx.Dr7 |= 1u << (2 * n);
        ERR( "break %u at %#Ix\n", n, addr );
        n++;
    }
    if (n) NtSetContextThread( GetCurrentThread(), &ctx );
}

void uwp_rearm_breaks( void )
{
    arm_uwp_breaks();
}

struct rtl_feature_configuration
{
    ULONG feature_id;
    ULONG flags;   /* priority:4, enabled_state:2, is_wexp:1, has_subscriptions:1, variant:6, kind:2 */
    ULONG payload;
};

/*********************************************************************
 *           RtlQueryFeatureConfiguration [NTDLL.@]
 *
 * Windows keeps per-feature "velocity" state in a staging store that Wine has no equivalent for,
 * so every feature runs at the caller's compiled-in default.  WINE_UWP_FEATURES overrides that for
 * named features: a comma separated list of <hex feature id>:<state>, or "all:<state>", where the
 * state is 0 default, 1 disabled, 2 enabled.
 */
NTSTATUS WINAPI RtlQueryFeatureConfiguration( ULONG feature_id, ULONG type, ULONGLONG *change_stamp,
                                              struct rtl_feature_configuration *config )
{
    static WCHAR buffer[256];
    static LONG have_features = -1;    /* the environment does not change; WIL asks for every feature check */
    WCHAR *p;
    UNICODE_STRING name, value;
    ULONG state = 0;

    TRACE( "%#lx %lu %p %p\n", feature_id, type, change_stamp, config );

    if (change_stamp) *change_stamp = 0;
    if (!config) return STATUS_INVALID_PARAMETER;

    if (have_features < 0)
    {
        RtlInitUnicodeString( &name, L"WINE_UWP_FEATURES" );
        value.Buffer = buffer;
        value.MaximumLength = sizeof(buffer) - sizeof(WCHAR);
        if (!RtlQueryEnvironmentVariable_U( NULL, &name, &value ))
        {
            buffer[value.Length / sizeof(WCHAR)] = 0;
            have_features = 1;
        }
        else have_features = 0;
    }
    if (!have_features) return STATUS_NOT_FOUND;

    for (p = buffer; *p; )
    {
        WCHAR *end;
        ULONG id;

        if (p[0] == 'a' && p[1] == 'l' && p[2] == 'l' && p[3] == ':') { id = feature_id; end = p + 3; }
        else if ((end = p, id = wcstoul( p, &end, 16 ), end == p || *end != ':')) break;
        p = end + 1;
        if (id == feature_id)
        {
            state = wcstoul( p, &end, 10 );
            break;
        }
        if (!(p = wcschr( p, ',' ))) break;
        p++;
    }
    if (!state) return STATUS_NOT_FOUND;

    config->feature_id = feature_id;
    config->flags = (state & 3) << 4;
    config->payload = 0;
    return STATUS_SUCCESS;
}

/* Per-user state separation (a server SKU feature); never on for a normal desktop prefix. */
BOOLEAN WINAPI RtlIsStateSeparationEnabled( void )
{
    return FALSE;
}

BOOLEAN WINAPI RtlIsMultiSessionSku( void )
{
    arm_uwp_breaks();
    return FALSE;
}

/* The companion of RtlIsMultiSessionSku: several sessions of the same user on one machine, which a
 * desktop prefix never has. twinapi.appcore calls it while publishing an app window, and the missing
 * export aborts the process (Feedback Hub). */
BOOLEAN WINAPI RtlIsMultiUsersInSessionSku( void )
{
    return FALSE;
}

NTSTATUS WINAPI NtQuerySecurityAttributesToken( HANDLE token, const UNICODE_STRING *attributes, ULONG count,
                                                void *buffer, ULONG length, ULONG *ret_length )
{
    static int once;
    if (!once++) FIXME( "%p %p %lu %p %lu %p: stub\n", token, attributes, count, buffer, length, ret_length );
    if (ret_length) *ret_length = 0;
    return STATUS_NOT_FOUND;
}

/******************************************************************************
 *  RtlGetAppContainerNamedObjectPath   [NTDLL.@]
 *
 * Packaged apps under wine-uwp run full trust, so the object directory is the
 * plain session BaseNamedObjects directory.
 */
NTSTATUS WINAPI RtlGetAppContainerNamedObjectPath( HANDLE token, PSID sid, BOOLEAN relative, UNICODE_STRING *path )
{
    static const WCHAR full[] = L"\\Sessions\\1\\BaseNamedObjects";
    static const WCHAR rel[] = L"BaseNamedObjects";

    TRACE( "%p %p %u %p\n", token, sid, relative, path );
    if (!path) return STATUS_INVALID_PARAMETER;
    return RtlCreateUnicodeString( path, relative ? rel : full ) ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

NTSTATUS WINAPI RtlCheckSandboxedToken( HANDLE token, BOOLEAN *sandboxed )
{
    TRACE( "%p %p\n", token, sandboxed );
    if (sandboxed) *sandboxed = FALSE;   /* wine-uwp hosts packages at full trust */
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI RtlReportException( EXCEPTION_RECORD *record, CONTEXT *context, ULONG flags )
{
    FIXME( "%p %p %#lx: stub\n", record, context, flags );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI RtlGetTokenNamedObjectPath( HANDLE token, PSID sid, UNICODE_STRING *path )
{
    TRACE( "%p %p %p\n", token, sid, path );
    if (!path) return STATUS_INVALID_PARAMETER;
    return RtlCreateUnicodeString( path, L"\\Sessions\\1\\BaseNamedObjects" ) ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

NTSTATUS WINAPI RtlAllocateWnfSerializationGroup( void **group )
{
    FIXME( "%p: stub\n", group );
    if (group) *group = (void *)(ULONG_PTR)0x574e4647;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI RtlUnsubscribeWnfNotificationWithCompletionCallback( void *subscription, void *callback, void *context )
{
    TRACE( "%p %p %p\n", subscription, callback, context );
    return STATUS_SUCCESS;
}

void WINAPI TpTrimPools( void )
{
}

void WINAPI RtlFlushHeaps( void )
{
}

NTSTATUS WINAPI RtlIsParentOfChildAppContainer( PSID parent, PSID child )
{
    FIXME( "%p %p: stub\n", parent, child );
    return STATUS_INVALID_PARAMETER;
}
