//
//  kern_s0ix.cpp
//  S0ixSleep
//
//  Copyright © 2026 vikenzo196. BSD 3-Clause License, see LICENSE.
//
//  System sleep for laptops whose firmware has no working S3 (Modern Standby / S0ix only, FADT
//  LOW_POWER_S0_IDLE_CAPABLE): macOS only knows S3, and entering it on such firmware resets or hangs
//  the machine.
//
//  macOS keeps its whole sleep path: apps and drivers are told about sleep and powered down, the other
//  CPUs are turned off and the platform runs _PTS. Only the last step changes. acpi_sleep_kernel ends
//  with a callback that writes SLP_TYP/SLP_EN; with debug.s0ix.enable=1 that callback is replaced. The
//  boot CPU waits in a low power state, polling the ACPI event registers, until a wake event shows up:
//  power button, RTC alarm, or a general purpose event (GPE) that the OS armed for wake (lid, EC, ...).
//  Then it returns, and XNU runs its normal resume path (acpi_sleep_kernel "returns if sleep failed").
//  Devices were powered down by their drivers, so the package can still reach a low power state; the
//  waiting core runs at its lowest HWP performance level and waits in TPAUSE (C0.2) when the CPU has it.
//
//  Interrupts cannot end the wait: XNU has already shut the local APIC of this CPU down, so the event
//  registers are polled about every millisecond (PM1 status, GPE0 status against GPE0 enable). Ports
//  come from the FADT (PM1a_EVT_BLK, GPE0_BLK) and, for the embedded controller, the ECDT or 0x62/0x66.
//
//  EC events. The EC's own GPE (_GPE of the PNP0C09 device) fires for every EC event: battery, AC,
//  thermal, fans, as well as the lid and the key that should wake us. When it rises with no keyboard
//  byte waiting in the 8042, the EC event is read (ACPI EC QR_EC 0x84). With an ignore list
//  (debug.s0ix.ec_ignore) those events are consumed and the wait goes on; with a wake list
//  (debug.s0ix.ec_wake) only those events wake. With neither, every EC event wakes. A consumed event's
//  _Qxx method never runs, so the OS may see that battery or AC change only at the next event.
//
//  Full wake. After a wake caused by the user (power button, a GPE that is not the RTC or the safety
//  timer), the system would come back as a dark wake, because XNU does not recognise the wake reason as
//  user activity. IOPMrootDomain::requestUserActive is called one second after the first capability
//  message, the same promotion XNU uses for its own dark wakes.
//

#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/acpi/IOACPIPlatformExpert.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/thread_call.h>
#include <sys/errno.h>
#include <sys/sysctl.h>

#include "kern_s0ix.hpp"

