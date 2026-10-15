#pragma once

// std::sort as the game's libstdc++ does it (the Mac binary's GCC: the SGI
// introsort, read in cTSWantSimulator::PrioritizeGoals' instantiation, BV
// 0x16b5024 / __introsort_loop 0x16b4e5c / __final_insertion_sort
// 0x16b44a4; docs/wants-mac.md §9), so that elements the predicate cannot
// tell apart end up where the game puts them: depth limit 2 floor(log2 n),
// pivot = median of (first, middle, last - 1), unguarded partition, heap
// sort (partial_sort) past the limit, then insertion sort (the first 16
// guarded, the rest unguarded). Values are copied as the original copies
// them.

#include <algorithm>
#include <iterator>

namespace os2::vm::gnu {

namespace detail {

template <class It, class T, class Comp> void unguardedLinearInsert(It last, T val, Comp comp) {
    It next = last;
    --next;
    while (comp(val, *next)) {
        *last = *next;
        last = next;
        --next;
    }
    *last = val;
}

template <class It, class Comp> void insertionSort(It first, It last, Comp comp) {
    if (first == last)
        return;
    for (It i = first + 1; i != last; ++i) {
        auto val = *i;
        if (comp(val, *first)) {
            std::copy_backward(first, i, i + 1);
            *first = val;
        } else {
            unguardedLinearInsert(i, val, comp);
        }
    }
}

template <class It, class Comp> void unguardedInsertionSort(It first, It last, Comp comp) {
    for (It i = first; i != last; ++i)
        unguardedLinearInsert(i, *i, comp);
}

template <class It, class Comp> void finalInsertionSort(It first, It last, Comp comp) {
    if (last - first > 16) {
        insertionSort(first, first + 16, comp);
        unguardedInsertionSort(first + 16, last, comp);
    } else {
        insertionSort(first, last, comp);
    }
}

template <class T, class Comp> const T& median(const T& a, const T& b, const T& c, Comp comp) {
    if (comp(a, b)) {
        if (comp(b, c))
            return b;
        if (comp(a, c))
            return c;
        return a;
    }
    if (comp(a, c))
        return a;
    if (comp(b, c))
        return c;
    return b;
}

template <class It, class T, class Comp> It unguardedPartition(It first, It last, const T& pivot, Comp comp) {
    for (;;) {
        while (comp(*first, pivot))
            ++first;
        --last;
        while (comp(pivot, *last))
            --last;
        if (!(first < last))
            return first;
        std::iter_swap(first, last);
        ++first;
    }
}

template <class It, class D, class T, class Comp> void pushHeap(It first, D hole, D top, T val, Comp comp) {
    D parent = (hole - 1) / 2;
    while (hole > top && comp(*(first + parent), val)) {
        *(first + hole) = *(first + parent);
        hole = parent;
        parent = (hole - 1) / 2;
    }
    *(first + hole) = val;
}

template <class It, class D, class T, class Comp> void adjustHeap(It first, D hole, D len, T val, Comp comp) {
    const D top = hole;
    D second = 2 * hole + 2;
    while (second < len) {
        if (comp(*(first + second), *(first + (second - 1))))
            --second;
        *(first + hole) = *(first + second);
        hole = second;
        second = 2 * (second + 1);
    }
    if (second == len) {
        *(first + hole) = *(first + (second - 1));
        hole = second - 1;
    }
    pushHeap(first, hole, top, val, comp);
}

template <class It, class Comp> void makeHeap(It first, It last, Comp comp) {
    using D = typename std::iterator_traits<It>::difference_type;
    const D len = last - first;
    if (len < 2)
        return;
    for (D parent = (len - 2) / 2;; --parent) {
        auto val = *(first + parent);
        adjustHeap(first, parent, len, val, comp);
        if (parent == 0)
            return;
    }
}

template <class It, class T, class Comp> void popHeap(It first, It last, It result, T val, Comp comp) {
    using D = typename std::iterator_traits<It>::difference_type;
    *result = *first;
    adjustHeap(first, D(0), D(last - first), val, comp);
}

template <class It, class Comp> void partialSort(It first, It middle, It last, Comp comp) {
    makeHeap(first, middle, comp);
    for (It i = middle; i < last; ++i)
        if (comp(*i, *first)) {
            auto val = *i;
            popHeap(first, middle, i, val, comp);
        }
    while (middle - first > 1) {
        --middle;
        auto val = *middle;
        popHeap(first, middle, middle, val, comp);
    }
}

template <class It, class D, class Comp> void introsortLoop(It first, It last, D depth, Comp comp) {
    while (last - first > 16) {
        if (depth == 0) {
            partialSort(first, last, last, comp);
            return;
        }
        --depth;
        const auto pivot = median(*first, *(first + (last - first) / 2), *(last - 1), comp);
        const It cut = unguardedPartition(first, last, pivot, comp);
        introsortLoop(cut, last, depth, comp);
        last = cut;
    }
}

} // namespace detail

template <class It, class Comp> void sort(It first, It last, Comp comp) {
    if (first == last)
        return;
    using D = typename std::iterator_traits<It>::difference_type;
    D k = 0;
    for (D n = last - first; n != 1; n >>= 1)
        ++k;
    detail::introsortLoop(first, last, D(k * 2), comp);
    detail::finalInsertionSort(first, last, comp);
}

} // namespace os2::vm::gnu
