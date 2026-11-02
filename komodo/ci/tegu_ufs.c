/*
 * TEGU internal UFS root device.
 *
 * The Qualcomm UFS host (UFSHCI 2.1) is left enabled with the link up by the
 * bootloader, which loaded our boot image from it. This driver reuses that
 * state: it points the transfer request list at its own memory (verifying the
 * host took it before ringing any doorbell) and issues polled SCSI commands,
 * one at a time, to LUN 0. It finds one GPT partition by name (boot-arg
 * ufsroot=, default "userdata") and exposes it as the BSD disk ufsroot0
 * (rufsroot0), with 512-byte logical sectors over the device's 4 KiB blocks.
 * Root on it with rd=ufsroot0.
 *
 * All DMA goes through one physically contiguous allocation (descriptors and
 * a 1 MiB bounce buffer); the host is not cache coherent, so caches are
 * cleaned/invalidated around each command.
 *
 * Writes are refused unless boot-arg ufsrw=1 is set. Every write is checked
 * against the partition bounds again right before it is issued, so nothing
 * outside the named partition can be written.
 */
#if defined(ARM64_BOARD_CONFIG_TEGU)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <arm/caches_internal.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include <machine/atomic.h>

extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
/* osfmk/arm/machine_routines.h, not exported to the BSD layer */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
/* osfmk/arm/cpu_data.h, not exported to the BSD layer */
extern int get_preemption_level(void);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern int hz;
static void tegu_log_start(void);
extern void tegu_sample_start(void);
extern void tegu_keys_init(void);
extern void tegu_touch_init(void);
extern void tegu_usb_probe(void);

#define REG_CAP         0x00
#define REG_IS          0x20
#define REG_IE          0x24
#define REG_HCS         0x30
#define REG_HCE         0x34
#define REG_UTRLBA      0x50
#define REG_UTRLBAU     0x54
#define REG_UTRLDBR     0x58
#define REG_UTRLCLR     0x5c
#define REG_UTRLRSR     0x60

#define HCS_DP          (1u << 0)
#define HCS_UTRLRDY     (1u << 1)
#define CAP_64AS        (1u << 24)              /* 64-bit DMA addressing */

#define UFS_BLOCK       4096u
#define BOUNCE_SIZE     (1u << 20)
#define PRDT_CHUNK      (256u << 10)            /* PRDT byte count field: 18 bits */
/*
 * komodo (Tensor G4, Samsung Exynos UFS host): Linux ufs-exynos (gs101/zuma)
 * runs this host with UFSHCD_QUIRK_PRDT_BYTE_GRAN (UTRD offsets/lengths and
 * the PRDT length are in BYTES, not DWORDs) and programs
 * HCI_{TX,RX}PRDT_ENTRY_SIZE = 12, so one PRDT entry moves at most 4 KiB.
 * With FMP inline crypto the entries are 128 bytes instead of 16.
 * boot-args: ufsexynos=0 (back to the tegu/Qualcomm encoding), ufsprdt=128.
 */
#define PRDT_CHUNK_EXYNOS 4096u
#define PRDT_MAX        (BOUNCE_SIZE / PRDT_CHUNK_EXYNOS)
#define PRDT_STRIDE_MAX 128u
#define CMD_TIMEOUT_NS  (5ull * NSEC_PER_SEC)
#define CMD_SPIN_NS     (50ull * NSEC_PER_USEC)
#define CMD_SLEEP_NS    (200ull * NSEC_PER_USEC)

struct utrd {
	uint32_t dw0, dw1, dw2, dw3;
	uint32_t ucdba, ucdbau;
	uint32_t resp;          /* [31:16] response UPIU offset, [15:0] length, DWORDs */
	uint32_t prdt;          /* [31:16] PRDT offset (DWORDs), [15:0] entries */
};

struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	/* entries of 16 (standard) or 128 (Exynos FMP) bytes:
	 * { addr_lo, addr_hi, rsvd, dbc } then FMP key words */
	uint8_t prdt[PRDT_MAX * PRDT_STRIDE_MAX];
};

/* Layout of the contiguous DMA allocation. */
#define DMA_UTRL        0x0000                  /* 1 KiB aligned */
#define DMA_UCD         0x1000                  /* 128 byte aligned */
#define DMA_BOUNCE      0x10000                 /* after UCD 0x1000 + 1 KiB + 256 x 128 B */
#define DMA_SIZE        (DMA_BOUNCE + BOUNCE_SIZE)

static struct {
	vm_offset_t regs;
	vm_offset_t dma;                        /* kernel VA of the DMA allocation */
	uint64_t dma_pa;
	uint8_t lun;
	uint8_t tag;
	uint64_t first_block, nblocks;          /* partition, in 4 KiB device blocks */
	uint32_t secsize;                       /* logical sector size for the BSD layer */
	bool writable;                          /* boot-arg ufsrw=1 */
	bool cache;                             /* boot-arg ufscache=1 */
	bool dma_wc;                            /* DMA area is Normal-NC & outside physmap (ABL area): no cache ops */
	bool wait;                              /* boot-arg ufswait=1 */
	bool exynos;                            /* boot-arg ufsexynos (default 1) */
	uint32_t prdt_stride;                   /* boot-arg ufsprdt: 16 or 128 */
	/* komodo boot-arg ufsmode: bit0 byte-granular UTRD offsets/lengths,
	 * bit1 4 KiB PRDT entries, bit2 set HCI_UTRL_NEXUS_TYPE (SCSI) for the
	 * slot. Default 0x6: zuma's DT says fixed-prdt-req_list-ocs (the
	 * byte-granularity quirk is fixed in this host revision). */
	uint32_t mode;
	vm_offset_t vs;                         /* Exynos vendor HCI (hci + 0x1100) */
	vm_offset_t pma;                        /* Exynos UFS M-PHY PMA (hci + 0x4000) */
	vm_offset_t phy_iso;                    /* PMU UFS-PHY isolation reg page */
	vm_offset_t pma2;                       /* alt PMA candidate (hci + 0x8000) */
	int bmajor, cmajor;
	dev_t bdev;
	bool ready;
} su;

static LCK_GRP_DECLARE(tegu_ufs_grp, "tegu_ufs");
static LCK_MTX_DECLARE(tegu_ufs_lock, &tegu_ufs_grp);

static inline uint32_t
ufs_rd(uint32_t off)
{
	return *(volatile uint32_t *)(su.regs + off);
}

static inline void
ufs_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(su.regs + off) = v;
}

/* Exynos vendor HCI (Linux ufs-exynos.c offsets) */
#define VS_TXPRDT_ENTRY_SIZE    0x00
#define VS_RXPRDT_ENTRY_SIZE    0x04
#define VS_UTRL_NEXUS_TYPE      0x40
#define VS_DATA_REORDER         0x60
#define VS_CLKSTOP_CTRL         0xb0            /* bits 0-4: clock stop */
#define VS_MISC                 0xb4            /* bits 5-7: CLK_CTRL_EN */
#define VS_CLK_STOP_MASK        0x1fu
#define VS_CLK_CTRL_EN_MASK     0xe0u

/* UFSHCI UIC command interface */
#define REG_UICCMD              0x90
#define REG_UICARG1             0x94
#define REG_UICARG2             0x98
#define REG_UICARG3             0x9c
#define HCS_UCRDY               (1u << 3)
#define IS_UPMS                 (1u << 4)       /* UIC power-mode-change status */
#define IS_UHXS                 (1u << 5)       /* hibernate exit status */
#define IS_UE                   (1u << 2)       /* UIC error */
#define IS_UCCS                 (1u << 10)      /* UIC command completion */
#define UIC_DME_HIBER_EXIT      0x18
#define UIC_DME_GET             0x01
#define UIC_DME_PEER_GET        0x03      /* reads a DEVICE (peer) attribute over the link */
#define VS_IOP_ACG_DISABLE      0x100
#define HCI_GPIO_OUT            0x70            /* vendor: bit0 = UFS device RST_n */

/* UniPro attribute IDs (read-only probes). */
#define ATTR_PA_PWRMODE         0x1571      /* current power mode: [3:0] TX, [7:4] RX */
#define ATTR_PA_CONNTXLANES     0x1561      /* connected TX data lanes */
#define ATTR_PA_CONNRXLANES     0x1581      /* connected RX data lanes */

/*
 * komodo (Tensor G4) fencing: the Exynos UFS AXI master can only reach DRAM
 * the bootloader/EL3 allowed (sysreg IO-coherency) and, for data, the FMP/SMU
 * protector must be open or the controller never completes a transfer (OCS
 * 0xf). Both regions come from the device tree (reg[3] ufsp @ 0x132a0000 and
 * samsung,sysreg-phandle sysreg_ufs @ 0x13020000); hard-coded here because the
 * stock tegu driver only maps the HCI reg range. Gated behind ufsmode bits so
 * the default path is untouched. Offsets per Linux drivers/ufs/host/ufs-exynos.c.
 */
#define KOMO_UFSP_PA            0x132a0000ull   /* UFS protector (FMP/SMU)  */
#define KOMO_SYSREG_UFS_PA      0x13020000ull   /* sysreg_ufs               */
#define UFSPRSECURITY           0x010
#define UFSP_NSSMU              (1u << 14)
#define UFSPSBEGIN0             0x200
#define UFSPSEND0               0x204
#define UFSPSLUN0               0x208
#define UFSPSCTRL0             0x20c
#define SYSREG_UFS_IOCC         0x710           /* ufs-iocc: mask 3, val 3  */

