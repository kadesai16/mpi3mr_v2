/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Driver for Broadcom MPI3 Storage Controllers
 *
 * Copyright (C) 2017-2020 Broadcom Inc.
 *  (mailto: mpi3mr-linuxdrv.pdl@broadcom.com)
 *
 */

#ifndef MPI3MR_H_INCLUDED
#define MPI3MR_H_INCLUDED

#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/blk-mq-pci.h>
#include <linux/delay.h>
#include <linux/dmapool.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/utsname.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <asm/unaligned.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_tcq.h>

#include "mpi/mpi30_api.h"
#include "mpi3mr_debug.h"

/* Global list and lock for storing multiple adapters managed by the driver */
extern spinlock_t mrioc_list_lock;
extern struct list_head mrioc_list;

#define MPI3MR_DRIVER_VERSION	"00.255.45.01"
#define MPI3MR_DRIVER_RELDATE	"12-December-2020"

#define MPI3MR_DRIVER_NAME	"mpi3mr"
#define MPI3MR_DRIVER_LICENSE	"GPL"
#define MPI3MR_DRIVER_AUTHOR	"Broadcom Inc. <mpi3mr-linuxdrv.pdl@broadcom.com>"
#define MPI3MR_DRIVER_DESC	"MPI3 Storage Controller Device Driver"

#define MPI3MR_NAME_LENGTH	32
#define IOCNAME			"%s: "

/* Definitions for internal SGL and Chain SGL buffers */
#define MPI3MR_PAGE_SIZE_4K		4096
#define MPI3MR_SG_DEPTH		(PAGE_SIZE/sizeof(Mpi3SGESimple_t))

/* Definitions for MAX values for shost */
#define MPI3MR_MAX_CMDS_LUN	7
#define MPI3MR_MAX_CDB_LENGTH	32

/* Admin queue management definitions */
#define MPI3MR_ADMIN_REQ_Q_SIZE		(2 * MPI3MR_PAGE_SIZE_4K)
#define MPI3MR_ADMIN_REPLY_Q_SIZE	(4 * MPI3MR_PAGE_SIZE_4K)
#define MPI3MR_ADMIN_REQ_FRAME_SZ	128
#define MPI3MR_ADMIN_REPLY_FRAME_SZ	16

/* Operational queue management definitions */
#define MPI3MR_OP_REQ_Q_QD		512
#define MPI3MR_OP_REP_Q_QD		4096
#define MPI3MR_OP_REQ_Q_SEG_SIZE	4096
#define MPI3MR_OP_REP_Q_SEG_SIZE	4096
#define MPI3MR_MAX_SEG_LIST_SIZE	4096

/* Reserved Host Tag definitions */
#define MPI3MR_HOSTTAG_INVALID		0xFFFF
#define MPI3MR_HOSTTAG_INITCMDS		1
#define MPI3MR_HOSTTAG_IOCTLCMDS	2
#define MPI3MR_HOSTTAG_BLK_TMS		5

#define MPI3MR_NUM_DEVRMCMD		1
#define MPI3MR_HOSTTAG_DEVRMCMD_MIN	(MPI3MR_HOSTTAG_BLK_TMS + 1)
#define MPI3MR_HOSTTAG_DEVRMCMD_MAX	(MPI3MR_HOSTTAG_DEVRMCMD_MIN + \
						MPI3MR_NUM_DEVRMCMD - 1)

#define MPI3MR_INTERNAL_CMDS_RESVD     MPI3MR_HOSTTAG_DEVRMCMD_MAX

/* Reduced resource count definition for crash kernel */
#define MPI3MR_HOST_IOS_KDUMP		128

/* command/controller interaction timeout definitions in seconds */
#define MPI3MR_INTADMCMD_TIMEOUT		10
#define MPI3MR_PORTENABLE_TIMEOUT		300
#define MPI3MR_RESETTM_TIMEOUT			30
#define MPI3MR_DEFAULT_SHUTDOWN_TIME		120

#define MPI3MR_WATCHDOG_INTERVAL		1000 /* in milli seconds */

