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
//  Devices were powered down by their drivers, so the package can still reach a low power state.
//
//  The wait. XNU has shut the local APIC of this CPU down, so no interrupt arrives by itself: the event
//  registers are polled (PM1 status, GPE0 status against GPE0 enable). Ports come from the FADT
//  (PM1a_EVT_BLK, GPE0_BLK) and, for the embedded controller, the ECDT or 0x62/0x66. Between polls:
//   wait 2 (MWAIT): the core sleeps in MWAIT C10 until the LAPIC timer fires (debug.s0ix.period ms, default 50).
//     The APIC is enabled for the wait in the mode the kernel uses, its timer (TSC deadline mode) points at
//     vector 0xF2 of a copy of the kernel's IDT whose gate only sends EOI, and the task priority is 0xE0, so no
//     kernel vector can be delivered. Everything is put back exactly as found before returning. With the core
//     in C7 the package can leave PC0 (Galaxy Book3 Ultra: PC3 98% of the time, 2.9 W -> 1.85 W).
//   wait 1 (TPAUSE): TPAUSE C0.2 for ~1 ms. Simple, but C0.2 is C0: the package stays in PC0.
//   wait 0: PAUSE loop (debugging only).
//  With wait >= 1 the core's HWP request is set to its lowest performance level for the wait.
//
//  EC events. The EC's own GPE (_GPE of the PNP0C09 device) fires for every EC event: battery, AC,
//  thermal, fans, as well as the lid and the key that should wake us. When it rises with no keyboard
//  byte waiting in the 8042, the EC event is read (ACPI EC QR_EC 0x84). With an ignore list
//  (debug.s0ix.ec_ignore) those events are consumed and the wait goes on; with a wake list
//  (debug.s0ix.ec_wake) only those events wake. With neither, every EC event wakes. The _Qxx method of a
//  consumed event cannot run in the wait; with debug.s0ix.ec_replay=1 (default) each consumed event's method
//  is evaluated once after the wake (thread context, 0.5 s after kIOMessageSystemHasPoweredOn), so the OS
//  sees the battery and AC changes that happened during the sleep at once.
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

