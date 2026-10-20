/* WinRT Windows.Storage.Pickers Implementation
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

/*
 * Windows registers the pickers in twinui.appcore.dll, whose picker is shell UI in a broker process
 * a prefix does not have: PickSingleFileAsync never completed, and FileSavePicker's constructor
 * failed, which kept Shapr3D's dashboard from coming up. Here the pickers show the common file
 * dialogs, modal on the calling (UI) thread, which pumps messages while one is up; the operation is
 * then already complete. Closing a dialog is a cancelled pick: a NULL file, or no files.
 *
 * For scripted runs: WINE_UWP_PICKER_FILE names the file an open picker returns (if it matches the
 * picker's FileTypeFilter), WINE_UWP_PICKER_SAVE_FILE the file a save picker returns (created if it
 * does not exist), and WINE_UWP_PICKER_NODIALOG=1 makes any other pick a cancelled one.
 */

#include "initguid.h"
#include "private.h"
#include "commdlg.h"
#include "shlobj.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(pickers);

static BOOL dialogs_enabled(void)
{
    WCHAR value[8];
    return !GetEnvironmentVariableW( L"WINE_UWP_PICKER_NODIALOG", value, ARRAY_SIZE(value) );
}

/* StorageFiles and StorageFolders come from windows.storage, through its public statics. */

static HRESULT wait_for( IUnknown *operation )
{
    IAsyncInfo *info;
    AsyncStatus status = Started;
    HRESULT hr;
    int i;

    if (FAILED(hr = IUnknown_QueryInterface( operation, &IID_IAsyncInfo, (void **)&info ))) return hr;
    for (i = 0; SUCCEEDED(hr) && i < 1000; i++)
    {
        if (FAILED(hr = IAsyncInfo_get_Status( info, &status )) || status != Started) break;
        Sleep( 5 );
    }
    if (SUCCEEDED(hr) && status != Completed) IAsyncInfo_get_ErrorCode( info, &hr );
    if (SUCCEEDED(hr) && status != Completed) hr = E_FAIL;
    IAsyncInfo_Release( info );
    return hr;
}

static HRESULT storage_file_from_path( const WCHAR *path, IStorageFile **file )
{
    IAsyncOperation_StorageFile *operation;
    IStorageFileStatics *statics;
    HSTRING name, str;
    HRESULT hr;

    *file = NULL;
    if (FAILED(hr = WindowsCreateString( RuntimeClass_Windows_Storage_StorageFile,
                                         wcslen( RuntimeClass_Windows_Storage_StorageFile ), &name ))) return hr;
    hr = RoGetActivationFactory( name, &IID_IStorageFileStatics, (void **)&statics );
    WindowsDeleteString( name );
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = WindowsCreateString( path, wcslen( path ), &str )))
    {
        hr = IStorageFileStatics_GetFileFromPathAsync( statics, str, &operation );
        WindowsDeleteString( str );
        if (SUCCEEDED(hr))
        {
            if (SUCCEEDED(hr = wait_for( (IUnknown *)operation )))
                hr = IAsyncOperation_StorageFile_GetResults( operation, file );
            IAsyncOperation_StorageFile_Release( operation );
        }
    }
    IStorageFileStatics_Release( statics );
    TRACE( "%s -> %p, hr %#lx.\n", debugstr_w( path ), *file, hr );
    return hr;
}

static HRESULT storage_folder_from_path( const WCHAR *path, IStorageFolder **folder )
{
    IAsyncOperation_StorageFolder *operation;
    IStorageFolderStatics *statics;
    HSTRING name, str;
    HRESULT hr;

    *folder = NULL;
    if (FAILED(hr = WindowsCreateString( RuntimeClass_Windows_Storage_StorageFolder,
                                         wcslen( RuntimeClass_Windows_Storage_StorageFolder ), &name ))) return hr;
    hr = RoGetActivationFactory( name, &IID_IStorageFolderStatics, (void **)&statics );
    WindowsDeleteString( name );
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = WindowsCreateString( path, wcslen( path ), &str )))
    {
        hr = IStorageFolderStatics_GetFolderFromPathAsync( statics, str, &operation );
        WindowsDeleteString( str );
        if (SUCCEEDED(hr))
        {
            if (SUCCEEDED(hr = wait_for( (IUnknown *)operation )))
                hr = IAsyncOperation_StorageFolder_GetResults( operation, folder );
            IAsyncOperation_StorageFolder_Release( operation );
        }
    }
    IStorageFolderStatics_Release( statics );
    TRACE( "%s -> %p, hr %#lx.\n", debugstr_w( path ), *folder, hr );
    return hr;
}