static inline uint32_t
vs_rd(uint32_t off)
{
	return su.vs ? *(volatile uint32_t *)(su.vs + off) : 0xdeadu;
}

static inline void
vs_wr(uint32_t off, uint32_t v)
{
	if (su.vs) {
		*(volatile uint32_t *)(su.vs + off) = v;
	}
}

/*
 * Exynos/gs101 M-PHY PMA (the "phy-pma" reg). On komodo the UFS PHY is a
 * separate block at hci_base + 0x4000 (mainline gs101 DT: phy@...4000,
 * reg-names "phy-pma", size 0x3000; komodo's ufs node lists 0x13204000/0x4000
 * as reg[4]). Register "offsets" in the gs101 cal tables are APB word indices:
 * the byte offset is (index << 2). TRSV (per-lane transceiver) registers repeat
 * every 0x200 indices per lane; COMN (common) registers are lane-independent.
 * See drivers/phy/samsung/phy-gs101-ufs.c + phy-samsung-ufs.c (mainline).
 */
#define KOMO_PMA_OFFSET         0x4000ull       /* hci_base + 0x4000 */
#define KOMO_PMA_SIZE           0x3000u
#define PMA_LANE_STRIDE         0x200u          /* index units between lanes */
/* UFS PHY power isolation, in the gs101 PMU syscon. komodo DT:
 * /ufs@13200000/ufs-phy-iso offset = 0x3EC0; PMU (samsung,gs101-pmu) @ 0x15460000.
 * bit0 set = PHY un-isolated (APB reachable). The mainline samsung UFS phy
 * driver sets this in phy_power_on BEFORE any PMA access; if ABL/handoff left
 * it isolated, every PMA register reads 0x0. */
#define KOMO_PMU_PA             0x15460000ull
#define KOMO_PHY_ISO_OFF        0x3ec0u
#define KOMO_PHY_ISO_EN         (1u << 0)
/* gs101 CFG_PRE_HIBERN8_EXIT register indices (phy-gs101-ufs.c) */
#define PMA_COMN_0              0x000           /* 0xC6 on exit (clear PHY power-down) */
#define PMA_COMN_1              0x001           /* 0x0C on exit */
#define PMA_COMN_8              0x008           /* 0xE0 on exit (HS only) */
#define PMA_TRSV_222            0x222           /* CDR override/enable; 0x18 on HS exit */
#define PMA_TRSV_246            0x246           /* 0x03 on HS exit */
#define PMA_TRSV_262            0x262           /* 0x00 on exit */
#define PMA_TRSV_265            0x265           /* 0x00 on exit */
#define PMA_TRSV_339            0x339           /* RX CDR-lock monitor; bit3 = done */
#define PMA_LN_OVRD_RX_CDR_EN   (1u << 4)       /* 0x10 */
#define PMA_LN_RX_CDR_EN        (1u << 3)       /* 0x08 */
#define PMA_LN_MON_CDR_DONE     (1u << 3)       /* 0x08 in TRSV 0x339 */

static inline uint32_t
pma_rd(uint32_t idx)
{
	return su.pma ? *(volatile uint32_t *)(su.pma + (idx << 2)) : 0xdeadu;
}

static inline void
pma_wr(uint32_t idx, uint32_t v)
{
	if (su.pma) {
		*(volatile uint32_t *)(su.pma + (idx << 2)) = v;
	}
}

/*
 * Port of the gs101 PHY hibern8-exit assist. In Linux the hibern8-exit UIC
 * command is bracketed by vops->hibern8_notify(PRE_CHANGE), which for Exynos
 * calls phy_notify_state(PHY_UFS_HIBERN8_EXIT): that applies the
 * CFG_PRE_HIBERN8_EXIT PMA writes (take the M-PHY out of its hibern8 stall)
 * and then polls/overrides the RX clock-data-recovery (CDR) lock per lane.
 * Without it a bare DME_HIBER_EXIT fatals (UPMCRS = PWR_FATAL_ERROR 5) because
 * the PHY was never told to leave low-power. We run it just before issuing the
 * UIC command, exactly as Linux does. Values verbatim from
 * drivers/phy/samsung/phy-gs101-ufs.c (tensor_gs101_pre_h8_exit +
 * gs101_phy_wait_for_cdr_lock). hs=true applies the HS-only rows (our link was
 * at PA_PWRMode 0x11 = HS at handoff). lanes: write TRSV to both lanes (the
 * second-lane regs are inside the 0x3000 window and harmless if 1-lane).
 */
static struct {
	bool ran;
	bool cdr_locked[2];
	uint32_t trsv339[2];
	uint32_t iso_before, iso_after;   /* PMU PHY-isolation reg */
	uint32_t comn0_rb;                /* readback of COMN[0] after writing 0xC6 */
	uint32_t pma0_raw;                /* raw PMA idx0 before any write */
	uint32_t b1_raw, b1_rb, b2_raw, b2_rb;  /* idx0 write-readback @0x..4000 / @0x..8000 */
} g_phy;
static void
ufs_phy_h8_exit_assist(bool hs, unsigned lanes)
{
	if (!su.pma) {
		return;
	}
	/* PMU PHY-isolation reg is TrustZone-protected: a direct EL1 access raises
	 * an SError ("unhandled implementation specific error") and panics. ABL
	 * already un-isolated the PHY (the link reached HS), so skip it by default;
	 * only touch it under mode bit 0x200 for experiments. */
	if ((su.mode & 0x200) && su.phy_iso) {
		volatile uint32_t *iso =
		    (volatile uint32_t *)(su.phy_iso + (KOMO_PHY_ISO_OFF & 0xfffu));
		g_phy.iso_before = *iso;
		*iso = (*iso) | KOMO_PHY_ISO_EN;
		__builtin_arm_dsb(0xf);
		delay(10);
		g_phy.iso_after = *iso;
	}
	g_phy.pma0_raw = pma_rd(PMA_COMN_0);
	/* One-shot base disambiguation: write 0xC6 to idx0 of BOTH candidate PMA
	 * windows and read back. Whichever returns 0xC6 is the live, clocked PMA.
	 * idx0 is byte 0 -> inside the DT-declared size of both windows (safe). */
	{
		/* Writability test on a real config register. idx0 is read-only/status
		 * (reads a stable value, ignores writes), so probe TRSV 0x234 (gs101
		 * pre_init writes 0x60 there -> RW). If 0x5A sticks, PMA writes work. */
		volatile uint32_t *b1id = (volatile uint32_t *)su.pma;
		volatile uint32_t *b1rw = (volatile uint32_t *)(su.pma + (0x234u << 2));
		g_phy.b1_raw = *b1id;
		*b1rw = 0x5Au; __builtin_arm_dsb(0xf); g_phy.b1_rb = *b1rw;
		if (su.pma2) {
			volatile uint32_t *b2 = (volatile uint32_t *)su.pma2;
			g_phy.b2_raw = *b2; *b2 = 0xC6u; __builtin_arm_dsb(0xf); g_phy.b2_rb = *b2;
		}
	}
	/* CFG_PRE_HIBERN8_EXIT: COMN first, then TRSV per lane. */
	pma_wr(PMA_COMN_0, 0xC6);
	__builtin_arm_dsb(0xf);
	g_phy.comn0_rb = pma_rd(PMA_COMN_0);
	pma_wr(PMA_COMN_1, 0x0C);
	for (unsigned ln = 0; ln < lanes; ln++) {
		uint32_t b = ln * PMA_LANE_STRIDE;
		pma_wr(PMA_TRSV_262 + b, 0x00);
		pma_wr(PMA_TRSV_265 + b, 0x00);
	}
	if (hs) {
		pma_wr(PMA_COMN_8, 0xE0);
		for (unsigned ln = 0; ln < lanes; ln++) {
			uint32_t b = ln * PMA_LANE_STRIDE;
			pma_wr(PMA_TRSV_246 + b, 0x03);
			pma_wr(PMA_TRSV_222 + b, 0x18);
		}
	}
	__builtin_arm_dsb(0xf);
	/* Wait for RX CDR lock, overriding CDR enable each retry (100 x ~80us). */
	for (unsigned ln = 0; ln < lanes; ln++) {
		uint32_t b = ln * PMA_LANE_STRIDE;
		bool locked = false;
		for (int i = 0; i < 100; i++) {
			delay(40);
			if (pma_rd(PMA_TRSV_339 + b) & PMA_LN_MON_CDR_DONE) {
				locked = true;
				break;
			}
			delay(40);
			pma_wr(PMA_TRSV_222 + b, PMA_LN_OVRD_RX_CDR_EN);
			pma_wr(PMA_TRSV_222 + b, PMA_LN_OVRD_RX_CDR_EN | PMA_LN_RX_CDR_EN);
		}
		if (ln < 2) {
			g_phy.cdr_locked[ln] = locked;
			g_phy.trsv339[ln] = pma_rd(PMA_TRSV_339 + b);
		}
		printf("TEGU UFS: PHY h8-exit assist lane %u: CDR %s (TRSV339 0x%x)\n",
		    ln, locked ? "locked" : "TIMEOUT", pma_rd(PMA_TRSV_339 + b));
	}
	g_phy.ran = true;
	__builtin_arm_dsb(0xf);
}

