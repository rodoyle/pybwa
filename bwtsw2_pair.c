#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bwt.h"
#include "bntseq.h"
#include "bwtsw2.h"
#include "ksw.h"

#define MAX_INS       20000
#define MIN_RATIO     0.8
#define OUTLIER_BOUND 2.0
#define MAX_STDDEV    4.0
#define EXT_STDDEV    4.0

typedef struct {
	int low, high;
	double avg, std;
} bsw2pestat_t;

bsw2pestat_t bsw2_stat(int n, bwtsw2_t **buf)
{
	extern void ks_introsort_uint64_t(size_t n, uint64_t *a);
	int i, k, x, p25, p50, p75, tmp, max_len = 0;
	uint64_t *isize;
	bsw2pestat_t r;

	isize = calloc(n, 8);
	for (i = k = 0; i < n; i += 2) {
		bsw2hit_t *t[2];
		int l;
		if (buf[i] == 0 || buf[i]->n != 1 || buf[i+1]->n != 1) continue; // more than 1 hits
		t[0] = &buf[i]->hits[0]; t[1] = &buf[i+1]->hits[0];
		if (t[0]->G2 > 0.8 * t[0]->G) continue; // the best hit is not good enough
		if (t[1]->G2 > 0.8 * t[1]->G) continue; // the best hit is not good enough
		l = t[0]->k > t[1]->k? t[0]->k - t[1]->k + t[1]->len : t[1]->k - t[0]->k + t[0]->len;
		max_len = max_len > t[0]->end - t[0]->beg? max_len : t[0]->end - t[0]->beg;
		max_len = max_len > t[1]->end - t[1]->beg? max_len : t[1]->end - t[1]->beg;
		isize[k++] = l;
	}
	ks_introsort_uint64_t(k, isize);
	p25 = isize[(int)(.25 * k + .499)];
	p50 = isize[(int)(.50 * k + .499)];
	p75 = isize[(int)(.75 * k + .499)];
	tmp    = (int)(p25 - OUTLIER_BOUND * (p75 - p25) + .499);
	r.low  = tmp > max_len? tmp : max_len;
	r.high = (int)(p75 + OUTLIER_BOUND * (p75 - p25) + .499);
	fprintf(stderr, "[%s] (25, 50, 75) percentile: (%d, %d, %d)\n", __func__, p25, p50, p75);
	fprintf(stderr, "[%s] low and high boundaries for computing mean and std.dev: (%d, %d)\n", __func__, r.low, r.high);
	for (i = x = 0, r.avg = 0; i < k; ++i)
		if (isize[i] >= r.low && isize[i] <= r.high)
			r.avg += isize[i], ++x;
	r.avg /= x;
	for (i = 0, r.std = 0; i < k; ++i)
		if (isize[i] >= r.low && isize[i] <= r.high)
			r.std += (isize[i] - r.avg) * (isize[i] - r.avg);
	r.std = sqrt(r.std / x);
	fprintf(stderr, "[%s] mean and std.dev: (%.2f, %.2f)\n", __func__, r.avg, r.std);
	tmp  = (int)(p25 - 3. * (p75 - p25) + .499);
	r.low  = tmp > max_len? tmp : max_len;
	r.high = (int)(p75 + 3. * (p75 - p25) + .499);
	if (r.low > r.avg - MAX_STDDEV * 4.) r.low = (int)(r.avg - MAX_STDDEV * 4. + .499);
	r.low = tmp > max_len? tmp : max_len;
	if (r.high < r.avg - MAX_STDDEV * 4.) r.high = (int)(r.avg + MAX_STDDEV * 4. + .499);
	fprintf(stderr, "[%s] low and high boundaries for proper pairs: (%d, %d)\n", __func__, r.low, r.high);
	free(isize);
	return r;
}

typedef struct {
	int n_cigar, beg, end, len;
	int64_t pos;
	uint32_t *cigar;
} pairaux_t;

extern unsigned char nst_nt4_table[256];
static int8_t g_mat[25];