/* Internal admin command state definitions*/
#define MPI3MR_CMD_NOTUSED	0x8000
#define MPI3MR_CMD_COMPLETE	0x0001
#define MPI3MR_CMD_PENDING	0x0002
#define MPI3MR_CMD_REPLY_VALID	0x0004
#define MPI3MR_CMD_RESET	0x0008

/* Definitions for Event replies and sense buffer allocated per controller */
#define MPI3MR_NUM_EVT_REPLIES	64
#define MPI3MR_SENSEBUF_SZ	256
#define MPI3MR_SENSEBUF_FACTOR	3
#define MPI3MR_CHAINBUF_FACTOR	3

/* Invalid target device handle */
#define MPI3MR_INVALID_DEV_HANDLE	0xFFFF

/* Controller Reset related definitions */
#define MPI3MR_HOSTDIAG_UNLOCK_RETRY_COUNT	5
#define MPI3MR_MAX_RESET_RETRY_COUNT		3

/* ResponseCode definitions */
#define MPI3MR_RI_MASK_RESPCODE		(0x000000FF)
#define MPI3MR_RSP_TM_COMPLETE		0x00
#define MPI3MR_RSP_INVALID_FRAME	0x02
#define MPI3MR_RSP_TM_NOT_SUPPORTED	0x04
#define MPI3MR_RSP_TM_FAILED		0x05
#define MPI3MR_RSP_TM_SUCCEEDED		0x08
#define MPI3MR_RSP_TM_INVALID_LUN	0x09
#define MPI3MR_RSP_TM_OVERLAPPED_TAG	0x0A
#define MPI3MR_RSP_IO_QUEUED_ON_IOC \
			MPI3_SCSITASKMGMT_RSPCODE_IO_QUEUED_ON_IOC

/* SGE Flag definition */
#define MPI3MR_SGEFLAGS_SYSTEM_SIMPLE_END_OF_LIST \
	(MPI3_SGE_FLAGS_ELEMENT_TYPE_SIMPLE | MPI3_SGE_FLAGS_DLAS_SYSTEM | \
	MPI3_SGE_FLAGS_END_OF_LIST)

/* MSI Index from Reply Queue Index */
#define REPLY_QUEUE_IDX_TO_MSIX_IDX(qidx, offset)	(qidx + offset)

/* IOC State definitions */
enum mpi3mr_iocstate {
	MRIOC_STATE_READY = 1,
	MRIOC_STATE_RESET,
	MRIOC_STATE_FAULT,
	MRIOC_STATE_BECOMING_READY,
	MRIOC_STATE_RESET_REQUESTED,
	MRIOC_STATE_UNRECOVERABLE,
};

/* Reset reason code definitions*/
enum mpi3mr_reset_reason {
	MPI3MR_RESET_FROM_BRINGUP = 1,
	MPI3MR_RESET_FROM_FAULT_WATCH = 2,
	MPI3MR_RESET_FROM_IOCTL = 3,
	MPI3MR_RESET_FROM_EH_HOS = 4,
	MPI3MR_RESET_FROM_TM_TIMEOUT = 5,
	MPI3MR_RESET_FROM_IOCTL_TIMEOUT = 6,
	MPI3MR_RESET_FROM_MUR_FAILURE = 7,
	MPI3MR_RESET_FROM_CTLR_CLEANUP = 8,
	MPI3MR_RESET_FROM_CIACTIV_FAULT = 9,
	MPI3MR_RESET_FROM_PE_TIMEOUT = 10,
	MPI3MR_RESET_FROM_TSU_TIMEOUT = 11,
	MPI3MR_RESET_FROM_DELREQQ_TIMEOUT = 12,
	MPI3MR_RESET_FROM_DELREPQ_TIMEOUT = 13,
	MPI3MR_RESET_FROM_CREATEREPQ_TIMEOUT = 14,
	MPI3MR_RESET_FROM_CREATEREQQ_TIMEOUT = 15,
	MPI3MR_RESET_FROM_IOCFACTS_TIMEOUT = 16,
	MPI3MR_RESET_FROM_IOCINIT_TIMEOUT = 17,
	MPI3MR_RESET_FROM_EVTNOTIFY_TIMEOUT = 18,
	MPI3MR_RESET_FROM_EVTACK_TIMEOUT = 19,
	MPI3MR_RESET_FROM_CIACTVRST_TIMER = 20,
	MPI3MR_RESET_FROM_GETPKGVER_TIMEOUT = 21,
};

