// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for Broadcom MPI3 Storage Controllers
 *
 * Copyright (C) 2017-2020 Broadcom Inc.
 *  (mailto: mpi3mr-linuxdrv.pdl@broadcom.com)
 *
 */

#include "mpi3mr.h"

#if defined(writeq) && defined(CONFIG_64BIT)
static inline void mpi3mr_writeq(__u64 b, volatile void __iomem *addr)
{
	writeq(b, addr);
}
#else
static inline void mpi3mr_writeq(__u64 b, volatile void __iomem *addr)
{
	__u64 data_out = b;

	writel((u32)(data_out), addr);
	writel((u32)(data_out >> 32), (addr + 4));
}
#endif

static void mpi3mr_sync_irqs(struct mpi3mr_ioc *mrioc)
{
	u16 i, max_vectors;

	max_vectors = mrioc->intr_info_count;

	for (i = 0; i < max_vectors; i++)
		synchronize_irq(pci_irq_vector(mrioc->pdev, i));
}

void mpi3mr_ioc_disable_intr(struct mpi3mr_ioc *mrioc)
{
	mrioc->intr_enabled = 0;
	mpi3mr_sync_irqs(mrioc);
}

void mpi3mr_ioc_enable_intr(struct mpi3mr_ioc *mrioc)
{
	mrioc->intr_enabled = 1;
}

static void mpi3mr_cleanup_isr(struct mpi3mr_ioc *mrioc)
{
	u16 i;

	mpi3mr_ioc_disable_intr(mrioc);

	if (!mrioc->intr_info)
		return;

	for (i = 0; i < mrioc->intr_info_count; i++)
		free_irq(pci_irq_vector(mrioc->pdev, i),
		    (mrioc->intr_info + i));

	kfree(mrioc->intr_info);
	mrioc->intr_info = NULL;
	mrioc->intr_info_count = 0;
	pci_free_irq_vectors(mrioc->pdev);
}

void mpi3mr_add_sg_single(void *paddr, u8 flags, u32 length,
	dma_addr_t dma_addr)
{
	Mpi3SGESimple_t *sgel = paddr;

	sgel->Flags = flags;
	sgel->Length = cpu_to_le32(length);
	sgel->Address = cpu_to_le64(dma_addr);
}

void mpi3mr_build_zero_len_sge(void *paddr)
{
	u8 sgl_flags = MPI3MR_SGEFLAGS_SYSTEM_SIMPLE_END_OF_LIST;

	mpi3mr_add_sg_single(paddr, sgl_flags, 0, -1);

}
void *mpi3mr_get_reply_virt_addr(struct mpi3mr_ioc *mrioc,
	dma_addr_t phys_addr)
{
	if (!phys_addr)
		return NULL;

	if ((phys_addr < mrioc->reply_buf_dma) ||
	    (phys_addr > mrioc->reply_buf_dma_max_address))
		return NULL;

	return mrioc->reply_buf + (phys_addr - mrioc->reply_buf_dma);
}

void *mpi3mr_get_sensebuf_virt_addr(struct mpi3mr_ioc *mrioc,
	dma_addr_t phys_addr)
{
	if (!phys_addr)
		return NULL;

	return mrioc->sense_buf + (phys_addr - mrioc->sense_buf_dma);
}

static void mpi3mr_repost_reply_buf(struct mpi3mr_ioc *mrioc,
	u64 reply_dma)
{
	u32 old_idx = 0;

	spin_lock(&mrioc->reply_free_queue_lock);
	old_idx  =  mrioc->reply_free_queue_host_index;
	mrioc->reply_free_queue_host_index = (
	    (mrioc->reply_free_queue_host_index ==
	    (mrioc->reply_free_qsz - 1)) ? 0 :
	    (mrioc->reply_free_queue_host_index + 1));
	mrioc->reply_free_q[old_idx] = cpu_to_le64(reply_dma);
	writel(mrioc->reply_free_queue_host_index,
	    &mrioc->sysif_regs->ReplyFreeHostIndex);
	spin_unlock(&mrioc->reply_free_queue_lock);
}

void mpi3mr_repost_sense_buf(struct mpi3mr_ioc *mrioc,
	u64 sense_buf_dma)
{
	u32 old_idx = 0;

	spin_lock(&mrioc->sbq_lock);
	old_idx  =  mrioc->sbq_host_index;
	mrioc->sbq_host_index = ((mrioc->sbq_host_index ==
	    (mrioc->sense_buf_q_sz - 1)) ? 0 :
	    (mrioc->sbq_host_index + 1));
	mrioc->sense_buf_q[old_idx] = cpu_to_le64(sense_buf_dma);
	writel(mrioc->sbq_host_index,
	    &mrioc->sysif_regs->SenseBufferFreeHostIndex);
	spin_unlock(&mrioc->sbq_lock);
}

static void mpi3mr_handle_events(struct mpi3mr_ioc *mrioc,
	Mpi3DefaultReply_t *def_reply)
{
	Mpi3EventNotificationReply_t *event_reply =
	    (Mpi3EventNotificationReply_t *)def_reply;

	mrioc->change_count = le16_to_cpu(event_reply->IOCChangeCount);
}

static struct mpi3mr_drv_cmd *
mpi3mr_get_drv_cmd(struct mpi3mr_ioc *mrioc, u16 host_tag,
	Mpi3DefaultReply_t *def_reply)
{
	switch (host_tag) {
	case MPI3MR_HOSTTAG_INITCMDS:
		return &mrioc->init_cmds;
	case MPI3MR_HOSTTAG_INVALID:
		if (def_reply && def_reply->Function ==
		    MPI3_FUNCTION_EVENT_NOTIFICATION)
			mpi3mr_handle_events(mrioc, def_reply);
		return NULL;
	default:
		break;
	}

	return NULL;
}

static void mpi3mr_process_admin_reply_desc(struct mpi3mr_ioc *mrioc,
	Mpi3DefaultReplyDescriptor_t *reply_desc, u64 *reply_dma)
{
	u16 reply_desc_type, host_tag = 0;
	u16 ioc_status = MPI3_IOCSTATUS_SUCCESS;
	u32 ioc_loginfo = 0;
	Mpi3StatusReplyDescriptor_t *status_desc;
	Mpi3AddressReplyDescriptor_t *addr_desc;
	Mpi3SuccessReplyDescriptor_t *success_desc;
	Mpi3DefaultReply_t *def_reply = NULL;
	struct mpi3mr_drv_cmd *cmdptr = NULL;
	Mpi3SCSIIOReply_t *scsi_reply;
	u8 *sense_buf = NULL;

	*reply_dma = 0;
	reply_desc_type = le16_to_cpu(reply_desc->ReplyFlags) &
	    MPI3_REPLY_DESCRIPT_FLAGS_TYPE_MASK;
	switch (reply_desc_type) {
	case MPI3_REPLY_DESCRIPT_FLAGS_TYPE_STATUS:
		status_desc = (Mpi3StatusReplyDescriptor_t *)reply_desc;
		host_tag = le16_to_cpu(status_desc->HostTag);
		ioc_status = le16_to_cpu(status_desc->IOCStatus);
		if (ioc_status &
		    MPI3_REPLY_DESCRIPT_STATUS_IOCSTATUS_LOGINFOAVAIL)
			ioc_loginfo = le32_to_cpu(status_desc->IOCLogInfo);
		ioc_status &= MPI3_REPLY_DESCRIPT_STATUS_IOCSTATUS_STATUS_MASK;
		break;
	case MPI3_REPLY_DESCRIPT_FLAGS_TYPE_ADDRESS_REPLY:
		addr_desc = (Mpi3AddressReplyDescriptor_t *)reply_desc;
		*reply_dma = le64_to_cpu(addr_desc->ReplyFrameAddress);
		def_reply = mpi3mr_get_reply_virt_addr(mrioc, *reply_dma);
		if (!def_reply)
			goto out;
		host_tag = le16_to_cpu(def_reply->HostTag);
		ioc_status = le16_to_cpu(def_reply->IOCStatus);
		if (ioc_status &
		    MPI3_REPLY_DESCRIPT_STATUS_IOCSTATUS_LOGINFOAVAIL)
			ioc_loginfo = le32_to_cpu(def_reply->IOCLogInfo);
		ioc_status &= MPI3_REPLY_DESCRIPT_STATUS_IOCSTATUS_STATUS_MASK;
		if (def_reply->Function == MPI3_FUNCTION_SCSI_IO) {
			scsi_reply = (Mpi3SCSIIOReply_t *)def_reply;
			sense_buf = mpi3mr_get_sensebuf_virt_addr(mrioc,
			    le64_to_cpu(scsi_reply->SenseDataBufferAddress));
		}
		break;
	case MPI3_REPLY_DESCRIPT_FLAGS_TYPE_SUCCESS:
		success_desc = (Mpi3SuccessReplyDescriptor_t *)reply_desc;
		host_tag = le16_to_cpu(success_desc->HostTag);
		break;
	default:
		break;
	}

	cmdptr = mpi3mr_get_drv_cmd(mrioc, host_tag, def_reply);
	if (cmdptr) {
		if (cmdptr->state & MPI3MR_CMD_PENDING) {
			cmdptr->state |= MPI3MR_CMD_COMPLETE;
			cmdptr->ioc_loginfo = ioc_loginfo;
			cmdptr->ioc_status = ioc_status;
			cmdptr->state &= ~MPI3MR_CMD_PENDING;
			if (def_reply) {
				cmdptr->state |= MPI3MR_CMD_REPLY_VALID;
				memcpy((u8 *)cmdptr->reply, (u8 *)def_reply,
				    mrioc->facts.reply_sz);
			}
			if (cmdptr->is_waiting) {
				complete(&cmdptr->done);
				cmdptr->is_waiting = 0;
			} else if (cmdptr->callback)
				cmdptr->callback(mrioc, cmdptr);
		}
	}
out:
	if (sense_buf)
		mpi3mr_repost_sense_buf(mrioc,
		    le64_to_cpu(scsi_reply->SenseDataBufferAddress));
}