/* The picked item is known before the operation exists; the callback hands it over. */
static HRESULT return_result( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_UNKNOWN;
    if ((result->punkVal = param)) IUnknown_AddRef( param );
    return S_OK;
}

/* The dialogs */

/* The directory a dialog starts in; Documents unless the app suggests somewhere else. */
static void start_directory( PickerLocationId location, WCHAR *dir )
{
    const KNOWNFOLDERID *id;
    WCHAR *path;

    switch (location)
    {
    case PickerLocationId_Desktop:         id = &FOLDERID_Desktop; break;
    case PickerLocationId_Downloads:       id = &FOLDERID_Downloads; break;
    case PickerLocationId_MusicLibrary:    id = &FOLDERID_Music; break;
    case PickerLocationId_PicturesLibrary: id = &FOLDERID_Pictures; break;
    case PickerLocationId_VideosLibrary:   id = &FOLDERID_Videos; break;
    case PickerLocationId_Objects3D:       id = &FOLDERID_Objects3D; break;
    default:                               id = &FOLDERID_Documents; break;
    }
    dir[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath( id, KF_FLAG_CREATE, NULL, &path )) ||
        SUCCEEDED(SHGetKnownFolderPath( &FOLDERID_Documents, 0, NULL, &path )))
    {
        lstrcpynW( dir, path, MAX_PATH );
        CoTaskMemFree( path );
    }
}

/* Appends "*.ext" for ".ext" to a ';'-separated pattern list; "*" is everything. */
static void append_pattern( WCHAR *patterns, size_t size, const WCHAR *ext )
{
    size_t len = wcslen( patterns );

    if (!ext || !*ext) return;
    swprintf( patterns + len, size - len, L"%s%s%s", len ? L";" : L"", wcscmp( ext, L"*" ) ? L"*" : L"",
              wcscmp( ext, L"*" ) ? ext : L"*.*" );
}

static void vector_patterns( IVector_HSTRING *vector, WCHAR *patterns, size_t size, WCHAR *first_ext, size_t ext_size )
{
    UINT32 count = 0, i;
    HSTRING ext;

    if (FAILED(IVector_HSTRING_get_Size( vector, &count ))) return;
    for (i = 0; i < count; i++)
    {
        const WCHAR *str;
        if (FAILED(IVector_HSTRING_GetAt( vector, i, &ext ))) continue;
        str = WindowsGetStringRawBuffer( ext, NULL );
        append_pattern( patterns, size, str );
        if (first_ext && !first_ext[0] && str[0] == '.') lstrcpynW( first_ext, str, ext_size );
        WindowsDeleteString( ext );
    }
}

/* Appends one filter entry, "name\0patterns\0", to a double-NUL-terminated OPENFILENAME filter. */
static void add_filter( WCHAR *filter, size_t size, size_t *pos, const WCHAR *name, const WCHAR *patterns )
{
    size_t name_len = wcslen( name ) + 1, len = wcslen( patterns ) + 1;

    if (*pos + name_len + len + 1 > size) return;
    memcpy( filter + *pos, name, name_len * sizeof(WCHAR) );
    *pos += name_len;
    memcpy( filter + *pos, patterns, len * sizeof(WCHAR) );
    *pos += len;
    filter[*pos] = 0;
}

static BOOL matches_filter( IVector_HSTRING *filter, const WCHAR *path )
{
    const WCHAR *ext = wcsrchr( path, '.' ), *str;
    UINT32 count = 0, i;
    BOOL ok = FALSE;
    HSTRING item;

    if (FAILED(IVector_HSTRING_get_Size( filter, &count )) || !count) return TRUE;
    for (i = 0; i < count && !ok; i++)
    {
        if (FAILED(IVector_HSTRING_GetAt( filter, i, &item ))) continue;
        str = WindowsGetStringRawBuffer( item, NULL );
        ok = !wcscmp( str, L"*" ) || (ext && !wcsicmp( str, ext ));
        WindowsDeleteString( item );
    }
    return ok;
}

/* Runs the open dialog; buffer gets the path, or with multiple the directory and then the file
 * names, each NUL-terminated, ending with an empty one. */
