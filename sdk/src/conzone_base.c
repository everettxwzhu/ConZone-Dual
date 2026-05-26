// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE

#include "conzone_base.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/nvme_ioctl.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CZ_OPC_READ 0x02
#define CZ_OPC_WRITE 0x01
#define CZ_OPC_ZONE_MGMT_SEND 0x79
#define CZ_OPC_ZONE_MGMT_RECV 0x7a
#define CZ_OPC_ZONE_APPEND 0x7d
#define CZ_OPC_CROSS_NS_COPY 0xc0

#define CZ_ZSA_CLOSE 0x01
#define CZ_ZSA_FINISH 0x02
#define CZ_ZSA_OPEN 0x03
#define CZ_ZSA_RESET 0x04
#define CZ_ZSA_SELECT_ALL (1U << 8)

/*
 * The current simulator reports MDTS=6, so one passthrough I/O command must not
 * exceed 4KiB * 2^6 = 256KiB. The block layer may split large fio requests, but
 * NVME_IOCTL_IO64_CMD passthrough does not.
 */
#define CZ_MAX_CMD_LBAS 512U
#define CZ_DEFAULT_COPY_CHUNK (4U * 1024U * 1024U)

struct cz_nvme_zone_desc {
	uint8_t zt;
	uint8_t zs;
	uint8_t za;
	uint8_t zai;
	uint32_t rsvd0;
	uint64_t zcap;
	uint64_t zslba;
	uint64_t wp;
	uint8_t rsvd1[32];
} __attribute__((packed));

struct cz_nvme_zone_report {
	uint64_t nr_zones;
	uint64_t rsvd[7];
	struct cz_nvme_zone_desc zd[];
} __attribute__((packed));

struct cz_meta_ent {
	cz_extent_t ext;
	bool valid;
};

struct cz_device {
	int fd;
	uint32_t nsid;
	char *path;
	cz_zone_info_t *zones;
	uint64_t *live_lbas;
	size_t nr_zones;
};

struct cz_handle {
	struct cz_device dev[CZ_MEDIA_COUNT];
	uint32_t lba_size;
	struct cz_meta_ent *meta;
	size_t meta_len;
	size_t meta_cap;
	uint64_t next_generation;
	FILE *journal;
	cz_stats_t stats;
};

static int cz_errno(void)
{
	return errno ? -errno : -EIO;
}

const char *cz_strerror(int err)
{
	if (err >= 0)
		return "success";
	return strerror(-err);
}

void cz_free(void *ptr)
{
	free(ptr);
}

static bool cz_valid_media(cz_media_t media)
{
	return media == CZ_MEDIA_SLC || media == CZ_MEDIA_TLC;
}

static struct cz_device *cz_dev(cz_handle_t *h, cz_media_t media)
{
	if (!h || !cz_valid_media(media))
		return NULL;
	return &h->dev[media];
}

static int cz_nsid_from_fd(int fd, uint32_t *nsid)
{
	int ret = ioctl(fd, NVME_IOCTL_ID);
	if (ret < 0)
		return cz_errno();
	*nsid = (uint32_t)ret;
	return 0;
}

static int cz_passthru64(int fd, struct nvme_passthru_cmd64 *cmd)
{
	int ret = ioctl(fd, NVME_IOCTL_IO64_CMD, cmd);
	if (ret < 0)
		return cz_errno();
	if (ret != 0)
		return -EIO;
	return 0;
}

