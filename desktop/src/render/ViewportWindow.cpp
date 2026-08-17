#include "render/ViewportWindow.h"

#include "render/DisplayLink.h"
#include "render/NativeSurface.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QWheelEvent>
#include <QWidget>

#include <filament/Box.h>
#include <filament/Camera.h>
#include <filament/ColorGrading.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/ToneMapper.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>

#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace filament;

namespace lidarscan {
namespace {

constexpr double kFovYDegrees = 60.0;
// Measure-tool click tolerance, in screen pixels, converted to a world-space
// radius at each candidate point's own depth (see ViewportWindow::pickPoint).
constexpr double kPickTolerancePx = 10.0;
constexpr int kMeasureLineSamples = 32;  // points drawn per completed segment

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const double idx = p * double(v.size() - 1);
  const size_t lo = size_t(std::floor(idx));
  const size_t hi = std::min(v.size() - 1, lo + 1);
  const double frac = idx - double(lo);
  return v[lo] * (1.0 - frac) + v[hi] * frac;
}

double mean(const std::vector<double>& v) {
  if (v.empty()) return 0.0;
  double s = 0;
  for (double x : v) s += x;
  return s / double(v.size());
}

void trimSamples(std::vector<double>& v, size_t cap = 600) {
  if (v.size() > cap) v.erase(v.begin(), v.begin() + (v.size() - cap));
}

}  // namespace

ViewportWindow::ViewportWindow(QWindow* parent) : QWindow(parent) {
  setSurfaceType(NativeSurface::backendForThisPlatform() == RenderBackend::kMetal
                     ? QSurface::MetalSurface
                     : QSurface::VulkanSurface);
  clock_.start();
  connect(this, &QWindow::screenChanged, this, [this](QScreen*) {
    // The multi-monitor / DPI-change path S3 could not test (REPORT.md §5).
    // Force a full reconfigure: the backing scale, the drawable size and the
    // display link's target display can all have changed at once.
    resize_pending_ = true;
    if (link_) link_->retargetScreen();
  });
}

ViewportWindow::~ViewportWindow() { destroyFilament(); }

void ViewportWindow::exposeEvent(QExposeEvent*) {
  if (isExposed() && !initialized_ && init_error_.isEmpty()) initFilament();
}

void ViewportWindow::resizeEvent(QResizeEvent*) {
  // Coalesced: the render tick applies at most one swapchain recreate per
  // frame. S3 measured ~8-9 ms of CPU per recreate, so doing it per event
  // during a live drag is exactly the mistake REPORT.md §5 warns about.
  ++stats_.resize_events;
  resize_pending_ = true;
}

void ViewportWindow::setPointStore(const scanengine::PageStore* store) {
  if (store_ == store) return;
  store_ = store;
  if (initialized_) cloud_.reset();
}

void ViewportWindow::setDisplayParams(const scanengine::DisplayParams& p) {
  params_ = p;
  scanengine::clamp_display_params(params_);
  params_dirty_ = true;
}

void ViewportWindow::setVsync(bool on) {
  vsync_ = on;
  resize_pending_ = true;  // reconfigure the layer on the next tick
}

double ViewportWindow::nextLowerRefreshNotch(double from) {
  // The ladder the auto-downshift walks (item 17). Coarse on purpose: a
  // 60 -> 57 fps step would neither help a struggling machine nor be worth an
  // inline note about.
  static const double kNotches[] = {120.0, 90.0, 60.0, 48.0, 30.0, 24.0, 15.0, 10.0, 5.0};
  double best = 0.0;  // the HIGHEST notch strictly below `from`
  for (const double n : kNotches) {
    if (n < from - 0.01 && n > best) best = n;
  }
  return best > 0.0 ? best : from;  // already on the floor
}

void ViewportWindow::setRefreshCeiling(double hz) {
  refresh_ceiling_ = hz > 1.0 ? hz : 0.0;
  if (refresh_ceiling_ > 0.0 && max_fps_ > refresh_ceiling_) setMaxFps(refresh_ceiling_);
}

void ViewportWindow::setMaxFps(double fps) {
  if (fps < 0.0) fps = 0.0;
  // Hardware ceiling (item 17): asking for more than the screen can present is
  // not a cap at all, and pretending otherwise is how a "120 fps" setting on a
  // 60 Hz panel turns into a support question.
  if (refresh_ceiling_ > 0.0 && fps > refresh_ceiling_) fps = refresh_ceiling_;
  max_fps_ = fps;
  overrun_windows_ = 0;
  // Present the very next tick rather than making the new cap wait out the
  // interval of the old one — a slider drag from 5 to 60 fps has to feel
  // immediate, and the only state the throttle keeps is this timestamp.
  last_presented_s_ = 0.0;
}

QString ViewportWindow::surfaceDescription() const {
  return surface_ ? surface_->describe() : QString("<no surface>");
}

QString ViewportWindow::displayLinkName() const {
  return link_ ? link_->name() : QString("<no display link>");
}

bool ViewportWindow::platformVerified() const {
  return surface_ ? surface_->isVerifiedPlatform() : false;
}

