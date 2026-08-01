/*
 * Protocol optimization changes in this file are derived from waywallen.
 * Source: local reference tree waywallen/ and upstream https://github.com/waywallen/waywallen.
 * Copyright owner for the waywallen-derived protocol optimization code:
 * https://github.com/hypengw <hypengwip@gmail.com>.
 */

#include "vivid_display.hpp"

#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QRunnable>
#include <QScreen>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTransformNode>
#include <QThread>
#include <QVulkanInstance>
#include <QWheelEvent>
#include <QtGui/qopenglcontext_platform.h>
#include <QtQuick/qsgtexture_platform.h>

#include <KScreen/Config>
#include <KScreen/GetConfigOperation>
#include <KScreen/Output>

#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#ifdef VIVID_KDE_HAVE_GBM
#include <gbm.h>
#endif

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "vivid_protocol_cpp.hpp"
#include "vivid_protocol_meta.hpp"
#include "vivid_protocol_json_fields.hpp"

namespace Proto = vivid::protocol;

Q_LOGGING_CATEGORY(lcWallpaperKde, "wallpaper.display.kde")

namespace
{
class FunctionRenderJob final : public QRunnable {
public:
    explicit FunctionRenderJob(std::function<void()> function)
        : m_function(std::move(function))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        if (m_function)
            m_function();
    }

private:
    std::function<void()> m_function;
};

constexpr quint64 DrmFormatModInvalid = (1ull << 56) - 1ull;
constexpr quint64 DrmFormatModLinear = 0;

constexpr const char BackendKdeEgl[] = "kde-qt6-egl-gl-texture-2d";
constexpr const char BackendKdeVulkan[] = "kde-qt6-vulkan-shadow-image";
constexpr const char BackendKdeUnsupported[] = "kde-qt6-unsupported";
constexpr const char RendererQt6Egl[] = "qt6-egl-image";
constexpr const char RendererQt6VulkanShadow[] = "qt6-vulkan-shadow-image";
constexpr const char RendererNone[] = "none";
constexpr const char RelayShadowCopy[] = "shadow-copy-v1";
constexpr const char MemoryHostVisible[] = "host-visible";
constexpr const char MemoryDeviceLocal[] = "device-local";
constexpr const char MemoryImplicitLinear[] = "implicit-linear";
constexpr const char SyncImplicit[] = "implicit";
constexpr const char SyncExplicitFd[] = "explicit-sync-fd";
constexpr const char SyncDrmSyncobjRelease[] = "drm-syncobj-release";
constexpr const char ColorSrgb[] = "srgb";
constexpr const char ColorLimitedRange[] = "limited-range";
constexpr const char ColorPremultipliedAlpha[] = "premultiplied-alpha";
constexpr const char TextureTargetGl2D[] = "GL_TEXTURE_2D";
constexpr const char TextureTargetVulkanShadow[] = "QSGVulkanTextureShadowVkImage";
constexpr const char FeatureDmaBufShadowCopy[] = "dmabuf-shadow-copy-v1";

QString displayKeyFromKScreenOutput(const KScreen::OutputPtr& output,
                                      const QString&            instanceIdFallback)
{
    if (!output)
        return {};

    const QString hash = output->hashMd5().trimmed();
    const QString name = output->name().trimmed();
    if (!hash.isEmpty())
        return hash;
    if (!name.isEmpty())
        return name;
    return instanceIdFallback.trimmed();
}

constexpr const char* VulkanDeviceExtensions[] = {
    "VK_KHR_external_memory",
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_queue_family_foreign",
    "VK_EXT_image_drm_format_modifier",
    "VK_KHR_external_semaphore",
    "VK_KHR_external_semaphore_fd",
};

QJsonArray jsonStringArray(std::initializer_list<const char*> values)
{
    QJsonArray array;
    for (const char* value : values)
        array.append(QString::fromLatin1(value));
    return array;
}

QJsonArray shadowCopyRelayModes()
{
    return jsonStringArray({ RelayShadowCopy });
}

QJsonArray syncCaps()
{
    return jsonStringArray({ SyncImplicit, SyncExplicitFd, SyncDrmSyncobjRelease });
}

QJsonArray colorCaps()
{
    return jsonStringArray({ ColorSrgb, ColorLimitedRange, ColorPremultipliedAlpha });
}

QJsonArray hostVisibleMemoryHints()
{
    return jsonStringArray({ MemoryHostVisible });
}

QJsonObject unlimitedExtentCaps()
{
    return QJsonObject {
        { QStringLiteral("width"), 0 },
        { QStringLiteral("height"), 0 },
    };
}

QList<KScreen::OutputPtr> sortedEnabledOutputs(const KScreen::ConfigPtr& config)
{
    QList<KScreen::OutputPtr> enabledOutputs;
    if (!config)
        return enabledOutputs;

    const KScreen::OutputList outputs = config->outputs();
    for (auto it = outputs.cbegin(); it != outputs.cend(); ++it) {
        const KScreen::OutputPtr output = it.value();
        if (!output || !output->isConnected() || !output->isEnabled())
            continue;
        enabledOutputs.append(output);
    }

    std::sort(enabledOutputs.begin(), enabledOutputs.end(), [](const auto& left, const auto& right) {
        const uint32_t leftPriority = left ? left->priority() : 0;
        const uint32_t rightPriority = right ? right->priority() : 0;
        if (leftPriority != rightPriority)
            return leftPriority < rightPriority;
        const QPoint leftPos = left ? left->pos() : QPoint();
        const QPoint rightPos = right ? right->pos() : QPoint();
        if (leftPos.y() != rightPos.y())
            return leftPos.y() < rightPos.y();
        if (leftPos.x() != rightPos.x())
            return leftPos.x() < rightPos.x();
        return (left ? left->id() : 0) < (right ? right->id() : 0);
    });
    return enabledOutputs;
}

QList<QByteArray> vulkanDeviceExtensionNames()
{
    QList<QByteArray> extensions;
    for (const char* extension : VulkanDeviceExtensions)
        extensions.append(QByteArray(extension));
    return extensions;
}

quint64 monotonicUsec()
{
    using namespace std::chrono;
    return static_cast<quint64>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

void writeU16LE(QByteArray& bytes, qsizetype offset, quint16 value)
{
    bytes[offset] = static_cast<char>(value & 0xffu);
    bytes[offset + 1] = static_cast<char>((value >> 8) & 0xffu);
}

void closeFd(int& fd)
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void closeRecvStateFds(VividDisplayRecvState* state)
{
    if (!state)
        return;

    for (size_t index = 0; index < state->n_fds; index++) {
        int fd = vivid_display_recv_state_steal_fd(state, index);
        closeFd(fd);
    }
}

/*
 * Minimal DRM syncobj uAPI mirror. This follows waywallen-display's approach:
 * use the kernel ioctl ABI directly so the KDE consumer does not need libdrm
 * headers or a libdrm link dependency just to signal the release syncobj. Keep
 * these layouts in lockstep with <linux/drm.h>.
 */
struct VividDrmSyncobjHandle {
    quint32 handle;
    quint32 flags;
    qint32 fd;
    quint32 pad;
};

struct VividDrmSyncobjDestroy {
    quint32 handle;
    quint32 pad;
};

struct VividDrmSyncobjArray {
    quint64 handles;
    quint32 countHandles;
    quint32 pad;
};

#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE 'd'
#endif

#define VIVID_DRM_IOCTL_SYNCOBJ_DESTROY \
    _IOWR(DRM_IOCTL_BASE, 0xC0, struct VividDrmSyncobjDestroy)
#define VIVID_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE \
    _IOWR(DRM_IOCTL_BASE, 0xC2, struct VividDrmSyncobjHandle)
#define VIVID_DRM_IOCTL_SYNCOBJ_SIGNAL \
    _IOWR(DRM_IOCTL_BASE, 0xC5, struct VividDrmSyncobjArray)

bool signalReleaseSyncobj(const QString& renderNode, int syncobjFd, const QString& context)
{
    if (renderNode.isEmpty() || syncobjFd < 0) {
        qCWarning(lcWallpaperKde,
                  "cannot signal release syncobj context=%s render-node=%s fd=%d",
                  qPrintable(context),
                  qPrintable(renderNode.isEmpty() ? QStringLiteral("(missing)") : renderNode),
                  syncobjFd);
        return false;
    }

    const int drmFd = ::open(qPrintable(renderNode), O_RDWR | O_CLOEXEC);
    if (drmFd < 0) {
        qCWarning(lcWallpaperKde,
                  "open(%s) for release syncobj signal failed context=%s: %s",
                  qPrintable(renderNode),
                  qPrintable(context),
                  strerror(errno));
        return false;
    }
    VividDrmSyncobjHandle import {
        .handle = 0,
        .flags = 0,
        .fd = syncobjFd,
        .pad = 0,
    };
    errno = 0;
    if (::ioctl(drmFd, VIVID_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import) != 0) {
        const int error = errno;
        qCWarning(lcWallpaperKde,
                  "DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE(%s) failed context=%s: %s",
                  qPrintable(renderNode),
                  qPrintable(context),
                  strerror(error));
        ::close(drmFd);
        return false;
    }

    quint32 handles[1] = { import.handle };
    VividDrmSyncobjArray signal {
        .handles = reinterpret_cast<quintptr>(handles),
        .countHandles = 1,
        .pad = 0,
    };
    errno = 0;
    const bool signalOk = (::ioctl(drmFd, VIVID_DRM_IOCTL_SYNCOBJ_SIGNAL, &signal) == 0);
    const int signalError = errno;

    VividDrmSyncobjDestroy destroy {
        .handle = import.handle,
        .pad = 0,
    };
    errno = 0;
    if (::ioctl(drmFd, VIVID_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy) != 0) {
        const int destroyError = errno;
        qCWarning(lcWallpaperKde,
                  "drmSyncobjDestroy(release handle=%u) failed context=%s: %s",
                  import.handle,
                  qPrintable(context),
                  strerror(destroyError));
    }
    ::close(drmFd);

    if (!signalOk) {
        qCWarning(lcWallpaperKde,
                  "DRM_IOCTL_SYNCOBJ_SIGNAL(%s handle=%u) failed context=%s: %s",
                  qPrintable(renderNode),
                  import.handle,
                  qPrintable(context),
                  strerror(signalError));
        return false;
    }
    return true;
}

struct VulkanReleaseSignalContext {
    QString renderNode;
    QString context;
};

int signalReleaseSyncobjFromVulkanBlit(int releaseSyncobjFd, void* userData)
{
    auto* context = static_cast<VulkanReleaseSignalContext*>(userData);
    if (!context) {
        return -EINVAL;
    }
    const bool signaled =
        signalReleaseSyncobj(context->renderNode, releaseSyncobjFd, context->context);
    return signaled ? 0 : -EIO;
}

quint64 jsonUInt64(const QJsonValue& value, quint64 fallback = 0)
{
    if (value.isString()) {
        bool ok = false;
        const quint64 parsed = value.toString().toULongLong(&ok, 0);
        return ok ? parsed : fallback;
    }
    if (value.isDouble())
        return static_cast<quint64>(std::max(0.0, value.toDouble()));
    return fallback;
}

quint32 jsonUInt32(const QJsonValue& value, quint32 fallback = 0)
{
    return static_cast<quint32>(jsonUInt64(value, fallback));
}

QJsonObject parseJsonObject(const QByteArray& body, QString* errorText)
{
    QJsonParseError error {};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError) {
        if (errorText)
            *errorText = error.errorString();
        return {};
    }
    if (!doc.isObject()) {
        if (errorText)
            *errorText = QStringLiteral("JSON payload root is not an object");
        return {};
    }
    return doc.object();
}

QString formatErrno(int code)
{
    const int positive = code < 0 ? -code : code;
    return QString::fromLocal8Bit(std::strerror(positive));
}

quint32 transformCode(const QJsonValue& value)
{
    if (value.isDouble())
        return static_cast<quint32>(std::max(0.0, value.toDouble()));

    const QString text = value.toString(QStringLiteral("normal")).toLower();
    if (text == QStringLiteral("90") || text == QStringLiteral("rotate-90") ||
        text == QStringLiteral("rotated-90"))
        return 1;
    if (text == QStringLiteral("180") || text == QStringLiteral("rotate-180") ||
        text == QStringLiteral("rotated-180"))
        return 2;
    if (text == QStringLiteral("270") || text == QStringLiteral("rotate-270") ||
        text == QStringLiteral("rotated-270"))
        return 3;
    if (text == QStringLiteral("flipped") || text == QStringLiteral("flipped-normal"))
        return 4;
    if (text == QStringLiteral("flipped-90"))
        return 5;
    if (text == QStringLiteral("flipped-180"))
        return 6;
    if (text == QStringLiteral("flipped-270"))
        return 7;
    return 0;
}

quint32 qtButtonToProtocolButton(Qt::MouseButton button)
{
    /*
     * The display protocol follows GNOME Shell/Clutter button numbering:
     * 1 = left, 2 = middle, 3 = right. Do not forward Linux BTN_* codes here;
     * the scene backend consumes this protocol value directly, and the web
     * backend uses the same value to build CEF button/modifier state.
     */
    switch (button) {
    case Qt::LeftButton:
        return 1;
    case Qt::MiddleButton:
        return 2;
    case Qt::RightButton:
        return 3;
    default:
        return 0;
    }
}

using EglCreateImageKhr = PFNEGLCREATEIMAGEKHRPROC;
using EglDestroyImageKhr = PFNEGLDESTROYIMAGEKHRPROC;
using GlEglImageTargetTexture2DOes = PFNGLEGLIMAGETARGETTEXTURE2DOESPROC;
using EglQueryDisplayAttribExt = PFNEGLQUERYDISPLAYATTRIBEXTPROC;
using EglQueryDeviceStringExt = PFNEGLQUERYDEVICESTRINGEXTPROC;
using EglQueryDmaBufFormatsExt = PFNEGLQUERYDMABUFFORMATSEXTPROC;
using EglQueryDmaBufModifiersExt = PFNEGLQUERYDMABUFMODIFIERSEXTPROC;
using EglCreateSyncKhr = PFNEGLCREATESYNCKHRPROC;
using EglWaitSyncKhr = PFNEGLWAITSYNCKHRPROC;
using EglDestroySyncKhr = PFNEGLDESTROYSYNCKHRPROC;

struct GpuIdentity {
    QString renderNode;
    QString vendor;
    QString pciAddress;
    QString deviceUuid;
    QString driverUuid;
    QString diagnostics;
};

EGLDisplay qtWindowEglDisplay(QQuickWindow* window)
{
    if (!window)
        return EGL_NO_DISPLAY;

    auto* rendererInterface = window->rendererInterface();
    auto* context = static_cast<QOpenGLContext*>(
        rendererInterface
            ? rendererInterface->getResource(window, QSGRendererInterface::OpenGLContextResource)
            : nullptr);
    if (!context)
        context = QOpenGLContext::currentContext();
    if (!context)
        return EGL_NO_DISPLAY;

    auto* eglInterface = context->nativeInterface<QNativeInterface::QEGLContext>();
    return eglInterface ? eglInterface->display() : EGL_NO_DISPLAY;
}

template<typename T>
T resolveEglProc(const char* name)
{
    return reinterpret_cast<T>(eglGetProcAddress(name));
}

struct EglSyncApi {
    EglCreateSyncKhr createSync { nullptr };
    EglWaitSyncKhr waitSync { nullptr };
    EglDestroySyncKhr destroySync { nullptr };
};

const EglSyncApi& eglSyncApi()
{
    static const EglSyncApi api {
        resolveEglProc<EglCreateSyncKhr>("eglCreateSyncKHR"),
        resolveEglProc<EglWaitSyncKhr>("eglWaitSyncKHR"),
        resolveEglProc<EglDestroySyncKhr>("eglDestroySyncKHR"),
    };
    return api;
}

bool enqueueEglAcquireWait(EGLDisplay display, int& syncFd, const QString& context)
{
    if (syncFd < 0 || display == EGL_NO_DISPLAY) {
        qCWarning(lcWallpaperKde,
                  "invalid EGL acquire fence display=%p fd=%d context=%s",
                  static_cast<void*>(display),
                  syncFd,
                  qPrintable(context));
        return false;
    }

    /*
     * Waywallen wraps each acquire sync_file in an
     * EGL_SYNC_NATIVE_FENCE_ANDROID and calls eglWaitSyncKHR instead of
     * blocking its protocol thread in poll(). Do the same GPU-side wait here,
     * but from the BeforeSynchronizingStage render job where Qt's GL context is
     * guaranteed current. EGL_KHR_wait_sync explicitly requires a current
     * client context; issuing this from Vivid's GUI-thread socket callback
     * would otherwise fail with EGL_BAD_MATCH under Qt's threaded render loop.
     */
    if (eglGetCurrentContext() == EGL_NO_CONTEXT || eglGetCurrentDisplay() != display) {
        qCWarning(lcWallpaperKde,
                  "EGL acquire fence reached render job without the matching current context "
                  "display=%p current-display=%p context=%s",
                  static_cast<void*>(display),
                  static_cast<void*>(eglGetCurrentDisplay()),
                  qPrintable(context));
        return false;
    }

    const EglSyncApi& api = eglSyncApi();
    if (!api.createSync || !api.waitSync || !api.destroySync) {
        qCWarning(lcWallpaperKde,
                  "EGL native-fence server-wait functions are unavailable context=%s",
                  qPrintable(context));
        return false;
    }

    const EGLint attrs[] = {
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
        syncFd,
        EGL_NONE,
    };
    const EGLSyncKHR sync =
        api.createSync(display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
    if (sync == EGL_NO_SYNC_KHR) {
        const EGLint error = eglGetError();
        qCWarning(lcWallpaperKde,
                  "eglCreateSyncKHR(acquire) failed egl=0x%x context=%s",
                  static_cast<unsigned>(error),
                  qPrintable(context));
        return false;
    }

    /* A successful eglCreateSyncKHR transfers ownership of the native fence
     * fd to EGL, irrespective of whether the following server wait succeeds. */
    syncFd = -1;
    const EGLint waitResult = api.waitSync(display, sync, 0);
    const EGLint waitError = waitResult == EGL_TRUE ? EGL_SUCCESS : eglGetError();
    (void)api.destroySync(display, sync);
    if (waitResult != EGL_TRUE) {
        qCWarning(lcWallpaperKde,
                  "eglWaitSyncKHR(acquire) failed egl=0x%x context=%s",
                  static_cast<unsigned>(waitError),
                  qPrintable(context));
        return false;
    }
    return true;
}

bool extensionListHasToken(const char* extensions, const char* token)
{
    if (!extensions || !token || !*token)
        return false;

    const char* cursor = extensions;
    const size_t tokenLen = std::strlen(token);
    while ((cursor = std::strstr(cursor, token)) != nullptr) {
        const bool startsToken = cursor == extensions || cursor[-1] == ' ';
        const char after = cursor[tokenLen];
        const bool endsToken = after == '\0' || after == ' ';
        if (startsToken && endsToken)
            return true;
        cursor += tokenLen;
    }

    return false;
}

bool eglDisplayHasExtension(EGLDisplay display, const char* extension)
{
    return display != EGL_NO_DISPLAY &&
        extensionListHasToken(eglQueryString(display, EGL_EXTENSIONS), extension);
}

constexpr quint32 DrmFormatXrgb8888 = 0x34325258u; // 'XR24'
constexpr quint32 DrmFormatArgb8888 = 0x34325241u; // 'AR24'
constexpr quint32 DrmFormatXbgr8888 = 0x34324258u; // 'XB24'
constexpr quint32 DrmFormatAbgr8888 = 0x34324241u; // 'AB24'

bool isVividRgbaFourcc(quint32 fourcc)
{
    switch (fourcc) {
    case DrmFormatXrgb8888:
    case DrmFormatArgb8888:
    case DrmFormatXbgr8888:
    case DrmFormatAbgr8888:
        return true;
    default:
        return false;
    }
}

QJsonObject dmabufModifierEntry(quint32 fourcc, quint64 modifier, quint32 planeCount)
{
    return QJsonObject {
        { QStringLiteral("fourcc"), static_cast<qint64>(fourcc) },
        { QStringLiteral("modifier"), QString::number(modifier) },
        { QStringLiteral("planeCount"), static_cast<int>(planeCount) },
    };
}

const char* sceneGraphApiName(QSGRendererInterface::GraphicsApi api)
{
    if (api == QSGRendererInterface::OpenGL)
        return "OpenGL";
    if (api == QSGRendererInterface::Vulkan)
        return "Vulkan";
    return "Other";
}

QString jsonStringMember(const QJsonObject& object,
                         std::initializer_list<QString> names)
{
    for (const QString& name : names) {
        const QJsonValue value = object.value(name);
        if (value.isString())
            return value.toString().trimmed();
    }
    return {};
}

QString jsonStringMember(const QJsonObject& object, const QString& primary)
{
    return jsonStringMember(object, { primary });
}

QString normalizeGpuText(const QString& value)
{
    return value.trimmed().toLower();
}

QString normalizedRenderNode(const QString& path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
        return {};

    const QString canonical = QFileInfo(trimmed).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(trimmed) : canonical;
}

QString uuidBytesToHex(const quint8 uuid[16])
{
    QString text;
    text.reserve(32);
    for (int i = 0; i < 16; i++)
        text += QStringLiteral("%1").arg(uuid[i], 2, 16, QLatin1Char('0'));
    return text;
}

QString dlErrorMessage()
{
    const char* error = ::dlerror();
    return QString::fromLocal8Bit(error ? error : "unknown dynamic-loader error");
}

template<typename T>
T resolveLibrarySymbol(void* library, const char* name)
{
    ::dlerror();
    void* symbol = ::dlsym(library, name);
    if (!symbol)
        return nullptr;
    return reinterpret_cast<T>(symbol);
}

struct VulkanProcTable {
    ~VulkanProcTable()
    {
        if (library)
            ::dlclose(library);
    }

    bool loadLoader(QString* diagnostics)
    {
        library = ::dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!library)
            library = ::dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            if (diagnostics) {
                *diagnostics += QStringLiteral(" Vulkan UUID probe dlopen failed: %1;")
                                    .arg(dlErrorMessage());
            }
            return false;
        }

        getInstanceProcAddr =
            resolveLibrarySymbol<PFN_vkGetInstanceProcAddr>(library, "vkGetInstanceProcAddr");
        if (!getInstanceProcAddr) {
            if (diagnostics) {
                *diagnostics += QStringLiteral(
                    " Vulkan UUID probe missing vkGetInstanceProcAddr: %1;")
                                    .arg(dlErrorMessage());
            }
            return false;
        }

        createInstance =
            reinterpret_cast<PFN_vkCreateInstance>(
                getInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
        if (!createInstance)
            createInstance = resolveLibrarySymbol<PFN_vkCreateInstance>(library, "vkCreateInstance");
        if (!createInstance) {
            if (diagnostics) {
                *diagnostics += QStringLiteral(" Vulkan UUID probe missing vkCreateInstance: %1;")
                                    .arg(dlErrorMessage());
            }
            return false;
        }

        return true;
    }

    bool loadInstanceFunctions(VkInstance instance, QString* diagnostics)
    {
        /*
         * Some Qt/Plasma build configurations include Vulkan headers with
         * VK_NO_PROTOTYPES, so this file must not rely on link-time function
         * declarations. Resolve the tiny probe surface explicitly from the
         * loader and keep the failure diagnostic in dmabufCaps instead of
         * silently advertising an incomplete device identity.
         */
        destroyInstance =
            reinterpret_cast<PFN_vkDestroyInstance>(
                getInstanceProcAddr(instance, "vkDestroyInstance"));
        enumeratePhysicalDevices =
            reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
                getInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
        getPhysicalDeviceProperties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));

        if (!destroyInstance)
            destroyInstance =
                resolveLibrarySymbol<PFN_vkDestroyInstance>(library, "vkDestroyInstance");
        if (!enumeratePhysicalDevices) {
            enumeratePhysicalDevices =
                resolveLibrarySymbol<PFN_vkEnumeratePhysicalDevices>(
                    library,
                    "vkEnumeratePhysicalDevices");
        }
        if (!getPhysicalDeviceProperties2) {
            getPhysicalDeviceProperties2 =
                reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                    getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2KHR"));
        }

        if (destroyInstance && enumeratePhysicalDevices && getPhysicalDeviceProperties2)
            return true;

        if (diagnostics) {
            *diagnostics += QStringLiteral(
                " Vulkan UUID probe missing instance functions destroy=%1 enumerate=%2 props2=%3;")
                                .arg(destroyInstance ? 1 : 0)
                                .arg(enumeratePhysicalDevices ? 1 : 0)
                                .arg(getPhysicalDeviceProperties2 ? 1 : 0);
        }
        return false;
    }

    void* library = nullptr;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkCreateInstance createInstance = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 getPhysicalDeviceProperties2 = nullptr;
};

