/* WinRT Windows.UI.Text.Core.CoreTextEditContext Implementation
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

#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(coreinputview);

/* An inert edit context: there is no text-service framework (IME/soft
 * keyboard) behind it, so the events never fire.  Keyboard input still
 * reaches the control through the normal WM_CHAR path. */
/* Undocumented interface that Windows.UI.Xaml's CTextInputPrivateSettings
 * queries on the edit context (Windows.UI.Core.TextInput.dll's CEditContext
 * exposes it as IPrivateEditContext).  Layout recovered from the native
 * vtable; the setters only carry input-settings hints. */
static const GUID IID_IPrivateEditContext = { 0x2f292ff4, 0x823c, 0x4a17, { 0x9f, 0xd5, 0x18, 0x8c, 0x56, 0xe2, 0x52, 0xf8 } };

typedef struct IPrivateEditContext IPrivateEditContext;
struct IPrivateEditContextVtbl
{
    HRESULT (WINAPI *QueryInterface)( IPrivateEditContext *iface, REFIID iid, void **out );
    ULONG (WINAPI *AddRef)( IPrivateEditContext *iface );
    ULONG (WINAPI *Release)( IPrivateEditContext *iface );
    HRESULT (WINAPI *put_InputSettings)( IPrivateEditContext *iface, DWORD settings );
    HRESULT (WINAPI *get_IsEmbeddedHandwritingViewEnabled)( IPrivateEditContext *iface, boolean *value );
    HRESULT (WINAPI *put_IsEmbeddedHandwritingViewEnabled)( IPrivateEditContext *iface, boolean value );
    HRESULT (WINAPI *get_IsEmbeddedHandwritingViewVisible)( IPrivateEditContext *iface, boolean *value );
    HRESULT (WINAPI *put_IsEmbeddedHandwritingViewVisible)( IPrivateEditContext *iface, boolean value );
    HRESULT (WINAPI *get_IsMultiLine)( IPrivateEditContext *iface, boolean *value );
    HRESULT (WINAPI *put_IsMultiLine)( IPrivateEditContext *iface, boolean value );
    HRESULT (WINAPI *FocusLeaveCompleted)( IPrivateEditContext *iface );
    HRESULT (WINAPI *ReplaceTextEx)( IPrivateEditContext *iface, void *a, void *b, void *c, void *d );
    HRESULT (WINAPI *SetSmartLink)( IPrivateEditContext *iface, void *a, void *b );
};
struct IPrivateEditContext { const struct IPrivateEditContextVtbl *lpVtbl; };

struct core_text_edit_context
{
    ICoreTextEditContext ICoreTextEditContext_iface;
    ICoreTextEditContext2 ICoreTextEditContext2_iface;
    IPrivateEditContext IPrivateEditContext_iface;
    LONG ref;
    DWORD input_settings;
    boolean handwriting_enabled, handwriting_visible, multi_line;
    HSTRING name;
    CoreTextInputScope input_scope;
    boolean read_only;
    CoreTextInputPaneDisplayPolicy pane_policy;
    LONG next_token;
};

static inline struct core_text_edit_context *impl_from_ICoreTextEditContext( ICoreTextEditContext *iface )
{
    return CONTAINING_RECORD( iface, struct core_text_edit_context, ICoreTextEditContext_iface );
}