void ViewportWindow::initFilament() {
  dpr_ = devicePixelRatio();
  px_w_ = std::max(1, int(width() * dpr_));
  px_h_ = std::max(1, int(height() * dpr_));

  QString err;
  surface_ = NativeSurface::create(this, &err);
  if (!surface_) {
    init_error_ = err.isEmpty() ? QString("no native surface") : err;
    Q_EMIT initFailed(init_error_);
    return;
  }

  fengine_ = Engine::Builder()
                 .backend(surface_->backend() == RenderBackend::kMetal ? Engine::Backend::METAL
                                                                       : Engine::Backend::VULKAN)
                 .build();
  if (!fengine_) {
    init_error_ = QString("Filament Engine::create(%1) failed").arg(to_string(surface_->backend()));
    Q_EMIT initFailed(init_error_);
    return;
  }

  renderer_ = fengine_->createRenderer();
  scene_ = fengine_->createScene();
  view_ = fengine_->createView();

  surface_->configure(SurfaceGeometry{px_w_, px_h_, dpr_, double(width()), double(height())},
                      vsync_);
  swapchain_ = fengine_->createSwapChain(surface_->swapChainHandle(), SwapChain::CONFIG_READABLE);
  if (!swapchain_) {
    init_error_ = "Engine::createSwapChain() returned null";
    Q_EMIT initFailed(init_error_);
    return;
  }
  surface_->configure(SurfaceGeometry{px_w_, px_h_, dpr_, double(width()), double(height())},
                      vsync_);

  camera_entity_ = utils::EntityManager::get().create();
  camera_ = fengine_->createCamera(camera_entity_);
  camera_->setExposure(1.0f);  // unlit points: keep authored colours

  rebuildSkybox();

  view_->setScene(scene_);
  view_->setCamera(camera_);
  view_->setViewport({0, 0, uint32_t(px_w_), uint32_t(px_h_)});
  view_->setPostProcessingEnabled(true);
  view_->setAntiAliasing(View::AntiAliasing::NONE);
  view_->setShadowingEnabled(false);
  view_->setScreenSpaceRefractionEnabled(false);
  view_->setDithering(View::Dithering::NONE);
  {
    View::MultiSampleAntiAliasingOptions msaa{};
    msaa.enabled = false;
    view_->setMultiSampleAntiAliasingOptions(msaa);
    View::TemporalAntiAliasingOptions taa{};
    taa.enabled = false;
    view_->setTemporalAntiAliasingOptions(taa);
    View::BloomOptions bloom{};
    bloom.enabled = false;
    view_->setBloomOptions(bloom);
    View::AmbientOcclusionOptions ao{};
    ao.enabled = false;
    view_->setAmbientOcclusionOptions(ao);
    View::DynamicResolutionOptions dro{};
    dro.enabled = false;
    view_->setDynamicResolutionOptions(dro);
    // Points carry authored RGB; a linear tone mapper keeps them faithful, so
    // what the display-parameter panel says is what reaches the screen.
    static const LinearToneMapper kLinearToneMapper;
    color_grading_ = ColorGrading::Builder().toneMapper(&kLinearToneMapper).build(*fengine_);
    view_->setColorGrading(color_grading_);
  }

  // C8: inside a .app bundle applicationDirPath() is Contents/MacOS, while
  // CMake's MACOSX_BUNDLE resource install puts points.filamat in
  // Contents/Resources — so both are searched, dev-build layout first.
  // (NOTES.md §7 listed this one line as "fine for a dev build, wrong for a
  // bundle, and it is C8's to change".)
  const QString appDir = QCoreApplication::applicationDirPath();
  QString matPath = appDir + "/points.filamat";
  if (!QFile::exists(matPath)) {
    const QString bundled = appDir + "/../Resources/points.filamat";
    if (QFile::exists(bundled)) matPath = bundled;
  }
  QFile f(matPath);
  if (!f.open(QIODevice::ReadOnly)) {
    init_error_ = "cannot open " + matPath;
    Q_EMIT initFailed(init_error_);
    return;
  }
  const QByteArray blob = f.readAll();
  material_ = Material::Builder().package(blob.constData(), size_t(blob.size())).build(*fengine_);
  if (!material_) {
    init_error_ = "points.filamat failed to load";
    Q_EMIT initFailed(init_error_);
    return;
  }
  material_instance_ = material_->createInstance();
  measure_material_ = material_->createInstance();
  // A THIRD instance for the trajectory trail (item 18): same forced-RGB,
  // fixed-size treatment as the measure markers, one notch smaller so the path
  // reads as a line rather than a string of beads.
  trail_material_ = material_->createInstance();

  buildColormapTexture();
  cloud_.init(fengine_, scene_, material_instance_);
  pushMarkerMaterialParams();
  params_dirty_ = true;

  link_ = DisplayLink::create(this, [this] { renderFrame(); });
  link_->start();

  initialized_ = true;
}

void ViewportWindow::buildColormapTexture() {
  // One 256 x 3 RGBA8 texture: row r is Colormap(r)'s 256-entry LUT, straight
  // from the engine (colormap_lut()). The shader samples it instead of
  // re-deriving the ramps, so the two can never drift.
  constexpr uint32_t kW = scanengine::kColormapLutSize;
  constexpr uint32_t kH = scanengine::kColormapCount;
  const size_t bytes = size_t(kW) * kH * 4;
  auto* pixels = static_cast<uint8_t*>(std::malloc(bytes));
  for (uint32_t row = 0; row < kH; ++row) {
    const auto& lut = scanengine::colormap_lut(static_cast<scanengine::Colormap>(row));
    for (uint32_t x = 0; x < kW; ++x) {
      uint8_t* px = pixels + (size_t(row) * kW + x) * 4;
      px[0] = lut[x].r;
      px[1] = lut[x].g;
      px[2] = lut[x].b;
      px[3] = lut[x].a;
    }
  }
  colormap_tex_ = Texture::Builder()
                      .width(kW)
                      .height(kH)
                      .levels(1)
                      .sampler(Texture::Sampler::SAMPLER_2D)
                      .format(Texture::InternalFormat::RGBA8)
                      .build(*fengine_);
  colormap_tex_->setImage(
      *fengine_, 0,
      Texture::PixelBufferDescriptor(pixels, bytes, Texture::Format::RGBA, Texture::Type::UBYTE,
                                     [](void* b, size_t, void*) { std::free(b); }));
}