static int mpi3mr_process_admin_reply_q(struct mpi3mr_ioc *mrioc)
{
	u32 exp_phase = mrioc->admin_reply_ephase;
	u32 admin_reply_ci = mrioc->admin_reply_ci;
	u32 num_admin_replies = 0;
	u64 reply_dma = 0;
	Mpi3DefaultReplyDescriptor_t *reply_desc;

	reply_desc = (Mpi3DefaultReplyDescriptor_t *)mrioc->admin_reply_base +
	    admin_reply_ci;

	if ((le16_to_cpu(reply_desc->ReplyFlags) &
	    MPI3_REPLY_DESCRIPT_FLAGS_PHASE_MASK) != exp_phase)
		return 0;

	do {
		mrioc->admin_req_ci = le16_to_cpu(reply_desc->RequestQueueCI);
		mpi3mr_process_admin_reply_desc(mrioc, reply_desc, &reply_dma);
		if (reply_dma)
			mpi3mr_repost_reply_buf(mrioc, reply_dma);
		num_admin_replies++;
		if (++admin_reply_ci == mrioc->num_admin_replies) {
			admin_reply_ci = 0;
			exp_phase ^= 1;
		}
		reply_desc =
		    (Mpi3DefaultReplyDescriptor_t *)mrioc->admin_reply_base +
		    admin_reply_ci;
		if ((le16_to_cpu(reply_desc->ReplyFlags) &
		    MPI3_REPLY_DESCRIPT_FLAGS_PHASE_MASK) != exp_phase)
			break;
	} while (1);

	writel(admin_reply_ci, &mrioc->sysif_regs->AdminReplyQueueCI);
	mrioc->admin_reply_ci = admin_reply_ci;
	mrioc->admin_reply_ephase = exp_phase;

	return num_admin_replies;
}

static irqreturn_t mpi3mr_isr_primary(int irq, void *privdata)
{
	struct mpi3mr_intr_info *intr_info = privdata;
	struct mpi3mr_ioc *mrioc;
	u16 midx;
	u32 num_admin_replies = 0;

	if (!intr_info)
		return IRQ_NONE;

	mrioc = intr_info->mrioc;

	if (!mrioc->intr_enabled)
		return IRQ_NONE;

	midx = intr_info->msix_index;

	if (!midx)
		num_admin_replies = mpi3mr_process_admin_reply_q(mrioc);

	if (num_admin_replies)
		return IRQ_HANDLED;
	else
		return IRQ_NONE;
}

static irqreturn_t mpi3mr_isr(int irq, void *privdata)
{
	struct mpi3mr_intr_info *intr_info = privdata;
	struct mpi3mr_ioc *mrioc;
	u16 midx;
	int ret;

	if (!intr_info)
		return IRQ_NONE;

	mrioc = intr_info->mrioc;
	midx = intr_info->msix_index;
	/* Call primary ISR routine */
	ret = mpi3mr_isr_primary(irq, privdata);

	return ret;
}

/**
 * mpi3mr_isr_poll - Reply queue polling routine
 * @irq: IRQ
 * @privdata: Interrupt info
 *
 * poll for pending I/O completions in a loop until pending I/Os
 * present or controller queue depth I/Os are processed.
 *
 * Return: IRQ_NONE or IRQ_HANDLED
 */
static irqreturn_t mpi3mr_isr_poll(int irq, void *privdata)
{
	return IRQ_HANDLED;
}

/**
 * mpi3mr_request_irq - Request IRQ and register ISR
 * @mrioc: Adapter instance reference
 * @index: IRQ vector index
 *
 * Request threaded ISR with primary ISR and secondary
 *
 * Return: 0 on success and non zero on failures.
 */
static inline int mpi3mr_request_irq(struct mpi3mr_ioc *mrioc, u16 index)
{
	struct pci_dev *pdev = mrioc->pdev;
	struct mpi3mr_intr_info *intr_info = mrioc->intr_info + index;
	int retval = 0;

	intr_info->mrioc = mrioc;
	intr_info->msix_index = index;
	intr_info->op_reply_q = NULL;

	snprintf(intr_info->name, MPI3MR_NAME_LENGTH, "%s%d-msix%d",
	    mrioc->driver_name, mrioc->id, index);

	retval = request_threaded_irq(pci_irq_vector(pdev, index), mpi3mr_isr,
	    mpi3mr_isr_poll, IRQF_SHARED, intr_info->name, intr_info);
	if (retval) {
		ioc_err(mrioc, "%s: Unable to allocate interrupt %d!\n",
		    intr_info->name, pci_irq_vector(pdev, index));
		return retval;
	}

	return retval;
}

/**
 * mpi3mr_setup_isr - Setup ISR for the controller
 * @mrioc: Adapter instance reference
 * @setup_one: Request one IRQ or more
 *
 * Allocate IRQ vectors and call mpi3mr_request_irq to setup ISR
 *
 * Return: 0 on success and non zero on failures.
 */
static int mpi3mr_setup_isr(struct mpi3mr_ioc *mrioc, u8 setup_one)
{
	unsigned int irq_flags = PCI_IRQ_MSIX;
	u16 max_vectors = 0, i;
	int retval = 0;
	struct irq_affinity desc = { .pre_vectors =  1};


	mpi3mr_cleanup_isr(mrioc);

	if (setup_one || reset_devices)
		max_vectors = 1;
	else {
		max_vectors =
		    min_t(int, mrioc->cpu_count + 1, mrioc->msix_count);

		ioc_info(mrioc,
		    "MSI-X vectors supported: %d, no of cores: %d,",
		    mrioc->msix_count, mrioc->cpu_count);
		ioc_info(mrioc,
		    "MSI-x vectors requested: %d\n", max_vectors);
	}

	irq_flags |= PCI_IRQ_AFFINITY | PCI_IRQ_ALL_TYPES;

	i = pci_alloc_irq_vectors_affinity(mrioc->pdev,
	    1, max_vectors, irq_flags, &desc);
	if (i <= 0) {
		ioc_err(mrioc, "Cannot alloc irq vectors\n");
		goto out_failed;
	}
	if (i != max_vectors) {
		ioc_info(mrioc,
		    "allocated vectors (%d) are less than configured (%d)\n",
		    i, max_vectors);

		max_vectors = i;
	}
	mrioc->intr_info = kzalloc(sizeof(struct mpi3mr_intr_info)*max_vectors,
	    GFP_KERNEL);
	if (!mrioc->intr_info) {
		retval = -1;
		pci_free_irq_vectors(mrioc->pdev);
		goto out_failed;
	}
	for (i = 0; i < max_vectors; i++) {
		retval = mpi3mr_request_irq(mrioc, i);
		if (retval) {
			mrioc->intr_info_count = i;
			goto out_failed;
		}
	}
	mrioc->intr_info_count = max_vectors;
	mpi3mr_ioc_enable_intr(mrioc);
	return retval;
out_failed:
	mpi3mr_cleanup_isr(mrioc);

	return retval;
}

static const struct {
	enum mpi3mr_iocstate value;
	char *name;
} mrioc_states[] = {
	{ MRIOC_STATE_READY, "ready" },
	{ MRIOC_STATE_FAULT, "fault" },
	{ MRIOC_STATE_RESET, "reset" },
	{ MRIOC_STATE_BECOMING_READY, "becoming ready" },
	{ MRIOC_STATE_RESET_REQUESTED, "reset requested" },
	{ MRIOC_STATE_UNRECOVERABLE, "unrecoverable error" },
};

static const char *mpi3mr_iocstate_name(enum mpi3mr_iocstate mrioc_state)
{
	int i;
	char *name = NULL;

	for (i = 0; i < ARRAY_SIZE(mrioc_states); i++) {
		if (mrioc_states[i].value == mrioc_state) {
			name = mrioc_states[i].name;
			break;
		}
	}
	return name;
}


/**
 * mpi3mr_print_fault_info - Display fault information
 * @mrioc: Adapter instance reference
 *
 * Display the controller fault information if there is a
 * controller fault.
 *
 * Return: Nothing.
 */
static void mpi3mr_print_fault_info(struct mpi3mr_ioc *mrioc)
{
	u32 ioc_status, code, code1, code2, code3;

	ioc_status = readl(&mrioc->sysif_regs->IOCStatus);

	if (ioc_status & MPI3_SYSIF_IOC_STATUS_FAULT) {
		code = readl(&mrioc->sysif_regs->Fault);
		code1 = readl(&mrioc->sysif_regs->FaultInfo[0]);
		code2 = readl(&mrioc->sysif_regs->FaultInfo[1]);
		code3 = readl(&mrioc->sysif_regs->FaultInfo[2]);

		ioc_info(mrioc,
		    "fault code(0x%08X): Additional code: (0x%08X:0x%08X:0x%08X)\n",
		    code, code1, code2, code3);
	}
}

