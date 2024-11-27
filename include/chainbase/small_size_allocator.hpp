#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <functional>
#include <mutex>

namespace chainbase {

namespace detail {

template <class backing_allocator, std::size_t sz>
class allocator {
   backing_allocator& _back_alloc;

   allocator(backing_allocator& back_alloc) :
      _back_alloc(back_alloc) {
   }

public:
   using pointer = backing_allocator::pointer;

   pointer allocate();
   void deallocate(pointer p);
};

} // namespace detail

// ---------------------------------------------------------------------------------------
//          An array of 64 allocators for sizes from 8 to 512 bytes
//          -------------------------------------------------------
//
//  All pointers used are of type `backing_allocator::pointer`
// ---------------------------------------------------------------------------------------
template <class backing_allocator, size_t num_allocators = 64, size_t size_increment = 8> 
class small_size_allocator {
public:
   using pointer = backing_allocator::pointer;

private:
   static constexpr size_t _mask = size_increment - 1;
   
   using alloc_tuple_t = decltype(make_slab_helper(1, std::make_index_sequence<num_allocators>{}));
   using alloc_fn_t    = std::function<pointer()>;
   using dealloc_fn_t  = std::function<void(const pointer&)>;

   alloc_tuple_t                            _allocators;
   std::array<alloc_fn_t, num_allocators>   _alloc_functions;  // using arrays of functions for fast access
   std::array<dealloc_fn_t, num_allocators> _dealloc_functions;
   backing_allocator&                       _back_alloc;

public:
   static constexpr size_t max_size = num_allocators * size_increment;

   small_size_allocator(backing_allocator& back_alloc)
      : _allocators(       make_alloc_tuple(back_alloc, std::make_index_sequence<num_allocators>{}))
      , _alloc_functions(  make_alloc_fn_array(std::make_index_sequence<num_allocators>{}))
      , _dealloc_functions(make_dealloc_fn_array(std::make_index_sequence<num_allocators>{}))
      , _back_alloc(back_alloc)
   {}

   pointer allocate(std::size_t sz) {
      if (sz <= max_size)
         return _alloc_functions[allocator_index(sz)]();
      return _back_alloc.allocate(sz);
   }

   void deallocate(const pointer &p, std::size_t sz) {
      if (sz <= max_size)
         _dealloc_functions[allocator_index(sz)](p);
      _back_alloc.deallocate(p, sz);
   }

private:
   template <std::size_t... I>
   static constexpr auto make_alloc_tuple(backing_allocator& back_alloc, std::index_sequence<I...>) {
      return std::tuple{new detail::allocator<backing_allocator, (I + 1) * size_increment>(back_alloc)...};
   }

   template <std::size_t... I>
   constexpr auto make_alloc_fn_array(std::index_sequence<I...>) {
      return std::array{std::function{[&allocator = std::get<I>(_allocators)] { return allocator->allocate(); }}...};
   }

   template <std::size_t... I>
   constexpr auto make_dealloc_fn_array(std::index_sequence<I...>) {
      return std::array{std::function{[&allocator = std::get<I>(_allocators)](pointer p) { allocator->deallocate(p); }}...};
   }

   static constexpr size_t allocator_index(size_t sz) { return (sz + _mask) & ~_mask; }
};

} // namespace chainbase
