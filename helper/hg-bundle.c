#include "git-compat-util.h"
#include "hg-bundle.h"
#include "hg-connect-internal.h"
#include <stdint.h>

size_t write_data(const unsigned char *buf, size_t size,
		  struct bundle_writer *out)
{
	switch (out->type) {
	case WRITER_FILE:
		return fwrite(buf, 1, size, out->out.file);
	case WRITER_STRBUF:
		strbuf_add(out->out.buf, buf, size);
		return size;
	default:
		return 0;
	}
}

size_t copy_data(size_t len, FILE *in, struct bundle_writer *out)
{
	unsigned char buf[4096];
	size_t ret = len;
	while (len) {
		uint32_t sz = len > sizeof(buf) ? sizeof(buf) : len;
		fread(buf, 1, sz, in);
		write_data(buf, sz, out);
		len -= sz;
	}
	return ret;
}

static size_t copy_chunk(int adjust, FILE *in, struct bundle_writer *out)
{
	unsigned char buf[4];
	const unsigned char *p = buf;
	uint32_t len;
	size_t ret = 0;
	//TODO: Check for errors, etc.
	fread(buf, 1, 4, in);
	write_data(buf, 4, out);
	len = get_be32(p);
	if (len <= adjust)
		//TODO: len != 0 is actually invalid
		return 0;
	ret = len -= adjust;
	copy_data(len, in, out);
	return ret;
}

static size_t copy_changegroup_chunk(FILE *in, struct bundle_writer *out)
{
	return copy_chunk(4, in, out);
}

static void copy_changegroup(FILE *in, struct bundle_writer *out)
{
	/* changesets */
	while (copy_changegroup_chunk(in, out)) {}
	/* manifests */
	while (copy_changegroup_chunk(in, out)) {}
	/* files */
	while (copy_changegroup_chunk(in, out)) {
		while (copy_changegroup_chunk(in, out)) {}
	}
}

static size_t copy_bundle2_chunk(FILE *in, struct bundle_writer *out)
{
	return copy_chunk(0, in, out);
}

static void copy_bundle_internal(FILE *in, struct bundle_writer *out)
{
	unsigned char buf[4];
	const unsigned char *p = buf;
	//TODO: Check for errors, etc.
	fread(buf, 1, 4, in);
	write_data(buf, 4, out);
	if (memcmp(buf, "HG20", 4)) {
		copy_data(get_be32(p) - 4, in, out);
		copy_changegroup(in, out);
		return;
	}
	// bundle2 parameters
	copy_bundle2_chunk(in, out);
	// bundle2 parts
	while (copy_bundle2_chunk(in, out)) {
		while (copy_bundle2_chunk(in, out)) {}
	}
}

void copy_bundle(FILE *in, FILE *out)
{
	struct bundle_writer writer;
	writer.type = WRITER_FILE;
	writer.out.file = out;
	copy_bundle_internal(in, &writer);
}

void copy_bundle_to_strbuf(FILE *in, struct strbuf *out)
{
	struct bundle_writer writer;
	writer.type = WRITER_STRBUF;
	writer.out.buf = out;
	copy_bundle_internal(in, &writer);
}

void rev_chunk_from_memory(struct rev_chunk *result, struct strbuf *buf,
                           const struct hg_object_id *delta_node)
{
	size_t data_offset = 80 + 20 * !!(delta_node == NULL);
	unsigned char *data = (unsigned char *) buf->buf;

	strbuf_swap(&result->raw, buf);
	if (result->raw.len < data_offset)
		die("Invalid revchunk");

	result->node = (const struct hg_object_id *)data;
	result->parent1 = (const struct hg_object_id *)(data + 20);
	result->parent2 = (const struct hg_object_id *)(data + 40);
	result->delta_node = delta_node ? delta_node
	                                : (const struct hg_object_id *)(data + 60);
/*	result->changeset = data + 60 + 20 * !!(delta_node == NULL); */
	result->diff_data = data + data_offset;
}

void rev_diff_start_iter(struct rev_diff_part *iterator,
                         struct rev_chunk *chunk)
{
	iterator->start = 0;
	iterator->end = 0;
	iterator->data.alloc = 0;
	iterator->data.len = 0;
	iterator->data.buf = NULL;
	iterator->chunk = chunk;
}

int rev_diff_iter_next(struct rev_diff_part *iterator)
{
	const char *part;
	const char *chunk_end = iterator->chunk->raw.buf +
	                        iterator->chunk->raw.len;

	if (iterator->data.buf == NULL)
		part = (char *) iterator->chunk->diff_data;
	else
		part = iterator->data.buf +
		       iterator->data.len;

	if (part == chunk_end)
		return 0;

	if (part > chunk_end - 12)
		die("Invalid revchunk");

	iterator->start = get_be32(part);
	iterator->end = get_be32(part + 4);
	iterator->data.len = get_be32(part + 8);
	iterator->data.buf = (char *) part + 12;

	if (iterator->data.buf + iterator->data.len > chunk_end ||
	    iterator->start > iterator->end)
		die("Invalid revchunk");

	return 1;
}
