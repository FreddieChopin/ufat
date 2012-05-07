/* uFAT -- small flexible VFAT implementation
 * Copyright (C) 2012 TracMap Holdings Ltd
 *
 * Author: Daniel Beer <dlbeer@gmail.com>, www.dlbeer.co.nz
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <string.h>
#include "ufat.h"
#include "ufat_internal.h"

static int cache_flush(struct ufat *uf, unsigned int cache_index)
{
	struct ufat_cache_desc *d = &uf->cache_desc[cache_index];

	if (!(d->flags & UFAT_CACHE_FLAG_DIRTY) ||
	    !(d->flags & UFAT_CACHE_FLAG_PRESENT))
		return 0;

	if (uf->dev->write(uf->dev, d->index, 1,
			   ufat_cache_data(uf, cache_index)) < 0)
		return -UFAT_ERR_IO;

	uf->stat.write++;
	uf->stat.write_blocks++;

	/* If this block is part of the FAT, mirror it to the other FATs. Not
	 * a fatal error if this fails.
	 */
	if (d->index >= uf->bpb.fat_start &&
	    d->index < uf->bpb.fat_start + uf->bpb.fat_size) {
		unsigned int i;
		ufat_block_t b = d->index;

		for (i = 1; i < uf->bpb.fat_count; i++) {
			b += uf->bpb.fat_size;
			uf->dev->write(uf->dev, d->index, 1,
				       ufat_cache_data(uf, cache_index));

			uf->stat.write++;
			uf->stat.write_blocks++;
		}
	}

	d->flags &= ~UFAT_CACHE_FLAG_DIRTY;
	return 0;
}

int ufat_cache_open(struct ufat *uf, ufat_block_t blk_index)
{
	unsigned int i;
	int oldest = -1;
	int free = -1;
	int err;
	unsigned int oldest_age = 0;

	/* Scan the cache, looking for:
	 *
	 *   (a) the item, if we already have it.
	 *   (b) a free slot, if one exists.
	 *   (c) the oldest cache item.
	 */
	for (i = 0; i < uf->cache_size; i++) {
		struct ufat_cache_desc *d = &uf->cache_desc[i];
		unsigned int age = uf->next_seq - d->seq;

		if ((d->flags & UFAT_CACHE_FLAG_PRESENT) &&
		    d->index == blk_index) {
			d->seq = uf->next_seq++;
			uf->stat.cache_hit++;
			return i;
		}

		if (!(d->flags & UFAT_CACHE_FLAG_PRESENT))
			free = i;

		if (oldest < 0 || age > oldest_age) {
			oldest_age = age;
			oldest = i;
		}
	}

	/* We don't have the item. Find a place to put it. */
	if (free >= 0) {
		i = free;
	} else {
		err = cache_flush(uf, oldest);
		if (err < 0)
			return err;

		i = oldest;
	}

	/* Read it in */
	err = uf->dev->read(uf->dev, blk_index, 1, ufat_cache_data(uf, i));
	if (err < 0) {
		uf->cache_desc[i].flags = 0;
		return err;
	} else {
		struct ufat_cache_desc *d = &uf->cache_desc[i];

		d->flags = UFAT_CACHE_FLAG_PRESENT;
		d->index = blk_index;
		d->seq = uf->next_seq++;
	}

	uf->stat.cache_miss++;
	uf->stat.read++;
	uf->stat.read_blocks++;

	return i;
}

static int log2_exact(unsigned int e, unsigned int *ret)
{
	unsigned int count = 0;

	if (!e)
		return -1;

	while (e > 1) {
		if (e & 1)
			return -1;

		e >>= 1;
		count++;
	}

	*ret = count;
	return 0;
}