QString readSysfsText(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll()).trimmed();
}

QString vendorFromIdText(const QString& text)
{
    bool ok = false;
    const quint32 id = text.trimmed().toUInt(&ok, 0);
    if (!ok)
        return {};

    switch (id) {
    case 0x10de:
        return QStringLiteral("nvidia");
    case 0x8086:
        return QStringLiteral("intel");
    case 0x1002:
    case 0x1022:
        return QStringLiteral("amd");
    default:
        return {};
    }
}

QString vendorFromGlString(const char* value)
{
    const QString text = QString::fromLatin1(value ? value : "").toLower();
    if (text.contains(QStringLiteral("nvidia")))
        return QStringLiteral("nvidia");
    if (text.contains(QStringLiteral("intel")))
        return QStringLiteral("intel");
    if (text.contains(QStringLiteral("amd")) || text.contains(QStringLiteral("radeon")) ||
        text.contains(QStringLiteral("ati")))
        return QStringLiteral("amd");
    return {};
}

void populateIdentityFromRenderNode(GpuIdentity& identity)
{
    const QString renderNode = normalizedRenderNode(identity.renderNode);
    if (renderNode.isEmpty())
        return;

    struct stat st {};
    if (::stat(renderNode.toLocal8Bit().constData(), &st) != 0) {
        identity.diagnostics += QStringLiteral(" stat(%1) failed: %2;")
                                    .arg(renderNode, formatErrno(errno));
        return;
    }

    const QString devicePath =
        QStringLiteral("/sys/dev/char/%1:%2/device").arg(major(st.st_rdev)).arg(minor(st.st_rdev));
    const QFileInfo deviceInfo(devicePath);
    const QString pciPath = deviceInfo.canonicalFilePath();
    if (!pciPath.isEmpty())
        identity.pciAddress = normalizeGpuText(QFileInfo(pciPath).fileName());

    if (identity.vendor.isEmpty())
        identity.vendor = vendorFromIdText(readSysfsText(devicePath + QStringLiteral("/vendor")));
}

QString renderNodeForDrmIds(qint64 renderMajor, qint64 renderMinor)
{
    for (quint32 minorId = 128; minorId <= 192; minorId++) {
        const QString path = QStringLiteral("/dev/dri/renderD%1").arg(minorId);
        struct stat st {};
        if (::stat(path.toLocal8Bit().constData(), &st) != 0 || !S_ISCHR(st.st_mode))
            continue;
        if (static_cast<qint64>(major(st.st_rdev)) == renderMajor &&
            static_cast<qint64>(minor(st.st_rdev)) == renderMinor) {
            return normalizedRenderNode(path);
        }
    }
    return {};
}

void populateIdentityUuidFromVulkan(GpuIdentity& identity)
{
    if (identity.renderNode.isEmpty())
        return;

    VulkanProcTable vk;
    if (!vk.loadLoader(&identity.diagnostics))
        return;

    const VkApplicationInfo appInfo {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = nullptr,
        .pApplicationName = "Vivid KDE consumer GPU identity probe",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "VividDisplayKde",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_1,
    };
    const VkInstanceCreateInfo createInfo {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = nullptr,
        .enabledExtensionCount = 0,
        .ppEnabledExtensionNames = nullptr,
    };

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vk.createInstance(&createInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        identity.diagnostics += QStringLiteral(" vkCreateInstance UUID probe failed result=%1;")
                                    .arg(static_cast<int>(result));
        return;
    }
    if (!vk.loadInstanceFunctions(instance, &identity.diagnostics)) {
        if (vk.destroyInstance)
            vk.destroyInstance(instance, nullptr);
        return;
    }

    uint32_t count = 0;
    result = vk.enumeratePhysicalDevices(instance, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
        vk.destroyInstance(instance, nullptr);
        return;
    }

    QVector<VkPhysicalDevice> devices(static_cast<int>(count));
    result = vk.enumeratePhysicalDevices(instance, &count, devices.data());
    if (result != VK_SUCCESS) {
        vk.destroyInstance(instance, nullptr);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceDrmPropertiesEXT drmProps {};
        drmProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceIDProperties idProps {};
        idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        idProps.pNext = &drmProps;
        VkPhysicalDeviceProperties2 props {};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &idProps;
        vk.getPhysicalDeviceProperties2(devices[static_cast<int>(i)], &props);
        if (!drmProps.hasRender)
            continue;

        const QString candidate =
            renderNodeForDrmIds(drmProps.renderMajor, drmProps.renderMinor);
        if (candidate != identity.renderNode)
            continue;

        identity.deviceUuid = uuidBytesToHex(idProps.deviceUUID);
        identity.driverUuid = uuidBytesToHex(idProps.driverUUID);
        break;
    }

    vk.destroyInstance(instance, nullptr);
    if (identity.deviceUuid.isEmpty()) {
        identity.diagnostics += QStringLiteral(
            " Vulkan UUID probe found no physical device for render node %1;")
                                    .arg(identity.renderNode);
    }
}

GpuIdentity currentPlasmaGpuIdentity(EGLDisplay eglDisplay)
{
    GpuIdentity identity;
    const auto queryDisplayAttrib =
        resolveEglProc<EglQueryDisplayAttribExt>("eglQueryDisplayAttribEXT");
    const auto queryDeviceString =
        resolveEglProc<EglQueryDeviceStringExt>("eglQueryDeviceStringEXT");

    /*
     * plasmashell fixes its EGL/GL device at process startup. KDE currently
     * imports producer DMA-BUFs inside that already-selected context, so this
     * identity is the only safe target for in-process imports. The EGL device
     * extensions give us the exact render node on Mesa and on drivers that
     * expose EGL_EXT_device_query/EGL_EXT_device_drm_render_node; GL strings are
     * only a vendor-level fallback for drivers that hide the DRM node.
     */
    if (eglDisplay != EGL_NO_DISPLAY && queryDisplayAttrib && queryDeviceString) {
        EGLAttrib deviceAttrib = 0;
        if (queryDisplayAttrib(eglDisplay, EGL_DEVICE_EXT, &deviceAttrib) && deviceAttrib != 0) {
            const auto device = reinterpret_cast<EGLDeviceEXT>(deviceAttrib);
#ifdef EGL_DRM_RENDER_NODE_FILE_EXT
            if (const char* renderNode = queryDeviceString(device, EGL_DRM_RENDER_NODE_FILE_EXT))
                identity.renderNode = QString::fromLocal8Bit(renderNode).trimmed();
#else
            identity.diagnostics += QStringLiteral(" EGL_DRM_RENDER_NODE_FILE_EXT not available at build time;");
#endif
#ifdef EGL_DRM_DEVICE_FILE_EXT
            if (identity.renderNode.isEmpty()) {
                if (const char* drmNode = queryDeviceString(device, EGL_DRM_DEVICE_FILE_EXT))
                    identity.renderNode = QString::fromLocal8Bit(drmNode).trimmed();
            }
#endif
        } else {
            identity.diagnostics += QStringLiteral(" eglQueryDisplayAttribEXT(EGL_DEVICE_EXT) failed;");
        }
    } else {
        identity.diagnostics += QStringLiteral(" EGL device query functions unavailable;");
    }

    identity.renderNode = normalizedRenderNode(identity.renderNode);
    populateIdentityFromRenderNode(identity);
    populateIdentityUuidFromVulkan(identity);

    if (identity.vendor.isEmpty()) {
        identity.vendor = vendorFromGlString(
            reinterpret_cast<const char*>(::glGetString(GL_VENDOR)));
    }
    if (identity.vendor.isEmpty()) {
        identity.vendor = vendorFromGlString(
            reinterpret_cast<const char*>(::glGetString(GL_RENDERER)));
    }

    return identity;
}

QString describeGpuIdentity(const GpuIdentity& identity)
{
    return QStringLiteral("render-node=%1 vendor=%2 pci=%3")
        .arg(identity.renderNode.isEmpty() ? QStringLiteral("(unknown)") : identity.renderNode,
             identity.vendor.isEmpty() ? QStringLiteral("(unknown)") : identity.vendor,
             identity.pciAddress.isEmpty() ? QStringLiteral("(unknown)") : identity.pciAddress);
}

std::optional<quint32> probeGbmModifierPlaneCount(quint32 fourcc,
                                                  quint64 modifier,
                                                  const GpuIdentity& identity,
                                                  QString* diagnostics)
{
#ifndef VIVID_KDE_HAVE_GBM
    Q_UNUSED(fourcc)
    Q_UNUSED(modifier)
    Q_UNUSED(identity)
    if (diagnostics)
        *diagnostics += QStringLiteral(" GBM plane probe disabled: libgbm not available;");
    return std::nullopt;
#else
    if (identity.renderNode.isEmpty()) {
        if (diagnostics)
            *diagnostics += QStringLiteral(" GBM plane probe skipped: render node unknown;");
        return std::nullopt;
    }

    const QByteArray renderNode = identity.renderNode.toLocal8Bit();
    const int fd = ::open(renderNode.constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (diagnostics) {
            *diagnostics += QStringLiteral(" GBM plane probe open(%1) failed: %2;")
                                .arg(identity.renderNode, formatErrno(errno));
        }
        return std::nullopt;
    }

    gbm_device* device = gbm_create_device(fd);
    if (!device) {
        if (diagnostics) {
            *diagnostics += QStringLiteral(" GBM plane probe gbm_create_device(%1) failed;")
                                .arg(identity.renderNode);
        }
        ::close(fd);
        return std::nullopt;
    }

    const uint64_t modifiers[] = { static_cast<uint64_t>(modifier) };
    gbm_bo* bo = gbm_bo_create_with_modifiers2(device,
                                               64,
                                               64,
                                               fourcc,
                                               modifiers,
                                               1,
                                               GBM_BO_USE_RENDERING);
    if (!bo) {
        if (diagnostics) {
            *diagnostics += QStringLiteral(" GBM plane probe failed fourcc=0x%1 modifier=0x%2;")
                                .arg(fourcc, 8, 16, QLatin1Char('0'))
                                .arg(modifier, 16, 16, QLatin1Char('0'));
        }
        gbm_device_destroy(device);
        ::close(fd);
        return std::nullopt;
    }

    const int planes = gbm_bo_get_plane_count(bo);
    gbm_bo_destroy(bo);
    gbm_device_destroy(device);
    ::close(fd);

    if (planes <= 0) {
        if (diagnostics) {
            *diagnostics += QStringLiteral(" GBM plane probe returned invalid plane count "
                                           "fourcc=0x%1 modifier=0x%2 planes=%3;")
                                .arg(fourcc, 8, 16, QLatin1Char('0'))
                                .arg(modifier, 16, 16, QLatin1Char('0'))
                                .arg(planes);
        }
        return std::nullopt;
    }

    return static_cast<quint32>(planes);
#endif
}

bool probeLinearFallbackImport(EGLDisplay eglDisplay,
                               quint32 fourcc,
                               const GpuIdentity& identity,
                               QString* diagnostics)
{
#ifndef VIVID_KDE_HAVE_GBM
    Q_UNUSED(eglDisplay)
    Q_UNUSED(fourcc)
    Q_UNUSED(identity)
    if (diagnostics)
        *diagnostics += QStringLiteral(" LINEAR fallback import probe disabled: libgbm not available;");
    return false;
#else
    if (eglDisplay == EGL_NO_DISPLAY) {
        if (diagnostics)
            *diagnostics += QStringLiteral(" LINEAR fallback import probe skipped: EGL display unavailable;");
        return false;
    }
    if (identity.renderNode.isEmpty()) {
        if (diagnostics)
            *diagnostics += QStringLiteral(" LINEAR fallback import probe skipped: render node unknown;");
        return false;
    }

    const auto createImage = resolveEglProc<EglCreateImageKhr>("eglCreateImageKHR");
    const auto destroyImage = resolveEglProc<EglDestroyImageKhr>("eglDestroyImageKHR");
    if (!createImage || !destroyImage) {
        if (diagnostics)
            *diagnostics += QStringLiteral(" LINEAR fallback import probe skipped: EGL image import functions unavailable;");
        return false;
    }

    const QByteArray renderNode = identity.renderNode.toLocal8Bit();
    const int fd = ::open(renderNode.constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (diagnostics) {
            *diagnostics += QStringLiteral(" LINEAR fallback import probe open(%1) failed: %2;")
                                .arg(identity.renderNode, formatErrno(errno));
        }
        return false;
    }

    gbm_device* device = gbm_create_device(fd);
    if (!device) {
        if (diagnostics) {
            *diagnostics += QStringLiteral(" LINEAR fallback import probe gbm_create_device(%1) failed;")
                                .arg(identity.renderNode);
        }
        ::close(fd);
        return false;
    }

    const uint64_t modifiers[] = { static_cast<uint64_t>(DrmFormatModLinear) };
    gbm_bo* bo = gbm_bo_create_with_modifiers2(device,
                                               64,
                                               64,
                                               fourcc,
                                               modifiers,
                                               1,
                                               GBM_BO_USE_RENDERING);
    if (!bo) {
        if (diagnostics) {
            *diagnostics += QStringLiteral(" LINEAR fallback import probe GBM allocation failed fourcc=0x%1;")
                                .arg(fourcc, 8, 16, QLatin1Char('0'));
        }
        gbm_device_destroy(device);
        ::close(fd);
        return false;
    }

    int boFd = gbm_bo_get_fd(bo);
    if (boFd < 0) {
        if (diagnostics) {
            *diagnostics += QStringLiteral(" LINEAR fallback import probe gbm_bo_get_fd failed fourcc=0x%1: %2;")
                                .arg(fourcc, 8, 16, QLatin1Char('0'))
                                .arg(formatErrno(errno));
        }
        gbm_bo_destroy(bo);
        gbm_device_destroy(device);
        ::close(fd);
        return false;
    }

    /*
     * This is the capability proof behind the fallback LINEAR tuple. The
     * producer only chooses CompatLinear when the consumer says a cross-GPU
     * shadow-copy import is possible; advertising that without creating an
     * EGLImage once makes the negotiation lie. Keep the smoke test intentionally
     * tiny and implicit-modifier, matching the runtime import path used for
     * LINEAR/INVALID buffers.
     */
    const EGLint attrs[] = {
        EGL_WIDTH, 64,
        EGL_HEIGHT, 64,
        EGL_LINUX_DRM_FOURCC_EXT, static_cast<EGLint>(fourcc),
        EGL_DMA_BUF_PLANE0_FD_EXT, boFd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<EGLint>(gbm_bo_get_stride(bo)),
        EGL_NONE,
    };
    EGLImageKHR image =
        createImage(eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    const EGLint eglError = image == EGL_NO_IMAGE_KHR ? eglGetError() : EGL_SUCCESS;
    if (image != EGL_NO_IMAGE_KHR)
        destroyImage(eglDisplay, image);
    ::close(boFd);
    gbm_bo_destroy(bo);
    gbm_device_destroy(device);
    ::close(fd);

    if (image == EGL_NO_IMAGE_KHR) {
        if (diagnostics) {
            *diagnostics += QStringLiteral(" LINEAR fallback import probe eglCreateImageKHR failed fourcc=0x%1 egl=0x%2;")
                                .arg(fourcc, 8, 16, QLatin1Char('0'))
                                .arg(static_cast<uint>(eglError), 0, 16);
        }
        return false;
    }
    return true;
#endif
}

void appendFourccOnce(QJsonArray& array, quint32 fourcc)
{
    const qint64 value = static_cast<qint64>(fourcc);
    for (const QJsonValue& existing : array) {
        if (existing.toInteger() == value)
            return;
    }
    array.append(value);
}

bool appendSmokeTestedLinearFallbackCaps(EGLDisplay eglDisplay,
                                         const GpuIdentity& identity,
                                         QJsonArray& fourccs,
                                         QJsonArray& modifiers,
                                         QJsonArray& implicitLinearFourccs,
                                         QString* diagnostics)
{
    const quint32 fallbackFormats[] = {
        DrmFormatAbgr8888,
        DrmFormatXrgb8888,
    };
    bool appended = false;
    for (quint32 fourcc : fallbackFormats) {
        if (!probeLinearFallbackImport(eglDisplay, fourcc, identity, diagnostics))
            continue;
        appendFourccOnce(fourccs, fourcc);
        appendFourccOnce(implicitLinearFourccs, fourcc);
        modifiers.append(dmabufModifierEntry(fourcc, DrmFormatModLinear, 1));
        appended = true;
    }
    return appended;
}

QJsonObject buildEglDmaBufCaps(EGLDisplay eglDisplay)
{
    const GpuIdentity identity = currentPlasmaGpuIdentity(eglDisplay);
    const auto queryFormats = resolveEglProc<EglQueryDmaBufFormatsExt>(
        "eglQueryDmaBufFormatsEXT");
    const auto queryModifiers = resolveEglProc<EglQueryDmaBufModifiersExt>(
        "eglQueryDmaBufModifiersEXT");

    QJsonArray fourccs;
    QJsonArray modifiers;
    QJsonArray implicitLinearFourccs;
    QString probeDiagnostics = identity.diagnostics;

    QString probeMode = QStringLiteral("unprobed");
    bool probeUnavailable = true;
    if (eglDisplay != EGL_NO_DISPLAY && queryFormats && queryModifiers) {
        EGLint formatCount = 0;
        if (queryFormats(eglDisplay, 0, nullptr, &formatCount) && formatCount > 0) {
            QVector<EGLint> queriedFormats(formatCount);
            if (queryFormats(eglDisplay, formatCount, queriedFormats.data(), &formatCount)) {
                probeMode = QStringLiteral("egl-query");
                probeUnavailable = false;
                for (EGLint i = 0; i < formatCount; i++) {
                    const quint32 fourcc = static_cast<quint32>(queriedFormats[i]);
                    if (!isVividRgbaFourcc(fourcc))
                        continue;

                    /*
                     * Match Waywallen's EGL contract: a fourcc is only a 2D
                     * texture capability when the driver either has no explicit
                     * modifier list (implicit LINEAR) or returns at least one
                     * non-external-only modifier. Merely appearing in
                     * eglQueryDmaBufFormatsEXT is not enough for GL_TEXTURE_2D.
                     */
                    EGLint modifierCount = 0;
                    if (!queryModifiers(eglDisplay,
                                        queriedFormats[i],
                                        0,
                                        nullptr,
                                        nullptr,
                                        &modifierCount)) {
                        probeDiagnostics += QStringLiteral(
                                                " EGL modifier count query failed fourcc=0x%1;")
                                                .arg(fourcc, 8, 16, QLatin1Char('0'));
                        continue;
                    }

                    if (modifierCount <= 0) {
                        appendFourccOnce(fourccs, fourcc);
                        appendFourccOnce(implicitLinearFourccs, fourcc);
                        continue;
                    }

                    QVector<EGLuint64KHR> queriedModifiers(modifierCount);
                    QVector<EGLBoolean> externalOnly(modifierCount);
                    if (!queryModifiers(eglDisplay,
                                        queriedFormats[i],
                                        modifierCount,
                                        queriedModifiers.data(),
                                        externalOnly.data(),
                                        &modifierCount)) {
                        probeDiagnostics += QStringLiteral(
                                                " EGL modifier list query failed fourcc=0x%1;")
                                                .arg(fourcc, 8, 16, QLatin1Char('0'));
                        continue;
                    }

                    for (EGLint modIndex = 0; modIndex < modifierCount; modIndex++) {
                        if (externalOnly[modIndex])
                            continue;
                        const quint64 modifier =
                            static_cast<quint64>(queriedModifiers[modIndex]);

                        /*
                         * EGL tells us which modifiers Plasma's current GL
                         * context can import, but the waywallen-style
                         * negotiation tuple also needs plane_count. GBM is the
                         * narrowest local probe here: allocate a tiny BO on the
                         * same render node, ask the driver for its plane count,
                         * then immediately destroy it. A failed probe falls
                         * back to 1 so unknown multi-plane modifiers are never
                         * selected accidentally by the strict tuple
                         * intersection.
                         */
                        const quint32 planeCount =
                            probeGbmModifierPlaneCount(fourcc, modifier, identity, &probeDiagnostics)
                                .value_or(1);
                        appendFourccOnce(fourccs, fourcc);
                        modifiers.append(dmabufModifierEntry(fourcc,
                                                             modifier,
                                                             planeCount));
                    }
                }
            }
        }
    }

    QJsonArray memoryHints = hostVisibleMemoryHints();
    if (!implicitLinearFourccs.isEmpty())
        memoryHints.append(QString::fromLatin1(MemoryImplicitLinear));
    if (fourccs.isEmpty()) {
        const bool fallbackAdded =
            appendSmokeTestedLinearFallbackCaps(eglDisplay,
                                               identity,
                                               fourccs,
                                               modifiers,
                                               implicitLinearFourccs,
                                               &probeDiagnostics);
        if (fallbackAdded) {
            probeMode = QStringLiteral("smoke-tested-linear-fallback");
            memoryHints.append(QStringLiteral("implicit-linear"));
            probeDiagnostics += QStringLiteral(
                " EGL DMA-BUF query produced no importable RGBA tuples; advertising only LINEAR formats that passed an EGLImage import smoke test;");
        } else {
            if (probeMode == QStringLiteral("unprobed") || probeMode == QStringLiteral("probe-empty"))
                probeMode = QString::fromLatin1(probeUnavailable ? "probe-unavailable" : "probe-empty");
            probeDiagnostics += QStringLiteral(
                " no LINEAR fallback advertised because EGLImage import smoke test did not prove support;");
        }
    }
    return QJsonObject {
        { QStringLiteral("version"), 3 },
        { QStringLiteral("backend"), QString::fromLatin1(BackendKdeEgl) },
        { QStringLiteral("probe"), probeMode },
        { QStringLiteral("relayModes"), shadowCopyRelayModes() },
        { QStringLiteral("renderNode"), identity.renderNode },
        { QStringLiteral("deviceUuid"), identity.deviceUuid },
        { QStringLiteral("driverUuid"), identity.driverUuid },
        { QStringLiteral("vendor"), identity.vendor },
        { QStringLiteral("pciAddress"), identity.pciAddress },
        { QStringLiteral("fourccs"), fourccs },
        { QStringLiteral("modifiers"), modifiers },
        { QStringLiteral("implicitLinearFourccs"), implicitLinearFourccs },
        { QStringLiteral("memoryHints"), memoryHints },
        { QStringLiteral("syncCaps"), syncCaps() },
        { QStringLiteral("colorCaps"), colorCaps() },
        { QStringLiteral("extentMax"), unlimitedExtentCaps() },
        { QStringLiteral("textureTarget"), QString::fromLatin1(TextureTargetGl2D) },
        { QStringLiteral("skipsExternalOnlyModifiers"), true },
        { QStringLiteral("diagnostics"), probeDiagnostics },
    };
}

struct VulkanCapsAccumulator {
    QJsonArray fourccs;
    QJsonArray modifiers;
};

void appendVulkanModifierCap(uint32_t fourcc, uint64_t modifier, uint32_t planeCount, void* userData)
{
    auto* caps = static_cast<VulkanCapsAccumulator*>(userData);
    if (!caps)
        return;
    appendFourccOnce(caps->fourccs, fourcc);
    caps->modifiers.append(dmabufModifierEntry(fourcc, modifier, planeCount));
}

QJsonObject buildEmptyVulkanDmaBufCaps(const QString& diagnostics)
{
    return QJsonObject {
        { QStringLiteral("version"), 3 },
        { QStringLiteral("backend"), QString::fromLatin1(BackendKdeVulkan) },
        { QStringLiteral("probe"), QStringLiteral("vulkan-unavailable") },
        { QStringLiteral("relayModes"), shadowCopyRelayModes() },
        { QStringLiteral("fourccs"), QJsonArray {} },
        { QStringLiteral("modifiers"), QJsonArray {} },
        { QStringLiteral("implicitLinearFourccs"), QJsonArray {} },
        { QStringLiteral("memoryHints"), hostVisibleMemoryHints() },
        { QStringLiteral("syncCaps"), syncCaps() },
        { QStringLiteral("colorCaps"), colorCaps() },
        { QStringLiteral("extentMax"), unlimitedExtentCaps() },
        { QStringLiteral("textureTarget"), QString::fromLatin1(TextureTargetVulkanShadow) },
        { QStringLiteral("diagnostics"), diagnostics },
    };
}

QJsonObject buildUnsupportedDmaBufCaps(const QString& diagnostics)
{
    return QJsonObject {
        { QStringLiteral("version"), 3 },
        { QStringLiteral("backend"), QString::fromLatin1(BackendKdeUnsupported) },
        { QStringLiteral("probe"), QStringLiteral("unsupported-scene-graph") },
        { QStringLiteral("relayModes"), QJsonArray {} },
        { QStringLiteral("fourccs"), QJsonArray {} },
        { QStringLiteral("modifiers"), QJsonArray {} },
        { QStringLiteral("implicitLinearFourccs"), QJsonArray {} },
        { QStringLiteral("memoryHints"), QJsonArray {} },
        { QStringLiteral("syncCaps"), syncCaps() },
        { QStringLiteral("colorCaps"), colorCaps() },
        { QStringLiteral("extentMax"), unlimitedExtentCaps() },
        { QStringLiteral("textureTarget"), QStringLiteral("none") },
        { QStringLiteral("diagnostics"), diagnostics },
    };
}

QJsonObject buildVulkanDmaBufCaps(const ww_vk_backend_t* backend)
{
    if (!backend || !backend->loaded) {
        return buildEmptyVulkanDmaBufCaps(
            QStringLiteral("Qt scene graph selected Vulkan but Vivid could not bind the Qt Vulkan backend; refusing to advertise EGL caps for a Vulkan scene graph."));
    }

    VulkanCapsAccumulator caps;
    QString diagnostics;
    /* Match Waywallen's KDE Vulkan consumer exactly. Imported producer
     * images are copied as TRANSFER_SRC into a Qt-device-local shadow; keep
     * SAMPLED in the probe mask because some drivers only expose the usable
     * modifier layout when both feature bits are requested. */
    const quint32 wantFeatures =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    const int capsRc = ww_vk_query_format_caps(backend,
                                               wantFeatures,
                                               appendVulkanModifierCap,
                                               &caps);
    QString probe = QStringLiteral("vulkan-query");
    if (capsRc != 0) {
        probe = QStringLiteral("vulkan-query-failed");
        diagnostics += QStringLiteral(" ww_vk_query_format_caps failed rc=%1;").arg(capsRc);
    } else if (caps.modifiers.isEmpty()) {
        probe = QStringLiteral("vulkan-query-empty");
        diagnostics += QStringLiteral(" Vulkan probe returned no importable RGBA modifier tuples;");
    }

    quint8 deviceUuid[16] = {};
    quint8 driverUuid[16] = {};
    QString deviceUuidText;
    QString driverUuidText;
    const int uuidRc = ww_vk_query_device_uuid(backend, deviceUuid, driverUuid);
    if (uuidRc == 0) {
        deviceUuidText = uuidBytesToHex(deviceUuid);
        driverUuidText = uuidBytesToHex(driverUuid);
    } else {
        diagnostics += QStringLiteral(" ww_vk_query_device_uuid failed rc=%1;").arg(uuidRc);
    }

    quint32 renderMajor = 0;
    quint32 renderMinor = 0;
    QString renderNode;
    const int drmRc = ww_vk_query_drm_render_node(backend, &renderMajor, &renderMinor);
    if (drmRc == 0) {
        renderNode = renderNodeForDrmIds(renderMajor, renderMinor);
        if (renderNode.isEmpty()) {
            diagnostics += QStringLiteral(" Vulkan DRM identity %1:%2 did not map to a render node;")
                               .arg(renderMajor)
                               .arg(renderMinor);
        }
    } else {
        diagnostics += QStringLiteral(" ww_vk_query_drm_render_node failed rc=%1;").arg(drmRc);
    }

    QJsonArray memoryHints = hostVisibleMemoryHints();
    int hasDeviceLocal = 0;
    const int deviceLocalRc = ww_vk_query_supports_device_local(backend, &hasDeviceLocal);
    if (deviceLocalRc == 0 && hasDeviceLocal != 0) {
        memoryHints.append(QString::fromLatin1(MemoryDeviceLocal));
    } else if (deviceLocalRc != 0) {
        diagnostics += QStringLiteral(" ww_vk_query_supports_device_local failed rc=%1;")
                           .arg(deviceLocalRc);
    }

    return QJsonObject {
        { QStringLiteral("version"), 3 },
        { QStringLiteral("backend"), QString::fromLatin1(BackendKdeVulkan) },
        { QStringLiteral("probe"), probe },
        { QStringLiteral("relayModes"), shadowCopyRelayModes() },
        { QStringLiteral("renderNode"), renderNode },
        { QStringLiteral("deviceUuid"), deviceUuidText },
        { QStringLiteral("driverUuid"), driverUuidText },
        { QStringLiteral("drmRenderMajor"), static_cast<int>(renderMajor) },
        { QStringLiteral("drmRenderMinor"), static_cast<int>(renderMinor) },
        { QStringLiteral("fourccs"), caps.fourccs },
        { QStringLiteral("modifiers"), caps.modifiers },
        { QStringLiteral("implicitLinearFourccs"), QJsonArray {} },
        { QStringLiteral("memoryHints"), memoryHints },
        { QStringLiteral("syncCaps"), syncCaps() },
        { QStringLiteral("colorCaps"), colorCaps() },
        { QStringLiteral("extentMax"), unlimitedExtentCaps() },
        { QStringLiteral("textureTarget"), QString::fromLatin1(TextureTargetVulkanShadow) },
        { QStringLiteral("diagnostics"), diagnostics },
    };
}

QString bindBuffersGpuMismatchReason(const QString& renderNode,
                                     const QString& vendor,
                                     const QString& pciAddress)
{
    const GpuIdentity producer {
        normalizedRenderNode(renderNode),
        normalizeGpuText(vendor),
        normalizeGpuText(pciAddress),
        {},
        {},
        {},
    };
    const GpuIdentity plasma = currentPlasmaGpuIdentity(eglGetCurrentDisplay());

    if (!producer.renderNode.isEmpty() && !plasma.renderNode.isEmpty() &&
        producer.renderNode != plasma.renderNode) {
        return QStringLiteral("producer %1; Plasma %2")
            .arg(describeGpuIdentity(producer), describeGpuIdentity(plasma));
    }
    if (!producer.pciAddress.isEmpty() && !plasma.pciAddress.isEmpty() &&
        producer.pciAddress != plasma.pciAddress) {
        return QStringLiteral("producer %1; Plasma %2")
            .arg(describeGpuIdentity(producer), describeGpuIdentity(plasma));
    }
    if (!producer.vendor.isEmpty() && !plasma.vendor.isEmpty() &&
        producer.vendor != plasma.vendor) {
        return QStringLiteral("producer %1; Plasma %2")
            .arg(describeGpuIdentity(producer), describeGpuIdentity(plasma));
    }

    if ((!producer.renderNode.isEmpty() || !producer.vendor.isEmpty() || !producer.pciAddress.isEmpty()) &&
        plasma.renderNode.isEmpty() && plasma.vendor.isEmpty() && plasma.pciAddress.isEmpty()) {
        return {};
    }

    return {};
}
} // namespace

