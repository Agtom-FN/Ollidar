// Theme.h — the redesign's design tokens, bundled typefaces and application
// stylesheet (docs/design/REVIEW_FEEDBACK.md + redesign-exports/05-desktop-macos.png,
// behaviour reference docs/design/lidarscan-interfaces.html).
//
// This is CHROME ONLY. Nothing here knows about the engine, the capture state
// machine or the renderer; every widget it styles keeps the behaviour it had
// before the redesign. That separation is what makes the rework safe to land
// on a shipped app.
//
// PORTABILITY. Everything is plain Qt: QFontDatabase for the fonts, QPalette
// for the roles Qt paints outside the stylesheet (tooltips, selections,
// QColorDialog), and one QSS string for the rest. There is no #ifdef and no
// platform call anywhere in this file, so the Windows/Linux CI legs
// (.github/workflows/desktop-ci.yml) compile it unchanged, and the macOS
// traffic lights / native window chrome are untouched — QSS never reaches
// them.
//
// Owner: redesign pass.
#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

class QApplication;

namespace lidarscan {
namespace theme {

// --- tokens ---------------------------------------------------------------
//
// The owner-approved palette. `ground` is the app backdrop, `panel`/`panel2`
// the two card elevations, `line` every hairline, `ink`/`mute` the two text
// weights, `ember` the single brand accent, and good/warn/bad the semantic
// triad the health readouts already needed (they were hard-coded #2e7d32 /
// #c62828 in CaptureWindow before this pass).
QColor ground();     // #12161B  app backdrop
QColor panel();      // #1A2027  card
QColor panel2();     // #222A33  raised card / input well
QColor panel3();     // #2A333D  hover / pressed
QColor line();       // #2B3540  hairline
QColor ink();        // #ECF1F5  primary text
QColor mute();       // #94A1AD  secondary text
QColor faint();      // #5C6873  tertiary text
QColor ember();      // #FF7A52  brand accent
QColor emberDim();   // #C25334  accent, pressed/disabled
QColor emberSoft();  // rgba(ember, .15) — the rail's active capsule wash
QColor good();       // #49D17F  RTK fixed / healthy / passed
QColor warn();       // #E5B93C  float / degraded / gated
QColor bad();        // #E05252  single / fault / recording
QColor pose();       // #6AA7E8  trajectory / pose stream

// Radii, in px. Cards are deliberately rounder than stock Qt.
constexpr int kRadiusCard = 18;
constexpr int kRadiusPanel = 20;
constexpr int kRadiusTile = 14;
constexpr int kRadiusPill = 999;  // QSS clamps to height/2

// The left icon rail's fixed width, shared by IconRail and MainWindow's
// overlay geometry maths.
constexpr int kRailWidth = 56;

// Width below which the floating inspector reflows into an ordinary dock.
constexpr int kInspectorReflowWidth = 880;

// --- typefaces ------------------------------------------------------------
//
// Resolved after install(); before it they fall back to the platform UI font,
// so a build with the .qrc stripped still runs (and says so).
QString displayFamily();  // Space Grotesk — headings, window/section titles
QString sansFamily();     // Inter — all body UI text
QString monoFamily();     // JetBrains Mono — EVERY telemetry/stat readout

// True once addApplicationFont() accepted at least one face of each family.
bool fontsLoaded();
// One line per family, e.g. "Space Grotesk (3 faces)". For Help -> About and
// for the --font-report CLI evidence hook.
QStringList fontReport();
// The bundled OFL text for a family name as returned above ("" if unknown).
QString licenceText(const QString& family);

// --- install --------------------------------------------------------------
//
// Loads the fonts out of :/fonts, sets the application palette and applies
// the stylesheet. Call once, immediately after constructing QApplication and
// BEFORE any widget is created (a QSS applied later still restyles, but
// widgets that cached a font metric at construction do not re-layout).
void install(QApplication& app);

// The stylesheet on its own, for a widget that needs to re-polish itself.
QString styleSheet();

// --- helpers --------------------------------------------------------------
//
// `rgba(r,g,b,a)` for embedding a token in a per-widget stylesheet.
QString css(const QColor& c, double alpha = 1.0);
// A mono font at `pt`, weight `weight` (QFont::Weight numeric), for the stat
// readouts that are set in code rather than QSS.
QString monoCss(int px, int weight = 400);

}  // namespace theme
}  // namespace lidarscan
