#pragma once
/*
 * XUser / XStore / XThreading ABI for QueryApiImpl. Layout matches
 * uwp-bin/ext/xgameruntime/include/{xuser,xstore,xasyncprovider}.idl and
 * uwp-bin/ext/xgameruntime-docs/COM/.
 */
#include <Windows.h>
#include <Unknwn.h>
#include <cstdint>
#include <cstring>

struct XAsyncBlock;
struct XTaskQueueObject;
using XTaskQueueHandle = XTaskQueueObject *;
struct XTaskQueuePortObject;
using XTaskQueuePortHandle = XTaskQueuePortObject *;

struct XTaskQueueRegistrationToken
{
    UINT64 token;
};

enum class XTaskQueueDispatchMode : UINT32
{
    Manual,
    ThreadPool,
    SerializedThreadPool,
    Immediate,
};

enum class XTaskQueuePort : UINT32
{
    Work,
    Completion,
};

using XAsyncCompletionRoutine = void __stdcall(XAsyncBlock *asyncBlock);
using XAsyncWork = HRESULT __stdcall(XAsyncBlock *asyncBlock);

struct XAsyncBlock
{
    XTaskQueueHandle queue;
    void *context;
    XAsyncCompletionRoutine *callback;
    unsigned char internal[sizeof(void *) * 4];
};

enum class XAsyncOp : UINT32
{
    Begin,
    DoWork,
    GetResult,
    Cancel,
    Cleanup,
};

struct XAsyncProviderData
{
    XAsyncBlock *async;
    SIZE_T bufferSize;
    void *buffer;
    void *context;
};

using XAsyncProvider = HRESULT __stdcall(XAsyncOp op, const XAsyncProviderData *data);

enum class XUserAddOptions : UINT32
{
    None = 0x00,
    AddDefaultUserSilently = 0x01,
    AllowGuests = 0x02,
    AddDefaultUserAllowingUI = 0x04,
};

enum class XUserAgeGroup : UINT32
{
    Unknown,
    Child,
    Teen,
    Adult,
};

enum class XUserGamertagComponent : UINT32
{
    Classic,
    Modern,
    ModernSuffix,
    UniqueModern,
};

enum class XUserPrivilege : UINT32
{
    CrossPlay = 185,
    Clubs = 188,
    Sessions = 189,
    Broadcast = 190,
    ManageProfilePrivacy = 196,
    GameDvr = 198,
    MultiplayerParties = 203,
    CloudManageSession = 207,
    CloudJoinSession = 208,
    CloudSavedGames = 209,
    SocialNetworkSharing = 220,
    UserGeneratedContent = 247,
    Communications = 252,
    Multiplayer = 254,
    AddFriends = 255,
};

enum class XUserPrivilegeDenyReason : UINT32
{
    None,
    PurchaseRequired,
    Restricted,
    Banned,
    Unknown = 0xFFFFFFFFu,
};

enum class XUserPrivilegeOptions : UINT32
{
    None = 0x00,
    AllUsers = 0x01,
};

enum class XUserState : UINT32
{
    SignedIn,
    SigningOut,
    SignedOut,
};

enum class XUserGamerPictureSize : UINT32
{
    Small,
    Medium,
    Large,
    ExtraLarge,
};

enum class XUserGetTokenAndSignatureOptions : UINT32
{
    None = 0x00,
    ForceRefresh = 0x01,
    AllUsers = 0x02,
};

enum class XUserGetMsaTokenSilentlyOptions : UINT32
{
    None,
};

enum class XStoreProductKind : UINT32
{
    None = 0x00,
    Consumable = 0x01,
    Durable = 0x02,
    Game = 0x04,
    Pass = 0x08,
    UnmanagedConsumable = 0x10,
};

enum class XStoreCanLicenseStatus : UINT32
{
    NotLicensableToUser,
    Licensable,
    LicenseActionNotApplicableToProduct,
};

struct XUserLocalId
{
    UINT64 value;
};

struct XUserGetTokenAndSignatureHttpHeader
{
    const char *name;
    const char *value;
};

struct XUserGetTokenAndSignatureData
{
    SIZE_T tokenSize;
    SIZE_T signatureSize;
    const char *token;
    const char *signature;
};

using XUserHandle = struct XUserObject *;
using XUserSignOutDeferralHandle = void *;
using XStoreContextHandle = void *;
using XStoreLicenseHandle = void *;
using XStoreProductQueryHandle = void *;

struct XStoreGameLicense
{
    char skuStoreId[18];
    BOOLEAN isActive;
    BOOLEAN isTrialOwnedByThisUser;
    BOOLEAN isDiscLicense;
    BOOLEAN isTrial;
    UINT32 trialTimeRemainingInSeconds;
    char trialUniqueId[64];
    time_t expirationDate;
};

struct XStoreCanAcquireLicenseResult
{
    char licensableSku[5];
    XStoreCanLicenseStatus status;
};

struct XStoreAddonLicense
{
    char skuStoreId[18];
    BOOLEAN isActive;
    time_t expirationDate;
};

struct XStoreConsumableResult
{
    UINT32 quantity;
};

struct XStoreRateAndReviewResult
{
    BOOLEAN wasUpdated;
};

struct XStoreAvailability;
struct XStoreProduct;
using XStoreProductQueryCallback = BOOLEAN(CALLBACK *)(void *context, const XStoreProduct *product);
using XStoreGameLicenseChangedCallback = void __stdcall(void *context);
using XStorePackageLicenseLostCallback = void __stdcall(void *context);
using XUserChangeEventCallback = void __stdcall(void *context, XUserLocalId userLocalId, /*XUserChangeEvent*/ UINT32 event);

