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

   const allocator_t::occupancy_array_t& get_occupancy() const {
      return _alloc->get_occupancy();
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

   static constexpr size_t total_size()         { return total_num_pages * page_size; }
   static constexpr size_t num_pixels()         { return window_width * window_height; }
   static constexpr size_t page_full_width()    { return page_width + border_width; }
   static constexpr size_t page_full_height()   { return page_height + border_width; }
   static constexpr size_t num_pages_per_line() { return window_width / page_full_width(); }
   static constexpr size_t bytes_per_pixel()    { return page_size / (page_width * page_height); }
   static constexpr size_t win_width()          { return window_width; }
   static constexpr size_t win_height()         { return window_height; }
   static constexpr size_t pg_width()           { return page_width; }
   static constexpr size_t pg_height()          { return page_height; }
   static constexpr size_t bd_width()           { return border_width; }

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

#if 1

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
   size_t        width = mapper.win_width();
   size_t        height = mapper.win_height();
   const segment_manager::occupancy_array_t& occupancy;


   GraphView(const Mapper& mapper, segment_manager* segment_mgr)
      : mapper(mapper)
      , segment_origin((char *)segment_mgr)
      , occupancy(segment_mgr->get_occupancy()) {
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
         auto keysym = XLookupKeysym((XKeyEvent*)&event, 0);
         res = keysym;
         break;
      }
      return res;
   }
};

#elif 1

#define GLAD_GL_IMPLEMENTATION
// <glad/gl.h> generated from https://gen.glad.sh/, gl 4.6 and glx 1.4,
// compat profile, header only, GL_ARB_direct_state_access extension
#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>

static const char* vertex_shader_text =  R"(
   #version 460
   layout(location = 0) in vec2 vPos;
   out vec2 texcoord;

   uniform mat4 u_mvp;

   void main()
   {
       gl_Position = u_mvp * vec4(vPos, 0.0, 1.0);
       texcoord = (vPos + vec2(1.0, 1.0)) * 0.5;
   }
)";

static const char* fragment_shader_text = R"(
   #version 460
   in  vec2 texcoord;
   out vec4 fragcolor;

   uniform sampler2D u_occupancy;

   void main()
   {
       fragcolor = texture(u_occupancy, texcoord);
   }
)";

static const float vertices[] {
    -1.0f, -1.0f,
     1.0f, -1.0f,
     1.0f,  1.0f,
    -1.0f,  1.0f,
   };

void GLAPIENTRY ogl_error_cb(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                             const GLchar* message, const void* userParam) {
   fprintf(stderr, "GL CALLBACK: %s type = 0x%x, severity = 0x%x, message = %s\n",
           (type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : ""), type, severity, message);
}

template <class Mapper>
struct GraphView {
   struct WinInfo {
      int       sqr_sz           = 1024;
      glm::vec3 mouse_pos        = glm::vec3(0.0f, 0.0f, 0.0f); // mouse position on quad in [-1, 1] screen space
      glm::vec3 translation      = glm::vec3(0.0f, 0.0f, 0.0f); // in model space
      float     zoom             = 1.0f;                        // 1.0x to whatever
      bool      left_button_down = false;                       // when left button is down, mouse move translates the model
      int       last_key         = 0;                           // last key pressed using GLFW defines
      GLint     mvp_loc          = 0;
      uint32_t  vao_id           = 0;
      glm::mat4 mvp;

      void update_mvp() {
         mvp =
          glm::scale(    glm::mat4(1.0f),  glm::vec3(zoom, zoom, zoom)) *
          glm::translate(glm::mat4(1.0f),  translation);
      }
   };

   static void glfw_error_cb(int error, const char* description) { fprintf(stderr, "Error: %s\n", description); }

   static void key_cb(GLFWwindow* window, int key, int scancode, int action, int mods) {
      if (action == GLFW_PRESS) {
         wi.last_key = key;
         if (key == GLFW_KEY_ESCAPE)
            glfwSetWindowShouldClose(window, GLFW_TRUE);
      }
   }

   static void resize_cb(GLFWwindow* window, int width, int height) {
      wi.sqr_sz = std::min(width, height);
      wi.sqr_sz = std::max(wi.sqr_sz, 1);
      glViewport(0, 0, wi.sqr_sz, wi.sqr_sz);
   }

