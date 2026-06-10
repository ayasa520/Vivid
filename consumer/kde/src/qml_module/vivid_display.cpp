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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTransformNode>
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
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <optional>

Q_LOGGING_CATEGORY(lcWallpaperKde, "wallpaper.display.kde")

namespace
{
constexpr quint64 DrmFormatModInvalid = (1ull << 56) - 1ull;
constexpr quint64 DrmFormatModLinear = 0;

quint64 monotonicUsec()
{
    using namespace std::chrono;
    return static_cast<quint64>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

quint32 readU32LE(const QByteArray& bytes, qsizetype offset)
{
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData());
    return static_cast<quint32>(data[offset]) |
        (static_cast<quint32>(data[offset + 1]) << 8) |
        (static_cast<quint32>(data[offset + 2]) << 16) |
        (static_cast<quint32>(data[offset + 3]) << 24);
}

quint64 readU64LE(const QByteArray& bytes, qsizetype offset)
{
    return static_cast<quint64>(readU32LE(bytes, offset)) |
        (static_cast<quint64>(readU32LE(bytes, offset + 4)) << 32);
}

void writeU16LE(QByteArray& bytes, qsizetype offset, quint16 value)
{
    bytes[offset] = static_cast<char>(value & 0xffu);
    bytes[offset + 1] = static_cast<char>((value >> 8) & 0xffu);
}

void writeU32LE(QByteArray& bytes, qsizetype offset, quint32 value)
{
    bytes[offset] = static_cast<char>(value & 0xffu);
    bytes[offset + 1] = static_cast<char>((value >> 8) & 0xffu);
    bytes[offset + 2] = static_cast<char>((value >> 16) & 0xffu);
    bytes[offset + 3] = static_cast<char>((value >> 24) & 0xffu);
}

void writeU64LE(QByteArray& bytes, qsizetype offset, quint64 value)
{
    writeU32LE(bytes, offset, static_cast<quint32>(value & 0xffffffffull));
    writeU32LE(bytes, offset + 4, static_cast<quint32>((value >> 32) & 0xffffffffull));
}

void writeF64LE(QByteArray& bytes, qsizetype offset, double value)
{
    static_assert(sizeof(double) == sizeof(quint64));
    quint64 raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    writeU64LE(bytes, offset, raw);
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

bool waitSyncFile(int fd, int timeoutMs, const QString& context)
{
    if (fd < 0) {
        qCWarning(lcWallpaperKde, "invalid acquire sync_file fd context=%s", qPrintable(context));
        return false;
    }

    pollfd pfd {
        .fd = fd,
        .events = POLLIN,
        .revents = 0,
    };
    for (;;) {
        const int result = ::poll(&pfd, 1, timeoutMs);
        if (result > 0) {
            if ((pfd.revents & POLLIN) != 0)
                return true;
            qCWarning(lcWallpaperKde,
                      "acquire sync_file fd=%d context=%s returned unexpected poll events=0x%x",
                      fd,
                      qPrintable(context),
                      pfd.revents);
            return false;
        }
        if (result == 0) {
            qCWarning(lcWallpaperKde,
                      "timed out waiting for acquire sync_file fd=%d context=%s timeout=%dms",
                      fd,
                      qPrintable(context),
                      timeoutMs);
            return false;
        }
        if (errno == EINTR)
            continue;
        qCWarning(lcWallpaperKde,
                  "poll(acquire sync_file fd=%d context=%s) failed: %s",
                  fd,
                  qPrintable(context),
                  strerror(errno));
        return false;
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

QString jsonStringMember(const QJsonObject& object,
                         const QString& primary,
                         const QString& fallback)
{
    return jsonStringMember(object, { primary, fallback });
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

QJsonObject buildDmaBufCaps(EGLDisplay eglDisplay)
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

    QJsonArray memoryHints { QStringLiteral("host-visible") };
    if (!implicitLinearFourccs.isEmpty())
        memoryHints.append(QStringLiteral("implicit-linear"));
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
        { QStringLiteral("backend"), QStringLiteral("kde-qt6-egl-gl-texture-2d") },
        { QStringLiteral("probe"), probeMode },
        { QStringLiteral("relayModes"),
          QJsonArray {
              QStringLiteral("direct-import-v1"),
              QStringLiteral("shadow-copy-v1"),
          } },
        { QStringLiteral("renderNode"), identity.renderNode },
        { QStringLiteral("deviceUuid"), identity.deviceUuid },
        { QStringLiteral("driverUuid"), identity.driverUuid },
        { QStringLiteral("vendor"), identity.vendor },
        { QStringLiteral("pciAddress"), identity.pciAddress },
        { QStringLiteral("fourccs"), fourccs },
        { QStringLiteral("modifiers"), modifiers },
        { QStringLiteral("implicitLinearFourccs"), implicitLinearFourccs },
        { QStringLiteral("memoryHints"), memoryHints },
        { QStringLiteral("syncCaps"),
          QJsonArray {
              QStringLiteral("implicit"),
              QStringLiteral("explicit-sync-fd"),
              QStringLiteral("drm-syncobj-release"),
          } },
        { QStringLiteral("colorCaps"),
          QJsonArray {
              QStringLiteral("srgb"),
              QStringLiteral("limited-range"),
              QStringLiteral("premultiplied-alpha"),
          } },
        { QStringLiteral("extentMax"),
          QJsonObject {
              { QStringLiteral("width"), 0 },
              { QStringLiteral("height"), 0 },
          } },
        { QStringLiteral("textureTarget"), QStringLiteral("GL_TEXTURE_2D") },
        { QStringLiteral("skipsExternalOnlyModifiers"), true },
        { QStringLiteral("diagnostics"), probeDiagnostics },
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
    connect(this, &QQuickItem::windowChanged, this, &VividDisplay::onWindowChanged);
}

VividDisplay::~VividDisplay()
{
    closeTransport(false);
    clearGenerations(QOpenGLContext::currentContext() != nullptr);
}

void VividDisplay::componentComplete()
{
    QQuickItem::componentComplete();
    installOrRemoveEventFilter();
    armSceneGraphReadyConnection();
    scheduleReconnect(0);
}

void VividDisplay::setSocketPath(const QString& path)
{
    if (m_socketPath == path)
        return;
    m_socketPath = path;
    emit socketPathChanged();
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

void VividDisplay::requestReconnect()
{
    if (!isComponentComplete())
        return;
    closeTransport(true);
    scheduleReconnect(100);
}

void VividDisplay::onWindowChanged(QQuickWindow*)
{
    installOrRemoveEventFilter();
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
    geometry.scale = 1.0;
    geometry.physicalWidth = std::max(1, qRound(qreal(m_logicalWidth) * geometry.scale));
    geometry.physicalHeight = std::max(1, qRound(qreal(m_logicalHeight) * geometry.scale));

    /*
     * QtQuick's Screen.devicePixelRatio is the Wayland backing-buffer scale.
     * On Plasma fractional scaling that value is intentionally rounded up
     * (160% -> buffer scale 2) and the compositor applies the fractional
     * viewport transform. The producer needs the real output scale for its
     * render target contract, so KScreen's output scale is the authoritative
     * input for the producer registration.
     */
    KScreen::GetConfigOperation operation(KScreen::ConfigOperation::NoEDID);
    if (!operation.exec() || operation.hasError())
        return geometry;

    const KScreen::ConfigPtr config = operation.config();
    if (!config)
        return geometry;

    KScreen::OutputPtr bestOutput;
    int bestScore = -1;
    const QPoint displayPos(m_displayX, m_displayY);
    const QSize logicalSize(m_logicalWidth, m_logicalHeight);
    const QPoint displayCenter(m_displayX + m_logicalWidth / 2, m_displayY + m_logicalHeight / 2);

    const KScreen::OutputList outputs = config->outputs();
    for (auto it = outputs.cbegin(); it != outputs.cend(); ++it) {
        const KScreen::OutputPtr output = it.value();
        if (!output || !output->isConnected() || !output->isEnabled())
            continue;

        const QSize outputLogicalSize = config->logicalSizeForOutputInt(*output);
        const QRect outputLogicalRect(output->pos(), outputLogicalSize);
        int score = 0;
        if (!m_screenName.isEmpty() && output->name() == m_screenName)
            score += 1000;
        if (!m_displayName.isEmpty() && output->name() == m_displayName)
            score += 200;
        if ((output->pos() - displayPos).manhattanLength() <= 2)
            score += 200;
        if (std::abs(outputLogicalSize.width() - logicalSize.width()) <= 2 &&
            std::abs(outputLogicalSize.height() - logicalSize.height()) <= 2)
            score += 200;
        if (outputLogicalRect.contains(displayCenter))
            score += 100;

        if (score > bestScore) {
            bestScore = score;
            bestOutput = output;
        }
    }

    if (!bestOutput || bestScore <= 0)
        return geometry;

    const qreal kscreenScale = std::max<qreal>(1.0, bestOutput->scale());
    geometry.scale = kscreenScale;
    geometry.physicalWidth = std::max(1, qRound(qreal(m_logicalWidth) * geometry.scale));
    geometry.physicalHeight = std::max(1, qRound(qreal(m_logicalHeight) * geometry.scale));
    return geometry;
}

bool VividDisplay::sceneGraphReadyForProtocol() const
{
    QQuickWindow* currentWindow = window();
    return currentWindow && currentWindow->isSceneGraphInitialized();
}

void VividDisplay::armSceneGraphReadyConnection()
{
    QQuickWindow* currentWindow = window();
    if (!currentWindow || currentWindow->isSceneGraphInitialized())
        return;

    connect(currentWindow,
            &QQuickWindow::sceneGraphInitialized,
            this,
            &VividDisplay::onSceneGraphInitialized,
            Qt::UniqueConnection);
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

void VividDisplay::onSceneGraphInitialized()
{
    scheduleReconnect(0);
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

    if (!keepLastFrame) {
        m_currentGeneration = 0;
        m_currentBuffer = 0;
        clearGenerations(QOpenGLContext::currentContext() != nullptr);
        update();
    }

    setStreamState(Inactive);
    setConnState(Disconnected);
}

void VividDisplay::queueFrame(quint16 opcode, const QByteArray& body)
{
    if (body.size() > static_cast<int>(VIVID_DISPLAY_CODEC_MAX_BODY_BYTES)) {
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
    queueJsonFrame(VIVID_DISPLAY_REQ_HELLO,
                   QJsonObject {
                       { QStringLiteral("protocol"), QStringLiteral(VIVID_DISPLAY_PROTOCOL_NAME) },
                       { QStringLiteral("version"), static_cast<int>(VIVID_DISPLAY_PROTOCOL_VERSION) },
                       { QStringLiteral("clientName"), QStringLiteral("kde-plasma-wallpaper") },
                       { QStringLiteral("role"), QStringLiteral("consumer") },
                       { QStringLiteral("features"),
                         QJsonArray {
                             QStringLiteral("dmabuf-egl-image-v1"),
                             QStringLiteral("dmabuf-caps-v3"),
                             QStringLiteral("explicit-sync-fd-v1"),
                             QStringLiteral("dmabuf-bind-failed-v1"),
                             QStringLiteral("dmabuf-unbind-done-v1"),
                             QStringLiteral("dmabuf-shadow-copy-v1"),
                             QStringLiteral("pointer-events-v1"),
                             QStringLiteral("window-state-v1"),
                             QStringLiteral("media-state-v1"),
                             QStringLiteral("audio-samples-v1"),
                         } },
                   });
}

void VividDisplay::sendConsumerCaps()
{
    const EGLDisplay eglDisplay = qtWindowEglDisplay(window());
    const QJsonObject dmabufCaps = buildDmaBufCaps(eglDisplay);
    qCInfo(lcWallpaperKde,
           "sending consumer caps backend=%s probe=%s render-node=%s fourccs=%lld modifiers=%lld implicit-linear=%lld",
           qPrintable(dmabufCaps.value(QStringLiteral("backend")).toString(QStringLiteral("(unknown)"))),
           qPrintable(dmabufCaps.value(QStringLiteral("probe")).toString(QStringLiteral("(unknown)"))),
           qPrintable(dmabufCaps.value(QStringLiteral("renderNode")).toString(QStringLiteral("(unknown)"))),
           static_cast<long long>(dmabufCaps.value(QStringLiteral("fourccs")).toArray().size()),
           static_cast<long long>(dmabufCaps.value(QStringLiteral("modifiers")).toArray().size()),
           static_cast<long long>(dmabufCaps.value(QStringLiteral("implicitLinearFourccs")).toArray().size()));

    queueJsonFrame(VIVID_DISPLAY_REQ_CONSUMER_CAPS,
                   QJsonObject {
                       { QStringLiteral("bufferImports"),
                         QJsonArray {
                             QJsonObject {
                                 { QStringLiteral("memoryType"), QStringLiteral("dmabuf") },
                                 { QStringLiteral("renderer"), QStringLiteral("qt6-egl-image") },
                                 { QStringLiteral("fourcc"),
                                   QJsonArray {
                                       QStringLiteral("XRGB8888"),
                                       QStringLiteral("ARGB8888"),
                                       QStringLiteral("XBGR8888"),
                                       QStringLiteral("ABGR8888"),
                                   } },
                                 { QStringLiteral("modifiers"), true },
                                 { QStringLiteral("relayModes"),
                                   QJsonArray {
                                       QStringLiteral("direct-import-v1"),
                                       QStringLiteral("shadow-copy-v1"),
                                   } },
                             },
                         } },
                       { QStringLiteral("explicitSync"), true },
                       { QStringLiteral("dmabufCaps"), dmabufCaps },
                       { QStringLiteral("pointerEvents"), true },
                       { QStringLiteral("mediaState"), true },
                       { QStringLiteral("audioSamples"),
                         QJsonObject {
                             { QStringLiteral("format"), QStringLiteral("spectrum-f32-json") },
                             { QStringLiteral("bands"), 128 },
                             { QStringLiteral("sampleRate"), 44100 },
                         } },
                   });
}

void VividDisplay::sendBindFailed(const Generation& generation,
                                  quint32           reason,
                                  const QString&    message)
{
    if (generation.fourcc == 0)
        return;

    qCWarning(lcWallpaperKde,
              "BIND_FAILED output=%u generation=%llu fourcc=0x%08x modifier=0x%016llx reason=%u: %s",
              generation.outputId,
              static_cast<unsigned long long>(generation.id),
              generation.fourcc,
              static_cast<unsigned long long>(generation.modifier),
              reason,
              qPrintable(message));
    queueJsonFrame(VIVID_DISPLAY_REQ_BIND_FAILED,
                   QJsonObject {
                       { QStringLiteral("outputId"), static_cast<qint64>(generation.outputId) },
                       { QStringLiteral("generation"), QString::number(generation.id) },
                       { QStringLiteral("fourcc"), static_cast<qint64>(generation.fourcc) },
                       { QStringLiteral("modifier"), QString::number(generation.modifier) },
                       { QStringLiteral("reason"), static_cast<int>(reason) },
                       { QStringLiteral("message"), message },
                   });
}

bool VividDisplay::sendMediaState(const QJsonObject& payload)
{
    if (m_fd < 0 || m_connState != Connected)
        return false;

    queueJsonFrame(VIVID_DISPLAY_REQ_MEDIA_STATE, payload);
    return true;
}

bool VividDisplay::sendAudioSamples(const QVector<double>& samples, quint64 timeUsec)
{
    if (m_fd < 0 || m_connState != Connected)
        return false;

    QJsonArray sampleArray;
    const int sampleCount = std::min<int>(static_cast<int>(samples.size()), 512);
    for (int i = 0; i < sampleCount; i++)
        sampleArray.append(std::clamp(samples.at(i), 0.0, 1.0));

    queueJsonFrame(VIVID_DISPLAY_REQ_AUDIO_SAMPLES,
                   QJsonObject {
                       { QStringLiteral("samples"), sampleArray },
                       { QStringLiteral("timeUsec"), QString::number(timeUsec) },
                   });
    return true;
}

void VividDisplay::sendRegisterOutput()
{
    m_outputGeometry = resolveOutputGeometry();

    queueJsonFrame(VIVID_DISPLAY_REQ_REGISTER_OUTPUT,
                   QJsonObject {
                       { QStringLiteral("consumerOutputId"), static_cast<int>(m_consumerOutputId) },
                       { QStringLiteral("monitorIndex"), static_cast<int>(m_monitorIndex) },
                       { QStringLiteral("x"), m_displayX },
                       { QStringLiteral("y"), m_displayY },
                       { QStringLiteral("width"), m_logicalWidth },
                       { QStringLiteral("height"), m_logicalHeight },
                       { QStringLiteral("physicalWidth"), m_outputGeometry.physicalWidth },
                       { QStringLiteral("physicalHeight"), m_outputGeometry.physicalHeight },
                       { QStringLiteral("scale"), m_outputGeometry.scale },
                       { QStringLiteral("transform"), QStringLiteral("normal") },
                       { QStringLiteral("refreshRateMhz"), static_cast<int>(m_refreshRateMhz) },
                       { QStringLiteral("desktop"), QStringLiteral("kde-plasma-wallpaper") },
                       { QStringLiteral("instanceId"), m_instanceId },
                       { QStringLiteral("displayName"), m_displayName },
                   });
}

void VividDisplay::sendWindowState()
{
    if (m_fd < 0 || m_connecting)
        return;

    const bool focused = (m_windowStateFlags & 2u) != 0;
    const bool maximizedOrFullscreen = (m_windowStateFlags & (4u | 8u)) != 0;

    /*
     * Plasma wallpaper instances are per-screen. Wallpaper's current policy fact
     * schema is session-wide, so this consumer reports the local screen state
     * using the same keys. The producer remains the policy owner, and this can
     * be widened later by adding a tiny session aggregator without changing the
     * render/display module API.
     */
    queueJsonFrame(VIVID_DISPLAY_REQ_WINDOW_STATE,
                   QJsonObject {
                       { QStringLiteral("schema"), QStringLiteral("display-window-state-v1") },
                       { QStringLiteral("source"), QStringLiteral("kde-plasma-wallpaper") },
                       { QStringLiteral("consumerOutputId"), static_cast<int>(m_consumerOutputId) },
                       { QStringLiteral("facts"),
                         QJsonObject {
                             { QStringLiteral("windowFocused"), focused },
                             { QStringLiteral("maximizedOrFullscreenOnAnyMonitor"), maximizedOrFullscreen },
                             { QStringLiteral("maximizedOrFullscreenOnAllMonitors"), maximizedOrFullscreen },
                         } },
                   });
}

void VividDisplay::onSocketReadable()
{
    if (m_fd < 0)
        return;

    for (;;) {
        const int result = vivid_display_recv_frame_nonblocking(m_fd, &m_recvState);
        if (result == VIVID_DISPLAY_CODEC_FRAME_NEED_IO)
            return;
        if (result == VIVID_DISPLAY_CODEC_FRAME_DONE) {
            QByteArray body(reinterpret_cast<const char*>(m_recvState.body),
                            static_cast<qsizetype>(m_recvState.body_len));
            handleIncomingFrame(m_recvState.opcode, body, &m_recvState);
            vivid_display_recv_state_clear(&m_recvState);
            continue;
        }

        handleProtocolError(-result, QStringLiteral("socket read failed: %1").arg(formatErrno(-result)));
        return;
    }
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
    case VIVID_DISPLAY_EVT_WELCOME:
        setConnState(Connected);
        return;
    case VIVID_DISPLAY_EVT_OUTPUT_ACCEPTED: {
        QString error;
        const QJsonObject object = parseJsonObject(body, &error);
        if (object.isEmpty() && !error.isEmpty()) {
            setLastError(QStringLiteral("invalid OUTPUT_ACCEPTED JSON: %1").arg(error));
            return;
        }
        handleOutputAccepted(object);
        return;
    }
    case VIVID_DISPLAY_EVT_BIND_BUFFERS:
        handleBindBuffers(body, state);
        return;
    case VIVID_DISPLAY_EVT_SET_CONFIG: {
        QString error;
        const QJsonObject object = parseJsonObject(body, &error);
        if (object.isEmpty() && !error.isEmpty()) {
            setLastError(QStringLiteral("invalid SET_CONFIG JSON: %1").arg(error));
            return;
        }
        handleSetConfig(object);
        return;
    }
    case VIVID_DISPLAY_EVT_FRAME_READY:
        handleFrameReady(body, state);
        return;
    case VIVID_DISPLAY_EVT_UNBIND:
        handleUnbind(body);
        return;
    case VIVID_DISPLAY_EVT_ERROR:
        setLastError(QString::fromUtf8(body));
        return;
    default:
        qCDebug(lcWallpaperKde, "ignored event opcode=%u", opcode);
        return;
    }
}

void VividDisplay::handleOutputAccepted(const QJsonObject& object)
{
    const quint32 consumerId = jsonUInt32(object.value(QStringLiteral("consumerOutputId")));
    const quint32 outputId = jsonUInt32(object.value(QStringLiteral("outputId")));
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
    generation.outputId = jsonUInt32(object.value(QStringLiteral("outputId")));
    generation.id = jsonUInt64(object.value(QStringLiteral("generation")));
    generation.width = static_cast<int>(jsonUInt32(object.value(QStringLiteral("width"))));
    generation.height = static_cast<int>(jsonUInt32(object.value(QStringLiteral("height"))));
    generation.fourcc = jsonUInt32(object.value(QStringLiteral("fourcc")));
    generation.modifier = jsonUInt64(object.value(QStringLiteral("modifier")), DrmFormatModInvalid);
    generation.planesPerBuffer = jsonUInt32(object.value(QStringLiteral("planesPerBuffer")));
    if (generation.planesPerBuffer == 0)
        generation.planesPerBuffer = jsonUInt32(object.value(QStringLiteral("planes_per_buffer")));
    generation.renderNode =
        jsonStringMember(object,
                         { QStringLiteral("render-node"),
                           QStringLiteral("producerRenderNode"),
                           QStringLiteral("renderNode") });
    generation.vendor = jsonStringMember(object, QStringLiteral("vendor"));
    generation.pciAddress = jsonStringMember(object,
                                             QStringLiteral("pci-address"),
                                             QStringLiteral("pciAddress"));
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

    const bool reusedGenerationId = findGeneration(generation.id) != nullptr;
    retireGeneration(generation.id);
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
    const quint32 outputId = jsonUInt32(object.value(QStringLiteral("outputId")), m_outputId);
    const quint64 generationId = jsonUInt64(object.value(QStringLiteral("generation")));
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

    const QJsonObject source = object.value(QStringLiteral("source")).toObject();
    const QJsonObject destination = object.value(QStringLiteral("destination")).toObject();
    const QJsonArray clear = object.value(QStringLiteral("clearColor")).toArray();

    m_sourceRect = QRectF(source.value(QStringLiteral("x")).toDouble(0.0),
                          source.value(QStringLiteral("y")).toDouble(0.0),
                          source.value(QStringLiteral("width")).toDouble(source.value(QStringLiteral("w")).toDouble(m_outputGeometry.physicalWidth)),
                          source.value(QStringLiteral("height")).toDouble(source.value(QStringLiteral("h")).toDouble(m_outputGeometry.physicalHeight)));
    m_destRect = QRectF(destination.value(QStringLiteral("x")).toDouble(0.0),
                        destination.value(QStringLiteral("y")).toDouble(0.0),
                        destination.value(QStringLiteral("width")).toDouble(destination.value(QStringLiteral("w")).toDouble(m_outputGeometry.physicalWidth)),
                        destination.value(QStringLiteral("height")).toDouble(destination.value(QStringLiteral("h")).toDouble(m_outputGeometry.physicalHeight)));
    m_transform = transformCode(object.value(QStringLiteral("transform")));

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
        jsonUInt64(object.value(QStringLiteral("configGeneration")), generationState->configGeneration);
    update();
}

void VividDisplay::handleFrameReady(const QByteArray& body, VividDisplayRecvState* state)
{
    if (body.size() != static_cast<int>(VIVID_DISPLAY_FRAME_READY_BODY_BYTES)) {
        setLastError(QStringLiteral("invalid FRAME_READY body length %1").arg(body.size()));
        return;
    }

    if (!state || state->n_fds != VIVID_DISPLAY_FRAME_READY_FD_COUNT) {
        setLastError(QStringLiteral("FRAME_READY invalid explicit sync fd count=%1 expected=%2")
                         .arg(state ? static_cast<qulonglong>(state->n_fds) : 0)
                         .arg(static_cast<qulonglong>(VIVID_DISPLAY_FRAME_READY_FD_COUNT)));
        closeRecvStateFds(state);
        return;
    }

    int acquireFd = vivid_display_recv_state_steal_fd(state, 0);
    int releaseFd = vivid_display_recv_state_steal_fd(state, 1);
    auto closeFrameFds = [&]() {
        closeFd(acquireFd);
        closeFd(releaseFd);
    };
    flushPendingReleaseSyncobj(QStringLiteral("frame-ready"));
    if (acquireFd < 0 || releaseFd < 0) {
        closeFrameFds();
        setLastError(QStringLiteral("FRAME_READY explicit sync fd extraction failed"));
        return;
    }

    const quint32 outputId = readU32LE(body, 0);
    const quint64 generation = readU64LE(body, 4);
    const quint32 bufferIndex = readU32LE(body, 12);
    if (m_outputId != 0 && outputId != m_outputId) {
        qCWarning(lcWallpaperKde, "FRAME_READY for unknown output=%u current=%u", outputId, m_outputId);
        closeFrameFds();
        return;
    }

    Generation* generationState = findGeneration(generation);
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
        QStringLiteral("output=%1 generation=%2 buffer=%3")
            .arg(outputId)
            .arg(generation)
            .arg(bufferIndex);
    if (!waitSyncFile(acquireFd, 1000, syncContext)) {
        signalReleaseSyncobj(generationState->renderNode, releaseFd, QStringLiteral("acquire-wait-failed"));
        closeFrameFds();
        return;
    }
    closeFd(acquireFd);

    signalPendingReleaseSyncobj(*generationState, *buffer, QStringLiteral("superseded"));
    buffer->releaseSyncobjFd = releaseFd;
    releaseFd = -1;
    buffer->releaseSyncContext = syncContext;
    buffer->releaseAttachedUsec = monotonicUsec();

    m_currentGeneration = generation;
    m_currentBuffer = bufferIndex;
    m_framesReceived++;
    setStreamState(Active);
    emit framesReceivedChanged();
    update();
    closeFd(releaseFd);
}

void VividDisplay::handleUnbind(const QByteArray& body)
{
    if (body.size() != static_cast<int>(VIVID_DISPLAY_UNBIND_BODY_BYTES)) {
        setLastError(QStringLiteral("invalid UNBIND body length %1").arg(body.size()));
        return;
    }

    const quint32 outputId = readU32LE(body, 0);
    const quint64 generation = readU64LE(body, 4);
    retireGeneration(generation);
    queueJsonFrame(VIVID_DISPLAY_REQ_UNBIND_DONE,
                   QJsonObject {
                       { QStringLiteral("outputId"), static_cast<qint64>(outputId) },
                       { QStringLiteral("generation"), QString::number(generation) },
                   });
    update();
}

void VividDisplay::handleProtocolError(int code, const QString& message)
{
    setLastError(message.isEmpty() ? formatErrno(code) : message);
    closeTransport(true);
    setConnState(Error);
    scheduleReconnect();
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
            item.retired = true;
    }
}

void VividDisplay::closeGenerationFds(Generation& generation)
{
    for (Buffer& buffer : generation.buffers) {
        signalPendingReleaseSyncobj(generation, buffer, QStringLiteral("generation-close"));
        for (Plane& plane : buffer.planes) {
            if (plane.fd >= 0) {
                ::close(plane.fd);
                plane.fd = -1;
            }
        }
    }
}

void VividDisplay::signalPendingReleaseSyncobj(Generation& generation,
                                               Buffer&     buffer,
                                               const QString& reason)
{
    if (buffer.releaseSyncobjFd < 0)
        return;

    const QString context = buffer.releaseSyncContext.isEmpty()
        ? reason
        : QStringLiteral("%1 %2").arg(buffer.releaseSyncContext, reason);
    signalReleaseSyncobj(generation.renderNode, buffer.releaseSyncobjFd, context);
    const quint64 ageUsec = buffer.releaseAttachedUsec > 0
        ? monotonicUsec() - buffer.releaseAttachedUsec
        : 0;
    if (ageUsec >= 100000) {
        qCInfo(lcWallpaperKde,
               "release syncobj signal was slow generation=%llu buffer=%u context=%s "
               "reason=%s age=%.2fms",
               static_cast<unsigned long long>(generation.id),
               buffer.index,
               qPrintable(context),
               qPrintable(reason),
               static_cast<double>(ageUsec) / 1000.0);
    }
    closeFd(buffer.releaseSyncobjFd);
    buffer.releaseSyncContext.clear();
    buffer.releaseAttachedUsec = 0;
}

void VividDisplay::flushPendingReleaseSyncobj(const QString& reason)
{
    /*
     * Mirror waywallen's EGL/QML display path: release the previously accepted
     * frame when the next FRAME_READY arrives. The Qt scene graph has no
     * portable "consumer finished sampling this imported EGLImage" fence here,
     * so updatePaintNode remains a fallback release point while this
     * arrival-driven flush prevents a delayed render pass from holding the
     * producer-side reaper for its full 500 ms timeout.
     */
    Generation* generation = findGeneration(m_currentGeneration);
    Buffer* buffer = findBuffer(generation, m_currentBuffer);
    if (!generation || !buffer)
        return;

    signalPendingReleaseSyncobj(*generation, *buffer, reason);
}

void VividDisplay::destroyImportedResources(Generation& generation)
{
    auto* gl = QOpenGLContext::currentContext()
        ? QOpenGLContext::currentContext()->extraFunctions()
        : nullptr;
    const EGLDisplay eglDisplay = eglGetCurrentDisplay();
    const auto destroyImage = resolveEglProc<EglDestroyImageKhr>("eglDestroyImageKHR");

    for (Buffer& buffer : generation.buffers) {
        if (gl && buffer.shadowFramebuffer != 0) {
            GLuint fbo = buffer.shadowFramebuffer;
            gl->glDeleteFramebuffers(1, &fbo);
            buffer.shadowFramebuffer = 0;
        }
        if (gl && buffer.shadowTexture != 0) {
            GLuint tex = buffer.shadowTexture;
            gl->glDeleteTextures(1, &tex);
            buffer.shadowTexture = 0;
        }
        if (gl && buffer.glTexture != 0) {
            GLuint tex = buffer.glTexture;
            gl->glDeleteTextures(1, &tex);
            buffer.glTexture = 0;
        }
        if (destroyImage && eglDisplay != EGL_NO_DISPLAY && buffer.eglImage != EGL_NO_IMAGE_KHR) {
            destroyImage(eglDisplay, buffer.eglImage);
            buffer.eglImage = EGL_NO_IMAGE_KHR;
        }
        buffer.importAttempted = false;
    }
}

void VividDisplay::clearGenerations(bool destroyGlResources)
{
    if (destroyGlResources) {
        for (Generation& generation : m_generations)
            destroyImportedResources(generation);
    }
    for (Generation& generation : m_generations)
        closeGenerationFds(generation);
    m_generations.clear();
}

void VividDisplay::destroyRetiredResources()
{
    for (qsizetype i = m_generations.size() - 1; i >= 0; --i) {
        if (!m_generations[i].retired || isCurrentGenerationIndex(i))
            continue;
        destroyImportedResources(m_generations[i]);
        closeGenerationFds(m_generations[i]);
        m_generations.removeAt(i);
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
    const EGLDisplay eglDisplay = eglGetCurrentDisplay();
    const auto createImage = resolveEglProc<EglCreateImageKhr>("eglCreateImageKHR");
    const auto destroyImage = resolveEglProc<EglDestroyImageKhr>("eglDestroyImageKHR");
    const auto imageTarget =
        resolveEglProc<GlEglImageTargetTexture2DOes>("glEGLImageTargetTexture2DOES");
    if (!gl || eglDisplay == EGL_NO_DISPLAY || !createImage || !destroyImage || !imageTarget) {
        setLastError(QStringLiteral("EGL dma-buf import functions are unavailable"));
        return false;
    }

    const QString gpuMismatch =
        bindBuffersGpuMismatchReason(generation.renderNode, generation.vendor, generation.pciAddress);
    if (!gpuMismatch.isEmpty()) {
        qCInfo(lcWallpaperKde,
               "producer GPU differs from Plasma GPU; attempting DMA-BUF import: %s",
               qPrintable(gpuMismatch));
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

    GLuint texture = 0;
    gl->glGenTextures(1, &texture);
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    while (gl->glGetError() != GL_NO_ERROR) {
    }
    imageTarget(GL_TEXTURE_2D, image);
    const GLenum importError = gl->glGetError();
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    if (importError != GL_NO_ERROR) {
        gl->glDeleteTextures(1, &texture);
        destroyImage(eglDisplay, image);
        setLastError(QStringLiteral("glEGLImageTargetTexture2DOES(GL_TEXTURE_2D) failed with GL error 0x%1")
                         .arg(static_cast<uint>(importError), 0, 16));
        return false;
    }

    buffer.eglImage = image;
    buffer.glTexture = texture;
    return true;
}

bool VividDisplay::generationUsesShadowCopy(const Generation& generation) const
{
    return generation.presentationPath == QStringLiteral("shadow-copy");
}

bool VividDisplay::ensureBufferPresentedThroughShadow(Generation& generation, Buffer& buffer)
{
    if (!generationUsesShadowCopy(generation))
        return true;
    if (!ensureBufferImported(generation, buffer))
        return false;

    auto* context = QOpenGLContext::currentContext();
    auto* gl = context ? context->extraFunctions() : nullptr;
    if (!gl) {
        setLastError(QStringLiteral("OpenGL functions are unavailable for shadow-copy"));
        return false;
    }

    const bool needsShadowAllocation = buffer.shadowTexture == 0;
    if (needsShadowAllocation) {
        GLuint shadowTexture = 0;
        gl->glGenTextures(1, &shadowTexture);
        gl->glBindTexture(GL_TEXTURE_2D, shadowTexture);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glTexImage2D(GL_TEXTURE_2D,
                         0,
                         GL_RGBA,
                         generation.width,
                         generation.height,
                         0,
                         GL_RGBA,
                         GL_UNSIGNED_BYTE,
                         nullptr);
        buffer.shadowTexture = shadowTexture;
    }

    GLint oldReadFramebuffer = 0;
    GLint oldDrawFramebuffer = 0;
    GLint oldViewport[4] = {0, 0, 0, 0};
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &oldReadFramebuffer);
    gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &oldDrawFramebuffer);
    gl->glGetIntegerv(GL_VIEWPORT, oldViewport);

    GLuint readFramebuffer = 0;
    gl->glGenFramebuffers(1, &readFramebuffer);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
    gl->glFramebufferTexture2D(GL_READ_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               buffer.glTexture,
                               0);
    const GLenum readStatus = gl->glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (readStatus != GL_FRAMEBUFFER_COMPLETE) {
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFramebuffer));
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFramebuffer));
        gl->glDeleteFramebuffers(1, &readFramebuffer);
        if (needsShadowAllocation)
            gl->glDeleteTextures(1, &buffer.shadowTexture);
        if (needsShadowAllocation)
            buffer.shadowTexture = 0;
        setLastError(QStringLiteral("producer framebuffer incomplete for shadow-copy status=0x%1")
                         .arg(static_cast<uint>(readStatus), 0, 16));
        return false;
    }

    const bool needsFramebufferAllocation = buffer.shadowFramebuffer == 0;
    if (needsFramebufferAllocation)
        gl->glGenFramebuffers(1, &buffer.shadowFramebuffer);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, buffer.shadowFramebuffer);
    gl->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,
                               GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D,
                               buffer.shadowTexture,
                               0);
    const GLenum status = gl->glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFramebuffer));
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFramebuffer));
        gl->glDeleteFramebuffers(1, &readFramebuffer);
        if (needsShadowAllocation)
            gl->glDeleteTextures(1, &buffer.shadowTexture);
        if (needsShadowAllocation)
            buffer.shadowTexture = 0;
        if (needsFramebufferAllocation)
            gl->glDeleteFramebuffers(1, &buffer.shadowFramebuffer);
        if (needsFramebufferAllocation)
            buffer.shadowFramebuffer = 0;
        setLastError(QStringLiteral("shadow framebuffer incomplete status=0x%1")
                         .arg(static_cast<uint>(status), 0, 16));
        return false;
    }

    /*
     * Shadow-copy mode decouples the producer slot from Qt/scene-graph
     * sampling. The imported DMA-BUF texture is used only as the source of this
     * GPU copy; the QSG node below samples buffer.shadowTexture. After glFinish
     * returns there is no outstanding GL work that reads the producer texture,
     * so the per-frame release syncobj can be signaled before Qt later submits
     * compositor work that samples the shadow.
     */
    gl->glViewport(0, 0, generation.width, generation.height);
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
    const GLenum copyError = gl->glGetError();
    gl->glFinish();
    gl->glBindTexture(GL_TEXTURE_2D, 0);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(oldReadFramebuffer));
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(oldDrawFramebuffer));
    gl->glViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);
    gl->glDeleteFramebuffers(1, &readFramebuffer);

    if (copyError != GL_NO_ERROR) {
        if (needsShadowAllocation)
            gl->glDeleteTextures(1, &buffer.shadowTexture);
        if (needsShadowAllocation)
            buffer.shadowTexture = 0;
        if (needsFramebufferAllocation)
            gl->glDeleteFramebuffers(1, &buffer.shadowFramebuffer);
        if (needsFramebufferAllocation)
            buffer.shadowFramebuffer = 0;
        setLastError(QStringLiteral("shadow-copy glBlitFramebuffer failed with GL error 0x%1")
                         .arg(static_cast<uint>(copyError), 0, 16));
        return false;
    }

    signalPendingReleaseSyncobj(generation, buffer, QStringLiteral("shadow-copy-complete"));
    return true;
}

