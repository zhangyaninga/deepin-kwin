// Copyright (C) 2014 Martin Gräßlin <mgraesslin@kde.org>
// Copyright 2014  Martin Gräßlin <mgraesslin@kde.org>
// SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "connection.h"
#include "context.h"
#include "device.h"
#include "events.h"
#include "../workspace.h"
#ifndef KWIN_BUILD_TESTING
#include "../screens.h"
#endif
#include "../logind.h"
#include "../udev.h"
#include "libinput_logging.h"

#include <QDBusMessage>
#include <QDBusConnection>
#include <QDBusPendingCall>
#include <QMutexLocker>
#include <QSocketNotifier>
#include <QThread>

#include <libinput.h>
#include <cmath>

#include <sys/sdt.h>

namespace KWin
{
namespace LibInput
{

class ConnectionAdaptor : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin.InputDeviceManager")
    Q_PROPERTY(QStringList devicesSysNames READ devicesSysNames CONSTANT)

private:
    Connection *m_con;

public:
    ConnectionAdaptor(Connection *con)
        : m_con(con)
    {
        connect(con, &Connection::deviceAddedSysName, this, &ConnectionAdaptor::deviceAdded, Qt::QueuedConnection);
        connect(con, &Connection::deviceRemovedSysName, this, &ConnectionAdaptor::deviceRemoved, Qt::QueuedConnection);

        QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/kde/KWin/InputDevice"),
                                                     QStringLiteral("org.kde.KWin.InputDeviceManager"),
                                                     this,
                                                     QDBusConnection::ExportAllProperties | QDBusConnection::ExportAllSignals
        );
    }

    ~ConnectionAdaptor() {
        QDBusConnection::sessionBus().unregisterObject(QStringLiteral("/org/kde/KWin/InputDeviceManager"));
    }

    QStringList devicesSysNames() {
        // TODO: is this allowed? directly calling function of object in another thread!?
        //       otherwise use signal-slot mechanism
        return m_con->devicesSysNames();
    }

Q_SIGNALS:
    void deviceAdded(QString sysName);
    void deviceRemoved(QString sysName);

};

Connection *Connection::s_self = nullptr;
QPointer<QThread> Connection::s_thread;

static ConnectionAdaptor *s_adaptor = nullptr;
static Context *s_context = nullptr;

static quint32 toLibinputLEDS(Xkb::LEDs leds)
{
    quint32 libinputLeds = 0;
    if (leds.testFlag(Xkb::LED::NumLock)) {
        libinputLeds = libinputLeds | LIBINPUT_LED_NUM_LOCK;
    }
    if (leds.testFlag(Xkb::LED::CapsLock)) {
        libinputLeds = libinputLeds | LIBINPUT_LED_CAPS_LOCK;
    }
    if (leds.testFlag(Xkb::LED::ScrollLock)) {
        libinputLeds = libinputLeds | LIBINPUT_LED_SCROLL_LOCK;
    }
    return libinputLeds;
}

Connection::Connection(QObject *parent)
    : Connection(nullptr, parent)
{
    // only here to fix build, using will crash, BUG 343529
}

void Connection::createThread()
{
    if (s_thread) {
        return;
    }
    s_thread = new QThread();
    s_thread->setObjectName(QStringLiteral("libinput-connection"));
    s_thread->start();
}

QPointer<QThread> Connection::getThread()
{
    if (s_thread) {
        return s_thread;
    }
    return NULL;
}

