#pragma once

#include "PagedCloud.h"
#include "PointSource.h"
#include "PointTypes.h"

#include <QElapsedTimer>
#include <QImage>
#include <QWindow>

#include <deque>
#include <string>
#include <vector>

#include <utils/Entity.h>

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
class View;
} // namespace filament

// Which native handle is handed to Engine::createSwapChain().
enum class NativeHandle {
    QtMetalLayer, // Qt's own QMetalLayer (a CAMetalLayer) behind QWindow::winId()
    OwnMetalLayer, // a CAMetalLayer we create as a sublayer of Qt's NSView
    NSView,       // the raw NSView* -- what SwapChain.h documents; see REPORT.md
};

struct SpikeOptions {
    bool bench = false;
    bool vsync = true;
    NativeHandle handle = NativeHandle::QtMetalLayer;
    bool stressResize = true;
    bool postProcessing = true;
    uint64_t preloadPoints = 2'000'000;
    float pointSize = 2.0f;
    double phaseSeconds = 6.0;
    double streamSeconds = 20.0;
    double streamRate = 200'000.0;
    uint64_t maxPoints = 10'000'000;
    std::string shotDir;
    std::string resultFile;
};

struct PhaseResult {
    std::string name;
    uint64_t points = 0;
    double seconds = 0;
    uint64_t frames = 0;
    double fps = 0;
    double cpuFrameP50 = 0;
    double cpuFrameP95 = 0;
    double frameIntervalP95 = 0;
    double gpuP50 = 0;
    double gpuP95 = 0;
    double ingestRate = 0;
    uint64_t dropped = 0;
    uint64_t skippedTicks = 0;
    std::string note;
};

class FilamentWindow : public QWindow {
    Q_OBJECT
public:
    explicit FilamentWindow(const SpikeOptions& opts, QWindow* parent = nullptr);
    ~FilamentWindow() override;

    void setTopLevel(QWidget* w) { mTopLevel = w; }
    const std::vector<PhaseResult>& results() const { return mResults; }
    const std::vector<std::string>& log() const { return mLog; }
    QString statusLine() const { return mStatus; }

signals:
    void statusChanged(const QString&);
    void benchFinished();

protected:
    void exposeEvent(QExposeEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    void initFilament();
    void destroyFilament();
    void renderFrame();
    void handleResizeIfNeeded(bool force);
    void updateCamera();
    void preload(uint64_t targetPoints);
    void note(const std::string& s);

    // benchmark driver
    void benchStep(double nowSec);
    void beginPhase(const std::string& name);
    void endPhase(uint64_t points, const std::string& extra);
    void captureShot(const std::string& path);

    SpikeOptions mOpts;
    QWidget* mTopLevel = nullptr;

    filament::Engine* mEngine = nullptr;
    filament::Renderer* mRenderer = nullptr;
    filament::Scene* mScene = nullptr;
    filament::View* mView = nullptr;
    filament::Camera* mCamera = nullptr;
    filament::Skybox* mSkybox = nullptr;
    filament::ColorGrading* mColorGrading = nullptr;
    filament::SwapChain* mSwapChain = nullptr;
    filament::Material* mMaterial = nullptr;
    filament::MaterialInstance* mMatInstance = nullptr;
    utils::Entity mCameraEntity;

    void* mNativeView = nullptr;
    void* mNativeWindowArg = nullptr; // what we hand to createSwapChain
    void* mMetalLayer = nullptr;

    PagedCloud mCloud;
    PointSource mSource;
    std::vector<PointVertex> mStage;

    bool mInitialized = false;
    int mPxW = 0, mPxH = 0;
    double mDpr = 1.0;
    uint32_t mSwapChainRecreates = 0;

    // camera orbit
    float mAzimuth = 2.2f, mElevation = 0.02f, mDistance = 2.6f;
    QPointF mLastMouse;
    bool mDragging = false;
    bool mAutoOrbit = true;

    // stats
    QElapsedTimer mClock;
    double mLastFrameStart = 0;
    std::vector<double> mCpuMs;
    std::vector<double> mIntervalMs;
    std::vector<double> mGpuMs;
    uint32_t mLastGpuFrameId = 0;
    uint64_t mFrameCount = 0;
    uint64_t mSkippedTicks = 0;
    double mPhaseStart = 0;
    double mStatusTick = 0;
    QString mStatus;
    QImage mLastShot;

    // bench state machine
    int mBenchStage = -1;
    double mStageEntered = 0;
    uint64_t mStreamStartPoints = 0;
    std::vector<PhaseResult> mResults;
    std::vector<std::string> mLog;
    std::string mCurrentPhase;
    uint32_t mResizeCounter = 0;
};
