/************************************************************************
SQL data field and tuple

(c) 1994-1996 Innobase Oy

Created 5/30/1994 Heikki Tuuri
*************************************************************************/

#include "data0data.h"

#ifdef UNIV_NONINL
#include "data0data.ic"
#endif

#include "rem0rec.h"
#include "rem0cmp.h"
#include "page0page.h"
#include "page0zip.h"
#include "dict0dict.h"
#include "btr0cur.h"

#include <ctype.h>

#ifdef UNIV_DEBUG
byte	data_error;	/* data pointers of tuple fields are initialized
			to point here for error checking */

ulint	data_dummy;	/* this is used to fool the compiler in
			dtuple_validate */
#endif /* UNIV_DEBUG */

/* Some non-inlined functions used in the MySQL interface: */
void
dfield_set_data_noninline(
	dfield_t*	field,	/* in: field */
	void*		data,	/* in: data */
	ulint		len)	/* in: length or UNIV_SQL_NULL */
{
	dfield_set_data(field, data, len);
}
void*
dfield_get_data_noninline(
	dfield_t* field)	/* in: field */
{
	return(dfield_get_data(field));
}
ulint
dfield_get_len_noninline(
	const dfield_t* field)	/* in: field */
{
	return(dfield_get_len(field));
}
ulint
dtuple_get_n_fields_noninline(
	const dtuple_t*	tuple)	/* in: tuple */
{
	return(dtuple_get_n_fields(tuple));
}
const dfield_t*
dtuple_get_nth_field_noninline(
	const dtuple_t*	tuple,	/* in: tuple */
	ulint		n)	/* in: index of field */
{
	return(dtuple_get_nth_field(tuple, n));
}

/*************************************************************************
Tests if dfield data length and content is equal to the given. */

ibool
dfield_data_is_binary_equal(
/*========================*/
				/* out: TRUE if equal */
	const dfield_t*	field,	/* in: field */
	ulint		len,	/* in: data length or UNIV_SQL_NULL */
	const byte*	data)	/* in: data */
{
	if (len != field->len) {

		return(FALSE);
	}

	if (len == UNIV_SQL_NULL) {

		return(TRUE);
	}

	if (0 != ut_memcmp(field->data, data, len)) {

		return(FALSE);
	}

	return(TRUE);
}

/****************************************************************
Compare two data tuples, respecting the collation of character fields. */

int
dtuple_coll_cmp(
/*============*/
				/* out: 1, 0 , -1 if tuple1 is greater, equal,
				less, respectively, than tuple2 */
	const dtuple_t*	tuple1,	/* in: tuple 1 */
	const dtuple_t*	tuple2)	/* in: tuple 2 */
{
	ulint	n_fields;
	ulint	i;

	ut_ad(tuple1 && tuple2);
	ut_ad(tuple1->magic_n == DATA_TUPLE_MAGIC_N);
	ut_ad(tuple2->magic_n == DATA_TUPLE_MAGIC_N);
	ut_ad(dtuple_check_typed(tuple1));
	ut_ad(dtuple_check_typed(tuple2));

	n_fields = dtuple_get_n_fields(tuple1);

	if (n_fields != dtuple_get_n_fields(tuple2)) {

		return(n_fields < dtuple_get_n_fields(tuple2) ? -1 : 1);
	}

	for (i = 0; i < n_fields; i++) {
		int		cmp;
		const dfield_t*	field1	= dtuple_get_nth_field(tuple1, i);
		const dfield_t*	field2	= dtuple_get_nth_field(tuple2, i);

		cmp = cmp_dfield_dfield(field1, field2);

		if (cmp) {
			return(cmp);
		}
	}

	return(0);
}

/*************************************************************************
Creates a dtuple for use in MySQL. */

dtuple_t*
dtuple_create_for_mysql(
/*====================*/
				/* out, own created dtuple */
	void**	heap,		/* out: created memory heap */
	ulint	n_fields)	/* in: number of fields */
{
	*heap = (void*)mem_heap_create(500);

	return(dtuple_create(*((mem_heap_t**)heap), n_fields));
}

/*************************************************************************
Frees a dtuple used in MySQL. */

