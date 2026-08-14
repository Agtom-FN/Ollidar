#include "FilamentWindow.h"
#include "MacBridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

#include <filament/Camera.h>
#include <filament/ColorGrading.h>
#include <filament/ToneMapper.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/View.h>
#include <filament/Viewport.h>

#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace filament;

namespace {

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

} // namespace

FilamentWindow::FilamentWindow(const SpikeOptions& opts, QWindow* parent)
    : QWindow(parent), mOpts(opts) {
    setSurfaceType(QSurface::MetalSurface);
    mStage.resize(1u << 18); // 256k point staging buffer for drains
    mClock.start();
}

FilamentWindow::~FilamentWindow() {
    mSource.stop();
    destroyFilament();
}

void FilamentWindow::note(const std::string& s) {
    mLog.push_back(s);
    std::fprintf(stderr, "[s3] %s\n", s.c_str());
    std::fflush(stderr);
}

void FilamentWindow::exposeEvent(QExposeEvent*) {
    if (isExposed() && !mInitialized) {
        initFilament();
    }
}

void FilamentWindow::resizeEvent(QResizeEvent*) {
    // handled lazily in the render loop (swapchain recreate)
}

void FilamentWindow::initFilament() {
    mDpr = devicePixelRatio();
    mPxW = std::max(1, int(width() * mDpr));
    mPxH = std::max(1, int(height() * mDpr));

    mNativeView = macbridge::viewFromWinId(uint64_t(winId()));
    macbridge::prepareView(mNativeView);

    note(std::string("QWindow::winId() -> NSView, view.layer is ")
         + macbridge::viewLayerClassName(mNativeView));

    switch (mOpts.handle) {
        case NativeHandle::QtMetalLayer:
            // Qt creates a QMetalLayer (a CAMetalLayer subclass) for a
            // QSurface::MetalSurface QWindow. Hand that straight to Filament.
            mMetalLayer = macbridge::findMetalLayer(mNativeView);
            mNativeWindowArg = mMetalLayer;
            note(std::string("native handle: Qt's own ")
                 + macbridge::layerClassName(mMetalLayer) + " (CAMetalLayer subclass)"
                 + (macbridge::viewOwnsLayerDirectly(mNativeView, mMetalLayer) ? " [view.layer]"
                                                                              : " [sublayer]"));
            break;
        case NativeHandle::OwnMetalLayer:
            mMetalLayer = macbridge::attachOwnMetalLayer(mNativeView, mDpr, mPxW, mPxH);
            mNativeWindowArg = mMetalLayer;
            note("native handle: app-created CAMetalLayer added as a sublayer of Qt's NSView");
            break;
        case NativeHandle::NSView:
            mNativeWindowArg = mNativeView;
            note("native handle: raw NSView* (as documented in SwapChain.h)");
            break;
    }
    if (!mNativeWindowArg) {
        note("FATAL: no usable native handle");
        return;
    }

    mEngine = Engine::Builder().backend(Engine::Backend::METAL).build();
    if (!mEngine) {
        note("FATAL: Engine::create(METAL) failed");
        return;
    }
    note(std::string("Metal device: ") + macbridge::metalDeviceName());

    mRenderer = mEngine->createRenderer();
    mScene = mEngine->createScene();
    mView = mEngine->createView();

    uint64_t flags = 0;
    if (!mOpts.shotDir.empty()) flags |= SwapChain::CONFIG_READABLE;
    mSwapChain = mEngine->createSwapChain(mNativeWindowArg, flags);
    if (!mSwapChain) {
        note("FATAL: createSwapChain() returned null");
        return;
    }
    note("createSwapChain() OK");

    if (!mMetalLayer) {
        mMetalLayer = macbridge::findMetalLayer(mNativeView);
    }
    macbridge::configureLayer(mMetalLayer, mDpr, mPxW, mPxH, width(), height(), mOpts.vsync);

    mCameraEntity = utils::EntityManager::get().create();
    mCamera = mEngine->createCamera(mCameraEntity);
    mCamera->setExposure(1.0f); // unlit points: keep authored colours

    mSkybox = Skybox::Builder().color({0.035f, 0.04f, 0.05f, 1.0f}).build(*mEngine);
    mScene->setSkybox(mSkybox);

    mView->setScene(mScene);
    mView->setCamera(mCamera);
    mView->setViewport({0, 0, uint32_t(mPxW), uint32_t(mPxH)});
    mView->setPostProcessingEnabled(mOpts.postProcessing);
    mView->setAntiAliasing(View::AntiAliasing::NONE);
    mView->setShadowingEnabled(false);
    mView->setScreenSpaceRefractionEnabled(false);
    mView->setDithering(View::Dithering::NONE);
    {
        View::MultiSampleAntiAliasingOptions msaa{};
        msaa.enabled = false;
        mView->setMultiSampleAntiAliasingOptions(msaa);
    }
    {
        View::TemporalAntiAliasingOptions taa{};
        taa.enabled = false;
        mView->setTemporalAntiAliasingOptions(taa);
        View::BloomOptions bloom{};
        bloom.enabled = false;
        mView->setBloomOptions(bloom);
        View::AmbientOcclusionOptions ao{};
        ao.enabled = false;
        mView->setAmbientOcclusionOptions(ao);
        View::DynamicResolutionOptions dro{};
        dro.enabled = false;
        mView->setDynamicResolutionOptions(dro);
        // Points carry authored RGB; a linear tone mapper keeps them faithful.
        static const LinearToneMapper kLinearToneMapper;
        mColorGrading = ColorGrading::Builder()
                                .toneMapper(&kLinearToneMapper)
                                .build(*mEngine);
        mView->setColorGrading(mColorGrading);
    }
    // Turn off Filament's frame pacing: we want every tick to produce a frame so
    // the measured rate reflects the renderer, not the pacer.
    mRenderer->setDisplayInfo({.refreshRate = 0.0f});

    // Load the point material compiled by matc at build time.
    const QString matPath = QCoreApplication::applicationDirPath() + "/points.filamat";
    QFile f(matPath);
    if (!f.open(QIODevice::ReadOnly)) {
        note("FATAL: cannot open " + matPath.toStdString());
        return;
    }
    const QByteArray blob = f.readAll();
    mMaterial = Material::Builder().package(blob.constData(), size_t(blob.size())).build(*mEngine);
    mMatInstance = mMaterial->createInstance();
    mMatInstance->setParameter("pointSize", mOpts.pointSize);
    note("points.filamat loaded (" + std::to_string(blob.size()) + " bytes)");

    // headroom for the "10M + live ingest" phase
    mCloud.init(mEngine, mScene, mMatInstance,
                uint32_t((mOpts.maxPoints + PagedCloud::kPageCapacity - 1) / PagedCloud::kPageCapacity) + 3);

    mInitialized = true;
    note("init complete: " + std::to_string(mPxW) + "x" + std::to_string(mPxH) + " px, dpr "
         + std::to_string(mDpr) + ", vsync " + (mOpts.vsync ? "on" : "off"));

    if (!mOpts.bench && mOpts.preloadPoints > 0) {
        preload(mOpts.preloadPoints);
    }
    if (mOpts.bench) {
        mBenchStage = 0;
        mStageEntered = mClock.nsecsElapsed() / 1e9;
    }

    auto* timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    connect(timer, &QTimer::timeout, this, [this] { renderFrame(); });
    timer->start(0);
}