VividDisplay::VividDisplay(QQuickItem* parent): QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    vivid_display_recv_state_init(&m_recvState);

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &VividDisplay::onReconnectTimer);
    const auto socketPathChanged = [this](const QString&) {
        refreshSocketWatcher();
        if (QFileInfo::exists(effectiveSocketPath()))
            scheduleReconnect(0);
    };
    connect(&m_socketWatcher,
            &QFileSystemWatcher::directoryChanged,
            this,
            socketPathChanged);
    connect(&m_socketWatcher,
            &QFileSystemWatcher::fileChanged,
            this,
            socketPathChanged);
}

VividDisplay::~VividDisplay()
{
    closeTransport(false);

    FinalGpuCleanup* cleanup = takeFinalGpuCleanup();
    if (!cleanup)
        return;

    const bool onRenderThread = QThread::currentThread() != thread();
    if (onRenderThread || QOpenGLContext::currentContext()) {
        if (destroyFinalGpuCleanup(cleanup))
            delete cleanup;
        return;
    }

    QQuickWindow* quickWindow = window();
    if (quickWindow && quickWindow->isSceneGraphInitialized() && quickWindow->isExposed()) {
        quickWindow->scheduleRenderJob(
            new FunctionRenderJob([cleanup]() {
                if (VividDisplay::destroyFinalGpuCleanup(cleanup))
                    delete cleanup;
            }),
            QQuickWindow::NoStage);
        quickWindow->update();
        return;
    }

    /* Match Waywallen's failure policy: without a live render context there
     * is no safe thread on which to delete GL/Vulkan objects.  Keep the
     * cleanup payload allocated so the driver handles remain explicitly
     * accounted for until process exit instead of silently discarding them. */
    qCCritical(lcWallpaperKde,
               "no render context for final Vivid GPU cleanup; resources are retained until process exit");
}

void VividDisplay::componentComplete()
{
    QQuickItem::componentComplete();
    connect(this,
            &QQuickItem::windowChanged,
            this,
            &VividDisplay::onWindowChanged,
            Qt::UniqueConnection);
    refreshSocketWatcher();
    refreshOutputSnapshot();
    onWindowChanged(window());
}

void VividDisplay::setSocketPath(const QString& path)
{
    if (m_socketPath == path)
        return;
    m_socketPath = path;
    emit socketPathChanged();
    refreshSocketWatcher();
    requestReconnect();
}

void VividDisplay::setDisplayName(const QString& name)
{
    if (m_displayName == name)
        return;
    m_displayName = name;
    emit displayNameChanged();
    requestReconnect();
}

void VividDisplay::setScreenName(const QString& name)
{
    if (m_screenName == name)
        return;
    m_screenName = name;
    emit screenNameChanged();
    if (isComponentComplete())
        refreshOutputSnapshot();
    requestReconnect();
}

void VividDisplay::setInstanceId(const QString& id)
{
    if (m_instanceId == id)
        return;
    m_instanceId = id;
    emit instanceIdChanged();
    requestReconnect();
}

void VividDisplay::setConsumerOutputId(quint32 id)
{
    id = id == 0 ? 1 : std::min<quint32>(id, 0x7fffffffu);
    if (m_consumerOutputId == id)
        return;
    m_consumerOutputId = id;
    emit consumerOutputIdChanged();
    requestReconnect();
}

void VividDisplay::setMonitorIndex(quint32 index)
{
    if (m_monitorIndex == index)
        return;
    m_monitorIndex = index;
    emit monitorIndexChanged();
    requestReconnect();
}

void VividDisplay::setDisplayX(int value)
{
    if (m_displayX == value)
        return;
    m_displayX = value;
    geometryPropertyChanged();
}

void VividDisplay::setDisplayY(int value)
{
    if (m_displayY == value)
        return;
    m_displayY = value;
    geometryPropertyChanged();
}

void VividDisplay::setLogicalWidth(int value)
{
    value = std::max(1, value);
    if (m_logicalWidth == value)
        return;
    m_logicalWidth = value;
    geometryPropertyChanged();
}

void VividDisplay::setLogicalHeight(int value)
{
    value = std::max(1, value);
    if (m_logicalHeight == value)
        return;
    m_logicalHeight = value;
    geometryPropertyChanged();
}

void VividDisplay::setDisplayWidth(int value)
{
    value = std::max(1, value);
    if (m_displayWidth == value)
        return;
    m_displayWidth = value;
    geometryPropertyChanged();
}

void VividDisplay::setDisplayHeight(int value)
{
    value = std::max(1, value);
    if (m_displayHeight == value)
        return;
    m_displayHeight = value;
    geometryPropertyChanged();
}

void VividDisplay::setDisplayScale(qreal value)
{
    value = std::max<qreal>(1.0, value);
    if (qFuzzyCompare(m_displayScale, value))
        return;
    m_displayScale = value;
    geometryPropertyChanged();
}

void VividDisplay::setRefreshRateMhz(quint32 value)
{
    if (m_refreshRateMhz == value)
        return;
    m_refreshRateMhz = value;
    geometryPropertyChanged();
}

void VividDisplay::setAutoReconnect(bool enabled)
{
    if (m_autoReconnect == enabled)
        return;
    m_autoReconnect = enabled;
    emit autoReconnectChanged();
}

void VividDisplay::setMouseForwardEnabled(bool enabled)
{
    if (m_mouseForwardEnabled == enabled)
        return;
    m_mouseForwardEnabled = enabled;
    installOrRemoveEventFilter();
    emit mouseForwardEnabledChanged();
}

void VividDisplay::setWindowStateFlags(quint32 flags)
{
    if (m_windowStateFlags == flags)
        return;
    m_windowStateFlags = flags;
    emit windowStateFlagsChanged();
    sendWindowState();
}

void VividDisplay::setCoveredScreenNames(const QStringList& names)
{
    QStringList normalized;
    for (const QString& name : names) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty() && !normalized.contains(trimmed))
            normalized.append(trimmed);
    }
    if (m_coveredScreenNames == normalized)
        return;
    m_coveredScreenNames = normalized;
    emit coveredScreenNamesChanged();
}

void VividDisplay::setFocusedScreenName(const QString& name)
{
    const QString normalized = name.trimmed();
    if (m_focusedScreenName == normalized)
        return;
    m_focusedScreenName = normalized;
    emit focusedScreenNameChanged();
}

void VividDisplay::setWindowFacts(const QJsonArray& facts)
{
    if (m_windowFacts == facts)
        return;
    m_windowFacts = facts;
    emit windowFactsChanged();
    sendWindowState();
}

void VividDisplay::setMprisPlaybackFacts(const QJsonArray& players)
{
    bool playing = false;
    for (const QJsonValue& value : players) {
        const QJsonObject player = value.toObject();
        if (player.value(QStringLiteral("playbackStatus")).toString() ==
            QStringLiteral("Playing")) {
            playing = true;
            break;
        }
    }

    if (m_mprisPlaying == playing && m_mprisPlayers == players)
        return;

    /*
     * KDE has a separate media payload for title/artwork/audio visualization,
     * but playback-on-audio is evaluated from the window-state policy facts.
     * Keep the MPRIS player list cached here so every policy report carries the
     * same session-level audio facts that GNOME already sends.
     */
    m_mprisPlaying = playing;
    m_mprisPlayers = players;
    sendWindowState();
}

void VividDisplay::requestReconnect()
{
    if (!isComponentComplete())
        return;

    if (m_fd < 0 && !m_connecting) {
        scheduleReconnect(0);
        return;
    }

    closeTransport(true);
    scheduleReconnect(100);
}

void VividDisplay::onWindowChanged(QQuickWindow*)
{
    installOrRemoveEventFilter();
    refreshOutputSnapshot();
    armSceneGraphReadyConnection();
    scheduleReconnect(0);
}

void VividDisplay::installOrRemoveEventFilter()
{
    QQuickWindow* currentWindow = window();
    if (!currentWindow) {
        m_filterInstalled = false;
        return;
    }

    if (m_mouseForwardEnabled && !m_filterInstalled) {
        currentWindow->installEventFilter(this);
        m_filterInstalled = true;
    } else if (!m_mouseForwardEnabled && m_filterInstalled) {
        currentWindow->removeEventFilter(this);
        m_filterInstalled = false;
    }
}

QString VividDisplay::effectiveSocketPath() const
{
    const QString trimmed = m_socketPath.trimmed();
    if (!trimmed.isEmpty())
        return trimmed;

    const QByteArray runtime = qgetenv("XDG_RUNTIME_DIR");
    const QString base = runtime.isEmpty()
        ? QDir::tempPath()
        : QString::fromLocal8Bit(runtime);
    return base + QStringLiteral("/vivid/display-v1.sock");
}

VividDisplay::OutputGeometry VividDisplay::resolveOutputGeometry() const
{
    OutputGeometry geometry;
    geometry.scale = std::max<qreal>(1.0, m_displayScale);
    geometry.physicalWidth = std::max(1, qRound(qreal(m_logicalWidth) * geometry.scale));
    geometry.physicalHeight = std::max(1, qRound(qreal(m_logicalHeight) * geometry.scale));
    geometry.consumerOutputId = m_consumerOutputId;
    geometry.monitorIndex = m_monitorIndex;
    geometry.displayKey = m_instanceId.trimmed();
    geometry.displayName = m_displayName;

    /*
     * Registration must never synchronously enter KScreen's nested event loop:
     * this function runs while the first Plasma wallpaper window and its scene
     * graph are still settling. Use QScreen as an immediately available,
     * Waywallen-compatible fallback. refreshOutputSnapshot() obtains the exact
     * fractional KScreen scale asynchronously and caches it for this or the
     * next connection.
     */
    const QList<QScreen*> screens = QGuiApplication::screens();
    QScreen* currentScreen = window() ? window()->screen() : nullptr;
    if (!currentScreen && !m_screenName.isEmpty()) {
        for (QScreen* screen : screens) {
            if (screen && screen->name().trimmed() == m_screenName.trimmed()) {
                currentScreen = screen;
                break;
            }
        }
    }
    if (currentScreen) {
        const int screenIndex = screens.indexOf(currentScreen);
        if (screenIndex >= 0) {
            geometry.consumerOutputId = static_cast<quint32>(screenIndex + 1);
            geometry.monitorIndex = static_cast<quint32>(screenIndex);
        }
        if (qFuzzyCompare(m_displayScale, 1.0))
            geometry.scale = std::max<qreal>(1.0, currentScreen->devicePixelRatio());
        geometry.physicalWidth =
            std::max(1, qRound(qreal(m_logicalWidth) * geometry.scale));
        geometry.physicalHeight =
            std::max(1, qRound(qreal(m_logicalHeight) * geometry.scale));
        if (geometry.displayKey.isEmpty())
            geometry.displayKey = currentScreen->name().trimmed();

        if (geometry.displayName.isEmpty() ||
            geometry.displayName == QStringLiteral("kde-plasma")) {
            QStringList vendorModelParts;
            if (!currentScreen->manufacturer().trimmed().isEmpty())
                vendorModelParts.append(currentScreen->manufacturer().trimmed());
            if (!currentScreen->model().trimmed().isEmpty())
                vendorModelParts.append(currentScreen->model().trimmed());
            const QString vendorModel = vendorModelParts.join(QLatin1Char(' '));
            if (!vendorModel.isEmpty())
                geometry.displayName = vendorModel;
            else if (!currentScreen->name().trimmed().isEmpty())
                geometry.displayName = currentScreen->name().trimmed();
        }
    }

    const OutputSnapshot* bestOutput = nullptr;
    int bestScore = -1;
    const QPoint displayPos(m_displayX, m_displayY);
    const QSize logicalSize(m_logicalWidth, m_logicalHeight);
    const QPoint displayCenter(m_displayX + m_logicalWidth / 2, m_displayY + m_logicalHeight / 2);

    for (const OutputSnapshot& output : m_outputSnapshots) {
        const QRect outputLogicalRect(output.position, output.logicalSize);
        int score = 0;
        if (!m_screenName.isEmpty() && output.name == m_screenName)
            score += 1000;
        if (!m_displayName.isEmpty() && output.name == m_displayName)
            score += 200;
        if ((output.position - displayPos).manhattanLength() <= 2)
            score += 200;
        if (std::abs(output.logicalSize.width() - logicalSize.width()) <= 2 &&
            std::abs(output.logicalSize.height() - logicalSize.height()) <= 2)
            score += 200;
        if (outputLogicalRect.contains(displayCenter))
            score += 100;

        if (score > bestScore) {
            bestScore = score;
            bestOutput = &output;
        }
    }

    if (!bestOutput || bestScore <= 0)
        return geometry;

    const int outputIndex =
        static_cast<int>(bestOutput - m_outputSnapshots.constData());
    if (outputIndex >= 0) {
        geometry.consumerOutputId = static_cast<quint32>(outputIndex + 1);
        geometry.monitorIndex = static_cast<quint32>(outputIndex);
    }

    geometry.scale = std::max<qreal>(1.0, bestOutput->scale);
    geometry.physicalWidth = std::max(1, qRound(qreal(m_logicalWidth) * geometry.scale));
    geometry.physicalHeight = std::max(1, qRound(qreal(m_logicalHeight) * geometry.scale));
    if (!bestOutput->displayKey.isEmpty())
        geometry.displayKey = bestOutput->displayKey;
    QStringList vendorModelParts;
    if (!bestOutput->vendor.isEmpty())
        vendorModelParts.append(bestOutput->vendor);
    if (!bestOutput->model.isEmpty())
        vendorModelParts.append(bestOutput->model);
    const QString vendorModel = vendorModelParts.join(QLatin1Char(' '));
    geometry.displayName = !m_displayName.isEmpty() && m_displayName != QStringLiteral("kde-plasma")
        ? m_displayName
        : (!vendorModel.isEmpty()
               ? vendorModel
               : (!bestOutput->name.isEmpty() ? bestOutput->name : geometry.displayName));
    return geometry;
}