static HRESULT WINAPI edit_context_QueryInterface( ICoreTextEditContext *iface, REFIID iid, void **out )
{
    struct core_text_edit_context *impl = impl_from_ICoreTextEditContext( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_ICoreTextEditContext ))
    {
        *out = &impl->ICoreTextEditContext_iface;
        IUnknown_AddRef( (IUnknown *)*out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_ICoreTextEditContext2 ))
    {
        *out = &impl->ICoreTextEditContext2_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IPrivateEditContext ))
    {
        *out = &impl->IPrivateEditContext_iface;
        IUnknown_AddRef( (IUnknown *)*out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI edit_context_AddRef( ICoreTextEditContext *iface )
{
    struct core_text_edit_context *impl = impl_from_ICoreTextEditContext( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI edit_context_Release( ICoreTextEditContext *iface )
{
    struct core_text_edit_context *impl = impl_from_ICoreTextEditContext( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        WindowsDeleteString( impl->name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI edit_context_GetIids( ICoreTextEditContext *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI edit_context_GetRuntimeClassName( ICoreTextEditContext *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.UI.Text.Core.CoreTextEditContext";
    TRACE( "iface %p, class_name %p.\n", iface, class_name );
    return WindowsCreateString( name, ARRAY_SIZE( name ) - 1, class_name );
}

static HRESULT WINAPI edit_context_GetTrustLevel( ICoreTextEditContext *iface, TrustLevel *trust_level )
{
    TRACE( "iface %p, trust_level %p.\n", iface, trust_level );
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI edit_context_get_Name( ICoreTextEditContext *iface, HSTRING *value )
{
    struct core_text_edit_context *impl = impl_from_ICoreTextEditContext( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    return WindowsDuplicateString( impl->name, value );
}

static HRESULT WINAPI edit_context_put_Name( ICoreTextEditContext *iface, HSTRING value )
{
    struct core_text_edit_context *impl = impl_from_ICoreTextEditContext( iface );
    TRACE( "iface %p, value %s.\n", iface, debugstr_hstring( value ) );
    WindowsDeleteString( impl->name );
    return WindowsDuplicateString( value, &impl->name );
}

static HRESULT WINAPI edit_context_get_InputScope( ICoreTextEditContext *iface, CoreTextInputScope *value )
{
    struct core_text_edit_context *impl = impl_from_ICoreTextEditContext( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->input_scope;
    return S_OK;
}

static HRESULT WINAPI edit_context_put_InputScope( ICoreTextEditContext *iface, CoreTextInputScope value )
{
    struct core_text_edit_context *impl = impl_from_ICoreTextEditContext( iface );
    TRACE( "iface %p, value %d.\n", iface, value );
    impl->input_scope = value;
    return S_OK;
}

static HRESULT WINAPI edit_context_get_IsReadOnly( ICoreTextEditContext *iface, boolean *value )
{
    struct core_text_edit_context *impl = impl_from_ICoreTextEditContext( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->read_only;
    return S_OK;
}

static HRESULT WINAPI edit_context_put_IsReadOnly( ICoreTextEditContext *iface, boolean value )
{
    struct core_text_edit_context *impl = impl_from_ICoreTextEditContext( iface );
    TRACE( "iface %p, value %d.\n", iface, value );
    impl->read_only = value;
    return S_OK;
}

static HRESULT WINAPI edit_context_get_InputPaneDisplayPolicy( ICoreTextEditContext *iface, CoreTextInputPaneDisplayPolicy *value )
{
    struct core_text_edit_context *impl = impl_from_ICoreTextEditContext( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->pane_policy;
    return S_OK;
}

static HRESULT WINAPI edit_context_put_InputPaneDisplayPolicy( ICoreTextEditContext *iface, CoreTextInputPaneDisplayPolicy value )
{
    struct core_text_edit_context *impl = impl_from_ICoreTextEditContext( iface );
    TRACE( "iface %p, value %d.\n", iface, value );
    impl->pane_policy = value;
    return S_OK;
}

static HRESULT add_event( ICoreTextEditContext *iface, const char *name, void *handler, EventRegistrationToken *cookie )
{
    struct core_text_edit_context *impl = impl_from_ICoreTextEditContext( iface );
    TRACE( "iface %p, %s handler %p, cookie %p: event never fires.\n", iface, name, handler, cookie );
    if (!handler) return E_POINTER;
    cookie->value = InterlockedIncrement( &impl->next_token );
    return S_OK;
}

static HRESULT remove_event( ICoreTextEditContext *iface, const char *name, EventRegistrationToken cookie )
{
    TRACE( "iface %p, %s cookie %I64x.\n", iface, name, cookie.value );
    return S_OK;
}

#define DEFINE_EVENT(name, type)                                                                          \
    static HRESULT WINAPI edit_context_add_##name( ICoreTextEditContext *iface, type *handler,           \
                                                   EventRegistrationToken *cookie )                      \
    {                                                                                                     \
        return add_event( iface, #name, handler, cookie );                                                \
    }                                                                                                     \
    static HRESULT WINAPI edit_context_remove_##name( ICoreTextEditContext *iface, EventRegistrationToken cookie ) \
    {                                                                                                     \
        return remove_event( iface, #name, cookie );                                                      \
    }

DEFINE_EVENT( TextRequested, ITypedEventHandler_CoreTextEditContext_CoreTextTextRequestedEventArgs )
DEFINE_EVENT( SelectionRequested, ITypedEventHandler_CoreTextEditContext_CoreTextSelectionRequestedEventArgs )
DEFINE_EVENT( LayoutRequested, ITypedEventHandler_CoreTextEditContext_CoreTextLayoutRequestedEventArgs )
DEFINE_EVENT( TextUpdating, ITypedEventHandler_CoreTextEditContext_CoreTextTextUpdatingEventArgs )
DEFINE_EVENT( SelectionUpdating, ITypedEventHandler_CoreTextEditContext_CoreTextSelectionUpdatingEventArgs )
DEFINE_EVENT( FormatUpdating, ITypedEventHandler_CoreTextEditContext_CoreTextFormatUpdatingEventArgs )
DEFINE_EVENT( CompositionStarted, ITypedEventHandler_CoreTextEditContext_CoreTextCompositionStartedEventArgs )
DEFINE_EVENT( CompositionCompleted, ITypedEventHandler_CoreTextEditContext_CoreTextCompositionCompletedEventArgs )
DEFINE_EVENT( FocusRemoved, ITypedEventHandler_CoreTextEditContext_IInspectable )

static HRESULT WINAPI edit_context_NotifyFocusEnter( ICoreTextEditContext *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static HRESULT WINAPI edit_context_NotifyFocusLeave( ICoreTextEditContext *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static HRESULT WINAPI edit_context_NotifyTextChanged( ICoreTextEditContext *iface, CoreTextRange modified,
                                                      INT32 new_length, CoreTextRange selection )
{
    TRACE( "iface %p, modified %d-%d, new_length %d, selection %d-%d.\n", iface, modified.StartCaretPosition,
           modified.EndCaretPosition, new_length, selection.StartCaretPosition, selection.EndCaretPosition );
    return S_OK;
}

static HRESULT WINAPI edit_context_NotifySelectionChanged( ICoreTextEditContext *iface, CoreTextRange selection )
{
    TRACE( "iface %p, selection %d-%d.\n", iface, selection.StartCaretPosition, selection.EndCaretPosition );
    return S_OK;
}

static HRESULT WINAPI edit_context_NotifyLayoutChanged( ICoreTextEditContext *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static const struct ICoreTextEditContextVtbl edit_context_vtbl =
{
    edit_context_QueryInterface,
    edit_context_AddRef,
    edit_context_Release,
    /* IInspectable methods */
    edit_context_GetIids,
    edit_context_GetRuntimeClassName,
    edit_context_GetTrustLevel,
    /* ICoreTextEditContext methods */
    edit_context_get_Name,
    edit_context_put_Name,
    edit_context_get_InputScope,
    edit_context_put_InputScope,
    edit_context_get_IsReadOnly,
    edit_context_put_IsReadOnly,
    edit_context_get_InputPaneDisplayPolicy,
    edit_context_put_InputPaneDisplayPolicy,
    edit_context_add_TextRequested,
    edit_context_remove_TextRequested,
    edit_context_add_SelectionRequested,
    edit_context_remove_SelectionRequested,
    edit_context_add_LayoutRequested,
    edit_context_remove_LayoutRequested,
    edit_context_add_TextUpdating,
    edit_context_remove_TextUpdating,
    edit_context_add_SelectionUpdating,
    edit_context_remove_SelectionUpdating,
    edit_context_add_FormatUpdating,
    edit_context_remove_FormatUpdating,
    edit_context_add_CompositionStarted,
    edit_context_remove_CompositionStarted,
    edit_context_add_CompositionCompleted,
    edit_context_remove_CompositionCompleted,
    edit_context_add_FocusRemoved,
    edit_context_remove_FocusRemoved,
    edit_context_NotifyFocusEnter,
    edit_context_NotifyFocusLeave,
    edit_context_NotifyTextChanged,
    edit_context_NotifySelectionChanged,
    edit_context_NotifyLayoutChanged,
};

static inline struct core_text_edit_context *impl_from_IPrivateEditContext( IPrivateEditContext *iface )
{
    return CONTAINING_RECORD( iface, struct core_text_edit_context, IPrivateEditContext_iface );
}

static HRESULT WINAPI private_QueryInterface( IPrivateEditContext *iface, REFIID iid, void **out )
{
    struct core_text_edit_context *impl = impl_from_IPrivateEditContext( iface );
    return ICoreTextEditContext_QueryInterface( &impl->ICoreTextEditContext_iface, iid, out );
}

static ULONG WINAPI private_AddRef( IPrivateEditContext *iface )
{
    struct core_text_edit_context *impl = impl_from_IPrivateEditContext( iface );
    return ICoreTextEditContext_AddRef( &impl->ICoreTextEditContext_iface );
}

static ULONG WINAPI private_Release( IPrivateEditContext *iface )
{
    struct core_text_edit_context *impl = impl_from_IPrivateEditContext( iface );
    return ICoreTextEditContext_Release( &impl->ICoreTextEditContext_iface );
}

static HRESULT WINAPI private_put_InputSettings( IPrivateEditContext *iface, DWORD settings )
{
    struct core_text_edit_context *impl = impl_from_IPrivateEditContext( iface );
    TRACE( "iface %p, settings %#lx.\n", iface, settings );
    impl->input_settings = settings;
    return S_OK;
}

#define DEFINE_BOOL_PROP(name, field)                                                                 \
    static HRESULT WINAPI private_get_##name( IPrivateEditContext *iface, boolean *value )             \
    {                                                                                                 \
        struct core_text_edit_context *impl = impl_from_IPrivateEditContext( iface );                 \
        TRACE( "iface %p, value %p.\n", iface, value );                                               \
        *value = impl->field;                                                                         \
        return S_OK;                                                                                  \
    }                                                                                                 \
    static HRESULT WINAPI private_put_##name( IPrivateEditContext *iface, boolean value )              \
    {                                                                                                 \
        struct core_text_edit_context *impl = impl_from_IPrivateEditContext( iface );                 \
        TRACE( "iface %p, value %d.\n", iface, value );                                               \
        impl->field = value;                                                                          \
        return S_OK;                                                                                  \
    }

DEFINE_BOOL_PROP( IsEmbeddedHandwritingViewEnabled, handwriting_enabled )
DEFINE_BOOL_PROP( IsEmbeddedHandwritingViewVisible, handwriting_visible )
DEFINE_BOOL_PROP( IsMultiLine, multi_line )

static HRESULT WINAPI private_FocusLeaveCompleted( IPrivateEditContext *iface )
{
    TRACE( "iface %p.\n", iface );
    return S_OK;
}

static HRESULT WINAPI private_ReplaceTextEx( IPrivateEditContext *iface, void *a, void *b, void *c, void *d )
{
    FIXME( "iface %p, %p %p %p %p stub!\n", iface, a, b, c, d );
    return E_NOTIMPL;
}

static HRESULT WINAPI private_SetSmartLink( IPrivateEditContext *iface, void *a, void *b )
{
    FIXME( "iface %p, %p %p stub!\n", iface, a, b );
    return E_NOTIMPL;
}

static const struct IPrivateEditContextVtbl private_vtbl =
{
    private_QueryInterface,
    private_AddRef,
    private_Release,
    private_put_InputSettings,
    private_get_IsEmbeddedHandwritingViewEnabled,
    private_put_IsEmbeddedHandwritingViewEnabled,
    private_get_IsEmbeddedHandwritingViewVisible,
    private_put_IsEmbeddedHandwritingViewVisible,
    private_get_IsMultiLine,
    private_put_IsMultiLine,
    private_FocusLeaveCompleted,
    private_ReplaceTextEx,
    private_SetSmartLink,
};


/* ICoreTextEditContext2 adds one event, raised when a focus-leave notification has been processed.
 * Wine's edit context has no asynchronous focus handshake, so the event never fires - but the
 * registration has to succeed: Solitaire QI's for this interface while building its first page and
 * faults on the null it gets back. */
DEFINE_IINSPECTABLE( edit_context2, ICoreTextEditContext2, struct core_text_edit_context, ICoreTextEditContext_iface )

static HRESULT WINAPI edit_context2_add_NotifyFocusLeaveCompleted( ICoreTextEditContext2 *iface,
        ITypedEventHandler_CoreTextEditContext_IInspectable *handler, EventRegistrationToken *cookie )
{
    FIXME( "iface %p, handler %p, cookie %p stub!\n", iface, handler, cookie );
    if (!cookie) return E_POINTER;
    cookie->value = 0;
    return S_OK;
}

static HRESULT WINAPI edit_context2_remove_NotifyFocusLeaveCompleted( ICoreTextEditContext2 *iface,
                                                                      EventRegistrationToken cookie )
{
    FIXME( "iface %p, cookie %#I64x stub!\n", iface, cookie.value );
    return S_OK;
}

static const struct ICoreTextEditContext2Vtbl edit_context2_vtbl =
{
    edit_context2_QueryInterface,
    edit_context2_AddRef,
    edit_context2_Release,
    /* IInspectable methods */
    edit_context2_GetIids,
    edit_context2_GetRuntimeClassName,
    edit_context2_GetTrustLevel,
    /* ICoreTextEditContext2 methods */
    edit_context2_add_NotifyFocusLeaveCompleted,
    edit_context2_remove_NotifyFocusLeaveCompleted,
};

HRESULT core_text_edit_context_create( ICoreTextEditContext **out )
{
    struct core_text_edit_context *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->ICoreTextEditContext_iface.lpVtbl = &edit_context_vtbl;
    impl->ICoreTextEditContext2_iface.lpVtbl = &edit_context2_vtbl;
    impl->IPrivateEditContext_iface.lpVtbl = &private_vtbl;
    impl->ref = 1;
    impl->input_scope = CoreTextInputScope_Default;
    impl->pane_policy = CoreTextInputPaneDisplayPolicy_Automatic;
    *out = &impl->ICoreTextEditContext_iface;
    return S_OK;
}
