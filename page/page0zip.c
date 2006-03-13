/******************************************************
Compressed page interface

(c) 2005 Innobase Oy

Created June 2005 by Marko Makela
*******************************************************/

#define THIS_MODULE
#include "page0zip.h"
#ifdef UNIV_NONINL
# include "page0zip.ic"
#endif
#undef THIS_MODULE
#include "page0page.h"
#include "mtr0log.h"
#include "ut0sort.h"
#include "dict0boot.h"
#include "dict0dict.h"
#include "btr0cur.h"
#include "page0types.h"
#include "zlib.h"

/* Please refer to ../include/page0zip.ic for a description of the
compressed page format. */

/* The infimum and supremum records are omitted from the compressed page.
On compress, we compare that the records are there, and on uncompress we
restore the records. */
static const byte infimum_extra[] = {
	0x01,			/* info_bits=0, n_owned=1 */
	0x00, 0x02		/* heap_no=0, status=2 */
	/* ?, ?	*/		/* next=(first user rec, or supremum) */
};
static const byte infimum_data[] = {
	0x69, 0x6e, 0x66, 0x69,
	0x6d, 0x75, 0x6d, 0x00	/* "infimum\0" */
};
static const byte supremum_extra_data[] = {
	/* 0x0?, */		/* info_bits=0, n_owned=1..8 */
	0x00, 0x0b,		/* heap_no=1, status=3 */
	0x00, 0x00,		/* next=0 */
	0x73, 0x75, 0x70, 0x72,
	0x65, 0x6d, 0x75, 0x6d	/* "supremum" */
};

/*****************************************************************
Gets the size of the compressed page trailer (the dense page directory),
including deleted records (the free list). */
UNIV_INLINE
ulint
page_zip_dir_size(
/*==============*/
						/* out: length of dense page
						directory, in bytes */
	const page_zip_des_t*	page_zip)	/* in: compressed page */
{
	/* Exclude the page infimum and supremum from the record count. */
	ulint	size = PAGE_ZIP_DIR_SLOT_SIZE
			* (page_dir_get_n_heap((page_t*) page_zip->data) - 2);
	ut_ad(page_zip->m_end + size < page_zip->size);
	return(size);
}

/*****************************************************************
Gets the size of the compressed page trailer (the dense page directory),
only including user records (excluding the free list). */
UNIV_INLINE
ulint
page_zip_dir_user_size(
/*===================*/
						/* out: length of dense page
						directory comprising existing
						records, in bytes */
	const page_zip_des_t*	page_zip)	/* in: compressed page */
{
	ulint	size = PAGE_ZIP_DIR_SLOT_SIZE
			* page_get_n_recs((page_t*) page_zip->data);
	ut_ad(size <= page_zip_dir_size(page_zip));
	return(size);
}

/*****************************************************************
Find the slot of the given non-free record in the dense page directory. */
UNIV_INLINE
byte*
page_zip_dir_find(
/*==============*/
						/* out: dense directory slot,
						or NULL if record not found */
	page_zip_des_t*	page_zip,		/* in: compressed page */
	ulint		offset)			/* in: offset of user record */
{
	byte*	slot;
	byte*	end;

	ut_ad(page_zip_simple_validate(page_zip));

	end = page_zip->data + page_zip->size;
	slot = end - page_zip_dir_user_size(page_zip);

	for (; slot < end; slot += PAGE_ZIP_DIR_SLOT_SIZE) {
		if ((mach_read_from_2(slot) & PAGE_ZIP_DIR_SLOT_MASK)
				== offset) {
			return(slot);
		}
	}

	return(NULL);
}

/*****************************************************************
Find the slot of the given free record in the dense page directory. */
UNIV_INLINE
byte*
page_zip_dir_find_free(
/*===================*/
						/* out: dense directory slot,
						or NULL if record not found */
	page_zip_des_t*	page_zip,		/* in: compressed page */
	ulint		offset)			/* in: offset of user record */
{
	byte*	slot;
	byte*	end;

	ut_ad(page_zip_simple_validate(page_zip));

	slot = end = page_zip->data + page_zip->size;
	slot -= page_zip_dir_size(page_zip);
	end -= page_zip_dir_user_size(page_zip);

	for (; slot < end; slot += PAGE_ZIP_DIR_SLOT_SIZE) {
		if ((mach_read_from_2(slot) & PAGE_ZIP_DIR_SLOT_MASK)
				== offset) {
			return(slot);
		}
	}

	return(NULL);
}

/*****************************************************************
Read a given slot in the dense page directory. */
UNIV_INLINE
ulint
page_zip_dir_get(
/*=============*/
						/* out: record offset
						on the uncompressed page,
						possibly ORed with
						PAGE_ZIP_DIR_SLOT_DEL or
						PAGE_ZIP_DIR_SLOT_OWNED */
	const page_zip_des_t*	page_zip,	/* in: compressed page */
	ulint			slot)		/* in: slot
						(0=first user record) */
{
	ut_ad(page_zip_simple_validate(page_zip));
	ut_ad(slot < page_zip_dir_size(page_zip) / PAGE_ZIP_DIR_SLOT_SIZE);
	return(mach_read_from_2(page_zip->data + page_zip->size
			- PAGE_ZIP_DIR_SLOT_SIZE * (slot + 1)));
}

/**************************************************************************
Encode the length of a fixed-length column. */
static
byte*
page_zip_fixed_field_encode(
/*========================*/
			/* out: buf + length of encoded val */
	byte*	buf,	/* in: pointer to buffer where to write */
	ulint	val)	/* in: value to write */
{
	ut_ad(val >= 2);

	if (UNIV_LIKELY(val < 126)) {
		/*
		0 = nullable variable field of at most 255 bytes length;
		1 = not null variable field of at most 255 bytes length;
		126 = nullable variable field with maximum length >255;
		127 = not null variable field with maximum length >255
		*/
		*buf++ = val;
	} else {
		*buf++ = 0x80 | val >> 8;
		*buf++ = 0xff & val;
	}

	return(buf);
}

/**************************************************************************
Write the index information for the compressed page. */
static
ulint
page_zip_fields_encode(
/*===================*/
				/* out: used size of buf */
	ulint		n,	/* in: number of fields to compress */
	dict_index_t*	index,	/* in: index comprising at least n fields */
	ulint		trx_id_pos,/* in: position of the trx_id column
				in the index, or ULINT_UNDEFINED if
				this is a non-leaf page */
	byte*		buf)	/* out: buffer of (n + 1) * 2 bytes */
{
	const byte*	buf_start	= buf;
	ulint		i;
	ulint		col;
	ulint		trx_id_col	= 0;
	/* sum of lengths of preceding non-nullable fixed fields, or 0 */
	ulint		fixed_sum	= 0;

	ut_ad(trx_id_pos == ULINT_UNDEFINED || trx_id_pos < n);

	for (i = col = 0; i < n; i++) {
		dict_field_t*	field = dict_index_get_nth_field(index, i);
		ulint		val;

		if (dtype_get_prtype(dict_col_get_type(
						dict_field_get_col(field)))
						& DATA_NOT_NULL) {
			val = 1; /* set the "not nullable" flag */
		} else {
			val = 0; /* nullable field */
		}

		if (!field->fixed_len) {
			/* variable-length field */

			dtype_t*	type = dict_col_get_type(
					dict_field_get_col(field));

			if (UNIV_UNLIKELY(dtype_get_len(type) > 255)
			    || UNIV_UNLIKELY(dtype_get_mtype(type)
							== DATA_BLOB)) {
				val |= 0x7e; /* max > 255 bytes */
			}

			if (fixed_sum) {
				/* write out the length of any
				preceding non-nullable fields */
				buf = page_zip_fixed_field_encode(buf,
						fixed_sum << 1 | 1);
				fixed_sum = 0;
				col++;
			}

			*buf++ = val;
			col++;
		} else if (val) {
			/* fixed-length non-nullable field */
			if (i && UNIV_UNLIKELY(i == trx_id_pos)) {
				if (fixed_sum) {
					/* Write out the length of any
					preceding non-nullable fields,
					and start a new trx_id column. */
					buf = page_zip_fixed_field_encode(buf,
							fixed_sum << 1 | 1);
				}

				fixed_sum = field->fixed_len;
				trx_id_col = ++col;
			} else {
				/* add to the sum */
				fixed_sum += field->fixed_len;
			}
		} else {
			/* fixed-length nullable field */

			if (fixed_sum) {
				/* write out the length of any
				preceding non-nullable fields */
				buf = page_zip_fixed_field_encode(buf,
						fixed_sum << 1 | 1);
				fixed_sum = 0;
				col++;
			}

			buf = page_zip_fixed_field_encode(buf,
						field->fixed_len << 1);
			col++;
		}
	}

	if (fixed_sum) {
		/* Write out the lengths of last fixed-length columns. */
		buf = page_zip_fixed_field_encode(buf, fixed_sum << 1 | 1);
	}

	if (trx_id_pos != ULINT_UNDEFINED) {
		/* Write out the position of the trx_id column */
		i = trx_id_col;
	} else {
		/* Write out the number of nullable fields */
		i = index->n_nullable;
	}

	if (i < 128) {
		*buf++ = i;
	} else {
		*buf++ = 0x80 | i >> 8;
		*buf++ = 0xff & i;
	}

	ut_ad((ulint) (buf - buf_start) <= (n + 2) * 2);
	return((ulint) (buf - buf_start));
}

