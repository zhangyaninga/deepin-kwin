// Copyright (C) 2015 Martin Gräßlin <mgraesslin@kde.org>
// Copyright 2014  Martin Gräßlin <mgraesslin@kde.org>
// SPDX-FileCopyrightText: 2022 2015 Martin Gräßlin <mgraesslin@kde.org>
// SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "wayland_server.h"
#include "client.h"
#include "platform.h"
#include "composite.h"
#include "idle_inhibition.h"
#include "screens.h"
#include "shell_client.h"
#include "workspace.h"
#include "abstract_output.h"

// Client
#include <KWayland/Client/connection_thread.h>
#include <KWayland/Client/event_queue.h>
#include <KWayland/Client/registry.h>
#include <KWayland/Client/compositor.h>
#include <KWayland/Client/shm_pool.h>
#include <KWayland/Client/surface.h>
#include <KWayland/Client/seat.h>
#include <KWayland/Client/datadevicemanager.h>
// Server
#include <KWayland/Server/appmenu_interface.h>
#include <KWayland/Server/compositor_interface.h>
#include <KWayland/Server/datadevicemanager_interface.h>
#include <KWayland/Server/datasource_interface.h>
#include <KWayland/Server/display.h>
#include <KWayland/Server/dpms_interface.h>
#include <KWayland/Server/idle_interface.h>
#include <KWayland/Server/idleinhibit_interface.h>
#include <KWayland/Server/output_interface.h>
#include <KWayland/Server/plasmashell_interface.h>
#include <KWayland/Server/plasmavirtualdesktop_interface.h>
#include <KWayland/Server/plasmawindowmanagement_interface.h>
#include <KWayland/Server/pointerconstraints_interface.h>
#include <KWayland/Server/pointergestures_interface.h>
#include <KWayland/Server/qtsurfaceextension_interface.h>
#include <KWayland/Server/seat_interface.h>
#include <KWayland/Server/server_decoration_interface.h>
#include <KWayland/Server/server_decoration_palette_interface.h>
#include <KWayland/Server/shadow_interface.h>
#include <KWayland/Server/subcompositor_interface.h>
#include <KWayland/Server/blur_interface.h>
#include <KWayland/Server/shell_interface.h>
#include <KWayland/Server/outputmanagement_interface.h>
#include <KWayland/Server/outputconfiguration_interface.h>
#include <KWayland/Server/xdgdecoration_interface.h>
#include <KWayland/Server/xdgshell_interface.h>
#include <KWayland/Server/xdgforeign_interface.h>
#include <KWayland/Server/xdgoutput_interface.h>
#include <KWayland/Server/ddeseat_interface.h>
#include <KWayland/Server/ddeshell_interface.h>
#include <KWayland/Server/strut_interface.h>
#include <KWayland/Server/xwayland_keyboard_grab_v1_interface.h>
#include <KWayland/Server/primaryselectiondevicemanager_v1_interface.h>
#include <KWayland/Server/datacontroldevicemanager_interface.h>

// Qt
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QWindow>

// system
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

//screenlocker
#include <KScreenLocker/KsldApp>
#include "log.h"

using namespace KWayland::Server;

