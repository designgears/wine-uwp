/*
 * Window messaging support
 *
 * Copyright 2001 Alexandre Julliard
 * Copyright 2008 Maarten Lankhorst
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

#include "ntstatus.h"
#include "user_private.h"
#include "controls.h"
#include "dde.h"
#include "appmodel.h"
#include "wine/debug.h"
#include "wine/exception.h"

WINE_DEFAULT_DEBUG_CHANNEL(msg);

#define MAX_ATOM_LEN  255

/* pack a pointer into a 32/64 portable format */
static inline ULONGLONG pack_ptr( const void *ptr )
{
    return (ULONG_PTR)ptr;
}

/* unpack a potentially 64-bit pointer, returning 0 when truncated */
static inline void *unpack_ptr( ULONGLONG ptr64 )
{
    if ((ULONG_PTR)ptr64 != ptr64) return 0;
    return (void *)(ULONG_PTR)ptr64;
}

static struct wm_char_mapping_data *get_wmchar_data(void)
{
    return (struct wm_char_mapping_data *)(UINT_PTR)NtUserGetThreadInfo()->wmchar_data;
}

/* check for pending WM_CHAR message with DBCS trailing byte */
static inline BOOL get_pending_wmchar( MSG *msg, UINT first, UINT last, BOOL remove )
{
    struct wm_char_mapping_data *data = get_wmchar_data();

    if (!data || !data->get_msg.message) return FALSE;
    if ((first || last) && (first > WM_CHAR || last < WM_CHAR)) return FALSE;
    if (!msg) return FALSE;
    *msg = data->get_msg;
    if (remove) data->get_msg.message = 0;
    return TRUE;
}


/***********************************************************************
 *           MessageWndProc
 *
 * Window procedure for "Message" windows (HWND_MESSAGE parent).
 */
LRESULT WINAPI MessageWndProc( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam )
{
    if (message == WM_NCCREATE) return TRUE;
    return 0;  /* all other messages are ignored */
}


DWORD get_input_codepage( void )
{
    DWORD cp;
    int ret;
    HKL hkl = NtUserGetKeyboardLayout( 0 );

    ret = GetLocaleInfoW( LOWORD(hkl), LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                          (WCHAR *)&cp, sizeof(cp) / sizeof(WCHAR) );
    if (!ret) cp = CP_ACP;
    return cp;
}

/***********************************************************************
 *		map_wparam_AtoW
 *
 * Convert the wparam of an ASCII message to Unicode.
 */
BOOL map_wparam_AtoW( UINT message, WPARAM *wparam, enum wm_char_mapping mapping )
{
    char ch[2];
    WCHAR wch[2];
    DWORD cp;

    wch[0] = wch[1] = 0;
    switch(message)
    {
    case WM_CHAR:
        /* WM_CHAR is magic: a DBCS char can be sent/posted as two consecutive WM_CHAR
         * messages, in which case the first char is stored, and the conversion
         * to Unicode only takes place once the second char is sent/posted.
         */
        if (mapping != WMCHAR_MAP_NOMAPPING)
        {
            struct wm_char_mapping_data *data = get_wmchar_data();
            BYTE low = LOBYTE(*wparam);
            cp = get_input_codepage();

            if (HIBYTE(*wparam))
            {
                ch[0] = low;
                ch[1] = HIBYTE(*wparam);
                MultiByteToWideChar( cp, 0, ch, 2, wch, 2 );
                TRACE( "map %02x,%02x -> %04x mapping %u\n", (BYTE)ch[0], (BYTE)ch[1], wch[0], mapping );
                if (data) data->lead_byte[mapping] = 0;
            }
            else if (data && data->lead_byte[mapping])
            {
                ch[0] = data->lead_byte[mapping];
                ch[1] = low;
                MultiByteToWideChar( cp, 0, ch, 2, wch, 2 );
                TRACE( "map stored %02x,%02x -> %04x mapping %u\n", (BYTE)ch[0], (BYTE)ch[1], wch[0], mapping );
                data->lead_byte[mapping] = 0;
            }
            else if (!IsDBCSLeadByte( low ))
            {
                ch[0] = low;
                MultiByteToWideChar( cp, 0, ch, 1, wch, 2 );
                TRACE( "map %02x -> %04x\n", (BYTE)ch[0], wch[0] );
                if (data) data->lead_byte[mapping] = 0;
            }
            else  /* store it and wait for trail byte */
            {
                if (!data)
                {
                    if (!(data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data) )))
                        return FALSE;
                    NtUserGetThreadInfo()->wmchar_data = (UINT_PTR)data;
                }
                TRACE( "storing lead byte %02x mapping %u\n", low, mapping );
                data->lead_byte[mapping] = low;
                return FALSE;
            }
            *wparam = MAKEWPARAM(wch[0], wch[1]);
            break;
        }
        /* else fall through */
    case WM_CHARTOITEM:
    case EM_SETPASSWORDCHAR:
    case WM_DEADCHAR:
    case WM_SYSCHAR:
    case WM_SYSDEADCHAR:
    case WM_MENUCHAR:
        cp = get_input_codepage();
        ch[0] = LOBYTE(*wparam);
        ch[1] = HIBYTE(*wparam);
        MultiByteToWideChar( cp, 0, ch, 2, wch, 2 );
        *wparam = MAKEWPARAM(wch[0], wch[1]);
        break;
    case WM_IME_CHAR:
        cp = get_input_codepage();
        ch[0] = HIBYTE(*wparam);
        ch[1] = LOBYTE(*wparam);
        if (ch[0]) MultiByteToWideChar( cp, 0, ch, 2, wch, 2 );
        else MultiByteToWideChar( cp, 0, ch + 1, 1, wch, 1 );
        *wparam = MAKEWPARAM(wch[0], HIWORD(*wparam));
        break;
    }
    return TRUE;
}


/***********************************************************************
 *		map_wparam_WtoA
 *
 * Convert the wparam of a Unicode message to ASCII.
 */
static void map_wparam_WtoA( MSG *msg, BOOL remove )
{
    BYTE ch[4] = { 0 };
    WCHAR wch[2];
    DWORD i, len;
    DWORD cp;

    switch(msg->message)
    {
    case WM_CHAR:
        if (!HIWORD(msg->wParam))
        {
            cp = get_input_codepage();
            wch[0] = LOWORD(msg->wParam);
            len = WideCharToMultiByte( cp, 0, wch, 1, (LPSTR)ch, 2, NULL, NULL );
            if (len == 2)  /* DBCS char */
            {
                struct wm_char_mapping_data *data = get_wmchar_data();
                if (!data)
                {
                    if (!(data = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*data) ))) return;
                    NtUserGetThreadInfo()->wmchar_data = (UINT_PTR)data;
                }
                if (remove)
                {
                    data->get_msg = *msg;
                    data->get_msg.wParam = ch[1];
                }
                msg->wParam = ch[0];
                return;
            }
        }
        /* else fall through */
    case WM_CHARTOITEM:
    case EM_SETPASSWORDCHAR:
    case WM_DEADCHAR:
    case WM_SYSCHAR:
    case WM_SYSDEADCHAR:
    case WM_MENUCHAR:
        cp = get_input_codepage();
        wch[0] = LOWORD(msg->wParam);
        wch[1] = HIWORD(msg->wParam);
        len = WideCharToMultiByte( cp, 0, wch, 2, (LPSTR)ch, 4, NULL, NULL );
        for (msg->wParam = i = 0; i < len; i++) msg->wParam |= ch[i] << (8 * i);
        break;
    case WM_IME_CHAR:
        cp = get_input_codepage();
        wch[0] = LOWORD(msg->wParam);
        len = WideCharToMultiByte( cp, 0, wch, 1, (LPSTR)ch, 2, NULL, NULL );
        if (len == 2)
            msg->wParam = MAKEWPARAM( (ch[0] << 8) | ch[1], HIWORD(msg->wParam) );
        else
            msg->wParam = MAKEWPARAM( ch[0], HIWORD(msg->wParam) );
        break;
    }
}


