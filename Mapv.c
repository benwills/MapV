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

#include "xxhash.h"

#define DBG 1
#define DBG_FFL {printf("DBG: File: [%s] Func: [%s] Line: [%d]\n", \
                 __FILE__, __FUNCTION__, __LINE__);fflush(stdout);}


//
// @NOTES: this is technically a probabilistic data structure, as the original
//         key is not compared. this is "okay" because 128 bits of a very high
//         quality hash is used for key lookups. there are multiple benefits:
//         primarily, cache locality of all lookup data.
//         secondarily, reduced memory usage from not storing original keys.
//                      this is particularly useful with long keys (urls)
//
//         i'll be creating another variation that does use original key
//         comparisons, and will be able to use 64 bit hashes vs 128.
//         this will double the speed up hash lookups, but involves another
//         random memory access. so we'll see how it does.
//
//         it's also worth noting that this uses 256-bit vector instructions.
//         avx512 would double the speed of lookups, and modifying the code
//         to handled that would be trivial. i'll have to see if i have access
//         to a cpu that supports avx512
//

//
// @NOTE for my future self:
//
// apparently an equality comparison of certain floating point double values,
// even though the bits are exactly the same, does not always return true with
// certain vector instructions. eg: _mm256_movemask_pd()
//
// the same 10 terms, out of 10,000 in my test set, were failing on lookups,
// even though the data was right there.
//
// that was a very long several hours. i honestly can't make sense of what was
// happening; whether it's a compiler/cpu bug, or if there's something i just
// don't understand about vector instructions (very possible).
//
// anyway...the hash table is now properly resizing when hitting certain
// thresholds. once deletes are done, it'll be a suitable alpha.
//


//
// 10k short keys, 1k iterations
//     currrent fastest: https://github.com/martinus/unordered_dense
//     lookups per second : 45,724,225.09
//     lookups per second : 45,586,745.62
//     lookups per second : 46,176,749.84
//     lookups per second : 45,053,718.42
//     lookups per second : 47,111,701.40
//     lookups per second : 43,749,540.65
//     lookups per second : 43,614,950.53
//     lookups per second : 46,957,599.19
//     lookups per second : 46,584,399.40
//     lookups per second : 48,132,160.20
//     average: 45,869,179.03
//
//     MapV:
//     lookups per second : 55,155,926.96
//     lookups per second : 55,046,138.41
//     lookups per second : 58,203,247.47
//     lookups per second : 61,124,753.78
//     lookups per second : 58,144,229.02
//     lookups per second : 59,746,045.25
//     lookups per second : 58,143,708.05
//     lookups per second : 62,624,697.12
//     lookups per second : 57,273,327.09
//     lookups per second : 54,008,191.17
//     average: 57,947,026.43
//
// 12MM urls, 10 iterations
//     ankerl:
//     lookups per second : 5,398,034.24
//
//     MapV:
//     lookups per second : 8,428,561.08
//




//==============================================================================
#define MAPV_HASH_BYTES      sizeof(Mapv_Hash_st)
#define MAPV_HASH_PART_BYTES sizeof(uint64_t)
#define MAPV_VAL_BYTES       sizeof(uint64_t)
#define MAPV_PSL_BYTES       sizeof(uint8_t)

#define MAPV_BKT_HASH_BYTES  (MAPV_HASH_BYTES * MAPV_BKT_ENTS)
#define MAPV_BKT_VAL_BYTES   (MAPV_VAL_BYTES  * MAPV_BKT_ENTS)
#define MAPV_BKT_PSL_BYTES   (MAPV_PSL_BYTES  * MAPV_BKT_ENTS)
#define MAPV_BKT_BYTES       (MAPV_HASH_BYTES+MAPV_VAL_BYTES+MAPV_BKT_PSL_BYTES)
#define MAPV_U64_PER_SLOT    4 // (sizeof(__m256i) / sizeof(uint64_t))
#define MAPV_BKT_SLOTS       4 // (MAPV_BKT_ENTS / MAPV_U64_PER_SLOT)

typedef XXH128_hash_t Mapv_Hash_st;
typedef uint64_t      Mapv_HashHi_t;
typedef uint64_t      Mapv_HashLo_t;
typedef uint64_t      Mapv_Val_t;