void
dtuple_free_for_mysql(
/*==================*/
	void*	heap) /* in: memory heap where tuple was created */
{
	mem_heap_free((mem_heap_t*)heap);
}

/*************************************************************************
Sets number of fields used in a tuple. Normally this is set in
dtuple_create, but if you want later to set it smaller, you can use this. */

void
dtuple_set_n_fields(
/*================*/
	dtuple_t*	tuple,		/* in: tuple */
	ulint		n_fields)	/* in: number of fields */
{
	ut_ad(tuple);

	tuple->n_fields = n_fields;
	tuple->n_fields_cmp = n_fields;
}

/**************************************************************
Checks that a data field is typed. */
static
ibool
dfield_check_typed_no_assert(
/*=========================*/
				/* out: TRUE if ok */
	const dfield_t*	field)	/* in: data field */
{
	if (dfield_get_type(field)->mtype > DATA_MYSQL
	    || dfield_get_type(field)->mtype < DATA_VARCHAR) {

		fprintf(stderr,
			"InnoDB: Error: data field type %lu, len %lu\n",
			(ulong) dfield_get_type(field)->mtype,
			(ulong) dfield_get_len(field));
		return(FALSE);
	}

	return(TRUE);
}

/**************************************************************
Checks that a data tuple is typed. */

ibool
dtuple_check_typed_no_assert(
/*=========================*/
				/* out: TRUE if ok */
	const dtuple_t*	tuple)	/* in: tuple */
{
	const dfield_t*	field;
	ulint		i;

	if (dtuple_get_n_fields(tuple) > REC_MAX_N_FIELDS) {
		fprintf(stderr,
			"InnoDB: Error: index entry has %lu fields\n",
			(ulong) dtuple_get_n_fields(tuple));
dump:
		fputs("InnoDB: Tuple contents: ", stderr);
		dtuple_print(stderr, tuple);
		putc('\n', stderr);

		return(FALSE);
	}

	for (i = 0; i < dtuple_get_n_fields(tuple); i++) {

		field = dtuple_get_nth_field(tuple, i);

		if (!dfield_check_typed_no_assert(field)) {
			goto dump;
		}
	}

	return(TRUE);
}

/**************************************************************
Checks that a data field is typed. Asserts an error if not. */

ibool
dfield_check_typed(
/*===============*/
				/* out: TRUE if ok */
	const dfield_t*	field)	/* in: data field */
{
	if (dfield_get_type(field)->mtype > DATA_MYSQL
	    || dfield_get_type(field)->mtype < DATA_VARCHAR) {

		fprintf(stderr,
			"InnoDB: Error: data field type %lu, len %lu\n",
			(ulong) dfield_get_type(field)->mtype,
			(ulong) dfield_get_len(field));

		ut_error;
	}

	return(TRUE);
}

/**************************************************************
Checks that a data tuple is typed. Asserts an error if not. */

ibool
dtuple_check_typed(
/*===============*/
				/* out: TRUE if ok */
	const dtuple_t*	tuple)	/* in: tuple */
{
	const dfield_t*	field;
	ulint		i;

	for (i = 0; i < dtuple_get_n_fields(tuple); i++) {

		field = dtuple_get_nth_field(tuple, i);

		ut_a(dfield_check_typed(field));
	}

	return(TRUE);
}

#ifdef UNIV_DEBUG
/**************************************************************
Validates the consistency of a tuple which must be complete, i.e,
all fields must have been set. */

ibool
dtuple_validate(
/*============*/
				/* out: TRUE if ok */
	const dtuple_t*	tuple)	/* in: tuple */
{
	const dfield_t*	field;
	const byte*	data;
	ulint		n_fields;
	ulint		len;
	ulint		i;
	ulint		j;

	ut_ad(tuple->magic_n == DATA_TUPLE_MAGIC_N);

	n_fields = dtuple_get_n_fields(tuple);

	/* We dereference all the data of each field to test
	for memory traps */

	for (i = 0; i < n_fields; i++) {

		field = dtuple_get_nth_field(tuple, i);
		len = dfield_get_len(field);

		if (len != UNIV_SQL_NULL) {

			data = field->data;

			for (j = 0; j < len; j++) {

				data_dummy  += *data; /* fool the compiler not
						      to optimize out this
						      code */
				data++;
			}
		}
	}

	ut_a(dtuple_check_typed(tuple));

	return(TRUE);
}
#endif /* UNIV_DEBUG */

