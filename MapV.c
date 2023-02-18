#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <limits.h>
#include <immintrin.h>

#include <xxhash.h>

#include "MapV.h"

#define MAPV_DBG 1
#define MAPV_DBG_FFL {printf("MAPV_DBG: File: [%s] Func: [%s] Line: [%d]\n",\
                      __FILE__, __FUNCTION__, __LINE__);fflush(stdout);}



//------------------------------------------------------------------------------
//
// static function headers
//

// only used for debugging:
// static void
// _print_hv(MapV_HV_st hv)

static inline uint64_t
_pow2_next_u64(uint64_t n);

static inline MapV_Hash_st
_hash(const void*  key,
      const size_t keyLen);

static inline uint64_t
_hashhi_from_slot(const MapV_st*      map,
                  const MapV_SlotId_t slotId);

static inline bool
_hashes_are_equal(const MapV_Hash_st hash1, const MapV_Hash_st hash2);

static inline bool
_hv_is_empty(const MapV_HV_st* hv);

static inline MapV_BktId_t
_bkt_from_slot(const MapV_SlotId_t slotId);

static inline MapV_BktId_t
_bktslot_from_slot(const MapV_SlotId_t slotId);

static inline MapV_SlotId_t
_slot_from_hash_hi(const MapV_st*      map,
                   const MapV_HashHi_t hashHi);

static inline MapV_Dist_t
_slot_hash_hi_dist(const MapV_st*      map,
                   const MapV_HashHi_t hashHi,
                   const MapV_SlotId_t cmpSlotId);

static inline MapV_SlotId_t
_slot_from_key(const MapV_st* map,
               const void*    key,
               const size_t   keyLen);

static inline void
_tbl_cap_update(MapV_st* map);

static inline void
_tbl_clear_slot(const MapV_st*      map,
                const MapV_SlotId_t slotId);

static inline void
_tbl_get_hv_from_slot(const MapV_st*      map,
                      const MapV_SlotId_t slotId,
                            MapV_HV_st*   hv);

static inline void
_tbl_set_hv_into_slot(const MapV_st*      map,
                      const MapV_SlotId_t slotId,
                      const MapV_HV_st*   hv);

static inline void
_tbl_dist_update(MapV_st*      map,
                 MapV_HashHi_t hashHi,
                 MapV_SlotId_t slotId);

static inline bool
_tbl_should_realloc(MapV_st* map);

static inline MapV_Err_et
_tbl_insert_hv(      MapV_st*   map,
                     MapV_HV_st newHv,
               const bool       overwriteIfExists);

static inline bool
_tbl_redistribute_hashes(MapV_st* map,
                         MapV_st* oldMap);

static inline bool
_tbl_realloc_grow(MapV_st* cur);




//==============================================================================
//
// MapV_*() : Public Functions
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
MapV_st*
MapV_Create(const MapV_Dist_t distSlotMax,
            const MapV_Dist_t distBktMax,
            const double      capPctMax,
            const int         memAlign,
            const uint64_t    initialSlotCount)
{
  if (memAlign % 32 != 0) {
    printf("alignment must be a multiple of 32 bytes. (4096 recommended)\n");
    printf("attempted to configure with %d bytes\n", memAlign);
    return NULL;
  }

  MapV_st* map = calloc(1, sizeof(*map));

  map->cfg.distSlotMax = distSlotMax;
  map->cfg.distBktMax  = distBktMax;
  map->cfg.capPctMax   = capPctMax;
  map->cfg.memAlign    = memAlign;

  map->meta.slotsCap   = initialSlotCount;

  if (!_tbl_realloc_grow(map)) {
    free(map);
    printf("_tbl_realloc_grow() failed\n");
    exit(1);
    return NULL;
  }

  return map;
}