typedef uint_fast32_t Mapv_SlotId_t;
typedef uint_fast32_t Mapv_BktId_t;
typedef uint_fast32_t Mapv_Dist_t; // distance. NOTE: unsigned. check compares

// used internally
typedef struct Mapv_HV_st {
  Mapv_Hash_st hash;
  Mapv_Val_t   val;
} Mapv_HV_st;

typedef struct Mapv_Bkt_st {
  Mapv_HashHi_t slotsHi[MAPV_BKT_SLOTS];
  Mapv_HashLo_t slotsLo[MAPV_BKT_SLOTS];
  Mapv_Val_t    vals   [MAPV_BKT_SLOTS];
} Mapv_Bkt_st;

typedef struct Mapv_Cfg_st {
  Mapv_Dist_t distSlotMax; // when we pass this, increase table sise
  Mapv_Dist_t distBktMax;  // when we pass this, increase table sise
  double      capPctMax;   // when we pass this, increase table sise
  int         memAlign;    // must be multuple of 32
} Mapv_Cfg_st;

typedef struct Mapv_Meta_st {
  // calculated on insert/delete
  // to make other calculations faster
  uint64_t tblBytes;        // after "alignment"
  uint64_t tblBytesReal;    // before "alignment"

  uint64_t bktsCnt;         // buckets have 4 slots for entries
  uint64_t bktsCntReal;     // buckets have 4 slots for entries
  uint64_t slotHashShift;   // pre-calc; for finding our bucket index

  uint64_t entriesCap;      // number of slots in the table
  uint64_t entriesCapReal;  // including extra final buckets
  uint64_t entriesUsed;     // # values in the table
  uint64_t entriesAvail;    // # of slots open
  double   entriesCapPct;   // tblSlotsUsed / tblSlotCap

  // distances: aka: probe sequence length
  // note that a distance of 1, is actualy two slots/buckets
  // so our iterator needs that incremented. hence these iter values
  uint64_t distSlotMax;
  uint64_t distSlotIter;
  uint64_t distBktMax;
  uint64_t distBktIter;
} Mapv_Meta_st;

typedef struct Mapv_Tbl_st {
  Mapv_Bkt_st* bktPtrReal; // ptr to free(). alloc extra for alignment
  Mapv_Bkt_st* bkt;
} Mapv_Tbl_st;


typedef struct Mapv_st {
  Mapv_Cfg_st  cfg;
  Mapv_Meta_st meta;
  Mapv_Tbl_st  tbl;
} Mapv_st;





//------------------------------------------------------------------------------

void
Mapv_PrintTableCfg(const Mapv_st* map);

void
Print_Hv(Mapv_HV_st hv);

static inline uint64_t
_pow2_next_u64(uint64_t n);

static inline Mapv_Hash_st
_hash(const void* key, const size_t keyLen);

static inline Mapv_BktId_t
_bkt_from_slot(const Mapv_SlotId_t slot);

static inline Mapv_BktId_t
_bktslot_from_slot(const Mapv_SlotId_t slot);

static inline Mapv_SlotId_t
_slot_from_hash_hi(const Mapv_st* map, Mapv_HashHi_t hashHi);

static inline Mapv_Dist_t
_slot_hash_hi_dist(const Mapv_st*      map,
                   const Mapv_HashHi_t hashHi,
                   const Mapv_SlotId_t cmpSlot);

static inline void
_tbl_cap_update(Mapv_st* map);

static inline void
_tbl_get_clear_slot(const Mapv_st*      map,
                    const Mapv_SlotId_t slot);

static inline void
_tbl_get_hv_from_slot(const Mapv_st*      map,
                      const Mapv_SlotId_t slot,
                            Mapv_HV_st*   hv);

static inline void
_tbl_set_hv_into_slot(const Mapv_st*      map,
                      const Mapv_SlotId_t slot,
                      const Mapv_HV_st*   hv);

static inline void
_tbl_dist_update(Mapv_st* map, Mapv_HashHi_t hashHi, Mapv_SlotId_t slot);

static inline bool
_tbl_should_realloc(Mapv_st* map);

static inline bool
_tbl_insert_hv(Mapv_st*   map,
               Mapv_HV_st newHv);

static inline bool
_tbl_redistribute_hashes(Mapv_st* map, Mapv_st* oldMap);

static inline bool
_tbl_realloc_grow(Mapv_st* cur);

void
Mapv_PrintTableData(const Mapv_st* map);