void FilamentWindow::destroyFilament() {
    if (!mEngine) return;
    mCloud.shutdown();
    if (mMatInstance) mEngine->destroy(mMatInstance);
    if (mMaterial) mEngine->destroy(mMaterial);
    if (mSkybox) mEngine->destroy(mSkybox);
    if (mColorGrading) mEngine->destroy(mColorGrading);
    if (mSwapChain) mEngine->destroy(mSwapChain);
    if (mView) mEngine->destroy(mView);
    if (mScene) mEngine->destroy(mScene);
    if (mRenderer) mEngine->destroy(mRenderer);
    if (!mCameraEntity.isNull()) {
        mEngine->destroyCameraComponent(mCameraEntity);
        utils::EntityManager::get().destroy(mCameraEntity);
    }
    Engine::destroy(&mEngine);
    mEngine = nullptr;
}

void FilamentWindow::preload(uint64_t targetPoints) {
    const uint64_t have = mCloud.total();
    if (targetPoints <= have) return;
    uint64_t remaining = targetPoints - have;
    std::vector<PointVertex> buf(PagedCloud::kPageCapacity);
    uint64_t genIndex = have;
    const double t0 = mClock.nsecsElapsed() / 1e9;
    while (remaining > 0) {
        const size_t n = size_t(std::min<uint64_t>(remaining, PagedCloud::kPageCapacity));
        PointSource::generateBulkParallel(buf.data(), n, genIndex);
        mCloud.append(buf.data(), n);
        genIndex += n;
        remaining -= n;
        mEngine->flushAndWait(); // keep the staging pool bounded during bulk upload
    }
    const double dt = mClock.nsecsElapsed() / 1e9 - t0;
    note("preloaded to " + std::to_string(mCloud.total()) + " pts in "
         + std::to_string(dt) + " s (" + std::to_string(mCloud.pageCount()) + " pages, "
         + std::to_string(mCloud.gpuBytes() / (1024 * 1024)) + " MB GPU buffers)");
}

