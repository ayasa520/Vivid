/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QPoint>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QSize>
#include <QSocketNotifier>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QVulkanInstance>
#include <qqml.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <vulkan/vulkan.h>

extern "C" {
#include "vivid_kde_vulkan_backend.h"
#include "vivid_kde_vulkan_blit.h"
}

#include "vivid_protocol_cpp.hpp"

class VividDisplay : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString socketPath READ socketPath WRITE setSocketPath NOTIFY socketPathChanged)
    Q_PROPERTY(QString displayName READ displayName WRITE setDisplayName NOTIFY displayNameChanged)
    Q_PROPERTY(QString screenName READ screenName WRITE setScreenName NOTIFY screenNameChanged)
    Q_PROPERTY(QString instanceId READ instanceId WRITE setInstanceId NOTIFY instanceIdChanged)
    Q_PROPERTY(quint32 consumerOutputId READ consumerOutputId WRITE setConsumerOutputId NOTIFY
                   consumerOutputIdChanged)
    Q_PROPERTY(
        quint32 monitorIndex READ monitorIndex WRITE setMonitorIndex NOTIFY monitorIndexChanged)
    Q_PROPERTY(int displayX READ displayX WRITE setDisplayX NOTIFY displayGeometryChanged)
    Q_PROPERTY(int displayY READ displayY WRITE setDisplayY NOTIFY displayGeometryChanged)
    Q_PROPERTY(
        int logicalWidth READ logicalWidth WRITE setLogicalWidth NOTIFY displayGeometryChanged)
    Q_PROPERTY(
        int logicalHeight READ logicalHeight WRITE setLogicalHeight NOTIFY displayGeometryChanged)
    Q_PROPERTY(int displayWidth READ displayWidth WRITE setDisplayWidth NOTIFY displayGeometryChanged)
    Q_PROPERTY(
        int displayHeight READ displayHeight WRITE setDisplayHeight NOTIFY displayGeometryChanged)
    Q_PROPERTY(qreal displayScale READ displayScale WRITE setDisplayScale NOTIFY displayGeometryChanged)
    Q_PROPERTY(quint32 refreshRateMhz READ refreshRateMhz WRITE setRefreshRateMhz NOTIFY
                   displayGeometryChanged)
    Q_PROPERTY(bool autoReconnect READ autoReconnect WRITE setAutoReconnect NOTIFY
                   autoReconnectChanged)
    Q_PROPERTY(bool mouseForwardEnabled READ mouseForwardEnabled WRITE setMouseForwardEnabled NOTIFY
                   mouseForwardEnabledChanged)
    Q_PROPERTY(quint32 windowStateFlags READ windowStateFlags WRITE setWindowStateFlags NOTIFY
                   windowStateFlagsChanged)
    Q_PROPERTY(QStringList coveredScreenNames READ coveredScreenNames WRITE setCoveredScreenNames
                   NOTIFY coveredScreenNamesChanged)
    Q_PROPERTY(QString focusedScreenName READ focusedScreenName WRITE setFocusedScreenName NOTIFY
                   focusedScreenNameChanged)
    Q_PROPERTY(QJsonArray windowFacts READ windowFacts WRITE setWindowFacts NOTIFY
                   windowFactsChanged)

    Q_PROPERTY(int framesReceived READ framesReceived NOTIFY framesReceivedChanged)
    Q_PROPERTY(quint32 outputId READ outputId NOTIFY outputIdChanged)
    Q_PROPERTY(ConnState connState READ connState NOTIFY connStateChanged)
    Q_PROPERTY(StreamState streamState READ streamState NOTIFY streamStateChanged)
    Q_PROPERTY(QColor clearColor READ clearColor NOTIFY clearColorChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    enum ConnState
    {
        Disconnected = 0,
        Connecting,
        Handshaking,
        Connected,
        Error,
    };
    Q_ENUM(ConnState)

    enum StreamState
    {
        Inactive = 0,
        Active,
    };
    Q_ENUM(StreamState)

    explicit VividDisplay(QQuickItem* parent = nullptr);
    ~VividDisplay() override;

    QString socketPath() const { return m_socketPath; }
    void    setSocketPath(const QString& path);

    QString displayName() const { return m_displayName; }
    void    setDisplayName(const QString& name);

    QString screenName() const { return m_screenName; }
    void    setScreenName(const QString& name);

    QString instanceId() const { return m_instanceId; }
    void    setInstanceId(const QString& id);

    quint32 consumerOutputId() const { return m_consumerOutputId; }
    void    setConsumerOutputId(quint32 id);

    quint32 monitorIndex() const { return m_monitorIndex; }
    void    setMonitorIndex(quint32 index);

    int  displayX() const { return m_displayX; }
    void setDisplayX(int value);
    int  displayY() const { return m_displayY; }
    void setDisplayY(int value);
    int  logicalWidth() const { return m_logicalWidth; }
    void setLogicalWidth(int value);
    int  logicalHeight() const { return m_logicalHeight; }
    void setLogicalHeight(int value);
    int  displayWidth() const { return m_displayWidth; }
    void setDisplayWidth(int value);
    int  displayHeight() const { return m_displayHeight; }
    void setDisplayHeight(int value);
    qreal displayScale() const { return m_displayScale; }
    void  setDisplayScale(qreal value);
    quint32 refreshRateMhz() const { return m_refreshRateMhz; }
    void    setRefreshRateMhz(quint32 value);

    bool autoReconnect() const { return m_autoReconnect; }
    void setAutoReconnect(bool enabled);

    bool mouseForwardEnabled() const { return m_mouseForwardEnabled; }
    void setMouseForwardEnabled(bool enabled);

    quint32 windowStateFlags() const { return m_windowStateFlags; }
    void    setWindowStateFlags(quint32 flags);
    QStringList coveredScreenNames() const { return m_coveredScreenNames; }
    void        setCoveredScreenNames(const QStringList& names);
    QString focusedScreenName() const { return m_focusedScreenName; }
    void    setFocusedScreenName(const QString& name);
    QJsonArray windowFacts() const { return m_windowFacts; }
    void       setWindowFacts(const QJsonArray& facts);

    int framesReceived() const { return m_framesReceived; }
    quint32 outputId() const { return m_outputId; }
    ConnState connState() const { return m_connState; }
    StreamState streamState() const { return m_streamState; }
    QColor clearColor() const { return m_clearColor; }
    QString lastError() const { return m_lastError; }

    Q_INVOKABLE void requestReconnect();

    void setMprisPlaybackFacts(const QJsonArray& players);
    bool sendMediaState(const QJsonObject& payload);
    bool sendAudioSamples(const QVector<double>& samples, quint64 timeUsec);

    bool eventFilter(QObject* object, QEvent* event) override;

signals:
    void socketPathChanged();
    void displayNameChanged();
    void screenNameChanged();
    void instanceIdChanged();
    void consumerOutputIdChanged();
    void monitorIndexChanged();
    void displayGeometryChanged();
    void autoReconnectChanged();
    void mouseForwardEnabledChanged();
    void windowStateFlagsChanged();
    void coveredScreenNamesChanged();
    void focusedScreenNameChanged();
    void windowFactsChanged();
    void framesReceivedChanged();
    void outputIdChanged();
    void connStateChanged();
    void streamStateChanged();
    void clearColorChanged();
    void lastErrorChanged();

protected:
    void     componentComplete() override;
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

private slots:
    void onSocketReadable();
    void onSocketWritable();
    void onReconnectTimer();
    void onWindowChanged(QQuickWindow* window);
    void onSceneGraphInitialized();
    void onSceneGraphInvalidated();

private:
    struct Plane {
        int     fd { -1 };
        quint32 stride { 0 };
        quint32 offset { 0 };
    };

    struct Buffer {
        quint32 index { 0 };
        quint64 size { 0 };
        QVector<Plane> planes;
        EGLImageKHR eglImage { EGL_NO_IMAGE_KHR };
        EGLDisplay eglDisplay { EGL_NO_DISPLAY };
        GLuint glTexture { 0 };
        bool importAttempted { false };
        ww_vk_imported_image_t vkImage {};
        bool hasVkImage { false };
        VkSemaphore acquireSemaphore { VK_NULL_HANDLE };
    };

    struct Generation {
        quint64 id { 0 };
        quint32 outputId { 0 };
        int width { 0 };
        int height { 0 };
        quint32 fourcc { 0 };
        quint64 modifier { 0 };
        quint32 planesPerBuffer { 0 };
        QString renderNode;
        QString vendor;
        QString pciAddress;
        QString negotiatedPath;
        QString memorySource;
        QString memoryHint;
        QString presentationPath;
        QString producerDriverUuid;
        quint32 producerDrmRenderMajor { 0 };
        quint32 producerDrmRenderMinor { 0 };
        QString consumerRenderNode;
        quint32 consumerDrmRenderMajor { 0 };
        quint32 consumerDrmRenderMinor { 0 };
        bool premultiplied { true };
        bool retired { false };
        /* GPU handles have been detached from this metadata record and moved
         * to m_pendingGenerations.  Keeping the lightweight record allows the
         * persistent shadow texture to remain visible until the replacement
         * generation presents its first frame. */
        bool resourcesQueued { false };
        bool hasConfig { false };
        quint64 configGeneration { 0 };
        QVector<Buffer> buffers;
    };

    struct FinalGpuCleanup {
        QVector<Generation> generations;
        ww_vk_backend_t vkBackend {};
        ww_vk_blitter_t vkBlitter {};
        bool vkBackendReady { false };
        bool vkBlitterReady { false };
        GLuint eglShadowTexture { 0 };
        GLuint eglShadowReadFramebuffer { 0 };
        GLuint eglShadowFramebuffer { 0 };
    };

    struct OutputGeometry {
        qreal scale { 1.0 };
        int physicalWidth { 1 };
        int physicalHeight { 1 };
        quint32 consumerOutputId { 1 };
        quint32 monitorIndex { 0 };
        QString displayKey;
        QString displayName;
    };

    struct OutputSnapshot {
        QString name;
        QString displayKey;
        QString vendor;
        QString model;
        QPoint position;
        QSize logicalSize;
        qreal scale { 1.0 };
    };

    struct PendingVulkanFrame {
        bool valid { false };
        quint64 generation { 0 };
        quint32 bufferIndex { 0 };
        int releaseSyncobjFd { -1 };
        QString renderNode;
        QString releaseContext;
    };

    struct PendingEglFrame {
        bool valid { false };
        quint64 generation { 0 };
        quint32 bufferIndex { 0 };
        quint64 sequence { 0 };
        int acquireSyncFd { -1 };
        QString acquireContext;
    };

    struct PendingEglRelease {
        quint64 generation { 0 };
        quint64 sequence { 0 };
        int syncobjFd { -1 };
        QString renderNode;
        QString context;
    };

    QString effectiveSocketPath() const;
    OutputGeometry resolveOutputGeometry() const;
    void refreshOutputSnapshot();
    bool sceneGraphReadyForProtocol() const;
    void armSceneGraphReadyConnection();
    void releaseSceneGraphResources();
    void releaseIdleProtocolBackend();
    void configureSceneGraphForProtocol();
    void scheduleReconnect(int delayMs = 1200);
    void refreshSocketWatcher();
    void closeTransport(bool keepLastFrame);
    void tryConnect();
    void finishConnect();
    void flushOutbox();
    void queueFrame(quint16 opcode, const QByteArray& body);
    void queueJsonFrame(quint16 opcode, const QJsonObject& object);
    void sendHello();
    void sendConsumerCaps();
    void sendBindFailed(const Generation& generation, quint32 reason, const QString& message);
    void sendRegisterOutput();
    void sendWindowState();
    QString displayKeyForScreenName(const QString& screenName) const;
    void sendPointerMotion(float x, float y, quint64 timeUsec);
    void sendPointerButton(float x, float y, quint32 button, bool pressed, quint64 timeUsec);
    void sendPointerAxis(float x, float y, double dx, double dy, quint64 timeUsec);

    void handleIncomingFrame(quint16 opcode, const QByteArray& body, VividDisplayRecvState* state);
    void handleOutputAccepted(const QJsonObject& object);
    void handleBindBuffers(const QByteArray& body, VividDisplayRecvState* state);
    void handleSetConfig(const QJsonObject& object);
    void handleFrameReady(const QByteArray& body, VividDisplayRecvState* state);
    void handleUnbind(const QByteArray& body);
    void handleProtocolError(int code, const QString& message);
    void handleProducerError(quint32 code, bool fatal, const QString& message);

    Generation* findGeneration(quint64 generation);
    Generation* latestPendingConfigGeneration(quint32 outputId);
    Generation* latestLiveGeneration(quint32 outputId);
    Buffer* findBuffer(Generation* generation, quint32 index);
    void retireGeneration(quint64 generation);
    void retireGenerationsForOutput(quint32 outputId, const QString& reason);
    void retireAllGenerations(const QString& reason);
    void queueGenerationResources(Generation& generation, const QString& reason);
    void clearGenerations(bool destroyGlResources);
    void closeGenerationFds(Generation& generation);
    void destroyImportedResources(Generation& generation);
    static void destroyImportedResourcesWithBackend(Generation& generation,
                                                     ww_vk_backend_t* backend,
                                                     bool backendReady);
    int drainPendingGenerationResources();
    void destroyRetiredResources();
    void destroyEglShadowResources();
    FinalGpuCleanup* takeFinalGpuCleanup();
    static bool destroyFinalGpuCleanup(FinalGpuCleanup* cleanup);
    bool ensureEglShadowResources(int width, int height);
    bool isCurrentGenerationIndex(qsizetype index) const;
    bool importEglImage(Generation& generation, Buffer& buffer, EGLDisplay eglDisplay);
    bool ensureBufferImported(Generation& generation, Buffer& buffer);
    bool ensureGenerationEglTextures(Generation& generation);
    bool ensureVulkanBufferImported(Generation& generation, Buffer& buffer);
    bool blitEglShadow(Generation& generation, Buffer& buffer);
    void renderThreadBlitEgl();
    bool ensureVulkanShadowCopy(Generation& generation,
                                Buffer&     buffer,
                                PendingVulkanFrame pending);
    bool bindVulkanBackend();
    void shutdownVulkanBackend();
    VkFormat vkFormatForFourcc(quint32 fourcc) const;
    void discardPendingEglFrame(quint64 generation);
    void signalPendingEglRelease(const QString& reason,
                                 quint64 generation = 0,
                                 quint64 sequence = 0);
    void signalPendingVulkanFrame(const QString& reason);

    void setConnState(ConnState state);
    void setStreamState(StreamState state);
    void setLastError(const QString& error);
    void geometryPropertyChanged();
    void installOrRemoveEventFilter();

    QString m_socketPath;
    QString m_displayName { QStringLiteral("kde-plasma") };
    QString m_screenName;
    QString m_instanceId;
    quint32 m_consumerOutputId { 1 };
    quint32 m_monitorIndex { 0 };
    int m_displayX { 0 };
    int m_displayY { 0 };
    int m_logicalWidth { 1 };
    int m_logicalHeight { 1 };
    int m_displayWidth { 1 };
    int m_displayHeight { 1 };
    qreal m_displayScale { 1.0 };
    quint32 m_refreshRateMhz { 0 };
    bool m_autoReconnect { true };
    bool m_mouseForwardEnabled { true };
    bool m_filterInstalled { false };
    quint32 m_windowStateFlags { 0 };
    QStringList m_coveredScreenNames;
    QString m_focusedScreenName;
    QJsonArray m_windowFacts;
    bool m_mprisPlaying { false };
    QJsonArray m_mprisPlayers;

    int m_framesReceived { 0 };
    quint32 m_outputId { 0 };
    ConnState m_connState { Disconnected };
    StreamState m_streamState { Inactive };
    QColor m_clearColor { Qt::black };
    QString m_lastError;

    int m_fd { -1 };
    bool m_connecting { false };
    VividDisplayRecvState m_recvState {};
    QPointer<QSocketNotifier> m_readNotifier;
    QPointer<QSocketNotifier> m_writeNotifier;
    QVector<QByteArray> m_outbox;
    qsizetype m_outboxOffset { 0 };
    QTimer m_reconnectTimer;
    QFileSystemWatcher m_socketWatcher;
    QPointer<QQuickWindow> m_sceneGraphWindow;

    OutputGeometry m_outputGeometry;
    QVector<OutputSnapshot> m_outputSnapshots;
    bool m_outputSnapshotRefreshPending { false };
    QVector<Generation> m_generations;
    QVector<Generation> m_pendingGenerations;
    QMutex m_pendingGenerationMutex;
    quint64 m_pendingPoolsQueued { 0 };
    quint64 m_pendingPoolsDrained { 0 };
    enum ActiveBackend {
        BackendNone,
        BackendEgl,
        BackendVulkan,
    };
    ActiveBackend m_activeBackend { BackendNone };
    ww_vk_backend_t m_vkBackend {};
    ww_vk_blitter_t m_vkBlitter {};
    bool m_vkBackendReady { false };
    bool m_vkBlitterReady { false };
    /* One persistent Qt-owned shadow texture per display, matching
     * Waywallen's EGL QML path. Imported producer slots are copied into this
     * texture; QSG never wraps a per-buffer texture or an EGLImage. */
    GLuint m_eglShadowTexture { 0 };
    GLuint m_eglShadowReadFramebuffer { 0 };
    GLuint m_eglShadowFramebuffer { 0 };
    int m_eglShadowWidth { 0 };
    int m_eglShadowHeight { 0 };
    bool m_eglShadowHasContent { false };
    quint64 m_eglShadowGeneration { 0 };
    quint32 m_eglShadowBuffer { 0 };
    quint64 m_eglShadowSequence { 0 };
    VkInstance m_vkInstance { VK_NULL_HANDLE };
    VkPhysicalDevice m_vkPhysicalDevice { VK_NULL_HANDLE };
    VkDevice m_vkDevice { VK_NULL_HANDLE };
    VkQueue m_vkQueue { VK_NULL_HANDLE };
    quint32 m_vkQueueFamilyIndex { 0 };
    ww_vk_get_instance_proc_addr_fn m_vkGetInstanceProcAddr { nullptr };
    PendingEglFrame m_pendingEglFrame;
    PendingEglRelease m_pendingEglRelease;
    QMutex m_pendingEglMutex;
    PendingVulkanFrame m_pendingVulkanFrame;
    QMutex m_pendingVulkanMutex;
    quint64 m_currentGeneration { 0 };
    quint32 m_currentBuffer { 0 };
    QRectF m_sourceRect;
    QRectF m_destRect;
    quint32 m_transform { 0 };
    quint32 m_negotiatedVersion { vivid::protocol::VIVID_DISPLAY_PROTOCOL_VERSION };
    int m_errorReconnectDelayMs { 1200 };
};
