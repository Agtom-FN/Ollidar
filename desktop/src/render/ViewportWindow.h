// ViewportWindow.h — the 3D viewport: a native QWindow with a Filament
// swapchain on it, embedded in the main window via QWidget::createWindowContainer().
//
// This is the productionized S3 FilamentWindow. What changed:
//   * the benchmark state machine, the synthetic point producer and the three
//     experimental native-handle modes are gone;
//   * the spinning QTimer(0) render loop is replaced by a DisplayLink
//     (REPORT.md §7);
//   * resizes are coalesced to at most one swapchain recreate per frame and
//     counted, because a recreate costs ~8-9 ms of CPU (REPORT.md §5);
//   * points come from the engine's PageStore through PagedCloudRenderer, not
//     from a generator;
//   * display parameters (A14) are pushed into the material instance;
//   * screenChanged is handled — the multi-monitor/DPI case S3 could not test
//     on a single-display machine (REPORT.md §5, "main residual macOS risk").
//
// THREADING: everything here runs on the GUI thread. Filament's Engine is
// created, used and destroyed on that one thread for the process lifetime.
//
// Owner: C1.
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QString>
#include <QWindow>

#include <array>
#include <memory>
#include <vector>

#include <utils/Entity.h>

#include "scanengine/cloud/display_params.h"
#include "render/PagedCloudRenderer.h"

namespace filament {
class Camera;
class ColorGrading;
class Engine;
class Material;
class MaterialInstance;
class Renderer;
class Scene;
class Skybox;
class SwapChain;
class Texture;
class View;
}  // namespace filament

namespace scanengine {
class PageStore;
}

class QWidget;

namespace lidarscan {

class DisplayLink;
class NativeSurface;

// One completed point-to-point measurement (C3 §3.13 "review workspace ...
// measure"). Positions are in the session's local metric frame — the same
// frame PointVertex/PageView use.
struct MeasureSegment {
  float a[3] = {0.f, 0.f, 0.f};
  float b[3] = {0.f, 0.f, 0.f};
  double distance_m = 0.0;
};

struct ViewportStats {
  double fps = 0.0;
  double cpu_ms_p95 = 0.0;
  double gpu_ms_p95 = 0.0;
  std::uint32_t swapchain_recreates = 0;
  std::uint32_t resize_events = 0;
  int px_w = 0, px_h = 0;
  double dpr = 1.0;
  CloudStats cloud;
};

class ViewportWindow : public QWindow {
  Q_OBJECT
 public:
  explicit ViewportWindow(QWindow* parent = nullptr);
  ~ViewportWindow() override;

  // The QMainWindow this viewport is embedded in — only used to compose the
  // Qt chrome into a screenshot (see captureScreenshot).
  void setTopLevel(QWidget* w) { top_level_ = w; }

  // The store the viewport mirrors. Null unpins it (and clears the GPU copy).
  void setPointStore(const scanengine::PageStore* store);

  void setDisplayParams(const scanengine::DisplayParams& p);
  const scanengine::DisplayParams& displayParams() const { return params_; }

  void setVsync(bool on);
  bool vsync() const { return vsync_; }

  // --- live refresh-rate throttle (REVIEW_FEEDBACK round 5 item 10) --------
  //
  // "before recording, stream live with all display parameters adjustable —
  // live refresh rate, point size, gamma, brightness". Everything on that list
  // except the refresh rate is an A14 DisplayParams field; the refresh rate is
  // not a display parameter at all, it is how often this window presents, so it
  // lives here. The DisplayLink still ticks at the display's own cadence (that
  // is the OS's clock, not ours to slow down); a tick that arrives sooner than
  // one throttle interval after the last PRESENTED frame returns without
  // syncing the cloud or touching the swapchain, which is where the CPU/GPU
  // cost of a frame actually is. 0 = uncapped, the C1 behaviour and still the
  // default — nothing outside the capture panel sets this.
  void setMaxFps(double fps);
  double maxFps() const { return max_fps_; }