// Gate of the LAPIC timer vector used by the MWAIT wait: count, EOI (xAPIC MMIO or x2APIC MSR), iretq.
extern "C" {
__attribute__((used)) volatile uint32_t gS0ixTimerIrqs = 0;
__attribute__((used)) volatile uint64_t gS0ixEoi = 0;   // xAPIC: virtual address of the EOI register; 0: x2APIC
void s0ixTimerGate();
void s0ixSpuriousGate();
}
asm(R"(
	.section __TEXT,__text,regular,pure_instructions
	.p2align 4
	.private_extern _s0ixTimerGate
_s0ixTimerGate:
	.byte 0xf3, 0x0f, 0x1e, 0xfa
	pushq %rax
	pushq %rcx
	pushq %rdx
	lock incl _gS0ixTimerIrqs(%rip)
	movq _gS0ixEoi(%rip), %rax
	testq %rax, %rax
	jz 1f
	movl $0, (%rax)
	jmp 2f
1:
	movl $0x80B, %ecx
	xorl %eax, %eax
	xorl %edx, %edx
	wrmsr
2:
	popq %rdx
	popq %rcx
	popq %rax
	iretq
	.p2align 4
	.private_extern _s0ixSpuriousGate
_s0ixSpuriousGate:
	.byte 0xf3, 0x0f, 0x1e, 0xfa
	iretq
)");

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
int waitMode = 1;       // 2 = MWAIT + LAPIC timer, 1 = TPAUSE, 0 = PAUSE spin (all >= 1 with the HWP floor)
int periodMs = 50;      // wait 2: LAPIC timer period
int safetySec = 0;      // wake by itself after N seconds (0 = never)
int gtReset = 0;        // 1 = reset the Intel GT before returning (Gen12+, see README)
int ecReplay = 1;       // 1 = run the _Qxx methods of the EC events consumed during the sleep after wake
int pmcDiag = 0;        // 1 = snapshot Intel PCH PMC registers (LTR, power gating, SLP_S0) around each sleep
int pciDiag = 0;        // 1 = snapshot the power and link state of every PCI function inside each sleep
uint32_t ecIgnore[8], ecWake[8];
char ecIgnoreText[200], ecWakeText[200];
// Extra GPEs whose status may end the wait although the OS did not arm them (for example a lid GPIO that has
// no _PRW). Their status is cleared when the wait starts, so a source that stays asserted never wakes it.
uint32_t wakeGpe[8];
char wakeGpeText[200];
// Lid state in EC RAM (byte lidAddr, bit lidBit, 1 = open unless lidInvert), read every 500 ms during the wait: the
// lid wakes the machine even when the EC does not send its lid event. -1 = off.
int lidAddr = -1, lidBit = 0;
bool lidInvert = false;
char lidText[16] = "-";
int lidEntry = -1, lidLast = -1;
uint32_t lidErrors = 0;

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
uint64_t lastLoops = 0, lastMs = 0, lastPkgMw = 0, lastCoreMw = 0, lastHwp = 0, lastPsysMw = 0;
uint32_t lastMwait = 0;   // 1 = last wait used MWAIT, 2 = asked for but not entered (see mwait_fail)
uint8_t gpeEn[MaxGpeBytes], gpeStsStart[MaxGpeBytes], gpeStsEnd[MaxGpeBytes];
volatile bool userWakePending = false;
thread_call_t userWakeCall = nullptr;
// EC events consumed during the wait, replayed after wake.
IOACPIPlatformDevice *ecDevice = nullptr;
uint32_t replayMap[8];
volatile bool replayPending = false;
thread_call_t replayCall = nullptr;
uint32_t replayedLast = 0, replayedTotal = 0;
// Battery re-read. Some embedded controllers (Samsung Galaxy Book3) report a wrong remaining capacity for a few
// seconds after a wake, and once about a minute after boot. The macOS battery driver (SMCBatteryManager) then keeps
// that value until its next poll a minute later, or across the next sleep if the wake was a short dark wake. A battery
// notification (0x80) to each PNP0C0A device makes it read _BST again, so a wrong value lasts seconds, not hours.
int battRefresh = 1;                       // 1 = re-read 5, 15, 30 and 60 s after each wake, every 15 s for 4 min after start
constexpr unsigned MaxBatteries = 2;
IOACPIPlatformDevice *batteries[MaxBatteries];
uint32_t batteryCount = 0, battNotifies = 0, battMsg[3];
bool batteriesSearched = false;
thread_call_t battCall = nullptr;
volatile uint32_t battLeft = 0;            // re-reads still to do in the current series
volatile uint32_t battGap = 0;             // seconds between them (0: the wake series 5, 15, 30, 60 s)
// Wait hooks added by other kexts through shared.addHook.
constexpr unsigned HookSlots = 4;
struct Hook { volatile S0ixSleepHook fn; void *volatile ctx; };
Hook hooks[HookSlots];
volatile uint32_t hookCount = 0;
uint32_t hookCalls = 0;
bool addHook(S0ixSleepHook fn, void *ctx) {
	if (!fn) return false;
	for (unsigned i = 0; i < HookSlots; i++) {
		S0ixSleepHook expected = nullptr;
		if (__atomic_compare_exchange_n(&hooks[i].fn, &expected, fn, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			hooks[i].ctx = ctx;
			__atomic_add_fetch(&hookCount, 1, __ATOMIC_ACQ_REL);
			return true;
		}
	}
	return false;
}
void runHooks(uint32_t phase, uint32_t reason) {
	for (unsigned i = 0; i < HookSlots; i++)
		if (S0ixSleepHook fn = __atomic_load_n(&hooks[i].fn, __ATOMIC_ACQUIRE)) { fn(hooks[i].ctx, phase, reason); hookCalls++; }
}
S0ixSleepShared shared {S0ixSleepSharedVersion, 0, 0, 0, 0, HookSlots, addHook};

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

// Reads one byte of EC RAM (RD_EC), or -1 on timeout.
int ecRead(uint8_t addr) {
	if (inb(ecCmd) & 1) (void)inb(ecData);    // drop a stale output byte
	if (!ecWaitFor(false)) return -1;
	outb(ecCmd, 0x80);                        // RD_EC
	if (!ecWaitFor(false)) return -1;
	outb(ecData, addr);
	if (!ecWaitFor(true)) return -1;
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
			if (dev && dev->evaluateInteger("_GPE", &g) == kIOReturnSuccess && g < MaxGpeBytes * 8) {
				ecGpe = (int)g;
				if (!ecDevice) { ecDevice = dev; dev->retain(); }
				break;
			}
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

void probeResidency();
void preparePmc();

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
	probeResidency();
	preparePmc();
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

// ─── PMC diagnostic (read only, opt-in) ─────────────────────────────────────────────────────────
// Intel PCH PMC registers (Cannon Point to Alder Point layout, as Linux intel_pmc_core reads them) at the
// PWRMBASE the firmware stores in \PWRM: SLP_S0 residency on Alder Lake (0x1098), SLP_S0 debug (0x10B4),
// low power mode registers (0x1700, Alder Lake status 0x170C / live 0x1764), SLP_S0 residency on Cannon Point
// to Tiger Lake (0x193C), LTR ignore (0x1B0C), per-IP / aggregated LTR values (0x1B50) and the IP power gating
// status PPFEAR (0x1D90, as in Linux for Cannon Point and later). Snapshots at the start and the end of the
// wait; the sysctl also reads them live.
struct PmcRange { uint16_t off, count; };
// Tiger Lake layout of the low power mode registers (also Alder Lake-P/N and Raptor Lake-P, Linux tgl_l): status latch
// enable 0x1C34, latched status 0x1C3C (6 maps), live status 0x1C5C, LPM_EN 0x1C78, priority 0x1C7C, residencies 0x1C80.
// ETR3 (0x1048) bit 28 clears the latched status; the kext clears it at the start of each wait when pmc_diag is on, so
// the latch shows the conditions at this sleep's first C10 entry.
constexpr PmcRange PmcRanges[] = {{0x1048, 1}, {0x1098, 1}, {0x10B4, 3}, {0x1700, 48}, {0x193C, 1}, {0x1B0C, 1}, {0x1B50, 48},
	{0x1C34, 24}, {0x1D90, 3}};
constexpr unsigned PmcDwords = 1 + 1 + 3 + 48 + 1 + 1 + 48 + 24 + 3;
// Low power mode requirements, as Linux pmc_core_get_tgl_lpm_reqs: _DSM 57a6512e-3979-4e9d-9708-ff13b2508972 function 1 of
// the Intel Power Engine device (INT33A1) returns a buffer of 8 modes x maps requirement dwords.
uint32_t lpmReq[64];
unsigned lpmReqWords = 0;
uint32_t lpmReqErr = 0;
IOMemoryMap *pmcMap = nullptr;
volatile uint32_t *pmcMmio = nullptr;
uint64_t pmcBase = 0;
uint32_t pmcEvalErr = 0, pmcSource = 0;   // source: 1 \\PWRM, 2 default 0xFE000000
uint32_t pmcStart[PmcDwords], pmcEnd[PmcDwords];
bool pmcHave = false;

void pmcSnapshot(uint32_t *out) {
	unsigned k = 0;
	for (const auto &r : PmcRanges)
		for (unsigned i = 0; i < r.count; i++) out[k++] = pmcMmio[(r.off + 4 * i) / 4];
}

bool mapPmc(uint64_t base) {
	IOMemoryMap *m = nullptr;
	if (IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(base, 0x2000, kIODirectionInOut)) {
		m = md->map(kIOMapInhibitCache);
		md->release();
	}
	if (!m) return false;
	auto *r = reinterpret_cast<volatile uint32_t *>(m->getVirtualAddress());
	// Plausibility: an unclaimed MMIO range reads all ones; the aggregated LTR register of a PMC does not.
	if (r[0x1B54 / 4] == 0xFFFFFFFFU && r[0x1B60 / 4] == 0xFFFFFFFFU) { m->release(); return false; }
	pmcMap = m;
	pmcMmio = r;
	pmcBase = base;
	return true;
}

void readLpmReqs() {
	if (lpmReqWords) return;
	OSIterator *it = IOService::getMatchingServices(IOService::nameMatching("INT33A1"));
	OSObject *o = it ? it->getNextObject() : nullptr;
	IOACPIPlatformDevice *pep = o && o->metaCast("IOACPIPlatformDevice") ? static_cast<IOACPIPlatformDevice *>(o) : nullptr;
	if (!pep) { lpmReqErr = 1; if (it) it->release(); return; }
	static const uint8_t uuid[16] = {0x2e, 0x51, 0xa6, 0x57, 0x79, 0x39, 0x9d, 0x4e, 0x97, 0x08, 0xff, 0x13, 0xb2, 0x50, 0x89, 0x72};
	OSObject *params[4] = {OSData::withBytes(uuid, sizeof(uuid)), OSNumber::withNumber(0ULL, 32), OSNumber::withNumber(1ULL, 32),
		OSArray::withCapacity(1)};
	OSObject *result = nullptr;
	const IOReturn e = params[0] && params[1] && params[2] && params[3] ? pep->evaluateObject("_DSM", &result, params, 4) : kIOReturnNoMemory;
	for (auto *p : params) if (p) p->release();
	if (it) it->release();
	if (OSData *d = OSDynamicCast(OSData, result)) {
		const unsigned n = d->getLength() / 4 < 64 ? d->getLength() / 4 : 64;
		memcpy(lpmReq, d->getBytesNoCopy(), n * 4);
		lpmReqWords = n;
	} else lpmReqErr = e ? (uint32_t)e : 2;
	if (result) result->release();
}

void preparePmc() {
	if (pmcDiag) readLpmReqs();
	if (!pmcDiag || pmcMmio || intel != 1) return;
	UInt32 base = 0;
	const IOReturn e = ecDevice ? ecDevice->evaluateInteger("\\PWRM", &base) : kIOReturnNotFound;
	pmcEvalErr = (uint32_t)e;
	if (e == kIOReturnSuccess && base && !(base & 0xFFF) && mapPmc(base)) { pmcSource = 1; return; }
	// Cannon Point to Alder Point keep the PMC at this fixed address (Linux intel_pmc_core does the same).
	if (mapPmc(0xFE000000ULL)) pmcSource = 2;
}

// ─── PCI diagnostics: what stays powered inside the sleep ────────────────────────────────────────────────────────
// For every IOPCIDevice: PMCSR (D-state), PCIe Link Control/Status (ASPM, link up), Device Control 2 (LTR enable)
// and L1 PM Substates Control 1, read through ECAM (acpi-mmcfg-seg0) so the snapshot can be taken inside the wait
// after every driver and hook has run. Capability offsets and ECAM pages are prepared in thread context.
constexpr unsigned PciMax = 64;
struct PciFn { uint16_t bdf, pm, pcie, l1ss; volatile uint8_t *cfg; IOMemoryMap *map; };
struct PciSnap { uint32_t id, pmcsr, lnk, dc2, l1ss; };
PciFn pciFns[PciMax];
PciSnap pciSleep[PciMax];
unsigned pciCount = 0;
uint32_t pciSnaps = 0;
uint64_t pciEcam = 0;
uint32_t rd32(volatile uint8_t *b, unsigned off) { return *reinterpret_cast<volatile uint32_t *>(b + off); }
void pciSnapshot(PciSnap *out) {
	for (unsigned i = 0; i < pciCount; i++) {
		const PciFn &f = pciFns[i];
		PciSnap &s = out[i];
		s.id = rd32(f.cfg, 0);
		s.pmcsr = f.pm ? rd32(f.cfg, f.pm + 4) & 0xFFFF : 0xFFFFFFFFU;
		s.lnk = f.pcie ? rd32(f.cfg, f.pcie + 0x10) : 0xFFFFFFFFU;
		s.dc2 = f.pcie ? rd32(f.cfg, f.pcie + 0x28) & 0xFFFF : 0xFFFFFFFFU;
		s.l1ss = f.l1ss ? rd32(f.cfg, f.l1ss + 8) : 0xFFFFFFFFU;
	}
}
void preparePci() {
	if (!pciDiag || pciCount) return;
	if (OSNumber *n = OSDynamicCast(OSNumber, IOService::getPlatform() ? IOService::getPlatform()->getProperty("acpi-mmcfg-seg0") : nullptr))
		pciEcam = n->unsigned64BitValue();
	if (!pciEcam || (pciEcam & 0xFFFFF)) return;
	OSIterator *it = IOService::getMatchingServices(IOService::serviceMatching("IOPCIDevice"));
	if (!it) return;
	while (OSObject *o = it->getNextObject()) {
		IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o);
		if (!d || pciCount >= PciMax) continue;
		PciFn f {};
		f.bdf = (uint16_t)((d->getBusNumber() << 8) | (d->getDeviceNumber() << 3) | d->getFunctionNumber());
		uint8_t pm = 0, pcie = 0;
		d->findPCICapability(kIOPCIPowerManagementCapability, &pm);
		d->findPCICapability(kIOPCIPCIExpressCapability, &pcie);
		IOByteCount l1 = 0;
		if (pcie) d->extendedFindPCICapability(static_cast<UInt32>(-0x1E), &l1);   // L1 PM Substates (extended ID 0x1E, negated)
		f.pm = pm; f.pcie = pcie; f.l1ss = (uint16_t)l1;
		const uint64_t phys = pciEcam + ((uint64_t)f.bdf << 12);
		// ECAM is physical: map it like the PMC (withPhysicalAddress, uncached).
		IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(phys, 4096, kIODirectionInOut);
		if (!md) continue;
		IOMemoryMap *m = md->map(kIOMapInhibitCache);
		md->release();
		if (!m || !m->getVirtualAddress()) { if (m) m->release(); continue; }
		f.map = m;
		f.cfg = reinterpret_cast<volatile uint8_t *>(m->getVirtualAddress());
		if (rd32(f.cfg, 0) != d->configRead32(0)) { m->release(); continue; }   // not this function's ECAM page
		pciFns[pciCount++] = f;
	}
	it->release();
}

// Sleep-time register writes and reads (debug.s0ix.cfgw, for experiments): PCI config dwords ("bb:dd.f:off") or
// physical MMIO dwords ("mADDR"), changed inside the wait after every driver and hook has run and put back before the
// wake hooks. An entry with "=value/mask" writes (the bits in mask take value); an entry without only reads, at the
// start and at the end of the wait (debug.s0ix.cfgr). Used to find the PCH function that keeps the package awake
// without touching its driver.
constexpr unsigned CfgwMax = 12;
struct Cfgw { bool write; uint32_t value, mask, saved, atEnter, atExit; volatile uint32_t *reg; IOMemoryMap *map; char label[24]; uint32_t delayMs; };
Cfgw cfgw[CfgwMax];
unsigned cfgwCount = 0;
uint32_t cfgwApplied = 0;
bool cfgwAnyWrite = false;
char cfgwText[256] = "";
void cfgwApply() {
	for (unsigned i = 0; i < cfgwCount; i++) {
		Cfgw &c = cfgw[i];
		if (c.delayMs) { IODelay(c.delayMs * 1000); continue; }
		if (c.write) { c.saved = *c.reg; *c.reg = (c.saved & ~c.mask) | (c.value & c.mask); }
		c.atEnter = *c.reg;
	}
	if (cfgwCount) cfgwApplied++;
}
void cfgwRestore() {
	for (unsigned i = cfgwCount; i-- > 0;) {
		Cfgw &c = cfgw[i];
		if (c.delayMs) { IODelay(c.delayMs * 1000); continue; }
		c.atExit = *c.reg;
		if (c.write) *c.reg = c.saved;
	}
	if (cfgwAnyWrite) IODelay(10000);   // D3hot → D0 needs 10 ms before the function is touched again
}
// One uncached mapping of the page holding a physical dword.
volatile uint32_t *mapDword(uint64_t phys, IOMemoryMap **out) {
	*out = nullptr;
	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(phys & ~0xFFFULL, 4096, kIODirectionInOut);
	IOMemoryMap *m = md ? md->map(kIOMapInhibitCache) : nullptr;
	if (md) md->release();
	if (!m || !m->getVirtualAddress()) { if (m) m->release(); return nullptr; }
	*out = m;
	return reinterpret_cast<volatile uint32_t *>(m->getVirtualAddress() + (phys & 0xFFC));
}

// PCI-PM L1.2 in sleep (debug.s0ix.l12, SLEEP_L12 in s0ixsleep.conf). A PCIe link whose downstream device sits in D3hot
// during the sleep stays in L1 with its reference clock request asserted unless L1 PM Substates are enabled, and an
// active clock request on a CPU root port keeps the package out of PC6 and deeper (on the tested machine the NVMe link
// alone held it in PC3: ~1.5 W, PC10 never; with L1.2 in sleep ~0.3 W, PC10 87%). macOS never programs L1 PM Substates,
// so the kext enables PCI-PM L1.2 on the listed root ports and their devices inside the wait, with the timing values
// Linux computes (aspm_calc_l12_info), and puts the registers back before the wake hooks. Only when the device is in
// D3hot at that point. Opt-in per root port: the board must route the CLKREQ# of that slot.
constexpr unsigned L12Max = 4;
struct L12Link {
	uint16_t port, dev, portL1ss, devL1ss, devPm, portExp, devExp;
	volatile uint8_t *pcfg, *dcfg;
	IOMemoryMap *pmap, *dmap;
	uint32_t ctl1Port, ctl1Dev, ctl2, saved[4];
	bool active;
};
L12Link l12[L12Max];
unsigned l12Count = 0;
uint32_t l12Applied = 0, l12Skipped = 0;
char l12Text[64] = "";
volatile uint8_t *ecamMap(uint64_t ecam, uint16_t bdf, IOMemoryMap **out) {
	*out = nullptr;
	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(ecam + ((uint64_t)bdf << 12), 4096, kIODirectionInOut);
	IOMemoryMap *m = md ? md->map(kIOMapInhibitCache) : nullptr;
	if (md) md->release();
	if (!m || !m->getVirtualAddress()) { if (m) m->release(); return nullptr; }
	*out = m;
	return reinterpret_cast<volatile uint8_t *>(m->getVirtualAddress());
}
uint16_t findCap(volatile uint8_t *cfg, uint8_t id) {
	if (!(rd32(cfg, 4) & (1U << 20))) return 0;
	uint8_t p = (uint8_t)(rd32(cfg, 0x34) & 0xFC);
	for (unsigned n = 0; p && n < 48; n++) {
		const uint32_t v = rd32(cfg, p);
		if ((v & 0xFF) == id) return p;
		p = (uint8_t)((v >> 8) & 0xFC);
	}
	return 0;
}
uint16_t findExtCap(volatile uint8_t *cfg, uint16_t id) {
	uint16_t p = 0x100;
	for (unsigned n = 0; p >= 0x100 && n < 64; n++) {
		const uint32_t v = rd32(cfg, p);
		if (v == 0 || v == 0xFFFFFFFFU) return 0;
		if ((v & 0xFFFF) == id) return p;
		p = (uint16_t)((v >> 20) & 0xFFC);
	}
	return 0;
}
uint32_t tPowerOnUs(uint32_t cap) {
	static const uint32_t scaleUs[] = {2, 10, 100, 0};
	return scaleUs[(cap >> 16) & 3] * ((cap >> 19) & 0x1F);
}
// Sets up one root port "bb:dd.f": device 0 of its secondary bus, both with PCI-PM L1.2 and L1 PM Substates.
int l12Prepare(uint64_t ecam, uint16_t portBdf, L12Link &l) {
	l = {};
	l.port = portBdf;
	if (!(l.pcfg = ecamMap(ecam, portBdf, &l.pmap))) return ENOENT;
	const uint32_t bus = (rd32(l.pcfg, 0x18) >> 8) & 0xFF;
	if ((rd32(l.pcfg, 0x0C) & 0x7F0000) != 0x010000 || !bus) return ENODEV;   // not a bridge, or no secondary bus
	l.dev = (uint16_t)(bus << 8);
	if (!(l.dcfg = ecamMap(ecam, l.dev, &l.dmap)) || rd32(l.dcfg, 0) == 0xFFFFFFFFU) return ENODEV;
	l.portL1ss = findExtCap(l.pcfg, 0x1E);
	l.devL1ss = findExtCap(l.dcfg, 0x1E);
	l.devPm = findCap(l.dcfg, 0x01);
	l.portExp = findCap(l.pcfg, 0x10);
	l.devExp = findCap(l.dcfg, 0x10);
	if (!l.portL1ss || !l.devL1ss || !l.devPm || !l.portExp || !l.devExp) return ENOTSUP;
	const uint32_t pc = rd32(l.pcfg, l.portL1ss + 4), dc = rd32(l.dcfg, l.devL1ss + 4);
	if (!(pc & dc & 0x01) || !(pc & dc & 0x10)) return ENOTSUP;   // PCI-PM L1.2 and L1 PM Substates on both ends
	// As Linux aspm_calc_l12_info: the larger Common_Mode_Restore_Time and T_POWER_ON of the two ends, and
	// LTR_L1.2_THRESHOLD = 2 us (T(POWER_OFF)) + 4 us (T(L1.2)) + Common_Mode_Restore_Time.
	const uint32_t cmrt = ((pc >> 8) & 0xFF) > ((dc >> 8) & 0xFF) ? ((pc >> 8) & 0xFF) : ((dc >> 8) & 0xFF);
	const uint32_t tpo = tPowerOnUs(pc) >= tPowerOnUs(dc) ? pc : dc;
	l.ctl2 = (((tpo >> 19) & 0x1F) << 3) | ((tpo >> 16) & 3);
	const uint64_t ns = (2ULL + 4ULL + cmrt) * 1000ULL;
	uint32_t scale = 0, value = (uint32_t)ns;
	for (uint64_t unit = 1; scale < 5 && ns > unit * 0x3FF; ) { scale++; unit = scale == 1 ? 32 : scale == 2 ? 1024 : scale == 3 ? 32768 : scale == 4 ? 1048576 : 33554432; value = (uint32_t)((ns + unit - 1) / unit); }
	const uint32_t th = (scale << 29) | ((value & 0x3FF) << 16);
	l.ctl1Port = th | (cmrt << 8) | 0x01;
	l.ctl1Dev = th | 0x01;
	return 0;
}
void l12Apply() {
	for (unsigned i = 0; i < l12Count; i++) {
		L12Link &l = l12[i];
		l.active = false;
		if ((rd32(l.dcfg, l.devPm + 4) & 3) != 3) { l12Skipped++; continue; }   // device not in D3hot: leave the link alone
		volatile uint32_t *p1 = reinterpret_cast<volatile uint32_t *>(l.pcfg + l.portL1ss + 8), *p2 = p1 + 1;
		volatile uint32_t *d1 = reinterpret_cast<volatile uint32_t *>(l.dcfg + l.devL1ss + 8), *d2 = d1 + 1;
		if ((*p1 & 1) && (*d1 & 1)) { l12Applied++; continue; }   // already in place (awake L1.2): nothing to change
		l.saved[0] = *p1; l.saved[1] = *p2; l.saved[2] = *d1; l.saved[3] = *d2;
		*p2 = l.ctl2; *d2 = l.ctl2;                 // T_POWER_ON, upstream end first
		*p1 = l.ctl1Port; *d1 = l.ctl1Dev;          // thresholds and PCI-PM L1.2 enable, upstream end first
		l.active = true;
		l12Applied++;
	}
}
void l12Restore() {
	for (unsigned i = l12Count; i-- > 0;) {
		L12Link &l = l12[i];
		if (!l.active) continue;
		volatile uint32_t *p1 = reinterpret_cast<volatile uint32_t *>(l.pcfg + l.portL1ss + 8), *p2 = p1 + 1;
		volatile uint32_t *d1 = reinterpret_cast<volatile uint32_t *>(l.dcfg + l.devL1ss + 8), *d2 = d1 + 1;
		*d1 = l.saved[2]; *p1 = l.saved[0];         // disable, downstream end first (wakes the link out of L1.2)
		*d2 = l.saved[3]; *p2 = l.saved[1];
		l.active = false;
	}
}

// ASPM L1.2 while awake (AWAKE_L12 / debug.s0ix.l12_awake) on the SLEEP_L12 links, with the order Linux uses in
// pcie_config_aspm_link: ASPM L1 off on both ends (device first), timing and thresholds, L1.2 enables (ASPM and PCI-PM,
// port first), ASPM L1 back on (port first). Programmed again after each wake if the registers came back without it.
int l12Awake = 0;
uint32_t l12AwakeRuns = 0;
thread_call_t l12AwakeCall = nullptr;
void l12AwakeSet(bool on) {
	for (unsigned i = 0; i < l12Count; i++) {
		L12Link &l = l12[i];
		volatile uint16_t *pl = reinterpret_cast<volatile uint16_t *>(l.pcfg + l.portExp + 0x10);
		volatile uint16_t *dl = reinterpret_cast<volatile uint16_t *>(l.dcfg + l.devExp + 0x10);
		volatile uint32_t *p1 = reinterpret_cast<volatile uint32_t *>(l.pcfg + l.portL1ss + 8), *p2 = p1 + 1;
		volatile uint32_t *d1 = reinterpret_cast<volatile uint32_t *>(l.dcfg + l.devL1ss + 8), *d2 = d1 + 1;
		if ((rd32(l.dcfg, 0) == 0xFFFFFFFFU) || (rd32(l.dcfg, l.devPm + 4) & 3)) continue;   // device gone or not in D0
		const uint16_t pAspm = *pl & 3, dAspm = *dl & 3;
		*dl = (uint16_t)(*dl & ~3U); *pl = (uint16_t)(*pl & ~3U);
		if (on) {
			*p2 = l.ctl2; *d2 = l.ctl2;
			*p1 = (l.ctl1Port & ~0xFU); *d1 = (l.ctl1Dev & ~0xFU);
			*p1 = l.ctl1Port | 0x5; *d1 = l.ctl1Dev | 0x5;   // ASPM L1.2 and PCI-PM L1.2
		} else {
			*d1 = 0; *p1 = 0; *d2 = 0; *p2 = 0;
		}
		*pl = (uint16_t)(*pl | pAspm); *dl = (uint16_t)(*dl | dAspm);
		l12AwakeRuns++;
	}
}
void l12AwakeWork(thread_call_param_t, thread_call_param_t) {
	if (!l12Awake) return;
	for (unsigned i = 0; i < l12Count; i++) {
		const L12Link &l = l12[i];
		if ((rd32(l.pcfg, l.portL1ss + 8) & 5) != 5 || (rd32(l.dcfg, l.devL1ss + 8) & 5) != 5) { l12AwakeSet(true); break; }
	}
}

// Intel Integrated Sensor Hub wake-up (ISH_WAKE / debug.s0ix.ish). Without a host driver the ISH firmware stays in
// WAIT_FOR_HOST and is never power gated, which keeps the PCH out of its deepest state (SLP_S0). The reset handshake of
// the Linux driver (intel-ish-ipc: host ready, DMA enable, MNG_RESET_NOTIFY, answer the firmware's own
// MNG_RESET_NOTIFY with MNG_RESET_NOTIFY_ACK) brings the firmware up (ILUP, ISHTP ready, sensor app running); with no
// client talking to it, it then power gates itself, awake and asleep. Only for the ISH generations that run firmware
// from flash (Broxton to Raptor Lake); the IPC registers are as in Linux hw-ish-regs.h.
constexpr uint16_t IshIds[] = {0x0AA2, 0x1AA2, 0x5AA2, 0x9D35, 0x9DFC, 0x31A2, 0xA37C, 0x34FC, 0xA135, 0x02FC, 0x06FC,
	0x4BB3, 0xA0FC, 0x43FC, 0x7AF8, 0x51FC, 0x54FC, 0x7A78};
constexpr unsigned IshFwSts = 0x34, IshHostComm = 0x38, IshH2iDrbl = 0x48, IshI2hDrbl = 0x54, IshI2hMsg = 0x60,
	IshH2iMsg = 0xE0, IshRmp2 = 0x368;
IOMemoryMap *ishMap = nullptr;
volatile uint8_t *ishMmio = nullptr;
int ishWake = 0;
uint32_t ishRuns = 0, ishOk = 0, ishLast = 0;
thread_call_t ishCall = nullptr;
uint32_t ishRd(unsigned off) { return *reinterpret_cast<volatile uint32_t *>(ishMmio + off); }
void ishWr(unsigned off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(ishMmio + off) = v; }
bool ishSendMng(uint32_t cmd, uint32_t payload) {
	for (unsigned i = 0; i < 100 && (ishRd(IshH2iDrbl) & 0x80000000U); i++) IOSleep(1);
	if (ishRd(IshH2iDrbl) & 0x80000000U) return false;
	ishWr(IshH2iMsg, payload);
	ishWr(IshH2iDrbl, 0x80000000U | (3U << 10) | (cmd << 16) | 4U);   // busy, protocol MNG, command, 4 bytes
	return true;
}
// Config space and BAR are reached directly (ECAM, physical BAR): IOPCIFamily puts the driverless ISH in D3 with memory
// decoding off at boot, and its config accessors do not bring it back for a client that is not its driver.
volatile uint8_t *ishCfg = nullptr;
IOMemoryMap *ishCfgMap = nullptr;
uint16_t ishPm = 0;
bool ishUp = false;
uint32_t ishCfg32(unsigned off) { return rd32(ishCfg, off); }
void ishCfgWr16(unsigned off, uint16_t v) { *reinterpret_cast<volatile uint16_t *>(ishCfg + off) = v; }
bool ishFind() {
	if (ishCfg) return true;
	uint64_t ecam = 0;
	if (OSNumber *n = OSDynamicCast(OSNumber, IOService::getPlatform() ? IOService::getPlatform()->getProperty("acpi-mmcfg-seg0") : nullptr))
		ecam = n->unsigned64BitValue();
	if (!ecam || (ecam & 0xFFFFF)) return false;
	for (uint16_t dev = 0; dev < 32 && !ishCfg; dev++) {
		IOMemoryMap *m = nullptr;
		volatile uint8_t *c = ecamMap(ecam, (uint16_t)(dev << 3), &m);
		if (!c) continue;
		const uint32_t id = rd32(c, 0);
		bool match = (id & 0xFFFF) == 0x8086;
		if (match) { match = false; for (uint16_t k : IshIds) if (k == (id >> 16)) match = true; }
		if (!match) { m->release(); continue; }
		ishCfg = c; ishCfgMap = m;
		ishPm = findCap(c, 0x01);
	}
	if (!ishCfg || !ishPm) return false;
	const uint64_t bar = ((uint64_t)ishCfg32(0x14) << 32) | (ishCfg32(0x10) & ~0xFULL);
	if (!bar || (ishCfg32(0x10) & 1)) return false;
	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(bar, 4096, kIODirectionInOut);
	ishMap = md ? md->map(kIOMapInhibitCache) : nullptr;
	if (md) md->release();
	if (!ishMap) return false;
	ishMmio = reinterpret_cast<volatile uint8_t *>(ishMap->getVirtualAddress());
	return true;
}
void ishPowerUp() {
	if ((ishCfg32(ishPm + 4) & 3) != 0) { ishCfgWr16(ishPm + 4, (uint16_t)(ishCfg32(ishPm + 4) & ~3U)); IOSleep(10); }
	ishCfgWr16(0x04, (uint16_t)(ishCfg32(0x04) | 0x6));   // memory space + bus master
}
// Thread context only (sleeps). Returns true when the firmware reports ILUP + ISHTP ready.
bool ishHandshake() {
	if (!ishFind()) return false;
	ishRuns++;
	ishPowerUp();
	ishLast = ishRd(IshFwSts);
	if (ishLast == 0xFFFFFFFFU) { ishUp = false; return false; }
	if ((ishLast & 3) == 3 && (ishRd(IshHostComm) & 0x80)) { ishUp = true; ishOk++; return true; }   // already up
	ishWr(IshHostComm, ishRd(IshHostComm) | 0x80);         // host ready
	ishWr(IshRmp2, 1);                                      // DMA enabled
	ishWr(IshI2hDrbl, 0);
	if (!ishSendMng(3, 1)) return false;                    // MNG_RESET_NOTIFY, reset id 1
	for (unsigned i = 0; i < 200; i++) {                    // up to 2 s, as Linux TIMEOUT_FOR_FW_RDY_MS
		const uint32_t d = ishRd(IshI2hDrbl);
		if (d & 0x80000000U) {
			if (((d >> 10) & 0xF) == 3 && ((d >> 16) & 0x3F) == 3) {   // the firmware's MNG_RESET_NOTIFY: acknowledge it
				const uint32_t resetId = ishRd(IshI2hMsg) & 0xFFFF;
				ishWr(IshHostComm, ishRd(IshHostComm) | 0x80);
				ishSendMng(4, resetId);
			}
			ishWr(IshI2hDrbl, 0);
		}
		ishLast = ishRd(IshFwSts);
		if (ishLast != 0xFFFFFFFFU && (ishLast & 3) == 3) break;
		IOSleep(10);
	}
	for (unsigned i = 0; i < 20; i++) {   // the completion indication of our ACK
		if (ishRd(IshI2hDrbl) & 0x80000000U) ishWr(IshI2hDrbl, 0);
		IOSleep(10);
	}
	ishLast = ishRd(IshFwSts);
	if (ishLast == 0xFFFFFFFFU || (ishLast & 3) != 3) { ishUp = false; return false; }
	ishUp = true;
	ishOk++;
	return true;
}
// Shutdown and restart: as Linux ish_shutdown, DMA off and host not ready before D3, so that the firmware is not left
// waiting on a host that is gone (it then ignored the next boot's handshake).
void ishShutdown() {
	if (!ishUp || !ishMmio || !ishCfg) return;
	ishWr(IshRmp2, 0);
	for (unsigned i = 0; i < 20 && (ishRd(IshFwSts) & 0xF0000U); i++) IODelay(5000);
	ishWr(IshHostComm, ishRd(IshHostComm) & ~0x80U);
	ishCfgWr16(ishPm + 4, (uint16_t)((ishCfg32(ishPm + 4) & ~3U) | 3U));
	ishUp = false;
}
uint32_t ishChecks = 0;
// Re-check: after the call that enabled it (IOPCIFamily may still power the device down), and after each wake.
void ishWork(thread_call_param_t, thread_call_param_t) {
	if (!ishWake) return;
	ishChecks++;
	if (!ishFind()) return;
	const uint32_t f = (ishCfg32(ishPm + 4) & 3) == 0 && (ishCfg32(0x04) & 2) ? ishRd(IshFwSts) : 0xFFFFFFFFU;
	if (f == 0xFFFFFFFFU || (f & 3) != 3) ishHandshake();
}

// 8254 PIT channel 0 (debug.s0ix.pit / PIT_STOP). Firmware leaves it counting in mode 3 (18.2 Hz). On recent Intel PCHs
// the 8254 lives in the ITSS, and a counting 8254 keeps the ITSS clock source request (LPM requirement "ITSS clock
// source request" on Meteor Lake, an unnamed vnn_misc bit on Alder Lake) asserted, so the chipset cannot reach its low
// power mode. macOS never uses the PIT once the TSC deadline timer runs, and with the HPET legacy replacement route set
// its interrupt is not even delivered. Stopping it as Linux does on real hardware (control word only) is harmless.
int pitStop = 0;
uint32_t pitStops = 0;
uint8_t pitStatusBefore = 0xFF, pitStatusAfter = 0xFF;
uint8_t pitStatus() { outb(0x43, 0xE2); return inb(0x40); }   // read-back command: status of counter 0
void pitHalt() {
	pitStatusBefore = pitStatus();
	// Counter 0, lobyte/hibyte, mode 0, and no count: writing the control word stops the counter until a count is
	// written (Linux i8253 shutdown on real hardware; writing a zero count would start a new countdown).
	outb(0x43, 0x30);
	pitStatusAfter = pitStatus();
	pitStops++;
}

// ─── MWAIT wait: APIC timer and IDT ────────────────────────────────────────────────────────────
constexpr uint8_t TimerVector = 0xF2, SpuriousVector = 0xFF;
alignas(4096) uint8_t idtCopy[4096];
alignas(64) volatile uint64_t monitorLine[8];
IOMemoryMap *apicMap = nullptr;
volatile uint32_t *apicMmio = nullptr;   // xAPIC page; nullptr: x2APIC MSRs
bool mwaitReady = false;
uint32_t mwaitFail = 0, mwaitIrqs = 0;
uint64_t apicFound = 0, coreC7Ms = 0;
mach_vm_address_t rdmsrCarefully = 0;
// Package C-state residency: PC2 PC3 PC6 PC7 PC8 PC9 PC10, probed with rdmsr_carefully.
constexpr uint32_t ResMsr[7] = {0x60D, 0x3F8, 0x3F9, 0x3FA, 0x630, 0x631, 0x632};
uint32_t resOk = 0, resPm[7];   // bits 0-6 ResMsr, 7 core C7 residency 0x3FE, 8 platform energy 0x64D, 31 probed

struct __attribute__((packed)) Idtr { uint16_t limit; uint64_t base; };
struct ApicSave { Idtr idt; uint64_t base, deadline; uint32_t svr, lvt, tpr; };

uint32_t apicRead(unsigned off) { return apicMmio ? apicMmio[off / 4] : (uint32_t)rdmsr(0x800 + off / 16); }
void apicWrite(unsigned off, uint32_t v) { if (apicMmio) apicMmio[off / 4] = v; else wrmsr(0x800 + off / 16, v); }

void setGate(uint8_t vector, uint64_t handler, uint16_t cs) {
	uint8_t *g = idtCopy + vector * 16;
	g[0] = (uint8_t)handler; g[1] = (uint8_t)(handler >> 8); g[2] = (uint8_t)cs; g[3] = (uint8_t)(cs >> 8);
	g[4] = 0; g[5] = 0x8E;   // IST 0; present, DPL 0, 64-bit interrupt gate
	g[6] = (uint8_t)(handler >> 16); g[7] = (uint8_t)(handler >> 24);
	for (unsigned i = 0; i < 4; i++) g[8 + i] = (uint8_t)(handler >> (32 + 8 * i));
	g[12] = g[13] = g[14] = g[15] = 0;
}

void probeResidency() {
	if (!rdmsrCarefully || (resOk & 0x80000000U)) return;
	auto rd = reinterpret_cast<int (*)(uint32_t, uint32_t *, uint32_t *)>(rdmsrCarefully);
	uint32_t lo, hi, ok = 0x80000000U;
	if (intel == 1) {
		for (unsigned i = 0; i < 7; i++) if (rd(ResMsr[i], &lo, &hi) == 0) ok |= 1U << i;
		if (rd(0x3FE, &lo, &hi) == 0) ok |= 1U << 7;
		if (rd(0x64D, &lo, &hi) == 0) ok |= 1U << 8;   // MSR_PLATFORM_ENERGY_COUNTER (Psys), same units as RAPL
	}
	resOk = ok;
}

// At runtime (sysctl wait=2): the CPU features the wait needs, and the APIC page mapping, which cannot be
// made in the sleep callback.
int prepareMwait() {
	uint32_t a = 1, b, c = 0, d;
	asm volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
	const bool hasMwait = (c >> 3) & 1, hasDeadline = (c >> 24) & 1;
	a = 6; c = 0;
	asm volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
	const bool arat = (a >> 2) & 1;   // APIC timer keeps running in deep C-states
	if (!hasMwait || !hasDeadline || !arat) return ENOTSUP;
	const uint64_t base = rdmsr(0x1B);
	if (!(base & 0x800)) return ENXIO;
	const bool x2 = (base & 0x400) != 0;
	if (!x2 && !apicMap) {
		if (IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(base & 0xFFFFFF000ULL, 0x1000, kIODirectionInOut)) {
			apicMap = md->map(kIOMapInhibitCache);
			md->release();
		}
		if (!apicMap) return ENOMEM;
	}
	apicMmio = x2 ? nullptr : reinterpret_cast<volatile uint32_t *>(apicMap->getVirtualAddress());
	gS0ixEoi = x2 ? 0 : reinterpret_cast<uint64_t>(apicMmio) + 0xB0;
	mwaitReady = true;
	return 0;
}

// In the sleep callback (interrupts off, other CPUs off). lapic_shutdown has masked the LVTs and may have
// software- or globally-disabled the APIC: it is enabled in the mode it was prepared in, and leaveMwait
// restores the exact state.
bool enterMwait(ApicSave &s) {
	s.base = rdmsr(0x1B);
	apicFound = s.base;
	const bool x2 = apicMmio == nullptr;
	if (s.base & 0x800) {
		if (((s.base & 0x400) != 0) != x2) { mwaitFail = 1; return false; }
	} else {
		wrmsr(0x1B, (s.base | 0x800) & ~0x400ULL);   // disabled -> xAPIC (-> x2APIC)
		if (x2) wrmsr(0x1B, s.base | 0xC00);
	}
	asm volatile("sidt %0" : "=m"(s.idt));
	// XNU sets the IDT limit to 0x1000 + the CPU number: the 256 gates are the first 4096 bytes.
	if ((size_t)s.idt.limit + 1 < sizeof(idtCopy)) {
		if (rdmsr(0x1B) != s.base) wrmsr(0x1B, s.base);
		mwaitFail = 2;
		return false;
	}
	s.svr = apicRead(0xF0);
	s.lvt = apicRead(0x320);
	s.tpr = apicRead(0x80);
	s.deadline = rdmsr(0x6E0);
	memcpy(idtCopy, reinterpret_cast<const void *>(s.idt.base), sizeof(idtCopy));
	uint16_t cs;
	asm volatile("mov %%cs, %0" : "=r"(cs));
	setGate(TimerVector, reinterpret_cast<uint64_t>(&s0ixTimerGate), cs);
	setGate(SpuriousVector, reinterpret_cast<uint64_t>(&s0ixSpuriousGate), cs);
	Idtr ours {s.idt.limit, reinterpret_cast<uint64_t>(idtCopy)};
	asm volatile("lidt %0" : : "m"(ours) : "memory");
	apicWrite(0xF0, (s.svr & ~0xFFU) | 0x100U | SpuriousVector);   // software enabled
	apicWrite(0x80, 0xE0);                                          // only vectors >= 0xF0
	apicWrite(0x320, (2U << 17) | TimerVector);                     // TSC deadline mode, unmasked
	mwaitFail = 0;
	return true;
}

void leaveMwait(const ApicSave &s) {
	wrmsr(0x6E0, 0);
	asm volatile("sti\n\tnop\n\tnop\n\tcli" : : : "memory");   // a deadline that fired after the last cli lands here
	apicWrite(0x320, s.lvt);
	apicWrite(0x80, s.tpr);
	apicWrite(0xF0, s.svr);
	asm volatile("lidt %0" : : "m"(s.idt) : "memory");
	if (s.deadline) wrmsr(0x6E0, s.deadline);
	if (rdmsr(0x1B) != s.base) wrmsr(0x1B, s.base);
}

// ─── the wait ───────────────────────────────────────────────────────────────────────────────────
void s0ixWait(void *) {
	runHooks(S0ixHookEnter, 0);
	l12Apply();
	cfgwApply();
	if (pciDiag && pciCount) { pciSnapshot(pciSleep); pciSnaps++; }
	uint8_t snap[MaxGpeBytes], extra[MaxGpeBytes];
	const unsigned nb = gpe0Bytes;
	// Only GPEs the OS armed for wake (GPE0 enable) may end the wait, plus the extra wake GPEs.
	for (unsigned i = 0; i < nb; i++) {
		extra[i] = (uint8_t)(wakeGpe[i >> 2] >> ((i & 3) * 8));
		if (extra[i]) outb(port(gpe0Sts, i), extra[i]);   // write 1 to clear a stale status
	}
	for (unsigned i = 0; i < nb; i++) {
		snap[i] = gpeStsStart[i] = inb(port(gpe0Sts, i));
		gpeEn[i] = inb(port(gpe0Sts, nb + i));
	}
	outw(pm1Sts, Pm1PwrBtn | Pm1Rtc);   // write 1 to clear
	uint32_t reason = 0;
	uint64_t loops = 0;
	const int wait = waitMode;
	const uint64_t tpms = tscPerMs, t0 = rdtsc();
	const uint64_t safety = safetySec > 0 && tpms ? (uint64_t)safetySec * 1000ULL * tpms : 0;
	const bool rapl = intel == 1;
	const uint64_t e0 = rapl ? rdmsr(MsrPkgEnergy) & 0xFFFFFFFFULL : 0, c0 = rapl ? rdmsr(MsrPp0Energy) & 0xFFFFFFFFULL : 0;
	const bool psys = rapl && (resOk & (1U << 8));
	const uint64_t s0 = psys ? rdmsr(0x64D) & 0xFFFFFFFFULL : 0;
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
	const uint32_t rok = resOk;
	uint64_t r0[7] = {};
	for (unsigned i = 0; i < 7; i++) if (rok & (1U << i)) r0[i] = rdmsr(ResMsr[i]);
	const bool pmc = pmcDiag && pmcMmio;
	if (pmc) {
		pmcMmio[0x1048 / 4] = pmcMmio[0x1048 / 4] | (1U << 28);   // ETR3: clear the latched low power mode status
		pmcSnapshot(pmcStart);
	}
	ApicSave as {};
	const bool mw = wait == 2 && mwaitReady && tpms && enterMwait(as);
	const uint64_t mwPeriod = tpms * (uint64_t)periodMs;
	const uint32_t irq0 = gS0ixTimerIrqs;
	const uint64_t c70 = mw && (rok & (1U << 7)) ? rdmsr(0x3FE) : 0;
	// Lid state from EC RAM. Our own EC transaction can raise the EC's SCI: clear that GPE status again unless a
	// real event is pending (SCI_EVT), so polling never looks like an EC wake.
	const int la = lidAddr;
	const uint8_t lbit = (uint8_t)(1U << lidBit);
	const bool linv = lidInvert, lidOn = la >= 0 && platformReady && tpms;
	bool lidWasClosed = false;
	uint64_t lidNext = 0;
	auto lidPoll = [&]() -> int {   // 1 open, 0 closed, -1 error
		const int v = ecRead((uint8_t)la);
		if (ecGpe >= 0 && (unsigned)ecGpe < nb * 8 && !(inb(ecCmd) & 0x20)) {
			const unsigned i = (unsigned)ecGpe >> 3;
			const uint8_t bit = (uint8_t)(1U << (ecGpe & 7));
			outb(port(gpe0Sts, i), bit);
			snap[i] = (uint8_t)(inb(port(gpe0Sts, i)) & ~bit);
		}
		if (v < 0) { lidErrors++; return -1; }
		lidLast = v;
		return ((v & lbit) != 0) != linv ? 1 : 0;
	};
	lidEntry = lidLast = -1;
	if (lidOn) {
		const int o = lidPoll();
		lidEntry = lidLast;
		lidWasClosed = o == 0;
		lidNext = rdtsc() + tpms * 500;
	}
	while (!reason) {
		const uint16_t pm1 = inw(pm1Sts);
		if (pm1 & Pm1PwrBtn) reason = 1;
		else if (pm1 & Pm1Rtc) reason = 2;
		else for (unsigned i = 0; i < nb; i++) {
			const uint8_t g = inb(port(gpe0Sts, i)), rise = g & ~snap[i] & (gpeEn[i] | extra[i]);
			if (rise) {
				const unsigned gpe = i * 8 + (unsigned)__builtin_ctz(rise);
				if ((int)gpe == ecGpe && rise == (1U << (gpe & 7)) && !(kbc == 1 && (inb(0x64) & 1))) {
					const uint32_t q = ecQuery();
					ecLast[3] = ecLast[2]; ecLast[2] = ecLast[1]; ecLast[1] = ecLast[0]; ecLast[0] = q;
					if (ecIgnorable(q)) {
						ecIgnored++;
						replayMap[q >> 5] |= 1U << (q & 31);
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
		if (!reason && lidOn && rdtsc() >= lidNext) {
			lidNext = rdtsc() + tpms * 500;
			const int o = lidPoll();
			if (o == 0) lidWasClosed = true;
			else if (o == 1 && lidWasClosed) reason = 4;
		}
		if (mw) {
			if (reason) break;
			wrmsr(0x6E0, rdtsc() + mwPeriod);
			asm volatile(".byte 0x0f, 0x01, 0xc8" : : "a"(monitorLine), "c"(0U), "d"(0U) : "memory");   // MONITOR
			asm volatile("sti\n\t.byte 0x0f, 0x01, 0xc9\n\tcli" : : "a"(0x60U), "c"(1U) : "memory");   // MWAIT C10
		} else if (wait >= 1 && waitPkg == 1 && tpms) tpause(rdtsc() + tpms);   // ~1 ms
		else spin(2000);
		++loops;
		if (!reason && safety && rdtsc() - t0 >= safety) reason = 3;
	}
	if (mw) {
		mwaitIrqs = gS0ixTimerIrqs - irq0;
		coreC7Ms = (rok & (1U << 7)) ? (rdmsr(0x3FE) - c70) / tpms : 0;
		leaveMwait(as);
	} else mwaitIrqs = 0;
	lastMwait = mw ? 1 : (wait == 2 ? 2 : 0);
	for (unsigned i = 0; i < nb; i++) gpeStsEnd[i] = inb(port(gpe0Sts, i));
	if (reason >= 0x100) {   // an extra wake GPE has no handler in the OS: clear its status here
		const unsigned g = reason - 0x100, i = g >> 3;
		const uint8_t bit = (uint8_t)(1U << (g & 7));
		if (i < nb && (extra[i] & bit) && !(gpeEn[i] & bit)) outb(port(gpe0Sts, i), bit);
	}
	if (pmc) { pmcSnapshot(pmcEnd); pmcHave = true; }
	{
		const uint64_t dt = rdtsc() - t0;
		for (unsigned i = 0; i < 7; i++) resPm[i] = (rok & (1U << i)) && dt ? (uint32_t)((rdmsr(ResMsr[i]) - r0[i]) * 1000ULL / dt) : 0;
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
			lastPsysMw = psys ? ((((rdmsr(0x64D) & 0xFFFFFFFFULL) - s0) & 0xFFFFFFFFULL) * 1000000ULL >> esu) / ms : 0;
			lastCoreMw = (((c1 - c0) & 0xFFFFFFFFULL) * 1000000ULL >> esu) / ms;
		} else lastPkgMw = lastCoreMw = 0;
	}
	if (reason == 1) outw(pm1Sts, Pm1PwrBtn);
	cfgwRestore();
	l12Restore();
	if (gtReset && gtMmio) resetGt();   // legacy option; graphics kexts should do this from a hook
	runHooks(S0ixHookExit, reason);
	lastReason = reason;
	lastLoops = loops;
	shared.reason = reason;
	__atomic_add_fetch(&shared.sleeps, 1, __ATOMIC_RELEASE);
	if (reason != 2 && reason != 3) __atomic_store_n(&userWakePending, true, __ATOMIC_RELEASE);
	if (mapAny(replayMap)) __atomic_store_n(&replayPending, true, __ATOMIC_RELEASE);
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

void replayWork(thread_call_param_t, thread_call_param_t) {
	uint32_t m[8];
	for (unsigned i = 0; i < 8; i++) m[i] = __atomic_exchange_n(&replayMap[i], 0U, __ATOMIC_ACQ_REL);
	uint32_t n = 0;
	if (ecReplay && ecDevice) {
		for (unsigned q = 0; q < 256; q++) {
			if (!((m[q >> 5] >> (q & 31)) & 1)) continue;
			char name[8];
			snprintf(name, sizeof(name), "_Q%02X", q);
			if (ecDevice->validateObject(name) == kIOReturnSuccess && ecDevice->evaluateObject(name) == kIOReturnSuccess) n++;
		}
	}
	replayedLast = n;
	replayedTotal += n;
}

void battSchedule(uint32_t seconds) {
	uint64_t deadline;
	clock_interval_to_deadline(seconds, kSecondScale, &deadline);
	thread_call_enter_delayed(battCall, deadline);
}

void battStart(uint32_t left, uint32_t gap, uint32_t first) {
	if (!battCall || !battRefresh) return;
	thread_call_cancel(battCall);
	__atomic_store_n(&battGap, gap, __ATOMIC_RELEASE);
	__atomic_store_n(&battLeft, left, __ATOMIC_RELEASE);
	battSchedule(first);
}

void battWork(thread_call_param_t, thread_call_param_t) {
	if (!batteriesSearched) {
		batteriesSearched = true;
		// The ACPI battery devices (_HID PNP0C0A), found the same way as the EC (no IOACPIFamily link dependency).
		if (OSIterator *it = IOService::getMatchingServices(IOService::nameMatching("PNP0C0A"))) {
			while (auto *o = it->getNextObject()) {
				if (batteryCount < MaxBatteries && o->metaCast("IOACPIPlatformDevice")) {
					auto *d = static_cast<IOACPIPlatformDevice *>(o);
					d->retain();
					batteries[batteryCount++] = d;
				}
			}
			it->release();
		}
	}
	const uint32_t left = __atomic_load_n(&battLeft, __ATOMIC_ACQUIRE);
	if (!left || !battRefresh) return;
	for (uint32_t i = 0; i < batteryCount; i++)
		batteries[i]->messageClients(kIOACPIMessageDeviceNotification, reinterpret_cast<void *>(static_cast<uintptr_t>(0x80)));
	battNotifies++;
	__atomic_store_n(&battLeft, left - 1, __ATOMIC_RELEASE);
	if (left > 1) {
		// Wake series: 5, 15, 30, 60 s after the wake (left 4, 3, 2, 1 → next gap 10, 15, 30).
		static const uint32_t wakeGaps[] = {0, 30, 15, 10};
		const uint32_t gap = __atomic_load_n(&battGap, __ATOMIC_ACQUIRE);
		battSchedule(gap ? gap : wakeGaps[(left - 1) < 4 ? (left - 1) : 3]);
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
	if (type == kIOMessageSystemHasPoweredOn && replayCall && __atomic_exchange_n(&replayPending, false, __ATOMIC_ACQ_REL)) {
		uint64_t deadline;
		clock_interval_to_deadline(500, kMillisecondScale, &deadline);
		thread_call_enter_delayed(replayCall, deadline);
	}
	if (type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart) ishShutdown();
	if (type == kIOMessageSystemWillSleep && battCall) { __atomic_store_n(&battLeft, 0U, __ATOMIC_RELEASE); thread_call_cancel(battCall); }
	// Battery re-read: start it on every wake signal, dark wakes included (on 27 Nov a dark wake reached neither
	// HasPoweredOn nor the re-read, and the EC's wrong reading stayed until the lid opened); a later signal of the same
	// wake just restarts the series. battMsg counts which signals arrive.
	if (type == kIOMessageSystemWillPowerOn || type == kIOMessageSystemHasPoweredOn || type == kIOMessageSystemCapabilityChange) {
		battMsg[type == kIOMessageSystemWillPowerOn ? 0 : type == kIOMessageSystemHasPoweredOn ? 1 : 2]++;
		battStart(4, 0, 5);
	}
	if (type == kIOMessageSystemHasPoweredOn && l12Awake && l12AwakeCall) {   // the NVMe driver is back in D0 by then
		uint64_t deadline;
		clock_interval_to_deadline(2, kSecondScale, &deadline);
		thread_call_enter_delayed(l12AwakeCall, deadline);
	}
	if (type == kIOMessageSystemHasPoweredOn && pitStop && (pitStatus() & 0x0E) != 0) pitHalt();   // counting again: stop
	if (type == kIOMessageSystemHasPoweredOn && ishWake && ishCall) {
		uint64_t deadline;
		clock_interval_to_deadline(3, kSecondScale, &deadline);
		thread_call_enter_delayed(ishCall, deadline);
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

int waitHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	int value = waitMode;
	const int error = sysctl_handle_int(oidp, &value, 0, req);
	if (error || !req->newptr) return error;
	if (value < 0 || value > 2) return EINVAL;
	if (value == 2) {
		if (!prepare()) return ENXIO;
		if (const int e = prepareMwait()) return e;
	}
	waitMode = value;
	return 0;
}
uint64_t hexField64(const char *&p, const char *stops);
int l12Set(const char *text) {
	if (__atomic_load_n(&shared.sleeping, __ATOMIC_ACQUIRE)) return EBUSY;
	uint64_t ecam = 0;
	if (OSNumber *n = OSDynamicCast(OSNumber, IOService::getPlatform() ? IOService::getPlatform()->getProperty("acpi-mmcfg-seg0") : nullptr))
		ecam = n->unsigned64BitValue();
	if (*text && (!ecam || (ecam & 0xFFFFF))) return ENODEV;
	L12Link parsed[L12Max];
	unsigned count = 0;
	int err = 0;
	for (const char *p = text; *p && count < L12Max && !err;) {
		const uint64_t bus = hexField64(p, ":"); if (*p == ':') p++;
		const uint64_t dev = hexField64(p, "."); if (*p == '.') p++;
		const uint64_t fn = hexField64(p, ","); if (*p == ',') p++;
		if (bus > 0xFF || dev > 0x1F || fn > 7) { err = EINVAL; break; }
		err = l12Prepare(ecam, (uint16_t)((bus << 8) | (dev << 3) | fn), parsed[count]);
		if (err) { if (parsed[count].pmap) parsed[count].pmap->release(); if (parsed[count].dmap) parsed[count].dmap->release(); }
		else count++;
	}
	if (err) { for (unsigned j = 0; j < count; j++) { parsed[j].pmap->release(); parsed[j].dmap->release(); } return err; }
	L12Link old[L12Max];
	const unsigned oldCount = l12Count;
	for (unsigned i = 0; i < oldCount; i++) old[i] = l12[i];
	l12Count = 0;
	for (unsigned i = 0; i < count; i++) l12[i] = parsed[i];
	l12Count = count;
	for (unsigned i = 0; i < oldCount; i++) { old[i].pmap->release(); old[i].dmap->release(); }
	strlcpy(l12Text, text, sizeof(l12Text));
	return 0;
}
int l12AwakeHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	int value = l12Awake;
	const int error = sysctl_handle_int(oidp, &value, 0, req);
	if (error || !req->newptr) return error;
	if (value != 0 && value != 1) return EINVAL;
	if (value && !l12Count) return ENODEV;
	if (__atomic_load_n(&shared.sleeping, __ATOMIC_ACQUIRE)) return EBUSY;
	l12Awake = value;
	l12AwakeSet(value == 1);
	return 0;
}
int l12Handler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	char buf[sizeof(l12Text)];
	strlcpy(buf, l12Text, sizeof(buf));
	const int error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error || !req->newptr) return error;
	return l12Set(buf);
}
int ishHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	int value = ishWake;
	const int error = sysctl_handle_int(oidp, &value, 0, req);
	if (error || !req->newptr) return error;
	if (value != 0 && value != 1) return EINVAL;
	ishWake = value;
	if (!value) return 0;
	const bool ok = ishHandshake();
	if (ishCall) {   // look again in 30 s
		uint64_t deadline;
		clock_interval_to_deadline(30, kSecondScale, &deadline);
		thread_call_enter_delayed(ishCall, deadline);
	}
	return ok ? 0 : EIO;
}
int pitHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	int value = pitStop;
	const int error = sysctl_handle_int(oidp, &value, 0, req);
	if (error || !req->newptr) return error;
	if (value != 0 && value != 1) return EINVAL;
	pitStop = value;
	if (value) pitHalt();
	return 0;
}
int periodHandler(struct sysctl_oid *o, void *, int, struct sysctl_req *r) { return intHandler(o, &periodMs, 5, 1000, r); }
int replayHandler(struct sysctl_oid *o, void *, int, struct sysctl_req *r) { return intHandler(o, &ecReplay, 0, 1, r); }
int pmcDiagHandler(struct sysctl_oid *o, void *, int, struct sysctl_req *r) {
	const int error = intHandler(o, &pmcDiag, 0, 1, r);
	if (!error && r->newptr && pmcDiag && platformReady) preparePmc();
	return error;
}
bool isStop(char c, const char *stops) { for (; *stops; stops++) if (*stops == c) return true; return false; }
uint64_t hexField64(const char *&p, const char *stops) {
	uint64_t v = 0;
	while (*p && !isStop(*p, stops)) {
		const char c = *p++;
		v = (v << 4) | (uint64_t)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
	}
	return v;
}
int cfgwHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	char buf[sizeof(cfgwText)];
	strlcpy(buf, cfgwText, sizeof(buf));
	const int error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error || !req->newptr) return error;
	if (__atomic_load_n(&shared.sleeping, __ATOMIC_ACQUIRE)) return EBUSY;
	uint64_t ecam = 0;
	if (OSNumber *n = OSDynamicCast(OSNumber, IOService::getPlatform() ? IOService::getPlatform()->getProperty("acpi-mmcfg-seg0") : nullptr))
		ecam = n->unsigned64BitValue();
	Cfgw parsed[CfgwMax] {};
	unsigned count = 0;
	int err = 0;
	for (const char *p = buf; *p && count < CfgwMax && !err;) {
		Cfgw c {};
		const char *begin = p;
		uint64_t phys;
		if (*p == 'd' || *p == 'D') {   // "dN": wait N ms (1-50) at this point, both ways
			p++;
			c.delayMs = (uint32_t)hexField64(p, ",");
			if (*p == ',') p++;
			if (!c.delayMs || c.delayMs > 0x50) { err = EINVAL; break; }
			strlcpy(c.label, "delay", sizeof(c.label));
			parsed[count++] = c;
			continue;
		}
		if (*p == 'm' || *p == 'M') {
			p++;
			phys = hexField64(p, "=,");
		} else {
			const uint64_t bus = hexField64(p, ":"); if (*p == ':') p++;
			const uint64_t dev = hexField64(p, "."); if (*p == '.') p++;
			const uint64_t fn = hexField64(p, ":"); if (*p == ':') p++;
			const uint64_t off = hexField64(p, "=,");
			if (!ecam || (ecam & 0xFFFFF) || bus > 0xFF || dev > 0x1F || fn > 7 || off > 0xFFC) { err = EINVAL; break; }
			phys = ecam + (((bus << 8) | (dev << 3) | fn) << 12) + (off & 0xFFC);
		}
		if (*p == '=') {
			p++;
			c.write = true;
			c.value = (uint32_t)hexField64(p, "/,");
			if (*p == '/') { p++; c.mask = (uint32_t)hexField64(p, ","); } else c.mask = 0xFFFFFFFFU;
			if (!c.mask) { err = EINVAL; break; }
		}
		const size_t len = (size_t)(p - begin);
		strlcpy(c.label, begin, len + 1 < sizeof(c.label) ? len + 1 : sizeof(c.label));
		if (*p == ',') p++;
		c.reg = mapDword(phys, &c.map);
		if (!c.reg || (*c.reg == 0xFFFFFFFFU && !(*begin == 'm' || *begin == 'M'))) { if (c.map) c.map->release(); err = ENOENT; break; }
		parsed[count++] = c;
	}
	if (err) { for (unsigned j = 0; j < count; j++) if (parsed[j].map) parsed[j].map->release(); return err; }
	Cfgw old[CfgwMax];
	const unsigned oldCount = cfgwCount;
	for (unsigned i = 0; i < oldCount; i++) old[i] = cfgw[i];
	cfgwCount = 0;
	bool any = false;
	for (unsigned i = 0; i < count; i++) { cfgw[i] = parsed[i]; any |= parsed[i].write; }
	cfgwAnyWrite = any;
	cfgwCount = count;
	for (unsigned i = 0; i < oldCount; i++) if (old[i].map) old[i].map->release();
	strlcpy(cfgwText, buf, sizeof(cfgwText));
	return 0;
}
char cfgrText[1024];
int cfgrHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	size_t u = 0;
	cfgrText[0] = '\0';
	for (unsigned i = 0; i < cfgwCount && u + 80 < sizeof(cfgrText); i++) {
		const Cfgw &c = cfgw[i];
		if (c.delayMs) continue;
		u += (size_t)snprintf(cfgrText + u, sizeof(cfgrText) - u, "%s: before=%08x enter=%08x exit=%08x now=%08x\n",
			c.label, c.write ? c.saved : c.atEnter, c.atEnter, c.atExit, *c.reg);
	}
	return sysctl_handle_string(oidp, cfgrText, sizeof(cfgrText), req);
}
// Immediate MMIO access (debug.s0ix.mmio, for experiments): write "ADDR" to select a physical dword, "ADDR=value/mask"
// to change it now; reading returns the selected address and its value.
IOMemoryMap *mmioMap = nullptr;
volatile uint32_t *mmioReg = nullptr;
uint64_t mmioPhys = 0;
char mmioText[64];
int mmioHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	if (mmioReg) snprintf(mmioText, sizeof(mmioText), "%llx=%08x", mmioPhys, *mmioReg);
	else strlcpy(mmioText, "-", sizeof(mmioText));
	char buf[64];
	strlcpy(buf, mmioText, sizeof(buf));
	const int error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error || !req->newptr) return error;
	const char *p = buf;
	const uint64_t phys = hexField64(p, "=");
	if (!phys) return EINVAL;
	if (!mmioReg || (phys & ~0xFFFULL) != (mmioPhys & ~0xFFFULL)) {
		IOMemoryMap *m = nullptr;
		volatile uint32_t *r = mapDword(phys, &m);
		if (!r) return ENOENT;
		if (mmioMap) mmioMap->release();
		mmioMap = m;
	}
	mmioPhys = phys & ~3ULL;
	mmioReg = reinterpret_cast<volatile uint32_t *>(mmioMap->getVirtualAddress() + (mmioPhys & 0xFFF));
	if (*p == '=') {
		p++;
		const uint32_t v = (uint32_t)hexField64(p, "/");
		uint32_t mask = 0xFFFFFFFFU;
		if (*p == '/') { p++; mask = (uint32_t)hexField64(p, ""); }
		*mmioReg = (*mmioReg & ~mask) | (v & mask);
	}
	return 0;
}
int pciDiagHandler(struct sysctl_oid *o, void *, int, struct sysctl_req *r) {
	const int error = intHandler(o, &pciDiag, 0, 1, r);
	if (!error && r->newptr && pciDiag) preparePci();
	return error;
}
// One line per function: "bb:dd.f id pm=D? lnk=<ctl>/<sts> dc2=<devctl2> l1ss=<ctl1>" for the snapshot of the last
// sleep, then the same for now ("live").
char pciText[PciMax * 2 * 64 + 200];
PciSnap pciLiveSnap[PciMax];
int pciHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	size_t u = (size_t)snprintf(pciText, sizeof(pciText), "ecam=0x%llx functions=%u sleeps=%u\n", pciEcam, pciCount, pciSnaps);
	pciSnapshot(pciLiveSnap);
	for (int pass = 0; pass < 2; pass++) {
		const PciSnap *v = pass ? pciLiveSnap : pciSleep;
		u += (size_t)snprintf(pciText + u, sizeof(pciText) - u, "%s\n", pass ? "live:" : "sleep:");
		for (unsigned i = 0; i < pciCount && u + 80 < sizeof(pciText); i++) {
			const PciFn &f = pciFns[i];
			u += (size_t)snprintf(pciText + u, sizeof(pciText) - u, "%02x:%02x.%x %04x:%04x pm=D%u lnk=%04x/%04x dc2=%04x l1ss=%08x\n",
				f.bdf >> 8, (f.bdf >> 3) & 0x1F, f.bdf & 7, v[i].id & 0xFFFF, v[i].id >> 16,
				v[i].pmcsr == 0xFFFFFFFFU ? 9U : v[i].pmcsr & 3, v[i].lnk & 0xFFFF, v[i].lnk >> 16, v[i].dc2 & 0xFFFF, v[i].l1ss);
		}
	}
	return sysctl_handle_string(oidp, pciText, sizeof(pciText), req);
}
// "base=0x... ranges=off:count,... live=<hex> start=<hex> end=<hex>" (hex dwords, the ranges concatenated)
char pmcText[3 * PmcDwords * 9 + 64 * 9 + 200];
int pmcHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	size_t u = 0;
	auto put = [&](const char *label, const uint32_t *v) {
		u += (size_t)snprintf(pmcText + u, sizeof(pmcText) - u, " %s=", label);
		for (unsigned i = 0; i < PmcDwords && u + 10 < sizeof(pmcText); i++)
			u += (size_t)snprintf(pmcText + u, sizeof(pmcText) - u, i ? ",%08x" : "%08x", v[i]);
	};
	u = (size_t)snprintf(pmcText, sizeof(pmcText), "base=0x%llx source=%u pwrm_err=0x%x ranges=", pmcBase, pmcSource, pmcEvalErr);
	for (const auto &r : PmcRanges) u += (size_t)snprintf(pmcText + u, sizeof(pmcText) - u, "%x:%u,", r.off, r.count);
	if (pmcMmio) {
		uint32_t live[PmcDwords];
		pmcSnapshot(live);
		put("live", live);
		if (pmcHave) { put("start", pmcStart); put("end", pmcEnd); }
	}
	u += (size_t)snprintf(pmcText + u, sizeof(pmcText) - u, " lpmreq=");
	for (unsigned i = 0; i < lpmReqWords && u + 10 < sizeof(pmcText); i++)
		u += (size_t)snprintf(pmcText + u, sizeof(pmcText) - u, i ? ",%08x" : "%08x", lpmReq[i]);
	if (!lpmReqWords) u += (size_t)snprintf(pmcText + u, sizeof(pmcText) - u, "err%x", lpmReqErr);
	return sysctl_handle_string(oidp, pmcText, sizeof(pmcText), req);
}
int battHandler(struct sysctl_oid *o, void *, int, struct sysctl_req *r) {
	const int e = intHandler(o, &battRefresh, 0, 1, r);
	if (!e && r->newptr && !battRefresh && battCall) { __atomic_store_n(&battLeft, 0U, __ATOMIC_RELEASE); thread_call_cancel(battCall); }
	return e;
}
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
	uint32_t *map = arg2 == 2 ? wakeGpe : arg2 ? ecWake : ecIgnore;
	char *text = arg2 == 2 ? wakeGpeText : arg2 ? ecWakeText : ecIgnoreText;
	char buf[200];
	strlcpy(buf, text, sizeof(buf));
	const int error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error || !req->newptr) return error;
	return parseList(buf, map, text, sizeof(ecIgnoreText)) ? 0 : EINVAL;
}