static int cz_report_zones_refresh(cz_handle_t *h, cz_media_t media)
{
	struct cz_device *dev = cz_dev(h, media);
	struct nvme_passthru_cmd64 cmd;
	struct cz_nvme_zone_report *report = NULL;
	size_t report_len;
	uint64_t nr_zones;
	int ret;

	if (!dev)
		return -EINVAL;

	report_len = sizeof(*report);
	report = calloc(1, report_len);
	if (!report)
		return -ENOMEM;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = CZ_OPC_ZONE_MGMT_RECV;
	cmd.nsid = dev->nsid;
	cmd.addr = (uintptr_t)report;
	cmd.data_len = report_len;
	cmd.cdw12 = (uint32_t)(report_len / 4 - 1);
	ret = cz_passthru64(dev->fd, &cmd);
	if (ret) {
		free(report);
		return ret;
	}

	nr_zones = report->nr_zones;
	free(report);

	report_len = sizeof(*report) + nr_zones * sizeof(struct cz_nvme_zone_desc);
	report = calloc(1, report_len);
	if (!report)
		return -ENOMEM;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = CZ_OPC_ZONE_MGMT_RECV;
	cmd.nsid = dev->nsid;
	cmd.addr = (uintptr_t)report;
	cmd.data_len = report_len;
	cmd.cdw12 = (uint32_t)(report_len / 4 - 1);
	ret = cz_passthru64(dev->fd, &cmd);
	if (ret) {
		free(report);
		return ret;
	}

	free(dev->zones);
	dev->zones = calloc(nr_zones, sizeof(*dev->zones));
	if (!dev->zones) {
		free(report);
		return -ENOMEM;
	}

	if (!dev->live_lbas) {
		dev->live_lbas = calloc(nr_zones, sizeof(*dev->live_lbas));
		if (!dev->live_lbas) {
			free(report);
			return -ENOMEM;
		}
	} else if (dev->nr_zones != nr_zones) {
		uint64_t *live = calloc(nr_zones, sizeof(*live));
		if (!live) {
			free(report);
			return -ENOMEM;
		}
		memcpy(live, dev->live_lbas,
		       sizeof(*live) * (dev->nr_zones < nr_zones ? dev->nr_zones : nr_zones));
		free(dev->live_lbas);
		dev->live_lbas = live;
	}

	dev->nr_zones = nr_zones;
	for (uint64_t i = 0; i < nr_zones; i++) {
		const struct cz_nvme_zone_desc *zd = &report->zd[i];
		dev->zones[i].zid = i;
		dev->zones[i].type = zd->zt & 0xf;
		dev->zones[i].state = (zd->zs >> 4) & 0xf;
		dev->zones[i].capacity_lba = zd->zcap;
		dev->zones[i].size_lba = i + 1 < nr_zones ? report->zd[i + 1].zslba - zd->zslba :
							 zd->zcap;
		dev->zones[i].zslba = zd->zslba;
		dev->zones[i].wp = zd->wp;
	}

	free(report);
	return 0;
}

static int cz_meta_journal(cz_handle_t *h, const char *fmt, ...)
{
	va_list ap;

	if (!h->journal)
		return 0;

	va_start(ap, fmt);
	vfprintf(h->journal, fmt, ap);
	va_end(ap);
	fflush(h->journal);
	return ferror(h->journal) ? -EIO : 0;
}

static int cz_meta_reserve(cz_handle_t *h)
{
	struct cz_meta_ent *next;
	size_t next_cap;

	if (h->meta_len < h->meta_cap)
		return 0;
	next_cap = h->meta_cap ? h->meta_cap * 2 : 128;
	next = realloc(h->meta, next_cap * sizeof(*h->meta));
	if (!next)
		return -ENOMEM;
	h->meta = next;
	h->meta_cap = next_cap;
	return 0;
}

static int cz_meta_add(cz_handle_t *h, const cz_extent_t *ext, bool journal)
{
	struct cz_device *dev;
	int ret;

	if (!ext || !cz_valid_media(ext->media))
		return -EINVAL;
	dev = &h->dev[ext->media];
	if (ext->zid >= dev->nr_zones)
		return -EINVAL;

	ret = cz_meta_reserve(h);
	if (ret)
		return ret;

	h->meta[h->meta_len].ext = *ext;
	h->meta[h->meta_len].valid = true;
	h->meta_len++;
	dev->live_lbas[ext->zid] += ext->nlb;

	if (journal)
		return cz_meta_journal(h, "E %" PRIu64 " %u %u %u %" PRIu64 " %" PRIu64
				       " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
				       ext->object_id, ext->object_type, ext->flags, ext->media,
				       ext->zid, ext->slba, ext->nlb, ext->generation);
	return 0;
}