  // --- item 17: the cap is HARDWARE-DERIVED and must never take the app down --
  //
  // Two halves. The CEILING is what the screen can present
  // (QScreen::refreshRate, set by MainWindow — this class only clamps to it), and
  // the FLOOR under load is measured: if a presented frame's p95 CPU time keeps
  // overrunning the budget the current cap implies, the cap is downshifted one
  // notch and refreshDownshifted() says so, quietly, in the capture panel. It
  // never shifts back up on its own — a display that oscillates between rates is
  // worse than one that settled low, and the operator can always drag the slider.
  //
  // WHAT THIS PROTECTS AGAINST is the classic flood: the render loop asking the
  // renderer to upload another PageStore delta every display tick while the GPU
  // is still behind. Nothing here queues — the throttle RETURNS from the tick
  // before PagedCloudRenderer::sync(), so a skipped frame skips its uploads
  // entirely rather than deferring them. Recording is never involved: it runs on
  // the engine's own threads and is never throttled by anything in this file.
  void setRefreshCeiling(double hz);
  double refreshCeiling() const { return refresh_ceiling_; }

  // --- item 18: the live trajectory trail ---------------------------------
  //
  // The walking path, in the session's local metric frame — the same frame the
  // cloud is in. Drawn as a point-sampled polyline through the SAME material the
  // measure tool uses (forced RGB, fixed marker size), so it cannot be recoloured
  // or shrunk away by the display parameters. Gated on
  // DisplayParams::show_trajectory. Rebuilt at most once per PRESENTED frame
  // (dirty flag, not a rebuild per call), so a 10 Hz pose poll costs at most one
  // buffer rebuild per frame and never one per pose.
  void setTrajectoryTrail(const std::vector<std::array<float, 3>>& path);
  void clearTrajectoryTrail();

  // Frame the whole cloud (or a unit box if there is none yet).
  void fitView();
  void setAutoOrbit(bool on) { auto_orbit_ = on; }
  bool autoOrbit() const { return auto_orbit_; }

  bool initialized() const { return initialized_; }
  const QString& initError() const { return init_error_; }
  QString surfaceDescription() const;
  QString displayLinkName() const;
  bool platformVerified() const;

  const ViewportStats& stats() const { return stats_; }

  // Renderer::readPixels() off the swapchain — the literal pixels Filament put
  // in the Qt window. Writes `path`, plus `<path>-qtchrome.png` (QWidget::grab
  // of the main window) and `<path>-window.png` (the two composited, i.e. what
  // the user sees). This is S3's evidence mechanism, kept because it has
  // strictly better provenance than an OS screen capture — and because the
  // shell running CI may have no Screen Recording permission (REPORT.md §6).
  bool captureScreenshot(const QString& path);

  // --- measure tool (C3) ---------------------------------------------------
  //
  // Click-pick against the resident PageStore pages: a straight O(N) nearest-
  // point-to-ray scan over every uploaded point (see pickPoint() in the .cpp
  // for the exact tolerance rule). No spatial index — fine at review-workspace
  // scale (the synthetic captures this was verified against top out around
  // 120k points; NOTES.md flags this as the thing to revisit for a
  // multi-million-point cloud).
  //
  // First click sets the pending point (rendered as a small yellow marker);
  // second click completes a segment (rendered as a cyan point-sampled line)
  // and adds it to measurements(). ESC (keyPressEvent) clears a pending point
  // without completing a segment. Toggling measure mode off also clears any
  // pending point, but never touches the completed segment list — that is
  // MeasureDock's "delete" button's job.
  void setMeasureMode(bool on);
  bool measureMode() const { return measure_mode_; }
  bool hasPendingMeasurePoint() const { return has_pending_measure_point_; }
  const std::vector<MeasureSegment>& measurements() const { return measure_segments_; }
  void removeMeasurement(int index);
  void clearMeasurements();

  // A read-only query wrapper around pickPoint() for evidence/CLI tooling
  // (C6's --merge-manual3, mirroring --measure-selftest's own "walk a grid
  // through the real pick path" posture) that needs to find where a KNOWN
  // world point projects on screen without performing a click. Does not
  // touch measure_mode_/pending-point state — a real click still has to go
  // through the normal mousePressEvent path.
  bool debugPickWorld(const QPointF& widgetPos, float outWorld[3]) const;

 Q_SIGNALS:
  void statusChanged(const QString& text);
  void initFailed(const QString& reason);
  void measurementsChanged();
  // The live refresh cap was lowered because this machine could not sustain the
  // previous one (item 17). `why` carries the measured numbers.
  void refreshDownshifted(double newHz, const QString& why);