// "83.0" = EC RAM byte 0x83, bit 0, 1 = open; "~83.0" = 0 = open; "" or "-" = off.
bool parseLid(const char *s) {
	bool inv = false;
	while (*s == ' ') s++;
	if (!*s || (s[0] == '-' && !s[1])) { lidAddr = -1; strlcpy(lidText, "-", sizeof(lidText)); return true; }
	if (*s == '~') { inv = true; s++; }
	unsigned a = 0, digits = 0;
	for (; *s && *s != '.'; s++, digits++) {
		const char c = *s;
		const int h = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (h < 0 || digits >= 2) return false;
		a = a * 16 + (unsigned)h;
	}
	if (!digits || *s != '.' || s[1] < '0' || s[1] > '7' || (s[2] && s[2] != '\n')) return false;
	lidAddr = (int)a; lidBit = s[1] - '0'; lidInvert = inv;
	snprintf(lidText, sizeof(lidText), "%s%02x.%d", inv ? "~" : "", a, lidBit);
	return true;
}
int lidHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	char buf[16];
	strlcpy(buf, lidText, sizeof(buf));
	const int error = sysctl_handle_string(oidp, buf, sizeof(buf), req);
	if (error || !req->newptr) return error;
	return parseLid(buf) ? 0 : EINVAL;
}

