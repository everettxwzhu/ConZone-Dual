// SPDX-License-Identifier: GPL-2.0-only

/**
 * nvmev.h - NVMeVirt core header file
 *
 * NVMeVirt is a kernel-module-based NVMe virtual device emulator. It maps a
 * reserved physical memory region as a virtual NVMe device exposed to the OS.
 * It supports multiple SSD models: conventional SSD (CONV), Zoned Namespace
 * SSD (ZNS), KV-SSD, and the ConZone dual-namespace prototype specific to
 * this project.
 *
 * Overall architecture:
 *   Reserved physical memory
 *     ├── 1 MiB metadata region (BAR registers, Doorbell registers, MSI-X table)
 *     └── Storage region (mapped as simulated NAND storage medium)
 *
 *   Kernel threads
 *     ├── nvmev_dispatcher: polls BAR register changes and Doorbells, dispatches
 *     │     I/O requests to IO workers
 *     └── nvmev_io_worker[N]: performs data copies and fills CQ completion entries
 *           when the target completion timestamp is reached
 */

#ifndef _LIB_NVMEV_H
#define _LIB_NVMEV_H

#include <linux/pci.h>
#include <linux/msi.h>
#include <asm/apic.h>

#include "nvme.h"

#define CONFIG_NVMEV_IO_WORKER_BY_SQ
#undef CONFIG_NVMEV_FAST_X86_IRQ_HANDLING

#undef CONFIG_NVMEV_VERBOSE
#undef CONFIG_NVMEV_DEBUG
#undef CONFIG_NVMEV_DEBUG_VERBOSE

/*
 * If CONFIG_NVMEVIRT_IDLE_TIMEOUT is set, sleep for a jiffie after
 * CONFIG_NVMEVIRT_IDLE_TIMEOUT seconds have passed to lower CPU power
 * consumption on idle.
 *
 * This may introduce a (1000/CONFIG_HZ) ms processing latency penalty
 * when exiting an I/O idle state.
 *
 * The default is set to 60 seconds, which is extremely conservative and
 * should not have an impact on I/O testing.
 */
#define CONFIG_NVMEVIRT_IDLE_TIMEOUT 60

/*************************/
#define NVMEV_DRV_NAME "NVMeVirt"
#define NVMEV_VERSION 0x0110
#define NVMEV_DEVICE_ID NVMEV_VERSION
#define NVMEV_VENDOR_ID 0x0c51
#define NVMEV_SUBSYSTEM_ID 0x370d
#define NVMEV_SUBSYSTEM_VENDOR_ID NVMEV_VENDOR_ID

#define NVMEV_INFO(string, args...) printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)
#define NVMEV_ERROR(string, args...) printk(KERN_ERR "%s: [ERROR]" string, NVMEV_DRV_NAME, ##args)
#define NVMEV_ASSERT(x) BUG_ON((!(x)))

#define NVMEV_CONZONE_PRINT_TIME(string, args...) \
	// printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)

#ifdef CONFIG_NVMEV_DEBUG
#define NVMEV_DEBUG(string, args...) printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)
#ifdef CONFIG_NVMEV_DEBUG_VERBOSE
#define NVMEV_DEBUG_VERBOSE(string, args...) printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)
#else
#define NVMEV_DEBUG_VERBOSE(string, args...)
#endif
#else
#define NVMEV_DEBUG(string, args...)
#define NVMEV_DEBUG_VERBOSE(string, args...)
#endif

#define NR_MAX_IO_QUEUE 72
#define NR_MAX_PARALLEL_IO 16384

#define NVMEV_INTX_IRQ 15

#define PAGE_OFFSET_MASK (PAGE_SIZE - 1)
#define PRP_PFN(x) ((unsigned long)((x) >> PAGE_SHIFT))

#define KB(k) ((k) << 10)
#define MB(m) ((m) << 20)
#define GB(g) ((g) << 30)