static int parse_bpb(unsigned int log2_bytes_per_block,
		     struct ufat_bpb *ufb, uint8_t *bpb)
{
	const uint16_t bytes_per_sector = r16(bpb + 0x00b);
	const uint8_t sectors_per_cluster = bpb[0x00d];
	const uint16_t reserved_sector_count = r16(bpb + 0x00e);
	const uint16_t root_entries = r16(bpb + 0x011);
	uint32_t sectors_per_fat = r16(bpb + 0x016);
	uint32_t total_logical_sectors = r16(bpb + 0x013);
	const uint8_t number_of_fats = bpb[0x010];
	const uint32_t root_cluster = r32(bpb + 0x02c);
	unsigned int log2_bytes_per_sector = 0;
	unsigned int log2_sectors_per_cluster = 0;
	const unsigned int root_sectors =
		(root_entries * UFAT_DIRENT_SIZE + bytes_per_sector - 1) /
		bytes_per_sector;

	/* Read and check BPB values */
	if (log2_bytes_per_block < 9)
		return -UFAT_ERR_BLOCK_SIZE;

	if (!total_logical_sectors)
		total_logical_sectors = r32(bpb + 0x020);
	if (!sectors_per_fat)
		sectors_per_fat = r32(bpb + 0x024);

	if (log2_exact(bytes_per_sector, &log2_bytes_per_sector) < 0 ||
	    log2_exact(sectors_per_cluster, &log2_sectors_per_cluster) < 0)
		return -UFAT_ERR_INVALID_BPB;

	if (r16(bpb + 0x1fe) != 0xaa55)
		return -UFAT_ERR_INVALID_BPB;

	/* Convert sectors to blocks */
	if (log2_bytes_per_block > log2_bytes_per_sector) {
		const unsigned int shift =
			log2_bytes_per_block - log2_bytes_per_sector;

		if (log2_sectors_per_cluster < shift)
			return -UFAT_ERR_BLOCK_ALIGNMENT;
		ufb->log2_blocks_per_cluster =
			log2_sectors_per_cluster - shift;

		if ((reserved_sector_count | sectors_per_fat | root_sectors) &
		    ((1 << shift) - 1))
			return -UFAT_ERR_BLOCK_ALIGNMENT;
		ufb->fat_start = reserved_sector_count >> shift;
		ufb->fat_size = sectors_per_fat >> shift;
		ufb->root_size = root_sectors >> shift;
	} else {
		const unsigned int shift =
			log2_bytes_per_sector - log2_bytes_per_block;

		ufb->log2_blocks_per_cluster =
			log2_sectors_per_cluster + shift;
		ufb->fat_start = reserved_sector_count << shift;
		ufb->fat_size = sectors_per_fat << shift;
		ufb->root_size = root_sectors << shift;
	}

	if (!number_of_fats)
		return -UFAT_ERR_INVALID_BPB;

	/* Various block-size independent values */
	ufb->fat_count = number_of_fats;
	ufb->num_clusters =
		((total_logical_sectors - reserved_sector_count -
		  sectors_per_fat * number_of_fats -
		  root_sectors) >>
		 log2_sectors_per_cluster) + 2;
	ufb->root_cluster = root_cluster & UFAT_CLUSTER_MASK;
	ufb->root_start = ufb->fat_start + ufb->fat_size * ufb->fat_count;
	ufb->cluster_start = ufb->root_start + ufb->root_size;

	/* Figure out filesystem type */
	if (!root_sectors) {
		ufb->type = UFAT_TYPE_FAT32;
	} else {
		ufb->root_cluster = 0;
		if (ufb->num_clusters < 4085)
			ufb->type = UFAT_TYPE_FAT12;
		else
			ufb->type = UFAT_TYPE_FAT16;
	}

	return 0;
}

static int read_bpb(struct ufat *uf)
{
	int idx;

	idx = ufat_cache_open(uf, 0);
	if (idx < 0)
		return idx;

	return parse_bpb(uf->dev->log2_block_size, &uf->bpb,
			 ufat_cache_data(uf, idx));
}