static int cz_replay_meta(cz_handle_t *h, const char *path)
{
	FILE *f;
	char op;

	f = fopen(path, "r");
	if (!f) {
		if (errno == ENOENT)
			return 0;
		return cz_errno();
	}

	while (fscanf(f, " %c", &op) == 1) {
		if (op == 'E') {
			cz_extent_t ext = {0};
			unsigned media;
			if (fscanf(f, "%" SCNu64 " %u %u %u %" SCNu64 " %" SCNu64
				   " %" SCNu64 " %" SCNu64,
				   &ext.object_id, &ext.object_type, &ext.flags, &media,
				   &ext.zid, &ext.slba, &ext.nlb, &ext.generation) != 8)
				break;
			ext.media = (cz_media_t)media;
			(void)cz_meta_add(h, &ext, false);
			if (ext.generation >= h->next_generation)
				h->next_generation = ext.generation + 1;
		} else if (op == 'I') {
			cz_extent_t ext = {0};
			unsigned media;
			if (fscanf(f, "%u %" SCNu64 " %" SCNu64 " %" SCNu64,
				   &media, &ext.slba, &ext.nlb, &ext.generation) != 4)
				break;
			ext.media = (cz_media_t)media;
			(void)cz_invalidate(h, &ext);
		} else if (op == 'R') {
			unsigned media;
			uint64_t zid;
			if (fscanf(f, "%u %" SCNu64, &media, &zid) != 2)
				break;
			if (cz_valid_media((cz_media_t)media) && zid < h->dev[media].nr_zones) {
				for (size_t i = 0; i < h->meta_len; i++) {
					cz_extent_t *ext = &h->meta[i].ext;
					if (h->meta[i].valid && ext->media == (cz_media_t)media &&
					    ext->zid == zid)
						h->meta[i].valid = false;
				}
				h->dev[media].live_lbas[zid] = 0;
			}
		} else {
			int c;
			while ((c = fgetc(f)) != '\n' && c != EOF)
				;
		}
	}

	fclose(f);
	return 0;
}

int cz_open(const cz_open_opts_t *opts, cz_handle_t **out)
{
	cz_handle_t *h;
	int ret;

	if (!opts || !opts->slc_path || !opts->tlc_path || !out)
		return -EINVAL;

	h = calloc(1, sizeof(*h));
	if (!h)
		return -ENOMEM;
	for (cz_media_t media = CZ_MEDIA_SLC; media < CZ_MEDIA_COUNT; media++)
		h->dev[media].fd = -1;
	h->lba_size = opts->lba_size ? opts->lba_size : CZ_DEFAULT_LBA_SIZE;
	h->next_generation = 1;

	h->dev[CZ_MEDIA_SLC].fd = open(opts->slc_path, O_RDWR | O_CLOEXEC);
	if (h->dev[CZ_MEDIA_SLC].fd < 0) {
		ret = cz_errno();
		goto fail;
	}
	h->dev[CZ_MEDIA_TLC].fd = open(opts->tlc_path, O_RDWR | O_CLOEXEC);
	if (h->dev[CZ_MEDIA_TLC].fd < 0) {
		ret = cz_errno();
		goto fail;
	}
	h->dev[CZ_MEDIA_SLC].path = strdup(opts->slc_path);
	h->dev[CZ_MEDIA_TLC].path = strdup(opts->tlc_path);
	if (!h->dev[CZ_MEDIA_SLC].path || !h->dev[CZ_MEDIA_TLC].path) {
		ret = -ENOMEM;
		goto fail;
	}

	for (cz_media_t media = CZ_MEDIA_SLC; media < CZ_MEDIA_COUNT; media++) {
		ret = cz_nsid_from_fd(h->dev[media].fd, &h->dev[media].nsid);
		if (ret)
			goto fail;
		ret = cz_report_zones_refresh(h, media);
		if (ret)
			goto fail;
	}

	if (opts->meta_path) {
		ret = cz_replay_meta(h, opts->meta_path);
		if (ret)
			goto fail;
		h->journal = fopen(opts->meta_path, "a");
		if (!h->journal) {
			ret = cz_errno();
			goto fail;
		}
	}

	*out = h;
	return 0;

fail:
	cz_close(h);
	return ret;
}

void cz_close(cz_handle_t *h)
{
	if (!h)
		return;
	if (h->journal)
		fclose(h->journal);
	for (cz_media_t media = CZ_MEDIA_SLC; media < CZ_MEDIA_COUNT; media++) {
		if (h->dev[media].fd >= 0)
			close(h->dev[media].fd);
		free(h->dev[media].path);
		free(h->dev[media].zones);
		free(h->dev[media].live_lbas);
	}
	free(h->meta);
	free(h);
}

