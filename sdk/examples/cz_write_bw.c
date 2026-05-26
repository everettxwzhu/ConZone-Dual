// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L

#include "conzone_base.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
	const char *slc_dev;
	const char *tlc_dev;
	const char *media_arg;
	uint64_t total_bytes;
	size_t chunk_bytes;
	int reset;
} bench_opts_t;

static int usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <slc-dev> <tlc-dev> <slc|tlc|both> <total-mib> <chunk-kib> [--reset]\n",
		argv0);
	return 2;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static const char *media_name(cz_media_t media)
{
	return media == CZ_MEDIA_SLC ? "slc" : "tlc";
}

static int reset_all_zones(cz_handle_t *h, cz_media_t media)
{
	cz_zone_info_t *zones = NULL;
	size_t nr_zones = 0;
	int ret;

	ret = cz_report_zones(h, media, &zones, &nr_zones);
	if (ret)
		return ret;

	for (size_t i = 0; i < nr_zones; i++) {
		ret = cz_reset_zone(h, media, zones[i].zid);
		if (ret) {
			cz_free(zones);
			return ret;
		}
	}

	cz_free(zones);
	return 0;
}

static int run_one(cz_handle_t *h, const bench_opts_t *opts, cz_media_t media, void *buf)
{
	uint64_t remaining = opts->total_bytes;
	uint64_t object_id = 1;
	uint64_t written = 0;
	uint64_t start_ns;
	uint64_t end_ns;
	int ret;

	if (opts->reset) {
		ret = reset_all_zones(h, media);
		if (ret) {
			fprintf(stderr, "%s reset failed: %s\n", media_name(media), cz_strerror(ret));
			return ret;
		}
	}

	start_ns = now_ns();
	while (remaining > 0) {
		size_t len = opts->chunk_bytes;
		cz_extent_t ext;

		if (len > remaining)
			len = (size_t)remaining;
		len -= len % CZ_DEFAULT_LBA_SIZE;
		if (!len)
			break;

		ret = cz_append(h, media, CZ_ANY_ZONE, object_id++, CZ_OBJECT_GENERIC, 0, buf, len,
				&ext);
		if (ret) {
			fprintf(stderr, "%s append failed after %" PRIu64 " bytes: %s\n",
				media_name(media), written, cz_strerror(ret));
			return ret;
		}

		written += len;
		remaining -= len;
	}
	end_ns = now_ns();

	double seconds = (double)(end_ns - start_ns) / 1000000000.0;
	double mib = (double)written / (1024.0 * 1024.0);
	double bw = seconds > 0.0 ? mib / seconds : 0.0;

	printf("%s,total_mib=%.2f,chunk_kib=%.2f,seconds=%.6f,bw_mib_s=%.2f\n",
	       media_name(media), mib, (double)opts->chunk_bytes / 1024.0, seconds, bw);
	return 0;
}

static int parse_args(int argc, char **argv, bench_opts_t *opts)
{
	if (argc < 6)
		return -EINVAL;

	opts->slc_dev = argv[1];
	opts->tlc_dev = argv[2];
	opts->media_arg = argv[3];
	opts->total_bytes = strtoull(argv[4], NULL, 0) * 1024ull * 1024ull;
	opts->chunk_bytes = (size_t)strtoull(argv[5], NULL, 0) * 1024ull;
	opts->reset = 0;

	if (!opts->total_bytes || !opts->chunk_bytes ||
	    (opts->chunk_bytes % CZ_DEFAULT_LBA_SIZE) != 0)
		return -EINVAL;

	for (int i = 6; i < argc; i++) {
		if (strcmp(argv[i], "--reset") == 0) {
			opts->reset = 1;
		} else {
			return -EINVAL;
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	bench_opts_t opts;
	cz_open_opts_t open_opts;
	cz_handle_t *h = NULL;
	void *buf = NULL;
	int ret;

	ret = parse_args(argc, argv, &opts);
	if (ret)
		return usage(argv[0]);

	ret = posix_memalign(&buf, 4096, opts.chunk_bytes);
	if (ret)
		return 1;
	memset(buf, 0x5a, opts.chunk_bytes);

	open_opts = (cz_open_opts_t){
		.slc_path = opts.slc_dev,
		.tlc_path = opts.tlc_dev,
		.lba_size = CZ_DEFAULT_LBA_SIZE,
	};

	ret = cz_open(&open_opts, &h);
	if (ret) {
		fprintf(stderr, "cz_open failed: %s\n", cz_strerror(ret));
		goto out;
	}

	if (strcmp(opts.media_arg, "slc") == 0) {
		ret = run_one(h, &opts, CZ_MEDIA_SLC, buf);
	} else if (strcmp(opts.media_arg, "tlc") == 0) {
		ret = run_one(h, &opts, CZ_MEDIA_TLC, buf);
	} else if (strcmp(opts.media_arg, "both") == 0) {
		ret = run_one(h, &opts, CZ_MEDIA_SLC, buf);
		if (!ret)
			ret = run_one(h, &opts, CZ_MEDIA_TLC, buf);
	} else {
		ret = usage(argv[0]);
	}

out:
	cz_close(h);
	free(buf);
	return ret ? 1 : 0;
}
