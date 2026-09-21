/*
 * Copyright 2018 Andrey Gusev
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
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "wine/debug.h"

#include "interactioncontext.h"

WINE_DEFAULT_DEBUG_CHANNEL(ninput);

/* Tap recognition: a pointer that goes down and up without moving far or waiting long is a tap.
 * Manipulations, holds, drags and cross-slides are not recognised (Windows.UI.Xaml scrolls
 * through DirectManipulation instead). */
#define TAP_SLOP_PIXELS 10
#define TAP_TIMEOUT_MS 500
#define DOUBLE_TAP_TIMEOUT_MS 500

struct tracked_pointer
{
    UINT32 id;
    BOOL down;
    BOOL moved_too_far;
    POINT down_pixel;
    POINT down_himetric;
    DWORD down_time;
};

struct interaction_context
{
    BOOL filter_pointers;
    BOOL ui_feedback;
    UINT32 units;   /* 0 = unset (pixels, see pointer_location), 1 = screen pixels, 2 = HIMETRIC */
    /* single-pointer manipulation (translation only) */
    BOOL manip_active;
    UINT32 manip_pointer;
    POINT manip_last;
    float manip_cum_x, manip_cum_y;
    INTERACTION_CONFIGURATION_FLAGS config_flags[INTERACTION_ID_CROSS_SLIDE];
    INTERACTION_CONTEXT_OUTPUT_CALLBACK callback;
    void *callback_data;
    struct tracked_pointer pointers[8];
    /* last tap, to count double taps */
    DWORD last_tap_time;
    POINT last_tap_pixel;
    UINT32 last_tap_count;
};

static struct tracked_pointer *find_pointer(struct interaction_context *context, UINT32 id, BOOL create)
{
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(context->pointers); i++)
        if (context->pointers[i].id == id && context->pointers[i].down) return &context->pointers[i];
    if (!create) return NULL;
    for (i = 0; i < ARRAY_SIZE(context->pointers); i++)
        if (!context->pointers[i].down)
        {
            memset(&context->pointers[i], 0, sizeof(context->pointers[i]));
            context->pointers[i].id = id;
            return &context->pointers[i];
        }
    return NULL;
}

static POINT pointer_location(const struct interaction_context *context, const POINTER_INFO *info)
{
    /* Documented default unit is HIMETRIC, but Windows.UI.Xaml (the one consumer here) never
     * sets the unit property and feeds the deltas straight into DIP-based manipulation
     * events: a 160 px drag reported in HIMETRIC (4233) panned the graph by 26 screens.
     * Report pixels unless the caller asked for HIMETRIC explicitly. */
    if (context->units == 2 && (info->ptHimetricLocation.x || info->ptHimetricLocation.y))
        return info->ptHimetricLocation;
    return info->ptPixelLocation;
}

static void send_manipulation(struct interaction_context *context, const POINTER_INFO *info,
                              INTERACTION_FLAGS flags, float dx, float dy)
{
    INTERACTION_CONFIGURATION_FLAGS config = context->config_flags[INTERACTION_ID_MANIPULATION - 1];
    INTERACTION_CONTEXT_OUTPUT output;
    POINT pos = pointer_location(context, info);

    if (!(config & INTERACTION_CONFIGURATION_FLAG_MANIPULATION_TRANSLATION_X)) dx = 0;
    if (!(config & INTERACTION_CONFIGURATION_FLAG_MANIPULATION_TRANSLATION_Y)) dy = 0;
    context->manip_cum_x += dx;
    context->manip_cum_y += dy;

    memset(&output, 0, sizeof(output));
    output.interactionId = INTERACTION_ID_MANIPULATION;
    output.interactionFlags = flags;
    output.inputType = info->pointerType;
    output.x = pos.x;
    output.y = pos.y;
    output.arguments.manipulation.delta.translationX = dx;
    output.arguments.manipulation.delta.translationY = dy;
    output.arguments.manipulation.delta.scale = 1.0f;
    output.arguments.manipulation.cumulative.translationX = context->manip_cum_x;
    output.arguments.manipulation.cumulative.translationY = context->manip_cum_y;
    output.arguments.manipulation.cumulative.scale = 1.0f;
    output.arguments.manipulation.railsState = MANIPULATION_RAILS_STATE_FREE;
    TRACE("manipulation flags %#x delta %.1f,%.1f cumulative %.1f,%.1f at %.0f,%.0f\n", flags, dx, dy,
          context->manip_cum_x, context->manip_cum_y, output.x, output.y);
    context->callback(context->callback_data, &output);
}