void VividDisplay::refreshOutputSnapshot()
{
    if (m_outputSnapshotRefreshPending)
        return;

    m_outputSnapshotRefreshPending = true;
    auto* operation =
        new KScreen::GetConfigOperation(KScreen::ConfigOperation::NoEDID, this);
    connect(operation,
            &KScreen::ConfigOperation::finished,
            this,
            [this, operation](KScreen::ConfigOperation*) {
                m_outputSnapshotRefreshPending = false;
                if (operation->hasError() || !operation->config()) {
                    qCDebug(lcWallpaperKde,
                            "asynchronous KScreen snapshot unavailable: %s",
                            qPrintable(operation->errorString()));
                    operation->deleteLater();
                    return;
                }

                QVector<OutputSnapshot> snapshots;
                const KScreen::ConfigPtr config = operation->config();
                const QList<KScreen::OutputPtr> enabledOutputs =
                    sortedEnabledOutputs(config);
                snapshots.reserve(enabledOutputs.size());
                for (const KScreen::OutputPtr& output : enabledOutputs) {
                    OutputSnapshot snapshot;
                    snapshot.name = output->name().trimmed();
                    snapshot.displayKey =
                        displayKeyFromKScreenOutput(output, m_instanceId);
                    snapshot.vendor = output->vendor().trimmed();
                    snapshot.model = output->model().trimmed();
                    snapshot.position = output->pos();
                    snapshot.logicalSize = config->logicalSizeForOutputInt(*output);
                    snapshot.scale = std::max<qreal>(1.0, output->scale());
                    snapshots.append(std::move(snapshot));
                }
                m_outputSnapshots = std::move(snapshots);
                qCDebug(lcWallpaperKde,
                        "cached asynchronous KScreen snapshot outputs=%lld",
                        static_cast<long long>(m_outputSnapshots.size()));
                operation->deleteLater();
            });
}

QString VividDisplay::displayKeyForScreenName(const QString& screenName) const
{
    const QString normalized = screenName.trimmed();
    if (normalized.isEmpty())
        return {};

    for (const OutputSnapshot& output : m_outputSnapshots) {
        if (output.name == normalized)
            return !output.displayKey.isEmpty() ? output.displayKey : m_instanceId.trimmed();
    }

    const QList<QScreen*> screens = QGuiApplication::screens();
    for (QScreen* screen : screens) {
        if (screen && screen->name().trimmed() == normalized)
            return !m_instanceId.trimmed().isEmpty() ? m_instanceId.trimmed() : normalized;
    }

    return {};
}

bool VividDisplay::sceneGraphReadyForProtocol() const
{
    QQuickWindow* currentWindow = window();
    return currentWindow && currentWindow->isSceneGraphInitialized();
}

void VividDisplay::armSceneGraphReadyConnection()
{
    QQuickWindow* currentWindow = window();
    if (!currentWindow)
        return;

    const bool firstArmForWindow = m_sceneGraphWindow != currentWindow;
    if (m_sceneGraphWindow && m_sceneGraphWindow != currentWindow) {
        disconnect(m_sceneGraphWindow,
                   &QQuickWindow::sceneGraphInitialized,
                   this,
                   &VividDisplay::onSceneGraphInitialized);
        disconnect(m_sceneGraphWindow,
                   &QQuickWindow::sceneGraphInvalidated,
                   this,
                   &VividDisplay::onSceneGraphInvalidated);
    }
    m_sceneGraphWindow = currentWindow;

    connect(currentWindow,
            &QQuickWindow::sceneGraphInitialized,
            this,
            &VividDisplay::onSceneGraphInitialized,
            Qt::UniqueConnection);
    connect(currentWindow,
            &QQuickWindow::sceneGraphInvalidated,
            this,
            &VividDisplay::onSceneGraphInvalidated,
            static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::UniqueConnection));
    if (currentWindow->isSceneGraphInitialized())
        return;

    /* setGraphicsConfiguration() is a pre-initialization operation. Apply it
     * exactly once per QQuickWindow, matching Waywallen, rather than once per
     * initial QML property binding/reconnect request. */
    if (!firstArmForWindow)
        return;

    auto config = currentWindow->graphicsConfiguration();
    const QList<QByteArray> extensions = vulkanDeviceExtensionNames();
    config.setDeviceExtensions(extensions);
    currentWindow->setGraphicsConfiguration(config);
}

void VividDisplay::releaseSceneGraphResources()
{
    /*
     * sceneGraphInvalidated is emitted on Qt Quick's render thread while the
     * GUI thread is blocked. Imported images, semaphores, and the shadow
     * blitter all belong to the scene graph's VkDevice/GL context, so they
     * must be destroyed before Qt tears that device/context down. Keeping
     * m_vkBackendReady across this boundary leaves later producer reconnects
     * using stale Qt Vulkan handles, which presents as audio with a black
     * wallpaper until plasmashell is restarted.
    */
    discardPendingEglFrame(0);
    signalPendingEglRelease(QStringLiteral("scene-graph-invalidated"));
    signalPendingVulkanFrame(QStringLiteral("scene-graph-invalidated"));
    clearGenerations(false);
    if (m_vkBlitterReady && m_vkBlitter.fence_armed) {
        /* A timed-out copy may still reference an imported generation.  The
         * blitter shutdown waits/reaps that submission before the pending
         * generation queue is drained. */
        ww_vk_blitter_shutdown(&m_vkBlitter);
        m_vkBlitterReady = false;
    }
    int drainResult = 0;
    do {
        drainResult = drainPendingGenerationResources();
    } while (drainResult > 0);
    if (drainResult < 0) {
        qCCritical(lcWallpaperKde,
                   "scene graph invalidated before pending generation resources could be drained");
        return;
    }
    destroyEglShadowResources();
    if (m_eglShadowTexture != 0 || m_eglShadowReadFramebuffer != 0 ||
        m_eglShadowFramebuffer != 0) {
        qCCritical(lcWallpaperKde,
                   "scene graph invalidated without a context capable of deleting EGL shadow resources");
        return;
    }
    shutdownVulkanBackend();
    m_activeBackend = BackendNone;
    {
        QMutexLocker lock(&m_pendingVulkanMutex);
        m_currentGeneration = 0;
        m_currentBuffer = 0;
    }
}

void VividDisplay::releaseIdleProtocolBackend()
{
    bool hasPendingGenerations = false;
    {
        QMutexLocker lock(&m_pendingGenerationMutex);
        hasPendingGenerations = !m_pendingGenerations.isEmpty();
    }

    /*
     * Only an early connection/handshake failure reaches the fully idle case.
     * Once a frame has existed, the render thread owns retirement and backend
     * shutdown ordering; unloading here would invalidate imported handles that
     * are still queued for destruction.
     */
    if (!m_generations.isEmpty() || hasPendingGenerations || m_vkBlitterReady ||
        m_eglShadowTexture != 0 || m_eglShadowReadFramebuffer != 0 ||
        m_eglShadowFramebuffer != 0) {
        return;
    }

    if (m_activeBackend == BackendVulkan)
        shutdownVulkanBackend();
    m_activeBackend = BackendNone;
}

void VividDisplay::configureSceneGraphForProtocol()
{
    QQuickWindow* currentWindow = window();
    auto* rendererInterface = currentWindow ? currentWindow->rendererInterface() : nullptr;
    if (!currentWindow || !rendererInterface || !currentWindow->isSceneGraphInitialized()) {
        m_activeBackend = BackendNone;
        qCWarning(lcWallpaperKde, "scene graph is not ready for protocol backend selection");
        return;
    }

    const QSGRendererInterface::GraphicsApi api = rendererInterface->graphicsApi();
    if (api == QSGRendererInterface::OpenGL) {
        const ActiveBackend previousBackend = m_activeBackend;
        if (m_activeBackend == BackendVulkan)
            shutdownVulkanBackend();
        m_activeBackend = BackendEgl;
        if (previousBackend != m_activeBackend) {
            qCInfo(lcWallpaperKde,
                   "Qt scene graph api=%s selected Vivid backend=BackendEgl",
                   sceneGraphApiName(api));
        }
        return;
    }

    if (api == QSGRendererInterface::Vulkan) {
        const ActiveBackend previousBackend = m_activeBackend;
        m_activeBackend = bindVulkanBackend() ? BackendVulkan : BackendNone;
        if (previousBackend != m_activeBackend) {
            qCInfo(lcWallpaperKde,
                   "Qt scene graph api=%s selected Vivid backend=%s",
                   sceneGraphApiName(api),
                   m_activeBackend == BackendVulkan ? "BackendVulkan" : "BackendNone");
        }
        return;
    }

    if (m_activeBackend == BackendVulkan)
        shutdownVulkanBackend();
    m_activeBackend = BackendNone;
    qCWarning(lcWallpaperKde,
              "Qt scene graph api=%s has no Vivid DMA-BUF backend",
              sceneGraphApiName(api));
}

bool VividDisplay::bindVulkanBackend()
{
    if (m_vkBackendReady)
        return true;

    QQuickWindow* currentWindow = window();
    auto* qvkInstance = currentWindow ? currentWindow->vulkanInstance() : nullptr;
    auto* rendererInterface = currentWindow ? currentWindow->rendererInterface() : nullptr;
    if (!currentWindow || !rendererInterface || !qvkInstance || !qvkInstance->isValid()) {
        qCWarning(lcWallpaperKde, "Vulkan backend bind failed: Qt Vulkan instance is unavailable");
        return false;
    }

    auto* physicalDevice = static_cast<VkPhysicalDevice*>(
        rendererInterface->getResource(currentWindow,
                                       QSGRendererInterface::PhysicalDeviceResource));
    auto* device = static_cast<VkDevice*>(
        rendererInterface->getResource(currentWindow, QSGRendererInterface::DeviceResource));
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    auto* queueFamily = static_cast<uint32_t*>(
        rendererInterface->getResource(currentWindow,
                                       QSGRendererInterface::GraphicsQueueFamilyIndexResource));
#else
    uint32_t fallbackQueueFamily = 0;
    auto* queueFamily = &fallbackQueueFamily;
#endif
    auto* queue = static_cast<VkQueue*>(
        rendererInterface->getResource(currentWindow,
                                       QSGRendererInterface::CommandQueueResource));
    if (!physicalDevice || !*physicalDevice || !device || !*device) {
        qCWarning(lcWallpaperKde,
                  "Vulkan backend bind failed: missing Qt render resources phys=%p device=%p",
                  physicalDevice ? reinterpret_cast<void*>(*physicalDevice) : nullptr,
                  device ? reinterpret_cast<void*>(*device) : nullptr);
        return false;
    }

    const VkInstance instance = qvkInstance->vkInstance();
    const VkPhysicalDevice vkPhysicalDevice = *physicalDevice;
    const VkDevice vkDevice = *device;
    const uint32_t vkQueueFamilyIndex = queueFamily ? *queueFamily : 0;
    VkQueue vkQueue = queue ? *queue : VK_NULL_HANDLE;
    auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        qvkInstance->getInstanceProcAddr("vkGetInstanceProcAddr"));
    if (!getInstanceProcAddr) {
        qCWarning(lcWallpaperKde, "Vulkan backend bind failed: vkGetInstanceProcAddr unavailable");
        return false;
    }
    if (vkQueue == VK_NULL_HANDLE) {
        /*
         * Keep this fallback aligned with waywallen's KDE Vulkan route:
         * when Qt does not expose CommandQueueResource, use queue 0 from
         * the graphics family returned by Qt. The warning is intentional
         * because correct ordering still depends on Qt submitting through
         * that same queue.
         */
        auto getDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
            getInstanceProcAddr(instance, "vkGetDeviceQueue"));
        if (getDeviceQueue) {
            getDeviceQueue(vkDevice, vkQueueFamilyIndex, 0, &vkQueue);
            qCWarning(lcWallpaperKde,
                      "Qt CommandQueueResource is unavailable; using vkGetDeviceQueue family=%u index=0, which assumes Qt submits on the same queue",
                      vkQueueFamilyIndex);
        }
    }
    if (vkQueue == VK_NULL_HANDLE) {
        qCWarning(lcWallpaperKde, "Vulkan backend bind failed: no VkQueue available");
        return false;
    }
    const int rc = ww_vk_backend_load(&m_vkBackend,
                                      instance,
                                      vkPhysicalDevice,
                                      vkDevice,
                                      vkQueueFamilyIndex,
                                      reinterpret_cast<ww_vk_get_instance_proc_addr_fn>(
                                          getInstanceProcAddr),
                                      false);
    if (rc != 0) {
        qCWarning(lcWallpaperKde, "Vulkan backend bind failed: ww_vk_backend_load rc=%d", rc);
        return false;
    }

    m_vkInstance = instance;
    m_vkPhysicalDevice = vkPhysicalDevice;
    m_vkDevice = vkDevice;
    m_vkQueue = vkQueue;
    m_vkQueueFamilyIndex = vkQueueFamilyIndex;
    m_vkGetInstanceProcAddr =
        reinterpret_cast<ww_vk_get_instance_proc_addr_fn>(getInstanceProcAddr);
    m_vkBackendReady = true;
    qCDebug(lcWallpaperKde,
            "Vulkan backend bound instance=%p physical=%p device=%p queue=%p queue-family=%u",
            reinterpret_cast<void*>(m_vkInstance),
            reinterpret_cast<void*>(m_vkPhysicalDevice),
            reinterpret_cast<void*>(m_vkDevice),
            reinterpret_cast<void*>(m_vkQueue),
            m_vkQueueFamilyIndex);
    return true;
}

void VividDisplay::shutdownVulkanBackend()
{
    if (m_vkBlitterReady) {
        ww_vk_blitter_shutdown(&m_vkBlitter);
        m_vkBlitterReady = false;
    }
    if (m_vkBackendReady) {
        ww_vk_backend_unload(&m_vkBackend);
        m_vkBackendReady = false;
    }
    m_vkInstance = VK_NULL_HANDLE;
    m_vkPhysicalDevice = VK_NULL_HANDLE;
    m_vkDevice = VK_NULL_HANDLE;
    m_vkQueue = VK_NULL_HANDLE;
    m_vkQueueFamilyIndex = 0;
    m_vkGetInstanceProcAddr = nullptr;
}

void VividDisplay::scheduleReconnect(int delayMs)
{
    if (!isComponentComplete() || !m_autoReconnect)
        return;
    if (!sceneGraphReadyForProtocol()) {
        armSceneGraphReadyConnection();
        return;
    }
    m_reconnectTimer.start(std::max(0, delayMs));
}

void VividDisplay::refreshSocketWatcher()
{
    const QStringList watchedFiles = m_socketWatcher.files();
    if (!watchedFiles.isEmpty())
        m_socketWatcher.removePaths(watchedFiles);
    const QStringList watchedDirectories = m_socketWatcher.directories();
    if (!watchedDirectories.isEmpty())
        m_socketWatcher.removePaths(watchedDirectories);

    const QString socketPath = effectiveSocketPath();
    if (socketPath.isEmpty())
        return;

    const QFileInfo socketInfo(socketPath);
    QString watchDirectory = socketInfo.absolutePath();
    while (!watchDirectory.isEmpty() && !QFileInfo::exists(watchDirectory)) {
        const QString parent = QFileInfo(watchDirectory).absolutePath();
        if (parent == watchDirectory)
            break;
        watchDirectory = parent;
    }
    if (!watchDirectory.isEmpty() && QFileInfo::exists(watchDirectory))
        m_socketWatcher.addPath(watchDirectory);
    if (socketInfo.exists())
        m_socketWatcher.addPath(socketPath);
}

void VividDisplay::onSceneGraphInitialized()
{
    scheduleReconnect(0);
}

void VividDisplay::onSceneGraphInvalidated()
{
    auto* invalidatedWindow = qobject_cast<QQuickWindow*>(sender());
    if (invalidatedWindow && m_sceneGraphWindow && invalidatedWindow != m_sceneGraphWindow)
        return;

    qCInfo(lcWallpaperKde, "scene graph invalidated; releasing Vivid GPU resources");
    releaseSceneGraphResources();

    /*
     * Socket notifiers and connection properties belong to the GUI thread.
     * Finish transport teardown there; the persistent initialized-signal
     * connection above will reconnect after Qt creates the next scene graph.
     */
    QMetaObject::invokeMethod(
        this,
        [this]() {
            closeTransport(false);
            armSceneGraphReadyConnection();
        },
        Qt::QueuedConnection);
}

void VividDisplay::onReconnectTimer()
{
    tryConnect();
}

void VividDisplay::tryConnect()
{
    if (!sceneGraphReadyForProtocol()) {
        armSceneGraphReadyConnection();
        return;
    }
    if (m_fd >= 0 || m_connecting)
        return;

    const QString path = effectiveSocketPath();
    const QByteArray nativePath = QFile::encodeName(path);
    if (nativePath.size() >= static_cast<int>(sizeof(sockaddr_un::sun_path))) {
        handleProtocolError(ENAMETOOLONG, QStringLiteral("socket path is too long: %1").arg(path));
        return;
    }

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        handleProtocolError(errno, QStringLiteral("socket() failed: %1").arg(formatErrno(errno)));
        return;
    }

    sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, nativePath.constData(), static_cast<size_t>(nativePath.size() + 1));

    setConnState(Connecting);
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        m_fd = fd;
        finishConnect();
        return;
    }

    if (errno != EINPROGRESS) {
        const int err = errno;
        ::close(fd);
        handleProtocolError(err, QStringLiteral("connect(%1) failed: %2").arg(path, formatErrno(err)));
        return;
    }

    m_fd = fd;
    m_connecting = true;
    m_writeNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Write, this);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &VividDisplay::onSocketWritable);
}

void VividDisplay::finishConnect()
{
    int error = 0;
    socklen_t len = sizeof(error);
    if (::getsockopt(m_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
        error = errno;

    if (error != 0) {
        handleProtocolError(error, QStringLiteral("connect finished with error: %1").arg(formatErrno(error)));
        return;
    }

    configureSceneGraphForProtocol();
    m_connecting = false;
    if (m_writeNotifier)
        m_writeNotifier->setEnabled(false);

    m_readNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &VividDisplay::onSocketReadable);

    setConnState(Handshaking);
    setLastError(QString());
    sendHello();
    sendConsumerCaps();
    sendRegisterOutput();
    sendWindowState();
    flushOutbox();
    qCInfo(lcWallpaperKde, "connected to %s", qPrintable(effectiveSocketPath()));
}

void VividDisplay::closeTransport(bool keepLastFrame)
{
    const bool hadPresentationState = !m_generations.isEmpty() || m_currentGeneration != 0;
    m_reconnectTimer.stop();

    delete m_readNotifier;
    m_readNotifier = nullptr;
    delete m_writeNotifier;
    m_writeNotifier = nullptr;

    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }

    m_connecting = false;
    m_outbox.clear();
    m_outboxOffset = 0;
    vivid_display_recv_state_clear(&m_recvState);

    if (m_outputId != 0) {
        m_outputId = 0;
        emit outputIdChanged();
    }

    discardPendingEglFrame(0);
    signalPendingEglRelease(QStringLiteral("transport-close"));
    signalPendingVulkanFrame(QStringLiteral("transport-close"));
    retireAllGenerations(QStringLiteral("transport-close"));

    if (!keepLastFrame) {
        {
            QMutexLocker lock(&m_pendingVulkanMutex);
            m_currentGeneration = 0;
            m_currentBuffer = 0;
        }
        /* queueGenerationResources() already detached every GPU handle.  The
         * remaining entries are presentation-only metadata and can be
         * discarded without losing ownership of driver resources. */
        m_generations.clear();
    }

    /* Ensure updatePaintNode gets a chance to drain an actual imported pool.
     * A failed pre-daemon connect has no render resources; dirtying the item in
     * that state creates an otherwise unnecessary Vulkan surface commit during
     * Plasma/KWin startup. */
    if (hadPresentationState)
        update();

    setStreamState(Inactive);
    setConnState(Disconnected);
}

void VividDisplay::queueFrame(quint16 opcode, const QByteArray& body)
{
    if (body.size() > static_cast<int>(Proto::VIVID_DISPLAY_CODEC_MAX_BODY_BYTES)) {
        handleProtocolError(EMSGSIZE, QStringLiteral("outgoing frame body is too large"));
        return;
    }

    QByteArray frame;
    frame.resize(4 + body.size());
    writeU16LE(frame, 0, opcode);
    writeU16LE(frame, 2, static_cast<quint16>(frame.size()));
    if (!body.isEmpty())
        std::memcpy(frame.data() + 4, body.constData(), static_cast<size_t>(body.size()));

    m_outbox.push_back(frame);
    flushOutbox();
}

