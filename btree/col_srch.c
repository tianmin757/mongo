/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_bt_search_col --
 *	Search a column-store tree for a specific record-based key.
 */
int
__wt_bt_search_col(WT_TOC *toc, u_int64_t recno, u_int32_t flags)
{
	DB *db;
	IDB *idb;
	WT_COL *cip;
	WT_COL_EXPAND *exp;
	WT_PAGE *page;
	WT_REPL *repl;
	u_int64_t record_cnt;
	u_int32_t addr, size, i;
	u_int16_t write_gen;
	int ret;

	toc->srch_page = NULL;			/* Return values. */
	toc->srch_ip = NULL;
	toc->srch_repl = repl = NULL;
	toc->srch_exp = exp = NULL;
	toc->srch_write_gen = 0;

	db = toc->db;
	idb = db->idb;

	WT_DB_FCHK(db, "__wt_bt_search_col", flags, WT_APIMASK_BT_SEARCH_COL);

restart:
	/* Check for a record past the end of the database. */
	page = idb->root_page;
	if (page->records < recno)
		return (WT_NOTFOUND);

	/* Search the tree. */
	for (;;) {
		/* Save the write generation value before the search. */
		write_gen = WT_PAGE_WRITE_GEN(page);

		/* Walk the page looking for the record. */
		switch (page->hdr->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			cip = page->u.icol + (recno - page->hdr->start_recno);
			goto done;
		case WT_PAGE_COL_RCC:
			/*
			 * Walk the page, counting records -- do the record
			 * count calculation in a funny way to avoid overflow.
			 */
			record_cnt = recno - page->hdr->start_recno;
			WT_INDX_FOREACH(page, cip, i) {
				if (record_cnt < WT_RCC_REPEAT_COUNT(cip->data))
					break;
				record_cnt -= WT_RCC_REPEAT_COUNT(cip->data);
			}
			goto done;
		case WT_PAGE_COL_INT:
		default:
			/*
			 * Walk the page, counting records -- do the record
			 * count calculation in a funny way to avoid overflow.
			 */
			record_cnt = recno - page->hdr->start_recno;
			WT_INDX_FOREACH(page, cip, i) {
				if (record_cnt < WT_COL_OFF_RECORDS(cip))
					break;
				record_cnt -= WT_COL_OFF_RECORDS(cip);
			}
			break;
		}

		/* cip references the subtree containing the record. */
		addr = WT_COL_OFF_ADDR(cip);
		size = WT_COL_OFF_SIZE(cip);

		/* Walk down to the next page. */
		if (page != idb->root_page)
			__wt_bt_page_out(toc, &page, 0);
		switch (ret = __wt_bt_page_in(toc, addr, size, 1, &page)) {
		case 0:
			break;
		case WT_RESTART:
			goto restart;
		default:
			goto err;
		}
	}

done:	/*
	 * We've found the right on-page WT_COL structure, but that's only the
	 * first step; the record may have been updated since reading the page
	 * into the cache.
	 */
	switch (page->hdr->type) {
	case WT_PAGE_COL_FIX:
		/*
		 * Check for a replacement in the page's WT_REPL array.  If
		 * found, check for deletion.   If not found, check for
		 * deletion in the original index.
		 */
		if ((repl = WT_COL_REPL(page, cip)) != NULL) {
			if (!LF_ISSET(WT_INSERT) && WT_REPL_DELETED_ISSET(repl))
				goto notfound;
			break;
		}
		if (!LF_ISSET(WT_INSERT) && WT_FIX_DELETE_ISSET(cip->data))
			goto notfound;
		break;
	case WT_PAGE_COL_RCC:
		/*
		 * Search for an individual record in the page's WT_COL_EXPAND
		 * array.  If found, the record has been modified before: check
		 * for deletion, and return its WT_COL_EXPAND entry.  If not
		 * found, check for deletion in the original index, and return
		 * the original index.
		 */
		for (exp =
		    WT_COL_EXPCOL(page, cip); exp != NULL; exp = exp->next)
			if (exp->recno == recno) {
				repl = exp->repl;
				if (!LF_ISSET(WT_INSERT) &&
				    WT_REPL_DELETED_ISSET(repl))
					goto notfound;
				break;
			}
		if (exp == NULL && !LF_ISSET(WT_INSERT) &&
		    WT_FIX_DELETE_ISSET(WT_RCC_REPEAT_DATA(cip->data)))
			goto notfound;
		break;
	case WT_PAGE_COL_VAR:
	default:
		/* Check for a replacement entry in the page's WT_REPL array. */
		if ((repl = WT_COL_REPL(page, cip)) != NULL) {
			if (!LF_ISSET(WT_INSERT) && WT_REPL_DELETED_ISSET(repl))
				goto notfound;
			break;
		}

		/* Otherwise, check to see if the item is deleted. */
		if (!LF_ISSET(WT_INSERT) &&
		    WT_ITEM_TYPE(cip->data) == WT_ITEM_DEL)
			goto notfound;
		break;
	}

	toc->srch_page = page;
	toc->srch_ip = cip;
	toc->srch_repl = repl;
	toc->srch_exp = exp;
	toc->srch_write_gen = write_gen;
	return (0);

notfound:
	ret = WT_NOTFOUND;

err:	if (page != idb->root_page)
		__wt_bt_page_out(toc, &page, 0);
	return (ret);
}