/* since the WM_DDE_ACK response to a WM_DDE_EXECUTE message should contain the handle
 * to the memory handle, we keep track (in the server side) of all pairs of handle
 * used (the client passes its value and the content of the memory handle), and
 * the server stored both values (the client, and the local one, created after the
 * content). When a ACK message is generated, the list of pair is searched for a
 * matching pair, so that the client memory handle can be returned.
 */
struct DDE_pair {
    HGLOBAL     client_hMem;
    HGLOBAL     server_hMem;
};

static      struct DDE_pair*    dde_pairs;
static      int                 dde_num_alloc;
static      int                 dde_num_used;

static CRITICAL_SECTION dde_crst;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &dde_crst,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": dde_crst") }
};
static CRITICAL_SECTION dde_crst = { &critsect_debug, -1, 0, 0, 0, 0 };

static BOOL dde_add_pair(HGLOBAL chm, HGLOBAL shm)
{
    int  i;
#define GROWBY  4

    EnterCriticalSection(&dde_crst);

    /* now remember the pair of hMem on both sides */
    if (dde_num_used == dde_num_alloc)
    {
        struct DDE_pair* tmp;
	if (dde_pairs)
	    tmp  = HeapReAlloc( GetProcessHeap(), 0, dde_pairs,
                                            (dde_num_alloc + GROWBY) * sizeof(struct DDE_pair));
	else
	    tmp  = HeapAlloc( GetProcessHeap(), 0, 
                                            (dde_num_alloc + GROWBY) * sizeof(struct DDE_pair));

        if (!tmp)
        {
            LeaveCriticalSection(&dde_crst);
            return FALSE;
        }
        dde_pairs = tmp;
        /* zero out newly allocated part */
        memset(&dde_pairs[dde_num_alloc], 0, GROWBY * sizeof(struct DDE_pair));
        dde_num_alloc += GROWBY;
    }
#undef GROWBY
    for (i = 0; i < dde_num_alloc; i++)
    {
        if (dde_pairs[i].server_hMem == 0)
        {
            dde_pairs[i].client_hMem = chm;
            dde_pairs[i].server_hMem = shm;
            dde_num_used++;
            break;
        }
    }
    LeaveCriticalSection(&dde_crst);
    return TRUE;
}

static HGLOBAL dde_get_pair(HGLOBAL shm)
{
    int  i;
    HGLOBAL     ret = 0;

    EnterCriticalSection(&dde_crst);
    for (i = 0; i < dde_num_alloc; i++)
    {
        if (dde_pairs[i].server_hMem == shm)
        {
            /* free this pair */
            dde_pairs[i].server_hMem = 0;
            dde_num_used--;
            ret = dde_pairs[i].client_hMem;
            break;
        }
    }
    LeaveCriticalSection(&dde_crst);
    return ret;
}

/***********************************************************************
 *		post_dde_message
 *
 * Post a DDE message
 */
NTSTATUS post_dde_message( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, DWORD dest_tid )
{
    struct post_dde_message_call_params params = { .dest_tid = dest_tid };
    UINT_PTR    uiLo, uiHi;
    LPARAM      lp;
    HGLOBAL     hunlock = 0;
    DWORD       res;
    ULONGLONG   hpack;

    if (!UnpackDDElParam( msg, lparam, &uiLo, &uiHi ))
        return STATUS_INVALID_PARAMETER;

    lp = lparam;
    switch (msg)
    {
        /* DDE messages which don't require packing are:
         * WM_DDE_INITIATE
         * WM_DDE_TERMINATE
         * WM_DDE_REQUEST
         * WM_DDE_UNADVISE
         */
    case WM_DDE_ACK:
        if (HIWORD(uiHi))
        {
            /* uiHi should contain a hMem from WM_DDE_EXECUTE */
            HGLOBAL h = dde_get_pair( (HANDLE)uiHi );
            if (h)
            {
                hpack = pack_ptr( h );
                /* send back the value of h on the other side */
                params.ptr = &hpack;
                params.size = sizeof(hpack);
                lp = uiLo;
                TRACE( "send dde-ack %Ix %08Ix => %p\n", uiLo, uiHi, h );
            }
        }
        else
        {
            /* uiHi should contain either an atom or 0 */
            TRACE( "send dde-ack %Ix atom=%Ix\n", uiLo, uiHi );
            lp = MAKELONG( uiLo, uiHi );
        }
        break;
    case WM_DDE_ADVISE:
    case WM_DDE_DATA:
    case WM_DDE_POKE:
        if (uiLo)
        {
            params.size = GlobalSize( (HGLOBAL)uiLo ) ;
            if ((msg == WM_DDE_ADVISE && params.size < sizeof(DDEADVISE)) ||
                (msg == WM_DDE_DATA   && params.size < FIELD_OFFSET(DDEDATA, Value)) ||
                (msg == WM_DDE_POKE   && params.size < FIELD_OFFSET(DDEPOKE, Value)))
                return STATUS_INVALID_PARAMETER;
        }
        else if (msg != WM_DDE_DATA) return STATUS_INVALID_PARAMETER;

        lp = uiHi;
        if (uiLo)
        {
            if ((params.ptr = GlobalLock( (HGLOBAL)uiLo) ))
            {
                DDEDATA *dde_data = params.ptr;
                TRACE("unused %d, fResponse %d, fRelease %d, fDeferUpd %d, fAckReq %d, cfFormat %d\n",
                       dde_data->unused, dde_data->fResponse, dde_data->fRelease,
                       dde_data->reserved, dde_data->fAckReq, dde_data->cfFormat);
                hunlock = (HGLOBAL)uiLo;
            }
        }
        TRACE( "send ddepack %u %Ix\n", params.size, uiHi );
        break;
    case WM_DDE_EXECUTE:
        if (lparam)
        {
            if ((params.ptr = GlobalLock( (HGLOBAL)lparam) ))
            {
                params.size = GlobalSize( (HGLOBAL)lparam );
                /* so that the other side can send it back on ACK */
                lp = lparam;
                hunlock = (HGLOBAL)lparam;
            }
        }
        break;
    }

    res = NtUserMessageCall( hwnd, msg, wparam, lp, &params, NtUserPostDdeCall, FALSE );
    if (!res) FreeDDElParam( msg, lparam );
    if (hunlock) GlobalUnlock(hunlock);

    return res;
}

/***********************************************************************
 *		unpack_dde_message
 *
 * Unpack a posted DDE message received from another process.
 */