/**
 * struct mpi3mr_compimg_ver - replica of component image
 * version defined in mpi30_image.h in host endianness
 *
 */
struct mpi3mr_compimg_ver {
	u16 build_num;
	u16 cust_id;
	u8 ph_minor;
	u8 ph_major;
	u8 gen_minor;
	u8 gen_major;
};

/**
 * struct mpi3mr_ioc_facs - replica of component image version
 * defined in mpi30_ioc.h in host endianness
 *
 */
struct mpi3mr_ioc_facts {
	u32 ioc_capabilities;
	struct mpi3mr_compimg_ver fw_ver;
	u32 mpi_version;
	u16 max_reqs;
	u16 product_id;
	u16 op_req_sz;
	u16 reply_sz;
	u16 exceptions;
	u16 max_perids;
	u16 max_pds;
	u16 max_sasexpanders;
	u16 max_sasinitiators;
	u16 max_enclosures;
	u16 max_pcieswitches;
	u16 max_nvme;
	u16 max_vds;
	u16 max_hpds;
	u16 max_advhpds;
	u16 max_raidpds;
	u16 min_devhandle;
	u16 max_devhandle;
	u16 max_op_req_q;
	u16 max_op_reply_q;
	u16 shutdown_timeout;
	u8 ioc_num;
	u8 who_init;
	u16 max_msix_vectors;
	u8 personality;
	u8 dma_mask;
	u8 protocol_flags;
	u8 sge_mod_mask;
	u8 sge_mod_value;
	u8 sge_mod_shift;
};

/**
 * struct segments - memory descriptor structure to store
 * virtual and dma addresses for operational queue segments.
 *
 * @segment: virtual address
 * @segment_dma: dma address
 */
struct segments {
	void *segment;
	dma_addr_t segment_dma;
};

/**
 * struct op_req_qinfo -  Operational Request Queue Information
 *
 * @ci: consumer index
 * @pi: producer index
 * @num_request: Maximum number of entries in the queue
 * @qid: Queue Id starting from 1
 * @reply_qid: Associated reply queue Id
 * @num_segments: Number of discontiguous memory segments
 * @segment_qd: Depth of each segments
 * @q_lock: Concurrent queue access lock
 * @q_segments: Segment descriptor pointer
 * @q_segment_list: Segment list base virtual address
 * @q_segment_list_dma: Segment list base DMA address
 */
struct op_req_qinfo {
	u16 ci;
	u16 pi;
	u16 num_requests;
	u16 qid;
	u16 reply_qid;
	u16 num_segments;
	u16 segment_qd;
	spinlock_t q_lock;
	struct segments *q_segments;
	void *q_segment_list;
	dma_addr_t q_segment_list_dma;
};

/**
 * struct op_reply_qinfo -  Operational Reply Queue Information
 *
 * @ci: consumer index
 * @qid: Queue Id starting from 1
 * @num_replies: Maximum number of entries in the queue
 * @num_segments: Number of discontiguous memory segments
 * @segment_qd: Depth of each segments
 * @q_segments: Segment descriptor pointer
 * @q_segment_list: Segment list base virtual address
 * @q_segment_list_dma: Segment list base DMA address
 * @ephase: Expected phased identifier for the reply queue
 */
struct op_reply_qinfo {
	u16 ci;
	u16 qid;
	u16 num_replies;
	u16 num_segments;
	u16 segment_qd;
	struct segments *q_segments;
	void *q_segment_list;
	dma_addr_t q_segment_list_dma;
	u8 ephase;
};

/**
 * struct mpi3mr_intr_info -  Interrupt cookie information
 *
 * @mrioc: Adapter instance reference
 * @msix_index: MSIx index
 * @op_reply_q: Associated operational reply queue
 * @name: Dev name for the irq claiming device
 */
struct mpi3mr_intr_info {
	struct mpi3mr_ioc *mrioc;
	u16 msix_index;
	struct op_reply_qinfo *op_reply_q;
	char name[MPI3MR_NAME_LENGTH];
};