static BOOL open_dialog( IVector_HSTRING *filter_vector, PickerLocationId location, BOOL multiple,
                         WCHAR *buffer, DWORD size )
{
    OPENFILENAMEW ofn = {.lStructSize = sizeof(ofn)};
    WCHAR patterns[512] = L"", filter[1024] = L"", dir[MAX_PATH];
    size_t pos = 0;

    vector_patterns( filter_vector, patterns, ARRAY_SIZE(patterns), NULL, 0 );
    if (patterns[0] && wcscmp( patterns, L"*.*" )) add_filter( filter, ARRAY_SIZE(filter), &pos, patterns, patterns );
    add_filter( filter, ARRAY_SIZE(filter), &pos, L"All files (*.*)", L"*.*" );
    start_directory( location, dir );

    buffer[0] = buffer[1] = 0;
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = size;
    ofn.lpstrInitialDir = dir[0] ? dir : NULL;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (multiple) ofn.Flags |= OFN_ALLOWMULTISELECT;
    if (GetOpenFileNameW( &ofn ))
    {
        /* a single file in a multiple pick comes back as one full path */
        if (multiple && ofn.nFileOffset && buffer[ofn.nFileOffset - 1])
            buffer[wcslen( buffer ) + 1] = 0;
        return TRUE;
    }
    TRACE( "cancelled, error %#lx.\n", CommDlgExtendedError() );
    return FALSE;
}

/* The save dialog's filter: one entry per FileTypeChoices choice, in the app's order, then all
 * files. first_ext gets the first choice's first extension. */
static void choices_filter( IMap_HSTRING_IVector_HSTRING *choices, WCHAR *filter, size_t size, WCHAR *first_ext,
                            size_t ext_size )
{
    IIterable_IKeyValuePair_HSTRING_IVector_HSTRING *iterable;
    IIterator_IKeyValuePair_HSTRING_IVector_HSTRING *iterator;
    IKeyValuePair_HSTRING_IVector_HSTRING *pair;
    WCHAR patterns[512], name[640];
    IVector_HSTRING *vector;
    boolean has = FALSE;
    size_t pos = 0;
    HSTRING key;

    filter[0] = filter[1] = 0;
    if (choices && SUCCEEDED(IMap_HSTRING_IVector_HSTRING_QueryInterface( choices,
                                 &IID_IIterable_IKeyValuePair_HSTRING_IVector_HSTRING, (void **)&iterable )))
    {
        if (SUCCEEDED(IIterable_IKeyValuePair_HSTRING_IVector_HSTRING_First( iterable, &iterator )))
        {
            IIterator_IKeyValuePair_HSTRING_IVector_HSTRING_get_HasCurrent( iterator, &has );
            while (has && SUCCEEDED(IIterator_IKeyValuePair_HSTRING_IVector_HSTRING_get_Current( iterator, &pair )))
            {
                patterns[0] = 0;
                if (SUCCEEDED(IKeyValuePair_HSTRING_IVector_HSTRING_get_Value( pair, &vector )))
                {
                    vector_patterns( vector, patterns, ARRAY_SIZE(patterns), first_ext, ext_size );
                    IVector_HSTRING_Release( vector );
                }
                if (patterns[0] && SUCCEEDED(IKeyValuePair_HSTRING_IVector_HSTRING_get_Key( pair, &key )))
                {
                    swprintf( name, ARRAY_SIZE(name), L"%s (%s)",
                              WindowsGetStringLen( key ) ? WindowsGetStringRawBuffer( key, NULL ) : patterns, patterns );
                    add_filter( filter, size, &pos, name, patterns );
                    WindowsDeleteString( key );
                }
                IKeyValuePair_HSTRING_IVector_HSTRING_Release( pair );
                if (FAILED(IIterator_IKeyValuePair_HSTRING_IVector_HSTRING_MoveNext( iterator, &has ))) break;
            }
            IIterator_IKeyValuePair_HSTRING_IVector_HSTRING_Release( iterator );
        }
        IIterable_IKeyValuePair_HSTRING_IVector_HSTRING_Release( iterable );
    }
    add_filter( filter, size, &pos, L"All files (*.*)", L"*.*" );
}

static BOOL save_dialog( IMap_HSTRING_IVector_HSTRING *choices, HSTRING suggested, HSTRING default_ext,
                         PickerLocationId location, WCHAR *path, DWORD size )
{
    OPENFILENAMEW ofn = {.lStructSize = sizeof(ofn)};
    WCHAR filter[2048], choice_ext[32] = L"", dir[MAX_PATH];
    const WCHAR *ext = WindowsGetStringRawBuffer( default_ext, NULL ), *name = WindowsGetStringRawBuffer( suggested, NULL );
    size_t len;

    choices_filter( choices, filter, ARRAY_SIZE(filter), choice_ext, ARRAY_SIZE(choice_ext) );
    if (!*ext) ext = choice_ext;
    start_directory( location, dir );

    lstrcpynW( path, name, size );
    len = wcslen( path );
    if (len && *ext && !(len >= wcslen( ext ) && !wcsicmp( path + len - wcslen( ext ), ext )))
        lstrcpynW( path + len, ext, size - len );

    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = size;
    ofn.lpstrDefExt = *ext == '.' ? ext + 1 : NULL;
    ofn.lpstrInitialDir = dir[0] ? dir : NULL;
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (GetSaveFileNameW( &ofn )) return TRUE;
    TRACE( "cancelled, error %#lx.\n", CommDlgExtendedError() );
    return FALSE;
}