#define BYTE_TO_KB(b) ((b) >> 10)
#define BYTE_TO_MB(b) ((b) >> 20)
#define BYTE_TO_GB(b) ((b) >> 30)

#define MS_PER_SEC(s) ((s) * 1000)
#define US_PER_SEC(s) (MS_PER_SEC(s) * 1000)
#define NS_PER_SEC(s) (US_PER_SEC(s) * 1000)

#define LBA_TO_BYTE(lba) ((lba) << LBA_BITS)
#define BYTE_TO_LBA(byte) ((byte) >> LBA_BITS)

#define BITMASK32_ALL (0xFFFFFFFF)
#define BITMASK64_ALL (0xFFFFFFFFFFFFFFFF)
#define ASSERT(X)

#include "ssd_config.h"

/* SQ statistics: updated by the dispatcher on each Doorbell processing pass */
struct nvmev_sq_stat {
	unsigned int nr_dispatched;		/* total number of commands dispatched */
	unsigned int nr_dispatch;		/* number of Doorbell processing passes (batches) */
	unsigned int nr_in_flight;		/* commands currently in-flight (submitted but not yet completed) */
	unsigned int max_nr_in_flight;	/* peak in-flight command count observed */
	unsigned long long total_io;	/* cumulative bytes of I/O processed */
};

struct nvmev_submission_queue {
	int qid;       /* queue identifier */
	int cqid;      /* associated completion queue ID */
	int priority;  /* queue scheduling priority */
	bool phys_contig; /* true if the queue memory is physically contiguous */

	int queue_size; /* number of entries in the queue */

	struct nvmev_sq_stat stat;

	struct nvme_command __iomem **sq; /* array of pointers to mapped SQ entries */
};

struct nvmev_completion_queue {
	int qid;           /* queue identifier */
	int irq_vector;    /* MSI-X vector number for this CQ */
	bool irq_enabled;  /* whether IRQ delivery is enabled for this CQ */
	bool interrupt_ready; /* true when a new completion is ready to trigger an IRQ */
	bool phys_contig;  /* true if the queue memory is physically contiguous */

	spinlock_t entry_lock; /* protects CQ head/tail and entry writes */
	struct mutex irq_lock; /* serializes IRQ signaling across workers */

	int queue_size; /* number of entries in the queue */

	int phase;    /* phase bit: toggles each time the head wraps around */
	int cq_head;  /* index of the next slot to write a completion entry */
	int cq_tail;  /* index last seen by the host driver (updated via CQ doorbell) */

	struct nvme_completion __iomem **cq; /* array of pointers to mapped CQ entries */
};

struct nvmev_admin_queue {
	int phase;

	int sq_depth;
	int cq_depth;

	int cq_head;

	struct nvme_command __iomem **nvme_sq;
	struct nvme_completion __iomem **nvme_cq;
};

#define NR_SQE_PER_PAGE (PAGE_SIZE / sizeof(struct nvme_command))
#define NR_CQE_PER_PAGE (PAGE_SIZE / sizeof(struct nvme_completion))

#define SQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_SQE_PER_PAGE)
#define CQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_CQE_PER_PAGE)

#define SQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_SQE_PER_PAGE)
#define CQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_CQE_PER_PAGE)

/**
 * struct nvmev_config - module-level configuration loaded from kernel module params
 *
 * The storage area begins at memmap_start + 1 MiB; the first 1 MiB is
 * reserved for the virtual BAR, doorbell registers, and MSI-X table.
 */
struct nvmev_config {
	unsigned long memmap_start; /* physical start address of the reserved memory (bytes) */
	unsigned long memmap_size;  /* total size of the reserved memory region (bytes) */

	unsigned long storage_start; /* physical start of the usable storage area (= memmap_start + 1MiB) */
	unsigned long storage_size;  /* usable storage size (= memmap_size - 1MiB) */