void VividDisplay::queueJsonFrame(quint16 opcode, const QJsonObject& object)
{
    queueFrame(opcode, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void VividDisplay::flushOutbox()
{
    if (m_fd < 0 || m_connecting)
        return;

    while (!m_outbox.isEmpty()) {
        const QByteArray& front = m_outbox.front();
        const qsizetype remaining = front.size() - m_outboxOffset;
        const ssize_t sent =
            vivid_display_send_bytes_nonblocking(m_fd,
                                                  reinterpret_cast<const uint8_t*>(front.constData() + m_outboxOffset),
                                                  static_cast<size_t>(remaining));
        if (sent < 0) {
            handleProtocolError(static_cast<int>(-sent),
                                QStringLiteral("socket write failed: %1").arg(formatErrno(static_cast<int>(-sent))));
            return;
        }
        if (sent == 0)
            break;

        m_outboxOffset += static_cast<qsizetype>(sent);
        if (m_outboxOffset >= front.size()) {
            m_outbox.pop_front();
            m_outboxOffset = 0;
        }
    }

    if (m_writeNotifier)
        m_writeNotifier->setEnabled(!m_outbox.isEmpty());
}

void VividDisplay::sendHello()
{
    queueJsonFrame(Proto::VIVID_DISPLAY_REQ_HELLO,
                   QJsonObject {
                       { Proto::HELLO::protocol,
                         QString::fromLatin1(Proto::VIVID_DISPLAY_PROTOCOL_NAME) },
                       { Proto::HELLO::version,
                         static_cast<int>(Proto::VIVID_DISPLAY_PROTOCOL_VERSION) },
                       { Proto::HELLO::clientName, QStringLiteral("kde-plasma-wallpaper") },
                       { Proto::HELLO::role, QStringLiteral("consumer") },
                       { Proto::HELLO::features,
                         QJsonArray {
                             QStringLiteral("dmabuf-egl-image-v1"),
                             QStringLiteral("dmabuf-caps-v3"),
                             QStringLiteral("explicit-sync-fd-v1"),
                             QStringLiteral("dmabuf-bind-failed-v1"),
                             QStringLiteral("dmabuf-unbind-done-v1"),
                             QStringLiteral("unbind-v2"),
                             QString::fromLatin1(FeatureDmaBufShadowCopy),
                             QStringLiteral("pointer-events-v1"),
                             QStringLiteral("display-window-state-v2"),
                             QStringLiteral("media-state-v1"),
                             QStringLiteral("audio-samples-bin-v2"),
                         } },
                   });
}

void VividDisplay::sendConsumerCaps()
{
    QJsonObject dmabufCaps;
    if (m_activeBackend == BackendVulkan) {
        dmabufCaps = buildVulkanDmaBufCaps(m_vkBackendReady ? &m_vkBackend : nullptr);
    } else if (m_activeBackend == BackendEgl) {
        const EGLDisplay eglDisplay = qtWindowEglDisplay(window());
        dmabufCaps = buildEglDmaBufCaps(eglDisplay);
    } else {
        dmabufCaps = buildUnsupportedDmaBufCaps(
            QStringLiteral("Qt scene graph did not expose a supported OpenGL or Vulkan backend"));
    }
    qCInfo(lcWallpaperKde,
           "sending consumer caps backend=%s probe=%s render-node=%s fourccs=%lld modifiers=%lld implicit-linear=%lld relay-modes=%lld memory-hints=%lld",
           qPrintable(dmabufCaps.value(QStringLiteral("backend")).toString(QStringLiteral("(unknown)"))),
           qPrintable(dmabufCaps.value(QStringLiteral("probe")).toString(QStringLiteral("(unknown)"))),
           qPrintable(dmabufCaps.value(QStringLiteral("renderNode")).toString(QStringLiteral("(unknown)"))),
           static_cast<long long>(dmabufCaps.value(QStringLiteral("fourccs")).toArray().size()),
           static_cast<long long>(dmabufCaps.value(QStringLiteral("modifiers")).toArray().size()),
           static_cast<long long>(dmabufCaps.value(QStringLiteral("implicitLinearFourccs")).toArray().size()),
           static_cast<long long>(dmabufCaps.value(QStringLiteral("relayModes")).toArray().size()),
           static_cast<long long>(dmabufCaps.value(QStringLiteral("memoryHints")).toArray().size()));

    queueJsonFrame(Proto::VIVID_DISPLAY_REQ_CONSUMER_CAPS,
                   QJsonObject {
                       { Proto::CONSUMER_CAPS::dmabufCaps, dmabufCaps },
                   });
}

void VividDisplay::sendBindFailed(const Generation& generation,
                                  quint32           reason,
                                  const QString&    message)
{
    if (generation.fourcc == 0)
        return;

    qCWarning(lcWallpaperKde,
              "BIND_FAILED output=%u generation=%llu fourcc=0x%08x modifier=0x%016llx reason=%u [%.*s]: %s",
              generation.outputId,
              static_cast<unsigned long long>(generation.id),
              generation.fourcc,
              static_cast<unsigned long long>(generation.modifier),
              reason,
              static_cast<int>(Proto::bind_failed_reason_name(reason).size()),
              Proto::bind_failed_reason_name(reason).data(),
              qPrintable(message));
    queueJsonFrame(Proto::VIVID_DISPLAY_REQ_BIND_FAILED,
                   QJsonObject {
                       { Proto::BIND_FAILED::outputId, static_cast<qint64>(generation.outputId) },
                       { Proto::BIND_FAILED::generation, QString::number(generation.id) },
                       { Proto::BIND_FAILED::fourcc, static_cast<qint64>(generation.fourcc) },
                       { Proto::BIND_FAILED::modifier, QString::number(generation.modifier) },
                       { Proto::BIND_FAILED::reason, static_cast<int>(reason) },
                       { Proto::BIND_FAILED::message, message },
                   });
}

bool VividDisplay::sendMediaState(const QJsonObject& payload)
{
    if (m_fd < 0 || m_connState != Connected)
        return false;

    queueJsonFrame(Proto::VIVID_DISPLAY_REQ_MEDIA_STATE, payload);
    return true;
}

bool VividDisplay::sendAudioSamples(const QVector<double>& samples, quint64 timeUsec)
{
    if (m_fd < 0 || m_connState != Connected)
        return false;

    constexpr int sampleCount = static_cast<int>(
        Proto::VIVID_DISPLAY_AUDIO_SAMPLES_BIN_MAX_COUNT
    );
    if (samples.size() != static_cast<qsizetype>(sampleCount))
        return false;

    QByteArray body;
    body.resize(12 + sampleCount * static_cast<int>(sizeof(float)));
    std::vector<float> encoded(static_cast<size_t>(sampleCount));
    for (int i = 0; i < sampleCount; i++) {
        const double value = samples.at(i);
        encoded[static_cast<size_t>(i)] =
            std::isfinite(value) && value > 0.0 ? static_cast<float>(value) : 0.0f;
    }
    const int written = vivid_display_audio_samples_bin_body_write(
        reinterpret_cast<uint8_t*>(body.data()),
        static_cast<size_t>(body.size()),
        static_cast<uint32_t>(sampleCount),
        timeUsec,
        encoded.data());
    if (written < 0)
        return false;
    body.resize(written);

    queueFrame(Proto::VIVID_DISPLAY_REQ_AUDIO_SAMPLES_BIN, body);
    return true;
}

void VividDisplay::sendRegisterOutput()
{
    m_outputGeometry = resolveOutputGeometry();
    if (m_consumerOutputId != m_outputGeometry.consumerOutputId) {
        m_consumerOutputId = m_outputGeometry.consumerOutputId;
        emit consumerOutputIdChanged();
    }
    if (m_monitorIndex != m_outputGeometry.monitorIndex) {
        m_monitorIndex = m_outputGeometry.monitorIndex;
        emit monitorIndexChanged();
    }

    queueJsonFrame(Proto::VIVID_DISPLAY_REQ_REGISTER_OUTPUT,
                   QJsonObject {
                       { Proto::REGISTER_OUTPUT::consumerOutputId, static_cast<int>(m_outputGeometry.consumerOutputId) },
                       { Proto::REGISTER_OUTPUT::monitorIndex, static_cast<int>(m_outputGeometry.monitorIndex) },
                       { Proto::REGISTER_OUTPUT::x, m_displayX },
                       { Proto::REGISTER_OUTPUT::y, m_displayY },
                       { Proto::REGISTER_OUTPUT::width, m_logicalWidth },
                       { Proto::REGISTER_OUTPUT::height, m_logicalHeight },
                       { Proto::REGISTER_OUTPUT::physicalWidth, m_outputGeometry.physicalWidth },
                       { Proto::REGISTER_OUTPUT::physicalHeight, m_outputGeometry.physicalHeight },
                       { Proto::REGISTER_OUTPUT::scale, m_outputGeometry.scale },
                       { Proto::REGISTER_OUTPUT::transform, QStringLiteral("normal") },
                       { Proto::REGISTER_OUTPUT::refreshRateMhz, static_cast<int>(m_refreshRateMhz) },
                       { Proto::REGISTER_OUTPUT::desktop, QStringLiteral("kde-plasma-wallpaper") },
                       { QStringLiteral("instanceId"), m_instanceId },
                       { Proto::REGISTER_OUTPUT::displayKey, m_outputGeometry.displayKey },
                       { Proto::REGISTER_OUTPUT::displayName, m_outputGeometry.displayName },
                   });
}

void VividDisplay::sendWindowState()
{
    if (m_fd < 0 || m_connecting)
        return;

    QJsonObject displays;
    const QString displayKey = m_outputGeometry.displayKey.trimmed();
    if (!displayKey.isEmpty()) {
        displays.insert(displayKey,
                        QJsonObject {
                            { QStringLiteral("flags"), static_cast<qint64>(m_windowStateFlags) },
                        });
    }

    queueJsonFrame(Proto::VIVID_DISPLAY_REQ_WINDOW_STATE,
                   QJsonObject {
                       { QStringLiteral("schema"),
                         QString::fromLatin1(Proto::VIVID_DISPLAY_WINDOW_STATE_SCHEMA_V2) },
                       { QStringLiteral("source"), QStringLiteral("kde-plasma-wallpaper") },
                       { QStringLiteral("displays"), displays },
                       { QStringLiteral("session"),
                         QJsonObject {
                             { QStringLiteral("mprisPlaying"), m_mprisPlaying },
                             { QStringLiteral("mprisPlayers"), m_mprisPlayers },
                             { QStringLiteral("windows"), QJsonArray {} },
                         } },
                   });
}

void VividDisplay::onSocketReadable()
{
    if (m_fd < 0)
        return;

    /*
     * Match waywallen_display_dispatch(): consume at most one protocol frame
     * per notifier activation and return directly to Qt's event dispatcher.
     * Keep the level-triggered notifier enabled. Disabling it and posting a
     * queued callback to re-enable it creates an artificial MetaCall/socket
     * loop under a full-rate producer, which can run ahead of the wallpaper
     * window's queued scene-graph synchronization indefinitely.
     */
    const int result = vivid_display_recv_frame_nonblocking(m_fd, &m_recvState);
    if (result == Proto::VIVID_DISPLAY_CODEC_FRAME_NEED_IO)
        return;

    if (result == Proto::VIVID_DISPLAY_CODEC_FRAME_DONE) {
        QByteArray body(reinterpret_cast<const char*>(m_recvState.body),
                        static_cast<qsizetype>(m_recvState.body_len));
        handleIncomingFrame(m_recvState.opcode, body, &m_recvState);
        vivid_display_recv_state_clear(&m_recvState);
        return;
    }

    handleProtocolError(-result, QStringLiteral("socket read failed: %1").arg(formatErrno(-result)));
}

void VividDisplay::onSocketWritable()
{
    if (m_connecting) {
        finishConnect();
        return;
    }
    flushOutbox();
}

void VividDisplay::handleIncomingFrame(quint16 opcode,
                                        const QByteArray& body,
                                        VividDisplayRecvState* state)
{
    switch (opcode) {
    case Proto::VIVID_DISPLAY_EVT_WELCOME: {
        QString error;
        const QJsonObject object = parseJsonObject(body, &error);
        if (!object.isEmpty()) {
            m_negotiatedVersion =
                jsonUInt32(object.value(QStringLiteral("negotiatedVersion")),
                           Proto::VIVID_DISPLAY_PROTOCOL_VERSION);
            qCInfo(lcWallpaperKde,
                   "welcome negotiatedVersion=%u",
                   m_negotiatedVersion);
        } else if (!error.isEmpty()) {
            qCWarning(lcWallpaperKde, "invalid WELCOME JSON: %s", qPrintable(error));
        }
        m_errorReconnectDelayMs = 1200;
        return;
    }
    case Proto::VIVID_DISPLAY_EVT_OUTPUT_ACCEPTED: {
        QString error;
        const QJsonObject object = parseJsonObject(body, &error);
        if (object.isEmpty() && !error.isEmpty()) {
            setLastError(QStringLiteral("invalid OUTPUT_ACCEPTED JSON: %1").arg(error));
            return;
        }
        handleOutputAccepted(object);
        return;
    }
    case Proto::VIVID_DISPLAY_EVT_BIND_BUFFERS:
        handleBindBuffers(body, state);
        return;
    case Proto::VIVID_DISPLAY_EVT_SET_CONFIG: {
        QString error;
        const QJsonObject object = parseJsonObject(body, &error);
        if (object.isEmpty() && !error.isEmpty()) {
            setLastError(QStringLiteral("invalid SET_CONFIG JSON: %1").arg(error));
            return;
        }
        handleSetConfig(object);
        return;
    }
    case Proto::VIVID_DISPLAY_EVT_FRAME_READY:
        handleFrameReady(body, state);
        return;
    case Proto::VIVID_DISPLAY_EVT_UNBIND:
        handleUnbind(body);
        return;
    case Proto::VIVID_DISPLAY_EVT_ERROR: {
        QString error;
        const QJsonObject object = parseJsonObject(body, &error);
        if (!object.isEmpty()) {
            handleProducerError(jsonUInt32(object.value(Proto::EVT_ERROR::code)),
                                object.value(Proto::EVT_ERROR::fatal).toBool(false),
                                object.value(Proto::EVT_ERROR::message).toString());
        } else {
            setLastError(error.isEmpty() ? QString::fromUtf8(body) : error);
        }
        return;
    }
    default:
        qCDebug(lcWallpaperKde, "ignored event opcode=%u", opcode);
        return;
    }
}

void VividDisplay::handleOutputAccepted(const QJsonObject& object)
{
    const quint32 consumerId = jsonUInt32(object.value(Proto::OUTPUT_ACCEPTED::consumerOutputId));
    const quint32 outputId = jsonUInt32(object.value(Proto::OUTPUT_ACCEPTED::outputId));
    if (consumerId != m_consumerOutputId || outputId == 0) {
        setLastError(QStringLiteral("invalid OUTPUT_ACCEPTED consumer=%1 output=%2")
                         .arg(consumerId)
                         .arg(outputId));
        return;
    }

    if (m_outputId != outputId) {
        m_outputId = outputId;
        emit outputIdChanged();
    }
    // Match Waywallen's handshake boundary: Connected means the complete
    // display registration handshake has finished, not merely that WELCOME
    // was received. Auxiliary KDE traffic is gated on this state.
    setConnState(Connected);
    qCInfo(lcWallpaperKde, "output accepted consumer=%u output=%u", consumerId, outputId);
}

void VividDisplay::handleBindBuffers(const QByteArray& body, VividDisplayRecvState* state)
{
    QString error;
    const QJsonObject object = parseJsonObject(body, &error);
    if (object.isEmpty() && !error.isEmpty()) {
        setLastError(QStringLiteral("invalid BIND_BUFFERS JSON: %1").arg(error));
        return;
    }

    Generation generation;
    generation.outputId = jsonUInt32(object.value(Proto::BIND_BUFFERS::outputId));
    generation.id = jsonUInt64(object.value(Proto::BIND_BUFFERS::generation));
    generation.width = static_cast<int>(jsonUInt32(object.value(QStringLiteral("width"))));
    generation.height = static_cast<int>(jsonUInt32(object.value(QStringLiteral("height"))));
    generation.fourcc = jsonUInt32(object.value(QStringLiteral("fourcc")));
    generation.modifier = jsonUInt64(object.value(QStringLiteral("modifier")), DrmFormatModInvalid);
    generation.planesPerBuffer = jsonUInt32(object.value(QStringLiteral("planesPerBuffer")));
    generation.renderNode = jsonStringMember(object, Proto::BIND_BUFFERS::producerRenderNode);
    generation.vendor = jsonStringMember(object, QStringLiteral("producerVendor"));
    generation.pciAddress = jsonStringMember(object, QStringLiteral("producerPciAddress"));
    generation.negotiatedPath = jsonStringMember(object, QStringLiteral("negotiatedPath"));
    generation.memorySource = jsonStringMember(object, QStringLiteral("memorySource"));
    generation.memoryHint = jsonStringMember(object, QStringLiteral("memoryHint"));
    generation.presentationPath = jsonStringMember(object, QStringLiteral("presentationPath"));
    generation.producerDriverUuid =
        jsonStringMember(object, QStringLiteral("producerDriverUuid"));
    generation.producerDrmRenderMajor =
        jsonUInt32(object.value(QStringLiteral("producerDrmRenderMajor")));
    generation.producerDrmRenderMinor =
        jsonUInt32(object.value(QStringLiteral("producerDrmRenderMinor")));
    generation.consumerRenderNode =
        jsonStringMember(object, QStringLiteral("consumerRenderNode"));
    generation.consumerDrmRenderMajor =
        jsonUInt32(object.value(QStringLiteral("consumerDrmRenderMajor")));
    generation.consumerDrmRenderMinor =
        jsonUInt32(object.value(QStringLiteral("consumerDrmRenderMinor")));
    generation.premultiplied = object.value(QStringLiteral("premultiplied")).toBool(true);

    const QJsonArray buffers = object.value(QStringLiteral("buffers")).toArray();
    if (generation.outputId == 0 || generation.id == 0 || generation.width <= 0 ||
        generation.height <= 0 || generation.fourcc == 0 || buffers.isEmpty()) {
        const QString message =
            QStringLiteral("BIND_BUFFERS is missing output/generation/size/format/buffers");
        setLastError(message);
        sendBindFailed(generation, 1, message);
        return;
    }

    if (generation.presentationPath != QStringLiteral("shadow-copy")) {
        const QString message =
            QStringLiteral("KDE consumer supports only shadow-copy presentation; "
                           "producer selected '%1'")
                .arg(generation.presentationPath.isEmpty()
                         ? QStringLiteral("(missing)")
                         : generation.presentationPath);
        closeRecvStateFds(state);
        setLastError(message);
        sendBindFailed(generation, 1, message);
        return;
    }

    const bool reusedGenerationId = findGeneration(generation.id) != nullptr;
    /* Waywallen treats a new BIND as the lifetime boundary for the previous
     * pool.  Do the same here: detach every older pool for this output before
     * importing the replacement, instead of waiting for a later UNBIND that
     * may be delayed or lost during rapid video-to-video reconfiguration. */
    retireGenerationsForOutput(generation.outputId, QStringLiteral("bind-replaced"));

    for (const QJsonValue& bufferValue : buffers) {
        const QJsonObject bufferObject = bufferValue.toObject();
        Buffer buffer;
        buffer.index = jsonUInt32(bufferObject.value(QStringLiteral("index")));
        buffer.size = jsonUInt64(bufferObject.value(QStringLiteral("size")));

        const QJsonArray planes = bufferObject.value(QStringLiteral("planes")).toArray();
        for (const QJsonValue& planeValue : planes) {
            const QJsonObject planeObject = planeValue.toObject();
            const quint32 fdIndex = jsonUInt32(planeObject.value(QStringLiteral("fdIndex")));
            const int fd = vivid_display_recv_state_steal_fd(state, fdIndex);
            if (fd < 0) {
                closeGenerationFds(generation);
                const QString message =
                    QStringLiteral("BIND_BUFFERS references missing fd index %1").arg(fdIndex);
                setLastError(message);
                sendBindFailed(generation, 1, message);
                return;
            }

            Plane plane;
            plane.fd = fd;
            plane.stride = jsonUInt32(planeObject.value(QStringLiteral("stride")));
            plane.offset = jsonUInt32(planeObject.value(QStringLiteral("offset")));
            buffer.planes.push_back(plane);
        }

        if (buffer.planes.isEmpty()) {
            closeGenerationFds(generation);
            const QString message = QStringLiteral("BIND_BUFFERS buffer has no planes");
            setLastError(message);
            sendBindFailed(generation, 1, message);
            return;
        }
        const quint32 bufferPlanes = static_cast<quint32>(buffer.planes.size());
        if (generation.planesPerBuffer == 0) {
            generation.planesPerBuffer = bufferPlanes;
        } else if (generation.planesPerBuffer != bufferPlanes) {
            closeGenerationFds(generation);
            const QString message =
                QStringLiteral("BIND_BUFFERS planes_per_buffer mismatch expected=%1 actual=%2")
                    .arg(generation.planesPerBuffer)
                    .arg(bufferPlanes);
            setLastError(message);
            sendBindFailed(generation, 1, message);
            return;
        }
        generation.buffers.push_back(buffer);
    }

    /*
     * waywallen completes Vulkan DMA-BUF import and creates one reusable
     * acquire semaphore per slot while handling BIND_BUFFERS.  Keep the same
     * invariant here: once SET_CONFIG / FRAME_READY are dispatched on later
     * event-loop iterations, the pool is already GPU-ready and the render
     * thread only has to consume the pending frame and run the shadow copy.
     * Vulkan object creation/import does not require a current Qt render
     * context; it uses the VkDevice captured from the initialized scene graph.
    */
    if (m_activeBackend == BackendVulkan) {
        bool imported = m_vkBackendReady;
        for (Buffer& buffer : generation.buffers) {
            if (!imported || !ensureVulkanBufferImported(generation, buffer)) {
                imported = false;
                break;
            }
            if (buffer.acquireSemaphore == VK_NULL_HANDLE) {
                const VkSemaphoreCreateInfo createInfo {
                    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                };
                const VkResult createResult =
                    m_vkBackend.vkCreateSemaphore(m_vkDevice,
                                                  &createInfo,
                                                  nullptr,
                                                  &buffer.acquireSemaphore);
                if (createResult != VK_SUCCESS) {
                    setLastError(QStringLiteral("vkCreateSemaphore(acquire) failed during BIND_BUFFERS result=%1")
                                     .arg(static_cast<int>(createResult)));
                    imported = false;
                    break;
                }
            }
        }
        if (!imported) {
            const QString message = m_lastError.isEmpty()
                ? QStringLiteral("Vulkan DMA-BUF pool import failed during BIND_BUFFERS")
                : m_lastError;
            destroyImportedResources(generation);
            closeGenerationFds(generation);
            sendBindFailed(generation, 2, message);
            return;
        }
        qCInfo(lcWallpaperKde,
               "Vulkan pool imported during BIND_BUFFERS generation=%llu buffers=%lld",
               static_cast<unsigned long long>(generation.id),
               static_cast<long long>(generation.buffers.size()));
    } else if (m_activeBackend == BackendEgl) {
        /*
         * Match Waywallen's two-stage EGL lifecycle.  eglCreateImageKHR does
         * not require a current GL context, so import the complete pool while
         * handling BIND_BUFFERS on the display I/O thread and close every
         * protocol plane fd immediately.  The Qt render thread later creates
         * only the GL texture siblings and performs the shadow copy.
         */
        const EGLDisplay eglDisplay = qtWindowEglDisplay(window());
        bool imported = eglDisplay != EGL_NO_DISPLAY;
        for (Buffer& buffer : generation.buffers) {
            if (!imported || !importEglImage(generation, buffer, eglDisplay)) {
                imported = false;
                break;
            }
        }
        if (!imported) {
            const QString message = m_lastError.isEmpty()
                ? QStringLiteral("EGL DMA-BUF pool import failed during BIND_BUFFERS")
                : m_lastError;
            destroyImportedResources(generation);
            closeGenerationFds(generation);
            sendBindFailed(generation, 2, message);
            return;
        }
        qCInfo(lcWallpaperKde,
               "EGLImage pool imported during BIND_BUFFERS generation=%llu buffers=%lld",
               static_cast<unsigned long long>(generation.id),
               static_cast<long long>(generation.buffers.size()));
    }

    m_generations.push_back(generation);
    setStreamState(Active);
    qCInfo(lcWallpaperKde,
           "bound output=%u generation=%llu size=%dx%d fourcc=0x%08x modifier=0x%016llx planes_per_buffer=%u buffers=%lld path=%s presentation=%s memory-source=%s memory-hint=%s producer-render-node=%s producer-drm=%u:%u consumer-render-node=%s consumer-drm=%u:%u reused-id=%s",
           generation.outputId,
           static_cast<unsigned long long>(generation.id),
           generation.width,
           generation.height,
           generation.fourcc,
           static_cast<unsigned long long>(generation.modifier),
           generation.planesPerBuffer,
           static_cast<long long>(generation.buffers.size()),
           qPrintable(generation.negotiatedPath.isEmpty()
                          ? QStringLiteral("(unknown)")
                          : generation.negotiatedPath),
           qPrintable(generation.presentationPath.isEmpty()
                          ? QStringLiteral("(unknown)")
                          : generation.presentationPath),
           qPrintable(generation.memorySource.isEmpty()
                          ? QStringLiteral("(unknown)")
                          : generation.memorySource),
           qPrintable(generation.memoryHint.isEmpty()
                          ? QStringLiteral("(unknown)")
                          : generation.memoryHint),
           qPrintable(generation.renderNode.isEmpty()
                          ? QStringLiteral("(unknown)")
                          : generation.renderNode),
           generation.producerDrmRenderMajor,
           generation.producerDrmRenderMinor,
           qPrintable(generation.consumerRenderNode.isEmpty()
                          ? QStringLiteral("(unknown)")
                          : generation.consumerRenderNode),
           generation.consumerDrmRenderMajor,
           generation.consumerDrmRenderMinor,
               reusedGenerationId ? "true" : "false");
}

void VividDisplay::handleSetConfig(const QJsonObject& object)
{
    const quint32 outputId = jsonUInt32(object.value(Proto::SET_CONFIG::outputId), m_outputId);

    const quint64 generationId = jsonUInt64(object.value(Proto::SET_CONFIG::generation));
    Generation* generationState = generationId != 0
        ? findGeneration(generationId)
        : latestPendingConfigGeneration(outputId);
    if (!generationState && generationId == 0)
        generationState = latestLiveGeneration(outputId);
    if (!generationState || (outputId != 0 && generationState->outputId != outputId)) {
        setLastError(QStringLiteral("SET_CONFIG references unknown output=%1 generation=%2")
                         .arg(outputId)
                         .arg(generationId));
        return;
    }

    const QJsonObject source = object.value(Proto::SET_CONFIG::source).toObject();
    const QJsonObject destination = object.value(Proto::SET_CONFIG::destination).toObject();
    const QJsonArray clear = object.value(Proto::SET_CONFIG::clearColor).toArray();

    m_sourceRect = QRectF(source.value(Proto::RECT::x).toDouble(0.0),
                          source.value(Proto::RECT::y).toDouble(0.0),
                          source.value(Proto::RECT::width).toDouble(source.value(QStringLiteral("w")).toDouble(m_outputGeometry.physicalWidth)),
                          source.value(Proto::RECT::height).toDouble(source.value(QStringLiteral("h")).toDouble(m_outputGeometry.physicalHeight)));
    m_destRect = QRectF(destination.value(Proto::RECT::x).toDouble(0.0),
                        destination.value(Proto::RECT::y).toDouble(0.0),
                        destination.value(Proto::RECT::width).toDouble(destination.value(QStringLiteral("w")).toDouble(m_outputGeometry.physicalWidth)),
                        destination.value(Proto::RECT::height).toDouble(destination.value(QStringLiteral("h")).toDouble(m_outputGeometry.physicalHeight)));
    m_transform = transformCode(object.value(Proto::SET_CONFIG::transform));

    const QColor nextClear = QColor::fromRgbF(clear.at(0).toDouble(0.0),
                                              clear.at(1).toDouble(0.0),
                                              clear.at(2).toDouble(0.0),
                                              clear.at(3).toDouble(1.0));
    if (m_clearColor != nextClear) {
        m_clearColor = nextClear;
        emit clearColorChanged();
    }
    generationState->hasConfig = true;
    generationState->configGeneration =
        jsonUInt64(object.value(Proto::SET_CONFIG::configGeneration), generationState->configGeneration);


    // Keep the GUI-to-render-thread hand-off identical to Waywallen:
    // configuration changes always dirty the native item, including the
    // initial SET_CONFIG immediately before the first FRAME_READY.
    update();
}

void VividDisplay::handleFrameReady(const QByteArray& body, VividDisplayRecvState* state)
{
    if (body.size() != static_cast<int>(Proto::VIVID_DISPLAY_FRAME_READY_BODY_BYTES)) {
        setLastError(QStringLiteral("invalid FRAME_READY body length %1").arg(body.size()));
        return;
    }

    if (!state || state->n_fds != Proto::VIVID_DISPLAY_FRAME_READY_FD_COUNT) {
        setLastError(QStringLiteral("FRAME_READY invalid explicit sync fd count=%1 expected=%2")
                         .arg(state ? static_cast<qulonglong>(state->n_fds) : 0)
                         .arg(static_cast<qulonglong>(Proto::VIVID_DISPLAY_FRAME_READY_FD_COUNT)));
        closeRecvStateFds(state);
        return;
    }

    int acquireFd = vivid_display_recv_state_steal_fd(state, 0);
    int releaseFd = vivid_display_recv_state_steal_fd(state, 1);
    auto closeFrameFds = [&]() {
        closeFd(acquireFd);
        closeFd(releaseFd);
    };
    if (acquireFd < 0 || releaseFd < 0) {
        closeFrameFds();
        setLastError(QStringLiteral("FRAME_READY explicit sync fd extraction failed"));
        return;
    }

    uint32_t outputIdRaw = 0;
    uint64_t generationRaw = 0;
    uint32_t bufferIndexRaw = 0;
    uint64_t sequenceRaw = 0;
    uint64_t targetTimeUsecRaw = 0;
    uint32_t flagsRaw = 0;
    if (vivid_display_frame_ready_body_read(reinterpret_cast<const uint8_t*>(body.constData()),
                                             static_cast<size_t>(body.size()),
                                             &outputIdRaw,
                                             &generationRaw,
                                             &bufferIndexRaw,
                                             &sequenceRaw,
                                             &targetTimeUsecRaw,
                                             &flagsRaw) < 0) {
        closeFrameFds();
        setLastError(QStringLiteral("invalid FRAME_READY body layout"));
        return;
    }

    const quint32 outputId = outputIdRaw;
    const quint64 generation = generationRaw;
    const quint32 bufferIndex = bufferIndexRaw;
    const quint64 sequence = sequenceRaw;
    const quint64 targetTimeUsec = targetTimeUsecRaw;

    if (m_outputId != 0 && outputId != m_outputId) {
        qCWarning(lcWallpaperKde, "FRAME_READY for unknown output=%u current=%u", outputId, m_outputId);
        closeFrameFds();
        return;
    }

    Generation* generationState = findGeneration(generation);
    if (generationState && (generationState->retired || generationState->resourcesQueued)) {
        qCDebug(lcWallpaperKde,
                "dropping FRAME_READY for released generation=%llu buffer=%u",
                static_cast<unsigned long long>(generation),
                bufferIndex);
        signalReleaseSyncobj(generationState->renderNode,
                             releaseFd,
                             QStringLiteral("released-generation"));
        closeFrameFds();
        return;
    }
    Buffer* buffer = findBuffer(generationState, bufferIndex);
    if (!generationState || !buffer) {
        qCWarning(lcWallpaperKde,
                  "FRAME_READY references unknown generation=%llu buffer=%u",
                  static_cast<unsigned long long>(generation),
                  bufferIndex);
        if (generationState)
            signalReleaseSyncobj(generationState->renderNode, releaseFd, QStringLiteral("unknown-buffer"));
        closeFrameFds();
        return;
    }
    if (!generationState->hasConfig) {
        qCWarning(lcWallpaperKde,
                  "FRAME_READY rejected before SET_CONFIG output=%u generation=%llu buffer=%u",
                  outputId,
                  static_cast<unsigned long long>(generation),
                  bufferIndex);
        signalReleaseSyncobj(generationState->renderNode, releaseFd, QStringLiteral("pending-config"));
        closeFrameFds();
        return;
    }

    const QString syncContext =
        QStringLiteral("output=%1 generation=%2 buffer=%3 sequence=%4 target_us=%5")
            .arg(outputId)
            .arg(generation)
            .arg(bufferIndex)
            .arg(sequence)
            .arg(targetTimeUsec);
    if (m_activeBackend == BackendVulkan) {
        /* Match Waywallen's KDE Vulkan hand-off: the socket/GUI thread only
         * imports the acquire sync_fd and replaces the pending frame. It must
         * never submit Vulkan work or wait for a GPU fence here, otherwise a
         * full-rate producer can starve Plasma's GUI-to-QSG synchronization
         * and the first texture node is never created. */
        signalPendingVulkanFrame(QStringLiteral("superseded"));
        if (!m_vkBackendReady) {
            signalReleaseSyncobj(generationState->renderNode,
                                 releaseFd,
                                 QStringLiteral("vulkan-backend-not-ready"));
            closeFrameFds();
            return;
        }
        if (buffer->acquireSemaphore == VK_NULL_HANDLE) {
            const VkSemaphoreCreateInfo createInfo {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
            };
            const VkResult createResult =
                m_vkBackend.vkCreateSemaphore(m_vkDevice,
                                              &createInfo,
                                              nullptr,
                                              &buffer->acquireSemaphore);
            if (createResult != VK_SUCCESS) {
                setLastError(QStringLiteral("vkCreateSemaphore(acquire) failed result=%1")
                                 .arg(static_cast<int>(createResult)));
                signalReleaseSyncobj(generationState->renderNode,
                                     releaseFd,
                                     QStringLiteral("acquire-semaphore-create-failed"));
                closeFrameFds();
                return;
            }
        }
        const int importRc =
            ww_vk_import_sync_fd(&m_vkBackend, buffer->acquireSemaphore, acquireFd);
        if (importRc != 0) {
            setLastError(QStringLiteral("ww_vk_import_sync_fd failed generation=%1 buffer=%2 rc=%3")
                             .arg(generation)
                             .arg(bufferIndex)
                             .arg(importRc));
            signalReleaseSyncobj(generationState->renderNode,
                                 releaseFd,
                                 QStringLiteral("acquire-sync-import-failed"));
            closeFrameFds();
            return;
        }
        acquireFd = -1;
        {
            QMutexLocker lock(&m_pendingVulkanMutex);
            m_currentGeneration = generation;
            m_currentBuffer = bufferIndex;
            m_pendingVulkanFrame.valid = true;
            m_pendingVulkanFrame.generation = generation;
            m_pendingVulkanFrame.bufferIndex = bufferIndex;
            m_pendingVulkanFrame.releaseSyncobjFd = releaseFd;
            m_pendingVulkanFrame.renderNode = generationState->renderNode;
            m_pendingVulkanFrame.releaseContext = syncContext;
        }
        releaseFd = -1;
    } else if (m_activeBackend == BackendEgl) {
        /*
         * Waywallen releases the previously accepted EGL frame when the next
         * FRAME_READY arrives, then coalesces the newest slot into a
         * BeforeSynchronizingStage render job. Keep that cadence, but carry the
         * acquire fence into the render job so eglWaitSyncKHR runs with Qt's GL
         * context current instead of blocking this socket callback.
         */
        signalPendingEglRelease(QStringLiteral("next-frame"));

        PendingEglFrame superseded;
        {
            QMutexLocker lock(&m_pendingEglMutex);
            superseded = std::move(m_pendingEglFrame);

            m_pendingEglFrame.valid = true;
            m_pendingEglFrame.generation = generation;
            m_pendingEglFrame.bufferIndex = bufferIndex;
            m_pendingEglFrame.sequence = sequence;
            m_pendingEglFrame.acquireSyncFd = acquireFd;
            m_pendingEglFrame.acquireContext = syncContext;

            m_pendingEglRelease.generation = generation;
            m_pendingEglRelease.sequence = sequence;
            m_pendingEglRelease.syncobjFd = releaseFd;
            m_pendingEglRelease.renderNode = generationState->renderNode;
            m_pendingEglRelease.context = syncContext;
        }
        acquireFd = -1;
        releaseFd = -1;
        closeFd(superseded.acquireSyncFd);

        QQuickWindow* quickWindow = window();
        if (!quickWindow) {
            discardPendingEglFrame(generation);
            signalPendingEglRelease(QStringLiteral("window-missing"), generation);
            closeFrameFds();
            return;
        }
        quickWindow->scheduleRenderJob(
            new FunctionRenderJob([this]() {
                renderThreadBlitEgl();
            }),
            QQuickWindow::BeforeSynchronizingStage);
    } else {
        signalReleaseSyncobj(generationState->renderNode,
                             releaseFd,
                             QStringLiteral("backend-not-ready"));
        closeFrameFds();
        return;
    }

    m_framesReceived++;
    setStreamState(Active);
    emit framesReceivedChanged();
    // Match Waywallen's Vulkan frame callback: hand the newest frame to the
    // render thread and dirty the native item once. Qt's normal scene-graph
    // scheduling owns the GUI-to-render-thread transition.
    update();
    closeFd(releaseFd);
}

void VividDisplay::handleUnbind(const QByteArray& body)
{
    if (body.size() != static_cast<int>(Proto::VIVID_DISPLAY_UNBIND_BODY_BYTES)) {
        setLastError(QStringLiteral("invalid UNBIND body length %1").arg(body.size()));
        return;
    }

    uint32_t outputIdRaw = 0;
    uint64_t generationRaw = 0;
    if (vivid_display_unbind_body_read(reinterpret_cast<const uint8_t*>(body.constData()),
                                      static_cast<size_t>(body.size()),
                                      &outputIdRaw,
                                      &generationRaw) < 0) {
        setLastError(QStringLiteral("invalid UNBIND body layout"));
        return;
    }

    const quint32 outputId = outputIdRaw;
    const quint64 generation = generationRaw;
    retireGeneration(generation);
    queueJsonFrame(Proto::VIVID_DISPLAY_REQ_UNBIND_DONE,
                   QJsonObject {
                       { Proto::UNBIND_DONE::outputId, static_cast<qint64>(outputId) },
                       { Proto::UNBIND_DONE::generation, QString::number(generation) },
                   });
    if (!latestLiveGeneration(outputId != 0 ? outputId : m_outputId)) {
        /* The socket notifier runs on Plasma's GUI thread.  In the OpenGL
         * route that thread has no current EGL context, so destroying the
         * EGLImages here would either be skipped (leaking the driver's
         * dma-buf references) or would call GL on the wrong thread.  Keep the
         * retired generation until updatePaintNode runs on the scene-graph
         * thread; it will release both the EGLImage and its native texture. */
        setStreamState(Inactive);
    }
    update();
}

void VividDisplay::handleProtocolError(int code, const QString& message)
{
    setLastError(message.isEmpty() ? formatErrno(code) : message);
    closeTransport(true);
    releaseIdleProtocolBackend();
    setConnState(Error);
    refreshSocketWatcher();
    m_errorReconnectDelayMs = 1200;

    /* Missing/refused startup sockets are driven by QFileSystemWatcher. The
     * producer must create or replace the Unix socket before it can accept a
     * connection, which changes the watched file/directory. Waywallen likewise
     * stays idle after its first failed connect and waits for its daemon-ready
     * signal instead of continuously dirtying the wallpaper surface. */
    if (code == ENOENT || code == ECONNREFUSED)
        return;

    scheduleReconnect(m_errorReconnectDelayMs);
}

void VividDisplay::handleProducerError(quint32 code, bool fatal, const QString& message)
{
    const QString formatted =
        QStringLiteral("producer error %1 %2 (%3/%4): %5")
            .arg(code)
            .arg(QString::fromLatin1(Proto::error_name(static_cast<int>(code)).data()))
            .arg(fatal ? QStringLiteral("fatal") : QStringLiteral("non-fatal"))
            .arg(QString::fromLatin1(Proto::error_domain(static_cast<int>(code)).data()))
            .arg(message);
    setLastError(formatted);
    qCWarning(lcWallpaperKde, "%s", qPrintable(formatted));

    if (!fatal)
        return;

    closeTransport(true);
    releaseIdleProtocolBackend();
    setConnState(Error);
    m_errorReconnectDelayMs = std::min(m_errorReconnectDelayMs * 2, 30000);
    scheduleReconnect(m_errorReconnectDelayMs);
}

VividDisplay::Generation* VividDisplay::findGeneration(quint64 generation)
{
    for (qsizetype i = m_generations.size() - 1; i >= 0; --i) {
        if (m_generations[i].id == generation)
            return &m_generations[i];
    }
    return nullptr;
}

VividDisplay::Generation* VividDisplay::latestPendingConfigGeneration(quint32 outputId)
{
    for (qsizetype i = m_generations.size() - 1; i >= 0; --i) {
        Generation& generation = m_generations[i];
        if (generation.retired || generation.hasConfig)
            continue;
        if (outputId != 0 && generation.outputId != outputId)
            continue;
        return &generation;
    }
    return nullptr;
}

VividDisplay::Generation* VividDisplay::latestLiveGeneration(quint32 outputId)
{
    for (qsizetype i = m_generations.size() - 1; i >= 0; --i) {
        Generation& generation = m_generations[i];
        if (generation.retired)
            continue;
        if (outputId != 0 && generation.outputId != outputId)
            continue;
        return &generation;
    }
    return nullptr;
}

VividDisplay::Buffer* VividDisplay::findBuffer(Generation* generation, quint32 index)
{
    if (!generation)
        return nullptr;
    for (Buffer& buffer : generation->buffers) {
        if (buffer.index == index)
            return &buffer;
    }
    return nullptr;
}

void VividDisplay::retireGeneration(quint64 generation)
{
    for (Generation& item : m_generations) {
        if (item.id == generation)
            queueGenerationResources(item, QStringLiteral("generation-retired"));
    }
}

void VividDisplay::retireGenerationsForOutput(quint32 outputId, const QString& reason)
{
    for (Generation& generation : m_generations) {
        if (outputId == 0 || generation.outputId == outputId)
            queueGenerationResources(generation, reason);
    }
}

void VividDisplay::retireAllGenerations(const QString& reason)
{
    for (Generation& generation : m_generations)
        queueGenerationResources(generation, reason);
}

void VividDisplay::queueGenerationResources(Generation& generation, const QString& reason)
{
    if (generation.resourcesQueued)
        return;

    closeGenerationFds(generation);

    Generation pending;
    pending.id = generation.id;
    pending.outputId = generation.outputId;
    pending.renderNode = generation.renderNode;
    pending.retired = true;
    pending.resourcesQueued = true;
    pending.buffers = std::move(generation.buffers);

    generation.retired = true;
    generation.resourcesQueued = true;

    qsizetype pendingCount = 0;
    quint64 queuedCount = 0;
    {
        QMutexLocker lock(&m_pendingGenerationMutex);
        m_pendingGenerations.push_back(std::move(pending));
        pendingCount = m_pendingGenerations.size();
        queuedCount = ++m_pendingPoolsQueued;
    }

    qCInfo(lcWallpaperKde,
           "queued generation GPU resources generation=%llu output=%u reason=%s pending=%lld queued-total=%llu",
           static_cast<unsigned long long>(generation.id),
           generation.outputId,
           qPrintable(reason),
           static_cast<long long>(pendingCount),
           static_cast<unsigned long long>(queuedCount));
}

void VividDisplay::closeGenerationFds(Generation& generation)
{
    discardPendingEglFrame(generation.id);
    signalPendingEglRelease(QStringLiteral("generation-close"), generation.id);

    bool pendingMatches = false;
    {
        QMutexLocker lock(&m_pendingVulkanMutex);
        pendingMatches = m_pendingVulkanFrame.valid &&
            m_pendingVulkanFrame.generation == generation.id;
    }
    if (pendingMatches)
        signalPendingVulkanFrame(QStringLiteral("generation-close"));

    for (Buffer& buffer : generation.buffers) {
        for (Plane& plane : buffer.planes) {
            if (plane.fd >= 0) {
                ::close(plane.fd);
                plane.fd = -1;
            }
        }
    }
}

void VividDisplay::discardPendingEglFrame(quint64 generation)
{
    PendingEglFrame pending;
    {
        QMutexLocker lock(&m_pendingEglMutex);
        if (!m_pendingEglFrame.valid ||
            (generation != 0 && m_pendingEglFrame.generation != generation)) {
            return;
        }
        pending = std::move(m_pendingEglFrame);
        m_pendingEglFrame = PendingEglFrame {};
    }
    closeFd(pending.acquireSyncFd);
}

void VividDisplay::signalPendingEglRelease(const QString& reason,
                                           quint64 generation,
                                           quint64 sequence)
{
    PendingEglRelease pending;
    {
        QMutexLocker lock(&m_pendingEglMutex);
        if (m_pendingEglRelease.syncobjFd < 0 ||
            (generation != 0 && m_pendingEglRelease.generation != generation) ||
            (sequence != 0 && m_pendingEglRelease.sequence != sequence)) {
            return;
        }
        pending = std::move(m_pendingEglRelease);
        m_pendingEglRelease = PendingEglRelease {};
    }
    if (pending.syncobjFd < 0)
        return;

    const QString context = pending.context.isEmpty()
        ? reason
        : QStringLiteral("%1 %2").arg(pending.context, reason);
    signalReleaseSyncobj(pending.renderNode, pending.syncobjFd, context);
    closeFd(pending.syncobjFd);
}

void VividDisplay::signalPendingVulkanFrame(const QString& reason)
{
    PendingVulkanFrame pending;
    {
        QMutexLocker lock(&m_pendingVulkanMutex);
        pending = m_pendingVulkanFrame;
        m_pendingVulkanFrame = PendingVulkanFrame {};
    }
    if (pending.releaseSyncobjFd < 0)
        return;

    const QString context = pending.releaseContext.isEmpty()
        ? reason
        : QStringLiteral("%1 %2").arg(pending.releaseContext, reason);
    signalReleaseSyncobj(pending.renderNode, pending.releaseSyncobjFd, context);
    closeFd(pending.releaseSyncobjFd);
}

void VividDisplay::destroyImportedResources(Generation& generation)
{
    destroyImportedResourcesWithBackend(generation, &m_vkBackend, m_vkBackendReady);
}

void VividDisplay::destroyImportedResourcesWithBackend(Generation& generation,
                                                       ww_vk_backend_t* backend,
                                                       bool backendReady)
{
    auto* gl = QOpenGLContext::currentContext()
        ? QOpenGLContext::currentContext()->extraFunctions()
        : nullptr;
    const auto destroyImage = resolveEglProc<EglDestroyImageKhr>("eglDestroyImageKHR");

    int eglImages = 0;
    int eglImageFailures = 0;
    int glTextures = 0;

    /* Match Waywallen's pool teardown ordering: delete every GL sibling first,
     * then destroy the EGLImages that back the pool. */
    QVector<GLuint> importedTextures;
    if (gl) {
        importedTextures.reserve(generation.buffers.size());
        for (Buffer& buffer : generation.buffers) {
            if (buffer.glTexture != 0) {
                importedTextures.push_back(buffer.glTexture);
                buffer.glTexture = 0;
            }
        }
        if (!importedTextures.isEmpty()) {
            /* Waywallen deletes the EGLImage texture siblings directly.
             * Re-defining them with glTexImage2D first creates replacement
             * NVIDIA storage and defeats the purpose of releasing the import;
             * on this driver that path opens another render-node/nvidiactl
             * pair per texture. */
            gl->glDeleteTextures(static_cast<GLsizei>(importedTextures.size()),
                                 importedTextures.constData());
            glTextures = importedTextures.size();
        }
    }

    for (Buffer& buffer : generation.buffers) {
        if (destroyImage && buffer.eglImage != EGL_NO_IMAGE_KHR) {
            const EGLDisplay imageDisplay = buffer.eglDisplay != EGL_NO_DISPLAY
                ? buffer.eglDisplay
                : eglGetCurrentDisplay();
            if (imageDisplay != EGL_NO_DISPLAY &&
                destroyImage(imageDisplay, buffer.eglImage) == EGL_TRUE) {
                eglImages++;
            } else {
                const EGLint error = eglGetError();
                qCWarning(lcWallpaperKde,
                          "eglDestroyImageKHR failed generation=%llu buffer=%u "
                          "display=%p image=%p egl=0x%x",
                          static_cast<unsigned long long>(generation.id),
                          buffer.index,
                          static_cast<void*>(imageDisplay),
                          static_cast<void*>(buffer.eglImage),
                          static_cast<unsigned>(error));
                eglImageFailures++;
            }
            const EGLint destroyError = eglGetError();
            if (destroyError != EGL_SUCCESS) {
                qCWarning(lcWallpaperKde,
                          "eglDestroyImageKHR post-error generation=%llu buffer=%u error=0x%x",
                          static_cast<unsigned long long>(generation.id),
                          buffer.index,
                          static_cast<unsigned>(destroyError));
            }
            buffer.eglImage = EGL_NO_IMAGE_KHR;
            buffer.eglDisplay = EGL_NO_DISPLAY;
        }
        if (buffer.hasVkImage && backendReady && backend) {
            ww_vk_destroy_imported_image(backend, &buffer.vkImage);
            buffer.hasVkImage = false;
        }
        if (buffer.acquireSemaphore != VK_NULL_HANDLE && backendReady && backend &&
            backend->vkDestroySemaphore) {
            backend->vkDestroySemaphore(backend->device, buffer.acquireSemaphore, nullptr);
            buffer.acquireSemaphore = VK_NULL_HANDLE;
        }
        buffer.importAttempted = false;
    }

    if (eglImages || eglImageFailures || glTextures) {
        qCInfo(lcWallpaperKde,
               "destroyed retired EGL resources generation=%llu images=%d image-failures=%d "
               "gl-textures=%d context=%p",
               static_cast<unsigned long long>(generation.id),
               eglImages,
               eglImageFailures,
               glTextures,
               static_cast<void*>(QOpenGLContext::currentContext()));
    }
}

void VividDisplay::clearGenerations(bool destroyGlResources)
{
    retireAllGenerations(QStringLiteral("clear-generations"));
    m_generations.clear();

    if (!destroyGlResources)
        return;

    while (drainPendingGenerationResources() > 0) {
    }
    destroyEglShadowResources();
}

int VividDisplay::drainPendingGenerationResources()
{
    if (m_vkBlitterReady && m_vkBlitter.fence_armed)
        return 0;

    QVector<Generation> pending;
    {
        QMutexLocker lock(&m_pendingGenerationMutex);
        if (m_pendingGenerations.isEmpty())
            return 0;
        pending.swap(m_pendingGenerations);
    }

    bool needsGlContext = false;
    bool needsVulkanBackend = false;
    for (const Generation& generation : pending) {
        for (const Buffer& buffer : generation.buffers) {
            needsGlContext = needsGlContext || buffer.eglImage != EGL_NO_IMAGE_KHR ||
                buffer.glTexture != 0;
            needsVulkanBackend = needsVulkanBackend || buffer.hasVkImage ||
                buffer.acquireSemaphore != VK_NULL_HANDLE;
        }
    }

    const bool canDestroy = (!needsGlContext || QOpenGLContext::currentContext()) &&
        (!needsVulkanBackend || m_vkBackendReady);
    if (!canDestroy) {
        QMutexLocker lock(&m_pendingGenerationMutex);
        QVector<Generation> newlyQueued;
        newlyQueued.swap(m_pendingGenerations);
        for (Generation& generation : pending)
            m_pendingGenerations.push_back(std::move(generation));
        for (Generation& generation : newlyQueued)
            m_pendingGenerations.push_back(std::move(generation));
        qCWarning(lcWallpaperKde,
                  "pending generation drain deferred gl-context=%s vk-backend=%s pending=%lld",
                  QOpenGLContext::currentContext() ? "current" : "missing",
                  m_vkBackendReady ? "ready" : "missing",
                  static_cast<long long>(m_pendingGenerations.size()));
        return -EAGAIN;
    }

    const int drained = pending.size();
    for (Generation& generation : pending)
        destroyImportedResources(generation);

    quint64 drainedTotal = 0;
    qsizetype remaining = 0;
    {
        QMutexLocker lock(&m_pendingGenerationMutex);
        m_pendingPoolsDrained += static_cast<quint64>(drained);
        drainedTotal = m_pendingPoolsDrained;
        remaining = m_pendingGenerations.size();
    }
    qCInfo(lcWallpaperKde,
           "drained generation GPU resources pools=%d pending=%lld drained-total=%llu",
           drained,
           static_cast<long long>(remaining),
           static_cast<unsigned long long>(drainedTotal));
    return drained;
}

void VividDisplay::destroyEglShadowResources()
{
    auto* context = QOpenGLContext::currentContext();
    auto* gl = context ? context->extraFunctions() : nullptr;
    const bool hasResources = m_eglShadowReadFramebuffer != 0 ||
        m_eglShadowFramebuffer != 0 || m_eglShadowTexture != 0;
    if (hasResources && !gl) {
        qCWarning(lcWallpaperKde,
                  "EGL shadow cleanup deferred because no GL context is current");
        return;
    }
    if (gl && hasResources) {
        if (m_eglShadowReadFramebuffer != 0) {
            GLuint fbo = m_eglShadowReadFramebuffer;
            gl->glDeleteFramebuffers(1, &fbo);
        }
        if (m_eglShadowFramebuffer != 0) {
            GLuint fbo = m_eglShadowFramebuffer;
            gl->glDeleteFramebuffers(1, &fbo);
        }
        if (m_eglShadowTexture != 0) {
            GLuint texture = m_eglShadowTexture;
            gl->glDeleteTextures(1, &texture);
        }
    }
    m_eglShadowReadFramebuffer = 0;
    m_eglShadowFramebuffer = 0;
    m_eglShadowTexture = 0;
    m_eglShadowWidth = 0;
    m_eglShadowHeight = 0;
    m_eglShadowHasContent = false;
    m_eglShadowGeneration = 0;
    m_eglShadowBuffer = 0;
    m_eglShadowSequence = 0;
}

VividDisplay::FinalGpuCleanup* VividDisplay::takeFinalGpuCleanup()
{
    retireAllGenerations(QStringLiteral("final-cleanup"));
    m_generations.clear();

    auto* cleanup = new FinalGpuCleanup;
    {
        QMutexLocker lock(&m_pendingGenerationMutex);
        cleanup->generations.swap(m_pendingGenerations);
    }

    cleanup->vkBackend = m_vkBackend;
    cleanup->vkBackendReady = m_vkBackendReady;
    std::memset(&m_vkBackend, 0, sizeof(m_vkBackend));
    m_vkBackendReady = false;

    cleanup->vkBlitter = m_vkBlitter;
    cleanup->vkBlitterReady = m_vkBlitterReady;
    std::memset(&m_vkBlitter, 0, sizeof(m_vkBlitter));
    m_vkBlitterReady = false;

    cleanup->eglShadowTexture = m_eglShadowTexture;
    cleanup->eglShadowReadFramebuffer = m_eglShadowReadFramebuffer;
    cleanup->eglShadowFramebuffer = m_eglShadowFramebuffer;
    m_eglShadowTexture = 0;
    m_eglShadowReadFramebuffer = 0;
    m_eglShadowFramebuffer = 0;
    m_eglShadowWidth = 0;
    m_eglShadowHeight = 0;
    m_eglShadowHasContent = false;
    m_eglShadowGeneration = 0;
    m_eglShadowBuffer = 0;
    m_eglShadowSequence = 0;

    m_vkInstance = VK_NULL_HANDLE;
    m_vkPhysicalDevice = VK_NULL_HANDLE;
    m_vkDevice = VK_NULL_HANDLE;
    m_vkQueue = VK_NULL_HANDLE;
    m_vkQueueFamilyIndex = 0;
    m_vkGetInstanceProcAddr = nullptr;

    const bool hasCleanup = !cleanup->generations.isEmpty() || cleanup->vkBackendReady ||
        cleanup->vkBlitterReady || cleanup->eglShadowTexture != 0 ||
        cleanup->eglShadowReadFramebuffer != 0 || cleanup->eglShadowFramebuffer != 0;
    if (!hasCleanup) {
        delete cleanup;
        return nullptr;
    }
    return cleanup;
}

bool VividDisplay::destroyFinalGpuCleanup(FinalGpuCleanup* cleanup)
{
    if (!cleanup)
        return true;

    bool needsGlContext = cleanup->eglShadowTexture != 0 ||
        cleanup->eglShadowReadFramebuffer != 0 || cleanup->eglShadowFramebuffer != 0;
    bool needsVulkanBackend = false;
    for (const Generation& generation : cleanup->generations) {
        for (const Buffer& buffer : generation.buffers) {
            needsGlContext = needsGlContext || buffer.eglImage != EGL_NO_IMAGE_KHR ||
                buffer.glTexture != 0;
            needsVulkanBackend = needsVulkanBackend || buffer.hasVkImage ||
                buffer.acquireSemaphore != VK_NULL_HANDLE;
        }
    }

    if (needsGlContext && !QOpenGLContext::currentContext()) {
        qCCritical(lcWallpaperKde,
                   "final Vivid GPU cleanup reached render job without a current GL context");
        return false;
    }
    if (needsVulkanBackend && !cleanup->vkBackendReady) {
        qCCritical(lcWallpaperKde,
                   "final Vivid GPU cleanup has Vulkan imports but no live backend");
        return false;
    }

    /* The blitter may still own an armed fence referencing an imported pool.
     * Shut it down first so its wait completes before those VkImages are
     * destroyed. */
    if (cleanup->vkBlitterReady) {
        ww_vk_blitter_shutdown(&cleanup->vkBlitter);
        cleanup->vkBlitterReady = false;
    }

    for (Generation& generation : cleanup->generations) {
        destroyImportedResourcesWithBackend(generation,
                                            &cleanup->vkBackend,
                                            cleanup->vkBackendReady);
    }

    if (needsGlContext) {
        auto* gl = QOpenGLContext::currentContext()->extraFunctions();
        if (cleanup->eglShadowReadFramebuffer != 0)
            gl->glDeleteFramebuffers(1, &cleanup->eglShadowReadFramebuffer);
        if (cleanup->eglShadowFramebuffer != 0)
            gl->glDeleteFramebuffers(1, &cleanup->eglShadowFramebuffer);
        if (cleanup->eglShadowTexture != 0)
            gl->glDeleteTextures(1, &cleanup->eglShadowTexture);
    }

    if (cleanup->vkBackendReady) {
        ww_vk_backend_unload(&cleanup->vkBackend);
        cleanup->vkBackendReady = false;
    }

    qCInfo(lcWallpaperKde,
           "final Vivid GPU cleanup drained pools=%lld",
           static_cast<long long>(cleanup->generations.size()));
    return true;
}

bool VividDisplay::ensureEglShadowResources(int width, int height)
{
    if (width <= 0 || height <= 0)
        return false;
    if (m_eglShadowTexture != 0 && m_eglShadowReadFramebuffer != 0 &&
        m_eglShadowFramebuffer != 0 && m_eglShadowWidth == width &&
        m_eglShadowHeight == height) {
        return true;
    }

    auto* context = QOpenGLContext::currentContext();
    auto* gl = context ? context->extraFunctions() : nullptr;
    if (!gl)
        return false;

    /*
     * Keep the same native texture name across resizes, as Waywallen does.
     * QSG only ever wraps this consumer-owned texture, so reallocating its
     * storage avoids churn in imported producer textures and keeps the last
     * completed frame independent from pool replacement.
     */
    if (m_eglShadowTexture == 0) {
        gl->glGenTextures(1, &m_eglShadowTexture);
        gl->glBindTexture(GL_TEXTURE_2D, m_eglShadowTexture);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    gl->glBindTexture(GL_TEXTURE_2D, m_eglShadowTexture);
    gl->glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA8,
                     width,
                     height,
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     nullptr);

    if (m_eglShadowFramebuffer == 0)
        gl->glGenFramebuffers(1, &m_eglShadowFramebuffer);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_eglShadowFramebuffer);
    gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               m_eglShadowTexture,
                               0);
    const GLenum drawStatus = gl->glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);

    if (m_eglShadowReadFramebuffer == 0)
        gl->glGenFramebuffers(1, &m_eglShadowReadFramebuffer);
    const bool ok = drawStatus == GL_FRAMEBUFFER_COMPLETE &&
        m_eglShadowTexture != 0 && m_eglShadowFramebuffer != 0 &&
        m_eglShadowReadFramebuffer != 0;
    if (!ok) {
        destroyEglShadowResources();
        setLastError(QStringLiteral("consumer shadow framebuffer incomplete status=0x%1")
                         .arg(static_cast<uint>(drawStatus), 0, 16));
        return false;
    }
    m_eglShadowWidth = width;
    m_eglShadowHeight = height;
    m_eglShadowHasContent = false;
    m_eglShadowGeneration = 0;
    m_eglShadowBuffer = 0;
    m_eglShadowSequence = 0;
    return true;
}

