/*******************************************************************************
 * src/parallel/bingmann-parallel_sample_sort.hpp
 *
 * Parallel Super Scalar String Sample-Sort, many variant via different
 * Classifier templates.
 *
 *******************************************************************************
 * Copyright (C) 2013-2017 Timo Bingmann <tb@panthema.net>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#ifndef PSS_SRC_PARALLEL_BINGMANN_PARALLEL_SAMPLE_SORT_HEADER
#define PSS_SRC_PARALLEL_BINGMANN_PARALLEL_SAMPLE_SORT_HEADER

#include <cstdlib>
#include <cstring>
#include <cmath>

#include <iostream>
#include <vector>
#include <algorithm>

#include "../tools/lcgrandom.hpp"
#include "../tools/stringtools.hpp"
#include "../tools/jobqueue.hpp"
#include "../tools/lockfree.hpp"

#include "../sequential/inssort.hpp"
#include "../sequential/bingmann-lcp_inssort.hpp"
#include "../sequential/bingmann-sample_sort_tree_builder.hpp"
#include "../sequential/bingmann-sample_sortBSC.hpp"
#include "../sequential/bingmann-sample_sortBTC.hpp"
#include "../sequential/bingmann-sample_sortBTCE.hpp"
#include "../sequential/bingmann-sample_sortBTCT.hpp"
#include "../../../indexed_string.hpp"

#include <tlx/string/hexdump.hpp>
#include <tlx/die.hpp>

namespace bingmann_parallel_sample_sort {

using namespace stringtools;
using namespace parallel_string_sorting;
using namespace jobqueue;

static const bool debug_steps = false;
static const bool debug_jobs = false;

static const bool debug_splitter = false;
static const bool debug_bucketsize = false;
static const bool debug_recursion = false;
static const bool debug_splitter_tree = false;
static const bool debug_lcp = false;

//! enable work freeing
static const bool use_work_sharing = true;

//! enable/disable various sorting levels
static const bool enable_parallel_sample_sort = true;
static const bool enable_sequential_sample_sort = true;
static const bool enable_sequential_mkqs = true;

//! first MKQS lcp variant: keep min/max during ternary split, second: keep
//! pivot and calculate later
#define PS5_CALC_LCP_MKQS 1

//! whether the base sequential_threshold() on the remaining unsorted string
//! set or on the whole string set.
#define PS5_ENABLE_RESTSIZE false

//! use LCP insertion sort for non-LCP pS5 ?
static const bool use_lcp_inssort = false;

//! terminate sort after first parallel sample sort step
#ifndef PS5_SINGLE_STEP
#define PS5_SINGLE_STEP false
#endif
static const bool use_only_first_sortstep = PS5_SINGLE_STEP;

//! maximum number of threads, used in a few static arrays
static const size_t MAXPROCS = 2 * 64 + 1; // +1 due to round up of processor number

//! L2 cache size, used to calculate classifier tree sizes
#ifndef PS5_L2CACHE
#define PS5_L2CACHE     256 * 1024
#endif
static const size_t l2cache = PS5_L2CACHE;

static const size_t g_smallsort_threshold = 1024 * 1024;
static const size_t g_inssort_threshold = 32;

typedef uint64_t key_type;

// ****************************************************************************
// *** Global Parallel Super Scalar String Sample Sort Context

template <bool CalcLcp_,
          template <typename> class JobQueueGroupType = DefaultJobQueueGroup>
class Context
{
public:
    //! total size of input
    size_t totalsize;

#if PS5_ENABLE_RESTSIZE
    //! number of remaining strings to sort
    lockfree::lazy_counter<MAXPROCS> restsize;
#endif

    static const bool CalcLcp = CalcLcp_;

    //! number of threads overall
    size_t threadnum;

    //! counters
    size_t para_ss_steps, seq_ss_steps, bs_steps;

    //! type of job queue group (usually a No-Op Class)
    typedef JobQueueGroupType<Context> jobqueuegroup_type;

    //! type of job queue
    typedef typename jobqueuegroup_type::jobqueue_type jobqueue_type;

    //! typedef of compatible job type
    typedef typename jobqueue_type::job_type job_type;

    //! job queue
    jobqueue_type jobqueue;

    //! context constructor
    Context(jobqueuegroup_type* jqg = NULL)
        : para_ss_steps(0), seq_ss_steps(0), bs_steps(0),
          jobqueue(*this, jqg)
    { }

    //! return sequential sorting threshold
    size_t sequential_threshold()
    {
#if PS5_ENABLE_RESTSIZE
        size_t wholesize = restsize.update().get();
#else
        size_t wholesize = totalsize;
#endif
        return std::max(g_smallsort_threshold, wholesize / threadnum);
    }

    //! decrement number of unordered strings
    void donesize(size_t n, size_t tid)
    {
#if PS5_ENABLE_RESTSIZE
        restsize.add(-n, tid);
#else
        (void)n;
        (void)tid;
#endif
    }
};

// ****************************************************************************
// *** SortStep to Keep Track of Substeps

class SortStep
{
private:
    //! Number of substeps still running
    std::atomic<size_t> m_substep_working;

    //! Pure virtual function called by substep when all substeps are done.
    virtual void substep_all_done() = 0;

protected:
    SortStep() : m_substep_working(0) { }

    virtual ~SortStep()
    {
        assert(m_substep_working == 0);
    }

    //! Register new substep
    void substep_add()
    {
        ++m_substep_working;
    }

public:
    //! Notify superstep that the currently substep is done.
    void substep_notify_done()
    {
        assert(m_substep_working > 0);
        if (--m_substep_working == 0)
            substep_all_done();
    }
};

// ****************************************************************************
// *** Classification Variants

static inline unsigned char
lcpKeyType(const key_type& a, const key_type& b)
{
    // XOR both values and count the number of zero bytes
    return count_high_zero_bits(a ^ b) / 8;
}

static inline unsigned char
lcpKeyDepth(const key_type& a)
{
    // count number of non-zero bytes
    return sizeof(key_type) - (count_low_zero_bits(a) / 8);
}

//! return the d-th character in the (swapped) key
static inline unsigned char
getCharAtDepth(const key_type& a, unsigned char d)
{
    return static_cast<unsigned char>(a >> (8 * (sizeof(key_type) - 1 - d)));
}

// ****************************************************************************
// *** Insertion Sort Type-Switch

template <typename StringSet>
static inline void
insertion_sort(const stringtools::StringShadowPtr<StringSet>& strptr,
               size_t depth)
{
    assert(!strptr.flipped());

    if (!use_lcp_inssort)
        inssort::inssort_generic(strptr.output(), depth);
    else
        bingmann::lcp_insertion_sort_nolcp(strptr.output(), depth);
}

template <typename StringSet>
static inline void
insertion_sort(const stringtools::StringShadowLcpPtr<StringSet>& strptr,
               size_t depth)
{
    assert(!strptr.flipped());

    bingmann::lcp_insertion_sort</* SaveCache */ false, StringSet>(
        strptr.output(), strptr.lcparray(), nullptr, depth);
}