static void process_manipulation(struct interaction_context *context, const POINTER_INFO *info,
                                 const struct tracked_pointer *pointer)
{
    INTERACTION_CONFIGURATION_FLAGS config = context->config_flags[INTERACTION_ID_MANIPULATION - 1];
    POINT pos = pointer_location(context, info);

    if (!(config & INTERACTION_CONFIGURATION_FLAG_MANIPULATION) || !context->callback) return;
    if (!(config & (INTERACTION_CONFIGURATION_FLAG_MANIPULATION_TRANSLATION_X |
                    INTERACTION_CONFIGURATION_FLAG_MANIPULATION_TRANSLATION_Y))) return;

    if (info->pointerFlags & (POINTER_FLAG_UP | POINTER_FLAG_CANCELED))
    {
        if (context->manip_active && context->manip_pointer == info->pointerId)
        {
            context->manip_active = FALSE;
            send_manipulation(context, info, (info->pointerFlags & POINTER_FLAG_CANCELED) ?
                              INTERACTION_FLAG_CANCEL : INTERACTION_FLAG_END, 0, 0);
        }
        return;
    }
    if (!context->manip_active)
    {
        if (!pointer->moved_too_far) return;
        context->manip_active = TRUE;
        context->manip_pointer = info->pointerId;
        context->manip_cum_x = context->manip_cum_y = 0;
        context->manip_last = pointer_location(context, &(POINTER_INFO){ .ptPixelLocation = pointer->down_pixel,
                                                                         .ptHimetricLocation = pointer->down_himetric });
        send_manipulation(context, info, INTERACTION_FLAG_BEGIN, 0, 0);
    }
    if (context->manip_pointer != info->pointerId) return;
    if (pos.x != context->manip_last.x || pos.y != context->manip_last.y)
    {
        send_manipulation(context, info, INTERACTION_FLAG_NONE,
                          (float)(pos.x - context->manip_last.x), (float)(pos.y - context->manip_last.y));
        context->manip_last = pos;
    }
}

