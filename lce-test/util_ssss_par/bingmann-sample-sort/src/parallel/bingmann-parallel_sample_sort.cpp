/*******************************************************************************
 * src/parallel/bingmann-parallel_sample_sort.cpp
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

#include "bingmann-parallel_sample_sort.hpp"

namespace bingmann_parallel_sample_sort {

/******************************************************************************/
// Parallel Sample Sort Instantiations

static inline void
parallel_sample_sortBTCUI(string* strings, size_t n)
{
    parallel_sample_sort_base<
        bingmann_sample_sort::ClassifyTreeUnrollInterleaveX>(
        UCharStringSet(strings, strings + n), 0);
}

static inline void
parallel_sample_sortBTCUI_out(string* strings, size_t n)
{
    string* output = new string[n];

    parallel_sample_sort_out_base<
        bingmann_sample_sort::ClassifyTreeUnrollInterleaveX>(
        UCharStringSet(strings, strings + n),
        UCharStringSet(output, output + n), 0);

    // copy back for verification
    memcpy(strings, output, n * sizeof(string));
    delete[] output;
}

/*----------------------------------------------------------------------------*/

static inline void
parallel_sample_sortBTCEUA(string* strings, size_t n)
{
    parallel_sample_sort_base<
        bingmann_sample_sort::ClassifyEqualUnrollAssembler>(
        UCharStringSet(strings, strings + n), 0);
}

/*----------------------------------------------------------------------------*/

static inline void
parallel_sample_sortBTCTUI(string* strings, size_t n)
{
    parallel_sample_sort_base<
        bingmann_sample_sort::ClassifyTreeCalcUnrollInterleaveX>(
        UCharStringSet(strings, strings + n), 0);
}

/******************************************************************************/
// Parallel Sample Sort with LCP Instantiations

static inline void
parallel_sample_sortBTCUI_lcp(string* strings, size_t n)
{
    parallel_sample_sort_lcp_base<
        bingmann_sample_sort::ClassifyTreeUnrollInterleaveX>(
        UCharStringSet(strings, strings + n), 0);
}

/*----------------------------------------------------------------------------*/

static inline void
parallel_sample_sortBTCEU_lcp(string* strings, size_t n)
{
    parallel_sample_sort_lcp_base<
        bingmann_sample_sort::ClassifyEqualUnrollAssembler>(
        UCharStringSet(strings, strings + n), 0);
}

/*----------------------------------------------------------------------------*/

static inline void
parallel_sample_sortBTCTUI_lcp(string* strings, size_t n)
{
    parallel_sample_sort_lcp_base<
        bingmann_sample_sort::ClassifyTreeCalcUnrollInterleaveX>(
        UCharStringSet(strings, strings + n), 0);
}

} // namespace bingmann_parallel_sample_sort

/******************************************************************************/