template <typename StringSet>
static inline void
insertion_sort(const stringtools::StringShadowOutPtr<StringSet>& strptr,
               size_t depth)
{
    assert(!strptr.flipped());

    if (!use_lcp_inssort)
        inssort::inssort_generic(strptr.output(), depth);
    else
        bingmann::lcp_insertion_sort_nolcp(strptr.output(), depth);
}

template <typename StringSet>
static inline void
insertion_sort(const stringtools::StringShadowLcpOutPtr<StringSet>& strptr,
               size_t depth)
{
    assert(!strptr.flipped());

    bingmann::lcp_insertion_sort(
        strptr.output(), strptr.lcparray(), depth);
}

template <typename StringSet>
static inline void
insertion_sort(const stringtools::StringShadowLcpCacheOutPtr<StringSet>& strptr,
               size_t depth)
{
    assert(!strptr.flipped());

    bingmann::lcp_insertion_sort</* SaveCache */ true>(
        strptr.output(), strptr.lcparray(), strptr.cache(), depth);
}

// ****************************************************************************
// *** LCP Calculation for finished Sample Sort Steps

template <size_t bktnum, typename Classify, typename StringPtr, typename BktSizeType>
void sample_sort_lcp(const Classify& classifier,
                     const StringPtr& strptr, size_t depth,
                     const BktSizeType* bkt)
{
    assert(!strptr.flipped());
    assert(strptr.check());

    const typename StringPtr::StringSet& strset = strptr.output();

    size_t b = 0;         // current bucket number
    key_type prevkey = 0; // previous key

    // the following while loops only check b < bktnum when b is odd,
    // because bktnum is always odd. We need a goto to jump into the loop,
    // as b == 0 start even.
    goto even_first;

    // find first non-empty bucket
    while (b < bktnum)
    {
        // odd bucket: = bkt
        if (bkt[b] != bkt[b + 1])
        {
            prevkey = classifier.get_splitter(b / 2);
            assert(prevkey == strset.get_uint64(strset.at(bkt[b + 1] - 1), depth));
            break;
        }
        ++b;
even_first:
        // even bucket: <, << or > bkt
        if (bkt[b] != bkt[b + 1])
        {
            prevkey = strset.get_uint64(strset.at(bkt[b + 1] - 1), depth);
            break;
        }
        ++b;
    }
    ++b;

    // goto depends on whether the first non-empty bucket was odd or
    // even. the while loop below encodes this in the program counter.
    if (b < bktnum && b % 2 == 0) goto even_bucket;

    // find next non-empty bucket
    while (b < bktnum)
    {
        // odd bucket: = bkt
        if (bkt[b] != bkt[b + 1])
        {
            key_type thiskey = classifier.get_splitter(b / 2);
            assert(thiskey == strset.get_uint64(strset.at(bkt[b]), depth));

            int rlcp = lcpKeyType(prevkey, thiskey);
            strptr.set_lcp(bkt[b], depth + rlcp);
            strptr.set_cache(bkt[b], getCharAtDepth(thiskey, rlcp));

            prevkey = thiskey;
            assert(prevkey == strset.get_uint64(strset.at(bkt[b + 1] - 1), depth));
        }
        ++b;
even_bucket:
        // even bucket: <, << or > bkt
        if (bkt[b] != bkt[b + 1])
        {
            key_type thiskey = strset.get_uint64(strset.at(bkt[b]), depth);

            int rlcp = lcpKeyType(prevkey, thiskey);
            strptr.set_lcp(bkt[b], depth + rlcp);
            strptr.set_cache(bkt[b], getCharAtDepth(thiskey, rlcp));

            prevkey = strset.get_uint64(strset.at(bkt[b + 1] - 1), depth);
        }
        ++b;
    }
}

// ****************************************************************************
// *** SampleSort non-recursive in-place sequential sample sort for small sorts

template <template <size_t> class Classify, typename Context, typename StringPtr>
void Enqueue(Context& ctx, SortStep* sstep,
             const StringPtr& strptr, size_t depth);

template <typename Context, template <size_t> class Classify,
          typename StringPtr, typename BktSizeType>
class SmallsortJob : public Context::job_type, public SortStep
{
public:
    //! parent sort step
    SortStep* pstep;

    size_t thrid;

    StringPtr in_strptr;
    size_t in_depth;

    typedef typename StringPtr::StringSet StringSet;

    typedef BktSizeType bktsize_type;

    SmallsortJob(SortStep* pstep,
                 const StringPtr& strptr, size_t depth)
        : pstep(pstep), in_strptr(strptr), in_depth(depth)
    { }

    class SeqSampleSortStep
    {
    public:
        StringPtr strptr;
        size_t idx;
        size_t depth;

        Classify<bingmann_sample_sort::DefaultTreebits> classifier;

        static const size_t numsplitters =
            Classify<bingmann_sample_sort::DefaultTreebits>::numsplitters;
        static const size_t bktnum = 2 * numsplitters + 1;

        unsigned char splitter_lcp[numsplitters + 1];
        bktsize_type bkt[bktnum + 1];

        SeqSampleSortStep(Context& ctx, const StringPtr& _strptr, size_t _depth,
                          uint16_t* bktcache)
            : strptr(_strptr), idx(0), depth(_depth)
        {
            size_t n = strptr.size();

            // step 1: select splitters with oversampling

            const size_t oversample_factor = 2;
            const size_t samplesize = oversample_factor * numsplitters;

            key_type samples[samplesize];

            const StringSet& strset = strptr.active();
            typename StringSet::Iterator begin = strset.begin();

            LCGRandom rng(&samples);

            for (size_t i = 0; i < samplesize; ++i)
                samples[i] = strset.get_uint64(strset[begin + rng() % n], depth);

            std::sort(samples, samples + samplesize);

            classifier.build(samples, samplesize, splitter_lcp);

            // step 2: classify all strings

            classifier.classify(
                strset, strset.begin(), strset.end(), bktcache, depth);

            // step 2.5: count bucket sizes

            bktsize_type bktsize[bktnum];
            memset(bktsize, 0, bktnum * sizeof(bktsize_type));

            for (size_t si = 0; si < n; ++si)
                ++bktsize[bktcache[si]];

            // step 3: inclusive prefix sum

            bkt[0] = bktsize[0];
            for (unsigned int i = 1; i < bktnum; ++i) {
                bkt[i] = bkt[i - 1] + bktsize[i];
            }
            assert(bkt[bktnum - 1] == n);
            bkt[bktnum] = n;

            // step 4: premute out-of-place

            const StringSet& strB = strptr.active();
            // get alternative shadow pointer array
            const StringSet& sorted = strptr.shadow();
            typename StringSet::Iterator sbegin = sorted.begin();

            for (typename StringSet::Iterator str = strB.begin();
                 str != strB.end(); ++str, ++bktcache)
                *(sbegin + --bkt[*bktcache]) = std::move(*str);

            // bkt is afterwards the exclusive prefix sum of bktsize

            // statistics

            ++ctx.seq_ss_steps;
        }