Mapv_st*
Mapv_Create(const Mapv_Dist_t distSlotMax,
            const Mapv_Dist_t distBktMax,
            const double      capPctMax,
            const int         memAlign,
            const uint64_t    initialEntryCount);

bool
Mapv_Delete(Mapv_st*     map,
            const void*  key,
            const size_t keyLen);

bool
Mapv_Insert(Mapv_st*         map,
            const void*      key,
            const size_t     keyLen,
            const Mapv_Val_t val);

bool
Mapv_Find(const Mapv_st* map,
          const void*    key,
          const size_t   keyLen,
              uint64_t*  val);






//==============================================================================
//
// print
//
//==============================================================================

//------------------------------------------------------------------------------
void
Mapv_PrintTableCfg(const Mapv_st* map)
{
  printf("\n\n");
  printf("\n----------------------------------------------------------------\n");

  printf("cfg.distSlotMax     : %"PRIuFAST32"\n", map->cfg.distSlotMax);
  printf("cfg.distBktMax      : %"PRIuFAST32"\n", map->cfg.distSlotMax);
  printf("cfg.capPctMax       : %f\n",            map->cfg.capPctMax);
  printf("\n");

  printf("meta.tblBytes       : %"PRIu64"\n", map->meta.tblBytes);
  printf("meta.tblBytesReal   : %"PRIu64"\n", map->meta.tblBytesReal);
  printf("meta.bktsCnt        : %"PRIu64"\n", map->meta.bktsCnt);
  printf("meta.bktsCntReal    : %"PRIu64"\n", map->meta.bktsCntReal);
  printf("meta.slotHashShift  : %"PRIu64"\n", map->meta.slotHashShift);
  printf("\n");

  printf("meta.entriesCap     : %"PRIu64"\n", map->meta.entriesCap);
  printf("meta.entriesCapReal : %"PRIu64"\n", map->meta.entriesCapReal);

  printf("meta.entriesUsed    : %"PRIu64"\n", map->meta.entriesUsed);
  printf("meta.entriesAvail   : %"PRIu64"\n", map->meta.entriesAvail);
  printf("meta.entriesCapPct  : %f\n",        map->meta.entriesCapPct);
  printf("\n");

  printf("meta.distSlotMax    : %"PRIu64"\n", map->meta.distSlotMax);
  printf("meta.distSlotIter   : %"PRIu64"\n", map->meta.distSlotIter);
  printf("meta.distBktMax     : %"PRIu64"\n", map->meta.distBktMax);
  printf("meta.distBktIter    : %"PRIu64"\n", map->meta.distBktIter);
  printf("\n");

  printf("tbl.bktPtrReal      : %p\n", map->tbl.bktPtrReal);
  printf("tbl.bkt             : %p\n", map->tbl.bkt);
  printf("\n");

  printf("\n----------------------------------------------------------------\n");
  printf("\n\n");
  fflush(stdout);
}


//------------------------------------------------------------------------------
void
Print_Hv(Mapv_HV_st hv) {
  printf("\n\n---------------\n");
  printf("HV:\n");
  printf("\thi64 : %"PRIu64"\n", hv.hash.high64);
  printf("\tlo64 : %"PRIu64"\n", hv.hash.low64);
  printf("\tval  : %"PRIu64"\n", hv.val);
  printf("\n");
  fflush(stdout);
}


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
static inline Mapv_Hash_st
_hash(const void* key, const size_t keyLen)
{
  return XXH3_128bits(key, keyLen);
}


//==============================================================================
//
// _bkt...()
//
//------------------------------------------------------------------------------
static inline Mapv_BktId_t
_bkt_from_slot(const Mapv_SlotId_t slot)
{
  return slot / MAPV_BKT_SLOTS;
}

//------------------------------------------------------------------------------
static inline Mapv_BktId_t
_bktslot_from_slot(const Mapv_SlotId_t slot)
{
  return slot % MAPV_BKT_SLOTS;
}


//==============================================================================
//
// _slot...()
//
//------------------------------------------------------------------------------
static inline Mapv_SlotId_t
_slot_from_hash_hi(const Mapv_st* map, Mapv_HashHi_t hashHi)
{
  return hashHi >> map->meta.slotHashShift;
}