namespace KWin
{

KWIN_SINGLETON_FACTORY(WaylandServer)

WaylandServer::WaylandServer(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<KWayland::Server::OutputInterface::DpmsMode>();

    connect(kwinApp(), &Application::screensCreated, this, &WaylandServer::initOutputs);
}

WaylandServer::~WaylandServer()
{
    destroyInputMethodConnection();
}

void WaylandServer::destroyInternalConnection()
{
    emit terminatingInternalClientConnection();
    if (m_internalConnection.client) {
        // delete all connections hold by plugins like e.g. widget style
        const auto connections = KWayland::Client::ConnectionThread::connections();
        for (auto c : connections) {
            if (c == m_internalConnection.client) {
                continue;
            }
            emit c->connectionDied();
        }

        delete m_internalConnection.registry;
        delete m_internalConnection.compositor;
        delete m_internalConnection.seat;
        delete m_internalConnection.ddm;
        delete m_internalConnection.shm;
        dispatch();
        m_internalConnection.client->deleteLater();
        m_internalConnection.clientThread->quit();
        m_internalConnection.clientThread->wait();
        delete m_internalConnection.clientThread;
        m_internalConnection.client = nullptr;
        m_internalConnection.server->destroy();
        m_internalConnection.server = nullptr;
    }
}

void WaylandServer::terminateClientConnections()
{
    destroyInternalConnection();
    destroyInputMethodConnection();
    if (m_display) {
        const auto connections = m_display->connections();
        for (auto it = connections.begin(); it != connections.end(); ++it) {
            (*it)->destroy();
        }
    }
}

template <class T>
void WaylandServer::createSurface(T *surface)
{
    if (!Workspace::self()) {
        // it's possible that a Surface gets created before Workspace is created
        return;
    }
    if (surface->client() == m_xwayland.client) {
        // skip Xwayland clients, those are created using standard X11 way
        return;
    }
    if (surface->client() == m_screenLockerClientConnection) {
        ScreenLocker::KSldApp::self()->lockScreenShown();
    }
    auto client = new ShellClient(surface);
    if (ServerSideDecorationInterface *deco = ServerSideDecorationInterface::get(surface->surface())) {
        client->installServerSideDecoration(deco);
    }
    auto it = std::find_if(m_plasmaShellSurfaces.begin(), m_plasmaShellSurfaces.end(),
        [client] (PlasmaShellSurfaceInterface *surface) {
            return client->surface() == surface->surface();
        }
    );
    if (it != m_plasmaShellSurfaces.end()) {
        client->installPlasmaShellSurface(*it);
        m_plasmaShellSurfaces.erase(it);
    }
    auto it_ddeShellSurface = std::find_if(m_ddeShellSurfaces.begin(), m_ddeShellSurfaces.end(),
        [client] (DDEShellSurfaceInterface *shellSurface) {
            return client->surface() == shellSurface->surface();
        }
    );
    if (it_ddeShellSurface != m_ddeShellSurfaces.end()) {
        client->installDDEShellSurface(*it_ddeShellSurface);
        m_ddeShellSurfaces.erase(it_ddeShellSurface);
    }
    if (auto menu = m_appMenuManager->appMenuForSurface(surface->surface())) {
        client->installAppMenu(menu);
    }
    if (auto palette = m_paletteManager->paletteForSurface(surface->surface())) {
        client->installPalette(palette);
    }
    if (client->isInternal()) {
        m_internalClients << client;
    } else {
        m_clients << client;
    }
    if (client->readyForPainting()) {
        emit shellClientAdded(client);
    } else {
        connect(client, &ShellClient::windowShown, this, &WaylandServer::shellClientShown);
    }

    //not directly connected as the connection is tied to client instead of this
    connect(m_XdgForeign, &KWayland::Server::XdgForeignInterface::transientChanged, client, [this](KWayland::Server::SurfaceInterface *child) {
        emit foreignTransientChanged(child);
    });
}

bool WaylandServer::init(const QByteArray &socketName, InitalizationFlags flags)
{
    m_initFlags = flags;
    m_display = new KWayland::Server::Display(this);
    if (!socketName.isNull() && !socketName.isEmpty()) {
        m_display->setSocketName(QString::fromUtf8(socketName));
    }
    m_display->start();
    if (!m_display->isRunning()) {
        return false;
    }
    m_compositor = m_display->createCompositor(m_display);
    m_compositor->create();
    connect(m_compositor, &CompositorInterface::surfaceCreated, this,
        [this] (SurfaceInterface *surface) {
            // check whether we have a Toplevel with the Surface's id
            Workspace *ws = Workspace::self();
            if (!ws) {
                // it's possible that a Surface gets created before Workspace is created
                return;
            }
            if (surface->client() != xWaylandConnection()) {
                // setting surface is only relevat for Xwayland clients
                return;
            }
            auto check = [surface] (const Toplevel *t) {
                return t->surfaceId() == surface->id();
            };
            if (Toplevel *t = ws->findToplevel(check)) {
                t->setSurface(surface);
            }
        }
    );
    m_shell = m_display->createShell(m_display);
    m_shell->create();
    connect(m_shell, &ShellInterface::surfaceCreated, this, &WaylandServer::createSurface<ShellSurfaceInterface>);

    m_xdgShell5 = m_display->createXdgShell(XdgShellInterfaceVersion::UnstableV5, m_display);
    m_xdgShell5->create();
    connect(m_xdgShell5, &XdgShellInterface::surfaceCreated, this, &WaylandServer::createSurface<XdgShellSurfaceInterface>);
    // TODO: verify seat and serial
    connect(m_xdgShell5, &XdgShellInterface::popupCreated, this, &WaylandServer::createSurface<XdgShellPopupInterface>);

    m_xdgShell6 = m_display->createXdgShell(XdgShellInterfaceVersion::UnstableV6, m_display);
    m_xdgShell6->create();
    connect(m_xdgShell6, &XdgShellInterface::surfaceCreated, this, &WaylandServer::createSurface<XdgShellSurfaceInterface>);
    connect(m_xdgShell6, &XdgShellInterface::xdgPopupCreated, this, &WaylandServer::createSurface<XdgShellPopupInterface>);

    m_xdgShell = m_display->createXdgShell(XdgShellInterfaceVersion::Stable, m_display);
    m_xdgShell->create();
    connect(m_xdgShell, &XdgShellInterface::surfaceCreated, this, &WaylandServer::createSurface<XdgShellSurfaceInterface>);
    connect(m_xdgShell, &XdgShellInterface::xdgPopupCreated, this, &WaylandServer::createSurface<XdgShellPopupInterface>);

    m_xdgDecorationManager = m_display->createXdgDecorationManager(m_xdgShell, m_display);
    m_xdgDecorationManager->create();
    connect(m_xdgDecorationManager, &XdgDecorationManagerInterface::xdgDecorationInterfaceCreated, this,  [this] (XdgDecorationInterface *deco) {
        if (ShellClient *client = findClient(deco->surface()->surface())) {
            client->installXdgDecoration(deco);
        }
    });

    m_display->createShm();
    m_seat = m_display->createSeat(m_display);
    m_seat->create();
    // qtwayland (qt5.15) need repeateRate > 0 to enable repeat key
    // so here we initialize key repeat_info with experienced values
    m_seat->setKeyRepeatInfo(25, 300);
    m_display->createPointerGestures(PointerGesturesInterfaceVersion::UnstableV1, m_display)->create();
    m_display->createPointerConstraints(PointerConstraintsInterfaceVersion::UnstableV1, m_display)->create();
    m_dataDeviceManager = m_display->createDataDeviceManager(m_display);
    m_dataDeviceManager->create();
    m_idle = m_display->createIdle(m_display);
    m_idle->create();
    auto idleInhibition = new IdleInhibition(m_idle);
    connect(this, &WaylandServer::shellClientAdded, idleInhibition, &IdleInhibition::registerShellClient);
    m_display->createIdleInhibitManager(IdleInhibitManagerInterfaceVersion::UnstableV1, m_display)->create();
    m_plasmaShell = m_display->createPlasmaShell(m_display);
    m_plasmaShell->create();
    connect(m_plasmaShell, &PlasmaShellInterface::surfaceCreated,
        [this] (PlasmaShellSurfaceInterface *surface) {
            if (ShellClient *client = findClient(surface->surface())) {
                client->installPlasmaShellSurface(surface);
            } else {
                m_plasmaShellSurfaces << surface;
                connect(surface, &QObject::destroyed, this,
                    [this, surface] {
                        m_plasmaShellSurfaces.removeOne(surface);
                    }
                );
            }
        }
    );


    m_qtExtendedSurface = m_display->createQtSurfaceExtension(m_display);
    m_qtExtendedSurface->create();
    connect(m_qtExtendedSurface, &QtSurfaceExtensionInterface::surfaceCreated,
        [this] (QtExtendedSurfaceInterface *surface) {
            if (ShellClient *client = findClient(surface->surface())) {
                client->installQtExtendedSurface(surface);
            }
        }
    );
    m_appMenuManager = m_display->createAppMenuManagerInterface(m_display);
    m_appMenuManager->create();
    connect(m_appMenuManager, &AppMenuManagerInterface::appMenuCreated,
        [this] (AppMenuInterface *appMenu) {
            if (ShellClient *client = findClient(appMenu->surface())) {
                client->installAppMenu(appMenu);
            }
        }
    );
    m_paletteManager = m_display->createServerSideDecorationPaletteManager(m_display);
    m_paletteManager->create();
    connect(m_paletteManager, &ServerSideDecorationPaletteManagerInterface::paletteCreated,
        [this] (ServerSideDecorationPaletteInterface *palette) {
            if (ShellClient *client = findClient(palette->surface())) {
                client->installPalette(palette);
            }
        }
    );

    m_windowManagement = m_display->createPlasmaWindowManagement(m_display);
    m_windowManagement->create();
    m_windowManagement->setShowingDesktopState(PlasmaWindowManagementInterface::ShowingDesktopState::Disabled);
    connect(m_windowManagement, &PlasmaWindowManagementInterface::requestChangeShowingDesktop, this,
        [] (PlasmaWindowManagementInterface::ShowingDesktopState state) {
            if (!workspace()) {
                return;
            }
            bool set = false;
            switch (state) {
            case PlasmaWindowManagementInterface::ShowingDesktopState::Disabled:
                set = false;
                break;
            case PlasmaWindowManagementInterface::ShowingDesktopState::Enabled:
                set = true;
                break;
            default:
                Q_UNREACHABLE();
                break;
            }
            if (set == workspace()->showingDesktop()) {
                return;
            }
            workspace()->setShowingDesktop(set);
        }
    );


    m_virtualDesktopManagement = m_display->createPlasmaVirtualDesktopManagement(m_display);
    m_virtualDesktopManagement->create();
    m_windowManagement->setPlasmaVirtualDesktopManagementInterface(m_virtualDesktopManagement);

    auto shadowManager = m_display->createShadowManager(m_display);
    shadowManager->create();

    m_display->createDpmsManager(m_display)->create();

    m_decorationManager = m_display->createServerSideDecorationManager(m_display);
    connect(m_decorationManager, &ServerSideDecorationManagerInterface::decorationCreated, this,
        [this] (ServerSideDecorationInterface *deco) {
            if (ShellClient *c = findClient(deco->surface())) {
                c->installServerSideDecoration(deco);
            }
            connect(deco, &ServerSideDecorationInterface::modeRequested, this,
                [this, deco] (ServerSideDecorationManagerInterface::Mode mode) {
                    // always acknowledge the requested mode
                    deco->setMode(mode);
                }
            );
        }
    );
    m_decorationManager->create();

    m_outputManagement = m_display->createOutputManagement(m_display);
    connect(m_outputManagement, &OutputManagementInterface::configurationChangeRequested,
            this, [this](KWayland::Server::OutputConfigurationInterface *config) {
                kwinApp()->platform()->configurationChangeRequested(config);
    });
    m_outputManagement->create();

    m_xdgOutputManager = m_display->createXdgOutputManager(m_display);
    m_xdgOutputManager->create();

    m_display->createSubCompositor(m_display)->create();

    m_XdgForeign = m_display->createXdgForeignInterface(m_display);
    m_XdgForeign->create();

    m_clientManagement = m_display->createClientManagement(m_display);
    m_clientManagement->create();
    connect(m_clientManagement, &ClientManagementInterface::windowStatesRequest, this,
        [this] () {
            if (!workspace()) {
                qWarning () << "windowStatesRequest before workspace initilized";
                return;
            }
            workspace()->updateWindowStates();
        }
    );
    connect(m_clientManagement, &ClientManagementInterface::captureWindowImageRequest, this,
        [this] (int windowId, wl_resource *buffer) {
            if (!workspace()) {
                qWarning () << __func__ << " workspace not initilized windowId " << windowId;
                return;
            }
            workspace()->captureWindowImage(windowId, buffer);
        }
    );

    m_ddeSeat = m_display->createDDESeat(m_display);
    m_ddeSeat->create();

    m_ddeShell = m_display->createDDEShell(m_display);
    m_ddeShell->create();
    connect(m_ddeShell, &DDEShellInterface::shellSurfaceCreated,
        [this] (DDEShellSurfaceInterface *shellSurface) {
            if (ShellClient *client = findClient(shellSurface->surface())) {
                client->installDDEShellSurface(shellSurface);
            } else {
                m_ddeShellSurfaces << shellSurface;
                connect(shellSurface, &QObject::destroyed, this,
                    [this, shellSurface] {
                        m_ddeShellSurfaces.removeOne(shellSurface);
                    }
                );
            }
        }
    );

    m_strut = m_display->createStrut(m_display);
    m_strut->create();
    connect(m_strut, &StrutInterface::setStrut,
        [this] (SurfaceInterface *surface,struct deepinKwinStrut& strutArea) {
            if (ShellClient *client = findClient(surface)) {
                client->setStrut(strutArea);
                workspace()->updateClientArea();
            } else {
                DLOGC("Client does not exist!!!");
            }
        }
    );

    m_grab = m_display->createZWPXwaylandKeyboardGrabManagerV1(m_display);
    m_grab->create();
    connect(m_grab, &ZWPXwaylandKeyboardGrabManagerV1Interface::zwpXwaylandKeyboardGrabV1Created,
            [this] (ZWPXwaylandKeyboardGrabV1Interface *grab) {
        qDebug() << "grab successfully!";
        m_grabClient = grab;
    });
    connect(m_grab, &ZWPXwaylandKeyboardGrabManagerV1Interface::zwpXwaylandKeyboardGrabV1Destroyed,
            [this] () {
        m_grabClient = nullptr;
        qDebug() << "grab destroyed!";
    });

    auto psdi = m_display->createPrimarySelectionDeviceManagerV1(m_display);
    psdi->create();

    auto dcdm = m_display->createDataControlDeviceManager(m_display);
    dcdm->create();

    return true;
}

SurfaceInterface *WaylandServer::findForeignTransientForSurface(SurfaceInterface *surface)
{
    return m_XdgForeign->transientFor(surface);
}

void WaylandServer::shellClientShown(Toplevel *t)
{
    ShellClient *c = dynamic_cast<ShellClient*>(t);
    if (!c) {
        qCWarning(KWIN_CORE) << "Failed to cast a Toplevel which is supposed to be a ShellClient to ShellClient";
        return;
    }
    disconnect(c, &ShellClient::windowShown, this, &WaylandServer::shellClientShown);
    emit shellClientAdded(c);
    if (c->checkClientAllowToTile()) {
        c->setSplitable(true);
    }
}

void WaylandServer::initWorkspace()
{
    VirtualDesktopManager::self()->setVirtualDesktopManagement(m_virtualDesktopManagement);

    if (m_windowManagement) {
        connect(workspace(), &Workspace::showingDesktopChanged, this,
            [this] (bool set) {
                using namespace KWayland::Server;
                m_windowManagement->setShowingDesktopState(set ?
                    PlasmaWindowManagementInterface::ShowingDesktopState::Enabled :
                    PlasmaWindowManagementInterface::ShowingDesktopState::Disabled
                );
            }
        );
    }

    if (hasScreenLockerIntegration()) {
        if (m_internalConnection.interfacesAnnounced) {
            initScreenLocker();
        } else {
            connect(m_internalConnection.registry, &KWayland::Client::Registry::interfacesAnnounced, this, &WaylandServer::initScreenLocker);
        }
    } else {
        emit initialized();
    }
}

void WaylandServer::initScreenLocker()
{
    ScreenLocker::KSldApp::self();
    ScreenLocker::KSldApp::self()->setWaylandDisplay(m_display);
    ScreenLocker::KSldApp::self()->setGreeterEnvironment(kwinApp()->processStartupEnvironment());
    ScreenLocker::KSldApp::self()->initialize();

    connect(ScreenLocker::KSldApp::self(), &ScreenLocker::KSldApp::greeterClientConnectionChanged, this,
        [this] () {
            m_screenLockerClientConnection = ScreenLocker::KSldApp::self()->greeterClientConnection();
        }
    );

    connect(ScreenLocker::KSldApp::self(), &ScreenLocker::KSldApp::unlocked, this,
        [this] () {
            m_screenLockerClientConnection = nullptr;
        }
    );

    if (m_initFlags.testFlag(InitalizationFlag::LockScreen)) {
        ScreenLocker::KSldApp::self()->lock(ScreenLocker::EstablishLock::Immediate);
    }
    emit initialized();
}

void WaylandServer::initOutputs()
{
    if (kwinApp()->platform()->handlesOutputs()) {
        return;
    }
    syncOutputsToWayland();
    connect(screens(), &Screens::changed, this,
        [this] {
            // when screens change we need to sync this to Wayland.
            // Unfortunately we don't have much information and cannot properly match a KWin screen
            // to a Wayland screen.
            // Thus we just recreate all outputs and delete the old ones
            const auto outputs = m_display->outputs();
            syncOutputsToWayland();
            qDeleteAll(outputs);
        }
    );
}

void WaylandServer::syncOutputsToWayland()
{
    Screens *s = screens();
    Q_ASSERT(s);
    for (int i = 0; i < s->count(); ++i) {
        OutputInterface *output = m_display->createOutput(m_display);
        auto xdgOutput = xdgOutputManager()->createXdgOutput(output, output);

        output->setScale(s->scale(i));
        const QRect &geo = s->geometry(i);
        output->setGlobalPosition(geo.topLeft());
        output->setPhysicalSize(s->physicalSize(i).toSize());
        output->addMode(geo.size());

        xdgOutput->setLogicalPosition(geo.topLeft());
        xdgOutput->setLogicalSize(geo.size());
        xdgOutput->done();

        output->create();
    }
}

WaylandServer::SocketPairConnection WaylandServer::createConnection()
{
    SocketPairConnection ret;
    int sx[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sx) < 0) {
        qCWarning(KWIN_CORE) << "Could not create socket";
        return ret;
    }
    ret.connection = m_display->createClient(sx[0]);
    ret.fd = sx[1];
    return ret;
}