	unsigned int cpu_nr_dispatcher;      /* CPU core pinned to the dispatcher thread */
	unsigned int nr_io_workers;          /* number of IO worker threads */
	unsigned int cpu_nr_io_workers[32];  /* CPU core pinned to each IO worker */

	/* I/O unit configuration for parallel storage simulation */
	unsigned int nr_io_units;    /* number of parallel I/O units */
	unsigned int io_unit_shift;  /* I/O unit size = 2^io_unit_shift bytes */

	/* Emulated device latency model: total latency = delay + time * (size/unit) + trailing */
	unsigned int read_delay;	 /* read command overhead (ns) */
	unsigned int read_time;		 /* per-unit read transfer time (ns) */
	unsigned int read_trailing;	 /* read completion tail latency (ns) */
	unsigned int write_delay;	 /* write command overhead (ns) */
	unsigned int write_time;	 /* per-unit write transfer time (ns) */
	unsigned int write_trailing; /* write completion tail latency (ns) */
};

/**
 * struct nvmev_io_work - a single pending I/O request tracked by an IO worker
 *
 * Work entries form a doubly-linked list sorted by nsecs_target inside each
 * IO worker's work_queue array. The list uses array indices rather than
 * pointers to avoid dynamic allocation overhead.
 */
struct nvmev_io_work {
	int sqid;     /* submission queue ID that originated this request */
	int cqid;     /* completion queue ID to post the result to */

	int sq_entry;          /* index of the SQ entry (command slot) */
	unsigned int command_id; /* NVMe command_id field, echoed in the CQE */

	unsigned long long nsecs_start;      /* wall-clock time when the command was received (ns) */
	unsigned long long nsecs_target;     /* simulated completion time (ns) */

	/* Debug/profiling timestamps (only populated with PERF_DEBUG) */
	unsigned long long nsecs_enqueue;    /* time the work entry was inserted */
	unsigned long long nsecs_copy_start; /* time data copy began */
	unsigned long long nsecs_copy_done;  /* time data copy finished */
	unsigned long long nsecs_cq_filled;  /* time CQE was written */

	bool is_copied;    /* true after the host-memory data copy is complete */
	bool is_completed; /* true after the CQE has been posted (or buffer released) */

	unsigned int status;  /* NVMe status code to report in the CQE */
	unsigned int result0; /* command-specific result DW0 */
	unsigned int result1; /* command-specific result DW1 */

	/* For internal (FTL-generated) operations such as write-buffer flushes */
	bool is_internal;           /* true: this is an internal FTL op, not a host command */
	void *write_buffer;         /* pointer to the write buffer to release on completion */
	size_t buffs_to_release;    /* number of buffer bytes to release */

	unsigned int next, prev; /* index links for the sorted doubly-linked list */
};

/**
 * struct nvmev_io_worker - one IO completion worker thread and its work queue
 *
 * The work_queue is a statically allocated ring of NR_MAX_PARALLEL_IO entries.
 * Free entries are chained via free_seq/free_seq_end; in-flight entries are
 * chained by nsecs_target order via io_seq/io_seq_end.
 */
struct nvmev_io_worker {
	struct nvmev_io_work *work_queue; /* flat array of all work slots */

	unsigned int free_seq;     /* index of the first free slot (free list head) */
	unsigned int free_seq_end; /* index of the last free slot (free list tail) */
	unsigned int io_seq;       /* index of the earliest-deadline in-flight entry (list head) */
	unsigned int io_seq_end;   /* index of the latest-deadline in-flight entry (list tail) */

	unsigned long long latest_nsecs; /* wall-clock time of the last processed completion */

	unsigned int id;                   /* worker index (0-based) */
	struct task_struct *task_struct;   /* the kthread running nvmev_io_worker() */
	char thread_name[32];              /* "nvmev_io_worker_N" */
};

