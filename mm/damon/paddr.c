// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON Primitives for The Physical Address Space
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#define pr_fmt(fmt) "damon-pa: " fmt

#include <linux/page_idle.h>
#include <linux/swap.h>

#include "../internal.h"
#include "prmtv-common.h"

static void __damon_pa_prepare_access_check(struct damon_ctx *ctx,
					    struct damon_region *r)
{
	r->sampling_addr = damon_rand(r->ar.start, r->ar.end);

	damon_pa_mkold(r->sampling_addr);
}

static void damon_pa_prepare_access_checks(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t)
			__damon_pa_prepare_access_check(ctx, r);
	}
}

static void __damon_pa_check_access(struct damon_ctx *ctx,
				    struct damon_region *r)
{
	static unsigned long last_addr;
	static unsigned long last_page_sz = PAGE_SIZE;
	static bool last_accessed;

	/* If the region is in the last checked page, reuse the result */
	if (ALIGN_DOWN(last_addr, last_page_sz) ==
				ALIGN_DOWN(r->sampling_addr, last_page_sz)) {
		if (last_accessed)
			r->nr_accesses++;
		return;
	}

	last_accessed = damon_pa_young(r->sampling_addr, &last_page_sz);
	if (last_accessed)
		r->nr_accesses++;

	last_addr = r->sampling_addr;
}

static unsigned int damon_pa_check_accesses(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;
	unsigned int max_nr_accesses = 0;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t) {
			__damon_pa_check_access(ctx, r);
			max_nr_accesses = max(r->nr_accesses, max_nr_accesses);
		}
	}

	return max_nr_accesses;
}

bool damon_pa_target_valid(void *t)
{
	return true;
}

static int damon_pa_apply_scheme(struct damon_ctx *ctx, struct damon_target *t,
		struct damon_region *r, struct damos *scheme)
{
	unsigned long addr;
	LIST_HEAD(page_list);

	if (scheme->action != DAMOS_PAGEOUT)
		return -EINVAL;

	for (addr = r->ar.start; addr < r->ar.end; addr += PAGE_SIZE) {
		struct page *page = damon_get_page(PHYS_PFN(addr));

		if (!page)
			continue;

		ClearPageReferenced(page);
		test_and_clear_page_young(page);
		if (isolate_lru_page(page)) {
			put_page(page);
			continue;
		}
		if (PageUnevictable(page)) {
			putback_lru_page(page);
		} else {
			list_add(&page->lru, &page_list);
			put_page(page);
		}
	}
	reclaim_pages(&page_list);
	cond_resched();
	return 0;
}

static int damon_pa_scheme_score(struct damon_ctx *context,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme)
{
	switch (scheme->action) {
	case DAMOS_PAGEOUT:
		return damon_pageout_score(context, r, scheme);
	default:
		break;
	}

	return DAMOS_MAX_SCORE;
}

void damon_pa_set_primitives(struct damon_ctx *ctx)
{
	ctx->primitive.init = NULL;
	ctx->primitive.update = NULL;
	ctx->primitive.prepare_access_checks = damon_pa_prepare_access_checks;
	ctx->primitive.check_accesses = damon_pa_check_accesses;
	ctx->primitive.reset_aggregated = NULL;
	ctx->primitive.target_valid = damon_pa_target_valid;
	ctx->primitive.cleanup = NULL;
	ctx->primitive.apply_scheme = damon_pa_apply_scheme;
	ctx->primitive.get_scheme_score = damon_pa_scheme_score;
}