//------------------------------------------------------------------------------
MapV_Err_et
MapV_Insert(      MapV_st*   map,
            const void*      key,
            const size_t     keyLen,
            const MapV_Val_t val,
            const bool       overwriteIfExists)
{
  MapV_HV_st newHv = { .hash = _hash(key, keyLen), .val = val, };

  MapV_Err_et err;
  if (MAPV_ERR__OK != (err = _tbl_insert_hv(map, newHv, overwriteIfExists))) {
    if (MAPV_ERR__TABLE_MUST_GROW == err) {
      if (!_tbl_realloc_grow(map)) {
        printf("MapV_Insert(): _tbl_realloc_grow() failed\n");
        return MAPV_ERR__TABLE_GROW_FAILED;
      }
    }
    // it's possible to get here? can't insert, but shouldn't realloc?
  } else {
    map->meta.slotsUsed++;
    _tbl_cap_update(map);
    return MAPV_ERR__OK;
  }

  // should always return MAPV_ERR__OK at this point...?
  // @TODO: vertify
  return _tbl_insert_hv(map, newHv, overwriteIfExists);
}

//------------------------------------------------------------------------------
// @IMPORTNT: changes must likely be made in _slot_from_key(), and vice versa
//
// @NOTE: this is almost an exact copy of _slot_from_key()
//        we don't use _slot_from_key(), because it would require
//        at least one additional branch, and it would duplicate some
//        instructions when translating the slot id into bkt+bktslot again.
//        we want find() to be fast, so we keep it all right here.
bool
MapV_Find(const MapV_st*  map,
          const void*     key,
          const size_t    keyLen,
                uint64_t* val)
{
  const MapV_Hash_st  hash     = _hash(key, keyLen);
        MapV_SlotId_t slotId   = _slot_from_hash_hi(map, hash.high64);

  const uint64_t*     pHashHi  = (uint64_t*)&hash.high64;
  const __m256i       needleHi = _mm256_set_epi64x(*pHashHi, *pHashHi,
                                                   *pHashHi, *pHashHi);
  const uint64_t*     pHashLo  = (uint64_t*)&hash.low64;
  const __m256i       needleLo = _mm256_set_epi64x(*pHashLo, *pHashLo,
                                                   *pHashLo, *pHashLo);

  __m256i found;
  __m256i haystack;

  const int maxIters = map->meta.distBktIter;
  for (int iter = 0; iter < maxIters; iter++)
  {
    // @NOTE: `| 0x100` in _mm256_movemask_pd is to set a highest bit as
    //        an indicator that nothing was found.
    //        when no matches were found, idxHi/Lo will == 8.
    //        could also set to 0x10 and check idx is 4.
    //        but that's less clear, given we're working with 4 array indices.

    const MapV_BktId_t bktId = slotId / MAPV_BKT_SLOTS;

    haystack = _mm256_load_si256((__m256i*)map->tbl.bkt[bktId].slotsHi);
    found    = _mm256_cmpeq_epi64(haystack, needleHi);
    const int idxHi = __builtin_ctz(_mm256_movemask_pd((__m256d)found) | 0x100);
    if (idxHi == 8) { // not found
      slotId += MAPV_BKT_SLOTS;
      continue;
    }

    haystack = _mm256_load_si256((__m256i*)map->tbl.bkt[bktId].slotsLo);
    found    = _mm256_cmpeq_epi64(haystack, needleLo);
    const int idxLo = __builtin_ctz(_mm256_movemask_pd((__m256d)found) | 0x100);
    if (idxHi == idxLo) { // found
      *val = map->tbl.bkt[bktId].vals[idxLo];
      return true;
    }

    // not found
    slotId += MAPV_BKT_SLOTS;
  }
  return false;
}

//------------------------------------------------------------------------------
MapV_Err_et
MapV_Delete(      MapV_st* map,
            const void*    key,
            const size_t   keyLen)
{
	MapV_SlotId_t curSlotId;
	if (UINT64_MAX == (curSlotId = _slot_from_key(map, key, keyLen))) {
		return MAPV_ERR__DELETE_KEY_NOT_FOUND;
	}

	MapV_HashHi_t hashHi = _hashhi_from_slot(map, curSlotId);
	MapV_Dist_t   dist   = _slot_hash_hi_dist(map, hashHi, curSlotId);
	_tbl_clear_slot(map, curSlotId);

	while (dist > 0)
	{
		MapV_SlotId_t nextSlotId = curSlotId + 1;

		MapV_HV_st nextSlotHv;
		_tbl_get_hv_from_slot(map, nextSlotId, &nextSlotHv);
		if (_hv_is_empty(&nextSlotHv)) {
			return MAPV_ERR__OK;
		}

		_tbl_clear_slot(map, nextSlotId);

		_tbl_set_hv_into_slot(map, curSlotId, &nextSlotHv);
		dist = _slot_hash_hi_dist(map, nextSlotHv.hash.high64, curSlotId);

		curSlotId++;
	}

  return MAPV_ERR__OK;
}