void ViewportWindow::rebuildSkybox() {
  if (!fengine_) return;
  if (skybox_ && skybox_color_ == params_.background) return;
  Skybox* old = skybox_;
  // Same sRGB -> linear conversion the point material applies (see
  // materials/points.mat): the background is authored as a display colour.
  auto toLinear = [](std::uint8_t v) {
    const float c = float(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
  };
  skybox_ = Skybox::Builder()
                .color({toLinear(params_.background.r), toLinear(params_.background.g),
                        toLinear(params_.background.b), 1.0f})
                .build(*fengine_);
  scene_->setSkybox(skybox_);
  skybox_color_ = params_.background;
  if (old) fengine_->destroy(old);
}

void ViewportWindow::destroyFilament() {
  if (link_) {
    link_->stop();
    link_.reset();
  }
  if (!fengine_) return;
  cloud_.shutdown();
  destroyMeasureGeometry();
  destroyTrailGeometry();
  if (measure_material_) fengine_->destroy(measure_material_);
  if (trail_material_) fengine_->destroy(trail_material_);
  if (colormap_tex_) fengine_->destroy(colormap_tex_);
  if (material_instance_) fengine_->destroy(material_instance_);
  if (material_) fengine_->destroy(material_);
  if (skybox_) fengine_->destroy(skybox_);
  if (color_grading_) fengine_->destroy(color_grading_);
  if (swapchain_) fengine_->destroy(swapchain_);
  if (view_) fengine_->destroy(view_);
  if (scene_) fengine_->destroy(scene_);
  if (renderer_) fengine_->destroy(renderer_);
  if (!camera_entity_.isNull()) {
    fengine_->destroyCameraComponent(camera_entity_);
    utils::EntityManager::get().destroy(camera_entity_);
  }
  Engine::destroy(&fengine_);
  fengine_ = nullptr;
  surface_.reset();
  initialized_ = false;
}

void ViewportWindow::applyResizeIfPending() {
  const double dpr = devicePixelRatio();
  const int wpx = std::max(1, int(width() * dpr));
  const int hpx = std::max(1, int(height() * dpr));
  const bool geometry_changed = (wpx != px_w_ || hpx != px_h_ || std::abs(dpr - dpr_) > 1e-9);
  if (!resize_pending_ && !geometry_changed) return;
  resize_pending_ = false;
  if (!geometry_changed && swapchain_) {
    // Only the vsync flag changed: no swapchain churn needed.
    surface_->configure(SurfaceGeometry{px_w_, px_h_, dpr_, double(width()), double(height())},
                        vsync_);
    return;
  }

  px_w_ = wpx;
  px_h_ = hpx;
  dpr_ = dpr;

  const SurfaceGeometry geom{px_w_, px_h_, dpr_, double(width()), double(height())};
  if (surface_->needsSwapChainRecreateOnResize()) {
    // Metal: CAMetalLayer does not renegotiate a live swapchain's drawable
    // size. Destroy + reconfigure + recreate, which is what filamentapp does
    // and what S3 hammered 1,105 times without a crash or artefact.
    fengine_->flushAndWait();
    fengine_->destroy(swapchain_);
    surface_->configure(geom, vsync_);
    swapchain_ = fengine_->createSwapChain(surface_->swapChainHandle(), SwapChain::CONFIG_READABLE);
    surface_->configure(geom, vsync_);
    ++stats_.swapchain_recreates;
  } else {
    surface_->configure(geom, vsync_);
  }
  view_->setViewport({0, 0, uint32_t(px_w_), uint32_t(px_h_)});
}

void ViewportWindow::updateCamera() {
  if (auto_orbit_) azimuth_ += 0.0045f;
  const float ex = target_[0] + distance_ * std::cos(elevation_) * std::cos(azimuth_);
  const float ey = target_[1] + distance_ * std::cos(elevation_) * std::sin(azimuth_);
  const float ez = target_[2] + distance_ * std::sin(elevation_);
  // Z-up: the engine's local metric frame is right-handed with Z up
  // (cloud/point_page.h), so the viewer must not silently use Filament's
  // Y-up convention or every cloud would appear on its side.
  camera_->lookAt({ex, ey, ez}, {target_[0], target_[1], target_[2]}, {0, 0, 1});
  const double aspect = double(px_w_) / double(std::max(1, px_h_));
  camera_->setProjection(kFovYDegrees, aspect, 0.05, 2000.0, Camera::Fov::VERTICAL);
}

void ViewportWindow::pushMaterialParams() {
  if (!material_instance_) return;
  const auto u = scanengine::to_uniforms(params_);

  material_instance_->setParameter("colorMode", u.color_mode);
  material_instance_->setParameter("colormap", u.colormap);
  material_instance_->setParameter("gamma", u.gamma);
  material_instance_->setParameter("invert", u.invert);
  material_instance_->setParameter("valueMin", u.value_min);
  material_instance_->setParameter("valueMax", u.value_max);
  material_instance_->setParameter("brightness", u.brightness);

  material_instance_->setParameter("pointSizeMode", u.point_size_mode);
  material_instance_->setParameter("pointSizeMinPx", u.point_size_min_px);
  material_instance_->setParameter("pointSizeMaxPx", u.point_size_max_px);
  material_instance_->setParameter("adaptiveReferenceM", u.adaptive_reference_m);
  material_instance_->setParameter("worldSizeM", u.world_size_m);

  material_instance_->setParameter("clipEnabledMask", u.clip_enabled_mask);
  material_instance_->setParameter("clipHeightMin", u.clip_height_min);
  material_instance_->setParameter("clipHeightMax", u.clip_height_max);
  material_instance_->setParameter(
      "clipBoxMin", math::float3{u.clip_box_min[0], u.clip_box_min[1], u.clip_box_min[2]});
  material_instance_->setParameter(
      "clipBoxMax", math::float3{u.clip_box_max[0], u.clip_box_max[1], u.clip_box_max[2]});

  if (colormap_tex_) {
    TextureSampler sampler(TextureSampler::MinFilter::LINEAR, TextureSampler::MagFilter::LINEAR);
    sampler.setWrapModeS(TextureSampler::WrapMode::CLAMP_TO_EDGE);
    sampler.setWrapModeT(TextureSampler::WrapMode::CLAMP_TO_EDGE);
    material_instance_->setParameter("colormapLut", colormap_tex_, sampler);
  }
  rebuildSkybox();
}

// --- measure tool ------------------------------------------------------------

void ViewportWindow::pushMarkerMaterialParams() {
  // Both overlay instances get the same treatment at different sizes; the loop
  // exists so a future overlay cannot be added with a half-configured instance
  // (every parameter in points.mat must be set or Filament asserts).
  struct Overlay {
    filament::MaterialInstance* mi;
    float px;
  };
  const Overlay overlays[] = {{measure_material_, 9.0f}, {trail_material_, 5.0f}};
  for (const Overlay& ov : overlays) {
    if (!ov.mi) continue;
    pushOverlayMaterialParams(ov.mi, ov.px);
  }
}

void ViewportWindow::pushOverlayMaterialParams(filament::MaterialInstance* mi, float marker_px) {
  if (!mi) return;
  // A DisplayParams tuned so the marker/segment points render at a fixed,
  // legible screen size in a fixed colour regardless of whatever the dock's
  // current color mode / clipping / EDL settings are — a user placed these
  // points on purpose, so clipping should not be able to hide them, and the
  // colour must not depend on color_mode (else a marker painted through the
  // "intensity" colormap could come out the same hue as the cloud around it).
  scanengine::DisplayParams mp{};
  mp.color_mode = scanengine::ColorMode::kRgb;
  mp.point_size.mode = scanengine::PointSizeMode::kFixedPixels;
  mp.point_size.fixed_px = marker_px;
  mp.edl_enabled = false;
  mp.clip_height_enabled = false;
  mp.clip_box_enabled = false;
  scanengine::clamp_display_params(mp);
  const auto u = scanengine::to_uniforms(mp);

  mi->setParameter("colorMode", u.color_mode);
  mi->setParameter("colormap", u.colormap);
  mi->setParameter("gamma", u.gamma);
  mi->setParameter("invert", u.invert);
  mi->setParameter("valueMin", u.value_min);
  mi->setParameter("valueMax", u.value_max);
  mi->setParameter("brightness", u.brightness);
  mi->setParameter("pointSizeMode", u.point_size_mode);
  mi->setParameter("pointSizeMinPx", u.point_size_min_px);
  mi->setParameter("pointSizeMaxPx", u.point_size_max_px);
  mi->setParameter("adaptiveReferenceM", u.adaptive_reference_m);
  mi->setParameter("worldSizeM", u.world_size_m);
  mi->setParameter("clipEnabledMask", u.clip_enabled_mask);
  mi->setParameter("clipHeightMin", u.clip_height_min);
  mi->setParameter("clipHeightMax", u.clip_height_max);
  mi->setParameter(
      "clipBoxMin", math::float3{u.clip_box_min[0], u.clip_box_min[1], u.clip_box_min[2]});
  mi->setParameter(
      "clipBoxMax", math::float3{u.clip_box_max[0], u.clip_box_max[1], u.clip_box_max[2]});
  mi->setParameter("cameraPos", math::float3{0.f, 0.f, 0.f});
  mi->setParameter("pxPerMeterAt1m", 1.0f);
  if (colormap_tex_) {
    TextureSampler sampler(TextureSampler::MinFilter::LINEAR, TextureSampler::MagFilter::LINEAR);
    sampler.setWrapModeS(TextureSampler::WrapMode::CLAMP_TO_EDGE);
    sampler.setWrapModeT(TextureSampler::WrapMode::CLAMP_TO_EDGE);
    mi->setParameter("colormapLut", colormap_tex_, sampler);
  }
}

void ViewportWindow::setMeasureMode(bool on) {
  measure_mode_ = on;
  if (!on && has_pending_measure_point_) {
    has_pending_measure_point_ = false;
    rebuildMeasureGeometry();
    Q_EMIT measurementsChanged();
  }
}

void ViewportWindow::removeMeasurement(int index) {
  if (index < 0 || size_t(index) >= measure_segments_.size()) return;
  measure_segments_.erase(measure_segments_.begin() + index);
  rebuildMeasureGeometry();
  Q_EMIT measurementsChanged();
}

void ViewportWindow::clearMeasurements() {
  measure_segments_.clear();
  has_pending_measure_point_ = false;
  rebuildMeasureGeometry();
  Q_EMIT measurementsChanged();
}

void ViewportWindow::destroyMeasureGeometry() {
  if (!fengine_) return;
  if (!measure_entity_.isNull()) {
    if (scene_) scene_->remove(measure_entity_);
    fengine_->destroy(measure_entity_);
    utils::EntityManager::get().destroy(measure_entity_);
    measure_entity_ = {};
  }
  if (measure_vb_) {
    fengine_->destroy(measure_vb_);
    measure_vb_ = nullptr;
  }
  if (measure_ib_) {
    fengine_->destroy(measure_ib_);
    measure_ib_ = nullptr;
  }
}

void ViewportWindow::rebuildMeasureGeometry() {
  if (!fengine_ || !scene_ || !measure_material_) return;
  destroyMeasureGeometry();

  // Same 16-byte interleaved layout the cloud uses (cloud/point_page.h) —
  // reusing PointVertex means no new vertex-attribute declaration and no risk
  // of the marker geometry drifting from what points.mat actually expects.
  std::vector<scanengine::PointVertex> verts;
  const auto push = [&](float x, float y, float z, scanengine::RGBA8 c) {
    scanengine::PointVertex v{};
    v.x = x;
    v.y = y;
    v.z = z;
    v.r = c.r;
    v.g = c.g;
    v.b = c.b;
    v.a = c.a;
    verts.push_back(v);
  };
  constexpr scanengine::RGBA8 kPendingColor{255, 210, 0, 255};   // yellow: in-progress pick
  constexpr scanengine::RGBA8 kSegmentColor{0, 220, 255, 255};   // cyan: a completed segment

  if (has_pending_measure_point_) {
    push(pending_measure_point_[0], pending_measure_point_[1], pending_measure_point_[2],
        kPendingColor);
  }
  for (const auto& seg : measure_segments_) {
    for (int i = 0; i <= kMeasureLineSamples; ++i) {
      const float t = float(i) / float(kMeasureLineSamples);
      push(seg.a[0] + (seg.b[0] - seg.a[0]) * t, seg.a[1] + (seg.b[1] - seg.a[1]) * t,
          seg.a[2] + (seg.b[2] - seg.a[2]) * t, kSegmentColor);
    }
  }
  if (verts.empty()) return;

  const std::uint32_t n = std::uint32_t(verts.size());
  const size_t bytes = size_t(n) * sizeof(scanengine::PointVertex);

  measure_vb_ = VertexBuffer::Builder()
                    .vertexCount(n)
                    .bufferCount(1)
                    .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3,
                              0, sizeof(scanengine::PointVertex))
                    .attribute(VertexAttribute::COLOR, 0, VertexBuffer::AttributeType::UBYTE4,
                              offsetof(scanengine::PointVertex, r),
                              sizeof(scanengine::PointVertex))
                    .normalized(VertexAttribute::COLOR)
                    .build(*fengine_);
  auto* vmem = std::malloc(bytes);
  std::memcpy(vmem, verts.data(), bytes);
  measure_vb_->setBufferAt(
      *fengine_, 0,
      VertexBuffer::BufferDescriptor(vmem, bytes, [](void* b, size_t, void*) { std::free(b); }));

  auto* idx = static_cast<std::uint32_t*>(std::malloc(sizeof(std::uint32_t) * n));
  for (std::uint32_t i = 0; i < n; ++i) idx[i] = i;
  measure_ib_ = IndexBuffer::Builder().indexCount(n).bufferType(IndexBuffer::IndexType::UINT).build(
      *fengine_);
  measure_ib_->setBuffer(*fengine_,
                         IndexBuffer::BufferDescriptor(
                             idx, sizeof(std::uint32_t) * n,
                             [](void* b, size_t, void*) { std::free(b); }));

  measure_entity_ = utils::EntityManager::get().create();
  RenderableManager::Builder(1)
      .boundingBox(Box{{0, 0, 0}, {1000.f, 1000.f, 1000.f}})
      .material(0, measure_material_)
      .geometry(0, RenderableManager::PrimitiveType::POINTS, measure_vb_, measure_ib_, 0, n)
      .culling(false)
      .castShadows(false)
      .receiveShadows(false)
      .build(*fengine_, measure_entity_);
  scene_->addEntity(measure_entity_);
}

