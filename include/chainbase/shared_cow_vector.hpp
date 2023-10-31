#pragma once
#include <boost/interprocess/offset_ptr.hpp>
#include <boost/interprocess/allocators/allocator.hpp>

#include <cstddef>
#include <cstring>
#include <algorithm>
#include <string>
#include <optional>

#include <chainbase/pinnable_mapped_file.hpp>

namespace chainbase {
   namespace bip = boost::interprocess;

   template<typename T>
   class shared_cow_vector {
      struct impl {
         uint32_t reference_count;
         uint32_t size;
         T data[0];
      };

   public:
      using allocator_type = bip::allocator<char, segment_manager>;
      using iterator       = const T*;
      using const_iterator = const T*;

      explicit shared_cow_vector() = default;

      template<typename Iter>
      explicit shared_cow_vector(Iter begin, Iter end) {
         std::size_t size = std::distance(begin, end);
         _alloc<false>(&*begin, size, size);
      }

      explicit shared_cow_vector(const T* ptr, std::size_t size) {
         _alloc<false>(ptr, size, size);
      }

      shared_cow_vector(const shared_cow_vector& other) {
         if (get_allocator(this) == other.get_allocator()) {
            _data = other._data;
            if (_data != nullptr)
               ++_data->reference_count;
         } else {
            std::construct_at(this, other.data(),  other.size());
         }
      }

      shared_cow_vector(shared_cow_vector&& other) noexcept {
         if (get_allocator() == other.get_allocator()) {
            _data = other._data;
            other._data = nullptr;
         } else {
            std::construct_at(this, other.data(),  other.size());
         }
      }

      template<class I>
      explicit shared_cow_vector(std::initializer_list<I> init) {
         clear_and_construct(init.size(), 0, [&](void* dest, std::size_t idx) {
            new (dest) T(init[idx]);
         });
      }

      explicit shared_cow_vector(const std::vector<T>& v) {
         clear_and_construct(v.size(), 0, [&](void* dest, std::size_t idx) {
            new (dest) T(v[idx]);
         });
      }

      shared_cow_vector& operator=(const shared_cow_vector& other) {
         if (this != &other) {
            if (get_allocator() == other.get_allocator()) {
               dec_refcount();
               _data = other._data;
               if (_data != nullptr) 
                  ++_data->reference_count;
            } else {
               assign(other.data(), other.size());
            }
         }
         return *this;
      }

      shared_cow_vector& operator=(shared_cow_vector&& other) noexcept {
         if (this != &other) {
            if (get_allocator() == other.get_allocator()) {
               dec_refcount();
               _data = other._data;
               other._data = nullptr;
            } else {
               clear_and_construct(other.size(), 0, [&](void* dest, std::size_t idx) {
                  new (dest) T(std::move(other[idx]));
               });
            }
         }
         return *this;
      }

      ~shared_cow_vector() {
         dec_refcount();
         _data = nullptr;
      }

      void clear() {
         dec_refcount();
         _data = nullptr;
      }

      template<typename F>
      void clear_and_construct(std::size_t new_size, std::size_t copy_size, F&& f) {
         assert(copy_size <= new_size);
         assert(copy_size == 0 || (_data && copy_size <= _data->size));
         
         if (_data && _data->reference_count == 1 && _data->size == new_size)
            std::destroy(_data->data + copy_size, _data->data + new_size);
         else {
            _alloc<false>(data(), new_size, copy_size); // construct == false => uninitialized memory
         }
         for (std::size_t i=copy_size; i<new_size; ++i)
            static_cast<F&&>(f)(_data->data + i, i); // `f` should construct objects in place 
      }

      void assign(const T* ptr, std::size_t size) {
         if (_data && _data->reference_count == 1 && _data->size == size)
            std::copy(ptr, ptr + size, data());
         else {
            _alloc<false>(ptr, size, size);
         }
      }

      void assign(const std::vector<T>& v) {
         assign(v.data(), v.size());
      }

      void push_back(const T& o) {
         clear_and_construct(size() + 1, size(), [&](void* dest, std::size_t idx) {
            new (dest) T(o);
         });
      }

      const T* data() const {
         return _data ? _data->data : nullptr;
      }

      T* data() {
         return _data ? _data->data : nullptr;
      }

      std::size_t size() const {
         return _data ? _data->size : 0;
      }

      bool empty() const {
         return size() == 0;
      }

      const_iterator begin() const { return data(); }

      const_iterator end() const {
         return _data ? _data->data + _data->size : nullptr;
      }

      const_iterator cbegin() const { return begin(); }
      const_iterator cend()   const { return end(); }

      const T& operator[](std::size_t idx) const { assert(_data); return _data->data[idx]; }

      bool operator==(const shared_cow_vector& rhs) const {
        return size() == rhs.size() && std::memcmp(data(), rhs.data(), size() * sizeof(T)) == 0;
      }

      bool operator!=(const shared_cow_vector& rhs) const { return !(*this == rhs); }

      static std::optional<allocator_type> get_allocator(void* obj) {
         return pinnable_mapped_file::get_allocator<char>(obj);
      }      

      const std::optional<allocator_type> get_allocator() const {
         return get_allocator((void *)this);
      }

    private:
      template<class Alloc>
      void dec_refcount(Alloc&& alloc) {
         if (_data && --_data->reference_count == 0) {
            assert(_data->size);                                    // if size == 0, _data should be nullptr
            std::destroy(_data->data, _data->data + _data->size);
            std::forward<Alloc>(alloc).deallocate((char*)&*_data, sizeof(impl) + (_data->size * sizeof(T)));
         }
      }

      void dec_refcount() {
         auto alloc = get_allocator(this);
         if (alloc)
            dec_refcount(*alloc);
         else
            dec_refcount(std::allocator<char>());
      }

      template<bool construct, class Alloc>
      void _alloc(Alloc&& alloc, const T* ptr, std::size_t size, std::size_t copy_size) {
         impl* new_data = nullptr;
         if (size > 0) {
            new_data = (impl*)&*std::forward<Alloc>(alloc).allocate(sizeof(impl) + (size * sizeof(T)));
            new_data->reference_count = 1;
            new_data->size = size;
            if (ptr && copy_size) {
               assert(copy_size <= size);
               std::uninitialized_copy(ptr, ptr + copy_size, new_data->data);
            }
            if constexpr (construct) {
               // construct objects that were not copied
               assert(ptr || copy_size == 0);
               for (std::size_t i=copy_size; i<size; ++i)
                  new (new_data->data + i) T();
            }
         }
         dec_refcount(std::forward<Alloc>(alloc)); // has to be after copy above
         _data = new_data;
      }
      
      template<bool construct>
      void _alloc(const T* ptr, std::size_t size, std::size_t copy_size) {
         auto alloc = get_allocator(this);
         if (alloc)
            _alloc<construct>(*alloc, ptr, size, copy_size);
         else
            _alloc<construct>(std::allocator<char>(), ptr, size, copy_size);
      }
      
      bip::offset_ptr<impl> _data { nullptr };
   };

}