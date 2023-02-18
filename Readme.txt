
Now that _Delete() and _Destroy() have been created,
this is at an alpha(?) level of completion.

Additional work was done for:
 - return codes where bool was insufficient
 - splitting into a .h and .c file
 - splitting the test code into its own .c file
 - initial makefile

There is still some work to be done in terns of:
 - creating tests and benchmarks
 - improving the makefile
 - turning it into a proper library structure

But I figured I'd publish it at this stage.


--------------------------------------------------------------------------------
performance:

    10k short keys, 1k iterations

        https://github.com/martinus/unordered_dense
        lookups per second : 45,724,225.09
        lookups per second : 45,586,745.62
        lookups per second : 46,176,749.84
        lookups per second : 45,053,718.42
        lookups per second : 47,111,701.40
        lookups per second : 43,749,540.65
        lookups per second : 43,614,950.53
        lookups per second : 46,957,599.19
        lookups per second : 46,584,399.40
        lookups per second : 48,132,160.20
        average: 45,869,179.03

        MapV:
        lookups per second : 55,155,926.96
        lookups per second : 55,046,138.41
        lookups per second : 58,203,247.47
        lookups per second : 61,124,753.78
        lookups per second : 58,144,229.02
        lookups per second : 59,746,045.25
        lookups per second : 58,143,708.05
        lookups per second : 62,624,697.12
        lookups per second : 57,273,327.09
        lookups per second : 54,008,191.17
        average: 57,947,026.43

    12MM urls, 10 iterations

        ankerl:
        lookups per second : 5,398,034.24

        MapV:
        lookups per second : 8,428,561.08


--------------------------------------------------------------------------------
@Requirements

  - CPU supporting AVX2
  - XXHash3


--------------------------------------------------------------------------------
@TODO: features/functionality

	- creating tests and benchmarks
	- improving the makefile
	- turning it into a proper library structure
	- cli testing, passing in a file for keys
	- test performance
	  - check for empty slots on lookup; allow to return faster on no key
	  - generic c? c++ template?


--------------------------------------------------------------------------------
@DOCUMENTATION / readme

	- probabilistic nature of 128-bit keys and dropping original keys
	  - benefits : memory access
	  - downsides: possible faslse positives
	- slotId shift from top of hash means rebuilding table maintains order
	  - ie: early-loaded keys are found faster, regardless of rebuilds
	- bucket structure = cache locality
	  - 8 byte values stored
	- bucket/slotId/entry terminology
	- configuration options
	- distance (probe sequence length) is unsigned.
	  - means that all compares must compare high to low to avoid branches
	  - this is implemented, but worth noting.
	- deleting does not adjust the number of buckets that must be scanned
	  in order to do this, the entire table would have to be scanned on delete.
	  a side effect of this is that a table will not shrink.
	  it would be possible to create a function like __Maybe_Shrink().
	  but my guess is that the use case for this is so rare that i won't be
	  implementing it


--------------------------------------------------------------------------------
@NOTES:

  this is technically a probabilistic data structure, as the original
  key is not compared. this is "okay" because 128 bits of a very high
  quality hash is used for key lookups. there are multiple benefits:
  primarily, cache locality of all lookup data.
  secondarily, reduced memory usage from not storing original keys.
               this is particularly useful with long keys (urls)

  i'll be creating another variation that does use original key
  comparisons, and will be able to use 64 bit hashes vs 128.
  this will double the speed up hash lookups, but involves another
  random memory access. so we'll see how it does.

  it's also worth noting that this uses 256-bit vector instructions.
  avx512 would double the speed of lookups, and modifying the code
  to handled that would be trivial. i'll have to see if i have access
  to a cpu that supports avx512


--------------------------------------------------------------------------------
@NOTE on SIMD for my future self:

	apparently an equality comparison of certain floating point double values,
	even though the bits are exactly the same, does not always return true with
	certain vector instructions. eg: _mm256_movemask_pd()

	the same 10 terms, out of 10,000 in my test set, were failing on lookups,
	even though the data was right there.

	that was a very long several hours. i honestly can't make sense of what was
	happening; whether it's a compiler/cpu bug, or if there's something i just
	don't understand about vector instructions (very possible).

	anyway...the hash table is now properly resizing when hitting certain
	thresholds. once deletes are done, it'll be a suitable alpha.


--------------------------------------------------------------------------------
@FUTURE: i'm on the fence about these, but leaving the notes

	- variants
	  - 16/32-bit data stored (eg: array ids, types, etc)
	    - would require 2x/4x bucket size for 32-byte alignment
	  - 64-bit: save keys and compare vs 128-bit probabilistic
	  - avx512
	    - 256-bit hashes?
	  - static tables
	    - post-hash optimizer for compaction
	    - would require table sizes != power of 2


