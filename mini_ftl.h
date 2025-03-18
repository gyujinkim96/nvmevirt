// SPDX-License-Identifier: GPL-2.0-only

#ifndef _NVMEVIRT_MINI_FTL_H
#define _NVMEVIRT_MINI_FTL_H

#include <linux/types.h>
#include "pqueue/pqueue.h"
#include "ssd_config.h"
#include "ssd.h"

// physical page offset
typedef uint8_t ppo; 


struct miniparams {

};


struct mini_ftl {
	struct ssd *ssd;

	struct miniparams mp;
	ppo *maptbl;
	char *valid_ppos;
	int *write_pointers;
};

uint64_t mixed_lpa_to_physical(struct mini_ftl *mini_ftl, uint64_t mixed_addr);

void mini_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher);

void mini_remove_namespace(struct nvmev_ns *ns);

bool mini_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			   struct nvmev_result *ret);

#endif