static BOOL folder_dialog( WCHAR *path )
{
    BROWSEINFOW info = {0};
    LPITEMIDLIST pidl;
    BOOL ok;

    info.hwndOwner = GetActiveWindow();
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (!(pidl = SHBrowseForFolderW( &info ))) return FALSE;
    ok = SHGetPathFromIDListW( pidl, path );
    CoTaskMemFree( pidl );
    return ok;
}

/* FileOpenPicker and FolderPicker: the same options */

struct picker
{
    IFileOpenPicker IFileOpenPicker_iface;
    IFolderPicker IFolderPicker_iface;
    LONG ref;
    BOOL folder;
    PickerViewMode view_mode;
    PickerLocationId location;
    HSTRING settings_id;
    HSTRING commit_text;
    IVector_HSTRING *filter;
};

static inline struct picker *impl_from_IFileOpenPicker( IFileOpenPicker *iface )
{
    return CONTAINING_RECORD( iface, struct picker, IFileOpenPicker_iface );
}

static HRESULT WINAPI open_picker_QueryInterface( IFileOpenPicker *iface, REFIID iid, void **out )
{
    struct picker *impl = impl_from_IFileOpenPicker( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) || IsEqualGUID( iid, &IID_IAgileObject ))
        *out = impl->folder ? (void *)&impl->IFolderPicker_iface : (void *)&impl->IFileOpenPicker_iface;
    else if (!impl->folder && IsEqualGUID( iid, &IID_IFileOpenPicker ))
        *out = &impl->IFileOpenPicker_iface;
    else if (impl->folder && IsEqualGUID( iid, &IID_IFolderPicker ))
        *out = &impl->IFolderPicker_iface;
    else
    {
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
        *out = NULL;
        return E_NOINTERFACE;
    }
    IInspectable_AddRef( (IInspectable *)*out );
    return S_OK;
}

static ULONG WINAPI open_picker_AddRef( IFileOpenPicker *iface )
{
    return InterlockedIncrement( &impl_from_IFileOpenPicker( iface )->ref );
}

