#include <boost/test/unit_test.hpp>
#include <chainbase/chainbase.hpp>

#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

#include <iostream>
#include <random>
#include <thread>
#include "temp_directory.hpp"

using namespace chainbase;
using namespace boost::multi_index;

namespace bip     = boost::interprocess;
using allocator_t = chainbase::allocator<char>;
using pointer_t   = typename allocator_t::pointer;

struct ptr_sz_t    { pointer_t ptr; size_t sz; };
struct offset_sz_t { size_t offset; size_t sz; };
struct segment_t   { size_t start;  size_t sz; };

// ----------------------------------------------------------------------------------------------
struct bucket {

   bucket(chainbase::allocator<char>& alloc, size_t sz) : _alloc(&alloc), _sz(sz) {}

   ptr_sz_t alloc() {
      _ptr = _alloc->allocate(_sz);
      return { _ptr, _sz };
   }

   ptr_sz_t free() {
      _alloc->deallocate(_ptr, _sz);
      return { _ptr, _sz };
   }

   size_t size() const {
      return _sz;
   }

   allocator_t* _alloc;
   pointer_t    _ptr;
   size_t       _sz;
};

// ----------------------------------------------------------------------------------------------
template <size_t total_num_pages, size_t allocated_pages,
          size_t window_width, size_t window_height,
          size_t page_width, size_t page_height,
          size_t border_width>
   requires(window_width % (page_width + border_width) == 0) && (window_height % (page_height + border_width) == 0) &&
           (4096 % (page_width * page_height) == 0)
struct Mapper {

   struct page_loc_t {
      size_t page_idx;
      size_t offset_in_page; // in bytes

      page_loc_t(size_t page_idx, size_t offset_in_page) : page_idx(page_idx), offset_in_page(offset_in_page) {}
      page_loc_t(size_t offset) : page_idx(offset / page_size), offset_in_page(offset % page_size) {}
   };

   struct pt_t { size_t x; size_t y; }; // pixels

   static constexpr size_t page_size = 4096;

   static constexpr size_t total_size() { return total_num_pages * page_size; }
   static constexpr size_t num_pixels() { return window_width * window_height; }
   static constexpr size_t page_full_width() { return page_width + border_width; }
   static constexpr size_t page_full_height() { return page_height + border_width; }
   static constexpr size_t num_pages_per_line() { return window_width / page_full_width(); }
   static constexpr size_t bytes_per_pixel() { return page_size / (page_width * page_height); }
   static constexpr size_t win_width() { return window_width; }
   static constexpr size_t win_height() { return window_height; }
   static constexpr size_t pg_width() { return page_width; }
   static constexpr size_t pg_height() { return page_height; }
   static constexpr size_t bd_width() { return border_width; }

   static constexpr pt_t pixel(page_loc_t page_loc) {
      return { (page_loc.page_idx % num_pages_per_line()) * page_full_width()  + (page_loc.offset_in_page / bytes_per_pixel()) % page_width,
               (page_loc.page_idx / num_pages_per_line()) * page_full_height() + (page_loc.offset_in_page / bytes_per_pixel()) / page_width };
   }

   // what size to allocate, if I want to fill 1 / num_sizes of the allocated pages with num_alloc buckets
   static constexpr size_t bucket_size(size_t num_alloc, size_t num_sizes) {
      return (allocated_pages * page_size) / num_sizes / num_alloc;
   }

   template <class DRAW_RECT_FN>
   static void draw(offset_sz_t offset_sz, const DRAW_RECT_FN& draw_rect_fn) {
      auto draw_full_page = [&](size_t p) {
         auto start_px = pixel(page_loc_t{p, 0});
         assert(start_px.x % page_full_width() == 0 && start_px.y % page_full_height() == 0); // because full page

         auto end_px = pixel(page_loc_t{p, page_size - 1});
         auto width  = end_px.x - start_px.x + 1;
         auto height = end_px.y - start_px.y + 1;
         assert(width == page_width && height == page_height); // because full page
         draw_rect_fn(start_px.x, start_px.y, width, height);
      };

      auto draw_partial_page = [&](page_loc_t start, page_loc_t end) {
         assert(start.page_idx == end.page_idx);
         auto start_px = pixel(start);
         auto end_px = pixel(end);
         assert(end_px.y >= start_px.y);
         if (end_px.y == start_px.y) {
            //std::cout << start_px.x << ", " << end_px.x << "  ---- " << '\n';
            draw_rect_fn(start_px.x, start_px.y, end_px.x - start_px.x + 1, 1);
         } else {
            auto bol = start_px.x - (start_px.x % page_full_width());
            auto remaining_width = bol + page_width - start_px.x;

            draw_rect_fn(start_px.x, start_px.y, remaining_width + 1, 1);                   // start to eol
            auto height = end_px.y - start_px.y + 1;
            if (height > 2)
               draw_rect_fn(bol, start_px.y + 1, page_width, height - 2);                   // draw rect between 1st and last line
            draw_rect_fn(bol, end_px.y, end_px.x - bol, 1);                                 // bol to end
         }
      };

      size_t first_byte = offset_sz.offset;
      size_t last_byte  = offset_sz.offset + offset_sz.sz - 1;

      auto start = page_loc_t{first_byte};
      auto end   = page_loc_t{last_byte};

      if (start.page_idx == end.page_idx)
         draw_partial_page(start, end);
      else {
         for (size_t p = start.page_idx; p <= end.page_idx; ++p) {
            if (p == start.page_idx)
               draw_partial_page(start, page_loc_t{p, page_size - 1});
            else if (p == end.page_idx)
               draw_partial_page(page_loc_t{p, 0}, end);
            else
               draw_full_page(p);
         }
      }
   }
};