void bsw2_pair1(const bsw2opt_t *opt, int64_t l_pac, const uint8_t *pac, const bsw2pestat_t *st, const bsw2hit_t *h, int l_mseq, const char *mseq, bsw2hit_t *a)
{
	extern void seq_reverse(int len, ubyte_t *seq, int is_comp);
	int64_t k, beg, end;
	uint8_t *seq, *ref;
	int i;
	ksw_query_t *q;
	ksw_aux_t aux[2];
	// compute the region start and end
	a->n_seeds = 1; a->flag |= BSW2_FLAG_MATESW; // before calling this routine, *a has been cleared with memset(0); the flag is set with 1<<6/7
	if (h->is_rev == 0) {
		beg = (int64_t)(h->k + st->avg - EXT_STDDEV * st->std - l_mseq + .499);
		end = (int64_t)(h->k + st->avg + EXT_STDDEV * st->std + .499);
		a->is_rev = 1; a->flag |= 16;
	} else {
		beg = (int64_t)(h->k + h->end - h->beg - st->avg - EXT_STDDEV * st->std + .499);
		end = (int64_t)(h->k + h->end - h->beg - st->avg + EXT_STDDEV * st->std + l_mseq + .499);
		a->is_rev = 0;
	}
	if (beg < 1) beg = 1;
	if (end > l_pac) end = l_pac;
	// generate the sequence
	seq = malloc(l_mseq + (end - beg));
	ref = seq + l_mseq;
	for (k = beg; k < end; ++k)
		ref[k - beg] = pac[k>>2] >> ((~k&3)<<1) & 0x3;
	if (h->is_rev == 0) {
		for (i = 0; i < l_mseq; ++i) { // on the reverse strand
			int c = nst_nt4_table[(int)mseq[i]];
			seq[l_mseq - 1 - i] = c > 3? 4 : 3 - c;
		}
	} else {
		for (i = 0; i < l_mseq; ++i) // on the forward strand
			seq[i] = nst_nt4_table[(int)mseq[i]];
	}
	// forward Smith-Waterman
	aux[0].T = opt->t; aux[0].gapo = opt->q; aux[0].gape = opt->r; aux[1] = aux[0];
	q = ksw_qinit(l_mseq * g_mat[0] < 250? 1 : 2, l_mseq, seq, 5, g_mat);
	ksw_sse2(q, end - beg, ref, &aux[0]);
	free(q);
	if (aux[0].score < opt->t) {
		free(seq);
		return;
	}
	++aux[0].qe; ++aux[0].te;
	// reverse Smith-Waterman
	seq_reverse(aux[0].qe, seq, 0);
	seq_reverse(aux[0].te, ref, 0);
	q = ksw_qinit(aux[0].qe * g_mat[0] < 250? 1 : 2, aux[0].qe, seq, 5, g_mat);
	ksw_sse2(q, aux[0].te, ref, &aux[1]);
	free(q);
	++aux[1].qe; ++aux[1].te;
	// write output
	a->G = aux[0].score;
	a->G2 = aux[0].score2 > aux[1].score2? aux[0].score2 : aux[1].score2;
	if (a->G2 < opt->t) a->G2 = 0;
	if (a->G2) a->flag |= BSW2_FLAG_TANDEM;
	a->k = beg + (aux[0].te - aux[1].te);
	a->len = aux[1].te;
	a->beg = aux[0].qe - aux[1].qe;
	a->end = aux[0].qe;
	if (a->is_rev) i = a->beg, a->beg = l_mseq - a->end, a->end = l_mseq - i;
	free(seq);
}