/**
 * mpi3mr_get_iocstate - Get IOC State
 * @mrioc: Adapter instance reference
 *
 * Return a proper IOC state enum based on the IOC status and
 * IOC configuration and unrcoverable state of the controller.
 *
 * Return: Current IOC state.
 */
enum mpi3mr_iocstate mpi3mr_get_iocstate(struct mpi3mr_ioc *mrioc)
{
	u32 ioc_status, ioc_config;
	u8 ready, enabled;

	ioc_status = readl(&mrioc->sysif_regs->IOCStatus);
	ioc_config = readl(&mrioc->sysif_regs->IOCConfiguration);

	if (mrioc->unrecoverable)
		return MRIOC_STATE_UNRECOVERABLE;
	if (ioc_status & MPI3_SYSIF_IOC_STATUS_FAULT)
		return MRIOC_STATE_FAULT;

	ready = (ioc_status & MPI3_SYSIF_IOC_STATUS_READY);
	enabled = (ioc_config & MPI3_SYSIF_IOC_CONFIG_ENABLE_IOC);

	if (ready && enabled)
		return MRIOC_STATE_READY;
	if ((!ready) && (!enabled))
		return MRIOC_STATE_RESET;
	if ((!ready) && (enabled))
		return MRIOC_STATE_BECOMING_READY;

	return MRIOC_STATE_RESET_REQUESTED;
}

/**
 * mpi3mr_clear_reset_history - Clear reset history
 * @mrioc: Adapter instance reference
 *
 * Write the reset history bit in IOC Status to clear the bit,
 * if it is already set.
 *
 * Return: Nothing.
 */
static inline void mpi3mr_clear_reset_history(struct mpi3mr_ioc *mrioc)
{
	u32 ioc_status;

	ioc_status = readl(&mrioc->sysif_regs->IOCStatus);
	if (ioc_status & MPI3_SYSIF_IOC_STATUS_RESET_HISTORY)
		writel(ioc_status, &mrioc->sysif_regs->IOCStatus);

}

/**
 * mpi3mr_issue_and_process_mur - Message Unit Reset handler
 * @mrioc: Adapter instance reference
 * @reset_reason: Reset reason code
 *
 * Issue Message Unit Reset to the controller and wait for it to
 * be complete.
 *
 * Return: 0 on success, -1 on failure.
 */
static int mpi3mr_issue_and_process_mur(struct mpi3mr_ioc *mrioc,
					u32 reset_reason)
{
	u32 ioc_config, timeout, ioc_status;
	int retval = -1;

	ioc_info(mrioc, "Issuing Message Unit Reset(MUR)\n");
	if (mrioc->unrecoverable) {
		ioc_info(mrioc, "IOC is unrecoverable MUR not issued\n");
		return retval;
	}
	mpi3mr_clear_reset_history(mrioc);
	writel(reset_reason, &mrioc->sysif_regs->Scratchpad[0]);
	ioc_config = readl(&mrioc->sysif_regs->IOCConfiguration);
	ioc_config &= ~MPI3_SYSIF_IOC_CONFIG_ENABLE_IOC;
	writel(ioc_config, &mrioc->sysif_regs->IOCConfiguration);

	timeout = mrioc->ready_timeout * 10;
	do {
		ioc_status = readl(&mrioc->sysif_regs->IOCStatus);
		if ((ioc_status & MPI3_SYSIF_IOC_STATUS_RESET_HISTORY)) {
			mpi3mr_clear_reset_history(mrioc);
			ioc_config =
			    readl(&mrioc->sysif_regs->IOCConfiguration);
			if (!((ioc_status & MPI3_SYSIF_IOC_STATUS_READY) ||
			    (ioc_status & MPI3_SYSIF_IOC_STATUS_FAULT) ||
			    (ioc_config & MPI3_SYSIF_IOC_CONFIG_ENABLE_IOC))) {
				retval = 0;
				break;
			}
		}
		msleep(100);
	} while (--timeout);

	ioc_status = readl(&mrioc->sysif_regs->IOCStatus);
	ioc_config = readl(&mrioc->sysif_regs->IOCConfiguration);

	ioc_info(mrioc, "Base IOC Sts/Config after %s MUR is (0x%x)/(0x%x)\n",
	    (!retval) ? "successful" : "failed", ioc_status, ioc_config);
	return retval;
}

/**
 * mpi3mr_bring_ioc_ready - Bring controller to ready state
 * @mrioc: Adapter instance reference
 *
 * Set Enable IOC bit in IOC configuration register and wait for
 * the controller to become ready.
 *
 * Return: 0 on success, -1 on failure.
 */
static int mpi3mr_bring_ioc_ready(struct mpi3mr_ioc *mrioc)
{
	u32 ioc_config, timeout;
	enum mpi3mr_iocstate current_state;

	ioc_config = readl(&mrioc->sysif_regs->IOCConfiguration);
	ioc_config |= MPI3_SYSIF_IOC_CONFIG_ENABLE_IOC;
	writel(ioc_config, &mrioc->sysif_regs->IOCConfiguration);

	timeout = mrioc->ready_timeout * 10;
	do {
		current_state = mpi3mr_get_iocstate(mrioc);
		if (current_state == MRIOC_STATE_READY)
			return 0;
		msleep(100);
	} while (--timeout);

	return -1;
}

/**
 * mpi3mr_set_diagsave - Set diag save bit for snapdump
 * @mrioc: Adapter reference
 *
 * Set diag save bit in IOC configuration register to enable
 * snapdump.
 *
 * Return: Nothing.
 */
static inline void mpi3mr_set_diagsave(struct mpi3mr_ioc *mrioc)
{
	u32 ioc_config;

	ioc_config = readl(&mrioc->sysif_regs->IOCConfiguration);
	ioc_config |= MPI3_SYSIF_IOC_CONFIG_DIAG_SAVE;
	writel(ioc_config, &mrioc->sysif_regs->IOCConfiguration);
}

/**
 * mpi3mr_issue_reset - Issue reset to the controller
 * @mrioc: Adapter reference
 * @reset_type: Reset type
 * @reset_reason: Reset reason code
 *
 * TBD
 *
 * Return: 0 on success, non-zero on failure.
 */
static int mpi3mr_issue_reset(struct mpi3mr_ioc *mrioc, u16 reset_type,
	u32 reset_reason)
{
	return 0;
}

/**
 * mpi3mr_admin_request_post - Post request to admin queue
 * @mrioc: Adapter reference
 * @admin_req: MPI3 request
 * @admin_req_sz: Request size
 * @ignore_reset: Ignore reset in process
 *
 * Post the MPI3 request into admin request queue and
 * inform the controller, if the queue is full return
 * appropriate error.
 *
 * Return: 0 on success, non-zero on failure.
 */
int mpi3mr_admin_request_post(struct mpi3mr_ioc *mrioc, void *admin_req,
	u16 admin_req_sz, u8 ignore_reset)
{
	u16 areq_pi = 0, areq_ci = 0, max_entries = 0;
	int retval = 0;
	unsigned long flags;
	u8 *areq_entry;


	if (mrioc->unrecoverable) {
		ioc_err(mrioc, "%s : Unrecoverable controller\n", __func__);
		return -EFAULT;
	}

	spin_lock_irqsave(&mrioc->admin_req_lock, flags);
	areq_pi = mrioc->admin_req_pi;
	areq_ci = mrioc->admin_req_ci;
	max_entries = mrioc->num_admin_req;
	if ((areq_ci == (areq_pi + 1)) || ((!areq_ci) &&
	    (areq_pi == (max_entries - 1)))) {
		ioc_err(mrioc, "AdminReqQ full condition detected\n");
		retval = -EAGAIN;
		goto out;
	}
	if (!ignore_reset && mrioc->reset_in_progress) {
		ioc_err(mrioc, "AdminReqQ submit reset in progress\n");
		retval = -EAGAIN;
		goto out;
	}
	areq_entry = (u8 *)mrioc->admin_req_base +
	    (areq_pi * MPI3MR_ADMIN_REQ_FRAME_SZ);
	memset(areq_entry, 0, MPI3MR_ADMIN_REQ_FRAME_SZ);
	memcpy(areq_entry, (u8 *)admin_req, admin_req_sz);

	if (++areq_pi == max_entries)
		areq_pi = 0;
	mrioc->admin_req_pi = areq_pi;

	writel(mrioc->admin_req_pi, &mrioc->sysif_regs->AdminRequestQueuePI);

out:
	spin_unlock_irqrestore(&mrioc->admin_req_lock, flags);

	return retval;
}


/**
 * mpi3mr_setup_admin_qpair - Setup admin queue pair
 * @mrioc: Adapter instance reference
 *
 * Allocate memory for admin queue pair if required and register
 * the admin queue with the controller.
 *
 * Return: 0 on success, non-zero on failures.
 */
