cmake_minimum_required(VERSION 3.20)
project(wine-uwp-probes CXX)
add_executable(probe-load probes/probe-load.cpp)
add_executable(probe-dpapi probes/probe-dpapi.cpp)
add_executable(probe-svg probes/probe-svg.cpp)
target_link_libraries(probe-svg PRIVATE d2d1.lib windowscodecs.lib shlwapi.lib ole32.lib)
set_target_properties(probe-svg PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
add_executable(probe-fonts probes/probe-fonts.cpp)
target_link_libraries(probe-fonts PRIVATE dwrite.lib)
set_target_properties(probe-fonts PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
add_executable(probe-text probes/probe-text.cpp)
target_link_libraries(probe-text PRIVATE d2d1.lib dwrite.lib windowscodecs.lib ole32.lib)
set_target_properties(probe-text PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
add_executable(probe-activate probes/probe-activate.cpp)
target_link_libraries(probe-activate PRIVATE runtimeobject.lib)
add_executable(probe-bitmapimage probes/probe-bitmapimage.cpp)
target_link_libraries(probe-bitmapimage PRIVATE runtimeobject.lib)
# Drives a real SetWindowPos on a running app's windows: an X-side resize never reaches Wine.
# Isolates the IXMLHTTPRequest2 layer libHttpClient uses on UWP, which is where Solitaire's
# PlayFab login disappears without opening a socket.
add_executable(probe-http probes/probe-http.cpp)
target_link_libraries(probe-http PRIVATE ole32.lib)

add_executable(probe-resize probes/probe-resize.cpp)
add_executable(probe-sendmsg probes/probe-sendmsg.cpp)

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
add_executable(probe-wave probes/probe-wave.cpp)
target_link_libraries(probe-wave PRIVATE winmm.lib)
add_executable(probe-play probes/probe-play.cpp)
target_link_libraries(probe-play PRIVATE runtimeobject.lib)
add_executable(probe-memorymanager probes/probe-memorymanager.cpp)
target_link_libraries(probe-memorymanager PRIVATE runtimeobject.lib)


# Windows.Networking.Connectivity.NetworkInformation. Wine registers NetworkStatusChanged and then
# never raises it, so an app that takes its initial reachability from the first callback waits for
# ever. Forwards everything and fires the event once after registration.

# Windows.Media.Audio.SpatialAudioDeviceConfiguration. Wine routes the class to
# windows.media.devices, whose factory refuses ISpatialAudioDeviceConfigurationStatics; Wwise asks
# with .as<>() and the resulting E_NOINTERFACE throw kills Solitaire on its audio thread.

# Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager. The Web Account Manager
# needs the out-of-process token broker; without it its IAsyncOperation is never completed and
# Solitaire's Xbox sign-in check hangs the whole startup.

# Windows.Services.Store.StoreContext - a traced stand-in used to find out which Store methods
# the app actually calls, before implementing any of them.
add_executable(probe-gdkchunks probes/probe-gdkchunks.cpp)
set_target_properties(probe-gdkchunks PROPERTIES CXX_STANDARD 17)
target_link_libraries(probe-gdkchunks PRIVATE ole32.lib)
set_target_properties(probe-gdkchunks PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")

# Xbox sign-in self-tests: drives Wine's winexboxauth.dll without the Xbox app.
add_executable(probe-xboxauth probes/probe-xboxauth.cpp)
set_target_properties(probe-xboxauth PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")

# Windows.System.Profile.WindowsIntegrityPolicy: not in S mode (Windows' asks a lockdown-policy service over RPC).


add_executable(probe-useractivity probes/probe-useractivity.cpp)
target_link_libraries(probe-useractivity PRIVATE runtimeobject.lib ole32.lib)
add_executable(probe-toast probes/probe-toast.cpp)
target_link_libraries(probe-toast PRIVATE runtimeobject.lib ole32.lib)

add_executable(probe-uchar probes/probe-uchar.cpp)
set_target_properties(probe-uchar PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
add_executable(probe-wicscale probes/probe-wicscale.cpp)
target_link_libraries(probe-wicscale PRIVATE windowscodecs.lib ole32.lib)
set_target_properties(probe-wicscale PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
add_executable(probe-threads probes/probe-threads.cpp)
set_target_properties(probe-threads PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
add_executable(probe-peek probes/probe-peek.cpp)
set_target_properties(probe-peek PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
add_executable(probe-uaf probes/probe-uaf.cpp)
set_target_properties(probe-uaf PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")

add_executable(probe-a8 probes/probe-a8.cpp)
target_link_libraries(probe-a8 PRIVATE d3d11.lib d2d1.lib dxguid.lib)
set_target_properties(probe-a8 PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")

add_executable(probe-a8perf probes/probe-a8perf.cpp)
target_link_libraries(probe-a8perf PRIVATE d3d11.lib dxguid.lib)
set_target_properties(probe-a8perf PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")

add_executable(probe-xchild probes/probe-xchild.cpp)
target_link_libraries(probe-xchild PRIVATE user32.lib gdi32.lib)
set_target_properties(probe-xchild PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
# WebView2 under Wine, windowed hosting. Needs the WebView2 SDK headers, which are Microsoft's and stay out of git:
# -DWEBVIEW2_SDK=<dir with WebView2.h> (build.sh passes $UWP_BIN/webview2/sdk/pkg/build/native/include if present).
if(WEBVIEW2_SDK)
  add_executable(probe-webview2 probes/probe-webview2.cpp)
  target_include_directories(probe-webview2 PRIVATE ${WEBVIEW2_SDK})
  target_link_libraries(probe-webview2 PRIVATE ole32.lib user32.lib shlwapi.lib)
  set_target_properties(probe-webview2 PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()
add_executable(probe-widen probes/probe-widen.cpp)
target_link_libraries(probe-widen PRIVATE d2d1.lib)
set_target_properties(probe-widen PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
add_executable(probe-launcher probes/probe-launcher.cpp)
target_link_libraries(probe-launcher PRIVATE runtimeobject.lib)
add_executable(probe-pickers probes/probe-pickers.cpp)
target_link_libraries(probe-pickers PRIVATE runtimeobject.lib)