char statText[1024];
int statHandler(struct sysctl_oid *oidp, void *, int, struct sysctl_req *req) {
	char en[2 * MaxGpeBytes + 1];
	size_t u = 0;
	en[0] = '\0';
	char s0[2 * MaxGpeBytes + 1], s1[2 * MaxGpeBytes + 1];
	size_t u0 = 0, u1 = 0;
	s0[0] = s1[0] = '\0';
	for (unsigned i = 0; i < gpe0Bytes && u + 3 <= sizeof(en); i++) {
		u += (size_t)snprintf(en + u, sizeof(en) - u, "%02x", gpeEn[i]);
		u0 += (size_t)snprintf(s0 + u0, sizeof(s0) - u0, "%02x", gpeStsStart[i]);
		u1 += (size_t)snprintf(s1 + u1, sizeof(s1) - u1, "%02x", gpeStsEnd[i]);
	}
	snprintf(statText, sizeof(statText),
		"route=%d enable=%d platform=%d lps0=%d pm1=0x%x gpe0=0x%x/%u ec=0x%x/0x%x ecgpe=%d kbc=%d intel=%d hwp_cap=%d waitpkg=%d tsc_ms=%llu"
		" sleeps=%u reason=0x%x loops=%llu ms=%llu pkg_mw=%llu core_mw=%llu psys_mw=%llu hwp=0x%llx gpe_en=%s"
		" ec_ign=%u ec_q=%x,%x,%x,%x userwake=%u gt=%d/0x%x hooks=%u/%u"
		" mwait=%u period_ms=%d timer_irqs=%u core_c7_ms=%llu apic_found=0x%llx mwait_fail=%u"
		 " pc=2:%u,3:%u,6:%u,7:%u,8:%u,9:%u,10:%u res_ok=0x%x ec_replay=%d replayed=%u/%u wake_gpe=%s gpe_sts=%s>%s lid_ec=%s lid=%d>%d/%u batt=%d/%u/%u battmsg=%u/%u/%u cfgw=%u/%u l12=%u/%u/%u l12_awake=%d/%u ish=%d/%u/%u/0x%x pit=%d/%u/0x%x>0x%x/0x%x",
		route, enabled, platformReady, lps0, pm1Sts, gpe0Sts, gpe0Bytes, ecCmd, ecData, ecGpe, kbc, intel, hwpCap, waitPkg, tscPerMs,
		shared.sleeps, lastReason, lastLoops, lastMs, lastPkgMw, lastCoreMw, lastPsysMw, lastHwp, en,
		ecIgnored, ecLast[0], ecLast[1], ecLast[2], ecLast[3], userWakes, gtReset, gtResult, hookCount, hookCalls,
		lastMwait, periodMs, mwaitIrqs, coreC7Ms, apicFound, mwaitFail,
		resPm[0], resPm[1], resPm[2], resPm[3], resPm[4], resPm[5], resPm[6], resOk, ecReplay, replayedLast, replayedTotal,
		wakeGpeText[0] ? wakeGpeText : "-", s0, s1, lidText, lidEntry, lidLast, lidErrors, battRefresh, batteryCount, battNotifies, battMsg[0], battMsg[1], battMsg[2], cfgwCount, cfgwApplied, l12Count, l12Applied, l12Skipped, l12Awake, l12AwakeRuns, ishWake, ishRuns, ishOk, ishLast, pitStop, pitStops, pitStatusBefore, pitStatusAfter, pitStatus());
	return sysctl_handle_string(oidp, statText, sizeof(statText), req);
}

} // namespace