// --- item 18: the live trajectory trail ------------------------------------

void ViewportWindow::setTrajectoryTrail(const std::vector<std::array<float, 3>>& path) {
  trail_path_ = path;
  // COALESCED, not queued: the poll runs at 10 Hz and the rebuild happens at
  // most once per presented frame (renderFrame). At a 5 fps cap that means one
  // rebuild for every two polls, which is the point — nothing accumulates.
  trail_dirty_ = true;
}

void ViewportWindow::clearTrajectoryTrail() {
  trail_path_.clear();
  trail_dirty_ = true;
}

void ViewportWindow::destroyTrailGeometry() {
  if (!fengine_) return;
  if (!trail_entity_.isNull()) {
    if (scene_) scene_->remove(trail_entity_);
    fengine_->destroy(trail_entity_);
    utils::EntityManager::get().destroy(trail_entity_);
    trail_entity_ = {};
  }
  if (trail_vb_) {
    fengine_->destroy(trail_vb_);
    trail_vb_ = nullptr;
  }
  if (trail_ib_) {
    fengine_->destroy(trail_ib_);
    trail_ib_ = nullptr;
  }
}

void ViewportWindow::rebuildTrailGeometry() {
  if (!fengine_ || !scene_ || !trail_material_) return;
  destroyTrailGeometry();
  // A14's own switch, so the trail obeys the display parameters like everything
  // else on screen (it is bound in DisplayParamsDock's "Overlays" group).
  if (!params_.show_trajectory || trail_path_.size() < 1) return;

  std::vector<scanengine::PointVertex> verts;
  const auto push = [&](float x, float y, float z, scanengine::RGBA8 c) {
    scanengine::PointVertex v{};
    v.x = x;
    v.y = y;
    v.z = z;
    v.r = c.r;
    v.g = c.g;
    v.b = c.b;
    v.a = c.a;
    verts.push_back(v);
  };
  // Ember for the walked path, bright white for the head (where the operator IS
  // — the one thing you look for while walking).
  constexpr scanengine::RGBA8 kTrail{255, 138, 76, 255};
  constexpr scanengine::RGBA8 kHead{255, 255, 255, 255};
  constexpr float kSampleSpacingM = 0.05f;  // 5 cm: reads as a line, not beads
  constexpr int kMaxSamplesPerLeg = 200;    // a teleporting pose must not blow the buffer
  constexpr std::size_t kMaxVerts = 200000;

  push(trail_path_.front()[0], trail_path_.front()[1], trail_path_.front()[2], kTrail);
  for (std::size_t i = 1; i < trail_path_.size() && verts.size() < kMaxVerts; ++i) {
    const auto& a = trail_path_[i - 1];
    const auto& b = trail_path_[i];
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    const int n = std::max(1, std::min(kMaxSamplesPerLeg, int(len / kSampleSpacingM)));
    for (int k = 1; k <= n; ++k) {
      const float t = float(k) / float(n);
      push(a[0] + dx * t, a[1] + dy * t, a[2] + dz * t, kTrail);
    }
  }
  // The head, drawn last and larger by being drawn as several coincident points
  // is NOT how this works — one point at the head in white is enough at 5 px.
  const auto& head = trail_path_.back();
  push(head[0], head[1], head[2], kHead);

  const std::uint32_t n = std::uint32_t(verts.size());
  const size_t bytes = size_t(n) * sizeof(scanengine::PointVertex);
  trail_vb_ = VertexBuffer::Builder()
                  .vertexCount(n)
                  .bufferCount(1)
                  .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3, 0,
                             sizeof(scanengine::PointVertex))
                  .attribute(VertexAttribute::COLOR, 0, VertexBuffer::AttributeType::UBYTE4,
                             offsetof(scanengine::PointVertex, r),
                             sizeof(scanengine::PointVertex))
                  .normalized(VertexAttribute::COLOR)
                  .build(*fengine_);
  auto* vmem = std::malloc(bytes);
  std::memcpy(vmem, verts.data(), bytes);
  trail_vb_->setBufferAt(
      *fengine_, 0,
      VertexBuffer::BufferDescriptor(vmem, bytes, [](void* b, size_t, void*) { std::free(b); }));

  auto* idx = static_cast<std::uint32_t*>(std::malloc(sizeof(std::uint32_t) * n));
  for (std::uint32_t i = 0; i < n; ++i) idx[i] = i;
  trail_ib_ = IndexBuffer::Builder()
                  .indexCount(n)
                  .bufferType(IndexBuffer::IndexType::UINT)
                  .build(*fengine_);
  trail_ib_->setBuffer(*fengine_, IndexBuffer::BufferDescriptor(
                                      idx, sizeof(std::uint32_t) * n,
                                      [](void* b, size_t, void*) { std::free(b); }));

  trail_entity_ = utils::EntityManager::get().create();
  RenderableManager::Builder(1)
      .boundingBox(Box{{0, 0, 0}, {1000.f, 1000.f, 1000.f}})
      .material(0, trail_material_)
      .geometry(0, RenderableManager::PrimitiveType::POINTS, trail_vb_, trail_ib_, 0, n)
      .culling(false)
      .castShadows(false)
      .receiveShadows(false)
      .build(*fengine_, trail_entity_);
  scene_->addEntity(trail_entity_);
}