/**
 * struct mpi3mr_stgt_priv_data - SCSI target private structure
 *
 * @starget: Scsi_target pointer
 * @dev_handle: FW device handle
 * @perst_id: FW assigned Persistent ID
 * @num_luns: Number of Logical Units
 * @block_io: I/O blocked to the device or not
 * @dev_removed: Device removed in the Firmware
 * @dev_removedelay: Device is waiting to be removed in FW
 * @dev_type: Device type
 * @tgt_dev: Internal target device pointer
 */
struct mpi3mr_stgt_priv_data {
	struct scsi_target *starget;
	u16 dev_handle;
	u16 perst_id;
	u32 num_luns;
	atomic_t block_io;
	u8 dev_removed;
	u8 dev_removedelay;
	u8 dev_type;
	struct mpi3mr_tgt_dev *tgt_dev;
};

/**
 * struct mpi3mr_stgt_priv_data - SCSI device private structure
 *
 * @tgt_priv_data: Scsi_target private data pointer
 * @lun_id: LUN ID of the device
 * @ncq_prio_enable: NCQ priority enable for SATA device
 */
struct mpi3mr_sdev_priv_data {
	struct mpi3mr_stgt_priv_data *tgt_priv_data;
	u32 lun_id;
	u8 ncq_prio_enable;
};

/**
 * struct mpi3mr_drv_cmd - Internal command tracker
 *
 * @mutex: Command mutex
 * @done: Completeor for wakeup
 * @reply: Firmware reply for internal commands
 * @sensebuf: Sensebuf for SCSI IO commands
 * @state: Command State
 * @dev_handle: Firmware handle for device specific commands
 * @ioc_status: IOC status from the firmware
 * @ioc_loginfo:IOC log info from the firmware
 * @is_waiting: Is the command issued in block mode
 * @retry_count: Retry count for retriable commands
 * @host_tag: Host tag used by the command
 * @callback: Callback for non blocking commands
 */
struct mpi3mr_drv_cmd {
	struct mutex mutex;
	struct completion done;
	void *reply;
	u8 *sensebuf;
	u16 state;
	u16 dev_handle;
	u16 ioc_status;
	u32 ioc_loginfo;
	u8 is_waiting;
	u8 retry_count;
	u16 host_tag;

	void (*callback)(struct mpi3mr_ioc *mrioc,
		struct mpi3mr_drv_cmd *drv_cmd);
};


/**
 * struct chain_element - memory descriptor structure to store
 * virtual and dma addresses for chain elements.
 *
 * @addr: virtual address
 * @dma_addr: dma address
 */
struct chain_element {
	void *addr;
	dma_addr_t dma_addr;
};

/**
 * struct scmd_priv - SCSI command private data
 *
 * @host_tag: Host tag specific to operational queue
 * @in_lld_scope: Command in LLD scope or not
 * @scmd: SCSI Command pointer
 * @req_q_idx: Operational request queue index
 * @chain_idx: Chain frame index
 * @mpi3mr_scsiio_req: MPI SCSI IO request
 */
struct scmd_priv {
	u16 host_tag;
	u8 in_lld_scope;
	struct scsi_cmnd *scmd;
	u16 req_q_idx;
	int chain_idx;
	u8 mpi3mr_scsiio_req[MPI3MR_ADMIN_REQ_FRAME_SZ];
};

