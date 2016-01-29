/*
    This file is part of darktable,
    copyright (c) 2014 johannes hanika.
    copyright (c) 2015-2016 LebedevRI

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/cache.h"
#include "common/dtpthread.h"
#include "common/darktable.h"

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <assert.h>

static void *dt_cache_hs_malloc(size_t size)
{
  return malloc(size);
}

static void dt_cache_hs_free(void *p, size_t size, bool defer)
{
  (void)size;
  (void)defer;
  free(p);
}

static struct ck_malloc dt_cache_hs_allocator = {.malloc = dt_cache_hs_malloc, .free = dt_cache_hs_free };

static unsigned long dt_cache_hs_hash(const void *object, unsigned long seed)
{
  const struct dt_cache_entry *entry = object;

  return entry->key;
}

static bool dt_cache_hs_compare(const void *previous, const void *compare)
{
  const struct dt_cache_entry *previous_entry = previous;
  const struct dt_cache_entry *compare_entry = compare;

  return (previous_entry->key == compare_entry->key);
}


// this implements a concurrent LRU cache

void dt_cache_init(
    dt_cache_t *cache,
    size_t entry_size,
    size_t cost_quota)
{
  ck_spinlock_fas_init(&(cache->spinlock));

  cache->cost = 0;
  CK_STAILQ_INIT(&(cache->lru));
  cache->entry_size = entry_size;
  cache->cost_quota = cost_quota;
  cache->allocate = 0;
  cache->allocate_data = 0;
  cache->cleanup = 0;
  cache->cleanup_data = 0;

  // capacity must be >= 8 (CK_RHS_PROBE_L1), at least for <ck-0.5.0. else it crashes.
  ck_rhs_init(&(cache->hashtable), CK_RHS_MODE_SPMC | CK_RHS_MODE_OBJECT | CK_RHS_MODE_READ_MOSTLY,
              dt_cache_hs_hash, dt_cache_hs_compare, &dt_cache_hs_allocator, 8, /* unused */ 0);
}

void dt_cache_cleanup(dt_cache_t *cache)
{
  ck_rhs_destroy(&(cache->hashtable));

  struct dt_cache_entry *entry;
  while((entry = CK_STAILQ_FIRST(&(cache->lru))) != NULL)
  {
    CK_STAILQ_REMOVE_HEAD(&(cache->lru), list_entry);

    if(cache->cleanup)
      cache->cleanup(cache->cleanup_data, entry);
    else
      dt_free_align(entry->data);

    g_slice_free1(sizeof(*entry), entry);
  }
}

int32_t dt_cache_contains(dt_cache_t *cache, const uint32_t key)
{
  ck_spinlock_fas_lock(&(cache->spinlock));
  struct dt_cache_entry _key = {.key = key };
  void *value = ck_rhs_get(&(cache->hashtable), key, &_key);
  ck_spinlock_fas_unlock(&(cache->spinlock));
  return (value != NULL);
}

int dt_cache_for_all(
    dt_cache_t *cache,
    int (*process)(const uint32_t key, const void *data, void *user_data),
    void *user_data)
{
  ck_spinlock_fas_lock(&(cache->spinlock));
  void *value;
  ck_rhs_iterator_t iterator = CK_RHS_ITERATOR_INITIALIZER;
  while(ck_rhs_next(&(cache->hashtable), &iterator, &value))
  {
    dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
    const int err = process(entry->key, entry->data, user_data);
    if(err)
    {
      ck_spinlock_fas_unlock(&(cache->spinlock));
      return err;
    }
  }
  ck_spinlock_fas_unlock(&(cache->spinlock));
  return 0;
}

// return read locked bucket, or NULL if it's not already there.
// never attempt to allocate a new slot.
dt_cache_entry_t *dt_cache_testget(dt_cache_t *cache, const uint32_t key, char mode)
{
  double start = dt_get_wtime();
  ck_spinlock_fas_lock(&(cache->spinlock));

  struct dt_cache_entry _key = {.key = key };
  void *value = ck_rhs_get(&(cache->hashtable), key, &_key);

  if(value)
  {
    dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
    // lock the cache entry
    bool result;
    if(mode == 'w')
      result = ck_rwlock_write_trylock(&entry->lock);
    else
      result = ck_rwlock_read_trylock(&entry->lock);
    if(result == false)
    { // need to give up mutex so other threads have a chance to get in between and
      // free the lock we're trying to acquire:
      ck_spinlock_fas_unlock(&(cache->spinlock));
      return 0;
    }

    // bubble up in lru list:
    CK_STAILQ_REMOVE(&(cache->lru), entry, dt_cache_entry, list_entry);
    CK_STAILQ_INSERT_TAIL(&(cache->lru), entry, list_entry);

    ck_spinlock_fas_unlock(&(cache->spinlock));
    double end = dt_get_wtime();
    if(end - start > 0.1)
      fprintf(stderr, "try+ wait time %.06fs mode %c \n", end - start, mode);
    return entry;
  }
  ck_spinlock_fas_unlock(&(cache->spinlock));
  double end = dt_get_wtime();
  if(end - start > 0.1)
    fprintf(stderr, "try- wait time %.06fs\n", end - start);
  return 0;
}