SYSCTL_NODE(_debug, OID_AUTO, s0ix, CTLFLAG_RW, 0, "S0ixSleep");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, enable, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, enableHandler, "I", "1 = sleep in s0ix instead of S3");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, wait, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, waitHandler, "I", "0 spin, 1 TPAUSE, 2 MWAIT + APIC timer");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, period, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, periodHandler, "I", "wait 2: APIC timer period (ms)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, ec_replay, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, replayHandler, "I", "1 = run consumed EC events' _Qxx after wake");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, pmc_diag, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, pmcDiagHandler, "I", "1 = PCH PMC snapshots around sleep");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, pmc, CTLTYPE_STRING | CTLFLAG_RD, nullptr, 0, pmcHandler, "A", "PCH PMC registers (live/start/end)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, pci_diag, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, pciDiagHandler, "I", "1 = PCI power/link snapshot inside each sleep");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, cfgw, CTLTYPE_STRING | CTLFLAG_RW, nullptr, 0, cfgwHandler, "A", "register writes/reads inside the wait: bb:dd.f:off or mADDR, =value/mask to write (hex)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, pit, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, pitHandler, "I", "1 = stop 8254 PIT counter 0 now and after each wake");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, ish, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, ishHandler, "I", "1 = bring the ISH firmware up (reset handshake) now and after each wake");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, l12_awake, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, l12AwakeHandler, "I", "1 = ASPM L1.2 also while awake on the l12 links");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, l12, CTLTYPE_STRING | CTLFLAG_RW, nullptr, 0, l12Handler, "A", "root ports whose link goes to PCI-PM L1.2 in sleep: bb:dd.f,... (hex)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, cfgr, CTLTYPE_STRING | CTLFLAG_RD, nullptr, 0, cfgrHandler, "A", "cfgw entries: values before, at the start and at the end of the last wait");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, mmio, CTLTYPE_STRING | CTLFLAG_RW, nullptr, 0, mmioHandler, "A", "physical dword: ADDR selects, ADDR=value/mask writes now (hex)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, pci, CTLTYPE_STRING | CTLFLAG_RD, nullptr, 0, pciHandler, "A", "PCI D-state, ASPM, LTR enable and L1 substates (sleep/live)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, batt_refresh, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, battHandler, "I", "1 = battery re-read after wake and boot");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, safety, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, safetyHandler, "I", "wake after N s (0 never)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, gtreset, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, gtHandler, "I", "1 = reset the Intel GT on wake");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, ec_ignore, CTLTYPE_STRING | CTLFLAG_RW, nullptr, 0, listHandler, "A", "EC events that do not wake (hex)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, ec_wake, CTLTYPE_STRING | CTLFLAG_RW, nullptr, 1, listHandler, "A", "only these EC events wake (hex)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, lid_ec, CTLTYPE_STRING | CTLFLAG_RW, nullptr, 0, lidHandler, "A", "lid state in EC RAM: byte.bit (hex), ~ = inverted");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, wake_gpe, CTLTYPE_STRING | CTLFLAG_RW, nullptr, 2, listHandler, "A", "extra GPEs that wake (hex)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, stat, CTLTYPE_STRING | CTLFLAG_RD, nullptr, 0, statHandler, "A", "status");