 protected:
  void exposeEvent(QExposeEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;

 private:
  void initFilament();
  void destroyFilament();
  void renderFrame();
  void applyResizeIfPending();
  void updateCamera();
  void pushMaterialParams();
  void rebuildSkybox();
  void buildColormapTexture();

  // measure tool
  bool pickPoint(const QPointF& widgetPos, float outWorld[3]) const;
  void rebuildMeasureGeometry();
  void destroyMeasureGeometry();
  void pushMarkerMaterialParams();
  // One overlay instance (measure markers, trajectory trail) configured to the
  // forced-RGB / fixed-size treatment at `marker_px`.
  void pushOverlayMaterialParams(filament::MaterialInstance* mi, float marker_px);

  // trajectory trail (item 18)
  void rebuildTrailGeometry();
  void destroyTrailGeometry();
  // One notch down the hardware-derived ladder (item 17). Returns `from` when
  // there is nowhere lower to go.
  static double nextLowerRefreshNotch(double from);

  QWidget* top_level_ = nullptr;

  std::unique_ptr<NativeSurface> surface_;
  std::unique_ptr<DisplayLink> link_;

  filament::Engine* fengine_ = nullptr;
  filament::Renderer* renderer_ = nullptr;
  filament::Scene* scene_ = nullptr;
  filament::View* view_ = nullptr;
  filament::Camera* camera_ = nullptr;
  filament::Skybox* skybox_ = nullptr;
  filament::ColorGrading* color_grading_ = nullptr;
  filament::SwapChain* swapchain_ = nullptr;
  filament::Material* material_ = nullptr;
  filament::MaterialInstance* material_instance_ = nullptr;
  filament::Texture* colormap_tex_ = nullptr;
  utils::Entity camera_entity_;

  // measure tool: a second, tiny renderable sharing `material_` (a second
  // MaterialInstance so it can force colorMode=kRgb / a fixed marker point
  // size regardless of the dock's current display parameters) and its own
  // small vertex/index buffers, rebuilt from scratch on every change — a
  // handful of clicks, never a per-frame cost.
  filament::MaterialInstance* measure_material_ = nullptr;
  filament::VertexBuffer* measure_vb_ = nullptr;
  filament::IndexBuffer* measure_ib_ = nullptr;
  utils::Entity measure_entity_;
  bool measure_mode_ = false;
  bool has_pending_measure_point_ = false;
  float pending_measure_point_[3] = {0.f, 0.f, 0.f};
  std::vector<MeasureSegment> measure_segments_;

  PagedCloudRenderer cloud_;
  const scanengine::PageStore* store_ = nullptr;

  scanengine::DisplayParams params_{};
  scanengine::RGBA8 skybox_color_{};
  bool params_dirty_ = true;

  bool initialized_ = false;
  QString init_error_;
  bool vsync_ = true;
  // Live refresh-rate throttle; see setMaxFps(). `last_presented_s_` is on the
  // same clock_ every other timing figure in this class uses.
  double max_fps_ = 0.0;
  double last_presented_s_ = 0.0;
  double refresh_ceiling_ = 0.0;      // 0 = unknown/unclamped (QScreen not asked yet)
  int overrun_windows_ = 0;           // consecutive 0.4 s stats windows over budget

  // trajectory trail
  filament::MaterialInstance* trail_material_ = nullptr;
  filament::VertexBuffer* trail_vb_ = nullptr;
  filament::IndexBuffer* trail_ib_ = nullptr;
  utils::Entity trail_entity_;
  std::vector<std::array<float, 3>> trail_path_;
  bool trail_dirty_ = false;

  // resize coalescing
  bool resize_pending_ = true;
  int px_w_ = 1, px_h_ = 1;
  double dpr_ = 1.0;

  // camera
  float azimuth_ = 2.2f, elevation_ = 0.45f, distance_ = 12.0f;
  float target_[3] = {0.f, 0.f, 0.f};
  bool auto_orbit_ = false;
  bool auto_framed_ = false;
  bool dragging_ = false;
  bool panning_ = false;
  QPointF last_mouse_;

  // stats
  QElapsedTimer clock_;
  double last_frame_start_ = 0;
  double last_status_ = 0;
  std::vector<double> cpu_ms_, interval_ms_, gpu_ms_;
  std::uint32_t last_gpu_frame_id_ = 0;
  ViewportStats stats_{};
  QImage last_shot_;
};

}  // namespace lidarscan
