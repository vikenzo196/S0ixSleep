#!/usr/bin/env python3
"""Decode `sysctl debug.s0ix.pmc` (Intel PCH PMC snapshot, Cannon Point to Alder Point layout).

With the low power mode requirements (from the firmware) it lists the conditions that kept the chipset out of each
enabled S0ix substate at the first C10 entry of the last sleep.

Usage: sudo sysctl debug.s0ix.pmc_diag=1   (once, then sleep and wake)
       python3 pmc_decode.py               (reads the sysctl)
       python3 pmc_decode.py FILE          (decodes a saved copy)

Register names follow Linux drivers/platform/x86/intel/pmc (cnp/tgl/adl maps). Names of reserved or
platform specific slots are best effort.
"""
import subprocess, sys

LTR = {0x1B50: 'CURRENT_PLATFORM', 0x1B54: 'AGGREGATED_SYSTEM', 0x1B60: 'SOUTHPORT_A', 0x1B64: 'SOUTHPORT_B',
       0x1B68: 'SATA', 0x1B6C: 'GIGABIT_ETHERNET', 0x1B70: 'XHCI', 0x1B74: 'SOUTHPORT_G', 0x1B78: 'ME',
       0x1B7C: 'SATA1', 0x1B80: 'SOUTHPORT_C', 0x1B84: 'HD_AUDIO', 0x1B8C: 'LPSS',
       0x1B90: 'SATA2', 0x1B94: 'SOUTHPORT_D', 0x1B98: 'SOUTHPORT_E', 0x1B9C: 'ESPI', 0x1BA0: 'SCC',
       0x1BA4: 'ISH', 0x1BD0: '0x1BD0 (not an IP)', 0x1BF0: 'CNV', 0x1BF4: 'EMMC', 0x1BF8: 'UFSX2',
       0x1BFC: 'WIGIG', 0x1C00: 'SOUTHPORT_F', 0x1C04: 'THC0', 0x1C08: 'THC1'}
DBG = {0x10B4: ['AUDIO_D3', 'OTG_D3', 'XHCI_D3', 'LPIO_D3', 'SDX_D3', 'SATA_D3', 'UFS0_D3', 'UFS1_D3', 'EMMC_D3'],
       0x10B8: ['SDIO_PLL_OFF', 'USB2_PLL_OFF', 'AUDIO_PLL_OFF', 'OC_PLL_OFF', 'MAIN_PLL_OFF', 'XOSC_OFF',
                'LPC_CLKS_GATED', 'PCIE_CLKREQS_IDLE', 'AUDIO_ROSC_OFF', 'HPET_XOSC_CLK_REQ', 'PMC_ROSC_SLOW_CLK',
                'AON2_ROSC_GATED', 'CLKACKS_DEASSERTED'],
       0x10BC: ['MPHY_CORE_GATED', 'CSME_GATED', 'USB2_SUS_GATED', 'DYN_FLEX_IO_IDLE', 'GBE_NO_LINK',
                'THERM_SEN_DISABLED', 'PCIE_LOW_POWER', 'ISH_VNNAON_REQ_ACT', 'ISH_VNN_REQ_ACT', 'CNV_VNNAON_REQ_ACT',
                'CNV_VNN_REQ_ACT', 'NPK_VNNON_REQ_ACT', 'PMSYNC_STATE_IDLE', 'ALST_GT_THRES', 'PMC_ARC_PG_READY']}
PFEAR = ['PMC', 'OPI-DMI', 'SPI/eSPI', 'XHCI', 'SPA', 'SPB', 'SPC', 'GBE', 'SATA', 'HDA_PGD0', 'HDA_PGD1',
         'HDA_PGD2', 'HDA_PGD3', 'SPD', 'LPSS', 'LPC', 'SMB', 'ISH', 'P2SB', 'NPK_VNN', 'SDX', 'SPE', 'Fuse',
         'SBR8', 'RSVD24', 'OTG', 'EXI', 'CSE', 'CSME_KVM', 'CSME_PMT', 'CSME_CLINK', 'CSME_PTIO', 'CSME_USBR',
         'CSME_SUSRAM', 'CSME_SMT1', 'RSVD35', 'CSME_SMS2', 'CSME_SMS', 'CSME_RTC', 'CSME_PSF', 'SBR0', 'SBR1',
         'SBR2', 'SBR3', 'SBR4', 'SBR5', 'CSME_PECI', 'PSF1', 'PSF2', 'PSF3', 'PSF4', 'CNVI', 'UFS0', 'EMMC',
         'SPF', 'SBR6', 'SBR7', 'NPK_AON', 'HDA_PGD4', 'HDA_PGD5', 'HDA_PGD6', 'PSF6', 'PSF7', 'PSF8']


