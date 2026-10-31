#include <assert.h>
#include <stdint.h>

#include "zzapp_runtime.h"
#include "zzapp_proto.h"

int main(void)
{
    zzapp_boot_init();
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    assert(zzapp_loader_begin() == 0);
    assert(zzapp_runtime_state() == ZZAPP_ST_LOADING);
    assert(zzapp_loader_chunk(0, "x", 1) == 0);
    assert(zzapp_loader_abort() == 0);
    assert(zzapp_runtime_state() == ZZAPP_ST_IDLE);
    return 0;
}