Connection *Connection::create(QObject *parent)
{
    Q_ASSERT(!s_self);
    static Udev s_udev;
    if (!s_udev.isValid()) {
        qCWarning(KWIN_LIBINPUT) << "Failed to initialize udev";
        return nullptr;
    }
    if (!s_context) {
        s_context = new Context(s_udev);
        if (!s_context->isValid()) {
            qCWarning(KWIN_LIBINPUT) << "Failed to create context from udev";
            delete s_context;
            s_context = nullptr;
            return nullptr;
        }
        if (!s_context->assignSeat(LogindIntegration::self()->seat().toUtf8().constData())) {
            qCWarning(KWIN_LIBINPUT) << "Failed to assign seat" << LogindIntegration::self()->seat();
            delete s_context;
            s_context = nullptr;
            return nullptr;
        }
    }
    Connection::createThread();
    s_self = new Connection(s_context);
    s_self->moveToThread(s_thread);
    QObject::connect(s_thread, &QThread::finished, s_self, &QObject::deleteLater);
    QObject::connect(s_thread, &QThread::finished, s_thread, &QObject::deleteLater);
    QObject::connect(parent, &QObject::destroyed, s_thread, &QThread::quit);
    if (!s_adaptor) {
        s_adaptor = new ConnectionAdaptor(s_self);
    }

    return s_self;
}


Connection::Connection(Context *input, QObject *parent)
    : QObject(parent)
    , m_input(input)
    , m_notifier(nullptr)
    , m_mutex(QMutex::Recursive)
    , m_leds()
{
    Q_ASSERT(m_input);
    // need to connect to KGlobalSettings as the mouse KCM does not emit a dedicated signal
    QDBusConnection::sessionBus().connect(QString(), QStringLiteral("/KGlobalSettings"), QStringLiteral("org.kde.KGlobalSettings"),
                                          QStringLiteral("notifyChange"), this, SLOT(slotKGlobalSettingsNotifyChange(int,int)));
}

Connection::~Connection()
{
    delete s_adaptor;
    s_adaptor = nullptr;
    s_self = nullptr;
    delete s_context;
    s_context = nullptr;
    qDebug() << QDateTime::currentDateTime().toString(QString::fromLatin1("hh:mm:ss.zzz")) << Q_FUNC_INFO << "input thread finish";
}

void Connection::setup()
{
    QMetaObject::invokeMethod(this, "doSetup", Qt::QueuedConnection);
}

void Connection::doSetup()
{
    connect(s_self, &Connection::deviceAdded, s_self, [](Device* device) {
                emit s_self->deviceAddedSysName(device->sysName());
            });
    connect(s_self, &Connection::deviceRemoved, s_self, [](Device* device) {
                emit s_self->deviceRemovedSysName(device->sysName());
            });

    Q_ASSERT(!m_notifier);
    m_notifier = new QSocketNotifier(m_input->fileDescriptor(), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Connection::handleEvent);

    LogindIntegration *logind = LogindIntegration::self();
    connect(logind, &LogindIntegration::sessionActiveChanged, this,
        [this](bool active) {
            if (active) {
                if (!m_input->isSuspended()) {
                    qDebug() << "sessionActiveChanged active m_input has resumed";
                    return;
                }
                qDebug() << "sessionActiveChanged active m_input->resume";
                m_input->resume();
                wasSuspended = true;
            } else {
                qDebug() << "sessionActiveChanged deactivate";
                deactivate();
            }
        }
    );
    handleEvent();
}

void Connection::deactivate()
{
    if (m_input->isSuspended()) {
        return;
    }
    m_keyboardBeforeSuspend = hasKeyboard();
    m_alphaNumericKeyboardBeforeSuspend = hasAlphaNumericKeyboard();
    m_pointerBeforeSuspend = hasPointer();
    m_touchBeforeSuspend = hasTouch();
    m_tabletModeSwitchBeforeSuspend = hasTabletModeSwitch();
    m_input->suspend();
    handleEvent();
}

void Connection::handleEvent()
{
    if (workspace() && workspace()->isKwinDebug()) {
        qDebug() << "begin";
    }
    QMutexLocker locker(&m_mutex);
    const bool wasEmpty = m_eventQueue.isEmpty();
    do {
        m_input->dispatch();
        Event *event = m_input->event();
        if (!event) {
            if (workspace() && workspace()->isKwinDebug()) {
                qDebug()<<"no event";
            }
            break;
        }
        if (workspace() && workspace()->isKwinDebug()) {
            qDebug()<<"append event"<<event->type();
        }
        m_eventQueue << event;
    } while (true);
    if (wasEmpty && !m_eventQueue.isEmpty()) {
        if (workspace() && workspace()->isKwinDebug()) {
            qDebug()<<"emit eventsRead";
        }
	    emit eventsRead();
    }
}

