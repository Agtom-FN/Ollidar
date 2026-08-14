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

 Q_SIGNALS:
  void statusChanged(const QString& text);
  void initFailed(const QString& reason);
  void measurementsChanged();

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
