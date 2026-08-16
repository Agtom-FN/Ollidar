// IconRail.h — the 56 px left navigation rail that replaces the tabbed-dock
// strip (redesign brief item 2; mockup `.rail`, docs/design/lidarscan-interfaces.html
// lines 457-466 and 2513-2523).
//
// WHAT IT REPLACES. Before this pass every workspace after C1 arrived as one
// more tab in a five-deep tabified dock group on the right, plus a Capture
// menu item. That strip is the "wide navigation" the redesign removes: the
// rail is now the only place a workspace is chosen, and MainWindow shows
// exactly one right-hand dock at a time in response.
//
// NOTHING IS LOST. Every dock keeps its QDockWidget identity — it can still
// float, resize, and be toggled from the View menu — and the two file-movement
// screens (transfer, export) that have no dock of their own get rail buttons
// that open their existing dialogs. See MainWindow::onRailActivated().
//
// The rail is a plain QWidget of fixed width; it is placed in the left dock
// area behind an empty title-bar widget so it cannot be dragged out or
// closed, which keeps the central native viewport untouched.
//
// Owner: redesign pass.
#pragma once

#include <QVector>
#include <QWidget>

#include "ui/Icons.h"

class QVBoxLayout;

namespace lidarscan {

class RailButton;

class IconRail : public QWidget {
  Q_OBJECT
 public:
  // The rail's identity for each destination. `kInspector` is the pinned
  // bottom item: it is a TOGGLE, not a workspace, and it is the only item
  // whose pressed state is not simply "this workspace is current".
  enum class Item {
    kProjects,
    kCapture,
    kReview,
    kPlan,
    kMerge,
    kJobs,
    kTransfer,
    kExport,
    kInspector,
  };
  Q_ENUM(Item)

  explicit IconRail(QWidget* parent = nullptr);

  // Marks `item` as the current workspace. kInspector is never "current" —
  // use setInspectorOn() for it.
  void setCurrent(Item item);
  Item current() const { return current_; }

  void setInspectorOn(bool on);
  bool inspectorOn() const { return inspector_on_; }

 Q_SIGNALS:
  void activated(lidarscan::IconRail::Item item);

 private:
  void addButton(Item item, icons::Glyph glyph, const QString& label, QVBoxLayout* into);
  void refreshStates();

  QVector<RailButton*> buttons_;
  Item current_ = Item::kProjects;
  bool inspector_on_ = true;
};

}  // namespace lidarscan
