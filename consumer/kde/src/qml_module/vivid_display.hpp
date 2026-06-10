/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#pragma once

#include <QColor>
#include <QJsonObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>
#include <QVector>
#include <qqml.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <vulkan/vulkan.h>

extern "C" {
#include "vivid_kde_vulkan_backend.h"
#include "vivid_kde_vulkan_blit.h"
}

extern "C" {
#include "vivid_display_protocol.h"
}

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

    int framesReceived() const { return m_framesReceived; }
    quint32 outputId() const { return m_outputId; }
    ConnState connState() const { return m_connState; }
    StreamState streamState() const { return m_streamState; }
    QColor clearColor() const { return m_clearColor; }
    QString lastError() const { return m_lastError; }

    Q_INVOKABLE void requestReconnect();

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
        GLuint glTexture { 0 };
        GLuint shadowTexture { 0 };
        GLuint shadowFramebuffer { 0 };
        bool importAttempted { false };
        int releaseSyncobjFd { -1 };
        QString releaseSyncContext;
        quint64 releaseAttachedUsec { 0 };
        ww_vk_imported_image_t vkImage {};
        bool hasVkImage { false };
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
        bool hasConfig { false };
        quint64 configGeneration { 0 };
        QVector<Buffer> buffers;
    };

    struct OutputGeometry {
        qreal scale { 1.0 };
        int physicalWidth { 1 };
        int physicalHeight { 1 };
    };

    QString effectiveSocketPath() const;
    OutputGeometry resolveOutputGeometry() const;
    bool sceneGraphReadyForProtocol() const;
    void armSceneGraphReadyConnection();
    void configureSceneGraphForProtocol();
    void scheduleReconnect(int delayMs = 1200);
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

    Generation* findGeneration(quint64 generation);
    Generation* latestPendingConfigGeneration(quint32 outputId);
    Generation* latestLiveGeneration(quint32 outputId);
    Buffer* findBuffer(Generation* generation, quint32 index);
    void retireGeneration(quint64 generation);
    void clearGenerations(bool destroyGlResources);
    void closeGenerationFds(Generation& generation);
    void destroyImportedResources(Generation& generation);
    void destroyRetiredResources();
    bool isCurrentGenerationIndex(qsizetype index) const;
    bool ensureBufferImported(Generation& generation, Buffer& buffer);
    bool ensureVulkanBufferImported(Generation& generation, Buffer& buffer);
    bool generationUsesShadowCopy(const Generation& generation) const;
    bool ensureBufferPresentedThroughShadow(Generation& generation, Buffer& buffer);
    bool ensureVulkanShadowCopy(Generation& generation, Buffer& buffer);
    bool bindVulkanBackend();
    void shutdownVulkanBackend();
    VkFormat vkFormatForFourcc(quint32 fourcc) const;
    void signalPendingReleaseSyncobj(Generation& generation,
                                     Buffer&     buffer,
                                     const QString& reason);
    void flushPendingReleaseSyncobj(const QString& reason);

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

    OutputGeometry m_outputGeometry;
    QVector<Generation> m_generations;
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
    VkInstance m_vkInstance { VK_NULL_HANDLE };
    VkPhysicalDevice m_vkPhysicalDevice { VK_NULL_HANDLE };
    VkDevice m_vkDevice { VK_NULL_HANDLE };
    VkQueue m_vkQueue { VK_NULL_HANDLE };
    quint32 m_vkQueueFamilyIndex { 0 };
    ww_vk_get_instance_proc_addr_fn m_vkGetInstanceProcAddr { nullptr };
    quint64 m_currentGeneration { 0 };
    quint32 m_currentBuffer { 0 };
    QRectF m_sourceRect;
    QRectF m_destRect;
    quint32 m_transform { 0 };
};
