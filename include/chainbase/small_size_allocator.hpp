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
public:
   backing_allocator& _back_alloc;

   allocator(backing_allocator* back_alloc) :
      _back_alloc(*back_alloc) {
   }

   using pointer = backing_allocator::pointer;

   pointer allocate();
   
   void deallocate(const pointer& p);

private:
   std::mutex _m;  // must be thread-safe
};

} // namespace detail


// ---------------------------------------------------------------------------------------
//          An array of 64 allocators for sizes from 8 to 512 bytes
//          -------------------------------------------------------
//
//  All pointers used are of type `backing_allocator::pointer`
//  allocate/deallocate specify size in bytes.
// ---------------------------------------------------------------------------------------
template <class backing_allocator, size_t num_allocators = 64, size_t size_increment = 8> 
class small_size_allocator {
public:
   using pointer = backing_allocator::pointer;

private:
   template <std::size_t... I>
   static constexpr auto make_alloc_tuple(backing_allocator* back_alloc, std::index_sequence<I...>) {
      return std::tuple{new detail::allocator<backing_allocator, (I + 1) * size_increment>(back_alloc)...};
   }

   static_assert(sizeof(typename backing_allocator::value_type) == 1, "backing_allocator should be allocating bytes");
   
   static constexpr size_t _mask = size_increment - 1;
   
   using alloc_tuple_t = decltype(make_alloc_tuple(nullptr, std::make_index_sequence<num_allocators>{}));
   using alloc_fn_t    = std::function<pointer()>;
   using dealloc_fn_t  = std::function<void(const pointer&)>;

   // we store a constructed `bip::allocator` in _back_alloc
   // all `bip::allocator` constructed from the same `segment_manager` are equivalent
   backing_allocator                        _back_alloc;
   
   alloc_tuple_t                            _allocators;
   std::array<alloc_fn_t, num_allocators>   _alloc_functions;  // using arrays of functions for fast access
   std::array<dealloc_fn_t, num_allocators> _dealloc_functions;

public:
   static constexpr size_t max_size = num_allocators * size_increment;

   small_size_allocator(backing_allocator back_alloc)
      : _back_alloc(std::move(back_alloc))
      , _allocators(       make_alloc_tuple(&_back_alloc, std::make_index_sequence<num_allocators>{}))
      , _alloc_functions(  make_alloc_fn_array(std::make_index_sequence<num_allocators>{}))
      , _dealloc_functions(make_dealloc_fn_array(std::make_index_sequence<num_allocators>{}))
   {}

   pointer allocate(std::size_t sz_in_bytes) {
      if (0 && sz_in_bytes <= max_size)
         return _alloc_functions[allocator_index(sz_in_bytes)]();
      return _back_alloc.allocate(sz_in_bytes);
   }

   void deallocate(const pointer& p, std::size_t sz_in_bytes) {
      if (0 && sz_in_bytes <= max_size)
         _dealloc_functions[allocator_index(sz_in_bytes)](p);
      _back_alloc.deallocate(p, sz_in_bytes);
   }

private:
   template <std::size_t... I>
   constexpr auto make_alloc_fn_array(std::index_sequence<I...>) {
      return std::array{std::function{[&allocator = std::get<I>(_allocators)] { return allocator->allocate(); }}...};
   }

   template <std::size_t... I>
   constexpr auto make_dealloc_fn_array(std::index_sequence<I...>) {
      return std::array{std::function{[&allocator = std::get<I>(_allocators)](const pointer& p) { allocator->deallocate(p); }}...};
   }

   static constexpr size_t allocator_index(size_t sz_in_bytes) { return (sz_in_bytes + _mask) & ~_mask; }
};


// ---------------------------------------------------------------------------------------
//          Object allocator
//          ----------------
//
//  emulates the API of `bip::allocator<T, segment_manager>`
// ---------------------------------------------------------------------------------------
template<typename T, class backing_allocator>
class object_allocator {
public:
   using pointer = backing_allocator::pointer;
   using value_type = T;
   
   object_allocator(backing_allocator* back_alloc) :_back_alloc(back_alloc) {
   }

   pointer allocate(std::size_t num_objects) {
      return _back_alloc->allocate(num_objects * sizeof(T));
   }

   void deallocate(const pointer& p, std::size_t num_objects) {
      return _back_alloc->deallocate(p, num_objects * sizeof(T));
   }

   bool operator==(const object_allocator&) const = default;
   
private:
   backing_allocator* _back_alloc; // allocates by size in bytes
};

} // namespace chainbase
