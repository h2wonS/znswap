// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/swap_state.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *
 *  Rewritten to use page cache, (C) 1998 Stephen Tweedie
 */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/pagevec.h>
#include <linux/migrate.h>
#include <linux/vmalloc.h>
#include <linux/swap_slots.h>
#include <linux/huge_mm.h>
#include <linux/shmem_fs.h>
#include "internal.h"

/*
 * swapper_space is a fiction, retained to simplify the path through
 * vmscan's shrink_page_list.
 */
static const struct address_space_operations swap_aops = {
	.writepage	= swap_writepage,
	.set_page_dirty	= swap_set_page_dirty,
#ifdef CONFIG_MIGRATION
	.migratepage	= migrate_page,
#endif
};

extern spinlock_t *lock_zone_cap_offset(struct zns_swap_info_struct *zi,
						unsigned int offset);
struct address_space *swapper_spaces[MAX_SWAPFILES] __read_mostly;
static unsigned int nr_swapper_spaces[MAX_SWAPFILES] __read_mostly;
static bool enable_vma_readahead __read_mostly = true;
int cached_per __read_mostly = 5;
int trim_4k __read_mostly = 0;

#define INC_CACHE_INFO(x)	data_race(swap_cache_info.x++)
#define ADD_CACHE_INFO(x, nr)	data_race(swap_cache_info.x += (nr))

static struct {
	unsigned long add_total;
	unsigned long del_total;
	unsigned long find_success;
	unsigned long find_total;
} swap_cache_info;

static atomic_t swapin_readahead_hits = ATOMIC_INIT(4);

void show_swap_cache_info(void)
{
	printk("%lu pages in swap cache\n", total_swapcache_pages());
	printk("Swap cache stats: add %lu, delete %lu, find %lu/%lu\n",
		swap_cache_info.add_total, swap_cache_info.del_total,
		swap_cache_info.find_success, swap_cache_info.find_total);
	printk("Free swap  = %ldkB\n",
		get_nr_swap_pages() << (PAGE_SHIFT - 10));
	printk("Total swap = %lukB\n", total_swap_pages << (PAGE_SHIFT - 10));
}

void *get_shadow_from_swap_cache(swp_entry_t entry)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	struct page *page;

	page = xa_load(&address_space->i_pages, idx);
	if (xa_is_value(page))
		return page;
	return NULL;
}

int __add_to_swap_cache(struct page *page, swp_entry_t entry,
			gfp_t gfp, void **shadowp, bool zone_acc, bool irq_lock)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	struct swap_info_struct *st;
	struct  zns_swap_info_struct *zi = NULL;
	int zone;
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, compound_order(page));
	unsigned long i, nr = thp_nr_pages(page);
	void *old;

	st = swap_type_to_swap_info(swp_type(entry));

	if (st)
		zi = st->zns_swap;

        BUG_ON(!zi);

	VM_BUG_ON_PAGE(!PageSwapBacked(page), page);

	page_ref_add(page, nr);
	SetPageSwapCache(page);

	do {
		unsigned long nr_shadows = 0;

		if (irq_lock)
			xas_lock_irq(&xas);
		else
			xas_lock(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			VM_BUG_ON_PAGE(xas.xa_index != idx + i, page);
			old = xas_load(&xas);
			if (xa_is_value(old)) {
				nr_shadows++;
				if (shadowp)
					*shadowp = old;
			}
			set_page_private(page + i, entry.val + i);
			xas_store(&xas, page);
			xas_next(&xas);
		}
		address_space->nrexceptional -= nr_shadows;
		address_space->nrpages += nr;
		__mod_node_page_state(page_pgdat(page), NR_FILE_PAGES, nr);
		__mod_lruvec_page_state(page, NR_SWAPCACHE, nr);
		ADD_CACHE_INFO(add_total, nr);
unlock:
		if (irq_lock)
			xas_unlock_irq(&xas);
		else
			xas_unlock(&xas);
	} while (xas_nomem(&xas, gfp));

	if (!xas_error(&xas)) {
		if (zone_acc && zi) {
			int has_cache;
			zone = zns_offset_to_zone_cap(zi, swp_offset(entry));
			has_cache = atomic_inc_return(&zi->swap_zones[zone].has_cache);
		}
		return 0;
	}

	ClearPageSwapCache(page);
	page_ref_sub(page, nr);
	return xas_error(&xas);
}

