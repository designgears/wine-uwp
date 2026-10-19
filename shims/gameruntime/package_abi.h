#pragma once

#include <Windows.h>
#include <Unknwn.h>

/*
 * ABI declarations transcribed from the GDK XPackage IDL reference shipped at
 * uwp-bin/gdk-reference/xpackage.idl. See also:
 * https://github.com/nocts-software/xgameruntime/blob/main/xpackage.idl
 *
 * IXPackageImpl::XPackageFindChunkAvailability is vtable slot 12 (byte offset
 * 0x60), counting IUnknown's three slots.
 */

enum class XPackageChunkAvailability : UINT32
{
    Ready,
    Pending,
    Installable,
    Unavailable,
};

enum class XPackageChunkSelectorType : UINT32
{
    Language,
    Tag,
    Chunk,
    Feature,
};

enum class XPackageEnumerationScope : UINT32
{
    ThisOnly,
    ThisAndRelated,
    ThisPublisher,
};

enum class XPackageKind : UINT32
{
    Game,
    Content,
    PublisherContent,
};

using XPackageInstallationMonitorHandle = void *;
using XPackageMountHandle = void *;
using XTaskQueueHandle = void *;
using XTaskQueueRegistrationToken = UINT64;
using XAsyncBlock = void;

struct XVersion
{
    UINT16 major;
    UINT16 minor;
    UINT16 build;
    UINT16 revision;
};

struct XPackageChunkSelector
{
    XPackageChunkSelectorType type;
    union
    {
        const char *language;
        const char *tag;
        UINT32 chunkId;
        const char *feature;
    };
};

struct XPackageDetails
{
    const char *packageIdentifier;
    XVersion version;
    XPackageKind kind;
    const char *displayName;
    const char *description;
    const char *publisher;
    const char *storeId;
    BOOLEAN installing;
    UINT32 index;
    UINT32 count;
    BOOLEAN ageRestricted;
    const char *titleID;
};

struct XPackageFeature
{
    const char *id;
    const char *displayName;
    const char *tags;
    BOOLEAN hidden;
    UINT32 storeIdCount;
    const char **storeIds;
};

struct XPackageInstallationProgress
{
    UINT64 totalBytes;
    UINT64 installedBytes;
    UINT64 launchBytes;
    BOOLEAN launchable;
    BOOLEAN completed;
};

struct XPackageWriteStats
{
    UINT64 interval;
    UINT64 budget;
    UINT64 elapsed;
    UINT64 bytesWritten;
};

using XPackageChunkAvailabilityCallback = BOOLEAN (CALLBACK *)(
    void *context, const XPackageChunkSelector *selector, XPackageChunkAvailability availability);
using XPackageEnumerationCallback = BOOLEAN (CALLBACK *)(void *context, const XPackageDetails *details);
using XPackageFeatureEnumerationCallback = BOOLEAN (CALLBACK *)(void *context, const XPackageFeature *feature);
using XPackageInstallationProgressCallback = void (CALLBACK *)(
    void *context, XPackageInstallationMonitorHandle monitor);
using XPackageInstalledCallback = void (CALLBACK *)(void *context, const XPackageDetails *details);

inline constexpr GUID CLSID_XPackageImpl =
    {0xaf406016, 0xe850, 0x4aa8, {0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac, 0x7e}};
inline constexpr GUID IID_IXPackageImpl =
    {0x3720de07, 0xe8e4, 0x44a3, {0xad, 0x32, 0xb3, 0x59, 0xe8, 0xad, 0xbe, 0x55}};
inline constexpr GUID IID_IXPackageImpl2 =
    {0xf92d8712, 0x2b27, 0x4d8a, {0xbf, 0x01, 0x11, 0xa6, 0xf8, 0xe3, 0xeb, 0x42}};
inline constexpr GUID IID_IXPackageImpl3 =
    {0xe2a4734b, 0x2f4a, 0x456d, {0xaa, 0x8f, 0xd0, 0x65, 0xe0, 0x4f, 0xb2, 0x09}};
inline constexpr GUID IID_IXPackageImpl4 =
    {0x0b9d556e, 0xab70, 0x43f4, {0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40}};