/**************************************************************************
Populate the dense page directory from the sparse directory. */
static
void
page_zip_dir_encode(
/*================*/
	const page_t*	page,	/* in: compact page */
	byte*		buf,	/* in: pointer to dense page directory[-1];
				out: dense directory on compressed page */
	const rec_t**	recs)	/* in: pointer to an array of 0, or NULL;
				out: dense page directory sorted by ascending
				address (and heap_no) */
{
	byte*	rec;
	ulint	status;
	ulint	min_mark;
	ulint	heap_no;
	ulint	i;
	ulint	n_heap;
	ulint	offs;

	min_mark = 0;

	if (page_is_leaf(page)) {
		status = REC_STATUS_ORDINARY;
	} else {
		status = REC_STATUS_NODE_PTR;
		if (UNIV_UNLIKELY(mach_read_from_4((page_t*) page
					+ FIL_PAGE_PREV) == FIL_NULL)) {
			min_mark = REC_INFO_MIN_REC_FLAG;
		}
	}

	n_heap = page_dir_get_n_heap((page_t*) page);

	/* Traverse the list of stored records in the collation order,
	starting from the first user record. */

	rec = (page_t*) page + PAGE_NEW_INFIMUM, TRUE;

	i = 0;

	for (;;) {
		ulint	info_bits;
		offs = rec_get_next_offs(rec, TRUE);
		if (UNIV_UNLIKELY(offs == PAGE_NEW_SUPREMUM)) {
			break;
		}
		rec = (page_t*) page + offs;
		heap_no = rec_get_heap_no_new(rec);
		ut_a(heap_no >= 2); /* not infimum or supremum */
		ut_a(heap_no < n_heap);
		ut_a(offs < UNIV_PAGE_SIZE - PAGE_DIR);
		ut_a(offs >= PAGE_ZIP_START);
#if PAGE_ZIP_DIR_SLOT_MASK & UNIV_PAGE_SIZE
# error "PAGE_ZIP_DIR_SLOT_MASK & UNIV_PAGE_SIZE"
#endif
		if (UNIV_UNLIKELY(rec_get_n_owned_new(rec))) {
			offs |= PAGE_ZIP_DIR_SLOT_OWNED;
		}

		info_bits = rec_get_info_bits(rec, TRUE);
		if (UNIV_UNLIKELY(info_bits & REC_INFO_DELETED_FLAG)) {
			info_bits &= ~REC_INFO_DELETED_FLAG;
			offs |= PAGE_ZIP_DIR_SLOT_DEL;
		}
		ut_a(info_bits == min_mark);
		/* Only the smallest user record can have
		REC_INFO_MIN_REC_FLAG set. */
		min_mark = 0;

		mach_write_to_2(buf - PAGE_ZIP_DIR_SLOT_SIZE * ++i, offs);

		if (UNIV_LIKELY_NULL(recs)) {
			/* Ensure that each heap_no occurs at most once. */
			ut_a(!recs[heap_no - 2]);
			/* exclude infimum and supremum */
			recs[heap_no - 2] = rec;
		}

		ut_a(rec_get_status(rec) == status);
	}

	offs = page_header_get_field((page_t*) page, PAGE_FREE);

	/* Traverse the free list (of deleted records). */
	while (offs) {
		ut_ad(!(offs & ~PAGE_ZIP_DIR_SLOT_MASK));
		rec = (page_t*) page + offs;

		heap_no = rec_get_heap_no_new(rec);
		ut_a(heap_no >= 2); /* not infimum or supremum */
		ut_a(heap_no < n_heap);

		ut_a(!rec[-REC_N_NEW_EXTRA_BYTES]); /* info_bits and n_owned */
		ut_a(rec_get_status(rec) == status);

		mach_write_to_2(buf - PAGE_ZIP_DIR_SLOT_SIZE * ++i, offs);

		if (UNIV_LIKELY_NULL(recs)) {
			/* Ensure that each heap_no occurs at most once. */
			ut_a(!recs[heap_no - 2]);
			/* exclude infimum and supremum */
			recs[heap_no - 2] = rec;
		}

		offs = rec_get_next_offs(rec, TRUE);
	}

	/* Ensure that each heap no occurs at least once. */
	ut_a(i + 2/* infimum and supremum */ == n_heap);
}

/**************************************************************************
Compress a page. */

