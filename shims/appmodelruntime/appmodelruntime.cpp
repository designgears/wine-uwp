// api-ms-win-appmodel-runtime-l1-1-1.dll: forward the apiset to kernelbase, and add the one
// function Wine is missing.
//
// After the StoreContext stand-in let Solitaire's startup get further, it hit another
// EXCEPTION_WINE_STUB (0x80000100) - the loader's unresolved-import stub again:
//
//     [   1.388] UserIdentityUtils::GetRpsTokenAsync - Calling with service: user.auth.xboxlive.com.
//     [   1.390] *** CRITICAL ERROR ***
//     wine: Unimplemented function api-ms-win-appmodel-runtime-l1-1-1.dll.
//           GetPackageFamilyNameFromToken called at address ...
//
// Eight functions are imported from this apiset across the DLLs in the prefix, and Wine's
// kernelbase exports seven of them. Only GetPackageFamilyNameFromToken is missing. It belongs in
// kernelbase, but that file is another session's, so this provides the apiset instead.
//
// **Shadowing an apiset is only safe if you provide all of it.** Unlike the advertising-id apiset,
// which Wine implements not at all, this one is in active use: a DLL here exporting only the
// missing function would break every other import from it. So everything kernelbase has is
// re-exported as a forwarder, and only the gap is implemented. The forwarded set is deliberately
// wider than the eight currently imported, so a DLL that starts importing another one later does
// not fail mysteriously.
//
// GetPackageFamilyNameFromToken is documented, so this is a real implementation rather than a
// refusal - the caller here is Xbox Live auth asking who the app is, and an error would just move
// the failure. A package family name is the first and last fields of the full name:
//
//     Name_Version_Architecture_ResourceId_PublisherId  ->  Name_PublisherId
//
// which is derived from kernelbase's GetPackageFullNameFromToken rather than from our own
// environment, so a token for some other package still gets the right answer.
#include <windows.h>
#include <appmodel.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* Everything Wine's kernelbase already implements, re-exported unchanged. */
#pragma comment(linker, "/export:FindPackagesByPackageFamily=kernelbase.FindPackagesByPackageFamily")
#pragma comment(linker, "/export:FormatApplicationUserModelId=kernelbase.FormatApplicationUserModelId")
#pragma comment(linker, "/export:GetApplicationUserModelIdFromToken=kernelbase.GetApplicationUserModelIdFromToken")
#pragma comment(linker, "/export:GetCurrentApplicationUserModelId=kernelbase.GetCurrentApplicationUserModelId")
#pragma comment(linker, "/export:GetCurrentPackageFamilyName=kernelbase.GetCurrentPackageFamilyName")
#pragma comment(linker, "/export:GetCurrentPackageFullName=kernelbase.GetCurrentPackageFullName")
#pragma comment(linker, "/export:GetCurrentPackageId=kernelbase.GetCurrentPackageId")
#pragma comment(linker, "/export:GetCurrentPackageInfo=kernelbase.GetCurrentPackageInfo")
#pragma comment(linker, "/export:GetCurrentPackagePath=kernelbase.GetCurrentPackagePath")
#pragma comment(linker, "/export:GetPackageFamilyName=kernelbase.GetPackageFamilyName")
#pragma comment(linker, "/export:GetPackageFullName=kernelbase.GetPackageFullName")
#pragma comment(linker, "/export:GetPackageFullNameFromToken=kernelbase.GetPackageFullNameFromToken")
#pragma comment(linker, "/export:GetPackagePathByFullName=kernelbase.GetPackagePathByFullName")
#pragma comment(linker, "/export:GetPackagesByPackageFamily=kernelbase.GetPackagesByPackageFamily")
#pragma comment(linker, "/export:GetStagedPackageOrigin=kernelbase.GetStagedPackageOrigin")
#pragma comment(linker, "/export:GetStagedPackagePathByFullName=kernelbase.GetStagedPackagePathByFullName")
#pragma comment(linker, "/export:ParseApplicationUserModelId=kernelbase.ParseApplicationUserModelId")

static void trace(const char *fmt, ...)
{
    if (!getenv("WINE_UWP_APPMODEL_TRACE")) return;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[appmodelruntime] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

/* Resolved from kernelbase at runtime, not imported: an import of GetPackageFullNameFromToken
 * binds to this very apiset, so the DLL would import from itself and fail to link. */
typedef LONG (WINAPI *full_name_from_token_fn)( HANDLE, UINT32 *, PWSTR );

static LONG call_full_name_from_token( HANDLE token, UINT32 *length, PWSTR name )
{
    static full_name_from_token_fn fn;
    static bool tried;

    if (!tried)
    {
        HMODULE kb = GetModuleHandleW( L"kernelbase.dll" );
        if (!kb) kb = LoadLibraryW( L"kernelbase.dll" );
        if (kb) fn = (full_name_from_token_fn)GetProcAddress( kb, "GetPackageFullNameFromToken" );
        tried = true;
        if (!fn) trace( "kernelbase has no GetPackageFullNameFromToken" );
    }
    if (!fn) return APPMODEL_ERROR_NO_PACKAGE;
    return fn( token, length, name );
}

/* Name_Version_Architecture_ResourceId_PublisherId -> Name_PublisherId.
 * Returns false if the full name does not have the five fields, rather than inventing one. */
static bool family_from_full( const WCHAR *full, WCHAR *out, size_t out_len )
{
    const WCHAR *underscore = wcschr( full, L'_' );
    const WCHAR *publisher = wcsrchr( full, L'_' );
    size_t name_len, pub_len;

    if (!underscore || !publisher || publisher <= underscore) return false;
    publisher++;                                  /* past the final '_' */
    name_len = (size_t)(underscore - full);
    pub_len = wcslen( publisher );
    if (!name_len || !pub_len || name_len + 1 + pub_len + 1 > out_len) return false;

    memcpy( out, full, name_len * sizeof(WCHAR) );
    out[name_len] = L'_';
    memcpy( out + name_len + 1, publisher, (pub_len + 1) * sizeof(WCHAR) );
    return true;
}

extern "C" __declspec(dllexport)
LONG WINAPI GetPackageFamilyNameFromToken( HANDLE token, UINT32 *length, PWSTR name )
{
    WCHAR full[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
    WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
    UINT32 full_len = ARRAYSIZE(full);
    UINT32 needed;
    LONG ret;

    if (!length) return ERROR_INVALID_PARAMETER;

    ret = call_full_name_from_token( token, &full_len, full );
    if (ret != ERROR_SUCCESS)
    {
        trace( "GetPackageFullNameFromToken failed %ld", ret );
        return ret;                               /* APPMODEL_ERROR_NO_PACKAGE, usually */
    }
    if (!family_from_full( full, family, ARRAYSIZE(family) ))
    {
        trace( "full name %ls is not Name_..._PublisherId", full );
        return APPMODEL_ERROR_NO_PACKAGE;
    }

    needed = (UINT32)wcslen( family ) + 1;        /* the docs count the terminating NUL */
    if (!name || *length < needed)
    {
        *length = needed;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    memcpy( name, family, needed * sizeof(WCHAR) );
    *length = needed;
    trace( "token %p -> %ls", token, family );
    return ERROR_SUCCESS;
}

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls( inst );
    return TRUE;
}