void FilamentWindow::handleResizeIfNeeded(bool force) {
    const double dpr = devicePixelRatio();
    const int wpx = std::max(1, int(width() * dpr));
    const int hpx = std::max(1, int(height() * dpr));
    if (!force && wpx == mPxW && hpx == mPxH && std::abs(dpr - mDpr) < 1e-9) return;

    mPxW = wpx;
    mPxH = hpx;
    mDpr = dpr;

    // Filament's own sample app does exactly this on macOS: destroy + recreate
    // the swapchain, because CAMetalLayer drawable size is not renegotiated.
    mEngine->flushAndWait();
    mEngine->destroy(mSwapChain);
    uint64_t flags = 0;
    if (!mOpts.shotDir.empty()) flags |= SwapChain::CONFIG_READABLE;
    macbridge::configureLayer(mMetalLayer, mDpr, mPxW, mPxH, width(), height(), mOpts.vsync);
    mSwapChain = mEngine->createSwapChain(mNativeWindowArg, flags);
    macbridge::configureLayer(mMetalLayer, mDpr, mPxW, mPxH, width(), height(), mOpts.vsync);

    mView->setViewport({0, 0, uint32_t(mPxW), uint32_t(mPxH)});
    ++mSwapChainRecreates;
}

void FilamentWindow::updateCamera() {
    if (mAutoOrbit) {
        mAzimuth += 0.0035f;
    }
    const float cx = 6.0f, cy = 4.0f, cz = 1.5f;
    const float ex = cx + mDistance * std::cos(mElevation) * std::cos(mAzimuth);
    const float ey = cy + mDistance * std::cos(mElevation) * std::sin(mAzimuth);
    const float ez = cz + mDistance * std::sin(mElevation);
    mCamera->lookAt({ex, ey, ez}, {cx, cy, cz}, {0, 0, 1});
    const double aspect = double(mPxW) / double(std::max(1, mPxH));
    mCamera->setProjection(60.0, aspect, 0.1, 200.0, Camera::Fov::VERTICAL);
}

void FilamentWindow::renderFrame() {
    const double now = mClock.nsecsElapsed() / 1e9;
    const double tick0 = now;
    if (mOpts.bench) benchStep(now);
    if (!mInitialized || !isExposed() || !mSwapChain) {
        mLastFrameStart = 0;
        return;
    }

    handleResizeIfNeeded(false);

    // Drain the live stream into the tail page.
    if (mSource.running() && mCloud.total() < mOpts.maxPoints) {
        size_t got = mSource.drain(mStage.data(), mStage.size());
        if (got) mCloud.append(mStage.data(), got);
    }

    updateCamera();

    // beginFrame() returns false when the backend has no free drawable -- i.e.
    // the render thread is ahead of the display / GPU. Those ticks are NOT
    // frames and must not be counted, or fps is meaningless.
    const double cpu0 = mClock.nsecsElapsed() / 1e9;
    const bool drew = mRenderer->beginFrame(mSwapChain);
    if (drew) {
        mRenderer->render(mView);
        mRenderer->endFrame();
    }
    const double cpu1 = mClock.nsecsElapsed() / 1e9;

    if (!drew) {
        ++mSkippedTicks;
        return;
    }

    ++mFrameCount;
    mCpuMs.push_back((cpu1 - tick0) * 1000.0); // whole tick: ingest + camera + submit
    if (mLastFrameStart > 0) mIntervalMs.push_back((cpu0 - mLastFrameStart) * 1000.0);
    mLastFrameStart = cpu0;

    // GPU timings straight from Filament's frame history.
    auto hist = mRenderer->getFrameInfoHistory(8);
    for (const auto& fi : hist) {
        if (fi.frameId > mLastGpuFrameId && fi.gpuFrameDuration > 0) {
            mLastGpuFrameId = fi.frameId;
            mGpuMs.push_back(double(fi.gpuFrameDuration) / 1e6);
        }
    }

    if (now - mStatusTick > 0.5) {
        mStatusTick = now;
        const double fps = mIntervalMs.empty() ? 0.0 : 1000.0 / mean(mIntervalMs);
        mStatus = QString("%1 pts (%2 pages) | %3 fps | cpu p95 %4 ms | gpu p95 %5 ms | %6x%7 px "
                          "dpr %8 | vsync %9 | swapchains %10")
                          .arg(mCloud.total())
                          .arg(mCloud.pageCount())
                          .arg(fps, 0, 'f', 1)
                          .arg(percentile(mCpuMs, 0.95), 0, 'f', 2)
                          .arg(percentile(mGpuMs, 0.95), 0, 'f', 2)
                          .arg(mPxW)
                          .arg(mPxH)
                          .arg(mDpr, 0, 'f', 2)
                          .arg(mOpts.vsync ? "on" : "off")
                          .arg(mSwapChainRecreates);
        emit statusChanged(mStatus);
    }
}