void VividDisplay::destroyRetiredResources()
{
    (void)drainPendingGenerationResources();

    int removed = 0;
    for (qsizetype i = m_generations.size() - 1; i >= 0; --i) {
        /* The current retired entry is presentation-only metadata for the
         * persistent shadow texture.  Keep it until a replacement frame
         * becomes current; its GPU handles have already moved to the pending
         * queue and are not retained here. */
        if (!m_generations[i].retired || isCurrentGenerationIndex(i))
            continue;
        m_generations.removeAt(i);
        removed++;
    }
    if (removed) {
        qCInfo(lcWallpaperKde,
               "retired generation sweep removed=%d remaining=%lld live=%s current=%llu",
               removed,
               static_cast<long long>(m_generations.size()),
               latestLiveGeneration(m_outputId) ? "true" : "false",
               static_cast<unsigned long long>(m_currentGeneration));
    }
}

bool VividDisplay::isCurrentGenerationIndex(qsizetype index) const
{
    if (index < 0 || index >= m_generations.size())
        return false;
    if (m_generations[index].id != m_currentGeneration)
        return false;

    for (qsizetype i = m_generations.size() - 1; i > index; --i) {
        if (m_generations[i].id == m_currentGeneration)
            return false;
    }
    return true;
}

bool VividDisplay::importEglImage(Generation& generation,
                                  Buffer&     buffer,
                                  EGLDisplay eglDisplay)
{
    if (buffer.eglImage != EGL_NO_IMAGE_KHR)
        return true;
    const auto createImage = resolveEglProc<EglCreateImageKhr>("eglCreateImageKHR");
    if (eglDisplay == EGL_NO_DISPLAY || !createImage) {
        setLastError(QStringLiteral("EGL DMA-BUF image import is unavailable"));
        return false;
    }

    const bool modifierIsImplicit =
        generation.modifier == DrmFormatModInvalid ||
        generation.modifier == DrmFormatModLinear;
    const bool hasModifierImport =
        eglDisplayHasExtension(eglDisplay, "EGL_EXT_image_dma_buf_import_modifiers");
    const bool includeModifier = !modifierIsImplicit;
    if (!hasModifierImport && !modifierIsImplicit) {
        setLastError(QStringLiteral(
                         "modifier=0x%1 requires EGL_EXT_image_dma_buf_import_modifiers")
                         .arg(static_cast<qulonglong>(generation.modifier), 16, 16, QLatin1Char('0')));
        return false;
    }

    QVector<EGLint> attrs;
    attrs.reserve(6 + buffer.planes.size() * 10);
    attrs << EGL_WIDTH << generation.width
          << EGL_HEIGHT << generation.height
          << EGL_LINUX_DRM_FOURCC_EXT << static_cast<EGLint>(generation.fourcc);

    for (int i = 0; i < buffer.planes.size(); i++) {
        const Plane& plane = buffer.planes[i];
        const EGLint fdAttr = i == 0 ? EGL_DMA_BUF_PLANE0_FD_EXT
            : i == 1 ? EGL_DMA_BUF_PLANE1_FD_EXT
            : i == 2 ? EGL_DMA_BUF_PLANE2_FD_EXT
                     : EGL_DMA_BUF_PLANE3_FD_EXT;
        const EGLint offsetAttr = i == 0 ? EGL_DMA_BUF_PLANE0_OFFSET_EXT
            : i == 1 ? EGL_DMA_BUF_PLANE1_OFFSET_EXT
            : i == 2 ? EGL_DMA_BUF_PLANE2_OFFSET_EXT
                     : EGL_DMA_BUF_PLANE3_OFFSET_EXT;
        const EGLint pitchAttr = i == 0 ? EGL_DMA_BUF_PLANE0_PITCH_EXT
            : i == 1 ? EGL_DMA_BUF_PLANE1_PITCH_EXT
            : i == 2 ? EGL_DMA_BUF_PLANE2_PITCH_EXT
                     : EGL_DMA_BUF_PLANE3_PITCH_EXT;
        attrs << fdAttr << plane.fd
              << offsetAttr << static_cast<EGLint>(plane.offset)
              << pitchAttr << static_cast<EGLint>(plane.stride);

        /*
         * Import LINEAR/INVALID buffers through the implicit-modifier EGL path
         * even when EGL_EXT_image_dma_buf_import_modifiers is available. Some
         * NVIDIA EGL stacks expose LINEAR as modifier-aware but only allow the
         * tagged import to bind as GL_TEXTURE_EXTERNAL_OES; Qt Quick needs a
         * normal GL_TEXTURE_2D. Non-linear modifiers still require explicit
         * plane modifier attributes because their auxiliary plane layout is not
         * recoverable from fd/stride/offset alone.
         */
        if (includeModifier) {
            const EGLint modLo = i == 0 ? EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
                : i == 1 ? EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT
                : i == 2 ? EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT
                         : EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT;
            const EGLint modHi = i == 0 ? EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT
                : i == 1 ? EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT
                : i == 2 ? EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT
                         : EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT;
            attrs << modLo << static_cast<EGLint>(generation.modifier & 0xffffffffull)
                  << modHi << static_cast<EGLint>((generation.modifier >> 32) & 0xffffffffull);
        }
    }
    attrs << EGL_NONE;

    EGLImageKHR image =
        createImage(eglDisplay, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.constData());
    if (image == EGL_NO_IMAGE_KHR) {
        setLastError(QStringLiteral("eglCreateImageKHR failed with EGL error 0x%1")
                         .arg(static_cast<uint>(eglGetError()), 0, 16));
        return false;
    }

    buffer.eglImage = image;
    buffer.eglDisplay = eglDisplay;

    /* EGL_EXT_image_dma_buf_import retains the dma-buf independently; the
     * protocol descriptors are no longer needed after image creation. */
    for (Plane& plane : buffer.planes)
        closeFd(plane.fd);

    return true;
}