static int mpi3mr_setup_admin_qpair(struct mpi3mr_ioc *mrioc)
{
	int retval = 0;
	u32 num_admin_entries = 0;

	mrioc->admin_req_q_sz = MPI3MR_ADMIN_REQ_Q_SIZE;
	mrioc->num_admin_req = mrioc->admin_req_q_sz /
	    MPI3MR_ADMIN_REQ_FRAME_SZ;
	mrioc->admin_req_ci = mrioc->admin_req_pi = 0;
	mrioc->admin_req_base = NULL;

	mrioc->admin_reply_q_sz = MPI3MR_ADMIN_REPLY_Q_SIZE;
	mrioc->num_admin_replies = mrioc->admin_reply_q_sz /
	    MPI3MR_ADMIN_REPLY_FRAME_SZ;
	mrioc->admin_reply_ci = 0;
	mrioc->admin_reply_ephase = 1;
	mrioc->admin_reply_base = NULL;

	if (!mrioc->admin_req_base) {
		mrioc->admin_req_base = dma_alloc_coherent(&mrioc->pdev->dev,
		    mrioc->admin_req_q_sz, &mrioc->admin_req_dma, GFP_KERNEL);

		if (!mrioc->admin_req_base) {
			retval = -1;
			goto out_failed;
		}

		mrioc->admin_reply_base = dma_alloc_coherent(&mrioc->pdev->dev,
		    mrioc->admin_reply_q_sz, &mrioc->admin_reply_dma,
		    GFP_KERNEL);

		if (!mrioc->admin_reply_base) {
			retval = -1;
			goto out_failed;
		}

	}

	num_admin_entries = (mrioc->num_admin_replies << 16) |
	    (mrioc->num_admin_req);
	writel(num_admin_entries, &mrioc->sysif_regs->AdminQueueNumEntries);
	mpi3mr_writeq(mrioc->admin_req_dma,
	    &mrioc->sysif_regs->AdminRequestQueueAddress);
	mpi3mr_writeq(mrioc->admin_reply_dma,
	    &mrioc->sysif_regs->AdminReplyQueueAddress);
	writel(mrioc->admin_req_pi, &mrioc->sysif_regs->AdminRequestQueuePI);
	writel(mrioc->admin_reply_ci, &mrioc->sysif_regs->AdminReplyQueueCI);
	return retval;

out_failed:

	if (mrioc->admin_reply_base) {
		dma_free_coherent(&mrioc->pdev->dev, mrioc->admin_reply_q_sz,
		    mrioc->admin_reply_base, mrioc->admin_reply_dma);
		mrioc->admin_reply_base = NULL;
	}
	if (mrioc->admin_req_base) {
		dma_free_coherent(&mrioc->pdev->dev, mrioc->admin_req_q_sz,
		    mrioc->admin_req_base, mrioc->admin_req_dma);
		mrioc->admin_req_base = NULL;
	}
	return retval;
}

/**
 * mpi3mr_issue_iocfacts - Send IOC Facts
 * @mrioc: Adapter instance reference
 *
 * Issue IOC Facts MPI request through admin queue and wait for
 * the completion of it or time out.
 *
 * Return: 0 on success, non-zero on failures.
 */
static int mpi3mr_issue_iocfacts(struct mpi3mr_ioc *mrioc,
	Mpi3IOCFactsData_t *facts_data)
{
	Mpi3IOCFactsRequest_t iocfacts_req;
	void *data = NULL;
	dma_addr_t data_dma;
	u32 data_len = sizeof(*facts_data);
	int retval = 0;
	u8 sgl_flags = MPI3MR_SGEFLAGS_SYSTEM_SIMPLE_END_OF_LIST;

	data = dma_alloc_coherent(&mrioc->pdev->dev, data_len, &data_dma,
	    GFP_KERNEL);

	if (!data) {
		retval = -1;
		goto out;
	}

	memset(&iocfacts_req, 0, sizeof(iocfacts_req));
	mutex_lock(&mrioc->init_cmds.mutex);
	if (mrioc->init_cmds.state & MPI3MR_CMD_PENDING) {
		retval = -1;
		ioc_err(mrioc, "Issue IOCFacts: Init command is in use\n");
		mutex_unlock(&mrioc->init_cmds.mutex);
		goto out;
	}
	mrioc->init_cmds.state = MPI3MR_CMD_PENDING;
	mrioc->init_cmds.is_waiting = 1;
	mrioc->init_cmds.callback = NULL;
	iocfacts_req.HostTag = cpu_to_le16(MPI3MR_HOSTTAG_INITCMDS);
	iocfacts_req.Function = MPI3_FUNCTION_IOC_FACTS;

	mpi3mr_add_sg_single(&iocfacts_req.SGL, sgl_flags, data_len,
	    data_dma);

	init_completion(&mrioc->init_cmds.done);
	retval = mpi3mr_admin_request_post(mrioc, &iocfacts_req,
	    sizeof(iocfacts_req), 1);
	if (retval) {
		ioc_err(mrioc, "Issue IOCFacts: Admin Post failed\n");
		goto out_unlock;
	}
	wait_for_completion_timeout(&mrioc->init_cmds.done,
	    (MPI3MR_INTADMCMD_TIMEOUT * HZ));
	if (!(mrioc->init_cmds.state & MPI3MR_CMD_COMPLETE)) {
		ioc_err(mrioc, "Issue IOCFacts: command timed out\n");
		mpi3mr_set_diagsave(mrioc);
		mpi3mr_issue_reset(mrioc,
		    MPI3_SYSIF_HOST_DIAG_RESET_ACTION_DIAG_FAULT,
		    MPI3MR_RESET_FROM_IOCFACTS_TIMEOUT);
		mrioc->unrecoverable = 1;
		retval = -1;
		goto out_unlock;
	}
	if ((mrioc->init_cmds.ioc_status & MPI3_IOCSTATUS_STATUS_MASK)
	    != MPI3_IOCSTATUS_SUCCESS) {
		ioc_err(mrioc,
		    "Issue IOCFacts: Failed IOCStatus(0x%04x) Loginfo(0x%08x)\n",
		    (mrioc->init_cmds.ioc_status & MPI3_IOCSTATUS_STATUS_MASK),
		    mrioc->init_cmds.ioc_loginfo);
		retval = -1;
		goto out_unlock;
	}
	memcpy(facts_data, (u8 *)data, data_len);
out_unlock:
	mrioc->init_cmds.state = MPI3MR_CMD_NOTUSED;
	mutex_unlock(&mrioc->init_cmds.mutex);

out:
	if (data)
		dma_free_coherent(&mrioc->pdev->dev, data_len, data, data_dma);

	return retval;
}

/**
 * mpi3mr_check_reset_dma_mask - Process IOC facts data
 * @mrioc: Adapter instance reference
 *
 * Check whether the new DMA mask requested through IOCFacts by
 * firmware needs to be set, if so set it .
 *
 * Return: 0 on success, non-zero on failure.
 */
static inline int mpi3mr_check_reset_dma_mask(struct mpi3mr_ioc *mrioc)
{
	struct pci_dev *pdev = mrioc->pdev;
	int r;
	u64 facts_dma_mask = DMA_BIT_MASK(mrioc->facts.dma_mask);

	if (!mrioc->facts.dma_mask || (mrioc->dma_mask <= facts_dma_mask))
		return 0;

	ioc_info(mrioc, "Changing DMA mask from 0x%016llx to 0x%016llx\n",
	    mrioc->dma_mask, facts_dma_mask);

	r = dma_set_mask_and_coherent(&pdev->dev, facts_dma_mask);
	if (r) {
		ioc_err(mrioc, "Setting DMA mask to 0x%016llx failed: %d\n",
		    facts_dma_mask, r);
		return r;
	}
	mrioc->dma_mask = facts_dma_mask;
	return r;
}
/**
 * mpi3mr_process_factsdata - Process IOC facts data
 * @mrioc: Adapter instance reference
 *
 * Convert IOC facts data into cpu endianness and cache it in
 * the driver .
 *
 * Return: Nothing.
 */