//------------------------------------------------------------------------------
static inline Mapv_Dist_t
_slot_hash_hi_dist(const Mapv_st*      map,
                   const Mapv_HashHi_t hashHi,
                   const Mapv_SlotId_t cmpSlot)
{
  const Mapv_SlotId_t iniSlot = _slot_from_hash_hi(map, hashHi);
  return cmpSlot - iniSlot;
}


//==============================================================================
//
// _tbl...()
//
//------------------------------------------------------------------------------
static inline void
_tbl_cap_update(Mapv_st* map)
{
  map->meta.entriesAvail  = map->meta.entriesCapReal - map->meta.entriesUsed;
  map->meta.entriesCapPct = (double)map->meta.entriesUsed
                          / (double)map->meta.entriesCapReal
                          * 100;
}

//------------------------------------------------------------------------------
static inline void
_tbl_get_clear_slot(const Mapv_st*      map,
                    const Mapv_SlotId_t slot)
{
  Mapv_BktId_t bktId     = _bkt_from_slot(slot);
  Mapv_BktId_t bktSlotId = _bktslot_from_slot(slot);
  map->tbl.bkt[bktId].slotsHi[bktSlotId] = 0;
  map->tbl.bkt[bktId].slotsLo[bktSlotId] = 0;
  map->tbl.bkt[bktId].vals   [bktSlotId] = 0;
}

//------------------------------------------------------------------------------
static inline void
_tbl_get_hv_from_slot(const Mapv_st*      map,
                      const Mapv_SlotId_t slot,
                            Mapv_HV_st*   hv)
{
  Mapv_BktId_t bktId     = _bkt_from_slot(slot);
  Mapv_BktId_t bktSlotId = _bktslot_from_slot(slot);
  hv->hash.high64 = map->tbl.bkt[bktId].slotsHi[bktSlotId];
  hv->hash.low64  = map->tbl.bkt[bktId].slotsLo[bktSlotId];
  hv->val         = map->tbl.bkt[bktId].vals   [bktSlotId];
}

//------------------------------------------------------------------------------
static inline void
_tbl_set_hv_into_slot(const Mapv_st*      map,
                      const Mapv_SlotId_t slot,
                      const Mapv_HV_st*   hv)
{
  Mapv_BktId_t bktId     = _bkt_from_slot(slot);
  Mapv_BktId_t bktSlotId = _bktslot_from_slot(slot);
  map->tbl.bkt[bktId].slotsHi[bktSlotId] = hv->hash.high64;
  map->tbl.bkt[bktId].slotsLo[bktSlotId] = hv->hash.low64;
  map->tbl.bkt[bktId].vals   [bktSlotId] = hv->val;
}

//------------------------------------------------------------------------------
static inline void
_tbl_dist_update(Mapv_st* map, Mapv_HashHi_t hashHi, Mapv_SlotId_t slot)
{
  const Mapv_Dist_t slotDist = _slot_hash_hi_dist(map, hashHi, slot);

  if (slotDist > map->meta.distSlotMax) {
    map->meta.distSlotMax  = slotDist;
    map->meta.distSlotIter = slotDist + 1;
  }

  const Mapv_SlotId_t targetSlot = _slot_from_hash_hi(map, hashHi);
  const Mapv_BktId_t  targetBkt  = _bkt_from_slot(targetSlot);
  const Mapv_BktId_t  actualBkt  = _bkt_from_slot(slot);
  const Mapv_Dist_t   bktDist    = (actualBkt > targetBkt)
                                 ? (actualBkt - targetBkt)
                                 : (targetBkt - actualBkt);
  if (bktDist > map->meta.distBktMax) {
    map->meta.distBktMax  = bktDist;
    map->meta.distBktIter = bktDist + 1;
  }
}

//------------------------------------------------------------------------------
static inline bool
_tbl_should_realloc(Mapv_st* map)
{
  if (map->meta.distSlotIter > map->cfg.distSlotMax) {
    return true;
  }
  if (map->meta.distBktIter > map->cfg.distBktMax) {
    return true;
  }
  if (map->meta.entriesCapPct > map->cfg.capPctMax) {
    return true;
  }
  return false;
}


