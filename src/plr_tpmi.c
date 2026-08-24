// SPDX-License-Identifier: GPL-3.0-only
/*
 * plr-tpmi - Intel Performance Limit Reasons (PLR) reader/decoder via TPMI.
 *
 * Targets Granite Rapids (GNR) and later Intel Xeon platforms that expose the
 * TPMI (Topology Aware Register and PM Capsule Interface) PLR feature
 * (TPMI ID 0x0C) through the OOB-MSM PCI device.
 *
 * The tool talks to the hardware directly:
 *   PCI config space (sysfs) -> Intel DVSEC/VSEC id 66 (TPMI)
 *   -> PFS (PM Feature Structure) table in the BAR
 *   -> TPMI ID 0x0C (PLR) register bank per power domain
 *
 * It depends only on the C library and standard Linux sysfs paths, so it can
 * be linked statically and dropped onto any target.
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PLR_TOOL_VERSION "1.0"

/* ------------------------------------------------------------------ */
/* PCI / VSEC / TPMI definitions (mirrors include/linux/intel_vsec.h)  */
/* ------------------------------------------------------------------ */

#define PCI_CFG_SPACE_EXP_SIZE		4096
#define PCI_EXT_CAP_START		0x100

#define PCI_EXT_CAP_ID_VNDR		0x0b
#define PCI_EXT_CAP_ID_DVSEC		0x23

#define PCI_VENDOR_ID_INTEL		0x8086

#define PCI_DVSEC_HEADER1		0x04
#define PCI_DVSEC_HEADER2		0x08
#define PCI_VNDR_HEADER			0x04

#define INTEL_DVSEC_ENTRIES		0x0a
#define INTEL_DVSEC_SIZE		0x0b
#define INTEL_DVSEC_TABLE		0x0c

#define VSEC_ID_TPMI			66

/* TPMI feature IDs, see include/linux/intel_tpmi.h */
#define TPMI_ID_RAPL			0x00
#define TPMI_ID_PEM			0x01
#define TPMI_ID_UNCORE			0x02
#define TPMI_ID_SST			0x05
#define TPMI_ID_PLR			0x0c
#define TPMI_CONTROL_ID			0x80
#define TPMI_INFO_ID			0x81

#define TPMI_CAP_OFFSET_UNIT		1024

/* TPMI_INFO layout */
#define TPMI_INFO_BUS_INFO_OFFSET	0x08

/* ------------------------------------------------------------------ */
/* PLR register bank (drivers/platform/x86/intel/plr_tpmi.c)           */
/* ------------------------------------------------------------------ */

#define PLR_HEADER			0x00
#define PLR_MAILBOX_INTERFACE		0x08
#define PLR_MAILBOX_DATA		0x10
#define PLR_DIE_LEVEL			0x18

#define PLR_MODULE_ID_SHIFT		12
#define PLR_MODULE_ID_MASK		0x00000000000ff000ULL	/* bits 19:12 */
#define PLR_RUN_BUSY			(1ULL << 63)
#define PLR_COMMAND_WRITE		1ULL

#define PLR_INVALID			0xffffffffffffffffULL

#define PLR_TIMEOUT_US			1000
#define PLR_POLL_STEP_US		5

#define PLR_COARSE_REASON_BITS		32

/* MSR 0x54: [15:11] PM_DOMAIN_ID, [10:3] MODULE_ID, [2:0] LP_ID */
#define MSR_PM_LOGICAL_ID		0x54
#define LP_ID_MASK			0x7ULL
#define MODULE_ID_SHIFT			3
#define MODULE_ID_MASK			0x7f8ULL
#define PM_DOMAIN_ID_SHIFT		11
#define PM_DOMAIN_ID_MASK		0xf800ULL

static const char *const plr_coarse_reasons[] = {
	"FREQUENCY",
	"CURRENT",
	"POWER",
	"THERMAL",
	"PLATFORM",
	"MCP",
	"RAS",
	"MISC",
	"QOS",
	"DFC",
};

