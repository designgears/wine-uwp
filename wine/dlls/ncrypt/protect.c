/*
 * DPAPI-NG: protection descriptors, NCryptProtectSecret and the protection streams
 *
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

/* Windows.Security.Cryptography.DataProtection.DataProtectionProvider is built on these, and
 * apps use it to keep credentials at rest - Shapr3D encrypts its sign-in session with it and
 * fail-fasted on the NCryptCreateProtectionDescriptor stub the moment a login succeeded.
 *
 * Only the principals that need no directory service can be honoured here: LOCAL=user,
 * LOCAL=machine and LOCAL=logon all protect to the local user through CryptProtectData, which is
 * what DPAPI means on Wine. Anything else (SID=, SDDL=, WEBCREDENTIALS=) is accepted as a
 * descriptor string and treated the same way, with a FIXME. The protected blob is ours - it records
 * the descriptor string and wraps a CryptProtectData blob - so it only round-trips through this
 * implementation, which is all an app on this machine can ask of it. */

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "wincrypt.h"
#include "ncryptprotect.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ncrypt);

#define DESCRIPTOR_MAGIC 0x43534544 /* 'DESC' */
#define STREAM_MAGIC     0x4d525453 /* 'STRM' */
#define BLOB_MAGIC       0x504e4757 /* 'WGNP' */

struct descriptor
{
    DWORD magic;
    WCHAR *string;
};

struct blob_header
{
    DWORD magic;
    DWORD desc_len;    /* in WCHARs, no terminator; the string follows, then the DPAPI blob */
};

struct stream
{
    DWORD magic;
    BOOL protect;
    struct descriptor *descriptor;
    NCRYPT_PROTECT_STREAM_INFO info;
    BYTE *data;
    SIZE_T size, capacity;
};

static struct descriptor *get_descriptor(NCRYPT_DESCRIPTOR_HANDLE handle)
{
    struct descriptor *desc = (struct descriptor *)handle;
    return desc && desc->magic == DESCRIPTOR_MAGIC ? desc : NULL;
}

static SECURITY_STATUS create_descriptor(const WCHAR *string, SIZE_T len, struct descriptor **out)
{
    struct descriptor *desc;

    if (!(desc = calloc(1, sizeof(*desc))) || !(desc->string = malloc((len + 1) * sizeof(WCHAR))))
    {
        free(desc);
        return NTE_NO_MEMORY;
    }
    memcpy(desc->string, string, len * sizeof(WCHAR));
    desc->string[len] = 0;
    desc->magic = DESCRIPTOR_MAGIC;
    *out = desc;
    return ERROR_SUCCESS;
}

static void free_descriptor(struct descriptor *desc)
{
    desc->magic = 0;
    free(desc->string);
    free(desc);
}

SECURITY_STATUS WINAPI NCryptCreateProtectionDescriptor(const WCHAR *string, DWORD flags,
                                                        NCRYPT_DESCRIPTOR_HANDLE *handle)
{
    struct descriptor *desc;
    SECURITY_STATUS status;

    TRACE("string %s, flags %#lx, handle %p.\n", debugstr_w(string), flags, handle);

    if (!string || !*string || !handle) return NTE_INVALID_PARAMETER;
    if (wcsnicmp(string, L"LOCAL=", 6))
        FIXME("principal %s protects to the local user\n", debugstr_w(string));

    if ((status = create_descriptor(string, wcslen(string), &desc))) return status;
    *handle = (NCRYPT_DESCRIPTOR_HANDLE)desc;
    return ERROR_SUCCESS;
}

SECURITY_STATUS WINAPI NCryptCloseProtectionDescriptor(NCRYPT_DESCRIPTOR_HANDLE handle)
{
    struct descriptor *desc = get_descriptor(handle);

    TRACE("handle %p.\n", handle);

    if (!desc) return NTE_INVALID_HANDLE;
    free_descriptor(desc);
    return ERROR_SUCCESS;
}

static BYTE *alloc_output(const NCRYPT_ALLOC_PARA *para, SIZE_T size)
{
    if (para && para->pfnAlloc) return para->pfnAlloc(size);
    return LocalAlloc(LMEM_FIXED, size);
}

