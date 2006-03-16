/******************************************************
Compressed page interface

(c) 2005 Innobase Oy

Created June 2005 by Marko Makela
*******************************************************/

#ifndef page0zip_h
#define page0zip_h

#ifdef UNIV_MATERIALIZE
# undef UNIV_INLINE
# define UNIV_INLINE
#endif

#include "mtr0types.h"
#include "page0types.h"
#include "dict0types.h"
#include "ut0byte.h"

/**************************************************************************
Initialize a compressed page descriptor. */
UNIV_INLINE
void
page_zip_des_init(
/*==============*/
	page_zip_des_t*	page_zip);	/* in/out: compressed page
					descriptor */

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
	__attribute__((nonnull(1,2,3)));

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
	__attribute__((warn_unused_result, nonnull(1, 2)));

#ifdef UNIV_DEBUG
/**************************************************************************
Validate a compressed page descriptor. */
UNIV_INLINE
ibool
page_zip_simple_validate(
/*=====================*/
						/* out: TRUE if ok */
	const page_zip_des_t*	page_zip);	/* in: compressed page
						descriptor */
#endif /* UNIV_DEBUG */

#if defined UNIV_DEBUG || defined UNIV_ZIP_DEBUG
/**************************************************************************
Check that the compressed and decompressed pages match. */

ibool
page_zip_validate(
/*==============*/
	const page_zip_des_t*	page_zip,/* in: compressed page */
	const page_t*		page)	/* in: uncompressed page */
	__attribute__((nonnull));
#endif /* UNIV_DEBUG || UNIV_ZIP_DEBUG */

/**************************************************************************
Ensure that enough space is available in the modification log.
If not, try to compress the page. */
UNIV_INLINE
ibool
page_zip_alloc(
/*===========*/
				/* out: TRUE if enough space is available */
	page_zip_des_t*	page_zip,/* in/out: compressed page;
				will only be modified if compression is needed
				and successful */
	const page_t*	page,	/* in: uncompressed page */
	dict_index_t*	index,	/* in: index of the B-tree node */
	mtr_t*		mtr,	/* in: mini-transaction handle,
				or NULL if no logging is desired */
	ulint		length,	/* in: combined size of the record */
	ulint		create)	/* in: nonzero=add the record to the heap */
	__attribute__((warn_unused_result, nonnull(1,2,3)));

/**************************************************************************
Write an entire record on the compressed page.  The data must already
have been written to the uncompressed page. */

void
page_zip_write_rec(
/*===============*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	const byte*	rec,	/* in: record being written */
	dict_index_t*	index,	/* in: the index the record belongs to */
	const ulint*	offsets,/* in: rec_get_offsets(rec, index) */
	ulint		create)	/* in: nonzero=insert, zero=update */
	__attribute__((nonnull));

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
	__attribute__((nonnull(1,2,3,4)));

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
	__attribute__((nonnull(1,2)));

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
	__attribute__((nonnull));

/**************************************************************************
Populate the dense page directory on the compressed page
from the sparse directory on the uncompressed row_format=compact page. */
void
page_zip_dir_rewrite(
/*=================*/
	page_zip_des_t*	page_zip,/* out: dense directory on compressed page */
	const page_t*	page)	/* in: uncompressed page  */
	__attribute__((nonnull));

/**************************************************************************
Write the "deleted" flag of a record on a compressed page.  The flag must
already have been written on the uncompressed page. */

void
page_zip_rec_set_deleted(
/*=====================*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	const byte*	rec,	/* in: record on the uncompressed page */
	ulint		flag)	/* in: the deleted flag (nonzero=TRUE) */
	__attribute__((nonnull));

/**************************************************************************
Write the "owned" flag of a record on a compressed page.  The n_owned field
must already have been written on the uncompressed page. */

void
page_zip_rec_set_owned(
/*===================*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	const byte*	rec,	/* in: record on the uncompressed page */
	ulint		flag)	/* in: the owned flag (nonzero=TRUE) */
	__attribute__((nonnull));

/**************************************************************************
Shift the dense page directory and the array of BLOB pointers
when a record is deleted. */

void
page_zip_dir_delete(
/*================*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	byte*		rec,	/* in: deleted record */
	dict_index_t*	index,	/* in: index of rec */
	const ulint*	offsets,/* in: rec_get_offsets(rec) */
	const byte*	free)	/* in: previous start of the free list */
	__attribute__((nonnull(1,2,3,4)));

/**************************************************************************
Add a slot to the dense page directory. */

void
page_zip_dir_add_slot(
/*==================*/
	page_zip_des_t*	page_zip,	/* in/out: compressed page */
	ulint		is_clustered)	/* in: nonzero for clustered index,
					zero for others */
	__attribute__((nonnull));

/**************************************************************************
Write data to the uncompressed header portion of a page.  The data must
already have been written to the uncompressed page.
However, the data portion of the uncompressed page may differ from
the compressed page when a record is being inserted in
page_cur_insert_rec_low(). */
UNIV_INLINE
void
page_zip_write_header(
/*==================*/
	page_zip_des_t*	page_zip,/* in/out: compressed page */
	const byte*	str,	/* in: address on the uncompressed page */
	ulint		length,	/* in: length of the data */
	mtr_t*		mtr)	/* in: mini-transaction, or NULL */
	__attribute__((nonnull(1,2)));

#ifdef UNIV_MATERIALIZE
# undef UNIV_INLINE
# define UNIV_INLINE	UNIV_INLINE_ORIGINAL
#endif

#ifndef UNIV_NONINL
# include "page0zip.ic"
#endif

#endif /* page0zip_h */
