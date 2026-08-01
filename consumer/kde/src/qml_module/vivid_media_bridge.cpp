#include "vivid_media_bridge.hpp"

#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QUrl>

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

Q_LOGGING_CATEGORY(lcWallpaperMedia, "wallpaper.display.kde.media")

namespace
{
constexpr const char* MprisPrefix = "org.mpris.MediaPlayer2.";
constexpr const char* MprisPlayerPath = "/org/mpris/MediaPlayer2";
constexpr const char* MprisPlayerInterface = "org.mpris.MediaPlayer2.Player";
constexpr const char* DbusPropertiesInterface = "org.freedesktop.DBus.Properties";

constexpr int MediaPlaybackStopped = 0;
constexpr int MediaPlaybackPlaying = 1;
constexpr int MediaPlaybackPaused = 2;
constexpr int MediaPlaybackOther = 3;
constexpr int ThumbnailDecodeSize = 512;
constexpr int MediaPollIntervalMs = 1000;
constexpr int MediaDebounceDelayMs = 80;

constexpr int AudioBandsPerChannel = 64;
constexpr int AudioFrameLength = AudioBandsPerChannel * 2;
constexpr int AudioReferenceSampleRate = 44100;
constexpr int AudioUpdateIntervalMs = 33;
constexpr double AudioWindowParameter = 30.0;
constexpr double AudioStepParameter = 10.0;
constexpr double AudioInputBias = 127.0;
constexpr double AudioBandExponent = 0.25;
constexpr double AudioWeightCenter = 0.5009999871;
constexpr double AudioOutputGain = 1.0;
constexpr double Pi = 3.14159265358979323846;

double clamp01(double value)
{
    if (!std::isfinite(value))
        return 0.0;
    return std::clamp(value, 0.0, 1.0);
}

/**
 * Execute an unscaled forward or scaled inverse radix-2 FFT in place. The
 * Wallpaper Engine transform length is derived from the endpoint sample rate
 * and is generally not a power of two, so Bluestein uses this primitive for
 * its power-of-two convolution without changing the analyzed DFT length.
 */
void transformRadix2InPlace(QVector<double>& real, QVector<double>& imag, bool inverse)
{
    const int size = std::min(real.size(), imag.size());
    for (int index = 1, reversed = 0; index < size; index++) {
        int bit = size >> 1;
        while (reversed & bit) {
            reversed ^= bit;
            bit >>= 1;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(real[index], real[reversed]);
            std::swap(imag[index], imag[reversed]);
        }
    }

    for (int blockSize = 2; blockSize <= size; blockSize <<= 1) {
        const int halfSize = blockSize >> 1;
        const double angle = (inverse ? 2.0 : -2.0) * Pi / blockSize;
        const double phaseRealStep = std::cos(angle);
        const double phaseImagStep = std::sin(angle);
        for (int offset = 0; offset < size; offset += blockSize) {
            double phaseReal = 1.0;
            double phaseImag = 0.0;
            for (int index = 0; index < halfSize; index++) {
                const int evenIndex = offset + index;
                const int oddIndex = evenIndex + halfSize;
                const double oddReal =
                    real[oddIndex] * phaseReal - imag[oddIndex] * phaseImag;
                const double oddImag =
                    real[oddIndex] * phaseImag + imag[oddIndex] * phaseReal;
                real[oddIndex] = real[evenIndex] - oddReal;
                imag[oddIndex] = imag[evenIndex] - oddImag;
                real[evenIndex] += oddReal;
                imag[evenIndex] += oddImag;
                const double nextPhaseReal =
                    phaseReal * phaseRealStep - phaseImag * phaseImagStep;
                const double nextPhaseImag =
                    phaseReal * phaseImagStep + phaseImag * phaseRealStep;
                phaseReal = nextPhaseReal;
                phaseImag = nextPhaseImag;
            }
        }
    }

    if (inverse) {
        for (int index = 0; index < size; index++) {
            real[index] /= size;
            imag[index] /= size;
        }
    }
}

quint64 monotonicUsec()
{
    return static_cast<quint64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

QVariant unwrapDbusVariant(QVariant value)
{
    while (value.userType() == qMetaTypeId<QDBusVariant>())
        value = value.value<QDBusVariant>().variant();
    return value;
}

QString stringFromVariant(const QVariant& input)
{
    const QVariant value = unwrapDbusVariant(input);
    if (value.canConvert<QString>())
        return value.toString();
    return {};
}

QString joinedStringsFromVariant(const QVariant& input)
{
    const QVariant value = unwrapDbusVariant(input);
    if (value.canConvert<QStringList>())
        return value.toStringList().join(QStringLiteral(", "));
    if (value.canConvert<QVariantList>()) {
        QStringList values;
        for (const QVariant& entry : value.toList()) {
            const QString text = stringFromVariant(entry);
            if (!text.isEmpty())
                values.append(text);
        }
        return values.join(QStringLiteral(", "));
    }
    return stringFromVariant(value);
}

QJsonArray colorArray(const QVector<double>& color)
{
    QJsonArray array;
    array.append(clamp01(color.value(0)));
    array.append(clamp01(color.value(1)));
    array.append(clamp01(color.value(2)));
    return array;
}

double luminance(const QVector<double>& color)
{
    return 0.2126 * color.value(0) + 0.7152 * color.value(1) + 0.0722 * color.value(2);
}

double colorDistanceSquared(const QVector<double>& left, const QVector<double>& right)
{
    const double dr = left.value(0) - right.value(0);
    const double dg = left.value(1) - right.value(1);
    const double db = left.value(2) - right.value(2);
    return dr * dr + dg * dg + db * db;
}

int mapPlaybackState(const QString& playbackStatus)
{
    if (playbackStatus == QLatin1String("Playing"))
        return MediaPlaybackPlaying;
    if (playbackStatus == QLatin1String("Paused"))
        return MediaPlaybackPaused;
    if (playbackStatus == QLatin1String("Stopped") || playbackStatus.isEmpty())
        return MediaPlaybackStopped;
    return MediaPlaybackOther;
}

bool isMprisPlayerName(const QString& name)
{
    return name.startsWith(QLatin1String(MprisPrefix));
}

QString mediaContentType(const QString& title,
                         const QString& artist,
                         const QString& albumArtist)
{
    return title.isEmpty() && artist.isEmpty() && albumArtist.isEmpty()
        ? QString()
        : QStringLiteral("music");
}

} // namespace

VividMediaBridge::VividMediaBridge(QObject* parent)
    : QObject(parent)
{
    m_mediaRefreshTimer.setSingleShot(true);
    m_mediaRefreshTimer.setInterval(MediaDebounceDelayMs);
    connect(&m_mediaRefreshTimer, &QTimer::timeout, this, &VividMediaBridge::refreshMediaState);

    m_mediaPollTimer.setInterval(MediaPollIntervalMs);
    connect(&m_mediaPollTimer, &QTimer::timeout, this, &VividMediaBridge::refreshMediaState);

    m_audioRestartTimer.setSingleShot(true);
    m_audioRestartTimer.setInterval(1000);
    connect(&m_audioRestartTimer, &QTimer::timeout, this, &VividMediaBridge::startAudioCapture);

    /*
     * PulseAudio fragment boundaries are independent from Wallpaper Engine's
     * analysis cadence. Capture callbacks append continuous PCM into the ring,
     * while this precise timer samples the newest complete analysis window every
     * 33 ms regardless of how the server grouped the source fragments.
     */
    m_audioProcessTimer.setTimerType(Qt::PreciseTimer);
    m_audioProcessTimer.setInterval(AudioUpdateIntervalMs);
    connect(&m_audioProcessTimer, &QTimer::timeout,
            this, &VividMediaBridge::processNextAudioFrame);

    m_audioFrame.fill(0.0, AudioFrameLength);
}

VividMediaBridge::~VividMediaBridge()
{
    stop();
}

void VividMediaBridge::setDisplay(VividDisplay* display)
{
    if (m_display == display)
        return;

    if (m_display)
        disconnect(m_display, nullptr, this, nullptr);

    m_display = display;
    if (m_display) {
        connect(m_display, &VividDisplay::connStateChanged,
                this, &VividMediaBridge::onDisplayConnectionChanged);
    }

    emit displayChanged();
    updateActiveState();
}

void VividMediaBridge::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    emit enabledChanged();
    updateActiveState();
}