        void calculate_lcp()
        {
            if (Context::CalcLcp)
                sample_sort_lcp<bktnum>(classifier, strptr.original(), depth, bkt);
        }
    };

    // *** Stack of Recursive Sample Sort Steps

    uint16_t* bktcache;
    size_t bktcache_size;

    size_t ss_pop_front;
    std::vector<SeqSampleSortStep> ss_stack;

    bool run(Context& ctx) final
    {
        size_t n = in_strptr.size();

        thrid = PS5_ENABLE_RESTSIZE ? omp_get_thread_num() : 0;

        // create anonymous wrapper job
        this->substep_add();

        bktcache = NULL;
        bktcache_size = 0;
        ss_pop_front = 0;
        ms_pop_front = 0;

        if (enable_sequential_sample_sort && n >= g_smallsort_threshold)
        {
            bktcache = new uint16_t[n];
            bktcache_size = n * sizeof(uint16_t);
            sort_sample_sort(ctx, in_strptr, in_depth);
        }
        else
        {
            sort_mkqs_cache(ctx, in_strptr, in_depth);
        }

        delete[] bktcache;

        // finish wrapper job, handler delete's this
        this->substep_notify_done();

        return false;
    }

    void sort_sample_sort(Context& ctx, const StringPtr& strptr, size_t depth)
    {
        typedef SeqSampleSortStep Step;

        assert(ss_pop_front == 0);
        assert(ss_stack.size() == 0);

        // sort first level
        ss_stack.emplace_back(ctx, strptr, depth, bktcache);

        // step 5: "recursion"

        while (ss_stack.size() > ss_pop_front)
        {
            Step& s = ss_stack.back();
            size_t i = s.idx++; // process the bucket s.idx

            if (i < Step::bktnum)
            {
                size_t bktsize = s.bkt[i + 1] - s.bkt[i];

                StringPtr sp = s.strptr.flip(s.bkt[i], bktsize);

                // i is even -> bkt[i] is less-than bucket
                if (i % 2 == 0)
                {
                    if (bktsize == 0)
                        ;
                    else if (bktsize < g_smallsort_threshold)
                    {
                        assert(i / 2 <= Step::numsplitters);

                        sort_mkqs_cache(
                            ctx, sp, s.depth + (s.splitter_lcp[i / 2] & 0x7F));
                    }
                    else
                    {
                        ss_stack.emplace_back(
                            ctx, sp, s.depth + (s.splitter_lcp[i / 2] & 0x7F), bktcache);
                    }
                }
                // i is odd -> bkt[i] is equal bucket
                else
                {
                    if (bktsize == 0)
                        ;
                    else if (s.splitter_lcp[i / 2] & 0x80) {
                        // equal-bucket has NULL-terminated key, done.
                        StringPtr spb = sp.copy_back();

                        if (Context::CalcLcp)
                            spb.fill_lcp(
                                s.depth + lcpKeyDepth(s.classifier.get_splitter(i / 2)));
                        ctx.donesize(bktsize, thrid);
                    }
                    else if (bktsize < g_smallsort_threshold)
                    {
                        sort_mkqs_cache(ctx, sp, s.depth + sizeof(key_type));
                    }
                    else
                    {
                        ss_stack.emplace_back(
                            ctx, sp, s.depth + sizeof(key_type), bktcache);
                    }
                }
            }
            else
            {
                // finished sort
                assert(ss_stack.size() > ss_pop_front);

                // after full sort: calculate LCPs at this level
                ss_stack.back().calculate_lcp();

                ss_stack.pop_back();
            }

            if (use_work_sharing && ctx.jobqueue.has_idle()) {
                sample_sort_free_work(ctx);
            }
        }
    }

    void sample_sort_free_work(Context& ctx)
    {
        assert(ss_stack.size() >= ss_pop_front);

        if (ss_stack.size() == ss_pop_front) {
            // ss_stack is empty, check other stack
            return mkqs_free_work(ctx);
        }

        // convert top level of stack into independent jobs
        typedef SeqSampleSortStep Step;
        Step& s = ss_stack[ss_pop_front];

        while (s.idx < Step::bktnum)
        {
            size_t i = s.idx++; // process the bucket s.idx

            size_t bktsize = s.bkt[i + 1] - s.bkt[i];

            StringPtr sp = s.strptr.flip(s.bkt[i], bktsize);

            // i is even -> bkt[i] is less-than bucket
            if (i % 2 == 0)
            {
                if (bktsize == 0)
                    ;
                else
                {
                    this->substep_add();
                    Enqueue<Classify>(ctx, this, sp,
                                      s.depth + (s.splitter_lcp[i / 2] & 0x7F));
                }
            }
            // i is odd -> bkt[i] is equal bucket
            else
            {
                if (bktsize == 0)
                    ;
                else if (s.splitter_lcp[i / 2] & 0x80) {
                    // equal-bucket has NULL-terminated key, done.
                    StringPtr spb = sp.copy_back();

                    if (Context::CalcLcp)
                        spb.fill_lcp(s.depth + lcpKeyDepth(s.classifier.get_splitter(i / 2)));
                    ctx.donesize(bktsize, thrid);
                }
                else
                {
                    this->substep_add();
                    Enqueue<Classify>(
                        ctx, this, sp, s.depth + sizeof(key_type));
                }
            }
        }

        // shorten the current stack
        ++ss_pop_front;
    }

    // *************************************************************************
    // *** Stack of Recursive MKQS Steps

    static inline int cmp(const key_type& a, const key_type& b)
    {
        return (a > b) ? 1 : (a < b) ? -1 : 0;
    }

    template <typename Type>
    static inline size_t
    med3(Type* A, size_t i, size_t j, size_t k)
    {
        if (A[i] == A[j]) return i;
        if (A[k] == A[i] || A[k] == A[j]) return k;
        if (A[i] < A[j]) {
            if (A[j] < A[k]) return j;
            if (A[i] < A[k]) return k;
            return i;
        }
        else {
            if (A[j] > A[k]) return j;
            if (A[i] < A[k]) return i;
            return k;
        }
    }

