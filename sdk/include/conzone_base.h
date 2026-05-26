// SPDX-License-Identifier: GPL-2.0-only
#ifndef CONZONE_BASE_H
#define CONZONE_BASE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CZ_DEFAULT_LBA_SIZE 512U
#define CZ_ANY_ZONE UINT64_MAX

typedef enum {
	CZ_MEDIA_SLC = 0,
	CZ_MEDIA_TLC = 1,
	CZ_MEDIA_COUNT = 2,
} cz_media_t;

typedef enum {
	CZ_OBJECT_GENERIC = 0,
	CZ_OBJECT_PARAM = 1,
	CZ_OBJECT_EXPERT = 2,
	CZ_OBJECT_KV = 3,
} cz_object_type_t;

typedef enum {
	CZ_MIGRATE_DEVICE = 1u << 0,
	CZ_MIGRATE_HOST_COPY = 1u << 1,
	CZ_MIGRATE_ALLOW_FALLBACK = 1u << 2,
} cz_migrate_flags_t;

typedef struct {
	cz_media_t media;
	uint64_t zid;
	uint64_t slba;
	uint64_t nlb;
	uint64_t object_id;
	uint32_t object_type;
	uint32_t flags;
	uint64_t generation;
} cz_extent_t;

typedef struct {
	uint64_t zid;
	uint64_t zslba;
	uint64_t wp;
	uint64_t capacity_lba;
	uint64_t size_lba;
	uint8_t type;
	uint8_t state;
} cz_zone_info_t;

typedef struct {
	uint64_t host_read_lbas;
	uint64_t host_write_lbas;
	uint64_t host_copy_read_lbas;
	uint64_t host_copy_write_lbas;
	uint64_t device_copy_lbas;
	uint64_t slc_to_tlc_lbas;
	uint64_t tlc_to_slc_lbas;
	uint64_t zone_reset_count[CZ_MEDIA_COUNT];
	uint64_t invalidated_lbas;
} cz_stats_t;

typedef struct {
	const char *slc_path;
	const char *tlc_path;
	uint32_t lba_size;
	const char *meta_path;
} cz_open_opts_t;

typedef struct cz_handle cz_handle_t;

int cz_open(const cz_open_opts_t *opts, cz_handle_t **out);
void cz_close(cz_handle_t *h);
const char *cz_strerror(int err);

int cz_report_zones(cz_handle_t *h, cz_media_t media, cz_zone_info_t **zones,
		    size_t *nr_zones);
void cz_free(void *ptr);

int cz_open_zone(cz_handle_t *h, cz_media_t media, uint64_t zid);
int cz_close_zone(cz_handle_t *h, cz_media_t media, uint64_t zid);
int cz_finish_zone(cz_handle_t *h, cz_media_t media, uint64_t zid);
int cz_reset_zone(cz_handle_t *h, cz_media_t media, uint64_t zid);

int cz_append(cz_handle_t *h, cz_media_t media, uint64_t zid_hint, uint64_t object_id,
	      uint32_t object_type, uint32_t object_flags, const void *buf, size_t len,
	      cz_extent_t *out);
int cz_read(cz_handle_t *h, const cz_extent_t *ext, uint64_t offset_bytes, void *buf,
	    size_t len);

int cz_migrate(cz_handle_t *h, const cz_extent_t *src, cz_media_t dst_media,
	       uint64_t dst_zid_hint, uint32_t migrate_flags, cz_extent_t *dst);
int cz_migrate_device(cz_handle_t *h, const cz_extent_t *src, cz_media_t dst_media,
		      uint64_t dst_zid_hint, cz_extent_t *dst);
int cz_migrate_host_copy(cz_handle_t *h, const cz_extent_t *src, cz_media_t dst_media,
			 uint64_t dst_zid_hint, size_t chunk_bytes, cz_extent_t *dst);

int cz_invalidate(cz_handle_t *h, const cz_extent_t *ext);
int cz_zone_live_bytes(cz_handle_t *h, cz_media_t media, uint64_t zid, uint64_t *live_bytes);
int cz_get_stats(cz_handle_t *h, cz_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
