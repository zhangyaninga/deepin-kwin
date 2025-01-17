// Copyright (C) 2013, 2016 Martin Gräßlin <mgraesslin@kde.org>
// Copyright 2014  Martin Gräßlin <mgraesslin@kde.org>
// SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "keyboard_input.h"
#include "input_event.h"
#include "input_event_spy.h"
#include "keyboard_layout.h"
#include "keyboard_repeat.h"
#include "abstract_client.h"
#include "modifier_only_shortcuts.h"
#include "utils.h"
#include "screenlockerwatcher.h"
#include "toplevel.h"
#include "wayland_server.h"
#include "workspace.h"
// KWayland
#include <KWayland/Server/datadevice_interface.h>
#include <KWayland/Server/seat_interface.h>
//screenlocker
#include <KScreenLocker/KsldApp>
// Frameworks
#include <KGlobalAccel>
// Qt
#include <QKeyEvent>

namespace KWin
{

KeyboardInputRedirection::KeyboardInputRedirection(InputRedirection *parent)
    : QObject(parent)
    , m_input(parent)
    , m_xkb(new Xkb(parent))
{
    connect(m_xkb.data(), &Xkb::ledsChanged, this, &KeyboardInputRedirection::ledsChanged);
    if (waylandServer()) {
        m_xkb->setSeat(waylandServer()->seat());
    }
}

KeyboardInputRedirection::~KeyboardInputRedirection() = default;

class KeyStateChangedSpy : public InputEventSpy
{
public:
    KeyStateChangedSpy(InputRedirection *input)
        : m_input(input)
    {
    }

    void keyEvent(KeyEvent *event) override
    {
        if (event->isAutoRepeat()) {
            return;
        }
        emit m_input->keyStateChanged(event->nativeScanCode(), event->type() == QEvent::KeyPress ? InputRedirection::KeyboardKeyPressed : InputRedirection::KeyboardKeyReleased);
    }

private:
    InputRedirection *m_input;
};

class ModifiersChangedSpy : public InputEventSpy
{
public:
    ModifiersChangedSpy(InputRedirection *input)
        : m_input(input)
        , m_modifiers()
    {
    }

    void keyEvent(KeyEvent *event) override
    {
        if (event->isAutoRepeat()) {
            return;
        }
        updateModifiers(event->modifiers());
    }