    // Insertion sort the strings only based on the cached characters.
    static inline void
    insertion_sort_cache_block(const StringPtr& strptr, key_type* cache)
    {
        const StringSet& strings = strptr.output();
        size_t n = strptr.size();
        size_t pi, pj;
        for (pi = 1; --n > 0; ++pi) {
            typename StringSet::String tmps = std::move(strings.at(pi));
            key_type tmpc = cache[pi];
            for (pj = pi; pj > 0; --pj) {
                if (cache[pj - 1] <= tmpc)
                    break;
                strings.at(pj) = std::move(strings.at(pj - 1));
                cache[pj] = cache[pj - 1];
            }
            strings.at(pj) = std::move(tmps);
            cache[pj] = tmpc;
        }
    }

    // Insertion sort, but use cached characters if possible.
    template <bool CacheDirty>
    static inline void
    insertion_sort_cache(const StringPtr& _strptr, key_type* cache, size_t depth)
    {
        StringPtr strptr = _strptr.copy_back();

        if (strptr.size() <= 1) return;
        if (CacheDirty) return insertion_sort(strptr, depth);

        insertion_sort_cache_block(strptr, cache);

        size_t start = 0, bktsize = 1;
        for (size_t i = 0; i < strptr.size() - 1; ++i) {
            // group areas with equal cache values
            if (cache[i] == cache[i + 1]) {
                ++bktsize;
                continue;
            }
            // calculate LCP between group areas
            if (start != 0) {
                int rlcp = lcpKeyType(cache[start - 1], cache[start]);
                strptr.set_lcp(start, depth + rlcp);
                strptr.set_cache(start, getCharAtDepth(cache[start], rlcp));
            }
            // sort group areas deeper if needed
            if (bktsize > 1) {
                if (cache[start] & 0xFF) {
                    // need deeper sort
                    insertion_sort(
                        strptr.sub(start, bktsize), depth + sizeof(key_type));
                }
                else {
                    // cache contains NULL-termination
                    strptr.sub(start, bktsize).fill_lcp(depth + lcpKeyDepth(cache[start]));
                }
            }
            bktsize = 1;
            start = i + 1;
        }
        // tail of loop for last item
        if (start != 0) {
            int rlcp = lcpKeyType(cache[start - 1], cache[start]);
            strptr.set_lcp(start, depth + rlcp);
            strptr.set_cache(start, getCharAtDepth(cache[start], rlcp));
        }
        if (bktsize > 1) {
            if (cache[start] & 0xFF) {
                // need deeper sort
                insertion_sort(
                    strptr.sub(start, bktsize), depth + sizeof(key_type));
            }
            else {
                // cache contains NULL-termination
                strptr.sub(start, bktsize).fill_lcp(depth + lcpKeyDepth(cache[start]));
            }
        }
    }

    class MKQSStep
    {
    public:
        StringPtr strptr;
        key_type* cache;
        size_t num_lt, num_eq, num_gt, depth;
        size_t idx;
        unsigned char eq_recurse;
#if PS5_CALC_LCP_MKQS == 1
        char_type dchar_eq, dchar_gt;
        unsigned char lcp_lt, lcp_eq, lcp_gt;
#elif PS5_CALC_LCP_MKQS == 2
        key_type pivot;
#endif

        MKQSStep(Context& ctx, const StringPtr& strptr,
                 key_type* cache, size_t depth, bool CacheDirty)
            : strptr(strptr), cache(cache), depth(depth), idx(0)
        {
            size_t n = strptr.size();

            const StringSet& strset = strptr.active();

            if (CacheDirty) {
                typename StringSet::Iterator it = strset.begin();
                for (size_t i = 0; i < n; ++i, ++it) {
                    cache[i] = strset.get_uint64(*it, depth);
                }
            }
            // select median of 9
            size_t p = med3(
                cache,
                med3(cache, 0, n / 8, n / 4),
                med3(cache, n / 2 - n / 8, n / 2, n / 2 + n / 8),
                med3(cache, n - 1 - n / 4, n - 1 - n / 8, n - 3));
            // swap pivot to first position
            std::swap(strset.at(0), strset.at(p));
            std::swap(cache[0], cache[p]);
            // save the pivot value
            key_type pivot = cache[0];
#if PS5_CALC_LCP_MKQS == 1
            // for immediate LCP calculation
            key_type max_lt = 0, min_gt = std::numeric_limits<key_type>::max();
#elif PS5_CALC_LCP_MKQS == 2
            this->pivot = pivot;
#endif
            // indexes into array: 0 [pivot] 1 [===] leq [<<<] llt [???] rgt [>>>] req [===] n-1
            size_t leq = 1, llt = 1, rgt = n - 1, req = n - 1;
            while (true)
            {
                while (llt <= rgt)
                {
                    int r = cmp(cache[llt], pivot);
                    if (r > 0) {
#if PS5_CALC_LCP_MKQS == 1
                        min_gt = std::min(min_gt, cache[llt]);
#endif
                        break;
                    }
                    else if (r == 0) {
                        std::swap(strset.at(leq), strset.at(llt));
                        std::swap(cache[leq], cache[llt]);
                        leq++;
                    }
                    else {
#if PS5_CALC_LCP_MKQS == 1
                        max_lt = std::max(max_lt, cache[llt]);
#endif
                    }
                    ++llt;
                }
                while (llt <= rgt)
                {
                    int r = cmp(cache[rgt], pivot);
                    if (r < 0) {
#if PS5_CALC_LCP_MKQS == 1
                        max_lt = std::max(max_lt, cache[rgt]);
#endif
                        break;
                    }
                    else if (r == 0) {
                        std::swap(strset.at(req), strset.at(rgt));
                        std::swap(cache[req], cache[rgt]);
                        req--;
                    }
                    else {
#if PS5_CALC_LCP_MKQS == 1
                        min_gt = std::min(min_gt, cache[rgt]);
#endif
                    }
                    --rgt;
                }
                if (llt > rgt)
                    break;
                std::swap(strset.at(llt), strset.at(rgt));
                std::swap(cache[llt], cache[rgt]);
                ++llt;
                --rgt;
            }
            // calculate size of areas = < and >, save into struct
            size_t num_leq = leq, num_req = n - 1 - req;
            num_eq = num_leq + num_req;
            num_lt = llt - leq;
            num_gt = req - rgt;
            assert(num_eq > 0);
            assert(num_lt + num_eq + num_gt == n);

            // swap equal values from left to center
            const size_t size1 = std::min(num_leq, num_lt);
            std::swap_ranges(strset.begin(), strset.begin() + size1,
                             strset.begin() + llt - size1);
            std::swap_ranges(cache, cache + size1, cache + llt - size1);

            // swap equal values from right to center
            const size_t size2 = std::min(num_req, num_gt);
            std::swap_ranges(strset.begin() + llt, strset.begin() + llt + size2,
                             strset.begin() + n - size2);
            std::swap_ranges(cache + llt, cache + llt + size2,
                             cache + n - size2);

            // No recursive sorting if pivot has a zero byte
            this->eq_recurse = (pivot & 0xFF);

#if PS5_CALC_LCP_MKQS == 1
            // save LCP values for writing into LCP array after sorting further
            if (num_lt > 0)
            {
                assert(max_lt == *std::max_element(cache + 0, cache + num_lt));

                lcp_lt = lcpKeyType(max_lt, pivot);
                dchar_eq = getCharAtDepth(pivot, lcp_lt);
            }

            // calculate equal area lcp: +1 for the equal zero termination byte
            lcp_eq = lcpKeyDepth(pivot);

            if (num_gt > 0)
            {
                assert(min_gt == *std::min_element(cache + num_lt + num_eq, cache + n));

                lcp_gt = lcpKeyType(pivot, min_gt);
                dchar_gt = getCharAtDepth(min_gt, lcp_gt);
            }
#endif
            ++ctx.bs_steps;
        }

