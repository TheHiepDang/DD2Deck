#define CINTERFACE
#include <windows.h>
#include <d3d12.h>
#include <stddef.h>

#include "platform.h"

const unsigned dd2_slots[Slot_Count] = {
#define DD2_SLOT_OFFSET(type, name) (unsigned)(offsetof(type##Vtbl, name) / sizeof(void*)),
    DD2_D3D_SLOTS(DD2_SLOT_OFFSET)
#undef DD2_SLOT_OFFSET
};
const unsigned dd2_allocator_reset_slot = (unsigned)(offsetof(ID3D12CommandAllocatorVtbl, Reset) / sizeof(void*));

int dd2_seh_filter(EXCEPTION_POINTERS* info, void* frame) {
    (void)frame;
    switch (info->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return EXCEPTION_EXECUTE_HANDLER;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

#ifdef _MSC_VER
int dd2_guarded_call(dd2_guarded_fn fn, void* data) {
    __try {
        return fn(data);
    } __except (dd2_seh_filter(GetExceptionInformation(), NULL)) {
        return 0;
    }
}
#endif