/*****************************************************************
Pretty prints a dfield value according to its data type. */

void
dfield_print(
/*=========*/
	const dfield_t*	dfield)	/* in: dfield */
{
	const byte*	data;
	ulint		len;
	ulint		mtype;
	ulint		i;

	len = dfield_get_len(dfield);
	data = dfield_get_data(dfield);

	if (len == UNIV_SQL_NULL) {
		fputs("NULL", stderr);

		return;
	}

	mtype = dtype_get_mtype(dfield_get_type(dfield));

	if ((mtype == DATA_CHAR) || (mtype == DATA_VARCHAR)) {

		for (i = 0; i < len; i++) {
			int	c = *data++;
			putc(isprint(c) ? c : ' ', stderr);
		}
	} else if (mtype == DATA_INT) {
		ut_a(len == 4); /* only works for 32-bit integers */
		fprintf(stderr, "%d", (int)mach_read_from_4(data));
	} else {
		ut_error;
	}
}

/*****************************************************************
Pretty prints a dfield value according to its data type. Also the hex string
is printed if a string contains non-printable characters. */

void
dfield_print_also_hex(
/*==================*/
	const dfield_t*	dfield)	/* in: dfield */
{
	const byte*	data;
	ulint		len;
	ulint		mtype;
	ulint		prtype;
	ulint		i;
	ibool		print_also_hex;

	len = dfield_get_len(dfield);
	data = dfield_get_data(dfield);

	if (len == UNIV_SQL_NULL) {
		fputs("NULL", stderr);

		return;
	}

	mtype = dtype_get_mtype(dfield_get_type(dfield));
	prtype = dtype_get_prtype(dfield_get_type(dfield));

	if ((mtype == DATA_CHAR) || (mtype == DATA_VARCHAR)) {

		print_also_hex = FALSE;

		for (i = 0; i < len; i++) {
			int c = *data++;

			if (!isprint(c)) {
				print_also_hex = TRUE;

				fprintf(stderr, "\\x%02x", (unsigned char) c);
			} else {
				putc(c, stderr);
			}
		}

		if (!print_also_hex) {

			return;
		}

		fputs(" Hex: ", stderr);

		data = dfield_get_data(dfield);

		for (i = 0; i < len; i++) {
			fprintf(stderr, "%02lx", (ulint)*data);

			data++;
		}
	} else if (mtype == DATA_BINARY) {
		data = dfield_get_data(dfield);
		fputs(" Hex: ",stderr);

		for (i = 0; i < len; i++) {
			fprintf(stderr, "%02lx", (ulint)*data);
			data++;
		}
	} else if (mtype == DATA_INT) {
		dulint big_val;

		if (len == 1) {
			ulint	val;

			val = (ulint)mach_read_from_1(data);

			if (!(prtype & DATA_UNSIGNED)) {
				val &= ~0x80;
				fprintf(stderr, "%ld", (long) val);
			} else {
				fprintf(stderr, "%lu", (ulong) val);
			}

		} else if (len == 2) {
			ulint	val;

			val = (ulint)mach_read_from_2(data);

			if (!(prtype & DATA_UNSIGNED)) {
				val &= ~0x8000;
				fprintf(stderr, "%ld", (long) val);
			} else {
				fprintf(stderr, "%lu", (ulong) val);
			}

		} else if (len == 3) {
			ulint	val;

			val = (ulint)mach_read_from_3(data);

			if (!(prtype & DATA_UNSIGNED)) {
				val &= ~0x800000;
				fprintf(stderr, "%ld", (long) val);
			} else {
				fprintf(stderr, "%lu", (ulong) val);
			}

		} else if (len == 4) {
			ulint	val;

			val = (ulint)mach_read_from_4(data);

			if (!(prtype & DATA_UNSIGNED)) {
				val &= ~0x80000000;
				fprintf(stderr, "%ld", (long) val);
			} else {
				fprintf(stderr, "%lu", (ulong) val);
			}

		} else if (len == 6) {
			big_val = (dulint)mach_read_from_6(data);
			fprintf(stderr, "{%lu %lu}",
				ut_dulint_get_high(big_val),
				ut_dulint_get_low(big_val));
		} else if (len == 7) {
			big_val = (dulint)mach_read_from_7(data);
			fprintf(stderr, "{%lu %lu}",
				ut_dulint_get_high(big_val),
				ut_dulint_get_low(big_val));
		} else if (len == 8) {
			big_val = (dulint)mach_read_from_8(data);
			fprintf(stderr, "{%lu %lu}",
				ut_dulint_get_high(big_val),
				ut_dulint_get_low(big_val));
		} else {
			fputs(" Hex: ",stderr);

			for (i = 0; i < len; i++) {
				fprintf(stderr, "%02lx", (ulint)*data);
				data++;
			}
		}
	} else if (mtype == DATA_SYS) {
		dulint	id;

		if (prtype & DATA_TRX_ID) {
			id = mach_read_from_6(data);

			fprintf(stderr, "trx_id {%lu %lu}",
				ut_dulint_get_high(id), ut_dulint_get_low(id));
		} else if (prtype & DATA_ROLL_PTR) {
			id = mach_read_from_7(data);

			fprintf(stderr, "roll_ptr {%lu %lu}",
				ut_dulint_get_high(id), ut_dulint_get_low(id));
		} else if (prtype & DATA_ROW_ID) {
			id = mach_read_from_6(data);

			fprintf(stderr, "row_id {%lu %lu}",
				ut_dulint_get_high(id), ut_dulint_get_low(id));
		} else {
			id = mach_dulint_read_compressed(data);

			fprintf(stderr, "mix_id {%lu %lu}",
				ut_dulint_get_high(id), ut_dulint_get_low(id));
		}

	} else {
		fputs(" Hex: ",stderr);

		for (i = 0; i < len; i++) {
			fprintf(stderr, "%02lx", (ulint)*data);
			data++;
		}
	}
}