/* Busy-wait (boot path, polled) until (reg & mask) or timeout_us. */
static bool
ufs_wait_bits(uint32_t reg, uint32_t mask, unsigned timeout_us)
{
	for (unsigned t = 0; t < timeout_us; t += 10) {
		if (ufs_rd(reg) & mask) {
			return true;
		}
		delay(10);
	}
	return (ufs_rd(reg) & mask) != 0;
}

/*
 * Read-only UIC DME_GET of a local UniPro attribute. Returns true on success
 * and stores the value; does not change link power state. Used to probe whether
 * the link is active (PA_PWRMode) without the destructive HIBERNATE_EXIT.
 */
static bool
ufs_dme_cmd(uint32_t opcode, uint32_t attr, uint32_t *val, uint32_t *res_out)
{
	if (!(ufs_rd(REG_HCS) & HCS_UCRDY)) {
		if (res_out) {
			*res_out = 0xff01;
		}
		return false;
	}
	ufs_wr(REG_IS, IS_UCCS);
	ufs_wr(REG_UICARG1, attr << 16);
	ufs_wr(REG_UICARG2, 0);
	ufs_wr(REG_UICARG3, 0);
	ufs_wr(REG_UICCMD, opcode);
	if (!ufs_wait_bits(REG_IS, IS_UCCS, 200000)) {
		if (res_out) {
			*res_out = 0xff02;
		}
		return false;
	}
	uint32_t res = ufs_rd(REG_UICARG2) & 0xff;
	if (val) {
		*val = ufs_rd(REG_UICARG3);
	}
	if (res_out) {
		*res_out = res;
	}
	ufs_wr(REG_IS, IS_UCCS);
	return res == 0;
}

static bool
ufs_dme_get(uint32_t attr, uint32_t *val)
{
	return ufs_dme_cmd(UIC_DME_GET, attr, val, NULL);
}

static void
ufs_probe_link(const char *why)
{
	uint32_t pwr = 0, tx = 0, rx = 0;
	bool okp = ufs_dme_get(ATTR_PA_PWRMODE, &pwr);
	bool okt = ufs_dme_get(ATTR_PA_CONNTXLANES, &tx);
	bool okr = ufs_dme_get(ATTR_PA_CONNRXLANES, &rx);
	printf("TEGU UFS: link %s: DME_GET %s PWRMODE 0x%x (TX %u RX %u lanes tx%s rx%s) UICCMD 0x%x\n",
	    why, okp ? "ok" : "FAIL", pwr, pwr & 0xf, (pwr >> 4) & 0xf,
	    okt ? "" : "?", okr ? "" : "?", ufs_rd(REG_UICCMD));
	(void)tx; (void)rx;
}

/*
 * komodo: ABL hands the Exynos UFS host over clock-gated (HCI_MISC had the
 * CLK_CTRL_EN bits set, 0xde0) and, as Linux does before gating, most likely
 * with the link in Hibern8. Commands then stay in the doorbell with OCS 0xf.
 * Wake it like Linux's exynos_ufs_pre_hibern8(): ungate, then DME_HIBERNATE_EXIT.
 * Link/clock operations only; nothing is read from or written to the medium.
 */
static void
ufs_exynos_wake(void)
{
	uint32_t stop = vs_rd(VS_CLKSTOP_CTRL), misc = vs_rd(VS_MISC);
	printf("TEGU UFS: wake: CLKSTOP 0x%x MISC 0x%x IS 0x%x HCS 0x%x\n",
	    stop, misc, ufs_rd(REG_IS), ufs_rd(REG_HCS));
	vs_wr(VS_CLKSTOP_CTRL, stop & ~VS_CLK_STOP_MASK);
	vs_wr(VS_MISC, misc & ~VS_CLK_CTRL_EN_MASK);
	__builtin_arm_dsb(0xf);
	delay(100);
	if (!ufs_wait_bits(REG_HCS, HCS_UCRDY, 100000)) {
		printf("TEGU UFS: wake: UIC not ready (HCS 0x%x)\n", ufs_rd(REG_HCS));
		return;
	}
	ufs_wr(REG_IS, IS_UCCS | IS_UHXS);
	ufs_wr(REG_UICARG1, 0);
	ufs_wr(REG_UICARG2, 0);
	ufs_wr(REG_UICARG3, 0);
	ufs_wr(REG_UICCMD, UIC_DME_HIBER_EXIT);
	bool done = ufs_wait_bits(REG_IS, IS_UCCS, 500000);
	bool hx = ufs_wait_bits(REG_IS, IS_UHXS, 500000);
	printf("TEGU UFS: wake: HIBER_EXIT %s, exit status %s, ARG2 0x%x IS 0x%x HCS 0x%x (UPMCRS %u)\n",
	    done ? "completed" : "NO completion", hx ? "seen" : "not seen",
	    ufs_rd(REG_UICARG2), ufs_rd(REG_IS), ufs_rd(REG_HCS), (ufs_rd(REG_HCS) >> 8) & 7);
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	printf("TEGU UFS: wake: now CLKSTOP 0x%x MISC 0x%x\n", vs_rd(VS_CLKSTOP_CTRL), vs_rd(VS_MISC));
}

static void
ufs_dump_regs(const char *why)
{
	printf("TEGU UFS: %s: IS 0x%x HCS 0x%x HCE 0x%x DBR 0x%x RSR 0x%x BA 0x%x:%x\n", why,
	    ufs_rd(REG_IS), ufs_rd(REG_HCS), ufs_rd(REG_HCE), ufs_rd(REG_UTRLDBR),
	    ufs_rd(REG_UTRLRSR), ufs_rd(REG_UTRLBAU), ufs_rd(REG_UTRLBA));
	printf("TEGU UFS:   UIC err PA 0x%x DL 0x%x N 0x%x T 0x%x DME 0x%x\n",
	    ufs_rd(0x38), ufs_rd(0x3c), ufs_rd(0x40), ufs_rd(0x44), ufs_rd(0x48));
	printf("TEGU UFS:   vs TXPRDT 0x%x RXPRDT 0x%x NEXUS 0x%x REORDER 0x%x CLKSTOP 0x%x MISC 0x%x ACG 0x%x\n",
	    vs_rd(VS_TXPRDT_ENTRY_SIZE), vs_rd(VS_RXPRDT_ENTRY_SIZE), vs_rd(VS_UTRL_NEXUS_TYPE),
	    vs_rd(VS_DATA_REORDER), vs_rd(VS_CLKSTOP_CTRL), vs_rd(VS_MISC), vs_rd(VS_IOP_ACG_DISABLE));
	/* komodo: full standard HCI register map, to see UFSHCI version + MCQ mode. */
	printf("TEGU UFS:   CAP 0x%x CAP2 0x%x VER 0x%x HCPID 0x%x HCMID 0x%x AHIT 0x%x\n",
	    ufs_rd(0x00), ufs_rd(0x04), ufs_rd(0x08), ufs_rd(0x0c), ufs_rd(0x10), ufs_rd(0x18));
	printf("TEGU UFS:   IE 0x%x HCSEXT 0x%x MCQCAP 0x%x CONFIG 0x%x IACTHRTR 0x%x\n",
	    ufs_rd(0x24), ufs_rd(0x2c), ufs_rd(0x14), ufs_rd(0x30), ufs_rd(0x34));
	printf("TEGU UFS:   raw: 00:%x 04:%x 08:%x 14:%x 30:%x 34:%x 38:%x 5c:%x 60:%x 90:%x\n",
	    ufs_rd(0x00), ufs_rd(0x04), ufs_rd(0x08), ufs_rd(0x14), ufs_rd(0x30),
	    ufs_rd(0x34), ufs_rd(0x38), ufs_rd(0x5c), ufs_rd(0x60), ufs_rd(0x90));
}

static void
dma_sync(uint32_t off, uint32_t len)
{
	if (su.dma_wc) {           /* Normal-NC + PA outside physmap: phystokv would panic */
		__builtin_arm_dsb(0xf);
		return;
	}
	flush_dcache64((addr64_t)(su.dma_pa + off), len, TRUE);
}

static void
dma_clean(uint32_t off, uint32_t len)
{
	if (su.dma_wc) {
		__builtin_arm_dsb(0xf);
		return;
	}
	clean_dcache64((addr64_t)(su.dma_pa + off), len, TRUE);
}

/*
 * One SCSI command in slot 0, data (if any) to/from the bounce buffer
 * (to the device when write is set). Returns 0 or an errno.
 */