void Connection::processEvents()
{
    QMutexLocker locker(&m_mutex);
    while (!m_eventQueue.isEmpty()) {
        QScopedPointer<Event> event(m_eventQueue.takeFirst());
        switch (event->type()) {
            case LIBINPUT_EVENT_DEVICE_ADDED: {
                auto device = new Device(event->nativeDevice());
                device->moveToThread(s_thread);
                m_devices << device;
                if (device->isKeyboard()) {
                    m_keyboard++;
                    if (device->isAlphaNumericKeyboard()) {
                        m_alphaNumericKeyboard++;
                        if (m_alphaNumericKeyboard == 1) {
                            emit hasAlphaNumericKeyboardChanged(true);
                        }
                    }
                    if (m_keyboard == 1) {
                        emit hasKeyboardChanged(true);
                    }
                }
                if (device->isPointer()) {
                    m_pointer++;
                    if (m_pointer == 1) {
                        emit hasPointerChanged(true);
                    }
                }
                if (device->isTouch()) {
                    m_touch++;
                    if (m_touch == 1) {
                        emit hasTouchChanged(true);
                    }
                }
                if (device->isTabletModeSwitch()) {
                    m_tabletModeSwitch++;
                    if (m_tabletModeSwitch == 1) {
                        emit hasTabletModeSwitchChanged(true);
                    }
                }
                applyDeviceConfig(device);
                applyScreenToDevice(device);

                // enable possible leds
                libinput_device_led_update(device->device(), static_cast<libinput_led>(toLibinputLEDS(m_leds)));
                qDebug() << "deviceAdded device" << device->name() << device->sysName();
                emit deviceAdded(device);
                break;
            }
            case LIBINPUT_EVENT_DEVICE_REMOVED: {
                auto it = std::find_if(m_devices.begin(), m_devices.end(), [&event] (Device *d) { return event->device() == d; } );
                if (it == m_devices.end()) {
                    // we don't know this device
                    break;
                }
                auto device = *it;
                m_devices.erase(it);
                qDebug() << "deviceRemoved device" << device->name() << device->sysName();
                emit deviceRemoved(device);

                if (device->isKeyboard()) {
                    m_keyboard--;
                    if (device->isAlphaNumericKeyboard()) {
                        m_alphaNumericKeyboard--;
                        if (m_alphaNumericKeyboard == 0) {
                            emit hasAlphaNumericKeyboardChanged(false);
                        }
                    }
                    if (m_keyboard == 0) {
                        emit hasKeyboardChanged(false);
                    }
                }
                if (device->isPointer()) {
                    m_pointer--;
                    if (m_pointer == 0) {
                        emit hasPointerChanged(false);
                    }
                }
                if (device->isTouch()) {
                    m_touch--;
                    if (m_touch == 0) {
                        emit hasTouchChanged(false);
                    }
                }
                if (device->isTabletModeSwitch()) {
                    m_tabletModeSwitch--;
                    if (m_tabletModeSwitch == 0) {
                        emit hasTabletModeSwitchChanged(false);
                    }
                }
                device->deleteLater();
                break;
            }
            case LIBINPUT_EVENT_KEYBOARD_KEY: {
                KeyEvent *ke = static_cast<KeyEvent*>(event.data());
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"emit keyChanged("<<ke->key()<<")";
                }
                emit keyChanged(ke->key(), ke->state(), ke->time(), ke->device());
                DTRACE_PROBE4(libinput, keyboard, "Keyboard", ke->key(), ke->state(), ke->time());
                break;
            }
            case LIBINPUT_EVENT_POINTER_AXIS: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_POINTER_AXIS";
                }
                PointerEvent *pe = static_cast<PointerEvent*>(event.data());
                struct Axis {
                    qreal delta = 0.0;
                    quint32 time = 0;
                };
                QMap<InputRedirection::PointerAxis, Axis> deltas;
                auto update = [&deltas] (PointerEvent *pe) {
                    const auto axis = pe->axis();
                    for (auto it = axis.begin(); it != axis.end(); ++it) {
                        deltas[*it].delta += pe->axisValue(*it);
                        deltas[*it].time = pe->time();
                    }
                };
                update(pe);
                auto it = m_eventQueue.begin();
                while (it != m_eventQueue.end()) {
                    if ((*it)->type() == LIBINPUT_EVENT_POINTER_AXIS) {
                        QScopedPointer<PointerEvent> p(static_cast<PointerEvent*>(*it));
                        update(p.data());
                        it = m_eventQueue.erase(it);
                    } else {
                        break;
                    }
                }
                for (auto it = deltas.constBegin(); it != deltas.constEnd(); ++it) {
                    emit pointerAxisChanged(it.key(), it.value().delta, it.value().time, pe->device());
                }
                break;
            }
            case LIBINPUT_EVENT_POINTER_BUTTON: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"emit pointerButtonChanged";
                }
                PointerEvent *pe = static_cast<PointerEvent*>(event.data());
                emit pointerButtonChanged(pe->button(), pe->buttonState(), pe->time(), pe->device());
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION: {
                PointerEvent *pe = static_cast<PointerEvent*>(event.data());
                auto delta = pe->delta();
                auto deltaNonAccel = pe->deltaUnaccelerated();
                quint32 latestTime = pe->time();
                quint64 latestTimeUsec = pe->timeMicroseconds();
                auto it = m_eventQueue.begin();
                while (it != m_eventQueue.end()) {
                    if ((*it)->type() == LIBINPUT_EVENT_POINTER_MOTION) {
                        QScopedPointer<PointerEvent> p(static_cast<PointerEvent*>(*it));
                        delta += p->delta();
                        deltaNonAccel += p->deltaUnaccelerated();
                        latestTime = p->time();
                        latestTimeUsec = p->timeMicroseconds();
                        it = m_eventQueue.erase(it);
                    } else {
                        break;
                    }
                }
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"emit pointerMotion"<<pe->delta()<<delta;
                }
                emit pointerMotion(delta, deltaNonAccel, latestTime, latestTimeUsec, pe->device());
                break;
            }
            case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
                PointerEvent *pe = static_cast<PointerEvent*>(event.data());
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"emit pointerMotionAbsolute("<<pe->absolutePos(m_size)<<")";
                }
                emit pointerMotionAbsolute(pe->absolutePos(), pe->absolutePos(m_size), pe->time(), pe->device());
                break;
            }
            case LIBINPUT_EVENT_TOUCH_DOWN: {
#ifndef KWIN_BUILD_TESTING
                TouchEvent *te = static_cast<TouchEvent*>(event.data());
                const auto &geo = screens()->geometry(te->device()->screenId());
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"emit touchDown("<<te->absolutePos(geo.size())<<")";
                }
                emit touchDown(te->id(), geo.topLeft() + te->absolutePos(geo.size()), te->time(), te->device());
                break;
