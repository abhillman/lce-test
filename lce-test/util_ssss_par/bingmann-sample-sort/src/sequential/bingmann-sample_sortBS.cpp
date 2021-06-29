/*******************************************************************************
 * src/sequential/bingmann-sample_sortBS.cpp
 *
 * Experiments with sequential Super Scalar String Sample-Sort (S^5).
 *
 * Binary search on splitters without bucket cache.
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

namespace bingmann_sample_sortBS {

using namespace bingmann_sample_sort;

// ----------------------------------------------------------------------------

/// binary search on splitter array for bucket number
static inline unsigned int
find_bkt_binsearch(const key_type& key, const key_type* splitter, size_t leaves)
{
    unsigned int lo = 0, hi = leaves;

    while (lo < hi)
    {
        unsigned int mid = (lo + hi) >> 1;
        assert(mid < leaves);

        if (key <= splitter[mid])
            hi = mid;
        else    // (key > splitter[mid])
            lo = mid + 1;
    }

#if 0
    // Verify result of binary search:
    int pos = leaves - 1;
    while (pos >= 0 && key <= splitter[pos]) --pos;
    pos++;

    //std::cout << "lo " << lo << " hi " << hi << " pos " << pos << "\n";
    assert(lo == pos);
#endif

    size_t b = lo * 2;                               // < bucket
    if (lo < leaves && splitter[lo] == key) b += 1;  // equal bucket

    return b;
}

/// Variant 1 of string sample-sort: use binary search on splitters, no caching.
void sample_sortBS(string* strings, size_t n, size_t depth)
{
#if 0
    static const size_t leaves = 32;
#else
    //static const size_t l2cache = 256*1024;

    // bounding equations:
    // splitters            + bktsize
    // n * sizeof(key_type) + (2*n+1) * sizeof(size_t) <= l2cache

    static const size_t leaves =
        (l2cache - sizeof(size_t)) / (sizeof(key_type) + 2 * sizeof(size_t));

#endif

    if (n < g_samplesort_smallsort)
    {
        return sample_sort_small_sort(strings, n, depth);
    }

    //std::cout << "leaves: " << leaves << "\n";

    // step 1: select splitters with oversampling

    size_t samplesize = oversample_factor * leaves;

    key_type* samples = new key_type[samplesize];

    LCGRandom rng(&samples);

    for (unsigned int i = 0; i < samplesize; ++i)
    {
        samples[i] = get_char<key_type>(strings[rng() % n], depth);
    }

    std::sort(samples, samples + samplesize);

    key_type splitter[leaves];
    unsigned char splitter_lcp[leaves];

    splitter_lcp[0] = 0; // sentinel for first < everything bucket
    for (size_t i = 0, j = oversample_factor / 2; i < leaves; ++i)
    {
        splitter[i] = samples[j];

        if (i != 0) {
            key_type xorSplit = splitter[i - 1] ^ splitter[i];

            splitter_lcp[i] = count_high_zero_bits(xorSplit) / 8;
        }

        j += oversample_factor;
    }

    delete[] samples;

    // step 2: classify all strings and count bucket sizes

    static const size_t bktnum = 2 * leaves + 1;

    size_t* bktsize = new size_t[bktnum];
    memset(bktsize, 0, bktnum * sizeof(size_t));

    for (size_t si = 0; si < n; ++si)
    {
        // binary search in splitter with equal check
        key_type key = get_char<key_type>(strings[si], depth);

        unsigned int b = find_bkt_binsearch(key, splitter, leaves);

        assert(b < bktnum);

        ++bktsize[b];
    }

    // step 3: prefix sum

    size_t bktindex[bktnum];
    bktindex[0] = bktsize[0];
    size_t last_bkt_size = bktsize[0];
    for (unsigned int i = 1; i < bktnum; ++i) {
        bktindex[i] = bktindex[i - 1] + bktsize[i];
        if (bktsize[i]) last_bkt_size = bktsize[i];
    }
    assert(bktindex[bktnum - 1] == n);

    // step 4: premute in-place

    for (size_t i = 0, j; i < n - last_bkt_size; )
    {
        string perm = strings[i];
        key_type key;
        unsigned int b;

        while (1)
        {
            key = get_char<key_type>(perm, depth);
            b = find_bkt_binsearch(key, splitter, leaves);

            j = --bktindex[b];

            if (j <= i)
                break;

            std::swap(perm, strings[j]);
        }

        strings[i] = perm;
        i += bktsize[b];
    }

    // step 5: recursion

    size_t i = 0, bsum = 0;
    while (i < bktnum - 1)
    {
        // i is even -> bkt[i] is less-than bucket
        if (bktsize[i] > 1)
        {
            if (!g_toplevel_only)
                sample_sortBS(strings + bsum, bktsize[i],
                              depth + splitter_lcp[i / 2]);
        }
        bsum += bktsize[i++];

        // i is odd -> bkt[i] is equal bucket
        if (bktsize[i] > 1)
        {
            if ((splitter[i / 2] & 0xFF) == 0) {
                // equal-bucket has NULL-terminated key, done.
            }
            else {
                if (!g_toplevel_only)
                    sample_sortBS(strings + bsum, bktsize[i],
                                  depth + sizeof(key_type));
            }
        }
        bsum += bktsize[i++];
    }
    if (bktsize[i] > 0)
    {
        if (!g_toplevel_only)
            sample_sortBS(strings + bsum, bktsize[i], depth);
    }
    bsum += bktsize[i++];
    assert(i == bktnum && bsum == n);

    delete[] bktsize;
}

void bingmann_sample_sortBS(string* strings, size_t n)
{
    sample_sortBS(strings, n, 0);
}

// ----------------------------------------------------------------------------

} // namespace bingmann_sample_sortBS

/******************************************************************************/