//------------------------------------------------------------------------------
static inline bool
_tbl_insert_hv(Mapv_st*   map,
               Mapv_HV_st newHv)
{
  if (_tbl_should_realloc(map)) {
    return false;
  }

  uint64_t slot = _slot_from_hash_hi(map, newHv.hash.high64);

  do
  {
    const int newSlotDist = _slot_hash_hi_dist(map, newHv.hash.high64, slot);

    Mapv_HV_st curHv;
    _tbl_get_hv_from_slot(map, slot, &curHv);
    if (curHv.hash.high64 == 0 && curHv.hash.low64 == 0 && curHv.val == 0) {
      _tbl_set_hv_into_slot(map, slot, &newHv);
      _tbl_dist_update(map, newHv.hash.high64, slot);
      return true;
    }

    const int curSlotDist = _slot_hash_hi_dist(map, curHv.hash.high64, slot);
    if (newSlotDist > curSlotDist) {
      _tbl_set_hv_into_slot(map, slot, &newHv);
      _tbl_dist_update(map, newHv.hash.high64, slot);
      newHv = curHv;
    }

    if (curSlotDist >= map->cfg.distSlotMax) {
      return false;
    }

    slot++;

  } while (1);
}


//------------------------------------------------------------------------------
static inline bool
_tbl_redistribute_hashes(Mapv_st* map, Mapv_st* oldMap)
{
  map->meta.distSlotMax  = 0;
  map->meta.distSlotIter = 0;
  map->meta.distBktMax   = 0;
  map->meta.distBktIter  = 0;

  for (int oldSlot = oldMap->meta.entriesCapReal; oldSlot >= 0; oldSlot--)
  {
    Mapv_HV_st newHv;
    _tbl_get_hv_from_slot(oldMap, oldSlot, &newHv);
    if (newHv.hash.high64 == 0 && newHv.hash.low64 == 0 && newHv.val == 0) {
      continue;
    }

    // should always return true after realloc...?
    // @TODO: vertify
    if (_tbl_insert_hv(map, newHv)) {
      _tbl_get_clear_slot(oldMap, oldSlot);
      continue;
    } else {
      Mapv_PrintTableCfg(map);
      Mapv_PrintTableCfg(oldMap);
      printf("could not re-insert on redistribute\n");
      exit(1);
    }
  }

  return true;
}

//------------------------------------------------------------------------------
static inline bool
_tbl_realloc_grow(Mapv_st* cur)
{
  printf("_tbl_realloc_grow()ing...\n");fflush(stdout);

  Mapv_st new = *cur; // copy our current table for modifications
                      // until we're certain memory has allocated, etc.

  new.meta.entriesCap = _pow2_next_u64(++new.meta.entriesCap);

  // pre-compute this so we're not calculating it on every lookup
  new.meta.slotHashShift = 64 - log2(new.meta.entriesCap);

  new.meta.bktsCnt = new.meta.entriesCap / MAPV_BKT_SLOTS;

  // add extra buckets for the last bucket's overflow
  // but do not increase .meta.entriesCap
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

  new.meta.entriesCapReal = new.meta.bktsCntReal * MAPV_BKT_SLOTS;

  new.meta.tblBytes = new.meta.entriesCapReal
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

  if (0 == cur->meta.entriesUsed) {
    *cur = new;
    return true;
  }

  if (!_tbl_redistribute_hashes(&new, cur)) {
    printf("_tbl_realloc_grow(): _tbl_redistribute_hashes() failed\n");
    return false;
  }

  free(cur->tbl.bktPtrReal);
  *cur = new;
  return true;
}