//------------------------------------------------------------------------------
MapV_Err_et
MapV_Destroy(MapV_st* map)
{
  // @TODO: test
	if (NULL != map) {
		if (NULL != map->tbl.bktPtrReal) {
			free(map->tbl.bktPtrReal);
		} else {
			// still allow the map to free...
			// return MAPV_ERR__DESTROY_MAP_BKTPTRREAL_IS_NULL;
		}
		free(map);
	} else {
		return MAPV_ERR__DESTROY_MAP_IS_NULL;
	}

  return MAPV_ERR__OK;
}




//==============================================================================
//
// MapV_Print*()
//
//==============================================================================

//------------------------------------------------------------------------------
void
MapV_PrintTableCfg(const MapV_st* map)
{
  printf("\n\n");
  printf("----------------------------------------------------------------\n");
  printf("\n");

  printf("cfg.distSlotMax    : %"PRIu64"\n", map->cfg.distSlotMax);
  printf("cfg.distBktMax     : %"PRIu64"\n", map->cfg.distSlotMax);
  printf("cfg.capPctMax      : %f\n",        map->cfg.capPctMax);
  printf("cfg.memAlign       : %d\n",        map->cfg.memAlign);
  printf("\n");

  printf("meta.tblBytes      : %"PRIu64"\n", map->meta.tblBytes);
  printf("meta.tblBytesReal  : %"PRIu64"\n", map->meta.tblBytesReal);
  printf("\n");

  printf("meta.bktsCnt       : %"PRIu64"\n", map->meta.bktsCnt);
  printf("meta.bktsCntReal   : %"PRIu64"\n", map->meta.bktsCntReal);
  printf("\n");

  printf("meta.slotHashShift : %"PRIu64"\n", map->meta.slotHashShift);
  printf("meta.slotsCap      : %"PRIu64"\n", map->meta.slotsCap);
  printf("meta.slotsCapReal  : %"PRIu64"\n", map->meta.slotsCapReal);
  printf("meta.slotsUsed     : %"PRIu64"\n", map->meta.slotsUsed);
  printf("meta.slotsAvail    : %"PRIu64"\n", map->meta.slotsAvail);
  printf("meta.slotsCapPct   : %f\n",        map->meta.slotsCapPct);
  printf("\n");

  printf("meta.distSlotMax   : %"PRIu64"\n", map->meta.distSlotMax);
  printf("meta.distSlotIter  : %"PRIu64"\n", map->meta.distSlotIter);
  printf("meta.distBktMax    : %"PRIu64"\n", map->meta.distBktMax);
  printf("meta.distBktIter   : %"PRIu64"\n", map->meta.distBktIter);
  printf("\n");

  printf("tbl.bktPtrReal     : %p\n", map->tbl.bktPtrReal);
  printf("tbl.bkt            : %p\n", map->tbl.bkt);
  printf("\n\n");

  printf("----------------------------------------------------------------\n");
  printf("\n\n");
  fflush(stdout);
}

//------------------------------------------------------------------------------
void
MapV_PrintTableData(const MapV_st* map)
{
  printf("\n\n");

  int distSlotMax = 0;
  int numFound    = 0;
  for (int slotId = 0; slotId < map->meta.slotsCapReal; slotId++)
  {
    MapV_HV_st hv;
    _tbl_get_hv_from_slot(map, slotId, &hv);
    if (_hv_is_empty(&hv)) {
      continue;
    }

    numFound++;
    const int slotDist = _slot_hash_hi_dist(map, hv.hash.high64, slotId);
    printf("\t[%d] %"PRIu64" %"PRIu64" : %"PRIu64" : %d\n",
           slotId, hv.hash.high64, hv.hash.low64, hv.val, slotDist);
    fflush(stdout);

    if (slotDist > distSlotMax) {
      distSlotMax = slotDist;
    }
  }

  printf("\n\n");
  printf("distSlotMax: %d\n", distSlotMax);
  printf("numFound: %d\n", numFound);
}