    void updateModifiers(Qt::KeyboardModifiers mods)
    {
        if (mods == m_modifiers) {
            return;
        }
        emit m_input->keyboardModifiersChanged(mods, m_modifiers);
        m_modifiers = mods;
    }

private:
    InputRedirection *m_input;
    Qt::KeyboardModifiers m_modifiers;
};

void KeyboardInputRedirection::init()
{
    Q_ASSERT(!m_inited);
    m_inited = true;
    const auto config = kwinApp()->kxkbConfig();
    m_xkb->setNumLockConfig(kwinApp()->inputConfig());
    m_xkb->setConfig(config);

    m_input->installInputEventSpy(new KeyStateChangedSpy(m_input));
    m_modifiersChangedSpy = new ModifiersChangedSpy(m_input);
    m_input->installInputEventSpy(m_modifiersChangedSpy);
    m_keyboardLayout = new KeyboardLayout(m_xkb.data());
    m_keyboardLayout->setConfig(config);
    m_keyboardLayout->init();
    m_input->installInputEventSpy(m_keyboardLayout);

    if (waylandServer()->hasGlobalShortcutSupport()) {
        m_input->installInputEventSpy(new ModifierOnlyShortcuts);
    }

    KeyboardRepeat *keyRepeatSpy = new KeyboardRepeat(m_xkb.data());
    connect(keyRepeatSpy, &KeyboardRepeat::keyRepeat, this,
        std::bind(&KeyboardInputRedirection::processKey, this, std::placeholders::_1, InputRedirection::KeyboardKeyAutoRepeat, std::placeholders::_2, nullptr));
    //workaround: disable repeat handling, since clients will do it again.
    //restore keyboard repeat spy for global shortcut.
    m_input->installInputEventSpy(keyRepeatSpy);

    connect(workspace(), &QObject::destroyed, this, [this] { m_inited = false; });
    connect(waylandServer(), &QObject::destroyed, this, [this] { m_inited = false; });
    connect(workspace(), &Workspace::clientActivated, this,
        [this] {
            disconnect(m_activeClientSurfaceChangedConnection);
            if (auto c = workspace()->activeClient()) {
                m_activeClientSurfaceChangedConnection = connect(c, &Toplevel::surfaceChanged, this, &KeyboardInputRedirection::update);
            } else {
                m_activeClientSurfaceChangedConnection = QMetaObject::Connection();
            }
            update();
        }
    );
    if (waylandServer()->hasScreenLockerIntegration()) {
        connect(ScreenLocker::KSldApp::self(), &ScreenLocker::KSldApp::lockStateChanged, this, &KeyboardInputRedirection::update);
    }
}

void KeyboardInputRedirection::update()
{
    if (!m_inited) {
        return;
    }
    auto seat = waylandServer()->seat();
    // TODO: this needs better integration
    Toplevel *found = nullptr;
    if (waylandServer()->isScreenLocked()) {
        const ToplevelList &stacking = Workspace::self()->stackingOrder();
        if (!stacking.isEmpty()) {
            auto it = stacking.end();
            do {
                --it;
                Toplevel *t = (*it);
                if (t->isDeleted()) {
                    // a deleted window doesn't get mouse events
                    continue;
                }
                if (!t->isLockScreen()) {
                    continue;
                }
                if (!t->readyForPainting()) {
                    continue;
                }
                found = t;
                break;
            } while (it != stacking.begin());
        }
    } else if (!input()->isSelectingWindow()) {
        found = workspace()->activeClient();
    }
    if (found && found->surface()) {
        if (found->surface() != seat->focusedKeyboardSurface()) {
            seat->setFocusedKeyboardSurface(found->surface());
        }
    } else {
        //NOTE(sonald): clear focus will make qt app lost input, this may be a bug of Qt itself
        //need to fix it later
        //seat->setFocusedKeyboardSurface(nullptr);
    }
}

bool KeyboardInputRedirection::isTopScreen() {
    StackingUpdatesBlocker blocker(Workspace::self());
    const ToplevelList &stacking = Workspace::self()->stackingOrder();
    if (!stacking.isEmpty()) {
        auto it = stacking.end();
        do {
            --it;
            Toplevel *t = (*it);
            if (t->isOverride() || t->isOnScreenDisplay()) {
                return true;
            }
        } while (it != stacking.begin());
    }
    return false;
}

void KeyboardInputRedirection::processKey(uint32_t key, InputRedirection::KeyboardKeyState state, uint32_t time, LibInput::Device *device)
{
    QEvent::Type type;
    bool autoRepeat = false;
    switch (state) {
    case InputRedirection::KeyboardKeyAutoRepeat:
        autoRepeat = true;
        // fall through
    case InputRedirection::KeyboardKeyPressed:
        type = QEvent::KeyPress;
        break;
    case InputRedirection::KeyboardKeyReleased:
        type = QEvent::KeyRelease;
        break;
    default:
        Q_UNREACHABLE();
    }

    m_xkb->updateKey(key, state);

    const xkb_keysym_t keySym = m_xkb->currentKeysym();

    KeyEvent event(type,
                   m_xkb->toQtKey(keySym),
                   m_xkb->modifiers(),
                   key,
                   keySym,
                   m_xkb->toString(keySym),
                   autoRepeat,
                   time,
                   device);
    event.setModifiersRelevantForGlobalShortcuts(m_xkb->modifiersRelevantForGlobalShortcuts());

    // crtl+alt+f1 will switch to command-line access from GUI
    // we want still show GUI instead of switch to command-line
    // so we need to skip crtl+alt+f1 action
    if (m_xkb->enableCrtlAltShortcuts()) {
        // first we need to find crtl+alt modifiers
        // then skip f1(59)
        if (59 == key) {
            return;
        }
    }

    // vnc使用fakeinput机制来处理虚拟按键
    // 虚拟按键的情况下，可能会因为网络延迟的原因导致release事件收不到，触发repeat机制
    // 从而导致字符自动连续输出，此处屏蔽掉fakeinput机制下的kwin repeat机制
    if (time != 0) {    //该判断解决虚拟按键延时问题
        m_input->processSpies(std::bind(&InputEventSpy::keyEvent, std::placeholders::_1, &event));
    }
    if (!m_inited) {
        return;
    }

    if (m_input->processGrab(std::bind(&InputEventFilter::keyEvent, std::placeholders::_1, &event)) && !isTopScreen()) {
        qDebug()<<"processGrab true.";
        m_xkb->forwardModifiers();
        return;
    }

    m_input->processFilters(std::bind(&InputEventFilter::keyEvent, std::placeholders::_1, &event));

    m_xkb->forwardModifiers();
}

void KeyboardInputRedirection::processModifiers(uint32_t modsDepressed, uint32_t modsLatched, uint32_t modsLocked, uint32_t group)
{
    if (!m_inited) {
        return;
    }
    // TODO: send to proper Client and also send when active Client changes
    m_xkb->updateModifiers(modsDepressed, modsLatched, modsLocked, group);
    m_modifiersChangedSpy->updateModifiers(modifiers());
    m_keyboardLayout->checkLayoutChange();
}

void KeyboardInputRedirection::processKeymapChange(int fd, uint32_t size)
{
    if (!m_inited) {
        return;
    }
    // TODO: should we pass the keymap to our Clients? Or only to the currently active one and update
    m_xkb->installKeymap(fd, size);
    m_keyboardLayout->resetLayout();
}

}