int cz_report_zones(cz_handle_t *h, cz_media_t media, cz_zone_info_t **zones,
		    size_t *nr_zones)
{
	struct cz_device *dev = cz_dev(h, media);
	cz_zone_info_t *copy;
	int ret;

	if (!dev || !zones || !nr_zones)
		return -EINVAL;
	ret = cz_report_zones_refresh(h, media);
	if (ret)
		return ret;

	copy = calloc(dev->nr_zones, sizeof(*copy));
	if (!copy)
		return -ENOMEM;
	memcpy(copy, dev->zones, dev->nr_zones * sizeof(*copy));
	*zones = copy;
	*nr_zones = dev->nr_zones;
	return 0;
}

static int cz_zone_mgmt(cz_handle_t *h, cz_media_t media, uint64_t zid, uint8_t action)
{
	struct cz_device *dev = cz_dev(h, media);
	struct nvme_passthru_cmd64 cmd;
	uint64_t slba;
	int ret;

	if (!dev)
		return -EINVAL;
	ret = cz_report_zones_refresh(h, media);
	if (ret)
		return ret;
	if (zid >= dev->nr_zones)
		return -EINVAL;
	slba = dev->zones[zid].zslba;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = CZ_OPC_ZONE_MGMT_SEND;
	cmd.nsid = dev->nsid;
	cmd.cdw10 = (uint32_t)slba;
	cmd.cdw11 = (uint32_t)(slba >> 32);
	cmd.cdw13 = action;
	ret = cz_passthru64(dev->fd, &cmd);
	if (ret)
		return ret;

	if (action == CZ_ZSA_RESET) {
		for (size_t i = 0; i < h->meta_len; i++) {
			cz_extent_t *ext = &h->meta[i].ext;
			if (h->meta[i].valid && ext->media == media && ext->zid == zid) {
				h->stats.invalidated_lbas += ext->nlb;
				h->meta[i].valid = false;
			}
		}
		dev->live_lbas[zid] = 0;
		h->stats.zone_reset_count[media]++;
		(void)cz_meta_journal(h, "R %u %" PRIu64 "\n", media, zid);
	}

	return cz_report_zones_refresh(h, media);
}

int cz_open_zone(cz_handle_t *h, cz_media_t media, uint64_t zid)
{
	return cz_zone_mgmt(h, media, zid, CZ_ZSA_OPEN);
}

int cz_close_zone(cz_handle_t *h, cz_media_t media, uint64_t zid)
{
	return cz_zone_mgmt(h, media, zid, CZ_ZSA_CLOSE);
}

int cz_finish_zone(cz_handle_t *h, cz_media_t media, uint64_t zid)
{
	return cz_zone_mgmt(h, media, zid, CZ_ZSA_FINISH);
}

int cz_reset_zone(cz_handle_t *h, cz_media_t media, uint64_t zid)
{
	return cz_zone_mgmt(h, media, zid, CZ_ZSA_RESET);
}

int cz_reset_all_zones(cz_handle_t *h, cz_media_t media)
{
	struct cz_device *dev = cz_dev(h, media);
	struct nvme_passthru_cmd64 cmd;
	int ret;

	if (!dev)
		return -EINVAL;
	ret = cz_report_zones_refresh(h, media);
	if (ret)
		return ret;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = CZ_OPC_ZONE_MGMT_SEND;
	cmd.nsid = dev->nsid;
	cmd.cdw13 = CZ_ZSA_RESET | CZ_ZSA_SELECT_ALL;
	ret = cz_passthru64(dev->fd, &cmd);
	if (ret)
		return ret;

	for (size_t i = 0; i < h->meta_len; i++) {
		cz_extent_t *ext = &h->meta[i].ext;
		if (h->meta[i].valid && ext->media == media) {
			h->stats.invalidated_lbas += ext->nlb;
			h->meta[i].valid = false;
		}
	}
	for (size_t i = 0; i < dev->nr_zones; i++) {
		dev->live_lbas[i] = 0;
		h->stats.zone_reset_count[media]++;
		(void)cz_meta_journal(h, "R %u %" PRIu64 "\n", media, (uint64_t)i);
	}

	return cz_report_zones_refresh(h, media);
}