// if found, the data void* is returned. if not, it is set to be
// the given *data and a new hash table entry is created, which can be
// found using the given key later on.
dt_cache_entry_t *dt_cache_get_with_caller(dt_cache_t *cache, const uint32_t key, char mode, const char *file, int line)
{
  double start = dt_get_wtime();
restart:
  ck_spinlock_fas_lock(&(cache->spinlock));

  struct dt_cache_entry _key = {.key = key };
  void *value = ck_rhs_get(&(cache->hashtable), key, &_key);

  if(value)
  { // yay, found. read lock and pass on.
    dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
    bool result;
    if(mode == 'w')
      result = ck_rwlock_write_trylock(&entry->lock);
    else
      result = ck_rwlock_read_trylock(&entry->lock);
    if(result == false)
    { // need to give up mutex so other threads have a chance to get in between and
      // free the lock we're trying to acquire:
      ck_spinlock_fas_unlock(&(cache->spinlock));
      g_usleep(5);
      goto restart;
    }

    // bubble up in lru list:
    CK_STAILQ_REMOVE(&(cache->lru), entry, dt_cache_entry, list_entry);
    CK_STAILQ_INSERT_TAIL(&(cache->lru), entry, list_entry);

    ck_spinlock_fas_unlock(&(cache->spinlock));

    return entry;
  }

  // else, not found, need to allocate.

  // first try to clean up.
  // also wait if we can't free more than the requested fill ratio.
  if(cache->cost > 0.8f * cache->cost_quota)
  {
    // need to roll back all the way to get a consistent lock state:
    dt_cache_gc(cache, 0.8f);
  }

  // here dies your 32-bit system:
  dt_cache_entry_t *entry = (dt_cache_entry_t *)g_slice_alloc(sizeof(dt_cache_entry_t));
  ck_rwlock_init(&entry->lock);
  entry->data = 0;
  entry->cost = cache->entry_size;
  entry->key = key;
  ck_rhs_put(&(cache->hashtable), key, entry);
  // if allocate callback is given, always return a write lock
  int write = ((mode == 'w') || cache->allocate);
  if(cache->allocate)
    cache->allocate(cache->allocate_data, entry);
  else
    entry->data = dt_alloc_align(16, cache->entry_size);
  // write lock in case the caller requests it:
  if(write)
    ck_rwlock_write_lock(&entry->lock);
  else
    ck_rwlock_read_lock(&entry->lock);
  cache->cost += entry->cost;

  // put at end of lru list (most recently used):
  CK_STAILQ_INSERT_TAIL(&(cache->lru), entry, list_entry);

  ck_spinlock_fas_unlock(&(cache->spinlock));
  double end = dt_get_wtime();
  if(end - start > 0.1)
    fprintf(stderr, "wait time %.06fs\n", end - start);
  return entry;
}

int dt_cache_remove(dt_cache_t *cache, const uint32_t key)
{
restart:
  ck_spinlock_fas_lock(&(cache->spinlock));

  struct dt_cache_entry _key = {.key = key };
  void *value = ck_rhs_get(&(cache->hashtable), key, &_key);

  if(!value)
  { // not found in cache, not deleting.
    ck_spinlock_fas_unlock(&(cache->spinlock));
    return 1;
  }

  // need write lock to be able to delete:
  dt_cache_entry_t *entry = (dt_cache_entry_t *)value;
  if(ck_rwlock_write_trylock(&entry->lock) == false)
  {
    ck_spinlock_fas_unlock(&(cache->spinlock));
    g_usleep(5);
    goto restart;
  }

  gboolean removed = (entry == ck_rhs_remove(&(cache->hashtable), key, entry));
  (void)removed; // make non-assert compile happy
  assert(removed);

  CK_STAILQ_REMOVE(&(cache->lru), entry, dt_cache_entry, list_entry);

  if(cache->cleanup)
    cache->cleanup(cache->cleanup_data, entry);
  else
    dt_free_align(entry->data);
  ck_rwlock_write_unlock(&entry->lock);
  cache->cost -= entry->cost;
  g_slice_free1(sizeof(*entry), entry);

  ck_spinlock_fas_unlock(&(cache->spinlock));
  return 0;
}

// best-effort garbage collection. never blocks, never fails. well, sometimes it just doesn't free anything.
void dt_cache_gc(dt_cache_t *cache, const float fill_ratio)
{
  int cnt = 0;
  struct dt_cache_entry *entry, *safe;
  CK_STAILQ_FOREACH_SAFE(entry, &(cache->lru), list_entry, safe)
  {
    cnt++;

    if(cache->cost < cache->cost_quota * fill_ratio) break;

    // if still locked by anyone else give up:
    if(ck_rwlock_write_trylock(&entry->lock) == false) continue;

    // delete!
    gboolean removed = (entry == ck_rhs_remove(&(cache->hashtable), entry->key, entry));
    (void)removed; // make non-assert compile happy
    assert(removed);
    CK_STAILQ_REMOVE(&(cache->lru), entry, dt_cache_entry, list_entry);
    cache->cost -= entry->cost;

    if(cache->cleanup)
      cache->cleanup(cache->cleanup_data, entry);
    else
      dt_free_align(entry->data);
    ck_rwlock_write_unlock(&entry->lock);
    g_slice_free1(sizeof(*entry), entry);
  }

  // in <=ck-0.5.1, in CK_RHS_MODE_READ_MOSTLY, ck_rhs_gc() was broken
  // (see concurrencykit/ck@7f625a6fe16e23e487d1745405f04a7e62dc01b4)
  // ck_rhs_gc(&(cache->hashtable));
  ck_rhs_rebuild(&(cache->hashtable));
}

void dt_cache_downgrade(dt_cache_t *cache, dt_cache_entry_t *entry)
{
  ck_rwlock_write_downgrade(&entry->lock);
}

void dt_cache_release(dt_cache_t *cache, dt_cache_entry_t *entry, char mode)
{
  if(mode == 'w')
    ck_rwlock_write_unlock(&entry->lock);
  else
    ck_rwlock_read_unlock(&entry->lock);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
