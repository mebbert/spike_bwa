#include <limits.h>
#include <inttypes.h>
#include "bwa.h"
#include "bwamem.h"
#include "bntseq.h"
#include "kstring.h"

/***************************
 * SMEM iterator interface *
 ***************************/

struct __smem_i {
	const bwt_t *bwt;
	const uint8_t *query;
	int start, len;
	int min_intv, max_len;
	uint64_t max_intv;
	bwtintv_v *matches; // matches; to be returned by smem_next()
	bwtintv_v *sub;     // sub-matches inside the longest match; temporary
	bwtintv_v *tmpvec[2]; // temporary arrays
};

smem_i *smem_itr_init(const bwt_t *bwt)
{
	smem_i *itr;
	itr = calloc(1, sizeof(smem_i));
	itr->bwt = bwt;
	itr->tmpvec[0] = calloc(1, sizeof(bwtintv_v));
	itr->tmpvec[1] = calloc(1, sizeof(bwtintv_v));
	itr->matches   = calloc(1, sizeof(bwtintv_v));
	itr->sub       = calloc(1, sizeof(bwtintv_v));
	itr->min_intv = 1;
	itr->max_len  = INT_MAX;
	itr->max_intv = 0;
	return itr;
}

void smem_itr_destroy(smem_i *itr)
{
	free(itr->tmpvec[0]->a); free(itr->tmpvec[0]);
	free(itr->tmpvec[1]->a); free(itr->tmpvec[1]);
	free(itr->matches->a);   free(itr->matches);
	free(itr->sub->a);       free(itr->sub);
	free(itr);
}

void smem_set_query(smem_i *itr, int len, const uint8_t *query)
{
	itr->query = query;
	itr->start = 0;
	itr->len = len;
}

void smem_config(smem_i *itr, int min_intv, int max_len, uint64_t max_intv)
{
	itr->min_intv = min_intv;
	itr->max_len  = max_len;
	itr->max_intv = max_intv;
}

const bwtintv_v *smem_next(smem_i *itr)
{
	int ori_start;
	itr->tmpvec[0]->n = itr->tmpvec[1]->n = itr->matches->n = itr->sub->n = 0;
	if (itr->start >= itr->len || itr->start < 0) return 0;
	while (itr->start < itr->len && itr->query[itr->start] > 3) ++itr->start; // skip ambiguous bases
	if (itr->start == itr->len) return 0;
	ori_start = itr->start;
	itr->start = bwt_smem1a(itr->bwt, itr->len, itr->query, ori_start, itr->min_intv, itr->max_intv, itr->matches, itr->tmpvec); // search for SMEM
	return itr->matches;
}

/***********************
 *** Extra functions ***
 ***********************/

mem_alnreg_v mem_align1(const mem_opt_t *opt, const bwt_t *bwt, const bntseq_t *bns, const uint8_t *pac, int l_seq, const char *seq_)
{ // the difference from mem_align1_core() is that this routine: 1) calls mem_mark_primary_se(); 2) does not modify the input sequence
	extern mem_alnreg_v mem_align1_core(const mem_opt_t *opt, const bwt_t *bwt, const bntseq_t *bns, const uint8_t *pac, int l_seq, char *seq, void *buf);
	extern void mem_mark_primary_se(const mem_opt_t *opt, int n, mem_alnreg_t *a, int64_t id);
	mem_alnreg_v ar;
	char *seq;
	seq = malloc(l_seq);
	memcpy(seq, seq_, l_seq); // makes a copy of seq_
	ar = mem_align1_core(opt, bwt, bns, pac, l_seq, seq, 0);
	mem_mark_primary_se(opt, ar.n, ar.a, lrand48());
	free(seq);
	return ar;
}

void mem_reg2ovlp(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, bseq1_t *s, mem_alnreg_v *a)
{
	int i;
	kstring_t str = {0,0,0};
	for (i = 0; i < a->n; ++i) {
		const mem_alnreg_t *p = &a->a[i];
		int is_rev, rid, qb = p->qb, qe = p->qe;
		int64_t pos, rb = p->rb, re = p->re;
		pos = bns_depos(bns, rb < bns->l_pac? rb : re - 1, &is_rev);
		rid = bns_pos2rid(bns, pos);
		assert(rid == p->rid);
		pos -= bns->anns[rid].offset;
		kputs(s->name, &str); kputc('\t', &str);
		kputw(s->l_seq, &str); kputc('\t', &str);
		if (is_rev) qb ^= qe, qe ^= qb, qb ^= qe; // swap
		kputw(qb, &str); kputc('\t', &str); kputw(qe, &str); kputc('\t', &str);
		kputs(bns->anns[rid].name, &str); kputc('\t', &str);
		kputw(bns->anns[rid].len, &str); kputc('\t', &str);
		kputw(pos, &str); kputc('\t', &str); kputw(pos + (re - rb), &str); kputc('\t', &str);
		ksprintf(&str, "%.3f", (double)p->truesc / opt->a / (qe - qb > re - rb? qe - qb : re - rb));
		kputc('\n', &str);
	}
	s->sam = str.s;
}

