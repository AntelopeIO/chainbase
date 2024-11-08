#include <chainbase/mem_visualizer.hpp>
#include <iostream>
#include <thread>
#include <atomic>

#define GLAD_GL_IMPLEMENTATION
// <glad/gl.h> generated from https://gen.glad.sh/, gl 4.6 and glx 1.4,
// compat profile, header only, GL_ARB_direct_state_access extension
#include <glad/gl.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>

namespace chainbase {

// ----------------------------------------------------------------------------------------------------------------
struct WinInfo {
   int       sqr_sz           = 1024;
   glm::vec3 mouse_pos        = glm::vec3(0.0f, 0.0f, 0.0f); // mouse position on quad in [-1, 1] screen space
   glm::vec3 translation      = glm::vec3(0.0f, 0.0f, 0.0f); // in model space
   float     zoom             = 1.0f;                        // 1.0x to whatever
   bool      left_button_down = false; // when left button is down, mouse move translates the model
   int       last_key         = 0;     // last key pressed using GLFW defines
   GLint     mvp_loc          = 0;
   uint32_t  vao_id           = 0;
   glm::mat4 mvp;

   void update_mvp() {
      mvp = glm::scale(glm::mat4(1.0f), glm::vec3(zoom, zoom, zoom)) * glm::translate(glm::mat4(1.0f), translation);
   }
};

// ----------------------------------------------------------------------------------------------------------------
static const char* vertex_shader_text = R"(
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

static const float vertices[]{
   -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,
};

static void GLAPIENTRY ogl_error_cb(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                                    const GLchar* message, const void* userParam) {
   fprintf(stderr, "GL CALLBACK: %s type = 0x%x, severity = 0x%x, message = %s\n",
           (type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : ""), type, severity, message);
}

// ----------------------------------------------------------------------------------------------------------------
class mem_visualizer_impl {
private:
   GLFWwindow* window           = nullptr;
   int         sqr_sz           = 1024;
   glm::vec3   mouse_pos        = glm::vec3(0.0f, 0.0f, 0.0f); // mouse position on quad in [-1, 1] screen space
   glm::vec3   translation      = glm::vec3(0.0f, 0.0f, 0.0f); // in model space
   float       zoom             = 1.0f;                        // 1.0x to whatever
   bool        left_button_down = false; // when left button is down, mouse move translates the model
   int         last_key         = 0;     // last key pressed using GLFW defines
   GLint       mvp_loc          = 0;
   uint32_t    vao_id           = 0;
   glm::mat4   mvp;
   std::thread work_thread;

   std::atomic<bool> shutting_down = false;

public:
   // -------------------------------------------------------------------------
   mem_visualizer_impl(pinnable_mapped_file& pmf, uint64_t shared_file_size) {
      mvp = glm::mat4(1.0f); // glm::ortho(0.f, 1.f, 0.f, 1.f, 0.f, 1.f);

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
      // ---------------
      window = glfwCreateWindow(sqr_sz, sqr_sz, "Memory Occupancy view", nullptr, nullptr);
      if (!window) {
         terminate("Failed to create window");
         return;
      }
      glfwSetWindowUserPointer(window, this);

      // set callbacks
      glfwSetKeyCallback(window, key_cb);
      glfwSetFramebufferSizeCallback(window, resize_cb);
      glfwSetCursorPosCallback(window, mousemove_cb);
      glfwSetMouseButtonCallback(window, mouse_button_cb);
      glfwSetScrollCallback(window, scroll_cb);
      glfwSetWindowCloseCallback(window, close_cb);

      // Start work thread
      // -----------------
      work_thread = std::thread([&]() {
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

            mvp_loc          = glGetUniformLocation(program_id, "u_mvp");
            texture_location = glGetUniformLocation(program_id, "u_occupancy");

            vpos_location = glGetAttribLocation(program_id, "vPos");
         }

         // Create Vertex Array Buffer for vertices
         // ---------------------------------------
         {
            unsigned int vbo_id;
            glCreateBuffers(1, &vbo_id);
            glNamedBufferStorage(vbo_id, sizeof(vertices), &vertices[0], GL_DYNAMIC_STORAGE_BIT);

            glCreateVertexArrays(1, &vao_id);

            glVertexArrayVertexBuffer(vao_id, // vao to bind
                                      0,      // Could be 1, 2... if there were several vbo to source.
                                      vbo_id, // VBO to bound at index 0
                                      0,      // offset of the first element in the buffer hctVBO.
                                      2 * sizeof(float));

            glEnableVertexArrayAttrib(vao_id, vpos_location);
            glVertexArrayAttribFormat(vao_id, vpos_location, 2, GL_FLOAT, false, 0);
            glVertexArrayAttribBinding(vao_id, vpos_location, 0);
         }

         glUseProgram(program_id);
         glUniform1i(texture_location, 0);
         glBindTexture(GL_TEXTURE_2D, texture_id);


         while (!shutting_down) {
            render();
            [[maybe_unused]] int last_key = process_events();

            std::this_thread::sleep_for(std::chrono::milliseconds{25}); // be nice don't hog the CPU
         };
      });
   }

   // -------------------------------------------------------------------------
   ~mem_visualizer_impl() {}

   // -------------------------------------------------------------------------
   void render() const {
      if (!window || glfwWindowShouldClose(window))
         return;

      // Clear screen
      glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);

      glUniformMatrix4fv(mvp_loc, 1, GL_FALSE, &mvp[0].x);

      // std::cout << glm::to_string(mvp) << '\n';
      glBindVertexArray(vao_id);
      glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

