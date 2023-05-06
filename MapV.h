#ifndef _MapV_MapV_h_
#define _MapV_MapV_h_

#include <inttypes.h>
#include <stdbool.h>

#include <xxhash.h>




//==============================================================================
#define MAPV_HASH_BYTES      sizeof(MapV_Hash_st)
#define MAPV_HASH_PART_BYTES sizeof(uint64_t)
#define MAPV_VAL_BYTES       sizeof(uint64_t)
#define MAPV_PSL_BYTES       sizeof(uint8_t)

#define MAPV_BKT_HASH_BYTES  (MAPV_HASH_BYTES * MAPV_BKT_ENTS)
#define MAPV_BKT_VAL_BYTES   (MAPV_VAL_BYTES  * MAPV_BKT_ENTS)
#define MAPV_BKT_PSL_BYTES   (MAPV_PSL_BYTES  * MAPV_BKT_ENTS)
#define MAPV_BKT_BYTES       (MAPV_HASH_BYTES+MAPV_VAL_BYTES+MAPV_BKT_PSL_BYTES)
#define MAPV_U64_PER_SLOT    4 // (sizeof(__m256i) / sizeof(uint64_t))
#define MAPV_BKT_SLOTS       4 // (MAPV_BKT_ENTS / MAPV_U64_PER_SLOT)




//------------------------------------------------------------------------------
typedef enum MapV_Err_et
{
	MAPV_ERR__OK,

	MAPV_ERR__TABLE_MUST_GROW,
	MAPV_ERR__TABLE_GROW_FAILED,

	MAPV_ERR__INSERT_KEY_EXISTS,

	MAPV_ERR__DELETE_KEY_NOT_FOUND,

	MAPV_ERR__DESTROY_MAP_IS_NULL,
	MAPV_ERR__DESTROY_MAP_BKTPTRREAL_IS_NULL, // unused. see MapV_Destroy()

	//------------------------------------
	MAPV_ERR___FIRST = MAPV_ERR__OK,
	MAPV_ERR___LAST  = MAPV_ERR__DELETE_KEY_NOT_FOUND,
	MAPV_ERR___COUNT = MAPV_ERR___LAST,
} MapV_Err_et;


//------------------------------------------------------------------------------
typedef XXH128_hash_t MapV_Hash_st;
typedef uint64_t      MapV_HashHi_t;
typedef uint64_t      MapV_HashLo_t;
typedef uint64_t      MapV_SlotId_t;
typedef uint64_t      MapV_BktId_t;
typedef uint64_t      MapV_Dist_t; // distance / PSL (probe sequence length)
                                   // NOTE: careful; unsigned.
typedef union         MapV_Val_ut {
	uint64_t    u64;
	const void* ptr;
} MapV_Val_ut;


//------------------------------------------------------------------------------
// used internally: "hv" = "hash and val," where val is the 8 byte ptr/data
typedef struct MapV_HV_st {
  MapV_Hash_st hash;
  MapV_Val_ut  val;
} MapV_HV_st;

typedef struct MapV_Bkt_st {
  MapV_HashHi_t slotsHi[MAPV_BKT_SLOTS];
  MapV_HashLo_t slotsLo[MAPV_BKT_SLOTS];
  MapV_Val_ut   vals   [MAPV_BKT_SLOTS];
} MapV_Bkt_st;

typedef struct MapV_Cfg_st {
  MapV_Dist_t distSlotMax;      // max slot probe distance before resize
  MapV_Dist_t distBktMax;       // max bucket probe distance before resize
  double      capPctMax;        // max capacity percentage before resize
  int         memAlign;         // hash table memory alignment. multiple of 32
  uint64_t    initialSlotCount; // if you know how many entries you have,
                                // set it here, with extra, to avoid reallocing
                                // and rebuilding the table as it grows.
} MapV_Cfg_st;

typedef struct MapV_Meta_st {
  uint64_t tblBytes;      // after "alignment"
  uint64_t tblBytesReal;  // before "alignment"

  uint64_t bktsCnt;       // buckets have 4 slots for entries
  uint64_t bktsCntReal;   // buckets have 4 slots for entries

  uint64_t slotHashShift; // pre-calc; for finding our bucket index
  uint64_t slotsCap;      // number of slots in the table
  uint64_t slotsCapReal;  // including extra final buckets
  uint64_t slotsUsed;     // # values in the table
  uint64_t slotsAvail;    // # of slots open
  double   slotsCapPct;   // tblSlotsUsed / tblSlotCap

  // distances: aka: probe sequence length
  // note that a distance of 1, is actualy two slots/buckets
  // so our iterator needs that incremented.
  // hence the pre-calculated iter values.
  uint64_t distSlotMax;
  uint64_t distSlotIter;
  uint64_t distBktMax;
  uint64_t distBktIter;
} MapV_Meta_st;

typedef struct MapV_Tbl_st {
  MapV_Bkt_st* bktPtrReal; // ptr to free(). alloc extra for alignment
  MapV_Bkt_st* bkt;
} MapV_Tbl_st;

typedef struct MapV_Stats_st {
	uint64_t mm256Loads;
} MapV_Stats_st;

typedef struct MapV_st {
  MapV_Cfg_st   cfg;
  MapV_Meta_st  meta;
  MapV_Tbl_st   tbl;
  MapV_Stats_st stats;
} MapV_st;




//------------------------------------------------------------------------------
MapV_st*
MapV_Create(const MapV_Cfg_st* cfg);

MapV_Err_et
MapV_Insert(      MapV_st*    map,
            const void*       key,
            const size_t      keyLen,
            const MapV_Val_ut val,
            const bool        overwriteIfExists);

bool
MapV_Find(      MapV_st*     map,
          const void*        key,
          const size_t       keyLen,
                MapV_Val_ut* val);

MapV_Err_et
MapV_Delete(      MapV_st* map,
            const void*    key,
            const size_t   keyLen);

MapV_Err_et
MapV_Destroy(MapV_st* map);

void
MapV_PrintTableCfg(const MapV_st* map);

void
MapV_PrintTableData(const MapV_st* map);

const char*
MapV_PrintErr(MapV_Err_et err);



#endif // _MapV_MapV_h_
