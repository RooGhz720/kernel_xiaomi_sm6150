// SPDX-License-Identifier: GPL-2.0
/*
 * Common Primitives for Data Access Monitoring
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>

#include "ops-common.h"

/*
 * Get an online page for a pfn if it's in the LRU list.  Otherwise, returns
 * NULL.
 *
 * The body of this function is stolen from the 'page_idle_get_page()'.  We
 * steal rather than reuse it because the code is quite simple.
 */
struct page *damon_get_page(unsigned long pfn)
{
	struct page *page = pfn_to_online_page(pfn);

	if (!page || !PageLRU(page) || !get_page_unless_zero(page))
		return NULL;

	if (unlikely(!PageLRU(page))) {
		put_page(page);
		page = NULL;
	}
	return page;
}

void damon_ptep_mkold(pte_t *pte, struct mm_struct *mm, unsigned long addr)
{
	bool referenced = false;
	struct page *page = damon_get_page(pte_pfn(*pte));

	if (!page)
		return;

	if (pte_young(*pte)) {
		referenced = true;
		*pte = pte_mkold(*pte);
	}

#ifdef CONFIG_MMU_NOTIFIER
	if (mmu_notifier_clear_young(mm, addr, addr + PAGE_SIZE))
		referenced = true;
#endif /* CONFIG_MMU_NOTIFIER */

	if (referenced)
		set_page_young(page);

	set_page_idle(page);
	put_page(page);
}

void damon_pmdp_mkold(pmd_t *pmd, struct mm_struct *mm, unsigned long addr)
{
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	bool referenced = false;
	struct page *page = damon_get_page(pmd_pfn(*pmd));

	if (!page)
		return;

	if (pmd_young(*pmd)) {
		referenced = true;
		*pmd = pmd_mkold(*pmd);
	}

#ifdef CONFIG_MMU_NOTIFIER
	if (mmu_notifier_clear_young(mm, addr, addr + HPAGE_PMD_SIZE))
		referenced = true;
#endif /* CONFIG_MMU_NOTIFIER */

	if (referenced)
		set_page_young(page);

	set_page_idle(page);
	put_page(page);
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
}

static bool __damon_pa_mkold(struct page *page, struct vm_area_struct *vma,
		unsigned long addr, void *arg)
{
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = addr,
	};

	while (page_vma_mapped_walk(&pvmw)) {
		addr = pvmw.address;
		if (pvmw.pte)
			damon_ptep_mkold(pvmw.pte, vma->vm_mm, addr);
		else
			damon_pmdp_mkold(pvmw.pmd, vma->vm_mm, addr);
	}
	return true;
}

void damon_pa_mkold(unsigned long paddr)
{
	struct page *page = damon_get_page(PHYS_PFN(paddr));
	struct rmap_walk_control rwc = {
		.rmap_one = __damon_pa_mkold,
		.anon_lock = page_lock_anon_vma_read,
	};
	bool need_lock;

	if (!page)
		return;

	if (!page_mapped(page) || !page_rmapping(page)) {
		set_page_idle(page);
		goto out;
	}

	need_lock = !PageAnon(page) || PageKsm(page);
	if (need_lock && !trylock_page(page))
		goto out;

	rmap_walk(page, &rwc);

	if (need_lock)
		unlock_page(page);

out:
	put_page(page);
}

struct damon_pa_access_chk_result {
	unsigned long page_sz;
	bool accessed;
};

static bool __damon_pa_young(struct page *page, struct vm_area_struct *vma,
		unsigned long addr, void *arg)
{
	struct damon_pa_access_chk_result *result = arg;
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = addr,
	};

	result->accessed = false;
	result->page_sz = PAGE_SIZE;
	while (page_vma_mapped_walk(&pvmw)) {
		addr = pvmw.address;
		if (pvmw.pte) {
			result->accessed = pte_young(*pvmw.pte) ||
				!page_is_idle(page) ||
				mmu_notifier_test_young(vma->vm_mm, addr);
		} else {
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
			result->accessed = pmd_young(*pvmw.pmd) ||
				!page_is_idle(page) ||
				mmu_notifier_test_young(vma->vm_mm, addr);
			result->page_sz = HPAGE_PMD_SIZE;
#else
			WARN_ON_ONCE(1);
#endif	/* CONFIG_TRANSPARENT_HUGEPAGE */
		}
		if (result->accessed) {
			page_vma_mapped_walk_done(&pvmw);
			break;
		}
	}

	/* If accessed, stop walking */
	return !result->accessed;
}

