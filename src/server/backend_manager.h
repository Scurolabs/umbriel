#pragma once

#include "config/config.h"

#include <expected>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

struct wl_display;
struct wlr_backend;
struct wlr_renderer;
struct wlr_session;

namespace umbriel {

  // Owns backend selection, hotplug handling, and the GPU exclusions fixed at startup.
  class BackendManager {
  public:
    [[nodiscard]] static std::unique_ptr<BackendManager> create(wl_display* display, const Config::Drm& config);
    ~BackendManager();

    BackendManager(const BackendManager&) = delete;
    BackendManager& operator=(const BackendManager&) = delete;

    [[nodiscard]] wlr_backend* backend() const;
    [[nodiscard]] wlr_session* session() const;
    [[nodiscard]] wlr_renderer* createRenderer();
    [[nodiscard]] bool verifyOpenDevices(std::string_view context);
    void markStarted();
    [[nodiscard]] nlohmann::json gpuStatus() const;
    [[nodiscard]] std::expected<void, std::string> releaseGpu(std::string_view pciAddress);
    [[nodiscard]] std::expected<void, std::string> acquireGpu(std::string_view pciAddress);

  private:
    class Impl;
    explicit BackendManager(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