/* CLSIDs / IIDs */
inline constexpr GUID CLSID_XUserImpl =
    {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
inline constexpr GUID IID_IXUserImpl =
    {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
inline constexpr GUID IID_IXUserImpl2 =
    {0xeb9bf948, 0x18dc, 0x4d82, {0xbb, 0xcc, 0x40, 0xe0, 0xa8, 0x09, 0xc4, 0xc0}};
inline constexpr GUID IID_IXUserImpl3 =
    {0x1bf2f8c5, 0xd507, 0x4e52, {0xbb, 0x05, 0xf7, 0x26, 0xd0, 0xe7, 0x11, 0x61}};
inline constexpr GUID IID_IXUserImpl4 =
    {0x079415e3, 0x6727, 0x437f, {0x8e, 0x9d, 0x8f, 0x8f, 0x9b, 0x24, 0x39, 0xf7}};
inline constexpr GUID IID_IXUserImpl5 =
    {0x26f3c674, 0xa2fe, 0x44fa, {0xb6, 0xc4, 0xa3, 0x23, 0xbc, 0x94, 0xff, 0x53}};
inline constexpr GUID IID_IXUserImpl6 =
    {0x5131d685, 0x4394, 0x4ee6, {0x8c, 0x18, 0xbf, 0xb5, 0xd4, 0xae, 0xf1, 0xff}};
inline constexpr GUID IID_IXUserGamertagImpl =
    {0xcef4fac0, 0x7676, 0x4a94, {0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74}};

inline constexpr GUID CLSID_XStoreImpl =
    {0x0dd112ac, 0x7c24, 0x448c, {0xb9, 0x2b, 0x39, 0x60, 0xfb, 0x5b, 0xd3, 0x0c}};
inline constexpr GUID IID_IXStoreImpl =
    {0x0dd112ac, 0x7c24, 0x448c, {0xb9, 0x2b, 0x39, 0x60, 0xfb, 0x5b, 0xd3, 0x0c}};
inline constexpr GUID IID_IXStoreImpl2 =
    {0x60b09f4e, 0x1b85, 0x45b1, {0x82, 0x6c, 0x16, 0x91, 0x18, 0xe2, 0x30, 0xe1}};
inline constexpr GUID IID_IXStoreImpl3 =
    {0x2d42fea5, 0xe71d, 0x4b76, {0x97, 0xcd, 0xc5, 0x0a, 0xfb, 0xb3, 0xae, 0x5d}};
inline constexpr GUID IID_IXStoreImpl4 =
    {0xde3dbdd4, 0x0b37, 0x4bdb, {0xa1, 0x0e, 0xac, 0xf3, 0xa3, 0x54, 0xd0, 0x6a}};
inline constexpr GUID IID_IXStoreImpl5 =
    {0x5c48dedf, 0x0b67, 0x4492, {0xa4, 0xb5, 0x68, 0x29, 0xb8, 0xe7, 0x96, 0xe1}};
inline constexpr GUID IID_IXStoreImpl6 =
    {0xb09d803c, 0x2414, 0x4a05, {0x82, 0xc6, 0x66, 0xdf, 0xdc, 0x9e, 0x9a, 0x44}};

inline constexpr GUID CLSID_XThreadingImpl =
    {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
inline constexpr GUID IID_IXThreadingImpl =
    {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

HRESULT query_xuser(REFIID iid, void **out);
HRESULT query_xstore(REFIID iid, void **out);
HRESULT query_xthreading(REFIID iid, void **out);
HRESULT query_xgame(REFIID iid, void **out);
HRESULT query_xsystem(REFIID iid, void **out);

inline constexpr GUID CLSID_XGameImpl =
    {0x973a344e, 0x24bf, 0x4d0f, {0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29}};
inline constexpr GUID IID_IXGameImpl =
    {0x973a344e, 0x24bf, 0x4d0f, {0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29}};
inline constexpr GUID IID_IXGameImpl2 =
    {0x50849859, 0x0ad8, 0x4f81, {0x80, 0xe4, 0x5b, 0xc7, 0x86, 0x26, 0xf8, 0x52}};
inline constexpr GUID IID_IXGameImpl3 =
    {0x2549f142, 0x6419, 0x4a06, {0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34}};

inline constexpr GUID CLSID_XSystemImpl =
    {0xe349bd1a, 0xfc20, 0x4e40, {0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f}};
inline constexpr GUID IID_IXSystemImpl =
    {0xe349bd1a, 0xfc20, 0x4e40, {0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f}};
inline constexpr GUID IID_IXSystemImpl2 =
    {0x6fd71f09, 0x7513, 0x49f0, {0x89, 0xbc, 0xbf, 0xaf, 0x5d, 0xf6, 0xf8, 0x52}};
inline constexpr GUID IID_IXSystemImpl3 =
    {0x67ce4bfc, 0xb1d1, 0x4ac7, {0xbc, 0x3a, 0xcb, 0x92, 0x19, 0xa9, 0x7a, 0x85}};
inline constexpr GUID IID_IXSystemImpl4 =
    {0xdadc2895, 0x34b0, 0x4ef5, {0xa8, 0x3e, 0x45, 0x11, 0x4d, 0x62, 0x9b, 0x80}};
inline constexpr GUID IID_IXSystemImpl5 =
    {0x1861cf2e, 0xe18b, 0x4834, {0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf}};