bool damon_pa_young(unsigned long paddr, unsigned long *page_sz)
{
	struct page *page = damon_get_page(PHYS_PFN(paddr));
	struct damon_pa_access_chk_result result = {
		.page_sz = PAGE_SIZE,
		.accessed = false,
	};
	struct rmap_walk_control rwc = {
		.arg = &result,
		.rmap_one = __damon_pa_young,
		.anon_lock = page_lock_anon_vma_read,
	};
	bool need_lock;

	if (!page)
		return false;

	if (!page_mapped(page) || !page_rmapping(page)) {
		if (page_is_idle(page))
			result.accessed = false;
		else
			result.accessed = true;
		put_page(page);
		goto out;
	}

	need_lock = !PageAnon(page) || PageKsm(page);
	if (need_lock && !trylock_page(page)) {
		put_page(page);
		return false;
	}

	rmap_walk(page, &rwc);

	if (need_lock)
		unlock_page(page);
	put_page(page);

out:
	*page_sz = result.page_sz;
	return result.accessed;
}

#define DAMON_MAX_SUBSCORE	(100)
#define DAMON_MAX_AGE_IN_LOG	(32)

int damon_pageout_score(struct damon_ctx *c, struct damon_region *r,
			struct damos *s)
{
	unsigned int max_nr_accesses;
	int freq_subscore;
	unsigned int age_in_sec;
	int age_in_log, age_subscore;
	unsigned int freq_weight = s->quota.weight_nr_accesses;
	unsigned int age_weight = s->quota.weight_age;
	int hotness;

	max_nr_accesses = c->aggr_interval / c->sample_interval;
	freq_subscore = r->nr_accesses * DAMON_MAX_SUBSCORE / max_nr_accesses;

	age_in_sec = (unsigned long)r->age * c->aggr_interval / 1000000;
	for (age_in_log = 0; age_in_log < DAMON_MAX_AGE_IN_LOG && age_in_sec;
			age_in_log++, age_in_sec >>= 1)
		;

	/* If frequency is 0, higher age means it's colder */
	if (freq_subscore == 0)
		age_in_log *= -1;

	/*
	 * Now age_in_log is in [-DAMON_MAX_AGE_IN_LOG, DAMON_MAX_AGE_IN_LOG].
	 * Scale it to be in [0, 100] and set it as age subscore.
	 */
	age_in_log += DAMON_MAX_AGE_IN_LOG;
	age_subscore = age_in_log * DAMON_MAX_SUBSCORE /
		DAMON_MAX_AGE_IN_LOG / 2;

	hotness = (freq_weight * freq_subscore + age_weight * age_subscore);
	if (freq_weight + age_weight)
		hotness /= freq_weight + age_weight;
	/*
	 * Transform it to fit in [0, DAMOS_MAX_SCORE]
	 */
	hotness = hotness * DAMOS_MAX_SCORE / DAMON_MAX_SUBSCORE;

	/* Return coldness of the region */
	return DAMOS_MAX_SCORE - hotness;
}

int damon_hot_score(struct damon_ctx *c, struct damon_region *r,
			struct damos *s)
{
	unsigned int max_nr_accesses;
	int freq_subscore;
	unsigned int age_in_sec;
	int age_in_log, age_subscore;
	unsigned int freq_weight = s->quota.weight_nr_accesses;
	unsigned int age_weight = s->quota.weight_age;
	int hotness;

	max_nr_accesses = c->aggr_interval / c->sample_interval;
	freq_subscore = r->nr_accesses * DAMON_MAX_SUBSCORE / max_nr_accesses;

	age_in_sec = (unsigned long)r->age * c->aggr_interval / 1000000;
	for (age_in_log = 0; age_in_log < DAMON_MAX_AGE_IN_LOG && age_in_sec;
			age_in_log++, age_in_sec >>= 1)
		;

	/* If frequency is 0, higher age means it's colder */
	if (freq_subscore == 0)
		age_in_log *= -1;

	/*
	 * Now age_in_log is in [-DAMON_MAX_AGE_IN_LOG, DAMON_MAX_AGE_IN_LOG].
	 * Scale it to be in [0, 100] and set it as age subscore.
	 */
	age_in_log += DAMON_MAX_AGE_IN_LOG;
	age_subscore = age_in_log * DAMON_MAX_SUBSCORE /
		DAMON_MAX_AGE_IN_LOG / 2;

	hotness = (freq_weight * freq_subscore + age_weight * age_subscore);
	if (freq_weight + age_weight)
		hotness /= freq_weight + age_weight;
	/*
	 * Transform it to fit in [0, DAMOS_MAX_SCORE]
	 */
	hotness = hotness * DAMOS_MAX_SCORE / DAMON_MAX_SUBSCORE;

	return hotness;
}