BOOL unpack_dde_message( HWND hwnd, UINT message, WPARAM *wparam, LPARAM *lparam,
                         const void *buffer, size_t size )
{
    UINT_PTR	uiLo, uiHi;
    HGLOBAL	hMem = 0;
    void*	ptr;

    switch (message)
    {
    case WM_DDE_ACK:
        if (size)
        {
            ULONGLONG hpack;
            /* hMem is being passed */
            if (size != sizeof(hpack)) return FALSE;
            uiLo = *lparam;
            memcpy( &hpack, buffer, size );
            hMem = unpack_ptr( hpack );
            uiHi = (UINT_PTR)hMem;
            TRACE("recv dde-ack %Ix mem=%Ix[%Ix]\n", uiLo, uiHi, GlobalSize( hMem ));
        }
        else
        {
            uiLo = LOWORD( *lparam );
            uiHi = HIWORD( *lparam );
            TRACE("recv dde-ack %Ix atom=%Ix\n", uiLo, uiHi);
        }
	*lparam = PackDDElParam( WM_DDE_ACK, uiLo, uiHi );
	break;
    case WM_DDE_ADVISE:
    case WM_DDE_DATA:
    case WM_DDE_POKE:
	if (!size && message != WM_DDE_DATA) return FALSE;
	uiHi = *lparam;
        if (size)
        {
            if (!(hMem = GlobalAlloc( GMEM_MOVEABLE|GMEM_DDESHARE, size )))
                return FALSE;
            if ((ptr = GlobalLock( hMem )))
            {
                memcpy( ptr, buffer, size );
                GlobalUnlock( hMem );
            }
            else
            {
                GlobalFree( hMem );
                return FALSE;
            }
        }
        uiLo = (UINT_PTR)hMem;

	*lparam = PackDDElParam( message, uiLo, uiHi );
	break;
    case WM_DDE_EXECUTE:
	if (size)
	{
            if (!(hMem = GlobalAlloc( GMEM_MOVEABLE|GMEM_DDESHARE, size ))) return FALSE;
            if ((ptr = GlobalLock( hMem )))
	    {
		memcpy( ptr, buffer, size );
		GlobalUnlock( hMem );
                TRACE( "exec: pairing c=%08Ix s=%p\n", *lparam, hMem );
                if (!dde_add_pair( (HGLOBAL)*lparam, hMem ))
                {
                    GlobalFree( hMem );
                    return FALSE;
                }
            }
            else
            {
                GlobalFree( hMem );
                return FALSE;
            }
	} else return FALSE;
        *lparam = (LPARAM)hMem;
        break;
    }
    return TRUE;
}

/***********************************************************************
 *		SendMessageTimeoutW  (USER32.@)
 */
LRESULT WINAPI SendMessageTimeoutW( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                    UINT flags, UINT timeout, PDWORD_PTR res_ptr )
{
    struct send_message_timeout_params params = { .flags = flags, .timeout = timeout };
    LRESULT res;

    res = NtUserMessageCall( hwnd, msg, wparam, lparam, &params, NtUserSendMessageTimeout, FALSE );
    if (res_ptr) *res_ptr = res;
    return params.result;
}

/***********************************************************************
 *		SendMessageTimeoutA  (USER32.@)
 */
LRESULT WINAPI SendMessageTimeoutA( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                    UINT flags, UINT timeout, PDWORD_PTR res_ptr )
{
    struct send_message_timeout_params params = { .flags = flags, .timeout = timeout };
    LRESULT res = 0;

    if (msg != WM_CHAR || WIN_IsCurrentThread( hwnd ))
    {
        res = NtUserMessageCall( hwnd, msg, wparam, lparam, &params,
                                 NtUserSendMessageTimeout, TRUE );
    }
    else if (map_wparam_AtoW( msg, &wparam, WMCHAR_MAP_SENDMESSAGE ))
    {
        res = NtUserMessageCall( hwnd, msg, wparam, lparam, &params,
                                 NtUserSendMessageTimeout, FALSE );
    }

    if (res_ptr) *res_ptr = res;
    return params.result;
}


static LRESULT dispatch_send_message( struct win_proc_params *params, WPARAM wparam, LPARAM lparam )
{
    struct ntuser_thread_info *thread_info = NtUserGetThreadInfo();
    INPUT_MESSAGE_SOURCE prev_source = thread_info->msg_source;
    LRESULT retval = 0;

    static const INPUT_MESSAGE_SOURCE msg_source_unavailable = { IMDT_UNAVAILABLE, IMO_UNAVAILABLE };

    /* params may contain arguments modified by wow, use original parameters instead */
    params->wparam = wparam;
    params->lparam = lparam;

    thread_info->recursion_count++;

    thread_info->msg_source = msg_source_unavailable;
    SPY_EnterMessage( SPY_SENDMESSAGE, params->hwnd, params->msg, params->wparam, params->lparam );

    retval = dispatch_win_proc_params( params );

    SPY_ExitMessage( SPY_RESULT_OK, params->hwnd, params->msg, retval, params->wparam, params->lparam );
    thread_info->msg_source = prev_source;
    thread_info->recursion_count--;
    return retval;
}


/***********************************************************************
 *		SendMessageW  (USER32.@)
 */
LRESULT WINAPI SendMessageW( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct win_proc_params params;
    LRESULT retval;

    params.hwnd = 0;
    retval = NtUserMessageCall( hwnd, msg, wparam, lparam, &params, NtUserSendMessage, FALSE );
    if (params.hwnd) retval = dispatch_send_message( &params, wparam, lparam );
    return retval;
}


/***********************************************************************
 *		SendMessageA  (USER32.@)
 */
LRESULT WINAPI SendMessageA( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    struct win_proc_params params;
    LRESULT retval;

    if (msg == WM_CHAR && !WIN_IsCurrentThread( hwnd ))
    {
        if (!map_wparam_AtoW( msg, &wparam, WMCHAR_MAP_SENDMESSAGE ))
            return 0;
        return NtUserMessageCall( hwnd, msg, wparam, lparam, NULL, NtUserSendMessage, FALSE );
    }

    params.hwnd = 0;
    retval = NtUserMessageCall( hwnd, msg, wparam, lparam, &params, NtUserSendMessage, TRUE );
    if (params.hwnd) retval = dispatch_send_message( &params, wparam, lparam );
    return retval;
}


/***********************************************************************
 *		SendNotifyMessageA  (USER32.@)
 */
BOOL WINAPI SendNotifyMessageA( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (!WIN_IsCurrentThread( hwnd ) && !map_wparam_AtoW( msg, &wparam, WMCHAR_MAP_SENDMESSAGE ))
        return FALSE;

    return NtUserMessageCall( hwnd, msg, wparam, lparam, 0, NtUserSendNotifyMessage, TRUE );
}


/***********************************************************************
 *		SendNotifyMessageW  (USER32.@)
 */
BOOL WINAPI SendNotifyMessageW( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    return NtUserMessageCall( hwnd, msg, wparam, lparam, 0, NtUserSendNotifyMessage, FALSE );
}


/***********************************************************************
 *		SendMessageCallbackA  (USER32.@)
 */
BOOL WINAPI SendMessageCallbackA( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                  SENDASYNCPROC callback, ULONG_PTR data )
{
    struct send_message_callback_params params = { .callback = callback, .data = data };

    if (!WIN_IsCurrentThread( hwnd ) && !map_wparam_AtoW( msg, &wparam, WMCHAR_MAP_SENDMESSAGE ))
        return FALSE;

    return NtUserMessageCall( hwnd, msg, wparam, lparam, &params, NtUserSendMessageCallback, TRUE );
}


