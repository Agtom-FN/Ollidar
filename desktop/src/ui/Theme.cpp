#include "ui/Theme.h"

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <QStringList>

namespace lidarscan {
namespace theme {
namespace {

// The owner-approved tokens. These are the redesign brief's values; the
// mockup's own :root (docs/design/lidarscan-interfaces.html) sits a hair
// darker/cooler on the neutrals (#11151a / #181e25 / #2a333d / #e9edf1 /
// #8b97a3) and identical on the accents. The brief wins on the neutrals, the
// mockup supplies the semantic triad it does not name.
constexpr const char* kGround = "#12161B";
constexpr const char* kPanel = "#1A2027";
constexpr const char* kPanel2 = "#222A33";
constexpr const char* kPanel3 = "#2A333D";
constexpr const char* kLine = "#2B3540";
constexpr const char* kInk = "#ECF1F5";
constexpr const char* kMute = "#94A1AD";
constexpr const char* kFaint = "#5C6873";
constexpr const char* kEmber = "#FF7A52";
constexpr const char* kEmberDim = "#C25334";
constexpr const char* kGood = "#49D17F";
constexpr const char* kWarn = "#E5B93C";
constexpr const char* kBad = "#E05252";
constexpr const char* kPose = "#6AA7E8";

// Ink used ON an ember fill. Pure white on #FF7A52 fails contrast; the mockup
// uses this near-black brown throughout (`color:#1a0d08`).
constexpr const char* kOnEmber = "#1A0D08";

struct FontState {
  QString display;
  QString sans;
  QString mono;
  int display_faces = 0;
  int sans_faces = 0;
  int mono_faces = 0;
  bool installed = false;
};

FontState& fonts() {
  static FontState s;
  return s;
}

// Adds one .ttf out of the resource system and returns the family it
// registered, or an empty string. Qt returns -1 for a file it cannot parse
// (and, notably, for a .qrc that was never compiled in), so this doubles as
// the "were the resources actually built?" probe.
QString addFace(const QString& path, int* face_count) {
  const int id = QFontDatabase::addApplicationFont(path);
  if (id < 0) return QString();
  const QStringList fams = QFontDatabase::applicationFontFamilies(id);
  if (fams.isEmpty()) return QString();
  if (face_count) ++(*face_count);
  return fams.front();
}

QString firstNonEmpty(const QStringList& l, const QString& fallback) {
  for (const QString& s : l) {
    if (!s.isEmpty()) return s;
  }
  return fallback;
}

void loadFonts() {
  FontState& f = fonts();
  if (f.installed) return;
  f.installed = true;

  QStringList display, sans, mono;
  display << addFace(":/fonts/SpaceGrotesk-Regular.ttf", &f.display_faces)
          << addFace(":/fonts/SpaceGrotesk-Medium.ttf", &f.display_faces)
          << addFace(":/fonts/SpaceGrotesk-Bold.ttf", &f.display_faces);
  sans << addFace(":/fonts/Inter-Regular.ttf", &f.sans_faces)
       << addFace(":/fonts/Inter-Medium.ttf", &f.sans_faces)
       << addFace(":/fonts/Inter-SemiBold.ttf", &f.sans_faces)
       << addFace(":/fonts/Inter-Bold.ttf", &f.sans_faces);
  mono << addFace(":/fonts/JetBrainsMono-Regular.ttf", &f.mono_faces)
       << addFace(":/fonts/JetBrainsMono-Medium.ttf", &f.mono_faces)
       << addFace(":/fonts/JetBrainsMono-Bold.ttf", &f.mono_faces);

  // Fall back to whatever the platform has rather than to nothing: an app
  // that cannot find its own resources should still be legible. The mono
  // fallback matters most, because every telemetry readout is column-aligned
  // and a proportional fallback makes the numbers jitter.
  f.display = firstNonEmpty(display, QString());
  f.sans = firstNonEmpty(sans, QString());
  f.mono = firstNonEmpty(mono, QString());
  if (f.display.isEmpty()) f.display = QApplication::font().family();
  if (f.sans.isEmpty()) f.sans = QApplication::font().family();
  if (f.mono.isEmpty()) {
    f.mono = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
  }
}

}  // namespace

QColor ground() { return QColor(kGround); }
QColor panel() { return QColor(kPanel); }
QColor panel2() { return QColor(kPanel2); }
QColor panel3() { return QColor(kPanel3); }
QColor line() { return QColor(kLine); }
QColor ink() { return QColor(kInk); }
QColor mute() { return QColor(kMute); }
QColor faint() { return QColor(kFaint); }
QColor ember() { return QColor(kEmber); }
QColor emberDim() { return QColor(kEmberDim); }
QColor emberSoft() {
  QColor c(kEmber);
  c.setAlphaF(0.15);
  return c;
}
QColor good() { return QColor(kGood); }
QColor warn() { return QColor(kWarn); }
QColor bad() { return QColor(kBad); }
QColor pose() { return QColor(kPose); }

QString displayFamily() {
  loadFonts();
  return fonts().display;
}
QString sansFamily() {
  loadFonts();
  return fonts().sans;
}
QString monoFamily() {
  loadFonts();
  return fonts().mono;
}

bool fontsLoaded() {
  loadFonts();
  const FontState& f = fonts();
  return f.display_faces > 0 && f.sans_faces > 0 && f.mono_faces > 0;
}

QStringList fontReport() {
  loadFonts();
  const FontState& f = fonts();
  QStringList out;
  out << QString("display: %1 (%2 faces)%3")
             .arg(f.display)
             .arg(f.display_faces)
             .arg(f.display_faces ? "" : "  [BUNDLE MISSING — platform fallback]");
  out << QString("sans:    %1 (%2 faces)%3")
             .arg(f.sans)
             .arg(f.sans_faces)
             .arg(f.sans_faces ? "" : "  [BUNDLE MISSING — platform fallback]");
  out << QString("mono:    %1 (%2 faces)%3")
             .arg(f.mono)
             .arg(f.mono_faces)
             .arg(f.mono_faces ? "" : "  [BUNDLE MISSING — platform fallback]");
  return out;
}

QString licenceText(const QString& family) {
  QString path;
  if (family.contains("Inter", Qt::CaseInsensitive)) path = ":/licenses/Inter-OFL.txt";
  else if (family.contains("Grotesk", Qt::CaseInsensitive)) path = ":/licenses/SpaceGrotesk-OFL.txt";
  else if (family.contains("JetBrains", Qt::CaseInsensitive)) path = ":/licenses/JetBrainsMono-OFL.txt";
  if (path.isEmpty()) return QString();
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return QString();
  return QString::fromUtf8(f.readAll());
}

QString css(const QColor& c, double alpha) {
  if (alpha >= 1.0) {
    return QString("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue());
  }
  return QString("rgba(%1,%2,%3,%4)")
      .arg(c.red())
      .arg(c.green())
      .arg(c.blue())
      .arg(alpha, 0, 'f', 3);
}

QString monoCss(int px, int weight) {
  return QString("font-family:'%1';font-size:%2px;font-weight:%3;")
      .arg(monoFamily())
      .arg(px)
      .arg(weight);
}

QString styleSheet() {
  loadFonts();
  const FontState& f = fonts();

  // Colours that need an alpha channel in QSS. Qt's stylesheet parser accepts
  // rgba() everywhere a colour is accepted, including in `border` shorthand.
  const QString emberWash = css(ember(), 0.16);
  const QString emberRing = css(ember(), 0.30);
  const QString badWash = css(bad(), 0.18);
  const QString badRing = css(bad(), 0.80);
  const QString warnWash = css(warn(), 0.10);
  const QString warnRing = css(warn(), 0.45);
  const QString goodWash = css(good(), 0.10);
  const QString goodRing = css(good(), 0.45);

  QString s;
  s += QString(R"(
/* ===== base ============================================================ */
* { outline: none; }

QWidget {
  background: %GROUND%;
  color: %INK%;
  font-family: '%SANS%';
  font-size: 13px;
}

QMainWindow, QDialog { background: %GROUND%; }

QToolTip {
  background: %PANEL3%;
  color: %INK%;
  border: 1px solid %LINE%;
  border-radius: 8px;
  padding: 6px 9px;
  font-family: '%SANS%';
  font-size: 12px;
}

/* ===== menus =========================================================== */
QMenuBar { background: %PANEL2%; color: %MUTE%; border-bottom: 1px solid %LINE%; padding: 2px 6px; }
QMenuBar::item { padding: 4px 10px; border-radius: 6px; background: transparent; }
QMenuBar::item:selected { background: %PANEL3%; color: %INK%; }
QMenu {
  background: %PANEL3%;
  border: 1px solid %LINE%;
  border-radius: 10px;
  padding: 5px;
}
QMenu::item { padding: 6px 14px; border-radius: 6px; font-size: 12.5px; }
QMenu::item:selected { background: %EMBERWASH%; color: %EMBER%; }
QMenu::item:disabled { color: %FAINT%; }
QMenu::separator { height: 1px; background: %LINE%; margin: 4px 8px; }

/* ===== docks =========================================================== */
QDockWidget {
  color: %FAINT%;
  font-family: '%MONO%';
  font-size: 10px;
  titlebar-close-icon: none;
  titlebar-normal-icon: none;
}
QDockWidget::title {
  background: %GROUND%;
  padding: 12px 14px 7px 14px;
  text-align: left;
  border: none;
}
QDockWidget > QWidget { background: %PANEL%; }
QDockWidget::close-button, QDockWidget::float-button {
  background: transparent; border: none; padding: 0px; icon-size: 11px;
}
QDockWidget::close-button:hover, QDockWidget::float-button:hover { background: %PANEL3%; border-radius: 5px; }
QMainWindow::separator { background: %GROUND%; width: 6px; height: 6px; }
QMainWindow::separator:hover { background: %LINE%; }

/* ===== cards =========================================================== */
QGroupBox {
  background: %PANEL%;
  border: 1px solid %LINESOFT%;
  border-radius: %RCARD%px;
  margin-top: 16px;
  padding: 14px 13px 12px 13px;
  font-family: '%MONO%';
  font-size: 9.5px;
  font-weight: 600;
  color: %FAINT%;
}
QGroupBox::title {
  subcontrol-origin: margin;
  subcontrol-position: top left;
  left: 4px;
  top: 2px;
  padding: 0px 2px;
  background: transparent;
  color: %FAINT%;
}
QFrame[card="true"] {
  background: %PANEL%;
  border: 1px solid %LINESOFT%;
  border-radius: %RCARD%px;
}

/* ===== buttons ========================================================= */
QPushButton {
  background: %PANEL2%;
  border: 1px solid %LINE%;
  border-radius: 14px;
  padding: 7px 15px;
  color: %INK%;
  font-size: 12.5px;
  font-weight: 500;
  min-height: 14px;
}
QPushButton:hover { background: %PANEL3%; border-color: %LINEHOVER%; }
QPushButton:pressed { background: %PANEL%; }
QPushButton:disabled { color: %FAINT%; background: %PANEL%; border-color: %LINESOFT%; }
QPushButton:default { border-color: %EMBERDIM%; }

/* the one brand call-to-action */
QPushButton[accent="ember"] {
  background: %EMBER%;
  border: 1px solid %EMBER%;
  color: %ONEMBER%;
  font-family: '%DISPLAY%';
  font-weight: 700;
}
QPushButton[accent="ember"]:hover { background: #FF8F6C; border-color: #FF8F6C; }
QPushButton[accent="ember"]:pressed { background: %EMBERDIM%; border-color: %EMBERDIM%; }
QPushButton[accent="ember"]:disabled { background: %EMBERDIM%; border-color: %EMBERDIM%; color: %FAINT%; }

QPushButton[accent="danger"] {
  background: %BADWASH%;
  border: 1px solid %BADRING%;
  color: #FF9A9A;
  font-family: '%DISPLAY%';
  font-weight: 700;
}
QPushButton[accent="danger"]:hover { background: %BADHOVER%; border-color: %BAD%; }

QPushButton[flat="true"] { background: transparent; border: none; padding: 4px 8px; color: %MUTE%; }
QPushButton[flat="true"]:hover { color: %INK%; background: %PANEL2%; }

/* segmented chips (colour mode, units, ...) */
QPushButton[chip="true"] {
  background: transparent;
  border: 1px solid transparent;
  border-radius: 11px;
  padding: 4px 7px;
  font-family: '%MONO%';
  font-size: 9px;
  font-weight: 500;
  color: %MUTE%;
  min-height: 0px;
}
QPushButton[chip="true"]:hover { background: %PANEL3%; color: %INK%; }
QPushButton[chip="true"]:checked {
  background: %EMBER%;
  border-color: %EMBER%;
  color: %ONEMBER%;
  font-weight: 700;
}
QPushButton[chip="true"]:disabled { color: %FAINT%; }
QWidget[chipgroup="true"] {
  background: %PANEL2%;
  border: 1px solid %LINE%;
  border-radius: 14px;
}

/* ===== inputs ========================================================== */
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {
  background: %PANEL2%;
  border: 1px solid %LINE%;
  border-radius: 10px;
  padding: 5px 9px;
  color: %INK%;
  font-family: '%MONO%';
  font-size: 12px;
  selection-background-color: %EMBERWASH%;
  selection-color: %INK%;
}
QLineEdit:focus, QPlainTextEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
  border-color: %EMBERDIM%;
}
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled, QComboBox:disabled {
  color: %FAINT%; background: %PANEL%;
}
QPlainTextEdit, QTextEdit { padding: 8px 10px; font-size: 11px; }

/* The drop-down/spin ARROWS are deliberately left to Fusion. The CSS
   "transparent side borders + one solid border" triangle trick that works in a
   browser does not render in Qt's stylesheet engine — it comes out as a 3 px
   dash — and the alternative (a bitmap per arrow per DPR) is an asset pipeline
   for six glyphs. Fusion draws real arrows and takes their colour from the
   palette this theme already sets, so they match. */
QComboBox::drop-down { border: none; width: 18px; background: transparent; }
QComboBox QAbstractItemView {
  background: %PANEL3%;
  border: 1px solid %LINE%;
  border-radius: 10px;
  padding: 4px;
  outline: none;
  selection-background-color: %EMBERWASH%;
  selection-color: %EMBER%;
}
/* Spin buttons: NOT styled, on purpose. Styling the ::up-button/::down-button
   subcontrol makes Qt stop drawing the default ::up-arrow/::down-arrow with
   it, and the arrow cannot be restyled from QSS either (the CSS
   transparent-border triangle does not render in Qt, and Qt has no arrow
   glyph property) — so the arrows vanish and the spinner becomes
   unclickable-looking. Left to Fusion, which draws real arrows tinted from the
   QPalette this theme sets. Verified: they were missing for one build before
   this comment existed.
   The FIELD itself is still styled, by the shared input rule above. */


/* ===== check boxes ===================================================== */
QCheckBox, QRadioButton { color: %INK%; font-size: 12.5px; spacing: 8px; background: transparent; }
QCheckBox:disabled, QRadioButton:disabled { color: %FAINT%; }
QCheckBox::indicator, QRadioButton::indicator { width: 15px; height: 15px; }
QCheckBox::indicator {
  border: 1px solid %LINEHOVER%; border-radius: 5px; background: %PANEL2%;
}
QCheckBox::indicator:checked { background: %EMBER%; border-color: %EMBER%; }
QCheckBox::indicator:disabled { border-color: %LINESOFT%; background: %PANEL%; }
QRadioButton::indicator { border: 1px solid %LINEHOVER%; border-radius: 8px; background: %PANEL2%; }
QRadioButton::indicator:checked { background: %EMBER%; border-color: %EMBER%; }

/* ===== sliders ========================================================= */
/* 3 px track, ember fill, round knob — the mockup leaves `accent-color` to the
   browser, so the knob geometry is this implementation's own. */
QSlider::groove:horizontal {
  height: 3px; background: %LINE%; border-radius: 2px; margin: 0px;
}
QSlider::sub-page:horizontal { background: %EMBER%; border-radius: 2px; }
QSlider::add-page:horizontal { background: %LINE%; border-radius: 2px; }
QSlider::handle:horizontal {
  background: #FFFFFF;
  border: none;
  width: 13px; height: 13px;
  margin: -5px 0px;
  border-radius: 7px;
}
QSlider::handle:horizontal:hover { background: %INK%; }
QSlider::handle:horizontal:disabled { background: %FAINT%; }
QSlider::sub-page:horizontal:disabled { background: %EMBERDIM%; }

/* ===== progress ======================================================== */
QProgressBar {
  background: %LINESOFT%; border: none; border-radius: 3px; height: 6px;
  text-align: center; color: transparent; font-size: 1px;
}
QProgressBar::chunk { background: %EMBER%; border-radius: 3px; }
QProgressBar[state="good"]::chunk { background: %GOOD%; }
QProgressBar[state="bad"]::chunk { background: %BAD%; }

/* ===== item views ====================================================== */
QListWidget, QTreeWidget, QTableWidget, QListView, QTreeView, QTableView {
  background: %PANEL%;
  border: 1px solid %LINESOFT%;
  border-radius: %RTILE%px;
  outline: none;
  alternate-background-color: %PANEL2%;
  font-size: 12px;
}
QListWidget::item, QTreeWidget::item, QTreeView::item {
  padding: 5px 7px; border-radius: 8px; color: %INK%;
}
QListWidget::item:hover, QTreeWidget::item:hover { background: %PANEL2%; }
QListWidget::item:selected, QTreeWidget::item:selected, QTreeView::item:selected {
  background: %EMBERWASH%; color: %EMBER%;
}
QHeaderView::section {
  background: %PANEL2%;
  color: %FAINT%;
  border: none;
  border-bottom: 1px solid %LINE%;
  padding: 5px 7px;
  font-family: '%MONO%';
  font-size: 9.5px;
  font-weight: 600;
}
QTreeWidget::branch { background: transparent; }

/* ===== tabs ============================================================ */
QTabWidget::pane { border: 1px solid %LINESOFT%; border-radius: %RTILE%px; background: %PANEL%; top: -1px; }
QTabBar { background: transparent; qproperty-drawBase: 0; }
QTabBar::tab {
  background: transparent;
  border: 1px solid transparent;
  border-radius: 12px;
  padding: 6px 14px;
  margin: 2px 3px 6px 0px;
  color: %MUTE%;
  font-size: 12px;
}
QTabBar::tab:hover { background: %PANEL2%; color: %INK%; }
QTabBar::tab:selected {
  background: %EMBERWASH%; border-color: %EMBERRING%; color: %EMBER%; font-weight: 600;
}

/* ===== scrollbars ====================================================== */
QScrollBar:vertical { background: transparent; width: 9px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 9px; margin: 2px; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
  background: %LINE%; border-radius: 3px; min-height: 26px; min-width: 26px;
}
QScrollBar::handle:hover { background: %LINEHOVER%; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0px; width: 0px; border: none; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }

/* ===== status bar ====================================================== */
QStatusBar {
  background: %PANEL2%;
  border-top: 1px solid %LINE%;
  color: %FAINT%;
  font-family: '%MONO%';
  font-size: 10.5px;
  padding: 3px 6px;
}
QStatusBar::item { border: none; }
/* 16 px between segments, matching the mockup's `.deskstatus{gap:16px}`. A
   QStatusBar packs its widgets with no spacing at all, so without this the
   four segments run into one another and read as one sentence. */
QStatusBar QLabel {
  background: transparent; color: %FAINT%; font-family: '%MONO%'; font-size: 10.5px;
  padding: 0px 8px;
}

/* ===== typed labels ==================================================== */
QLabel { background: transparent; }
QLabel[role="section"] {
  font-family: '%MONO%'; font-size: 9.5px; font-weight: 600; color: %FAINT%;
  letter-spacing: 2px;
}
QLabel[role="title"] {
  font-family: '%DISPLAY%'; font-size: 16px; font-weight: 700; color: %INK%;
}
QLabel[role="mono"] { font-family: '%MONO%'; font-size: 11px; color: %MUTE%; }
QLabel[role="stat"] { font-family: '%MONO%'; font-size: 10.5px; color: %INK%; }
QLabel[role="hint"] { font-size: 11.5px; color: %FAINT%; }
QLabel[tone="good"] { color: %GOOD%; }
QLabel[tone="warn"] { color: %WARN%; }
QLabel[tone="bad"]  { color: %BAD%; }
QLabel[tone="ember"] { color: %EMBER%; }
QLabel[tone="pose"] { color: %POSE%; }

/* pill badges (ARMED / REC / SELF-TEST REQUIRED / georef) */
QLabel[badge="true"] {
  font-family: '%MONO%'; font-size: 9.5px; font-weight: 600;
  letter-spacing: 1px;
  border: 1px solid %LINE%; border-radius: 10px;
  padding: 3px 9px; color: %FAINT%; background: %PANEL%;
}
QLabel[badge="true"][tone="warn"] { border-color: %WARNRING%; color: %WARN%; background: %WARNWASH%; }
QLabel[badge="true"][tone="good"] { border-color: %GOODRING%; color: %GOOD%; background: %GOODWASH%; }
QLabel[badge="true"][tone="bad"]  { border-color: %BADRING2%; color: #FF8F8F; background: %BADWASH2%; }
QLabel[badge="true"][tone="ember"] { border-color: %EMBERRING%; color: %EMBER%; background: %EMBERWASH%; }
)");

  s.replace("%GROUND%", css(ground()));
  s.replace("%PANEL2%", css(panel2()));
  s.replace("%PANEL3%", css(panel3()));
  s.replace("%PANEL%", css(panel()));
  s.replace("%LINESOFT%", css(panel2()));
  s.replace("%LINEHOVER%", css(panel3()));
  s.replace("%LINE%", css(line()));
  s.replace("%INK%", css(ink()));
  s.replace("%MUTE%", css(mute()));
  s.replace("%FAINT%", css(faint()));
  s.replace("%EMBERDIM%", css(emberDim()));
  s.replace("%EMBERWASH%", emberWash);
  s.replace("%EMBERRING%", emberRing);
  s.replace("%EMBER%", css(ember()));
  s.replace("%ONEMBER%", kOnEmber);
  s.replace("%GOODRING%", goodRing);
  s.replace("%GOODWASH%", goodWash);
  s.replace("%GOOD%", css(good()));
  s.replace("%WARNRING%", warnRing);
  s.replace("%WARNWASH%", warnWash);
  s.replace("%WARN%", css(warn()));
  s.replace("%BADWASH2%", css(bad(), 0.12));
  s.replace("%BADRING2%", css(bad(), 0.50));
  s.replace("%BADWASH%", badWash);
  s.replace("%BADRING%", badRing);
  s.replace("%BADHOVER%", css(bad(), 0.30));
  s.replace("%BAD%", css(bad()));
  s.replace("%POSE%", css(pose()));
  s.replace("%RCARD%", QString::number(kRadiusCard));
  s.replace("%RTILE%", QString::number(kRadiusTile));
  s.replace("%DISPLAY%", f.display);
  s.replace("%SANS%", f.sans);
  s.replace("%MONO%", f.mono);
  return s;
}

void install(QApplication& app) {
  loadFonts();
  const FontState& f = fonts();

  // Fusion is the only built-in style that honours a QPalette AND a
  // stylesheet consistently on all three platforms. The macOS style paints
  // several controls natively and would ignore half of the sheet — but note
  // this changes only the WIDGETS: the window frame, the traffic lights and
  // the title bar stay native, because Qt never draws those.
  app.setStyle("Fusion");

  QFont base(f.sans, 13);
  base.setHintingPreference(QFont::PreferFullHinting);
  app.setFont(base);

  // The palette matters for everything QSS does not reach: QColorDialog (the
  // background picker), native-ish selection colours, and disabled-state text
  // in views.
  QPalette p;
  p.setColor(QPalette::Window, ground());
  p.setColor(QPalette::WindowText, ink());
  p.setColor(QPalette::Base, panel2());
  p.setColor(QPalette::AlternateBase, panel());
  p.setColor(QPalette::Text, ink());
  p.setColor(QPalette::Button, panel2());
  p.setColor(QPalette::ButtonText, ink());
  p.setColor(QPalette::BrightText, QColor(kOnEmber));
  p.setColor(QPalette::Highlight, ember());
  p.setColor(QPalette::HighlightedText, QColor(kOnEmber));
  p.setColor(QPalette::ToolTipBase, panel3());
  p.setColor(QPalette::ToolTipText, ink());
  p.setColor(QPalette::Link, ember());
  p.setColor(QPalette::LinkVisited, emberDim());
  p.setColor(QPalette::PlaceholderText, faint());
  p.setColor(QPalette::Disabled, QPalette::Text, faint());
  p.setColor(QPalette::Disabled, QPalette::ButtonText, faint());
  p.setColor(QPalette::Disabled, QPalette::WindowText, faint());
  app.setPalette(p);

  app.setStyleSheet(styleSheet());
}

}  // namespace theme
}  // namespace lidarscan
