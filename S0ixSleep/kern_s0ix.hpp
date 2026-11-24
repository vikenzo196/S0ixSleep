//
//  kern_s0ix.hpp
//  S0ixSleep
//
//  Copyright © 2026 vikenzo196. BSD 3-Clause License, see LICENSE.
//

#pragma once

#include <Headers/kern_patcher.hpp>

namespace S0ix {

// Routes acpi_sleep_kernel and registers the debug.s0ix sysctl node. Sleep stays off until
// debug.s0ix.enable=1 (the s0ix-arm daemon sets it, then allows sleep with pmset).
void install(KernelPatcher &patcher);

}

// Published on IOResources once the route is installed, so other kexts (graphics drivers that
// must handle a wake where the GPU kept its power) can match on it. Its value is an OSData holding
// the kernel address of an S0ixSleepShared (8 bytes, little endian), valid while S0ixSleep is loaded.
#define kS0ixSleepResource "S0ixSleep"

static constexpr uint32_t S0ixSleepSharedVersion = 1;

struct S0ixSleepShared {
	uint32_t version;            // S0ixSleepSharedVersion
	volatile uint32_t enabled;   // debug.s0ix.enable
	volatile uint32_t sleeping;  // 1 from kIOMessageSystemWillSleep to kIOMessageSystemHasPoweredOn of an s0ix sleep
	volatile uint32_t sleeps;    // completed s0ix waits
	volatile uint32_t reason;    // last wake: 1 power button, 2 RTC, 3 safety timer, 0x100 + GPE number
};