        void calculate_lcp()
        {
#if PS5_CALC_LCP_MKQS == 1
            if (num_lt > 0)
            {
                strptr.original().set_lcp(num_lt, depth + lcp_lt);
                strptr.original().set_cache(num_lt, dchar_eq);
            }

            if (num_gt > 0)
            {
                strptr.original().set_lcp(num_lt + num_eq, depth + lcp_gt);
                strptr.original().set_cache(num_lt + num_eq, dchar_gt);
            }
#elif PS5_CALC_LCP_MKQS == 2
            if (num_lt > 0)
            {
                key_type max_lt = strptr.original().output().get_uint64(
                    strptr.original().out(num_lt - 1), depth);

                unsigned int rlcp = lcpKeyType(max_lt, pivot);

                strptr.original().set_lcp(num_lt, depth + rlcp);
                strptr.original().set_cache(num_lt, getCharAtDepth(pivot, rlcp));
            }
            if (num_gt > 0)
            {
                key_type min_gt = strptr.original().output().get_uint64(
                    strptr.original().out(num_lt + num_eq), depth);

                unsigned int rlcp = lcpKeyType(pivot, min_gt);

                strptr.original().set_lcp(num_lt + num_eq, depth + rlcp);
                strptr.original().set_cache(num_lt + num_eq, getCharAtDepth(min_gt, rlcp));
            }
#endif
        }
    };

    size_t ms_pop_front;
    std::vector<MKQSStep> ms_stack;

    void sort_mkqs_cache(Context& ctx, const StringPtr& strptr, size_t depth)
    {
        if (!enable_sequential_mkqs ||
            strptr.size() < g_inssort_threshold) {

            insertion_sort(strptr.copy_back(), depth);
            ctx.donesize(strptr.size(), thrid);
            return;
        }

        if (bktcache_size < strptr.size() * sizeof(key_type)) {
            delete[] bktcache;
            bktcache = (uint16_t*)new key_type[strptr.size()];
            bktcache_size = strptr.size() * sizeof(key_type);
        }

        key_type* cache = (key_type*)bktcache; // reuse bktcache as keycache

        assert(ms_pop_front == 0);
        assert(ms_stack.size() == 0);

        // std::deque is much slower than std::vector, so we use an artificial
        // pop_front variable.
        ms_stack.emplace_back(ctx, strptr, cache, depth, true);

        while (ms_stack.size() > ms_pop_front)
        {
            MKQSStep& ms = ms_stack.back();
            ++ms.idx; // increment here, because stack may change

            // process the lt-subsequence
            if (ms.idx == 1)
            {
                if (ms.num_lt == 0)
                    ;
                else if (ms.num_lt < g_inssort_threshold) {
                    insertion_sort_cache<false>(ms.strptr.sub(0, ms.num_lt),
                                                ms.cache, ms.depth);
                    ctx.donesize(ms.num_lt, thrid);
                }
                else {
                    ms_stack.emplace_back(
                        ctx,
                        ms.strptr.sub(0, ms.num_lt),
                        ms.cache, ms.depth, false);
                }
            }
            // process the eq-subsequence
            else if (ms.idx == 2)
            {
                StringPtr sp = ms.strptr.sub(ms.num_lt, ms.num_eq);

                assert(ms.num_eq > 0);

                if (!ms.eq_recurse) {
                    StringPtr spb = sp.copy_back();
#if PS5_CALC_LCP_MKQS == 1
                    spb.fill_lcp(ms.depth + ms.lcp_eq);
#elif PS5_CALC_LCP_MKQS == 2
                    spb.fill_lcp(ms.depth + lcpKeyDepth(ms.pivot));
#endif
                    ctx.donesize(spb.size(), thrid);
                }
                else if (ms.num_eq < g_inssort_threshold) {
                    insertion_sort_cache<true>(sp, ms.cache + ms.num_lt,
                                               ms.depth + sizeof(key_type));
                    ctx.donesize(ms.num_eq, thrid);
                }
                else {
                    ms_stack.emplace_back(
                        ctx, sp,
                        ms.cache + ms.num_lt,
                        ms.depth + sizeof(key_type), true);
                }
            }
            // process the gt-subsequence
            else if (ms.idx == 3)
            {
                StringPtr sp = ms.strptr.sub(ms.num_lt + ms.num_eq, ms.num_gt);

                if (ms.num_gt == 0)
                    ;
                else if (ms.num_gt < g_inssort_threshold) {
                    insertion_sort_cache<false>(sp, ms.cache + ms.num_lt + ms.num_eq,
                                                ms.depth);
                    ctx.donesize(ms.num_gt, thrid);
                }
                else {
                    ms_stack.emplace_back(
                        ctx, sp,
                        ms.cache + ms.num_lt + ms.num_eq,
                        ms.depth, false);
                }
            }
            // calculate lcps
            else
            {
                // finished sort
                assert(ms_stack.size() > ms_pop_front);

                // calculate LCP after the three parts are sorted
                ms_stack.back().calculate_lcp();

                ms_stack.pop_back();
            }

            if (use_work_sharing && ctx.jobqueue.has_idle()) {
                sample_sort_free_work(ctx);
            }
        }
    }