static void mpi3mr_process_factsdata(struct mpi3mr_ioc *mrioc,
	Mpi3IOCFactsData_t *facts_data)
{
	u32 ioc_config, req_sz, facts_flags;

	if ((le16_to_cpu(facts_data->IOCFactsDataLength)) !=
	    (sizeof(*facts_data)/4)) {
		ioc_warn(mrioc,
		    "IOCFactsdata length mismatch driver_sz(%ld) firmware_sz(%d)\n",
		    sizeof(*facts_data),
		    le16_to_cpu(facts_data->IOCFactsDataLength) * 4);
	}

	ioc_config = readl(&mrioc->sysif_regs->IOCConfiguration);
	req_sz = 1 << ((ioc_config & MPI3_SYSIF_IOC_CONFIG_OPER_REQ_ENT_SZ) >>
	    MPI3_SYSIF_IOC_CONFIG_OPER_REQ_ENT_SZ_SHIFT);
	if (le16_to_cpu(facts_data->IOCRequestFrameSize) != (req_sz/4)) {
		ioc_err(mrioc,
		    "IOCFacts data reqFrameSize mismatch hw_size(%d) firmware_sz(%d)\n",
		    req_sz/4, le16_to_cpu(facts_data->IOCRequestFrameSize));
	}

	memset(&mrioc->facts, 0, sizeof(mrioc->facts));

	facts_flags = le32_to_cpu(facts_data->Flags);
	mrioc->facts.op_req_sz = req_sz;
	mrioc->op_reply_desc_sz = 1 << ((ioc_config &
	    MPI3_SYSIF_IOC_CONFIG_OPER_RPY_ENT_SZ) >>
	    MPI3_SYSIF_IOC_CONFIG_OPER_RPY_ENT_SZ_SHIFT);

	mrioc->facts.ioc_num = facts_data->IOCNumber;
	mrioc->facts.who_init = facts_data->WhoInit;
	mrioc->facts.max_msix_vectors = le16_to_cpu(facts_data->MaxMSIxVectors);
	mrioc->facts.personality = (facts_flags &
	    MPI3_IOCFACTS_FLAGS_PERSONALITY_MASK);
	mrioc->facts.dma_mask = (facts_flags &
	    MPI3_IOCFACTS_FLAGS_DMA_ADDRESS_WIDTH_MASK) >>
	    MPI3_IOCFACTS_FLAGS_DMA_ADDRESS_WIDTH_SHIFT;
	mrioc->facts.protocol_flags = facts_data->ProtocolFlags;
	mrioc->facts.mpi_version = le32_to_cpu(facts_data->MPIVersion.Word);
	mrioc->facts.max_reqs = le16_to_cpu(facts_data->MaxOutstandingRequest);
	mrioc->facts.product_id = le16_to_cpu(facts_data->ProductID);
	mrioc->facts.reply_sz = le16_to_cpu(facts_data->ReplyFrameSize) * 4;
	mrioc->facts.exceptions = le16_to_cpu(facts_data->IOCExceptions);
	mrioc->facts.max_perids = le16_to_cpu(facts_data->MaxPersistentID);
	mrioc->facts.max_pds = le16_to_cpu(facts_data->MaxPDs);
	mrioc->facts.max_vds = le16_to_cpu(facts_data->MaxVDs);
	mrioc->facts.max_hpds = le16_to_cpu(facts_data->MaxHostPDs);
	mrioc->facts.max_advhpds = le16_to_cpu(facts_data->MaxAdvancedHostPDs);
	mrioc->facts.max_raidpds = le16_to_cpu(facts_data->MaxRAIDPDs);
	mrioc->facts.max_nvme = le16_to_cpu(facts_data->MaxNVMe);
	mrioc->facts.max_pcieswitches =
	    le16_to_cpu(facts_data->MaxPCIeSwitches);
	mrioc->facts.max_sasexpanders =
	    le16_to_cpu(facts_data->MaxSASExpanders);
	mrioc->facts.max_sasinitiators =
	    le16_to_cpu(facts_data->MaxSASInitiators);
	mrioc->facts.max_enclosures = le16_to_cpu(facts_data->MaxEnclosures);
	mrioc->facts.min_devhandle = le16_to_cpu(facts_data->MinDevHandle);
	mrioc->facts.max_devhandle = le16_to_cpu(facts_data->MaxDevHandle);
	mrioc->facts.max_op_req_q =
	    le16_to_cpu(facts_data->MaxOperationalRequestQueues);
	mrioc->facts.max_op_reply_q =
	    le16_to_cpu(facts_data->MaxOperationalReplyQueues);
	mrioc->facts.ioc_capabilities =
	    le32_to_cpu(facts_data->IOCCapabilities);
	mrioc->facts.fw_ver.build_num =
	    le16_to_cpu(facts_data->FWVersion.BuildNum);
	mrioc->facts.fw_ver.cust_id =
	    le16_to_cpu(facts_data->FWVersion.CustomerID);
	mrioc->facts.fw_ver.ph_minor = facts_data->FWVersion.PhaseMinor;
	mrioc->facts.fw_ver.ph_major = facts_data->FWVersion.PhaseMajor;
	mrioc->facts.fw_ver.gen_minor = facts_data->FWVersion.GenMinor;
	mrioc->facts.fw_ver.gen_major = facts_data->FWVersion.GenMajor;
	mrioc->msix_count = min_t(int, mrioc->msix_count,
	    mrioc->facts.max_msix_vectors);
	mrioc->facts.sge_mod_mask = facts_data->SGEModifierMask;
	mrioc->facts.sge_mod_value = facts_data->SGEModifierValue;
	mrioc->facts.sge_mod_shift = facts_data->SGEModifierShift;
	mrioc->facts.shutdown_timeout =
	    le16_to_cpu(facts_data->ShutdownTimeout);

	ioc_info(mrioc, "ioc_num(%d), maxopQ(%d), maxopRepQ(%d), maxdh(%d),",
	    mrioc->facts.ioc_num, mrioc->facts.max_op_req_q,
	    mrioc->facts.max_op_reply_q, mrioc->facts.max_devhandle);
	ioc_info(mrioc,
	    "maxreqs(%d), mindh(%d) maxPDs(%d) maxvectors(%d) maxperids(%d)\n",
	    mrioc->facts.max_reqs, mrioc->facts.min_devhandle,
	    mrioc->facts.max_pds, mrioc->facts.max_msix_vectors,
	    mrioc->facts.max_perids);
	ioc_info(mrioc, "SGEModMask 0x%x SGEModVal 0x%x SGEModShift 0x%x ",
	    mrioc->facts.sge_mod_mask, mrioc->facts.sge_mod_value,
	    mrioc->facts.sge_mod_shift);
	ioc_info(mrioc, "DMA Mask %d InitialPE Status 0x%x\n",
	    mrioc->facts.dma_mask, (facts_flags &
	    MPI3_IOCFACTS_FLAGS_INITIAL_PORT_ENABLE_MASK));

	mrioc->max_host_ios = mrioc->facts.max_reqs - MPI3MR_INTERNAL_CMDS_RESVD;

	if (reset_devices)
		mrioc->max_host_ios = min_t(int, mrioc->max_host_ios,
		    MPI3MR_HOST_IOS_KDUMP);

}

/**
 * mpi3mr_alloc_reply_sense_bufs - Send IOC Init
 * @mrioc: Adapter instance reference
 *
 * Allocate and initialize the reply free buffers, sense
 * buffers, reply free queue and sense buffer queue.
 *
 * Return: 0 on success, non-zero on failures.
 */
static int mpi3mr_alloc_reply_sense_bufs(struct mpi3mr_ioc *mrioc)
{
	int retval = 0;
	u32 sz, i;
	dma_addr_t phy_addr;

	if (mrioc->init_cmds.reply)
		goto post_reply_sbuf;

	mrioc->init_cmds.reply = kzalloc(mrioc->facts.reply_sz, GFP_KERNEL);
	if (!mrioc->init_cmds.reply)
		goto out_failed;


	mrioc->num_reply_bufs = mrioc->facts.max_reqs + MPI3MR_NUM_EVT_REPLIES;
	mrioc->reply_free_qsz = mrioc->num_reply_bufs + 1;
	mrioc->num_sense_bufs = mrioc->facts.max_reqs / MPI3MR_SENSEBUF_FACTOR;
	mrioc->sense_buf_q_sz = mrioc->num_sense_bufs + 1;

	/* reply buffer pool, 16 byte align */
	sz = mrioc->num_reply_bufs * mrioc->facts.reply_sz;
	mrioc->reply_buf_pool = dma_pool_create("reply_buf pool",
	    &mrioc->pdev->dev, sz, 16, 0);
	if (!mrioc->reply_buf_pool) {
		ioc_err(mrioc, "reply buf pool: dma_pool_create failed\n");
		goto out_failed;
	}

	mrioc->reply_buf = dma_pool_zalloc(mrioc->reply_buf_pool, GFP_KERNEL,
	    &mrioc->reply_buf_dma);
	if (!mrioc->reply_buf)
		goto out_failed;

	mrioc->reply_buf_dma_max_address = mrioc->reply_buf_dma + sz;

	/* reply free queue, 8 byte align */
	sz = mrioc->reply_free_qsz * 8;
	mrioc->reply_free_q_pool = dma_pool_create("reply_free_q pool",
	    &mrioc->pdev->dev, sz, 8, 0);
	if (!mrioc->reply_free_q_pool) {
		ioc_err(mrioc, "reply_free_q pool: dma_pool_create failed\n");
		goto out_failed;
	}
	mrioc->reply_free_q = dma_pool_zalloc(mrioc->reply_free_q_pool,
	    GFP_KERNEL, &mrioc->reply_free_q_dma);
	if (!mrioc->reply_free_q)
		goto out_failed;

	/* sense buffer pool,  4 byte align */
	sz = mrioc->num_sense_bufs * MPI3MR_SENSEBUF_SZ;
	mrioc->sense_buf_pool = dma_pool_create("sense_buf pool",
	    &mrioc->pdev->dev, sz, 4, 0);
	if (!mrioc->sense_buf_pool) {
		ioc_err(mrioc, "sense_buf pool: dma_pool_create failed\n");
		goto out_failed;
	}
	mrioc->sense_buf = dma_pool_zalloc(mrioc->sense_buf_pool, GFP_KERNEL,
	    &mrioc->sense_buf_dma);
	if (!mrioc->sense_buf)
		goto out_failed;

	/* sense buffer queue, 8 byte align */
	sz = mrioc->sense_buf_q_sz * 8;
	mrioc->sense_buf_q_pool = dma_pool_create("sense_buf_q pool",
	    &mrioc->pdev->dev, sz, 8, 0);
	if (!mrioc->sense_buf_q_pool) {
		ioc_err(mrioc, "sense_buf_q pool: dma_pool_create failed\n");
		goto out_failed;
	}
	mrioc->sense_buf_q = dma_pool_zalloc(mrioc->sense_buf_q_pool,
	    GFP_KERNEL, &mrioc->sense_buf_q_dma);
	if (!mrioc->sense_buf_q)
		goto out_failed;

post_reply_sbuf:
	sz = mrioc->num_reply_bufs * mrioc->facts.reply_sz;
	ioc_info(mrioc,
	    "reply buf pool(0x%p): depth(%d), frame_size(%d), pool_size(%d kB), reply_dma(0x%llx)\n",
	    mrioc->reply_buf, mrioc->num_reply_bufs, mrioc->facts.reply_sz,
	    (sz / 1024), (unsigned long long)mrioc->reply_buf_dma);
	sz = mrioc->reply_free_qsz * 8;
	ioc_info(mrioc,
	    "reply_free_q pool(0x%p): depth(%d), frame_size(%d), pool_size(%d kB), reply_dma(0x%llx)\n",
	    mrioc->reply_free_q, mrioc->reply_free_qsz, 8, (sz / 1024),
	    (unsigned long long)mrioc->reply_free_q_dma);
	sz = mrioc->num_sense_bufs * MPI3MR_SENSEBUF_SZ;
	ioc_info(mrioc,
	    "sense_buf pool(0x%p): depth(%d), frame_size(%d), pool_size(%d kB), sense_dma(0x%llx)\n",
	    mrioc->sense_buf, mrioc->num_sense_bufs, MPI3MR_SENSEBUF_SZ,
	    (sz / 1024), (unsigned long long)mrioc->sense_buf_dma);
	sz = mrioc->sense_buf_q_sz * 8;
	ioc_info(mrioc,
	    "sense_buf_q pool(0x%p): depth(%d), frame_size(%d), pool_size(%d kB), sense_dma(0x%llx)\n",
	    mrioc->sense_buf_q, mrioc->sense_buf_q_sz, 8, (sz / 1024),
	    (unsigned long long)mrioc->sense_buf_q_dma);

	/* initialize Reply buffer Queue */
	for (i = 0, phy_addr = mrioc->reply_buf_dma;
	    i < mrioc->num_reply_bufs; i++, phy_addr += mrioc->facts.reply_sz)
		mrioc->reply_free_q[i] = cpu_to_le64(phy_addr);
	mrioc->reply_free_q[i] = cpu_to_le64(0);

	/* initialize Sense Buffer Queue */
	for (i = 0, phy_addr = mrioc->sense_buf_dma;
	    i < mrioc->num_sense_bufs; i++, phy_addr += MPI3MR_SENSEBUF_SZ)
		mrioc->sense_buf_q[i] = cpu_to_le64(phy_addr);
	mrioc->sense_buf_q[i] = cpu_to_le64(0);
	return retval;

out_failed:
	retval = -1;
	return retval;
}

