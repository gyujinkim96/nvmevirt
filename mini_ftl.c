// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "mini_ftl.h"


static void init_maptbl(struct mini_ftl *mini_ftl)
{
	int i;
	struct ssdparams *spp = &mini_ftl->ssd->sp;
	unsigned long bitmap_size = (spp->tt_pgs + sizeof(char)) / sizeof(char);

	mini_ftl->maptbl = vzmalloc(sizeof(ppo) * spp->tt_pgs);
	mini_ftl->valid_ppos = vzmalloc(sizeof(char) * bitmap_size);
}

static void remove_maptbl(struct mini_ftl *mini_ftl)
{
	vfree(mini_ftl->maptbl);
	vfree(mini_ftl->valid_ppos);
}

static void mini_init_params(struct miniparams *mpp, struct ssdparams *spp, uint64_t capacity)
{

}


static void init_write_pointers(struct mini_ftl *mini_ftl) 
{
	int i;
	struct ssdparams *spp = &mini_ftl->ssd->sp;

	mini_ftl->write_pointers = vzalloc(sizeof(int) * spp->tt_blks);
}

static remove_write_pointers(struct mini_ftl *mini_ftl) 
{
	vfree(mini_ftl->write_pointers);
}

static void mini_init_ftl(struct mini_ftl *mini_ftl, struct miniparams *mpp, struct ssd *ssd)
{
	mini_ftl->mp = *mpp;
	mini_ftl->ssd = ssd;

	init_maptbl(mini_ftl);
	init_write_pointers(mini_ftl);

	NVMEV_INFO("Init FTL instance with %d channels (%ld pages)\n", conv_ftl->ssd->sp.nchs,
		   conv_ftl->ssd->sp.tt_pgs);
}

uint64_t mixed_to_block_idx(uint64_t mixed) {
	return mixed >> sizeof(ppo);
}

bool is_valid_ppos(char *valid_ppos, uint64_t idx) {
	return valid_ppos[idx / sizeof(char)] & (idx % sizeof(char));
}

int get_maximum_size(int cnt) {
	int ret = 0;
	int power = 1;

	while (power < cnt) {
		power *= 2;
		ret++;
	}

	return ret;
}

uint64_t mixed_lpa_to_physical(struct mini_ftl *mini_ftl, uint64_t mixed_addr) {
	uint64_t physical_block_idx = mixed_to_block_idx(mixed_addr);
	ppo ppo = mini_ftl->maptbl[physical_block_idx];
	struct ssdparams *ssp = mini_ftl->ssd->sp;
	struct ppa ppa = 0;
	int bit_cnt;

	if (!is_valid_ppo(mini_ftl->valid_ppos, physical_block_idx)) {
		return -1;
	}

	mixed_addr &= ((1<<sizeof(ppo))-1);
	return mixed_addr + ppo;
}

struct ppa mixed_lpa_to_ppa(struct mini_ftl *mini_ftl, uint64_t mixed_addr) {
	uint64_t physical_block_idx = mixed_to_block_idx(mixed_addr);
	ppo ppo = mini_ftl->maptbl[physical_block_idx];
	struct ssdparams *ssp = mini_ftl->ssd->sp;
	struct ppa ppa = 0;
	int bit_cnt;

	if (!is_valid_ppo(mini_ftl->valid_ppos, physical_block_idx)) {
		return UNMAPPED_PPA;
	}

	ppa.g.pg = ppo;
	mixed_addr >>= sizeof(ppo);

	ppa.g.ch = mixed_addr % ssp->nchs;
	mixed_addr /= ssp->nchs;
	
	ppa.g.lun = mixed_addr % ssp->luns_per_ch;
	mixed_addr /= ssp->luns_per_ch;

	ppa.g.pl = mixed_addr % ssp->pls_per_lun;
	mixed_addr /= ssp->pls_per_lun;

	ppa.g.blk = mixed_addr % ssp->blks_per_pl;
	