    void mkqs_free_work(Context& ctx)
    {
        assert(ms_stack.size() >= ms_pop_front);

        for (unsigned int fl = 0; fl < 8; ++fl)
        {
            if (ms_stack.size() == ms_pop_front) {
                return;
            }

            // convert top level of stack into independent jobs

            MKQSStep& ms = ms_stack[ms_pop_front];

            if (ms.idx == 0 && ms.num_lt != 0)
            {
                this->substep_add();
                Enqueue<Classify>(ctx, this, ms.strptr.sub(0, ms.num_lt), ms.depth);
            }
            if (ms.idx <= 1) // st.num_eq > 0 always
            {
                assert(ms.num_eq > 0);

                StringPtr sp = ms.strptr.sub(ms.num_lt, ms.num_eq);

                if (ms.eq_recurse) {
                    this->substep_add();
                    Enqueue<Classify>(ctx, this, sp,
                                      ms.depth + sizeof(key_type));
                }
                else {
                    StringPtr spb = sp.copy_back();
#if PS5_CALC_LCP_MKQS == 1
                    spb.fill_lcp(ms.depth + ms.lcp_eq);
#elif PS5_CALC_LCP_MKQS == 2
                    spb.fill_lcp(ms.depth + lcpKeyDepth(ms.pivot));
#else
                    UNUSED(spb);
#endif
                    ctx.donesize(ms.num_eq, thrid);
                }
            }
            if (ms.idx <= 2 && ms.num_gt != 0)
            {
                this->substep_add();
                Enqueue<Classify>(ctx, this,
                                  ms.strptr.sub(ms.num_lt + ms.num_eq, ms.num_gt), ms.depth);
            }

            // shorten the current stack
            ++ms_pop_front;
        }
    }

    void substep_all_done() final
    {
        while (ms_pop_front > 0) {
            ms_stack[--ms_pop_front].calculate_lcp();
        }

        while (ss_pop_front > 0) {
            ss_stack[--ss_pop_front].calculate_lcp();
        }

        if (pstep) pstep->substep_notify_done();
        delete this;
    }
};

// ****************************************************************************
// *** SampleSortStep out-of-place parallel sample sort with separate Jobs

template <typename Context, template <size_t> class Classify, typename StringPtr>
class SampleSortStep : public SortStep
{
public:
    typedef typename StringPtr::StringSet StringSet;
    typedef typename StringSet::Iterator StrIterator;

    //! type of Job
    typedef typename Context::job_type job_type;

    //! parent sort step notification
    SortStep* pstep;

    //! string pointers, size, and current sorting depth
    StringPtr strptr;
    size_t depth;

    //! number of parts into which the strings were split
    size_t parts;
    //! size of all parts except the last
    size_t psize;
    //! number of threads still working
    std::atomic<size_t> pwork;

    //! classifier instance and variables (contains splitter tree
    Classify<bingmann_sample_sort::DefaultTreebits> classifier;

    static const size_t treebits =
        Classify<bingmann_sample_sort::DefaultTreebits>::treebits;
    static const size_t numsplitters =
        Classify<bingmann_sample_sort::DefaultTreebits>::numsplitters;
    static const size_t bktnum = 2 * numsplitters + 1;

    //! LCPs of splitters, needed for recursive calls
    unsigned char splitter_lcp[numsplitters + 1];

    //! individual bucket array of threads, keep bkt[0] for DistributeJob
    size_t* bkt[MAXPROCS];
    //! bucket ids cache, created by classifier and later counted
    uint16_t* bktcache[MAXPROCS];

    // *** Classes for JobQueue

    struct SampleJob : public job_type
    {
        SampleSortStep* step;

        SampleJob(SampleSortStep* _step)
            : step(_step) { }

        bool run(Context& ctx) final
        {
            step->sample(ctx);
            return true;
        }
    };

    struct CountJob : public job_type
    {
        SampleSortStep* step;
        unsigned int  p;

        CountJob(SampleSortStep* _step, unsigned int _p)
            : step(_step), p(_p) { }

        bool run(Context& ctx) final
        {
            step->count(p, ctx);
            return true;
        }
    };

    struct DistributeJob : public job_type
    {
        SampleSortStep* step;
        unsigned int  p;

        DistributeJob(SampleSortStep* _step, unsigned int _p)
            : step(_step), p(_p) { }

        bool run(Context& ctx) final
        {
            step->distribute(p, ctx);
            return true;
        }
    };

    // *** Constructor

    SampleSortStep(Context& ctx, SortStep* pstep,
                   const StringPtr& strptr, size_t depth)
        : pstep(pstep), strptr(strptr), depth(depth)
    {
        parts = strptr.size() / ctx.sequential_threshold() * 2;
        if (parts == 0) parts = 1;
        if (parts > MAXPROCS) parts = MAXPROCS;

        psize = (strptr.size() + parts - 1) / parts;

        ctx.jobqueue.enqueue(new SampleJob(this));
        ++ctx.para_ss_steps;
    }

    virtual ~SampleSortStep() { }

    // *** Sample Step

    void sample(Context& ctx)
    {
        const size_t oversample_factor = 2;
        size_t samplesize = oversample_factor * numsplitters;

        const StringSet& strset = strptr.active();
        StrIterator begin = strset.begin();
        size_t n = strset.size();

        key_type samples[samplesize];

        LCGRandom rng(&samples);

        for (size_t i = 0; i < samplesize; ++i)
            samples[i] = strset.get_uint64(strset[begin + rng() % n], depth);

        std::sort(samples, samples + samplesize);

        classifier.build(samples, samplesize, splitter_lcp);

        // create new jobs
        pwork = parts;
        for (unsigned int p = 0; p < parts; ++p)
            ctx.jobqueue.enqueue(new CountJob(this, p));
    }

    // *** Counting Step

    void count(unsigned int p, Context& ctx)
    {
        const StringSet& strset = strptr.active();

        StrIterator strB = strset.begin() + p * psize;
        StrIterator strE = strset.begin() + std::min((p + 1) * psize, strptr.size());
        if (strE < strB) strE = strB;

        uint16_t* mybktcache = bktcache[p] = new uint16_t[strE - strB];
        classifier.classify(strset, strB, strE, mybktcache, depth);

        size_t* mybkt = bkt[p] = new size_t[bktnum + (p == 0 ? 1 : 0)];
        memset(mybkt, 0, bktnum * sizeof(size_t));

        for (uint16_t* bc = mybktcache; bc != mybktcache + (strE - strB); ++bc)
            ++mybkt[*bc];

        if (--pwork == 0)
            count_finished(ctx);
    }