#endif
            }
            case LIBINPUT_EVENT_TOUCH_UP: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_TOUCH_UP";
                }
                TouchEvent *te = static_cast<TouchEvent*>(event.data());
                emit touchUp(te->id(), te->time(), te->device());
                break;
            }
            case LIBINPUT_EVENT_TOUCH_MOTION: {
#ifndef KWIN_BUILD_TESTING
                TouchEvent *te = static_cast<TouchEvent*>(event.data());
                const auto &geo = screens()->geometry(te->device()->screenId());
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"emit touchMotion("<<te->absolutePos()<<")";
                }
                emit touchMotion(te->id(), geo.topLeft() + te->absolutePos(geo.size()), te->time(), te->device());
                break;
#endif
            }
            case LIBINPUT_EVENT_TOUCH_CANCEL: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_TOUCH_CANCEL";
                }
                emit touchCanceled(event->device());
                break;
            }
            case LIBINPUT_EVENT_TOUCH_FRAME: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_TOUCH_FRAME";
                }
                emit touchFrame(event->device());
                break;
            }
            case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_GESTURE_PINCH_BEGIN";
                }
                PinchGestureEvent *pe = static_cast<PinchGestureEvent*>(event.data());
                emit pinchGestureBegin(pe->fingerCount(), pe->time(), pe->device());
                break;
            }
            case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_GESTURE_PINCH_UPDATE";
                }
                PinchGestureEvent *pe = static_cast<PinchGestureEvent*>(event.data());
                emit pinchGestureUpdate(pe->scale(), pe->angleDelta(), pe->delta(), pe->time(), pe->device());
                break;
            }
            case LIBINPUT_EVENT_GESTURE_PINCH_END: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_GESTURE_PINCH_END";
                }
                PinchGestureEvent *pe = static_cast<PinchGestureEvent*>(event.data());
                if (pe->isCancelled()) {
                    //emit pinchGestureCancelled(pe->time(), pe->device());
                } else {
                    //emit pinchGestureEnd(pe->time(), pe->device());
                }
                break;
            }
            case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN";
                }
                SwipeGestureEvent *se = static_cast<SwipeGestureEvent*>(event.data());
                emit swipeGestureBegin(se->fingerCount(), se->time(), se->device());
                break;
            }
            case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE";
                }
                SwipeGestureEvent *se = static_cast<SwipeGestureEvent*>(event.data());
                emit swipeGestureUpdate(se->delta(), se->time(), se->device());
                break;
            }
            case LIBINPUT_EVENT_GESTURE_SWIPE_END: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_GESTURE_SWIPE_END";
                }
                SwipeGestureEvent *se = static_cast<SwipeGestureEvent*>(event.data());
                if (se->isCancelled()) {
                    emit swipeGestureCancelled(se->time(), se->device());
                } else {
                    emit swipeGestureEnd(se->time(), se->device());
                }
                break;
            }
            case LIBINPUT_EVENT_SWITCH_TOGGLE: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_SWITCH_TOGGLE";
                }
                SwitchEvent *se = static_cast<SwitchEvent*>(event.data());
                switch (se->state()) {
                case SwitchEvent::State::Off:
                    emit switchToggledOff(se->time(), se->timeMicroseconds(), se->device());
                    break;
                case SwitchEvent::State::On:
                    emit switchToggledOn(se->time(), se->timeMicroseconds(), se->device());
                    break;
                default:
                    Q_UNREACHABLE();
                }
                break;
            }
            case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
            case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
            case LIBINPUT_EVENT_TABLET_TOOL_TIP: {
                auto *tte = static_cast<TabletToolEvent *>(event.data());

                KWin::InputRedirection::TabletEventType tabletEventType;
                switch (event->type()) {
                case LIBINPUT_EVENT_TABLET_TOOL_AXIS:
                    if (workspace() && workspace()->isKwinDebug()) {
                        qDebug()<<"LIBINPUT_EVENT_TABLET_TOOL_AXIS"<<tte->xTilt()<<tte->yTilt();
                    }
                    tabletEventType = KWin::InputRedirection::Axis;
                    break;
                case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY:
                    if (workspace() && workspace()->isKwinDebug()) {
                        qDebug()<<"LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY"<<tte->xTilt()<<tte->yTilt();
                    }
                    tabletEventType = KWin::InputRedirection::Proximity;
                    break;
                case LIBINPUT_EVENT_TABLET_TOOL_TIP:
                    if (workspace() && workspace()->isKwinDebug()) {
                        qDebug()<<"LIBINPUT_EVENT_TABLET_TOOL_TIP"<<tte->xTilt()<<tte->yTilt();
                    }
                default:
                    tabletEventType = KWin::InputRedirection::Tip;
                    break;
                }
                auto serial = libinput_tablet_tool_get_serial(tte->tool());
                auto toolId = libinput_tablet_tool_get_tool_id(tte->tool());

#ifndef KWIN_BUILD_TESTING
                const auto &geo = screens()->geometry(tte->device()->screenId());
                emit tabletToolEvent(tabletEventType,
                                     geo.topLeft() + tte->transformedPosition(geo.size()), tte->pressure(),
                                     tte->xTilt(), tte->yTilt(), tte->rotation(),
                                     tte->isTipDown(), tte->isNearby(), serial,
                                     toolId, event->device());
#else
                 emit tabletToolEvent(tabletEventType,
                                      tte->transformedPosition(m_size), tte->pressure(),
                                      tte->xTilt(), tte->yTilt(), tte->rotation(),
                                      tte->isTipDown(), tte->isNearby(), serial,
                                      toolId, event->device());

#endif
                break;
            }
            case LIBINPUT_EVENT_TABLET_TOOL_BUTTON: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_TABLET_TOOL_BUTTON";
                }
                auto *tabletEvent = static_cast<TabletToolButtonEvent *>(event.data());
                emit tabletToolButtonEvent(tabletEvent->buttonId(),
                                           tabletEvent->isButtonPressed());
                break;
            }
            case LIBINPUT_EVENT_TABLET_PAD_BUTTON: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_TABLET_PAD_BUTTON";
                }
                auto *tabletEvent = static_cast<TabletPadButtonEvent *>(event.data());
                emit tabletPadButtonEvent(tabletEvent->buttonId(),
                                          tabletEvent->isButtonPressed());
                break;
            }
            case LIBINPUT_EVENT_TABLET_PAD_RING: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_TABLET_PAD_RING";
                }
                auto *tabletEvent = static_cast<TabletPadRingEvent *>(event.data());
                emit tabletPadRingEvent(tabletEvent->number(),
                                        tabletEvent->position(),
                                        tabletEvent->source() ==
                                            LIBINPUT_TABLET_PAD_RING_SOURCE_FINGER);
                break;
            }
            case LIBINPUT_EVENT_TABLET_PAD_STRIP: {
                if (workspace() && workspace()->isKwinDebug()) {
                    qDebug()<<"LIBINPUT_EVENT_TABLET_PAD_STRIP";
                }
                auto *tabletEvent = static_cast<TabletPadStripEvent *>(event.data());
                emit tabletPadStripEvent(tabletEvent->number(),
                                         tabletEvent->position(),
                                         tabletEvent->source() ==
                                             LIBINPUT_TABLET_PAD_STRIP_SOURCE_FINGER);
                break;
            }
            default:
                // nothing
                break;
        }
    }
    if (wasSuspended) {
        if (m_keyboardBeforeSuspend && !m_keyboard) {
            emit hasKeyboardChanged(false);
        }
        if (m_alphaNumericKeyboardBeforeSuspend && !m_alphaNumericKeyboard) {
            emit hasAlphaNumericKeyboardChanged(false);
        }
        if (m_pointerBeforeSuspend && !m_pointer) {
            emit hasPointerChanged(false);
        }
        if (m_touchBeforeSuspend && !m_touch) {
            emit hasTouchChanged(false);
        }
        if (m_tabletModeSwitchBeforeSuspend && !m_tabletModeSwitch) {
            emit hasTabletModeSwitchChanged(false);
        }
        wasSuspended = false;
    }
}