/**
 * struct mpi3mr_ioc - Adapter anchor structure stored in shost
 * private data
 *
 * @list: List pointer
 * @pdev: PCI device pointer
 * @shost: Scsi_Host pointer
 * @id: Controller ID
 * @cpu_count: Number of online CPUs
 * @name: Controller ASCII name
 * @driver_name: Driver ASCII name
 * @sysif_regs: System interface registers virtual address
 * @sysif_regs_phys: System interface registers physical address
 * @bars: PCI BARS
 * @dma_mask: DMA mask
 * @msix_count: Number of MSIX vectors used
 * @intr_enabled: Is interrupts enabled
 * @num_admin_req: Number of admin requests
 * @admin_req_q_sz: Admin request queue size
 * @admin_req_pi: Admin request queue producer index
 * @admin_req_ci: Admin request queue consumer index
 * @admin_req_base: Admin request queue base virtual address
 * @admin_req_dma: Admin request queue base dma address
 * @admin_req_lock: Admin queue access lock
 * @num_admin_replies: Number of admin replies
 * @admin_reply_q_sz: Admin reply queue size
 * @admin_reply_ci: Admin reply queue consumer index
 * @admin_reply_ephase:Admin reply queue expected phase
 * @admin_reply_base: Admin reply queue base virtual address
 * @admin_reply_dma: Admin reply queue base dma address
 * @ready_timeout: Controller ready timeout
 * @intr_info: Interrupt cookie pointer
 * @intr_info_count: Number of interrupt cookies
 * @num_queues: Number of operational queues
 * @num_op_req_q: Number of operational request queues
 * @req_qinfo: Operational request queue info pointer
 * @num_op_reply_q: Number of operational reply queues
 * @op_reply_qinfo: Operational reply queue info pointer
 * @init_cmds: Command tracker for initialization commands
 * @facts: Cached IOC facts data
 * @op_reply_desc_sz: Operational reply descriptor size
 * @num_reply_bufs: Number of reply buffers allocated
 * @reply_buf_pool: Reply buffer pool
 * @reply_buf: Reply buffer base virtual address
 * @reply_buf_dma: Reply buffer DMA address
 * @reply_buf_dma_max_address: Reply DMA address max limit
 * @reply_free_qsz: Reply free queue size
 * @reply_free_q_pool: Reply free queue pool
 * @reply_free_q: Reply free queue base virtual address
 * @reply_free_q_dma: Reply free queue base DMA address
 * @reply_free_queue_lock: Reply free queue lock
 * @reply_free_queue_host_index: Reply free queue host index
 * @num_sense_bufs: Number of sense buffers
 * @sense_buf_pool: Sense buffer pool
 * @sense_buf: Sense buffer base virtual address
 * @sense_buf_dma: Sense buffer base DMA address
 * @sense_buf_q_sz: Sense buffer queue size
 * @sense_buf_q_pool: Sense buffer queue pool
 * @sense_buf_q: Sense buffer queue virtual address
 * @sense_buf_q_dma: Sense buffer queue DMA address
 * @sbq_lock: Sense buffer queue lock
 * @sbq_host_index: Sense buffer queuehost index
 * @is_driver_loading: Is driver still loading
 * @scan_started: Async scan started
 * @scan_failed: Asycn scan failed
 * @stop_drv_processing: Stop all command processing
 * @max_host_ios: Maximum host I/O count
 * @chain_buf_count: Chain buffer count
 * @chain_buf_pool: Chain buffer pool
 * @chain_sgl_list: Chain SGL list
 * @chain_bitmap_sz: Chain buffer allocator bitmap size
 * @chain_bitmap: Chain buffer allocator bitmap
 * @chain_buf_lock: Chain buffer list lock
 * @reset_in_progress: Reset in progress flag
 * @unrecoverable: Controller unrecoverable flag
 * @logging_level: Controller debug logging level
 * @current_event: Firmware event currently in process
 * @driver_info: Driver, Kernel, OS information to firmware
 * @change_count: Topology change count
 * @op_reply_q_offset: Operational reply queue offset with MSIx
 */
struct mpi3mr_ioc {
	struct list_head list;
	struct pci_dev *pdev;
	struct Scsi_Host *shost;
	u8 id;
	int cpu_count;
	bool enable_segqueue;

	char name[MPI3MR_NAME_LENGTH];
	char driver_name[MPI3MR_NAME_LENGTH];

	Mpi3SysIfRegs_t __iomem *sysif_regs;
	resource_size_t sysif_regs_phys;
	int bars;
	u64 dma_mask;

	u16 msix_count;
	u8 intr_enabled;

	u16 num_admin_req;
	u32 admin_req_q_sz;
	u16 admin_req_pi;
	u16 admin_req_ci;
	void *admin_req_base;
	dma_addr_t admin_req_dma;
	spinlock_t admin_req_lock;

	u16 num_admin_replies;
	u32 admin_reply_q_sz;
	u16 admin_reply_ci;
	u8 admin_reply_ephase;
	void *admin_reply_base;
	dma_addr_t admin_reply_dma;

	u32 ready_timeout;