bool ViewportWindow::debugPickWorld(const QPointF& widgetPos, float outWorld[3]) const {
  return pickPoint(widgetPos, outWorld);
}

bool ViewportWindow::pickPoint(const QPointF& widgetPos, float outWorld[3]) const {
  if (!store_ || px_w_ <= 0 || px_h_ <= 0) return false;

  const double px = widgetPos.x() * dpr_;
  const double py = widgetPos.y() * dpr_;
  const double ndc_x = (2.0 * px / double(px_w_)) - 1.0;
  const double ndc_y = 1.0 - (2.0 * py / double(px_h_));  // Qt is y-down; NDC is y-up

  const double ex = double(target_[0]) + double(distance_) * std::cos(elevation_) * std::cos(azimuth_);
  const double ey = double(target_[1]) + double(distance_) * std::cos(elevation_) * std::sin(azimuth_);
  const double ez = double(target_[2]) + double(distance_) * std::sin(elevation_);

  double fwd[3] = {double(target_[0]) - ex, double(target_[1]) - ey, double(target_[2]) - ez};
  const double flen = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
  if (flen < 1e-9) return false;
  fwd[0] /= flen;
  fwd[1] /= flen;
  fwd[2] /= flen;

  // Z-up world (see updateCamera()'s comment on the engine's local frame).
  const double up_world[3] = {0.0, 0.0, 1.0};
  double right[3] = {fwd[1] * up_world[2] - fwd[2] * up_world[1],
                     fwd[2] * up_world[0] - fwd[0] * up_world[2],
                     fwd[0] * up_world[1] - fwd[1] * up_world[0]};
  const double rlen = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
  if (rlen < 1e-9) return false;
  right[0] /= rlen;
  right[1] /= rlen;
  right[2] /= rlen;
  const double up[3] = {right[1] * fwd[2] - right[2] * fwd[1], right[2] * fwd[0] - right[0] * fwd[2],
                        right[0] * fwd[1] - right[1] * fwd[0]};

  const double half_h = std::tan(kFovYDegrees * 0.5 * M_PI / 180.0);
  const double aspect = double(px_w_) / double(std::max(1, px_h_));
  const double half_w = half_h * aspect;

  double dir[3];
  for (int k = 0; k < 3; ++k) dir[k] = fwd[k] + right[k] * ndc_x * half_w + up[k] * ndc_y * half_h;
  const double dlen = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
  if (dlen < 1e-9) return false;
  dir[0] /= dlen;
  dir[1] /= dlen;
  dir[2] /= dlen;

  bool found = false;
  double best_t = 0.0;
  float best[3] = {0.f, 0.f, 0.f};
  for (scanengine::PageId id : store_->page_ids()) {
    const scanengine::PageView view = store_->page_view(id);
    if (!view.valid()) continue;
    for (std::uint32_t i = 0; i < view.count; ++i) {
      const auto& p = view.data[i];
      const double to_p[3] = {double(p.x) - ex, double(p.y) - ey, double(p.z) - ez};
      const double t = to_p[0] * dir[0] + to_p[1] * dir[1] + to_p[2] * dir[2];
      if (t <= 0.0) continue;
      const double cx = ex + dir[0] * t, cy = ey + dir[1] * t, cz = ez + dir[2] * t;
      const double dx = double(p.x) - cx, dy = double(p.y) - cy, dz = double(p.z) - cz;
      const double perp = std::sqrt(dx * dx + dy * dy + dz * dz);
      const double world_per_px = t * half_h * 2.0 / double(std::max(1, px_h_));
      if (perp > world_per_px * kPickTolerancePx) continue;
      if (!found || t < best_t) {
        found = true;
        best_t = t;
        best[0] = p.x;
        best[1] = p.y;
        best[2] = p.z;
      }
    }
  }
  if (found) {
    outWorld[0] = best[0];
    outWorld[1] = best[1];
    outWorld[2] = best[2];
  }
  return found;
}