def latency(half):
    if not (half >> 15) & 1:
        return 'none'
    ns = (half & 0x3FF) * (32 ** ((half >> 10) & 7))
    return '%.3g ms' % (ns / 1e6) if ns >= 1e6 else ('%.3g us' % (ns / 1e3) if ns >= 1e3 else '%d ns' % ns)


# Low power mode status maps of the Tiger Lake layout (Linux tgl_lpm_maps; clock source names of Alder Lake, which the
# Alder Lake-P/N and Raptor Lake-P PCH use with this layout). Unnamed bits print as bitN.
LPM_MAPS = [
    ('clock sources', {0: 'CLKPART1_OFF', 1: 'CLKPART2_OFF', 2: 'CLKPART3_OFF', 3: 'CLKPART4_OFF', 4: 'CLKPART5_OFF',
        5: 'CLKPART6_OFF', 6: 'CLKPART7_OFF', 7: 'CLKPART8_OFF', 10: 'PCIE0PLL_OFF', 11: 'PCIE1PLL_OFF', 12: 'PCIE2PLL_OFF',
        13: 'PCIE3PLL_OFF', 14: 'PCIE4PLL_OFF', 15: 'PCIE5PLL_OFF', 16: 'PCIE6PLL_OFF', 18: 'USB2PLL_OFF',
        19: 'PCIe/USB3.1_Gen2PLL_OFF', 20: 'PCIe_Gen3PLL_OFF', 21: 'OPIOPLL_OFF', 22: 'OCPLL_OFF', 23: 'MainPLL/AUDIOPLL_OFF',
        24: 'MIPIPLL/GBEPLL_OFF', 25: 'Fast_XTAL_Osc_OFF', 26: 'AC_Ring_Osc_OFF', 27: 'MC_Ring_Osc_OFF', 29: 'SATAPLL_OFF',
        31: 'XTAL_USB2PLL/USB3PLL_OFF'}),
    ('power gating', {0: 'CSME_PG', 1: 'SATA_PG', 2: 'xHCI_PG', 3: 'UFSX2_PG', 5: 'OTG_PG', 6: 'SPA_PG', 7: 'SPB_PG',
        8: 'SPC_PG', 9: 'SPD_PG', 10: 'SPE_PG', 11: 'SPF_PG', 13: 'LSX_PG', 14: 'P2SB_PG', 15: 'PSF_PG', 16: 'SBR_PG',
        17: 'OPIDMI_PG', 18: 'THC0_PG', 19: 'THC1_PG', 20: 'GBETSN_PG', 21: 'GBE_PG', 22: 'LPSS_PG', 23: 'MMP_UFSX2_PG',
        24: 'MMP_UFSX2B_PG', 25: 'FIA_PG'}),
    ('D3', {0: 'ADSP_D3', 1: 'SATA_D3', 2: 'xHCI0_D3', 5: 'xDCI1_D3', 6: 'SDX_D3', 7: 'EMMC_D3', 8: 'IS_D3', 9: 'THC0_D3',
        10: 'THC1_D3', 11: 'GBE_D3', 12: 'GBE_TSN_D3'}),
    ('VNN requests', {1: 'GPIO_COM0_VNN', 2: 'GPIO_COM1_VNN', 3: 'GPIO_COM2_VNN', 4: 'GPIO_COM3_VNN', 5: 'GPIO_COM4_VNN',
        6: 'GPIO_COM5_VNN', 7: 'Audio_VNN', 8: 'ISH_VNN', 9: 'CNVI_VNN', 10: 'eSPI_VNN', 11: 'Display_VNN', 12: 'DTS_VNN',
        14: 'SMBUS_VNN', 15: 'CSME_VNN', 16: 'SMLINK0_VNN', 17: 'SMLINK1_VNN', 20: 'CLINK_VNN', 21: 'DCI_VNN', 22: 'ITH_VNN',
        24: 'CSME_VNN24', 25: 'GBE_VNN'}),
    ('VNN misc', {0: 'CPU_C10_REQ', 3: 'PCIe_LPM_En_REQ', 5: 'ITH_REQ', 6: 'CNVI_REQ', 7: 'ISH_REQ',
        10: 'USB2_SUS_PG_Sys_REQ', 12: 'PCIe_Clk_REQ', 16: 'MPHY_Core_DL_REQ', 17: 'Break-even_En_REQ',
        18: 'Auto-demo_En_REQ', 22: 'MPHY_SUS_REQ', 24: 'xDCI_attached_REQ'}),
    ('signals', {}),
]
LPM_MODES = ['S0i2.0', 'S0i2.1', 'S0i2.2', 'S0i3.0', 'S0i3.1', 'S0i3.2', 'S0i3.3', 'S0i3.4']