void FilamentWindow::beginPhase(const std::string& name) {
    mCurrentPhase = name;
    mCpuMs.clear();
    mIntervalMs.clear();
    mGpuMs.clear();
    mFrameCount = 0;
    mSkippedTicks = 0;
    mLastFrameStart = 0;
    mPhaseStart = mClock.nsecsElapsed() / 1e9;
}

void FilamentWindow::endPhase(uint64_t points, const std::string& extra) {
    PhaseResult r;
    r.name = mCurrentPhase;
    r.points = points;
    r.seconds = mClock.nsecsElapsed() / 1e9 - mPhaseStart;
    r.frames = mFrameCount;
    r.fps = r.seconds > 0 ? double(r.frames) / r.seconds : 0.0;
    r.cpuFrameP50 = percentile(mCpuMs, 0.50);
    r.cpuFrameP95 = percentile(mCpuMs, 0.95);
    r.frameIntervalP95 = percentile(mIntervalMs, 0.95);
    r.gpuP50 = percentile(mGpuMs, 0.50);
    r.gpuP95 = percentile(mGpuMs, 0.95);
    r.skippedTicks = mSkippedTicks;
    r.note = extra;
    mResults.push_back(r);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "PHASE %-28s pts=%-10llu frames=%-6llu %.2fs -> %.1f fps | cpu p50 %.2f p95 %.2f "
                  "| gpu p50 %.2f p95 %.2f | interval p95 %.2f | idle ticks %llu | %s",
                  r.name.c_str(), (unsigned long long) r.points, (unsigned long long) r.frames,
                  r.seconds, r.fps, r.cpuFrameP50, r.cpuFrameP95, r.gpuP50, r.gpuP95,
                  r.frameIntervalP95, (unsigned long long) r.skippedTicks, r.note.c_str());
    note(buf);
}