void ViewportWindow::renderFrame() {
  if (!initialized_ || !isExposed() || !swapchain_) {
    last_frame_start_ = 0;
    return;
  }
  const double tick0 = clock_.nsecsElapsed() / 1e9;

  applyResizeIfPending();

  // The live refresh-rate throttle (setMaxFps). Deliberately AFTER the resize
  // apply — a window being dragged must not lag behind by a throttle interval —
  // and BEFORE the PageStore sync, because syncing and presenting is the work
  // being throttled. The 1.5 ms slack keeps a 60 fps cap from beating against a
  // 60 Hz display link and silently halving the rate.
  if (max_fps_ > 0.0 && last_presented_s_ > 0.0 &&
      (tick0 - last_presented_s_) < (1.0 / max_fps_) - 0.0015) {
    return;
  }

  // Mirror the engine's PageStore. Always a full re-read of the page counts —
  // see PagedCloudRenderer.h for why that, and not the event stream, is the
  // correct source of truth.
  if (store_) cloud_.sync(*store_, params_.lod_point_budget);

  // Auto-range (A14: "when true, the caller/renderer refreshes manual_min/max
  // every frame from the actual data range — for height that is
  // PageView::bounds_min[2]/bounds_max[2], already exposed for this purpose").
  // The same rule is applied to intensity, whose range PageView cannot carry
  // and which PagedCloudRenderer therefore measures during upload.
  {
    const auto& cs = cloud_.stats();
    auto retarget = [this](scanengine::ScalarColorParams& sp, float lo, float hi) {
      if (hi - lo < 1e-4f) hi = lo + 1.0f;  // a degenerate range maps everything to 0
      if (std::abs(lo - sp.manual_min) > 1e-5f || std::abs(hi - sp.manual_max) > 1e-5f) {
        sp.manual_min = lo;
        sp.manual_max = hi;
        params_dirty_ = true;
      }
    };
    if (params_.color_mode == scanengine::ColorMode::kHeight && params_.height.auto_range &&
        cs.bounds_valid) {
      retarget(params_.height, cs.bounds_min[2], cs.bounds_max[2]);
    } else if (params_.color_mode == scanengine::ColorMode::kIntensity &&
               params_.intensity.auto_range && cs.intensity_valid) {
      retarget(params_.intensity, cs.intensity_min, cs.intensity_max);
    }

    // Frame the cloud the first time there is one — a viewer that opens on an
    // empty default camera and needs a manual "fit" before showing anything is
    // a bug, not a feature.
    if (!auto_framed_ && cs.bounds_valid && cs.resident_points > 0) {
      auto_framed_ = true;
      fitView();
    }
  }

  if (params_dirty_) {
    params_dirty_ = false;
    pushMaterialParams();
  }

  // The trail's buffers are rebuilt HERE, at most once per presented frame, no
  // matter how many poses arrived since the last one (item 17's "coalesce, do
  // not queue"; item 18's trail).
  if (trail_dirty_) {
    trail_dirty_ = false;
    rebuildTrailGeometry();
  }

  updateCamera();

  // Per-frame material inputs that depend on the camera/viewport, not on the
  // display parameters: the point-size shader needs the eye position and the
  // pixels-per-metre-at-1-m scale of the current projection.
  if (material_instance_) {
    const float ex = target_[0] + distance_ * std::cos(elevation_) * std::cos(azimuth_);
    const float ey = target_[1] + distance_ * std::cos(elevation_) * std::sin(azimuth_);
    const float ez = target_[2] + distance_ * std::sin(elevation_);
    material_instance_->setParameter("cameraPos", math::float3{ex, ey, ez});
    const double half_fov = kFovYDegrees * 0.5 * M_PI / 180.0;
    material_instance_->setParameter(
        "pxPerMeterAt1m", float(double(px_h_) / (2.0 * std::tan(half_fov))));
  }

  // beginFrame() returns false when the backend has no free drawable — the
  // render thread is ahead of the display. Those ticks are backpressure, not
  // frames, and must not be counted or fps is meaningless.
  const double cpu0 = clock_.nsecsElapsed() / 1e9;
  const bool drew = renderer_->beginFrame(swapchain_);
  if (drew) {
    renderer_->render(view_);
    renderer_->endFrame();
  }
  const double cpu1 = clock_.nsecsElapsed() / 1e9;
  if (!drew) return;

  last_presented_s_ = cpu1;
  cpu_ms_.push_back((cpu1 - tick0) * 1000.0);
  if (last_frame_start_ > 0) interval_ms_.push_back((cpu0 - last_frame_start_) * 1000.0);
  last_frame_start_ = cpu0;
  trimSamples(cpu_ms_);
  trimSamples(interval_ms_);

  for (const auto& fi : renderer_->getFrameInfoHistory(8)) {
    if (fi.frameId > last_gpu_frame_id_ && fi.gpuFrameDuration > 0) {
      last_gpu_frame_id_ = fi.frameId;
      gpu_ms_.push_back(double(fi.gpuFrameDuration) / 1e6);
    }
  }
  trimSamples(gpu_ms_);

  const double now = cpu1;
  if (now - last_status_ > 0.4) {
    last_status_ = now;
    stats_.fps = interval_ms_.empty() ? 0.0 : 1000.0 / mean(interval_ms_);
    stats_.cpu_ms_p95 = percentile(cpu_ms_, 0.95);
    stats_.gpu_ms_p95 = percentile(gpu_ms_, 0.95);
    stats_.px_w = px_w_;
    stats_.px_h = px_h_;
    stats_.dpr = dpr_;
    stats_.cloud = cloud_.stats();
    Q_EMIT statusChanged(
        QString("%1 pts (%2 drawn) · %3 pages · %4 fps · cpu p95 %5 ms · gpu p95 %6 ms · "
                "%7x%8 px dpr %9 · vsync %10 · swapchains %11")
            .arg(stats_.cloud.resident_points)
            .arg(stats_.cloud.drawn_points)
            .arg(stats_.cloud.pages)
            .arg(stats_.fps, 0, 'f', 1)
            .arg(stats_.cpu_ms_p95, 0, 'f', 2)
            .arg(stats_.gpu_ms_p95, 0, 'f', 2)
            .arg(px_w_)
            .arg(px_h_)
            .arg(dpr_, 0, 'f', 2)
            .arg(vsync_ ? "on" : "off")
            .arg(stats_.swapchain_recreates));

    // --- item 17: measured auto-downshift --------------------------------
    //
    // A cap this machine cannot sustain is worse than a lower one it can: the
    // renderer falls further behind every frame while the capture — which is
    // NEVER throttled and never touched from here — keeps producing points. So
    // when a presented frame's p95 CPU time has been eating most of the cap's
    // budget across a sustained window (5 status windows ≈ 2 s, not one spike),
    // step the cap one notch down and say so quietly. Only ever downward, and
    // never below the 5 fps floor the notch ladder ends at.
    if (max_fps_ > 0.0 && cpu_ms_.size() >= 8) {
      const double budget_ms = 1000.0 / max_fps_;
      // TWO signals, because one of them alone lies. p95 CPU catches a frame
      // that is expensive to draw. But the throttle SKIPS ticks, and the cost of
      // a skipped tick (a swapchain recreate during a window drag, say) is never
      // sampled — so a machine can miss its cap badly while every frame it did
      // present looks cheap. Measured on the resize-storm stress: 18.9 fps
      // against a cap, with cpu p95 0.24 ms. Delivered rate versus the cap is
      // the signal that catches that, and 0.75 is loose enough not to fire on
      // ordinary display-link jitter.
      const bool cpu_over = stats_.cpu_ms_p95 > budget_ms * 0.9;
      const bool rate_under = stats_.fps > 0.0 && stats_.fps < max_fps_ * 0.75;
      if (cpu_over || rate_under) {
        ++overrun_windows_;
      } else {
        overrun_windows_ = 0;
      }
      if (overrun_windows_ >= 5) {
        const double lower = nextLowerRefreshNotch(max_fps_);
        const double was = max_fps_;
        overrun_windows_ = 0;
        if (lower < was) {
          setMaxFps(lower);  // also resets the overrun counter
          Q_EMIT refreshDownshifted(
              lower, QString("delivered %1 fps against a %2 fps cap, frame cpu p95 %3 ms of a "
                             "%4 ms budget")
                         .arg(stats_.fps, 0, 'f', 1)
                         .arg(was, 0, 'f', 0)
                         .arg(stats_.cpu_ms_p95, 0, 'f', 2)
                         .arg(budget_ms, 0, 'f', 1));
        }
      }
    }
  }
}