static void process_pointer(struct interaction_context *context, const POINTER_INFO *info)
{
    struct tracked_pointer *pointer;
    DWORD now = info->dwTime ? info->dwTime : GetTickCount();

    TRACE("context %p pointer %u flags %#x at %ld,%ld time %lu (tick %lu)\n", context, info->pointerId,
          info->pointerFlags, info->ptPixelLocation.x, info->ptPixelLocation.y, info->dwTime, GetTickCount());

    if (info->pointerFlags & POINTER_FLAG_DOWN)
    {
        if (!(pointer = find_pointer(context, info->pointerId, TRUE))) return;
        pointer->down = TRUE;
        pointer->moved_too_far = FALSE;
        pointer->down_pixel = info->ptPixelLocation;
        pointer->down_himetric = info->ptHimetricLocation;
        pointer->down_time = now;
        return;
    }
    if (!(pointer = find_pointer(context, info->pointerId, FALSE))) return;
    if (abs(info->ptPixelLocation.x - pointer->down_pixel.x) > TAP_SLOP_PIXELS ||
        abs(info->ptPixelLocation.y - pointer->down_pixel.y) > TAP_SLOP_PIXELS)
        pointer->moved_too_far = TRUE;
    process_manipulation(context, info, pointer);
    if (info->pointerFlags & POINTER_FLAG_CANCELED)
    {
        pointer->down = FALSE;
        return;
    }
    if (info->pointerFlags & POINTER_FLAG_UP)
    {
        pointer->down = FALSE;
        if (pointer->moved_too_far || now - pointer->down_time > TAP_TIMEOUT_MS) return;
        if ((context->config_flags[INTERACTION_ID_TAP - 1] & INTERACTION_CONFIGURATION_FLAG_TAP) && context->callback)
        {
            INTERACTION_CONTEXT_OUTPUT output;
            UINT32 count = 1;

            if ((context->config_flags[INTERACTION_ID_TAP - 1] & INTERACTION_CONFIGURATION_FLAG_TAP_DOUBLE) &&
                context->last_tap_count && now - context->last_tap_time <= DOUBLE_TAP_TIMEOUT_MS &&
                abs(pointer->down_pixel.x - context->last_tap_pixel.x) <= TAP_SLOP_PIXELS &&
                abs(pointer->down_pixel.y - context->last_tap_pixel.y) <= TAP_SLOP_PIXELS)
                count = context->last_tap_count + 1;
            context->last_tap_time = now;
            context->last_tap_pixel = pointer->down_pixel;
            context->last_tap_count = count == 2 ? 0 : count;

            memset(&output, 0, sizeof(output));
            output.interactionId = INTERACTION_ID_TAP;
            output.interactionFlags = INTERACTION_FLAG_NONE;
            output.inputType = info->pointerType;
            {
                POINTER_INFO down = *info;
                POINT pos;
                down.ptPixelLocation = pointer->down_pixel;
                down.ptHimetricLocation = pointer->down_himetric;
                pos = pointer_location(context, &down);
                output.x = pos.x;
                output.y = pos.y;
            }
            output.arguments.tap.count = count;
            TRACE("tap at %.0f,%.0f count %u (pointer %u)\n", output.x, output.y, count, info->pointerId);
            context->callback(context->callback_data, &output);
        }
    }
}

static struct interaction_context *context_from_handle(HINTERACTIONCONTEXT handle)
{
    return (struct interaction_context *)handle;
}

HRESULT WINAPI BufferPointerPacketsInteractionContext(HINTERACTIONCONTEXT context,
        UINT32 entries_count, const POINTER_INFO *pointer_info)
{
    FIXME("context %p, entries_count %u, pointer_info %p: stub!\n", context, entries_count, pointer_info);

    if (!context)
        return E_HANDLE;
    if (!entries_count)
        return E_INVALIDARG;
    if (!pointer_info)
        return E_POINTER;

    return S_OK;
}

HRESULT WINAPI CreateInteractionContext(HINTERACTIONCONTEXT *handle)
{
    struct interaction_context *context;

    TRACE("handle %p.\n", handle);

    if (!handle)
        return E_POINTER;

    if (!(context = calloc(1, sizeof(*context))))
        return E_OUTOFMEMORY;

    context->filter_pointers = TRUE;
    context->ui_feedback = TRUE;

    TRACE("Created context %p.\n", context);

    *handle = (HINTERACTIONCONTEXT)context;

    return S_OK;
}

HRESULT WINAPI DestroyInteractionContext(HINTERACTIONCONTEXT handle)
{
    struct interaction_context *context = context_from_handle(handle);

    TRACE("context %p.\n", context);

    if (!context)
        return E_HANDLE;

    free(context);
    return S_OK;
}

