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
struct timespec
timer_start();

long
timer_end(struct timespec start_time);

char**
file_to_str_arr(const char* fname, uint64_t* cnt);

uint64_t rngstate[4];
uint64_t randNext(void);
void     randSeed(void);

void printNsWithCommas(uint64_t ns);


//------------------------------------------------------------------------------
int main(int argc, char *argv[])
{
  if (argc < 2 || argc > 3) {
  	printf("FATAL: One test input file is required.");
  	exit(1);
	}
	const char* file_keys = argv[1];

	randSeed();
	uint64_t rand = randNext();

  //---------------------------
	printf("\n--------------------------------\n");
	printf("Running test using key file: %s\n\n", file_keys);

  //---------------------------
  uint64_t valArrCnt = 0;
  uint64_t valLenSum = 0;
  char**   valArr    = file_to_str_arr(file_keys, &valArrCnt);
  size_t*  strLenArr = calloc(valArrCnt, sizeof(size_t));
  for (uint64_t i = 0; i < valArrCnt; i++) {
    strLenArr[i] = strlen(valArr[i]);
    valLenSum   += strlen(valArr[i]);
  }

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

  //---------------------------
  // MapV_PrintTableCfg(map);

  //------------------------------------------------------------
  // Insert
  printf("Inserting : %"PRIu64" keys...", valArrCnt);
  fflush(stdout);
  for (uint64_t i = 0; i < valArrCnt; i++)
  {
  	const char*  key    = valArr[i];
  	const size_t keyLen = strLenArr[i];
	  const MapV_Val_ut val = {
			.u64 = i,
			// .ptr = (const void*)&(LookupArr[i]),
	  };

	  MapV_Err_et err;
    if (MAPV_ERR__OK != (err = MapV_Insert(map, key, keyLen, val, true))) {
      MapV_PrintTableCfg(map);
      printf("MapV_Insert failed\n");
      printf("Returned error code:\n\t");
      MapV_PrintErr(err);
      printf("\n");
      exit(1);
    }
  }

  //---------------------------
  printf("done.\n");

  //---------------------------
  // printf("deleting key: \"that\"\n"); fflush(stdout);
  // MapV_Err_et err;
  // if (MAPV_ERR__OK != (err = MapV_Delete(map, "that", 4))) {
	//   printf("could not delete\n"); fflush(stdout);
  //   MapV_PrintErr(err);
  //   exit(1);
  // }

  //------------------------------------------------------------
  // Find
  int count      = 0;
  int boolCount  = 0;
  int iterations = 100;

  printf("Running %d _Find() iterations on all keys...", iterations);
  fflush(stdout);

  struct timespec vartime = timer_start();
  for (int iter = 0; iter < iterations; ++iter)
  {
    for (uint64_t i = 0; i < valArrCnt; i++)
    {
      const char*  key    = valArr[i];
      const size_t keyLen = strLenArr[i];

	    MapV_Val_ut val = {0};
      bool ret = MapV_Find(map, key, keyLen, &val);
      if (false == ret) {
        // printf("MapV_Find failed: %d: %.*s\n", (int)i, (int)keyLen, key);
        // exit(1);
      } else {
        // printf("FOUND VAL: %"PRIu64"\n", val.u64);
        boolCount++;
	      rand += randNext();
      }

      rand += randNext();
      count++;
    }
  }

  //---------------------------
  long time_elapsed_nanos = timer_end(vartime);

  printf("done.\n\n");
  // MapV_PrintTableCfg(map);

  printf("Time taken (nanoseconds): %ld\n", time_elapsed_nanos);

  double iterPerSec = count * ((double)1000000000 / (double)time_elapsed_nanos);
  printf("Lookups per second      : ");
  printNsWithCommas((uint64_t)iterPerSec);
  printf("\n");

  float keyLenAvg = (float)((float)valLenSum / (float)valArrCnt);
  printf("Average key len         : %.2f\n", keyLenAvg);
  printf("Rand                    : %"PRIu64"\n", rand);

  // subtract iterations since we deleted a key
  // if (boolCount != count - iterations) {
  if (boolCount != count) {
    printf("\ncount and boolCount do not match!!!\n");
	  printf("\tboolCount : %d\n", boolCount);
	  printf("\tcount     : %d\n", count);
  }

  //---------------------------
  printf("\n_Destroy()ing...");fflush(stdout);
  MapV_Err_et err;
  if (MAPV_ERR__OK != (err = MapV_Destroy(map))) {
	  printf("FAILED\n");fflush(stdout);
  } else {
	  printf("ok\n\n");fflush(stdout);
  }

  return 0;
}



//------------------------------------------------------------------------------
struct timespec timer_start() {
  struct timespec start_time;
  clock_gettime(CLOCK_MONOTONIC, &start_time);
  return start_time;
}

long timer_end(struct timespec start_time) {
  struct timespec end_time;
  clock_gettime(CLOCK_MONOTONIC, &end_time);
  long diffInNanos = (end_time.tv_sec - start_time.tv_sec)
                   * (long)1e9
                   + (end_time.tv_nsec - start_time.tv_nsec);
  return diffInNanos;
}

static inline uint64_t rotl(const uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

// Returns a Uint64 random number
uint64_t randNext(void) {
    const uint64_t result = rotl(rngstate[0] + rngstate[3], 23) + rngstate[0];
    const uint64_t t = rngstate[1] << 17;
    rngstate[2] ^= rngstate[0];
    rngstate[3] ^= rngstate[1];
    rngstate[1] ^= rngstate[2];
    rngstate[0] ^= rngstate[3];
    rngstate[2] ^= t;
    rngstate[3] = rotl(rngstate[3], 45);
    return result;
}


// Returns a Uint64 random number
void randSeed(void) {
	rngstate[0] = timer_end((struct timespec){0});
	rngstate[1] = timer_end((struct timespec){1});
	rngstate[2] = timer_end((struct timespec){2});
	rngstate[3] = timer_end((struct timespec){3});
}


//----------------------------------------------------------------------------
void printNsWithCommas(uint64_t ns)
{
	char str[32] = {0};
	char* pos = str + sizeof(str) - 2; // leave a null byte
	int digits = 0;
	while (ns) {
		digits++;
		*pos = (ns%10)+'0';
		pos--;
		ns /= 10;
		if (0 == (digits % 3)) {
			*pos = ',';
			pos--;
		}
	};
	++pos;
	if (*pos == ',') {
		++pos;
	}
	printf("%s", pos);
}


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
