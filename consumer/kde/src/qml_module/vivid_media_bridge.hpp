#pragma once

#include "vivid_display.hpp"

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QVector>
#include <qqml.h>

#include <optional>
#include <cstddef>

struct pa_context;
struct pa_server_info;
struct pa_stream;
struct pa_threaded_mainloop;

class VividMediaBridge : public QObject {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(VividDisplay* display READ display WRITE setDisplay NOTIFY displayChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)

public:
    explicit VividMediaBridge(QObject* parent = nullptr);
    ~VividMediaBridge() override;

    VividDisplay* display() const { return m_display; }
    void setDisplay(VividDisplay* display);

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);

signals:
    void displayChanged();
    void enabledChanged();

private slots:
    void refreshMediaState();
    void onDisplayConnectionChanged();
    void onMprisNameOwnerChanged(const QString& name,
                                 const QString& oldOwner,
                                 const QString& newOwner);
    void onMprisPropertiesChanged(const QString& interfaceName,
                                  const QVariantMap& changedProperties,
                                  const QStringList& invalidatedProperties);

private:
    struct MprisSnapshot {
        QString name;
        QString playbackStatus;
        QString title;
        QString artist;
        QString albumTitle;
        QString albumArtist;
        QString subTitle;
        QString genres;
        QString contentType;
        QString artUrl;
        int score { 0 };
    };

    struct ThumbnailPayload {
        bool valid { false };
        QString path;
        QVector<double> primary { 0.0, 0.0, 0.0 };
        QVector<double> secondary { 1.0, 1.0, 1.0 };
        QVector<double> tertiary { 1.0, 1.0, 1.0 };
        QVector<double> text { 1.0, 1.0, 1.0 };
        QVector<double> highContrast { 1.0, 1.0, 1.0 };
    };

    struct SpectrumState {
        QVector<double> smoothed;
        QVector<double> lastDb;
        QVector<double> bandDb;
        QVector<double> normalized;
        QVector<double> horizontal;
        QVector<double> real;
        QVector<double> imag;
        QVector<double> magnitudes;
        QVector<double> normalizedMagnitudes;
        QVector<double> binDb;
    };

    void start();
    void stop();
    void updateActiveState();

    void startMprisMonitor();
    void stopMprisMonitor();
    QVector<MprisSnapshot> queryMprisSnapshots() const;
    MprisSnapshot queryMprisSnapshot(const QString& name) const;
    void scheduleMediaRefresh(int delayMs = 80);
    void sendMediaPayload(const MprisSnapshot* snapshot, const ThumbnailPayload* thumbnail);
    QJsonObject defaultMediaPayload() const;

    ThumbnailPayload loadThumbnail(const QString& artUrl);
    QString mediaCacheDir() const;
    QString localArtworkPath(const QString& artUrl) const;

    void startAudioCapture();
    void stopAudioCapture(bool emitSilence);
    void scheduleAudioRestart();
    void handlePulseAudioChunk(const QVector<float>& interleaved);
    void processAudioChunk(const QVector<float>& interleaved);
    QVector<double> processSpectrumFrame(const QVector<float>& pcm, SpectrumState& state);
    void resetSpectrumState();
    void initializeSpectrumTables();

    static void pulseContextStateCallback(pa_context* context, void* userdata);
    static void pulseServerInfoCallback(pa_context* context,
                                        const pa_server_info* info,
                                        void* userdata);
    static void pulseStreamStateCallback(pa_stream* stream, void* userdata);
    static void pulseStreamReadCallback(pa_stream* stream, size_t nbytes, void* userdata);

    QPointer<VividDisplay> m_display;
    bool m_enabled { true };
    bool m_running { false };

    QTimer m_mediaRefreshTimer;
    QTimer m_mediaPollTimer;
    QTimer m_audioRestartTimer;
    QHash<QString, ThumbnailPayload> m_thumbnailCache;
    QString m_lastMediaJson;
    std::optional<MprisSnapshot> m_lastSnapshot;

    pa_threaded_mainloop* m_paMainloop { nullptr };
    pa_context* m_paContext { nullptr };
    pa_stream* m_paStream { nullptr };
    bool m_audioShouldRun { false };
    quint64 m_audioFrameCount { 0 };
    quint64 m_lastAudioEmitUsec { 0 };

    QVector<float> m_leftSamples;
    QVector<float> m_rightSamples;
    SpectrumState m_leftSpectrum;
    SpectrumState m_rightSpectrum;
    QVector<double> m_audioFrame;

    QVector<double> m_bandEdgesHz;
    QVector<double> m_bandCentersHz;
    QVector<double> m_frequenciesHz;
    QVector<double> m_window;
    QVector<QPair<int, int>> m_bandBinRanges;
    double m_magnitudeReference { 1.0 };
};
