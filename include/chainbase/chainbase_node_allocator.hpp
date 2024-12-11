#pragma once

#include <cstddef>
#include <boost/interprocess/offset_ptr.hpp>

#include <chainbase/pinnable_mapped_file.hpp>

namespace chainbase {

   namespace bip = boost::interprocess;

   template<typename T, typename S>
   class chainbase_node_allocator {
    public:
      using value_type = T;
      using pointer = bip::offset_ptr<T>;

      chainbase_node_allocator(segment_manager* manager) : _manager{manager} {
         _ss_alloc = pinnable_mapped_file::get_small_size_allocator((std::byte*)manager);
      }

      chainbase_node_allocator(const chainbase_node_allocator& other) : chainbase_node_allocator(&*other._manager) {}

      template<typename U>
      chainbase_node_allocator(const chainbase_node_allocator<U, S>& other) : chainbase_node_allocator(&*other._manager) {}

      pointer allocate(std::size_t num) {
         if (num == 1) {
            if (_freelist == nullptr) {
               get_some(allocation_batch_size);
            }
            list_item* result = &*_freelist;
            _freelist = _freelist->_next;
            result->~list_item();
            --_freelist_size;
            return pointer{(T*)result};
         } else {
            return pointer{(T*)&*_ss_alloc->allocate(num*sizeof(T))};
         }
      }

      void deallocate(const pointer& p, std::size_t num) {
         if (num == 1) {
            _freelist = new (&*p) list_item{_freelist};
            ++_freelist_size;
         } else {
            _ss_alloc->deallocate(ss_allocator_t::pointer((char*)&*p), num*sizeof(T));
         }
      }

      void preallocate(std::size_t num) {
         if (num >= 2 * allocation_batch_size)
            get_some(((num - _freelist_size) + 7) & ~7);
      }

      bool operator==(const chainbase_node_allocator& other) const { return this == &other; }
      bool operator!=(const chainbase_node_allocator& other) const { return this != &other; }
      segment_manager* get_segment_manager() const { return _manager.get(); }
      size_t freelist_memory_usage() const { return _freelist_size * sizeof(T); }

    private:
      template<typename T2, typename S2>
      friend class chainbase_node_allocator;

      void get_some(size_t allocation_batch_size) {
         static_assert(sizeof(T) >= sizeof(list_item), "Too small for free list");
         static_assert(sizeof(T) % alignof(list_item) == 0, "Bad alignment for free list");

         char* result = (char*)_manager->allocate(sizeof(T) * allocation_batch_size);
         _freelist_size += allocation_batch_size;
         auto old_freelist = _freelist;
         _freelist = bip::offset_ptr<list_item>{(list_item*)result};
         for(unsigned i = 0; i < allocation_batch_size-1; ++i) {
            char* next = result + sizeof(T);
            new(result) list_item{bip::offset_ptr<list_item>{(list_item*)next}};
            result = next;
         }
         new(result) list_item{old_freelist};
      }

      struct list_item { bip::offset_ptr<list_item> _next; };

      static constexpr size_t allocation_batch_size = 512;
      bip::offset_ptr<ss_allocator_t>  _ss_alloc;
      bip::offset_ptr<segment_manager> _manager;
      bip::offset_ptr<list_item>       _freelist{};
      size_t                           _freelist_size = 0;
   };

}  // namepsace chainbase