static int
ufs_scsi(const uint8_t *cdb, unsigned cdb_len, uint32_t len, bool write)
{
	struct utrd *utrd = (struct utrd *)(su.dma + DMA_UTRL);
	struct ucd *ucd = (struct ucd *)(su.dma + DMA_UCD);
	uint64_t ucd_pa = su.dma_pa + DMA_UCD, bounce_pa = su.dma_pa + DMA_BOUNCE;

	assert(len <= BOUNCE_SIZE);
	bzero(utrd, sizeof(*utrd));
	bzero(ucd, sizeof(*ucd));

	ucd->cmd[0] = 0x01;                     /* COMMAND UPIU */
	ucd->cmd[1] = len ? (write ? 0x20 : 0x40) : 0x00;      /* W: data out, R: data in */
	ucd->cmd[2] = su.lun;
	ucd->cmd[3] = ++su.tag;
	ucd->cmd[12] = (uint8_t)(len >> 24);
	ucd->cmd[13] = (uint8_t)(len >> 16);
	ucd->cmd[14] = (uint8_t)(len >> 8);
	ucd->cmd[15] = (uint8_t)len;
	memcpy(&ucd->cmd[16], cdb, cdb_len);

	/* DD: 1 = memory to device, 2 = device to memory. */
	utrd->dw0 = (1u << 28) | ((len ? (write ? 1u : 2u) : 0u) << 25) | (1u << 24);
	utrd->dw2 = 0x0f;                       /* OCS invalid until updated */
	utrd->ucdba = (uint32_t)ucd_pa;
	utrd->ucdbau = (uint32_t)(ucd_pa >> 32);
	const bool bytegran = (su.mode & 1) != 0;
	const uint32_t chunk_max = (su.mode & 2) ? PRDT_CHUNK_EXYNOS : PRDT_CHUNK;
	const uint32_t stride = su.prdt_stride;
	/* response UPIU at 512, 512 bytes long; PRDT at 1024 */
	utrd->resp = bytegran ? (512u << 16 | 512u) : ((512u / 4) << 16 | (512u / 4));
	if (len) {
		uint32_t n = 0;
		for (uint32_t done = 0; done < len; done += chunk_max, n++) {
			uint32_t chunk = MIN(chunk_max, len - done);
			uint32_t *e = (uint32_t *)(void *)(ucd->prdt + n * stride);
			e[0] = (uint32_t)(bounce_pa + done);
			e[1] = (uint32_t)((bounce_pa + done) >> 32);
			e[2] = 0;
			e[3] = chunk - 1;
		}
		utrd->prdt = bytegran ? (1024u << 16 | (n * stride)) : ((1024u / 4) << 16 | n);
	}

	if (su.cache) {
		dma_clean(DMA_UTRL, sizeof(*utrd));
		dma_clean(DMA_UCD, 16 + cdb_len);
		dma_sync(DMA_UCD + offsetof(struct ucd, resp), sizeof(ucd->resp));
		if (len) {
			dma_clean(DMA_UCD + offsetof(struct ucd, prdt),
			    bytegran ? (utrd->prdt & 0xffff) : (utrd->prdt & 0xffff) * stride);
		}
	} else {
		dma_sync(DMA_UTRL, sizeof(*utrd));
		dma_sync(DMA_UCD, sizeof(*ucd));
	}
	if (len) {
		if (su.cache && write) {
			dma_clean(DMA_BOUNCE, len);
		} else {
			dma_sync(DMA_BOUNCE, len);
		}
	}
	__builtin_arm_dsb(0xf);

	if (su.mode & 4) {
		vs_wr(VS_UTRL_NEXUS_TYPE, vs_rd(VS_UTRL_NEXUS_TYPE) | 1u);   /* slot 0 = SCSI */
	}
	/*
	 * komodo: the bootloader hands the host over with UTRLCLR (0x5c) = 0, i.e.
	 * every slot flagged "cleared/aborted" (the operational default is all 1s).
	 * The controller then ignores the doorbell for a cleared slot: DBR stays 1,
	 * IS stays 0, OCS stays 0xf. Un-clear slot 0 (bit = 1) right before ringing.
	 */
	ufs_wr(REG_UTRLCLR, 0xffffffffu);
	__builtin_arm_dsb(0xf);
	ufs_wr(REG_UTRLDBR, 1);
	uint64_t deadline;
	nanoseconds_to_absolutetime(CMD_TIMEOUT_NS, &deadline);
	deadline += mach_absolute_time();
	bool sleepable = su.wait && su.ready && ml_get_interrupts_enabled() &&
	    !ml_at_interrupt_context() && get_preemption_level() == 0;
	uint64_t spin_until = 0, sleep_interval = 0;
	if (sleepable) {
		nanoseconds_to_absolutetime(CMD_SPIN_NS, &spin_until);
		spin_until += mach_absolute_time();
		nanoseconds_to_absolutetime(CMD_SLEEP_NS, &sleep_interval);
	}
	while (ufs_rd(REG_UTRLDBR) & 1) {
		uint64_t now = mach_absolute_time();
		if (now > deadline) {
			dma_sync(DMA_UTRL, sizeof(*utrd));
			printf("TEGU UFS: command 0x%02x timed out (mode 0x%x, ocs 0x%x, dma 0x%llx)\n",
			    cdb[0], su.mode, utrd->dw2 & 0xff, su.dma_pa);
			ufs_dump_regs("timeout");
			ufs_wr(REG_UTRLCLR, ~1u);
			return ETIMEDOUT;
		}
		if (sleepable && now >= spin_until) {
			uint64_t wake = now + MIN(sleep_interval, deadline - now);
			/* Keep the mutex: slot 0 and the bounce buffer are still owned. */
			if (assert_wait_deadline_with_leeway((event_t)&su.wait, THREAD_UNINT,
			    TIMEOUT_URGENCY_SYS_CRITICAL, wake, 0) == THREAD_WAITING) {
				thread_block(THREAD_CONTINUE_NULL);
			}
		} else {
			delay(1);
		}
	}
	__builtin_arm_dsb(0xf);
	ufs_wr(REG_IS, ufs_rd(REG_IS));         /* acknowledge completion status */

	dma_sync(DMA_UTRL, sizeof(*utrd));
	if (su.cache) {
		dma_sync(DMA_UCD + offsetof(struct ucd, resp), sizeof(ucd->resp));
	} else {
		dma_sync(DMA_UCD, sizeof(*ucd));
	}
	if (len && !(su.cache && write)) {
		dma_sync(DMA_BOUNCE, len);
	}

	uint32_t ocs = utrd->dw2 & 0xff;
	if (ocs || ucd->resp[0] != 0x21 || ucd->resp[6] || ucd->resp[7]) {
		printf("TEGU UFS: command 0x%02x failed: ocs 0x%x upiu 0x%x response 0x%x status 0x%x\n",
		    cdb[0], ocs, ucd->resp[0], ucd->resp[6], ucd->resp[7]);
		return EIO;
	}
	return 0;
}

/* READ(10) of count device blocks at lba into the bounce buffer. */
static int
ufs_read_blocks(uint64_t lba, uint32_t count)
{
	if (lba > UINT32_MAX || count > BOUNCE_SIZE / UFS_BLOCK) {
		return EINVAL;
	}
	uint8_t cdb[10] = {
		0x28, 0,
		(uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
		0, (uint8_t)(count >> 8), (uint8_t)count, 0
	};
	return ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, false);
}

/*
 * WRITE(10) of count device blocks from the bounce buffer to lba. The range
 * must lie inside the partition: this is the only place data goes out.
 */