inline int _add_to_swap_cache(struct page *page, swp_entry_t entry,
			gfp_t gfp, void **shadowp, bool zone_acc) {
	return __add_to_swap_cache(page, entry, gfp, shadowp, zone_acc, false);
}
inline int add_to_swap_cache(struct page *page, swp_entry_t entry,
			gfp_t gfp, void **shadowp) {
	return __add_to_swap_cache(page, entry, gfp, shadowp, true, true);
}
/*
 * This must be called only on pages that have
 * been verified to be in the swap cache.
 */
inline void __delete_from_swap_cache(struct page *page,
			swp_entry_t entry, void *shadow) {
	return ___delete_from_swap_cache(page, entry, shadow, true);
}
void ___delete_from_swap_cache(struct page *page,
			swp_entry_t entry, void *shadow, bool zone_acc)
{
	struct address_space *address_space = swap_address_space(entry);
	struct swap_info_struct *st;
	struct zns_swap_info_struct *zi = NULL;
	int zone;
	int i, nr = thp_nr_pages(page);
	pgoff_t idx = swp_offset(entry);
	XA_STATE(xas, &address_space->i_pages, idx);
	st = swap_type_to_swap_info(swp_type(entry));
	if (st)
		zi = st->zns_swap;

        BUG_ON(!zi);

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageSwapCache(page), page);
	VM_BUG_ON_PAGE(PageWriteback(page), page);

	for (i = 0; i < nr; i++) {
		void *entry = xas_store(&xas, shadow);
		VM_BUG_ON_PAGE(entry != page, entry);
		set_page_private(page + i, 0);
		xas_next(&xas);
	}
	if (zone_acc && zi) {
		zone = zns_offset_to_zone_cap(zi, swp_offset(entry));
		atomic_dec(&zi->swap_zones[zone].has_cache);
	}
	ClearPageSwapCache(page);
	if (shadow)
		address_space->nrexceptional += nr;
	address_space->nrpages -= nr;
	__mod_node_page_state(page_pgdat(page), NR_FILE_PAGES, -nr);
	__mod_lruvec_page_state(page, NR_SWAPCACHE, -nr);
	ADD_CACHE_INFO(del_total, nr);
}


int add_to_zswap(struct page *page)
{
	swp_entry_t entry;
	struct swap_info_struct *si;
	unsigned long type;
	pgoff_t off;
	int err;
	int zone;
	pgoff_t zone_off;
	int slot_count;
        struct page_ext *pe;

	VM_BUG_ON_PAGE(!PageUptodate(page), page);

	si = page_swap_info(page);
	entry.val = page_private(page);
	off = swp_offset(entry);
	type = swp_type(entry);
	zone = zns_offset_to_zone(si->zns_swap, off);
	zone_off = zns_offset_to_zone_off(si->zns_swap, off);

	entry = zns_swp_entry(type, off);
	WRITE_ONCE(si->zns_swap->swap_zones[zone].swap_map[zone_off], SWAP_HAS_CACHE);

#if 1 
        pe = get_page_stats(page);
        WRITE_ONCE(si->zns_swap->swap_zones[zone].mapping_arr[zone_off].index, page->index);
        WRITE_ONCE(si->zns_swap->swap_zones[zone].mapping_arr[zone_off].mapping, page->mapping);
        WRITE_ONCE(si->zns_swap->swap_zones[zone].mapping_arr[zone_off].accessed_bitmap, pe->accessed_bitmap);
        WRITE_ONCE(si->zns_swap->swap_zones[zone].mapping_arr[zone_off].num_samples, pe->num_samples);

#endif

	slot_count = atomic_inc_return(&si->zns_swap->swap_zones[zone].slot_count);
	if (slot_count == si->zns_swap->zone_capacity) {
		atomic_set(&zns_si->zns_swap->swap_zones[zone].open, 3);
		atomic_inc(&zns_si->zns_swap->available_open_zones);
	}

#if 0 
                printk("[%s::%s::%d] SLOT %d, mapping=0x%lx index=0x%lx, zone=%d, zone_off=0x%lx\n",
                __FILE__, __func__, __LINE__, slot_count, page->mapping, page->index, zone, zone_off);
#endif


	/* do not account the page in swap cache here */
	err = _add_to_swap_cache(page, entry,
			__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN, NULL,
			true);
	page_ref_sub(page, 1);
	if (err) {
		/* did not manage to add to swap cache, why? */
		pr_err("failed to add to swap cache %d\n", err);
		BUG();
		/*
		 * add_to_swap_cache() doesn't return -EEXIST, so we can safely
		 * clear SWAP_HAS_CACHE flag.
		 */
		goto fail;
	}

	if (mem_cgroup_try_charge_swap(page, entry)) {
		put_swap_page(page, entry);
		entry.val = 0;
	}

	return 1;

fail:
	return 0;
}