int WaylandServer::createXWaylandConnection()
{
    const auto socket = createConnection();
    if (!socket.connection) {
        return -1;
    }
    m_xwayland.client = socket.connection;
    m_xwayland.destroyConnection = connect(m_xwayland.client, &KWayland::Server::ClientConnection::disconnected, this,
        [] {
            fprintf(stderr, "Xwayland Connection died\n");
        }
    );
    return socket.fd;
}

void WaylandServer::destroyXWaylandConnection()
{
    if (!m_xwayland.client) {
        return;
    }
    // first terminate the clipboard sync
    disconnect(m_xwayland.destroyConnection);
    m_xwayland.client->destroy();
    m_xwayland.client = nullptr;
}

int WaylandServer::createInputMethodConnection()
{
    const auto socket = createConnection();
    if (!socket.connection) {
        return -1;
    }
    m_inputMethodServerConnection = socket.connection;
    return socket.fd;
}

void WaylandServer::destroyInputMethodConnection()
{
    if (!m_inputMethodServerConnection) {
        return;
    }
    m_inputMethodServerConnection->destroy();
    m_inputMethodServerConnection = nullptr;
}

void WaylandServer::createInternalConnection()
{
    const auto socket = createConnection();
    if (!socket.connection) {
        return;
    }
    m_internalConnection.server = socket.connection;
    using namespace KWayland::Client;
    m_internalConnection.client = new ConnectionThread();
    m_internalConnection.client->setSocketFd(socket.fd);
    m_internalConnection.clientThread = new QThread;
    m_internalConnection.client->moveToThread(m_internalConnection.clientThread);
    m_internalConnection.clientThread->start();

    connect(m_internalConnection.client, &ConnectionThread::connected, this,
        [this] {
            Registry *registry = new Registry(this);
            EventQueue *eventQueue = new EventQueue(this);
            eventQueue->setup(m_internalConnection.client);
            registry->setEventQueue(eventQueue);
            registry->create(m_internalConnection.client);
            m_internalConnection.registry = registry;
            connect(registry, &Registry::shmAnnounced, this,
                [this] (quint32 name, quint32 version) {
                    m_internalConnection.shm = m_internalConnection.registry->createShmPool(name, version, this);
                }
            );
            connect(registry, &Registry::interfacesAnnounced, this,
                [this, registry] {
                    m_internalConnection.interfacesAnnounced = true;

                    const auto compInterface = registry->interface(Registry::Interface::Compositor);
                    if (compInterface.name != 0) {
                        m_internalConnection.compositor = registry->createCompositor(compInterface.name, compInterface.version, this);
                    }
                    const auto seatInterface = registry->interface(Registry::Interface::Seat);
                    if (seatInterface.name != 0) {
                        m_internalConnection.seat = registry->createSeat(seatInterface.name, seatInterface.version, this);
                    }
                    const auto ddmInterface = registry->interface(Registry::Interface::DataDeviceManager);
                    if (ddmInterface.name != 0) {
                        m_internalConnection.ddm = registry->createDataDeviceManager(ddmInterface.name, ddmInterface.version, this);
                    }
                }
            );
            registry->setup();
        }
    );
    m_internalConnection.client->initConnection();
}

