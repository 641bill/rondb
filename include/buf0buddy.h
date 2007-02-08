/******************************************************
Binary buddy allocator for compressed pages

(c) 2006 Innobase Oy

Created December 2006 by Marko Makela
*******************************************************/

#ifndef buf0buddy_h
#define buf0buddy_h

#ifdef UNIV_MATERIALIZE
# undef UNIV_INLINE
# define UNIV_INLINE
#endif

#include "univ.i"
#include "buf0types.h"

/**************************************************************************
Allocate a block.  The thread calling this function must hold
buf_pool->mutex and must not hold buf_pool->zip_mutex or any block->mutex.
The buf_pool->mutex may only be released and reacquired if
lru == BUF_BUDDY_USE_LRU. */
UNIV_INLINE
void*
buf_buddy_alloc(
/*============*/
			/* out: allocated block,
			possibly NULL if lru == NULL */
	ulint	size,	/* in: block size, up to UNIV_PAGE_SIZE */
	ibool*	lru)	/* in: pointer to a variable that will be assigned
			TRUE if storage was allocated from the LRU list
			and buf_pool->mutex was temporarily released,
			or NULL if the LRU list should not be used */
	__attribute__((malloc));

/**************************************************************************
Release a block. */
UNIV_INLINE
void
buf_buddy_free(
/*===========*/
	void*	buf,	/* in: block to be freed, must not be
			pointed to by the buffer pool */
	ulint	size)	/* in: block size, up to UNIV_PAGE_SIZE */
	__attribute__((nonnull));

#ifndef UNIV_NONINL
# include "buf0buddy.ic"
#endif

#endif /* buf0buddy_h */