/***********************************************************************
 *		SendMessageCallbackW  (USER32.@)
 */
BOOL WINAPI SendMessageCallbackW( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                  SENDASYNCPROC callback, ULONG_PTR data )
{
    struct send_message_callback_params params = { .callback = callback, .data = data };
    return NtUserMessageCall( hwnd, msg, wparam, lparam, &params, NtUserSendMessageCallback, FALSE );
}


/***********************************************************************
 *		InSendMessage  (USER32.@)
 */
BOOL WINAPI InSendMessage(void)
{
    return (InSendMessageEx( NULL ) & (ISMEX_SEND | ISMEX_NOTIFY | ISMEX_CALLBACK)) != 0;
}


/***********************************************************************
 *		InSendMessageEx  (USER32.@)
 */
DWORD WINAPI InSendMessageEx( LPVOID reserved )
{
    return NtUserGetThreadState( UserThreadStateInSendMessage );
}


/***********************************************************************
 *		PostMessageA  (USER32.@)
 */
BOOL WINAPI PostMessageA( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (!map_wparam_AtoW( msg, &wparam, WMCHAR_MAP_POSTMESSAGE )) return TRUE;
    return PostMessageW( hwnd, msg, wparam, lparam );
}


/***********************************************************************
 *		PostMessageW  (USER32.@)
 */
BOOL WINAPI PostMessageW( HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam )
{
    return NtUserPostMessage( hwnd, msg, wparam, lparam );
}


/**********************************************************************
 *		PostThreadMessageA  (USER32.@)
 */
BOOL WINAPI PostThreadMessageA( DWORD thread, UINT msg, WPARAM wparam, LPARAM lparam )
{
    if (!map_wparam_AtoW( msg, &wparam, WMCHAR_MAP_POSTMESSAGE )) return TRUE;
    return NtUserPostThreadMessage( thread, msg, wparam, lparam );
}


/***********************************************************************
 *		PeekMessageW  (USER32.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH PeekMessageW( MSG *msg_out, HWND hwnd, UINT first, UINT last, UINT flags )
{
    return NtUserPeekMessage( msg_out, hwnd, first, last, flags );
}


/***********************************************************************
 *		PeekMessageA  (USER32.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH PeekMessageA( MSG *msg, HWND hwnd, UINT first, UINT last, UINT flags )
{
    if (get_pending_wmchar( msg, first, last, (flags & PM_REMOVE) )) return TRUE;
    if (!PeekMessageW( msg, hwnd, first, last, flags )) return FALSE;
    map_wparam_WtoA( msg, (flags & PM_REMOVE) );
    return TRUE;
}


/***********************************************************************
 *		GetMessageW  (USER32.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetMessageW( MSG *msg, HWND hwnd, UINT first, UINT last )
{
    return NtUserGetMessage( msg, hwnd, first, last );
}


/***********************************************************************
 *		GetMessageA  (USER32.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetMessageA( MSG *msg, HWND hwnd, UINT first, UINT last )
{
    if (get_pending_wmchar( msg, first, last, TRUE )) return TRUE;
    if (GetMessageW( msg, hwnd, first, last ) < 0) return -1;
    map_wparam_WtoA( msg, TRUE );
    return (msg->message != WM_QUIT);
}

static BOOL is_cjk(void)
{
    int lang_id = PRIMARYLANGID(GetUserDefaultLangID());

    if (lang_id == LANG_CHINESE || lang_id == LANG_JAPANESE || lang_id == LANG_KOREAN)
        return TRUE;
    return FALSE;
}

/***********************************************************************
 *		IsDialogMessageA (USER32.@)
 *		IsDialogMessage  (USER32.@)
 */
BOOL WINAPI IsDialogMessageA( HWND hwndDlg, LPMSG pmsg )
{
    enum wm_char_mapping mapping;
    MSG msg = *pmsg;

    mapping = is_cjk() ? WMCHAR_MAP_ISDIALOGMESSAGE : WMCHAR_MAP_NOMAPPING;
    if (!map_wparam_AtoW( msg.message, &msg.wParam, mapping ))
        return TRUE;
    return IsDialogMessageW( hwndDlg, &msg );
}


/***********************************************************************
 *		TranslateMessage (USER32.@)
 *
 * Implementation of TranslateMessage.
 *
 * TranslateMessage translates virtual-key messages into character-messages,
 * as follows :
 * WM_KEYDOWN/WM_KEYUP combinations produce a WM_CHAR or WM_DEADCHAR message.
 * ditto replacing WM_* with WM_SYS*
 * This produces WM_CHAR messages only for keys mapped to ASCII characters
 * by the keyboard driver.
 *
 * If the message is WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN, or WM_SYSKEYUP, the
 * return value is nonzero, regardless of the translation.
 *
 */
BOOL WINAPI TranslateMessage( const MSG *msg )
{
    return NtUserTranslateMessage( msg, 0 );
}


static LRESULT dispatch_message( const MSG *msg, BOOL ansi )
{
    struct win_proc_params params;
    LRESULT retval = 0;

    if (!NtUserMessageCall( msg->hwnd, msg->message, msg->wParam, msg->lParam,
                            &params, NtUserGetDispatchParams, ansi )) return 0;

    SPY_EnterMessage( SPY_DISPATCHMESSAGE, msg->hwnd, msg->message, msg->wParam, msg->lParam );
    retval = dispatch_win_proc_params( &params );
    SPY_ExitMessage( SPY_RESULT_OK, msg->hwnd, msg->message, retval, msg->wParam, msg->lParam );
    return retval;
}


/***********************************************************************
 *		DispatchMessageA (USER32.@)
 *
 * See DispatchMessageW.
 */
LRESULT WINAPI DECLSPEC_HOTPATCH DispatchMessageA( const MSG* msg )
{
    LRESULT retval;

      /* Process timer messages */
    if (msg->lParam && msg->message == WM_TIMER)
    {
        __TRY
        {
            retval = CallWindowProcA( (WNDPROC)msg->lParam, msg->hwnd,
                                      msg->message, msg->wParam, GetTickCount() );
        }
        __EXCEPT_ALL
        {
            retval = 0;
        }
        __ENDTRY
        return retval;
    }

    /* whenever possible, avoid using NtUserDispatchMessage to make the call unwindable */
    if (msg->message != WM_SYSTIMER && msg->message != WM_PAINT)
        return dispatch_message( msg, TRUE );

    return NtUserDispatchMessage( msg );
}


/***********************************************************************
 *		DispatchMessageW (USER32.@) Process a message
 *
 * Process the message specified in the structure *_msg_.
 *
 * If the lpMsg parameter points to a WM_TIMER message and the
 * parameter of the WM_TIMER message is not NULL, the lParam parameter
 * points to the function that is called instead of the window
 * procedure. The function stored in lParam (timer callback) is protected
 * from causing page-faults.
 *
 * The message must be valid.
 *
 * RETURNS
 *
 *   DispatchMessage() returns the result of the window procedure invoked.
 *
 * CONFORMANCE
 *
 *   ECMA-234, Win32
 *
 */