void WaylandServer::removeClient(ShellClient *c)
{
    m_clients.removeAll(c);
    m_internalClients.removeAll(c);
    emit shellClientRemoved(c);
}

void WaylandServer::dispatch()
{
    if (!m_display) {
        return;
    }
    if (m_internalConnection.server) {
        m_internalConnection.server->flush();
    }
    m_display->dispatchEvents(0);
}

static ShellClient *findClientInList(const QList<ShellClient*> &clients, quint32 id)
{
    auto it = std::find_if(clients.begin(), clients.end(),
        [id] (ShellClient *c) {
            return c->windowId() == id;
        }
    );
    if (it == clients.end()) {
        return nullptr;
    }
    return *it;
}

static ShellClient *findClientInList(const QList<ShellClient*> &clients, KWayland::Server::SurfaceInterface *surface)
{
    auto it = std::find_if(clients.begin(), clients.end(),
        [surface] (ShellClient *c) {
            return c->surface() == surface;
        }
    );
    if (it == clients.end()) {
        return nullptr;
    }
    return *it;
}

ShellClient *WaylandServer::findClient(quint32 id) const
{
    if (id == 0) {
        return nullptr;
    }
    if (ShellClient *c = findClientInList(m_clients, id)) {
        return c;
    }
    if (ShellClient *c = findClientInList(m_internalClients, id)) {
        return c;
    }
    return nullptr;
}

