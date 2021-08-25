/*
 *  SPDX-FileCopyrightText: 2015 Jouni Pentikäinen <joupent@gmail.com>
 *  SPDX-FileCopyrightText: 2021 Eoin O'Neill <eoinoneill1991@gmail.com>
 *  SPDX-FileCopyrightText: 2021 Emmet O'Neill <emmetoneill.pdx@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_animation_player.h"

#include "KisElapsedTimer.h"
#include <QTimer>
#include <QtMath>

//#define PLAYER_DEBUG_FRAMERATE

#include "kis_global.h"
#include "kis_algebra_2d.h"

#include "kis_config.h"
#include "kis_config_notifier.h"
#include "kis_image.h"
#include "kis_canvas2.h"
#include "kis_animation_frame_cache.h"
#include "kis_signal_auto_connection.h"
#include "kis_image_animation_interface.h"
#include "kis_time_span.h"
#include "kis_signal_compressor.h"
#include <KisDocument.h>
#include <QFileInfo>
#include <QReadWriteLock>
#include <QThread>
#include "KisSyncedAudioPlayback.h"
#include "kis_signal_compressor_with_param.h"
#include "kis_image_barrier_locker.h"
#include "kis_layer_utils.h"
#include "KisDecoratedNodeInterface.h"
#include "kis_keyframe_channel.h"
#include "kis_algebra_2d.h"

#include "kis_image_config.h"
#include <limits>

#include "KisViewManager.h"
#include "kis_icon_utils.h"

#include "KisPart.h"
#include "dialogs/KisAsyncAnimationCacheRenderDialog.h"
#include "KisRollingMeanAccumulatorWrapper.h"
#include "kis_onion_skin_compositor.h"

#include <atomic>

qint64 framesToMSec(qreal value, int fps) {
    return qRound(value / fps * 1000.0);
}

qreal msecToFrames(qint64 value, int fps) {
    return qreal(value) * fps / 1000.0;
}

int framesToScaledTimeMS(qreal frame, int fps, qreal playbackSpeed) {
    return qRound(framesToMSec(frame, fps) / playbackSpeed);
}

qreal scaledTimeToFrames(qint64 time, int fps, qreal playbackSpeed) {
    return msecToFrames(time, fps) * playbackSpeed;
}

const int AUDIO_BUFFER_SAMPLES = 512;
const int AUDIO_SAMPLE_RATE = 44100; // TODO: Querry audio device.

struct PlaybackData {
public:
    PlaybackData(int playhead) {
        setPlayheadFrame(playhead);
    }

    ~PlaybackData() = default;

    int playheadFrame() const {
        return m_playheadFrame;
    }

    void setPlayheadFrame(int frame) {
        m_playheadFrame = frame;
    }

    void advancePlayhead(int increment = 1) { //TODO check if necessary here or should only be in worker thread?
        setPlayheadFrame( (playheadFrame() - playbackRange().start() + increment)
                          % (playbackRange().end() + 1 - playbackRange().start())
                          + playbackRange().start() );
    }

    qreal playbackSpeed() const {
        return m_playbackSpeed;
    }

    void setPlaybackSpeed(qreal p_playbackSpeed) {
        m_playbackSpeed = p_playbackSpeed;
    }

    KisTimeSpan playbackRange() const {
        return m_playbackRange;
    }

    void setPlaybackRange(KisTimeSpan p_playbackRange) {
        m_playbackRange = p_playbackRange;
    }

    bool dropFrames() const {
        return m_dropFrames;
    }

    void setDropFrames(bool value) {
        m_dropFrames = value;
    }

    std::atomic<int> m_frameRate;

private:
    std::atomic<int> m_playheadFrame; //!< The **current** playback frame. May or may not be the same as the visible frame (Frame dropping, audio, etc...)
    std::atomic<qreal> m_playbackSpeed;
    std::atomic<KisTimeSpan> m_playbackRange;
    std::atomic<bool> m_dropFrames; //!< Whether we should be dropping frames to preserve playback timing.
};

class PlaybackWorker : public QObject {
    Q_OBJECT
public:
    PlaybackWorker(QSharedPointer<PlaybackData> shared)
        : QObject(nullptr)
        , m_sharedData(shared) {

    }

    //Where the thread "enters"...
    void run() {
        QTimer* timer = new QTimer(this);
        timer->setTimerType(Qt::PreciseTimer);
        connect(timer, &QTimer::timeout, this, &PlaybackWorker::process);
        timer->start(((qreal)AUDIO_BUFFER_SAMPLES / (qreal)AUDIO_SAMPLE_RATE) * 1000);
        m_timeSinceLastPlayheadChange.start();
        m_readyToPostFrame = true;
    }

    //Where the loop happens...
    void process() {
        // AUDIO
        // Every tick
        //     - Pull and resample audio from via via decoder.
        //     - Push buffer size audio to audio device.
        qDebug() << "Decode & resample source audio";
        qDebug() << "Push 512 samples to audio device";

        // VISUAL
        // Every tick
        //     - Check time since last frame. If larger than seconds per frame:
        //              - Advance playhead by one(?) frame
        //              - Try to display the frame (Drop frames???)
        const int msecPerFrame = framesToScaledTimeMS(1, m_sharedData->m_frameRate, m_sharedData->playbackSpeed());
        if ( m_readyToPostFrame && m_timeSinceLastPlayheadChange.elapsed() > msecPerFrame) {
            const int numFramesToAdvance = m_timeSinceLastPlayheadChange.elapsed() / msecPerFrame;
            m_sharedData->advancePlayhead(numFramesToAdvance);
            emit playheadChanged();
            const int remainderMS = m_timeSinceLastPlayheadChange.elapsed() - (msecPerFrame * numFramesToAdvance);
            m_timeSinceLastPlayheadChange.restart(remainderMS);
            m_readyToPostFrame = m_sharedData->dropFrames() ? false : true;
        }
    }

    void frameReady() {
        m_readyToPostFrame = true;
    }

    ~PlaybackWorker() = default;

Q_SIGNALS:
    void playheadChanged();

private:
    bool m_readyToPostFrame;
    QSharedPointer<PlaybackData> m_sharedData;
    KisElapsedTimer m_timeSinceLastPlayheadChange;
};

//Setup environment for playback.
class PlaybackEnvironment : public QObject {
    Q_OBJECT
public:
    PlaybackEnvironment(int originFrame, QSharedPointer<PlaybackData> data, KisAnimationPlayer* parent = nullptr)
        : QObject(parent)
        , m_originFrame(originFrame)
        , m_sharedData(data)
        , m_animPlayer(parent)
    {
        m_sharedData->setPlayheadFrame(m_originFrame);
        connect(&m_cancelTrigger, SIGNAL(output()), parent, SLOT(stop()));
    }

    ~PlaybackEnvironment() {
        restore();
    }

    PlaybackEnvironment() = delete;
    PlaybackEnvironment(const PlaybackEnvironment&) = delete;
    PlaybackEnvironment& operator= (const PlaybackEnvironment&) = delete;

    int originFrame() { return m_originFrame; }

    int playheadFrame() { return m_sharedData->playheadFrame(); }
    void setPlayheadFrame(int value) { m_sharedData->setPlayheadFrame(value);}

    void prepare(KisCanvas2* canvas)
    {
        KIS_ASSERT(canvas); // Sanity check...
        m_canvas = canvas;

        const KisTimeSpan range = canvas->image()->animationInterface()->playbackRange();
        m_sharedData->setPlaybackRange(range);

        // Initialize and optimize playback environment...
        if (canvas->frameCache()) {
            KisImageConfig cfg(true);

            const int dimensionLimit = cfg.useAnimationCacheFrameSizeLimit() ?
                        cfg.animationCacheFrameSizeLimit() : std::numeric_limits<int>::max();

            const int largestDimension = KisAlgebra2D::maxDimension(canvas->image()->bounds());

            const QRect regionOfInterest =
                        cfg.useAnimationCacheRegionOfInterest() && largestDimension > dimensionLimit ?
                            canvas->regionOfInterest() : canvas->coordinatesConverter()->imageRectInImagePixels();

            const QRect minimalRect =
                    canvas->coordinatesConverter()->widgetRectInImagePixels().toAlignedRect() &
                    canvas->coordinatesConverter()->imageRectInImagePixels();

            canvas->frameCache()->dropLowQualityFrames(range, regionOfInterest, minimalRect);
            canvas->setRenderingLimit(regionOfInterest);

            // Preemptively cache all frames...
            KisAsyncAnimationCacheRenderDialog dlg(canvas->frameCache(), range);
            dlg.setRegionOfInterest(regionOfInterest);
            dlg.regenerateRange(canvas->viewManager());
        } else {
            KisImageBarrierLocker locker(canvas->image());
            KisLayerUtils::recursiveApplyNodes(canvas->image()->root(), [this](KisNodeSP node){
                KisDecoratedNodeInterface* decoratedNode = dynamic_cast<KisDecoratedNodeInterface*>(node.data());
                if (decoratedNode && decoratedNode->decorationsVisible()) {
                    decoratedNode->setDecorationsVisible(false, false);
                    m_disabledDecoratedNodes.append(node);
                }
            });
        }

        // Setup appropriate interrupt connections...
        m_cancelStrokeConnections.addConnection(
                canvas->image().data(), SIGNAL(sigUndoDuringStrokeRequested()),
                &m_cancelTrigger, SLOT(tryFire()));

        m_cancelStrokeConnections.addConnection(
                canvas->image().data(), SIGNAL(sigStrokeCancellationRequested()),
                &m_cancelTrigger, SLOT(tryFire()));

        // We only want to stop on stroke end when running on a system
        // without cache / opengl / graphics driver support!
        if (canvas->frameCache()) {
            m_cancelStrokeConnections.addConnection(
                    canvas->image().data(), SIGNAL(sigStrokeEndRequested()),
                    &m_cancelTrigger, SLOT(tryFire()));
        }

        // Setup playback thread and connections...
        m_playbackThread.reset(new QThread(this));
        PlaybackWorker* worker = new PlaybackWorker(m_sharedData);
        worker->moveToThread(m_playbackThread.data());
        connect(m_playbackThread.data(), &QThread::finished, worker, &QObject::deleteLater);
        connect(m_playbackThread.data(), &QThread::started, worker, &PlaybackWorker::run );
        connect(this, &PlaybackEnvironment::finishedSeeking, worker, &PlaybackWorker::frameReady, Qt::QueuedConnection);
        connect(worker, &PlaybackWorker::playheadChanged, this, [this](){
            if (m_animPlayer) {
                m_animPlayer->seek(m_sharedData->playheadFrame());
                emit finishedSeeking();
            }
        }, Qt::QueuedConnection);

        m_playbackThread->start();
    }

    void restore() {
        m_playbackThread->quit();
        m_playbackThread->wait();
        m_cancelStrokeConnections.clear();

        if (m_canvas) {
            if (m_canvas->frameCache()) {
                m_canvas->setRenderingLimit(QRect());
            } else {
                KisImageBarrierLocker locker(m_canvas->image());
                Q_FOREACH(KisNodeWSP disabledNode, m_disabledDecoratedNodes) {
                    KisDecoratedNodeInterface* decoratedNode = dynamic_cast<KisDecoratedNodeInterface*>(disabledNode.data());
                    if (decoratedNode) {
                        decoratedNode->setDecorationsVisible(true, true);
                    }
                }
                m_disabledDecoratedNodes.clear();
            }

            m_canvas = nullptr;
        }
    }

Q_SIGNALS:
    void finishedSeeking();

private:
    int m_originFrame; //!< The frame user started playback from.
    KisSignalAutoConnectionsStore m_cancelStrokeConnections;
    SingleShotSignal m_cancelTrigger;
    QVector<KisNodeWSP> m_disabledDecoratedNodes;

    QScopedPointer<QThread> m_playbackThread;
    QSharedPointer<PlaybackData> m_sharedData;
    KisCanvas2* m_canvas;
    KisAnimationPlayer* m_animPlayer;
};

#include "kis_animation_player.moc"

struct KisAnimationPlayer::Private
{
public:
    Private()
        : data(new PlaybackData(-1))
        , visibleFrame(-1)
        , playbackStatisticsCompressor(1000, KisSignalCompressor::FIRST_INACTIVE)
          {}

    QScopedPointer<PlaybackEnvironment> playback;
    QSharedPointer<PlaybackData> data;

    KisCanvas2 *canvas;
    KisAnimationPlayer::PlaybackState playbackState;
    int visibleFrame; //!< This the frame that is currently being displayed on the canvas. Can be different from the current playhead.

    KisSignalCompressor playbackStatisticsCompressor;
};

KisAnimationPlayer::KisAnimationPlayer(KisCanvas2 *canvas)
    : QObject(canvas)
    , m_d(new Private())
{
    m_d->playbackState = STOPPED;
    m_d->data->setPlaybackSpeed(1.0f);
    m_d->canvas = canvas;

    connect(KisConfigNotifier::instance(),
            &KisConfigNotifier::dropFramesModeChanged,
            this,
            &KisAnimationPlayer::updateDropFramesMode);
    updateDropFramesMode();

    connect(&m_d->playbackStatisticsCompressor, SIGNAL(timeout()),
            this, SIGNAL(sigPlaybackStatisticsUpdated()));

    // Grow to new playback range when new frames added (configurable)...
    connect(m_d->canvas->image()->animationInterface(), &KisImageAnimationInterface::sigKeyframeAdded, this, [this](const KisKeyframeChannel*, int time){
        if (m_d->canvas && m_d->canvas->image()) {
            KisImageAnimationInterface* animInterface = m_d->canvas->image()->animationInterface();
            KisConfig cfg(true);
            if (animInterface && cfg.adaptivePlaybackRange()) {
                KisTimeSpan desiredPlaybackRange = animInterface->fullClipRange();
                desiredPlaybackRange.include(time);
                animInterface->setFullClipRange(desiredPlaybackRange);
            }
        }
    });

    connect(m_d->canvas->image()->animationInterface(), &KisImageAnimationInterface::sigFrameRegenerated, this, [this](int frame){
        if (playbackState() != PLAYING) {
            m_d->visibleFrame = frame;
        }
    });

    connect(m_d->canvas->image()->animationInterface(), &KisImageAnimationInterface::sigFramerateChanged, this, [this](){
       m_d->data->m_frameRate = m_d->canvas->image()->animationInterface()->framerate();
    });
    m_d->data->m_frameRate = m_d->canvas->image()->animationInterface()->framerate();

}

KisAnimationPlayer::~KisAnimationPlayer()
{}

KisAnimationPlayer::PlaybackState KisAnimationPlayer::playbackState()
{
    return m_d->playbackState;
}

void KisAnimationPlayer::updateDropFramesMode()
{
    KisConfig cfg(true);
    m_d->data->setDropFrames(cfg.animationDropFrames());
}

void KisAnimationPlayer::play()
{
    KIS_ASSERT(m_d->canvas);

    if (!m_d->playback) {
        m_d->playback.reset(new PlaybackEnvironment(visibleFrame(), m_d->data, this));
    }

    m_d->playback->prepare(m_d->canvas);
    setPlaybackState(PLAYING);
}

void KisAnimationPlayer::pause()
{
    if (playbackState() == PLAYING) {
        KIS_ASSERT(m_d->playback);
        m_d->playback->restore();
        setPlaybackState(PAUSED);
        if (m_d->playback) {
            seek(m_d->playback->playheadFrame());
        }
    }
}

void KisAnimationPlayer::playPause()
{
    if (m_d->playbackState == PLAYING) {
        pause();
    } else {
        play();
    }
}

void KisAnimationPlayer::stop()
{
    KisImageAnimationInterface* animation = m_d->canvas->image()->animationInterface();
    if(m_d->playbackState == STOPPED) {
        KIS_SAFE_ASSERT_RECOVER_RETURN(animation);
        const int startFrame = animation->fullClipRange().start();
        seek(startFrame);
    } else {
        const int origin = m_d->playback->originFrame();
        m_d->playback->restore();
        m_d->playback.reset();
        setPlaybackState(STOPPED);

        if (m_d->visibleFrame == origin) {
            m_d->canvas->refetchDataFromImage();
        } else {
            seek(origin);
        }
    }
}

void KisAnimationPlayer::seek(int frameIndex, bool preferCachedFrames)
{
    if (!m_d->canvas || !m_d->canvas->image()) return;

    if (m_d->playbackState == PLAYING || preferCachedFrames) {
        if (m_d->playback) {
            if (m_d->playback->playheadFrame() != frameIndex) {
                m_d->playback->setPlayheadFrame( frameIndex >= 0 ? frameIndex : m_d->playback->playheadFrame() );
            }
            displayFrame(m_d->playback->playheadFrame());
        } else {
            displayFrame(frameIndex);
        }
    } else {
        KisImageAnimationInterface *animInterface = m_d->canvas->image()->animationInterface();

        if (frameIndex == m_d->visibleFrame) {
            return;
        } else {
            if (m_d->playback) {
                m_d->playback->setPlayheadFrame(frameIndex > 0 ? frameIndex : m_d->playback->playheadFrame());
            }

            animInterface->requestTimeSwitchWithUndo(frameIndex);
        }
    }
}

void KisAnimationPlayer::previousFrame()
{
    if (!m_d->canvas) return;
    KisImageAnimationInterface *animInterface = m_d->canvas->image()->animationInterface();

    const int startFrame = animInterface->playbackRange().start();
    const int endFrame = animInterface->playbackRange().end();

    int frame = visibleFrame() - 1;

    if (frame < startFrame || frame >  endFrame) {
        frame = endFrame;
    }

    if (frame >= 0) {
        if (m_d->playbackState != STOPPED) {
            stop();
        }

        seek(frame);
    }
}

void KisAnimationPlayer::nextFrame()
{
    if (!m_d->canvas) return;
    KisImageAnimationInterface *animInterface = m_d->canvas->image()->animationInterface();

    const int startFrame = animInterface->playbackRange().start();
    const int endFrame = animInterface->playbackRange().end();

    int frame = visibleFrame() + 1;

    if (frame > endFrame || frame < startFrame ) {
        frame = startFrame;
    }

    if (frame >= 0) {
        if (m_d->playbackState != STOPPED) {
            stop();
        }

        seek(frame);
    }
}

void KisAnimationPlayer::previousKeyframe()
{
    if (!m_d->canvas) return;

    KisNodeSP node = m_d->canvas->viewManager()->activeNode();
    if (!node) return;

    KisKeyframeChannel *keyframes =
        node->getKeyframeChannel(KisKeyframeChannel::Raster.id());
    if (!keyframes) return;

    int currentFrame = visibleFrame();

    int destinationTime = -1;
    if (!keyframes->keyframeAt(currentFrame)) {
        destinationTime = keyframes->activeKeyframeTime(currentFrame);
    } else {
        destinationTime = keyframes->previousKeyframeTime(currentFrame);
    }

    if (keyframes->keyframeAt(destinationTime)) {
        if (m_d->playbackState != STOPPED) {
            stop();
        }

        seek(destinationTime);
    }
}

void KisAnimationPlayer::nextKeyframe()
{
    if (!m_d->canvas) return;
    KisNodeSP node = m_d->canvas->viewManager()->activeNode();
    if (!node) return;

    KisKeyframeChannel *keyframes =
        node->getKeyframeChannel(KisKeyframeChannel::Raster.id());
    if (!keyframes) return;

    int currentTime = visibleFrame();

    int destinationTime = -1;
    if (keyframes->activeKeyframeAt(currentTime)) {
        destinationTime = keyframes->nextKeyframeTime(currentTime);
    }

    if (keyframes->keyframeAt(destinationTime)) {
        // Jump to next key...
        if (m_d->playbackState != STOPPED) {
            stop();
        }

        seek(destinationTime);
    } else {
        // Jump ahead by estimated timing...
        const int activeKeyTime = keyframes->activeKeyframeTime(currentTime);
        const int previousKeyTime = keyframes->previousKeyframeTime(activeKeyTime);

        if (previousKeyTime != -1) {
            if (m_d->playbackState != STOPPED) {
                stop();
            }

            const int timing = activeKeyTime - previousKeyTime;
            seek(currentTime + timing);
        }
    }
}

void KisAnimationPlayer::previousMatchingKeyframe()
{
    if (!m_d->canvas) return;

    KisNodeSP node = m_d->canvas->viewManager()->activeNode();
    if (!node) return;

    KisKeyframeChannel *keyframes =
        node->getKeyframeChannel(KisKeyframeChannel::Raster.id());
    if (!keyframes) return;

    int time = visibleFrame();

    KisKeyframeSP currentKeyframe = keyframes->keyframeAt(time);
    int destinationTime = keyframes->activeKeyframeTime(time);
    const int desiredColor = currentKeyframe ? currentKeyframe->colorLabel() : keyframes->keyframeAt(destinationTime)->colorLabel();
    previousKeyframeWithColor(desiredColor);
}

void KisAnimationPlayer::nextMatchingKeyframe()
{
    if (!m_d->canvas) return;

    KisNodeSP node = m_d->canvas->viewManager()->activeNode();
    if (!node) return;

    KisKeyframeChannel *keyframes =
        node->getKeyframeChannel(KisKeyframeChannel::Raster.id());
    if (!keyframes) return;

    int time = visibleFrame();

    if (!keyframes->activeKeyframeAt(time)) {
        return;
    }

    int destinationTime = keyframes->activeKeyframeTime(time);
    nextKeyframeWithColor(keyframes->keyframeAt(destinationTime)->colorLabel());
}

void KisAnimationPlayer::previousUnfilteredKeyframe()
{
    previousKeyframeWithColor(KisOnionSkinCompositor::instance()->colorLabelFilter());
}

void KisAnimationPlayer::nextUnfilteredKeyframe()
{
    nextKeyframeWithColor(KisOnionSkinCompositor::instance()->colorLabelFilter());
}

void KisAnimationPlayer::goToStartFrame()
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(m_d->canvas);
    KisImageAnimationInterface *animation = m_d->canvas->image()->animationInterface();
    const int startFrame = animation->playbackRange().start();
    seek(startFrame);
}


void KisAnimationPlayer::displayFrame(int frameToDisplay)
{
    KisAnimationFrameCacheSP frameCache = m_d->canvas->frameCache();

    //Q1: Does `uploadFrame` block program until frame is "updated"?
    //Q2: updateCanvas has a signal compressor. What does this do in relation to frame updating?
    if (frameCache
        && frameCache->shouldUploadNewFrame(frameToDisplay, m_d->visibleFrame)
        && frameCache->uploadFrame(frameToDisplay)) {
        m_d->canvas->updateCanvas();

    } else if (!frameCache && m_d->canvas->image()->animationInterface()->hasAnimation()) {

        if (m_d->canvas->image()->tryBarrierLock(true)) {
            m_d->canvas->image()->unlock();
            m_d->canvas->image()->animationInterface()->switchCurrentTimeAsync(frameToDisplay);
        }
    }

    m_d->visibleFrame = frameToDisplay;
    emit sigFrameChanged();
}

void KisAnimationPlayer::nextKeyframeWithColor(int color)
{
    QSet<int> validColors;
    validColors.insert(color);
    nextKeyframeWithColor(validColors);
}

void KisAnimationPlayer::nextKeyframeWithColor(const QSet<int> &validColors)
{
    if (!m_d->canvas) return;

    KisNodeSP node = m_d->canvas->viewManager()->activeNode();
    if (!node) return;

    KisKeyframeChannel *keyframes =
        node->getKeyframeChannel(KisKeyframeChannel::Raster.id());
    if (!keyframes) return;

    int time = visibleFrame();

    if (!keyframes->activeKeyframeAt(time)) {
        return;
    }

    int destinationTime = keyframes->activeKeyframeTime(time);
    while ( keyframes->keyframeAt(destinationTime) &&
            ((destinationTime == time) ||
            !validColors.contains(keyframes->keyframeAt(destinationTime)->colorLabel()))) {

        destinationTime = keyframes->nextKeyframeTime(destinationTime);
    }

    if (keyframes->keyframeAt(destinationTime)) {
        if (m_d->playbackState != STOPPED) {
            stop();
        }

        seek(destinationTime);
    }
}

void KisAnimationPlayer::previousKeyframeWithColor(int color)
{
    QSet<int> validColors;
    validColors.insert(color);
    previousKeyframeWithColor(validColors);
}

void KisAnimationPlayer::previousKeyframeWithColor(const QSet<int> &validColors)
{
    if (!m_d->canvas) return;

    KisNodeSP node = m_d->canvas->viewManager()->activeNode();
    if (!node) return;

    KisKeyframeChannel *keyframes =
        node->getKeyframeChannel(KisKeyframeChannel::Raster.id());
    if (!keyframes) return;

    int time = visibleFrame();

    int destinationTime = keyframes->activeKeyframeTime(time);
    while (keyframes->keyframeAt(destinationTime) &&
           ((destinationTime == time) ||
           !validColors.contains(keyframes->keyframeAt(destinationTime)->colorLabel()))) {

        destinationTime = keyframes->previousKeyframeTime(destinationTime);
    }

    if (keyframes->keyframeAt(destinationTime)) {
        if (m_d->playbackState != STOPPED) {
            stop();
        }

        seek(destinationTime);
    }
}

KisTimeSpan KisAnimationPlayer::activePlaybackRange()
{
    if (!m_d->canvas || !m_d->canvas->image()) {
        return KisTimeSpan::infinite(0);
    }

    const KisImageAnimationInterface *animation = m_d->canvas->image()->animationInterface();
    return animation->playbackRange();
}

qreal KisAnimationPlayer::playbackSpeed()
{
    return m_d->data->playbackSpeed();
}

int KisAnimationPlayer::visibleFrame()
{
    if (m_d->playbackState != PLAYING) {
        if (m_d->canvas && m_d->canvas->image()) {
            return m_d->canvas->image()->animationInterface()->currentUITime();
        } else {
            return -1;
        }
    } else {
        return m_d->visibleFrame;
    }
}

void KisAnimationPlayer::setPlaybackSpeedPercent(int value)
{
    const float normalizedSpeed = value / 100.0;
    setPlaybackSpeedNormalized(normalizedSpeed);
}

void KisAnimationPlayer::setPlaybackSpeedNormalized(double value)
{
    if (m_d->data->playbackSpeed() != value) {
        m_d->data->setPlaybackSpeed( value );
        emit sigPlaybackSpeedChanged(m_d->data->playbackSpeed());
    }
}

void KisAnimationPlayer::setPlaybackState(PlaybackState state)
{
    if (m_d->playbackState != state) {
        m_d->playbackState = state;
        emit sigPlaybackStateChanged(m_d->playbackState);
    }
}