void Connection::setScreenSize(const QSize &size)
{
    m_size = size;
}

void Connection::updateScreens()
{
    QMutexLocker locker(&m_mutex);
    for (auto device: qAsConst(m_devices)) {
        applyScreenToDevice(device);
    }
}

void Connection::setTouchDeviceToScreenId(const QString &touchDeviceSysName, int screenId) {
    bool isHasTheTouchDeivce = false;
    for (auto device: qAsConst(m_devices)) {
        if (device->isTouch()) {
            if (touchDeviceSysName == device->sysName()) {
                isHasTheTouchDeivce = true;
                break;
            }
        }
    }
    if (!isHasTheTouchDeivce) {
        qDebug() << "do not find touch device!";
        return;
    }
    if (screenId > -1 && screenId >= screens()->count()) {
        qDebug() << "do not find screen!";
        return;
    }
    m_touchDeviceToScreenMap[touchDeviceSysName] = screenId;
    updateScreens();
}

QString Connection::getTouchDeviceToScreenInfo(){
    QMap<QString, int> allTouchDeviceToScreens;
    QJsonArray allTouchDevicesAndScreens;
    for (auto device: qAsConst(m_devices)) {
        if (device->isTouch()) {
            allTouchDeviceToScreens.insert(device->sysName(),0);
        }
    }
    qDebug()<<allTouchDeviceToScreens;

    QMapIterator<QString, int> iter(m_touchDeviceToScreenMap);
    while(iter.hasNext()) {
        iter.next();
        if (iter.value() < 0 || iter.value() >= screens()->count()) {
            continue;
        }
        allTouchDeviceToScreens[iter.key()] = iter.value();
    }

    QMapIterator<QString, int> allIter(allTouchDeviceToScreens);
    while(allIter.hasNext()) {
        allIter.next();
        QJsonObject accelObj;
        if (allIter.value() < 0 || allIter.value() >= screens()->count()) {
            accelObj.insert("ScreenUuid", "");
        } else {
            accelObj.insert("ScreenUuid", QString(screens()->uuid(allIter.value())));
        }
        accelObj.insert("TouchDevice", allIter.key());
        accelObj.insert("ScreenId", allIter.value());
        allTouchDevicesAndScreens.append(accelObj);

    }
    for (int i = 0; i < screens()->count(); i++) {
        bool isExist = false;
        foreach (int v, allTouchDeviceToScreens) {
            if (i == v) {
                isExist = true;
                break;
            }
        }
        if (isExist) {
            continue;
        }
        QJsonObject accelObj;
        accelObj.insert("ScreenUuid", QString(screens()->uuid(i)));
        accelObj.insert("ScreenId", i);
        allTouchDevicesAndScreens.append(accelObj);
    }

    return QJsonDocument(allTouchDevicesAndScreens).toJson(QJsonDocument::Compact);
}