ShellClient *WaylandServer::findClient(SurfaceInterface *surface) const
{
    if (!surface) {
        return nullptr;
    }
    if (ShellClient *c = findClientInList(m_clients, surface)) {
        return c;
    }
    if (ShellClient *c = findClientInList(m_internalClients, surface)) {
        return c;
    }
    return nullptr;
}

AbstractClient *WaylandServer::findAbstractClient(SurfaceInterface *surface) const
{
    return findClient(surface);
}

ShellClient *WaylandServer::findClient(QWindow *w) const
{
    if (!w) {
        return nullptr;
    }
    auto it = std::find_if(m_internalClients.constBegin(), m_internalClients.constEnd(),
        [w] (const ShellClient *c) {
            return c->internalWindow() == w;
        }
    );
    if (it != m_internalClients.constEnd()) {
        return *it;
    }
    return nullptr;
}

quint32 WaylandServer::createWindowId(SurfaceInterface *surface)
{
    auto it = m_clientIds.constFind(surface->client());
    quint16 clientId = 0;
    if (it != m_clientIds.constEnd()) {
        clientId = it.value();
    } else {
        clientId = createClientId(surface->client());
    }
    Q_ASSERT(clientId != 0);
    quint32 id = clientId;
    // TODO: this does not prevent that two surfaces of same client get same id
    id = (id << 16) | (surface->id() & 0xFFFF);
    if (findClient(id)) {
        qCWarning(KWIN_CORE) << "Invalid client windowId generated:" << id;
        return 0;
    }
    return id;
}

