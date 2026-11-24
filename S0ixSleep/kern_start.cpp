//
//  kern_start.cpp
//  S0ixSleep
//
//  Copyright © 2026 vikenzo196. BSD 3-Clause License, see LICENSE.
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>

#include "kern_s0ix.hpp"

static const char *bootargOff[] {
	"-s0ixoff"
};

static const char *bootargDebug[] {
	"-s0ixdbg"
};

static const char *bootargBeta[] {
	"-s0ixbeta"
};

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	bootargBeta,
	arrsize(bootargBeta),
	KernelVersion::Ventura,
	KernelVersion::Tahoe,
	[]() {
		lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) { S0ix::install(patcher); }, nullptr);
	}
};

// Kext lifecycle as Xcode builds it: libkmod forwards start/stop through _realmain and _antimain to the
// entry points of Lilu's plugin_start.cpp.
extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
extern kern_return_t S0ixSleep_kern_start(kmod_info_t *, void *);
extern kern_return_t S0ixSleep_kern_stop(kmod_info_t *, void *);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(io.github.vikenzo196.S0ixSleep, "0.1.0", _start, _stop)
__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain = S0ixSleep_kern_start;
__attribute__((visibility("hidden"))) kmod_stop_func_t *_antimain = S0ixSleep_kern_stop;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
}
