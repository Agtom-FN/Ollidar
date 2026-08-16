// Icons.h — the rail's line icons, drawn with QPainter.
//
// WHY NOT SVG. The mockup's icons are Lucide (24x24 viewBox, stroke-width
// 1.8, round caps and joins). Rendering them from SVG would mean linking
// Qt6::Svg, which is an extra module the Windows/Linux CI legs do not
// currently install — a new way for a cross-platform build to fail for a
// reason that has nothing to do with the redesign. QPainter draws the same
// geometry with no new dependency and no asset pipeline, and it recolours per
// state for free (the rail needs three tints: faint / ink / ember).
//
// Everything is authored in the same 24x24 space Lucide uses and scaled to
// the requested pixel size, so a 20 px rail icon and a 15 px inline icon are
// the same drawing.
//
// Owner: redesign pass.
#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>

class QPainter;

namespace lidarscan {
namespace icons {

enum class Glyph {
  kFolder,    // projects
  kRadar,     // capture
  kRotate3d,  // review
  kLayout,    // floor plan
  kGitMerge,  // merge
  kLayers,    // jobs / processing
  kSliders,   // display inspector (rail bottom)
  kPackage,   // transfer bundle
  kFileDown,  // export
  kShare,     // the export button's leading glyph
};

// Strokes `g` into `p`'s current 24x24 logical space using the current pen.
// Exposed so a widget that already has a painter (the record cluster's dot,
// a delegate) can draw one inline without allocating a pixmap.
void paint(QPainter& p, Glyph g);

// A device-pixel-ratio-correct pixmap of `g` at `px` logical pixels, stroked
// in `color`. `dpr` should be the target widget's devicePixelRatioF().
QPixmap pixmap(Glyph g, const QColor& color, int px, qreal dpr);

// Normal/active variants baked into one QIcon (Normal = `color`,
// Selected/Active = `active`).
QIcon icon(Glyph g, const QColor& color, const QColor& active, int px, qreal dpr);

}  // namespace icons
}  // namespace lidarscan
