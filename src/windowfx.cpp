#include "windowfx.h"

#include <QGuiApplication>
#include <QOperatingSystemVersion>
#include <QWindow>
#include <QtGui/qevent.h>   // QPlatformSurfaceEvent

#include <map>

#if defined(_WIN32)
#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>   // MARGINS

// Windows 11 SDK values; older SDK headers have neither the attribute nor the enum.
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#define DWMSBT_NONE 1
#define DWMSBT_MAINWINDOW 2
#endif
#endif

#if defined(FLOW_WAYLAND_BLUR)
#include <QtGui/qguiapplication_platform.h>   // QNativeInterface::QWaylandApplication (public)
#include <qpa/qplatformwindow_p.h>            // QNativeInterface::Private::QWaylandWindow (GuiPrivate)
#include <QtWaylandClient/qwaylandclientextension.h>
#include <wayland-client-protocol.h>
#include "qwayland-ext-background-effect-v1.h"

namespace {

// ext_background_effect_manager_v1, bound through Qt's extension template. Qt
// binds the global from the registry and emits activeChanged when it appears
// or goes away.
class BackgroundEffectManager final
    : public QWaylandClientExtensionTemplate<BackgroundEffectManager>
    , public QtWayland::ext_background_effect_manager_v1
{
public:
    BackgroundEffectManager()
        : QWaylandClientExtensionTemplate<BackgroundEffectManager>(1)
    {
    }

    ~BackgroundEffectManager() override
    {
        if (isInitialized())
            destroy();
    }
};

wl_compositor *waylandCompositor()
{
    auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    return wayland ? wayland->compositor() : nullptr;
}

// The region covers the whole surface. The protocol clips it to the surface
// size, and wl_region is copied by set_blur_region, so it can be destroyed here.
void setBlurRegion(QtWayland::ext_background_effect_surface_v1 *blur, const QSize &size)
{
    wl_compositor *compositor = waylandCompositor();
    if (!compositor)
        return;
    wl_region *region = wl_compositor_create_region(compositor);
    wl_region_add(region, 0, 0, size.width(), size.height());
    blur->set_blur_region(region);
    wl_region_destroy(region);
}

} // namespace
#endif

// Per-window state: what the caller last asked for, and the native object that
// carries it out. The request outlives the native surface, so a surface that
// is recreated gets the same effect.
struct WindowFx::Target
{
    bool wanted = false;   // last applyGlass() request for this window
#if defined(FLOW_WAYLAND_BLUR)
    QtWayland::ext_background_effect_surface_v1 *blur = nullptr;   // owned; null while off
#endif
};

struct WindowFx::Native
{
    std::map<QWindow *, Target> targets;
#if defined(FLOW_WAYLAND_BLUR)
    std::unique_ptr<BackgroundEffectManager> blur;   // null unless Wayland
#endif
};

WindowFx::WindowFx(QObject *parent)
    : QObject(parent)
    , m_native(std::make_unique<Native>())
{
#if defined(FLOW_WAYLAND_BLUR)
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
        m_native->blur = std::make_unique<BackgroundEffectManager>();
        connect(m_native->blur.get(), &QWaylandClientExtension::activeChanged,
                this, &WindowFx::onBlurActiveChanged);
    }
#endif
}

WindowFx::~WindowFx()
{
    for (auto &[window, target] : m_native->targets) {
        window->removeEventFilter(this);
        release(target);
    }
}

bool WindowFx::blurAvailable() const
{
#if defined(_WIN32)
    return QOperatingSystemVersion::current()
        >= QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 22621);
#elif defined(FLOW_WAYLAND_BLUR)
    return m_native->blur && m_native->blur->isActive();
#else
    return false;
#endif
}

void WindowFx::applyGlass(QObject *object, bool on)
{
#if defined(_WIN32) || defined(FLOW_WAYLAND_BLUR)
    auto *window = qobject_cast<QWindow *>(object);
    if (!window)
        return;

    auto it = m_native->targets.find(window);
    if (it == m_native->targets.end()) {
        if (!on)
            return;   // nothing was ever turned on
        it = m_native->targets.try_emplace(window).first;
        window->installEventFilter(this);
        connect(window, &QObject::destroyed, this, [this, window] { forget(window); });
    }
    it->second.wanted = on;
    apply(window, it->second);
#else
    Q_UNUSED(object);
    Q_UNUSED(on);
#endif
}

void WindowFx::apply(QWindow *window, Target &target)
{
    // No native window yet: SurfaceCreated applies the remembered request.
    if (!window->handle())
        return;

#if defined(_WIN32)
    if (!blurAvailable())
        return;
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    const int type = target.wanted ? DWMSBT_MAINWINDOW : DWMSBT_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof type);
    // Extending the frame lets the backdrop show through the client area.
    // Off resets it, so the client area goes back to being opaque.
    const MARGINS margins = target.wanted ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
#elif defined(FLOW_WAYLAND_BLUR)
    if (!target.wanted) {
        release(target);
        window->requestUpdate();   // the next commit drops the effect
        return;
    }

    // Until the global is bound, onBlurActiveChanged() applies the request.
    BackgroundEffectManager *manager = m_native->blur.get();
    if (!manager || !manager->isActive())
        return;

    auto *waylandWindow = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    wl_surface *surface = waylandWindow ? waylandWindow->surface() : nullptr;
    if (!surface)
        return;

    if (!target.blur)
        target.blur = new QtWayland::ext_background_effect_surface_v1(manager->get_background_effect(surface));
    setBlurRegion(target.blur, window->size());
    // The blur is double-buffered and only lands on the next commit, which Qt
    // makes when it renders. Ask for a frame so an idle window still updates.
    window->requestUpdate();
#else
    Q_UNUSED(window);
    Q_UNUSED(target);
#endif
}

void WindowFx::release(Target &target)
{
#if defined(FLOW_WAYLAND_BLUR)
    if (target.blur) {
        // The generated class does not send the destructor request itself.
        target.blur->destroy();
        delete target.blur;
        target.blur = nullptr;
    }
#else
    Q_UNUSED(target);
#endif
}

void WindowFx::forget(QWindow *window)
{
    auto it = m_native->targets.find(window);
    if (it == m_native->targets.end())
        return;
    release(it->second);
    m_native->targets.erase(it);
}

bool WindowFx::eventFilter(QObject *watched, QEvent *event)
{
    auto *window = qobject_cast<QWindow *>(watched);
    auto it = window ? m_native->targets.find(window) : m_native->targets.end();
    if (it != m_native->targets.end()) {
        switch (event->type()) {
        case QEvent::PlatformSurface:
            if (static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType()
                == QPlatformSurfaceEvent::SurfaceCreated) {
                apply(window, it->second);
            } else {
                // The native surface is going away; the request itself is kept.
                release(it->second);
            }
            break;
#if defined(FLOW_WAYLAND_BLUR)
        case QEvent::Resize:
            if (it->second.blur)
                setBlurRegion(it->second.blur, window->size());
            break;
#endif
        default:
            break;
        }
    }
    return QObject::eventFilter(watched, event);
}

void WindowFx::onBlurActiveChanged()
{
#if defined(FLOW_WAYLAND_BLUR)
    const bool active = m_native->blur && m_native->blur->isActive();
    for (auto &[window, target] : m_native->targets) {
        if (active)
            apply(window, target);
        else
            release(target);
    }
#endif
    emit blurAvailableChanged();
}
