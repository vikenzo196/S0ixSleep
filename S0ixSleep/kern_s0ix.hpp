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

static constexpr uint32_t S0ixSleepSharedVersion = 2;

// Wait hook (version >= 2). Called by S0ixSleep inside the sleep, on the CPU that waits, with interrupts off and
// the other CPUs off: phase S0ixHookEnter just before the low power loop, phase S0ixHookExit just after it (reason
// = the wake reason). Only register (MMIO/port/MSR) accesses and short polls belong there: no locks, allocations,
// IOKit calls or logging. Typical use: a graphics driver resetting or powering down engines that would have lost
// power in a real S3.
enum : uint32_t { S0ixHookEnter = 0, S0ixHookExit = 1 };
typedef void (*S0ixSleepHook)(void *context, uint32_t phase, uint32_t reason);

struct S0ixSleepShared {
	uint32_t version;            // S0ixSleepSharedVersion; consumers accept >= the version they need
	volatile uint32_t enabled;   // debug.s0ix.enable
	volatile uint32_t sleeping;  // 1 from kIOMessageSystemWillSleep to kIOMessageSystemHasPoweredOn of an s0ix sleep
	volatile uint32_t sleeps;    // completed s0ix waits
	volatile uint32_t reason;    // last wake: 1 power button, 2 RTC, 3 safety timer, 0x100 + GPE number
	// version >= 2
	uint32_t hookSlots;          // number of hooks that can be added
	bool (*addHook)(S0ixSleepHook hook, void *context);   // thread context; false when all slots are taken
};
