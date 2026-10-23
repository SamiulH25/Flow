#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

#include <memory>

class QEvent;
class QWindow;

// Native backdrop blur behind translucent windows. Windows 11 (build 22621+)
// gets the system main-window backdrop; Wayland compositors that advertise
// ext_background_effect_manager_v1 get a blur region. Everywhere else the
// calls are no-ops and blurAvailable is false.
class WindowFx : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool blurAvailable READ blurAvailable NOTIFY blurAvailableChanged)

public:
    explicit WindowFx(QObject *parent = nullptr);
    ~WindowFx() override;

    bool blurAvailable() const;

    // Turns the blur behind `window` (a QWindow, e.g. a QQuickWindow) on or off.
    // Safe before the native window exists: the request is remembered and
    // applied once the window has a native surface.
    Q_INVOKABLE void applyGlass(QObject *window, bool on);

signals:
    void blurAvailableChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Target;   // per-window request and native handles
    struct Native;   // platform state: window targets and the Wayland manager

    void apply(QWindow *window, Target &target);
    void release(Target &target);
    void forget(QWindow *window);
    void onBlurActiveChanged();

    std::unique_ptr<Native> m_native;
};