/*****************************************************************
Print a dfield value using ut_print_buf. */
static
void
dfield_print_raw(
/*=============*/
	FILE*		f,		/* in: output stream */
	const dfield_t*	dfield)		/* in: dfield */
{
	ulint	len	= dfield->len;
	if (len != UNIV_SQL_NULL) {
		ulint	print_len = ut_min(len, 1000);
		ut_print_buf(f, dfield->data, print_len);
		if (len != print_len) {
			fprintf(f, "(total %lu bytes)", (ulong) len);
		}
	} else {
		fputs(" SQL NULL", f);
	}
}

/**************************************************************
The following function prints the contents of a tuple. */

void
dtuple_print(
/*=========*/
	FILE*		f,	/* in: output stream */
	const dtuple_t*	tuple)	/* in: tuple */
{
	ulint		n_fields;
	ulint		i;

	n_fields = dtuple_get_n_fields(tuple);

	fprintf(f, "DATA TUPLE: %lu fields;\n", (ulong) n_fields);

	for (i = 0; i < n_fields; i++) {
		fprintf(f, " %lu:", (ulong) i);

		dfield_print_raw(f, dtuple_get_nth_field(tuple, i));

		putc(';', f);
	}

	putc('\n', f);
	ut_ad(dtuple_validate(tuple));
}

/******************************************************************
Moves parts of long fields in entry to the big record vector so that
the size of tuple drops below the maximum record size allowed in the
database. Moves data only from those fields which are not necessary
to determine uniquely the insertion place of the tuple in the index. */