static SECURITY_STATUS protect(struct descriptor *desc, const BYTE *data, ULONG size,
                               const NCRYPT_ALLOC_PARA *para, BYTE **out, ULONG *out_size)
{
    DATA_BLOB in = { size, (BYTE *)data }, sealed;
    struct blob_header header;
    SIZE_T desc_bytes, total;
    BYTE *blob;

    if (!CryptProtectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &sealed))
        return HRESULT_FROM_WIN32(GetLastError());

    header.magic = BLOB_MAGIC;
    header.desc_len = wcslen(desc->string);
    desc_bytes = header.desc_len * sizeof(WCHAR);
    total = sizeof(header) + desc_bytes + sealed.cbData;
    if (!(blob = alloc_output(para, total)))
    {
        LocalFree(sealed.pbData);
        return NTE_NO_MEMORY;
    }
    memcpy(blob, &header, sizeof(header));
    memcpy(blob + sizeof(header), desc->string, desc_bytes);
    memcpy(blob + sizeof(header) + desc_bytes, sealed.pbData, sealed.cbData);
    LocalFree(sealed.pbData);

    *out = blob;
    *out_size = total;
    return ERROR_SUCCESS;
}

static SECURITY_STATUS unprotect(DWORD flags, const BYTE *blob, SIZE_T size, const NCRYPT_ALLOC_PARA *para,
                                 struct descriptor **desc_out, BYTE **out, ULONG *out_size)
{
    struct blob_header header;
    DATA_BLOB in, plain;
    SIZE_T desc_bytes;
    SECURITY_STATUS status;

    if (size < sizeof(header)) return NTE_BAD_DATA;
    memcpy(&header, blob, sizeof(header));
    desc_bytes = (SIZE_T)header.desc_len * sizeof(WCHAR);
    if (header.magic != BLOB_MAGIC || desc_bytes > size - sizeof(header)) return NTE_BAD_DATA;

    if (desc_out &&
        (status = create_descriptor((const WCHAR *)(blob + sizeof(header)), header.desc_len, desc_out)))
        return status;
    if (flags & NCRYPT_UNPROTECT_NO_DECRYPT)
    {
        if (out) *out = NULL;
        if (out_size) *out_size = 0;
        return ERROR_SUCCESS;
    }

    in.pbData = (BYTE *)blob + sizeof(header) + desc_bytes;
    in.cbData = size - sizeof(header) - desc_bytes;
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &plain))
    {
        status = HRESULT_FROM_WIN32(GetLastError());
        goto failed;
    }
    if (!(*out = alloc_output(para, plain.cbData ? plain.cbData : 1)))
    {
        SecureZeroMemory(plain.pbData, plain.cbData);
        LocalFree(plain.pbData);
        status = NTE_NO_MEMORY;
        goto failed;
    }
    memcpy(*out, plain.pbData, plain.cbData);
    *out_size = plain.cbData;
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return ERROR_SUCCESS;

failed:
    if (desc_out)
    {
        free_descriptor(*desc_out);
        *desc_out = NULL;
    }
    return status;
}

SECURITY_STATUS WINAPI NCryptProtectSecret(NCRYPT_DESCRIPTOR_HANDLE handle, DWORD flags, const BYTE *data,
                                           ULONG size, const NCRYPT_ALLOC_PARA *para, HWND hwnd,
                                           BYTE **blob, ULONG *blob_size)
{
    struct descriptor *desc = get_descriptor(handle);

    TRACE("handle %p, flags %#lx, data %p, size %lu, para %p, hwnd %p, blob %p, blob_size %p.\n",
          handle, flags, data, size, para, hwnd, blob, blob_size);

    if (!desc) return NTE_INVALID_HANDLE;
    if ((!data && size) || !blob || !blob_size) return NTE_INVALID_PARAMETER;
    return protect(desc, data, size, para, blob, blob_size);
}

SECURITY_STATUS WINAPI NCryptUnprotectSecret(NCRYPT_DESCRIPTOR_HANDLE *handle, DWORD flags, const BYTE *blob,
                                             ULONG blob_size, const NCRYPT_ALLOC_PARA *para, HWND hwnd,
                                             BYTE **data, ULONG *size)
{
    struct descriptor *desc = NULL;
    SECURITY_STATUS status;

    TRACE("handle %p, flags %#lx, blob %p, blob_size %lu, para %p, hwnd %p, data %p, size %p.\n",
          handle, flags, blob, blob_size, para, hwnd, data, size);

    if (!blob || (!(flags & NCRYPT_UNPROTECT_NO_DECRYPT) && (!data || !size))) return NTE_INVALID_PARAMETER;
    status = unprotect(flags, blob, blob_size, para, handle ? &desc : NULL, data, size);
    if (handle) *handle = (NCRYPT_DESCRIPTOR_HANDLE)desc;
    return status;
}