/**
 * mpi3mr_issue_iocinit - Send IOC Init
 * @mrioc: Adapter instance reference
 *
 * Issue IOC Init MPI request through admin queue and wait for
 * the completion of it or time out.
 *
 * Return: 0 on success, non-zero on failures.
 */
static int mpi3mr_issue_iocinit(struct mpi3mr_ioc *mrioc)
{
	Mpi3IOCInitRequest_t iocinit_req;
	Mpi3DriverInfoLayout_t *drv_info;
	dma_addr_t data_dma;
	u32 data_len = sizeof(*drv_info);
	int retval = 0;
	ktime_t current_time;

	drv_info = dma_alloc_coherent(&mrioc->pdev->dev, data_len, &data_dma,
	    GFP_KERNEL);
	if (!drv_info) {
		retval = -1;
		goto out;
	}
	drv_info->InformationLength = cpu_to_le32(data_len);
	strcpy(drv_info->DriverSignature, "Broadcom");
	strcpy(drv_info->OsName, utsname()->sysname);
	drv_info->OsName[sizeof(drv_info->OsName)-1] = 0;
	strcpy(drv_info->OsVersion, utsname()->release);
	drv_info->OsVersion[sizeof(drv_info->OsVersion)-1] = 0;
	strcpy(drv_info->DriverName, MPI3MR_DRIVER_NAME);
	strcpy(drv_info->DriverVersion, MPI3MR_DRIVER_VERSION);
	strcpy(drv_info->DriverReleaseDate, MPI3MR_DRIVER_RELDATE);
	drv_info->DriverCapabilities = 0;
	memcpy((u8 *)&mrioc->driver_info, (u8 *)drv_info,
	    sizeof(mrioc->driver_info));

	memset(&iocinit_req, 0, sizeof(iocinit_req));
	mutex_lock(&mrioc->init_cmds.mutex);
	if (mrioc->init_cmds.state & MPI3MR_CMD_PENDING) {
		retval = -1;
		ioc_err(mrioc, "Issue IOCInit: Init command is in use\n");
		mutex_unlock(&mrioc->init_cmds.mutex);
		goto out;
	}
	mrioc->init_cmds.state = MPI3MR_CMD_PENDING;
	mrioc->init_cmds.is_waiting = 1;
	mrioc->init_cmds.callback = NULL;
	iocinit_req.HostTag = cpu_to_le16(MPI3MR_HOSTTAG_INITCMDS);
	iocinit_req.Function = MPI3_FUNCTION_IOC_INIT;
	iocinit_req.MPIVersion.Struct.Dev = MPI3_VERSION_DEV;
	iocinit_req.MPIVersion.Struct.Unit = MPI3_VERSION_UNIT;
	iocinit_req.MPIVersion.Struct.Major = MPI3_VERSION_MAJOR;
	iocinit_req.MPIVersion.Struct.Minor = MPI3_VERSION_MINOR;
	iocinit_req.WhoInit = MPI3_WHOINIT_HOST_DRIVER;
	iocinit_req.ReplyFreeQueueDepth = cpu_to_le16(mrioc->reply_free_qsz);
	iocinit_req.ReplyFreeQueueAddress =
	    cpu_to_le64(mrioc->reply_free_q_dma);
	iocinit_req.SenseBufferLength = cpu_to_le16(MPI3MR_SENSEBUF_SZ);
	iocinit_req.SenseBufferFreeQueueDepth =
	    cpu_to_le16(mrioc->sense_buf_q_sz);
	iocinit_req.SenseBufferFreeQueueAddress =
	    cpu_to_le64(mrioc->sense_buf_q_dma);
	iocinit_req.DriverInformationAddress = cpu_to_le64(data_dma);

	current_time = ktime_get_real();
	iocinit_req.TimeStamp = cpu_to_le64(ktime_to_ms(current_time));

	init_completion(&mrioc->init_cmds.done);
	retval = mpi3mr_admin_request_post(mrioc, &iocinit_req,
	    sizeof(iocinit_req), 1);
	if (retval) {
		ioc_err(mrioc, "Issue IOCInit: Admin Post failed\n");
		goto out_unlock;
	}
	wait_for_completion_timeout(&mrioc->init_cmds.done,
	    (MPI3MR_INTADMCMD_TIMEOUT * HZ));
	if (!(mrioc->init_cmds.state & MPI3MR_CMD_COMPLETE)) {
		mpi3mr_set_diagsave(mrioc);
		mpi3mr_issue_reset(mrioc,
		    MPI3_SYSIF_HOST_DIAG_RESET_ACTION_DIAG_FAULT,
		    MPI3MR_RESET_FROM_IOCINIT_TIMEOUT);
		mrioc->unrecoverable = 1;
		ioc_err(mrioc, "Issue IOCInit: command timed out\n");
		retval = -1;
		goto out_unlock;
	}
	if ((mrioc->init_cmds.ioc_status & MPI3_IOCSTATUS_STATUS_MASK)
	    != MPI3_IOCSTATUS_SUCCESS) {
		ioc_err(mrioc,
		    "Issue IOCInit: Failed IOCStatus(0x%04x) Loginfo(0x%08x)\n",
		    (mrioc->init_cmds.ioc_status & MPI3_IOCSTATUS_STATUS_MASK),
		    mrioc->init_cmds.ioc_loginfo);
		retval = -1;
		goto out_unlock;
	}

out_unlock:
	mrioc->init_cmds.state = MPI3MR_CMD_NOTUSED;
	mutex_unlock(&mrioc->init_cmds.mutex);

out:
	if (drv_info)
		dma_free_coherent(&mrioc->pdev->dev, data_len, drv_info,
		    data_dma);

	return retval;
}


/**
 * mpi3mr_alloc_chain_bufs - Allocate chain buffers
 * @mrioc: Adapter instance reference
 *
 * Allocate chain buffers and set a bitmap to indicate free
 * chain buffers. Chain buffers are used to pass the SGE
 * information along with MPI3 SCSI IO requests for host I/O.
 *
 * Return: 0 on success, non-zero on failure
 */
static int mpi3mr_alloc_chain_bufs(struct mpi3mr_ioc *mrioc)
{
	int retval = 0;
	u32 sz, i;
	u16 num_chains;

	num_chains = mrioc->max_host_ios/MPI3MR_CHAINBUF_FACTOR;

	mrioc->chain_buf_count = num_chains;
	sz = sizeof(struct chain_element) * num_chains;
	mrioc->chain_sgl_list = kzalloc(sz, GFP_KERNEL);
	if (!mrioc->chain_sgl_list)
		goto out_failed;

	sz = MPI3MR_PAGE_SIZE_4K;
	mrioc->chain_buf_pool = dma_pool_create("chain_buf pool",
	    &mrioc->pdev->dev, sz, 16, 0);
	if (!mrioc->chain_buf_pool) {
		ioc_err(mrioc, "chain buf pool: dma_pool_create failed\n");
		goto out_failed;
	}

	for (i = 0; i < num_chains; i++) {
		mrioc->chain_sgl_list[i].addr =
		    dma_pool_zalloc(mrioc->chain_buf_pool, GFP_KERNEL,
		    &mrioc->chain_sgl_list[i].dma_addr);

		if (!mrioc->chain_sgl_list[i].addr)
			goto out_failed;
	}
	mrioc->chain_bitmap_sz = num_chains / 8;
	if (num_chains % 8)
		mrioc->chain_bitmap_sz++;
	mrioc->chain_bitmap = kzalloc(mrioc->chain_bitmap_sz, GFP_KERNEL);
	if (!mrioc->chain_bitmap)
		goto out_failed;
	return retval;
out_failed:
	retval = -1;
	return retval;
}