static int cz_choose_zone(struct cz_device *dev, uint64_t zid_hint, uint64_t nlb,
			  uint64_t *zid)
{
	if (zid_hint != CZ_ANY_ZONE) {
		if (zid_hint >= dev->nr_zones)
			return -EINVAL;
		if (dev->zones[zid_hint].wp + nlb >
		    dev->zones[zid_hint].zslba + dev->zones[zid_hint].capacity_lba)
			return -ENOSPC;
		*zid = zid_hint;
		return 0;
	}

	for (size_t i = 0; i < dev->nr_zones; i++) {
		if (dev->zones[i].wp + nlb <= dev->zones[i].zslba + dev->zones[i].capacity_lba) {
			*zid = i;
			return 0;
		}
	}
	return -ENOSPC;
}

static int cz_rw_cmd(struct cz_device *dev, uint8_t opcode, uint32_t lba_size, uint64_t slba,
		     void *buf, uint64_t nlb)
{
	uint64_t done = 0;

	while (done < nlb) {
		uint64_t todo = nlb - done;
		struct nvme_passthru_cmd64 cmd;

		if (todo > CZ_MAX_CMD_LBAS)
			todo = CZ_MAX_CMD_LBAS;

		memset(&cmd, 0, sizeof(cmd));
		cmd.opcode = opcode;
		cmd.nsid = dev->nsid;
		cmd.addr = (uintptr_t)((char *)buf + done * lba_size);
		cmd.data_len = (uint32_t)(todo * lba_size);
		cmd.cdw10 = (uint32_t)(slba + done);
		cmd.cdw11 = (uint32_t)((slba + done) >> 32);
		cmd.cdw12 = (uint32_t)(todo - 1);

		int ret = cz_passthru64(dev->fd, &cmd);
		if (ret)
			return ret;
		done += todo;
	}
	return 0;
}

static int cz_append_raw(cz_handle_t *h, cz_media_t media, uint64_t zid_hint, const void *buf,
			 uint64_t nlb, uint64_t *start_slba, uint64_t *zid_out)
{
	struct cz_device *dev = cz_dev(h, media);
	uint64_t zid;
	uint64_t done = 0;
	int ret;

	if (!dev || !buf || !nlb || !start_slba || !zid_out)
		return -EINVAL;
	ret = cz_report_zones_refresh(h, media);
	if (ret)
		return ret;
	while (done < nlb) {
		uint64_t todo = nlb - done;
		uint64_t zone_slba;
		uint64_t zone_free;
		struct nvme_passthru_cmd64 cmd;

		if (todo > CZ_MAX_CMD_LBAS)
			todo = CZ_MAX_CMD_LBAS;
		ret = cz_choose_zone(dev, zid_hint, todo, &zid);
		if (ret)
			return ret;
		if (done == 0) {
			*start_slba = dev->zones[zid].wp;
			*zid_out = zid;
		}
		zone_free = dev->zones[zid].zslba + dev->zones[zid].capacity_lba -
			    dev->zones[zid].wp;
		if (todo > zone_free)
			todo = zone_free;
		zone_slba = dev->zones[zid].zslba;

		memset(&cmd, 0, sizeof(cmd));
		cmd.opcode = CZ_OPC_ZONE_APPEND;
		cmd.nsid = dev->nsid;
		cmd.addr = (uintptr_t)((const char *)buf + done * h->lba_size);
		cmd.data_len = (uint32_t)(todo * h->lba_size);
		cmd.cdw10 = (uint32_t)zone_slba;
		cmd.cdw11 = (uint32_t)(zone_slba >> 32);
		cmd.cdw12 = (uint32_t)(todo - 1);

		ret = cz_passthru64(dev->fd, &cmd);
		if (ret)
			return ret;
		dev->zones[zid].wp += todo;
		done += todo;
	}
	return cz_report_zones_refresh(h, media);
}

int cz_append(cz_handle_t *h, cz_media_t media, uint64_t zid_hint, uint64_t object_id,
	      uint32_t object_type, uint32_t object_flags, const void *buf, size_t len,
	      cz_extent_t *out)
{
	cz_extent_t ext = {0};
	uint64_t nlb;
	int ret;

	if (!h || !buf || !out || len == 0 || (len % h->lba_size) != 0)
		return -EINVAL;
	nlb = len / h->lba_size;

	ext.media = media;
	ext.object_id = object_id;
	ext.object_type = object_type;
	ext.flags = object_flags;
	ext.nlb = nlb;
	ext.generation = h->next_generation++;

	ret = cz_append_raw(h, media, zid_hint, buf, nlb, &ext.slba, &ext.zid);
	if (ret)
		return ret;
	ret = cz_meta_add(h, &ext, true);
	if (ret)
		return ret;

	h->stats.host_write_lbas += nlb;
	*out = ext;
	return 0;
}

