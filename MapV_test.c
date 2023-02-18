#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "MapV.h"


//==============================================================================
//==============================================================================
//==============================================================================
//==============================================================================
//
// everything below here is for testing
//
//------------------------------------------------------------------------------
const char* INPUT_FILE = "/media/src/c/hashing/hsh.key/_in/00000--google-10000-english.txt";
// const char* INPUT_FILE = "/media/src/c/hashing/hsh.key/_in/00887--urls.12MM.txt";

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

//------------------------------------------------------------------------------
int main()
{
  uint64_t valArrCnt = 0;
  char**   valArr    = file_to_str_arr(INPUT_FILE, &valArrCnt);
  printf("valArrCnt : %"PRIu64"\n\n", valArrCnt);

  size_t* strLenArr = calloc(valArrCnt, sizeof(size_t));
  for (uint64_t i = 0; i < valArrCnt; i++) {
    strLenArr[i] = strlen(valArr[i]);
  }

  //---------------------------
  MapV_st* map = MapV_Create(32, 8, 90, 4096, 10);
  if (NULL == map) {
    printf("MapV_Create failed\n");
    exit(1);
  }

  //---------------------------
  MapV_PrintTableCfg(map);

  //---------------------------
  for (uint64_t i = 0; i < valArrCnt; i++) {
	  // printf("%"PRIu64" ", i); fflush(stdout);

	  MapV_Err_et err;
    if (MAPV_ERR__OK != (err = MapV_Insert(map, valArr[i], strLenArr[i], i, true))) {
      MapV_PrintTableCfg(map);
      printf("MapV_Insert failed\n");
      printf("Returned error code:\n\t");
      MapV_PrintErr(err);
      printf("\n");
      exit(1);
    }
  }

  //---------------------------
  printf("done inserting\n"); fflush(stdout);
  MapV_PrintTableCfg(map);

  //---------------------------
  printf("deleting key: \"that\"\n"); fflush(stdout);
  MapV_Err_et err;
  if (MAPV_ERR__OK != (err = MapV_Delete(map, "that", 4))) {
	  printf("could not delete\n"); fflush(stdout);
    MapV_PrintErr(err);
    exit(1);
  }


  //---------------------------
  int count      = 0;
  int boolCount  = 0;
  int iterations = 1000;

  struct timespec vartime = timer_start();
  for (int iter = 0; iter < iterations; ++iter)
  {
    for (uint64_t i = 0; i < valArrCnt; i++)
    {
      const char*  key    = valArr[i];
      const size_t keyLen = strLenArr[i];

      uint64_t val;
      bool ret = MapV_Find(map, key, keyLen, &val);
      if (false == ret) {
        // printf("MapV_Find failed: %d: %.*s\n", (int)i, (int)keyLen, key);
        // exit(1);
      } else {
        // printf("FOUND VAL: %"PRIu64"\n", val);
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

  if (boolCount != count - iterations) {
    printf("count and boolCount do not match!!!\n");
  }
}
