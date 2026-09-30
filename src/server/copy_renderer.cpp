#include "server/copy_renderer.h"
#if UMBRIEL_HAS_NATIVE_DRM_POLICY
#include "core/log.h"
#include "wlr_color.h"

extern "C" {
#include <GLES2/gl2.h>
#include <wlr/render/interface.h>
#include <wlr/render/gles2.h>
#include <wlr/render/egl.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/drm_syncobj.h>
}

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;
extern "C" wlr_egl* wlr_egl_create_with_drm_fd(int);
extern "C" wlr_renderer* wlr_gles2_renderer_create(wlr_egl*);
extern "C" bool wlr_egl_make_current(wlr_egl*, void*);

namespace {
  constexpr Logger kLog("copy-renderer");
  constexpr int kWorkerSocket = 3;
  constexpr int kTimeoutMs = 3000;
  constexpr size_t kMaxFormats = 2048;
  constexpr size_t kMaxFds = 8;
  bool isolated = false;

  enum class Operation : uint32_t { Initialize = 1, Blit, Stop };
  struct Request {
    Operation operation{};
    wlr_dmabuf_attributes source{};
    wlr_dmabuf_attributes destination{};
  };
  struct Format {
    uint32_t fourcc{};
    uint64_t modifier{};
  };
  struct Reply {
    uint32_t success{};
    uint32_t textureCount{};
    uint32_t renderCount{};
    std::array<Format, kMaxFormats> formats{};
  };

  bool sendPacket(int socket, const void* data, size_t size, const std::vector<int>& fds = {}) {
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * kMaxFds)> control{};
    iovec vector{const_cast<void*>(data), size};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    if (!fds.empty()) {
      if (fds.size() > kMaxFds) { return false; }
      message.msg_control = control.data();
      message.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
      auto* ancillary = CMSG_FIRSTHDR(&message);
      ancillary->cmsg_level = SOL_SOCKET;
      ancillary->cmsg_type = SCM_RIGHTS;
      ancillary->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
      std::memcpy(CMSG_DATA(ancillary), fds.data(), sizeof(int) * fds.size());
    }
    pollfd ready{socket, POLLOUT, 0};
    return poll(&ready, 1, kTimeoutMs) == 1
        && sendmsg(socket, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(size);
  }