/**
 * add_to_swap - allocate swap space for a page
 * @page: page we want to move to swap
 *
 * Allocate swap space for the page and add the page to the
 * swap cache.  Caller needs to hold the page lock. 
 */
int add_to_swap(struct page *page, struct swappolicy *sp,
		bool *requires_flush)
{
	swp_entry_t entry;
	int err;

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageUptodate(page), page);

	entry = get_swap_page(page, sp, requires_flush);
	if (!entry.val){
		return 0;
        }

	if (is_zns_tmp_swp_entry(entry)) {
		/* tmp - do not support THP yet */
		VM_BUG_ON_PGFLAGS(PageHead(page), page);
		/* treat as psuedo-swap cache - without XA entry
		 * useful for page_mapped*/
		page_ref_add(page, 1);
		set_page_private(page, entry.val);
		SetPageSwapCache(page);
		set_page_dirty(page);
		return 2;
	}

	/*
	 * XArray node allocations from PF_MEMALLOC contexts could
	 * completely exhaust the page allocator. __GFP_NOMEMALLOC
	 * stops emergency reserves from being allocated.
	 *
	 * TODO: this could cause a theoretical memory reclaim
	 * deadlock in the swap out path.
	 */
	/*
	 * Add it to the swap cache.
	 */
	err = add_to_swap_cache(page, entry,
			__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN, NULL);
	if (err)
		/*
		 * add_to_swap_cache() doesn't return -EEXIST, so we can safely
		 * clear SWAP_HAS_CACHE flag.
		 */
		goto fail;
	/*
	 * Normally the page will be dirtied in unmap because its pte should be
	 * dirty. A special case is MADV_FREE page. The page's pte could have
	 * dirty bit cleared but the page's SwapBacked bit is still set because
	 * clearing the dirty bit and SwapBacked bit has no lock protected. For
	 * such page, unmap will not set dirty bit for it, so page reclaim will
	 * not write the page out. This can cause data corruption when the page
	 * is swap in later. Always setting the dirty bit for the page solves
	 * the problem.
	 */
	set_page_dirty(page);
	return 1;

fail:
	put_swap_page(page, entry);
	return 0;
}

/*
 * This must be called only on pages that have
 * been verified to be in the swap cache and locked.
 * It will never put the page into the free list,
 * the caller has a reference on the page.
 */
void delete_from_swap_cache(struct page *page)
{
	swp_entry_t entry = { .val = page_private(page) };
	struct address_space *address_space = swap_address_space(entry);

	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_cache(page, entry, NULL);
	xa_unlock_irq(&address_space->i_pages);

	put_swap_page(page, entry);
	page_ref_sub(page, thp_nr_pages(page));
}