static const char *const plr_fine_reasons[] = {
	"FREQUENCY_CDYN0",
	"FREQUENCY_CDYN1",
	"FREQUENCY_CDYN2",
	"FREQUENCY_CDYN3",
	"FREQUENCY_CDYN4",
	"FREQUENCY_CDYN5",
	"FREQUENCY_FCT",
	"FREQUENCY_PCS_TRL",
	"CURRENT_MTPMAX",
	"POWER_FAST_RAPL",
	"POWER_PKG_PL1_MSR_TPMI",
	"POWER_PKG_PL1_MMIO",
	"POWER_PKG_PL1_PCS",
	"POWER_PKG_PL2_MSR_TPMI",
	"POWER_PKG_PL2_MMIO",
	"POWER_PKG_PL2_PCS",
	"POWER_PLATFORM_PL1_MSR_TPMI",
	"POWER_PLATFORM_PL1_MMIO",
	"POWER_PLATFORM_PL1_PCS",
	"POWER_PLATFORM_PL2_MSR_TPMI",
	"POWER_PLATFORM_PL2_MMIO",
	"POWER_PLATFORM_PL2_PCS",
	NULL,				/* bit 54: reserved */
	"THERMAL_PER_CORE",
	"DFC_UFS",
	"PLATFORM_PROCHOT",
	"PLATFORM_HOT_VR",
	NULL,				/* bit 59: reserved */
	NULL,				/* bit 60: reserved */
	"MISC_PCS_PSTATE",
};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static bool opt_verbose;

