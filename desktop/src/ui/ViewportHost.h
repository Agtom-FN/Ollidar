// ViewportHost.h — the widget that owns the embedded native viewport and the
// chrome that floats on top of it (the project chip, the render-stats chip,
// the RECORDING/PAUSED badge and the floating display inspector).
//
// THE ONE HARD PROBLEM THIS SOLVES. The 3D viewport is a native child window
// (QWidget::createWindowContainer over a QWindow with a CAMetalLayer / Vulkan
// surface on it — NOTES.md §1.1). A native child window is composited by the
// WINDOW SYSTEM, above every non-native sibling widget, so an ordinary QWidget
// laid out "over" it is simply invisible: Qt paints it into the backing store
// that the native surface then covers. This is not a macOS quirk; it is the
// same on Windows (child HWND) and X11.
//
// The fix is to make each overlay native too, and then order the siblings.
// Sibling native surfaces DO honour Z-order on all three platforms
// (NSView ordering, HWND Z-order, X11 stacking), so `overlay->raise()` after
// the container is created puts the overlay above the swapchain. That is one
// Qt attribute and one call, with no platform code — the Windows/Linux CI legs
// compile this file unchanged.
//
// The cost is that a native overlay cannot be alpha-composited against the
// live 3D content: its background is opaque as far as the compositor is
// concerned. The design absorbs that by painting the card at the panel colour
// over a ~92 % wash rather than relying on real translucency; against the
// viewport's near-black backdrop the two are visually indistinguishable, and
// the alternative (a separate frameless tool window that has to chase the main
// window around the screen) is far more to go wrong.
//
// Owner: redesign pass.
#pragma once

#include <QVector>
#include <QWidget>

namespace lidarscan {

class ViewportHost : public QWidget {
  Q_OBJECT
 public:
  // Where an overlay parks. Anchors are resolved in layoutOverlays(), so a
  // caller never positions anything by hand.
  enum class Anchor { kTopLeft, kTopRight, kBottomLeft, kBottomRight };

  explicit ViewportHost(QWidget* parent = nullptr);

  // Takes the createWindowContainer() widget. Must be called exactly once,
  // before any overlay is added.
  void setViewportWidget(QWidget* container);
  QWidget* viewportWidget() const { return container_; }

  // Registers `w` as a floating overlay. Ownership moves to this widget.
  // `inset` is the margin from the anchored corner in logical pixels.
  // Overlays sharing an anchor stack in registration order, `gap` apart.
  void addOverlay(QWidget* w, Anchor anchor, int inset = 14, int gap = 8);
  void removeOverlay(QWidget* w);

  // Re-runs the anchor maths. Called automatically on resize and whenever an
  // overlay is shown/hidden or changes size; public so a caller that has just
  // changed an overlay's contents can force it without waiting for an event.
  void layoutOverlays();

 protected:
  void resizeEvent(QResizeEvent* e) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  struct Entry {
    QWidget* w = nullptr;
    Anchor anchor = Anchor::kTopLeft;
    int inset = 14;
    int gap = 8;
  };

  void promoteToNative(QWidget* w);

  QWidget* container_ = nullptr;
  QVector<Entry> overlays_;
};

}  // namespace lidarscan