#ifdef __unix__

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>

int error_handler(Display *dpy) {
   std::cout << "IO error\n";
   return 0;
}

// ----------------------------------------------------------------------------------------------
template<class Mapper>
struct GraphView {
   const Mapper& mapper;
   char*         segment_origin;

   Display*      d;
   int           s;   // screen
   Visual*       visual;
   Window        w;
   GC            gc;
   XImage*       img;
   size_t width = mapper.win_width();
   size_t height = mapper.win_height();

   GraphView(const Mapper& mapper, char *segment_origin) : mapper(mapper), segment_origin(segment_origin) {
      XInitThreads();

      d = XOpenDisplay(NULL);
      if (!d) {
         fprintf(stderr, "Cannot open display\n");
         exit(1);
      }
      s = DefaultScreen(d);
      XSetWindowAttributes attributes = {};
      w = XCreateWindow(d,  DefaultRootWindow(d), 10, 10, width, height, 0, 0, InputOutput,
                        CopyFromParent, 0, &attributes);
      XSelectInput(d, w, ExposureMask | KeyPressMask);
      XMapWindow(d, w);
      visual  = XDefaultVisual(d, 0);
      gc=XCreateGC(d, w, 0,0);
      _create_image();
      XSetIOErrorHandler(error_handler);
      XSynchronize(d, true);
   }

   ~GraphView() {
      std::this_thread::sleep_for(std::chrono::seconds{2500});
      XDestroyImage(img);
      XCloseDisplay(d);
   }

   void show(ptr_sz_t ptr_sz, bool black = true) {
      uintptr_t offset = (char *)ptr_sz.ptr.get() - segment_origin;
      mapper.draw({offset, ptr_sz.sz}, [&](size_t x, size_t y, size_t w, size_t h) {
         _draw_rect(x, y, w, h, black);
      });
      _put_image();
   }

   void hide(ptr_sz_t ptr_sz) {
      show(ptr_sz, false);
   }

   int process_events() {
      int res = 0;
      constexpr size_t max_events = 64;
      XEvent events[max_events];
      int cur_idx = 0;

      auto merge_events = [&](int a, int last_seen_idx) {
         if (events[cur_idx].type == a) {
            if (last_seen_idx != -1)
               events[last_seen_idx].type = 0;
            last_seen_idx = cur_idx;
         }
      };

      int configureIndex = -1, motionIndex = -1, exposeIndex = -1;
      while (cur_idx < max_events && XPending(d)) {
         XNextEvent(d, events + cur_idx);
         merge_events(ConfigureNotify, configureIndex);
         merge_events(MotionNotify, motionIndex);
         merge_events(Expose, exposeIndex);
         ++cur_idx;
      }

      for (int i = 0; i < cur_idx; ++i) {
         if (events[i].type) {
            auto ev = _process_event(events[i]);
            if (ev)
               res = ev;
         }
      }

      return res;
   }

private:

   void _draw_rect(size_t _x, size_t _y, size_t w, size_t h, bool black) {
      auto fw = mapper.pg_width() + mapper.bd_width();
      auto fh = mapper.pg_height() + mapper.bd_width();
      for (size_t y=_y; y<_y+h; ++y) {
         for (size_t x=_x; x<_x+w; ++x) {
            bool border = ((y % fh) > mapper.pg_height()) || ((x % fw) > mapper.pg_width());
            assert(!border);
            XPutPixel(img, x, y, black ? 0x0 : 0xFFFFFF);
         }
      }
   }

   void _create_image() {
      uint32_t* data = (uint32_t*)malloc(width * height * 4);
      img = XCreateImage(d, visual, 24, ZPixmap, 0, (char *)data, width, height, 32, 0);

      auto fw = mapper.pg_width() + mapper.bd_width();
      auto fh = mapper.pg_height() + mapper.bd_width();
      for (size_t y=0; y<height; ++y) {
         for (size_t x=0; x<width; ++x) {
            bool border = ((y % fh) > mapper.pg_height()) || ((x % fw) > mapper.pg_width());
            XPutPixel(img, x, y, border ? 0xAA5500 : 0xFFFFFF);
         }
      }
   }