static int
ufs_write_blocks(uint64_t lba, uint32_t count)
{
	if (!su.writable || !su.nblocks || count == 0 || count > BOUNCE_SIZE / UFS_BLOCK ||
	    lba < su.first_block || lba - su.first_block >= su.nblocks ||
	    count > su.nblocks - (lba - su.first_block) || lba + count - 1 > UINT32_MAX) {
		printf("TEGU UFS: refusing write of %u blocks at 0x%llx\n", count, lba);
		return EPERM;
	}
	uint8_t cdb[10] = {
		0x2a, 0,
		(uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
		0, (uint8_t)(count >> 8), (uint8_t)count, 0
	};
	return ufs_scsi(cdb, sizeof(cdb), count * UFS_BLOCK, true);
}

static int
ufs_sync_cache(void)
{
	static const uint8_t cdb[10] = { 0x35 };       /* SYNCHRONIZE CACHE(10), whole LUN */
	return ufs_scsi(cdb, sizeof(cdb), 0, false);
}

static int
ufs_find_partition(const char *want)
{
	const uint8_t *b = (const uint8_t *)(su.dma + DMA_BOUNCE);
	int error = ufs_read_blocks(1, 1);
	if (error) {
		return error;
	}
	if (memcmp(b, "EFI PART", 8) != 0) {
		printf("TEGU UFS: LUN %u has no GPT\n", su.lun);
		return ENXIO;
	}
	uint64_t entry_lba;
	uint32_t nent, esz;
	memcpy(&entry_lba, b + 72, 8);
	memcpy(&nent, b + 80, 4);
	memcpy(&esz, b + 84, 4);
	if (esz != 128 || nent == 0 || nent * esz > BOUNCE_SIZE) {
		printf("TEGU UFS: unsupported GPT entries %u x %u\n", nent, esz);
		return ENXIO;
	}
	error = ufs_read_blocks(entry_lba, (nent * esz + UFS_BLOCK - 1) / UFS_BLOCK);
	if (error) {
		return error;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = b + i * esz;
		char name[37];
		unsigned c;
		for (c = 0; c < 36; c++) {
			uint16_t ch = (uint16_t)(e[56 + 2 * c] | e[57 + 2 * c] << 8);
			if (ch == 0 || ch > 0x7f) {
				break;
			}
			name[c] = (char)ch;
		}
		name[c] = 0;
		if (strcmp(name, want) != 0) {
			continue;
		}
		uint64_t first, last;
		memcpy(&first, e + 32, 8);
		memcpy(&last, e + 40, 8);
		if (last < first) {
			return ENXIO;
		}
		su.first_block = first;
		su.nblocks = last - first + 1;
		return 0;
	}
	printf("TEGU UFS: no GPT partition named \"%s\"\n", want);
	return ENOENT;
}

/*
 * komodo: lift the Tensor fences before ringing a doorbell.
 *   ufsmode bit6 (0x40) - set IO-coherency for the UFS AXI master in sysreg_ufs
 *                         so the controller's reads of our DRAM are allowed.
 *   ufsmode bit5 (0x20) - open the FMP/SMU protector (Linux exynos_ufs_config_smu):
 *                         NSSMU + whole-LBA bypass region, so transfers complete.
 * Both are read-modify-write pokes to Device MMIO; harmless if the bit is clear.
 */
static void
ufs_komodo_unfence(void)
{
	if (su.mode & 0x40) {
		vm_offset_t sr = ml_io_map(KOMO_SYSREG_UFS_PA, 0x1000);
		if (sr) {
			volatile uint32_t *r = (volatile uint32_t *)(sr + SYSREG_UFS_IOCC);
			uint32_t v = *r;
			*r = (v & ~3u) | 3u;
			__builtin_arm_dsb(0xf);
			printf("TEGU UFS: iocc sysreg[0x710] 0x%x -> 0x%x\n", v, *r);
		} else {
			printf("TEGU UFS: iocc sysreg map failed\n");
		}
	}
	if (su.mode & 0x20) {
		vm_offset_t p = ml_io_map(KOMO_UFSP_PA, 0x1000);
		if (p) {
			volatile uint32_t *sec = (volatile uint32_t *)(p + UFSPRSECURITY);
			uint32_t old = *sec;
			*sec = old | UFSP_NSSMU;
			*(volatile uint32_t *)(p + UFSPSBEGIN0) = 0;
			*(volatile uint32_t *)(p + UFSPSEND0)   = 0xffffffffu;
			*(volatile uint32_t *)(p + UFSPSLUN0)   = 0xffu;
			*(volatile uint32_t *)(p + UFSPSCTRL0)  = 0xf1u;
			__builtin_arm_dsb(0xf);
			printf("TEGU UFS: FMP/SMU open: SECURITY 0x%x -> 0x%x\n", old, *sec);
		} else {
			printf("TEGU UFS: FMP/SMU map failed\n");
		}
	}
}

/*
 * komodo: the bootloader leaves auto-hibernate on (AHIT 0x14 = 0x1ce), so the
 * UniPro link drops into Hibernate (H8) ~0.5 ms after it goes idle (UICCMD
 * readback 0x17 = DME_HIBERNATE_ENTER). Our transfer requests then land on a
 * sleeping link and time out with OCS 0xf. Turn auto-hibernate off and, if the
 * link is already in H8, bring it back with DME_HIBERNATE_EXIT.
 */
#define UIC_DME_LINKSTARTUP 0x16
/*
 * komodo mode bit7: the "take over ABL's running host" approach never gets the
 * transfer-request engine to process slot 0 (OCS stays 0xf, no IS). Do a proper
 * host-controller reset (HCE 0->1) and re-run link startup. The M-PHY/PMA block
 * (0x13208000) is not reset by HCE, so ABL's PHY calibration persists and
 * DME_LINKSTARTUP should re-establish the link without a full PHY re-cal.
 */
static void
ufs_full_reset_relink(void)
{
	printf("TEGU UFS: reset: HCE 0x%x HCS 0x%x -> resetting\n",
	    ufs_rd(REG_HCE), ufs_rd(REG_HCS));
	/* Ungate vendor clocks first (as before HCE in Linux exynos). */
	if (su.vs) {
		vs_wr(VS_CLKSTOP_CTRL, vs_rd(VS_CLKSTOP_CTRL) & ~VS_CLK_STOP_MASK);
		vs_wr(VS_MISC, vs_rd(VS_MISC) & ~VS_CLK_CTRL_EN_MASK);
	}
	ufs_wr(REG_HCE, 0);
	__builtin_arm_dsb(0xf);
	for (unsigned t = 0; t < 100000 && (ufs_rd(REG_HCE) & 1); t += 10) {
		delay(10);
	}
	/*
	 * Device hardware reset via HCI_GPIO_OUT bit0 (= UFS device RST_n), the step
	 * Linux's exynos_ufs_dev_hw_reset does at HCE enable. Pulls the device out of
	 * its hibernated/locked state so it re-links as if freshly powered.
	 */
	if (su.vs) {
		vs_wr(HCI_GPIO_OUT, 0);        /* assert RST_n */
		__builtin_arm_dsb(0xf);
		delay(20);
		vs_wr(HCI_GPIO_OUT, 1);        /* deassert */
		__builtin_arm_dsb(0xf);
		delay(5000);
		printf("TEGU UFS: reset: device HW reset via GPIO_OUT (now 0x%x)\n", vs_rd(HCI_GPIO_OUT));
	}
	ufs_wr(REG_HCE, 1);
	__builtin_arm_dsb(0xf);
	bool ucrdy = ufs_wait_bits(REG_HCS, HCS_UCRDY, 200000);
	/* re-ungate clocks after HCE */
	if (su.vs) {
		vs_wr(VS_CLKSTOP_CTRL, vs_rd(VS_CLKSTOP_CTRL) & ~VS_CLK_STOP_MASK);
		vs_wr(VS_MISC, vs_rd(VS_MISC) & ~VS_CLK_CTRL_EN_MASK);
		vs_wr(VS_IOP_ACG_DISABLE, 1u);
	}
	printf("TEGU UFS: reset: HCE back 0x%x, UCRDY %s (HCS 0x%x)\n",
	    ufs_rd(REG_HCE), ucrdy ? "yes" : "NO", ufs_rd(REG_HCS));
	/* DME_LINKSTARTUP (retry a few times; the first after reset often needs it) */
	bool done = false, dp = false;
	for (int i = 0; i < 3 && !dp; i++) {
		ufs_wr(REG_IS, ufs_rd(REG_IS));
		ufs_wr(REG_UICARG1, 0);
		ufs_wr(REG_UICARG2, 0);
		ufs_wr(REG_UICARG3, 0);
		ufs_wr(REG_UICCMD, UIC_DME_LINKSTARTUP);
		done = ufs_wait_bits(REG_IS, IS_UCCS, 500000);
		dp = ufs_wait_bits(REG_HCS, HCS_DP, 500000);
		printf("TEGU UFS: reset: LINKSTARTUP %d %s, device %s (HCS 0x%x UPMCRS %u ARG2 0x%x UECPA 0x%x)\n",
		    i, done ? "done" : "NO-UCCS", dp ? "present" : "ABSENT",
		    ufs_rd(REG_HCS), (ufs_rd(REG_HCS) >> 8) & 0xf, ufs_rd(REG_UICARG2), ufs_rd(0x38));
		ufs_wr(REG_IS, ufs_rd(REG_IS));
		if (!dp) {
			delay(10000);
		}
	}
}

/*
 * komodo: force the Exynos vendor clocks on and disable auto clock gating.
 * ABL hands the host over with IOP auto-clock-gating active (VS_IOP_ACG_DISABLE
 * reads 0) and the UTP/DMA clocks may gate while idle, so a rung doorbell is
 * never serviced: DBR stays 1, IS stays 0, the UTRD OCS stays 0xf (untouched),
 * with no error reported. Linux's exynos_ufs_ungate_clks + ACG disable keep the
 * engine clocked. Vendor-register writes only; nothing touches the medium.
 */
static void
ufs_exynos_clocks_on(void)
{
	if (!su.vs) {
		return;
	}
	uint32_t stop = vs_rd(VS_CLKSTOP_CTRL), misc = vs_rd(VS_MISC), acg = vs_rd(VS_IOP_ACG_DISABLE);
	vs_wr(VS_CLKSTOP_CTRL, stop & ~VS_CLK_STOP_MASK);   /* no clock stop */
	vs_wr(VS_MISC, misc & ~VS_CLK_CTRL_EN_MASK);        /* ungate (disable auto ctrl) */
	vs_wr(VS_IOP_ACG_DISABLE, 1u);                      /* disable IOP auto clock gating */
	__builtin_arm_dsb(0xf);
	printf("TEGU UFS: clocks on: CLKSTOP 0x%x->0x%x MISC 0x%x->0x%x ACG 0x%x->0x%x\n",
	    stop, vs_rd(VS_CLKSTOP_CTRL), misc, vs_rd(VS_MISC), acg, vs_rd(VS_IOP_ACG_DISABLE));
}

/*
 * komodo: the bootloader hibernates the UniPro link (DME_HIBERNATE_ENTER) before
 * handoff, so the DEVICE is unreachable (local DME_GET ok, DME_PEER_GET fails).
 * Exit H8 with the full power-mode handshake (wait UCCS then UPMS, check UPMCRS
 * == PWR_LOCAL), ungating vendor clocks first, retrying a few times. Returns
 * true once the link reports a non-fatal power mode. Read-only w.r.t. the medium.
 */
static struct {
	uint32_t carg2, is, upmcrs, uecpa_before, uecpa_after, tries;
	bool uccs, upms;
} g_h8;

static bool
ufs_h8_exit_robust(void)
{
	for (int attempt = 0; attempt < 4; attempt++) {
		if (su.vs) {       /* ungate UniPro/refclk (Exynos pre-hibern8-exit) */
			vs_wr(VS_CLKSTOP_CTRL, vs_rd(VS_CLKSTOP_CTRL) & ~VS_CLK_STOP_MASK);
			vs_wr(VS_MISC, vs_rd(VS_MISC) & ~VS_CLK_CTRL_EN_MASK);
			__builtin_arm_dsb(0xf);
		}
		/* gs101 PHY hibern8-exit assist: take the M-PHY out of its stall
		 * and relock RX CDR before the UIC command (what Linux's
		 * hibern8_notify(PRE_CHANGE)->phy_notify_state does). Gated by
		 * ufsmode bit8 so it can be A/B'd against the bare exit. The link
		 * was HS (PA_PWRMode 0x11) and 2-lane at handoff. */
		if (su.mode & 0x100) {
			ufs_phy_h8_exit_assist(true, 2);
		}
		/* Read-to-clear the UIC error counters + clear IS.UE, or a stale PHY
		 * adapter error (UECPA 0x80000010) can make the next UIC power-mode
		 * command fail fatally. */
		uint32_t pa0 = ufs_rd(0x38), dl0 = ufs_rd(0x3c), n0 = ufs_rd(0x40),
		    t0 = ufs_rd(0x44), dme0 = ufs_rd(0x48);
		ufs_wr(REG_IS, ufs_rd(REG_IS));     /* clear stale status incl IS.UE */
		(void)pa0; (void)dl0; (void)n0; (void)t0; (void)dme0;
		if (!ufs_wait_bits(REG_HCS, HCS_UCRDY, 100000)) {
			printf("TEGU UFS: H8 exit %d: UIC not ready\n", attempt);
			continue;
		}
		ufs_wr(REG_UICARG1, 0);
		ufs_wr(REG_UICARG2, 0);
		ufs_wr(REG_UICARG3, 0);
		ufs_wr(REG_UICCMD, UIC_DME_HIBER_EXIT);
		bool uccs = ufs_wait_bits(REG_IS, IS_UCCS, 200000);
		uint32_t carg2 = ufs_rd(REG_UICARG2) & 0xff;
		bool upms = ufs_wait_bits(REG_IS, IS_UPMS | IS_UHXS, 700000);
		uint32_t is = ufs_rd(REG_IS);
		uint32_t upmcrs = (ufs_rd(REG_HCS) >> 8) & 0xf;
		g_h8.carg2 = carg2; g_h8.is = is; g_h8.upmcrs = upmcrs;
		g_h8.uecpa_before = pa0; g_h8.uecpa_after = ufs_rd(0x38);
		g_h8.uccs = uccs; g_h8.upms = upms; g_h8.tries = attempt + 1;
		printf("TEGU UFS: H8 exit %d: UCCS %d carg2 0x%x UPMS/UHXS %d IS 0x%x UPMCRS %u UECPA 0x%x->0x%x\n",
		    attempt, uccs, carg2, upms, is, upmcrs, pa0, g_h8.uecpa_after);
		ufs_wr(REG_IS, ufs_rd(REG_IS));
		if (upmcrs == 1 || upmcrs == 0) {   /* PWR_LOCAL / PWR_OK = success */
			return true;
		}
		delay(20000);
	}
	return false;
}

static int
ufs_take_over_host(void)
{
	uint32_t hce = ufs_rd(REG_HCE), hcs = ufs_rd(REG_HCS);
	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		printf("TEGU UFS: host not ready (hce 0x%x hcs 0x%x)\n", hce, hcs);
		return ENXIO;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		printf("TEGU UFS: requests outstanding from the bootloader\n");
		return EBUSY;
	}
	/* With RAM above 4 GiB (mkadt.py --highmem) the DMA memory can be there too. */
	if (((su.dma_pa + DMA_SIZE - 1) >> 32) && !(ufs_rd(REG_CAP) & CAP_64AS)) {
		printf("TEGU UFS: DMA memory at 0x%llx but the host has no 64-bit addressing\n",
		    su.dma_pa);
		return ENXIO;
	}
	ufs_wr(REG_IE, 0);                      /* polled operation */
	ufs_wr(REG_IS, ufs_rd(REG_IS));
	ufs_wr(REG_UTRLRSR, 0);
	uint64_t list = su.dma_pa + DMA_UTRL;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	ufs_wr(0x80, 1);                    /* UTMRLRSR (0x80): run the task-mgmt list too */
	ufs_wr(REG_UTRLCLR, 0xffffffffu);   /* ABL left slots flagged cleared (0x5c=0) */
	/* Never ring a doorbell unless the host uses our list. */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		printf("TEGU UFS: host did not take our request list\n");
		return EIO;
	}
	return 0;
}