void FilamentWindow::captureShot(const std::string& path) {
    if (mOpts.shotDir.empty()) return;
    const int w = mPxW, h = mPxH;
    const size_t bytes = size_t(w) * h * 4;
    void* mem = std::malloc(bytes);
    struct Ctx {
        int w, h;
        std::string path;
        FilamentWindow* self;
    };
    auto* ctx = new Ctx{w, h, path, this};
    // beginFrame() can legitimately fail (no free drawable) -- retry until we
    // own a frame, otherwise the screenshot silently never happens.
    bool drew = false;
    for (int tries = 0; tries < 500 && !drew; ++tries) {
        drew = mRenderer->beginFrame(mSwapChain);
        if (!drew) mEngine->flushAndWait();
    }
    if (drew) {
        mRenderer->render(mView);
        mRenderer->readPixels(0, 0, uint32_t(w), uint32_t(h),
                              backend::PixelBufferDescriptor(
                                      mem, bytes, backend::PixelDataFormat::RGBA,
                                      backend::PixelDataType::UBYTE,
                                      [](void* buf, size_t, void* user) {
                                          auto* c = static_cast<Ctx*>(user);
                                          QImage img(c->w, c->h, QImage::Format_RGBA8888);
                                          const auto* src = static_cast<const uchar*>(buf);
                                          for (int y = 0; y < c->h; ++y) {
                                              std::memcpy(img.scanLine(y),
                                                          src + size_t(y) * c->w * 4,
                                                          size_t(c->w) * 4);
                                          }
                                          img.save(QString::fromStdString(c->path));
                                          if (c->self) c->self->mLastShot = img;
                                          std::fprintf(stderr, "[s3] wrote %s\n", c->path.c_str());
                                          std::free(buf);
                                          delete c;
                                      },
                                      ctx));
        mRenderer->endFrame();
    } else {
        std::free(mem);
        delete ctx;
        note("screenshot skipped: could not acquire a drawable");
    }
    mEngine->flushAndWait();

    // Also grab the Qt side of the window (chrome + status bar). The Filament
    // area is a native child window, so Qt paints nothing there -- which is
    // itself evidence that the 3D content is not going through Qt's painter.
    if (mTopLevel) {
        const QPixmap chrome = mTopLevel->grab();
        chrome.save(QString::fromStdString(path).replace(".png", "-qtchrome.png"));
        // ...and a composite of the two, which is what the user actually sees.
        if (!mLastShot.isNull()) {
            QPixmap composed = chrome;
            QPainter pr(&composed);
            const QPoint at = mTopLevel->mapFromGlobal(mapToGlobal(QPoint(0, 0)));
            pr.drawImage(QRectF(at.x(), at.y(), width(), height()), mLastShot);
            pr.end();
            composed.save(QString::fromStdString(path).replace(".png", "-window.png"));
        }
    }
}

