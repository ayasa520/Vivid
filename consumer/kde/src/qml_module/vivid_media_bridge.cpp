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
constexpr int AudioSampleRate = 44100;
constexpr int AudioFftSize = 2048;
constexpr int AudioSampleBufferSize = AudioFftSize * 2;
constexpr quint64 AudioUpdateIntervalUsec = 16667;
constexpr double AudioMinFrequencyHz = 30.0;
constexpr double AudioMaxFrequencyHz = 18000.0;
constexpr double AudioMinDb = -80.0;
constexpr double AudioMaxDb = 0.0;
constexpr double AudioSilenceRmsThreshold = 0.003;
constexpr double AudioSpectrumOutputGain = 4.0;
constexpr double AudioBandPeakBlend = 0.35;
constexpr double AudioBandEdgeRatio = 1.1051178997261066;
constexpr double Pi = 3.14159265358979323846;

double clamp01(double value)
{
    if (!std::isfinite(value))
        return 0.0;
    return std::clamp(value, 0.0, 1.0);
}

double clip(double value, double min, double max)
{
    if (!std::isfinite(value))
        return min;
    return std::clamp(value, min, max);
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

    initializeSpectrumTables();
    resetSpectrumState();
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

    stopAudioCapture(false);
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
        stopAudioCapture(false);
    }
}

void VividMediaBridge::startMprisMonitor()
{
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
    m_mediaPollTimer.start();
}

