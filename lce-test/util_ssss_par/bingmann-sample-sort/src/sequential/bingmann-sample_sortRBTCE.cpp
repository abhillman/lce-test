/*******************************************************************************
 * src/sequential/bingmann-sample_sortRBTCE.cpp
 *
 * Experiments with sequential Super Scalar String Sample-Sort (S^5).
 *
 * Binary tree search with equality branch, recursive subtrees and bucket
 * cache. While constructing the splitter tree from the sample array, the area
 * of equal samples is known. If it is large enough, a subtree is constructed
 * for that equal key and marked as splitter_subtree. Most functions are
 * implemented recursively on SplitterTree.
 *
 * Possible improvements: manage memory of bktsize and bktindex better.
 *
 *******************************************************************************
 * Copyright (C) 2013 Timo Bingmann <tb@panthema.net>
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

#include "bingmann-sample_sort.hpp"

namespace bingmann_sample_sortRBTCE {

using namespace bingmann_sample_sort;

static const bool debug_subtree = false;
static const bool debug_splitter_subtree = false;

// ----------------------------------------------------------------------------

class SplitterTree
{
public:
#if 0
    static const size_t numsplitters2 = 16;
#else
    // bounding equations:
    // splitters            + bktsize
    // n * sizeof(key_type) + (2*n+1) * sizeof(size_t) <= l2cache

    static const size_t numsplitters2 = (l2cache - sizeof(size_t)) / (2 * sizeof(size_t));
#endif

    static const size_t treebits = tlx::Log2Floor<numsplitters2>::value;
    static const size_t numsplitters = (1 << treebits) - 1;

    static const size_t bktnum = 2 * numsplitters + 1;

    typedef std::pair<key_type, size_t> samplepair_type;

    key_type splitter_tree[numsplitters + 1];
    unsigned char splitter_lcp[numsplitters + 1];
    unsigned char splitter_subtree[numsplitters];

    class Builder
    {
    public:
        key_type* m_tree;
        unsigned char* m_lcp_iter;
        unsigned char* m_subtree_iter;
        samplepair_type* m_samples;

        std::vector<SplitterTree*>& m_treelist;
        string* m_strings;
        size_t m_depth;

        Builder(SplitterTree& st,
                samplepair_type* samples, samplepair_type* samples_end,
                std::vector<SplitterTree*>& treelist,
                string* strings, size_t depth)
            : m_tree(st.splitter_tree),
              m_lcp_iter(st.splitter_lcp),
              m_subtree_iter(st.splitter_subtree),
              m_samples(samples),
              m_treelist(treelist),
              m_strings(strings),
              m_depth(depth)
        {
            std::fill(st.splitter_subtree, st.splitter_subtree + numsplitters, 0);

            key_type sentinel = 0;
            recurse(samples, samples_end, 1, sentinel);

            assert(m_lcp_iter == st.splitter_lcp + numsplitters);
            assert(m_subtree_iter == st.splitter_subtree + numsplitters);

            // overwrite sentinel lcp for first < everything bucket
            st.splitter_lcp[0] &= 0x80;
        }

        ptrdiff_t snum(samplepair_type* s) const
        {
            return (ptrdiff_t)(s - m_samples);
        }

        void keynode(key_type& prevkey, key_type& mykey,
                     samplepair_type* midlo, samplepair_type* midhi)
        {
            key_type xorSplit = prevkey ^ mykey;

            *m_lcp_iter++ = (count_high_zero_bits(xorSplit) / 8) |
                            ((mykey & 0xFF) ? 0 : 0x80);  // marker for done splitters

            // decide whether to build a subtree:
            if (midhi - midlo >= (ptrdiff_t)(numsplitters / 2) // enough samples
                && (mykey & 0xFF) != 0                         // key is not NULL-terminated
                && m_treelist.size() < 255)                    // not too many subtrees
            {
                for (samplepair_type* s = midlo; s < midhi; ++s)
                {
                    size_t p = s->second;
                    s->first = get_char<key_type>(m_strings[p], m_depth + sizeof(key_type));
                }

                std::sort(midlo, midhi);

                *m_subtree_iter = m_treelist.size();
                m_treelist.push_back(new SplitterTree());

                SplitterTree::Builder(*m_treelist.back(), midlo, midhi,
                                      m_treelist, m_strings, m_depth + sizeof(key_type));
            }
            ++m_subtree_iter;
        }

        key_type recurse(samplepair_type* lo, samplepair_type* hi, unsigned int treeidx,
                         key_type& rec_prevkey)
        {
            // pick middle element as splitter
            samplepair_type* mid = lo + (ptrdiff_t)(hi - lo) / 2;

            key_type mykey = m_tree[treeidx] = mid->first;

            samplepair_type* midlo = mid;
            while (lo < midlo && (midlo - 1)->first == mykey) midlo--;

            samplepair_type* midhi = mid;
            while (midhi + 1 < hi && midhi->first == mykey) midhi++;

            if (2 * treeidx < numsplitters)
            {
                key_type prevkey = recurse(lo, midlo, 2 * treeidx + 0, rec_prevkey);

                keynode(prevkey, mykey, midlo, midhi);

                return recurse(midhi, hi, 2 * treeidx + 1, mykey);
            }
            else
            {
                keynode(rec_prevkey, mykey, midlo, midhi);

                return mykey;
            }
        }
    };

    static std::string binary(uint16_t v)
    {
        char binstr[17];
        binstr[16] = 0;
        for (int i = 0; i < 16; i++) {
            binstr[15 - i] = (v & 1) ? '1' : '0';
            v /= 2;
        }
        return binstr;
    }

    static unsigned int treeid_to_bkt(
        unsigned int id, size_t treebits, size_t numsplitters)
    {
        assert(id > 0);

        //int treebits = 4;
        //int bitmask = ((1 << treebits)-1);
        static const int bitmask = numsplitters;

        int hi = treebits - 32 + count_high_zero_bits<uint32_t>(id);

        unsigned int bkt = ((id << (hi + 1)) & bitmask) | (1 << hi);

        return bkt;
    }

    /// search in splitter tree for bucket number
    unsigned int find_bkt_tree_equal(const key_type& key)
    {
        // binary tree traversal without left branch

        unsigned int i = 1;

        while (i <= numsplitters)
        {
            if (key == splitter_tree[i])
                return 2 * treeid_to_bkt(i, treebits, numsplitters) - 1;
            else if (key < splitter_tree[i])
                i = 2 * i + 0;
            else    // (key > splitter_tree[i])
                i = 2 * i + 1;
        }

        i -= numsplitters + 1;

        return 2 * i; // < or > bucket
    }

    /// binary search on splitter array for bucket number
    unsigned int find_bkt_tree_asmequal(const key_type& key)
    {
        unsigned int i;

        // hand-coded assembler binary tree traversal with equality
        asm ("mov    $1, %%rax \n"             // rax = i
             // body of while loop
             "1: \n"
             "cmpq	(%[splitter_tree],%%rax,8), %[key] \n"
             "je     2f \n"
             "lea    (%%rax,%%rax), %%rax \n"
             "lea    1(%%rax), %%rcx \n"
             "cmova  %%rcx, %%rax \n"            // CMOV rax = 2 * i + 1
             "cmp    %[numsplitters1], %%rax \n" // i < numsplitters+1
             "jb     1b \n"
             "sub    %[numsplitters1], %%rax \n" // i -= numsplitters+1;
             "lea    (%%rax,%%rax), %%rax \n"    // i = i*2
             "jmp    3f \n"
             "2: \n"
             "bsr    %%rax, %%rdx \n"            // dx = bit number of highest one
             "mov    %[treebits], %%rcx \n"
             "sub    %%rdx, %%rcx \n"            // cx = treebits - highest
             "shl    %%cl, %%rax \n"             // shift ax to left
             "and    %[numsplitters], %%rax \n"  // mask off other bits
             "lea    -1(%%rcx), %%rcx \n"
             "mov    $1, %%rdx \n"               // dx = (1 << (hi-1))
             "shl    %%cl, %%rdx \n"             //
             "or     %%rdx, %%rax \n"            // ax = OR of both
             "lea    -1(%%rax,%%rax), %%rax \n"  // i = i * 2 - 1
             "3: \n"
             : "=&a" (i)
             :[key] "r" (key), [splitter_tree] "r" (splitter_tree),
             [numsplitters1] "g" (numsplitters + 1),
             [treebits] "g" (treebits),
             [numsplitters] "g" (numsplitters)
             : "rcx", "rdx");

        return i;
    }

    std::vector<uint16_t> bktcache;     // bktcache for all trees != 0

    size_t bktsize[bktnum], bktindex[bktnum];

    void calc_bktsize_prefixsum(uint16_t* bktcache, size_t n)
    {
        memset(bktsize, 0, bktnum * sizeof(size_t));

        for (uint16_t* b = bktcache; b != bktcache + n; ++b)
            ++bktsize[*b];

        bktindex[0] = 0;
        for (unsigned int i = 1; i < bktnum; ++i) {
            bktindex[i] = bktindex[i - 1] + bktsize[i - 1];
        }
        assert(bktindex[bktnum - 1] + bktsize[bktnum - 1] == n);
    }

    void recursive_permute(string* strings, size_t n, uint16_t* bktcache,
                           string* sorted, std::vector<SplitterTree*>& treelist)
    {
        // step 4: premute out-of-place

        for (size_t i = 0; i < n; ++i)
            sorted[bktindex[bktcache[i]]++] = strings[i];

        memcpy(strings, sorted, n * sizeof(string));

        {
            std::vector<uint16_t> bktcache_delete;
            bktcache_delete.swap(this->bktcache);
        }

        // step 4.5: recursively permute subtrees

        size_t i = 0, bsum = 0;
        while (i < bktnum - 1)
        {
            // i is even -> bkt[i] is less-than bucket
            bsum += bktsize[i++];

            // i is odd -> bkt[i] is equal bucket
            if (splitter_subtree[i / 2])
            {
                assert(splitter_subtree[i / 2] < treelist.size());
                SplitterTree& t = *treelist[splitter_subtree[i / 2]];

                assert(bktsize[i] == t.bktcache.size());
                t.recursive_permute(strings + bsum, bktsize[i],
                                    t.bktcache.data(), sorted, treelist);
            }
            bsum += bktsize[i++];
        }
        bsum += bktsize[i++];
        assert(i == bktnum && bsum == n);
    }

    template <unsigned int(SplitterTree::* find_bkt) (const key_type&)>
    void recursive_sort(
        string* strings, size_t n,
        std::vector<SplitterTree*>& treelist, size_t depth)
    {
        size_t i = 0, bsum = 0;
        while (i < bktnum - 1)
        {
            // i is even -> bkt[i] is less-than bucket
            if (bktsize[i] > 1)
            {
                if (!g_toplevel_only)
                    sort<find_bkt>(strings + bsum, bktsize[i],
                                   depth + (splitter_lcp[i / 2] & 0x7F));
            }
            bsum += bktsize[i++];

            // i is odd -> bkt[i] is equal bucket
            if (bktsize[i] > 1)
            {
                if (splitter_lcp[i / 2] & 0x80) {
                    // equal-bucket has NULL-terminated key, done.
                }
                else if (splitter_subtree[i / 2])
                {
                    assert(splitter_subtree[i / 2] < treelist.size());
                    SplitterTree& t = *treelist[splitter_subtree[i / 2]];

                    t.recursive_sort<find_bkt>(
                        strings + bsum, bktsize[i], treelist,
                        depth + sizeof(key_type));
                }
                else {
                    if (!g_toplevel_only)
                        sort<find_bkt>(strings + bsum, bktsize[i],
                                       depth + sizeof(key_type));
                }
            }
            bsum += bktsize[i++];
        }
        if (bktsize[i] > 0)
        {
            if (!g_toplevel_only)
                sort<find_bkt>(strings + bsum, bktsize[i], depth);
        }
        bsum += bktsize[i++];
        assert(i == bktnum && bsum == n);

        // delete [] bktsize;
    }

    /// Variant of string sample-sort: use super-scalar binary search on
    /// splitters, with index caching.
    template <unsigned int(SplitterTree::* find_bkt) (const key_type&)>
    static void sort(string* strings, size_t n, size_t depth)
    {
        if (n < g_samplesort_smallsort)
        {
            sample_sort_small_sort(strings, n, depth);
            return;
        }

        // step 1: select splitters with oversampling
        //const size_t oversample_factor = 8;
        const size_t samplesize = oversample_factor * numsplitters;

        samplepair_type samples[samplesize];

        LCGRandom rng(&strings);

        for (unsigned int i = 0; i < samplesize; ++i)
        {
            size_t p = rng() % n;
            samples[i] = samplepair_type(get_char<key_type>(strings[p], depth), p);
        }

        std::sort(samples, samples + samplesize);

        // step 1.5: create splitter trees recursively
        std::vector<SplitterTree*> treelist;
        treelist.push_back(new SplitterTree());
        SplitterTree::Builder(*treelist.back(), samples, samples + samplesize,
                              treelist, strings, depth);

        // step 2.2: classify all strings and count bucket sizes
        // subtree 0 has exactly n strings, the number of strings in other
        // subtrees is not known yet.
        uint16_t* bktcache = new uint16_t[n];

        for (size_t si = 0; si < n; ++si)
        {
            unsigned int t = 0, d = 0;

            // binary search in splitter with equal check
            key_type key = get_char<key_type>(strings[si], depth);

            unsigned int b = (treelist[t]->*find_bkt)(key);
            assert(b < bktnum);

            bktcache[si] = b;

            // if equal bucket and subtree is attached, recursive into subtree
            while ((b & 1) && (t = treelist[t]->splitter_subtree[b / 2]) != 0)
            {
                d += sizeof(key_type);

                key = get_char<key_type>(strings[si], depth + d);

                b = (treelist[t]->*find_bkt)(key);
                assert(b < bktnum);

                treelist[t]->bktcache.push_back(b);
            }
        }

        // step 3: calculate bktsize on all subtrees and prefix sum
        treelist[0]->calc_bktsize_prefixsum(bktcache, n);

        for (unsigned int ti = 1; ti < treelist.size(); ++ti)
        {
            SplitterTree& t = *treelist[ti];
            t.calc_bktsize_prefixsum(t.bktcache.data(), t.bktcache.size());
        }

        // step 4: permute recursively
        string* sorted = new string[n];
        treelist[0]->recursive_permute(strings, n, bktcache, sorted, treelist);
        delete[] sorted;

        delete[] bktcache;

        // step 5: recursion
        treelist[0]->recursive_sort<find_bkt>(strings, n, treelist, depth);
    }
};

void bingmann_sample_sortRBTCE(string* strings, size_t n)
{
    SplitterTree::sort<&SplitterTree::find_bkt_tree_equal>(strings, n, 0);
}

void bingmann_sample_sortRBTCEA(string* strings, size_t n)
{
    SplitterTree::sort<&SplitterTree::find_bkt_tree_asmequal>(strings, n, 0);
}

// ----------------------------------------------------------------------------

} // namespace bingmann_sample_sortRBTCE

/******************************************************************************/