QSGNode* VividDisplay::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    destroyRetiredResources();

    Generation* generation = findGeneration(m_currentGeneration);
    Buffer* buffer = findBuffer(generation, m_currentBuffer);
    if (!generation || !buffer) {
        delete oldNode;
        return nullptr;
    }

    if (!ensureBufferImported(*generation, *buffer) ||
        !ensureBufferPresentedThroughShadow(*generation, *buffer)) {
        sendBindFailed(*generation,
                       2,
                       m_lastError.isEmpty()
                           ? QStringLiteral("EGL/GL DMA-BUF import or shadow-copy failed")
                           : m_lastError);
        signalPendingReleaseSyncobj(*generation, *buffer, QStringLiteral("import-failed"));
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

    QQuickWindow* quickWindow = window();
    const GLuint presentationTexture = generationUsesShadowCopy(*generation)
        ? buffer->shadowTexture
        : buffer->glTexture;
    if (!quickWindow || presentationTexture == 0) {
        delete oldNode;
        signalPendingReleaseSyncobj(*generation, *buffer, QStringLiteral("window-or-texture-missing"));
        return nullptr;
    }

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

    QSGTexture* texture = QNativeInterface::QSGOpenGLTexture::fromNative(
        presentationTexture,
        quickWindow,
        QSize(generation->width, generation->height),
        QQuickWindow::TextureHasAlphaChannel);
    if (!texture) {
        delete transformNode;
        signalPendingReleaseSyncobj(*generation, *buffer, QStringLiteral("texture-node-failed"));
        return nullptr;
    }
    textureNode->setTexture(texture);

    const QRectF source = m_sourceRect.width() > 0 && m_sourceRect.height() > 0
        ? m_sourceRect
        : QRectF(0, 0, generation->width, generation->height);
    textureNode->setSourceRect(source);

    const QRectF bounds = boundingRect();
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
        const bool swapsDimensions = (m_transform == 1 || m_transform == 3);
        const qreal preWidth = swapsDimensions ? height : width;
        const qreal preHeight = swapsDimensions ? width : height;
        matrix.translate(static_cast<float>(width / 2.0), static_cast<float>(height / 2.0));
        matrix.rotate(static_cast<float>(m_transform * 90u), 0.0f, 0.0f, 1.0f);
        matrix.translate(static_cast<float>(-preWidth / 2.0), static_cast<float>(-preHeight / 2.0));
    }
    if (transformNode->matrix() != matrix) {
        transformNode->setMatrix(matrix);
        transformNode->markDirty(QSGNode::DirtyMatrix);
    }

    textureNode->markDirty(QSGNode::DirtyGeometry);
    if (!generationUsesShadowCopy(*generation))
        signalPendingReleaseSyncobj(*generation, *buffer, QStringLiteral("texture-node-updated"));
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
    body.resize(VIVID_DISPLAY_POINTER_MOTION_BODY_BYTES);
    writeU32LE(body, 0, m_outputId);
    writeF64LE(body, 4, x);
    writeF64LE(body, 12, y);
    writeU64LE(body, 20, timeUsec);
    queueFrame(VIVID_DISPLAY_REQ_POINTER_MOTION, body);
}