static inline int get_pri_idx(double XA_drop_ratio, const mem_alnreg_t *a, int i)
{
	int k = a[i].secondary_all;
	if (k >= 0 && a[i].score >= a[k].score * XA_drop_ratio) return k;
	return -1;
}

// Okay, returning strings is bad, but this has happened a lot elsewhere. If I have time, I need serious code cleanup.
char **mem_gen_alt(const mem_opt_t *opt, const bntseq_t *bns, const uint8_t *pac, const mem_alnreg_v *a, int l_query, const char *query) // ONLY work after mem_mark_primary_se()
{
	int i, k, r, *cnt, tot;
	kstring_t *aln = 0, str = {0,0,0};
	char **XA = 0, *has_alt;

	cnt = calloc(a->n, sizeof(int));
	has_alt = calloc(a->n, 1);
	for (i = 0, tot = 0; i < a->n; ++i) {
		r = get_pri_idx(opt->XA_drop_ratio, a->a, i);
		if (r >= 0) {
			++cnt[r], ++tot;
			if (a->a[i].is_alt) has_alt[r] = 1;
		}
	}
	if (tot == 0) goto end_gen_alt;
	aln = calloc(a->n, sizeof(kstring_t));
	for (i = 0; i < a->n; ++i) {
		mem_aln_t t;
		if ((r = get_pri_idx(opt->XA_drop_ratio, a->a, i)) < 0) continue;
		if (cnt[r] > opt->max_XA_hits_alt || (!has_alt[r] && cnt[r] > opt->max_XA_hits)) continue;

		/* I (Mark Ebbert) am getting the parent, if there is one. Pass the parent too. */
		mem_alnreg_t *parent;
		mem_alnreg_t *p = &a->a[i];
		if(p->secondary >= 0){
			parent = &a->a[p->secondary];
		}
		else{
			parent = 0;
		}

		t = mem_reg2aln(opt, bns, pac, l_query, query, &a->a[i], parent);
		str.l = 0;
		kputs(bns->anns[t.rid].name, &str);
		kputc(',', &str); kputc("+-"[t.is_rev], &str); kputl(t.pos + 1, &str);
		kputc(',', &str);
		for (k = 0; k < t.n_cigar; ++k) {
			kputw(t.cigar[k]>>4, &str);
			kputc("MIDSHN"[t.cigar[k]&0xf], &str);
		}
		kputc(',', &str); kputw(t.NM, &str);




// 		fprintf(stderr, "\n#####\n");
// 		fprintf(stderr, "# t #\n");
// 		fprintf(stderr, "#####\n");
// 
// 		fprintf(stderr, "ref rb - re: %" PRId64 "\n", t.pos);
// 		fprintf(stderr, "alt score: %d\n", t.alt_sc);
// 		fprintf(stderr, "score: %d\n", t.score);
// 		fprintf(stderr, "sub score: %d\n", t.sub);
// 		fprintf(stderr, "is alt: %d\n", t.is_alt);
//
//
//		if(p->secondary >= 0){
//			fprintf(stderr, "\n\n\n\n##########\n");
//			fprintf(stderr, "# Parent #\n");
//			fprintf(stderr, "##########\n");
//	 
//			fprintf(stderr, "parent rb - re: %" PRId64 " - %" PRId64 "\n", parent->rb, parent->re);
//			fprintf(stderr, "alt score: %d\n", parent->alt_sc);
//			fprintf(stderr, "score: %d\n", parent->score);
//			fprintf(stderr, "true score: %d\n", parent->truesc);
//			fprintf(stderr, "sub score: %d\n", parent->sub);
//			fprintf(stderr, "csub score: %d\n", parent->csub);
//			fprintf(stderr, "n sub: %d\n", parent->sub_n);
//			fprintf(stderr, "seedcov: %d\n", parent->seedcov);
//			fprintf(stderr, "secondary: %d\n", parent->secondary);
//			fprintf(stderr, "is alt: %d\n", parent->is_alt);
//			fprintf(stderr, "frac_rep: %f\n", parent->frac_rep);
//		}



		/* I (Mark Ebbert) added this to include the quality score for the secondary alignment.
		 * I also made it calculate quality scores for secondary alignments, instead of making
		 * them 0.
		 */
		kputc(',', &str); kputw(t.mapq, &str);

		kputc(';', &str);
		free(t.cigar);
		kputsn(str.s, str.l, &aln[r]);
	}
	XA = calloc(a->n, sizeof(char*));
	for (k = 0; k < a->n; ++k)
		XA[k] = aln[k].s;

end_gen_alt:
	free(has_alt); free(cnt); free(aln); free(str.s);
	return XA;
}