void VividMediaBridge::stopMprisMonitor()
{
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
                                                    const QStringList&)
{
    if (interfaceName == QLatin1String(MprisPlayerInterface) &&
        (changedProperties.contains(QStringLiteral("PlaybackStatus")) ||
         changedProperties.contains(QStringLiteral("Metadata")))) {
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
    if (snapshots.isEmpty()) {
        m_lastSnapshot.reset();
        sendMediaPayload(nullptr, nullptr);
        return;
    }

    m_lastSnapshot = snapshots.first();
    ThumbnailPayload thumbnail = loadThumbnail(m_lastSnapshot->artUrl);
    sendMediaPayload(&m_lastSnapshot.value(), thumbnail.valid ? &thumbnail : nullptr);
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
    if (pa_context_connect(m_paContext, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0 ||
        pa_threaded_mainloop_start(m_paMainloop) < 0) {
        qCWarning(lcWallpaperMedia, "PulseAudio capture failed: unable to connect/start mainloop");
        scheduleAudioRestart();
        return;
    }
}

void VividMediaBridge::stopAudioCapture(bool emitSilence)
{
    Q_UNUSED(emitSilence)
    m_audioRestartTimer.stop();
    m_audioShouldRun = false;

    if (m_paMainloop)
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
    if (m_paMainloop)
        pa_threaded_mainloop_unlock(m_paMainloop);
    if (m_paMainloop) {
        pa_threaded_mainloop_stop(m_paMainloop);
        pa_threaded_mainloop_free(m_paMainloop);
        m_paMainloop = nullptr;
    }
}

void VividMediaBridge::scheduleAudioRestart()
{
    QMetaObject::invokeMethod(this, [this]() {
        if (!m_audioShouldRun)
            return;
        stopAudioCapture(false);
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

    const QByteArray monitorName =
        QByteArray(info->default_sink_name) + QByteArrayLiteral(".monitor");
    pa_sample_spec spec {};
    spec.format = PA_SAMPLE_FLOAT32LE;
    spec.rate = AudioSampleRate;
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
    attr.fragsize = static_cast<uint32_t>(AudioSampleRate * sizeof(float) * 2 / 60);
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
    case PA_STREAM_READY:
        qCInfo(lcWallpaperMedia, "audio sample capture started bands=%d sampleRate=%d",
               AudioFrameLength, AudioSampleRate);
        break;
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
    while (pa_stream_peek(stream, &data, &bytes) >= 0 && bytes > 0) {
        if (data) {
            const auto* samples = static_cast<const float*>(data);
            const int count = static_cast<int>(bytes / sizeof(float));
            QVector<float> chunk(count);
            std::copy(samples, samples + count, chunk.begin());
            QMetaObject::invokeMethod(self, [self, chunk = std::move(chunk)]() {
                self->handlePulseAudioChunk(chunk);
            }, Qt::QueuedConnection);
        }
        pa_stream_drop(stream);
        data = nullptr;
        bytes = 0;
    }
}

void VividMediaBridge::handlePulseAudioChunk(const QVector<float>& interleaved)
{
    if (!m_audioShouldRun || !m_display ||
        m_display->connState() != VividDisplay::Connected)
        return;
    processAudioChunk(interleaved);
}

void VividMediaBridge::processAudioChunk(const QVector<float>& interleaved)
{
    const int frameCount = interleaved.size() / 2;
    if (frameCount <= 0)
        return;

    if (frameCount >= AudioSampleBufferSize) {
        const int startFrame = frameCount - AudioSampleBufferSize;
        for (int i = 0; i < AudioSampleBufferSize; i++) {
            const int source = (startFrame + i) * 2;
            const float left = interleaved.value(source);
            m_leftSamples[i] = left;
            m_rightSamples[i] = interleaved.value(source + 1, left);
        }
    } else {
        std::move(m_leftSamples.begin() + frameCount, m_leftSamples.end(), m_leftSamples.begin());
        std::move(m_rightSamples.begin() + frameCount, m_rightSamples.end(), m_rightSamples.begin());
        const int writeOffset = AudioSampleBufferSize - frameCount;
        for (int i = 0; i < frameCount; i++) {
            const int source = i * 2;
            const float left = interleaved.value(source);
            m_leftSamples[writeOffset + i] = left;
            m_rightSamples[writeOffset + i] = interleaved.value(source + 1, left);
        }
    }

    const quint64 now = monotonicUsec();
    if (m_lastAudioEmitUsec != 0 && now - m_lastAudioEmitUsec < AudioUpdateIntervalUsec)
        return;
    m_lastAudioEmitUsec = now;

    QVector<float> left(AudioFftSize);
    QVector<float> right(AudioFftSize);
    const int offset = AudioSampleBufferSize - AudioFftSize;
    std::copy(m_leftSamples.begin() + offset, m_leftSamples.end(), left.begin());
    std::copy(m_rightSamples.begin() + offset, m_rightSamples.end(), right.begin());

    const QVector<double> leftValues = processSpectrumFrame(left, m_leftSpectrum);
    const QVector<double> rightValues = processSpectrumFrame(right, m_rightSpectrum);
    for (int i = 0; i < AudioBandsPerChannel; i++) {
        m_audioFrame[i] = leftValues.value(i);
        m_audioFrame[i + AudioBandsPerChannel] = rightValues.value(i);
    }

    if (m_display && m_display->sendAudioSamples(m_audioFrame, now)) {
        m_audioFrameCount++;
        if (m_audioFrameCount <= 8 || m_audioFrameCount % 300 == 0) {
            double maxSample = 0.0;
            for (double value : m_audioFrame)
                maxSample = std::max(maxSample, value);
            qCDebug(lcWallpaperMedia, "audio samples sent frame=%llu count=%d max=%.4f",
                    static_cast<unsigned long long>(m_audioFrameCount),
                    static_cast<int>(m_audioFrame.size()),
                    maxSample);
        }
    }
}

void VividMediaBridge::initializeSpectrumTables()
{
    m_bandEdgesHz.resize(AudioBandsPerChannel + 1);
    m_bandEdgesHz[0] = AudioMinFrequencyHz;
    for (int i = 1; i <= AudioBandsPerChannel; i++)
        m_bandEdgesHz[i] = m_bandEdgesHz[i - 1] * AudioBandEdgeRatio;
    m_bandEdgesHz[AudioBandsPerChannel] = AudioMaxFrequencyHz;
    m_bandCentersHz.resize(AudioBandsPerChannel);
    for (int i = 0; i < AudioBandsPerChannel; i++)
        m_bandCentersHz[i] = std::sqrt(m_bandEdgesHz[i] * m_bandEdgesHz[i + 1]);

    const int frequencyCount = AudioFftSize / 2 + 1;
    m_frequenciesHz.resize(frequencyCount);
    for (int i = 0; i < frequencyCount; i++)
        m_frequenciesHz[i] = (static_cast<double>(i) * AudioSampleRate) / AudioFftSize;

    m_window.resize(AudioFftSize);
    double windowSum = 0.0;
    for (int i = 0; i < AudioFftSize; i++) {
        m_window[i] = 0.5 * (1.0 - std::cos((2.0 * Pi * i) / (AudioFftSize - 1)));
        windowSum += m_window[i];
    }
    m_magnitudeReference = std::max(1.0, windowSum * 0.5);

    m_bandBinRanges.resize(AudioBandsPerChannel);
    for (int i = 0; i < AudioBandsPerChannel; i++) {
        const double low = m_bandEdgesHz[i];
        const double high = m_bandEdgesHz[i + 1];
        int begin = 0;
        while (begin < m_frequenciesHz.size() && m_frequenciesHz[begin] < low)
            begin++;
        int end = begin;
        while (end < m_frequenciesHz.size() && m_frequenciesHz[end] < high)
            end++;
        m_bandBinRanges[i] = { begin, end };
    }
}

void VividMediaBridge::resetSpectrumState()
{
    m_leftSamples.fill(0.0f, AudioSampleBufferSize);
    m_rightSamples.fill(0.0f, AudioSampleBufferSize);
    m_audioFrame.fill(0.0, AudioFrameLength);
    m_audioFrameCount = 0;
    m_lastAudioEmitUsec = 0;

    auto initState = [](SpectrumState& state) {
        state.smoothed.fill(0.0, AudioBandsPerChannel);
        state.lastDb.fill(AudioMinDb, AudioBandsPerChannel);
        state.bandDb.fill(0.0, AudioBandsPerChannel);
        state.normalized.fill(0.0, AudioBandsPerChannel);
        state.horizontal.fill(0.0, AudioBandsPerChannel);
        state.real.fill(0.0, AudioFftSize);
        state.imag.fill(0.0, AudioFftSize);
        state.magnitudes.fill(0.0, AudioFftSize / 2 + 1);
        state.normalizedMagnitudes.fill(0.0, AudioFftSize / 2 + 1);
        state.binDb.fill(0.0, AudioFftSize / 2 + 1);
    };
    initState(m_leftSpectrum);
    initState(m_rightSpectrum);
}

QVector<double> VividMediaBridge::processSpectrumFrame(const QVector<float>& pcm,
                                                           SpectrumState& state)
{
    double sumSquares = 0.0;
    for (float sample : pcm)
        sumSquares += static_cast<double>(sample) * sample;
    const double rms =
        std::sqrt(sumSquares / static_cast<double>(std::max<qsizetype>(1, pcm.size())) + 1e-12);
    if (rms < AudioSilenceRmsThreshold) {
        for (double& value : state.smoothed)
            value *= 0.82;
        state.lastDb.fill(AudioMinDb);
        return state.smoothed;
    }

    std::fill(state.real.begin(), state.real.end(), 0.0);
    std::fill(state.imag.begin(), state.imag.end(), 0.0);
    for (int i = 0; i < AudioFftSize; i++)
        state.real[i] = static_cast<double>(pcm.value(i)) * m_window[i];

    for (int i = 1, j = 0; i < AudioFftSize; i++) {
        int bit = AudioFftSize >> 1;
        while (j & bit) {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if (i < j) {
            std::swap(state.real[i], state.real[j]);
            std::swap(state.imag[i], state.imag[j]);
        }
    }

    for (int size = 2; size <= AudioFftSize; size <<= 1) {
        const int halfSize = size >> 1;
        const double theta = (-2.0 * Pi) / size;
        const double phaseRealStep = std::cos(theta);
        const double phaseImagStep = std::sin(theta);
        for (int offset = 0; offset < AudioFftSize; offset += size) {
            double phaseReal = 1.0;
            double phaseImag = 0.0;
            for (int i = 0; i < halfSize; i++) {
                const int even = offset + i;
                const int odd = even + halfSize;
                const double oddReal =
                    state.real[odd] * phaseReal - state.imag[odd] * phaseImag;
                const double oddImag =
                    state.real[odd] * phaseImag + state.imag[odd] * phaseReal;
                state.real[odd] = state.real[even] - oddReal;
                state.imag[odd] = state.imag[even] - oddImag;
                state.real[even] += oddReal;
                state.imag[even] += oddImag;
                const double nextPhaseReal =
                    phaseReal * phaseRealStep - phaseImag * phaseImagStep;
                const double nextPhaseImag =
                    phaseReal * phaseImagStep + phaseImag * phaseRealStep;
                phaseReal = nextPhaseReal;
                phaseImag = nextPhaseImag;
            }
        }
    }

    for (int i = 0; i < state.magnitudes.size(); i++) {
        const double magnitude =
            std::sqrt(state.real[i] * state.real[i] + state.imag[i] * state.imag[i]);
        const double normalized =
            clamp01((magnitude / m_magnitudeReference) * AudioSpectrumOutputGain);
        state.magnitudes[i] = magnitude;
        state.normalizedMagnitudes[i] = normalized;
        state.binDb[i] = normalized <= 0.0
            ? AudioMinDb
            : clip(20.0 * std::log10(normalized + 1e-12), AudioMinDb, AudioMaxDb);
    }

    auto interpolate = [](const QVector<double>& xs, const QVector<double>& ys, double target) {
        if (xs.isEmpty() || ys.isEmpty())
            return AudioMinDb;
        if (target <= xs.first())
            return ys.first();
        const int last = std::min(xs.size(), ys.size()) - 1;
        if (target >= xs[last])
            return ys[last];
        int lower = 0;
        while (lower < last && xs[lower + 1] < target)
            lower++;
        const int upper = std::min(last, lower + 1);
        const double lowerX = xs[lower];
        const double upperX = xs[upper];
        if (upperX <= lowerX)
            return ys[lower];
        const double mix = (target - lowerX) / (upperX - lowerX);
        return ys[lower] + (ys[upper] - ys[lower]) * mix;
    };

    for (int i = 0; i < AudioBandsPerChannel; i++) {
        const double centerMagnitude =
            interpolate(m_frequenciesHz, state.normalizedMagnitudes, m_bandCentersHz[i]);
        double powerSum = 0.0;
        int sampleCount = 0;
        double peakMagnitude = 0.0;
        const auto [begin, end] = m_bandBinRanges[i];
        for (int bin = begin; bin < end && bin < state.normalizedMagnitudes.size(); bin++) {
            const double magnitude = state.normalizedMagnitudes[bin];
            powerSum += magnitude * magnitude;
            peakMagnitude = std::max(peakMagnitude, magnitude);
            sampleCount++;
        }
        double bandMagnitude = centerMagnitude;
        if (sampleCount > 0) {
            const double rmsMagnitude = std::sqrt(powerSum / sampleCount + 1e-12);
            const double blended =
                peakMagnitude * AudioBandPeakBlend + rmsMagnitude * (1.0 - AudioBandPeakBlend);
            bandMagnitude = std::max(centerMagnitude, blended);
        }
        bandMagnitude = clamp01(bandMagnitude);
        state.normalized[i] = bandMagnitude;
        state.bandDb[i] = bandMagnitude <= 0.0
            ? AudioMinDb
            : clip(20.0 * std::log10(bandMagnitude + 1e-12), AudioMinDb, AudioMaxDb);
    }

    state.lastDb = state.bandDb;
    for (int i = 0; i < AudioBandsPerChannel; i++) {
        const double left = state.normalized[std::max(0, i - 1)];
        const double center = state.normalized[i];
        const double right = state.normalized[std::min(AudioBandsPerChannel - 1, i + 1)];
        state.horizontal[i] = left * 0.08 + center * 0.84 + right * 0.08;
    }
    for (int i = 0; i < AudioBandsPerChannel; i++) {
        const double target = state.horizontal[i];
        const double current = state.smoothed[i];
        state.smoothed[i] = target > current
            ? current * 0.25 + target * 0.75
            : current * 0.75 + target * 0.25;
    }
    return state.smoothed;
}
