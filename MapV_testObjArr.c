#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "MapV.h"
#include "MapV.c"

/*
make clean && make && make test
*/

//------------------------------------------------------------------------------
void Print_One(void) {
	printf("Print_One\n");
}
void Print_Two(void) {
	printf("Print_Two\n");
}
void Print_Three(void) {
	printf("Print_Three\n");
}
typedef struct Lookup_st {
	const char* strKey;
	void        (*fn)(void);
} Lookup_st;
const Lookup_st LookupArr[3] = {
	{ "one",   &Print_One,   },
	{ "two",   &Print_Two,   },
	{ "three", &Print_Three, },
};


//------------------------------------------------------------------------------
int main(int argc, char *argv[])
{
  uint64_t valArrCnt = sizeof(LookupArr) / sizeof(LookupArr[0]);

  //------------------------------------------------------------
  // Init
  MapV_Cfg_st cfg = {
  	.distSlotMax      = 32,   // max slot probe distance before resize
  	.distBktMax       = 8,    // max bucket probe distance before resize
  	.capPctMax        = 90,   // max capacity percentage before resize
  	.memAlign         = 4096, // hash table memory alignment. multiple of 32
  	.initialSlotCount = 10,    // if you know how many entries you have,
                               // set it here, with extra, to avoid reallocing
                               // and rebuilding the table as it grows.
  };
  MapV_st* map;
  if (NULL == (map = MapV_Create(&cfg))) {
    printf("MapV_Create failed\n");
    exit(1);
  }

  //------------------------------------------------------------
  // Insert
  for (uint64_t i = 0; i < valArrCnt; i++)
  {
  	const char*  key    = LookupArr[i].strKey;
  	const size_t keyLen = strlen(LookupArr[i].strKey);
	  const MapV_Val_ut val = {
			// .u64 = (uint64_t)&(LookupArr[i]),
			.ptr = (const void*)&(LookupArr[i]),
	  };

	  MapV_Err_et err;
    if (MAPV_ERR__OK != (err = MapV_Insert(map, key, keyLen, val, false))) {
      MapV_PrintTableCfg(map);
      printf("MapV_Insert failed\n");
      printf("Returned error code:\n\t");
      MapV_PrintErr(err);
      printf("\n");
      exit(1);
    }
  }

  //------------------------------------------------------------
  // Find
  for (uint64_t i = 0; i < valArrCnt; i++)
  {
    const char*  key    = LookupArr[i].strKey;
    const size_t keyLen = strlen(LookupArr[i].strKey);

    MapV_Val_ut val = {0};
    bool ret = MapV_Find(map, key, keyLen, &val);
    if (false == ret) {
			printf("MapV_Find failed\n");
			exit(1);
    }
    Lookup_st* entry = (Lookup_st*)val.ptr;
    entry->fn();
  }

  //---------------------------
  printf("done\n");

  return 0;
}


