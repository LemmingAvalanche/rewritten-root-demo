#include "cache.h"
#include "chunk-format.h"
#include "csum-file.h"

/*
 * When writing a chunk-based file format, collect the chunks in
 * an array of chunk_info structs. The size stores the _expected_
 * amount of data that will be written by write_fn.
 */
struct chunk_info {
	uint32_t id;
	uint64_t size;
	chunk_write_fn write_fn;

	const void *start;
};

struct chunkfile {
	struct hashfile *f;

	struct chunk_info *chunks;
	size_t chunks_nr;
	size_t chunks_alloc;
};

struct chunkfile *init_chunkfile(struct hashfile *f)
{
	struct chunkfile *cf = xcalloc(1, sizeof(*cf));
	cf->f = f;
	return cf;
}

void free_chunkfile(struct chunkfile *cf)
{
	if (!cf)
		return;
	free(cf->chunks);
	free(cf);
}

int get_num_chunks(struct chunkfile *cf)
{
	return cf->chunks_nr;
}

void add_chunk(struct chunkfile *cf,
	       uint32_t id,
	       size_t size,
	       chunk_write_fn fn)
{
	ALLOC_GROW(cf->chunks, cf->chunks_nr + 1, cf->chunks_alloc);

	cf->chunks[cf->chunks_nr].id = id;
	cf->chunks[cf->chunks_nr].write_fn = fn;
	cf->chunks[cf->chunks_nr].size = size;
	cf->chunks_nr++;
}

int write_chunkfile(struct chunkfile *cf, void *data)
{
	int i, result = 0;
	uint64_t cur_offset = hashfile_total(cf->f);

	trace2_region_enter("chunkfile", "write", the_repository);

	/* Add the table of contents to the current offset */
	cur_offset += (cf->chunks_nr + 1) * CHUNK_TOC_ENTRY_SIZE;

	for (i = 0; i < cf->chunks_nr; i++) {
		hashwrite_be32(cf->f, cf->chunks[i].id);
		hashwrite_be64(cf->f, cur_offset);

		cur_offset += cf->chunks[i].size;
	}

	/* Trailing entry marks the end of the chunks */
	hashwrite_be32(cf->f, 0);
	hashwrite_be64(cf->f, cur_offset);

	for (i = 0; i < cf->chunks_nr; i++) {
		off_t start_offset = hashfile_total(cf->f);
		result = cf->chunks[i].write_fn(cf->f, data);

		if (result)
			goto cleanup;

		if (hashfile_total(cf->f) - start_offset != cf->chunks[i].size)
			BUG("expected to write %"PRId64" bytes to chunk %"PRIx32", but wrote %"PRId64" instead",
			    cf->chunks[i].size, cf->chunks[i].id,
			    hashfile_total(cf->f) - start_offset);
	}

cleanup:
	trace2_region_leave("chunkfile", "write", the_repository);
	return result;
}

int read_table_of_contents(struct chunkfile *cf,
			   const unsigned char *mfile,
			   size_t mfile_size,
			   uint64_t toc_offset,
			   int toc_length)
{
	int i;
	uint32_t chunk_id;
	const unsigned char *table_of_contents = mfile + toc_offset;

	ALLOC_GROW(cf->chunks, toc_length, cf->chunks_alloc);

	while (toc_length--) {
		uint64_t chunk_offset, next_chunk_offset;

		chunk_id = get_be32(table_of_contents);
		chunk_offset = get_be64(table_of_contents + 4);

		if (!chunk_id) {
			error(_("terminating chunk id appears earlier than expected"));
			return 1;
		}

		table_of_contents += CHUNK_TOC_ENTRY_SIZE;
		next_chunk_offset = get_be64(table_of_contents + 4);

		if (next_chunk_offset < chunk_offset ||
		    next_chunk_offset > mfile_size - the_hash_algo->rawsz) {
			error(_("improper chunk offset(s) %"PRIx64" and %"PRIx64""),
			      chunk_offset, next_chunk_offset);
			return -1;
		}

		for (i = 0; i < cf->chunks_nr; i++) {
			if (cf->chunks[i].id == chunk_id) {
				error(_("duplicate chunk ID %"PRIx32" found"),
					chunk_id);
				return -1;
			}
		}

		cf->chunks[cf->chunks_nr].id = chunk_id;
		cf->chunks[cf->chunks_nr].start = mfile + chunk_offset;
		cf->chunks[cf->chunks_nr].size = next_chunk_offset - chunk_offset;
		cf->chunks_nr++;
	}

	chunk_id = get_be32(table_of_contents);
	if (chunk_id) {
		error(_("final chunk has non-zero id %"PRIx32""), chunk_id);
		return -1;
	}

	return 0;
}

static int pair_chunk_fn(const unsigned char *chunk_start,
			 size_t chunk_size,
			 void *data)
{
	const unsigned char **p = data;
	*p = chunk_start;
	return 0;
}

int pair_chunk(struct chunkfile *cf,
	       uint32_t chunk_id,
	       const unsigned char **p)
{
	return read_chunk(cf, chunk_id, pair_chunk_fn, p);
}

int read_chunk(struct chunkfile *cf,
	       uint32_t chunk_id,
	       chunk_read_fn fn,
	       void *data)
{
	int i;

	for (i = 0; i < cf->chunks_nr; i++) {
		if (cf->chunks[i].id == chunk_id)
			return fn(cf->chunks[i].start, cf->chunks[i].size, data);
	}

	return CHUNK_NOT_FOUND;
}