void S0ix::install(KernelPatcher &patcher) {
	char arg[200];
	if (PE_parse_boot_argn("s0ixecign", arg, sizeof(arg))) parseList(arg, ecIgnore, ecIgnoreText, sizeof(ecIgnoreText));
	if (PE_parse_boot_argn("s0ixecwake", arg, sizeof(arg))) parseList(arg, ecWake, ecWakeText, sizeof(ecWakeText));
	if (PE_parse_boot_argn("s0ixwakegpe", arg, sizeof(arg))) parseList(arg, wakeGpe, wakeGpeText, sizeof(wakeGpeText));
	if (PE_parse_boot_argn("s0ixlidec", arg, sizeof(arg))) parseLid(arg);

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
		replayCall = thread_call_allocate(replayWork, nullptr);
		battCall = thread_call_allocate(battWork, nullptr);
		ishCall = thread_call_allocate(ishWork, nullptr);
		l12AwakeCall = thread_call_allocate(l12AwakeWork, nullptr);
		if (PE_parse_boot_argn("s0ixbatt", arg, sizeof(arg)) && arg[0] == '0') battRefresh = 0;
		battStart(16, 15, 20);   // boot: every 15 s from 20 s to about 4 min
		rdmsrCarefully = patcher.solveSymbol(KernelPatcher::KernelID, "_rdmsr_carefully");
		patcher.clearError();
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
	sysctl_register_oid(&sysctl__debug_s0ix_period);
	sysctl_register_oid(&sysctl__debug_s0ix_ec_replay);
	sysctl_register_oid(&sysctl__debug_s0ix_pmc_diag);
	sysctl_register_oid(&sysctl__debug_s0ix_pmc);
	sysctl_register_oid(&sysctl__debug_s0ix_pci_diag);
	sysctl_register_oid(&sysctl__debug_s0ix_pci);
	sysctl_register_oid(&sysctl__debug_s0ix_l12);
	sysctl_register_oid(&sysctl__debug_s0ix_l12_awake);
	// Register access tools for experiments (physical memory and PCI config writes, the 8254 and the ISH handshake):
	// only with the boot argument -s0ixdebug, since they give root raw access to physical memory.
	if (checkKernelArgument("-s0ixdebug")) {
		sysctl_register_oid(&sysctl__debug_s0ix_cfgw);
		sysctl_register_oid(&sysctl__debug_s0ix_cfgr);
		sysctl_register_oid(&sysctl__debug_s0ix_mmio);
		sysctl_register_oid(&sysctl__debug_s0ix_pit);
		sysctl_register_oid(&sysctl__debug_s0ix_ish);
	}
	sysctl_register_oid(&sysctl__debug_s0ix_safety);
	sysctl_register_oid(&sysctl__debug_s0ix_batt_refresh);
	sysctl_register_oid(&sysctl__debug_s0ix_gtreset);
	sysctl_register_oid(&sysctl__debug_s0ix_ec_ignore);
	sysctl_register_oid(&sysctl__debug_s0ix_ec_wake);
	sysctl_register_oid(&sysctl__debug_s0ix_wake_gpe);
	sysctl_register_oid(&sysctl__debug_s0ix_lid_ec);
	sysctl_register_oid(&sysctl__debug_s0ix_stat);
	SYSLOG("s0ix", "acpi_sleep_kernel route %d, requestUserActive %s", route, requestUserActive ? "found" : "missing");
}
