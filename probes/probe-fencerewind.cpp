// A queued ID3D12CommandQueue::Wait must be met by a signal that lands before the fence is
// rewound from the CPU. Gears of War 4 recycles one-shot shared fences (0 -> 1 -> reset to 0)
// and froze at its title screen when vkd3d only looked at the value once the waiting queue got
// round to the wait. Queue A: Wait(gate, 1), Wait(f, 1), Signal(done, 1). Queue B: Signal(f, 1).
// The CPU waits for f to reach 1, rewinds it to 0, then opens the gate. Prints PASS when done
// reaches 1 and FAIL after a timeout.
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>

static ID3D12Fence *make_fence(ID3D12Device *device)
{
    ID3D12Fence *fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), (void **)&fence);
    return fence;
}

int main()
{
    ID3D12Device *device = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&device);
    if (FAILED(hr)) { printf("D3D12CreateDevice failed %#lx\n", hr); return 1; }

    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *a = nullptr, *b = nullptr;
    device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue), (void **)&a);
    desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    device->CreateCommandQueue(&desc, __uuidof(ID3D12CommandQueue), (void **)&b);

    ID3D12Fence *gate = make_fence(device), *f = make_fence(device), *done = make_fence(device);
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    a->Wait(gate, 1);
    a->Wait(f, 1);
    a->Signal(done, 1);

    b->Signal(f, 1);
    f->SetEventOnCompletion(1, event);
    if (WaitForSingleObject(event, 5000)) { printf("FAIL: f never reached 1\n"); return 1; }
    f->Signal(0);
    printf("f rewound to %llu\n", (unsigned long long)f->GetCompletedValue());

    gate->Signal(1);
    done->SetEventOnCompletion(1, event);
    if (WaitForSingleObject(event, 5000))
    {
        printf("FAIL: queue A still blocked, done %llu\n", (unsigned long long)done->GetCompletedValue());
        return 1;
    }
    printf("PASS: done %llu\n", (unsigned long long)done->GetCompletedValue());
    return 0;
}
