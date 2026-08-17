#include "ui/IconRail.h"

#include <QAbstractButton>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include "ui/Theme.h"

namespace lidarscan {

// A 38x38 rail button that paints its own active capsule. A QSS-styled
// QToolButton could get close, but the capsule needs an inset ring at 30 %
// ember over a background at 16 % ember, and QSS cannot express an inset
// box-shadow — so this draws it, which also makes the icon tint and the
// capsule impossible to get out of sync.
class RailButton : public QAbstractButton {
 public:
  RailButton(IconRail::Item item, icons::Glyph glyph, const QString& label, QWidget* parent)
      : QAbstractButton(parent), item_(item), glyph_(glyph) {
    setCheckable(true);
    setFixedSize(38, 38);
    setCursor(Qt::PointingHandCursor);
    setToolTip(label);
    setAccessibleName(label);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover, true);
  }

  IconRail::Item item() const { return item_; }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool on = isChecked();
    const bool hover = underMouse() && isEnabled();
    const QRectF r(0.5, 0.5, width() - 1.0, height() - 1.0);

    if (on) {
      QColor wash = theme::ember();
      wash.setAlphaF(0.16);
      QPainterPath cap;
      cap.addRoundedRect(r, 11.0, 11.0);
      p.fillPath(cap, wash);
      QColor ring = theme::ember();
      ring.setAlphaF(0.30);
      p.setPen(QPen(ring, 1.0));
      p.setBrush(Qt::NoBrush);
      p.drawPath(cap);
    } else if (hover) {
      QPainterPath cap;
      cap.addRoundedRect(r, 11.0, 11.0);
      p.fillPath(cap, theme::panel2());
    }

    QColor tint = theme::faint();
    if (on) tint = theme::ember();
    else if (hover) tint = theme::ink();
    if (!isEnabled()) tint = theme::line();

    const QPixmap pm = icons::pixmap(glyph_, tint, 20, devicePixelRatioF());
    const QPointF at((width() - 20) / 2.0, (height() - 20) / 2.0);
    p.drawPixmap(at, pm);

    if (hasFocus()) {
      QColor ring = theme::ember();
      ring.setAlphaF(0.55);
      p.setPen(QPen(ring, 1.0, Qt::DashLine));
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(r.adjusted(1.5, 1.5, -1.5, -1.5), 9.0, 9.0);
    }
  }

  void enterEvent(QEnterEvent* e) override {
    QAbstractButton::enterEvent(e);
    update();
  }
  void leaveEvent(QEvent* e) override {
    QAbstractButton::leaveEvent(e);
    update();
  }

 private:
  IconRail::Item item_;
  icons::Glyph glyph_;
};

IconRail::IconRail(QWidget* parent) : QWidget(parent) {
  setObjectName("iconRail");
  setFixedWidth(theme::kRailWidth);
  setAutoFillBackground(true);
  // The rail sits on the app backdrop, not on a panel — it is the edge of the
  // window, and the mockup separates it from the body with one soft hairline.
  setStyleSheet(QString("#iconRail { background: %1; border-right: 1px solid %2; }")
                    .arg(theme::css(theme::ground()), theme::css(theme::panel2())));

  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 10, 0, 10);
  v->setSpacing(6);
  v->setAlignment(Qt::AlignHCenter);

  addButton(Item::kProjects, icons::Glyph::kFolder, "Projects", v);
  addButton(Item::kCapture, icons::Glyph::kRadar, "Capture", v);
  addButton(Item::kReview, icons::Glyph::kRotate3d, "Review", v);
  addButton(Item::kPlan, icons::Glyph::kLayout, "Floor plan", v);
  // NO Merge and NO Processing button any more (round-5 follow-up, owner:
  // "there are NO separate Processing/Merge tabs — fold them into the Projects
  // tab"). Both docks still exist, unchanged, and both are still reachable —
  // from the PROJECTS panel's own selection-driven actions (one project selected
  // -> Process/Export; two or more -> Merge), from the View menu, and from
  // --workspace jobs|merge, which now lands on Projects with that panel raised.
  // Item::kMerge/Item::kJobs therefore stay in the enum: they name a panel
  // inside Projects rather than a rail destination. See
  // MainWindow::onRailActivated().

  v->addStretch(1);

  addButton(Item::kTransfer, icons::Glyph::kPackage, "Transfer bundle", v);
  addButton(Item::kExport, icons::Glyph::kFileDown, "Export", v);
  addButton(Item::kInspector, icons::Glyph::kSliders, "Display inspector", v);

  refreshStates();
}

void IconRail::addButton(Item item, icons::Glyph glyph, const QString& label, QVBoxLayout* into) {
  auto* b = new RailButton(item, glyph, label, this);
  connect(b, &QAbstractButton::clicked, this, [this, item] {
    Q_EMIT activated(item);
    // The rail does NOT assume the click succeeded: MainWindow calls
    // setCurrent()/setInspectorOn() back once it has actually switched, so a
    // refused navigation (e.g. Export with no cloud loaded) cannot leave the
    // rail lit on a workspace that is not showing.
    refreshStates();
  });
  buttons_.push_back(b);
  into->addWidget(b, 0, Qt::AlignHCenter);
}

void IconRail::setCurrent(Item item) {
  if (item == Item::kInspector) return;  // a toggle, never "the workspace"
  current_ = item;
  refreshStates();
}

void IconRail::setInspectorOn(bool on) {
  inspector_on_ = on;
  refreshStates();
}

void IconRail::refreshStates() {
  for (RailButton* b : buttons_) {
    const bool on = b->item() == Item::kInspector
                        ? (current_ == Item::kReview && inspector_on_)
                        : b->item() == current_;
    if (b->isChecked() != on) b->setChecked(on);
    b->update();
  }
}

}  // namespace lidarscan