   static void mousemove_cb(GLFWwindow* window, double x, double y) {
      // x, y are in screen pixels, origin at top-left of window
      x = std::min(x, (double)(wi.sqr_sz)) / wi.sqr_sz;
      y = std::min(y, (double)(wi.sqr_sz)) / wi.sqr_sz;
      y = 1.0 - y;                                          // y origin at bottom

      // update coordinates for [-1, 1] range
      x = x * 2 - 1;
      y = y * 2 - 1;

      glm::vec3 new_mouse_pos = glm::vec3(x, y, 0);
      glm::vec3 mouse_offset  = new_mouse_pos - wi.mouse_pos;

      if (wi.left_button_down) {
         wi.translation += mouse_offset / wi.zoom;
         wi.update_mvp();
      }
      wi.mouse_pos = new_mouse_pos;

      // fprintf(stderr, "x = %.2f, y = %.2f\n", wi.position.x, wi.position.y);
   }

   static void mouse_button_cb(GLFWwindow* window, int button, int action, int mods) {
      if (button == GLFW_MOUSE_BUTTON_LEFT)
         wi.left_button_down = (action == GLFW_PRESS);
   }

   static void scroll_cb(GLFWwindow* window, double xoffset, double yoffset) {
      // normal mouse wheel provides offsets on the y axis
      // fprintf(stderr, "xoffset = %d, yoffset = %d\n", (int)xoffset, (int)yoffset);
      float zoomSpeed = 1.0f;
      float zoom = wi.zoom;

      zoom += -yoffset * zoomSpeed;
      zoom = std::max(1.0f, zoom); // Prevent zooming out

      // figure out how much the point that was under the mouse has moved away because
      // of the zoom by prohecting it into the model space, applying the zoom, and projecting
      // it back into screen space again.
      // ------------------------------------------------------------------------------------
      auto inv_mvp = glm::inverse(wi.mvp);
      glm::vec4 mouse_pos(wi.mouse_pos, 1.0f);
      mouse_pos = mouse_pos * inv_mvp; // now in model space

      wi.zoom = zoom;
      wi.update_mvp();
      mouse_pos = mouse_pos * wi.mvp; // back in screen space after updating zoom
      glm::vec3 offset = glm::vec3(mouse_pos.x, mouse_pos.y, mouse_pos.z) - wi.mouse_pos; // offset in screen space

      // now that we figure out how far the point under the mouse has moved away, translate
      // it back so it stays under the mouse.
      // ----------------------------------------------------------------------------------
      wi.translation -= offset / zoom;

      // update mvp again to take translation into account
      // -------------------------------------------------
      wi.update_mvp();
   }

   static void check_shader_error(const char* type, GLuint id) {
      GLint success = 0;
      glGetShaderiv(id, GL_COMPILE_STATUS, &success);
      if (success == GL_FALSE) {
         // Setup Error Log
         GLint maxLength = 0;
         glGetShaderiv(id, GL_INFO_LOG_LENGTH, &maxLength);
         std::vector<char> errorLog(maxLength);
         glGetShaderInfoLog(id, maxLength, &maxLength, &errorLog[0]);
         std::cout << '\n' << type << ":\n" << &errorLog[0] << '\n';
      }
   }

   GraphView(const Mapper& mapper, segment_manager* segment_origin) {
      wi.mvp = glm::mat4(1.0f);//glm::ortho(0.f, 1.f, 0.f, 1.f, 0.f, 1.f);

      glfwSetErrorCallback(glfw_error_cb);

      if (!glfwInit()) {
         terminate("Failed to initialize GLFW");
         return;
      }

      // Set OpenGL version and profile
      glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
      glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
      glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

      // Create a window
      window = glfwCreateWindow(wi.sqr_sz, wi.sqr_sz, "Memory Occupancy view", nullptr, nullptr);
      if (!window) {
         terminate("Failed to create window");
         return;
      }

      // set callbacks
      glfwSetKeyCallback(window, key_cb);
      glfwSetFramebufferSizeCallback(window, resize_cb);
      glfwSetCursorPosCallback(window, mousemove_cb);
      glfwSetMouseButtonCallback(window, mouse_button_cb);
      glfwSetScrollCallback(window, scroll_cb);

      // Make the window's context current
      glfwMakeContextCurrent(window);

      // Only enable vsync for the first of the windows to be swapped to
      // avoid waiting out the interval for each window
      glfwSwapInterval(1);

      // Initialize GLAD
      if (!gladLoadGL(glfwGetProcAddress)) {
         terminate("Failed to initialize GLAD");
         return;
      }
      if (!GLAD_GL_ARB_direct_state_access) {
         /* see
          * https://www.khronos.org/registry/OpenGL/extensions/ARB/ARB_direct_state_access.txt
          * https://www.khronos.org/opengl/wiki/Direct_State_Access. This is the way.
          */
         terminate("DSA not supported!");
         return;
      }

      glEnable(GL_DEBUG_OUTPUT);
      glDebugMessageCallback(ogl_error_cb, 0);

      GLuint texture_id, program_id;
      GLint  vpos_location, texture_location;

      // generate color texture
      // ----------------------
      {
         struct rgba {
            uint8_t r, g, b, a;
         };
         rgba pixels[16 * 16];

         glGenTextures(1, &texture_id);
         glBindTexture(GL_TEXTURE_2D, texture_id);

         for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
               pixels[y * 16 + x] = rgba{(uint8_t)(x * 16), (uint8_t)(y * 16), 0, 255};
            }
         }

         glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      }

      // compile program and get uniform and attribute locations
      // -------------------------------------------------------
      {
         GLuint vertex_shader_id, fragment_shader_id;

         vertex_shader_id = glCreateShader(GL_VERTEX_SHADER);
         glShaderSource(vertex_shader_id, 1, &vertex_shader_text, NULL);
         glCompileShader(vertex_shader_id);
         check_shader_error("vertex", vertex_shader_id);

         fragment_shader_id = glCreateShader(GL_FRAGMENT_SHADER);
         glShaderSource(fragment_shader_id, 1, &fragment_shader_text, NULL);
         glCompileShader(fragment_shader_id);
         check_shader_error("fragment", fragment_shader_id);

         program_id = glCreateProgram();
         glAttachShader(program_id, vertex_shader_id);
         glAttachShader(program_id, fragment_shader_id);
         glLinkProgram(program_id);
         GLint params = -1;
         glGetProgramiv(program_id, GL_LINK_STATUS, &params);
         if (GL_TRUE != params) {
            terminate("Error linking program");
            return;
         }

         wi.mvp_loc     = glGetUniformLocation(program_id, "u_mvp");
         texture_location = glGetUniformLocation(program_id, "u_occupancy");

         vpos_location = glGetAttribLocation(program_id, "vPos");
      }

      // Create Vertex Array Buffer for vertices
      // ---------------------------------------
      {
         unsigned int vbo_id;
         glCreateBuffers(1, &vbo_id);
         glNamedBufferStorage(vbo_id, sizeof(vertices), &vertices[0], GL_DYNAMIC_STORAGE_BIT);

         glCreateVertexArrays(1, &wi.vao_id);

         glVertexArrayVertexBuffer(wi.vao_id, // vao to bind
                                   0,         // Could be 1, 2... if there were several vbo to source.
                                   vbo_id,    // VBO to bound at index 0
                                   0,         // offset of the first element in the buffer hctVBO.
                                   2 * sizeof(float));

         glEnableVertexArrayAttrib(wi.vao_id, vpos_location);
         glVertexArrayAttribFormat(wi.vao_id, vpos_location, 2, GL_FLOAT, false, 0);
         glVertexArrayAttribBinding(wi.vao_id, vpos_location, 0);
      }

      glUseProgram(program_id);
      glUniform1i(texture_location, 0);
      glBindTexture(GL_TEXTURE_2D, texture_id);
   }

   void render() const {
      glfwMakeContextCurrent(window);

      // Clear screen
      glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);

      glUniformMatrix4fv(wi.mvp_loc, 1, GL_FALSE, &wi.mvp[0].x);

      // std::cout << glm::to_string(wi.mvp) << '\n';
      glBindVertexArray(wi.vao_id);
      glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

      glfwSwapBuffers(window);
      glfwPollEvents();
   }

   void terminate(std::string_view message) {
      std::cerr << "closing: " << message << '\n';
      window = nullptr;
      glfwTerminate();
   }

   void show(ptr_sz_t ptr_sz, bool black = true) {}
   void hide(ptr_sz_t ptr_sz) {}

   int  process_events() {
      if (!window || glfwWindowShouldClose(window))
         return GLFW_KEY_ESCAPE;

      render();

      int last_key = wi.last_key;
      wi.last_key = 0;
      return last_key;
   }

private:
   static WinInfo wi;
   GLFWwindow* window = nullptr;
};

template <class Mapper> GraphView<Mapper>::WinInfo GraphView<Mapper>::wi;

#else

template<class Mapper>
struct GraphView {
   GraphView(const Mapper& mapper, segment_manager* segment_origin) {}
   void show(ptr_sz_t ptr_sz, bool black = true) {}
   void hide(ptr_sz_t ptr_sz) {}
   int process_events() { return 0; }
};

#endif

#endif // __unix__

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
   using ThisMapper = Mapper<118'784, 65536, 2304, 2088, 4, 4, 2>;
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

#if 0
   alloc_buckets.operator()<100>();
#elif 0
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

   GraphView graph_view(mapper, pmf.get_segment_manager());
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
      if (event == ' ') { // hit space on keyboard
         while (true) {
            std::this_thread::sleep_for(std::chrono::microseconds{25});
            if (graph_view.process_events() == ' ') // we sleep until space is pressed again
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