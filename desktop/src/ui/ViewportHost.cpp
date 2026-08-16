#include "ui/ViewportHost.h"

#include <QEvent>
#include <QResizeEvent>
#include <QVBoxLayout>

#include "ui/Theme.h"

namespace lidarscan {

ViewportHost::ViewportHost(QWidget* parent) : QWidget(parent) {
  setObjectName("viewportHost");
  setAutoFillBackground(true);
  // The viewport's own backdrop shows through only for the instant before the
  // renderer's first frame, and in the (documented) case where the renderer
  // fails to initialise at all. Paint it the mockup's viewport ground rather
  // than leaving it the app backdrop, so that failure looks deliberate.
  setStyleSheet("#viewportHost { background: #0B0E12; }");
}

void ViewportHost::setViewportWidget(QWidget* container) {
  container_ = container;
  container_->setParent(this);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);
  v->addWidget(container_);
}

void ViewportHost::promoteToNative(QWidget* w) {
  // WA_NativeWindow gives the overlay its own backing surface, which is the
  // only way it can be ordered above the viewport's swapchain surface (see
  // the header). WA_DontCreateNativeAncestors keeps the promotion from
  // cascading up the parent chain and turning every dock into a native
  // window, which costs real memory and breaks nothing visibly but is pure
  // waste.
  w->setAttribute(Qt::WA_DontCreateNativeAncestors, true);
  w->setAttribute(Qt::WA_NativeWindow, true);
}

void ViewportHost::addOverlay(QWidget* w, Anchor anchor, int inset, int gap) {
  w->setParent(this);
  promoteToNative(w);
  w->raise();
  w->installEventFilter(this);
  overlays_.push_back(Entry{w, anchor, inset, gap});
  layoutOverlays();
}

void ViewportHost::removeOverlay(QWidget* w) {
  for (int i = 0; i < overlays_.size(); ++i) {
    if (overlays_[i].w == w) {
      overlays_[i].w->removeEventFilter(this);
      overlays_.remove(i);
      break;
    }
  }
  layoutOverlays();
}

void ViewportHost::layoutOverlays() {
  // Per-anchor running offset, so two chips on the same corner stack instead
  // of overprinting each other.
  int used[4] = {0, 0, 0, 0};
  for (const Entry& e : overlays_) {
    if (!e.w || e.w->isHidden()) continue;
    const int idx = int(e.anchor);
    QSize sz = e.w->sizeHint();
    // An overlay may cap itself (the inspector does, at ~62 % of the viewport
    // height); honour maximumHeight/Width rather than the raw hint.
    sz.setWidth(qMin(qMax(sz.width(), e.w->minimumWidth()),
                     qMin(e.w->maximumWidth(), width() - 2 * e.inset)));
    sz.setHeight(qMin(qMax(sz.height(), e.w->minimumHeight()),
                      qMin(e.w->maximumHeight(), height() - 2 * e.inset)));
    if (sz.width() < 1 || sz.height() < 1) continue;

    int x = e.inset;
    int y = e.inset;
    switch (e.anchor) {
      case Anchor::kTopLeft:
        y = e.inset + used[idx];
        break;
      case Anchor::kTopRight:
        x = width() - e.inset - sz.width();
        y = e.inset + used[idx];
        break;
      case Anchor::kBottomLeft:
        y = height() - e.inset - used[idx] - sz.height();
        break;
      case Anchor::kBottomRight:
        x = width() - e.inset - sz.width();
        y = height() - e.inset - used[idx] - sz.height();
        break;
    }
    e.w->setGeometry(x, y, sz.width(), sz.height());
    e.w->raise();
    used[idx] += sz.height() + e.gap;
  }
}

void ViewportHost::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  layoutOverlays();
}

bool ViewportHost::eventFilter(QObject* watched, QEvent* event) {
  switch (event->type()) {
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::LayoutRequest:
      for (const Entry& e : overlays_) {
        if (e.w == watched) {
          layoutOverlays();
          break;
        }
      }
      break;
    default:
      break;
  }
  return QWidget::eventFilter(watched, event);
}

}  // namespace lidarscan