HRESULT WINAPI GetPropertyInteractionContext(HINTERACTIONCONTEXT handle,
        INTERACTION_CONTEXT_PROPERTY property, UINT32 *value)
{
    struct interaction_context *context = context_from_handle(handle);

    TRACE("context %p, property %#x, value %p.\n", context, property, value);

    if (!context)
        return E_HANDLE;
    if (!value)
        return E_POINTER;

    switch (property)
    {
        case INTERACTION_CONTEXT_PROPERTY_MEASUREMENT_UNITS:
            *value = context->units == 1;
            return S_OK;

        case INTERACTION_CONTEXT_PROPERTY_INTERACTION_UI_FEEDBACK:
            *value = context->ui_feedback;
            return S_OK;

        case INTERACTION_CONTEXT_PROPERTY_FILTER_POINTERS:
            *value = context->filter_pointers;
            return S_OK;

        default:
            WARN("Invalid property %#x.\n", property);
            return E_INVALIDARG;
    }
}

HRESULT WINAPI GetStateInteractionContext(HINTERACTIONCONTEXT handle,
        const POINTER_INFO *pointer_info, INTERACTION_STATE *state)
{
    struct interaction_context *context = context_from_handle(handle);
    unsigned int i;

    TRACE("context %p, pointer_info %p, state %p.\n", context, pointer_info, state);

    if (!context)
        return E_HANDLE;
    if (!state)
        return E_POINTER;

    *state = INTERACTION_STATE_IDLE;
    for (i = 0; i < ARRAY_SIZE(context->pointers); i++)
    {
        if (!context->pointers[i].down) continue;
        if (pointer_info && context->pointers[i].id != pointer_info->pointerId) continue;
        *state = INTERACTION_STATE_IN_INTERACTION;
    }
    return S_OK;
}

HRESULT WINAPI SetPropertyInteractionContext(HINTERACTIONCONTEXT handle,
        INTERACTION_CONTEXT_PROPERTY property, UINT32 value)
{
    struct interaction_context *context = context_from_handle(handle);

    TRACE("context %p, property %#x, value %#x.\n", context, property, value);

    if (!context)
        return E_HANDLE;

    switch (property)
    {
        case INTERACTION_CONTEXT_PROPERTY_MEASUREMENT_UNITS:
            if (value > 1) return E_INVALIDARG;
            context->units = value ? 1 : 2;  /* 1 = screen, 2 = explicit HIMETRIC */
            return S_OK;

        case INTERACTION_CONTEXT_PROPERTY_INTERACTION_UI_FEEDBACK:
            if (value != FALSE && value != TRUE)
                return E_INVALIDARG;
            context->ui_feedback = value;
            return S_OK;

        case INTERACTION_CONTEXT_PROPERTY_FILTER_POINTERS:
            if (value != FALSE && value != TRUE)
                return E_INVALIDARG;
            context->filter_pointers = value;
            return S_OK;

        default:
            WARN("Invalid property %#x.\n", property);
            return E_INVALIDARG;
    }
}

HRESULT WINAPI SetInteractionConfigurationInteractionContext(HINTERACTIONCONTEXT handle,
        UINT32 count, const INTERACTION_CONTEXT_CONFIGURATION *configuration)
{
    struct interaction_context *context = context_from_handle(handle);

    TRACE("context %p, count %u, configuration %p.\n", context, count, configuration);

    if (!context)
        return E_HANDLE;
    if (!count)
        return E_INVALIDARG;
    if (!configuration)
        return E_POINTER;

    for (unsigned int i = 0; i < count; i++)
    {
        if (configuration[i].interactionId == INTERACTION_ID_NONE ||
                configuration[i].interactionId > INTERACTION_ID_CROSS_SLIDE)
            return E_INVALIDARG;
    }

    for (unsigned int i = 0; i < count; i++)
    {
        unsigned int index = configuration[i].interactionId - 1;
        context->config_flags[index] = configuration[i].enable;
    }

    return S_OK;
}