void clear_shadow_from_swap_cache(int type, unsigned long begin,
				unsigned long end)
{
	unsigned long curr = begin;
	void *old;

	for (;;) {
		unsigned long nr_shadows = 0;
		swp_entry_t entry = swp_entry(type, curr);
		struct address_space *address_space = swap_address_space(entry);
		XA_STATE(xas, &address_space->i_pages, curr);

		xa_lock_irq(&address_space->i_pages);
		xas_for_each(&xas, old, end) {
			if (!xa_is_value(old))
				continue;
			xas_store(&xas, NULL);
			nr_shadows++;
		}
		address_space->nrexceptional -= nr_shadows;
		xa_unlock_irq(&address_space->i_pages);

		/* search the next swapcache until we meet end */
		curr >>= SWAP_ADDRESS_SPACE_SHIFT;
		curr++;
		curr <<= SWAP_ADDRESS_SPACE_SHIFT;
		if (curr > end)
			break;
	}
}

/* 
 * If we are the only user, then try to free up the swap cache. 
 * 
 * Its ok to check for PageSwapCache without the page lock
 * here because we are going to recheck again inside
 * try_to_free_swap() _with_ the lock.
 * 					- Marcelo
 */
static inline void free_swap_cache(struct page *page)
{
	if (PageSwapCache(page) && !page_mapped(page)) {
		if(trylock_page(page)) {
			try_to_free_swap(page);
			unlock_page(page);
		}
	}
}

/* 
 * Perform a free_page(), also freeing any swap cache associated with
 * this page if it is the last user of the page.
 */
void free_page_and_swap_cache(struct page *page)
{
	free_swap_cache(page);
	if (!is_huge_zero_page(page))
		put_page(page);
}

/*
 * Passed an array of pages, drop them all from swapcache and then release
 * them.  They are removed from the LRU and freed if this is their last use.
 */
void free_pages_and_swap_cache(struct page **pages, int nr)
{
	struct page **pagep = pages;
	int i;

	lru_add_drain();
	for (i = 0; i < nr; i++)
		free_swap_cache(pagep[i]);
	release_pages(pagep, nr);
}

static inline bool swap_use_vma_readahead(void)
{
	return READ_ONCE(enable_vma_readahead) && !atomic_read(&nr_rotate_swap);
}

/*
 * Lookup a swap entry in the swap cache. A found page will be returned
 * unlocked and with its refcount incremented - we rely on the kernel
 * lock getting page table operations atomic even if we drop the page
 * lock before returning.
 */
struct page *lookup_swap_cache(swp_entry_t entry, struct vm_area_struct *vma,
			       unsigned long addr)
{
	struct page *page;
	struct swap_info_struct *si;

	si = get_swap_device(entry);
	if (!si)
		return NULL;
	page = find_get_page(swap_address_space(entry), swp_offset(entry));
	put_swap_device(si);

	INC_CACHE_INFO(find_total);
	if (page) {
		bool vma_ra = swap_use_vma_readahead();
		bool readahead;

		INC_CACHE_INFO(find_success);
		/*
		 * At the moment, we don't support PG_readahead for anon THP
		 * so let's bail out rather than confusing the readahead stat.
		 */
		if (unlikely(PageTransCompound(page)))
			return page;

		readahead = TestClearPageReadahead(page);
		if (vma && vma_ra) {
			unsigned long ra_val;
			int win, hits;

			ra_val = GET_SWAP_RA_VAL(vma);
			win = SWAP_RA_WIN(ra_val);
			hits = SWAP_RA_HITS(ra_val);
			if (readahead)
				hits = min_t(int, hits + 1, SWAP_RA_HITS_MAX);
			atomic_long_set(&vma->swap_readahead_info,
					SWAP_RA_VAL(addr, win, hits));
		}

		if (readahead) {
			count_vm_event(SWAP_RA_HIT);
			if (!vma || !vma_ra)
				atomic_inc(&swapin_readahead_hits);
		}
	}

	return page;
}

/**
 * find_get_incore_page - Find and get a page from the page or swap caches.
 * @mapping: The address_space to search.
 * @index: The page cache index.
 *
 * This differs from find_get_page() in that it will also look for the
 * page in the swap cache.
 *
 * Return: The found page or %NULL.
 */