struct nvmev_dev {
	struct pci_bus *virt_bus;
	void *virtDev;
	struct pci_header *pcihdr;
	struct pci_pm_cap *pmcap;
	struct pci_msix_cap *msixcap;
	struct pcie_cap *pciecap;
	struct pci_ext_cap *extcap;

	struct pci_dev *pdev;

	struct nvmev_config config;
	struct task_struct *nvmev_dispatcher;

	void *storage_mapped;

	struct nvmev_io_worker *io_workers;
	unsigned int io_worker_turn;

	void __iomem *msix_table;

	bool intx_disabled;

	struct __nvme_bar *old_bar;
	struct nvme_ctrl_regs __iomem *bar;

	u32 *old_dbs;
	u32 __iomem *dbs;

	struct nvmev_ns *ns;
	unsigned int nr_ns;
	unsigned int nr_sq;
	unsigned int nr_cq;

	struct nvmev_admin_queue *admin_q;
	struct nvmev_submission_queue *sqes[NR_MAX_IO_QUEUE + 1];
	struct nvmev_completion_queue *cqes[NR_MAX_IO_QUEUE + 1];

	unsigned int mdts;

	struct proc_dir_entry *proc_root;
	struct proc_dir_entry *proc_read_times;
	struct proc_dir_entry *proc_write_times;
	struct proc_dir_entry *proc_io_units;
	struct proc_dir_entry *proc_stat;
	struct proc_dir_entry *proc_debug;

	unsigned long long *io_unit_stat;
};

struct nvmev_request {
	struct nvme_command *cmd;
	uint32_t sq_id;
	uint64_t nsecs_start;
};

struct nvmev_result {
	uint32_t status;
	uint64_t nsecs_target;
};

/**
 * struct nvmev_ns - one NVMe namespace
 *
 * Each namespace has its own FTL instance(s) and a set of I/O command
 * handler callbacks registered at initialization time.
 */
struct nvmev_ns {
	uint32_t id;   /* 0-based namespace index */
	uint32_t csi;  /* Command Set Identifier (NVM, ZNS, …) */
	uint64_t size; /* logical namespace size in bytes */
	void *mapped;  /* pointer to the namespace's region in storage_mapped */

	/* FTL layout: one ftl instance per partition */
	uint32_t nr_parts; /* number of partitions (currently always 1) */
	void *ftls;        /* array of FTL instances; cast to conv_ftl/zns_ftl/zms_ftl */

	/* Primary I/O command dispatch callback - called for every NVMe I/O command */
	bool (*proc_io_cmd)(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret);

	/* Optional CSS-specific command identification and execution hooks */
	bool (*identify_io_cmd)(struct nvmev_ns *ns, struct nvme_command cmd);
	unsigned int (*perform_io_cmd)(struct nvmev_ns *ns, struct nvme_command *cmd, uint32_t *status);
};

// VDEV Init, Final Function
extern struct nvmev_dev *nvmev_vdev;
struct nvmev_dev *VDEV_INIT(void);
void VDEV_FINALIZE(struct nvmev_dev *nvmev_vdev);

// OPS_PCI
bool nvmev_proc_bars(void);
bool NVMEV_PCI_INIT(struct nvmev_dev *dev);
void nvmev_signal_irq(int msi_index);

// OPS ADMIN QUEUE
void nvmev_proc_admin_sq(int new_db, int old_db);
void nvmev_proc_admin_cq(int new_db, int old_db);

// OPS I/O QUEUE
struct buffer;
void schedule_internal_operation(int sqid, unsigned long long nsecs_target,
								 struct buffer *write_buffer, size_t buffs_to_release);
void NVMEV_IO_WORKER_INIT(struct nvmev_dev *nvmev_vdev);
void NVMEV_IO_WORKER_FINAL(struct nvmev_dev *nvmev_vdev);
int nvmev_proc_io_sq(int qid, int new_db, int old_db);
void nvmev_proc_io_cq(int qid, int new_db, int old_db);

#endif /* _LIB_NVMEV_H */