void ViewportWindow::fitView() {
  const auto& c = cloud_.stats();
  if (!c.bounds_valid) {
    target_[0] = target_[1] = target_[2] = 0.f;
    distance_ = 10.f;
    return;
  }
  float radius = 0.f;
  for (int k = 0; k < 3; ++k) {
    target_[k] = 0.5f * (c.bounds_min[k] + c.bounds_max[k]);
    radius = std::max(radius, 0.5f * (c.bounds_max[k] - c.bounds_min[k]));
  }
  radius = std::max(radius, 0.5f);
  // Fit the bounding sphere in the vertical FOV, with 30% headroom.
  distance_ = float(radius / std::tan(kFovYDegrees * 0.5 * M_PI / 180.0) * 1.3);
  elevation_ = 0.6f;
}

bool ViewportWindow::captureScreenshot(const QString& path) {
  if (!initialized_ || !renderer_ || !swapchain_) return false;
  const int w = px_w_, h = px_h_;
  const size_t bytes = size_t(w) * h * 4;
  void* mem = std::malloc(bytes);
  if (!mem) return false;

  struct Ctx {
    int w, h;
    QString path;
    ViewportWindow* self;
  };
  auto* ctx = new Ctx{w, h, path, this};

  // beginFrame() can legitimately fail (no free drawable); retry, or the
  // screenshot silently never happens.
  bool drew = false;
  for (int tries = 0; tries < 500 && !drew; ++tries) {
    drew = renderer_->beginFrame(swapchain_);
    if (!drew) fengine_->flushAndWait();
  }
  if (!drew) {
    std::free(mem);
    delete ctx;
    return false;
  }
  renderer_->render(view_);
  renderer_->readPixels(
      0, 0, uint32_t(w), uint32_t(h),
      backend::PixelBufferDescriptor(
          mem, bytes, backend::PixelDataFormat::RGBA, backend::PixelDataType::UBYTE,
          [](void* buf, size_t, void* user) {
            auto* c = static_cast<Ctx*>(user);
            QImage img(c->w, c->h, QImage::Format_RGBA8888);
            const auto* src = static_cast<const uchar*>(buf);
            // readPixels() returns bottom-up; flip into Qt's top-down image.
            for (int y = 0; y < c->h; ++y) {
              std::memcpy(img.scanLine(c->h - 1 - y), src + size_t(y) * c->w * 4,
                          size_t(c->w) * 4);
            }
            img.save(c->path);
            if (c->self) c->self->last_shot_ = img;
            std::free(buf);
            delete c;
          },
          ctx));
  renderer_->endFrame();
  fengine_->flushAndWait();

  if (top_level_) {
    const QPixmap chrome = top_level_->grab();
    QString chromePath = path;
    chrome.save(chromePath.replace(".png", "-qtchrome.png"));
    if (!last_shot_.isNull()) {
      QPixmap composed = chrome;
      QPainter pr(&composed);
      const QPoint at = top_level_->mapFromGlobal(mapToGlobal(QPoint(0, 0)));
      pr.drawImage(QRectF(at.x(), at.y(), width(), height()), last_shot_);
      pr.end();
      QString windowPath = path;
      composed.save(windowPath.replace(".png", "-window.png"));
    }
  }
  return true;
}