	struct mpi3mr_intr_info *intr_info;
	u16 intr_info_count;

	u16 num_queues;
	u16 num_op_req_q;
	struct op_req_qinfo *req_qinfo;

	u16 num_op_reply_q;
	struct op_reply_qinfo *op_reply_qinfo;

	struct mpi3mr_drv_cmd init_cmds;
	struct mpi3mr_ioc_facts facts;
	u16 op_reply_desc_sz;

	u32 num_reply_bufs;
	struct dma_pool *reply_buf_pool;
	u8 *reply_buf;
	dma_addr_t reply_buf_dma;
	dma_addr_t reply_buf_dma_max_address;

	u16 reply_free_qsz;
	struct dma_pool *reply_free_q_pool;
	U64 *reply_free_q;
	dma_addr_t reply_free_q_dma;
	spinlock_t reply_free_queue_lock;
	u32 reply_free_queue_host_index;

	u32 num_sense_bufs;
	struct dma_pool *sense_buf_pool;
	u8 *sense_buf;
	dma_addr_t sense_buf_dma;

	u16 sense_buf_q_sz;
	struct dma_pool *sense_buf_q_pool;
	U64 *sense_buf_q;
	dma_addr_t sense_buf_q_dma;
	spinlock_t sbq_lock;
	u32 sbq_host_index;

	u8 is_driver_loading;
	u8 scan_started;
	u16 scan_failed;
	u8 stop_drv_processing;

	u16 max_host_ios;

	u32 chain_buf_count;
	struct dma_pool *chain_buf_pool;
	struct chain_element *chain_sgl_list;
	u16  chain_bitmap_sz;
	void *chain_bitmap;
	spinlock_t chain_buf_lock;

	u8 reset_in_progress;
	u8 unrecoverable;

	int logging_level;

	struct mpi3mr_fwevt *current_event;
	Mpi3DriverInfoLayout_t driver_info;
	u16 change_count;
	u16 op_reply_q_offset;
};

int mpi3mr_setup_resources(struct mpi3mr_ioc *mrioc);
void mpi3mr_cleanup_resources(struct mpi3mr_ioc *mrioc);
int mpi3mr_init_ioc(struct mpi3mr_ioc *mrioc);
void mpi3mr_cleanup_ioc(struct mpi3mr_ioc *mrioc);
int mpi3mr_issue_port_enable(struct mpi3mr_ioc *mrioc, u8 async);
int mpi3mr_admin_request_post(struct mpi3mr_ioc *mrioc, void *admin_req,
u16 admin_req_sz, u8 ignore_reset);
int mpi3mr_op_request_post(struct mpi3mr_ioc *mrioc,
			   struct op_req_qinfo *opreqq, u8 *req);
void mpi3mr_add_sg_single(void *paddr, u8 flags, u32 length,
			  dma_addr_t dma_addr);
void mpi3mr_build_zero_len_sge(void *paddr);
void *mpi3mr_get_sensebuf_virt_addr(struct mpi3mr_ioc *mrioc,
				     dma_addr_t phys_addr);
void *mpi3mr_get_reply_virt_addr(struct mpi3mr_ioc *mrioc,
				     dma_addr_t phys_addr);
void mpi3mr_repost_sense_buf(struct mpi3mr_ioc *mrioc,
				     u64 sense_buf_dma);

void mpi3mr_process_op_reply_desc(struct mpi3mr_ioc *mrioc,
				  Mpi3DefaultReplyDescriptor_t *reply_desc,
				  u64 *reply_dma, u16 qidx);
void mpi3mr_start_watchdog(struct mpi3mr_ioc *mrioc);
void mpi3mr_stop_watchdog(struct mpi3mr_ioc *mrioc);

int mpi3mr_soft_reset_handler(struct mpi3mr_ioc *mrioc,
			      u32 reset_reason, u8 snapdump);
void mpi3mr_ioc_disable_intr(struct mpi3mr_ioc *mrioc);
void mpi3mr_ioc_enable_intr(struct mpi3mr_ioc *mrioc);

enum mpi3mr_iocstate mpi3mr_get_iocstate(struct mpi3mr_ioc *mrioc);

#endif /*MPI3MR_H_INCLUDED*/