void VividDisplay::sendPointerButton(float x,
                                      float y,
                                      quint32 button,
                                      bool pressed,
                                      quint64 timeUsec)
{
    QByteArray body;
    body.resize(VIVID_DISPLAY_POINTER_BUTTON_BODY_BYTES);
    writeU32LE(body, 0, m_outputId);
    writeF64LE(body, 4, x);
    writeF64LE(body, 12, y);
    writeU32LE(body, 20, button);
    writeU32LE(body, 24, pressed ? VIVID_DISPLAY_BUTTON_PRESSED : VIVID_DISPLAY_BUTTON_RELEASED);
    writeU64LE(body, 28, timeUsec);
    queueFrame(VIVID_DISPLAY_REQ_POINTER_BUTTON, body);
}

void VividDisplay::sendPointerAxis(float x, float y, double dx, double dy, quint64 timeUsec)
{
    QByteArray body;
    body.resize(VIVID_DISPLAY_POINTER_AXIS_BODY_BYTES);
    writeU32LE(body, 0, m_outputId);
    writeF64LE(body, 4, x);
    writeF64LE(body, 12, y);
    writeF64LE(body, 20, dx);
    writeF64LE(body, 28, dy);
    writeU32LE(body, 36, VIVID_DISPLAY_AXIS_WHEEL);
    writeU64LE(body, 40, timeUsec);
    queueFrame(VIVID_DISPLAY_REQ_POINTER_AXIS, body);
}