/* The streams gather everything and do the work on the final update: the output of a
 * CryptProtectData blob cannot be produced incrementally anyway. The callback is called once,
 * with fFinal set. */
SECURITY_STATUS WINAPI NCryptStreamOpenToProtect(NCRYPT_DESCRIPTOR_HANDLE handle, DWORD flags, HWND hwnd,
                                                 NCRYPT_PROTECT_STREAM_INFO *info, NCRYPT_STREAM_HANDLE *out)
{
    struct descriptor *desc = get_descriptor(handle);
    struct stream *stream;
    SECURITY_STATUS status;

    TRACE("handle %p, flags %#lx, hwnd %p, info %p, out %p.\n", handle, flags, hwnd, info, out);

    if (!desc) return NTE_INVALID_HANDLE;
    if (!info || !info->pfnStreamOutput || !out) return NTE_INVALID_PARAMETER;
    if (!(stream = calloc(1, sizeof(*stream)))) return NTE_NO_MEMORY;
    /* the stream keeps its own copy: the caller may close the descriptor first */
    if ((status = create_descriptor(desc->string, wcslen(desc->string), &stream->descriptor)))
    {
        free(stream);
        return status;
    }
    stream->magic = STREAM_MAGIC;
    stream->protect = TRUE;
    stream->info = *info;
    *out = (NCRYPT_STREAM_HANDLE)stream;
    return ERROR_SUCCESS;
}

SECURITY_STATUS WINAPI NCryptStreamOpenToUnprotect(NCRYPT_PROTECT_STREAM_INFO *info, DWORD flags, HWND hwnd,
                                                   NCRYPT_STREAM_HANDLE *out)
{
    struct stream *stream;

    TRACE("info %p, flags %#lx, hwnd %p, out %p.\n", info, flags, hwnd, out);

    if (!info || !info->pfnStreamOutput || !out) return NTE_INVALID_PARAMETER;
    if (!(stream = calloc(1, sizeof(*stream)))) return NTE_NO_MEMORY;
    stream->magic = STREAM_MAGIC;
    stream->info = *info;
    *out = (NCRYPT_STREAM_HANDLE)stream;
    return ERROR_SUCCESS;
}

SECURITY_STATUS WINAPI NCryptStreamUpdate(NCRYPT_STREAM_HANDLE handle, const BYTE *data, SIZE_T size, BOOL final)
{
    struct stream *stream = (struct stream *)handle;
    SECURITY_STATUS status;
    BYTE *out = NULL;
    ULONG out_size = 0;

    TRACE("handle %p, data %p, size %Iu, final %d.\n", handle, data, size, final);

    if (!stream || stream->magic != STREAM_MAGIC) return NTE_INVALID_HANDLE;
    if (!data && size) return NTE_INVALID_PARAMETER;

    if (size)
    {
        if (stream->size + size > stream->capacity)
        {
            SIZE_T capacity = max(stream->capacity * 2, stream->size + size);
            BYTE *grown = realloc(stream->data, capacity);
            if (!grown) return NTE_NO_MEMORY;
            stream->data = grown;
            stream->capacity = capacity;
        }
        memcpy(stream->data + stream->size, data, size);
        stream->size += size;
    }
    if (!final) return ERROR_SUCCESS;

    if (stream->protect)
        status = protect(stream->descriptor, stream->data, stream->size, NULL, &out, &out_size);
    else
        status = unprotect(0, stream->data, stream->size, NULL, NULL, &out, &out_size);
    if (status) return status;

    status = stream->info.pfnStreamOutput(stream->info.pvCallbackCtxt, out, out_size, TRUE);
    SecureZeroMemory(out, out_size);
    LocalFree(out);
    return status;
}

SECURITY_STATUS WINAPI NCryptStreamClose(NCRYPT_STREAM_HANDLE handle)
{
    struct stream *stream = (struct stream *)handle;

    TRACE("handle %p.\n", handle);

    if (!stream || stream->magic != STREAM_MAGIC) return NTE_INVALID_HANDLE;
    stream->magic = 0;
    if (stream->descriptor) free_descriptor(stream->descriptor);
    if (stream->data)
    {
        SecureZeroMemory(stream->data, stream->size);
        free(stream->data);
    }
    free(stream);
    return ERROR_SUCCESS;
}