HRESULT WINAPI GetInteractionConfigurationInteractionContext(HINTERACTIONCONTEXT handle,
        UINT32 count, INTERACTION_CONTEXT_CONFIGURATION *configuration)
{
    struct interaction_context *context = context_from_handle(handle);

    FIXME("context %p, count %u, configuration %p: semi-stub!\n", context, count, configuration);

    if (!context)
        return E_HANDLE;
    if (!count)
        return S_OK;
    if (!configuration)
        return E_POINTER;

    for (unsigned int i = 0; i < count; i++)
    {
        if (configuration[i].interactionId == INTERACTION_ID_NONE ||
                configuration[i].interactionId > INTERACTION_ID_CROSS_SLIDE)
            return E_INVALIDARG;
    }

    for (unsigned int i = 0; i < count; i++)
    {
        unsigned int index = configuration[i].interactionId - 1;
        configuration[i].enable = context->config_flags[index];
    }

    return S_OK;
}

HRESULT WINAPI RegisterOutputCallbackInteractionContext(HINTERACTIONCONTEXT handle,
        INTERACTION_CONTEXT_OUTPUT_CALLBACK callback, void *data)
{
    struct interaction_context *context = context_from_handle(handle);

    TRACE("context %p, callback %p, data %p.\n", context, callback, data);

    if (!context)
        return E_HANDLE;

    context->callback = callback;
    context->callback_data = data;
    return S_OK;
}

HRESULT WINAPI ProcessBufferedPacketsInteractionContext(HINTERACTIONCONTEXT context)
{
    FIXME("context %p: stub!\n", context);

    if (!context)
        return E_HANDLE;

    return S_OK;
}

HRESULT WINAPI ProcessInertiaInteractionContext(HINTERACTIONCONTEXT context)
{
    FIXME("context %p: stub!\n", context);
    return E_NOTIMPL;
}

HRESULT WINAPI ProcessPointerFramesInteractionContext(HINTERACTIONCONTEXT handle,
        UINT32 entries_count, UINT32 pointer_count, const POINTER_INFO *pointer_info)
{
    struct interaction_context *context = context_from_handle(handle);
    unsigned int i;

    TRACE("context %p, entries_count %u, pointer_count %u, pointer_info %p.\n",
          context, entries_count, pointer_count, pointer_info);

    if (!context)
        return E_HANDLE;
    if (!entries_count || !pointer_count)
        return E_INVALIDARG;
    if (!pointer_info)
        return E_POINTER;

    /* the frames are ordered newest first; process them oldest first */
    for (i = entries_count * pointer_count; i > 0; i--)
        process_pointer(context, &pointer_info[i - 1]);
    return S_OK;
}

HRESULT WINAPI AddPointerInteractionContext(HINTERACTIONCONTEXT context, UINT32 pointer_id)
{
    TRACE("context %p, pointer_id %u.\n", context, pointer_id);

    if (!context)
        return E_HANDLE;

    return S_OK;
}

HRESULT WINAPI RemovePointerInteractionContext(HINTERACTIONCONTEXT handle, UINT32 pointer_id)
{
    struct interaction_context *context = context_from_handle(handle);
    struct tracked_pointer *pointer;

    TRACE("context %p, pointer_id %u.\n", context, pointer_id);

    if (!context)
        return E_HANDLE;

    if ((pointer = find_pointer(context, pointer_id, FALSE))) pointer->down = FALSE;
    return S_OK;
}

HRESULT WINAPI ResetInteractionContext(HINTERACTIONCONTEXT handle)
{
    struct interaction_context *context = context_from_handle(handle);

    TRACE("context %p.\n", context);

    if (!context)
        return E_HANDLE;

    memset(context->pointers, 0, sizeof(context->pointers));
    context->last_tap_count = 0;
    context->manip_active = FALSE;
    return S_OK;
}

HRESULT WINAPI StopInteractionContext(HINTERACTIONCONTEXT context)
{
    TRACE("context %p.\n", context);

    if (!context)
        return E_HANDLE;

    return S_OK;
}

/* Undocumented function at ordinal 2502 */
HRESULT WINAPI NINPUT_2502(HINTERACTIONCONTEXT *context, void *p1, void *p2)
{
    FIXME("context %p p1 %p p2 %p: stub!\n", context, p1, p2);

    if (!context)
        return E_HANDLE;

    return S_OK;
}