//------------------------------------------------------------------------------
const char*
MapV_PrintErr(MapV_Err_et err)
{
	if (err > MAPV_ERR___LAST || err < MAPV_ERR___FIRST) {
		return "INVALID MapV_Err_et VALUE";
	}
	static const char* strArr[] = {
		[MAPV_ERR__OK] =
		"MAPV_ERR__OK",
		[MAPV_ERR__TABLE_MUST_GROW] =
		"MAPV_ERR__TABLE_MUST_GROW",
		[MAPV_ERR__TABLE_GROW_FAILED] =
		"MAPV_ERR__TABLE_GROW_FAILED",
		[MAPV_ERR__INSERT_KEY_EXISTS] =
		"MAPV_ERR__INSERT_KEY_EXISTS",
		[MAPV_ERR__DELETE_KEY_NOT_FOUND] =
		"MAPV_ERR__DELETE_KEY_NOT_FOUND",
		[MAPV_ERR__DESTROY_MAP_IS_NULL] =
		"MAPV_ERR__DESTROY_MAP_IS_NULL",
		[MAPV_ERR__DESTROY_MAP_BKTPTRREAL_IS_NULL] =
		"MAPV_ERR__DESTROY_MAP_BKTPTRREAL_IS_NULL",
	};
	return strArr[err];
}

//------------------------------------------------------------------------------
// only used for debugging
// static void
// _print_hv(MapV_HV_st hv) {
//   printf("\n\n---------------\n");
//   printf("HV:\n");
//   printf("\thi64 : %"PRIu64"\n", hv.hash.high64);
//   printf("\tlo64 : %"PRIu64"\n", hv.hash.low64);
//   printf("\tval  : %"PRIu64"\n", hv.val);
//   printf("\n");
//   fflush(stdout);
// }




//==============================================================================
//
// Static Functions
//
//==============================================================================




//==============================================================================
//
// _pow2...()
//
//------------------------------------------------------------------------------
static inline uint64_t
_pow2_next_u64(uint64_t n) {
  --n;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  n |= n >> 32;
  ++n;
  return n;
}


//==============================================================================
//
// _hash...()
//
//------------------------------------------------------------------------------
static inline MapV_Hash_st
_hash(const void*  key,
      const size_t keyLen)
{
  return XXH3_128bits(key, keyLen);
}

//------------------------------------------------------------------------------
// @NOTE: no error checking.
//        this must only be called when you know the slot is in range
static inline MapV_HashHi_t
_hashhi_from_slot(const MapV_st*      map,
                  const MapV_SlotId_t slotId)
{
  const MapV_BktId_t  bktId     = slotId / MAPV_BKT_SLOTS;
	const MapV_SlotId_t bktSlotId = slotId % MAPV_BKT_SLOTS;
	return map->tbl.bkt[bktId].slotsHi[bktSlotId];
}

//------------------------------------------------------------------------------
// @NOTE: pass by value since the data is small enough
static inline bool
_hashes_are_equal(const MapV_Hash_st hash1, const MapV_Hash_st hash2)
{
	// @TODO: XORing and checking == 0 may be faster
  return (   hash1.high64 == hash2.high64
          && hash1.low64  == hash2.low64);
}


//==============================================================================
//
// _hv...()
//
//------------------------------------------------------------------------------
static inline bool
_hv_is_empty(const MapV_HV_st* hv)
{
	// @TODO: ORing fields and checking == 0 may be faster
  return (   hv->hash.high64 == 0
          && hv->hash.low64  == 0
          && hv->val         == 0);
}


//==============================================================================
//
// _bkt...()
//
//------------------------------------------------------------------------------
static inline MapV_BktId_t
_bkt_from_slot(const MapV_SlotId_t slotId)
{
  return slotId / MAPV_BKT_SLOTS;
}

//------------------------------------------------------------------------------
static inline MapV_BktId_t
_bktslot_from_slot(const MapV_SlotId_t slotId)
{
  return slotId % MAPV_BKT_SLOTS;
}