quint16 WaylandServer::createClientId(ClientConnection *c)
{
    auto ids = m_clientIds.values().toSet();
    quint16 id = 1;
    if (!ids.isEmpty()) {
        for (quint16 i = ids.count() + 1; i >= 1 ; i--) {
            if (!ids.contains(i)) {
                id = i;
                break;
            }
        }
    }
    Q_ASSERT(!ids.contains(id));
    m_clientIds.insert(c, id);
    connect(c, &ClientConnection::disconnected, this,
        [this] (ClientConnection *c) {
            m_clientIds.remove(c);
        }
    );
    return id;
}

bool WaylandServer::isScreenLocked() const
{
    if (!hasScreenLockerIntegration()) {
        return false;
    }
    return ScreenLocker::KSldApp::self()->lockState() == ScreenLocker::KSldApp::Locked ||
           ScreenLocker::KSldApp::self()->lockState() == ScreenLocker::KSldApp::AcquiringLock;
}

bool WaylandServer::hasScreenLockerIntegration() const
{
    return !m_initFlags.testFlag(InitalizationFlag::NoLockScreenIntegration);
}

bool WaylandServer::hasGlobalShortcutSupport() const
{
    return !m_initFlags.testFlag(InitalizationFlag::NoGlobalShortcuts);
}

void WaylandServer::simulateUserActivity()
{
    if (m_idle) {
        m_idle->simulateUserActivity();
    }
}

AbstractOutput *WaylandServer::findOutput(KWayland::Server::OutputInterface *outputIface) const
{
    AbstractOutput *outputFound = nullptr;
    const auto outputs = kwinApp()->platform()->enabledOutputs();
    for (auto output : outputs) {
        if (static_cast<AbstractOutput *>(output)->waylandOutput() == outputIface) {
            outputFound = static_cast<AbstractOutput *>(output);
        }
    }
    return outputFound;
}

}
