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
add_library(staterepository SHARED shims/staterepository/staterepository.cpp)
set_target_properties(staterepository PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_staterepository")
target_link_libraries(staterepository PRIVATE runtimeobject.lib advapi32.lib ole32.lib)
add_library(storagelibrary SHARED shims/storagelibrary/storagelibrary.cpp)
set_target_properties(storagelibrary PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_storagelibrary")
target_link_libraries(storagelibrary PRIVATE runtimeobject.lib)
add_executable(probe-wave probes/probe-wave.cpp)
target_link_libraries(probe-wave PRIVATE winmm.lib)
add_executable(probe-play probes/probe-play.cpp)
target_link_libraries(probe-play PRIVATE runtimeobject.lib)
add_library(filepicker SHARED shims/filepicker/filepicker.cpp)
set_target_properties(filepicker PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_filepicker")
target_link_libraries(filepicker PRIVATE runtimeobject.lib comdlg32.lib shell32.lib ole32.lib)
add_library(accesscache SHARED shims/accesscache/accesscache.cpp)
set_target_properties(accesscache PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_accesscache")
target_link_libraries(accesscache PRIVATE runtimeobject.lib)
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
add_library(gameruntime SHARED shims/gameruntime/gameruntime.cpp shims/gameruntime/xuser_xstore.cpp shims/gameruntime/hd_softskip.cpp)
set_target_properties(gameruntime PROPERTIES PREFIX "" OUTPUT_NAME "xgameruntime" CXX_STANDARD 17)
target_link_libraries(gameruntime PRIVATE ole32.lib crypt32.lib)
add_executable(probe-gdkchunks probes/probe-gdkchunks.cpp)
set_target_properties(probe-gdkchunks PROPERTIES CXX_STANDARD 17)
target_link_libraries(probe-gdkchunks PRIVATE ole32.lib)
set_target_properties(probe-gdkchunks PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")

add_library(gameplatform SHARED shims/gameplatform/gameplatform.cpp)
set_target_properties(gameplatform PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_gameplatform")
target_link_libraries(gameplatform PRIVATE runtimeobject.lib advapi32.lib winhttp.lib ole32.lib)
# appxinstall.dll's API, from our Wine source (the header alone: the shims build against the Windows SDK)
target_compile_definitions(gameplatform PRIVATE "APPXINSTALL_H=\"$ENV{WINE_SRC}/include/wine/appxinstall.h\"")
# Microsoft.GamePlatform.Services.Identity: Xbox sign-in (MSA device code + Xbox Live tokens) for the Xbox app.
add_library(xboxauth SHARED shims/xboxauth/xboxauth.cpp)
set_target_properties(xboxauth PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_xboxauth")
target_link_libraries(xboxauth PRIVATE runtimeobject.lib winhttp.lib crypt32.lib bcrypt.lib shell32.lib ole32.lib)
add_executable(probe-xboxauth probes/probe-xboxauth.cpp)
set_target_properties(probe-xboxauth PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")

# Windows.ApplicationModel.Store.Preview.InstallControl.AppInstallManager for the Microsoft Store.
add_library(appinstallmanager SHARED shims/appinstallmanager/appinstallmanager.cpp)
set_target_properties(appinstallmanager PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_appinstallmanager")
target_link_libraries(appinstallmanager PRIVATE runtimeobject.lib winhttp.lib ole32.lib)
target_compile_definitions(appinstallmanager PRIVATE "APPXINSTALL_H=\"$ENV{WINE_SRC}/include/wine/appxinstall.h\"")
# Windows.System.Profile.WindowsIntegrityPolicy: not in S mode (Windows' asks a lockdown-policy service over RPC).

# Windows.UI.Shell.TaskbarManager. The real one needs ImmersiveShell, which does not exist here,
# and leaves its IAsyncOperation permanently incomplete - which is what stops Solitaire's startup.
# Windows.Security.Cryptography.Certificates.Certificate: lives in certenroll.dll, which this image
# lacks, and Windows.Web.Http needs it for every HTTPS response.
add_library(certificate SHARED shims/certificate/certificate.cpp)
set_target_properties(certificate PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_certificate")
target_link_libraries(certificate PRIVATE runtimeobject.lib crypt32.lib ole32.lib)
add_library(launcher SHARED shims/launcher/launcher.cpp)
set_target_properties(launcher PROPERTIES PREFIX "" OUTPUT_NAME "wineuwp_launcher")
target_link_libraries(launcher PRIVATE runtimeobject.lib shell32.lib)

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
# The WebView2 runtime host (EmbeddedBrowserWebView.dll stand-in over the real runtime), also needs WebView2.h.
if(WEBVIEW2_SDK)
  add_library(webview2host SHARED shims/webview2/webview2.cpp)
  target_include_directories(webview2host PRIVATE ${WEBVIEW2_SDK})
  target_link_libraries(webview2host PRIVATE ole32.lib user32.lib gdi32.lib windowscodecs.lib)
  set_target_properties(webview2host PROPERTIES PREFIX "" OUTPUT_NAME "EmbeddedBrowserWebView")
endif()
add_executable(probe-widen probes/probe-widen.cpp)
target_link_libraries(probe-widen PRIVATE d2d1.lib)
set_target_properties(probe-widen PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE")
