#include "simd_imprints.h"

unsigned long
simple_scan(Column *column, ValRecord low, ValRecord high, long *timer)
{
	unsigned long i, res_cnt = 0;
	unsigned long colcnt = column->colcount;

	#define simplescan(X,T) {					\
		T  *restrict col = (T *) column->col;	\
		T l = low.X;							\
		T h = high.X;							\
		for (i = 0; i < colcnt; i++) {			\
			if (col[i] > l && col[i] <= h) {	\
				res_cnt++;						\
			}									\
		}										\
	}

	*timer = usec();
	switch(column->coltype){
	case TYPE_bte:
		simplescan(bval, char);
		break;
	case TYPE_sht:
		simplescan(sval, short);
		break;
	case TYPE_int:
		simplescan(ival, int);
		break;
	case TYPE_lng:
		simplescan(lval, long);
		break;
	case TYPE_oid:
		simplescan(ulval, unsigned long);
		break;
	case TYPE_flt:
		simplescan(fval, float);
		break;
	case TYPE_dbl:
		simplescan(dval, double);
		break;
	}
	*timer = usec() - *timer;
	return res_cnt;
}

unsigned long
imprints_scan(Column *column, Imprints_index *imps, ValRecord low, ValRecord high, long *timer)
{
	unsigned long i, res_cnt = 0;
	unsigned long first, last;
	unsigned long colcnt = column->colcount;
	unsigned long dcnt, icnt, top_icnt, bcnt, lim;
	int values_per_block = imps->blocksize/column->typesize;

	dcnt = icnt = bcnt = 0;


	#define impsscan(X,T,_T) {							\
		T  *restrict col = (T *) column->col;			\
		_T *restrict imprints = (_T *) imps->imprints;	\
		T l = low.X;									\
		T h = high.X;									\
		_T mask = 0, innermask = 0;						\
		GETBIT_GENERAL(first, low.X, X);				\
		GETBIT_GENERAL(last, high.X, X);				\
		for (i=first; i <= last; i++)					\
			mask = setBit(mask, i);						\
		for (i=first+1; i < last; i++)					\
			innermask = setBit(innermask, i);			\
		for (i = 0, dcnt = 0; dcnt < imps->dct_cnt; dcnt++) {						\
			if (imps->dct[dcnt].repeated == 0) {									\
				top_icnt = icnt + imps->dct[dcnt].blks;								\
				for (; icnt < top_icnt; bcnt++, icnt++) {							\
					if (imprints[icnt] & mask) {									\
						i = bcnt * values_per_block;								\
						lim = i + values_per_block;									\
						lim = lim > colcnt ? colcnt: lim;							\
						if ((imprints[icnt] & ~innermask) == 0) {					\
							res_cnt += (lim-i);										\
						} else {													\
							for (; i < lim; i++) {									\
								if (col[i] > l && col[i] <= h) {					\
									res_cnt++;										\
								}													\
							}														\
						}															\
					}																\
				}																	\
			} else { /* repeated mask case */										\
				if (imprints[icnt] & mask) {										\
					i = bcnt * values_per_block;									\
					lim = i + values_per_block * imps->dct[dcnt].blks;				\
					lim = lim > colcnt ? colcnt : lim;								\
					if ((imprints[icnt] & ~innermask) == 0) {						\
						res_cnt += (lim -i);								\
					} else {														\
						for (; i < lim; i++) {										\
							if (col[i] > l && col[i] <= h) {						\
								res_cnt++;											\
							}														\
						}															\
					}																\
				}																	\
				bcnt += imps->dct[dcnt].blks;										\
				icnt++;																\
			}																		\
		}																			\
	}

#define COLTYPE_SWITCH(_T)\
	switch(column->coltype){\
	case TYPE_bte:\
		impsscan(bval, char,_T);\
		break;\
	case TYPE_sht:\
		impsscan(sval, short,_T);\
		break;\
	case TYPE_int:\
		impsscan(ival, int,_T);\
		break;\
	case TYPE_lng:\
		impsscan(lval, long,_T);\
		break;\
	case TYPE_oid:\
		impsscan(ulval, unsigned long,_T);\
		break;\
	case TYPE_flt:\
		impsscan(fval, float,_T);\
		break;\
	case TYPE_dbl:\
		impsscan(dval, double,_T);\
		break;\
	}

	*timer = usec();
	switch (imps->imprintsize) {
		case 1: COLTYPE_SWITCH(uint8_t); break;
		case 2: COLTYPE_SWITCH(uint16_t); break;
		case 4: COLTYPE_SWITCH(uint32_t); break;
		case 8: COLTYPE_SWITCH(uint64_t); break;
	}
	*timer = usec() - *timer;
	return res_cnt;
}