struct page *find_get_incore_page(struct address_space *mapping, pgoff_t index)
{
	swp_entry_t swp;
	struct swap_info_struct *si;
	struct page *page = pagecache_get_page(mapping, index,
						FGP_ENTRY | FGP_HEAD, 0);

	if (!page)
		return page;
	if (!xa_is_value(page))
		return find_subpage(page, index);
	if (!shmem_mapping(mapping))
		return NULL;

	swp = radix_to_swp_entry(page);
	/* Prevent swapoff from happening to us */
	si = get_swap_device(swp);
	if (!si)
		return NULL;
	page = find_get_page(swap_address_space(swp), swp_offset(swp));
	put_swap_device(si);
	return page;
}

struct page *__read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
			struct vm_area_struct *vma, unsigned long addr,
			bool *new_page_allocated)
{
	struct swap_info_struct *si;
	struct page *page;
	void *shadow = NULL;

	*new_page_allocated = false;

	for (;;) {
		int err;
		/*
		 * First check the swap cache.  Since this is normally
		 * called after lookup_swap_cache() failed, re-calling
		 * that would confuse statistics.
		 */
		si = get_swap_device(entry);
                if(!si->zns_swap)
                    BUG();
		if (!si)
			return NULL;
		page = find_get_page(swap_address_space(entry),
				     swp_offset(entry));
		put_swap_device(si);
		if (page)
			return page;

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!__swp_swapcount(entry) && swap_slot_cache_enabled)
			return NULL;

		/*
		 * Get a new page to read into from swap.  Allocate it now,
		 * before marking swap_map SWAP_HAS_CACHE, when -EEXIST will
		 * cause any racers to loop around until we add it to cache.
		 */
		page = alloc_page_vma(gfp_mask, vma, addr);
		if (!page)
			return NULL;

		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare(entry);
		if (!err)
			break;

		put_page(page);
		if (err != -EEXIST)
			return NULL;

		/*
		 * We might race against __delete_from_swap_cache(), and
		 * stumble across a swap_map entry whose SWAP_HAS_CACHE
		 * has not yet been cleared.  Or race against another
		 * __read_swap_cache_async(), which has set SWAP_HAS_CACHE
		 * in swap_map, but not yet added its page to swap cache.
		 */
		cond_resched();
	}

	/*
	 * The swap entry is ours to swap in. Prepare the new page.
	 */

	__SetPageLocked(page);
	__SetPageSwapBacked(page);

	/* May fail (-ENOMEM) if XArray node allocation failed. */
	if (add_to_swap_cache(page, entry, gfp_mask & GFP_RECLAIM_MASK, &shadow)) {
		put_swap_page(page, entry);
		goto fail_unlock;
	}

	if (mem_cgroup_charge(page, NULL, gfp_mask)) {
		delete_from_swap_cache(page);
		goto fail_unlock;
	}

	if (shadow)
		workingset_refault(page, shadow);

	/* Caller will initiate read into locked page */
	lru_cache_add(page);
	*new_page_allocated = true;
	return page;

fail_unlock:
	unlock_page(page);
	put_page(page);
	return NULL;
}

/*
 * Locate a page of swap in physical memory, reserving swap cache space
 * and reading the disk if it is not already cached.
 * A failure return means that either the page allocation failed or that
 * the swap entry is no longer in use.
 */
struct page *read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
		struct vm_area_struct *vma, unsigned long addr, bool do_poll)
{
	bool page_was_allocated;
	struct page *retpage = __read_swap_cache_async(entry, gfp_mask,
			vma, addr, &page_was_allocated);

	if (page_was_allocated){
		swap_readpage(retpage, do_poll);
        }

	return retpage;
}

