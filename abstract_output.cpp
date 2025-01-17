// Copyright 2014  Martin Gräßlin <mgraesslin@kde.org>
// Copyright 2018 Roman Gilg <subdiff@gmail.com>
// SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "abstract_output.h"
#include "wayland_server.h"
#include "screens.h"

// KWayland
#include <KWayland/Server/display.h>
#include <KWayland/Server/outputchangeset.h>
#include <KWayland/Server/xdgoutput_interface.h>
// KF5
#include <KLocalizedString>

#include <cmath>

#include "colorcorrection/gammaramp.h"

namespace KWin
{

AbstractOutput::AbstractOutput(QObject *parent)
    : QObject(parent)
{
    m_waylandOutput = waylandServer()->display()->createOutput(this);
    m_waylandOutputDevice = waylandServer()->display()->createOutputDevice(this);
    m_xdgOutput = waylandServer()->xdgOutputManager()->createXdgOutput(m_waylandOutput, this);
    connect(m_waylandOutput, &KWayland::Server::Global::aboutToDestroyGlobal, this,
            [this]() {
            qDebug() << "------ about to destroy output global" << m_waylandOutputDevice->uuid();
        });

    connect(m_waylandOutput, &KWayland::Server::OutputInterface::dpmsModeRequested, this,
        [this] (KWayland::Server::OutputInterface::DpmsMode mode) {
        if (mode == KWayland::Server::OutputInterface::DpmsMode::On) {
            qDebug() << "-------" << "dpmsModeRequested on" << m_waylandOutput;
            QTimer::singleShot(150, this, [=] { updateDpms(mode); });
        } else {
            qDebug() << "-------" << "dpmsModeRequested off" << m_waylandOutput;
            updateDpms(mode);
        }
        }, Qt::DirectConnection
    );
}

AbstractOutput::~AbstractOutput()
{
}

QString AbstractOutput::name() const
{
    if (!m_waylandOutput) {
        return i18n("unknown");
    }
    return QStringLiteral("%1 %2").arg(m_waylandOutput->manufacturer()).arg(m_waylandOutput->model());
}

QRect AbstractOutput::geometry() const
{
    return QRect(globalPos(), pixelSize() / scale());
}

QSize AbstractOutput::physicalSize() const
{
    return orientateSize(m_physicalSize);
}

int AbstractOutput::refreshRate() const
{
    if (!m_waylandOutput) {
        return 60000;
    }
    return m_waylandOutput->refreshRate();
}

void AbstractOutput::setGlobalPos(const QPoint &pos)
{
    if (!isEnabled()) return;
    m_waylandOutputDevice->setGlobalPosition(pos);

    m_waylandOutput->setGlobalPosition(pos);
    m_xdgOutput->setLogicalPosition(pos);
    m_xdgOutput->done();
}

void AbstractOutput::setScale(qreal scale)
{
    if (!isEnabled()) return;
    m_waylandOutputDevice->setScaleF(scale);

    // this is the scale that clients will ideally use for their buffers
    // this has to be an int which is fine

    // I don't know whether we want to round or ceil
    // or maybe even set this to 3 when we're scaling to 1.5
    // don't treat this like it's chosen deliberately
    m_waylandOutput->setScale(std::ceil(scale));
    m_xdgOutput->setLogicalSize(pixelSize() / scale);
    m_xdgOutput->done();
}
void AbstractOutput::setChanges(KWayland::Server::OutputChangeSet *changes)
{
    qCDebug(KWIN_CORE) << "Set changes in AbstractOutput." << m_waylandOutputDevice->uuid();

    bool updated = false;
    bool overallSizeCheckNeeded = false;

    if (!changes) {
        qCDebug(KWIN_CORE) << "No changes.";
        // No changes to an output is an entirely valid thing
        return;
    }
    //enabledChanged is handled by plugin code
    if (changes->modeChanged()) {
        qCDebug(KWIN_CORE) << "Setting new mode:" << changes->mode();
        m_waylandOutputDevice->setCurrentMode(changes->mode());
        updateMode(changes->mode());
        updated = true;
    }
    if (changes->transformChanged()) {
        qCDebug(KWIN_CORE) << "Server setting transform: " << (int)(changes->transform());
        transform(changes->transform());
        updated = true;
    }
    if (changes->positionChanged()) {
        qCDebug(KWIN_CORE) << "Server setting position: " << changes->position();
        setGlobalPos(changes->position());
        m_positionSet = true;
        // may just work already!
        overallSizeCheckNeeded = true;
    }
    if (changes->scaleChanged()) {
        qCDebug(KWIN_CORE) << "Setting scale:" << changes->scale();
        setScale(changes->scaleF());
        updated = true;
    }
    if (changes->colorCurvesChanged()) {
        qCDebug(KWIN_CORE) << "Receive new colorCurves:" << changes->colorCurves().red << " " << changes->colorCurves().green << " " << changes->colorCurves().blue;
        m_waylandOutputDevice->setColorCurves(changes->colorCurves());
        updateColorCurves(changes->colorCurves());
    }

    overallSizeCheckNeeded |= updated;
    if (overallSizeCheckNeeded) {
        emit screens()->changed();
    }

    if (updated) {
        emit modeChanged();
    }
}

void AbstractOutput::setEnabled(bool enable)
{
    if (enable == isEnabled()) {
        return;
    }

    qDebug() << "-------- " << __func__ << enable << this;
    if (enable) {
        m_waylandOutputDevice->setEnabled(KWayland::Server::OutputDeviceInterface::Enablement::Enabled);
        m_waylandOutput->create();
        updateEnablement(true);
    } else {
        m_waylandOutputDevice->setEnabled(KWayland::Server::OutputDeviceInterface::Enablement::Disabled);
        m_waylandOutput->destroy();
        updateEnablement(false);
    }
}

void AbstractOutput::setOutputDisconnected()
{
    m_waylandOutput->setOutputDisconnected(true);
}

bool AbstractOutput::hasSetGlobalPosition()
{
    return m_positionSet;
}

const ColorCorrect::GammaRamp* AbstractOutput::getGammaRamp()
{
    return nullptr;
}

void AbstractOutput::setWaylandMode(const QSize &size, int refreshRate)
{
    if (!isEnabled()) return;
    qCDebug(KWIN_CORE) <<  " DrmOutput::pixelSize "<<pixelSize()<<"m_xdgOutput->setLogicalSize "<<(pixelSize() / scale());
    m_waylandOutput->setCurrentMode(size, refreshRate);
    m_xdgOutput->setLogicalSize(pixelSize() / scale());
    m_xdgOutput->done();
}

void AbstractOutput::setOriginalEdid(QByteArray edid)
{
    m_waylandOutputDevice->setEdid(edid);
}

QByteArray AbstractOutput::getUuid()
{
    return m_waylandOutputDevice->uuid();
}

void AbstractOutput::initWaylandOutputDevice(const QString &name,
                                             const QString &model,
                                             const QString &manufacturer,
                                             const QByteArray &uuid,
                                             const QVector<KWayland::Server::OutputDeviceInterface::Mode> &modes)
{
    qDebug() << "-------" << __func__ << model << manufacturer << uuid;
    m_waylandOutputDevice->setUuid(uuid);

    if (!manufacturer.isEmpty()) {
        m_waylandOutputDevice->setManufacturer(manufacturer);
    } else {
        m_waylandOutputDevice->setManufacturer(i18n("unknown"));
    }

    m_waylandOutputDevice->setModel(model);
    m_waylandOutputDevice->setPhysicalSize(m_physicalSize);
    /*
     *  add base wayland output data
     */
    m_waylandOutput->setManufacturer(m_waylandOutputDevice->manufacturer());
    m_waylandOutput->setModel(m_waylandOutputDevice->model());
    m_waylandOutput->setPhysicalSize(rawPhysicalSize());

    const ColorCorrect::GammaRamp* gamma = getGammaRamp();
    if (gamma) {
        KWayland::Server::OutputDeviceInterface::ColorCurves color;
        for (unsigned int i = 0; i < gamma->size; i++) {
            color.red.push_back(gamma->red[i]);
            color.green.push_back(gamma->green[i]);
            color.blue.push_back(gamma->blue[i]);
        }
        m_waylandOutputDevice->setColorCurves(color);
    }

    int i = 0;
    for (auto mode : modes) {
        QString flags_str;
        KWayland::Server::OutputInterface::ModeFlags flags;

        if (mode.flags & KWayland::Server::OutputDeviceInterface::ModeFlag::Preferred) {
            flags_str += " preferred";
            flags |= KWayland::Server::OutputInterface::ModeFlag::Preferred;
        }
        if (mode.flags & KWayland::Server::OutputDeviceInterface::ModeFlag::Current) {
            flags_str += " current";
            flags |= KWayland::Server::OutputInterface::ModeFlag::Current;
        }
        qCDebug(KWIN_CORE).nospace() << "Adding mode " << ++i << ": " << mode.size
            << " [" << mode.refreshRate << "]" << flags_str;

        m_waylandOutputDevice->addMode(mode);
        m_waylandOutput->addMode(mode.size, flags, mode.refreshRate);
    }
    m_waylandOutputDevice->create();

    m_waylandOutput->create();
    m_xdgOutput->setLogicalSize(pixelSize() / scale());
    m_xdgOutput->setName(name);
    m_xdgOutput->setDescription(manufacturer + ' ' + model);
    m_xdgOutput->done();
}

QSize AbstractOutput::orientateSize(const QSize &size) const
{
    if (m_orientation == Qt::PortraitOrientation || m_orientation == Qt::InvertedPortraitOrientation) {
        return size.transposed();
    }
    return size;
}

AbstractOutput::Transform AbstractOutput::transformWayland() const
{
    return static_cast<Transform>(m_waylandOutputDevice->transform());
}

QMatrix4x4 AbstractOutput::logicalToNativeMatrix(const QRect &rect, qreal scale, Transform transform)
{
    QMatrix4x4 matrix;
    matrix.scale(scale);

    switch (transform) {
    case Transform::Normal:
    case Transform::Flipped:
        break;
    case Transform::Rotated90:
    case Transform::Flipped90:
        matrix.translate(0, rect.width());
        matrix.rotate(-90, 0, 0, 1);
        break;
    case Transform::Rotated180:
    case Transform::Flipped180:
        matrix.translate(rect.width(), rect.height());
        matrix.rotate(-180, 0, 0, 1);
        break;
    case Transform::Rotated270:
    case Transform::Flipped270:
        matrix.translate(rect.height(), 0);
        matrix.rotate(-270, 0, 0, 1);
        break;
    }

    switch (transform) {
    case Transform::Flipped:
    case Transform::Flipped90:
    case Transform::Flipped180:
    case Transform::Flipped270:
        matrix.translate(rect.width(), 0);
        matrix.scale(-1, 1);
        break;
    default:
        break;
    }

    matrix.translate(-rect.x(), -rect.y());

    return matrix;
}

}