unsigned long
imprints_simd_scan(Column *column, Imprints_index *imps, ValRecord low, ValRecord high, long *timer)
{
	unsigned long i, res_cnt = 0;
	unsigned long first = 0, last = 0;
	unsigned long colcnt = column->colcount;
	unsigned long dcnt, icnt, top_icnt, bcnt, lim;
	char          *mask, *innermask;
	int           impssize = imps->imprintsize;
	int           v_idx, *p, e;
	int           values_per_block    = imps->blocksize/column->typesize;
	int           values_per_simd     = 32/column->typesize;
	char *restrict imprints = imps->imprints;
	/* simd stuff */
	__m256i __m256i_low, __m256i_high;

	dcnt = icnt = bcnt = 0;

	p = (int *) malloc(sizeof(int) * values_per_simd);
	for (i = 0; i < values_per_simd; i++) {
		p[i] = 1 << i * column->typesize;
	}

	mask = (char *) aligned_alloc(32, 32);
	innermask = (char *) aligned_alloc(32, 32);
	for (i = 0; i < 32; i++) {
		mask[i] = innermask[i] = 0;
	}

	#define SIMD_QUERYBOUNDS(SIMDTYPE, X)					\
	__m256i_low  = _mm256_set1_##SIMDTYPE(low.X);			\
	__m256i_high = _mm256_set1_##SIMDTYPE(high.X);			\
	GETBIT_GENERAL(first, low.X, X);						\
	GETBIT_GENERAL(last, high.X, X);

	switch (column->coltype) {
		case TYPE_bte: SIMD_QUERYBOUNDS(epi8, bval); break;
		case TYPE_sht: SIMD_QUERYBOUNDS(epi16, sval); break;
		case TYPE_int: SIMD_QUERYBOUNDS(epi32, ival); break;
		case TYPE_lng: SIMD_QUERYBOUNDS(epi64x, lval); break;
		case TYPE_oid: SIMD_QUERYBOUNDS(epi64x, ulval); break;
		case TYPE_flt: SIMD_QUERYBOUNDS(ps, fval); break;
		case TYPE_dbl: SIMD_QUERYBOUNDS(pd, dval); break;
		default: break;
	}


	for (i = first; i <= last; i++)
		mask[i/8] = setBit(mask[i/8], i%8);
	for (i = first + 1; i < last; i++)
		innermask[i/8] = setBit(innermask[i/8], i%8);
	for (i = 0; i < imps->imprintsize; i++) {
		innermask[i] = ~innermask[i];
	}

	#define simd_impsscan(X,T,SIMDTYPE) {										\
		T  *restrict col = (T *) column->col;										\
		__m256i simd_mask = _mm256_load_si256((__m256i*) mask);						\
		__m256i simd_innermask = _mm256_load_si256((__m256i*) innermask);			\
		__m256i current_imprint;													\
		for (i = 0, dcnt = 0; dcnt < imps->dct_cnt; dcnt++) {						\
			if (imps->dct[dcnt].repeated == 0) {									\
				top_icnt = icnt + imps->dct[dcnt].blks;								\
				for (; icnt < top_icnt; bcnt++, icnt++) {							\
					current_imprint = _mm256_loadu_si256((__m256i *) (imprints+(icnt*impssize)));\
					if (_mm256_testz_si256(simd_mask, current_imprint) == 0) {		\
						i = bcnt * values_per_block;								\
						lim = i + values_per_block;									\
						lim = lim > colcnt ? colcnt: lim;							\
						if (_mm256_testz_si256(simd_innermask, current_imprint)) {	\
							res_cnt += (lim-i);										\
						} else {													\
							for (; i < lim; i+=values_per_simd) {							\
								__m256i values_v = _mm256_load_si256((__m256i*) (col+i));	\
								v_idx = _mm256_movemask_epi8(								\
								_mm256_sub_##SIMDTYPE(										\
									_mm256_cmpgt_##SIMDTYPE(values_v, __m256i_low),			\
									_mm256_cmpgt_##SIMDTYPE(values_v, __m256i_high)));		\
								for (e = 0; e < values_per_simd; e++) {						\
									res_cnt += ((v_idx & p[e]) != 0);						\
								}															\
							}																\
						}															\
					}																\
				}																	\
			} else { /* repeated mask case */										\
				current_imprint = _mm256_loadu_si256((__m256i *) (imprints+(icnt*impssize)));	\
				if (_mm256_testz_si256(simd_mask, current_imprint) == 0) {			\
					i = bcnt * values_per_block;									\
					lim = i + values_per_block * imps->dct[dcnt].blks;				\
					lim = lim > colcnt ? colcnt : lim;								\
					if (_mm256_testz_si256(simd_innermask, current_imprint)) {		\
						res_cnt += (lim -i);										\
					} else {														\
						for (; i < lim; i+=values_per_simd) {								\
							__m256i values_v = _mm256_load_si256((__m256i*) (col+i));		\
							v_idx = _mm256_movemask_epi8(									\
							_mm256_sub_##SIMDTYPE(											\
								_mm256_cmpgt_##SIMDTYPE(values_v, __m256i_low),				\
								_mm256_cmpgt_##SIMDTYPE(values_v, __m256i_high)));			\
							for (int e = 0; e < values_per_simd; e++) {						\
								res_cnt += ((v_idx & p[e]) != 0);							\
							}																\
						}																	\
					}																\
				}																	\
				bcnt += imps->dct[dcnt].blks;										\
				icnt++;																\
			}																		\
		}																			\
	}

	*timer = usec();
	switch(column->coltype){
	case TYPE_bte:
		simd_impsscan(bval, char,epi8);
		break;
	case TYPE_sht:
		simd_impsscan(sval, short,epi16);
		break;
	case TYPE_int:
		simd_impsscan(ival, int,epi32);
		break;
	case TYPE_lng:
		simd_impsscan(lval, long,epi64);
		break;
	case TYPE_oid:
		simd_impsscan(ulval, unsigned long,epi64);
		break;
	case TYPE_flt:
		break;
	case TYPE_dbl:
		break;
	}
	*timer = usec() - *timer;
	return res_cnt;
}

unsigned long
zonemaps_scan(Column *column, Zonemap_index *zmaps, ValRecord low, ValRecord high, long *timer)
{
	unsigned long i, lim, k, res_cnt = 0;

	#define zonequery(X,T) \
		for (i = 0; i < zmaps->zmaps_cnt; i++) {		\
			if (low.X < zmaps->zmaps[i].min.X && high.X >= zmaps->zmaps[i].max.X) { /* all qualify */			\
					for (k = i * zmaps->zonesize, lim = k + zmaps->zonesize < column->colcount ? k+zmaps->zonesize : column->colcount; k < lim; k++) {	\
						res_cnt++;															\
					}																				\
				} else if (!(high.X < zmaps->zmaps[i].min.X || low.X >= zmaps->zmaps[i].max.X)) {					\
					/* zone maps are inclusive */													\
					for (k = i * zmaps->zonesize, lim = k + zmaps->zonesize < column->colcount ? k+zmaps->zonesize : column->colcount; k < lim; k++) {	\
						if (((T*)column->col)[k] <= high.X && ((T*)column->col)[k] > low.X ) {					\
							res_cnt++;														\
						}\
					} \
				}\
			}

	*timer = usec();
	switch(column->coltype){
	case TYPE_bte:
		zonequery(bval,char);
		break;
	case TYPE_sht:
		zonequery(sval,short);
		break;
	case TYPE_int:
		zonequery(ival,int);
		break;
	case TYPE_lng:
		zonequery(lval,long);
		break;
	case TYPE_oid:
		zonequery(ulval,unsigned long);
		break;
	case TYPE_flt:
		zonequery(fval,float);
		break;
	case TYPE_dbl:
		zonequery(dval,double);
	}

	*timer = usec() - *timer;
	return res_cnt;
}