// --- interaction -------------------------------------------------------------

void ViewportWindow::mousePressEvent(QMouseEvent* e) {
  last_mouse_ = e->position();

  if (measure_mode_ && e->button() == Qt::LeftButton) {
    float world[3];
    if (pickPoint(e->position(), world)) {
      if (!has_pending_measure_point_) {
        has_pending_measure_point_ = true;
        pending_measure_point_[0] = world[0];
        pending_measure_point_[1] = world[1];
        pending_measure_point_[2] = world[2];
      } else {
        MeasureSegment seg;
        seg.a[0] = pending_measure_point_[0];
        seg.a[1] = pending_measure_point_[1];
        seg.a[2] = pending_measure_point_[2];
        seg.b[0] = world[0];
        seg.b[1] = world[1];
        seg.b[2] = world[2];
        const double dx = double(seg.b[0]) - double(seg.a[0]);
        const double dy = double(seg.b[1]) - double(seg.a[1]);
        const double dz = double(seg.b[2]) - double(seg.a[2]);
        seg.distance_m = std::sqrt(dx * dx + dy * dy + dz * dz);
        measure_segments_.push_back(seg);
        has_pending_measure_point_ = false;
      }
      rebuildMeasureGeometry();
      Q_EMIT measurementsChanged();
    }
    dragging_ = false;
    panning_ = false;
    return;
  }

  dragging_ = (e->button() == Qt::LeftButton);
  panning_ = (e->button() == Qt::MiddleButton) ||
             (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ShiftModifier));
  if (dragging_) auto_orbit_ = false;
}

void ViewportWindow::mouseReleaseEvent(QMouseEvent*) {
  dragging_ = false;
  panning_ = false;
}

void ViewportWindow::mouseMoveEvent(QMouseEvent* e) {
  const QPointF d = e->position() - last_mouse_;
  last_mouse_ = e->position();
  if (panning_) {
    // Pan in the camera's screen plane, scaled by distance so the drag feels
    // the same at any zoom.
    const float s = float(distance_) * 0.0016f;
    const float ca = std::cos(azimuth_), sa = std::sin(azimuth_);
    target_[0] += float(d.x()) * s * sa - float(d.y()) * s * ca * std::sin(elevation_);
    target_[1] += -float(d.x()) * s * ca - float(d.y()) * s * sa * std::sin(elevation_);
    target_[2] += float(d.y()) * s * std::cos(elevation_);
  } else if (dragging_) {
    azimuth_ -= float(d.x()) * 0.008f;
    elevation_ = std::clamp(elevation_ + float(d.y()) * 0.008f, -1.5f, 1.5f);
  }
}

void ViewportWindow::wheelEvent(QWheelEvent* e) {
  distance_ = std::clamp(distance_ * (1.0f - float(e->angleDelta().y()) * 0.0012f), 0.05f, 2000.0f);
}

void ViewportWindow::keyPressEvent(QKeyEvent* e) {
  switch (e->key()) {
    case Qt::Key_Space: auto_orbit_ = !auto_orbit_; break;
    case Qt::Key_F: fitView(); break;
    case Qt::Key_Escape:
      // Clears the in-progress pick only; the completed segment list is
      // MeasureDock's delete button's job (per-item, or "clear all").
      if (has_pending_measure_point_) {
        has_pending_measure_point_ = false;
        rebuildMeasureGeometry();
        Q_EMIT measurementsChanged();
      }
      break;
    default: QWindow::keyPressEvent(e); break;
  }
}

}  // namespace lidarscan