void bsw2_pair(const bsw2opt_t *opt, int64_t l_pac, const uint8_t *pac, int n, bsw2seq1_t *seq, bwtsw2_t **hits)
{
	extern int bsw2_resolve_duphits(const bntseq_t *bns, const bwt_t *bwt, bwtsw2_t *b, int IS);
	bsw2pestat_t pes;
	int i, j, k, n_rescued = 0, n_moved = 0, n_fixed = 0;
	pes = bsw2_stat(n, hits);
	for (i = k = 0; i < 5; ++i) {
		for (j = 0; j < 4; ++j)
			g_mat[k++] = i == j? opt->a : -opt->b;
		g_mat[k++] = 0;
	}
	for (i = 0; i < n; i += 2) {
		bsw2hit_t a[2];
		memset(&a, 0, sizeof(bsw2hit_t) * 2);
		a[0].flag = 1<<6; a[1].flag = 1<<7;
		for (j = 0; j < 2; ++j) { // set the read1/2 flag
			if (hits[i+j] == 0) continue;
			for (k = 0; k < hits[i+j]->n; ++k) {
				bsw2hit_t *p = &hits[i+j]->hits[k];
				p->flag |= 1<<(6+j);
			}
		}
		if (hits[i] == 0 || hits[i+1] == 0) continue; // one end has excessive N
		if (hits[i]->n != 1 && hits[i+1]->n != 1) continue; // no end has exactly one hit
		if (hits[i]->n > 1 || hits[i+1]->n > 1) continue; // one read has more than one hit
		if (hits[i+0]->n == 1) bsw2_pair1(opt, l_pac, pac, &pes, &hits[i+0]->hits[0], seq[i+1].l, seq[i+1].seq, &a[1]);
		if (hits[i+1]->n == 1) bsw2_pair1(opt, l_pac, pac, &pes, &hits[i+1]->hits[0], seq[i+0].l, seq[i+0].seq, &a[0]);
		// the following enumerate all possibilities. It is tedious but necessary...
		if (hits[i]->n + hits[i+1]->n == 1) { // one end mapped; the other not;
			bwtsw2_t *p[2];
			int which;
			if (hits[i]->n == 1) p[0] = hits[i], p[1] = hits[i+1], which = 1;
			else p[0] = hits[i+1], p[1] = hits[i], which = 0;
			if (a[which].G == 0) continue;
			a[which].flag |= BSW2_FLAG_RESCUED;
			if (p[1]->max == 0) {
				p[1]->max = 1;
				p[1]->hits = malloc(sizeof(bsw2hit_t));
			}
			p[1]->hits[0] = a[which];
			p[1]->n = 1;
			p[0]->hits[0].flag |= 2;
			p[1]->hits[0].flag |= 2;
			++n_rescued;
		} else { // then both ends mapped
			int is_fixed = 0;
			//fprintf(stderr, "%d; %lld,%lld; %d,%d\n", a[0].is_rev, hits[i]->hits[0].k, a[0].k, hits[i]->hits[0].end, a[0].end);
			for (j = 0; j < 2; ++j) { // fix wrong mappings and wrong suboptimal alignment score
				bsw2hit_t *p = &hits[i+j]->hits[0];
				if (p->G < a[j].G) { // the orginal mapping is suboptimal
					a[j].G2 = a[j].G2 > p->G? a[j].G2 : p->G; // FIXME: reset BSW2_FLAG_TANDEM?
					*p = a[j];
					++n_fixed;
					is_fixed = 1;
				} else if (p->k != a[j].k && p->G2 < a[j].G) {
					p->G2 = a[j].G;
				} else if (p->k == a[j].k && p->G2 < a[j].G2) {
					p->G2 = a[j].G2;
				}
			}
			if (hits[i]->hits[0].k == a[0].k && hits[i+1]->hits[0].k == a[1].k) { // properly paired and no ends need to be moved
				for (j = 0; j < 2; ++j)
					hits[i+j]->hits[0].flag |= 2 | (a[j].flag & BSW2_FLAG_TANDEM);
			} else if (hits[i]->hits[0].k == a[0].k || hits[i+1]->hits[0].k == a[1].k) { // a tandem match
				for (j = 0; j < 2; ++j) {
					hits[i+j]->hits[0].flag |= 2;
					if (hits[i+j]->hits[0].k != a[j].k)
						hits[i+j]->hits[0].flag |= BSW2_FLAG_TANDEM;
				}
			} else if (!is_fixed && (a[0].G || a[1].G)) { // it is possible to move one end
				if (a[0].G && a[1].G) { // now we have two "proper pairs"
					int G[2];
					double diff;
					G[0] = hits[i]->hits[0].G + a[1].G;
					G[1] = hits[i+1]->hits[0].G + a[0].G;
					diff = fabs(G[0] - G[1]) / (opt->a + opt->b) / ((hits[i]->hits[0].len + a[1].len + hits[i+1]->hits[0].len + a[0].len) / 2.);
					if (diff > 0.05) a[G[0] > G[1]? 0 : 1].G = 0;
				}
				if (a[0].G == 0 || a[1].G == 0) { // one proper pair only
					bsw2hit_t *p[2]; // p[0] points the unchanged hit; p[1] to the hit to be moved
					int which, isize;
					double dev, diff;
					if (a[0].G) p[0] = &hits[i+1]->hits[0], p[1] = &hits[i]->hits[0], which = 0;
					else p[0] = &hits[i]->hits[0], p[1] = &hits[i+1]->hits[0], which = 1;
					isize = p[0]->is_rev? p[0]->k + p[0]->len - a[which].k : a[which].k + a[which].len - p[0]->k;
					dev = fabs(isize - pes.avg) / pes.std;
					diff = (double)(p[1]->G - a[which].G) / (opt->a + opt->b) / (p[1]->end - p[1]->beg) * 100.0;
					if (diff < dev * 2.) { // then move (heuristic)
						a[which].G2 = a[which].G;
						p[1][0] = a[which];
						p[1]->flag |= BSW2_FLAG_MOVED | 2;
						p[0]->flag |= 2;
						++n_moved;
					}
				}
			} else if (is_fixed) {
				hits[i+0]->hits[0].flag |= 2;
				hits[i+1]->hits[0].flag |= 2;
			}
		}
	}
	fprintf(stderr, "[%s] #fixed=%d, #rescued=%d, #moved=%d\n", __func__, n_fixed, n_rescued, n_moved);
}