int cz_read(cz_handle_t *h, const cz_extent_t *ext, uint64_t offset_bytes, void *buf, size_t len)
{
	struct cz_device *dev;
	uint64_t offset_lba;
	uint64_t nlb;
	int ret;

	if (!h || !ext || !buf || (offset_bytes % h->lba_size) != 0 ||
	    (len % h->lba_size) != 0)
		return -EINVAL;
	offset_lba = offset_bytes / h->lba_size;
	nlb = len / h->lba_size;
	if (offset_lba + nlb > ext->nlb)
		return -EINVAL;
	dev = cz_dev(h, ext->media);
	if (!dev)
		return -EINVAL;

	ret = cz_rw_cmd(dev, CZ_OPC_READ, h->lba_size, ext->slba + offset_lba, buf, nlb);
	if (!ret)
		h->stats.host_read_lbas += nlb;
	return ret;
}

int cz_migrate_device(cz_handle_t *h, const cz_extent_t *src, cz_media_t dst_media,
		      uint64_t dst_zid_hint, cz_extent_t *dst)
{
	struct cz_device *src_dev;
	struct cz_device *dst_dev;
	struct nvme_passthru_cmd64 cmd;
	cz_extent_t out = {0};
	int ret;

	if (!h || !src || !dst || !src->nlb)
		return -EINVAL;
	src_dev = cz_dev(h, src->media);
	dst_dev = cz_dev(h, dst_media);
	if (!src_dev || !dst_dev)
		return -EINVAL;

	ret = cz_report_zones_refresh(h, dst_media);
	if (ret)
		return ret;
	ret = cz_choose_zone(dst_dev, dst_zid_hint, src->nlb, &out.zid);
	if (ret)
		return ret;

	out.media = dst_media;
	out.slba = dst_dev->zones[out.zid].wp;
	out.nlb = src->nlb;
	out.object_id = src->object_id;
	out.object_type = src->object_type;
	out.flags = src->flags;
	out.generation = h->next_generation++;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = CZ_OPC_CROSS_NS_COPY;
	cmd.nsid = src_dev->nsid;
	cmd.cdw10 = (uint32_t)src->slba;
	cmd.cdw11 = (uint32_t)(src->slba >> 32);
	cmd.cdw12 = dst_dev->nsid;
	cmd.cdw13 = (uint32_t)out.slba;
	cmd.cdw14 = (uint32_t)(out.slba >> 32);
	cmd.cdw15 = (uint32_t)src->nlb;

	ret = cz_passthru64(src_dev->fd, &cmd);
	if (ret)
		return ret;

	ret = cz_report_zones_refresh(h, dst_media);
	if (ret)
		return ret;
	ret = cz_meta_add(h, &out, true);
	if (ret)
		return ret;

	h->stats.device_copy_lbas += src->nlb;
	if (src->media == CZ_MEDIA_SLC && dst_media == CZ_MEDIA_TLC)
		h->stats.slc_to_tlc_lbas += src->nlb;
	else if (src->media == CZ_MEDIA_TLC && dst_media == CZ_MEDIA_SLC)
		h->stats.tlc_to_slc_lbas += src->nlb;

	*dst = out;
	return 0;
}