def lpm_report(snaps, keys, req):
    """Requirements of each enabled mode against the latched status (conditions at the first C10 entry of the last
    sleep: the kext clears the latch at the start of each wait) and the live status."""
    if 0x1C78 not in snaps[keys[0]]:
        return
    en = snaps[keys[0]][0x1C78] & 0xFF
    print('\nLow power modes (Tiger Lake layout): LPM_EN 0x1C78 = 0x%02x (%s)' % (en, ', '.join(m for i, m in enumerate(LPM_MODES) if en >> i & 1) or 'none'))
    for m in range(8):
        if not en >> m & 1:
            continue
        resid = [(snaps[k].get(0x1C80 + 4 * m, 0)) for k in keys]
        print('  %s residency counter 0x%X: %s' % (LPM_MODES[m], 0x1C80 + 4 * m, ' '.join('%08x' % r for r in resid)))
        if len(req) < 8 * len(LPM_MAPS):
            print('  requirements not available (lpmreq: _DSM of INT33A1 failed)')
            continue
        for col, base in (('latched at C10 (end of last sleep)', 0x1C3C), ('live now', 0x1C5C)):
            k = 'end' if col.startswith('latched') and 'end' in snaps else 'live'
            missing = []
            for i, (mname, names) in enumerate(LPM_MAPS):
                need = req[m * len(LPM_MAPS) + i] & ~snaps[k].get(base + 4 * i, 0)
                missing += ['%s:%s' % (mname, names.get(b, 'bit%d' % b)) for b in range(32) if need >> b & 1]
            print('  %s, unmet: %s' % (col, ', '.join(missing) or 'none'))


def parse(text):
    f = dict(x.split('=', 1) for x in text.split() if '=' in x)
    ranges = [tuple(int(v, 16) if i == 0 else int(v) for i, v in enumerate(r.split(':')))
              for r in f['ranges'].strip(',').split(',')]
    offs = [o + 4 * i for o, n in ranges for i in range(n)]
    snaps = {k: dict(zip(offs, (int(v, 16) for v in f[k].split(',')))) for k in ('live', 'start', 'end') if k in f}
    req = [int(v, 16) for v in f.get('lpmreq', '').split(',') if v and not v.startswith('err')]
    return f['base'], snaps, req


def main():
    text = open(sys.argv[1]).read() if len(sys.argv) > 1 else \
        subprocess.check_output(['sysctl', '-n', 'debug.s0ix.pmc'], text=True)
    base, snaps, req = parse(text)
    keys = [k for k in ('live', 'start', 'end') if k in snaps]
    print('PMC base', base, '| columns:', ', '.join(keys), '(live = now, start/end = around the last sleep)')
    print('\nLTR (snoop / non-snoop latency tolerance; "none" = no requirement)')
    for off, name in sorted(LTR.items()):
        if off not in snaps[keys[0]]:
            continue
        cells = []
        for k in keys:
            v = snaps[k][off]
            cells.append('%-22s' % ('%s / %s' % (latency(v & 0xFFFF), latency(v >> 16))) if v else '%-22s' % '-')
        print('  %-18s %s' % (name, ' '.join(cells)))
    print('\nLTR ignore 0x1B0C:', ' '.join('%08x' % snaps[k].get(0x1B0C, 0) for k in keys))
    print('\nSLP_S0 debug (1 = condition met)')
    for off, names in DBG.items():
        for i, n in enumerate(names):
            vals = [(snaps[k].get(off, 0) >> i) & 1 for k in keys]
            print('  %-20s %s' % (n, ' '.join(str(v) for v in vals)))
    pfo = 0x1D90 if 0x1D90 in snaps[keys[0]] else 0x1590   # 0x1590 only in snapshots of S0ixSleep 0.1.0-0.1.3
    pf = {k: snaps[k].get(pfo, 0) | (snaps[k].get(pfo + 4, 0) << 32) for k in keys}
    print('\nPPFEAR at 0x%X (1 = IP power gated / off)%s' % (pfo, '' if pfo == 0x1D90 else ': OLD OFFSET, not valid on Cannon Point and later'))
    for i, n in enumerate(PFEAR):
        vals = [(pf[k] >> i) & 1 for k in keys]
        if any(vals) or True:
            print('  %-12s %s' % (n, ' '.join(str(v) for v in vals)))
    if 'start' in snaps and 'end' in snaps:
        for off, what in ((0x1098, 'Alder Lake'), (0x193C, 'Cannon Point to Tiger Lake')):
            if off in snaps['start']:
                d = (snaps['end'][off] - snaps['start'][off]) & 0xFFFFFFFF
                print('\nSLP_S0 residency during the last sleep (0x%X, %s): %d counts (~%.1f s at 122 us/count)' % (off, what, d, d * 122e-6))
    if any(snaps[k].get(0x1700 + 4 * i, 0) for k in keys for i in range(48)):
        print('\nLPM registers 0x1700-0x17BC (Alder Lake-S layout):')
        for k in keys:
            print('  %-5s %s' % (k, ' '.join('%08x' % snaps[k].get(0x1700 + 4 * i, 0) for i in range(48))))
    lpm_report(snaps, keys, req)


if __name__ == '__main__':
    main()