	return ppa;
}

static bool mini_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret) {
	struct mini_ftl *mini_ftl = (struct mini_ftl *)ns->ftls;
	struct ssdparams *spp = &mini_ftl->ssd->sp;

	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba;
	uint64_t nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg;
	uint64_t end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
	uint64_t lpn;

	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed, nsecs_latest = nsecs_start;

	struct ppa ppa;
	struct nand_cmd swr;

	uint64_t pgs = 0, pg_off;

	swr.type = USER_IO;
	swr.cmd = NAND_READ;
	swr.stime = nsecs_latest;
	swr.interleave_pci_dma = false; // todo ?

	for (lpn = start_lpn; lpn <= end_lpn; lpn += pgs) {
		ppa = mixed_lpa_to_ppa(mini_ftl, lpn);

		if (ppa == UNMAPPED_PPA) {
			NVMEV_DEBUG_VERBOSE("lpn 0x%llx not mapped to valid ppa\n", lpn);
			NVMEV_DEBUG_VERBOSE("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d\n",
					ppa.g.ch, ppa.g.lun, ppa.g.blk,
					ppa.g.pl, ppa.g.pg);
			continue;		
		}

		pg_off = ppa.g.pg % spp->pgs_per_flashpg;
		pgs = min(end_lpn - lpn + 1, (uint64_t)(spp->pgs_per_flashpg - pg_off));
		swr.xfer_size = pgs * spp->pgsz;
		swr.ppa = &ppa;
		nsecs_completed = ssd_advance_nand(mini_ftl->ssd, &swr);
		nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	}

	if (swr.interleave_pci_dma == false) {
		nsecs_completed = ssd_advance_pcie(mini_ftl->ssd, nsecs_latest, nr_lba * spp->secsz);
		nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	}

	ret->status = status;
	ret->nsecs_target = nsecs_latest;
	return true;
}