LRESULT WINAPI DECLSPEC_HOTPATCH DispatchMessageW( const MSG* msg )
{
    LRESULT retval;

      /* Process timer messages */
    if ((msg->message == WM_TIMER) || (msg->message == WM_SYSTIMER))
    {
        if (msg->lParam)
        {
            __TRY
            {
                retval = CallWindowProcW( (WNDPROC)msg->lParam, msg->hwnd,
                                          msg->message, msg->wParam, GetTickCount() );
            }
            __EXCEPT_ALL
            {
                retval = 0;
            }
            __ENDTRY
            return retval;
        }
    }

    /* whenever possible, avoid using NtUserDispatchMessage to make the call unwindable */
    if (msg->message != WM_SYSTIMER && msg->message != WM_PAINT)
        return dispatch_message( msg, FALSE );

    return NtUserDispatchMessage( msg );
}


/***********************************************************************
 *		GetMessageTime (USER.120)
 *		GetMessageTime (USER32.@)
 *
 * GetMessageTime() returns the message time for the last message
 * retrieved by the function. The time is measured in milliseconds with
 * the same offset as GetTickCount().
 *
 * Since the tick count wraps, this is only useful for moderately short
 * relative time comparisons.
 *
 * RETURNS
 *
 * Time of last message on success, zero on failure.
 */
LONG WINAPI GetMessageTime(void)
{
    return NtUserGetThreadState( UserThreadStateMessageTime );
}


/***********************************************************************
 *		GetMessageExtraInfo (USER.288)
 *		GetMessageExtraInfo (USER32.@)
 */
LPARAM WINAPI GetMessageExtraInfo(void)
{
    return NtUserGetThreadState( UserThreadStateExtraInfo );
}


/***********************************************************************
 *		MsgWaitForMultipleObjects (USER32.@)
 */
DWORD WINAPI MsgWaitForMultipleObjects( DWORD count, const HANDLE *handles,
                                        BOOL wait_all, DWORD timeout, DWORD mask )
{
    return NtUserMsgWaitForMultipleObjectsEx( count, handles, timeout, mask,
                                              wait_all ? MWMO_WAITALL : 0 );
}


/***********************************************************************
 *		WaitForInputIdle (USER32.@)
 */
DWORD WINAPI WaitForInputIdle( HANDLE process, DWORD timeout )
{
    return NtUserWaitForInputIdle( process, timeout, FALSE );
}


/***********************************************************************
 *		RegisterWindowMessageA (USER32.@)
 *		RegisterWindowMessage (USER.118)
 */
UINT WINAPI RegisterWindowMessageA( LPCSTR name )
{
    WCHAR buf[MAX_ATOM_LEN + 1];
    UNICODE_STRING str = {.Buffer = buf, .MaximumLength = sizeof(buf)};
    STRING ansi;

    TRACE( "%s\n", debugstr_a(name) );

    RtlInitAnsiString( &ansi, name );
    RtlAnsiStringToUnicodeString( &str, &ansi, FALSE );
    return NtUserRegisterWindowMessage( &str );
}


/***********************************************************************
 *		RegisterWindowMessageW (USER32.@)
 */
UINT WINAPI RegisterWindowMessageW( LPCWSTR name )
{
    UNICODE_STRING str;

    TRACE( "%s\n", debugstr_w(name) );

    RtlInitUnicodeString( &str, name );
    return NtUserRegisterWindowMessage( &str );
}


typedef struct BroadcastParm
{
    DWORD flags;
    LPDWORD recipients;
    UINT msg;
    WPARAM wp;
    LPARAM lp;
    BOOL success;
    HWINSTA winsta;
} BroadcastParm;

static BOOL CALLBACK bcast_childwindow( HWND hw, LPARAM lp )
{
    BroadcastParm *parm = (BroadcastParm*)lp;
    DWORD_PTR retval = 0;
    LRESULT lresult;

    if (parm->flags & BSF_IGNORECURRENTTASK && WIN_IsCurrentProcess(hw))
    {
        TRACE("Not telling myself %p\n", hw);
        return TRUE;
    }

    /* I don't know 100% for sure if this is what Windows does, but it fits the tests */
    if (parm->flags & BSF_QUERY)
    {
        TRACE("Telling window %p using SendMessageTimeout\n", hw);

        /* Not tested for conflicting flags */
        if (parm->flags & BSF_FORCEIFHUNG || parm->flags & BSF_NOHANG)
            lresult = SendMessageTimeoutW( hw, parm->msg, parm->wp, parm->lp, SMTO_ABORTIFHUNG, 2000, &retval );
        else if (parm->flags & BSF_NOTIMEOUTIFNOTHUNG)
            lresult = SendMessageTimeoutW( hw, parm->msg, parm->wp, parm->lp, SMTO_NOTIMEOUTIFNOTHUNG, 2000, &retval );
        else
            lresult = SendMessageTimeoutW( hw, parm->msg, parm->wp, parm->lp, SMTO_NORMAL, 2000, &retval );

        if (!lresult && GetLastError() == ERROR_TIMEOUT)
        {
            WARN("Timed out!\n");
            if (!(parm->flags & BSF_FORCEIFHUNG))
                goto fail;
        }
        if (retval == BROADCAST_QUERY_DENY)
            goto fail;

        return TRUE;

fail:
        parm->success = FALSE;
        return FALSE;
    }
    else if (parm->flags & BSF_POSTMESSAGE)
    {
        TRACE("Telling window %p using PostMessage\n", hw);
        PostMessageW( hw, parm->msg, parm->wp, parm->lp );
    }
    else
    {
        TRACE("Telling window %p using SendNotifyMessage\n", hw);
        SendNotifyMessageW( hw, parm->msg, parm->wp, parm->lp );
    }

    return TRUE;
}

static BOOL CALLBACK bcast_desktop( LPWSTR desktop, LPARAM lp )
{
    BOOL ret;
    HDESK hdesktop;
    BroadcastParm *parm = (BroadcastParm*)lp;

    TRACE("desktop: %s\n", debugstr_w( desktop ));

    hdesktop = open_winstation_desktop( parm->winsta, desktop, 0, FALSE, DESKTOP_ENUMERATE|DESKTOP_WRITEOBJECTS|STANDARD_RIGHTS_WRITE );
    if (!hdesktop)
    {
        FIXME("Could not open desktop %s\n", debugstr_w(desktop));
        return TRUE;
    }

    ret = EnumDesktopWindows( hdesktop, bcast_childwindow, lp );
    NtUserCloseDesktop( hdesktop );
    TRACE("-->%d\n", ret);
    return parm->success;
}

static BOOL CALLBACK bcast_winsta( LPWSTR winsta, LPARAM lp )
{
    BOOL ret;
    HWINSTA hwinsta = OpenWindowStationW( winsta, FALSE, WINSTA_ENUMDESKTOPS );
    TRACE("hwinsta: %p/%s/%08lx\n", hwinsta, debugstr_w( winsta ), GetLastError());
    if (!hwinsta)
        return TRUE;
    ((BroadcastParm *)lp)->winsta = hwinsta;
    ret = EnumDesktopsW( hwinsta, bcast_desktop, lp );
    NtUserCloseWindowStation( hwinsta );
    TRACE("-->%d\n", ret);
    return ret;
}

/***********************************************************************
 *		BroadcastSystemMessageA (USER32.@)
 *		BroadcastSystemMessage  (USER32.@)
 */