static int
tegu_ufs_open(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	if (!su.ready) {
		return ENXIO;
	}
	return ((flags & FWRITE) && !su.writable) ? EROFS : 0;
}

static int
tegu_ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static void
tegu_ufs_strategy(buf_t bp)
{
	int error = 0;
	caddr_t vaddr = NULL;
	uint64_t off = (uint64_t)buf_blkno(bp) * su.secsize;
	uint32_t count = buf_count(bp);
	uint64_t part_bytes = su.nblocks * UFS_BLOCK;

	bool rd = buf_flags(bp) & B_READ;

	buf_setresid(bp, count);
	if (!rd && !su.writable) {
		error = EROFS;
		goto done;
	}
	if (off > part_bytes || count > part_bytes - off) {
		error = EINVAL;
		goto done;
	}
	if (count == 0) {
		goto done;
	}
	if (buf_map(bp, &vaddr)) {
		error = EIO;
		goto done;
	}

	lck_mtx_lock(&tegu_ufs_lock);
	for (uint32_t done = 0; done < count;) {
		uint64_t pos = off + done;
		uint64_t lba = pos / UFS_BLOCK;
		uint32_t skip = (uint32_t)(pos % UFS_BLOCK);
		uint32_t want = MIN(count - done, BOUNCE_SIZE - skip);
		uint32_t blocks = (skip + want + UFS_BLOCK - 1) / UFS_BLOCK;
		void *bounce = (void *)(su.dma + DMA_BOUNCE);
		if (rd) {
			error = ufs_read_blocks(su.first_block + lba, blocks);
			if (!error) {
				memcpy(vaddr + done, (const char *)bounce + skip, want);
			}
		} else {
			/* Partial device blocks at either end: read, merge, write back. */
			if (skip || (want % UFS_BLOCK)) {
				error = ufs_read_blocks(su.first_block + lba, blocks);
			}
			if (!error) {
				memcpy((char *)bounce + skip, vaddr + done, want);
				error = ufs_write_blocks(su.first_block + lba, blocks);
			}
		}
		if (error) {
			break;
		}
		done += want;
		buf_setresid(bp, count - done);
	}
	lck_mtx_unlock(&tegu_ufs_lock);
	buf_unmap(bp);

done:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
tegu_ufs_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag,
    __unused struct proc *p, int is_char)
{
	uint32_t *f = (uint32_t *)data;
	uint64_t *o = (uint64_t *)data;

	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*f = su.secsize;
		return 0;
	case DKIOCSETBLOCKSIZE:
		if (is_char) {
			return ENODEV;
		}
		if (*f < DEV_BSIZE || *f > UFS_BLOCK || (*f & (*f - 1))) {
			return EINVAL;
		}
		su.secsize = *f;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*f = UFS_BLOCK;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*o = su.nblocks * UFS_BLOCK / su.secsize;
		return 0;
	case DKIOCISWRITABLE:
		*f = su.writable;
		return 0;
	case DKIOCSYNCHRONIZE:
	case DKIOCSYNCHRONIZECACHE: {
		if (!su.writable) {
			return 0;
		}
		lck_mtx_lock(&tegu_ufs_lock);
		int error = ufs_sync_cache();
		lck_mtx_unlock(&tegu_ufs_lock);
		return error;
	}
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*o = BOUNCE_SIZE / su.secsize;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*o = su.writable ? BOUNCE_SIZE / su.secsize : 0;
		return 0;
	case DKIOCGETMAXSEGMENTCOUNTREAD:
	case DKIOCGETMAXSEGMENTCOUNTWRITE:
		*o = 1;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
tegu_ufs_bioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return tegu_ufs_ioctl(dev, cmd, data, flag, p, 0);
}

static int
tegu_ufs_cioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return tegu_ufs_ioctl(dev, cmd, data, flag, p, 1);
}

static int
tegu_ufs_psize(__unused dev_t dev)
{
	return su.ready ? (int)MIN(su.nblocks * UFS_BLOCK / su.secsize, INT_MAX) : -1;
}

static int
tegu_ufs_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(tegu_ufs_strategy, NULL, dev, B_READ, minphys, uio, (int)su.secsize);
}

static int
tegu_ufs_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	if (!su.writable) {
		return EROFS;
	}
	return physio(tegu_ufs_strategy, NULL, dev, B_WRITE, minphys, uio, (int)su.secsize);
}