void Connection::applyScreenToDevice(Device *device)
{
#ifndef KWIN_BUILD_TESTING
    QMutexLocker locker(&m_mutex);
    // calibration tablet events in tablet screen
    if (device->isTabletTool()) {
        int tabletScreenId = -1;
        // a special patch for tablet used for 0102/0106 branch
        // after that, we should reconstruct the calibration for tablet
        bool m_hasInternal = false;
        for (int i = 0; i < screens()->count(); i++) {
            if (screens()->isInternal(i)) {
                m_hasInternal = true;
                break;
            }
        }
        if (m_hasInternal) {
            // for 0102, we have an internal output
            // then tablet output is the external output
            for (int i = 0; i < screens()->count(); i++) {
                if (!screens()->isInternal(i)) {
                    tabletScreenId = i;
                    break;
                }
            }
        } else {
            // for 0106, we have two external output
            // one is VGA, one is HDMI such as tablet output
            for (int i = 0; i < screens()->count(); i++) {
                if (!screens()->name(i).contains("VGA")) {
                    tabletScreenId = i;
                    break;
                }
            }
        }
        int id = (tabletScreenId != -1) ? tabletScreenId : 0;
        device->setScreenId(id);
        device->setOrientation(screens()->orientation(id));
        return;
    }
    if (!device->isTouch()) {
        return;
    }
    int id = -1;
    // let's try to find a screen for it
    if (screens()->count() == 1) {
        id = 0;
    }
    if (screens()->count() > 1 && m_touchDeviceToScreenMap.contains(device->sysName()) && m_touchDeviceToScreenMap[device->sysName()] > -1 && m_touchDeviceToScreenMap[device->sysName()] < screens()->count()) {
        id = m_touchDeviceToScreenMap[device->sysName()];
    }
    if (id == -1 && !device->outputName().isEmpty()) {
        // we have an output name, try to find a screen with matching name
        for (int i = 0; i < screens()->count(); i++) {
            if (screens()->name(i) == device->outputName()) {
                id = i;
                break;
            }
        }
    }
    if (id == -1) {
        // do we have an internal screen?
        int internalId = -1;
        for (int i = 0; i < screens()->count(); i++) {
            if (screens()->isInternal(i)) {
                internalId = i;
                break;
            }
        }
        auto testScreenMatches = [device] (int id) {
            const auto &size = device->size();
            const auto &screenSize = screens()->physicalSize(id);
            return std::round(size.width()) == std::round(screenSize.width())
                && std::round(size.height()) == std::round(screenSize.height());
        };
        if (internalId != -1 && testScreenMatches(internalId)) {
            id = internalId;
        }
        // let's compare all screens for size
        for (int i = 0; i < screens()->count(); i++) {
            if (testScreenMatches(i)) {
                id = i;
                break;
            }
        }
        if (id == -1) {
            // still not found
            if (internalId != -1) {
                // we have an internal id, so let's use that
                id = internalId;
            } else {
                // just take first screen, we have no clue
                id = 0;
            }
        }
    }
    device->setScreenId(id);
    device->setOrientation(screens()->orientation(id));
#else
    Q_UNUSED(device)
#endif
}