//==============================================================================
//
// _slot...()
//
//------------------------------------------------------------------------------
static inline MapV_SlotId_t
_slot_from_hash_hi(const MapV_st*      map,
                   const MapV_HashHi_t hashHi)
{
  return hashHi >> map->meta.slotHashShift;
}

//------------------------------------------------------------------------------
static inline MapV_Dist_t
_slot_hash_hi_dist(const MapV_st*      map,
                   const MapV_HashHi_t hashHi,
                   const MapV_SlotId_t cmpSlotId)
{
  const MapV_SlotId_t slotId = _slot_from_hash_hi(map, hashHi);
  return cmpSlotId - slotId;
}

//------------------------------------------------------------------------------
// @IMPORTNT: changes must likely be made in _Find(), and vice versa
//
// @NOTE: this isn't used by find(). see notes on that function.
//        this _is_ used by delete, and maybe others in the future.
static inline MapV_SlotId_t
_slot_from_key(const MapV_st* map,
               const void*    key,
               const size_t   keyLen)
{
  const MapV_Hash_st  hash     = _hash(key, keyLen);
        MapV_SlotId_t slotId   = _slot_from_hash_hi(map, hash.high64);

  const uint64_t*     pHashHi  = (uint64_t*)&hash.high64;
  const __m256i       needleHi = _mm256_set_epi64x(*pHashHi, *pHashHi,
                                                   *pHashHi, *pHashHi);
  const uint64_t*     pHashLo  = (uint64_t*)&hash.low64;
  const __m256i       needleLo = _mm256_set_epi64x(*pHashLo, *pHashLo,
                                                   *pHashLo, *pHashLo);

  __m256i found;
  __m256i haystack;

  const int maxIters = map->meta.distBktIter;
  for (int iter = 0; iter < maxIters; iter++)
  {
    // @NOTE: `| 0x100` in _mm256_movemask_pd is to set a highest bit as
    //        an indicator that nothing was found.
    //        when no matches were found, idxHi/Lo will == 8.
    //        could also set to 0x10 and check idx is 4.
    //        but that's less clear, given we're working with 4 array indices.

    const MapV_BktId_t bktId = slotId / MAPV_BKT_SLOTS;

    haystack = _mm256_load_si256((__m256i*)map->tbl.bkt[bktId].slotsHi);
    found    = _mm256_cmpeq_epi64(haystack, needleHi);
    const int idxHi = __builtin_ctz(_mm256_movemask_pd((__m256d)found) | 0x100);
    if (idxHi == 8) { // not found
      slotId += MAPV_BKT_SLOTS;
      continue;
    }

    haystack = _mm256_load_si256((__m256i*)map->tbl.bkt[bktId].slotsLo);
    found    = _mm256_cmpeq_epi64(haystack, needleLo);
    const int idxLo = __builtin_ctz(_mm256_movemask_pd((__m256d)found) | 0x100);
    if (idxHi == idxLo) { // found
      // *val = map->tbl.bkt[bktId].vals[idxLo];
      // return true;
      return slotId;
    }

    // not found
    slotId += MAPV_BKT_SLOTS;
  }

  // ie: not found
  return UINT64_MAX;
}



//==============================================================================
//
// _tbl...()
//
//------------------------------------------------------------------------------
static inline void
_tbl_cap_update(MapV_st* map)
{
  map->meta.slotsAvail  = map->meta.slotsCapReal - map->meta.slotsUsed;
  map->meta.slotsCapPct = (double)map->meta.slotsUsed
                        / (double)map->meta.slotsCapReal
                        * 100;
}

//------------------------------------------------------------------------------
static inline void
_tbl_clear_slot(const MapV_st*      map,
                const MapV_SlotId_t slotId)
{
  const MapV_BktId_t bktId     = _bkt_from_slot(slotId);
  const MapV_BktId_t bktSlotId = _bktslot_from_slot(slotId);
  map->tbl.bkt[bktId].slotsHi[bktSlotId] = 0;
  map->tbl.bkt[bktId].slotsLo[bktSlotId] = 0;
  map->tbl.bkt[bktId].vals   [bktSlotId] = 0;
}