static const struct bdevsw tegu_ufs_bdevsw = {
	.d_open     = tegu_ufs_open,
	.d_close    = tegu_ufs_close,
	.d_strategy = tegu_ufs_strategy,
	.d_ioctl    = tegu_ufs_bioctl,
	.d_dump     = eno_dump,
	.d_psize    = tegu_ufs_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw tegu_ufs_cdevsw = {
	.d_open       = tegu_ufs_open,
	.d_close      = tegu_ufs_close,
	.d_read       = tegu_ufs_read,
	.d_write      = tegu_ufs_write,
	.d_ioctl      = tegu_ufs_cioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

static int
tegu_ufs_init(void)
{
	DTEntry node;
	uintptr_t const *reg = NULL;
	unsigned int size = 0;
	char want[64] = "userdata";
	int error;

	if (SecureDTLookupEntry(NULL, "/tegu-ufs", &node) != kSuccess ||
	    SecureDTGetProperty(node, "reg", (void const **)&reg, &size) != kSuccess ||
	    size < 2 * sizeof(uintptr_t)) {
		printf("TEGU UFS: no /tegu-ufs node in the device tree\n");
		return ENXIO;
	}
	PE_parse_boot_argn("ufsroot", want, sizeof(want));
	uint32_t rw = 0;
	PE_parse_boot_argn("ufsrw", &rw, sizeof(rw));
	/* komodo: default ON. The Exynos host is not cache-coherent (see the
	 * clean/invalidate around every command); the UTRD/UCD the CPU writes
	 * must be pushed to DRAM before the doorbell or the host fetches stale
	 * cache-line contents and the command sits with OCS 0xf, IS 0. Set
	 * ufscache=0 to go back to the old (no-maintenance) behaviour. */
	uint32_t cache = 1, wait = 0;
	PE_parse_boot_argn("ufscache", &cache, sizeof(cache));
	PE_parse_boot_argn("ufswait", &wait, sizeof(wait));
	su.cache = (cache != 0);
	su.wait = (wait == 1);
	uint32_t exynos = 1, prdt = 16;
	PE_parse_boot_argn("ufsexynos", &exynos, sizeof(exynos));
	PE_parse_boot_argn("ufsprdt", &prdt, sizeof(prdt));
	su.exynos = (exynos != 0);
	su.prdt_stride = (prdt == 128) ? 128u : 16u;
	uint32_t mode = su.exynos ? 0x116u : 0x0u; /* +bit8 (0x100): gs101 PHY h8-exit assist */
	PE_parse_boot_argn("ufsmode", &mode, sizeof(mode));
	su.mode = mode;
	printf("TEGU UFS: mode 0x%x (%s offsets, %s PRDT entries%s), entry %u bytes\n", su.mode,
	    (su.mode & 1) ? "byte" : "dword", (su.mode & 2) ? "4 KiB" : "256 KiB",
	    (su.mode & 4) ? ", NEXUS" : "", su.prdt_stride);

	su.regs = ml_io_map(reg[0], reg[1]);
	if (su.exynos) {
		su.vs = ml_io_map(reg[0] + 0x1100, 0x2000);   /* DT: vs_hci @ hci + 0x1100 */
		su.pma = ml_io_map(reg[0] + KOMO_PMA_OFFSET, KOMO_PMA_SIZE); /* M-PHY PMA */
		su.phy_iso = ml_io_map(KOMO_PMU_PA + (KOMO_PHY_ISO_OFF & ~0xfffu), 0x1000);
		su.pma2 = ml_io_map(reg[0] + 0x8000, 0x1000); /* 0x13208000 candidate */
	}
	if (kmem_alloc_contig(kernel_map, &su.dma, DMA_SIZE, PAGE_MASK, 0, 0,
	    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("TEGU UFS: no contiguous DMA memory\n");
		return ENOMEM;
	}
	ppnum_t pn = pmap_find_phys(kernel_pmap, (addr64_t)su.dma);
	if (!pn) {
		return ENOMEM;
	}
	su.dma_pa = ptoa_64(pn);
	su.lun = 0;
	su.secsize = DEV_BSIZE;

	lck_mtx_lock(&tegu_ufs_lock);
	uint64_t abl_utrl = ((uint64_t)ufs_rd(REG_UTRLBAU) << 32) | ufs_rd(REG_UTRLBA);
	printf("TEGU UFS: ABL request list at 0x%llx (task list 0x%x:%x)\n", abl_utrl,
	    ufs_rd(0x74), ufs_rd(0x70));
	/*
	 * komodo, ufsmode bit4: put our descriptors/bounce buffer where ABL kept
	 * its own request list. Every TUR with our own DMA memory (below and
	 * above 4 GiB) stayed in the doorbell with OCS 0xf: the host's DMA is
	 * most likely fenced (Tensor S2MPU) to the bootloader's buffers.
	 */
	if ((su.mode & 16) && abl_utrl) {
		uint64_t pa = abl_utrl & ~0xfffull;
		/* Normal non-cacheable: bzero/memcpy need unaligned access (Device faults) */
		vm_offset_t va = ml_io_map_wcomb(pa, DMA_SIZE);
		if (va) {
			bzero((void *)va, DMA_SIZE);
			su.dma = va;
			su.dma_pa = pa;
			/* This mapping is Normal-NC (write-combine) and its PA is outside
			 * the kernel physmap, so cache maintenance is both unnecessary and
			 * fatal (flush_dcache64 -> phystokv_range panics). A DSB is enough. */
			su.cache = false;
			su.dma_wc = true;
			printf("TEGU UFS: using ABL's DMA area 0x%llx-0x%llx (cache off)\n", pa, pa + DMA_SIZE);
		}
	}
	ufs_komodo_unfence();
	ufs_exynos_clocks_on();        /* keep UTP/DMA clocks running (ACG off) */
	ufs_probe_link("handoff");     /* read-only: is the link active or hibernated? */
	if (su.mode & 0x80) {
		/* Full recovery: device HW reset (GPIO) + HCE + LINKSTARTUP. */
		ufs_full_reset_relink();
		ufs_probe_link("post-reset");
	} else {
		/* ABL hibernates the link -> device unreachable. Try to exit H8. */
		bool h8ok = ufs_h8_exit_robust();
		ufs_probe_link(h8ok ? "post-H8-exit" : "H8-exit-FAILED");
	}
	if (su.vs && (su.mode & 8)) {
		ufs_exynos_wake();
	}
	error = ufs_take_over_host();
	if (!error) {
		ufs_dump_regs("taken over");
		static const uint8_t tur[6] = { 0x00 };
		/* The first command after the bootloader may report a unit attention.
		 * komodo: on a timeout flip the offset granularity for the next try,
		 * so one boot tests both encodings. */
		for (int i = 0; i < 4 && (error = ufs_scsi(tur, sizeof(tur), 0, false)); i++) {
			if (error == ETIMEDOUT) {
				su.mode ^= 1u;
				printf("TEGU UFS: retrying with mode 0x%x\n", su.mode);
			}
		}
		if (!error) {
			printf("TEGU UFS: TEST UNIT READY ok with mode 0x%x\n", su.mode);
		}
	}
	if (!error) {
		error = ufs_find_partition(want);
	}
	lck_mtx_unlock(&tegu_ufs_lock);
	/*
	 * komodo boot-arg ufsbanner=1: the UFS verdict is otherwise overwritten by
	 * later boot spam on the wrapping fbcon. Print a big banner many times (so
	 * it fills the screen) and spin, so the result is the only thing visible.
	 */
	{
		uint32_t banner = 0;
		PE_parse_boot_argn("ufsbanner", &banner, sizeof(banner));
		if (banner) {
			uint32_t is = ufs_rd(REG_IS), hcs = ufs_rd(REG_HCS), dbr = ufs_rd(REG_UTRLDBR);
			uint32_t pwr = 0xbad, ppwr = 0xbad, pres = 0, lres = 0;
			bool okp = ufs_dme_cmd(UIC_DME_GET, ATTR_PA_PWRMODE, &pwr, &lres);
			bool okpeer = ufs_dme_cmd(UIC_DME_PEER_GET, ATTR_PA_PWRMODE, &ppwr, &pres);
			for (int n = 0; n < 10; n++) {
				printf("\n################ UFS VERDICT (mode 0x%x) ################\n", su.mode);
				printf("## %s  (err %d)\n",
				    error ? "UFS FAIL - no read" : "UFS OK - reads work!!!", error);
				printf("## IS 0x%x HCS 0x%x DBR 0x%x part0 0x%llx %llu MiB\n",
				    is, hcs, dbr, su.first_block, su.nblocks * UFS_BLOCK >> 20);
				printf("## LINK local DME_GET %s PWR 0x%x (res 0x%x); DEVICE peer %s PWR 0x%x (res 0x%x)\n",
				    okp ? "OK" : "FAIL", pwr, lres, okpeer ? "OK" : "FAIL", ppwr, pres);
				printf("## H8exit: tries %u UCCS %d carg2 0x%x UPMS %d UPMCRS %u UECPA 0x%x->0x%x\n",
				    g_h8.tries, g_h8.uccs, g_h8.carg2, g_h8.upms, g_h8.upmcrs,
				    g_h8.uecpa_before, g_h8.uecpa_after);
				printf("## PHY assist: %s  CDR L0 %s (0x%x)  L1 %s (0x%x)\n",
				    g_phy.ran ? "ran" : "OFF",
				    g_phy.cdr_locked[0] ? "lock" : "no", g_phy.trsv339[0],
				    g_phy.cdr_locked[1] ? "lock" : "no", g_phy.trsv339[1]);
				printf("## PHY iso 0x%x->0x%x  PMA0 raw 0x%x wrote0xC6 rb 0x%x  CDR L0 %s(0x%x) L1 %s(0x%x)\n",
				    g_phy.iso_before, g_phy.iso_after, g_phy.pma0_raw, g_phy.comn0_rb,
				    g_phy.cdr_locked[0] ? "lk" : "no", g_phy.trsv339[0],
				    g_phy.cdr_locked[1] ? "lk" : "no", g_phy.trsv339[1]);
				printf("## PMA @04000 id 0x%x RWtest(0x234<-0x5A) rb 0x%x | @08000 id 0x%x rb 0x%x  (rb 0x5a = writable)\n",
				    g_phy.b1_raw, g_phy.b1_rb, g_phy.b2_raw, g_phy.b2_rb);
				printf("########################################################\n");
			}
			for (;;) {
				__builtin_arm_dsb(0xf);
			}
		}
	}
	if (error) {
		return error;
	}

	su.bmajor = bdevsw_add(-1, &tegu_ufs_bdevsw);
	if (su.bmajor < 0) {
		return ENXIO;
	}
	su.cmajor = cdevsw_add_with_bdev(-1, &tegu_ufs_cdevsw, su.bmajor);
	if (su.cmajor < 0) {
		return ENXIO;
	}
	su.bdev = makedev(su.bmajor, 0);
	devfs_make_node(su.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "ufsroot0");
	devfs_make_node(makedev(su.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rufsroot0");
	/* Only now, with the partition bounds known, can writes be allowed. */
	su.writable = (rw != 0);
	su.ready = true;
	printf("TEGU UFS: \"%s\" on LUN %u: first block 0x%llx, %llu MiB, %s, DMA at 0x%llx\n",
	    want, su.lun, su.first_block, su.nblocks * UFS_BLOCK >> 20,
	    su.writable ? "read-write" : "read-only", su.dma_pa);
	tegu_log_start();
	tegu_sample_start();
	/* Input drivers (HID services in iokit/Kernel/arm/hid/TeguHID.cpp).
	 * komodo (Pixel 9 Pro XL, Tensor G4): these three drivers program
	 * Qualcomm SM7150 blocks at fixed physical addresses (SPMI PMIC 0x0c440000,
	 * GENI SE 0x00a84000 / TLMM 0x03900000 / GCC 0x00100000, DWC3 0x0a600000 +
	 * QUSB2 0x088e2000). None of those exist on zumapro; touching them would
	 * SError or hang. They stay off unless booted with qcomdrv=1 (the
	 * original tegu behaviour). komodo replacements: Synaptics TCM touch on
	 * USI SPI, dwc3 @ 0x11210000, S2MPG PMIC keys. */
	int qcomdrv = 0;
	(void)PE_parse_boot_argn("qcomdrv", &qcomdrv, sizeof(qcomdrv));
	if (qcomdrv == 1) {
		tegu_keys_init();
		tegu_touch_init();
		tegu_usb_probe();
	} else {
		printf("komodo: Qualcomm keys/touch/usb drivers disabled (qcomdrv=1 to force)\n");
	}
	return 0;
}

/*
 * Persistent console log ("tegulog"). Filming the phone screen is the only
 * other way to see the console, so every character that reaches the
 * console (kernel printf and /dev/console writes of daemons, both through
 * _cnputs) is also kept in a 4 MiB ring, and once the partition is known
 * and writable a thread copies new ring blocks to the LAST 4 MiB + 4 KiB of
 * the partition every ufslogsecs seconds (default 10), far behind the HFS
 * image at its start. Device block 0 of that area is a header:
 *   "TEGULOG" | u32 version 1 | u32 ring size | u64 total bytes written
 *   | u64 flush sequence
 * ring offset of byte n = n % ring size. Read it from a recovery with
 * tools/tegu-log-pull.sh. Disabled with boot-arg tegulog=0.
 */
#define TEGU_LOG_SIZE          (4u << 20)
#define TEGU_LOG_BLOCKS        (1 + TEGU_LOG_SIZE / UFS_BLOCK)
#define TEGU_LOG_SECS          10u

static char tegu_log_ring[TEGU_LOG_SIZE];
static uint64_t _Atomic tegu_log_total;
static int tegu_log_chan;
static uint32_t tegu_log_secs = TEGU_LOG_SECS;

/* Called for all console output, any context: never blocks. */
void tegu_log_append(const char *c, int size);

void
tegu_log_append(const char *c, int size)
{
	if (size <= 0) {
		return;
	}
	uint64_t at = os_atomic_add_orig(&tegu_log_total, (uint64_t)size, relaxed);
	for (int i = 0; i < size; i++) {
		tegu_log_ring[(at + (uint64_t)i) % TEGU_LOG_SIZE] = c[i];
	}
}

/*
 * Copy console bytes [*pos, total) (at most max) for live log streaming
 * (tegu_usb.c). A reader that fell more than the ring behind skips ahead.
 * Returns the byte count; *pos advances.
 */
uint32_t tegu_log_copy(uint64_t *pos, char *out, uint32_t max);

uint32_t
tegu_log_copy(uint64_t *pos, char *out, uint32_t max)
{
	uint64_t total = os_atomic_load(&tegu_log_total, relaxed);
	if (total - *pos > TEGU_LOG_SIZE) {
		*pos = total - TEGU_LOG_SIZE;
	}
	uint32_t n = (uint32_t)MIN((uint64_t)max, total - *pos);
	for (uint32_t i = 0; i < n; i++) {
		out[i] = tegu_log_ring[(*pos + i) % TEGU_LOG_SIZE];
	}
	*pos += n;
	return n;
}

uint64_t tegu_log_position(void);

uint64_t
tegu_log_position(void)
{
	return os_atomic_load(&tegu_log_total, relaxed);
}

struct tegu_log_header {
	char magic[8];
	uint32_t version, size;
	uint64_t total, seq;
};

static void
tegu_log_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t base = su.first_block + su.nblocks - TEGU_LOG_BLOCKS;
	uint64_t flushed = 0, seq = 0;
	void *bounce = (void *)(su.dma + DMA_BOUNCE);

	printf("TEGU LOG: console log to the last %u KiB of the partition (block 0x%llx)\n",
	    TEGU_LOG_BLOCKS * UFS_BLOCK >> 10, base);
	for (;;) {
		(void)tsleep(&tegu_log_chan, PZERO, "tegulog", hz * (int)tegu_log_secs);
		uint64_t total = os_atomic_load(&tegu_log_total, relaxed);
		if (total == flushed) {
			continue;
		}
		uint64_t pos = total - flushed >= TEGU_LOG_SIZE ? total - TEGU_LOG_SIZE : flushed;
		int error = 0;

		lck_mtx_lock(&tegu_ufs_lock);
		while (pos < total && !error) {
			uint32_t off = (uint32_t)(pos % TEGU_LOG_SIZE);
			uint32_t blk = off / UFS_BLOCK;
			uint64_t need = ((uint64_t)off % UFS_BLOCK + (total - pos) + UFS_BLOCK - 1) / UFS_BLOCK;
			uint32_t n = (uint32_t)MIN(MIN(need, (uint64_t)(TEGU_LOG_SIZE / UFS_BLOCK - blk)),
			    (uint64_t)(BOUNCE_SIZE / UFS_BLOCK));
			memcpy(bounce, tegu_log_ring + (uint64_t)blk * UFS_BLOCK, n * UFS_BLOCK);
			error = ufs_write_blocks(base + 1 + blk, n);
			pos += (uint64_t)(blk + n) * UFS_BLOCK - off;
		}
		if (!error) {
			struct tegu_log_header h = {
				.magic = { 'S', 'W', 'E', 'E', 'T', 'L', 'O', 'G' },
				.version = 1, .size = TEGU_LOG_SIZE, .total = total, .seq = ++seq,
			};
			bzero(bounce, UFS_BLOCK);
			memcpy(bounce, &h, sizeof(h));
			error = ufs_write_blocks(base, 1);
		}
		if (!error) {
			error = ufs_sync_cache();
		}
		lck_mtx_unlock(&tegu_ufs_lock);
		if (error) {
			printf("TEGU LOG: write failed (%d), stopping\n", error);
			return;
		}
		flushed = total;
	}
}

static void
tegu_log_start(void)
{
	uint32_t on = 1;
	thread_t thread;

	PE_parse_boot_argn("tegulog", &on, sizeof(on));
	if (!on || !su.writable || su.nblocks < 2 * TEGU_LOG_BLOCKS) {
		return;
	}
	PE_parse_boot_argn("ufslogsecs", &tegu_log_secs, sizeof(tegu_log_secs));
	if (tegu_log_secs == 0 || tegu_log_secs > 60) {
		tegu_log_secs = TEGU_LOG_SECS;
	}
	if (kernel_thread_start(tegu_log_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* Root device for rd=ufsroot0 (IOFindBSDRoot). */
int tegu_ufs_rootdev(dev_t *dev);

int
tegu_ufs_rootdev(dev_t *dev)
{
	if (!su.ready) {
		int error = tegu_ufs_init();
		if (error) {
			return error;
		}
	}
	*dev = su.bdev;
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_TEGU */