static void verbose(const char *fmt, ...)
{
	va_list ap;

	if (!opt_verbose)
		return;

	va_start(ap, fmt);
	fprintf(stderr, "debug: ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void usec_sleep(long usec)
{
	struct timespec ts = { .tv_sec = usec / 1000000,
			       .tv_nsec = (usec % 1000000) * 1000 };

	nanosleep(&ts, NULL);
}

static ssize_t read_file(const char *path, void *buf, size_t len)
{
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	n = pread(fd, buf, len, 0);
	close(fd);

	return n;
}

static int read_file_int(const char *path, long *val)
{
	char buf[64];
	ssize_t n;

	n = read_file(path, buf, sizeof(buf) - 1);
	if (n <= 0)
		return -1;

	buf[n] = '\0';
	errno = 0;
	*val = strtol(buf, NULL, 0);

	return errno ? -1 : 0;
}

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t get_le16(const uint8_t *p)
{
	return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* MMIO accessors: the BAR is mapped uncached, volatile access is enough on x86. */
static uint64_t mmio_read64(const volatile void *base, unsigned int off)
{
	const volatile uint64_t *p =
		(const volatile uint64_t *)((const volatile char *)base + off);
	uint64_t v = *p;

	__asm__ __volatile__("" ::: "memory");
	return v;
}

static void mmio_write64(volatile void *base, unsigned int off, uint64_t val)
{
	volatile uint64_t *p = (volatile uint64_t *)((volatile char *)base + off);

	*p = val;
	__asm__ __volatile__("" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* CPU topology / MSR                                                  */
/* ------------------------------------------------------------------ */

struct cpu_info {
	int cpu;
	int package_id;
	int domain_id;		/* punit power domain */
	int module_id;		/* punit module (core) id */
	int thread_id;
};

static struct cpu_info *cpu_table;
static int cpu_count;
static bool cpu_map_valid;

static int read_msr(int cpu, uint32_t reg, uint64_t *val)
{
	char path[64];
	ssize_t n;
	int fd;

	snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	n = pread(fd, val, sizeof(*val), reg);
	close(fd);

	return n == (ssize_t)sizeof(*val) ? 0 : -1;
}

/* Parse a Linux cpu list such as "0-7,16,20-23". */
static int parse_cpu_list(const char *s, int **out)
{
	int *list = NULL, n = 0, cap = 0;

	while (*s) {
		long a, b;
		char *end;

		while (*s == ',' || isspace((unsigned char)*s))
			s++;
		if (!*s)
			break;

		a = strtol(s, &end, 10);
		if (end == s)
			break;
		s = end;
		b = a;
		if (*s == '-') {
			s++;
			b = strtol(s, &end, 10);
			if (end == s)
				break;
			s = end;
		}

		for (long i = a; i <= b; i++) {
			if (n == cap) {
				int *tmp;

				cap = cap ? cap * 2 : 64;
				tmp = realloc(list, (size_t)cap * sizeof(*list));
				if (!tmp) {
					free(list);
					return -1;
				}
				list = tmp;
			}
			list[n++] = (int)i;
		}
	}

	*out = list;
	return n;
}

static void build_cpu_map(void)
{
	char buf[4096];
	int *cpus = NULL;
	ssize_t len;
	int n;

	len = read_file("/sys/devices/system/cpu/online", buf, sizeof(buf) - 1);
	if (len <= 0)
		return;
	buf[len] = '\0';

	n = parse_cpu_list(buf, &cpus);
	if (n <= 0) {
		free(cpus);
		return;
	}

	cpu_table = calloc((size_t)n, sizeof(*cpu_table));
	if (!cpu_table) {
		free(cpus);
		return;
	}

	for (int i = 0; i < n; i++) {
		struct cpu_info *ci = &cpu_table[cpu_count];
		char path[128];
		uint64_t msr;
		long pkg;

		if (read_msr(cpus[i], MSR_PM_LOGICAL_ID, &msr))
			continue;

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
			 cpus[i]);
		if (read_file_int(path, &pkg))
			continue;

		ci->cpu = cpus[i];
		ci->package_id = (int)pkg;
		ci->domain_id = (int)((msr & PM_DOMAIN_ID_MASK) >> PM_DOMAIN_ID_SHIFT);
		ci->module_id = (int)((msr & MODULE_ID_MASK) >> MODULE_ID_SHIFT);
		ci->thread_id = (int)(msr & LP_ID_MASK);
		cpu_count++;
	}

	free(cpus);
	cpu_map_valid = cpu_count > 0;

	if (!cpu_map_valid)
		verbose("no CPU->punit mapping available (is the 'msr' module loaded?)\n");
}

/* ------------------------------------------------------------------ */
/* TPMI discovery                                                      */
/* ------------------------------------------------------------------ */

struct pfs_entry {
	uint8_t tpmi_id;
	uint8_t num_entries;	/* number of instances (power domains) */
	uint16_t entry_size;	/* in 32-bit words */
	uint16_t cap_offset;	/* in KB from pfs_start */
	uint8_t attribute;
	uint64_t vsec_offset;	/* absolute physical address of instance 0 */
};

#define MAX_PFS_ENTRIES 64

struct tpmi_device {
	char bdf[32];
	int segment, bus, dev, fn;

	uint64_t bar_start;
	uint64_t bar_len;
	int tbir;
	uint64_t pfs_start;

	volatile unsigned char *map;
	bool writable;

	struct pfs_entry pfs[MAX_PFS_ENTRIES];
	int pfs_count;

	/* From TPMI_INFO */
	bool info_valid;
	int package_id;
	int partition;
	uint32_t cdie_mask;
};

static void *map_bar(struct tpmi_device *td, bool want_write)
{
	char path[128];
	void *p = MAP_FAILED;
	int prot, fd;

	prot = PROT_READ | (want_write ? PROT_WRITE : 0);

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource%d",
		 td->bdf, td->tbir);
	fd = open(path, want_write ? O_RDWR | O_CLOEXEC : O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		p = mmap(NULL, (size_t)td->bar_len, prot, MAP_SHARED, fd, 0);
		close(fd);
		if (p != MAP_FAILED) {
			verbose("%s: mapped BAR%d via sysfs resource\n",
				td->bdf, td->tbir);
			return p;
		}
		verbose("%s: sysfs resource mmap failed: %s\n", td->bdf,
			strerror(errno));
	}

	/* Fallback: /dev/mem */
	fd = open("/dev/mem", want_write ? O_RDWR | O_SYNC : O_RDONLY | O_SYNC);
	if (fd < 0)
		return MAP_FAILED;

	p = mmap(NULL, (size_t)td->bar_len, prot, MAP_SHARED, fd,
		 (off_t)td->bar_start);
	close(fd);
	if (p != MAP_FAILED)
		verbose("%s: mapped BAR%d via /dev/mem\n", td->bdf, td->tbir);

	return p;
}

static const volatile void *tpmi_ptr(const struct tpmi_device *td, uint64_t phys)
{
	return (const volatile unsigned char *)td->map + (phys - td->bar_start);
}

static int read_bar_range(struct tpmi_device *td, int bar)
{
	char path[128], buf[2048];
	ssize_t n;
	char *line;

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", td->bdf);
	n = read_file(path, buf, sizeof(buf) - 1);
	if (n <= 0)
		return -1;
	buf[n] = '\0';

	line = buf;
	for (int i = 0; i < bar; i++) {
		line = strchr(line, '\n');
		if (!line)
			return -1;
		line++;
	}

	unsigned long long s, e, f;

	if (sscanf(line, "%llx %llx %llx", &s, &e, &f) != 3)
		return -1;
	if (!s || e < s)
		return -1;

	td->bar_start = s;
	td->bar_len = e - s + 1;

	return 0;
}

/*
 * Look for an Intel TPMI VSEC/DVSEC capability in the device config space.
 * Returns 0 and fills in the BAR/offset information on success.
 */
static int find_tpmi_vsec(const uint8_t *cfg, size_t cfg_len, int *tbir,
			  uint64_t *table_off, uint8_t *num_entries,
			  uint8_t *entry_size)
{
	uint32_t off = PCI_EXT_CAP_START;
	int guard = 0;

	if (get_le16(cfg) != PCI_VENDOR_ID_INTEL)
		return -1;

	while (off >= PCI_EXT_CAP_START && off + 0x10 <= cfg_len && guard++ < 64) {
		uint32_t hdr = get_le32(cfg + off);
		uint16_t cap_id = hdr & 0xffff;
		uint32_t next = (hdr >> 20) & 0xffc;
		uint16_t id = 0;
		uint8_t rev = 0;
		bool match = false;

		if (!hdr || hdr == 0xffffffff)
			break;

		if (cap_id == PCI_EXT_CAP_ID_DVSEC) {
			uint32_t h1 = get_le32(cfg + off + PCI_DVSEC_HEADER1);
			uint32_t h2 = get_le32(cfg + off + PCI_DVSEC_HEADER2);

			if ((h1 & 0xffff) == PCI_VENDOR_ID_INTEL) {
				rev = (h1 >> 16) & 0xf;
				id = h2 & 0xffff;
				match = true;
			}
		} else if (cap_id == PCI_EXT_CAP_ID_VNDR) {
			uint32_t h = get_le32(cfg + off + PCI_VNDR_HEADER);

			rev = (h >> 16) & 0xf;
			id = h & 0xffff;
			match = true;
		}

		if (match && id == VSEC_ID_TPMI && rev == 1) {
			uint32_t table = get_le32(cfg + off + INTEL_DVSEC_TABLE);

			*num_entries = cfg[off + INTEL_DVSEC_ENTRIES];
			*entry_size = cfg[off + INTEL_DVSEC_SIZE];
			*tbir = (int)(table & 0x7);
			*table_off = table & ~0x7u;

			return 0;
		}

		if (next == off)
			break;
		off = next;
	}

	return -1;
}

static void decode_pfs_entry(uint64_t raw, struct pfs_entry *e)
{
	e->tpmi_id = raw & 0xff;
	e->num_entries = (raw >> 8) & 0xff;
	e->entry_size = (raw >> 16) & 0xffff;
	e->cap_offset = (raw >> 32) & 0xffff;
	e->attribute = (raw >> 48) & 0x3;
}

static void tpmi_read_info(struct tpmi_device *td, const struct pfs_entry *e)
{
	uint64_t hdr, info;

	hdr = mmio_read64(tpmi_ptr(td, e->vsec_offset), 0);
	if (hdr == PLR_INVALID)
		return;

	/* Major version must be 0 (see tpmi_process_info()). */
	if (((hdr >> 5) & 0x7) != 0)
		return;

	info = mmio_read64(tpmi_ptr(td, e->vsec_offset), TPMI_INFO_BUS_INFO_OFFSET);
	if (info == PLR_INVALID)
		return;

	/* fn:3 dev:5 bus:8 pkg:8 segment:8 partition:2 cdie_mask:16 */
	td->fn = (int)(info & 0x7);
	td->dev = (int)((info >> 3) & 0x1f);
	td->bus = (int)((info >> 8) & 0xff);
	td->package_id = (int)((info >> 16) & 0xff);

	if ((hdr & 0x1f) >= 2) {	/* minor version >= 2 */
		td->segment = (int)((info >> 24) & 0xff);
		td->partition = (int)((info >> 32) & 0x3);
		td->cdie_mask = (uint32_t)((info >> 34) & 0xffff);
	}

	td->info_valid = true;
}

/* Probe a single PCI device; returns 0 if it exposes TPMI. */
static int probe_device(const char *bdf, struct tpmi_device *td, bool want_write)
{
	uint8_t cfg[PCI_CFG_SPACE_EXP_SIZE];
	uint8_t num_entries, entry_size;
	uint64_t table_off;
	char path[128];
	ssize_t n;
	int tbir;

	memset(td, 0, sizeof(*td));
	if (strlen(bdf) >= sizeof(td->bdf))
		return -1;
	memcpy(td->bdf, bdf, strlen(bdf) + 1);

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", td->bdf);
	n = read_file(path, cfg, sizeof(cfg));
	if (n < PCI_EXT_CAP_START + 0x10)
		return -1;

	if (find_tpmi_vsec(cfg, (size_t)n, &tbir, &table_off, &num_entries,
			   &entry_size))
		return -1;

	if (!num_entries || !entry_size)
		return -1;

	if (read_bar_range(td, tbir))
		return -1;

	td->tbir = tbir;
	td->pfs_start = td->bar_start + table_off;

	if (td->pfs_start + (uint64_t)num_entries * entry_size * 4 >
	    td->bar_start + td->bar_len) {
		fprintf(stderr, "%s: PFS table outside BAR%d, skipping\n", bdf, tbir);
		return -1;
	}

	td->map = map_bar(td, want_write);
	if (td->map == MAP_FAILED) {
		td->map = NULL;
		fprintf(stderr, "%s: cannot map BAR%d (%s); run as root\n", bdf,
			tbir, strerror(errno));
		return -1;
	}
	td->writable = want_write;

	/* Fill in defaults from the BDF, TPMI_INFO may override them. */
	sscanf(td->bdf, "%x:%x:%x.%x", (unsigned int *)&td->segment,
	       (unsigned int *)&td->bus, (unsigned int *)&td->dev,
	       (unsigned int *)&td->fn);
	td->package_id = -1;

	for (int i = 0; i < num_entries && i < MAX_PFS_ENTRIES; i++) {
		uint64_t raw = mmio_read64(tpmi_ptr(td, td->pfs_start),
					   (unsigned int)(i * entry_size * 4));
		struct pfs_entry *e = &td->pfs[td->pfs_count];

		if (raw == PLR_INVALID || raw == 0)
			continue;

		decode_pfs_entry(raw, e);
		if (e->entry_size == 0 || e->entry_size > 1024)
			continue;

		e->vsec_offset = td->pfs_start +
				 (uint64_t)e->cap_offset * TPMI_CAP_OFFSET_UNIT;

		if (e->vsec_offset + (uint64_t)e->num_entries * e->entry_size * 4 >
		    td->bar_start + td->bar_len)
			continue;

		td->pfs_count++;

		if (e->tpmi_id == TPMI_INFO_ID)
			tpmi_read_info(td, e);
	}

	return td->pfs_count ? 0 : -1;
}

static void unprobe_device(struct tpmi_device *td)
{
	if (td->map)
		munmap((void *)td->map, (size_t)td->bar_len);
	td->map = NULL;
}

static int enumerate_devices(struct tpmi_device **out, bool want_write)
{
	const char *dir_path = "/sys/bus/pci/devices";
	struct tpmi_device *list = NULL;
	struct dirent *de;
	int count = 0, cap = 0;
	DIR *d;

	d = opendir(dir_path);
	if (!d) {
		fprintf(stderr, "cannot open %s: %s\n", dir_path, strerror(errno));
		return -1;
	}

	while ((de = readdir(d))) {
		struct tpmi_device td;

		if (de->d_name[0] == '.')
			continue;

		if (probe_device(de->d_name, &td, want_write))
			continue;

		if (count == cap) {
			struct tpmi_device *tmp;

			cap = cap ? cap * 2 : 8;
			tmp = realloc(list, (size_t)cap * sizeof(*list));
			if (!tmp) {
				unprobe_device(&td);
				break;
			}
			list = tmp;
		}
		list[count++] = td;
	}

	closedir(d);
	*out = list;

	return count;
}

static const struct pfs_entry *find_feature(const struct tpmi_device *td, uint8_t id)
{
	for (int i = 0; i < td->pfs_count; i++)
		if (td->pfs[i].tpmi_id == id)
			return &td->pfs[i];

	return NULL;
}

static const char *tpmi_feature_name(uint8_t id)
{
	switch (id) {
	case TPMI_ID_RAPL:	return "rapl";
	case TPMI_ID_PEM:	return "pem";
	case TPMI_ID_UNCORE:	return "uncore";
	case TPMI_ID_SST:	return "sst";
	case TPMI_ID_PLR:	return "plr";
	case TPMI_CONTROL_ID:	return "control";
	case TPMI_INFO_ID:	return "info";
	default:		return "unknown";
	}
}

/* ------------------------------------------------------------------ */
/* PLR access and decoding                                             */
/* ------------------------------------------------------------------ */

static uint64_t plr_domain_base(const struct pfs_entry *plr, int domain)
{
	return plr->vsec_offset + (uint64_t)domain * plr->entry_size * 4;
}

static int plr_wait_mailbox(const volatile void *base)
{
	for (long waited = 0; waited <= PLR_TIMEOUT_US; waited += PLR_POLL_STEP_US) {
		if (!(mmio_read64(base, PLR_MAILBOX_INTERFACE) & PLR_RUN_BUSY))
			return 0;
		usec_sleep(PLR_POLL_STEP_US);
	}

	return -1;
}

static int plr_read_module(volatile void *base, int module_id, uint64_t *status)
{
	uint64_t cmd;

	cmd = ((uint64_t)module_id << PLR_MODULE_ID_SHIFT) & PLR_MODULE_ID_MASK;
	cmd |= PLR_RUN_BUSY;

	mmio_write64(base, PLR_MAILBOX_INTERFACE, cmd);
	if (plr_wait_mailbox(base))
		return -1;

	*status = mmio_read64(base, PLR_MAILBOX_DATA);

	return 0;
}

static int plr_clear_module(volatile void *base, int module_id)
{
	uint64_t cmd;

	cmd = ((uint64_t)module_id << PLR_MODULE_ID_SHIFT) & PLR_MODULE_ID_MASK;
	cmd |= PLR_RUN_BUSY | PLR_COMMAND_WRITE;

	mmio_write64(base, PLR_MAILBOX_DATA, 0);
	mmio_write64(base, PLR_MAILBOX_INTERFACE, cmd);

	return plr_wait_mailbox(base);
}

static void print_reasons(uint64_t val, int bits)
{
	bool first = true;

	if (!val) {
		printf(" none");
		return;
	}

	for (int bit = 0; bit < bits; bit++) {
		const char *str = NULL;
		char fallback[24];

		if (!(val & (1ULL << bit)))
			continue;

		if (bit < PLR_COARSE_REASON_BITS) {
			if ((size_t)bit < ARRAY_SIZE(plr_coarse_reasons))
				str = plr_coarse_reasons[bit];
		} else {
			int index = bit - PLR_COARSE_REASON_BITS;

			if ((size_t)index < ARRAY_SIZE(plr_fine_reasons))
				str = plr_fine_reasons[index];
		}

		if (!str) {
			snprintf(fallback, sizeof(fallback), "UNKNOWN(%d)", bit);
			str = fallback;
		}

		printf("%s%s", first ? " " : " ", str);
		first = false;
	}
}

/* A punit module holds a handful of logical CPUs (SMT threads / atom cores). */
#define MODULE_MAX_CPUS 8
#define MAX_MODULES 512

struct module_entry {
	int module_id;
	int cpus[MODULE_MAX_CPUS];
	int ncpus;
};

/* Collect the punit modules belonging to <package, domain>. */
static int collect_modules(int package_id, int domain_id,
			   struct module_entry *mods, int max_mods)
{
	int nmods = 0;

	if (!cpu_map_valid)
		return 0;

	for (int i = 0; i < cpu_count; i++) {
		const struct cpu_info *ci = &cpu_table[i];
		int j;

		if (package_id >= 0 && ci->package_id != package_id)
			continue;
		if (ci->domain_id != domain_id)
			continue;

		for (j = 0; j < nmods; j++)
			if (mods[j].module_id == ci->module_id)
				break;

		if (j == nmods) {
			if (nmods == max_mods)
				continue;
			mods[nmods].module_id = ci->module_id;
			mods[nmods].ncpus = 0;
			nmods++;
		}

		if (mods[j].ncpus < (int)ARRAY_SIZE(mods[j].cpus))
			mods[j].cpus[mods[j].ncpus++] = ci->cpu;
	}

	/* Sort by module id for stable output. */
	for (int i = 1; i < nmods; i++) {
		struct module_entry key = mods[i];
		int j = i - 1;

		while (j >= 0 && mods[j].module_id > key.module_id) {
			mods[j + 1] = mods[j];
			j--;
		}
		mods[j + 1] = key;
	}

	return nmods;
}

static void print_cpu_list(const struct module_entry *m)
{
	for (int i = 0; i < m->ncpus; i++)
		printf("%s%d", i ? "," : "", m->cpus[i]);
}

static int show_plr(struct tpmi_device *td, bool clear, bool only_active)
{
	static struct module_entry mods[MAX_MODULES];
	const struct pfs_entry *plr;
	int shown = 0;

	plr = find_feature(td, TPMI_ID_PLR);
	if (!plr)
		return 0;

	printf("TPMI device %s  package %d  segment %d  partition %d  (PFS 0x%llx)\n",
	       td->bdf, td->package_id, td->segment, td->partition,
	       (unsigned long long)td->pfs_start);

	for (int dom = 0; dom < plr->num_entries; dom++) {
		uint64_t phys = plr_domain_base(plr, dom);
		volatile void *base = (volatile unsigned char *)td->map +
				      (phys - td->bar_start);
		uint64_t hdr, die;
		int nmods;

		hdr = mmio_read64(base, PLR_HEADER);
		if (hdr == PLR_INVALID)
			continue;

		die = mmio_read64(base, PLR_DIE_LEVEL);

		printf("  domain %d  (base 0x%llx)\n", dom,
		       (unsigned long long)phys);
		printf("    die-level                  0x%08llx :",
		       (unsigned long long)(die & 0xffffffffULL));
		print_reasons(die & 0xffffffffULL, 32);
		printf("\n");
		shown++;

		if (clear && td->writable)
			mmio_write64(base, PLR_DIE_LEVEL, 0);

		if (!td->writable) {
			printf("    (per-core status needs write access to the mailbox)\n");
			continue;
		}

		nmods = collect_modules(td->package_id, dom, mods,
					(int)ARRAY_SIZE(mods));
		if (!nmods) {
			if (!cpu_map_valid)
				printf("    (per-core status unavailable: load the 'msr' module)\n");
			continue;
		}

		for (int i = 0; i < nmods; i++) {
			uint64_t status;

			if (plr_read_module(base, mods[i].module_id, &status)) {
				printf("    module %-3d               : <mailbox timeout>\n",
				       mods[i].module_id);
				continue;
			}

			if (clear)
				plr_clear_module(base, mods[i].module_id);

			if (only_active && !status)
				continue;

			printf("    module %-3d cpu ", mods[i].module_id);
			print_cpu_list(&mods[i]);
			printf("\n      0x%016llx :", (unsigned long long)status);
			print_reasons(status, 64);
			printf("\n");
		}
	}

	return shown;
}

static void list_features(const struct tpmi_device *td)
{
	printf("TPMI device %s  package %d  BAR%d 0x%llx+0x%llx  PFS 0x%llx\n",
	       td->bdf, td->package_id, td->tbir,
	       (unsigned long long)td->bar_start,
	       (unsigned long long)td->bar_len,
	       (unsigned long long)td->pfs_start);
	printf("  %-6s %-10s %-8s %-10s %-10s %s\n", "id", "name", "entries",
	       "entry_sz", "cap_off", "base");

	for (int i = 0; i < td->pfs_count; i++) {
		const struct pfs_entry *e = &td->pfs[i];

		printf("  0x%02x   %-10s %-8u 0x%04x     0x%04x     0x%llx\n",
		       e->tpmi_id, tpmi_feature_name(e->tpmi_id), e->num_entries,
		       e->entry_size, e->cap_offset,
		       (unsigned long long)e->vsec_offset);
	}
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	printf("plr-tpmi %s - decode Intel Performance Limit Reasons via TPMI\n\n"
	       "Usage: %s [options]\n\n"
	       "Options:\n"
	       "  -l, --list           list all TPMI features found on each device\n"
	       "  -c, --clear          clear the PLR status after reading it\n"
	       "  -a, --active-only    only print cores that report a limit reason\n"
	       "  -w, --watch SEC      repeat every SEC seconds (0.1 granularity)\n"
	       "  -n, --count N        stop after N iterations (with --watch)\n"
	       "  -v, --verbose        print discovery details on stderr\n"
	       "  -V, --version        print version and exit\n"
	       "  -h, --help           this help\n\n"
	       "Notes:\n"
	       "  * Requires root (PCI BAR mapping) on GNR or newer Xeon.\n"
	       "  * Per-core reasons additionally require 'modprobe msr'.\n",
	       PLR_TOOL_VERSION, prog);
}

int main(int argc, char **argv)
{
	static const struct option long_opts[] = {
		{ "list",        no_argument,       NULL, 'l' },
		{ "clear",       no_argument,       NULL, 'c' },
		{ "active-only", no_argument,       NULL, 'a' },
		{ "watch",       required_argument, NULL, 'w' },
		{ "count",       required_argument, NULL, 'n' },
		{ "verbose",     no_argument,       NULL, 'v' },
		{ "version",     no_argument,       NULL, 'V' },
		{ "help",        no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	bool do_list = false, do_clear = false, active_only = false;
	double watch_sec = 0.0;
	long watch_count = 0;
	struct tpmi_device *devs;
	int ndevs, opt, rc = 0;

	while ((opt = getopt_long(argc, argv, "lcaw:n:vVh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'l': do_list = true; break;
		case 'c': do_clear = true; break;
		case 'a': active_only = true; break;
		case 'w': watch_sec = atof(optarg); break;
		case 'n': watch_count = atol(optarg); break;
		case 'v': opt_verbose = true; break;
		case 'V': printf("plr-tpmi %s\n", PLR_TOOL_VERSION); return 0;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	if (geteuid() != 0)
		fprintf(stderr, "warning: not running as root, MMIO access will likely fail\n");

	build_cpu_map();

	ndevs = enumerate_devices(&devs, true);
	if (ndevs <= 0) {
		/* Retry read-only: die-level reasons still work. */
		ndevs = enumerate_devices(&devs, false);
		if (ndevs <= 0) {
			fprintf(stderr,
				"no Intel TPMI capable PCI device found (GNR or newer required)\n");
			free(cpu_table);
			return 1;
		}
	}

	if (do_list) {
		for (int i = 0; i < ndevs; i++)
			list_features(&devs[i]);
	}

	for (long iter = 0;; iter++) {
		int shown = 0;

		if (iter && watch_sec > 0.0)
			printf("\n");

		for (int i = 0; i < ndevs; i++)
			shown += show_plr(&devs[i], do_clear, active_only);

		if (!shown && !iter) {
			fprintf(stderr,
				"no PLR (TPMI id 0x%02x) feature present; use --list to inspect TPMI features\n",
				TPMI_ID_PLR);
			rc = 1;
			break;
		}

		if (watch_sec <= 0.0)
			break;
		if (watch_count && iter + 1 >= watch_count)
			break;

		fflush(stdout);
		usec_sleep((long)(watch_sec * 1000000.0));
	}

	for (int i = 0; i < ndevs; i++)
		unprobe_device(&devs[i]);

	free(devs);
	free(cpu_table);

	return rc;
}