LONG WINAPI BroadcastSystemMessageA( DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp )
{
    return BroadcastSystemMessageExA( flags, recipients, msg, wp, lp, NULL );
}


/***********************************************************************
 *		BroadcastSystemMessageW (USER32.@)
 */
LONG WINAPI BroadcastSystemMessageW( DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp )
{
    return BroadcastSystemMessageExW( flags, recipients, msg, wp, lp, NULL );
}

/***********************************************************************
 *              BroadcastSystemMessageExA (USER32.@)
 */
LONG WINAPI BroadcastSystemMessageExA( DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp, PBSMINFO pinfo )
{
    map_wparam_AtoW( msg, &wp, WMCHAR_MAP_NOMAPPING );
    return BroadcastSystemMessageExW( flags, recipients, msg, wp, lp, pinfo );
}


/***********************************************************************
 *              BroadcastSystemMessageExW (USER32.@)
 */
LONG WINAPI BroadcastSystemMessageExW( DWORD flags, LPDWORD recipients, UINT msg, WPARAM wp, LPARAM lp, PBSMINFO pinfo )
{
    BroadcastParm parm;
    DWORD recips = BSM_ALLCOMPONENTS;
    BOOL ret = TRUE;
    static const DWORD all_flags = ( BSF_QUERY | BSF_IGNORECURRENTTASK | BSF_FLUSHDISK | BSF_NOHANG
                                   | BSF_POSTMESSAGE | BSF_FORCEIFHUNG | BSF_NOTIMEOUTIFNOTHUNG
                                   | BSF_ALLOWSFW | BSF_SENDNOTIFYMESSAGE | BSF_RETURNHDESK | BSF_LUID );

    TRACE("Flags: %08lx, recipients: %p(0x%lx), msg: %04x, wparam: %08Ix, lparam: %08Ix\n", flags, recipients,
         (recipients ? *recipients : recips), msg, wp, lp);

    if (flags & ~all_flags)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    if (!recipients)
        recipients = &recips;

    if ( pinfo && flags & BSF_QUERY )
        FIXME("Not returning PBSMINFO information yet\n");

    parm.flags = flags;
    parm.recipients = recipients;
    parm.msg = msg;
    parm.wp = wp;
    parm.lp = lp;
    parm.success = TRUE;

    if (*recipients & BSM_ALLDESKTOPS || *recipients == BSM_ALLCOMPONENTS)
        ret = EnumWindowStationsW(bcast_winsta, (LONG_PTR)&parm);
    else if (*recipients & BSM_APPLICATIONS)
    {
        EnumWindows(bcast_childwindow, (LONG_PTR)&parm);
        ret = parm.success;
    }
    else
        FIXME("Recipients %08lx not supported!\n", *recipients);

    return ret;
}

/***********************************************************************
 *		SetMessageQueue (USER32.@)
 */
BOOL WINAPI SetMessageQueue( INT size )
{
    /* now obsolete the message queue will be expanded dynamically as necessary */
    return TRUE;
}


/******************************************************************
 *      SetTimer (USER32.@)
 */
UINT_PTR WINAPI SetTimer( HWND hwnd, UINT_PTR id, UINT timeout, TIMERPROC proc )
{
    return NtUserSetTimer( hwnd, id, timeout, proc, TIMERV_DEFAULT_COALESCING );
}


/******************************************************************
 *      SetSystemTimer (USER32.@)
 */
UINT_PTR WINAPI SetSystemTimer( HWND hwnd, UINT_PTR id, UINT timeout, void *unknown )
{
    if (unknown) FIXME( "ignoring unknown parameter %p\n", unknown );

    return NtUserSetSystemTimer( hwnd, id, timeout );
}


/**********************************************************************
 *		IsGUIThread  (USER32.@)
 */
BOOL WINAPI IsGUIThread( BOOL convert )
{
    FIXME( "%u: stub\n", convert );
    return TRUE;
}


/******************************************************************
 *		IsHungAppWindow (USER32.@)
 *
 */
BOOL WINAPI IsHungAppWindow( HWND hWnd )
{
    return HandleToUlong( NtUserQueryWindow( hWnd, WindowIsHung ));
}

/******************************************************************
 *      ChangeWindowMessageFilter (USER32.@)
 */
BOOL WINAPI ChangeWindowMessageFilter( UINT message, DWORD flag )
{
    FIXME( "%x %08lx\n", message, flag );
    return TRUE;
}

/******************************************************************
 *      ChangeWindowMessageFilterEx (USER32.@)
 */
BOOL WINAPI ChangeWindowMessageFilterEx( HWND hwnd, UINT message, DWORD action, CHANGEFILTERSTRUCT *changefilter )
{
    FIXME( "%p %x %ld %p\n", hwnd, message, action, changefilter );
    return TRUE;
}

/***********************************************************************
 *      GetQueueStatusForCoreMessaging (USER32.@)
 *
 * Private CoreMessaging entry point (ext-ms-win-rtcore-minuser-private-ext). Argument use unknown;
 * report the queue status like GetQueueStatus does.
 */
DWORD WINAPI GetQueueStatusForCoreMessaging( UINT flags )
{
    FIXME( "%#x: semi-stub\n", flags );
    return NtUserGetQueueStatus( QS_ALLINPUT );
}

/***********************************************************************
 *      ScheduleCoreMessagingDispatch (USER32.@)
 *
 * Private CoreMessaging entry point. Asks the thread's queue to schedule a dispatch of the given
 * kind; CoreMessaging then drains it from its event loop. Waking the queue is enough here.
 */
/* Debugging aid: arm a hardware write watchpoint on WINE_UWP_WATCH for this thread. The UWP view
 * thread reaches here early, before XAML builds its frames, so a stack slot corrupted later reports
 * the writing instruction as a single-step exception in the +seh log. */
static ULONG_PTR uwp_watch_addr;

