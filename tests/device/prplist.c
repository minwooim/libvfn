// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2022 The libvfn Authors. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <stdbool.h>
#include <stdint.h>

#include <nvme/types.h>

#include "ccan/err/err.h"
#include "ccan/opt/opt.h"
#include "ccan/tap/tap.h"

#include "vfn/nvme.h"

#include "common.h"

/* two prplist pages, so the list must chain to cover the test buffer */
#define NPRPLISTS 2

/*
 * @id must be a page-aligned mapping (see pgmap()): nvme_admin() DMA-maps it
 * on the fly, and the VFIO/iommufd backends require the vaddr, length and
 * iova to all be page aligned. A stack- or heap-allocated buffer isn't
 * guaranteed to be, and the mapping call fails before the command is ever
 * submitted.
 */
static int identify_ctrl(struct nvme_id_ctrl *id)
{
	union nvme_cmd cmd = {
		.identify = (struct nvme_cmd_identify) {
			.opcode = nvme_admin_identify,
			.cns = NVME_IDENTIFY_CNS_CTRL,
		},
	};

	return nvme_admin(&ctrl, &cmd, id, NVME_IDENTIFY_DATA_SIZE, NULL);
}

static int identify_ns(struct nvme_id_ns *id)
{
	union nvme_cmd cmd = {
		.identify = (struct nvme_cmd_identify) {
			.opcode = nvme_admin_identify,
			.cns = NVME_IDENTIFY_CNS_NS,
			.nsid = cpu_to_le32(nsid),
		},
	};

	return nvme_admin(&ctrl, &cmd, id, NVME_IDENTIFY_DATA_SIZE, NULL);
}

static int do_io(uint8_t opcode, leint64_t *prplists, iova_t iova, size_t len, uint16_t nlb)
{
	struct nvme_rq *rq;
	struct nvme_cqe cqe;
	int ret;

	union nvme_cmd cmd = {
		.rw = (struct nvme_cmd_rw) {
			.opcode = opcode,
			.nsid = cpu_to_le32(nsid),
			.nlb = cpu_to_le16(nlb),
		},
	};

	if (nvme_map_prp(&ctrl, prplists, NPRPLISTS, &cmd, iova, len))
		return -1;

	rq = nvme_rq_acquire(sq);
	if (!rq)
		return -1;

	nvme_rq_exec(rq, &cmd);

	ret = nvme_rq_spin(rq, &cqe);

	nvme_rq_release(rq);

	return ret;
}

int main(int argc, char **argv)
{
	struct nvme_id_ctrl *id_ctrl;
	struct nvme_id_ns *id_ns;
	struct iommu_ctx *ictx;
	void *prppages, *wbuf, *rbuf;
	leint64_t *prplists;
	iova_t prplist_iova, wiova, riova;
	int pageshift, max_prps;
	size_t pagesize, lba_size, len;
	uint64_t max_xfer, nlba;
	uint16_t nlb;
	unsigned int i;

	setup_io(argc, argv);

	plan_tests(1);

	if (!nsid) {
		skip(1, "namespace identifier not set");
		goto out;
	}

	if (pgmap((void **)&id_ctrl, NVME_IDENTIFY_DATA_SIZE) < 0)
		err(1, "failed to map identify controller buffer");

	if (pgmap((void **)&id_ns, NVME_IDENTIFY_DATA_SIZE) < 0)
		err(1, "failed to map identify namespace buffer");

	if (identify_ctrl(id_ctrl)) {
		skip(1, "failed to identify controller");
		goto out;
	}

	if (identify_ns(id_ns)) {
		skip(1, "failed to identify namespace");
		goto out;
	}

	pageshift = __mps_to_pageshift(ctrl.config.mps);
	pagesize = __mps_to_pagesize(ctrl.config.mps);
	max_prps = 1 << (pageshift - 3);

	lba_size = 1ULL << id_ns->lbaf[id_ns->flbas & 0xf].ds;

	/*
	 * pick a length that requires (max_prps + 1) PRP entries, i.e. one more
	 * than fits in a single prplist page, so the mapping can only succeed if
	 * the list is chained across the two pages in @prplists.
	 */
	len = (size_t)(max_prps + 1) * pagesize;
	nlba = len / lba_size;

	if (id_ctrl->mdts && id_ctrl->mdts < 64)
		max_xfer = (1ULL << id_ctrl->mdts) * pagesize;
	else
		max_xfer = UINT64_MAX;

	if (len > max_xfer) {
		skip(1, "device MDTS too small to force multi-page prplist chaining");
		goto out;
	}

	if (nlba > le64_to_cpu(id_ns->nsze)) {
		skip(1, "namespace too small for multi-page prplist test");
		goto out;
	}

	if (!nlba || nlba - 1 > UINT16_MAX) {
		skip(1, "buffer size does not fit a single read/write command");
		goto out;
	}

	nlb = (uint16_t)(nlba - 1);

	pgunmap(id_ns, NVME_IDENTIFY_DATA_SIZE);
	pgunmap(id_ctrl, NVME_IDENTIFY_DATA_SIZE);

	ictx = __iommu_ctx(&ctrl);

	if (pgmapn(&prppages, NPRPLISTS, pagesize) < 0)
		err(1, "failed to map prplist pages");

	if (iommu_map_vaddr(ictx, prppages, NPRPLISTS * pagesize, &prplist_iova, 0x0))
		err(1, "failed to map prplist pages into the iommu");

	prplists = prppages;

	wbuf = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (wbuf == MAP_FAILED)
		err(1, "failed to map write buffer");

	rbuf = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (rbuf == MAP_FAILED)
		err(1, "failed to map read buffer");

	if (iommu_map_vaddr(ictx, wbuf, len, &wiova, 0x0))
		err(1, "failed to map write buffer into the iommu");

	if (iommu_map_vaddr(ictx, rbuf, len, &riova, 0x0))
		err(1, "failed to map read buffer into the iommu");

	/* fill the write buffer with a recognizable, non-repeating pattern */
	for (i = 0; i < len; i++)
		((uint8_t *)wbuf)[i] = (uint8_t)(i ^ (i >> 8));

	memset(rbuf, 0, len);
	memset(prplists, 0x0, NPRPLISTS * pagesize);

	if (do_io(nvme_cmd_write, prplists, wiova, len, nlb))
		err(1, "write command failed");

	/* reset the prplist pages so the read exercises chaining afresh */
	memset(prplists, 0x0, NPRPLISTS * pagesize);

	if (do_io(nvme_cmd_read, prplists, riova, len, nlb))
		err(1, "read command failed");

	ok(!memcmp(wbuf, rbuf, len),
	   "data written and read back through a chained multi-page prplist matches");

out:
	teardown();

	return 0;
}
