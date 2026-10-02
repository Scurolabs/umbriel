#pragma once

namespace umbriel {

  // Where a view's content sits inside its frame, and what decides how chrome around it is drawn.
  struct ViewChromeGeometry {
    int contentX = 0;
    int contentY = 0;
    int contentWidth = 0;
    int contentHeight = 0;
    // Border thickness drawn around the content, 0 when undecorated.
    int borderInset = 0;
    // Scale of the output presenting the view, for chrome that renders text.
    float scale = 1.0F;
    // Set while the view draws nothing around its content: unmapped, fullscreen, or maximized to the edges.
    bool suppressed = false;
    bool operator==(const ViewChromeGeometry&) const = default;
  };

  // Compositor chrome a view carries beside its own borders, such as a tab bar. Its scene nodes hang under the view's
  // frame, so it follows every position, slide, and visibility change the view makes. The view owns it, destroys it
  // before the frame, and reports to it every change chrome follows; it knows nothing else about it.
  class ViewChromeAttachment {
  public:
    ViewChromeAttachment() = default;
    virtual ~ViewChromeAttachment() = default;
    ViewChromeAttachment(const ViewChromeAttachment&) = delete;
    ViewChromeAttachment& operator=(const ViewChromeAttachment&) = delete;

    virtual void layout(const ViewChromeGeometry& geometry) = 0;
    virtual void setFocused(bool focused) = 0;
    // The view's fade, drag, and overview opacity, which chrome fades with.
    virtual void setAlpha(float alpha) = 0;
    // The configuration was reloaded.
    virtual void reloadConfig() = 0;
  };

} // namespace umbriel
