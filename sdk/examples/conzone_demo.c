// SPDX-License-Identifier: GPL-2.0-only
#include "conzone_base.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(const char *argv0)
{
	fprintf(stderr, "usage: %s <slc-dev> <tlc-dev> <bytes>\n", argv0);
	return 2;
}

int main(int argc, char **argv)
{
	cz_handle_t *h = NULL;
	cz_extent_t slc_ext;
	cz_extent_t tlc_ext;
	cz_open_opts_t opts;
	size_t len;
	void *buf = NULL;
	int ret;

	if (argc != 4)
		return usage(argv[0]);

	len = strtoull(argv[3], NULL, 0);
	if (!len || len % CZ_DEFAULT_LBA_SIZE)
		return usage(argv[0]);

	buf = malloc(len);
	if (!buf)
		return 1;
	memset(buf, 0x5a, len);

	opts = (cz_open_opts_t){
		.slc_path = argv[1],
		.tlc_path = argv[2],
		.lba_size = CZ_DEFAULT_LBA_SIZE,
	};

	ret = cz_open(&opts, &h);
	if (ret)
		goto out;

	ret = cz_append(h, CZ_MEDIA_SLC, 0, 1, CZ_OBJECT_GENERIC, 0, buf, len, &slc_ext);
	if (ret)
		goto out;

	ret = cz_migrate(h, &slc_ext, CZ_MEDIA_TLC, 0,
			 CZ_MIGRATE_DEVICE | CZ_MIGRATE_HOST_COPY | CZ_MIGRATE_ALLOW_FALLBACK,
			 &tlc_ext);
	if (ret)
		goto out;

	printf("SLC extent: zid=%lu slba=%lu nlb=%lu\n", slc_ext.zid, slc_ext.slba,
	       slc_ext.nlb);
	printf("TLC extent: zid=%lu slba=%lu nlb=%lu\n", tlc_ext.zid, tlc_ext.slba,
	       tlc_ext.nlb);

out:
	if (ret)
		fprintf(stderr, "error: %s\n", cz_strerror(ret));
	cz_close(h);
	free(buf);
	return ret ? 1 : 0;
}