static LONG CALLBACK uwp_watch_handler( EXCEPTION_POINTERS *info )
{
    if (info->ExceptionRecord->ExceptionCode == STATUS_SINGLE_STEP)
    {
        CONTEXT *ctx = info->ContextRecord;
        ERR( "hit %p rsp %p slot=%p rdi %p rax %p\n", (void *)ctx->Rip, (void *)ctx->Rsp,
             uwp_watch_addr ? *(void **)uwp_watch_addr : NULL, (void *)ctx->Rdi, (void *)ctx->Rax );
        ctx->Dr6 = 0;
        ctx->EFlags |= 0x10000;   /* resume flag: do not re-trigger on this instruction */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void arm_uwp_watchpoint(void)
{
    static LONG done;
    WCHAR value[128], *p, *next;
    CONTEXT ctx;
    HMODULE module;
    unsigned int n = 0;

    if (InterlockedExchange( &done, 1 )) return;
    if (GetEnvironmentVariableW( L"WINE_UWP_WATCH", value, ARRAY_SIZE(value) ))
    {
        ULONG_PTR addr = wcstoull( value, NULL, 16 );

        if (!addr) return;
        uwp_watch_addr = addr;
        AddVectoredExceptionHandler( 1, uwp_watch_handler );
        memset( &ctx, 0, sizeof(ctx) );
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        ctx.Dr0 = addr;
        ctx.Dr7 = 1 | (1 << 16) | (2 << 18);   /* L0, write, 8 bytes */
        if (!SetThreadContext( GetCurrentThread(), &ctx )) ERR( "could not arm watchpoint\n" );
        else ERR( "watching writes to %#Ix\n", addr );
        return;
    }
    if (!GetEnvironmentVariableW( L"WINE_UWP_BREAK", value, ARRAY_SIZE(value) )) return;
    if (!(module = GetModuleHandleW( L"windows.ui.xaml.dll" ))) return;
    AddVectoredExceptionHandler( 1, uwp_watch_handler );

    memset( &ctx, 0, sizeof(ctx) );
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    for (p = value; *p && n < 4; p = next)
    {
        ULONG_PTR addr = (ULONG_PTR)module + wcstoull( p, &next, 16 );
        if (next == p) break;
        while (*next == ',') next++;
        switch (n)
        {
        case 0: ctx.Dr0 = addr; break;
        case 1: ctx.Dr1 = addr; break;
        case 2: ctx.Dr2 = addr; break;
        case 3: ctx.Dr3 = addr; break;
        }
        ctx.Dr7 |= 1u << (2 * n);   /* local enable, execute, one byte */
        ERR( "breakpoint %u at %#Ix\n", n, addr );
        n++;
    }
    if (!n) return;
    if (!SetThreadContext( GetCurrentThread(), &ctx )) ERR( "could not arm breakpoints\n" );
}

UINT WINAPI ScheduleCoreMessagingDispatch( HWND hwnd, DWORD priority, WPARAM wparam, LPARAM lparam, UINT *error )
{
    UINT ret;

    TRACE( "%p %lu %#Ix %#Ix %p\n", hwnd, priority, wparam, lparam, error );
    arm_uwp_watchpoint();
    /* CoreMessaging's UserAdapter falls back to exactly these when the export is missing:
     * 0  AsyncSendMessage, SendMessageCallbackW( hwnd, 0x60, wparam, lparam );
     * 1  AsyncPostMessage, PostMessageW( hwnd, 0x60, wparam, lparam );
     * 2  ScheduleInputPriorityUserDispatch, which reads the result as ScheduleDispatchNotification's:
     *    1 makes it send itself 0x60/7 and ask again, 2 makes it arm its dispatch timer. Answering 1
     *    (a plain TRUE) therefore spun 0x60/7 and 0x60/0 at tens of thousands a second; like
     *    NtUserScheduleDispatchNotification, answer 2;
     * 3  its backup timer, SetTimer( hwnd, wparam, lparam ).
     * The window procedure runs UserAdapter::DoWork for each. ERROR_INVALID_WINDOW_HANDLE is the one
     * failure the caller tolerates (the adapter is going away). */
    switch (priority)
    {
    case 0: ret = SendMessageCallbackW( hwnd, 0x60, wparam, lparam, NULL, 1 ); break;
    case 2: ret = IsWindow( hwnd ) ? 2 : 0; if (!ret) SetLastError( ERROR_INVALID_WINDOW_HANDLE ); break;
    case 3: ret = SetTimer( hwnd, wparam, lparam, NULL ) != 0; break;
    default: ret = PostMessageW( hwnd, 0x60, wparam, lparam ); break;
    }
    if (error) *error = ret ? 0 : GetLastError();
    return ret;
}

/***********************************************************************
 *      IsOneCoreTransformMode (USER32.@)
 *
 * Private, reached through api-ms-win-rtcore-ntuser-private. Desktop Win32 reports FALSE.
 * Packaged UWP apps run CoreWindow through composition islands; XamlOneCoreTransforms caches
 * this bit and, when it is FALSE, DCompTreeHost::EnsureDCompDevice never calls
 * CreateVisualTreeCompositionIslandAdapter and CJupiterWindow::SetCoreWindow never calls
 * EnsureCompositionIslandCreated, so no composition target is created.
 */
BOOL WINAPI IsOneCoreTransformMode( void )
{
    static int once, cached = -1;
    WCHAR pkg[8];

    /* Off by default even for packaged apps: XAML's OneCore mode wants the CoreWindow at DXamlCore
     * init, before the view is activated, and it also keeps XAML from subclassing the CoreWindow for
     * WM_POINTER input (CJupiterWindow::UseCoreWindowSubclassedEvents). WINE_UWP_ONECORE=1 opts in. */
    if (cached < 0)
        cached = GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FULL_NAME", pkg, ARRAY_SIZE(pkg) ) > 0 &&
                 GetEnvironmentVariableW( L"WINE_UWP_ONECORE", pkg, ARRAY_SIZE(pkg) ) > 0;
    if (!once++) FIXME( "returning %d\n", cached );
    return cached;
}

/***********************************************************************
 *      GetWindowCompositionAttribute (USER32.@)
 */
BOOL WINAPI GetWindowCompositionAttribute( HWND hwnd, void *data )
{
    static int once;
    if (!once++) FIXME( "%p %p: stub\n", hwnd, data );
    SetLastError( ERROR_NOT_SUPPORTED );
    return FALSE;
}

/***********************************************************************
 *      SetClassScope (USER32.@)
 */
BOOL WINAPI SetClassScope( HWND hwnd, DWORD scope )
{
    static int once;
    if (!once++) FIXME( "%p %#lx: stub\n", hwnd, scope );
    return TRUE;
}

/***********************************************************************
 *      EvaluateProximityToPolygon (USER32.@)
 */
BOOL WINAPI EvaluateProximityToPolygon( UINT32 count, const POINT *points, const TOUCH_HIT_TESTING_INPUT *hit_test, TOUCH_HIT_TESTING_PROXIMITY_EVALUATION *eval )
{
    static int once;
    if (!once++) FIXME( "%u %p %p %p: stub\n", count, points, hit_test, eval );
    return FALSE;
}

/***********************************************************************
 *      AddVisualIdentifier (USER32.@)
 *
 * Associates a composition island's visual identifier with a window.  Wine has no
 * cross-process visual tree, so the association is a no-op that must still succeed.
 */
BOOL WINAPI AddVisualIdentifier( HWND hwnd, const void *id )
{
    static int once;
    if (!once++) FIXME( "%p %p: stub\n", hwnd, id );
    return TRUE;
}

/***********************************************************************
 *      MapPointsByVisualIdentifier (USER32.@)
 */
BOOL WINAPI MapPointsByVisualIdentifier( const void *from, const void *to, POINT *points, UINT count )
{
    static int once;
    if (!once++) FIXME( "%p %p %p %u: stub\n", from, to, points, count );
    return FALSE;
}

BOOL WINAPI CallModalExperience( DWORD kind, void *data )
{
    FIXME( "%#lx %p: stub\n", kind, data );
    return FALSE;
}

BOOL WINAPI EnableOneCoreTransformMode( BOOL enable )
{
    FIXME( "%u: stub\n", enable );
    return FALSE;
}

BOOL WINAPI GetWindowBand( HWND hwnd, DWORD *band )
{
    TRACE( "%p %p\n", hwnd, band );
    if (band) *band = 1;   /* ZBID_DESKTOP */
    return TRUE;
}

/***********************************************************************
 *      CreateWindowInBand (USER32.@)
 *
 * Private UWP window creation. dwBand is a ZBID_* z-order band; ignored here
 * (the window is a normal top-level HWND). Used by Windows.UI!PrivateCreateCoreWindow.
 */
HWND WINAPI CreateWindowInBand( DWORD ex_style, const WCHAR *class, const WCHAR *name,
                                DWORD style, INT x, INT y, INT cx, INT cy,
                                HWND parent, HMENU menu, HINSTANCE instance, void *param, DWORD band )
{
    FIXME( "ex %#lx class %s name %s style %#lx %d,%d %dx%d parent %p band %lu\n",
           ex_style, debugstr_w(class), debugstr_w(name), style, x, y, cx, cy, parent, band );
    if (!style) style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    /* Band windows are top-level and shown by the shell on activation. There is no
     * shell here; wineuwp is the activation host, so the window is born visible.
     * PrivateCreateCoreWindow passes WS_POPUP without WS_VISIBLE and nothing else
     * would ever map it. */
    style |= WS_VISIBLE;
    if (!cx) cx = CW_USEDEFAULT;
    if (!cy) cy = CW_USEDEFAULT;
    return CreateWindowExW( ex_style, class, name, style, x, y, cx, cy, parent, menu, instance, param );
}

HWND WINAPI CreateWindowInBandEx( DWORD ex_style, const WCHAR *class, const WCHAR *name,
                                  DWORD style, INT x, INT y, INT cx, INT cy,
                                  HWND parent, HMENU menu, HINSTANCE instance, void *param,
                                  DWORD band, DWORD flags )
{
    FIXME( "flags %#lx\n", flags );
    return CreateWindowInBand( ex_style, class, name, style, x, y, cx, cy, parent, menu, instance, param, band );
}

BOOL WINAPI EnableMouseInPointerForWindow( HWND hwnd, BOOL enable )
{
    FIXME( "%p %d: stub\n", hwnd, enable );
    return TRUE;
}

BOOL WINAPI SetCoreWindow( HWND hwnd, void *core_window )
{
    FIXME( "%p %p: stub\n", hwnd, core_window );
    return TRUE;
}

BOOL WINAPI SetCoreWindowPartner( HWND hwnd, void *partner )
{
    FIXME( "%p %p: stub\n", hwnd, partner );
    return TRUE;
}

/* NtUserCreateActivationObject: the caller passes the window and the input-site item id and
 * expects back the LUID that identifies the activation object.  There is no kernel object
 * behind this here, so hand out a process-unique LUID; callers only use it for identity. */
BOOL WINAPI CreateActivationObject( HWND hwnd, const UINT64 *item_id, LUID *luid )
{
    static LONG counter;

    FIXME( "%p %p %p: semi-stub\n", hwnd, item_id, luid );

    if (!luid)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }
    luid->LowPart = InterlockedIncrement( &counter );
    luid->HighPart = 0;
    return TRUE;
}

/* NtUserInternalClipCursor: clip the cursor to a window owned by this process.  With
 * use_window_rect set and no explicit rect the window's own rectangle is used. */
void WINAPI InternalClipCursor( HWND hwnd, BOOL use_window_rect, const RECT *rect )
{
    RECT window_rect;
    DWORD pid = 0;

    GetWindowThreadProcessId( hwnd, &pid );
    if (pid != GetCurrentProcessId())
    {
        SetLastError( ERROR_ACCESS_DENIED );
        return;
    }
    if (!use_window_rect)
    {
        if (rect) SetLastError( ERROR_INVALID_PARAMETER );
    }
    else if (!rect) rect = GetWindowRect( hwnd, &window_rect ) ? &window_rect : NULL;

    NtUserClipCursor( rect );
}

/* NtUserDestroyActivationObject / NtUserConfigureActivationObject: the LUID handed out by
 * CreateActivationObject names no kernel object here, so there is nothing to tear down or tune. */
BOOL WINAPI DestroyActivationObject( const LUID *luid )
{
    static int once;
    if (!once++) FIXME( "%p: stub\n", luid );
    return TRUE;
}

BOOL WINAPI ConfigureActivationObject( const LUID *luid, DWORD unk1, DWORD unk2, DWORD unk3, DWORD unk4 )
{
    static int once;
    if (!once++) FIXME( "%p %#lx %#lx %#lx %#lx: stub\n", luid, unk1, unk2, unk3, unk4 );
    return TRUE;
}

BOOL WINAPI SetForegroundRedirectionForActivationObject( const LUID *from, const LUID *to )
{
    static int once;
    if (!once++) FIXME( "%p %p: stub\n", from, to );
    return TRUE;
}

/* NtUserGetProcessUIContextInformation: which UI context a process runs in. A packaged app runs
 * in the immersive one; windows.storage uses this to decide how to resolve its app data. */
BOOL WINAPI GetProcessUIContextInformation( HANDLE process, DWORD *context )
{
    LONG WINAPI GetCurrentPackageId( UINT32 *length, BYTE *buffer );   /* kernelbase, no header */
    UINT32 size = 0;

    if (!context)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }
    if (process != GetCurrentProcess())
        FIXME( "%p: only the current process is reported\n", process );

    *context = GetCurrentPackageId( &size, NULL ) == ERROR_INSUFFICIENT_BUFFER ? 2 : 1;
    TRACE( "%p -> %lu\n", process, *context );
    return TRUE;
}