/**
 * mpi3mr_cleanup_resources - Free PCI resources
 * @mrioc: Adapter instance reference
 *
 * Unmap PCI device memory and disable PCI device.
 *
 * Return: 0 on success and non-zero on failure.
 */
void mpi3mr_cleanup_resources(struct mpi3mr_ioc *mrioc)
{
	struct pci_dev *pdev = mrioc->pdev;

	mpi3mr_cleanup_isr(mrioc);

	if (mrioc->sysif_regs) {
		iounmap(mrioc->sysif_regs);
		mrioc->sysif_regs = NULL;
	}

	if (pci_is_enabled(pdev)) {
		if (mrioc->bars)
			pci_release_selected_regions(pdev, mrioc->bars);
		pci_disable_device(pdev);
	}
}

/**
 * mpi3mr_setup_resources - Enable PCI resources
 * @mrioc: Adapter instance reference
 *
 * Enable PCI device memory, MSI-x registers and set DMA mask.
 *
 * Return: 0 on success and non-zero on failure.
 */
int mpi3mr_setup_resources(struct mpi3mr_ioc *mrioc)
{
	struct pci_dev *pdev = mrioc->pdev;
	u32 memap_sz = 0;
	int i, retval = 0, capb = 0;
	u16 message_control;
	u64 dma_mask = mrioc->dma_mask ? mrioc->dma_mask :
	    (((dma_get_required_mask(&pdev->dev) > DMA_BIT_MASK(32)) &&
	    (sizeof(dma_addr_t) > 4)) ? DMA_BIT_MASK(64):DMA_BIT_MASK(32));

	if (pci_enable_device_mem(pdev)) {
		ioc_err(mrioc, "pci_enable_device_mem: failed\n");
		retval = -ENODEV;
		goto out_failed;
	}

	capb = pci_find_capability(pdev, PCI_CAP_ID_MSIX);
	if (!capb) {
		ioc_err(mrioc, "Unable to find MSI-X Capabilities\n");
		retval = -ENODEV;
		goto out_failed;
	}
	mrioc->bars = pci_select_bars(pdev, IORESOURCE_MEM);

	if (pci_request_selected_regions(pdev, mrioc->bars,
	    mrioc->driver_name)) {
		ioc_err(mrioc, "pci_request_selected_regions: failed\n");
		retval = -ENODEV;
		goto out_failed;
	}

	for (i = 0; (i < DEVICE_COUNT_RESOURCE); i++) {
		if (pci_resource_flags(pdev, i) & IORESOURCE_MEM) {
			mrioc->sysif_regs_phys = pci_resource_start(pdev, i);
			memap_sz = pci_resource_len(pdev, i);
			mrioc->sysif_regs =
			    ioremap(mrioc->sysif_regs_phys, memap_sz);
			break;
		}
	}

	pci_set_master(pdev);

	retval = dma_set_mask_and_coherent(&pdev->dev, dma_mask);
	if (retval) {
		if (dma_mask != DMA_BIT_MASK(32)) {
			ioc_warn(mrioc, "Setting 64 bit DMA mask failed\n");
			dma_mask = DMA_BIT_MASK(32);
			retval = dma_set_mask_and_coherent(&pdev->dev,
			    dma_mask);
		}
		if (retval) {
			mrioc->dma_mask = 0;
			ioc_err(mrioc, "Setting 32 bit DMA mask also failed\n");
			goto out_failed;
		}
	}
	mrioc->dma_mask = dma_mask;

	if (mrioc->sysif_regs == NULL) {
		ioc_err(mrioc,
		    "Unable to map adapter memory or resource not found\n");
		retval = -EINVAL;
		goto out_failed;
	}

	pci_read_config_word(pdev, capb + 2, &message_control);
	mrioc->msix_count = (message_control & 0x3FF) + 1;

	pci_save_state(pdev);

	pci_set_drvdata(pdev, mrioc->shost);

	mpi3mr_ioc_disable_intr(mrioc);

	ioc_info(mrioc, "iomem(0x%016llx), mapped(0x%p), size(%d)\n",
	    (unsigned long long)mrioc->sysif_regs_phys,
	    mrioc->sysif_regs, memap_sz);
	ioc_info(mrioc, "Number of MSI-X vectors found in capabilities: (%d)\n",
	    mrioc->msix_count);
	return retval;

out_failed:
	mpi3mr_cleanup_resources(mrioc);
	return retval;
}

/**
 * mpi3mr_init_ioc - Initialize the controller
 * @mrioc: Adapter instance reference
 *
 * This the controller initialization routine, executed either
 * after soft reset or from pci probe callback.
 * Setup the required resources, memory map the controller
 * registers, create admin and operational reply queue pairs,
 * allocate required memory for reply pool, sense buffer pool,
 * issue IOC init request to the firmware, unmask the events and
 * issue port enable to discover SAS/SATA/NVMe devies and RAID
 * volumes.
 *
 * Return: 0 on success and non-zero on failure.
 */
int mpi3mr_init_ioc(struct mpi3mr_ioc *mrioc)
{
	int retval = 0;
	enum mpi3mr_iocstate ioc_state;
	u64 base_info;
	u32 timeout;
	u32 ioc_status, ioc_config;
	Mpi3IOCFactsData_t facts_data;

	mrioc->change_count = 0;
	mrioc->cpu_count = num_online_cpus();
	retval = mpi3mr_setup_resources(mrioc);
	if (retval) {
		ioc_err(mrioc, "Failed to setup resources:error %d\n",
		    retval);
		goto out_nocleanup;
	}
	ioc_status = readl(&mrioc->sysif_regs->IOCStatus);
	ioc_config = readl(&mrioc->sysif_regs->IOCConfiguration);

	ioc_info(mrioc, "SOD status %x configuration %x\n",
	    ioc_status, ioc_config);

	base_info = readq(&mrioc->sysif_regs->IOCInformation);
	ioc_info(mrioc, "SOD base_info %llx\n",	base_info);

	/*The timeout value is in 2sec unit, changing it to seconds*/
	mrioc->ready_timeout =
	    ((base_info & MPI3_SYSIF_IOC_INFO_LOW_TIMEOUT_MASK) >>
	    MPI3_SYSIF_IOC_INFO_LOW_TIMEOUT_SHIFT) * 2;

	ioc_info(mrioc, "IOC ready timeout %d\n", mrioc->ready_timeout);

	ioc_state = mpi3mr_get_iocstate(mrioc);
	ioc_info(mrioc, "IOC in %s state during detection\n",
	    mpi3mr_iocstate_name(ioc_state));

	if (ioc_state == MRIOC_STATE_BECOMING_READY ||
			ioc_state == MRIOC_STATE_RESET_REQUESTED) {
		timeout = mrioc->ready_timeout * 10;
		do {
			msleep(100);
		} while (--timeout);

		ioc_state = mpi3mr_get_iocstate(mrioc);
		ioc_info(mrioc,
			"IOC in %s state after waiting for reset time\n",
			mpi3mr_iocstate_name(ioc_state));
	}

	if (ioc_state == MRIOC_STATE_READY) {
		retval = mpi3mr_issue_and_process_mur(mrioc,
		    MPI3MR_RESET_FROM_BRINGUP);
		if (retval) {
			ioc_err(mrioc, "Failed to MU reset IOC error %d\n",
			    retval);
		}
		ioc_state = mpi3mr_get_iocstate(mrioc);
	}
	if (ioc_state != MRIOC_STATE_RESET) {
		mpi3mr_print_fault_info(mrioc);
		retval = mpi3mr_issue_reset(mrioc,
		    MPI3_SYSIF_HOST_DIAG_RESET_ACTION_SOFT_RESET,
		    MPI3MR_RESET_FROM_BRINGUP);
		if (retval) {
			ioc_err(mrioc,
			    "%s :Failed to soft reset IOC error %d\n",
			    __func__, retval);
			goto out_failed;
		}
	}
	ioc_state = mpi3mr_get_iocstate(mrioc);
	if (ioc_state != MRIOC_STATE_RESET) {
		ioc_err(mrioc, "Cannot bring IOC to reset state\n");
		goto out_failed;
	}

	retval = mpi3mr_setup_admin_qpair(mrioc);
	if (retval) {
		ioc_err(mrioc, "Failed to setup admin Qs: error %d\n",
		    retval);
		goto out_failed;
	}

	retval = mpi3mr_bring_ioc_ready(mrioc);
	if (retval) {
		ioc_err(mrioc, "Failed to bring ioc ready: error %d\n",
		    retval);
		goto out_failed;
	}

	retval = mpi3mr_setup_isr(mrioc, 1);
	if (retval) {
		ioc_err(mrioc, "Failed to setup ISR error %d\n",
		    retval);
		goto out_failed;
	}

	retval = mpi3mr_issue_iocfacts(mrioc, &facts_data);
	if (retval) {
		ioc_err(mrioc, "Failed to Issue IOC Facts %d\n",
		    retval);
		goto out_failed;
	}

	mpi3mr_process_factsdata(mrioc, &facts_data);
	retval = mpi3mr_check_reset_dma_mask(mrioc);
	if (retval) {
		ioc_err(mrioc, "Resetting dma mask failed %d\n",
		    retval);
		goto out_failed;
	}

	retval = mpi3mr_alloc_reply_sense_bufs(mrioc);
	if (retval) {
		ioc_err(mrioc,
		    "%s :Failed to allocated reply sense buffers %d\n",
		    __func__, retval);
		goto out_failed;
	}

	retval = mpi3mr_alloc_chain_bufs(mrioc);
	if (retval) {
		ioc_err(mrioc, "Failed to allocated chain buffers %d\n",
		    retval);
		goto out_failed;
	}

	retval = mpi3mr_issue_iocinit(mrioc);
	if (retval) {
		ioc_err(mrioc, "Failed to Issue IOC Init %d\n",
		    retval);
		goto out_failed;
	}
	mrioc->reply_free_queue_host_index = mrioc->num_reply_bufs;
	writel(mrioc->reply_free_queue_host_index,
	    &mrioc->sysif_regs->ReplyFreeHostIndex);

	mrioc->sbq_host_index = mrioc->num_sense_bufs;
	writel(mrioc->sbq_host_index,
	    &mrioc->sysif_regs->SenseBufferFreeHostIndex);

	retval = mpi3mr_setup_isr(mrioc, 0);
	if (retval) {
		ioc_err(mrioc, "Failed to re-setup ISR, error %d\n",
		    retval);
		goto out_failed;
	}

	return retval;

out_failed:
	mpi3mr_cleanup_ioc(mrioc);
out_nocleanup:
	return retval;
}