   void _put_image() {
      XPutImage(d, w, gc, img, 0, 0, 0, 0, width, height);
      XSync(d, false);
   }

   int _process_event(const XEvent& event) {
      int res = 0;
      switch(event.type) {
      case Expose:
         //XPutImage(d, w, gc, img, 0, 0, 0, 0, width, height);
         break;

      case KeyPress:
         res = event.xkey.keycode;
         break;
      }
      return res;
   }
};

#else
template<class Mapper>
struct GraphView {
   GraphView(const Mapper& mapper, char *segment_origin) {}
   void show(ptr_sz_t ptr_sz) {)
   void hide(ptr_sz_t ptr_sz) {}
};
#endif


// ----------------------------------------------------------------------------------------------
//                   Visualize memory locality for allocator
//                   ---------------------------------------
//
// total memory available: 14,848 4k pages (128 x 116 grid, or 2304 x 2088 pixels)
// max memory used:         7,500 4k pages = 30,720 KB // (when using 4 counts, each count uses 1/4 or  7,680 KB)
//
// visualization:       window 3150 x 2088 pixel, 16x16 pixel represents 1 4k page, 15,000 pages
//
// random allocation/deallocation (10% more allocs than deallocs) until all buckets are allocated
// ----------------------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(locality) {
   using ThisMapper = Mapper<14'848, 7'500, 2304, 2088, 16, 16, 2>;
   ThisMapper mapper;

   temp_directory temp_dir;
   const auto&    temp = temp_dir.path();

   pinnable_mapped_file pmf(temp, true, mapper.total_size(), false, pinnable_mapped_file::map_mode::mapped);
   std::optional<chainbase::allocator<char>> alloc = chainbase::allocator<char>(pmf.get_segment_manager());

   size_t free_memory = pmf.get_segment_manager()->get_free_memory();

   std::vector<bucket> allocated;
   std::vector<bucket> available;

   auto _alloc = [&]<size_t num_counts, size_t count>() {
      for (size_t i = 0; i < count; ++i) {
         auto sz = ThisMapper::bucket_size(count, num_counts);
         available.emplace_back(*alloc, sz);
      }
   };

   auto alloc_buckets = [&]<size_t... counts>() {
      constexpr size_t num_counts = sizeof...(counts);
      (_alloc.operator()<num_counts, counts>(), ...); // Calls _alloc<n, count> for each count
   };

#if 1
   alloc_buckets.operator()<100>();
#elif 1
   alloc_buckets.operator()<50, 166>();
#elif 1
   alloc_buckets.operator()<50, 111, 166, 275>();
#else
   alloc_buckets.operator()<50, 111, 167, 200, 238, 266>();
#endif

   // get random engine to shuffle the vector and perturb the sizes
   auto rng = std::default_random_engine{};
   std::uniform_real_distribution<> dist(0.8, 1.2);

   const auto wait_time = std::chrono::microseconds{25};

   GraphView graph_view(mapper, (char *)pmf.get_segment_manager());
   size_t cnt = 0;

   while (!available.empty()) {
      // allocate 11
      // -----------
      std::shuffle(std::begin(available), std::end(available), rng);
      size_t last = (++cnt % 1) == 0 ? 11 : 10;
      for (size_t i=0; i<last; ++i) {
         if (available.empty())
            break;
         bucket& b = available.back();
#if 0
         // randomly change the allocated size within -20% to +20%
         b._sz = std::max((size_t)(b._sz * dist(rng)), size_t(8));
#endif
         graph_view.show(b.alloc());
         allocated.push_back(std::move(b));
         available.pop_back();
         std::this_thread::sleep_for(wait_time);
      }

      if (available.empty())
         break;

      // free 10
      // -------
      std::shuffle(std::begin(allocated), std::end(allocated), rng);
      for (size_t i=0; i<10; ++i) {
         assert(!allocated.empty());
         graph_view.hide(allocated.back().free());
         available.push_back(std::move(allocated.back()));
         allocated.pop_back();
         std::this_thread::sleep_for(wait_time);
      }

      auto event = graph_view.process_events();
      if (event == 65) { // space
         while (true) {
            std::this_thread::sleep_for(std::chrono::microseconds{25});
            if (graph_view.process_events() == 65) // space
               break;
         }
      }
      //std::cout << allocated.size() << ", " << available.size() << ", free memory = " << pmf.get_segment_manager()->get_free_memory() << '\n';
   }

   // Can't really expect the number of free bytes to match exactly
   // -------------------------------------------------------------
   size_t total_size_allocated = std::accumulate(std::begin(allocated), std::end(allocated), size_t(0),
                                                 [](size_t acc, const auto& bucket) { return acc + bucket.size(); });

   BOOST_REQUIRE_GE(free_memory, pmf.get_segment_manager()->get_free_memory() + total_size_allocated);
}