void VividMediaBridge::start()
{
    if (m_running)
        return;

    m_running = true;
    startMprisMonitor();
    scheduleMediaRefresh(0);
    if (m_display && m_display->connState() == VividDisplay::Connected)
        startAudioCapture();
}

void VividMediaBridge::stop()
{
    if (!m_running && !m_paMainloop)
        return;

    stopAudioCapture();
    stopMprisMonitor();
    m_mediaRefreshTimer.stop();
    m_mediaPollTimer.stop();
    m_running = false;
}

void VividMediaBridge::updateActiveState()
{
    if (m_enabled && m_display)
        start();
    else
        stop();
}

void VividMediaBridge::onDisplayConnectionChanged()
{
    if (!m_running || !m_display)
        return;

    if (m_display->connState() == VividDisplay::Connected) {
        m_lastMediaJson.clear();
        scheduleMediaRefresh(0);
        startAudioCapture();
    } else {
        stopAudioCapture();
        stopMprisMonitor();
    }
}

void VividMediaBridge::startMprisMonitor()
{
    if (m_mprisMonitoring)
        return;

    auto connection = QDBusConnection::sessionBus();
    connection.connect(QString(),
                       QStringLiteral("/org/freedesktop/DBus"),
                       QStringLiteral("org.freedesktop.DBus"),
                       QStringLiteral("NameOwnerChanged"),
                       this,
                       SLOT(onMprisNameOwnerChanged(QString,QString,QString)));
    connection.connect(QString(),
                       QString::fromLatin1(MprisPlayerPath),
                       QString::fromLatin1(DbusPropertiesInterface),
                       QStringLiteral("PropertiesChanged"),
                       this,
                       SLOT(onMprisPropertiesChanged(QString,QVariantMap,QStringList)));
    m_mprisMonitoring = true;
    m_mediaPollTimer.start();
}

void VividMediaBridge::stopMprisMonitor()
{
    if (!m_mprisMonitoring)
        return;

    auto connection = QDBusConnection::sessionBus();
    connection.disconnect(QString(),
                          QStringLiteral("/org/freedesktop/DBus"),
                          QStringLiteral("org.freedesktop.DBus"),
                          QStringLiteral("NameOwnerChanged"),
                          this,
                          SLOT(onMprisNameOwnerChanged(QString,QString,QString)));
    connection.disconnect(QString(),
                          QString::fromLatin1(MprisPlayerPath),
                          QString::fromLatin1(DbusPropertiesInterface),
                          QStringLiteral("PropertiesChanged"),
                          this,
                          SLOT(onMprisPropertiesChanged(QString,QVariantMap,QStringList)));
    m_mediaPollTimer.stop();
    m_mprisMonitoring = false;
}

void VividMediaBridge::onMprisNameOwnerChanged(const QString& name,
                                                   const QString&,
                                                   const QString&)
{
    if (isMprisPlayerName(name))
        scheduleMediaRefresh();
}

