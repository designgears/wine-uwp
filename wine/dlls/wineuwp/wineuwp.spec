@ stdcall -private DllCanUnloadNow()
@ stdcall -private DllGetClassObject(ptr ptr ptr)

# ext-ms-win-sysmain-pfsapi-l1-1-0 (SysMain's offerable heap). Windows.UI.Xaml's OfferableHeap delay-loads
# these; a failed delay load aborts the process. They report the heap as unavailable and XAML uses the
# normal heap.
@ stdcall PfsOfferableHeapInitialize(ptr ptr ptr ptr)
@ stdcall PfsOfferableHeapStart(ptr ptr ptr ptr)
@ stdcall PfsOfferableHeapAlloc(ptr long ptr ptr)
@ stdcall PfsOfferableHeapFree(ptr ptr ptr ptr)
@ stdcall PfsOfferableHeapOffer(ptr ptr ptr ptr)
@ stdcall PfsOfferableHeapReclaim(ptr ptr ptr ptr)
@ stdcall PfsOfferableHeapCleanup(ptr ptr ptr ptr)