void FilamentWindow::benchStep(double now) {
    auto next = [&] {
        ++mBenchStage;
        mStageEntered = now;
    };
    const double el = now - mStageEntered;

    switch (mBenchStage) {
        case 0: // warm up the pipeline with a small cloud
            if (mCloud.total() == 0) preload(500'000);
            if (el > 1.5) {
                preload(2'000'000);
                beginPhase("2M static");
                next();
            }
            break;
        case 1:
            if (el > mOpts.phaseSeconds) {
                endPhase(mCloud.total(), "psize=" + std::to_string(mOpts.pointSize));
                if (!mOpts.shotDir.empty()) captureShot(mOpts.shotDir + "/02-2M.png");
                preload(5'000'000);
                beginPhase("5M static");
                next();
            }
            break;
        case 2:
            if (el > mOpts.phaseSeconds) {
                endPhase(mCloud.total(), "psize=" + std::to_string(mOpts.pointSize));
                preload(10'000'000);
                beginPhase("10M static");
                next();
            }
            break;
        case 3:
            if (el > mOpts.phaseSeconds) {
                endPhase(mCloud.total(), "psize=" + std::to_string(mOpts.pointSize));
                if (!mOpts.shotDir.empty()) captureShot(mOpts.shotDir + "/03-10M.png");
                mMatInstance->setParameter("pointSize", 1.0f);
                beginPhase("10M static, pointSize 1");
                next();
            }
            break;
        case 4:
            if (el > 4.0) {
                endPhase(mCloud.total(), "psize=1.0");
                mMatInstance->setParameter("pointSize", 4.0f);
                beginPhase("10M static, pointSize 4");
                next();
            }
            break;
        case 5:
            if (el > 4.0) {
                endPhase(mCloud.total(), "psize=4.0");
                mMatInstance->setParameter("pointSize", mOpts.pointSize);
                mCloud.clear();
                mSource.resetCounters();
                mSource.start(mOpts.streamRate);
                mStreamStartPoints = 0;
                beginPhase("live ingest 200k pts/s (0 -> N)");
                next();
            }
            break;
        case 6:
            if (el > mOpts.streamSeconds) {
                const double secs = mClock.nsecsElapsed() / 1e9 - mPhaseStart;
                const uint64_t ingested = mCloud.total() - mStreamStartPoints;
                mSource.stop();
                char extra[256];
                std::snprintf(extra, sizeof(extra), "ingested %llu pts = %.0f pts/s, ring drops %llu",
                              (unsigned long long) ingested, double(ingested) / secs,
                              (unsigned long long) mSource.dropped());
                endPhase(mCloud.total(), extra);
                mResults.back().ingestRate = double(ingested) / secs;
                mResults.back().dropped = mSource.dropped();
                if (!mOpts.shotDir.empty()) captureShot(mOpts.shotDir + "/04-streaming.png");
                preload(10'000'000);
                beginPhase("10M + live ingest 200k pts/s");
                mSource.resetCounters();
                mSource.start(mOpts.streamRate);
                mStreamStartPoints = mCloud.total();
                mOpts.maxPoints = 13'000'000;
                next();
            }
            break;
        case 7:
            if (el > 10.0) {
                const double secs = mClock.nsecsElapsed() / 1e9 - mPhaseStart;
                const uint64_t ingested = mCloud.total() - mStreamStartPoints;
                mSource.stop();
                char extra[256];
                std::snprintf(extra, sizeof(extra), "ingested %llu pts = %.0f pts/s",
                              (unsigned long long) ingested, double(ingested) / secs);
                endPhase(mCloud.total(), extra);
                mResults.back().ingestRate = double(ingested) / secs;
                if (mOpts.stressResize) {
                    note("=== resize stress: continuous window resize while rendering 10M+ pts ===");
                    beginPhase("resize stress @ 10M");
                } else {
                    beginPhase("skipped");
                }
                next();
            }
            break;
        case 8: { // continuous resize
            if (mOpts.stressResize && mTopLevel) {
                const int base = 700;
                const int amp = 520;
                const double t = el * 2.7;
                const int w = base + int(amp * (0.5 + 0.5 * std::sin(t)));
                const int h = 500 + int(360 * (0.5 + 0.5 * std::sin(t * 1.37 + 0.9)));
                mTopLevel->resize(w, h);
                ++mResizeCounter;
            }
            if (el > 10.0) {
                char extra[256];
                std::snprintf(extra, sizeof(extra), "%u resize requests, %u swapchain recreates, no crash",
                              mResizeCounter, mSwapChainRecreates);
                endPhase(mCloud.total(), extra);
                if (mTopLevel) mTopLevel->resize(1280, 800);
                beginPhase("post-resize steady state");
                next();
            }
            break;
        }
        case 9:
            if (el > 4.0) {
                endPhase(mCloud.total(), "after resize storm");
                if (!mOpts.shotDir.empty()) captureShot(mOpts.shotDir + "/05-after-resize.png");
                note("=== minimize/restore test ===");
                if (mTopLevel) mTopLevel->showMinimized();
                next();
            }
            break;
        case 10:
            if (el > 2.5) {
                if (mTopLevel) {
                    mTopLevel->showNormal();
                    mTopLevel->raise();
                }
                note("restored from minimized");
                beginPhase("after minimize/restore");
                next();
            }
            break;
        case 11:
            if (el > 4.0) {
                endPhase(mCloud.total(), "window survived minimize/restore");
                if (!mOpts.shotDir.empty()) captureShot(mOpts.shotDir + "/06-after-minimize.png");
                next();
            }
            break;
        case 12:
            note("benchmark complete");
            mBenchStage = -1;
            emit benchFinished();
            break;
        default:
            break;
    }
}

void FilamentWindow::mousePressEvent(QMouseEvent* e) {
    mLastMouse = e->position();
    mDragging = true;
    mAutoOrbit = false;
}

void FilamentWindow::mouseMoveEvent(QMouseEvent* e) {
    if (!mDragging) return;
    const QPointF d = e->position() - mLastMouse;
    mLastMouse = e->position();
    mAzimuth -= float(d.x()) * 0.008f;
    mElevation = std::clamp(mElevation + float(d.y()) * 0.008f, -1.4f, 1.4f);
}

void FilamentWindow::wheelEvent(QWheelEvent* e) {
    mDistance = std::clamp(mDistance * (1.0f - float(e->angleDelta().y()) * 0.001f), 1.0f, 120.0f);
}

void FilamentWindow::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
        case Qt::Key_Space: mAutoOrbit = !mAutoOrbit; break;
        case Qt::Key_S:
            if (mSource.running()) {
                mSource.stop();
                note("stream stopped");
            } else {
                mSource.start(mOpts.streamRate);
                note("stream started @ 200k pts/s");
            }
            break;
        case Qt::Key_1: preload(2'000'000); break;
        case Qt::Key_2: preload(5'000'000); break;
        case Qt::Key_3: preload(10'000'000); break;
        case Qt::Key_C: mCloud.clear(); break;
        default: break;
    }
}