namespace {

inline uint8_t inb(uint16_t port) { uint8_t v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
inline uint16_t inw(uint16_t port) { uint16_t v; asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
inline void outb(uint16_t port, uint8_t v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
inline void outw(uint16_t port, uint16_t v) { asm volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
inline uint64_t rdmsr(uint32_t m) { uint32_t lo, hi; asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return ((uint64_t)hi << 32) | lo; }
inline void wrmsr(uint32_t m, uint64_t v) { asm volatile("wrmsr" : : "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }
inline uint64_t rdtsc() { uint32_t lo, hi; asm volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
// TPAUSE with ecx bit 0 = 0 selects C0.2; it ends at the EDX:EAX TSC deadline. Encoded as bytes for older assemblers.
inline void tpause(uint64_t deadline) {
	asm volatile(".byte 0x66, 0x0f, 0xae, 0xf1" : : "c"(0U), "a"((uint32_t)deadline), "d"((uint32_t)(deadline >> 32)) : "cc", "memory");
}
inline uint16_t port(uint16_t base, unsigned off) { return (uint16_t)(base + off); }
inline void spin(unsigned n) { for (unsigned k = 0; k < n; k++) __builtin_ia32_pause(); }

constexpr uint32_t MsrPmEnable = 0x770, MsrHwpCapabilities = 0x771, MsrHwpRequest = 0x774;
constexpr uint32_t MsrRaplPowerUnit = 0x606, MsrPkgEnergy = 0x611, MsrPp0Energy = 0x639;
constexpr uint16_t Pm1PwrBtn = 1U << 8, Pm1Rtc = 1U << 10;
constexpr unsigned MaxGpeBytes = 32;

// Accessor for the protected IOACPIPlatformExpert::getACPITableData (a virtual call, no IOACPIFamily link).
struct AcpiTables : public IOACPIPlatformExpert {
	static const OSData *get(IOService *pe, const char *name) { return static_cast<AcpiTables *>(pe)->getACPITableData(name, 0); }
};

// ─── configuration (sysctl debug.s0ix.*) ────────────────────────────────────────────────────────
int enabled = 0;        // 1 = replace the S3 entry with the s0ix wait
int waitMode = 1;       // 1 = HWP floor + TPAUSE, 0 = PAUSE spin
int safetySec = 0;      // wake by itself after N seconds (0 = never)
int gtReset = 0;        // 1 = reset the Intel GT before returning (Gen12+, see README)
uint32_t ecIgnore[8], ecWake[8];
char ecIgnoreText[200], ecWakeText[200];

// ─── platform, read when the sleep is enabled ──────────────────────────────────────────────────
bool platformReady = false;
uint16_t pm1Sts = 0, gpe0Sts = 0, ecCmd = 0x66, ecData = 0x62;
unsigned gpe0Bytes = 0;  // status bytes; the enable bytes follow them
int ecGpe = -1, kbc = -1, waitPkg = -1, lps0 = -1;
int intel = -1, hwpCap = 0;   // MSRs (RAPL energy, HWP) are read only on Intel family 6, HWP only with CPUID.06H:EAX[7]
uint64_t tscPerMs = 0;
IOMemoryMap *gtMap = nullptr;
volatile uint8_t *gtMmio = nullptr;

// ─── state ──────────────────────────────────────────────────────────────────────────────────────
typedef void (*AcpiSleepCallback)(void *);
mach_vm_address_t orgAcpiSleep = 0, requestUserActive = 0;
int route = 0;
uint32_t lastReason = 0, ecIgnored = 0, ecLast[4] = {0x1FF, 0x1FF, 0x1FF, 0x1FF}, userWakes = 0, gtResult = 0;
uint64_t lastLoops = 0, lastMs = 0, lastPkgMw = 0, lastCoreMw = 0, lastHwp = 0;
uint8_t gpeEn[MaxGpeBytes];
volatile bool userWakePending = false;
thread_call_t userWakeCall = nullptr;
S0ixSleepShared shared {S0ixSleepSharedVersion, 0, 0, 0, 0};

bool mapHas(const uint32_t *m, uint32_t q) { return q < 256 && (m[q >> 5] >> (q & 31)) & 1; }
bool mapAny(const uint32_t *m) { for (unsigned i = 0; i < 8; i++) if (m[i]) return true; return false; }

// ─── embedded controller ────────────────────────────────────────────────────────────────────────
bool ecWaitFor(bool outputFull) {
	for (unsigned i = 0; i < 20000; i++) {
		const uint8_t st = inb(ecCmd);
		if (outputFull ? (st & 1) : !(st & 2)) return true;
		spin(50);
	}
	return false;
}

// Returns the EC event number, or 0x100 no event pending, 0x101/0x102 EC timeout.
uint32_t ecQuery() {
	if (!(inb(ecCmd) & 0x20)) return 0x100;   // SCI_EVT clear
	if (!ecWaitFor(false)) return 0x101;
	outb(ecCmd, 0x84);                        // QR_EC
	if (!ecWaitFor(true)) return 0x102;
	return inb(ecData);
}

bool ecIgnorable(uint32_t q) {
	if (q > 0xFF) return false;
	if (mapAny(ecWake)) return !mapHas(ecWake, q);
	return mapHas(ecIgnore, q);
}

// ─── platform discovery ─────────────────────────────────────────────────────────────────────────
uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t le64(const uint8_t *p) { return le32(p) | ((uint64_t)le32(p + 4) << 32); }

bool readPlatform() {
	IOService *pe = IOService::getPlatform();
	if (!pe || !pe->metaCast("IOACPIPlatformExpert")) return false;
	const OSData *facp = AcpiTables::get(pe, "FACP");
	if (!facp || facp->getLength() < 116) return false;
	auto f = static_cast<const uint8_t *>(facp->getBytesNoCopy());
	uint64_t pm1 = le32(f + 56), gpe = le32(f + 80);
	// ACPI 2.0+: the extended blocks win when they are in system I/O space.
	if (facp->getLength() >= 232) {
		if (f[148] == 1 && le64(f + 152)) pm1 = le64(f + 152);
		if (f[220] == 1 && le64(f + 224)) gpe = le64(f + 224);
	}
	lps0 = (int)((le32(f + 112) >> 21) & 1);
	const unsigned pm1Len = f[88], gpeLen = f[92];
	if (!pm1 || pm1 > 0xFFFF || pm1Len < 4 || !gpe || gpe > 0xFFFF || gpeLen < 2) return false;
	pm1Sts = (uint16_t)pm1;
	gpe0Sts = (uint16_t)gpe;
	gpe0Bytes = gpeLen / 2 > MaxGpeBytes ? MaxGpeBytes : gpeLen / 2;

	int ecdtGpe = -1;
	if (const OSData *ecdt = AcpiTables::get(pe, "ECDT"); ecdt && ecdt->getLength() >= 65) {
		auto e = static_cast<const uint8_t *>(ecdt->getBytesNoCopy());
		if (e[36] == 1 && le64(e + 40) && le64(e + 40) <= 0xFFFF) ecCmd = (uint16_t)le64(e + 40);
		if (e[48] == 1 && le64(e + 52) && le64(e + 52) <= 0xFFFF) ecData = (uint16_t)le64(e + 52);
		ecdtGpe = e[64];
	}
	if (OSIterator *it = IOService::getMatchingServices(IOService::nameMatching("PNP0C09"))) {
		while (auto *o = it->getNextObject()) {
			auto *dev = o->metaCast("IOACPIPlatformDevice") ? static_cast<IOACPIPlatformDevice *>(o) : nullptr;
			UInt32 g = 0;
			if (dev && dev->evaluateInteger("_GPE", &g) == kIOReturnSuccess && g < MaxGpeBytes * 8) { ecGpe = (int)g; break; }
		}
		it->release();
	}
	if (ecGpe < 0 && ecdtGpe >= 0 && ecdtGpe < (int)(MaxGpeBytes * 8)) ecGpe = ecdtGpe;
	kbc = inb(0x64) != 0xFF;   // no legacy keyboard controller: the port floats high
	return true;
}

void mapGt() {
	if (gtMmio) return;
	OSIterator *it = IOService::getMatchingServices(IOService::serviceMatching("IOPCIDevice"));
	if (!it) return;
	while (auto *o = it->getNextObject()) {
		auto *d = OSDynamicCast(IOPCIDevice, o);
		if (!d || d->getBusNumber() || d->getDeviceNumber() != 2 || d->getFunctionNumber()) continue;
		if ((d->configRead32(kIOPCIConfigVendorID) & 0xFFFF) != 0x8086) continue;
		gtMap = d->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
		if (gtMap) gtMmio = reinterpret_cast<volatile uint8_t *>(gtMap->getVirtualAddress());
		break;
	}
	it->release();
}

bool prepare() {
	if (!platformReady) platformReady = readPlatform();
	if (!platformReady) return false;
	if (intel < 0) {
		auto cpuid = [](uint32_t leaf, uint32_t r[4]) { r[0] = leaf; r[2] = 0; asm volatile("cpuid" : "+a"(r[0]), "=b"(r[1]), "+c"(r[2]), "=d"(r[3])); };
		uint32_t r[4];
		cpuid(0, r);
		const uint32_t maxLeaf = r[0];
		const bool genuine = r[1] == 0x756E6547 && r[3] == 0x49656E69 && r[2] == 0x6C65746E;   // "GenuineIntel"
		cpuid(1, r);
		intel = genuine && ((r[0] >> 8) & 0xF) == 6;
		if (intel && maxLeaf >= 6) { cpuid(6, r); hwpCap = (r[0] >> 7) & 1; }
		waitPkg = 0;
		if (maxLeaf >= 7) { cpuid(7, r); waitPkg = (int)((r[2] >> 5) & 1); }
	}
	if (!tscPerMs) {
		const uint64_t n0 = mach_absolute_time(), t0 = rdtsc();
		IODelay(20000);
		const uint64_t n1 = mach_absolute_time(), t1 = rdtsc();
		uint64_t ns = 0;
		absolutetime_to_nanoseconds(n1 - n0, &ns);
		if (ns) tscPerMs = (t1 - t0) * 1000000ULL / ns;
	}
	if (gtReset) mapGt();
	return true;
}

// The render engine keeps its state through an s0ix sleep, while a graphics driver's sleep/wake path may
// expect the cold GT of an S3 resume. GEN6_GDRST (0x941C) GRDOM_FULL with forcewake held, like Linux
// __intel_gt_reset. Forcewake offsets are the Gen11/Gen12 ones (render, GT, VDBOX0, VEBOX0).
void resetGt() {
	static constexpr uint32_t fw[4][2] = {{0xA278, 0x0D84}, {0xA188, 0x130044}, {0xA540, 0x0D50}, {0xA560, 0x0D70}};
	auto reg = [](uint32_t r) { return reinterpret_cast<volatile uint32_t *>(gtMmio + r); };
	for (auto &f : fw) {
		*reg(f[0]) = 0x00010001U;
		for (unsigned n = 0; !(*reg(f[1]) & 1U) && n < 200; n++) spin(2000);
	}
	volatile uint32_t *gdrst = reg(0x941C);
	*gdrst = 1U;
	uint32_t n = 0;
	while ((*gdrst & 1U) && n < 2000) { spin(2000); n++; }
	gtResult = (*gdrst & 1U) ? 0x80000000U | n : n + 1;
	for (auto &f : fw) *reg(f[0]) = 0x00010000U;
}

// ─── the wait ───────────────────────────────────────────────────────────────────────────────────
void s0ixWait(void *) {
	uint8_t snap[MaxGpeBytes];
	const unsigned nb = gpe0Bytes;
	// Only GPEs the OS armed for wake (GPE0 enable) may end the wait.
	for (unsigned i = 0; i < nb; i++) { snap[i] = inb(port(gpe0Sts, i)); gpeEn[i] = inb(port(gpe0Sts, nb + i)); }
	outw(pm1Sts, Pm1PwrBtn | Pm1Rtc);   // write 1 to clear
	uint32_t reason = 0;
	uint64_t loops = 0;
	const int wait = waitMode;
	const uint64_t tpms = tscPerMs, t0 = rdtsc();
	const uint64_t safety = safetySec > 0 && tpms ? (uint64_t)safetySec * 1000ULL * tpms : 0;
	const bool rapl = intel == 1;
	const uint64_t e0 = rapl ? rdmsr(MsrPkgEnergy) & 0xFFFFFFFFULL : 0, c0 = rapl ? rdmsr(MsrPp0Energy) & 0xFFFFFFFFULL : 0;
	uint64_t hwp = 0;
	bool hwpSet = false;
	if (wait >= 1 && hwpCap && (rdmsr(MsrPmEnable) & 1)) {
		hwp = rdmsr(MsrHwpRequest);
		if (!(hwp & (1ULL << 42))) {   // package-level control: leave it alone
			uint64_t floor = hwp & 0xFF;
			if (!floor) floor = (rdmsr(MsrHwpCapabilities) >> 24) & 0xFF;   // lowest performance
			if (floor) { wrmsr(MsrHwpRequest, (hwp & ~0xFFFFFFULL) | floor | (floor << 8)); hwpSet = true; }
		}
	}
	lastHwp = hwpSet ? hwp : 0;
	while (!reason) {
		const uint16_t pm1 = inw(pm1Sts);
		if (pm1 & Pm1PwrBtn) reason = 1;
		else if (pm1 & Pm1Rtc) reason = 2;
		else for (unsigned i = 0; i < nb; i++) {
			const uint8_t g = inb(port(gpe0Sts, i)), rise = g & ~snap[i] & gpeEn[i];
			if (rise) {
				const unsigned gpe = i * 8 + (unsigned)__builtin_ctz(rise);
				if ((int)gpe == ecGpe && rise == (1U << (gpe & 7)) && !(kbc == 1 && (inb(0x64) & 1))) {
					const uint32_t q = ecQuery();
					ecLast[3] = ecLast[2]; ecLast[2] = ecLast[1]; ecLast[1] = ecLast[0]; ecLast[0] = q;
					if (ecIgnorable(q)) {
						ecIgnored++;
						outb(port(gpe0Sts, i), rise);   // clear this GPE only
						snap[i] = g & (uint8_t)~rise;
						continue;
					}
				}
				reason = 0x100 + gpe;
				break;
			}
			snap[i] = g;
		}
		if (wait >= 1 && waitPkg == 1 && tpms) tpause(rdtsc() + tpms);   // ~1 ms
		else spin(2000);
		++loops;
		if (!reason && safety && rdtsc() - t0 >= safety) reason = 3;
	}
	if (hwpSet) wrmsr(MsrHwpRequest, hwp);
	{
		const uint64_t t1 = rdtsc(), ms = tpms ? (t1 - t0) / tpms : 0;
		lastMs = ms;
		if (rapl && ms) {
			const uint64_t e1 = rdmsr(MsrPkgEnergy) & 0xFFFFFFFFULL, c1 = rdmsr(MsrPp0Energy) & 0xFFFFFFFFULL;
			const uint32_t esu = (uint32_t)(rdmsr(MsrRaplPowerUnit) >> 8) & 0x1F;
			// mW = (counts / 2^esu J) / (ms / 1000 s) * 1000
			lastPkgMw = (((e1 - e0) & 0xFFFFFFFFULL) * 1000000ULL >> esu) / ms;
			lastCoreMw = (((c1 - c0) & 0xFFFFFFFFULL) * 1000000ULL >> esu) / ms;
		} else lastPkgMw = lastCoreMw = 0;
	}
	if (reason == 1) outw(pm1Sts, Pm1PwrBtn);
	if (gtReset && gtMmio) resetGt();
	lastReason = reason;
	lastLoops = loops;
	shared.reason = reason;
	__atomic_add_fetch(&shared.sleeps, 1, __ATOMIC_RELEASE);
	if (reason != 2 && reason != 3) __atomic_store_n(&userWakePending, true, __ATOMIC_RELEASE);
}

void acpiSleepKernel(AcpiSleepCallback func, void *refcon) {
	if (enabled == 1 && platformReady) func = s0ixWait;
	FunctionCast(acpiSleepKernel, orgAcpiSleep)(func, refcon);
}

// ─── full wake ──────────────────────────────────────────────────────────────────────────────────
void userWakeWork(thread_call_param_t, thread_call_param_t) {
	IOPMrootDomain *root = IOService::getPMRootDomain();
	if (root && requestUserActive) {
		reinterpret_cast<void (*)(IOPMrootDomain *, IOService *, const char *)>(requestUserActive)(root, root, "S0ixSleep user wake");
		userWakes++;
	}
}

IOReturn sleepInterest(void *, void *, UInt32 type, IOService *, void *, vm_size_t) {
	if ((type == kIOMessageSystemCapabilityChange || type == kIOMessageSystemWillPowerOn || type == kIOMessageSystemHasPoweredOn) &&
	    userWakeCall && __atomic_exchange_n(&userWakePending, false, __ATOMIC_ACQ_REL)) {
		// Not at once: a graphics driver's dark wake transitions must run first.
		uint64_t deadline;
		clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
		thread_call_enter_delayed(userWakeCall, deadline);
	}
	if (type == kIOMessageSystemWillSleep && enabled == 1 && platformReady) __atomic_store_n(&shared.sleeping, 1U, __ATOMIC_RELEASE);
	else if (type == kIOMessageSystemHasPoweredOn) __atomic_store_n(&shared.sleeping, 0U, __ATOMIC_RELEASE);
	return kIOReturnSuccess;
}

// ─── sysctl ─────────────────────────────────────────────────────────────────────────────────────
int intHandler(struct sysctl_oid *oidp, int *var, int lo, int hi, struct sysctl_req *req) {
	int value = *var;
	const int error = sysctl_handle_int(oidp, &value, 0, req);
	if (error || !req->newptr) return error;
	if (value < lo || value > hi) return EINVAL;
	*var = value;
	return 0;
}

int enableHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	int value = enabled;
	const int error = sysctl_handle_int(oidp, &value, 0, req);
	if (error || !req->newptr) return error;
	if (value < 0 || value > 1) return EINVAL;
	if (!orgAcpiSleep) return ENODEV;
	if (value == 1 && !prepare()) return ENXIO;
	enabled = value;
	shared.enabled = (uint32_t)value;
	return 0;
}

int waitHandler(struct sysctl_oid *o, void *, int, struct sysctl_req *r) { return intHandler(o, &waitMode, 0, 1, r); }
int safetyHandler(struct sysctl_oid *o, void *, int, struct sysctl_req *r) { return intHandler(o, &safetySec, 0, 86400, r); }
int gtHandler(struct sysctl_oid *o, void *, int, struct sysctl_req *r) {
	const int error = intHandler(o, &gtReset, 0, 1, r);
	if (!error && r->newptr && gtReset && enabled) mapGt();
	return error;
}

// "51,52,5b" (hex, separated by commas or spaces) into a 256-bit map and its canonical text.
bool parseList(const char *s, uint32_t *map, char *text, size_t size) {
	uint32_t m[8] = {};
	unsigned v = 0;
	int digits = 0;
	for (const char *p = s;; p++) {
		const char c = *p;
		int h = -1;
		if (c >= '0' && c <= '9') h = c - '0';
		else if (c >= 'a' && c <= 'f') h = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') h = c - 'A' + 10;
		if (h >= 0) {
			if (++digits > 2) return false;
			v = v * 16 + (unsigned)h;
			continue;
		}
		if (c == 'x' && digits == 1 && v == 0) { digits = 0; continue; }   // allow 0x prefixes
		if (c != ',' && c != ' ' && c != '\0' && c != '\n') return false;
		if (digits) { m[v >> 5] |= 1U << (v & 31); v = 0; digits = 0; }
		if (!c) break;
	}
	size_t u = 0;
	text[0] = '\0';
	for (unsigned q = 0; q < 256 && u + 4 < size; q++)
		if ((m[q >> 5] >> (q & 31)) & 1) u += (size_t)snprintf(text + u, size - u, u ? ",%02x" : "%02x", q);
	memcpy(map, m, sizeof(m));
	return true;
}

int listHandler(struct sysctl_oid *oidp, void *, int arg2, struct sysctl_req *req) {
	uint32_t *map = arg2 ? ecWake : ecIgnore;
	char *text = arg2 ? ecWakeText : ecIgnoreText;
	char buf[200];
	strlcpy(buf, text, sizeof(buf));
	const int error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error || !req->newptr) return error;
	return parseList(buf, map, text, sizeof(ecIgnoreText)) ? 0 : EINVAL;
}

char statText[480];
int statHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	char en[2 * MaxGpeBytes + 1];
	size_t u = 0;
	en[0] = '\0';
	for (unsigned i = 0; i < gpe0Bytes && u + 3 <= sizeof(en); i++) u += (size_t)snprintf(en + u, sizeof(en) - u, "%02x", gpeEn[i]);
	snprintf(statText, sizeof(statText),
		"route=%d enable=%d platform=%d lps0=%d pm1=0x%x gpe0=0x%x/%u ec=0x%x/0x%x ecgpe=%d kbc=%d intel=%d hwp_cap=%d waitpkg=%d tsc_ms=%llu"
		" sleeps=%u reason=0x%x loops=%llu ms=%llu pkg_mw=%llu core_mw=%llu hwp=0x%llx gpe_en=%s"
		" ec_ign=%u ec_q=%x,%x,%x,%x userwake=%u gt=%d/0x%x",
		route, enabled, platformReady, lps0, pm1Sts, gpe0Sts, gpe0Bytes, ecCmd, ecData, ecGpe, kbc, intel, hwpCap, waitPkg, tscPerMs,
		shared.sleeps, lastReason, lastLoops, lastMs, lastPkgMw, lastCoreMw, lastHwp, en,
		ecIgnored, ecLast[0], ecLast[1], ecLast[2], ecLast[3], userWakes, gtReset, gtResult);
	return sysctl_handle_string(oidp, statText, sizeof(statText), req);
}

} // namespace

SYSCTL_NODE(_debug, OID_AUTO, s0ix, CTLFLAG_RW, 0, "S0ixSleep");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, enable, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, enableHandler, "I", "1 = sleep in s0ix instead of S3");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, wait, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, waitHandler, "I", "0 spin, 1 low power");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, safety, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, safetyHandler, "I", "wake after N s (0 never)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, gtreset, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, gtHandler, "I", "1 = reset the Intel GT on wake");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, ec_ignore, CTLTYPE_STRING | CTLFLAG_RW, nullptr, 0, listHandler, "A", "EC events that do not wake (hex)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, ec_wake, CTLTYPE_STRING | CTLFLAG_RW, nullptr, 1, listHandler, "A", "only these EC events wake (hex)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, stat, CTLTYPE_STRING | CTLFLAG_RD, nullptr, 0, statHandler, "A", "status");

void S0ix::install(KernelPatcher &patcher) {
	char arg[200];
	if (PE_parse_boot_argn("s0ixecign", arg, sizeof(arg))) parseList(arg, ecIgnore, ecIgnoreText, sizeof(ecIgnoreText));
	if (PE_parse_boot_argn("s0ixecwake", arg, sizeof(arg))) parseList(arg, ecWake, ecWakeText, sizeof(ecWakeText));

	const mach_vm_address_t fn = patcher.solveSymbol(KernelPatcher::KernelID, "_acpi_sleep_kernel");
	patcher.clearError();
	static const uint8_t prologue[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56};   // push rbp; mov rbp,rsp; push r15; push r14
	bool ok = fn != 0;
	for (size_t i = 0; ok && i < sizeof(prologue); i++) if (reinterpret_cast<const uint8_t *>(fn)[i] != prologue[i]) ok = false;
	if (ok) {
		orgAcpiSleep = patcher.routeFunction(fn, reinterpret_cast<mach_vm_address_t>(acpiSleepKernel), true);
		if (patcher.getError() != KernelPatcher::Error::NoError) { orgAcpiSleep = 0; patcher.clearError(); }
	}
	route = orgAcpiSleep ? 1 : (fn ? 3 : 2);
	if (orgAcpiSleep) {
		requestUserActive = patcher.solveSymbol(KernelPatcher::KernelID, "__ZN14IOPMrootDomain17requestUserActiveEP9IOServicePKc");
		patcher.clearError();
		userWakeCall = requestUserActive ? thread_call_allocate(userWakeWork, nullptr) : nullptr;
		registerPrioritySleepWakeInterest(sleepInterest, nullptr, nullptr);
		const uint64_t ptr = reinterpret_cast<uint64_t>(&shared);
		if (OSData *d = OSData::withBytes(&ptr, sizeof(ptr))) {
			IOService::publishResource(kS0ixSleepResource, d);
			d->release();
		}
	}
	sysctl_register_oid(&sysctl__debug_s0ix);
	sysctl_register_oid(&sysctl__debug_s0ix_enable);
	sysctl_register_oid(&sysctl__debug_s0ix_wait);
	sysctl_register_oid(&sysctl__debug_s0ix_safety);
	sysctl_register_oid(&sysctl__debug_s0ix_gtreset);
	sysctl_register_oid(&sysctl__debug_s0ix_ec_ignore);
	sysctl_register_oid(&sysctl__debug_s0ix_ec_wake);
	sysctl_register_oid(&sysctl__debug_s0ix_stat);
	SYSLOG("s0ix", "acpi_sleep_kernel route %d, requestUserActive %s", route, requestUserActive ? "found" : "missing");
}