ibool
page_zip_compress(
/*==============*/
				/* out: TRUE on success, FALSE on failure;
				page_zip will be left intact on failure. */
	page_zip_des_t*	page_zip,/* in: size; out: data, n_blobs,
				m_start, m_end */
	const page_t*	page,	/* in: uncompressed page */
	dict_index_t*	index,	/* in: index of the B-tree node */
	mtr_t*		mtr)	/* in: mini-transaction handle,
				or NULL if no logging is needed */
{
	z_stream	c_stream;
	int		err;
	ulint		n_fields;/* number of index fields needed */
	byte*		fields;	/* index field information */
	byte*		buf;	/* compressed payload of the page */
	byte*		buf_end;/* end of buf */
	ulint		n_dense;
	const rec_t**	recs;	/* dense page directory, sorted by address */
	mem_heap_t*	heap;
	ulint		trx_id_col;
	ulint*		offsets	= NULL;
	ulint		n_blobs	= 0;
	byte*		storage;/* storage of uncompressed columns */

	ut_a(page_is_comp((page_t*) page));
	ut_ad(page_simple_validate_new((page_t*) page));
	ut_ad(page_zip_simple_validate(page_zip));

	/* Check the data that will be omitted. */
	ut_a(!memcmp(page + (PAGE_NEW_INFIMUM - REC_N_NEW_EXTRA_BYTES),
		     infimum_extra, sizeof infimum_extra));
	ut_a(!memcmp(page + PAGE_NEW_INFIMUM,
		     infimum_data, sizeof infimum_data));
	ut_a(page[PAGE_NEW_SUPREMUM - REC_N_NEW_EXTRA_BYTES]
				/* info_bits == 0, n_owned <= max */
				<= PAGE_DIR_SLOT_MAX_N_OWNED);
	ut_a(!memcmp(page + (PAGE_NEW_SUPREMUM - REC_N_NEW_EXTRA_BYTES + 1),
		     supremum_extra_data, sizeof supremum_extra_data));

	if (UNIV_UNLIKELY(!page_get_n_recs((page_t*) page))) {
		ut_a(rec_get_next_offs((page_t*) page + PAGE_NEW_INFIMUM, TRUE)
			== PAGE_NEW_SUPREMUM);
	}

	if (page_is_leaf(page)) {
		n_fields = dict_index_get_n_fields(index);
	} else {
		n_fields = dict_index_get_n_unique_in_tree(index);
	}

	/* The dense directory excludes the infimum and supremum records. */
	n_dense = page_dir_get_n_heap((page_t*) page) - 2;
	if (UNIV_UNLIKELY(n_dense * PAGE_ZIP_DIR_SLOT_SIZE
					>= page_zip->size)) {
		return(FALSE);
	}

	heap = mem_heap_create(page_zip->size
		+ n_fields * (2 + sizeof *offsets)
		+ n_dense * ((sizeof *recs) - PAGE_ZIP_DIR_SLOT_SIZE));

	recs = mem_heap_alloc(heap, n_dense * sizeof *recs);
	memset(recs, 0, n_dense * sizeof *recs);

	fields = mem_heap_alloc(heap, (n_fields + 1) * 2);

	buf = mem_heap_alloc(heap, page_zip->size - PAGE_DATA);
	buf_end = buf + page_zip->size - PAGE_DATA;

	/* Compress the data payload. */
	c_stream.zalloc = (alloc_func) 0;
	c_stream.zfree = (free_func) 0;
	c_stream.opaque = (voidpf) 0;

	err = deflateInit(&c_stream, Z_DEFAULT_COMPRESSION);
	ut_a(err == Z_OK);

	c_stream.next_out = buf;
	/* Subtract the space reserved for uncompressed data. */
	/* Page header and the end marker of the modification log */
	c_stream.avail_out = buf_end - buf - 1;
	/* Dense page directory and uncompressed columns, if any */
	if (page_is_leaf(page)) {
		if (dict_index_is_clust(index)) {
			trx_id_col = dict_index_get_sys_col_pos(
					index, DATA_TRX_ID);
			ut_ad(trx_id_col > 0);
			ut_ad(trx_id_col != ULINT_UNDEFINED);

			c_stream.avail_out -= n_dense * (PAGE_ZIP_DIR_SLOT_SIZE
				+ DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
		} else {
			/* Signal the absence of trx_id
			in page_zip_fields_encode() */
			ut_ad(dict_index_get_sys_col_pos(
					index, DATA_TRX_ID)
					== ULINT_UNDEFINED);
			trx_id_col = 0;
			c_stream.avail_out -= n_dense * PAGE_ZIP_DIR_SLOT_SIZE;
		}
	} else {
		c_stream.avail_out -= n_dense * (PAGE_ZIP_DIR_SLOT_SIZE
				+ REC_NODE_PTR_SIZE);
		trx_id_col = ULINT_UNDEFINED;
	}

	c_stream.avail_in = page_zip_fields_encode(
					n_fields, index, trx_id_col, fields);
	c_stream.next_in = fields;
	if (!trx_id_col) {
		trx_id_col = ULINT_UNDEFINED;
	}

	err = deflate(&c_stream, Z_FULL_FLUSH);
	if (err != Z_OK) {
		goto zlib_error;
	}

	ut_ad(!c_stream.avail_in);

	page_zip_dir_encode(page, buf_end, recs);

	c_stream.next_in = (byte*) page + PAGE_ZIP_START;

	storage = buf_end - n_dense * PAGE_ZIP_DIR_SLOT_SIZE;

	if (page_is_leaf(page)) {
		/* BTR_EXTERN_FIELD_REF storage */
		byte*	externs;
		if (UNIV_UNLIKELY(trx_id_col != ULINT_UNDEFINED)) {
			externs = storage - n_dense
				* (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
		} else {
			externs = storage;
		}

		while (n_dense--) {
			ulint	i;
			rec_t*	rec = (rec_t*) *recs++;

			offsets = rec_get_offsets(rec, index, offsets,
					ULINT_UNDEFINED, &heap);
			ut_ad(rec_offs_n_fields(offsets) == n_fields);

			/* Compress the extra bytes. */
			c_stream.avail_in = rec - REC_N_NEW_EXTRA_BYTES
					- c_stream.next_in;

			if (c_stream.avail_in) {
				err = deflate(&c_stream, Z_NO_FLUSH);
				if (err != Z_OK) {
					goto zlib_error;
				}
			}
			ut_ad(!c_stream.avail_in);
			ut_ad(c_stream.next_in == rec - REC_N_NEW_EXTRA_BYTES);

			/* Compress the data bytes. */

			c_stream.next_in = rec;

			/* Check if there are any externally stored columns.
			For each externally stored column, store the
			BTR_EXTERN_FIELD_REF separately._*/

			for (i = 0; i < n_fields; i++) {
				ulint	len;
				byte*	src;

				if (UNIV_UNLIKELY(i == trx_id_col)) {
				    ut_ad(!rec_offs_nth_extern(offsets, i));
				    /* Store trx_id and roll_ptr
				    in uncompressed form. */
				    src = rec_get_nth_field(rec, offsets,
								i, &len);
				    ut_ad(src + DATA_TRX_ID_LEN
						== rec_get_nth_field(
						rec, offsets, i + 1, &len));
				    ut_ad(len == DATA_ROLL_PTR_LEN);

				    /* Compress any preceding bytes. */
				    c_stream.avail_in = src - c_stream.next_in;

				    if (c_stream.avail_in) {
					err = deflate(&c_stream, Z_NO_FLUSH);
					if (err != Z_OK) {
					    goto zlib_error;
					}
				    }

				    ut_ad(!c_stream.avail_in);
				    ut_ad(c_stream.next_in == src);

				    memcpy(storage - (DATA_TRX_ID_LEN
							+ DATA_ROLL_PTR_LEN)
					* (rec_get_heap_no_new(rec) - 1),
					c_stream.next_in,
					DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

				    c_stream.next_in +=
					DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;

				    /* Skip also roll_ptr */
				    i++;
				} else if (rec_offs_nth_extern(offsets, i)) {
				    src = rec_get_nth_field(rec, offsets,
								i, &len);
				    ut_ad(len > BTR_EXTERN_FIELD_REF_SIZE);
				    src += len - BTR_EXTERN_FIELD_REF_SIZE;

				    c_stream.avail_in = src - c_stream.next_in;
				    ut_ad(c_stream.avail_in);
				    err = deflate(&c_stream, Z_NO_FLUSH);
				    if (err != Z_OK) {
					goto zlib_error;
				    }

				    ut_ad(!c_stream.avail_in);
				    ut_ad(c_stream.next_in == src);

				    /* Reserve space for the data at
				    the end of the space reserved for
				    the compressed data and the page
				    modification log. */

				    if (UNIV_UNLIKELY(c_stream.avail_out
					    <= BTR_EXTERN_FIELD_REF_SIZE)) {
					/* out of space */
					goto zlib_error;
				    }

				    c_stream.avail_out
						-= BTR_EXTERN_FIELD_REF_SIZE;
				    externs -= BTR_EXTERN_FIELD_REF_SIZE;

				    ut_ad(externs == c_stream.next_out
					+ c_stream.avail_out
					+ 1/* end of modification log */);

				    /* Copy the BLOB pointer */
				    memcpy(externs, c_stream.next_in,
						BTR_EXTERN_FIELD_REF_SIZE);
				    c_stream.next_in +=
						BTR_EXTERN_FIELD_REF_SIZE;
				    /* Increment the BLOB counter */
				    n_blobs++;
				}
			}

			/* Compress the last bytes of the record. */
			c_stream.avail_in = rec_get_end(rec, offsets)
					- c_stream.next_in;

			if (c_stream.avail_in) {
				err = deflate(&c_stream, Z_NO_FLUSH);
				if (err != Z_OK) {
					goto zlib_error;
				}
			}
			ut_ad(!c_stream.avail_in);
		}
	} else {
		/* This is a node pointer page. */
		while (n_dense--) {
			rec_t*	rec = (rec_t*) *recs++;

			offsets = rec_get_offsets(rec, index, offsets,
					ULINT_UNDEFINED, &heap);
			ut_ad(rec_offs_n_fields(offsets) == n_fields + 1);
			/* Non-leaf nodes should not have any externally
			stored columns. */
			ut_ad(!rec_offs_any_extern(offsets));

			/* Compress the extra bytes. */
			c_stream.avail_in = rec - REC_N_NEW_EXTRA_BYTES
					- c_stream.next_in;

			if (c_stream.avail_in) {
				err = deflate(&c_stream, Z_NO_FLUSH);
				if (err != Z_OK) {
					goto zlib_error;
				}
			}
			ut_ad(!c_stream.avail_in);

			/* Compress the data bytes, except node_ptr. */
			c_stream.next_in = rec;
			c_stream.avail_in = rec_offs_data_size(offsets)
					- REC_NODE_PTR_SIZE;
			ut_ad(c_stream.avail_in);

			err = deflate(&c_stream, Z_NO_FLUSH);
			if (err != Z_OK) {
				goto zlib_error;
			}

			ut_ad(!c_stream.avail_in);

			memcpy(storage - REC_NODE_PTR_SIZE
					* (rec_get_heap_no_new(rec) - 1),
					c_stream.next_in, REC_NODE_PTR_SIZE);
			c_stream.next_in += REC_NODE_PTR_SIZE;
		}
	}

	/* Finish the compression. */
	ut_ad(!c_stream.avail_in);
	/* Compress any trailing garbage, in case the last record was
	allocated from an originally longer space on the free list. */
	c_stream.avail_in = page_header_get_field(
				(page_t*) page, PAGE_HEAP_TOP)
				- (c_stream.next_in - page);
	ut_a(c_stream.avail_in <= UNIV_PAGE_SIZE - PAGE_ZIP_START - PAGE_DIR);

	err = deflate(&c_stream, Z_FINISH);

	if (UNIV_UNLIKELY(err != Z_STREAM_END)) {
zlib_error:
		deflateEnd(&c_stream);
		mem_heap_free(heap);
		return(FALSE);
	}

	err = deflateEnd(&c_stream);
	ut_a(err == Z_OK);

	ut_ad(buf + c_stream.total_out == c_stream.next_out);
	ut_ad((ulint) (storage - c_stream.next_out) >= c_stream.avail_out);

	/* Zero out the area reserved for the modification log.
	Space for the end marker of the modification log is not
	included in avail_out. */
	memset(c_stream.next_out, 0, c_stream.avail_out + 1/* end marker */);

	page_zip->m_end = page_zip->m_start = PAGE_DATA + c_stream.total_out;
	page_zip->n_blobs = n_blobs;
	/* Copy the page header */
	memcpy(page_zip->data, page, PAGE_DATA);
	/* Copy the rest of the compressed page */
	memcpy(page_zip->data + PAGE_DATA, buf, page_zip->size - PAGE_DATA);
	mem_heap_free(heap);
#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
	ut_a(page_zip_validate(page_zip, page));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */

	if (UNIV_LIKELY_NULL(mtr)) {
		mlog_open_and_write_index(mtr, (page_t*) page, index,
					MLOG_ZIP_COMPRESS, 0);
	}

	return(TRUE);
}

/**************************************************************************
Compare two page directory entries. */
UNIV_INLINE
ibool
page_zip_dir_cmp(
/*=============*/
				/* out: positive if rec1 > rec2 */
	const rec_t*	rec1,	/* in: rec1 */
	const rec_t*	rec2)	/* in: rec2 */
{
	return(rec1 > rec2);
}

/**************************************************************************
Sort the dense page directory by address (heap_no). */
static
void
page_zip_dir_sort(
/*==============*/
	rec_t**	arr,	/* in/out: dense page directory */
	rec_t**	aux_arr,/* in/out: work area */
	ulint	low,	/* in: lower bound of the sorting area, inclusive */
	ulint	high)	/* in: upper bound of the sorting area, exclusive */
{
	UT_SORT_FUNCTION_BODY(page_zip_dir_sort, arr, aux_arr, low, high,
							page_zip_dir_cmp);
}

/**************************************************************************
Deallocate the index information initialized by page_zip_fields_decode(). */
static
void
page_zip_fields_free(
/*=================*/
	dict_index_t*	index)	/* in: dummy index to be freed */
{
	if (index) {
		dict_table_t*	table = index->table;
		mem_heap_free(index->heap);
		mutex_free(&(table->autoinc_mutex));
		mem_heap_free(table->heap);
	}
}

/**************************************************************************
Read the index information for the compressed page. */
static
dict_index_t*
page_zip_fields_decode(
/*===================*/
				/* out,own: dummy index describing the page,
				or NULL on error */
	const byte*	buf,	/* in: index information */
	const byte*	end,	/* in: end of buf */
	ulint*		trx_id_col)/* in: NULL for non-leaf pages;
				for leaf pages, pointer to where to store
				the position of the trx_id column */
{
	const byte*	b;
	ulint		n;
	ulint		i;
	ulint		val;
	dict_table_t*	table;
	dict_index_t*	index;

	/* Determine the number of fields. */
	for (b = buf, n = 0; b < end; n++) {
		if (*b++ & 0x80) {
			b++; /* skip the second byte */
		}
	}

	n--; /* n_nullable or trx_id */

	if (UNIV_UNLIKELY(n > REC_MAX_N_FIELDS)
			|| UNIV_UNLIKELY(b > end)) {

		return(NULL);
	}

	table = dict_mem_table_create("ZIP_DUMMY", DICT_HDR_SPACE, n, TRUE);
	index = dict_mem_index_create("ZIP_DUMMY", "ZIP_DUMMY",
				DICT_HDR_SPACE, 0, n);
	index->table = table;
	index->n_uniq = n;
	/* avoid ut_ad(index->cached) in dict_index_get_n_unique_in_tree */
	index->cached = TRUE;

	/* Initialize the fields. */
	for (b = buf, i = 0; i < n; i++) {
		ulint	mtype;
		ulint	len;

		val = *b++;

		if (UNIV_UNLIKELY(val & 0x80)) {
			/* fixed length > 62 bytes */
			val = (val & 0x7f) << 8 | *b++;
			len = val >> 1;
			mtype = DATA_FIXBINARY;
		} else if (UNIV_UNLIKELY(val >= 126)) {
			/* variable length with max > 255 bytes */
			len = 0x7fff;
			mtype = DATA_BINARY;
		} else if (val <= 1) {
			/* variable length with max <= 255 bytes */
			len = 0;
			mtype = DATA_BINARY;
		} else {
			/* fixed length < 62 bytes */
			len = val >> 1;
			mtype = DATA_FIXBINARY;
		}

		dict_mem_table_add_col(table, "DUMMY", mtype,
				val & 1 ? DATA_NOT_NULL : 0, len, 0);
		dict_index_add_col(index,
				dict_table_get_nth_col(table, i), 0);
	}

	val = *b++;
	if (UNIV_UNLIKELY(val & 0x80)) {
		val = (val & 0x7f) << 8 | *b++;
	}

	/* Decode the position of the trx_id column. */
	if (trx_id_col) {
		if (UNIV_UNLIKELY(val >= n)) {
			page_zip_fields_free(index);
			index = NULL;
		}

		if (!val) {
			val = ULINT_UNDEFINED;
		}

		*trx_id_col = val;
	} else {
		/* Decode the number of nullable fields. */
		if (UNIV_UNLIKELY(index->n_nullable > val)) {
			page_zip_fields_free(index);
			index = NULL;
		} else {
			index->n_nullable = val;
		}
	}

	ut_ad(b == end);

	return(index);
}

/**************************************************************************
Populate the sparse page directory from the dense directory. */
static
ibool
page_zip_dir_decode(
/*================*/
					/* out: TRUE on success,
					FALSE on failure */
	const page_zip_des_t*	page_zip,/* in: dense page directory on
					compressed page */
	page_t*			page,	/* in: compact page with valid header;
					out: trailer and sparse page directory
					filled in */
	rec_t**			recs,	/* out: dense page directory sorted by
					ascending address (and heap_no) */
	rec_t**			recs_aux,/* in/out: scratch area */
	ulint			n_dense)/* in: number of user records, and
					size of recs[] and recs_aux[] */
{
	ulint	i;
	ulint	n_recs;
	byte*	slot;

	n_recs = page_get_n_recs(page);

	if (UNIV_UNLIKELY(n_recs > n_dense)) {
		return(FALSE);
	}

	/* Traverse the list of stored records in the sorting order,
	starting from the first user record. */

	slot = page + (UNIV_PAGE_SIZE - PAGE_DIR - PAGE_DIR_SLOT_SIZE);
	UNIV_PREFETCH_RW(slot);

	/* Zero out the page trailer. */
	memset(slot + PAGE_DIR_SLOT_SIZE, 0, PAGE_DIR);

	mach_write_to_2(slot, PAGE_NEW_INFIMUM);
	slot -= PAGE_DIR_SLOT_SIZE;
	UNIV_PREFETCH_RW(slot);

	/* Initialize the sparse directory and copy the dense directory. */
	for (i = 0; i < n_recs; i++) {
		ulint	offs = page_zip_dir_get(page_zip, i);

		if (offs & PAGE_ZIP_DIR_SLOT_OWNED) {
			mach_write_to_2(slot, offs & PAGE_ZIP_DIR_SLOT_MASK);
			slot -= PAGE_DIR_SLOT_SIZE;
			UNIV_PREFETCH_RW(slot);
		}

		ut_ad((offs & PAGE_ZIP_DIR_SLOT_MASK)
				>= PAGE_ZIP_START + REC_N_NEW_EXTRA_BYTES);

		recs[i] = page + (offs & PAGE_ZIP_DIR_SLOT_MASK);
	}

	mach_write_to_2(slot, PAGE_NEW_SUPREMUM);
	if (UNIV_UNLIKELY(slot != page_dir_get_nth_slot(page,
				page_dir_get_n_slots(page) - 1))) {
		return(FALSE);
	}

	/* Copy the rest of the dense directory. */
	for (; i < n_dense; i++) {
		ulint	offs = page_zip_dir_get(page_zip, i);

		if (UNIV_UNLIKELY(offs & ~PAGE_ZIP_DIR_SLOT_MASK)) {
			return(FALSE);
		}

		recs[i] = page + offs;
	}

	if (UNIV_LIKELY(n_dense > 1)) {
		page_zip_dir_sort(recs, recs_aux, 0, n_dense);
	}
	return(TRUE);
}

/**************************************************************************
Initialize the REC_N_NEW_EXTRA_BYTES of each record. */
static
ibool
page_zip_set_extra_bytes(
/*=====================*/
					/* out: TRUE on success,
					FALSE on failure */
	const page_zip_des_t*	page_zip,/* in: compressed page */
	page_t*			page,	/* in/out: uncompressed page */
	ulint			info_bits)/* in: REC_INFO_MIN_REC_FLAG or 0 */
{
	ulint	n;
	ulint	i;
	ulint	n_owned = 1;
	ulint	offs;
	rec_t*	rec;

	n = page_get_n_recs(page);
	rec = page + PAGE_NEW_INFIMUM;

	for (i = 0; i < n; i++) {
		offs = page_zip_dir_get(page_zip, i);

		if (UNIV_UNLIKELY(offs & PAGE_ZIP_DIR_SLOT_DEL)) {
			info_bits |= REC_INFO_DELETED_FLAG;
		}
		if (UNIV_UNLIKELY(offs & PAGE_ZIP_DIR_SLOT_OWNED)) {
			info_bits |= n_owned;
			n_owned = 1;
		} else {
			n_owned++;
		}
		offs &= PAGE_ZIP_DIR_SLOT_MASK;
		if (UNIV_UNLIKELY(offs < PAGE_ZIP_START
					+ REC_N_NEW_EXTRA_BYTES)) {
			return(FALSE);
		}

		rec_set_next_offs_new(rec, offs);
		rec = page + offs;
		rec[-REC_N_NEW_EXTRA_BYTES] = info_bits;
		info_bits = 0;
	}

	/* Set the next pointer of the last user record. */
	rec_set_next_offs_new(rec, PAGE_NEW_SUPREMUM);

	/* Set n_owned of the supremum record. */
	page[PAGE_NEW_SUPREMUM - REC_N_NEW_EXTRA_BYTES] = n_owned;

	/* The dense directory excludes the infimum and supremum records. */
	n = page_dir_get_n_heap(page) - 2;

	if (i >= n) {

		return(UNIV_LIKELY(i == n));
	}

	offs = page_zip_dir_get(page_zip, i);

	/* Set the extra bytes of deleted records on the free list. */
	for (;;) {
		if (UNIV_UNLIKELY(!offs)
		    || UNIV_UNLIKELY(offs & ~PAGE_ZIP_DIR_SLOT_MASK)) {
			return(FALSE);
		}

		rec = page + offs;
		rec[-REC_N_NEW_EXTRA_BYTES] = 0; /* info_bits and n_owned */

		if (++i == n) {
			break;
		}

		offs = page_zip_dir_get(page_zip, i);
		rec_set_next_offs_new(rec, offs);
	}

	/* Terminate the free list. */
	rec[-REC_N_NEW_EXTRA_BYTES] = 0; /* info_bits and n_owned */
	rec_set_next_offs_new(rec, 0);

	return(TRUE);
}

/**************************************************************************
Apply the modification log to an uncompressed page.
Do not copy the fields that are stored separately. */
static
const byte*
page_zip_apply_log(
/*===============*/
				/* out: pointer to end of modification log,
				or NULL on failure */
	const byte*	data,	/* in: modification log */
	ulint		size,	/* in: maximum length of the log, in bytes */
	rec_t**		recs,	/* in: dense page directory,
				sorted by address (indexed by heap_no - 2) */
	ulint		n_dense,/* in: size of recs[] */
	ulint		trx_id_col,/* in: column number of trx_id in the index;
				ignored if heap_status & REC_STATUS_NODE_PTR */
	ulint		heap_status,
				/* in: heap_no and status bits for
				the next record to uncompress */
	dict_index_t*	index,	/* in: index of the page */
	ulint*		offsets)/* in/out: work area for
				rec_get_offsets_reverse() */
{
	const byte* const end = data + size;

	for (;;) {
		ulint	val;
		rec_t*	rec;
		ulint	len;
		ulint	hs;

		val = *data++;
		if (UNIV_UNLIKELY(!val)) {
			return(data - 1);
		}
		if (val & 0x80) {
			val = (val & 0x7f) << 8 | *data++;
			if (UNIV_UNLIKELY(!val)) {
				return(NULL);
			}
		}
		if (UNIV_UNLIKELY(data >= end)) {
			return(NULL);
		}
		if (UNIV_UNLIKELY(val > n_dense)) {
			return(NULL);
		}

		/* Determine the heap number and status bits of the record. */
		rec = recs[val - 1];
		hs = (val + 1) << REC_HEAP_NO_SHIFT;
		hs |= heap_status & ((1 << REC_HEAP_NO_SHIFT) - 1);

		/* This may either be an old record that is being
		overwritten (updated in place, or allocated from
		the free list), or a new record, with the next
		available_heap_no. */
		if (UNIV_UNLIKELY(hs > heap_status)) {
			return(NULL);
		} else if (hs == heap_status) {
			/* A new record was allocated from the heap. */
			heap_status += 1 << REC_HEAP_NO_SHIFT;
		}

		mach_write_to_2(rec - REC_NEW_HEAP_NO, hs);
#if REC_STATUS_NODE_PTR != TRUE
# error "REC_STATUS_NODE_PTR != TRUE"
#endif
		rec_get_offsets_reverse(data, index,
				hs & REC_STATUS_NODE_PTR,
				offsets);
		rec_offs_make_valid(rec, index, offsets);

		/* Copy the extra bytes (backwards). */
		{
			byte*	start	= rec_get_start(rec, offsets);
			byte*	b	= rec - REC_N_NEW_EXTRA_BYTES;
			while (b != start) {
				*--b = *data++;
			}
		}

		/* Copy the data bytes. */
		if (UNIV_UNLIKELY(hs & REC_STATUS_NODE_PTR)) {
			/* Non-leaf nodes should not contain any
			externally stored columns. */
			if (UNIV_UNLIKELY(rec_offs_any_extern(offsets))) {
				return(NULL);
			}

			len = rec_offs_data_size(offsets)
					- REC_NODE_PTR_SIZE;
			/* Copy the data bytes, except node_ptr. */
			if (UNIV_UNLIKELY(data + len >= end)) {
				return(NULL);
			}
			memcpy(rec, data, len);
			data += len;
		} else {
			ulint	i;
			byte*	next_out = rec;

			/* Check if there are any externally stored columns.
			For each externally stored column, skip the
			BTR_EXTERN_FIELD_REF._*/

			for (i = 0; i < rec_offs_n_fields(offsets); i++) {
				byte*	dst;

				if (UNIV_UNLIKELY(i == trx_id_col)) {
					/* Skip trx_id and roll_ptr */
					dst = rec_get_nth_field(
							rec, offsets, i, &len);
					if (UNIV_UNLIKELY(dst - next_out
							>= end - data)
					    || UNIV_UNLIKELY(len
							< DATA_TRX_ID_LEN
							+ DATA_ROLL_PTR_LEN)
					    || rec_offs_nth_extern(
							offsets, i)) {
						return(NULL);
					}

					memcpy(next_out, data, dst - next_out);
					data += dst - next_out;
					next_out = dst + (DATA_TRX_ID_LEN
							+ DATA_ROLL_PTR_LEN);
				} else if (rec_offs_nth_extern(offsets, i)) {
					dst = rec_get_nth_field(
							rec, offsets, i, &len);
					ut_ad(len > BTR_EXTERN_FIELD_REF_SIZE);

					len += dst - next_out
						- BTR_EXTERN_FIELD_REF_SIZE;

					if (UNIV_UNLIKELY(data + len >= end)) {
						return(NULL);
					}

					memcpy(next_out, data, len);
					data += len;
					next_out += len
						+ BTR_EXTERN_FIELD_REF_SIZE;
				}
			}

			/* Copy the last bytes of the record. */
			len = rec_get_end(rec, offsets) - next_out;
			if (UNIV_UNLIKELY(data + len >= end)) {
				return(NULL);
			}
			memcpy(next_out, data, len);
			data += len;
		}
	}
}

/**************************************************************************
Decompress a page.  This function should tolerate errors on the compressed
page.  Instead of letting assertions fail, it will return FALSE if an
inconsistency is detected. */

ibool
page_zip_decompress(
/*================*/
				/* out: TRUE on success, FALSE on failure */
	page_zip_des_t*	page_zip,/* in: data, size;
				out: m_start, m_end, n_blobs */
	page_t*		page,	/* out: uncompressed page, may be trashed */
	mtr_t*		mtr)	/* in: mini-transaction handle,
				or NULL if no logging is needed */
{
	z_stream	d_stream;
	dict_index_t*	index	= NULL;
	rec_t**		recs;	/* dense page directory, sorted by address */
	rec_t**		recsc;	/* cursor to dense page directory */
	ulint		heap_status;/* heap_no and status bits */
	ulint		n_dense;/* number of user records on the page */
	ulint		trx_id_col = ULINT_UNDEFINED;
	mem_heap_t*	heap;
	ulint*		offsets	= NULL;
	ulint		info_bits = 0;
	const byte*	storage;
	const byte*	externs;

	ut_ad(page_zip_simple_validate(page_zip));

	/* The dense directory excludes the infimum and supremum records. */
	n_dense = page_dir_get_n_heap(page_zip->data) - 2;
	if (UNIV_UNLIKELY(n_dense * PAGE_ZIP_DIR_SLOT_SIZE
				>= page_zip->size)) {
		return(FALSE);
	}

	heap = mem_heap_create(n_dense * (3 * sizeof *recs));
	recsc = recs = mem_heap_alloc(heap, n_dense * (2 * sizeof *recs));

	/* Copy the page header. */
	memcpy(page, page_zip->data, PAGE_DATA);

	/* Copy the page directory. */
	if (UNIV_UNLIKELY(!page_zip_dir_decode(page_zip, page,
				recs, recs + n_dense, n_dense))) {
		mem_heap_free(heap);
		return(FALSE);
	}

	/* Copy the infimum and supremum records. */
	memcpy(page + (PAGE_NEW_INFIMUM - REC_N_NEW_EXTRA_BYTES),
		     infimum_extra, sizeof infimum_extra);
	if (UNIV_UNLIKELY(!page_get_n_recs((page_t*) page))) {
		rec_set_next_offs_new(page + PAGE_NEW_INFIMUM,
				PAGE_NEW_SUPREMUM);
	} else {
		rec_set_next_offs_new(page + PAGE_NEW_INFIMUM,
				page_zip_dir_get(page_zip, 0)
				& PAGE_ZIP_DIR_SLOT_MASK);
	}
	memcpy(page + PAGE_NEW_INFIMUM, infimum_data, sizeof infimum_data);
	memcpy(page + (PAGE_NEW_SUPREMUM - REC_N_NEW_EXTRA_BYTES + 1),
		     supremum_extra_data, sizeof supremum_extra_data);

	d_stream.zalloc = (alloc_func) 0;
	d_stream.zfree = (free_func) 0;
	d_stream.opaque = (voidpf) 0;

	if (UNIV_UNLIKELY(inflateInit(&d_stream) != Z_OK)) {
		ut_error;
	}

	d_stream.next_in = page_zip->data + PAGE_DATA;
	/* Subtract the space reserved for
	the page header and the end marker of the modification log. */
	d_stream.avail_in = page_zip->size - (PAGE_DATA + 1);

	d_stream.next_out = page + PAGE_ZIP_START;
	d_stream.avail_out = UNIV_PAGE_SIZE - PAGE_ZIP_START;

	/* Decode the zlib header and the index information. */
	if (UNIV_UNLIKELY(inflate(&d_stream, Z_BLOCK) != Z_OK)
	    || UNIV_UNLIKELY(inflate(&d_stream, Z_BLOCK) != Z_OK)) {

		goto zlib_error;
	}

	index = page_zip_fields_decode(page + PAGE_ZIP_START,
				d_stream.next_out,
				page_is_leaf(page) ? &trx_id_col : NULL);

	if (UNIV_UNLIKELY(!index)) {

		goto zlib_error;
	}

	/* Decompress the user records. */
	d_stream.next_out = page + PAGE_ZIP_START;

	{
		/* Pre-allocate the offsets
		for rec_get_offsets_reverse(). */
		ulint	n;

		if (page_is_leaf(page)) {
			n = dict_index_get_n_fields(index);
			heap_status = REC_STATUS_ORDINARY
				| 2 << REC_HEAP_NO_SHIFT;

			/* Subtract the space reserved
			for uncompressed data. */
			if (trx_id_col != ULINT_UNDEFINED) {
				d_stream.avail_in -= n_dense
					* (PAGE_ZIP_DIR_SLOT_SIZE
					+ DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
			} else {
				d_stream.avail_in -= n_dense
					* PAGE_ZIP_DIR_SLOT_SIZE;
			}
		} else {
			n = dict_index_get_n_unique_in_tree(index) + 1;
			heap_status = REC_STATUS_NODE_PTR
				| 2 << REC_HEAP_NO_SHIFT;

			if (UNIV_UNLIKELY(mach_read_from_4((page_t*) page
					+ FIL_PAGE_PREV) == FIL_NULL)) {
				info_bits = REC_INFO_MIN_REC_FLAG;
			}

			/* Subtract the space reserved
			for uncompressed data. */
			d_stream.avail_in -= n_dense
				* (PAGE_ZIP_DIR_SLOT_SIZE + REC_NODE_PTR_SIZE);
		}

		n += 1 + REC_OFFS_HEADER_SIZE;
		offsets = mem_heap_alloc(heap, n * sizeof(ulint));
		*offsets = n;
	}

	page_zip->n_blobs = 0;

	while (n_dense--) {
		byte* const	last	= d_stream.next_out;
		rec_t*		rec	= *recsc++;

		d_stream.avail_out = rec - REC_N_NEW_EXTRA_BYTES - last;

		ut_ad(d_stream.avail_out < UNIV_PAGE_SIZE
			      - PAGE_ZIP_START - PAGE_DIR);
		switch (inflate(&d_stream, Z_SYNC_FLUSH)) {
		case Z_STREAM_END:
			/* Apparently, n_dense has grown
			since the time the page was last compressed. */
			if (UNIV_UNLIKELY(d_stream.next_out != last)) {
				/* Somehow, we got a partial record. */
				goto zlib_error;
			}
			goto zlib_done;
		case Z_OK:
		case Z_BUF_ERROR:
			if (!d_stream.avail_out) {
				break;
			}
		default:
			goto zlib_error;
		}

		ut_ad(d_stream.next_out == rec - REC_N_NEW_EXTRA_BYTES);
		/* Prepare to decompress the data bytes. */
		d_stream.next_out = rec;
		/* Set heap_no and the status bits. */
		mach_write_to_2(rec - REC_NEW_HEAP_NO, heap_status);
		heap_status += 1 << REC_HEAP_NO_SHIFT;

		/* Read the offsets. The status bits are needed here. */
		offsets = rec_get_offsets(rec, index, offsets,
					ULINT_UNDEFINED, &heap);

		if (page_is_leaf(page)) {
			ulint	i;

			/* Check if there are any externally stored columns.
			For each externally stored column, restore the
			BTR_EXTERN_FIELD_REF separately._*/

			for (i = 0; i < rec_offs_n_fields(offsets); i++) {
				ulint	len;
				byte*	dst;

				if (UNIV_UNLIKELY(i == trx_id_col)) {
					/* Skip trx_id and roll_ptr */
					dst = rec_get_nth_field(
							rec, offsets, i, &len);
					if (UNIV_UNLIKELY(len < DATA_TRX_ID_LEN
							+ DATA_ROLL_PTR_LEN)
					    || rec_offs_nth_extern(
							offsets, i)) {

						goto zlib_error;
					}

					d_stream.avail_out = dst
						- d_stream.next_out;

					switch (inflate(&d_stream,
							Z_SYNC_FLUSH)) {
					case Z_STREAM_END:
					case Z_OK:
					case Z_BUF_ERROR:
						if (!d_stream.avail_out) {
							break;
						}
						/* fall through */
					default:
						goto zlib_error;
					}

					ut_ad(d_stream.next_out == dst);

					d_stream.next_out += DATA_TRX_ID_LEN
							+ DATA_ROLL_PTR_LEN;
				} else if (rec_offs_nth_extern(offsets, i)) {
					dst = rec_get_nth_field(
							rec, offsets, i, &len);
					ut_ad(len > BTR_EXTERN_FIELD_REF_SIZE);
					dst += len - BTR_EXTERN_FIELD_REF_SIZE;

					d_stream.avail_out = dst
						- d_stream.next_out;
					switch (inflate(&d_stream,
							Z_SYNC_FLUSH)) {
					case Z_STREAM_END:
					case Z_OK:
					case Z_BUF_ERROR:
						if (!d_stream.avail_out) {
							break;
						}
						/* fall through */
					default:
						goto zlib_error;
					}

					ut_ad(d_stream.next_out == dst);

					/* Reserve space for the data at
					the end of the space reserved for
					the compressed data and the
					page modification log. */

					if (UNIV_UNLIKELY(d_stream.avail_in
					    <= BTR_EXTERN_FIELD_REF_SIZE)) {
						/* out of space */
						goto zlib_error;
					}

					d_stream.avail_in
						-= BTR_EXTERN_FIELD_REF_SIZE;
					d_stream.next_out
						+= BTR_EXTERN_FIELD_REF_SIZE;
					page_zip->n_blobs++;
				}
			}

			/* Decompress the last bytes of the record. */
			d_stream.avail_out = rec_get_end(rec, offsets)
					- d_stream.next_out;

			switch (inflate(&d_stream, Z_SYNC_FLUSH)) {
			case Z_STREAM_END:
			case Z_OK:
			case Z_BUF_ERROR:
				if (!d_stream.avail_out) {
					break;
				}
				/* fall through */
			default:
				goto zlib_error;
			}
		} else {
			/* Non-leaf nodes should not have any externally
			stored columns. */
			ut_ad(!rec_offs_any_extern(offsets));

			/* Decompress the data bytes, except node_ptr. */
			d_stream.avail_out = rec_offs_data_size(offsets)
					- REC_NODE_PTR_SIZE;

			switch (inflate(&d_stream, Z_SYNC_FLUSH)) {
			case Z_STREAM_END:
			case Z_OK:
			case Z_BUF_ERROR:
				if (!d_stream.avail_out) {
					break;
				}
				/* fall through */
			default:
				goto zlib_error;
			}

			d_stream.next_out += REC_NODE_PTR_SIZE;
		}

		ut_ad(d_stream.next_out == rec_get_end(rec, offsets));
	}

	/* Decompress any trailing garbage, in case the last record was
	allocated from an originally longer space on the free list. */
	d_stream.avail_out = page_header_get_field(
				(page_t*) page, PAGE_HEAP_TOP)
				- (d_stream.next_out - page);
	if (UNIV_UNLIKELY(d_stream.avail_out > UNIV_PAGE_SIZE
			      - PAGE_ZIP_START - PAGE_DIR)) {

		goto zlib_error;
	}

	if (UNIV_UNLIKELY(inflate(&d_stream, Z_FINISH) != Z_STREAM_END)) {
zlib_error:
		inflateEnd(&d_stream);
		goto err_exit;
	}

	/* Note that d_stream.avail_out > 0 may hold here
	if the modification log is nonempty. */

zlib_done:
	if (UNIV_UNLIKELY(inflateEnd(&d_stream) != Z_OK)) {
		ut_error;
	}

	ut_ad(page_zip->data + PAGE_DATA + d_stream.total_in
					== d_stream.next_in);

	/* Clear the unused heap space on the uncompressed page. */
	memset(d_stream.next_out, 0, page_dir_get_nth_slot(page,
			page_dir_get_n_slots(page) - 1) - d_stream.next_out);

	/* The dense directory excludes the infimum and supremum records. */
	n_dense = page_dir_get_n_heap(page) - 2;

	page_zip->m_start = PAGE_DATA + d_stream.total_in;

	/* Apply the modification log. */
	{
		const byte*	mod_log_ptr;
		mod_log_ptr = page_zip_apply_log(
				page_zip->data + page_zip->m_start,
				d_stream.avail_in, recs, n_dense, trx_id_col,
				heap_status, index, offsets);

		if (UNIV_UNLIKELY(!mod_log_ptr)) {
			goto err_exit;
		}
		page_zip->m_end = mod_log_ptr - page_zip->data;
	}

	if (UNIV_UNLIKELY(!page_zip_set_extra_bytes(
				page_zip, page, info_bits))) {
err_exit:
		page_zip_fields_free(index);
		mem_heap_free(heap);
		return(FALSE);
	}

	/* Copy the uncompressed fields. */

	storage = page_zip->data + page_zip->size
			- n_dense * PAGE_ZIP_DIR_SLOT_SIZE;
	if (trx_id_col != ULINT_UNDEFINED) {
		externs = storage - n_dense
				* (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
	} else {
		externs = storage;
	}
	page_zip->n_blobs = 0;
	recsc = recs;

	while (n_dense--) {
		rec_t*	rec	= *recsc++;

		/* Read the offsets. The status bits are needed here. */
		offsets = rec_get_offsets(rec, index, offsets,
					ULINT_UNDEFINED, &heap);

		if (page_is_leaf(page)) {
			ulint	i;
			ulint	len;
			byte*	dst;

			/* Check if there are any externally stored columns.
			For each externally stored column, restore the
			BTR_EXTERN_FIELD_REF separately._*/

			for (i = 0; i < rec_offs_n_fields(offsets); i++) {
				if (rec_offs_nth_extern(offsets, i)) {
					dst = rec_get_nth_field(
							rec, offsets, i, &len);
					ut_ad(len > BTR_EXTERN_FIELD_REF_SIZE);
					dst += len - BTR_EXTERN_FIELD_REF_SIZE;

					externs -= BTR_EXTERN_FIELD_REF_SIZE;

					/* Copy the BLOB pointer */
					memcpy(dst, externs,
						BTR_EXTERN_FIELD_REF_SIZE);
				}
			}

			if (trx_id_col != ULINT_UNDEFINED) {
				dst = rec_get_nth_field(rec, offsets,
						trx_id_col, &len);
				ut_ad(len >= DATA_TRX_ID_LEN
					+ DATA_ROLL_PTR_LEN);
				storage -= DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN;
				memcpy(dst, storage,
					DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
			}
		} else {
			/* Non-leaf nodes should not have any externally
			stored columns. */
			ut_ad(!rec_offs_any_extern(offsets));
			storage -= REC_NODE_PTR_SIZE;

			memcpy(rec_get_end(rec, offsets) - REC_NODE_PTR_SIZE,
					storage, REC_NODE_PTR_SIZE);
		}
	}

	ut_a(page_is_comp(page));
	ut_ad(page_simple_validate_new(page));

	if (UNIV_LIKELY_NULL(mtr)) {
		mlog_write_initial_log_record(page, MLOG_ZIP_DECOMPRESS, mtr);
	}

	page_zip_fields_free(index);
	mem_heap_free(heap);

	return(TRUE);
}

#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
/**************************************************************************
Check that the compressed and decompressed pages match. */

ibool
page_zip_validate(
/*==============*/
					/* out: TRUE if valid, FALSE if not */
	const page_zip_des_t*	page_zip,/* in: compressed page */
	const page_t*		page)	/* in: uncompressed page */
{
	page_zip_des_t	temp_page_zip = *page_zip;
	page_t*		temp_page = buf_frame_alloc();
	ibool		valid;

	ut_a(buf_block_get_page_zip(buf_block_align((byte*)page))
		== page_zip);
	ut_a(page_is_comp((page_t*) page));

	valid = page_zip_decompress(&temp_page_zip, temp_page, NULL)
				&& !memcmp(page, temp_page,
				UNIV_PAGE_SIZE - FIL_PAGE_DATA_END);
	buf_frame_free(temp_page);
	return(valid);
}
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */

/**************************************************************************
Write an entire record on the compressed page.  The data must already
have been written to the uncompressed page. */

void
page_zip_write_rec(
/*===============*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	const byte*	rec,	/* in: record being written */
	dict_index_t*	index,	/* in: the index the record belongs to */
	const ulint*	offsets)/* in: rec_get_offsets(rec, index) */
{
	page_t*	page;
	byte*	data;
	byte*	storage;
	ulint	heap_no;

	ut_ad(buf_block_get_page_zip(buf_block_align((byte*)rec)) == page_zip);
	ut_ad(page_zip_simple_validate(page_zip));
	ut_ad(page_zip->size > PAGE_DATA + page_zip_dir_size(page_zip));
	ut_ad(rec_offs_comp(offsets));
	ut_ad(rec_offs_validate((rec_t*) rec, index, offsets));

	ut_ad(page_zip->m_start >= PAGE_DATA);
	ut_ad(!memcmp(ut_align_down((byte*) rec, UNIV_PAGE_SIZE),
		page_zip->data, PAGE_DATA));

	page = ut_align_down((rec_t*) rec, UNIV_PAGE_SIZE);

	ut_ad(rec_get_start((rec_t*) rec, offsets) >= page + PAGE_ZIP_START);
	ut_ad(rec_get_end((rec_t*) rec, offsets) <= page + UNIV_PAGE_SIZE
			- PAGE_DIR - PAGE_DIR_SLOT_SIZE
			* page_dir_get_n_slots(page));

	heap_no = rec_get_heap_no_new((rec_t*) rec);
	ut_ad(heap_no >= 2); /* not infimum or supremum */
	ut_ad(heap_no < page_dir_get_n_heap(page));

	/* Append to the modification log. */
	data = page_zip->data + page_zip->m_end;
	ut_ad(!*data);

	/* Identify the record by writing its heap number - 1.
	0 is reserved to indicate the end of the modification log. */

	if (UNIV_UNLIKELY(heap_no - 1 >= 128)) {
		*data++ = 0x80 | (heap_no - 1) >> 8;
	}
	*data++ = heap_no - 1;

	{
		const byte*	start	= rec_get_start((rec_t*) rec, offsets);
		const byte*	b	= rec - REC_N_NEW_EXTRA_BYTES;

		/* Write the extra bytes backwards, so that
		rec_offs_extra_size() can be easily computed in
		page_zip_apply_log() by invoking
		rec_get_offsets_reverse(). */

		while (b != start) {
			*data++ = *--b;
		}
	}

	/* Write the data bytes.  Store the uncompressed bytes separately. */
	storage = page_zip->data + page_zip->size
			- (page_dir_get_n_heap(page) - 2)
			* PAGE_ZIP_DIR_SLOT_SIZE;

	if (page_is_leaf(page)) {
		ulint		i;
		ulint		len;
		const byte*	start = rec;
		ulint		trx_id_col;

		if (dict_index_is_clust(index)) {
			trx_id_col = dict_index_get_sys_col_pos(
					index, DATA_TRX_ID);
			ut_ad(trx_id_col != ULINT_UNDEFINED);
			/* Do not assert on page_zip->n_blobs, because
			it may change later in this function. */
			ut_ad(data + rec_offs_data_size(offsets)
					- (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN)
					< storage
					- (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN)
					* (page_dir_get_n_heap(page) - 2));
		} else {
			trx_id_col = ULINT_UNDEFINED;
			ut_ad(dict_index_get_sys_col_pos(
					index, DATA_TRX_ID)
					== ULINT_UNDEFINED);
			/* Do not assert on page_zip->n_blobs, because
			it may change later in this function. */
			ut_ad(data + rec_offs_data_size(offsets) < storage);
		}

		/* Check if there are any externally stored columns.
		For each externally stored column, store the
		BTR_EXTERN_FIELD_REF separately._*/

		for (i = 0; i < rec_offs_n_fields(offsets); i++) {
			const byte*	src;

			if (UNIV_UNLIKELY(i == trx_id_col)) {
				ut_ad(!rec_offs_nth_extern(offsets, i));
				ut_ad(!rec_offs_nth_extern(offsets, i + 1));
				/* Store trx_id and roll_ptr separately. */
				src = rec_get_nth_field((rec_t*) rec,
						offsets, i, &len);
				ut_ad(len == DATA_TRX_ID_LEN);
				ut_ad(src + DATA_TRX_ID_LEN
					== rec_get_nth_field((rec_t*) rec,
					offsets, i + 1, &len));
				ut_ad(len == DATA_ROLL_PTR_LEN);

				/* Log the preceding fields. */
				memcpy(data, start, src - start);
				data += src - start;
				start = src + (DATA_TRX_ID_LEN
						+ DATA_ROLL_PTR_LEN);

				/* Store trx_id and roll_ptr separately. */
				memcpy(storage
					- (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN)
					* (heap_no - 1),
					src,
					DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
				i++; /* skip also roll_ptr */
			} else if (rec_offs_nth_extern(offsets, i)) {
				src = rec_get_nth_field((rec_t*) rec,
						offsets, i, &len);

				ut_ad(len > BTR_EXTERN_FIELD_REF_SIZE);
				src += len - BTR_EXTERN_FIELD_REF_SIZE;

				memcpy(data, start, src - start);
				data += src - start;
				start = src + BTR_EXTERN_FIELD_REF_SIZE;

				/* TODO: copy the BLOB pointer to
				the appropriate place in the
				uncompressed BLOB pointer array */

				/* TODO: update page_zip->n_blobs */
			}
		}

		/* Log the last bytes of the record. */
		len = rec_get_end((rec_t*) rec, offsets) - start;

		memcpy(data, start, len);
		data += len;
	} else {
		/* This is a node pointer page. */
		ulint	len;

		/* Non-leaf nodes should not have any externally
		stored columns. */
		ut_ad(!rec_offs_any_extern(offsets));

		/* Copy the data bytes, except node_ptr. */
		len = rec_offs_data_size(offsets) - REC_NODE_PTR_SIZE;
		ut_ad(data + len < storage - REC_NODE_PTR_SIZE
				* (page_dir_get_n_heap(page) - 2));
		memcpy(data, rec, len);
		data += len;

		/* Copy the node pointer to the uncompressed area. */
		memcpy(storage - REC_NODE_PTR_SIZE
				* (heap_no - 1),
				rec + len,
				REC_NODE_PTR_SIZE);
	}

	ut_a(!*data);
	page_zip->m_end = data - page_zip->data;

#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
	ut_a(page_zip_validate(page_zip,
			ut_align_down((byte*) rec, UNIV_PAGE_SIZE)));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */
}

/**************************************************************************
Write the BLOB pointer of a record on the leaf page of a clustered index.
The information must already have been updated on the uncompressed page. */

void
page_zip_write_blob_ptr(
/*====================*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	const byte*	rec,	/* in/out: record whose data is being
				written */
	dict_index_t*	index,	/* in: index of the page */
	const ulint*	offsets,/* in: rec_get_offsets(rec, index) */
	ulint		n,	/* in: column index */
	mtr_t*		mtr)	/* in: mini-transaction handle,
				or NULL if no logging is needed */
{
	byte*	field;
	byte*	externs;
	page_t*	page	= buf_frame_align((byte*) rec);
	ulint	blob_no;
	ulint	next_offs;
	ulint	len;

	ut_ad(buf_block_get_page_zip(buf_block_align((byte*)rec)) == page_zip);
	ut_ad(page_zip_simple_validate(page_zip));
	ut_ad(page_zip->size > PAGE_DATA + page_zip_dir_size(page_zip));
	ut_ad(rec_offs_comp(offsets));
	ut_ad(rec_offs_validate((rec_t*) rec, NULL, offsets));
	ut_ad(rec_offs_nth_extern(offsets, n));

	ut_ad(page_zip->m_start >= PAGE_DATA);
	ut_ad(!memcmp(page, page_zip->data, PAGE_DATA));

	ut_ad(page_is_leaf(page));

	blob_no = 0;
	next_offs = rec_get_next_offs(page + PAGE_NEW_INFIMUM, TRUE);
	ut_a(next_offs > PAGE_NEW_SUPREMUM_END);

	do {
		rec_t*	r = page + next_offs;

		if (r == rec) {
			goto found;
		}

		blob_no += rec_get_n_extern_new(r, index, ULINT_UNDEFINED);

		next_offs = rec_get_next_offs(r, TRUE);
		ut_a(next_offs > 0);
	} while (next_offs != PAGE_NEW_SUPREMUM);

	ut_error;

found:
	blob_no += rec_get_n_extern_new(rec, index, n);
	ut_a(blob_no < page_zip->n_blobs);

	/* The heap number of the first user record is 2. */
	if (dict_index_is_clust(index)) {
		externs = page_zip->data + page_zip->size
			- (page_dir_get_n_heap(page) - 2)
			* (PAGE_ZIP_DIR_SLOT_SIZE
			+ DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
	} else {
		externs = page_zip->data + page_zip->size
			- (page_dir_get_n_heap(page) - 2)
			* PAGE_ZIP_DIR_SLOT_SIZE;
	}

	field = rec_get_nth_field((rec_t*) rec, offsets, n, &len);

	memcpy(externs - blob_no * BTR_EXTERN_FIELD_REF_SIZE,
			field + len - BTR_EXTERN_FIELD_REF_SIZE,
			BTR_EXTERN_FIELD_REF_SIZE);

	if (mtr) {
		mlog_write_initial_log_record(
				(rec_t*) rec, MLOG_ZIP_WRITE_BLOB_PTR, mtr);
		/* TODO: write n */
	}
}

/**************************************************************************
Write the node pointer of a record on a non-leaf compressed page. */

void
page_zip_write_node_ptr(
/*====================*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	byte*		rec,	/* in/out: record */
	ulint		size,	/* in: data size of rec */
	ulint		ptr,	/* in: node pointer */
	mtr_t*		mtr)	/* in: mini-transaction, or NULL */
{
	byte*	field;
	byte*	storage;
	page_t*	page	= buf_frame_align(rec);

	ut_ad(buf_block_get_page_zip(buf_block_align(rec)) == page_zip);
	ut_ad(page_zip_simple_validate(page_zip));
	ut_ad(page_zip->size > PAGE_DATA + page_zip_dir_size(page_zip));
	ut_ad(page_rec_is_comp(rec));

	ut_ad(page_zip->m_start >= PAGE_DATA);
	ut_ad(!memcmp(page, page_zip->data, PAGE_DATA));

	ut_ad(!page_is_leaf(page));

	/* The heap number of the first user record is 2. */
	storage = page_zip->data + page_zip->size
			- (page_dir_get_n_heap(page) - 2)
			* PAGE_ZIP_DIR_SLOT_SIZE
			- (rec_get_heap_no_new(rec) - 1) * REC_NODE_PTR_SIZE;
	field = rec + size - REC_NODE_PTR_SIZE;

#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
	ut_a(!memcmp(storage, field, REC_NODE_PTR_SIZE));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */
#if REC_NODE_PTR_SIZE != 4
# error "REC_NODE_PTR_SIZE != 4"
#endif
	mach_write_to_4(field, ptr);
	memcpy(storage, field, REC_NODE_PTR_SIZE);

	if (mtr) {
		mlog_write_initial_log_record(
				rec, MLOG_ZIP_WRITE_NODE_PTR, mtr);
	}
}

/**************************************************************************
Write the trx_id and roll_ptr of a record on a B-tree leaf node page. */

void
page_zip_write_trx_id_and_roll_ptr(
/*===============================*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	byte*		rec,	/* in/out: record */
	const ulint*	offsets,/* in: rec_get_offsets(rec, index) */
	ulint		trx_id_col,/* in: column number of TRX_ID in rec */
	dulint		trx_id,	/* in: transaction identifier */
	dulint		roll_ptr)/* in: roll_ptr */
{
	byte*	field;
	byte*	storage;
	page_t*	page	= ut_align_down(rec, UNIV_PAGE_SIZE);
	ulint	len;

	ut_ad(buf_block_get_page_zip(buf_block_align(rec)) == page_zip);
	ut_ad(page_zip_simple_validate(page_zip));
	ut_ad(page_zip->size > PAGE_DATA + page_zip_dir_size(page_zip));
	ut_ad(rec_offs_validate(rec, NULL, offsets));
	ut_ad(rec_offs_comp(offsets));

	ut_ad(page_zip->m_start >= PAGE_DATA);
	ut_ad(!memcmp(page, page_zip->data, PAGE_DATA));

	ut_ad(page_is_leaf(page));

	/* The heap number of the first user record is 2. */
	storage = page_zip->data + page_zip->size
			- (page_dir_get_n_heap(page) - 2)
			* PAGE_ZIP_DIR_SLOT_SIZE
			- (rec_get_heap_no_new(rec) - 1)
			* (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);

#if DATA_TRX_ID + 1 != DATA_ROLL_PTR
# error "DATA_TRX_ID + 1 != DATA_ROLL_PTR"
#endif
	field = rec_get_nth_field(rec, offsets, trx_id_col, &len);
	ut_ad(len == DATA_TRX_ID_LEN);
	ut_ad(field + DATA_TRX_ID_LEN
		== rec_get_nth_field(rec, offsets, trx_id_col + 1, &len));
	ut_ad(len == DATA_ROLL_PTR_LEN);
#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
	ut_a(!memcmp(storage, field, DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */
#if DATA_TRX_ID_LEN != 6
# error "DATA_TRX_ID_LEN != 6"
#endif
	mach_write_to_6(field, trx_id);
#if DATA_ROLL_PTR_LEN != 7
# error "DATA_ROLL_PTR_LEN != 7"
#endif
	mach_write_to_7(field, roll_ptr);
	memcpy(storage, field, DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN);
}

/**************************************************************************
Clear an area on the uncompressed and compressed page, if possible. */

void
page_zip_clear_rec(
/*===============*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	byte*		rec,	/* in: record to clear */
	dict_index_t*	index,	/* in: index of rec */
	const ulint*	offsets,/* in: rec_get_offsets(rec, index) */
	mtr_t*		mtr)	/* in: mini-transaction */
{
#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
	ut_a(page_zip_validate(page_zip, ut_align_down(rec, UNIV_PAGE_SIZE)));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */
	ut_ad(rec_offs_validate(rec, index, offsets));

	if (page_zip_available(page_zip, rec_offs_size(offsets),
				page_is_leaf(page_zip->data),
				dict_index_is_clust(index), 0)) {
		memset(rec - rec_offs_extra_size(offsets), 0,
			rec_offs_extra_size(offsets) - REC_N_NEW_EXTRA_BYTES);
		memset(rec, 0, rec_offs_data_size(offsets));

		/* Log that the data was zeroed out. */
		page_zip_write_rec(page_zip, rec, index, offsets);
	} else {
		/* There is not enough space to log the clearing.
		Try to clear the block and to recompress the page. */

		byte*	buf = mem_alloc(rec_offs_size(offsets));
		memcpy(buf, rec - rec_offs_extra_size(offsets),
					rec_offs_size(offsets));

		memset(rec - rec_offs_extra_size(offsets), 0,
			rec_offs_extra_size(offsets) - REC_N_NEW_EXTRA_BYTES);
		memset(rec, 0, rec_offs_data_size(offsets));
		/* TODO: maybe log the memset()s? */

		if (UNIV_UNLIKELY(!page_zip_compress(page_zip,
					ut_align_down(rec, UNIV_PAGE_SIZE),
					index, mtr))) {
			/* Compression failed. Restore the block. */
			memcpy(rec - rec_offs_extra_size(offsets), buf,
				rec_offs_size(offsets));
		}

		mem_free(buf);
	}
}

/**************************************************************************
Populate the dense page directory on the compressed page
from the sparse directory on the uncompressed row_format=compact page. */
void
page_zip_dir_rewrite(
/*=================*/
	page_zip_des_t*	page_zip,/* out: dense directory on compressed page */
	const page_t*	page)	/* in: uncompressed page  */
{
	ut_ad(page_zip_simple_validate(page_zip));

	page_zip_dir_encode(page, page_zip->data + page_zip->size, NULL);
}

/**************************************************************************
Write the "deleted" flag of a record on a compressed page.  The flag must
already have been written on the uncompressed page. */

void
page_zip_rec_set_deleted(
/*=====================*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	const byte*	rec,	/* in: record on the uncompressed page */
	ulint		flag)	/* in: the deleted flag (nonzero=TRUE) */
{
	byte*	slot = page_zip_dir_find(page_zip,
				ut_align_offset(rec, UNIV_PAGE_SIZE));
	ut_a(slot);
	if (flag) {
		*slot |= (PAGE_ZIP_DIR_SLOT_DEL >> 8);
	} else {
		*slot &= ~(PAGE_ZIP_DIR_SLOT_DEL >> 8);
	}
}

/**************************************************************************
Write the "owned" flag of a record on a compressed page.  The n_owned field
must already have been written on the uncompressed page. */

void
page_zip_rec_set_owned(
/*===================*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	const byte*	rec,	/* in: record on the uncompressed page */
	ulint		flag)	/* in: the owned flag (nonzero=TRUE) */
{
	byte*	slot = page_zip_dir_find(page_zip,
				ut_align_offset(rec, UNIV_PAGE_SIZE));
	ut_a(slot);
	if (flag) {
		*slot |= (PAGE_ZIP_DIR_SLOT_OWNED >> 8);
	} else {
		*slot &= ~(PAGE_ZIP_DIR_SLOT_OWNED >> 8);
	}
}


/**************************************************************************
Shift the dense page directory when a record is deleted. */

void
page_zip_dir_delete(
/*================*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	const byte*	rec,	/* in: deleted record */
	const byte*	free)	/* in: previous start of the free list */
{
	byte*	slot_rec;
	byte*	slot_free;

	ut_ad(rec);

	slot_rec = page_zip_dir_find(page_zip,
			ut_align_offset(rec, UNIV_PAGE_SIZE));

	ut_a(slot_rec);

	if (UNIV_UNLIKELY(!free)) {
		/* Make the last slot the start of the free list. */
		slot_free = page_zip->data + page_zip->size
				- PAGE_ZIP_DIR_SLOT_SIZE
				* (page_dir_get_n_heap(page_zip->data) - 2);
	} else {
		slot_free = page_zip_dir_find_free(page_zip,
				ut_align_offset(free, UNIV_PAGE_SIZE));
		ut_a(slot_free < slot_rec);
		/* Grow the free list by one slot by moving the start. */
		slot_free += PAGE_ZIP_DIR_SLOT_SIZE;
	}

	if (UNIV_LIKELY(slot_rec > slot_free)) {
		memmove(slot_free + PAGE_ZIP_DIR_SLOT_SIZE,
			slot_free,
			slot_rec - slot_free);
	}

	/* Write the entry for the deleted record.
	The "owned" and "deleted" flags will be cleared. */
	mach_write_to_2(slot_free, ut_align_offset(rec, UNIV_PAGE_SIZE));
}

/**************************************************************************
Add a slot to the dense page directory. */

void
page_zip_dir_add_slot(
/*==================*/
	page_zip_des_t*	page_zip,	/* in/out: compressed page */
	ulint		is_clustered)	/* in: nonzero for clustered index,
					zero for others */
{
	ulint	n_dense;
	byte*	dir;
	byte*	stored;

	ut_ad(page_is_comp(page_zip->data));

	/* Read the old n_dense (n_heap has already been incremented).
	Subtract 2 for the infimum and supremum records. */
	n_dense = page_dir_get_n_heap(page_zip->data) - 3;

	dir = page_zip->data + page_zip->size
			- PAGE_ZIP_DIR_SLOT_SIZE * n_dense;

	if (!page_is_leaf(page_zip->data)) {
		ut_ad(!page_zip->n_blobs);
		stored = dir - n_dense * REC_NODE_PTR_SIZE;
	} else if (UNIV_UNLIKELY(is_clustered)) {
		stored = dir - n_dense
			* (DATA_TRX_ID_LEN + DATA_ROLL_PTR_LEN)
			- page_zip->n_blobs * BTR_EXTERN_FIELD_REF_SIZE;
	} else {
		stored = dir
			- page_zip->n_blobs * BTR_EXTERN_FIELD_REF_SIZE;
	}

	/* Move the uncompressed area backwards to make space
	for one directory slot. */
	memmove(stored - PAGE_ZIP_DIR_SLOT_SIZE, stored, dir - stored);
}

/**************************************************************************
Write a log record of writing to the uncompressed header portion of a page. */

void
page_zip_write_header_log(
/*======================*/
	const page_zip_des_t*	page_zip,/* in: compressed page */
	ulint			offset,	/* in: offset to the data */
	ulint			length,	/* in: length of the data */
	mtr_t*			mtr)	/* in: mini-transaction */
{
	byte*	log_ptr = mlog_open(mtr, 11 + 2 + 1);
	ut_ad(offset < PAGE_DATA);
	ut_ad(offset + length < PAGE_DATA);
#if PAGE_DATA > 255
# error "PAGE_DATA > 255"
#endif
	ut_ad(length < 256);

	/* If no logging is requested, we may return now */
	if (log_ptr == NULL) {

		return;
	}

	log_ptr = mlog_write_initial_log_record_fast(page_zip->data + offset,
				MLOG_ZIP_WRITE_HEADER, log_ptr, mtr);
	mach_write_to_2(log_ptr, offset);
	log_ptr += 2;

	mach_write_to_1(log_ptr, length);
	mlog_close(mtr, log_ptr);

	mlog_catenate_string(mtr, page_zip->data + offset, length);
}
