#pragma once
#include <chainbase/pinnable_mapped_file.hpp>
#include <iomanip>

namespace chainbase {

constexpr size_t header_size = 1024;
constexpr uint64_t header_id = 0x3242444f49534f45ULL; //"EOSIODB2" little endian

struct environment  {
   environment() {
#if (defined(__GNUC__) || defined(__clang_major__)) && (defined(_LIBCPP_VERSION) || defined(_GLIBCXX_RELEASE))
      std::string s;
#ifdef __clang_major__
	s = "Clang " + std::to_string(__clang_major__) + " ";
#else
	s = "GCC " + std::to_string(__GNUC__) + " ";
#endif
#ifdef _GLIBCXX_RELEASE
	s += "libstdc++ " + std::to_string(_GLIBCXX_RELEASE);
#else
	s += "libc++ " + std::to_string(_LIBCPP_VERSION/10000);
#endif
      strncpy(compiler, s.c_str(), sizeof(compiler)-1);
#else
      strncpy(compiler, __VERSION__, sizeof(compiler)-1);
#endif
   }

   enum os_t : unsigned char {
      OS_LINUX,
      OS_MACOS,
      OS_WINDOWS,
      OS_OTHER
   };
   enum arch_t : unsigned char {
      ARCH_X86_64,
      ARCH_ARM,
      ARCH_RISCV,
      ARCH_OTHER
   };

   bool debug =
#ifndef NDEBUG
      true;
#else
      false;
#endif
   os_t os =
#if defined(__linux__)
      OS_LINUX;
#elif defined(__APPLE__)
      OS_MACOS;
#elif defined(_WIN32)
      OS_WINDOWS;
#else
      OS_OTHER;
#endif
   arch_t arch =
#if defined(__x86_64__)
      ARCH_X86_64;
#elif defined(__aarch64__)
      ARCH_ARM;
#elif defined(__riscv__)
      ARCH_RISCV;
#else
      ARCH_OTHER;
#endif

   unsigned boost_version = BOOST_VERSION;
   uint8_t reserved[512] = {};
   char compiler[256] = {};

   bool operator==(const environment& other) const {
      return !memcmp(this, &other, sizeof(environment));
   } 
   bool operator!=(const environment& other) const {
      return !(*this == other);
   }
} __attribute__ ((packed));

struct db_header  {
   uint64_t id = header_id;
   bool dirty = false;
   environment dbenviron;
} __attribute__ ((packed));

constexpr size_t header_dirty_bit_offset = offsetof(db_header, dirty);

static_assert(sizeof(db_header) <= header_size, "DB header struct too large");

std::ostream& operator<<(std::ostream& os, const chainbase::environment& dt);

}