struct IXPackageImpl : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE XPackageGetCurrentProcessPackageIdentifier(
        SIZE_T bufferSize, char *buffer) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XPackageIsPackagedProcess() = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageCreateInstallationMonitor(
        const char *packageIdentifier, UINT32 selectorCount, const XPackageChunkSelector *selectors,
        UINT32 minimumUpdateIntervalMs, XTaskQueueHandle queue,
        XPackageInstallationMonitorHandle *installationMonitor) = 0;
    virtual void STDMETHODCALLTYPE XPackageCloseInstallationMonitorHandle(
        XPackageInstallationMonitorHandle installationMonitor) = 0;
    virtual void STDMETHODCALLTYPE XPackageGetInstallationProgress(
        XPackageInstallationMonitorHandle installationMonitor, XPackageInstallationProgress *progress) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XPackageUpdateInstallationMonitor(
        XPackageInstallationMonitorHandle installationMonitor) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageRegisterInstallationProgressChanged(
        XPackageInstallationMonitorHandle installationMonitor, void *context,
        XPackageInstallationProgressCallback callback, XTaskQueueRegistrationToken *token) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XPackageUnregisterInstallationProgressChanged(
        XPackageInstallationMonitorHandle installationMonitor, XTaskQueueRegistrationToken token,
        BOOLEAN wait) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageGetUserLocale(SIZE_T localeSize, char *locale) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageFindChunkAvailability(
        const char *packageIdentifier, UINT32 selectorCount, const XPackageChunkSelector *selectors,
        XPackageChunkAvailability *availability) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageEnumerateChunkAvailability(
        const char *packageIdentifier, XPackageChunkSelectorType type, void *context,
        XPackageChunkAvailabilityCallback callback) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageChangeChunkInstallOrder(
        const char *packageIdentifier, UINT32 selectorCount, const XPackageChunkSelector *selectors) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageInstallChunks(
        const char *packageIdentifier, UINT32 selectorCount, XPackageChunkSelector *selectors,
        UINT32 minimumUpdateIntervalMs, BOOLEAN suppressUserConfirmation, XTaskQueueHandle queue,
        XPackageInstallationMonitorHandle *installationMonitor) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageInstallChunksAsync(
        const char *packageIdentifier, UINT32 selectorCount, const XPackageChunkSelector *selectors,
        UINT32 minimumUpdateIntervalMs, BOOLEAN suppressUserConfirmation, XAsyncBlock *asyncBlock) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageInstallChunksResult(
        XAsyncBlock *asyncBlock, XPackageInstallationMonitorHandle *installationMonitor) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageEstimateDownloadSize(
        const char *packageIdentifier, UINT32 selectorCount, const XPackageChunkSelector *selectors,
        UINT64 *downloadSize, BOOLEAN *shouldPresentUserConfirmation) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageUninstallChunks(
        const char *packageIdentifier, UINT32 selectorCount, const XPackageChunkSelector *selectors) = 0;
    virtual HRESULT STDMETHODCALLTYPE Padding0() = 0;
    virtual HRESULT STDMETHODCALLTYPE Padding1() = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XPackageUnregisterPackageInstalled(
        XTaskQueueRegistrationToken token, BOOLEAN wait) = 0;
    virtual HRESULT STDMETHODCALLTYPE Padding2() = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageGetMountPathSize(XPackageMountHandle mount, SIZE_T *pathSize) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageGetMountPath(
        XPackageMountHandle mount, SIZE_T pathSize, char *path) = 0;
    virtual void STDMETHODCALLTYPE XPackageCloseMountHandle(XPackageMountHandle mount) = 0;
    virtual HRESULT STDMETHODCALLTYPE Padding3() = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageEnumeratePackages(
        XPackageKind kind, XPackageEnumerationScope scope, void *context,
        XPackageEnumerationCallback callback) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageRegisterPackageInstalled(
        XTaskQueueHandle queue, void *context, XPackageInstalledCallback callback,
        XTaskQueueRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageGetWriteStats(XPackageWriteStats *writeStats) = 0;
    virtual HRESULT STDMETHODCALLTYPE Padding4() = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageUninstallUWPInstance(const char *packageName) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageEnumerateFeatures(
        const char *packageIdentifier, void *context, XPackageFeatureEnumerationCallback callback) = 0;
    virtual BOOLEAN STDMETHODCALLTYPE XPackageUninstallPackage(const char *packageIdentifier) = 0;
};

struct IXPackageImpl2 : IXPackageImpl
{
    /* The IDL repeats these method names with the same signatures. Distinct C++ names preserve
     * the appended ABI slots without making the declarations illegal overloads. */
    virtual HRESULT STDMETHODCALLTYPE XPackageEnumeratePackages2(
        XPackageKind kind, XPackageEnumerationScope scope, void *context,
        XPackageEnumerationCallback callback) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageRegisterPackageInstalled2(
        XTaskQueueHandle queue, void *context, XPackageInstalledCallback callback,
        XTaskQueueRegistrationToken *token) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageMountWithUiAsync(
        const char *packageIdentifier, XAsyncBlock *asyncBlock) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageMountWithUiResult(
        XAsyncBlock *asyncBlock, XPackageMountHandle *mount) = 0;
};

struct IXPackageImpl3 : IXPackageImpl2
{
    virtual HRESULT STDMETHODCALLTYPE XPackageEnumeratePackages3(
        XPackageKind kind, XPackageEnumerationScope scope, void *context,
        XPackageEnumerationCallback callback) = 0;
    virtual HRESULT STDMETHODCALLTYPE XPackageRegisterPackageInstalled3(
        XTaskQueueHandle queue, void *context, XPackageInstalledCallback callback,
        XTaskQueueRegistrationToken *token) = 0;
};

struct IXPackageImpl4 : IXPackageImpl3
{
    virtual HRESULT STDMETHODCALLTYPE XPackageGetKind(
        const char *packageIdentifier, XPackageKind *kind) = 0;
};