void VividMediaBridge::onMprisPropertiesChanged(const QString& interfaceName,
                                                    const QVariantMap& changedProperties,
                                                    const QStringList& invalidatedProperties)
{
    if (interfaceName == QLatin1String(MprisPlayerInterface) &&
        (changedProperties.contains(QStringLiteral("PlaybackStatus")) ||
         changedProperties.contains(QStringLiteral("Metadata")) ||
         invalidatedProperties.contains(QStringLiteral("PlaybackStatus")) ||
         invalidatedProperties.contains(QStringLiteral("Metadata")))) {
        scheduleMediaRefresh();
    }
}

void VividMediaBridge::scheduleMediaRefresh(int delayMs)
{
    if (!m_running)
        return;
    m_mediaRefreshTimer.start(std::max(0, delayMs));
}

QVector<VividMediaBridge::MprisSnapshot> VividMediaBridge::queryMprisSnapshots() const
{
    QVector<MprisSnapshot> snapshots;
    auto* interface = QDBusConnection::sessionBus().interface();
    if (!interface)
        return snapshots;

    const QDBusReply<QStringList> namesReply = interface->registeredServiceNames();
    if (!namesReply.isValid())
        return snapshots;

    for (const QString& name : namesReply.value()) {
        if (!isMprisPlayerName(name))
            continue;
        const MprisSnapshot snapshot = queryMprisSnapshot(name);
        if (snapshot.score > 0)
            snapshots.append(snapshot);
    }

    std::sort(snapshots.begin(), snapshots.end(), [](const MprisSnapshot& left,
                                                     const MprisSnapshot& right) {
        if (left.score != right.score)
            return left.score > right.score;
        return left.name < right.name;
    });
    return snapshots;
}

VividMediaBridge::MprisSnapshot VividMediaBridge::queryMprisSnapshot(const QString& name) const
{
    MprisSnapshot snapshot;
    snapshot.name = name;

    QDBusInterface properties(name,
                              QString::fromLatin1(MprisPlayerPath),
                              QString::fromLatin1(DbusPropertiesInterface),
                              QDBusConnection::sessionBus());
    const QDBusReply<QVariantMap> reply =
        properties.call(QStringLiteral("GetAll"), QString::fromLatin1(MprisPlayerInterface));
    if (!reply.isValid())
        return snapshot;

    const QVariantMap values = reply.value();
    snapshot.playbackStatus = stringFromVariant(values.value(QStringLiteral("PlaybackStatus")));

    const QVariant metadataVariant = unwrapDbusVariant(values.value(QStringLiteral("Metadata")));
    const QVariantMap metadata = qdbus_cast<QVariantMap>(metadataVariant);
    snapshot.title = stringFromVariant(metadata.value(QStringLiteral("xesam:title")));
    snapshot.artist = joinedStringsFromVariant(metadata.value(QStringLiteral("xesam:artist")));
    snapshot.albumTitle = stringFromVariant(metadata.value(QStringLiteral("xesam:album")));
    snapshot.albumArtist =
        joinedStringsFromVariant(metadata.value(QStringLiteral("xesam:albumArtist")));
    snapshot.subTitle = stringFromVariant(metadata.value(QStringLiteral("xesam:comment")));
    snapshot.genres = joinedStringsFromVariant(metadata.value(QStringLiteral("xesam:genre")));
    snapshot.artUrl = stringFromVariant(metadata.value(QStringLiteral("mpris:artUrl")));
    snapshot.contentType =
        mediaContentType(snapshot.title, snapshot.artist, snapshot.albumArtist);
    snapshot.score = snapshot.playbackStatus == QLatin1String("Playing") ? 3
        : snapshot.playbackStatus == QLatin1String("Paused") ? 2
        : (!snapshot.title.isEmpty() || !snapshot.artist.isEmpty() || !snapshot.artUrl.isEmpty()) ? 1
        : 0;
    return snapshot;
}

void VividMediaBridge::refreshMediaState()
{
    const QVector<MprisSnapshot> snapshots = queryMprisSnapshots();
    updateDisplayMprisPolicyFacts(snapshots);
    if (snapshots.isEmpty()) {
        m_lastSnapshot.reset();
        sendMediaPayload(nullptr, nullptr);
        return;
    }

    m_lastSnapshot = snapshots.first();
    ThumbnailPayload thumbnail = loadThumbnail(m_lastSnapshot->artUrl);
    sendMediaPayload(&m_lastSnapshot.value(), thumbnail.valid ? &thumbnail : nullptr);
}

void VividMediaBridge::updateDisplayMprisPolicyFacts(const QVector<MprisSnapshot>& snapshots)
{
    QJsonArray players;
    for (const MprisSnapshot& snapshot : snapshots) {
        if (snapshot.name.isEmpty())
            continue;

        players.append(QJsonObject {
            { QStringLiteral("name"), snapshot.name },
            { QStringLiteral("playbackStatus"), snapshot.playbackStatus },
        });
    }

    /*
     * Title/artwork updates are consumed by wallpapers, while the producer's
     * playback-on-audio policy reads mprisPlaying/mprisPlayers from
     * window-state facts. Publish both from the same MPRIS snapshot so KDE does
     * not let the media UI and policy engine drift apart.
     */
    if (m_display)
        m_display->setMprisPlaybackFacts(players);
}