    void count_finished(Context& ctx)
    {
        // abort sorting if we're measuring only the top level
        if (use_only_first_sortstep)
            return;

        // inclusive prefix sum over bkt
        size_t sum = 0;
        for (unsigned int i = 0; i < bktnum; ++i)
        {
            for (unsigned int p = 0; p < parts; ++p)
            {
                bkt[p][i] = (sum += bkt[p][i]);
            }
        }
        assert(sum == strptr.size());

        // create new jobs
        pwork = parts;
        for (unsigned int p = 0; p < parts; ++p)
            ctx.jobqueue.enqueue(new DistributeJob(this, p));
    }

    // *** Distribute Step

    void distribute(unsigned int p, Context& ctx)
    {
        const StringSet& strset = strptr.active();

        StrIterator strB = strset.begin() + p * psize;
        StrIterator strE = strset.begin() + std::min((p + 1) * psize, strptr.size());
        if (strE < strB) strE = strB;

        const StringSet& sorted = strptr.shadow(); // get alternative shadow pointer array
        typename StringSet::Iterator sbegin = sorted.begin();

        uint16_t* mybktcache = bktcache[p];
        size_t* mybkt = bkt[p];

        for (StrIterator str = strB; str != strE; ++str, ++mybktcache)
            *(sbegin + --mybkt[*mybktcache]) = std::move(*str);

        if (p != 0) // p = 0 is needed for recursion into bkts
            delete[] bkt[p];

        delete[] bktcache[p];

        if (--pwork == 0)
            distribute_finished(ctx);
    }

    void distribute_finished(Context& ctx)
    {
        size_t thrid = PS5_ENABLE_RESTSIZE ? omp_get_thread_num() : 0;

        size_t* bkt = this->bkt[0];
        assert(bkt);

        // first processor's bkt pointers are boundaries between bkts, just add sentinel:
        assert(bkt[0] == 0);
        bkt[bktnum] = strptr.size();

        // keep anonymous subjob handle while creating subjobs
        this->substep_add();

        size_t i = 0;
        while (i < bktnum - 1)
        {
            // i is even -> bkt[i] is less-than bucket
            size_t bktsize = bkt[i + 1] - bkt[i];
            if (bktsize == 0)
                ;
            else if (bktsize == 1) { // just one string pointer, copyback
                strptr.flip(bkt[i], 1).copy_back();
                ctx.donesize(1, thrid);
            }
            else
            {
                this->substep_add();
                Enqueue<Classify>(ctx, this, strptr.flip(bkt[i], bktsize),
                                  depth + (splitter_lcp[i / 2] & 0x7F));
            }
            ++i;
            // i is odd -> bkt[i] is equal bucket
            bktsize = bkt[i + 1] - bkt[i];
            if (bktsize == 0)
                ;
            else if (bktsize == 1) { // just one string pointer, copyback
                strptr.flip(bkt[i], 1).copy_back();
                ctx.donesize(1, thrid);
            }
            else
            {
                if (splitter_lcp[i / 2] & 0x80) {
                    // equal-bucket has NULL-terminated key, done.
                    StringPtr sp = strptr.flip(bkt[i], bktsize).copy_back();
                    sp.fill_lcp(depth + lcpKeyDepth(classifier.get_splitter(i / 2)));
                    ctx.donesize(bktsize, thrid);
                }
                else {
                    this->substep_add();
                    Enqueue<Classify>(ctx, this, strptr.flip(bkt[i], bktsize),
                                      depth + sizeof(key_type));
                }
            }
            ++i;
        }

        size_t bktsize = bkt[i + 1] - bkt[i];

        if (bktsize == 0)
            ;
        else if (bktsize == 1) { // just one string pointer, copyback
            strptr.flip(bkt[i], 1).copy_back();
            ctx.donesize(1, thrid);
        }
        else
        {
            this->substep_add();
            Enqueue<Classify>(ctx, this, strptr.flip(bkt[i], bktsize), depth);
        }

        this->substep_notify_done(); // release anonymous subjob handle

        if (!Context::CalcLcp) {
            delete[] bkt;
        }
    }

    // *** After Recursive Sorting

    void substep_all_done() final
    {
        if (Context::CalcLcp) {
            sample_sort_lcp<bktnum>(classifier, strptr.original(), depth, bkt[0]);
            delete[] bkt[0];
        }

        if (pstep) pstep->substep_notify_done();
        delete this;
    }
};

template <template <size_t> class Classify, typename Context, typename StringPtr>
void Enqueue(Context& ctx, SortStep* pstep,
             const StringPtr& strptr, size_t depth)
{
    if (enable_parallel_sample_sort &&
        (strptr.size() > ctx.sequential_threshold() || use_only_first_sortstep)) {
        new SampleSortStep<Context, Classify, StringPtr>(
            ctx, pstep, strptr, depth);
    }
    else {
        if (strptr.size() < ((uint64_t)1 << 32)) {
            ctx.jobqueue.enqueue(
                new SmallsortJob<Context, Classify, StringPtr, uint32_t>(
                    pstep, strptr, depth));
        }
        else {
            ctx.jobqueue.enqueue(
                new SmallsortJob<Context, Classify, StringPtr, uint64_t>(
                    pstep, strptr, depth));
        }
    }
}

/******************************************************************************/
// Externally Callable Sorting Methods

//! Main Parallel Sample Sort Function. See below for more convenient wrappers.
template <template <size_t> class Classify =
              bingmann_sample_sort::ClassifyTreeCalcUnrollInterleaveX,
          typename StringPtr>
void parallel_sample_sort(const StringPtr& strptr, size_t depth)
{
    using SContext = Context<false>;
    SContext ctx;
    ctx.totalsize = strptr.size();
#if PS5_ENABLE_RESTSIZE
    ctx.restsize = strptr.size();
#endif
    ctx.threadnum = omp_get_max_threads();

    Enqueue<Classify>(ctx, NULL, strptr, depth);
    ctx.jobqueue.loop();

#if PS5_ENABLE_RESTSIZE
    assert(!PS5_ENABLE_RESTSIZE || ctx.restsize.update().get() == 0);
#endif
}

//! call Sample Sort on a generic StringSet, this allocates the shadow array for
//! flipping.
template <template <size_t> class Classify =
              bingmann_sample_sort::ClassifyTreeCalcUnrollInterleaveX,
          typename StringSet>
void parallel_sample_sort_base(const StringSet& strset, size_t depth)
{
    typedef stringtools::StringShadowPtr<StringSet> StringShadowPtr;
    typedef typename StringSet::Container Container;

    // allocate shadow pointer array
    Container shadow = strset.allocate(strset.size());
    StringShadowPtr strptr(strset, StringSet(shadow));

    parallel_sample_sort<Classify>(strptr, depth);

    StringSet::deallocate(shadow);
}

