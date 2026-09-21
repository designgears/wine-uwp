cmake_minimum_required(VERSION 3.20)
project(wine-uwp-probes CXX)
add_executable(probe-load probes/probe-load.cpp)
add_executable(probe-activate probes/probe-activate.cpp)
target_link_libraries(probe-activate PRIVATE runtimeobject.lib)
add_library(mrt100 SHARED shims/mrt100/mrt100.cpp)
set_target_properties(mrt100 PROPERTIES PREFIX "" OUTPUT_NAME "mrt100")
# Drives a real SetWindowPos on a running app's windows: an X-side resize never reaches Wine.
add_executable(probe-resize probes/probe-resize.cpp)

add_executable(probe-token probes/probe-token.cpp)
target_link_libraries(probe-token PRIVATE ntdll.lib)
add_executable(probe-alpc probes/probe-alpc.cpp)
target_link_libraries(probe-alpc PRIVATE ntdll.lib)
add_executable(probe-wcp probes/probe-wcp.cpp)
target_link_libraries(probe-wcp PRIVATE ntdll.lib)
add_executable(probe-wait probes/probe-wait.cpp)
target_link_libraries(probe-wait PRIVATE ntdll.lib)
add_executable(probe-avl probes/probe-avl.cpp)
target_link_libraries(probe-avl PRIVATE ntdll.lib)
add_executable(probe-glyph probes/probe-glyph.cpp)
target_link_libraries(probe-glyph PRIVATE dwrite.lib)
add_executable(probe-langs probes/probe-langs.cpp)
target_link_libraries(probe-langs PRIVATE runtimeobject.lib)
add_executable(probe-mrt probes/probe-mrt.cpp)
target_link_libraries(probe-mrt PRIVATE runtimeobject.lib)
add_executable(probe-surface probes/probe-surface.cpp)
target_link_libraries(probe-surface PRIVATE runtimeobject.lib d3d11.lib d2d1.lib dxguid.lib)
add_executable(probe-dwrite probes/probe-dwrite.cpp)
target_link_libraries(probe-dwrite PRIVATE dwrite.lib)
add_executable(probe-fontcollection probes/probe-fontcollection.cpp)
target_link_libraries(probe-fontcollection PRIVATE dwrite.lib)
add_library(staterepository SHARED shims/staterepository/staterepository.cpp)
set_target_properties(staterepository PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_staterepository")
target_link_libraries(staterepository PRIVATE runtimeobject.lib advapi32.lib ole32.lib)
add_library(appcapability SHARED shims/appcapability/appcapability.cpp)
set_target_properties(appcapability PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_appcapability")
target_link_libraries(appcapability PRIVATE runtimeobject.lib)
add_library(storagelibrary SHARED shims/storagelibrary/storagelibrary.cpp)
set_target_properties(storagelibrary PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_storagelibrary")
target_link_libraries(storagelibrary PRIVATE runtimeobject.lib)
add_executable(probe-wave probes/probe-wave.cpp)
target_link_libraries(probe-wave PRIVATE winmm.lib)
add_executable(probe-play probes/probe-play.cpp)
target_link_libraries(probe-play PRIVATE runtimeobject.lib)
add_library(filepicker SHARED shims/filepicker/filepicker.cpp)
set_target_properties(filepicker PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_filepicker")
target_link_libraries(filepicker PRIVATE runtimeobject.lib)
add_library(userpicker SHARED shims/userpicker/userpicker.cpp)
set_target_properties(userpicker PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_userpicker")
target_link_libraries(userpicker PRIVATE runtimeobject.lib)
add_library(accesscache SHARED shims/accesscache/accesscache.cpp)
set_target_properties(accesscache PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_accesscache")
target_link_libraries(accesscache PRIVATE runtimeobject.lib)
add_executable(probe-memorymanager probes/probe-memorymanager.cpp)
target_link_libraries(probe-memorymanager PRIVATE runtimeobject.lib)
add_library(memorymanager SHARED shims/memorymanager/memorymanager.cpp)
set_target_properties(memorymanager PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_memorymanager")
target_link_libraries(memorymanager PRIVATE runtimeobject.lib ole32.lib psapi.lib)
add_library(deviceinformation SHARED shims/deviceinformation/deviceinformation.cpp)
set_target_properties(deviceinformation PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_deviceinformation")
target_link_libraries(deviceinformation PRIVATE runtimeobject.lib)

# api-ms-win-appmodel-runtime-l1-1-1: forwards the apiset to kernelbase and adds the one function
# Wine is missing (GetPackageFamilyNameFromToken). Must provide the whole apiset, not just the gap.
add_library(appmodelruntime SHARED shims/appmodelruntime/appmodelruntime.cpp)
set_target_properties(appmodelruntime PROPERTIES PREFIX "" SUFFIX ""
                      OUTPUT_NAME "api-ms-win-appmodel-runtime-l1-1-1.dll")

# Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager. The Web Account Manager
# needs the out-of-process token broker; without it its IAsyncOperation is never completed and
# Solitaire's Xbox sign-in check hangs the whole startup.
add_library(webauthcore SHARED shims/webauthcore/webauthcore.cpp)
set_target_properties(webauthcore PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_webauthcore")
target_link_libraries(webauthcore PRIVATE runtimeobject.lib)

# Windows.Services.Store.StoreContext - a traced stand-in used to find out which Store methods
# the app actually calls, before implementing any of them.
add_library(storecontext SHARED shims/storecontext/storecontext.cpp)
set_target_properties(storecontext PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_storecontext")
target_link_libraries(storecontext PRIVATE runtimeobject.lib)

# api-ms-win-appmodel-advertisingid-l1-1-0: twinapi.appcore imports three functions from this
# apiset and Wine implements none, so the loader's unresolved-import stub raises
# EXCEPTION_WINE_STUB (0x80000100) mid-startup. Built with the apiset's exact file name.
add_library(advertisingid SHARED shims/advertisingid/advertisingid.cpp)
set_target_properties(advertisingid PROPERTIES PREFIX "" SUFFIX ""
                      OUTPUT_NAME "api-ms-win-appmodel-advertisingid-l1-1-0.dll")

# Windows.UI.Shell.TaskbarManager. The real one needs ImmersiveShell, which does not exist here,
# and leaves its IAsyncOperation permanently incomplete - which is what stops Solitaire's startup.
add_library(taskbarmanager SHARED shims/taskbarmanager/taskbarmanager.cpp)
set_target_properties(taskbarmanager PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_taskbarmanager")
target_link_libraries(taskbarmanager PRIVATE runtimeobject.lib)

add_executable(probe-useractivity probes/probe-useractivity.cpp)
target_link_libraries(probe-useractivity PRIVATE runtimeobject.lib ole32.lib)
add_library(useractivitychannel SHARED shims/useractivitychannel/useractivitychannel.cpp)
set_target_properties(useractivitychannel PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_useractivitychannel")
target_link_libraries(useractivitychannel PRIVATE runtimeobject.lib ole32.lib)
add_executable(probe-toast probes/probe-toast.cpp)
target_link_libraries(probe-toast PRIVATE runtimeobject.lib ole32.lib)

add_library(toastnotifier SHARED shims/toastnotifier/toastnotifier.cpp)
set_target_properties(toastnotifier PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_toastnotifier")
target_link_libraries(toastnotifier PRIVATE runtimeobject.lib ole32.lib)
