#include "server/copy_renderer.h"
#include "core/log.h"
#include "wlr_color.h"
extern "C" {
#include <gbm.h>
#include <GLES2/gl2.h>
#include <wlr/render/gles2.h>
#include <wlr/render/interface.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/interfaces/wlr_buffer.h>
}
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <csignal>
#include <cerrno>

struct Buffer {
 wlr_buffer base{};
 gbm_bo* bo{};
 wlr_dmabuf_attributes attributes{};
};
bool attributes(wlr_buffer* base, wlr_dmabuf_attributes* output) {
 *output = reinterpret_cast<Buffer*>(base)->attributes;
 return true;
}
void destroy(wlr_buffer* base) {
 auto* buffer = reinterpret_cast<Buffer*>(base);
 close(buffer->attributes.fd[0]);
 gbm_bo_destroy(buffer->bo);
 delete buffer;
}
const wlr_buffer_impl implementation{.destroy = destroy, .get_dmabuf = attributes};
Buffer* allocate(gbm_device* device, uint64_t modifier) {
 auto* bo = gbm_bo_create_with_modifiers(device, 256, 256, GBM_FORMAT_XRGB8888, &modifier, 1);
 if (!bo) return nullptr;
 if (gbm_bo_get_plane_count(bo) != 1) { gbm_bo_destroy(bo); return nullptr; }
 auto* buffer = new Buffer;
 buffer->bo = bo;
 buffer->attributes = {.width=256, .height=256, .format=GBM_FORMAT_XRGB8888,
  .modifier=gbm_bo_get_modifier(bo), .n_planes=1};
 buffer->attributes.fd[0]=gbm_bo_get_fd(bo);
 buffer->attributes.stride[0]=gbm_bo_get_stride(bo);
 wlr_buffer_init(&buffer->base, &implementation, 256, 256);
 return buffer;
}
int main(int argc, char** argv) {
 if (argc == 2 && strcmp(argv[1], "--internal-copy-renderer") == 0) return umbriel::copyRendererWorker();
 if (argc != 3) {
  fprintf(stderr, "Usage: %s INTEL_RENDER_NODE SECONDARY_RENDER_NODE\n", argv[0]);
  return 64;
 }
 wlr_log_init(WLR_INFO, nullptr);
 int nvfd = open(argv[2], O_RDWR | O_CLOEXEC);
 int ifd = open(argv[1], O_RDWR | O_CLOEXEC);
 if (nvfd < 0 || ifd < 0) return 1;
 struct stat secondaryIdentity{};
 if (fstat(nvfd, &secondaryIdentity) != 0 || !S_ISCHR(secondaryIdentity.st_mode)) return 1;
 umbriel::setIsolatedCopyRenderer(true);
 auto* renderer = wlr_gles2_renderer_create_with_drm_fd(nvfd);
 if (!renderer) return 2;
 umbriel::setIsolatedCopyRenderer(false);
 auto* intelRenderer = wlr_gles2_renderer_create_with_drm_fd(ifd);
 if (!intelRenderer) return 9;
 auto* nv = gbm_create_device(nvfd);
 auto* intel = gbm_create_device(ifd);
 if (!nv || !intel) return 3;
 auto* source = allocate(intel, 0);
 const auto* formats = renderer->WLR_PRIVATE.impl->get_render_formats(renderer);
 const auto* format = wlr_drm_format_set_get(formats, GBM_FORMAT_XRGB8888);
 if (!source || !format || !format->len) return 4;
 auto* target = allocate(nv, format->modifiers[0]);
 if (!target) return 5;
 auto* sourcePass = wlr_renderer_begin_buffer_pass(intelRenderer, &source->base, nullptr);
 if (!sourcePass) return 6;
 wlr_render_rect_options rectangle{.box={.width=256, .height=256}, .color={.r=1, .a=1}};
 wlr_render_pass_add_rect(sourcePass, &rectangle);
 if (!wlr_render_pass_submit(sourcePass)) return 10;
 auto* texture = wlr_texture_from_buffer(renderer, &source->base);
 if (!texture) return 7;
 auto* rejectedPass = wlr_renderer_begin_buffer_pass(renderer, &target->base, nullptr);
 if (!rejectedPass) return 7;
 // A rejected frame must not prevent a later valid copy.
 float alpha = 0.5f;
 wlr_render_texture_options rejectedOptions{.texture=texture, .alpha=&alpha};
 wlr_render_pass_add_texture(rejectedPass, &rejectedOptions);
 if (wlr_render_pass_submit(rejectedPass)) return 11;
 // App exits interrupt the compositor's socket waits through SIGCHLD.
 // Repeated signals exercise that path without touching desktop processes.
 struct sigaction action{};
 action.sa_handler = +[](int) {};
 sigemptyset(&action.sa_mask);
 if (sigaction(SIGUSR1, &action, nullptr) != 0 || sigaction(SIGCHLD, &action, nullptr) != 0) return 13;
 const pid_t parent = getpid();
 const pid_t sender = fork();
 if (sender < 0) return 13;
 if (sender == 0) {
  for (int i = 0; i < 256; ++i) { usleep(500); kill(parent, SIGUSR1); }
  _exit(0);
 }
 bool result = true;
 for (int i = 0; result && i < 256; ++i) {
  auto* pass = wlr_renderer_begin_buffer_pass(renderer, &target->base, nullptr);
  if (!pass) { result = false; break; }
  wlr_render_texture_options options{.texture = texture, .blend_mode = WLR_RENDER_BLEND_MODE_NONE};
  wlr_render_pass_add_texture(pass, &options);
  result = wlr_render_pass_submit(pass);
 }
 int senderStatus{};
 pid_t waited;
 do { waited = waitpid(sender, &senderStatus, 0); } while (waited < 0 && errno == EINTR);
 result = result && waited == sender && WIFEXITED(senderStatus) && WEXITSTATUS(senderStatus) == 0;
 printf("GPU copy result: %d\n", result);
 wlr_texture_destroy(texture);
 wlr_renderer_destroy(intelRenderer);
 wlr_buffer_drop(&source->base);
 wlr_buffer_drop(&target->base);
 gbm_device_destroy(intel);
 gbm_device_destroy(nv);
 wlr_renderer_destroy(renderer);
 close(nvfd);
 close(ifd);
 int residual = 0;
 DIR* directory = opendir("/proc/self/fd");
 if (!directory) return 12;
 while (auto* entry = readdir(directory)) {
  char path[128], targetPath[256];
  snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
  ssize_t size = readlink(path, targetPath, sizeof(targetPath) - 1);
  if (size < 0) continue;
  targetPath[size] = 0;
  struct stat identity{};
  const bool secondary = stat(path, &identity) == 0 && S_ISCHR(identity.st_mode)
    && identity.st_rdev == secondaryIdentity.st_rdev;
  if (strstr(targetPath, "/dev/nvidia") || secondary) {
   printf("Residual descriptor: %s\n", targetPath);
   residual++;
  }
 }
 closedir(directory);
 printf("Residual GPU descriptors: %d\n", residual);
 return result && !residual ? 0 : 8;
}
