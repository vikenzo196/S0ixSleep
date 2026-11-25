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
uint32_t ecIgnore[8], ecWake[8];
char ecIgnoreText[200], ecWakeText[200];
// Extra GPEs whose status may end the wait although the OS did not arm them (for example a lid GPIO that has
// no _PRW). Their status is cleared when the wait starts, so a source that stays asserted never wakes it.
uint32_t wakeGpe[8];
char wakeGpeText[200];

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
// PWRMBASE the firmware stores in \PWRM: SLP_S0 debug (0x10B4), power gating PPFEAR (0x1590), low power
// mode status (0x1700), SLP_S0 residency (0x193C), LTR ignore (0x1B0C) and the per-IP / aggregated LTR
// values (0x1B50). Snapshots at the start and the end of the wait; the sysctl also reads them live.
struct PmcRange { uint16_t off, count; };
constexpr PmcRange PmcRanges[] = {{0x10B4, 3}, {0x1590, 2}, {0x1700, 48}, {0x193C, 1}, {0x1B0C, 1}, {0x1B50, 48}};
constexpr unsigned PmcDwords = 3 + 2 + 48 + 1 + 1 + 48;
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

void preparePmc() {
	if (!pmcDiag || pmcMmio || intel != 1) return;
	UInt32 base = 0;
	const IOReturn e = ecDevice ? ecDevice->evaluateInteger("\\PWRM", &base) : kIOReturnNotFound;
	pmcEvalErr = (uint32_t)e;
	if (e == kIOReturnSuccess && base && !(base & 0xFFF) && mapPmc(base)) { pmcSource = 1; return; }
	// Cannon Point to Alder Point keep the PMC at this fixed address (Linux intel_pmc_core does the same).
	if (mapPmc(0xFE000000ULL)) pmcSource = 2;
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
uint32_t resOk = 0, resPm[7];   // bits 0-6 ResMsr, 7 core C7 residency 0x3FE, 31 probed

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
	if (pmc) pmcSnapshot(pmcStart);
	ApicSave as {};
	const bool mw = wait == 2 && mwaitReady && tpms && enterMwait(as);
	const uint64_t mwPeriod = tpms * (uint64_t)periodMs;
	const uint32_t irq0 = gS0ixTimerIrqs;
	const uint64_t c70 = mw && (rok & (1U << 7)) ? rdmsr(0x3FE) : 0;
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
			lastCoreMw = (((c1 - c0) & 0xFFFFFFFFULL) * 1000000ULL >> esu) / ms;
		} else lastPkgMw = lastCoreMw = 0;
	}
	if (reason == 1) outw(pm1Sts, Pm1PwrBtn);
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
int periodHandler(struct sysctl_oid *o, void *, int, struct sysctl_req *r) { return intHandler(o, &periodMs, 5, 1000, r); }
int replayHandler(struct sysctl_oid *o, void *, int, struct sysctl_req *r) { return intHandler(o, &ecReplay, 0, 1, r); }
int pmcDiagHandler(struct sysctl_oid *o, void *, int, struct sysctl_req *r) {
	const int error = intHandler(o, &pmcDiag, 0, 1, r);
	if (!error && r->newptr && pmcDiag && platformReady) preparePmc();
	return error;
}
// "base=0x... ranges=off:count,... live=<hex> start=<hex> end=<hex>" (hex dwords, the ranges concatenated)
char pmcText[3 * PmcDwords * 9 + 160];
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
	return sysctl_handle_string(oidp, pmcText, sizeof(pmcText), req);
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
		" sleeps=%u reason=0x%x loops=%llu ms=%llu pkg_mw=%llu core_mw=%llu hwp=0x%llx gpe_en=%s"
		" ec_ign=%u ec_q=%x,%x,%x,%x userwake=%u gt=%d/0x%x hooks=%u/%u"
		" mwait=%u period_ms=%d timer_irqs=%u core_c7_ms=%llu apic_found=0x%llx mwait_fail=%u"
		 " pc=2:%u,3:%u,6:%u,7:%u,8:%u,9:%u,10:%u res_ok=0x%x ec_replay=%d replayed=%u/%u wake_gpe=%s gpe_sts=%s>%s",
		route, enabled, platformReady, lps0, pm1Sts, gpe0Sts, gpe0Bytes, ecCmd, ecData, ecGpe, kbc, intel, hwpCap, waitPkg, tscPerMs,
		shared.sleeps, lastReason, lastLoops, lastMs, lastPkgMw, lastCoreMw, lastHwp, en,
		ecIgnored, ecLast[0], ecLast[1], ecLast[2], ecLast[3], userWakes, gtReset, gtResult, hookCount, hookCalls,
		lastMwait, periodMs, mwaitIrqs, coreC7Ms, apicFound, mwaitFail,
		resPm[0], resPm[1], resPm[2], resPm[3], resPm[4], resPm[5], resPm[6], resOk, ecReplay, replayedLast, replayedTotal,
		wakeGpeText[0] ? wakeGpeText : "-", s0, s1);
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
SYSCTL_PROC(_debug_s0ix, OID_AUTO, safety, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, safetyHandler, "I", "wake after N s (0 never)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, gtreset, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, gtHandler, "I", "1 = reset the Intel GT on wake");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, ec_ignore, CTLTYPE_STRING | CTLFLAG_RW, nullptr, 0, listHandler, "A", "EC events that do not wake (hex)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, ec_wake, CTLTYPE_STRING | CTLFLAG_RW, nullptr, 1, listHandler, "A", "only these EC events wake (hex)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, wake_gpe, CTLTYPE_STRING | CTLFLAG_RW, nullptr, 2, listHandler, "A", "extra GPEs that wake (hex)");
SYSCTL_PROC(_debug_s0ix, OID_AUTO, stat, CTLTYPE_STRING | CTLFLAG_RD, nullptr, 0, statHandler, "A", "status");

void S0ix::install(KernelPatcher &patcher) {
	char arg[200];
	if (PE_parse_boot_argn("s0ixecign", arg, sizeof(arg))) parseList(arg, ecIgnore, ecIgnoreText, sizeof(ecIgnoreText));
	if (PE_parse_boot_argn("s0ixecwake", arg, sizeof(arg))) parseList(arg, ecWake, ecWakeText, sizeof(ecWakeText));
	if (PE_parse_boot_argn("s0ixwakegpe", arg, sizeof(arg))) parseList(arg, wakeGpe, wakeGpeText, sizeof(wakeGpeText));

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
	sysctl_register_oid(&sysctl__debug_s0ix_safety);
	sysctl_register_oid(&sysctl__debug_s0ix_gtreset);
	sysctl_register_oid(&sysctl__debug_s0ix_ec_ignore);
	sysctl_register_oid(&sysctl__debug_s0ix_ec_wake);
	sysctl_register_oid(&sysctl__debug_s0ix_wake_gpe);
	sysctl_register_oid(&sysctl__debug_s0ix_stat);
	SYSLOG("s0ix", "acpi_sleep_kernel route %d, requestUserActive %s", route, requestUserActive ? "found" : "missing");
}
