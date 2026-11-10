from pathlib import Path
import re

p = Path('xnu/fuxi-h1/fuxi_keys.c')
s = p.read_text().replace('\r\n', '\n')
i = s.find('/*\n * WiFi stage 1')
assert i > 0, 'probe section not found'
head = s[:i].rstrip('\n')

# refresh the stale prototype comment
head = head.replace(
    'void fuxi_wifi_probe_init(void); /* bsd/dev/fuxi_wifi.c: stage 1, fuxiwifiprobe */',
    'void fuxi_wifi_probe_init(void); /* fuxiwifiprobe: WiFi stage 1 read-only, stage 2 rails */')

new = r'''
/*
 * WiFi bring-up probe (boot-arg fuxiwifiprobe=STAGE), read-only except stage 2.
 *
 * Hardware from the live DT (build/live.dtb) and docs-lanes:
 *   GCC 0x100000, PCIe0 PARF 0x1c00000, PHY 0x1c06000, DBI 0x60000000,
 *   pinctrl@f000000; apps_rsc DRV2 0x17a20000 (tcs-offset 0xd00),
 *   AOP cmd-db 0x81c60000 (0x20000); PMICs PM8550/PM8550VS/PM8550VE.
 *   WLAN_EN GPIO 80, SW_CTRL GPIO 82, PERST# GPIO 94 (active low).
 * The register map, the rail list and the request format come from
 * UniXNU-sheng package/{WifiProbe,QcomRpmh,WcnPower} (BSD-2-Clause-Patent).
 *
 *   stage 1  GCC votes, PCIe0 GDSC and the branch halt registers (read only)
 *   stage 2  stage 1 + vote the six WCN7850 rails ON through the apps RSC,
 *            then read the GCC state again (writes: RPMh rail votes only)
 *   stage 3  reserved for WLAN_EN/PERST#/GDSC/PCIe training
 * Nothing else writes a register. A block that is not clocked is never read:
 * the PCIe controller registers only after the GDSC is on and the four
 * required branches run.
 */
#define FW_GCC		0x00100000ULL
#define FW_PARF		0x01c00000ULL
#define FW_DBI		0x60000000ULL
#define FW_RSC		0x17a20000ULL
#define FW_CMDD_B	0x81c60000ULL
#define FW_CMDD_SZ	0x20000u
#define FW_TCS		0xd00u
#define FW_VOTE8	0x52008u
#define FW_VOTE0	0x52000u
#define FW_GDSC		0x6b004u
#define FW_PARF_LTSSM	0x1b0u
#define FW_PARF_PHYCTL	0x40u
#define FW_LINK_CONTROL	0x80u

#define FW_MSGID_WR	(8u | (1u << 8) | (1u << 16))
#define FW_CTL_ACTIVE	(1u << 16)
#define FW_CTL_TRIGGER	(1u << 24)
#define FW_DB_MAGIC	0x0c0330dbu

struct fw_rail {
	const char *name;
	uint32_t millivolts;
};

/* WCN7850 PMU supplies, in the order the power sequencer takes them. */
static const struct fw_rail fw_rails[] = {
	{ "smpg5", 850 }, { "ldob15", 1800 }, { "smpg2", 850 },
	{ "smpe4", 950 }, { "smpg4", 1300 }, { "smpg6", 1860 },
};

struct fw_rsc_layout {
	uint32_t tcs_stride, cmd_stride, irq_status, irq_clear, wait_for_cmpl,
	    control, status, cmd_enable, cmd_msgid, cmd_addr, cmd_data;
};

static const uint32_t fw_branches[] = {
	0x6b020, 0x6b02c, 0x6b038, 0x6b03c, 0x6b048,
	0x6b01c, 0x10030, 0x1003c, 0x1004c, 0x6b054,
};

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

static struct fw_rsc_layout
fw_layout(uint32_t drv_id)
{
	struct fw_rsc_layout v3 = { 672, 24, 0x04, 0x08, 0x20, 0x24, 0x28,
		0x2c, 0x34, 0x38, 0x3c };
	struct fw_rsc_layout v2 = { 672, 20, 0x04, 0x08, 0x10, 0x14, 0x18,
		0x1c, 0x30, 0x34, 0x38 };
	return ((drv_id >> 16) & 0xffu) >= 3u ? v3 : v2;
}

static uint16_t
fw_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t
fw_le32(const uint8_t *p)
{
	return (uint32_t)fw_le16(p) | ((uint32_t)fw_le16(p + 2) << 16);
}

/* AOP command database: header, then 8 slave records, then the entries the
 * offsets count from (byte 144). Entries are 24 bytes with an 8-byte name. */
static int
fw_db_find(const uint8_t *db, size_t bytes, const char *name, uint32_t *address)
{
	char id[8] = { 0 };
	unsigned s, i;

	if (bytes < 144 || fw_le32(db + 4) != FW_DB_MAGIC) {
		return -1;
	}
	for (i = 0; i < 8 && name[i]; i++) {
		id[i] = name[i];
	}
	for (s = 0; s < 8; s++) {
		const uint8_t *slave = db + 8 + 16 * s;
		uint32_t heads, count, e;

		if (!fw_le16(slave)) {
			break;
		}
		heads = 144u + fw_le16(slave + 2);
		count = fw_le16(slave + 6);
		if ((size_t)heads + (size_t)count * 24 > bytes) {
			return -1;
		}
		for (e = 0; e < count; e++) {
			const uint8_t *entry = db + heads + 24 * e;
			unsigned same = 1;

			for (i = 0; i < 8 && same; i++) {
				same = entry[i] == (uint8_t)id[i];
			}
			if (!same) {
				continue;
			}
			*address = fw_le32(entry + 16);
			return *address ? 0 : -1;
		}
	}
	return -1;
}

/* One active-only request of a single command in TCS 0, applied in order and
 * released after two milliseconds (the RSC's done latch reads zero here, as
 * sheng found with the same firmware). */
static void
fw_rpmh_send(volatile uint32_t *r, const struct fw_rsc_layout *l, uint32_t addr,
    uint32_t value)
{
	const uint32_t b = FW_TCS + l->tcs_stride * 0;
	const uint32_t cmd = b + l->cmd_stride * 0;

	r[(b + l->control) / 4] = 0;
	r[(b + l->irq_clear) / 4] = 1u << 0;
	r[(cmd + l->cmd_msgid) / 4] = FW_MSGID_WR;
	r[(cmd + l->cmd_addr) / 4] = addr;
	r[(cmd + l->cmd_data) / 4] = value;
	r[(b + l->wait_for_cmpl) / 4] = 1u;
	r[(b + l->cmd_enable) / 4] = 1u;
	r[(b + l->control) / 4] = FW_CTL_ACTIVE;
	__asm__ volatile("dsb sy" ::: "memory");
	r[(b + l->control) / 4] = FW_CTL_ACTIVE | FW_CTL_TRIGGER;
	__asm__ volatile("dsb sy" ::: "memory");

	delay(2000);

	r[(b + l->irq_clear) / 4] = 1u << 0;
	r[(b + l->control) / 4] = 0;
	r[(b + l->cmd_enable) / 4] = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
fw_read_gcc(const char *tag)
{
	volatile uint32_t *gcc;
	uint32_t vote8, vote0, gdsc, halt = 0;
	unsigned i;

	gcc = (volatile uint32_t *)ml_io_map(FW_GCC, 0x80000);
	if (!gcc) {
		printf("FUXI WIFI: %s gcc map failed\n", tag);
		return;
	}
	vote8 = gcc[FW_VOTE8 / 4u];
	vote0 = gcc[FW_VOTE0 / 4u];
	gdsc = gcc[FW_GDSC / 4u];
	for (i = 0; i < sizeof fw_branches / sizeof fw_branches[0]; i++) {
		if (gcc[fw_branches[i] / 4u] & (1u << 31)) {
			halt |= 1u << i;
		}
	}
	printf("FUXI WIFI: %s gcc vote8 %#x vote0 %#x gdsc %#x halt %#x\n",
	    tag, vote8, vote0, gdsc, halt);
}

static void
fw_power_rails(void)
{
	volatile uint32_t *r;
	const uint8_t *db;
	struct fw_rsc_layout l;
	uint32_t found = 0;
	unsigned i;

	db = (const uint8_t *)ml_io_map(FW_CMDD_B, FW_CMDD_SZ);
	if (!db) {
		printf("FUXI WIFI: cmd-db map failed\n");
		return;
	}
	r = (volatile uint32_t *)ml_io_map(FW_RSC, 0x10000);
	if (!r) {
		printf("FUXI WIFI: apps RSC map failed\n");
		return;
	}
	l = fw_layout(r[0]);
	printf("FUXI WIFI: rsc drv id %#x tcs-cfg %#x stride %u cmd %u\n",
	    r[0], r[0x0cu / 4u], l.tcs_stride, l.cmd_stride);

	/* Keep the three active TCS done interrupts enabled and clear stale bits. */
	r[FW_TCS / 4] = 7u;
	r[(FW_TCS + l.irq_clear) / 4] = 7u;
	__asm__ volatile("dsb sy" ::: "memory");

	for (i = 0; i < sizeof fw_rails / sizeof fw_rails[0]; i++) {
		uint32_t addr = 0;

		if (fw_db_find(db, FW_CMDD_SZ, fw_rails[i].name, &addr)) {
			printf("FUXI WIFI: rail %s missing in cmd-db\n", fw_rails[i].name);
			continue;
		}
		found |= 1u << i;
		printf("FUXI WIFI: rail %s addr %#x -> %u mV\n", fw_rails[i].name,
		    addr, fw_rails[i].millivolts);
		fw_rpmh_send(r, &l, addr + 8, 7);                 /* mode: HPM */
		fw_rpmh_send(r, &l, addr, fw_rails[i].millivolts); /* voltage */
		fw_rpmh_send(r, &l, addr + 4, 1);                 /* enable */
		delay(20000);
	}
	printf("FUXI WIFI: rails found %#x of %#x\n", found,
	    (uint32_t)((1u << (sizeof fw_rails / sizeof fw_rails[0])) - 1u));
}

static void
fk_wifi_thread(void *arg, wait_result_t wr)
{
	unsigned stage = (unsigned)(uintptr_t)arg;

	(void)wr;
	thread_set_thread_name(current_thread(), "fuxi-wifi");
	printf("FUXI WIFI: stage %u starting\n", stage);

	fw_read_gcc("before");
	if (stage >= 2) {
		printf("FUXI WIFI: voting WCN7850 rails\n");
		fw_power_rails();
		fw_read_gcc("after");
	}
	if (stage >= 3) {
		printf("FUXI WIFI: stage 3 not implemented yet\n");
	}
	printf("FUXI WIFI: probe done\n");
}

void
fuxi_wifi_probe_init(void)
{
	unsigned stage = 0;
	thread_t thread = THREAD_NULL;

	if (!PE_parse_boot_argn("fuxiwifiprobe", &stage, sizeof stage) || stage == 0) {
		return;
	}
	if (kernel_thread_start(fk_wifi_thread, (void *)(uintptr_t)stage, &thread) ==
	    KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("FUXI WIFI: thread start failed\n");
	}
}
#endif /* FUXI */
'''
p.write_bytes((head + '\n' + new).encode())
print('written', len(head + new))