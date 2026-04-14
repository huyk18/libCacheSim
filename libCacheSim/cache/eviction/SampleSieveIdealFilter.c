//
//  SampleSieveIdealFilter.c
//  libCacheSim
//
//  Sample k objects from the hash table and evict one victim based on
//  Sieve-like priority: lower freq first, then older access time.
//

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dataStructure/hashtable/hashtable.h"
#include "libCacheSim/evictionAlgo.h"
#include "libCacheSim/macro.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SampleSieveIdealFilter_params {
  int32_t k;
  cache_obj_t **eviction_candidates;
} SampleSieveIdealFilter_params_t;

static const char *DEFAULT_CACHE_PARAMS = "k=5";

static void SampleSieveIdealFilter_free(cache_t *cache);
static bool SampleSieveIdealFilter_get(cache_t *cache, const request_t *req);
static cache_obj_t *SampleSieveIdealFilter_find(cache_t *cache,
                                                const request_t *req,
                                                bool update_cache);
static cache_obj_t *SampleSieveIdealFilter_insert(cache_t *cache,
                                                  const request_t *req);
static cache_obj_t *SampleSieveIdealFilter_to_evict(cache_t *cache,
                                                    const request_t *req);
static void SampleSieveIdealFilter_evict(cache_t *cache, const request_t *req);
static bool SampleSieveIdealFilter_remove(cache_t *cache, obj_id_t obj_id);
static void SampleSieveIdealFilter_parse_params(
    cache_t *cache, const char *cache_specific_params);

static bool is_duplicate_candidate(cache_obj_t *candidate,
                                   cache_obj_t **candidates,
                                   int32_t num_candidates) {
  for (int32_t i = 0; i < num_candidates; i++) {
    if (candidates[i] == candidate) {
      return true;
    }
  }
  return false;
}

cache_t *SampleSieveIdealFilter_init(const common_cache_params_t ccache_params,
                                     const char *cache_specific_params) {
  common_cache_params_t ccache_params_copy = ccache_params;
  ccache_params_copy.hashpower = MAX(12, ccache_params_copy.hashpower - 8);

  cache_t *cache = cache_struct_init("SampleSieveIdealFilter",
                                     ccache_params_copy, cache_specific_params);
  cache->cache_init = SampleSieveIdealFilter_init;
  cache->cache_free = SampleSieveIdealFilter_free;
  cache->get = SampleSieveIdealFilter_get;
  cache->find = SampleSieveIdealFilter_find;
  cache->insert = SampleSieveIdealFilter_insert;
  cache->to_evict = SampleSieveIdealFilter_to_evict;
  cache->evict = SampleSieveIdealFilter_evict;
  cache->remove = SampleSieveIdealFilter_remove;

  if (ccache_params.consider_obj_metadata) {
    cache->obj_md_size = 1;
  } else {
    cache->obj_md_size = 0;
  }

  cache->eviction_params = (SampleSieveIdealFilter_params_t *)malloc(
      sizeof(SampleSieveIdealFilter_params_t));
  SampleSieveIdealFilter_params_t *params =
      (SampleSieveIdealFilter_params_t *)(cache->eviction_params);
  memset(params, 0, sizeof(SampleSieveIdealFilter_params_t));

  SampleSieveIdealFilter_parse_params(cache, DEFAULT_CACHE_PARAMS);
  if (cache_specific_params != NULL) {
    SampleSieveIdealFilter_parse_params(cache, cache_specific_params);
  }

  if (params->k <= 0) {
    ERROR("%s requires k > 0\n", cache->cache_name);
    abort();
  }

  params->eviction_candidates =
      (cache_obj_t **)malloc(sizeof(cache_obj_t *) * params->k);
  snprintf(cache->cache_name, CACHE_NAME_ARRAY_LEN, "SampleSieveIdealFilter-%d",
           params->k);

  return cache;
}

static void SampleSieveIdealFilter_free(cache_t *cache) {
  SampleSieveIdealFilter_params_t *params =
      (SampleSieveIdealFilter_params_t *)(cache->eviction_params);
  free(params->eviction_candidates);
  free(params);
  cache_struct_free(cache);
}

static bool SampleSieveIdealFilter_get(cache_t *cache, const request_t *req) {
  return cache_get_base(cache, req);
}

