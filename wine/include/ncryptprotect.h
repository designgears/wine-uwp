/*
 * Copyright 2026 Bobby Kircher
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

#ifndef __WINE_NCRYPTPROTECT_H
#define __WINE_NCRYPTPROTECT_H

#include <ncrypt.h>

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_HANDLE(NCRYPT_DESCRIPTOR_HANDLE);
DECLARE_HANDLE(NCRYPT_STREAM_HANDLE);

typedef SECURITY_STATUS (WINAPI *PFNCryptStreamOutputCallback)(void *ctx, const BYTE *data, SIZE_T size, BOOL final);

typedef struct NCRYPT_PROTECT_STREAM_INFO
{
    PFNCryptStreamOutputCallback pfnStreamOutput;
    void *pvCallbackCtxt;
} NCRYPT_PROTECT_STREAM_INFO;

#define NCRYPT_UNPROTECT_NO_DECRYPT 0x00000001

SECURITY_STATUS WINAPI NCryptCreateProtectionDescriptor(const WCHAR *, DWORD, NCRYPT_DESCRIPTOR_HANDLE *);
SECURITY_STATUS WINAPI NCryptCloseProtectionDescriptor(NCRYPT_DESCRIPTOR_HANDLE);
SECURITY_STATUS WINAPI NCryptProtectSecret(NCRYPT_DESCRIPTOR_HANDLE, DWORD, const BYTE *, ULONG,
                                           const NCRYPT_ALLOC_PARA *, HWND, BYTE **, ULONG *);
SECURITY_STATUS WINAPI NCryptUnprotectSecret(NCRYPT_DESCRIPTOR_HANDLE *, DWORD, const BYTE *, ULONG,
                                             const NCRYPT_ALLOC_PARA *, HWND, BYTE **, ULONG *);
SECURITY_STATUS WINAPI NCryptStreamOpenToProtect(NCRYPT_DESCRIPTOR_HANDLE, DWORD, HWND,
                                                 NCRYPT_PROTECT_STREAM_INFO *, NCRYPT_STREAM_HANDLE *);
SECURITY_STATUS WINAPI NCryptStreamOpenToUnprotect(NCRYPT_PROTECT_STREAM_INFO *, DWORD, HWND, NCRYPT_STREAM_HANDLE *);
SECURITY_STATUS WINAPI NCryptStreamUpdate(NCRYPT_STREAM_HANDLE, const BYTE *, SIZE_T, BOOL);
SECURITY_STATUS WINAPI NCryptStreamClose(NCRYPT_STREAM_HANDLE);

#ifdef __cplusplus
}
#endif

#endif /* __WINE_NCRYPTPROTECT_H */
