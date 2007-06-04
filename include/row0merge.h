/******************************************************
Index build routines using a merge sort

(c) 2005 Innobase Oy

Created 13/06/2005 Jan Lindstrom
*******************************************************/

#ifndef row0merge_h
#define row0merge_h

#include "univ.i"
#include "data0data.h"
#include "dict0types.h"
#include "trx0types.h"
#include "que0types.h"
#include "mtr0mtr.h"
#include "rem0types.h"
#include "rem0rec.h"
#include "read0types.h"
#include "btr0types.h"
#include "row0mysql.h"

/* Information about temporary files used in merge sort are stored
to this structure */

struct merge_file_struct {
	os_file_t	file;			/* File descriptor */
	ulint		offset;			/* File offset */
	ulint		num_of_blocks;		/* Number of blocks */
};

typedef struct merge_file_struct merge_file_t;

/* This structure holds index field definitions */

struct merge_index_field_struct {
	ulint	col_type;	/* Column type */
	ulint	prefix_len;	/* Prefix len */
	char*	field_name;	/* Field name */
};

typedef struct merge_index_field_struct merge_index_field_t;

/* This structure holds index definitions */

struct merge_index_def_struct {
	ulint	n_fields;		/* Number of fields in index */
	ulint	ind_type;		/* 0, DICT_UNIQUE or DICT_CLUSTERED */
	char*	name;			/* Index name */
	merge_index_field_t* fields;	/* Field definitions */
};

typedef struct merge_index_def_struct merge_index_def_t;

/************************************************************************
Reads clustered index of the table and create temporary files
containing index entries for indexes to be built. */

ulint
row_merge_read_clustered_index(
/*===========================*/
					/* out: DB_SUCCESS if successfull,
					or ERROR code */
	trx_t*		trx,		/* in: transaction */
	dict_table_t*	table,		/* in: table where index is created */
	dict_index_t**	index,		/* in: indexes to be created */
	merge_file_t*	files,		/* in: Files where to write index
					entries */
	ulint		num_of_idx);	/* in: number of indexes to be
					created */
/************************************************************************
Read sorted file containing index data tuples and insert these data
data tuples to the index */

ulint
row_merge_insert_index_tuples(
/*==========================*/
					/* out: 0 or error number */
	trx_t*		trx, 		/* in: transaction */
	dict_index_t*	index,		/* in: index */
	dict_table_t*	table,		/* in: table */
	os_file_t	file,		/* in: file handle */
	ulint		offset);	/* in: offset where to start
					reading */
/*****************************************************************
Merge sort for linked list in the disk. */

ulint
row_merge_sort_linked_list_in_disk(
/*===============================*/
					/* out: offset to first block in
					the list or ULINT_UNDEFINED in
					case of error */
	dict_index_t*	index,		/* in: index to be created */
	os_file_t	file,		/* in: File handle */
	int*		error);		/* out: 0 or error */
/*************************************************************************
Drop an index from the InnoDB system tables. */

void
row_merge_drop_index(
/*=================*/
				/* out: error code or DB_SUCCESS */
	dict_index_t*	index,	/* in: index to be removed */
	dict_table_t*	table,	/* in: table */
	trx_t*		trx);	/* in: transaction handle */
/*************************************************************************
Drop those indexes which were created before an error occurred
when building an index. */

void
row_merge_drop_indexes(
/*===================*/
	trx_t*		trx,		/* in: transaction */
	dict_table_t*	table,		/* in: table containing the indexes */
	dict_index_t**	index,		/* in: indexes to drop */
	ulint		num_created);	/* in: number of elements in index[] */
/*************************************************************************
Initialize memory for a merge file structure */

void
row_merge_file_create(
/*==================*/
	merge_file_t*	merge_file);	/* out: merge file structure */

/*************************************************************************
Create a temporary table using a definition of the old table. You must
lock data dictionary before calling this function. */

dict_table_t*
row_merge_create_temporary_table(
/*=============================*/
					/* out: new temporary table
					definition */
	const char*	table_name,	/* in: new table name */
	dict_table_t*	table,		/* in: old table definition */
	trx_t*		trx,		/* in: trx */
	ulint*		error);		/* in:out/ error code or DB_SUCCESS */
/*************************************************************************
Update all prebuilts for this table */

void
row_merge_prebuilts_update(
/*=======================*/

	trx_t*		trx,		/* in: trx */
	dict_table_t*	old_table);	/* in: old table */
/*************************************************************************
Create a temporary table using a definition of the old table. You must
lock data dictionary before calling this function. */

dict_table_t*
row_merge_create_temporary_table(
/*=============================*/
					/* out: new temporary table
					definition */
	const char*	table_name,	/* in: new table name */
	dict_table_t*	table,		/* in: old table definition */
	trx_t*		trx,		/* in: trx */
	ulint*		error);		/* in:out/ error code or DB_SUCCESS */
/*************************************************************************
Rename the indexes in the dicitionary. */

ulint
row_merge_rename_index(
/*===================*/
					/* out: DB_SUCCESS if all OK */
	trx_t*		trx,		/* in: Transaction */
	dict_table_t*	table,		/* in: Table for index */
	dict_index_t*	index);		/* in: Index to rename */
/*************************************************************************
Create the index and load in to the dicitionary. */

ulint
row_merge_create_index(
/*===================*/
					/* out: DB_SUCCESS if all OK */
	trx_t*		trx,		/* in: transaction */
	dict_index_t**	index,		/* out: the instance of the index */
	dict_table_t*	table,		/* in: the index is on this table */
	const merge_index_def_t*	/* in: the index definition */
			index_def);
/*************************************************************************
Check if a transaction can use an index.*/

ibool
row_merge_is_index_usable(
/*======================*/
					/* out: TRUE if index can be used by
					the transaction else FALSE*/
	const trx_t*		trx,	/* in: transaction */
	const dict_index_t*	index);	/* in: index to check */
/*************************************************************************
If there are views that refer to the old table name then we "attach" to
the new instance of the table else we drop it immediately.*/

ulint
row_merge_drop_table(
/*=================*/
					/* out: DB_SUCCESS if all OK else
					error code.*/
	trx_t*		trx,		/* in: transaction */
	dict_table_t*	table);		/* in: table instance to drop */
#endif /* row0merge.h */
