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
#include <boost/interprocess/offset_ptr.hpp>

namespace bip = boost::interprocess;

namespace chainbase {

namespace detail {

template <class backing_allocator>
class allocator_base {
public:
   using pointer = backing_allocator::pointer;
   virtual pointer allocate() = 0;
   virtual void deallocate(const pointer& p) = 0;
};

template <class backing_allocator, std::size_t sz>
class allocator : public allocator_base<backing_allocator> {
public:
   allocator(backing_allocator back_alloc) :
      _back_alloc(back_alloc) {
   }

   using pointer = backing_allocator::pointer;

   pointer allocate() final {
      std::lock_guard g(_m);
      return pointer(nullptr);
   }
   
   void deallocate(const pointer& p) final {
      std::lock_guard g(_m);
   }

private:
   backing_allocator _back_alloc;
   std::mutex _m;
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
   using base_alloc_ptr = bip::offset_ptr<detail::allocator_base<backing_allocator>>;

   backing_allocator                          _back_alloc;
   std::array<base_alloc_ptr, num_allocators> _allocators;

   static constexpr size_t mask     = size_increment - 1;
   static constexpr size_t max_size = num_allocators * size_increment;
   
   static constexpr size_t allocator_index(size_t sz_in_bytes) { return (sz_in_bytes + mask) & ~mask; }

   template <std::size_t... I>
   auto make_allocators(backing_allocator back_alloc, std::index_sequence<I...>) {
      return std::array<base_alloc_ptr, num_allocators>{
         new (&*_back_alloc.allocate(sizeof(detail::allocator<backing_allocator, (I + 1) * size_increment>)))
            detail::allocator<backing_allocator, (I + 1) * size_increment>(back_alloc)...};
   }

public:
   small_size_allocator(backing_allocator back_alloc)
      : _back_alloc(std::move(back_alloc))
      , _allocators(make_allocators(back_alloc, std::make_index_sequence<num_allocators>{})) {}

   pointer allocate(std::size_t sz_in_bytes) {
      if (0 && sz_in_bytes <= max_size)
         return _allocators[allocator_index(sz_in_bytes)]->allocate();
      return _back_alloc.allocate(sz_in_bytes);
   }

   void deallocate(const pointer& p, std::size_t sz_in_bytes) {
      if (0 && sz_in_bytes <= max_size)
         _allocators[allocator_index(sz_in_bytes)]->deallocate(p);
      _back_alloc.deallocate(p, sz_in_bytes);
   }
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
   using char_pointer = backing_allocator::pointer;
   using pointer      = char_pointer::template rebind<T>;
   using value_type   = T;

   object_allocator(backing_allocator* back_alloc) :_back_alloc(back_alloc) {
   }

   pointer allocate(std::size_t num_objects) {
      return pointer(static_cast<T*>(static_cast<void*>(_back_alloc->allocate(num_objects * sizeof(T)).get())));
   }

   void deallocate(const pointer& p, std::size_t num_objects) {
      return _back_alloc->deallocate(char_pointer(static_cast<char*>(static_cast<void*>(p.get()))), num_objects * sizeof(T));
   }

   bool operator==(const object_allocator&) const = default;
   
private:
   bip::offset_ptr<backing_allocator> _back_alloc; // allocates by size in bytes
};

} // namespace chainbase