static bool mini_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret) {
	struct mini_ftl *mini_ftl = (struct mini_ftl *)ns->ftls;

	/* wbuf and spp are shared by all instances */
	struct ssdparams *spp = &mini_ftl->ssd->sp;
	struct nvme_rw_command *cmd = &(req->cmd->rw);
	uint64_t slba = cmd->slba;
	uint64_t nr_lba = cmd->length + 1;
	uint64_t slpn, elpn, lpn;

	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_xfer_completed = nsecs_start;
	uint64_t nsecs_latest = nsecs_start;
	uint32_t status = NVME_SC_SUCCESS;

	uint64_t pgs = 0;

	struct buffer *write_buffer;

	slpn = slba / spp->secs_per_pg;
	slba = mini_ftl->write_pointers[mixed_to_block_idx(slpn)];

	slpn = slba / spp->secs_per_pg;
	elpn = (slba + nr_lba - 1) / spp->secs_per_pg;

	write_buffer = mini_ftl->ssd->write_buffer;

	if (buffer_allocate(write_buffer, LBA_TO_BYTE(nr_lba)) < LBA_TO_BYTE(nr_lba))
		return false;

	// todo error

	mini_ftl->write_pointers[mixed_to_block_idx(slpn)] =+= nr_lba;

	nsecs_latest = nsecs_start;
	nsecs_latest = ssd_advance_write_buffer(mini_ftl->ssd, nsecs_latest, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	for (lpn = slpn; lpn <= elpn; lpn += pgs) {
		struct ppa ppa;
		uint64_t pg_off;

		ppa = mixed_lpa_to_ppa(mini_ftl, lpn);
		pg_off = ppa.g.pg % spp->pgs_per_oneshotpg;
		pgs = min(elpn - lpn + 1, (uint64_t)(spp->pgs_per_oneshotpg - pg_off));

		if (((pg_off + pgs) == spp->pgs_per_oneshotpg) || ((lpn + pgs - 1) % spp->pgs_per_blk == spp->pgs_per_blk-1)) {
			struct nand_cmd swr = {
				.type = USER_IO,
				.cmd = NAND_WRITE,
				.stime = nsecs_xfer_completed,
				.xfer_size = spp->pgs_per_oneshotpg * spp->pgsz,
				.interleave_pci_dma = false,
				.ppa = &ppa,
			};
			size_t bufs_to_release;
			uint32_t unaligned_space =
				(spp->pgs_per_blk * spp->pgsz) % (spp->pgs_per_oneshotpg * spp->pgsz);
			uint64_t nsecs_completed = ssd_advance_nand(mini_ftl->ssd, &swr);

			nsecs_latest = max(nsecs_completed, nsecs_latest);
			// NVMEV_ZNS_DEBUG("%s Flush slba 0x%llx nr_lba 0x%llx zone_id %d state %d\n",
			// 		__func__, slba, nr_lba, zid, state);

			if (((lpn + pgs - 1) % spp->pgs_per_blk == spp->pgs_per_blk-1) && (unaligned_space > 0))
				bufs_to_release = unaligned_space;
			else
				bufs_to_release = spp->pgs_per_oneshotpg * spp->pgsz;

			schedule_internal_operation(req->sq_id, nsecs_completed, write_buffer,
						    bufs_to_release);
		}
	}

	out:
	ret->status = status;
	if ((cmd->control & NVME_RW_FUA) ||
	    (spp->write_early_completion == 0)) /*Wait all flash operations*/
		ret->nsecs_target = nsecs_latest;
	else /*Early completion*/
		ret->nsecs_target = nsecs_xfer_completed;

	return true;
}

static void mini_flush(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret) {
	uint64_t start, latest;
	uint32_t i;
	struct mini_ftl *mini_ftls = (struct mini_ftl *)ns->ftls;

	start = local_clock();
	latest = start;
	for (i = 0; i < ns->nr_parts; i++) {
		latest = max(latest, ssd_next_idle_time(mini_ftls[i].ssd));
	}

	NVMEV_DEBUG_VERBOSE("%s: latency=%llu\n", __func__, latest - start);

	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = latest;
	return;
}

void mini_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
uint32_t cpu_nr_dispatcher) {
	struct ssdparams spp;
	struct miniparams mpp;
	struct mini_ftl *mini_ftl;
	struct ssd *ssd;
	uint32_t i;

	const uint32_t nr_parts = 1; /* Not support multi partitions for zns*/
	NVMEV_ASSERT(nr_parts == 1);

	ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
	ssd_init_params(&spp, size, nr_parts);
	ssd_init(ssd, &spp, cpu_nr_dispatcher);

	mini_ftl = kmalloc(sizeof(struct zns_ftl) * nr_parts, GFP_KERNEL);
	mini_init_params(&mpp, &spp, size);
	mini_init_ftl(mini_ftl, &mpp, ssd);


	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = nr_parts;
	ns->ftls = (void *)mini_ftl;
	ns->size = size;
	ns->mapped = mapped_addr;
	/*register io command handler*/
	ns->proc_io_cmd = mini_proc_nvme_io_cmd;

	NVMEV_INFO("FTL physical space: %lld, logical space: %lld\n",
			size, ns->size);

	return;
}


void mini_remove_namespace(struct nvmev_ns *ns) {
	struct mini_ftl *mini_ftl = (struct mini_ftl *)ns->ftls;

	ssd_remove(mini_ftl->ssd);

	remove_maptbl(mini_ftl);
	remove_write_pointers(mini_ftl);

	kfree(mini_ftl->ssd);
	kfree(mini_ftl);

	ns->ftls = NULL;
}

bool mini_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
		if (!mini_write(ns, req, ret))
			return false;
		break;
	case nvme_cmd_read:
		if (!mini_read(ns, req, ret))
			return false;
		break;
	case nvme_cmd_flush:
		mini_flush(ns, req, ret);
		break;
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
				nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

	return true;
}