      glfwSwapBuffers(window);
   }

   // -------------------------------------------------------------------------
   int process_events() {
      if (!window || glfwWindowShouldClose(window))
         return GLFW_KEY_ESCAPE;

      glfwPollEvents();

      int last_key = last_key;
      last_key     = 0;
      return last_key;
   }

   // -------------------------------------------------------------------------
   void terminate(std::string_view message) {
      if (shutting_down)
         return;
      shutting_down = true;

      std::cerr << "closing: " << message << '\n';
      if (window) {
         window = nullptr;
         glfwTerminate();
      }
   }

private:
   // -------------------------------------------------------------------------
   void update_mvp() {
      mvp = glm::scale(glm::mat4(1.0f), glm::vec3(zoom, zoom, zoom)) * glm::translate(glm::mat4(1.0f), translation);
   }

   // -------------------------------------------------------------------------
   static void glfw_error_cb(int error, const char* description) { fprintf(stderr, "Error: %s\n", description); }

   // -------------------------------------------------------------------------
   static void close_cb(GLFWwindow* window) {
      auto& memv = *static_cast<mem_visualizer_impl*>(glfwGetWindowUserPointer(window));
      memv.terminate("Close button hit");
   }

   // -------------------------------------------------------------------------
   static void key_cb(GLFWwindow* window, int key, int scancode, int action, int mods) {
      auto& memv = *static_cast<mem_visualizer_impl*>(glfwGetWindowUserPointer(window));
      if (action == GLFW_PRESS) {
         memv.last_key = key;
         if (key == GLFW_KEY_ESCAPE)
            memv.terminate("Escape key hit");
      }
   }

   // -------------------------------------------------------------------------
   static void resize_cb(GLFWwindow* window, int width, int height) {
      auto& memv  = *static_cast<mem_visualizer_impl*>(glfwGetWindowUserPointer(window));
      memv.sqr_sz = std::min(width, height);
      memv.sqr_sz = std::max(memv.sqr_sz, 1);
      glViewport(0, 0, memv.sqr_sz, memv.sqr_sz);
   }

   // -------------------------------------------------------------------------
   static void mousemove_cb(GLFWwindow* window, double x, double y) {
      // x, y are in screen pixels, origin at top-left of window
      auto& memv = *static_cast<mem_visualizer_impl*>(glfwGetWindowUserPointer(window));

      x = std::min(x, (double)(memv.sqr_sz)) / memv.sqr_sz;
      y = std::min(y, (double)(memv.sqr_sz)) / memv.sqr_sz;
      y = 1.0 - y; // y origin at bottom

      // update coordinates for [-1, 1] range
      x = x * 2 - 1;
      y = y * 2 - 1;

      glm::vec3 new_mouse_pos = glm::vec3(x, y, 0);
      glm::vec3 mouse_offset  = new_mouse_pos - memv.mouse_pos;

      if (memv.left_button_down) {
         memv.translation += mouse_offset / memv.zoom;
         memv.update_mvp();
      }
      memv.mouse_pos = new_mouse_pos;

      // fprintf(stderr, "x = %.2f, y = %.2f\n", memv.position.x, memv.position.y);
   }

   // -------------------------------------------------------------------------
   static void mouse_button_cb(GLFWwindow* window, int button, int action, int mods) {
      auto& memv = *static_cast<mem_visualizer_impl*>(glfwGetWindowUserPointer(window));

      if (button == GLFW_MOUSE_BUTTON_LEFT)
         memv.left_button_down = (action == GLFW_PRESS);
   }

   // -------------------------------------------------------------------------
   static void scroll_cb(GLFWwindow* window, double xoffset, double yoffset) {
      // normal mouse wheel provides offsets on the y axis
      // fprintf(stderr, "xoffset = %d, yoffset = %d\n", (int)xoffset, (int)yoffset);
      auto& memv = *static_cast<mem_visualizer_impl*>(glfwGetWindowUserPointer(window));

      float zoomSpeed = 1.0f;
      float zoom      = memv.zoom;

      zoom += -yoffset * zoomSpeed;
      zoom = std::max(1.0f, zoom); // Prevent zooming out

      // figure out how much the point that was under the mouse has moved away because
      // of the zoom by prohecting it into the model space, applying the zoom, and projecting
      // it back into screen space again.
      // ------------------------------------------------------------------------------------
      auto      inv_mvp = glm::inverse(memv.mvp);
      glm::vec4 mouse_pos(memv.mouse_pos, 1.0f);
      mouse_pos = mouse_pos * inv_mvp; // now in model space

      memv.zoom = zoom;
      memv.update_mvp();
      mouse_pos        = mouse_pos * memv.mvp; // back in screen space after updating zoom
      glm::vec3 offset = glm::vec3(mouse_pos.x, mouse_pos.y, mouse_pos.z) - memv.mouse_pos; // offset in screen space

      // now that we figure out how far the point under the mouse has moved away, translate
      // it back so it stays under the mouse.
      // ----------------------------------------------------------------------------------
      memv.translation -= offset / zoom;

      // update mvp again to take translation into account
      // -------------------------------------------------
      memv.update_mvp();
   }

   // -------------------------------------------------------------------------
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
};

// ----------------------------------------------------------------------------------------------------------------
mem_visualizer::mem_visualizer(pinnable_mapped_file& pmf, uint64_t shared_file_size)
   : my(new mem_visualizer_impl(pmf, shared_file_size)) {}

// ----------------------------------------------------------------------------------------------------------------
mem_visualizer::~mem_visualizer() {
   my->terminate("mem_visualizer destructor called");
}

} // namespace chainbase