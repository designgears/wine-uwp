/* Diagnostic for wine-uwp's single-local-user controller association.
 * Requires Wine's generated WinRT headers and a connected gamepad.
 * Exit 77 means no gamepad was enumerated; it is not a passing result. */
#define COBJMACROS
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_System
#define WIDL_using_Windows_Gaming_Input
#include <stdio.h>
#include "windows.h"
#include "initguid.h"
#include "roapi.h"
#include "winstring.h"
#include "windows.gaming.input.h"

int main(void)
{
    IUserStatics2 *users = NULL;
    IGamepadStatics *gamepads = NULL;
    IVectorView_Gamepad *view = NULL;
    IGamepad *gamepad = NULL;
    IGameController *controller = NULL;
    IUser *expected = NULL, *actual = NULL;
    HSTRING name = NULL, expected_id = NULL, actual_id = NULL;
    UserAuthenticationStatus status;
    UINT32 count = 0;
    INT32 comparison = 1;
    HRESULT hr;
    int i, result = 1;

#define CHECK(call) do { hr = (call); if (FAILED(hr)) { \
    printf("FAIL %s: %#lx\n", #call, (unsigned long)hr); goto done; } } while (0)

    hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) return 1;
    CHECK(WindowsCreateString(L"Windows.System.User", 19, &name));
    CHECK(RoGetActivationFactory(name, &IID_IUserStatics2, (void **)&users));
    WindowsDeleteString(name); name = NULL;
    CHECK(IUserStatics2_GetDefault(users, &expected));
    if (!expected) { puts("FAIL default User is NULL"); goto done; }
    CHECK(IUser_get_NonRoamableId(expected, &expected_id));
    CHECK(WindowsCreateString(L"Windows.Gaming.Input.Gamepad", 28, &name));
    CHECK(RoGetActivationFactory(name, &IID_IGamepadStatics, (void **)&gamepads));
    WindowsDeleteString(name); name = NULL;
    for (i = 0; i < 50; ++i)
    {
        CHECK(IGamepadStatics_get_Gamepads(gamepads, &view));
        CHECK(IVectorView_Gamepad_get_Size(view, &count));
        if (count) break;
        IVectorView_Gamepad_Release(view); view = NULL;
        Sleep(100);
    }
    if (!count) { puts("SKIP no gamepad after 5 seconds"); result = 77; goto done; }
    CHECK(IVectorView_Gamepad_GetAt(view, 0, &gamepad));
    CHECK(IGamepad_QueryInterface(gamepad, &IID_IGameController, (void **)&controller));
    if (IGameController_get_User(controller, NULL) != E_POINTER)
    { puts("FAIL null output did not return E_POINTER"); goto done; }
    CHECK(IGameController_get_User(controller, &actual));
    if (!actual) { puts("FAIL controller User is NULL"); goto done; }
    CHECK(IUser_get_NonRoamableId(actual, &actual_id));
    CHECK(WindowsCompareStringOrdinal(expected_id, actual_id, &comparison));
    CHECK(IUser_get_AuthenticationStatus(actual, &status));
    if (comparison || status != UserAuthenticationStatus_LocallyAuthenticated)
    { puts("FAIL controller User differs from the locally authenticated default User"); goto done; }
    puts("PASS controller User matches default local User; null output returns E_POINTER");
    result = 0;
done:
    WindowsDeleteString(name);
    WindowsDeleteString(expected_id);
    WindowsDeleteString(actual_id);
    if (actual) IUser_Release(actual);
    if (expected) IUser_Release(expected);
    if (controller) IGameController_Release(controller);
    if (gamepad) IGamepad_Release(gamepad);
    if (view) IVectorView_Gamepad_Release(view);
    if (gamepads) IGamepadStatics_Release(gamepads);
    if (users) IUserStatics2_Release(users);
    RoUninitialize();
    return result;
}