static unsigned int __swapin_nr_pages(unsigned long prev_offset,
				      unsigned long offset,
				      int hits,
				      int max_pages,
				      int prev_win)
{
	unsigned int pages, last_ra;

	/*
	 * This heuristic has been found to work well on both sequential and
	 * random loads, swapping to hard disk or to SSD: please don't ask
	 * what the "+ 2" means, it just happens to work well, that's all.
	 */
	pages = hits + 2;
	if (pages == 2) {
		/*
		 * We can have no readahead hits to judge by: but must not get
		 * stuck here forever, so check for an adjacent offset instead
		 * (and don't even bother to check whether swap type is same).
		 */
		if (offset != prev_offset + 1 && offset != prev_offset - 1)
			pages = 1;
	} else {
		unsigned int roundup = 4;
		while (roundup < pages)
			roundup <<= 1;
		pages = roundup;
	}

	if (pages > max_pages)
		pages = max_pages;

	/* Don't shrink readahead too fast */
	last_ra = prev_win / 2;
	if (pages < last_ra)
		pages = last_ra;

	return pages;
}

static unsigned long swapin_nr_pages(unsigned long offset)
{
	static unsigned long prev_offset;
	unsigned int hits, pages, max_pages;
	static atomic_t last_readahead_pages;

	max_pages = 1 << READ_ONCE(page_cluster);
	if (max_pages <= 1)
		return 1;

	hits = atomic_xchg(&swapin_readahead_hits, 0);
	pages = __swapin_nr_pages(READ_ONCE(prev_offset), offset, hits,
				  max_pages,
				  atomic_read(&last_readahead_pages));
	if (!hits)
		WRITE_ONCE(prev_offset, offset);
	atomic_set(&last_readahead_pages, pages);

	return pages;
}

/**
 * swap_cluster_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read an aligned block of
 * (1 << page_cluster) entries in the swap area. This method is chosen
 * because it doesn't cost us any seek time.  We also make sure to queue
 * the 'original' request together with the readahead ones...
 *
 * This has been extended to use the NUMA policies from the mm triggering
 * the readahead.
 *
 * Caller must hold read mmap_lock if vmf->vma is not NULL.
 */
struct page *swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask,
				struct vm_fault *vmf)
{
	struct page *page;
	unsigned long entry_offset = swp_offset(entry);
	unsigned long offset = entry_offset;
	unsigned long start_offset, end_offset;
	unsigned long mask;
	struct swap_info_struct *si = swp_swap_info(entry);
	struct blk_plug plug;
	bool do_poll = true, page_allocated;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address;

	mask = swapin_nr_pages(offset) - 1;
	if (!mask)
		goto skip;

	/* Test swap type to make sure the dereference is safe */
	if (likely(si->flags & (SWP_BLKDEV | SWP_FS_OPS))) {
		struct inode *inode = si->swap_file->f_mapping->host;
		if (inode_read_congested(inode))
			goto skip;
	}

	do_poll = false;
	/* Read a page_cluster sized and aligned cluster around offset. */
	start_offset = offset & ~mask;
	end_offset = offset | mask;
	if (!start_offset)	/* First page is swap header. */
		start_offset++;
	if (end_offset >= si->max)
		end_offset = si->max - 1;

	blk_start_plug(&plug);
	for (offset = start_offset; offset <= end_offset ; offset++) {
		/* Ok, do the async read-ahead now */
		page = __read_swap_cache_async(
			swp_entry(swp_type(entry), offset),
			gfp_mask, vma, addr, &page_allocated);
		if (!page)
			continue;
		if (page_allocated) {
			swap_readpage(page, false);
			if (offset != entry_offset) {
				SetPageReadahead(page);
				count_vm_event(SWAP_RA);
			}
		}
		put_page(page);
	}
	blk_finish_plug(&plug);

	lru_add_drain();	/* Push any new pages onto the LRU now */
skip:
	return read_swap_cache_async(entry, gfp_mask, vma, addr, do_poll);
}

