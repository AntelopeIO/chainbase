#pragma once
#include <cstdint>
#include <memory>

namespace chainbase {

// forward decl
// ------------
class pinnable_mapped_file;
class mem_visualizer_impl;

// ----------------------
class mem_visualizer {
public:
   mem_visualizer(pinnable_mapped_file& pmf, uint64_t shared_file_size);
   ~mem_visualizer();

private:
   std::unique_ptr<mem_visualizer_impl> my;
};

} // chainbase