bool Connection::isSuspended() const
{
    if (!s_context) {
        return false;
    }
    return s_context->isSuspended();
}

void Connection::applyDeviceConfig(Device *device)
{
    // pass configuration to Device
    device->setConfig(m_config->group("Libinput").group(QString::number(device->vendor())).group(QString::number(device->product())).group(device->name()));
    device->loadConfiguration();
}

void Connection::slotKGlobalSettingsNotifyChange(int type, int arg)
{
    if (type == 3 /**SettingsChanged**/ && arg == 0 /** SETTINGS_MOUSE **/) {
        m_config->reparseConfiguration();
        for (auto it = m_devices.constBegin(), end = m_devices.constEnd(); it != end; ++it) {
            if ((*it)->isPointer()) {
                applyDeviceConfig(*it);
            }
        }
    }
}

void Connection::toggleTouchpads()
{
    bool changed = false;
    m_touchpadsEnabled = !m_touchpadsEnabled;
    for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it) {
        auto device = *it;
        if (!device->isTouchpad()) {
            continue;
        }
        const bool old = device->isEnabled();
        device->setEnabled(m_touchpadsEnabled);
        if (old != device->isEnabled()) {
            changed = true;
        }
    }
    if (changed) {
        // send OSD message
        QDBusMessage msg = QDBusMessage::createMethodCall(
            QStringLiteral("org.kde.plasmashell"),
            QStringLiteral("/org/kde/osdService"),
            QStringLiteral("org.kde.osdService"),
            QStringLiteral("touchpadEnabledChanged")
        );
        msg.setArguments({m_touchpadsEnabled});
        QDBusConnection::sessionBus().asyncCall(msg);
    }
}

void Connection::enableTouchpads()
{
    if (m_touchpadsEnabled) {
        return;
    }
    toggleTouchpads();
}

void Connection::disableTouchpads()
{
    if (!m_touchpadsEnabled) {
        return;
    }
    toggleTouchpads();
}

void Connection::updateLEDs(Xkb::LEDs leds)
{
    if (m_leds == leds) {
        return;
    }
    m_leds = leds;
    // update on devices
    const libinput_led l = static_cast<libinput_led>(toLibinputLEDS(leds));
    for (auto it = m_devices.constBegin(), end = m_devices.constEnd(); it != end; ++it) {
        libinput_device_led_update((*it)->device(), l);
    }
}

QStringList Connection::devicesSysNames() const {
    QStringList sl;
    foreach (Device *d, m_devices) {
        sl.append(d->sysName());
    }
    return sl;
}

}
}

#include "connection.moc"