int init_swap_address_space(unsigned int type, unsigned long nr_pages)
{
	struct address_space *spaces, *space;
	unsigned int i, nr;

	nr = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_PAGES);
	spaces = kvcalloc(nr, sizeof(struct address_space), GFP_KERNEL);
	if (!spaces)
		return -ENOMEM;
	for (i = 0; i < nr; i++) {
		space = spaces + i;
		xa_init_flags(&space->i_pages, XA_FLAGS_LOCK_IRQ);
		atomic_set(&space->i_mmap_writable, 0);
		space->a_ops = &swap_aops;
		/* swap cache doesn't use writeback related tags */
		mapping_set_no_writeback_tags(space);
	}
	nr_swapper_spaces[type] = nr;
	swapper_spaces[type] = spaces;

	return 0;
}

void exit_swap_address_space(unsigned int type)
{
	kvfree(swapper_spaces[type]);
	nr_swapper_spaces[type] = 0;
	swapper_spaces[type] = NULL;
}

static inline void swap_ra_clamp_pfn(struct vm_area_struct *vma,
				     unsigned long faddr,
				     unsigned long lpfn,
				     unsigned long rpfn,
				     unsigned long *start,
				     unsigned long *end)
{
	*start = max3(lpfn, PFN_DOWN(vma->vm_start),
		      PFN_DOWN(faddr & PMD_MASK));
	*end = min3(rpfn, PFN_DOWN(vma->vm_end),
		    PFN_DOWN((faddr & PMD_MASK) + PMD_SIZE));
}

static void swap_ra_info(struct vm_fault *vmf,
			struct vma_swap_readahead *ra_info)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long ra_val;
	swp_entry_t entry;
	unsigned long faddr, pfn, fpfn;
	unsigned long start, end;
	pte_t *pte, *orig_pte;
	unsigned int max_win, hits, prev_win, win, left;
#ifndef CONFIG_64BIT
	pte_t *tpte;
#endif

	max_win = 1 << min_t(unsigned int, READ_ONCE(page_cluster),
			     SWAP_RA_ORDER_CEILING);
	if (max_win == 1) {
		ra_info->win = 1;
		return;
	}

	faddr = vmf->address;
	orig_pte = pte = pte_offset_map(vmf->pmd, faddr);
	entry = pte_to_swp_entry(*pte);
	if ((unlikely(non_swap_entry(entry)))) {
		pte_unmap(orig_pte);
		return;
	}

	fpfn = PFN_DOWN(faddr);
	ra_val = GET_SWAP_RA_VAL(vma);
	pfn = PFN_DOWN(SWAP_RA_ADDR(ra_val));
	prev_win = SWAP_RA_WIN(ra_val);
	hits = SWAP_RA_HITS(ra_val);
	ra_info->win = win = __swapin_nr_pages(pfn, fpfn, hits,
					       max_win, prev_win);
	atomic_long_set(&vma->swap_readahead_info,
			SWAP_RA_VAL(faddr, win, 0));

	if (win == 1) {
		pte_unmap(orig_pte);
		return;
	}

	/* Copy the PTEs because the page table may be unmapped */
	if (fpfn == pfn + 1)
		swap_ra_clamp_pfn(vma, faddr, fpfn, fpfn + win, &start, &end);
	else if (pfn == fpfn + 1)
		swap_ra_clamp_pfn(vma, faddr, fpfn - win + 1, fpfn + 1,
				  &start, &end);
	else {
		left = (win - 1) / 2;
		swap_ra_clamp_pfn(vma, faddr, fpfn - left, fpfn + win - left,
				  &start, &end);
	}
	ra_info->nr_pte = end - start;
	ra_info->offset = fpfn - start;
	pte -= ra_info->offset;
#ifdef CONFIG_64BIT
	ra_info->ptes = pte;
#else
	tpte = ra_info->ptes;
	for (pfn = start; pfn != end; pfn++)
		*tpte++ = *pte++;
#endif
	pte_unmap(orig_pte);
}

/**
 * swap_vma_readahead - swap in pages in hope we need them soon
 * @fentry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read in a few pages whoes
 * virtual addresses are around the fault address in the same vma.
 *
 * Caller must hold read mmap_lock if vmf->vma is not NULL.
 *
 */