/* NtUserGetResizeDCompositionSynchronizationObject: the handle the compositor uses to keep a resize
 * and its first frame together. Wine has no compositor here, so there is no object to hand out. */
BOOL WINAPI GetResizeDCompositionSynchronizationObject( HWND hwnd, HANDLE *handle )
{
    static int once;
    if (!once++) FIXME( "%p %p: stub\n", hwnd, handle );
    if (handle) *handle = NULL;
    return TRUE;
}

/* NtUserEnableResizeLayoutSynchronization: asks the compositor to keep window resize and layout in
 * lockstep. Wine draws the window itself, so there is nothing to synchronise. */
BOOL WINAPI EnableResizeLayoutSynchronization( HWND hwnd, BOOL enable )
{
    static int once;
    if (!once++) FIXME( "%p %d: stub\n", hwnd, enable );
    return TRUE;
}

/* NtUserLayoutCompleted: the other half of EnableResizeLayoutSynchronization - the window tells
 * the compositor that the layout for the current size is done, so the new frame may be shown.
 * Nothing here holds a frame back, but failing the call aborts a packaged app's first layout
 * pass, so report success. */
BOOL WINAPI LayoutCompleted( HWND hwnd, DWORD unknown )
{
    static int once;
    if (!once++) FIXME( "%p %#lx: stub\n", hwnd, unknown );
    return TRUE;
}

/* NtUserSetWindowServicesDestroyCallback: registers (or with a NULL callback clears) a notification
 * for when the window's view services go away. Wine has no view services to tear down. */
BOOL WINAPI SetWindowServicesDestroyCallback( HWND hwnd, void *callback )
{
    static int once;
    if (!once++) FIXME( "%p %p: stub\n", hwnd, callback );
    return TRUE;
}

BOOL WINAPI SetWindowFeedbackSetting( HWND hwnd, FEEDBACK_TYPE feedback, DWORD flags,
                                      UINT32 size, const void *config )
{
    FIXME( "%p %u %#lx %u %p: stub\n", hwnd, feedback, flags, size, config );
    return TRUE;
}

BOOL WINAPI IsEnhancedWindowingEnabled( void )
{
    return FALSE;
}
