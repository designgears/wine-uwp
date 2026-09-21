# Microsoft binaries needed as native overrides

Copy these from a Windows 11 install you are licensed for, all from the same build, into `~/uwp-bin/win11/`
and record the build number here. x64 only. Wine loads them via `WINEDLLOVERRIDES` set by `tools/wine-uwp/run.sh`.

Build used: 10.0.26200.9168 (Windows 11 IoT Enterprise 25H2), read read-only from /dev/nvme0n1p2 on 2026-09-09. Collected automatically, see Findings-M1.md; the PowerShell below is only for a machine without a mounted Windows partition.

## Required for M1 to M3
| File (System32) | Why |
|---|---|
| Windows.UI.Xaml.dll | The XAML engine. Wine ships a 62 KB stub. |
| Windows.UI.Xaml.Resources.19h1.dll, Windows.UI.Xaml.Resources.rs*.dll, Windows.UI.Xaml.Resources.th.dll (whatever exists) | Generic.xaml theme resources loaded by the engine |
| Windows.UI.Xaml.Phone.dll | Pulled in by the engine on some paths |
| Windows.UI.dll | CoreWindow, Windows.UI.Core, Windows.UI.Composition, Windows.UI.ViewManagement |
| twinapi.appcore.dll | CoreApplication, ApplicationView, activation |
| CoreMessaging.dll | Dispatcher queues used by CoreWindow and XAML |
| InputHost.dll | Input pipeline behind CoreWindow |
| MrmCoreR.dll | PRI resource manager (`ResourceContext`, `ResourceLoader`, `ms-appx://`) |
| Windows.ApplicationModel.dll | App model surface (package, resources hooks) |
| dcomp.dll | DirectComposition. Wine's is a stub. Needed to see what the engine asks of it, before we write our own. |
| Windows.Globalization.dll, bcp47langs.dll, bcp47mrm.dll | Number/date formatting the app uses; Wine's is partial |
| Windows.StateRepository* (only if activation lookups demand it) | Package/state DB behind the app model |

## Added after M2 (all same build)
| File (System32) | Why |
|---|---|
| KernelBase.dll, SHCore.dll | Reference only: signatures and behaviour of undocumented package/scaling APIs were read from these, nothing is loaded from them |
| MrmDeploy.dll | Implements `ext-ms-win-resources-deployment-l1-1-0`; loaded natively |
| rometadata.dll | WinRT metadata reader; Wine's returns E_NOINTERFACE for the table interfaces |
| BCP47mrm.dll, BCP47Langs.dll | Language tag handling MrmCoreR delay-loads |
| profapi.dll | Profile paths used by MrmCoreR |
| Windows.UI.Xaml.Controls.dll, .Phone.dll, .InkControls.dll, .Maps.dll | XAML built-in type info, loaded as resource-only images |
| Windows.UI.Xaml.Resources.*.dll, Windows.UI.XamlHost.dll | Default styles per OS release |
| WinMetadata/*.winmd (20 files) | Real WinRT metadata; Wine ships placeholders |

Framework packages are staged from `Program Files/WindowsApps` as whole package directories, not as
loose DLLs, because a UWP app resolves framework content through the package graph.

## Registry
Export `HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\WindowsRuntime\ActivatableClassId` to a `.reg` file. That gives
the class -> DLL -> threading model map for every system WinRT class, which our launcher rewrites for the prefix.

## How to collect (PowerShell on the Windows machine)
```powershell
$dst = "$env:USERPROFILE\Desktop\win11-uwp"; mkdir $dst
$files = "Windows.UI.Xaml.dll","Windows.UI.Xaml.Phone.dll","Windows.UI.dll","twinapi.appcore.dll","CoreMessaging.dll",
         "InputHost.dll","MrmCoreR.dll","Windows.ApplicationModel.dll","dcomp.dll","Windows.Globalization.dll",
         "bcp47langs.dll","bcp47mrm.dll","WindowManagementAPI.dll","CoreUIComponents.dll","UiaManager.dll"
$files | % { copy "$env:WINDIR\System32\$_" $dst }
Get-ChildItem "$env:WINDIR\System32\Windows.UI.Xaml.Resources.*.dll" | copy -Destination $dst
reg export "HKLM\SOFTWARE\Microsoft\WindowsRuntime\ActivatableClassId" "$dst\ActivatableClassId.reg"
[Environment]::OSVersion.Version.ToString() | Out-File "$dst\BUILD.txt"
```
Then `scp` the folder to `~/uwp-bin/win11/` here.

## Already obtained without Windows (redistributable)
See Findings-M0.md: Microsoft.UI.Xaml 2.8, .NET Native Runtime + Framework 2.2, VCLibs Desktop.

## Added 2026-09-10 (same build, same read-only source)
`WindowManagementAPI.dll`, `CoreUIComponents.dll`, `UiaManager.dll`,
`Microsoft.Internal.WarpPal.dll` —
needed for the CoreWindow / activation path, see Findings-M4.md. Same
personal-research boundary as everything else listed here: these files
are not redistributable and are not committed to this repository.

## Added 2026-09-15 (same build, read-only ntfs3 mount of nvme0n1p2 inside a container)
`Windows.Globalization.dll` (already in the dump) is now used natively instead of Wine's
builtin, with its companions `icu.dll`, `icuuc.dll`, `icuin.dll`, `globinputhost.dll`,
`WinLangdb.dll` (System32 no longer has a copy; taken from
`WinSxS\amd64_microsoft-windows-languagesdb-onecore_..._10.0.26100.8972_...`),
`en-US\Windows.Globalization.dll.mui`, and the data files `Windows\Globalization\ICU\*`
(icudtl.dat and the time-zone .res files) plus `Windows\Globalization\Time Zone\*`.
install-overrides.sh copies the data into `C:\windows\globalization\icu` (lowercase: the prefix
already had a lowercase `globalization` directory and Wine takes the exact-case match).

## Added 2026-09-16
`msftedit.dll` (from the same 26100 dump, already in the directory) is now used natively. XAML's
RichEditBox drives the text-services object through undocumented interfaces
(`{6bda2cdc-d9b3-4310-8fcb-5690308c2e7c}` and more), and Wine's riched20 only reaches the second
query; see Findings-M4.md. It needs `ntdll.SbSelectProcedure`, which Wine now stubs.

## Added 2026-09-16, later: the Store graphing engine and msxml6
- `~/uwp-bin/store-calc/`: a copy of the installed Store package
  `Microsoft.WindowsCalculator_11.2607.0.0_x64__8wekyb3d8bbwe` (from the same drive). The
  repo's `GraphingImpl` is a mock that parses and draws nothing; the Store package carries the
  real engine as `GraphingImpl.dll` + `omsautimmss.dll` (the OneNote math solver). `stage.sh`
  copies both over the mock when the directory exists. Same non-redistributable status.
- `msxml6.dll` + `msxml6r.dll` (System32) are used natively: the engine parses MathML through
  the SAX reader and the app's `Windows.Data.Xml.Dom.XmlDocument` also lives in msxml6 on
  Windows; Wine's msxml6 has neither the WinRT factory nor a SAX reader that satisfied it.
