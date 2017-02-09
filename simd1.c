#include <ammintrin.h>
#include <smmintrin.h>
#include <immintrin.h>
#include <sys/time.h>


#include <stdio.h>

char p[5] = {1, 1 << 1, 1 << 2, 1 << 3, 1 << 4};

void dump(__m256i val) {
		for (int i = 0; i < 8; i++) {
		printf("%i ", _mm256_extract_epi32(val, i));
	}
	printf("\n");
}

void printBits(size_t const size, void const * const ptr)
{
    unsigned char *b = (unsigned char*) ptr;
    unsigned char byte;
    int i, j;

    for (i=size-1;i>=0;i--)
    {
        for (j=7;j>=0;j--)
        {
            byte = b[i] & (1<<j);
            byte >>= j;
            printf("%c", byte == 0 ? '_' : 'X');
        }
    }
    puts("");
}

__m256i setbit_256(__m256i x,int k){
// constants that will (hopefully) be hoisted out of a loop after inlining  
  __m256i indices = _mm256_set_epi32(224,192,160,128,96,64,32,0);
  __m256i one = _mm256_set1_epi32(-1);
  one = _mm256_srli_epi32(one, 31);    // set1(0x1)


  __m256i kvec = _mm256_set1_epi32(k);  
// if 0<=k<=255 then kvec-indices has exactly one element with a value between 0 and 31
  __m256i shiftcounts = _mm256_sub_epi32(kvec, indices);
  __m256i kbit        = _mm256_sllv_epi32(one, shiftcounts);   // shift counts outside 0..31 shift the bit out of the element
                                                               // kth bit set, all 255 other bits zero.
  return _mm256_or_si256(kbit, x);                             // use _mm256_andnot_si256 to unset the k-th bit
}


#define VALUES_PER_IMPRINT 256

#define VALUE_BITS 32

static struct timeval tm1;

static inline void start()
{
    gettimeofday(&tm1, NULL);
}

static inline void stop()
{
    struct timeval tm2;
    gettimeofday(&tm2, NULL);

    unsigned long long t = 1000 * (tm2.tv_sec - tm1.tv_sec) + (tm2.tv_usec - tm1.tv_usec) / 1000;
    printf("%llu\n", t);
}


int main(int argc,char** argv) {
	int imprint_bits = atoi(argv[2]);
	int n = 256 * atoi(argv[1]); // nicely divisible by 8 and 256

	int* values       = malloc(sizeof(int) * n);
	char* value_ptr   = (char*) values;
	__m256i* limits       = malloc(sizeof(__m256i) * imprint_bits);
	__m256i* imprints = calloc(sizeof(__m256i) * n/VALUES_PER_IMPRINT, 1);

	if (!values || !limits || !imprints) {
		return -1;
	}
	// generate some values
	for (int i = 0; i < n; i++) {
		values[i] = i; // best case for imprints, yes?
	}
	// some even-spaced limits, pre-set SIMD words
	for (int i = 0; i < imprint_bits; i++) {
		limits[i] = _mm256_set1_epi32(i* n/imprint_bits);
	}

	__m256i zero = _mm256_setzero_si256();

	__m256i bitmasks[256];
	for (int i = 0; i < 256; i++) {
		bitmasks[i] = setbit_256(zero, i);
	}

	start();
	for (int chunk = 0; chunk < n/VALUES_PER_IMPRINT; chunk++) {
		for (int chunk2 = 0; chunk2 < 32; chunk2++){
			__m256i values_v    = _mm256_load_si256((__m256i*) value_ptr);
			__m256i result      = _mm256_setzero_si256();

			// doing two limits at once was faster than a single one, but it made no difference between two and eight
			// this is the critical part
			for (int l1 = 0, l2=1; l1 < imprint_bits-2; l1+=2, l2+=2) {
				result = _mm256_add_epi32(result, _mm256_add_epi32(_mm256_cmpgt_epi32(values_v, limits[l1]),_mm256_cmpgt_epi32(values_v, limits[l2])));
			}
			result = _mm256_abs_epi32(result);
			
			// in profiling, this was not performance-critical
			for (int i = 0; i < imprint_bits/VALUE_BITS; i++) {
				imprints[chunk] = _mm256_or_si256(imprints[chunk], bitmasks[_mm256_extract_epi32(result, i)]);
			}
			value_ptr += sizeof(__m256i);
		}
	//	printBits(sizeof(__m256i), &imprints[chunk]);
	}
	stop();
/*
	int range_start = 60;
	int  range_stop = 65;


	__m256i query = zero;
	__m256i query_inner = zero;

	for (int i = range_start; i <= range_stop; i++) {
		query = _mm256_or_si256(query, bitmasks[i]);
	}
	for (int i = range_start+1; i < range_stop; i++) {
		query_inner = _mm256_or_si256(query_inner, bitmasks[i]);
	}

	__m256i query_inner_negation = zero;


printBits(sizeof(__m256i), &query);
printBits(sizeof(__m256i), &query_inner);


	for (int i = 0; i < n/VALUES_PER_IMPRINT; i++) {
		__m256i match = _mm256_and_si256(query, imprints[i]);
		//__m256i match_inner = _mm256_and_si256(query_inner, imprints[i]);
		_mm256_and_si256(query_inner, imprints[i]) // needs to have a 1
		_mm256_andnot_si256(query_inner, imprints[i]) // needs to have no 1

		// special case: inner match, all values are results
		// normal case: check individual values


		// todo: materialize result in memory (make a vector)

		printBits(sizeof(__m256i), &match);
	}
*/
//	
//	printBits(sizeof(__m256i), &imprints[n/VALUES_PER_IMPRINT-1]);

	return 0;
}