//------------------------------------------------------------------------------
static inline void
_tbl_get_hv_from_slot(const MapV_st*      map,
                      const MapV_SlotId_t slotId,
                            MapV_HV_st*   hv)
{
  const MapV_BktId_t bktId     = _bkt_from_slot(slotId);
  const MapV_BktId_t bktSlotId = _bktslot_from_slot(slotId);
  hv->hash.high64 = map->tbl.bkt[bktId].slotsHi[bktSlotId];
  hv->hash.low64  = map->tbl.bkt[bktId].slotsLo[bktSlotId];
  hv->val         = map->tbl.bkt[bktId].vals   [bktSlotId];
}

//------------------------------------------------------------------------------
static inline void
_tbl_set_hv_into_slot(const MapV_st*      map,
                      const MapV_SlotId_t slotId,
                      const MapV_HV_st*   hv)
{
  const MapV_BktId_t bktId     = _bkt_from_slot(slotId);
  const MapV_BktId_t bktSlotId = _bktslot_from_slot(slotId);
  map->tbl.bkt[bktId].slotsHi[bktSlotId] = hv->hash.high64;
  map->tbl.bkt[bktId].slotsLo[bktSlotId] = hv->hash.low64;
  map->tbl.bkt[bktId].vals   [bktSlotId] = hv->val;
}

//------------------------------------------------------------------------------
static inline void
_tbl_dist_update(MapV_st*      map,
                 MapV_HashHi_t hashHi,
                 MapV_SlotId_t slotId)
{
  const MapV_Dist_t slotDist = _slot_hash_hi_dist(map, hashHi, slotId);

  if (slotDist > map->meta.distSlotMax) {
    map->meta.distSlotMax  = slotDist;
    map->meta.distSlotIter = slotDist + 1;
  }

  const MapV_SlotId_t targetSlot = _slot_from_hash_hi(map, hashHi);
  const MapV_BktId_t  targetBkt  = _bkt_from_slot(targetSlot);
  const MapV_BktId_t  actualBkt  = _bkt_from_slot(slotId);
  const MapV_Dist_t   bktDist    = (actualBkt > targetBkt)
                                 ? (actualBkt - targetBkt)
                                 : (targetBkt - actualBkt);
  if (bktDist > map->meta.distBktMax) {
    map->meta.distBktMax  = bktDist;
    map->meta.distBktIter = bktDist + 1;
  }
}

//------------------------------------------------------------------------------
static inline bool
_tbl_should_realloc(MapV_st* map)
{
  if (   map->meta.distSlotIter > map->cfg.distSlotMax
      || map->meta.distBktIter  > map->cfg.distBktMax
      || map->meta.slotsCapPct  > map->cfg.capPctMax) {
    return true;
  }
  return false;
}

//------------------------------------------------------------------------------
static inline MapV_Err_et
_tbl_insert_hv(      MapV_st*   map,
                     MapV_HV_st newHv,
               const bool       overwriteIfExists)
{
  if (_tbl_should_realloc(map)) {
    return MAPV_ERR__TABLE_MUST_GROW;
  }

  uint64_t slotId = _slot_from_hash_hi(map, newHv.hash.high64);

  do {
    const int newSlotDist = _slot_hash_hi_dist(map, newHv.hash.high64, slotId);

    MapV_HV_st curHv;
    _tbl_get_hv_from_slot(map, slotId, &curHv);
    if (_hv_is_empty(&curHv)) {
      _tbl_set_hv_into_slot(map, slotId, &newHv);
      _tbl_dist_update(map, newHv.hash.high64, slotId);
      return MAPV_ERR__OK;
    }

    if (_hashes_are_equal(curHv.hash, newHv.hash)) {
    	if (overwriteIfExists) {
	      _tbl_set_hv_into_slot(map, slotId, &newHv);
	      // no need to call _tbl_dist_update()
	      return MAPV_ERR__OK;
    	} else {
	    	return MAPV_ERR__INSERT_KEY_EXISTS;
    	}
    }

    const int curSlotDist = _slot_hash_hi_dist(map, curHv.hash.high64, slotId);
    if (newSlotDist > curSlotDist) {
      _tbl_set_hv_into_slot(map, slotId, &newHv);
      _tbl_dist_update(map, newHv.hash.high64, slotId);
      newHv = curHv;
    }

    if (curSlotDist >= map->cfg.distSlotMax) {
      return MAPV_ERR__TABLE_MUST_GROW;
    }

    slotId++;

  } while (1);
}