/**
 * mpi3mr_free_mem - Free memory allocated for a controller
 * @mrioc: Adapter instance reference
 *
 * Free all the memory allocated for a controller.
 *
 * Return: Nothing.
 */
static void mpi3mr_free_mem(struct mpi3mr_ioc *mrioc)
{
	u16 i;
	struct mpi3mr_intr_info *intr_info;

	if (mrioc->sense_buf_pool) {
		if (mrioc->sense_buf)
			dma_pool_free(mrioc->sense_buf_pool, mrioc->sense_buf,
			    mrioc->sense_buf_dma);
		dma_pool_destroy(mrioc->sense_buf_pool);
		mrioc->sense_buf = NULL;
		mrioc->sense_buf_pool = NULL;
	}
	if (mrioc->sense_buf_q_pool) {
		if (mrioc->sense_buf_q)
			dma_pool_free(mrioc->sense_buf_q_pool,
			    mrioc->sense_buf_q, mrioc->sense_buf_q_dma);
		dma_pool_destroy(mrioc->sense_buf_q_pool);
		mrioc->sense_buf_q = NULL;
		mrioc->sense_buf_q_pool = NULL;
	}

	if (mrioc->reply_buf_pool) {
		if (mrioc->reply_buf)
			dma_pool_free(mrioc->reply_buf_pool, mrioc->reply_buf,
			    mrioc->reply_buf_dma);
		dma_pool_destroy(mrioc->reply_buf_pool);
		mrioc->reply_buf = NULL;
		mrioc->reply_buf_pool = NULL;
	}
	if (mrioc->reply_free_q_pool) {
		if (mrioc->reply_free_q)
			dma_pool_free(mrioc->reply_free_q_pool,
			    mrioc->reply_free_q, mrioc->reply_free_q_dma);
		dma_pool_destroy(mrioc->reply_free_q_pool);
		mrioc->reply_free_q = NULL;
		mrioc->reply_free_q_pool = NULL;
	}

	for (i = 0; i < mrioc->intr_info_count; i++) {
		intr_info = mrioc->intr_info + i;
		if (intr_info)
			intr_info->op_reply_q = NULL;
	}

	kfree(mrioc->req_qinfo);
	mrioc->req_qinfo = NULL;
	mrioc->num_op_req_q = 0;

	kfree(mrioc->op_reply_qinfo);
	mrioc->op_reply_qinfo = NULL;
	mrioc->num_op_reply_q = 0;

	kfree(mrioc->init_cmds.reply);
	mrioc->init_cmds.reply = NULL;

	kfree(mrioc->chain_bitmap);
	mrioc->chain_bitmap = NULL;

	if (mrioc->chain_buf_pool) {
		for (i = 0; i < mrioc->chain_buf_count; i++) {
			if (mrioc->chain_sgl_list[i].addr) {
				dma_pool_free(mrioc->chain_buf_pool,
				    mrioc->chain_sgl_list[i].addr,
				    mrioc->chain_sgl_list[i].dma_addr);
				mrioc->chain_sgl_list[i].addr = NULL;
			}
		}
		dma_pool_destroy(mrioc->chain_buf_pool);
		mrioc->chain_buf_pool = NULL;
	}

	kfree(mrioc->chain_sgl_list);
	mrioc->chain_sgl_list = NULL;

	if (mrioc->admin_reply_base) {
		dma_free_coherent(&mrioc->pdev->dev, mrioc->admin_reply_q_sz,
		    mrioc->admin_reply_base, mrioc->admin_reply_dma);
		mrioc->admin_reply_base = NULL;
	}
	if (mrioc->admin_req_base) {
		dma_free_coherent(&mrioc->pdev->dev, mrioc->admin_req_q_sz,
		    mrioc->admin_req_base, mrioc->admin_req_dma);
		mrioc->admin_req_base = NULL;
	}

}

/**
 * mpi3mr_issue_ioc_shutdown - Shutdown controller
 * @mrioc: Adapter instance reference
 *
 * Send shutodwn notification to the controller and wait for the
 * shutdown_timeout for it to be completed.
 *
 * Return: Nothing.
 */
static void mpi3mr_issue_ioc_shutdown(struct mpi3mr_ioc *mrioc)
{
	u32 ioc_config, ioc_status;
	u8 retval = 1;
	u32 timeout = MPI3MR_DEFAULT_SHUTDOWN_TIME * 10;

	ioc_info(mrioc, "Issuing Shutdown Notification\n");
	if (mrioc->unrecoverable) {
		ioc_warn(mrioc,
		    "IOC is unrecoverable Shutdown is not issued\n");
		return;
	}
	ioc_status = readl(&mrioc->sysif_regs->IOCStatus);
	if ((ioc_status & MPI3_SYSIF_IOC_STATUS_SHUTDOWN_MASK)
	    == MPI3_SYSIF_IOC_STATUS_SHUTDOWN_IN_PROGRESS) {
		ioc_info(mrioc, "Shutdown already in progress\n");
		return;
	}

	ioc_config = readl(&mrioc->sysif_regs->IOCConfiguration);
	ioc_config |= MPI3_SYSIF_IOC_CONFIG_SHUTDOWN_NORMAL;
	ioc_config |= MPI3_SYSIF_IOC_CONFIG_DEVICE_SHUTDOWN;

	writel(ioc_config, &mrioc->sysif_regs->IOCConfiguration);

	if (mrioc->facts.shutdown_timeout)
		timeout = mrioc->facts.shutdown_timeout * 10;

	do {
		ioc_status = readl(&mrioc->sysif_regs->IOCStatus);
		if ((ioc_status & MPI3_SYSIF_IOC_STATUS_SHUTDOWN_MASK)
		    == MPI3_SYSIF_IOC_STATUS_SHUTDOWN_COMPLETE) {
			retval = 0;
			break;
		}
		msleep(100);
	} while (--timeout);


	ioc_status = readl(&mrioc->sysif_regs->IOCStatus);
	ioc_config = readl(&mrioc->sysif_regs->IOCConfiguration);

	if (retval) {
		if ((ioc_status & MPI3_SYSIF_IOC_STATUS_SHUTDOWN_MASK)
		    == MPI3_SYSIF_IOC_STATUS_SHUTDOWN_IN_PROGRESS)
			ioc_warn(mrioc,
			    "Shutdown still in progress after timeout\n");
	}

	ioc_info(mrioc,
	    "Base IOC Sts/Config after %s shutdown is (0x%x)/(0x%x)\n",
	    (!retval)?"successful":"failed", ioc_status,
	    ioc_config);
}

/**
 * mpi3mr_cleanup_ioc - Cleanup controller
 * @mrioc: Adapter instance reference
 *
 * Controller cleanup handler, Message unit reset or soft reset
 * and shutdown notification is issued to the controller and the
 * associated memory resources are freed.
 *
 * Return: Nothing.
 */
void mpi3mr_cleanup_ioc(struct mpi3mr_ioc *mrioc)
{
	enum mpi3mr_iocstate ioc_state;

	mpi3mr_ioc_disable_intr(mrioc);

	ioc_state = mpi3mr_get_iocstate(mrioc);

	if ((!mrioc->unrecoverable) && (!mrioc->reset_in_progress) &&
	     (ioc_state == MRIOC_STATE_READY)) {
		if (mpi3mr_issue_and_process_mur(mrioc,
		    MPI3MR_RESET_FROM_CTLR_CLEANUP))
			mpi3mr_issue_reset(mrioc,
			    MPI3_SYSIF_HOST_DIAG_RESET_ACTION_SOFT_RESET,
			    MPI3MR_RESET_FROM_MUR_FAILURE);

		 mpi3mr_issue_ioc_shutdown(mrioc);
	}

	mpi3mr_free_mem(mrioc);
	mpi3mr_cleanup_resources(mrioc);
}


/**
 * mpi3mr_soft_reset_handler - Reset the controller
 * @mrioc: Adapter instance reference
 * @reset_reason: Reset reason code
 * @snapdump: Flag to generate snapdump in firmware or not
 *
 * TBD
 *
 * Return: 0 on success, non-zero on failure.
 */
int mpi3mr_soft_reset_handler(struct mpi3mr_ioc *mrioc,
	u32 reset_reason, u8 snapdump)
{
	return 0;
}