QJsonObject VividMediaBridge::defaultMediaPayload() const
{
    return QJsonObject {
        { QStringLiteral("title"), QString() },
        { QStringLiteral("artist"), QString() },
        { QStringLiteral("albumTitle"), QString() },
        { QStringLiteral("albumArtist"), QString() },
        { QStringLiteral("subTitle"), QString() },
        { QStringLiteral("genres"), QString() },
        { QStringLiteral("contentType"), QString() },
        { QStringLiteral("hasThumbnail"), false },
        { QStringLiteral("playbackState"), MediaPlaybackStopped },
        { QStringLiteral("primaryColor"), colorArray({ 0.0, 0.0, 0.0 }) },
        { QStringLiteral("secondaryColor"), colorArray({ 1.0, 1.0, 1.0 }) },
        { QStringLiteral("tertiaryColor"), colorArray({ 1.0, 1.0, 1.0 }) },
        { QStringLiteral("textColor"), colorArray({ 1.0, 1.0, 1.0 }) },
        { QStringLiteral("highContrastColor"), colorArray({ 1.0, 1.0, 1.0 }) },
        { QStringLiteral("thumbnailPath"), QString() },
    };
}

void VividMediaBridge::sendMediaPayload(const MprisSnapshot* snapshot,
                                            const ThumbnailPayload* thumbnail)
{
    QJsonObject payload = defaultMediaPayload();
    if (snapshot) {
        payload[QStringLiteral("title")] = snapshot->title;
        payload[QStringLiteral("artist")] = snapshot->artist;
        payload[QStringLiteral("albumTitle")] = snapshot->albumTitle;
        payload[QStringLiteral("albumArtist")] = snapshot->albumArtist;
        payload[QStringLiteral("subTitle")] = snapshot->subTitle;
        payload[QStringLiteral("genres")] = snapshot->genres;
        payload[QStringLiteral("contentType")] = snapshot->contentType;
        payload[QStringLiteral("playbackState")] = mapPlaybackState(snapshot->playbackStatus);
    }

    if (thumbnail && thumbnail->valid) {
        payload[QStringLiteral("hasThumbnail")] = true;
        payload[QStringLiteral("thumbnailPath")] = thumbnail->path;
        payload[QStringLiteral("primaryColor")] = colorArray(thumbnail->primary);
        payload[QStringLiteral("secondaryColor")] = colorArray(thumbnail->secondary);
        payload[QStringLiteral("tertiaryColor")] = colorArray(thumbnail->tertiary);
        payload[QStringLiteral("textColor")] = colorArray(thumbnail->text);
        payload[QStringLiteral("highContrastColor")] = colorArray(thumbnail->highContrast);
    }

    const QString nextJson = QString::fromUtf8(
        QJsonDocument(payload).toJson(QJsonDocument::Compact));
    if (nextJson == m_lastMediaJson)
        return;

    if (m_display && m_display->sendMediaState(payload)) {
        m_lastMediaJson = nextJson;
        qCDebug(lcWallpaperMedia, "media state sent title=%s artist=%s thumbnail=%s",
                qPrintable(payload.value(QStringLiteral("title")).toString()),
                qPrintable(payload.value(QStringLiteral("artist")).toString()),
                payload.value(QStringLiteral("hasThumbnail")).toBool() ? "true" : "false");
    }
}

QString VividMediaBridge::mediaCacheDir() const
{
    QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDir.isEmpty())
        runtimeDir = QDir::tempPath();
    return QDir(runtimeDir).filePath(QStringLiteral("vivid/scene-media-cache"));
}

QString VividMediaBridge::localArtworkPath(const QString& artUrl) const
{
    if (artUrl.isEmpty())
        return {};

    const QUrl url(artUrl);
    if (!url.isValid() || url.scheme().isEmpty())
        return artUrl;
    if (url.isLocalFile())
        return url.toLocalFile();
    return {};
}

VividMediaBridge::ThumbnailPayload VividMediaBridge::loadThumbnail(const QString& artUrl)
{
    if (artUrl.isEmpty())
        return {};
    if (m_thumbnailCache.contains(artUrl))
        return m_thumbnailCache.value(artUrl);

    QImage image;
    const QString localPath = localArtworkPath(artUrl);
    if (!localPath.isEmpty()) {
        image.load(localPath);
    } else {
        qCDebug(lcWallpaperMedia, "ignoring non-local MPRIS artwork url: %s",
                qPrintable(artUrl));
        return {};
    }

    if (image.isNull())
        return {};

    QDir().mkpath(mediaCacheDir());
    const QByteArray hash = QCryptographicHash::hash(artUrl.toUtf8(),
                                                     QCryptographicHash::Sha256).toHex();
    const QString path = QDir(mediaCacheDir()).filePath(QString::fromLatin1(hash) + QStringLiteral(".png"));
    const QImage thumbnail =
        image.scaled(ThumbnailDecodeSize,
                     ThumbnailDecodeSize,
                     Qt::KeepAspectRatio,
                     Qt::SmoothTransformation);
    thumbnail.save(path, "PNG");

    struct Bucket {
        int count { 0 };
        double r { 0.0 };
        double g { 0.0 };
        double b { 0.0 };
    };
    QHash<int, Bucket> buckets;
    const QImage sampled =
        image.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < sampled.height(); y++) {
        const auto* line = reinterpret_cast<const QRgb*>(sampled.constScanLine(y));
        for (int x = 0; x < sampled.width(); x++) {
            const QColor color = QColor::fromRgba(line[x]);
            if (color.alpha() < 16)
                continue;
            const int key = ((color.red() >> 3) << 10) |
                ((color.green() >> 3) << 5) |
                (color.blue() >> 3);
            Bucket& bucket = buckets[key];
            bucket.count++;
            bucket.r += color.redF();
            bucket.g += color.greenF();
            bucket.b += color.blueF();
        }
    }

    QVector<QPair<double, QVector<double>>> ranked;
    int maxCount = 1;
    for (const Bucket& bucket : buckets)
        maxCount = std::max(maxCount, bucket.count);
    for (const Bucket& bucket : buckets) {
        if (bucket.count <= 0)
            continue;
        QVector<double> color {
            bucket.r / bucket.count,
            bucket.g / bucket.count,
            bucket.b / bucket.count,
        };
        const double population = static_cast<double>(bucket.count) / maxCount;
        const double rank = population * 0.35 + (1.0 - std::abs(luminance(color) - 0.52)) * 1.15;
        ranked.append({ rank, color });
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        return left.first > right.first;
    });

    ThumbnailPayload payload;
    payload.valid = true;
    payload.path = path;
    payload.primary = ranked.isEmpty() ? QVector<double> { 0.0, 0.0, 0.0 } : ranked.first().second;
    payload.text = luminance(payload.primary) > 0.55
        ? QVector<double> { 0.05, 0.05, 0.05 }
        : QVector<double> { 0.95, 0.95, 0.95 };
    payload.highContrast = payload.text;
    payload.secondary = {
        clamp01(payload.primary.value(0) * 0.7 + payload.text.value(0) * 0.3),
        clamp01(payload.primary.value(1) * 0.7 + payload.text.value(1) * 0.3),
        clamp01(payload.primary.value(2) * 0.7 + payload.text.value(2) * 0.3),
    };
    payload.tertiary = payload.secondary;
    for (const auto& candidate : ranked) {
        if (colorDistanceSquared(payload.primary, candidate.second) >= 0.045) {
            payload.secondary = candidate.second;
            break;
        }
    }
    for (const auto& candidate : ranked) {
        if (colorDistanceSquared(payload.primary, candidate.second) >= 0.045 &&
            colorDistanceSquared(payload.secondary, candidate.second) >= 0.045) {
            payload.tertiary = candidate.second;
            break;
        }
    }

    m_thumbnailCache.insert(artUrl, payload);
    return payload;
}