static ULONG WINAPI open_picker_Release( IFileOpenPicker *iface )
{
    struct picker *impl = impl_from_IFileOpenPicker( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->settings_id );
        WindowsDeleteString( impl->commit_text );
        IVector_HSTRING_Release( impl->filter );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI open_picker_GetIids( IFileOpenPicker *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI open_picker_GetRuntimeClassName( IFileOpenPicker *iface, HSTRING *class_name )
{
    const WCHAR *name = impl_from_IFileOpenPicker( iface )->folder ? RuntimeClass_Windows_Storage_Pickers_FolderPicker
                                                                    : RuntimeClass_Windows_Storage_Pickers_FileOpenPicker;
    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT WINAPI open_picker_GetTrustLevel( IFileOpenPicker *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT replace_string( HSTRING *field, HSTRING value )
{
    HSTRING copy;
    HRESULT hr;

    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    WindowsDeleteString( *field );
    *field = copy;
    return S_OK;
}

static HRESULT WINAPI open_picker_get_ViewMode( IFileOpenPicker *iface, PickerViewMode *value )
{
    *value = impl_from_IFileOpenPicker( iface )->view_mode;
    return S_OK;
}

static HRESULT WINAPI open_picker_put_ViewMode( IFileOpenPicker *iface, PickerViewMode value )
{
    impl_from_IFileOpenPicker( iface )->view_mode = value;
    return S_OK;
}

static HRESULT WINAPI open_picker_get_SettingsIdentifier( IFileOpenPicker *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IFileOpenPicker( iface )->settings_id, value );
}

static HRESULT WINAPI open_picker_put_SettingsIdentifier( IFileOpenPicker *iface, HSTRING value )
{
    return replace_string( &impl_from_IFileOpenPicker( iface )->settings_id, value );
}

static HRESULT WINAPI open_picker_get_SuggestedStartLocation( IFileOpenPicker *iface, PickerLocationId *value )
{
    *value = impl_from_IFileOpenPicker( iface )->location;
    return S_OK;
}

static HRESULT WINAPI open_picker_put_SuggestedStartLocation( IFileOpenPicker *iface, PickerLocationId value )
{
    TRACE( "iface %p, value %d.\n", iface, value );
    impl_from_IFileOpenPicker( iface )->location = value;
    return S_OK;
}

static HRESULT WINAPI open_picker_get_CommitButtonText( IFileOpenPicker *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IFileOpenPicker( iface )->commit_text, value );
}

static HRESULT WINAPI open_picker_put_CommitButtonText( IFileOpenPicker *iface, HSTRING value )
{
    return replace_string( &impl_from_IFileOpenPicker( iface )->commit_text, value );
}

static HRESULT WINAPI open_picker_get_FileTypeFilter( IFileOpenPicker *iface, IVector_HSTRING **value )
{
    IVector_HSTRING_AddRef( (*value = impl_from_IFileOpenPicker( iface )->filter) );
    return S_OK;
}

static HRESULT WINAPI open_picker_PickSingleFileAsync( IFileOpenPicker *iface, IAsyncOperation_StorageFile **operation )
{
    struct picker *impl = impl_from_IFileOpenPicker( iface );
    IStorageFile *file = NULL;
    WCHAR path[MAX_PATH];
    HRESULT hr;

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (GetEnvironmentVariableW( L"WINE_UWP_PICKER_FILE", path, ARRAY_SIZE(path) ))
    {
        if (matches_filter( impl->filter, path )) storage_file_from_path( path, &file );
        else WARN( "%s does not match the filter.\n", debugstr_w( path ) );
    }
    else if (dialogs_enabled() && open_dialog( impl->filter, impl->location, FALSE, path, ARRAY_SIZE(path) ))
        storage_file_from_path( path, &file );

    TRACE( "picked %s.\n", debugstr_w( file ? path : NULL ) );
    hr = async_operation_file_create( (IUnknown *)iface, (IUnknown *)file, return_result, operation );
    if (file) IStorageFile_Release( file );
    return hr;
}

static HRESULT WINAPI open_picker_PickMultipleFilesAsync( IFileOpenPicker *iface,
                                                          IAsyncOperation_IVectorView_StorageFile **operation )
{
    struct picker *impl = impl_from_IFileOpenPicker( iface );
    IStorageFile **files = NULL, *file;
    IVectorView_StorageFile *list;
    WCHAR *buffer, *name, path[MAX_PATH];
    UINT32 count = 0;
    HRESULT hr;

    TRACE( "iface %p, operation %p.\n", iface, operation );

    *operation = NULL;
    if (!(buffer = malloc( 32768 * sizeof(WCHAR) ))) return E_OUTOFMEMORY;
    if (GetEnvironmentVariableW( L"WINE_UWP_PICKER_FILE", buffer, 32767 ))
    {
        if (!matches_filter( impl->filter, buffer )) buffer[0] = 0;
        buffer[wcslen( buffer ) + 1] = 0;
    }
    else if (!dialogs_enabled() || !open_dialog( impl->filter, impl->location, TRUE, buffer, 32768 ))
        buffer[0] = buffer[1] = 0;

    /* one full path, or a directory followed by the names in it */
    name = buffer + wcslen( buffer ) + 1;
    if (!*name && *buffer) name = buffer;
    for (; *buffer && *name; name += wcslen( name ) + 1)
    {
        IStorageFile **grown;
        if (name == buffer) lstrcpynW( path, buffer, ARRAY_SIZE(path) );
        else swprintf( path, ARRAY_SIZE(path), L"%s\\%s", buffer, name );
        if (FAILED(storage_file_from_path( path, &file ))) continue;
        if (!(grown = realloc( files, (count + 1) * sizeof(*files) )))
        {
            IStorageFile_Release( file );
            break;
        }
        files = grown;
        files[count++] = file;
        if (name == buffer) break;
    }
    free( buffer );

    TRACE( "picked %u files.\n", count );
    if (FAILED(hr = file_list_create( files, count, &list )))
    {
        while (count) IStorageFile_Release( files[--count] );
        free( files );
        return hr;
    }
    hr = async_operation_files_create( (IUnknown *)iface, (IUnknown *)list, return_result, operation );
    IVectorView_StorageFile_Release( list );
    return hr;
}

static const struct IFileOpenPickerVtbl open_picker_vtbl =
{
    open_picker_QueryInterface,
    open_picker_AddRef,
    open_picker_Release,
    /* IInspectable methods */
    open_picker_GetIids,
    open_picker_GetRuntimeClassName,
    open_picker_GetTrustLevel,
    /* IFileOpenPicker methods */
    open_picker_get_ViewMode,
    open_picker_put_ViewMode,
    open_picker_get_SettingsIdentifier,
    open_picker_put_SettingsIdentifier,
    open_picker_get_SuggestedStartLocation,
    open_picker_put_SuggestedStartLocation,
    open_picker_get_CommitButtonText,
    open_picker_put_CommitButtonText,
    open_picker_get_FileTypeFilter,
    open_picker_PickSingleFileAsync,
    open_picker_PickMultipleFilesAsync,
};

DEFINE_IINSPECTABLE( folder_picker, IFolderPicker, struct picker, IFileOpenPicker_iface )

static HRESULT WINAPI folder_picker_get_ViewMode( IFolderPicker *iface, PickerViewMode *value )
{
    return open_picker_get_ViewMode( &impl_from_IFolderPicker( iface )->IFileOpenPicker_iface, value );
}

static HRESULT WINAPI folder_picker_put_ViewMode( IFolderPicker *iface, PickerViewMode value )
{
    return open_picker_put_ViewMode( &impl_from_IFolderPicker( iface )->IFileOpenPicker_iface, value );
}

static HRESULT WINAPI folder_picker_get_SettingsIdentifier( IFolderPicker *iface, HSTRING *value )
{
    return open_picker_get_SettingsIdentifier( &impl_from_IFolderPicker( iface )->IFileOpenPicker_iface, value );
}

static HRESULT WINAPI folder_picker_put_SettingsIdentifier( IFolderPicker *iface, HSTRING value )
{
    return open_picker_put_SettingsIdentifier( &impl_from_IFolderPicker( iface )->IFileOpenPicker_iface, value );
}

static HRESULT WINAPI folder_picker_get_SuggestedStartLocation( IFolderPicker *iface, PickerLocationId *value )
{
    return open_picker_get_SuggestedStartLocation( &impl_from_IFolderPicker( iface )->IFileOpenPicker_iface, value );
}

static HRESULT WINAPI folder_picker_put_SuggestedStartLocation( IFolderPicker *iface, PickerLocationId value )
{
    return open_picker_put_SuggestedStartLocation( &impl_from_IFolderPicker( iface )->IFileOpenPicker_iface, value );
}

static HRESULT WINAPI folder_picker_get_CommitButtonText( IFolderPicker *iface, HSTRING *value )
{
    return open_picker_get_CommitButtonText( &impl_from_IFolderPicker( iface )->IFileOpenPicker_iface, value );
}

static HRESULT WINAPI folder_picker_put_CommitButtonText( IFolderPicker *iface, HSTRING value )
{
    return open_picker_put_CommitButtonText( &impl_from_IFolderPicker( iface )->IFileOpenPicker_iface, value );
}

static HRESULT WINAPI folder_picker_get_FileTypeFilter( IFolderPicker *iface, IVector_HSTRING **value )
{
    return open_picker_get_FileTypeFilter( &impl_from_IFolderPicker( iface )->IFileOpenPicker_iface, value );
}

static HRESULT WINAPI folder_picker_PickSingleFolderAsync( IFolderPicker *iface, IAsyncOperation_StorageFolder **operation )
{
    IStorageFolder *folder = NULL;
    WCHAR path[MAX_PATH];
    HRESULT hr;

    TRACE( "iface %p, operation %p.\n", iface, operation );

    if (dialogs_enabled() && folder_dialog( path )) storage_folder_from_path( path, &folder );
    TRACE( "picked %s.\n", debugstr_w( folder ? path : NULL ) );
    hr = async_operation_folder_create( (IUnknown *)iface, (IUnknown *)folder, return_result, operation );
    if (folder) IStorageFolder_Release( folder );
    return hr;
}

static const struct IFolderPickerVtbl folder_picker_vtbl =
{
    folder_picker_QueryInterface,
    folder_picker_AddRef,
    folder_picker_Release,
    /* IInspectable methods */
    folder_picker_GetIids,
    folder_picker_GetRuntimeClassName,
    folder_picker_GetTrustLevel,
    /* IFolderPicker methods */
    folder_picker_get_ViewMode,
    folder_picker_put_ViewMode,
    folder_picker_get_SettingsIdentifier,
    folder_picker_put_SettingsIdentifier,
    folder_picker_get_SuggestedStartLocation,
    folder_picker_put_SuggestedStartLocation,
    folder_picker_get_CommitButtonText,
    folder_picker_put_CommitButtonText,
    folder_picker_get_FileTypeFilter,
    folder_picker_PickSingleFolderAsync,
};

static HRESULT picker_create( BOOL folder, IInspectable **out )
{
    struct picker *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (FAILED(hr = hstring_vector_create( &impl->filter )))
    {
        free( impl );
        return hr;
    }
    impl->IFileOpenPicker_iface.lpVtbl = &open_picker_vtbl;
    impl->IFolderPicker_iface.lpVtbl = &folder_picker_vtbl;
    impl->ref = 1;
    impl->folder = folder;
    impl->location = PickerLocationId_Unspecified;
    *out = folder ? (IInspectable *)&impl->IFolderPicker_iface : (IInspectable *)&impl->IFileOpenPicker_iface;
    return S_OK;
}

/* FileSavePicker */

struct save_picker
{
    IFileSavePicker IFileSavePicker_iface;
    LONG ref;
    PickerLocationId location;
    HSTRING settings_id;
    HSTRING commit_text;
    HSTRING default_ext;
    HSTRING file_name;
    IStorageFile *suggested_file;
    IMap_HSTRING_IVector_HSTRING *choices;
};

static inline struct save_picker *impl_from_IFileSavePicker( IFileSavePicker *iface )
{
    return CONTAINING_RECORD( iface, struct save_picker, IFileSavePicker_iface );
}

static HRESULT WINAPI save_picker_QueryInterface( IFileSavePicker *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IFileSavePicker ))
    {
        *out = iface;
        IInspectable_AddRef( (IInspectable *)iface );
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI save_picker_AddRef( IFileSavePicker *iface )
{
    return InterlockedIncrement( &impl_from_IFileSavePicker( iface )->ref );
}

static ULONG WINAPI save_picker_Release( IFileSavePicker *iface )
{
    struct save_picker *impl = impl_from_IFileSavePicker( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    if (!ref)
    {
        WindowsDeleteString( impl->settings_id );
        WindowsDeleteString( impl->commit_text );
        WindowsDeleteString( impl->default_ext );
        WindowsDeleteString( impl->file_name );
        if (impl->suggested_file) IStorageFile_Release( impl->suggested_file );
        IMap_HSTRING_IVector_HSTRING_Release( impl->choices );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI save_picker_GetIids( IFileSavePicker *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI save_picker_GetRuntimeClassName( IFileSavePicker *iface, HSTRING *class_name )
{
    return WindowsCreateString( RuntimeClass_Windows_Storage_Pickers_FileSavePicker,
                                wcslen( RuntimeClass_Windows_Storage_Pickers_FileSavePicker ), class_name );
}

static HRESULT WINAPI save_picker_GetTrustLevel( IFileSavePicker *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI save_picker_get_SettingsIdentifier( IFileSavePicker *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IFileSavePicker( iface )->settings_id, value );
}

static HRESULT WINAPI save_picker_put_SettingsIdentifier( IFileSavePicker *iface, HSTRING value )
{
    return replace_string( &impl_from_IFileSavePicker( iface )->settings_id, value );
}

static HRESULT WINAPI save_picker_get_SuggestedStartLocation( IFileSavePicker *iface, PickerLocationId *value )
{
    *value = impl_from_IFileSavePicker( iface )->location;
    return S_OK;
}

static HRESULT WINAPI save_picker_put_SuggestedStartLocation( IFileSavePicker *iface, PickerLocationId value )
{
    TRACE( "iface %p, value %d.\n", iface, value );
    impl_from_IFileSavePicker( iface )->location = value;
    return S_OK;
}

static HRESULT WINAPI save_picker_get_CommitButtonText( IFileSavePicker *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IFileSavePicker( iface )->commit_text, value );
}

static HRESULT WINAPI save_picker_put_CommitButtonText( IFileSavePicker *iface, HSTRING value )
{
    return replace_string( &impl_from_IFileSavePicker( iface )->commit_text, value );
}

static HRESULT WINAPI save_picker_get_FileTypeChoices( IFileSavePicker *iface, IMap_HSTRING_IVector_HSTRING **value )
{
    IMap_HSTRING_IVector_HSTRING_AddRef( (*value = impl_from_IFileSavePicker( iface )->choices) );
    return S_OK;
}

static HRESULT WINAPI save_picker_get_DefaultFileExtension( IFileSavePicker *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IFileSavePicker( iface )->default_ext, value );
}

static HRESULT WINAPI save_picker_put_DefaultFileExtension( IFileSavePicker *iface, HSTRING value )
{
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    return replace_string( &impl_from_IFileSavePicker( iface )->default_ext, value );
}

static HRESULT WINAPI save_picker_get_SuggestedSaveFile( IFileSavePicker *iface, IStorageFile **value )
{
    if ((*value = impl_from_IFileSavePicker( iface )->suggested_file)) IStorageFile_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI save_picker_put_SuggestedSaveFile( IFileSavePicker *iface, IStorageFile *value )
{
    struct save_picker *impl = impl_from_IFileSavePicker( iface );

    if (value) IStorageFile_AddRef( value );
    if (impl->suggested_file) IStorageFile_Release( impl->suggested_file );
    impl->suggested_file = value;
    return S_OK;
}

static HRESULT WINAPI save_picker_get_SuggestedFileName( IFileSavePicker *iface, HSTRING *value )
{
    return WindowsDuplicateString( impl_from_IFileSavePicker( iface )->file_name, value );
}

static HRESULT WINAPI save_picker_put_SuggestedFileName( IFileSavePicker *iface, HSTRING value )
{
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    return replace_string( &impl_from_IFileSavePicker( iface )->file_name, value );
}

static HRESULT WINAPI save_picker_PickSaveFileAsync( IFileSavePicker *iface, IAsyncOperation_StorageFile **operation )
{
    struct save_picker *impl = impl_from_IFileSavePicker( iface );
    IStorageFile *file = NULL;
    WCHAR path[MAX_PATH];
    BOOL picked;
    HANDLE handle;
    HRESULT hr;

    TRACE( "iface %p, operation %p.\n", iface, operation );

    *operation = NULL;
    picked = GetEnvironmentVariableW( L"WINE_UWP_PICKER_SAVE_FILE", path, ARRAY_SIZE(path) ) ||
             (dialogs_enabled() && save_dialog( impl->choices, impl->file_name, impl->default_ext, impl->location,
                                                path, ARRAY_SIZE(path) ));
    if (picked)
    {
        /* a save picker hands back a file that exists */
        handle = CreateFileW( path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
        if (handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
        CloseHandle( handle );
        storage_file_from_path( path, &file );
    }
    TRACE( "picked %s.\n", debugstr_w( file ? path : NULL ) );
    hr = async_operation_file_create( (IUnknown *)iface, (IUnknown *)file, return_result, operation );
    if (file) IStorageFile_Release( file );
    return hr;
}

static const struct IFileSavePickerVtbl save_picker_vtbl =
{
    save_picker_QueryInterface,
    save_picker_AddRef,
    save_picker_Release,
    /* IInspectable methods */
    save_picker_GetIids,
    save_picker_GetRuntimeClassName,
    save_picker_GetTrustLevel,
    /* IFileSavePicker methods */
    save_picker_get_SettingsIdentifier,
    save_picker_put_SettingsIdentifier,
    save_picker_get_SuggestedStartLocation,
    save_picker_put_SuggestedStartLocation,
    save_picker_get_CommitButtonText,
    save_picker_put_CommitButtonText,
    save_picker_get_FileTypeChoices,
    save_picker_get_DefaultFileExtension,
    save_picker_put_DefaultFileExtension,
    save_picker_get_SuggestedSaveFile,
    save_picker_put_SuggestedSaveFile,
    save_picker_get_SuggestedFileName,
    save_picker_put_SuggestedFileName,
    save_picker_PickSaveFileAsync,
};

static HRESULT save_picker_create( IInspectable **out )
{
    struct save_picker *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (FAILED(hr = file_type_choices_create( &impl->choices )))
    {
        free( impl );
        return hr;
    }
    impl->IFileSavePicker_iface.lpVtbl = &save_picker_vtbl;
    impl->ref = 1;
    impl->location = PickerLocationId_Unspecified;
    *out = (IInspectable *)&impl->IFileSavePicker_iface;
    return S_OK;
}

/* The three factories */

enum picker_kind { OPEN_PICKER, SAVE_PICKER, FOLDER_PICKER };

struct factory
{
    IActivationFactory IActivationFactory_iface;
    enum picker_kind kind;
    const WCHAR *name;
};

static inline struct factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct factory, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = iface;
        return S_OK;
    }
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    return 2;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    return 1;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    const WCHAR *name = impl_from_IActivationFactory( iface )->name;
    return WindowsCreateString( name, wcslen( name ), class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "%s, instance %p.\n", debugstr_w( impl->name ), instance );

    switch (impl->kind)
    {
    case OPEN_PICKER:   return picker_create( FALSE, instance );
    case FOLDER_PICKER: return picker_create( TRUE, instance );
    case SAVE_PICKER:   return save_picker_create( instance );
    }
    return E_UNEXPECTED;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

static struct factory factories[] =
{
    {{&factory_vtbl}, OPEN_PICKER, RuntimeClass_Windows_Storage_Pickers_FileOpenPicker},
    {{&factory_vtbl}, SAVE_PICKER, RuntimeClass_Windows_Storage_Pickers_FileSavePicker},
    {{&factory_vtbl}, FOLDER_PICKER, RuntimeClass_Windows_Storage_Pickers_FolderPicker},
};

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid( clsid ), debugstr_guid( riid ), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *name = WindowsGetStringRawBuffer( classid, NULL );
    UINT i;

    TRACE( "classid %s, factory %p.\n", debugstr_hstring( classid ), factory );

    *factory = NULL;
    for (i = 0; i < ARRAY_SIZE(factories); i++)
    {
        if (wcscmp( name, factories[i].name )) continue;
        *factory = &factories[i].IActivationFactory_iface;
        return S_OK;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}