  bool receivePacket(int socket, void* data, size_t size, std::vector<int>& fds, int timeout = kTimeoutMs) {
    pollfd ready{socket, POLLIN, 0};
    if (poll(&ready, 1, timeout) != 1) { return false; }
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * kMaxFds)> control{};
    iovec vector{data, size};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    const ssize_t length = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
    for (auto* ancillary = CMSG_FIRSTHDR(&message); ancillary; ancillary = CMSG_NXTHDR(&message, ancillary)) {
      if (ancillary->cmsg_level != SOL_SOCKET || ancillary->cmsg_type != SCM_RIGHTS
          || ancillary->cmsg_len < CMSG_LEN(0)) { continue; }
      const size_t count = (ancillary->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      auto* values = reinterpret_cast<int*>(CMSG_DATA(ancillary));
      fds.insert(fds.end(), values, values + count);
    }
    return length == static_cast<ssize_t>(size) && !(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
  }

  void closeFds(const std::vector<int>& fds) {
    for (int fd : fds) { close(fd); }
  }

  struct RemoteRenderer {
    wlr_renderer base{};
    int socket = -1;
    int drmFd = -1;
    pid_t child = -1;
    wlr_drm_format_set textureFormats{};
    wlr_drm_format_set renderFormats{};
    bool fault = false;
  };
  struct RemoteTexture {
    wlr_texture base{};
    wlr_buffer* buffer = nullptr;
  };
  struct RemotePass {
    wlr_render_pass base{};
    RemoteRenderer* renderer = nullptr;
    wlr_buffer* destination = nullptr;
    wlr_buffer* source = nullptr;
    bool valid = true;
  };

  // This renderer accepts only the whole-buffer DMA-BUF blits used by the
  // secondary DRM backend. The scene renderer remains in the parent process.

  void stopChild(RemoteRenderer* renderer) {
    if (renderer->socket >= 0) {
      Request stop{.operation = Operation::Stop};
      sendPacket(renderer->socket, &stop, sizeof(stop));
      close(renderer->socket);
    }
    if (renderer->child > 0) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
      int status{};
      while (waitpid(renderer->child, &status, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
          kLog.error("copy renderer {} failed to exit, terminating its private worker", renderer->child);
          kill(renderer->child, SIGKILL);
          const auto killedDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
          while (waitpid(renderer->child, &status, WNOHANG) == 0) {
            if (std::chrono::steady_clock::now() >= killedDeadline) {
              kLog.error("copy renderer {} remains blocked after termination", renderer->child);
              break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    if (renderer->drmFd >= 0) { close(renderer->drmFd); }
    wlr_drm_format_set_finish(&renderer->textureFormats);
    wlr_drm_format_set_finish(&renderer->renderFormats);
  }

  const wlr_drm_format_set* textureFormats(wlr_renderer* base, uint32_t caps) {
    return caps & WLR_BUFFER_CAP_DMABUF ? &reinterpret_cast<RemoteRenderer*>(base)->textureFormats : nullptr;
  }
  const wlr_drm_format_set* renderFormats(wlr_renderer* base) {
    return &reinterpret_cast<RemoteRenderer*>(base)->renderFormats;
  }
  void destroyRenderer(wlr_renderer* base) {
    auto* renderer = reinterpret_cast<RemoteRenderer*>(base);
    stopChild(renderer);
    delete renderer;
  }
  int rendererFd(wlr_renderer* base) { return reinterpret_cast<RemoteRenderer*>(base)->drmFd; }
  void destroyTexture(wlr_texture* base) {
    auto* texture = reinterpret_cast<RemoteTexture*>(base);
    wlr_buffer_unlock(texture->buffer);
    delete texture;
  }
  const wlr_texture_impl textureImplementation{.destroy = destroyTexture};
  wlr_texture* textureFromBuffer(wlr_renderer* renderer, wlr_buffer* buffer) {
    wlr_dmabuf_attributes attributes{};
    if (!wlr_buffer_get_dmabuf(buffer, &attributes)) { return nullptr; }
    auto* texture = new RemoteTexture;
    wlr_texture_init(&texture->base, renderer, &textureImplementation, buffer->width, buffer->height);
    texture->buffer = wlr_buffer_lock(buffer);
    return &texture->base;
  }

  bool validAttributes(const wlr_dmabuf_attributes& attributes) {
    return attributes.width > 0 && attributes.width <= 16384 && attributes.height > 0
        && attributes.height <= 16384 && attributes.n_planes > 0 && attributes.n_planes <= WLR_DMABUF_MAX_PLANES;
  }

  bool submitPass(wlr_render_pass* base) {
    auto* pass = reinterpret_cast<RemotePass*>(base);
    Request request{.operation = Operation::Blit};
    bool success = !pass->renderer->fault && pass->valid && pass->source
        && wlr_buffer_get_dmabuf(pass->source, &request.source)
        && wlr_buffer_get_dmabuf(pass->destination, &request.destination)
        && validAttributes(request.source) && validAttributes(request.destination);
    if (success) {
      std::vector<int> fds;
      for (int i = 0; i < request.source.n_planes; ++i) { fds.push_back(request.source.fd[i]); }
      for (int i = 0; i < request.destination.n_planes; ++i) { fds.push_back(request.destination.fd[i]); }
      uint32_t reply{};
      std::vector<int> received;
      success = sendPacket(pass->renderer->socket, &request, sizeof(request), fds)
          && receivePacket(pass->renderer->socket, &reply, sizeof(reply), received)
          && received.empty() && reply == 1;
      closeFds(received);
    }
    if (!success) { pass->renderer->fault = true; }
    if (pass->source) { wlr_buffer_unlock(pass->source); }
    wlr_buffer_unlock(pass->destination);
    delete pass;
    return success;
  }

  void addTexture(wlr_render_pass* base, const wlr_render_texture_options* options) {
    auto* pass = reinterpret_cast<RemotePass*>(base);
    if (pass->source || options->texture->impl != &textureImplementation || options->alpha || options->clip
        || options->transform != WL_OUTPUT_TRANSFORM_NORMAL || options->src_box.width || options->src_box.height
        || options->src_box.x || options->src_box.y || options->dst_box.x || options->dst_box.y
        || (options->dst_box.width && options->dst_box.width != pass->destination->width)
        || (options->dst_box.height && options->dst_box.height != pass->destination->height)
        || options->blend_mode != WLR_RENDER_BLEND_MODE_NONE || options->primaries || options->luminance_multiplier) {
      pass->valid = false;
      return;
    }
    if (options->wait_timeline) {
      const int fence = wlr_drm_syncobj_timeline_export_sync_file(options->wait_timeline, options->wait_point);
      pollfd ready{fence, POLLIN, 0};
      const bool signalled = fence >= 0 && poll(&ready, 1, kTimeoutMs) == 1 && (ready.revents & POLLIN);
      if (fence >= 0) { close(fence); }
      if (!signalled) { pass->valid = false; return; }
    }
    pass->source = wlr_buffer_lock(reinterpret_cast<RemoteTexture*>(options->texture)->buffer);
  }
  void addRect(wlr_render_pass* base, const wlr_render_rect_options*) {
    reinterpret_cast<RemotePass*>(base)->valid = false;
  }
  const wlr_render_pass_impl passImplementation{.submit = submitPass, .add_texture = addTexture, .add_rect = addRect};
  wlr_render_pass* beginPass(wlr_renderer* renderer, wlr_buffer* buffer, const wlr_buffer_pass_options* options) {
    if (options && (options->timer || options->color_transform || options->signal_timeline)) { return nullptr; }
    auto* pass = new RemotePass;
    wlr_render_pass_init(&pass->base, &passImplementation);
    pass->renderer = reinterpret_cast<RemoteRenderer*>(renderer);
    pass->destination = wlr_buffer_lock(buffer);
    return &pass->base;
  }
  const wlr_renderer_impl rendererImplementation{
      .get_texture_formats = textureFormats, .get_render_formats = renderFormats, .destroy = destroyRenderer,
      .get_drm_fd = rendererFd, .texture_from_buffer = textureFromBuffer, .begin_buffer_pass = beginPass};

  wlr_renderer* createRemoteRenderer(int drmFd) {
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) { return nullptr; }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sockets[1], kWorkerSocket);
    posix_spawn_file_actions_addclosefrom_np(&actions, kWorkerSocket + 1);
    char executable[] = "/proc/self/exe";
    char argument[] = "--internal-copy-renderer";
    char* arguments[] = {executable, argument, nullptr};
    pid_t child{};
    const int error = posix_spawn(&child, executable, &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(sockets[1]);
    if (error != 0) { close(sockets[0]); return nullptr; }
    auto* renderer = new RemoteRenderer;
    renderer->socket = sockets[0];
    renderer->child = child;
    renderer->drmFd = fcntl(drmFd, F_DUPFD_CLOEXEC, 0);
    Request request{.operation = Operation::Initialize};
    Reply reply{};
    std::vector<int> received;
    bool success = renderer->drmFd >= 0 && sendPacket(renderer->socket, &request, sizeof(request), {drmFd})
        && receivePacket(renderer->socket, &reply, sizeof(reply), received)
        && received.empty() && reply.success == 1 && reply.textureCount > 0 && reply.renderCount > 0
        && reply.textureCount <= kMaxFormats && reply.renderCount <= kMaxFormats - reply.textureCount;
    closeFds(received);
    for (size_t i = 0; success && i < reply.textureCount + reply.renderCount; ++i) {
      auto* set = i < reply.textureCount ? &renderer->textureFormats : &renderer->renderFormats;
      success = wlr_drm_format_set_add(set, reply.formats[i].fourcc, reply.formats[i].modifier);
    }
    if (!success) { stopChild(renderer); delete renderer; return nullptr; }
    wlr_renderer_init(&renderer->base, &rendererImplementation, WLR_BUFFER_CAP_DMABUF);
    kLog.info("copy renderer isolated in worker {}", child);
    return &renderer->base;
  }

  struct ImportedBuffer {
    wlr_buffer base{};
    wlr_dmabuf_attributes attributes{};
  };
  void destroyBuffer(wlr_buffer* base) {
    auto* buffer = reinterpret_cast<ImportedBuffer*>(base);
    wlr_dmabuf_attributes_finish(&buffer->attributes);
    delete buffer;
  }
  bool getDmabuf(wlr_buffer* base, wlr_dmabuf_attributes* attributes) {
    *attributes = reinterpret_cast<ImportedBuffer*>(base)->attributes;
    return true;
  }
  const wlr_buffer_impl bufferImplementation{.destroy = destroyBuffer, .get_dmabuf = getDmabuf};
  ImportedBuffer* importBuffer(wlr_dmabuf_attributes attributes, const std::vector<int>& fds, size_t offset) {
    auto* buffer = new ImportedBuffer;
    for (int i = 0; i < attributes.n_planes; ++i) { attributes.fd[i] = fds[offset + i]; }
    buffer->attributes = attributes;
    wlr_buffer_init(&buffer->base, &bufferImplementation, attributes.width, attributes.height);
    return buffer;
  }
  bool appendFormats(Reply& reply, const wlr_drm_format_set* set, size_t& count) {
    if (!set) { return false; }
    for (size_t i = 0; i < set->len; ++i) {
      const auto& format = set->formats[i];
      for (size_t j = 0; j < format.len; ++j) {
        if (count == kMaxFormats) { return false; }
        reply.formats[count++] = {format.format, format.modifiers[j]};
      }
    }
    return true;
  }

  bool waitForWriters(const wlr_dmabuf_attributes& attributes) {
    // NVIDIA does not reliably wait for foreign implicit fences. Complete
    // every producer write before importing the source into its copy renderer.
    for (int i = 0; i < attributes.n_planes; ++i) {
      dma_buf_export_sync_file fence{.flags = DMA_BUF_SYNC_READ, .fd = -1};
      if (ioctl(attributes.fd[i], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &fence) != 0) { return false; }
      pollfd ready{fence.fd, POLLIN, 0};
      const bool signalled = poll(&ready, 1, kTimeoutMs) == 1 && (ready.revents & POLLIN);
      close(fence.fd);
      if (!signalled) { return false; }
    }
    return true;
  }

}

namespace umbriel {
  bool setIsolatedCopyRenderer(bool enabled) {
    const bool previous = isolated;
    isolated = enabled;
    return previous;
  }

  int copyRendererWorker() {
    prctl(PR_SET_NAME, "umbriel-copy", 0, 0, 0);
    const pid_t parent = getppid();
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent) { return 1; }
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    int type{};
    socklen_t typeSize = sizeof(type);
    if (getsockopt(kWorkerSocket, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0
        || credentials.uid != getuid() || credentials.pid != parent
        || getsockopt(kWorkerSocket, SOL_SOCKET, SO_TYPE, &type, &typeSize) != 0 || type != SOCK_SEQPACKET) { return 1; }
    fcntl(kWorkerSocket, F_SETFD, FD_CLOEXEC);
    setenv("__EGL_VENDOR_LIBRARY_FILENAMES",
        "/usr/share/glvnd/egl_vendor.d/10_nvidia.json:/usr/share/glvnd/egl_vendor.d/50_mesa.json", 1);
    unsetenv("WLR_RENDER_DRM_DEVICE");
    Request request{};
    std::vector<int> fds;
    if (!receivePacket(kWorkerSocket, &request, sizeof(request), fds)
        || request.operation != Operation::Initialize || fds.size() != 1) { closeFds(fds); return 1; }
    const int drmFd = fds[0];
    struct stat descriptor{};
    if (fstat(drmFd, &descriptor) != 0 || !S_ISCHR(descriptor.st_mode) || major(descriptor.st_rdev) != 226) {
      close(drmFd);
      return 1;
    }
    wlr_egl* egl = wlr_egl_create_with_drm_fd(drmFd);
    wlr_renderer* renderer = egl ? wlr_gles2_renderer_create(egl) : nullptr;
    if (!renderer) { close(drmFd); return 1; }
    Reply reply{};
    size_t count{};
    bool success = appendFormats(reply, wlr_renderer_get_texture_formats(renderer, WLR_BUFFER_CAP_DMABUF), count);
    reply.textureCount = count;
    success = success && appendFormats(reply, renderer->WLR_PRIVATE.impl->get_render_formats(renderer), count);
    reply.renderCount = count - reply.textureCount;
    reply.success = success;
    success = sendPacket(kWorkerSocket, &reply, sizeof(reply));
    while (success) {
      fds.clear();
      if (!receivePacket(kWorkerSocket, &request, sizeof(request), fds, -1)) { closeFds(fds); break; }
      if (request.operation == Operation::Stop && fds.empty()) { break; }
      if (request.operation != Operation::Blit || !validAttributes(request.source)
          || !validAttributes(request.destination)
          || fds.size() != static_cast<size_t>(request.source.n_planes + request.destination.n_planes)) {
        closeFds(fds);
        break;
      }
      auto* source = importBuffer(request.source, fds, 0);
      auto* destination = importBuffer(request.destination, fds, request.source.n_planes);
      auto* texture = waitForWriters(source->attributes) ? wlr_texture_from_buffer(renderer, &source->base) : nullptr;
      bool copied = false;
      if (texture) {
        auto* pass = wlr_renderer_begin_buffer_pass(renderer, &destination->base, nullptr);
        if (pass) {
          const wlr_render_texture_options options{
              .texture = texture, .blend_mode = WLR_RENDER_BLEND_MODE_NONE};
          wlr_render_pass_add_texture(pass, &options);
          copied = wlr_render_pass_submit(pass);
          if (copied && wlr_egl_make_current(egl, nullptr)) {
            // Complete the copy before scanout and buffer reuse in the parent.
            glFinish();
            copied = glGetError() == GL_NO_ERROR;
          }
          else { copied = false; }
        }
        wlr_texture_destroy(texture);
      }
      wlr_buffer_drop(&source->base);
      wlr_buffer_drop(&destination->base);
      const uint32_t acknowledgement = copied;
      success = sendPacket(kWorkerSocket, &acknowledgement, sizeof(acknowledgement));
    }
    wlr_renderer_destroy(renderer);
    close(drmFd);
    close(kWorkerSocket);
    return 0;
  }
}

extern "C" wlr_renderer* wlr_gles2_renderer_create_with_drm_fd(int drmFd) {
  if (isolated) { return createRemoteRenderer(drmFd); }
  wlr_egl* egl = wlr_egl_create_with_drm_fd(drmFd);
  return egl ? wlr_gles2_renderer_create(egl) : nullptr;
}
#else
namespace umbriel {
  bool setIsolatedCopyRenderer(bool) { return false; }
  int copyRendererWorker() { return 1; }
}
#endif