void VividMediaBridge::startAudioCapture()
{
    if (m_audioShouldRun || !m_display ||
        m_display->connState() != VividDisplay::Connected)
        return;

    m_audioShouldRun = true;
    resetSpectrumState();

    m_paMainloop = pa_threaded_mainloop_new();
    if (!m_paMainloop) {
        qCWarning(lcWallpaperMedia, "PulseAudio capture failed: pa_threaded_mainloop_new returned null");
        scheduleAudioRestart();
        return;
    }

    pa_mainloop_api* api = pa_threaded_mainloop_get_api(m_paMainloop);
    m_paContext = pa_context_new(api, "VividWallpaperVisualizer");
    if (!m_paContext) {
        qCWarning(lcWallpaperMedia, "PulseAudio capture failed: pa_context_new returned null");
        scheduleAudioRestart();
        return;
    }

    pa_context_set_state_callback(m_paContext, &VividMediaBridge::pulseContextStateCallback, this);
    if (pa_context_connect(m_paContext, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        qCWarning(lcWallpaperMedia, "PulseAudio capture failed: unable to connect context");
        scheduleAudioRestart();
        return;
    }
    if (pa_threaded_mainloop_start(m_paMainloop) < 0) {
        qCWarning(lcWallpaperMedia, "PulseAudio capture failed: unable to start mainloop");
        scheduleAudioRestart();
        return;
    }
    m_paMainloopStarted = true;
}

void VividMediaBridge::stopAudioCapture()
{
    m_audioRestartTimer.stop();
    m_audioProcessTimer.stop();
    m_audioShouldRun = false;
    m_leftPcmRing.fill(0.0f);
    m_rightPcmRing.fill(0.0f);
    m_pcmWriteIndex = 0;
    m_pcmFramesAvailable = 0;
    m_pcmFramesWritten = 0;
    m_lastAnalyzedFramesWritten = 0;

    /*
     * pa_threaded_mainloop_start() may fail after the context has already been
     * created. In that state no worker thread exists, so locking or stopping the
     * mainloop violates libpulse's lifecycle contract. Track the successful
     * start explicitly and only synchronize with a thread that actually exists.
     */
    if (m_paMainloop && m_paMainloopStarted)
        pa_threaded_mainloop_lock(m_paMainloop);
    if (m_paStream) {
        pa_stream_set_read_callback(m_paStream, nullptr, nullptr);
        pa_stream_set_state_callback(m_paStream, nullptr, nullptr);
        pa_stream_disconnect(m_paStream);
        pa_stream_unref(m_paStream);
        m_paStream = nullptr;
    }
    if (m_paContext) {
        pa_context_set_state_callback(m_paContext, nullptr, nullptr);
        pa_context_disconnect(m_paContext);
        pa_context_unref(m_paContext);
        m_paContext = nullptr;
    }
    if (m_paMainloop && m_paMainloopStarted)
        pa_threaded_mainloop_unlock(m_paMainloop);
    if (m_paMainloop) {
        if (m_paMainloopStarted)
            pa_threaded_mainloop_stop(m_paMainloop);
        pa_threaded_mainloop_free(m_paMainloop);
        m_paMainloop = nullptr;
    }
    m_paMainloopStarted = false;
}

void VividMediaBridge::scheduleAudioRestart()
{
    QMetaObject::invokeMethod(this, [this]() {
        if (!m_audioShouldRun)
            return;
        stopAudioCapture();
        m_audioShouldRun = false;
        m_audioRestartTimer.start();
    }, Qt::QueuedConnection);
}

void VividMediaBridge::pulseContextStateCallback(pa_context* context, void* userdata)
{
    auto* self = static_cast<VividMediaBridge*>(userdata);
    switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY: {
        pa_operation* operation = pa_context_get_server_info(
            context,
            &VividMediaBridge::pulseServerInfoCallback,
            self);
        if (operation)
            pa_operation_unref(operation);
        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        self->scheduleAudioRestart();
        break;
    default:
        break;
    }
}

void VividMediaBridge::pulseServerInfoCallback(pa_context* context,
                                                   const pa_server_info* info,
                                                   void* userdata)
{
    auto* self = static_cast<VividMediaBridge*>(userdata);
    if (!info || !info->default_sink_name) {
        self->scheduleAudioRestart();
        return;
    }

    pa_operation* operation = pa_context_get_sink_info_by_name(
        context,
        info->default_sink_name,
        &VividMediaBridge::pulseSinkInfoCallback,
        self);
    if (operation)
        pa_operation_unref(operation);
    else
        self->scheduleAudioRestart();
}

void VividMediaBridge::pulseSinkInfoCallback(pa_context* context,
                                              const pa_sink_info* info,
                                              int eol,
                                              void* userdata)
{
    auto* self = static_cast<VividMediaBridge*>(userdata);
    if (eol > 0)
        return;
    if (!info || !info->monitor_source_name || info->sample_spec.rate == 0) {
        self->scheduleAudioRestart();
        return;
    }

    const QByteArray monitorName(info->monitor_source_name);
    const int sampleRate = static_cast<int>(info->sample_spec.rate);

    /*
     * Pulse callbacks run on pa_threaded_mainloop's worker thread, while the
     * timer and PCM ring live on the QObject thread. Queue configuration before
     * stream creation; subsequent stream-state and read callbacks enqueue their
     * work from the same Pulse thread, preserving this event order without a
     * cross-thread blocking call during shutdown.
     */
    QMetaObject::invokeMethod(self, [self, sampleRate]() {
        if (!self->configureSpectrumTransform(sampleRate)) {
            qCWarning(lcWallpaperMedia,
                      "audio spectrum configuration failed sampleRate=%d",
                      sampleRate);
            self->scheduleAudioRestart();
        }
    }, Qt::QueuedConnection);

    pa_sample_spec spec {};
    spec.format = PA_SAMPLE_FLOAT32LE;
    spec.rate = static_cast<uint32_t>(sampleRate);
    spec.channels = 2;
    if (!pa_sample_spec_valid(&spec)) {
        self->scheduleAudioRestart();
        return;
    }

    pa_stream* stream = pa_stream_new(context, "VividWallpaperVisualizer", &spec, nullptr);
    if (!stream) {
        self->scheduleAudioRestart();
        return;
    }

    self->m_paStream = stream;
    pa_stream_set_state_callback(stream, &VividMediaBridge::pulseStreamStateCallback, self);
    pa_stream_set_read_callback(stream, &VividMediaBridge::pulseStreamReadCallback, self);

    pa_buffer_attr attr {};
    attr.maxlength = static_cast<uint32_t>(-1);
    const uint64_t captureFragmentFrames =
        static_cast<uint64_t>(sampleRate) * AudioUpdateIntervalMs / 1000;
    attr.fragsize = static_cast<uint32_t>(
        captureFragmentFrames * sizeof(float) * spec.channels
    );
    if (pa_stream_connect_record(stream,
                                  monitorName.constData(),
                                  &attr,
                                  PA_STREAM_ADJUST_LATENCY) < 0) {
        self->scheduleAudioRestart();
    }
}

void VividMediaBridge::pulseStreamStateCallback(pa_stream* stream, void* userdata)
{
    auto* self = static_cast<VividMediaBridge*>(userdata);
    switch (pa_stream_get_state(stream)) {
    case PA_STREAM_READY: {
        QMetaObject::invokeMethod(self, [self]() {
            if (!self->m_audioShouldRun || !self->m_spectrumConfiguration.valid())
                return;
            self->m_audioProcessTimer.start();
        }, Qt::QueuedConnection);
        break;
    }
    case PA_STREAM_FAILED:
    case PA_STREAM_TERMINATED:
        self->scheduleAudioRestart();
        break;
    default:
        break;
    }
}

void VividMediaBridge::pulseStreamReadCallback(pa_stream* stream,
                                                   size_t,
                                                   void* userdata)
{
    auto* self = static_cast<VividMediaBridge*>(userdata);
    const void* data = nullptr;
    size_t bytes = 0;
    int peekResult = 0;
    while ((peekResult = pa_stream_peek(stream, &data, &bytes)) >= 0 && bytes > 0) {
        constexpr size_t bytesPerFrame = sizeof(float) * 2;
        const size_t frameCount = bytes / bytesPerFrame;
        const qsizetype sampleCount = static_cast<qsizetype>(frameCount * 2);
        if (bytes % bytesPerFrame != 0) {
            qCWarning(lcWallpaperMedia,
                      "PulseAudio capture returned a non-frame-aligned fragment bytes=%zu",
                      bytes);
        }

        if (sampleCount > 0) {
            /*
             * A null data pointer with a non-zero byte count is a PulseAudio
             * hole, not an empty callback. Materialize it as zero PCM so the
             * continuous capture timeline advances and the previous spectrum
             * does not remain visible after the sink becomes silent.
             */
            QVector<float> chunk(sampleCount, 0.0f);
            if (data) {
                const auto* samples = static_cast<const float*>(data);
                std::copy(samples, samples + sampleCount, chunk.begin());
            }
            QMetaObject::invokeMethod(self, [self, chunk = std::move(chunk)]() mutable {
                self->handlePulseAudioChunk(std::move(chunk));
            }, Qt::QueuedConnection);
        }
        if (pa_stream_drop(stream) < 0) {
            pa_context* context = pa_stream_get_context(stream);
            qCWarning(lcWallpaperMedia,
                      "PulseAudio capture failed while dropping fragment: %s",
                      context ? pa_strerror(pa_context_errno(context)) : "unknown error");
            self->scheduleAudioRestart();
            return;
        }
        data = nullptr;
        bytes = 0;
    }

    if (peekResult < 0) {
        pa_context* context = pa_stream_get_context(stream);
        qCWarning(lcWallpaperMedia,
                  "PulseAudio capture failed while peeking fragment: %s",
                  context ? pa_strerror(pa_context_errno(context)) : "unknown error");
        self->scheduleAudioRestart();
    }
}

void VividMediaBridge::handlePulseAudioChunk(QVector<float> interleaved)
{
    if (!m_audioShouldRun || !m_display ||
        m_display->connState() != VividDisplay::Connected)
        return;

    appendAudioChunk(interleaved);
}

void VividMediaBridge::processNextAudioFrame()
{
    if (!m_audioShouldRun || !m_display ||
        m_display->connState() != VividDisplay::Connected ||
        !m_spectrumConfiguration.valid() ||
        m_pcmFramesWritten == m_lastAnalyzedFramesWritten)
        return;

    const quint64 snapshotFramesWritten = m_pcmFramesWritten;
    const int populatedFrames = copyLatestAudioWindow();
    if (populatedFrames <= 0)
        return;

    const QVector<double> leftValues = processSpectrumFrame(m_leftSamples, m_leftSpectrum);
    const QVector<double> rightValues = processSpectrumFrame(m_rightSamples, m_rightSpectrum);
    for (int i = 0; i < AudioBandsPerChannel; i++) {
        m_audioFrame[i] = leftValues.value(i);
        m_audioFrame[i + AudioBandsPerChannel] = rightValues.value(i);
    }

    m_display->sendAudioSamples(m_audioFrame, monotonicUsec());
    m_lastAnalyzedFramesWritten = snapshotFramesWritten;
}

void VividMediaBridge::appendAudioChunk(const QVector<float>& interleaved)
{
    if (!m_spectrumConfiguration.valid() || m_leftPcmRing.isEmpty() ||
        m_leftPcmRing.size() != m_rightPcmRing.size())
        return;

    const qsizetype frameCount = interleaved.size() / 2;
    const qsizetype capacity = m_leftPcmRing.size();
    for (qsizetype frame = 0; frame < frameCount; frame++) {
        const qsizetype source = frame * 2;
        const float left = interleaved.at(source);
        m_leftPcmRing[m_pcmWriteIndex] = left;
        m_rightPcmRing[m_pcmWriteIndex] = interleaved.value(source + 1, left);
        m_pcmWriteIndex = (m_pcmWriteIndex + 1) % capacity;
    }

    m_pcmFramesAvailable = std::min(capacity, m_pcmFramesAvailable + frameCount);
    m_pcmFramesWritten += static_cast<quint64>(frameCount);
}

int VividMediaBridge::copyLatestAudioWindow()
{
    if (!m_spectrumConfiguration.valid() || m_leftPcmRing.isEmpty())
        return 0;

    m_leftSamples.fill(0.0f);
    m_rightSamples.fill(0.0f);
    const qsizetype targetCount = m_spectrumConfiguration.pcmSampleCount;
    const qsizetype copyCount = std::min(targetCount, m_pcmFramesAvailable);
    const qsizetype targetOffset = targetCount - copyCount;
    const qsizetype capacity = m_leftPcmRing.size();
    const qsizetype sourceStart =
        (m_pcmWriteIndex - copyCount + capacity) % capacity;
    for (qsizetype frame = 0; frame < copyCount; frame++) {
        const qsizetype source = (sourceStart + frame) % capacity;
        m_leftSamples[targetOffset + frame] = m_leftPcmRing[source];
        m_rightSamples[targetOffset + frame] = m_rightPcmRing[source];
    }
    return static_cast<int>(copyCount);
}

bool VividMediaBridge::configureSpectrumTransform(int sampleRate)
{
    if (sampleRate <= 0)
        return false;

    SpectrumConfiguration configuration;
    configuration.sampleRate = sampleRate;
    configuration.fftSize = static_cast<int>(
        std::max(static_cast<double>(sampleRate) / AudioReferenceSampleRate, 1.0) *
        AudioBandsPerChannel * AudioWindowParameter
    );
    configuration.analysisBinCount = static_cast<int>(
        AudioBandsPerChannel * AudioStepParameter
    );
    configuration.pcmSampleCount = static_cast<int>(
        configuration.fftSize -
        (AudioStepParameter / AudioWindowParameter) * configuration.fftSize
    );
    configuration.convolutionSize = 1;
    while (configuration.convolutionSize < configuration.fftSize * 2 - 1)
        configuration.convolutionSize <<= 1;
    if (!configuration.valid())
        return false;

    const bool planMatches =
        m_spectrumConfiguration.sampleRate == configuration.sampleRate &&
        m_spectrumConfiguration.fftSize == configuration.fftSize &&
        m_spectrumConfiguration.pcmSampleCount == configuration.pcmSampleCount &&
        m_spectrumConfiguration.analysisBinCount == configuration.analysisBinCount &&
        m_spectrumConfiguration.convolutionSize == configuration.convolutionSize &&
        m_audioChirpReal.size() == configuration.fftSize &&
        m_audioKernelReal.size() == configuration.convolutionSize;
    m_spectrumConfiguration = configuration;

    const int captureFramesPerTick = static_cast<int>(
        (static_cast<int64_t>(sampleRate) * AudioUpdateIntervalMs + 999) / 1000
    );
    const int ringCapacity =
        configuration.pcmSampleCount + captureFramesPerTick * 2;
    m_leftPcmRing.fill(0.0f, ringCapacity);
    m_rightPcmRing.fill(0.0f, ringCapacity);
    m_leftSamples.fill(0.0f, configuration.pcmSampleCount);
    m_rightSamples.fill(0.0f, configuration.pcmSampleCount);

    if (planMatches) {
        resetSpectrumState();
        return true;
    }

    m_audioChirpReal.fill(0.0, configuration.fftSize);
    m_audioChirpImag.fill(0.0, configuration.fftSize);
    m_audioKernelReal.fill(0.0, configuration.convolutionSize);
    m_audioKernelImag.fill(0.0, configuration.convolutionSize);

    /*
     * Bluestein preserves Wallpaper Engine's sample-rate-derived, potentially
     * non-power-of-two transform length. The immutable convolution kernel is
     * rebuilt only when the PulseAudio server mix rate changes.
     */
    for (int index = 0; index < configuration.fftSize; index++) {
        const qint64 squared = static_cast<qint64>(index) * index;
        const double angle = Pi * (squared % (configuration.fftSize * 2)) /
            configuration.fftSize;
        const double cosine = std::cos(angle);
        const double sine = std::sin(angle);
        m_audioChirpReal[index] = cosine;
        m_audioChirpImag[index] = -sine;
        m_audioKernelReal[index] = cosine;
        m_audioKernelImag[index] = sine;
        if (index > 0) {
            m_audioKernelReal[configuration.convolutionSize - index] = cosine;
            m_audioKernelImag[configuration.convolutionSize - index] = sine;
        }
    }
    transformRadix2InPlace(m_audioKernelReal, m_audioKernelImag, false);
    resetSpectrumState();
    return true;
}

void VividMediaBridge::resetSpectrumState()
{
    m_audioFrame.fill(0.0, AudioFrameLength);
    m_leftPcmRing.fill(0.0f);
    m_rightPcmRing.fill(0.0f);
    m_leftSamples.fill(0.0f);
    m_rightSamples.fill(0.0f);
    m_pcmWriteIndex = 0;
    m_pcmFramesAvailable = 0;
    m_pcmFramesWritten = 0;
    m_lastAnalyzedFramesWritten = 0;

    if (!m_spectrumConfiguration.valid())
        return;

    const int convolutionSize = m_spectrumConfiguration.convolutionSize;
    auto initState = [convolutionSize](SpectrumState& state) {
        state.real.fill(0.0, convolutionSize);
        state.imag.fill(0.0, convolutionSize);
        state.values.fill(0.0, AudioBandsPerChannel);
    };
    initState(m_leftSpectrum);
    initState(m_rightSpectrum);
}

QVector<double> VividMediaBridge::processSpectrumFrame(const QVector<float>& pcm,
                                                        SpectrumState& state)
{
    const SpectrumConfiguration& configuration = m_spectrumConfiguration;
    std::fill(state.real.begin(), state.real.end(), 0.0);
    std::fill(state.imag.begin(), state.imag.end(), 0.0);

    /*
     * Wallpaper Engine fills the sample-rate-derived populated prefix from PCM
     * and leaves the remaining slots at the encoding of a zero sample. This is
     * deliberately not a conventional real-input FFT or a windowed spectrum.
     */
    for (int index = 0; index < configuration.fftSize; index++) {
        const double sample = index < configuration.pcmSampleCount
            ? static_cast<double>(pcm.value(index))
            : 0.0;
        const double inputReal = AudioInputBias * (sample + 1.0);
        const double inputImag = 1.0 / inputReal;
        const double chirpReal = m_audioChirpReal[index];
        const double chirpImag = m_audioChirpImag[index];
        state.real[index] = inputReal * chirpReal - inputImag * chirpImag;
        state.imag[index] = inputReal * chirpImag + inputImag * chirpReal;
    }

    transformRadix2InPlace(state.real, state.imag, false);
    for (int index = 0; index < configuration.convolutionSize; index++) {
        const double valueReal = state.real[index];
        const double valueImag = state.imag[index];
        const double kernelReal = m_audioKernelReal[index];
        const double kernelImag = m_audioKernelImag[index];
        state.real[index] = valueReal * kernelReal - valueImag * kernelImag;
        state.imag[index] = valueReal * kernelImag + valueImag * kernelReal;
    }
    transformRadix2InPlace(state.real, state.imag, true);

    for (int index = 0; index < configuration.fftSize; index++) {
        const double valueReal = state.real[index];
        const double valueImag = state.imag[index];
        const double chirpReal = m_audioChirpReal[index];
        const double chirpImag = m_audioChirpImag[index];
        state.real[index] = valueReal * chirpReal - valueImag * chirpImag;
        state.imag[index] = valueReal * chirpImag + valueImag * chirpReal;
    }

    state.values.fill(0.0);
    int previousBand = 0;
    for (int bin = 1; bin < configuration.analysisBinCount; bin++) {
        const double realValue = state.real[bin];
        const double imagValue = state.imag[bin];
        double magnitudeSquared = realValue * realValue + imagValue * imagValue;
        if (!std::isfinite(magnitudeSquared))
            magnitudeSquared = 0.0;

        const double position =
            static_cast<double>(bin - 1) / (configuration.analysisBinCount - 1);
        const int rawBand = static_cast<int>(
            std::pow(position, AudioBandExponent) * AudioBandsPerChannel
        ) % AudioBandsPerChannel;
        const int band = std::min(rawBand, previousBand + 1);
        previousBand = band;

        const double weight = AudioWeightCenter -
            std::cos(Pi * position) * (1.0 - AudioWeightCenter);
        const double magnitude = std::sqrt(magnitudeSquared * weight);
        state.values[band] = std::max(state.values[band], magnitude);
    }

    const double outputScale = AudioOutputGain * 0.001 * configuration.analysisBinCount /
        (configuration.fftSize * 0.5);
    for (double& value : state.values)
        value *= outputScale;
    return state.values;
}
