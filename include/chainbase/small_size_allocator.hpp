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

   virtual pointer allocate()                   = 0;
   virtual void    deallocate(const pointer& p) = 0;
   virtual size_t  freelist_memory_usage()      = 0;
   virtual size_t  num_blocks_allocated()       = 0;
};

template <class backing_allocator, std::size_t sz>
class allocator : public allocator_base<backing_allocator> {
public:
   using T = std::array<char, sz>;
   using pointer = backing_allocator::pointer;

   allocator(backing_allocator back_alloc) :
      _back_alloc(back_alloc) {
   }

   pointer allocate() final {
      std::lock_guard g(_m);
      if (_freelist == nullptr) {
         get_some();
      }
      list_item* result = &*_freelist;
      _freelist         = _freelist->_next;
      result->~list_item();
      --_freelist_size;
      return pointer{(typename backing_allocator::value_type*)result};
   }

   void deallocate(const pointer& p) final {
      std::lock_guard g(_m);
      _freelist = new (&*p) list_item{_freelist};
      ++_freelist_size;
   }

   size_t freelist_memory_usage() final {
      std::lock_guard g(_m);
      return _freelist_size * sizeof(T);
   }

   size_t num_blocks_allocated() final {
      std::lock_guard g(_m);
      return _num_blocks_allocated;
   }

private:
   struct list_item { bip::offset_ptr<list_item> _next; };
   static constexpr size_t allocation_batch_size = 512;

   void get_some() {
      static_assert(sizeof(T) >= sizeof(list_item), "Too small for free list");
      static_assert(sizeof(T) % alignof(list_item) == 0, "Bad alignment for free list");

      char* result = (char*)&*_back_alloc.allocate(sizeof(T) * allocation_batch_size);
      _freelist_size += allocation_batch_size;
      ++_num_blocks_allocated;
      _freelist = bip::offset_ptr<list_item>{(list_item*)result};
      for (unsigned i = 0; i < allocation_batch_size - 1; ++i) {
         char* next = result + sizeof(T);
         new (result) list_item{bip::offset_ptr<list_item>{(list_item*)next}};
         result = next;
      }
      new (result) list_item{nullptr};
   }

   backing_allocator          _back_alloc;
   bip::offset_ptr<list_item> _freelist;
   size_t                     _freelist_size        = 0;
   size_t                     _num_blocks_allocated = 0; // number of blocks allocated from boost segment allocator
   std::mutex                 _m;
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
requires ((size_increment & (size_increment - 1)) == 0) // power of two
class small_size_allocator {
public:
   using pointer = backing_allocator::pointer;

private:
   using base_alloc_ptr = bip::offset_ptr<detail::allocator_base<backing_allocator>>;

   backing_allocator                          _back_alloc;
   std::array<base_alloc_ptr, num_allocators> _allocators;

   static constexpr size_t max_size = num_allocators * size_increment;

   static constexpr size_t allocator_index(size_t sz_in_bytes) {
      assert(sz_in_bytes > 0);
      return (sz_in_bytes - 1) / size_increment;
   }

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
      if (sz_in_bytes <= max_size) {
         return _allocators[allocator_index(sz_in_bytes)]->allocate();
      }
      return _back_alloc.allocate(sz_in_bytes);
   }

   void deallocate(const pointer& p, std::size_t sz_in_bytes) {
      if (sz_in_bytes <= max_size) {
         _allocators[allocator_index(sz_in_bytes)]->deallocate(p);
      } else
         _back_alloc.deallocate(p, sz_in_bytes);
   }

   size_t freelist_memory_usage() const {
      size_t sz = 0;
      for (auto& alloc : _allocators)
         sz += alloc->freelist_memory_usage();
      return sz;
   }

   size_t num_blocks_allocated() const {
      size_t sz = 0;
      for (auto& alloc : _allocators)
         sz += alloc->num_blocks_allocated();
      return sz;
   }

};


// ---------------------------------------------------------------------------------------
//          Object allocator
//          ----------------
//
//  emulates the API of `bip::allocator<T, segment_manager>`
//
//  backing_allocator is `the small_size_allocator`
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
      assert(p != nullptr);
      return _back_alloc->deallocate(char_pointer(static_cast<char*>(static_cast<void*>(p.get()))), num_objects * sizeof(T));
   }

   bool operator==(const object_allocator&) const = default;
   
private:
   bip::offset_ptr<backing_allocator> _back_alloc; // allocates by size in bytes
};

} // namespace chainbase