bool VividDisplay::ensureBufferImported(Generation& generation, Buffer& buffer)
{
    if (buffer.glTexture != 0)
        return true;
    if (buffer.importAttempted)
        return false;

    buffer.importAttempted = true;

    auto* context = QOpenGLContext::currentContext();
    if (!context) {
        setLastError(QStringLiteral("OpenGL context is not current on render thread"));
        return false;
    }

    auto* gl = context->extraFunctions();
    const auto destroyImage = resolveEglProc<EglDestroyImageKhr>("eglDestroyImageKHR");
    /* Resolve the image target through the host GL context first. NVIDIA
     * exposes distinct dispatch stubs for desktop GL and GLES. */
    auto imageTarget = reinterpret_cast<GlEglImageTargetTexture2DOes>(
        context->getProcAddress("glEGLImageTargetTexture2DOES"));
    if (!imageTarget)
        imageTarget = resolveEglProc<GlEglImageTargetTexture2DOes>(
            "glEGLImageTargetTexture2DOES");
    if (!gl || !destroyImage || !imageTarget) {
        setLastError(QStringLiteral("EGL DMA-BUF texture functions are unavailable"));
        return false;
    }

    const EGLDisplay eglDisplay = buffer.eglDisplay != EGL_NO_DISPLAY
        ? buffer.eglDisplay
        : eglGetCurrentDisplay();
    if (buffer.eglImage == EGL_NO_IMAGE_KHR &&
        !importEglImage(generation, buffer, eglDisplay)) {
        return false;
    }

    const QString gpuMismatch =
        bindBuffersGpuMismatchReason(generation.renderNode, generation.vendor, generation.pciAddress);
    if (!gpuMismatch.isEmpty()) {
        qCInfo(lcWallpaperKde,
               "producer GPU differs from Plasma GPU; attempting DMA-BUF import: %s",
               qPrintable(gpuMismatch));
    }

    GLuint texture = 0;
    gl->glGenTextures(1, &texture);
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    while (gl->glGetError() != GL_NO_ERROR) {
    }
    imageTarget(GL_TEXTURE_2D, buffer.eglImage);
    const GLenum importError = gl->glGetError();
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    if (importError != GL_NO_ERROR) {
        gl->glDeleteTextures(1, &texture);
        destroyImage(eglDisplay, buffer.eglImage);
        buffer.eglImage = EGL_NO_IMAGE_KHR;
        buffer.eglDisplay = EGL_NO_DISPLAY;
        setLastError(QStringLiteral("glEGLImageTargetTexture2DOES(GL_TEXTURE_2D) failed with GL error 0x%1")
                         .arg(static_cast<uint>(importError), 0, 16));
        return false;
    }

    buffer.glTexture = texture;

    return true;
}

bool VividDisplay::ensureGenerationEglTextures(Generation& generation)
{
    /*
     * Waywallen creates the complete set of GL texture siblings on the first
     * render-thread frame after BIND. That makes every later FRAME_READY a
     * fence wait plus one blit, with no slot-dependent import spike.
     */
    for (Buffer& buffer : generation.buffers) {
        if (!ensureBufferImported(generation, buffer))
            return false;
    }
    return true;
}

bool VividDisplay::ensureVulkanBufferImported(Generation& generation, Buffer& buffer)
{
    if (buffer.hasVkImage)
        return true;
    if (buffer.importAttempted)
        return false;
    buffer.importAttempted = true;

    if (!m_vkBackendReady) {
        setLastError(QStringLiteral("Qt Vulkan backend is not bound"));
        return false;
    }
    if (generation.planesPerBuffer == 0 || generation.planesPerBuffer > WW_VK_MAX_PLANES ||
        buffer.planes.size() != static_cast<int>(generation.planesPerBuffer)) {
        setLastError(QStringLiteral("invalid Vulkan DMA-BUF plane count generation=%1 planes=%2 expected=%3")
                         .arg(generation.id)
                         .arg(buffer.planes.size())
                         .arg(generation.planesPerBuffer));
        return false;
    }

    ww_vk_dmabuf_import_t import {};
    import.fourcc = generation.fourcc;
    import.width = static_cast<uint32_t>(generation.width);
    import.height = static_cast<uint32_t>(generation.height);
    import.modifier = generation.modifier;
    import.n_planes = generation.planesPerBuffer;
    for (uint32_t i = 0; i < import.n_planes; i++) {
        const Plane& plane = buffer.planes.at(static_cast<int>(i));
        import.fds[i] = plane.fd;
        import.strides[i] = plane.stride;
        import.offsets[i] = plane.offset;
    }

    const int rc = ww_vk_import_dmabuf(&m_vkBackend, &import, &buffer.vkImage);
    if (rc != 0) {
        setLastError(QStringLiteral("Vulkan DMA-BUF import failed generation=%1 buffer=%2 fourcc=0x%3 modifier=0x%4 planes=%5 rc=%6")
                         .arg(generation.id)
                         .arg(buffer.index)
                         .arg(generation.fourcc, 8, 16, QLatin1Char('0'))
                         .arg(static_cast<qulonglong>(generation.modifier), 16, 16, QLatin1Char('0'))
                         .arg(generation.planesPerBuffer)
                         .arg(rc));
        qCWarning(lcWallpaperKde, "%s", qPrintable(m_lastError));
        return false;
    }

    /*
     * ww_vk_import_dmabuf consumes plane 0's DMA-BUF fd on success because the
     * Vulkan external-memory import owns that handle. Additional plane fds are
     * only descriptors for the same image layout after import, so close them
     * now to avoid accidentally closing a reused fd later from closeGenerationFds.
     */
    if (!buffer.planes.isEmpty())
        buffer.planes[0].fd = -1;
    for (int i = 1; i < buffer.planes.size(); i++)
        closeFd(buffer.planes[i].fd);

    buffer.hasVkImage = true;
    return true;
}