//! call Sample Sort on a generic input StringSet, but write output to output
//! StringSet, use output as shadow array for flipping.
template <template <size_t> class Classify =
              bingmann_sample_sort::ClassifyTreeCalcUnrollInterleaveX,
          typename StringSet>
void parallel_sample_sort_out_base(
    const StringSet& strset, const StringSet& output, size_t depth)
{
    typedef stringtools::StringShadowOutPtr<StringSet> StringOutPtr;

    StringOutPtr strptr(strset, output, output);
    parallel_sample_sort<Classify>(strptr, depth);
}

template <template <size_t> class Classify =
              bingmann_sample_sort::ClassifyTreeCalcUnrollInterleaveX,
          typename StringSet>
void parallel_sample_sort_out_test(const StringSet& strset, size_t depth)
{
    typename StringSet::Container out = strset.allocate(strset.size());
    StringSet output(out);
    parallel_sample_sort_out_base<Classify>(strset, output, depth);

    // move strings back to strset
    std::move(output.begin(), output.end(), strset.begin());

    StringSet::deallocate(out);
}

/******************************************************************************/

template <template <size_t> class Classify, typename StringSet>
void parallel_sample_sort_lcp_base(
    const StringSet& strset, uintptr_t* lcp, size_t depth)
{
    typedef stringtools::StringShadowLcpPtr<StringSet> StringShadowLcpPtr;
    typedef typename StringSet::Container Container;

    // allocate shadow pointer array
    Container shadow = strset.allocate(strset.size());

    StringShadowLcpPtr strptr(strset, StringSet(shadow), lcp);

    parallel_sample_sort<Classify>(strptr, depth);

    StringSet::deallocate(shadow);
}

template <template <size_t> class Classify =
              bingmann_sample_sort::ClassifyTreeCalcUnrollInterleaveX,
          typename StringSet>
void parallel_sample_sort_lcp_base(const StringSet& strset, size_t depth)
{
    std::vector<uintptr_t> tmp_lcp(strset.size());
    parallel_sample_sort_lcp_base<Classify>(strset, tmp_lcp.data(), depth);
}

template <template <size_t> class Classify =
              bingmann_sample_sort::ClassifyTreeCalcUnrollInterleaveX,
          typename StringSet>
void parallel_sample_sort_lcp_verify(const StringSet& strset, size_t depth)
{
    std::vector<uintptr_t> tmp_lcp(strset.size());
    tmp_lcp[0] = 42;                 // must keep lcp[0] unchanged
    std::fill(tmp_lcp.begin() + 1, tmp_lcp.end(), -1);
    parallel_sample_sort_lcp_base<Classify>(strset, tmp_lcp.data(), depth);
    die_unless(stringtools::verify_lcp(strset, tmp_lcp.data(), 42));
}

template <template <size_t> class Classify =
              bingmann_sample_sort::ClassifyTreeCalcUnrollInterleaveX,
          typename StringSet>
void parallel_sample_sort_out_lcp_verify(const StringSet& strset, size_t depth)
{
    typename StringSet::Container out = strset.allocate(strset.size());
    StringSet output(out);

    std::vector<uintptr_t> tmp_lcp(strset.size());
    tmp_lcp[0] = 42;                 // must keep lcp[0] unchanged
    std::fill(tmp_lcp.begin() + 1, tmp_lcp.end(), -1);

    typedef stringtools::StringShadowLcpOutPtr<StringSet> StringOutPtr;

    StringOutPtr strptr(strset, output, output, tmp_lcp.data());
    parallel_sample_sort<Classify>(strptr, depth);

    // verify LCPs
    die_unless(stringtools::verify_lcp(output, tmp_lcp.data(), 42));

    // move strings back to strset
    std::move(output.begin(), output.end(), strset.begin());

    StringSet::deallocate(out);
}

//! Call for NUMA aware parallel sorting
static inline
void parallel_sample_sort_numa(bingmann::string* strings, size_t n,
                               int numaNode, int numberOfThreads,
                               const LcpCacheStringPtr& output)
{
    // tie thread to a NUMA node
    numa_run_on_node(numaNode);
    numa_set_preferred(numaNode);

    Context</* CalcLcp */ true> ctx;
    ctx.totalsize = n;
#if PS5_ENABLE_RESTSIZE
    ctx.restsize = n;
#endif
    ctx.threadnum = numberOfThreads;

    typedef UCharStringSet StringSet;
    StringSet strset(strings, strings + n);
    StringSet outputss(output.strings, output.strings + n);

    StringShadowLcpCacheOutPtr<StringSet> strptr(
        strset, outputss, outputss, output.lcps, output.cachedChars);

    Enqueue<bingmann_sample_sort::ClassifyTreeCalcUnrollInterleaveX>(
        ctx, NULL, strptr, 0);
    ctx.jobqueue.numaLoop(numaNode, numberOfThreads);

    // fixup first entry of LCP and charcache
    output.firstLcp() = 0;
    output.firstCached() = output.firstString()[0];

#if PS5_ENABLE_RESTSIZE
    assert(ctx.restsize.update().get() == 0);
#endif
}

//! Call for NUMA aware parallel sorting
static inline
void parallel_sample_sort_numa2(const UCharStringShadowLcpCacheOutPtr* strptr,
                                unsigned numInputs)
{
    typedef Context</* CalcLcp */ true, NumaJobQueueGroup> context_type;

    context_type::jobqueuegroup_type group;

    // construct one Context per input
    context_type* ctx[numInputs];

    for (unsigned i = 0; i < numInputs; ++i)
    {
        ctx[i] = new context_type(&group);

        ctx[i]->totalsize = strptr[i].size();
#if PS5_ENABLE_RESTSIZE
        ctx[i]->restsize = strptr[i].size();
#endif
        ctx[i]->threadnum = group.calcThreadNum(i, numInputs);
        if (ctx[i]->threadnum == 0)
            ctx[i]->threadnum = 1;

        Enqueue<bingmann_sample_sort::ClassifyTreeCalcUnrollInterleaveX>(
            *ctx[i], NULL, strptr[i], 0);

        group.add_jobqueue(&ctx[i]->jobqueue);
    }

    group.numaLaunch();

    for (unsigned i = 0; i < numInputs; ++i)
    {
        // fixup first entry of LCP and charcache
        strptr[i].lcparray()[0] = 0;
        strptr[i].set_cache(0, strptr[i].out(0)[0]);

#if PS5_ENABLE_RESTSIZE
        assert(ctx[i].restsize.update().get() == 0);
#endif

        delete ctx[i];
    }
}

} // namespace bingmann_parallel_sample_sort

#endif // !PSS_SRC_PARALLEL_BINGMANN_PARALLEL_SAMPLE_SORT_HEADER

/******************************************************************************/