//------------------------------------------------------------------------------
static inline bool
_tbl_redistribute_hashes(MapV_st* map,
                         MapV_st* oldMap)
{
  map->meta.distSlotMax  = 0;
  map->meta.distSlotIter = 0;
  map->meta.distBktMax   = 0;
  map->meta.distBktIter  = 0;

  const uint64_t slotCnt = oldMap->meta.slotsCapReal;
  for (MapV_SlotId_t oldSlot = 0; oldSlot < slotCnt; oldSlot++)
  {
    MapV_HV_st newHv;
    _tbl_get_hv_from_slot(oldMap, oldSlot, &newHv);
    if (_hv_is_empty(&newHv)) {
      continue;
    }

    // should always return true after realloc...?
    // @TODO: vertify
    // @NOTE: we're setting overwriteIfExists to true.
    //        technically, this shold be false...?
	  MapV_Err_et err;
	  if (MAPV_ERR__OK == (err = _tbl_insert_hv(map, newHv, true))) {
      _tbl_clear_slot(oldMap, oldSlot);
      continue;
    } else {
      MapV_PrintTableCfg(map);
      MapV_PrintTableCfg(oldMap);
      printf("could not re-insert on redistribute\n");
      exit(1);
    }
  }

  return true;
}

//------------------------------------------------------------------------------
static inline bool
_tbl_realloc_grow(MapV_st* cur)
{
  MapV_st new = *cur; // copy our current table config for modifications
                      // until we're certain memory has allocated, etc.

  new.meta.slotsCap = _pow2_next_u64(++new.meta.slotsCap);

  // pre-compute this so we're not calculating it on every lookup
  new.meta.slotHashShift = 64 - log2(new.meta.slotsCap);

  new.meta.bktsCnt = new.meta.slotsCap / MAPV_BKT_SLOTS;

  // add extra buckets for the last bucket's overflow
  // but do not increase .meta.slotsCap
  // when inserting, we'll only check against .cfg.distSlotMax
  if (new.cfg.distSlotMax > (new.cfg.distBktMax * MAPV_BKT_SLOTS)) {
    new.meta.bktsCntReal = new.meta.bktsCnt
                         + (new.cfg.distSlotMax / MAPV_BKT_SLOTS)
                         - 1; // -1 because we already have an initial bucket
  } else {
    new.meta.bktsCntReal = new.meta.bktsCnt
                         + new.cfg.distBktMax
                         - 1; // -1 because we already have an initial bucket
  }

  new.meta.slotsCapReal = new.meta.bktsCntReal * MAPV_BKT_SLOTS;

  new.meta.tblBytes = new.meta.slotsCapReal
                    / MAPV_BKT_SLOTS
                    * sizeof(*new.tbl.bkt);

  // allocate extra, then trim for alignment
  new.meta.tblBytesReal = new.meta.tblBytes + (2 * new.cfg.memAlign);

  //--------------------------------------------------------------------
  // setup is done. now alloc and align.

  new.tbl.bktPtrReal = calloc(1, new.meta.tblBytesReal);
  if (NULL == new.tbl.bktPtrReal) {
    // @TODO: get error
    return false;
  }

  _tbl_cap_update(&new);

  // set our bucket to an aligned address
  new.tbl.bkt = (void*)(((uint64_t)new.tbl.bktPtrReal / new.cfg.memAlign)
                         * new.cfg.memAlign
                         + new.cfg.memAlign);

  if (0 == cur->meta.slotsUsed) {
    *cur = new;
    return true;
  }

  if (!_tbl_redistribute_hashes(&new, cur)) {
    // printf("_tbl_realloc_grow(): _tbl_redistribute_hashes() failed\n");
    return false;
  }

  free(cur->tbl.bktPtrReal);
  *cur = new;

  return true;
}