big_rec_t*
dtuple_convert_big_rec(
/*===================*/
				/* out, own: created big record vector,
				NULL if we are not able to shorten
				the entry enough, i.e., if there are
				too many fixed-length or short fields
				in entry or the index is clustered */
	dict_index_t*	index,	/* in: index */
	const dtuple_t*	entry,	/* in: index entry */
	const ulint*	ext_vec,/* in: array of externally stored fields,
				or NULL: if a field already is externally
				stored, then we cannot move it to the vector
				this function returns */
	ulint		n_ext_vec)/* in: number of elements is ext_vec */
{
	mem_heap_t*	heap;
	big_rec_t*	vector;
	dfield_t*	dfield;
	dict_field_t*	ifield;
	ulint		size;
	ulint		n_fields;

	if (UNIV_UNLIKELY(!dict_index_is_clust(index))) {
		return(NULL);
	}

	ut_a(dtuple_check_typed_no_assert(entry));

	size = rec_get_converted_size(index, entry, ext_vec, n_ext_vec);

	if (UNIV_UNLIKELY(size > 1000000000)) {
		fprintf(stderr,
			"InnoDB: Warning: tuple size very big: %lu\n",
			(ulong) size);
		fputs("InnoDB: Tuple contents: ", stderr);
		dtuple_print(stderr, entry);
		putc('\n', stderr);
	}

	heap = mem_heap_create(size + dtuple_get_n_fields(entry)
			       * sizeof(big_rec_field_t) + 1000);

	vector = mem_heap_alloc(heap, sizeof(big_rec_t));

	vector->heap = heap;
	vector->fields = mem_heap_alloc(heap, dtuple_get_n_fields(entry)
					* sizeof(big_rec_field_t));

	/* Decide which fields to shorten: the algorithm is to look for
	a variable-length field that yields the biggest savings when
	stored externally */

	n_fields = 0;

	while (page_zip_rec_needs_ext(rec_get_converted_size(index, entry,
							     ext_vec,
							     n_ext_vec),
				      dict_table_is_comp(index->table),
				      dict_table_zip_size(index->table))) {
		ulint	i;
		ulint	longest		= 0;
		ulint	longest_i	= ULINT_MAX;

		for (i = dict_index_get_n_unique_in_tree(index);
		     i < dtuple_get_n_fields(entry); i++) {
			ulint	savings;

			dfield = dtuple_get_nth_field(entry, i);
			ifield = dict_index_get_nth_field(index, i);

			/* Skip fixed-length or NULL or short columns */

			if (ifield->fixed_len
			    || dfield->len == UNIV_SQL_NULL
			    || dfield->len <= BTR_EXTERN_FIELD_REF_SIZE * 2) {
				goto skip_field;
			}

			savings = dfield->len - BTR_EXTERN_FIELD_REF_SIZE;

			/* Check that there would be savings */
			if (longest >= savings) {
				goto skip_field;
			}

			/* Skip externally stored columns */

			if (ext_vec) {
				ulint	j;

				for (j = 0; j < n_ext_vec; j++) {
					if (ext_vec[j] == i) {
						goto skip_field;
					}
				}
			}

			longest_i = i;
			longest = savings;

skip_field:
			continue;
		}

		if (!longest) {
			/* Cannot shorten more */

			mem_heap_free(heap);

			return(NULL);
		}

		/* Move data from field longest_i to big rec vector.

		We store the first bytes locally to the record. Then
		we can calculate all ordering fields in all indexes
		from locally stored data. */

		dfield = dtuple_get_nth_field(entry, longest_i);
		ifield = dict_index_get_nth_field(index, longest_i);
		vector->fields[n_fields].field_no = longest_i;

		vector->fields[n_fields].len = dfield->len;

		vector->fields[n_fields].data = dfield->data;

		/* Set the extern field reference in dfield to zero */
		dfield->len = BTR_EXTERN_FIELD_REF_SIZE;
		dfield->data = mem_heap_alloc(heap, BTR_EXTERN_FIELD_REF_SIZE);
		memset(dfield->data, 0, BTR_EXTERN_FIELD_REF_SIZE);
		n_fields++;
		ut_ad(n_fields < dtuple_get_n_fields(entry));
	}

	vector->n_fields = n_fields;
	return(vector);
}

/******************************************************************
Puts back to entry the data stored in vector. Note that to ensure the
fields in entry can accommodate the data, vector must have been created
from entry with dtuple_convert_big_rec. */

void
dtuple_convert_back_big_rec(
/*========================*/
	dict_index_t*	index __attribute__((unused)),	/* in: index */
	dtuple_t*	entry,	/* in: entry whose data was put to vector */
	big_rec_t*	vector)	/* in, own: big rec vector; it is
				freed in this function */
{
	dfield_t*	dfield;
	ulint		i;

	for (i = 0; i < vector->n_fields; i++) {

		dfield = dtuple_get_nth_field(entry,
					      vector->fields[i].field_no);
		dfield->data = vector->fields[i].data;
		dfield->len = vector->fields[i].len;
	}

	mem_heap_free(vector->heap);
}