int cz_migrate_host_copy(cz_handle_t *h, const cz_extent_t *src, cz_media_t dst_media,
			 uint64_t dst_zid_hint, size_t chunk_bytes, cz_extent_t *dst)
{
	struct cz_device *dst_dev;
	cz_extent_t out = {0};
	uint64_t copied = 0;
	void *buf;
	int ret;

	if (!h || !src || !dst || !src->nlb)
		return -EINVAL;
	if (chunk_bytes == 0)
		chunk_bytes = CZ_DEFAULT_COPY_CHUNK;
	chunk_bytes -= chunk_bytes % h->lba_size;
	if (!chunk_bytes)
		return -EINVAL;
	dst_dev = cz_dev(h, dst_media);
	if (!dst_dev)
		return -EINVAL;

	ret = cz_report_zones_refresh(h, dst_media);
	if (ret)
		return ret;
	ret = cz_choose_zone(dst_dev, dst_zid_hint, src->nlb, &out.zid);
	if (ret)
		return ret;

	out.media = dst_media;
	out.slba = dst_dev->zones[out.zid].wp;
	out.nlb = src->nlb;
	out.object_id = src->object_id;
	out.object_type = src->object_type;
	out.flags = src->flags;
	out.generation = h->next_generation++;

	buf = malloc(chunk_bytes);
	if (!buf)
		return -ENOMEM;

	while (copied < src->nlb) {
		uint64_t todo = src->nlb - copied;
		if (todo > chunk_bytes / h->lba_size)
			todo = chunk_bytes / h->lba_size;

		ret = cz_read(h, src, copied * h->lba_size, buf, todo * h->lba_size);
		if (ret)
			goto out_free;
		h->stats.host_copy_read_lbas += todo;

		uint64_t slba;
		uint64_t zid;
		ret = cz_append_raw(h, dst_media, out.zid, buf, todo, &slba, &zid);
		if (ret)
			goto out_free;
		if (zid != out.zid || slba != out.slba + copied) {
			ret = -EIO;
			goto out_free;
		}
		h->stats.host_copy_write_lbas += todo;
		copied += todo;
	}

	ret = cz_meta_add(h, &out, true);
	if (!ret) {
		if (src->media == CZ_MEDIA_SLC && dst_media == CZ_MEDIA_TLC)
			h->stats.slc_to_tlc_lbas += src->nlb;
		else if (src->media == CZ_MEDIA_TLC && dst_media == CZ_MEDIA_SLC)
			h->stats.tlc_to_slc_lbas += src->nlb;
		*dst = out;
	}

out_free:
	free(buf);
	return ret;
}

int cz_migrate(cz_handle_t *h, const cz_extent_t *src, cz_media_t dst_media,
	       uint64_t dst_zid_hint, uint32_t migrate_flags, cz_extent_t *dst)
{
	int ret = -EINVAL;

	if (migrate_flags & CZ_MIGRATE_DEVICE)
		ret = cz_migrate_device(h, src, dst_media, dst_zid_hint, dst);
	if (ret && (migrate_flags & CZ_MIGRATE_ALLOW_FALLBACK) &&
	    (migrate_flags & CZ_MIGRATE_HOST_COPY))
		ret = cz_migrate_host_copy(h, src, dst_media, dst_zid_hint, 0, dst);
	else if (!(migrate_flags & CZ_MIGRATE_DEVICE) && (migrate_flags & CZ_MIGRATE_HOST_COPY))
		ret = cz_migrate_host_copy(h, src, dst_media, dst_zid_hint, 0, dst);
	return ret;
}

int cz_invalidate(cz_handle_t *h, const cz_extent_t *ext)
{
	if (!h || !ext || !cz_valid_media(ext->media))
		return -EINVAL;

	for (size_t i = 0; i < h->meta_len; i++) {
		cz_extent_t *cur = &h->meta[i].ext;
		if (!h->meta[i].valid)
			continue;
		if (cur->media == ext->media && cur->slba == ext->slba && cur->nlb == ext->nlb &&
		    (!ext->generation || cur->generation == ext->generation)) {
			h->meta[i].valid = false;
			if (cur->zid < h->dev[cur->media].nr_zones &&
			    h->dev[cur->media].live_lbas[cur->zid] >= cur->nlb)
				h->dev[cur->media].live_lbas[cur->zid] -= cur->nlb;
			h->stats.invalidated_lbas += cur->nlb;
			return cz_meta_journal(h, "I %u %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
					       cur->media, cur->slba, cur->nlb, cur->generation);
		}
	}
	return -ENOENT;
}

int cz_zone_live_bytes(cz_handle_t *h, cz_media_t media, uint64_t zid, uint64_t *live_bytes)
{
	struct cz_device *dev = cz_dev(h, media);

	if (!dev || !live_bytes || zid >= dev->nr_zones)
		return -EINVAL;
	*live_bytes = dev->live_lbas[zid] * h->lba_size;
	return 0;
}

int cz_get_stats(cz_handle_t *h, cz_stats_t *stats)
{
	if (!h || !stats)
		return -EINVAL;
	*stats = h->stats;
	return 0;
}