static cache_obj_t *SampleSieveIdealFilter_find(cache_t *cache,
                                                const request_t *req,
                                                bool update_cache) {
  cache_obj_t *obj = cache_find_base(cache, req, update_cache);
  if (obj != NULL && update_cache) {
    obj->sieve.freq = 1;
    obj->Random.last_access_vtime = cache->n_req;
  }

  return obj;
}

static cache_obj_t *SampleSieveIdealFilter_insert(cache_t *cache,
                                                  const request_t *req) {
  SampleSieveIdealFilter_params_t *params =
      (SampleSieveIdealFilter_params_t *)(cache->eviction_params);
  if (req->next_access_vtime < 0 ||
      req->next_access_vtime > req->replay_end_vtime) {
    return NULL;
  }
  cache_obj_t *obj = cache_insert_base(cache, req);
  obj->sieve.freq = 0;
  obj->Random.last_access_vtime = cache->n_req;

  return obj;
}

static cache_obj_t *SampleSieveIdealFilter_to_evict(cache_t *cache,
                                                    const request_t *req) {
  (void)req;
  SampleSieveIdealFilter_params_t *params =
      (SampleSieveIdealFilter_params_t *)(cache->eviction_params);
  const int32_t num_candidates = MIN(params->k, (int32_t)cache->n_obj);

  DEBUG_ASSERT(num_candidates > 0 || cache->occupied_byte == 0);
  cache->to_evict_candidate_gen_vtime = cache->n_req;

  if (num_candidates <= 0) {
    return NULL;
  }

  for (int32_t i = 0; i < num_candidates; i++) {
    cache_obj_t *candidate;
    do {
      candidate = hashtable_rand_obj(cache->hashtable);
    } while (num_candidates > 1 &&
             is_duplicate_candidate(candidate, params->eviction_candidates, i));
    params->eviction_candidates[i] = candidate;
  }

  cache_obj_t *obj_to_evict = params->eviction_candidates[0];
  for (int32_t i = 1; i < num_candidates; i++) {
    cache_obj_t *candidate = params->eviction_candidates[i];
    if (candidate->sieve.freq < obj_to_evict->sieve.freq ||
        (candidate->sieve.freq == obj_to_evict->sieve.freq &&
         candidate->Random.last_access_vtime <
             obj_to_evict->Random.last_access_vtime)) {
      obj_to_evict = candidate;
    }
  }

  return obj_to_evict;
}

static void SampleSieveIdealFilter_evict(cache_t *cache, const request_t *req) {
  cache_obj_t *obj_to_evict = SampleSieveIdealFilter_to_evict(cache, req);
  DEBUG_ASSERT(obj_to_evict != NULL);
  cache_evict_base(cache, obj_to_evict, true);
}

static bool SampleSieveIdealFilter_remove(cache_t *cache, obj_id_t obj_id) {
  cache_obj_t *obj = hashtable_find_obj_id(cache->hashtable, obj_id);
  if (obj == NULL) {
    return false;
  }
  cache_remove_obj_base(cache, obj, true);

  return true;
}

static void SampleSieveIdealFilter_parse_params(
    cache_t *cache, const char *cache_specific_params) {
  SampleSieveIdealFilter_params_t *params =
      (SampleSieveIdealFilter_params_t *)(cache->eviction_params);

  char *params_str = strdup(cache_specific_params);
  char *old_params_str = params_str;
  char *end;

  while (params_str != NULL && params_str[0] != '\0') {
    char *key = strsep((char **)&params_str, "=");
    char *value = strsep((char **)&params_str, ",");

    while (params_str != NULL && *params_str == ' ') {
      params_str++;
    }

    if (strcasecmp(key, "k") == 0 || strcasecmp(key, "n-sample") == 0 ||
        strcasecmp(key, "n-samples") == 0) {
      params->k = (int32_t)strtol(value, &end, 0);
    } else if (strcasecmp(key, "print") == 0) {
      printf("current parameters: k=%d\n", params->k);
      exit(0);
    } else {
      ERROR("%s does not have parameter %s\n", cache->cache_name, key);
      exit(1);
    }
  }
  free(old_params_str);
}

#ifdef __cplusplus
}
#endif