//------------------------------------------------------------------------------
void
Mapv_PrintTableData(const Mapv_st* map)
{
  printf("\n\n");

  int distSlotMax = 0;
  int numFound    = 0;
  for (int slot = 0; slot < map->meta.entriesCapReal; slot++)
  {
    Mapv_HV_st hv;
    _tbl_get_hv_from_slot(map, slot, &hv);
    if (hv.hash.high64 == 0 && hv.hash.low64 == 0 && hv.val == 0) {
      continue;
    }

    numFound++;
    const int slotDist = _slot_hash_hi_dist(map, hv.hash.high64, slot);
    printf("\t[%d] %"PRIu64" %"PRIu64" : %"PRIu64" : %d\n",
           slot, hv.hash.high64, hv.hash.low64, hv.val, slotDist);
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
//
// this is essentially our resize function. it'll just need some changes
//
Mapv_st*
Mapv_Create(const Mapv_Dist_t distSlotMax,
            const Mapv_Dist_t distBktMax,
            const double      capPctMax,
            const int         memAlign,
            const uint64_t    initialEntryCount)
{
  if (memAlign % 32 != 0) {
    printf("alignment must be a multiple of 32 bytes. (4096 recommended)\n");
    printf("attempted to configure with %d bytes\n", memAlign);
    return NULL;
  }

  Mapv_st* map = calloc(1, sizeof(*map));

  map->cfg.distSlotMax = distSlotMax;
  map->cfg.distBktMax  = distBktMax;
  map->cfg.capPctMax   = capPctMax;
  map->cfg.memAlign    = memAlign;

  map->meta.entriesCap = initialEntryCount;

  if (!_tbl_realloc_grow(map)) {
    free(map);
    printf("_tbl_realloc_grow() failed\n");
    exit(1);
    return NULL;
  }

  return map;
}


//------------------------------------------------------------------------------
bool
Mapv_Delete(Mapv_st*     map,
            const void*  key,
            const size_t keyLen)
{
  // @TODO
  return false;
}


//------------------------------------------------------------------------------
bool
Mapv_Insert(Mapv_st*         map,
            const void*      key,
            const size_t     keyLen,
            const Mapv_Val_t val)
{
  Mapv_HV_st newHv = { .hash = _hash(key, keyLen), .val = val, };

  if (!_tbl_insert_hv(map, newHv)) {
    if (_tbl_should_realloc(map)) {
      if (!_tbl_realloc_grow(map)) {
        printf("Mapv_Insert(): _tbl_realloc_grow() failed\n");
        return false;
      }
    }
    // it's possible to get here? can't insert, but shouldn't realloc?
  } else {
    map->meta.entriesUsed++;
    _tbl_cap_update(map);
    return true;
  }

  // should always return true at this point...?
  // @TODO: vertify
  return _tbl_insert_hv(map, newHv);
}


//------------------------------------------------------------------------------
bool
Mapv_Find(const Mapv_st* map,
          const void*    key,
          const size_t   keyLen,
              uint64_t*  val)
{
  // printf("\n");
  Mapv_Hash_st  hash   = _hash(key, keyLen);
  Mapv_SlotId_t slotId = _slot_from_hash_hi(map, hash.high64);

  const uint64_t* pHashHi = (uint64_t*)&hash.high64;
  const __m256i needleHi = _mm256_set_epi64x(*pHashHi,*pHashHi,
                                             *pHashHi,*pHashHi);
  const uint64_t* pHashLo = (uint64_t*)&hash.low64;
  const __m256i needleLo = _mm256_set_epi64x(*pHashLo,*pHashLo,
                                             *pHashLo,*pHashLo);

  __m256i found;
  __m256i haystack;

  const int maxIters = map->meta.distBktIter;
  for (int iter = 0; iter < maxIters; iter++)
  {
    // printf(".");

    // `| 0x100` is to set a highest bit as an indicator that nothing was found
    // if no matches were found found, idxFoundHi/Lo will == 8.
    // could also set to 0x10 and check idxFound is 4.
    // but that's less clear, given we're working with 4 array indexes.

    const Mapv_BktId_t  bktId     = slotId / MAPV_BKT_SLOTS;
    const Mapv_SlotId_t bktSlotId = slotId % MAPV_BKT_SLOTS;

    haystack = _mm256_load_si256((__m256i*)map->tbl.bkt[bktId].slotsHi);
    found    = _mm256_cmpeq_epi64(haystack, needleHi);
    const int idxFoundHi = __builtin_ctz(_mm256_movemask_pd((__m256d)found) | 0x100);
    if (idxFoundHi == 8) { // not found
      slotId += MAPV_BKT_SLOTS;
      continue;
    }

    haystack = _mm256_load_si256((__m256i*)map->tbl.bkt[bktId].slotsLo);
    found    = _mm256_cmpeq_epi64(haystack, needleLo);
    const int idxFoundLo = __builtin_ctz(_mm256_movemask_pd((__m256d)found) | 0x100);
    if (idxFoundHi == idxFoundLo) {
      *val = map->tbl.bkt[bktId].vals[idxFoundLo];
      return true;
    }

    // not found
    slotId += MAPV_BKT_SLOTS;
  }
  return false;
}




//==============================================================================
//==============================================================================
//==============================================================================
//==============================================================================
const char* INPUT_FILE = "/media/src/c/hashing/hsh.key/_in/00000--google-10000-english.txt";
// const char* INPUT_FILE = "/home/o/Desktop/xub-root/media/src/c/hashing/hsh.key/_in/00887--urls.12MM.txt";




//------------------------------------------------------------------------------
char**
file_to_str_arr(const char* fname, uint64_t* cnt)
{
  char**  arr  = NULL;
  size_t  len  = 0;
  ssize_t chs  = 0;
  char*   line = NULL;
  FILE*   fp   = fopen(fname, "r");

  if (fp == NULL) {
    printf("Tried to open: %s\n", fname);
    perror("Can't open input file.");
    fflush(stdout);
    exit(1);
  }

  *cnt = 0;
  fseek(fp, 0, SEEK_SET);
  while ((chs = getline(&line, &len, fp)) != -1) {
    if ((chs - 1) > 0) { // includes newline
      ++*cnt;
    }
  }
  arr = malloc(*cnt * sizeof(char*));

  *cnt = 0;
  fseek(fp, 0, SEEK_SET);
  while ((chs = getline(&line, &len, fp)) != -1) {
    if ((chs - 1) > 0) {
      arr[*cnt] = (char*)malloc(chs * sizeof(char));
      strncpy(arr[*cnt], line, chs);
      arr[*cnt][chs-1] = '\0';
      ++*cnt;
    }
  }
  fclose (fp);

  return arr;
}

// call this function to start a nanosecond-resolution timer
struct timespec timer_start(){
  struct timespec start_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);
  return start_time;
}

// call this function to end a timer, returning nanoseconds elapsed as a long
long timer_end(struct timespec start_time) {
  struct timespec end_time;
  clock_gettime(CLOCK_MONOTONIC, &end_time);
  long diffInNanos = (end_time.tv_sec - start_time.tv_sec)
                   * (long)1e9
                   + (end_time.tv_nsec - start_time.tv_nsec);
  return diffInNanos;
}



//------------------------------------------------------------------------------
int main()
{
  uint64_t valArrCnt = 0;
  char**   valArr    = file_to_str_arr(INPUT_FILE, &valArrCnt);
  printf("valArrCnt   : %"PRIu64"\n\n", valArrCnt);

  size_t* strLenArr = calloc(valArrCnt, sizeof(size_t));
  for (uint64_t i = 0; i < valArrCnt; i++) {
    strLenArr[i] = strlen(valArr[i]);
  }

  //---------------------------
  Mapv_st* map = Mapv_Create(32, 8, 90, 4096, 10);
  if (NULL == map) {
    printf("Mapv_Create failed\n");
    exit(1);
  }

  //---------------------------
  Mapv_PrintTableCfg(map);

  //---------------------------
  for (uint64_t i = 0; i < valArrCnt; i++) {
    // printf("\n> %d: ", (int)i);
    if (!Mapv_Insert(map, valArr[i], strLenArr[i], i)) {
      Mapv_PrintTableCfg(map);
      printf("Mapv_Insert failed\n");
      exit(1);
    }
  }

  //---------------------------
  printf("done\n");
  Mapv_PrintTableCfg(map);

  //---------------------------
  struct timespec vartime = timer_start();
  int sum = 0;
  int count      = 0;
  int iterations = 10;
  int  boolCount = 0;
  for (int iter = 0; iter < iterations; ++iter)
  {
    for (uint64_t i = 0; i < valArrCnt; i++)
    {
      const char*  key    = valArr[i];
      const size_t keyLen = strLenArr[i];

      uint64_t val;
      bool ret = Mapv_Find(map, key, keyLen, &val);
      if (false == ret) {
        printf("Mapv_Find failed: %d: %.*s\n", (int)i, (int)keyLen, key);
        // exit(1);
      } else {
        // printf("FOUND VAL: %"PRIu64"\n", val);
        // sum += val;
        boolCount++;
      }

      count++;
    }
  }

  long time_elapsed_nanos = timer_end(vartime);

  printf("Time taken (nanoseconds): %ld\n", time_elapsed_nanos);

  double iterPerSec = count * ((double)1000000000 / (double)time_elapsed_nanos);
  printf("lookups per second : %f\n", iterPerSec);

  printf("boolCount : %d\n\n", boolCount);
  printf("count : %d\n\n", count);
  printf("sum : %d\n\n", sum);

  if (boolCount != count) {
    printf("count and boolCount do not match!!!\n");
  }
}