static struct page *swap_vma_readahead(swp_entry_t fentry, gfp_t gfp_mask,
				       struct vm_fault *vmf)
{
	struct blk_plug plug;
	struct vm_area_struct *vma = vmf->vma;
	struct page *page;
	pte_t *pte, pentry;
	swp_entry_t entry;
	unsigned int i;
	bool page_allocated;
	struct vma_swap_readahead ra_info = {
		.win = 1,
	};

	swap_ra_info(vmf, &ra_info);
	if (ra_info.win == 1)
		goto skip;

	blk_start_plug(&plug);
	for (i = 0, pte = ra_info.ptes; i < ra_info.nr_pte;
	     i++, pte++) {
		pentry = *pte;
		if (pte_none(pentry))
			continue;
		if (pte_present(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		page = __read_swap_cache_async(entry, gfp_mask, vma,
					       vmf->address, &page_allocated);
		if (!page)
			continue;
		if (page_allocated) {
			swap_readpage(page, false);
			if (i != ra_info.offset) {
				SetPageReadahead(page);
				count_vm_event(SWAP_RA);
			}
		}
		put_page(page);
	}
	blk_finish_plug(&plug);
	lru_add_drain();
skip:
	return read_swap_cache_async(fentry, gfp_mask, vma, vmf->address,
				     ra_info.win == 1);
}

/**
 * swapin_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * It's a main entry function for swap readahead. By the configuration,
 * it will read ahead blocks by cluster-based(ie, physical disk based)
 * or vma-based(ie, virtual address based on faulty address) readahead.
 */
struct page *swapin_readahead(swp_entry_t entry, gfp_t gfp_mask,
				struct vm_fault *vmf)
{
	return swap_use_vma_readahead() ?
			swap_vma_readahead(entry, gfp_mask, vmf) :
			swap_cluster_readahead(entry, gfp_mask, vmf);
}

#ifdef CONFIG_SYSFS
static ssize_t trim_4k_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", trim_4k);
}
static ssize_t cached_per_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", cached_per);
}
static ssize_t trim_4k_store(struct kobject *kobj,
		struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	if(kstrtoint(buf, 10, &trim_4k)) {
		pr_info("err\n");
	}
	return count;
}
static ssize_t cached_per_store(struct kobject *kobj,
		struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	if(kstrtoint(buf, 10, &cached_per)) {
		pr_info("err\n");
	}
	return count;
}
static ssize_t vma_ra_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n",
			  enable_vma_readahead ? "true" : "false");
}
static ssize_t vma_ra_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	if (!strncmp(buf, "true", 4) || !strncmp(buf, "1", 1))
		enable_vma_readahead = true;
	else if (!strncmp(buf, "false", 5) || !strncmp(buf, "0", 1))
		enable_vma_readahead = false;
	else
		return -EINVAL;

	return count;
}

static struct kobj_attribute trim_4k_attr =
	__ATTR(trim_4k, 0644, trim_4k_show,
	       trim_4k_store);

static struct kobj_attribute cached_per_attr =
	__ATTR(cached_per, 0644, cached_per_show,
	       cached_per_store);

static struct kobj_attribute vma_ra_enabled_attr =
	__ATTR(vma_ra_enabled, 0644, vma_ra_enabled_show,
	       vma_ra_enabled_store);

static struct attribute *swap_attrs[] = {
	&vma_ra_enabled_attr.attr,
	&cached_per_attr.attr,
	&trim_4k_attr.attr,
	NULL,
};

static const struct attribute_group swap_attr_group = {
	.attrs = swap_attrs,
};

static int __init swap_init_sysfs(void)
{
	int err;
	struct kobject *swap_kobj;

	swap_kobj = kobject_create_and_add("swap", mm_kobj);
	if (!swap_kobj) {
		pr_err("failed to create swap kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(swap_kobj, &swap_attr_group);
	if (err) {
		pr_err("failed to register swap group\n");
		goto delete_obj;
	}
	return 0;

delete_obj:
	kobject_put(swap_kobj);
	return err;
}
subsys_initcall(swap_init_sysfs);
#endif