int ufat_open(struct ufat *uf, const struct ufat_device *dev)
{
	uf->dev = dev;

	uf->next_seq = 0;
	uf->cache_size = UFAT_CACHE_BYTES >> dev->log2_block_size;

	if (uf->cache_size > UFAT_CACHE_MAX_BLOCKS)
		uf->cache_size = UFAT_CACHE_MAX_BLOCKS;

	if (!uf->cache_size)
		return -UFAT_ERR_BLOCK_SIZE;

	memset(&uf->stat, 0, sizeof(uf->stat));
	memset(&uf->cache_desc, 0, sizeof(uf->cache_desc));

	return read_bpb(uf);
}

int ufat_sync(struct ufat *uf)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; i < uf->cache_size; i++) {
		int err = cache_flush(uf, i);

		if (err)
			ret = err;
	}

	return 0;
}

void ufat_close(struct ufat *uf)
{
	ufat_sync(uf);
}

const char *ufat_strerror(int err)
{
	static const char *const text[UFAT_MAX_ERR] = {
		[UFAT_OK] = "No error",
		[UFAT_ERR_IO] = "IO error",
		[UFAT_ERR_BLOCK_SIZE] = "Invalid block size",
		[UFAT_ERR_INVALID_BPB] = "Invalid BPB",
		[UFAT_ERR_BLOCK_ALIGNMENT] =
		"Filesystem is not aligned for this block size",
		[UFAT_ERR_INVALID_CLUSTER] = "Invalid cluster index",
		[UFAT_ERR_NAME_TOO_LONG] = "Filename too long",
		[UFAT_ERR_NOT_DIRECTORY] = "Not a directory",
		[UFAT_ERR_NOT_FILE] = "Not a file"
	};

	if (err < 0)
		err = -err;

	if (err >= UFAT_MAX_ERR)
		return "Invalid error code";

	return text[err];
}

static int read_fat12(struct ufat *uf, ufat_cluster_t index,
		      ufat_cluster_t *out)
{
	// FIXME
	return -1;
}

static int read_fat16(struct ufat *uf, ufat_cluster_t index,
		      ufat_cluster_t *out)
{
	const unsigned int shift = uf->dev->log2_block_size - 1;
	const unsigned int b = index >> shift;
	const unsigned int r = index & ((1 << shift) - 1);
	int i = ufat_cache_open(uf, uf->bpb.fat_start + b);
	uint16_t raw;

	if (i < 0)
		return i;

	raw = r16(ufat_cache_data(uf, i) + r * 2);

	if (raw >= 0xfff8) {
		*out = UFAT_CLUSTER_EOC;
		return 0;
	}

	if (raw >= 0xfff0) {
		*out = UFAT_CLUSTER_BAD;
		return 0;
	}

	*out = raw;
	return 0;
}

static int read_fat32(struct ufat *uf, ufat_cluster_t index,
		      ufat_cluster_t *out)
{
	const unsigned int shift = uf->dev->log2_block_size - 2;
	const unsigned int b = index >> shift;
	const unsigned int r = index & ((1 << shift) - 1);
	int i = ufat_cache_open(uf, uf->bpb.fat_start + b);
	uint32_t raw;

	if (i < 0)
		return i;

	raw = r32(ufat_cache_data(uf, i) + r * 4) & UFAT_CLUSTER_MASK;

	if (raw >= 0xffffff8) {
		*out = UFAT_CLUSTER_EOC;
		return 0;
	}

	if (raw >= 0xffffff0) {
		*out = UFAT_CLUSTER_BAD;
		return 0;
	}

	*out = raw;
	return 0;
}

int ufat_read_fat(struct ufat *uf, ufat_cluster_t index,
		  ufat_cluster_t *out)
{
	if (index >= uf->bpb.num_clusters)
		return -UFAT_ERR_INVALID_CLUSTER;

	switch (uf->bpb.type) {
	case UFAT_TYPE_FAT12: return read_fat12(uf, index, out);
	case UFAT_TYPE_FAT16: return read_fat16(uf, index, out);
	case UFAT_TYPE_FAT32: return read_fat32(uf, index, out);
	}

	return 0;
}