bool VividDisplay::blitEglShadow(Generation& generation, Buffer& buffer)
{
    if (buffer.glTexture == 0)
        return false;

    auto* context = QOpenGLContext::currentContext();
    auto* gl = context ? context->extraFunctions() : nullptr;
    if (!gl) {
        setLastError(QStringLiteral("OpenGL functions are unavailable for shadow-copy"));
        return false;
    }

    GLint oldReadFramebuffer = 0;
    GLint oldDrawFramebuffer = 0;
    const GLboolean oldScissor = gl->glIsEnabled(GL_SCISSOR_TEST);
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebuffer);
    gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebuffer);

    if (oldScissor)
        gl->glDisable(GL_SCISSOR_TEST);

    const auto restoreQtState = [&]() {
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                              static_cast<GLuint>(oldDrawFramebuffer));
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER,
                              static_cast<GLuint>(oldReadFramebuffer));
        if (oldScissor)
            gl->glEnable(GL_SCISSOR_TEST);
    };

    if (!ensureEglShadowResources(generation.width, generation.height)) {
        restoreQtState();
        setLastError(QStringLiteral("consumer shadow texture allocation failed"));
        return false;
    }

    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, m_eglShadowReadFramebuffer);
    gl->glFramebufferTexture2D(GL_READ_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               buffer.glTexture,
                               0);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_eglShadowFramebuffer);
    gl->glBlitFramebuffer(0,
                           0,
                           generation.width,
                           generation.height,
                           0,
                           0,
                           generation.width,
                           generation.height,
                           GL_COLOR_BUFFER_BIT,
                           GL_NEAREST);

    /* Match Waywallen's pool-safe teardown order: the read FBO never retains a
     * producer texture after the copy, so a later pool drain can delete every
     * GL sibling before destroying its EGLImage. */
    gl->glFramebufferTexture2D(GL_READ_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               0,
                               0);
    restoreQtState();

    m_eglShadowHasContent = true;
    return true;
}

void VividDisplay::renderThreadBlitEgl()
{
    PendingEglFrame pending;
    {
        QMutexLocker lock(&m_pendingEglMutex);
        if (!m_pendingEglFrame.valid)
            return;
        pending = std::move(m_pendingEglFrame);
        m_pendingEglFrame = PendingEglFrame {};
    }

    const auto dropFrame = [&](const QString& reason) {
        closeFd(pending.acquireSyncFd);
        signalPendingEglRelease(reason, pending.generation, pending.sequence);
    };

    if (m_activeBackend != BackendEgl) {
        dropFrame(QStringLiteral("egl-backend-inactive"));
        return;
    }

    Generation* generation = findGeneration(pending.generation);
    Buffer* buffer = findBuffer(generation, pending.bufferIndex);
    if (!generation || !buffer || generation->retired ||
        generation->resourcesQueued ||
        generation->presentationPath != QStringLiteral("shadow-copy")) {
        dropFrame(QStringLiteral("egl-generation-released"));
        return;
    }

    if (m_eglShadowHasContent &&
        m_eglShadowGeneration == pending.generation &&
        m_eglShadowSequence == pending.sequence) {
        dropFrame(QStringLiteral("duplicate-frame"));
        return;
    }

    const EGLDisplay eglDisplay = buffer->eglDisplay != EGL_NO_DISPLAY
        ? buffer->eglDisplay
        : qtWindowEglDisplay(window());
    if (!enqueueEglAcquireWait(eglDisplay,
                               pending.acquireSyncFd,
                               pending.acquireContext)) {
        dropFrame(QStringLiteral("egl-acquire-wait-failed"));
        return;
    }
    closeFd(pending.acquireSyncFd);

    if (!ensureGenerationEglTextures(*generation) ||
        !blitEglShadow(*generation, *buffer)) {
        const quint64 failedGeneration = generation->id;
        const QString failure = m_lastError.isEmpty()
            ? QStringLiteral("EGL DMA-BUF texture import or shadow-copy failed")
            : m_lastError;
        signalPendingEglRelease(QStringLiteral("egl-shadow-copy-failed"),
                                pending.generation,
                                pending.sequence);
        QMetaObject::invokeMethod(
            this,
            [this, failedGeneration, failure]() {
                Generation* failed = findGeneration(failedGeneration);
                if (!failed || failed->retired)
                    return;
                sendBindFailed(*failed, 2, failure);
                retireGeneration(failedGeneration);
                setStreamState(Inactive);
                update();
            },
            Qt::QueuedConnection);
        return;
    }

    m_eglShadowGeneration = pending.generation;
    m_eglShadowBuffer = pending.bufferIndex;
    m_eglShadowSequence = pending.sequence;
    {
        QMutexLocker lock(&m_pendingVulkanMutex);
        m_currentGeneration = pending.generation;
        m_currentBuffer = pending.bufferIndex;
    }
}

VkFormat VividDisplay::vkFormatForFourcc(quint32 fourcc) const
{
    return ww_fourcc_to_vk_format(fourcc);
}

bool VividDisplay::ensureVulkanShadowCopy(Generation& generation,
                                          Buffer&     buffer,
                                          PendingVulkanFrame pending)
{
    if (!pending.valid) {
        return ww_vk_blitter_shadow_has_content(&m_vkBlitter);
    }


    auto releaseWithoutCopy = [&](const QString& reason) {
        if (pending.releaseSyncobjFd < 0)
            return;
        const QString context = pending.releaseContext.isEmpty()
            ? reason
            : QStringLiteral("%1 %2").arg(pending.releaseContext, reason);
        signalReleaseSyncobj(pending.renderNode, pending.releaseSyncobjFd, context);
        closeFd(pending.releaseSyncobjFd);
    };

    if (pending.generation != generation.id || pending.bufferIndex != buffer.index) {
        setLastError(QStringLiteral("Vulkan pending frame changed generation=%1 buffer=%2 expected generation=%3 buffer=%4")
                         .arg(pending.generation)
                         .arg(pending.bufferIndex)
                         .arg(generation.id)
                         .arg(buffer.index));
        releaseWithoutCopy(QStringLiteral("pending-frame-mismatch"));
        return false;
    }

    if (!buffer.hasVkImage || buffer.vkImage.image == VK_NULL_HANDLE ||
        buffer.acquireSemaphore == VK_NULL_HANDLE) {
        setLastError(QStringLiteral("generation %1 buffer %2 has no Qt-imported Vulkan frame")
                         .arg(generation.id)
                         .arg(buffer.index));
        releaseWithoutCopy(QStringLiteral("imported-frame-missing"));
        return false;
    }
    if (!m_vkBlitterReady) {
        const int rc = ww_vk_blitter_init(&m_vkBlitter,
                                          m_vkInstance,
                                          m_vkPhysicalDevice,
                                          m_vkDevice,
                                          m_vkQueueFamilyIndex,
                                          m_vkQueue,
                                          m_vkGetInstanceProcAddr);
        if (rc != 0) {
            setLastError(QStringLiteral("ww_vk_blitter_init failed rc=%1").arg(rc));
            releaseWithoutCopy(QStringLiteral("blitter-init-failed"));
            return false;
        }
        m_vkBlitterReady = true;
    }

    const VkFormat format = vkFormatForFourcc(generation.fourcc);
    if (format == VK_FORMAT_UNDEFINED) {
        setLastError(QStringLiteral("unsupported Vulkan fourcc=0x%1")
                         .arg(generation.fourcc, 8, 16, QLatin1Char('0')));
        releaseWithoutCopy(QStringLiteral("unsupported-fourcc"));
        return false;
    }
    const int shadowRc = ww_vk_blitter_ensure_shadow(&m_vkBlitter,
                                                     static_cast<uint32_t>(generation.width),
                                                     static_cast<uint32_t>(generation.height),
                                                     format);
    if (shadowRc != 0) {
        setLastError(QStringLiteral("ww_vk_blitter_ensure_shadow failed generation=%1 size=%2x%3 format=%4 rc=%5")
                         .arg(generation.id)
                         .arg(generation.width)
                         .arg(generation.height)
                         .arg(static_cast<int>(format))
                         .arg(shadowRc));
        releaseWithoutCopy(QStringLiteral("shadow-allocation-failed"));
        return false;
    }

    /*
     * This is the Waywallen KDE Vulkan route: producer image import, acquire
     * semaphore wait, shadow copy, and producer release all happen on the Qt
     * scene graph's VkDevice/VkQueue. The socket thread never waits for GPU
     * work, and Qt samples only the local shadow image.
     */
    VulkanReleaseSignalContext signalContext {
        pending.renderNode,
        pending.releaseContext.isEmpty()
            ? QStringLiteral("qt-render-thread-shadow-copy-complete")
            : QStringLiteral("%1 qt-render-thread-shadow-copy-complete")
                  .arg(pending.releaseContext),
    };
    const int blitRc = ww_vk_blitter_blit(&m_vkBlitter,
                                          buffer.vkImage.image,
                                          static_cast<uint32_t>(generation.width),
                                          static_cast<uint32_t>(generation.height),
                                          buffer.acquireSemaphore,
                                          pending.releaseSyncobjFd,
                                          signalReleaseSyncobjFromVulkanBlit,
                                          &signalContext);
    if (blitRc != 0) {
        setLastError(QStringLiteral("ww_vk_blitter_blit failed generation=%1 buffer=%2 rc=%3")
                         .arg(generation.id)
                         .arg(buffer.index)
                         .arg(blitRc));
        return false;
    }
    return true;
}

QSGNode* VividDisplay::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    /* Pool replacement is protocol-thread work; actual GPU destruction is
     * deliberately drained here, with Qt's render context current. */
    destroyRetiredResources();

    quint64 currentGeneration = 0;
    quint32 currentBuffer = 0;
    PendingVulkanFrame pendingVulkanFrame;
    {
        QMutexLocker lock(&m_pendingVulkanMutex);
        // Consume the newest pending frame exactly once at the start of the
        // render-thread pass, like Waywallen. Do not first snapshot a separate
        // generation/buffer pair and then reject the pending frame if another
        // callback has replaced it between the two reads.
        pendingVulkanFrame = m_pendingVulkanFrame;
        m_pendingVulkanFrame = PendingVulkanFrame {};
        if (pendingVulkanFrame.valid) {
            m_currentGeneration = pendingVulkanFrame.generation;
            m_currentBuffer = pendingVulkanFrame.bufferIndex;
        }
        currentGeneration = m_currentGeneration;
        currentBuffer = m_currentBuffer;
    }
    if (m_vkBlitterReady)
        ww_vk_blitter_tick_pending_destroys(&m_vkBlitter);

    Generation* generation = findGeneration(currentGeneration);
    Buffer* buffer = findBuffer(generation, currentBuffer);
    const bool generationResourcesReleased = generation && generation->resourcesQueued;
    const bool shadowHasContent =
        (m_activeBackend == BackendEgl && m_eglShadowTexture != 0 && m_eglShadowHasContent)
#ifdef WW_HAVE_VULKAN
        || (m_activeBackend == BackendVulkan && m_vkBlitterReady &&
            ww_vk_blitter_shadow(&m_vkBlitter) != VK_NULL_HANDLE &&
            ww_vk_blitter_shadow_has_content(&m_vkBlitter))
#endif
        ;
    if (!generation || (!buffer && !generationResourcesReleased)) {
        if (pendingVulkanFrame.releaseSyncobjFd >= 0) {
            signalReleaseSyncobj(pendingVulkanFrame.renderNode,
                                 pendingVulkanFrame.releaseSyncobjFd,
                                 QStringLiteral("pending-generation-missing"));
            closeFd(pendingVulkanFrame.releaseSyncobjFd);
        }
        delete oldNode;
        return nullptr;
    }

    const bool useVulkan = m_activeBackend == BackendVulkan;
    QQuickWindow* quickWindow = window();
    if (!quickWindow) {
        delete oldNode;
        if (useVulkan) {
            if (pendingVulkanFrame.releaseSyncobjFd >= 0) {
                signalReleaseSyncobj(pendingVulkanFrame.renderNode,
                                     pendingVulkanFrame.releaseSyncobjFd,
                                     QStringLiteral("window-missing"));
                closeFd(pendingVulkanFrame.releaseSyncobjFd);
            }
        }
        return nullptr;
    }

    if (generationResourcesReleased) {
        /* The old imported pool has already been detached and queued.  The
         * persistent shadow is the only presentation resource left, so keep
         * the last frame visible until the replacement generation blits. */
        if (!shadowHasContent) {
            delete oldNode;
            return nullptr;
        }
    } else if (useVulkan) {
        const VkFormat nextFormat = vkFormatForFourcc(generation->fourcc);
        /*
         * Qt owns the QSGVulkanTexture wrapper and its VkImageView. When the
         * shadow image size/format changes, drop the node before asking the
         * blitter to replace the shadow so Qt can release the old view through
         * its normal scene-graph lifetime path.
         */
        if (oldNode && m_vkBlitterReady &&
            (m_vkBlitter.shadow_image == VK_NULL_HANDLE ||
             m_vkBlitter.shadow_w != static_cast<uint32_t>(generation->width) ||
             m_vkBlitter.shadow_h != static_cast<uint32_t>(generation->height) ||
             m_vkBlitter.shadow_fmt != nextFormat)) {
            delete oldNode;
            oldNode = nullptr;
        }
        const bool shadowCopyOk = ensureVulkanShadowCopy(*generation,
                                                         *buffer,
                                                         pendingVulkanFrame);
        if (!shadowCopyOk || !ww_vk_blitter_shadow_has_content(&m_vkBlitter)) {
            // SET_CONFIG can legitimately dirty the item before the first
            // FRAME_READY arrives. Waywallen simply has no texture yet and
            // returns nullptr; that is not a bind failure and must not retire
            // the generation before its first frame is accepted.
            if (!pendingVulkanFrame.valid) {
                delete oldNode;
                return nullptr;
            }
            sendBindFailed(*generation,
                           2,
                           m_lastError.isEmpty()
                               ? QStringLiteral("Vulkan DMA-BUF import or shadow-copy failed")
                               : m_lastError);
            retireGeneration(generation->id);
            if (m_currentGeneration == generation->id) {
                m_currentGeneration = 0;
                m_currentBuffer = 0;
                setStreamState(Inactive);
            }
            destroyRetiredResources();
            delete oldNode;
            return nullptr;
        }
    } else {
        /*
         * EGL import and blit are arrival-driven render jobs, matching
         * Waywallen. updatePaintNode only exposes the persistent consumer
         * shadow; unrelated QSG repaints never recopy a producer buffer.
         */
        if (m_eglShadowTexture == 0 || !m_eglShadowHasContent) {
            delete oldNode;
            return nullptr;
        }
    }

    const int presentationWidth =
#ifdef WW_HAVE_VULKAN
        useVulkan ? static_cast<int>(m_vkBlitter.shadow_w) : m_eglShadowWidth;
#else
        m_eglShadowWidth;
#endif
    const int presentationHeight =
#ifdef WW_HAVE_VULKAN
        useVulkan ? static_cast<int>(m_vkBlitter.shadow_h) : m_eglShadowHeight;
#else
        m_eglShadowHeight;
#endif

    QSGTransformNode* transformNode = nullptr;
    QSGSimpleTextureNode* textureNode = nullptr;
    if (oldNode && oldNode->type() == QSGNode::TransformNodeType) {
        transformNode = static_cast<QSGTransformNode*>(oldNode);
        textureNode = static_cast<QSGSimpleTextureNode*>(transformNode->firstChild());
    } else {
        delete oldNode;
        transformNode = new QSGTransformNode();
        textureNode = new QSGSimpleTextureNode();
        textureNode->setFiltering(QSGTexture::Linear);
        textureNode->setOwnsTexture(true);
        transformNode->appendChildNode(textureNode);
    }

    QSGTexture* texture = nullptr;
    if (useVulkan) {
        texture = QNativeInterface::QSGVulkanTexture::fromNative(
            ww_vk_blitter_shadow(&m_vkBlitter),
            ww_vk_blitter_shadow_layout(&m_vkBlitter),
            quickWindow,
            QSize(presentationWidth, presentationHeight),
            QQuickWindow::TextureHasAlphaChannel);
    } else {
        texture = QNativeInterface::QSGOpenGLTexture::fromNative(
            m_eglShadowTexture,
            quickWindow,
            QSize(presentationWidth, presentationHeight),
            QQuickWindow::TextureHasAlphaChannel);
    }
    if (!texture) {
        delete transformNode;
        if (useVulkan)
            signalPendingVulkanFrame(QStringLiteral("texture-node-failed"));
        return nullptr;
    }
    textureNode->setTexture(texture);

    const QRectF bounds = boundingRect();
    const QRectF source = m_sourceRect.width() > 0 && m_sourceRect.height() > 0
        ? m_sourceRect
        : QRectF(0, 0, presentationWidth, presentationHeight);
    textureNode->setSourceRect(source);

    if (m_destRect.width() > 0 && m_destRect.height() > 0 &&
        m_outputGeometry.physicalWidth > 0 && m_outputGeometry.physicalHeight > 0) {
        const qreal sx = bounds.width() / qreal(m_outputGeometry.physicalWidth);
        const qreal sy = bounds.height() / qreal(m_outputGeometry.physicalHeight);
        textureNode->setRect(QRectF(m_destRect.x() * sx,
                                    m_destRect.y() * sy,
                                    m_destRect.width() * sx,
                                    m_destRect.height() * sy));
    } else {
        textureNode->setRect(bounds);
    }

    QMatrix4x4 matrix;
    if (m_transform != 0) {
        const qreal width = bounds.width();
        const qreal height = bounds.height();
        const bool swapsDimensions =
            (m_transform == 1 || m_transform == 3 || m_transform == 5 || m_transform == 7);
        const qreal preWidth = swapsDimensions ? height : width;
        const qreal preHeight = swapsDimensions ? width : height;
        matrix.translate(static_cast<float>(width / 2.0), static_cast<float>(height / 2.0));
        if (m_transform >= 4) {
            matrix.scale(-1.0f, 1.0f);
            matrix.rotate(static_cast<float>((m_transform - 4) * 90), 0.0f, 0.0f, 1.0f);
        } else {
            matrix.rotate(static_cast<float>(m_transform * 90), 0.0f, 0.0f, 1.0f);
        }
        matrix.translate(static_cast<float>(-preWidth / 2.0),
                         static_cast<float>(-preHeight / 2.0));
    }
    if (transformNode->matrix() != matrix) {
        transformNode->setMatrix(matrix);
        transformNode->markDirty(QSGNode::DirtyMatrix);
    }

    textureNode->markDirty(QSGNode::DirtyGeometry);
    return transformNode;
}

void VividDisplay::setConnState(ConnState state)
{
    if (m_connState == state)
        return;
    m_connState = state;
    emit connStateChanged();
}

void VividDisplay::setStreamState(StreamState state)
{
    if (m_streamState == state)
        return;
    m_streamState = state;
    emit streamStateChanged();
}

void VividDisplay::setLastError(const QString& error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    if (!error.isEmpty())
        qCWarning(lcWallpaperKde, "%s", qPrintable(error));
    emit lastErrorChanged();
}

void VividDisplay::geometryPropertyChanged()
{
    emit displayGeometryChanged();
    if (isComponentComplete())
        refreshOutputSnapshot();
    if (m_fd >= 0 || m_connecting)
        requestReconnect();
}

bool VividDisplay::eventFilter(QObject* object, QEvent* event)
{
    if (!m_mouseForwardEnabled || object != window() || m_outputId == 0 ||
        m_connState != Connected)
        return false;

    const QRectF bounds = boundingRect();
    if (bounds.width() <= 0 || bounds.height() <= 0)
        return false;

    auto toSurface = [&](const QPointF& scenePosition, float& x, float& y) {
        const QPointF local = mapFromScene(scenePosition);
        if (!bounds.contains(local))
            return false;
        x = static_cast<float>(local.x() * qreal(m_outputGeometry.physicalWidth) / bounds.width());
        y = static_cast<float>(local.y() * qreal(m_outputGeometry.physicalHeight) / bounds.height());
        return true;
    };

    switch (event->type()) {
    case QEvent::MouseMove: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        float x = 0.0f;
        float y = 0.0f;
        if (toSurface(mouse->scenePosition(), x, y))
            sendPointerMotion(x, y, mouse->timestamp() > 0 ? mouse->timestamp() * 1000ull : monotonicUsec());
        break;
    }
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        auto* mouse = static_cast<QMouseEvent*>(event);
        float x = 0.0f;
        float y = 0.0f;
        const quint32 button = qtButtonToProtocolButton(mouse->button());
        if (button != 0 && toSurface(mouse->scenePosition(), x, y)) {
            sendPointerButton(x,
                              y,
                              button,
                              event->type() == QEvent::MouseButtonPress,
                              mouse->timestamp() > 0 ? mouse->timestamp() * 1000ull : monotonicUsec());
        }
        break;
    }
    case QEvent::Wheel: {
        auto* wheel = static_cast<QWheelEvent*>(event);
        float x = 0.0f;
        float y = 0.0f;
        if (!toSurface(wheel->scenePosition(), x, y))
            break;
        const QPoint pixelDelta = wheel->pixelDelta();
        const QPoint angleDelta = wheel->angleDelta();
        const double dx = !pixelDelta.isNull() ? pixelDelta.x() : angleDelta.x() / 120.0;
        const double dy = !pixelDelta.isNull() ? pixelDelta.y() : angleDelta.y() / 120.0;
        sendPointerAxis(x, y, dx, dy, wheel->timestamp() > 0 ? wheel->timestamp() * 1000ull : monotonicUsec());
        break;
    }
    default:
        break;
    }

    return false;
}

void VividDisplay::sendPointerMotion(float x, float y, quint64 timeUsec)
{
    QByteArray body;
    body.resize(Proto::VIVID_DISPLAY_POINTER_MOTION_BODY_BYTES);
    vivid_display_pointer_motion_body_write(reinterpret_cast<uint8_t*>(body.data()),
                                             m_outputId,
                                             x,
                                             y,
                                             timeUsec);
    queueFrame(Proto::VIVID_DISPLAY_REQ_POINTER_MOTION, body);
}

void VividDisplay::sendPointerButton(float x,
                                      float y,
                                      quint32 button,
                                      bool pressed,
                                      quint64 timeUsec)
{
    QByteArray body;
    body.resize(Proto::VIVID_DISPLAY_POINTER_BUTTON_BODY_BYTES);
    vivid_display_pointer_button_body_write(reinterpret_cast<uint8_t*>(body.data()),
                                             m_outputId,
                                             x,
                                             y,
                                             button,
                                             pressed ? Proto::VIVID_DISPLAY_BUTTON_PRESSED
                                                     : Proto::VIVID_DISPLAY_BUTTON_RELEASED,
                                             timeUsec);
    queueFrame(Proto::VIVID_DISPLAY_REQ_POINTER_BUTTON, body);
}

void VividDisplay::sendPointerAxis(float x, float y, double dx, double dy, quint64 timeUsec)
{
    QByteArray body;
    body.resize(Proto::VIVID_DISPLAY_POINTER_AXIS_BODY_BYTES);
    vivid_display_pointer_axis_body_write(reinterpret_cast<uint8_t*>(body.data()),
                                           m_outputId,
                                           x,
                                           y,
                                           dx,
                                           dy,
                                           Proto::VIVID_DISPLAY_AXIS_WHEEL,
                                           timeUsec);
    queueFrame(Proto::VIVID_DISPLAY_REQ_POINTER_AXIS, body);
}
