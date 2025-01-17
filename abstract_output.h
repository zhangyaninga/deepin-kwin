// Copyright 2014  Martin Gräßlin <mgraesslin@kde.org>
// Copyright 2018 Roman Gilg <subdiff@gmail.com>
// SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef KWIN_OUTPUT_H
#define KWIN_OUTPUT_H

#include <utils.h>
#include <kwin_export.h>

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QVector>
#include <QMatrix4x4>

#include <KWayland/Server/output_interface.h>
#include <KWayland/Server/outputdevice_interface.h>

namespace KWayland
{
namespace Server
{
class OutputInterface;
class OutputDeviceInterface;
class OutputChangeSet;
class OutputManagementInterface;
class XdgOutputInterface;
}
}

namespace KWin
{

namespace ColorCorrect {
struct GammaRamp;
}

/**
 * Generic output representation in a Wayland session
 **/
class KWIN_EXPORT AbstractOutput : public QObject
{
    Q_OBJECT
public:
    enum class Transform {
        Normal,
        Rotated90,
        Rotated180,
        Rotated270,
        Flipped,
        Flipped90,
        Flipped180,
        Flipped270
    };
    explicit AbstractOutput(QObject *parent = nullptr);
    virtual ~AbstractOutput();

    QString name() const;
    bool isEnabled() const {
        return m_waylandOutputDevice->enabled() == KWayland::Server::OutputDeviceInterface::Enablement::Enabled;
    }

    virtual QSize pixelSize() const = 0;
    virtual QSize modeSize() const = 0;
    qreal scale() const {
        return m_waylandOutputDevice->scaleF();
    }
    /*
     * The geometry of this output in global compositor co-ordinates (i.e scaled)
     */
    QRect geometry() const;
    QSize physicalSize() const;
    Qt::ScreenOrientation orientation() const {
        return m_orientation;
    }

    /**
     * Returns the orientation of this output.
     *
     * - Flipped along the vertical axis is landscape + inv. portrait.
     * - Rotated 90° and flipped along the horizontal axis is portrait + inv. landscape
     * - Rotated 180° and flipped along the vertical axis is inv. landscape + inv. portrait
     * - Rotated 270° and flipped along the horizontal axis is inv. portrait + inv. landscape +
     *   portrait
     */
    Transform transformWayland() const;

    /*
     * Current refresh rate in 1/ms
     */
    int refreshRate() const;

    bool isInternal() const {
        return m_internal;
    }

    void setGlobalPos(const QPoint &pos);
    void setScale(qreal scale);

    /**
     * This sets the changes and tests them against the specific output
     */
    void setChanges(KWayland::Server::OutputChangeSet *changeset);

    KWayland::Server::OutputInterface* waylandOutput() const {
        return m_waylandOutput;
    }

    /**
     * Enable or disable the output.
     * This differs from updateDpms as it also
     * removes the wl_output.
     * The default is on.
     */
    void setEnabled(bool enable);
    void setOutputDisconnected();
    bool hasSetGlobalPosition();

    virtual int getGammaRampSize() const {
        return 0;
    }
    virtual bool setGammaRamp(const ColorCorrect::GammaRamp &gamma) {
        Q_UNUSED(gamma);
        return false;
    }
    virtual const ColorCorrect::GammaRamp* getGammaRamp();

    virtual void updateEnablement(bool enable) {
        Q_UNUSED(enable);
    }

    QByteArray getUuid();

    void setOriginalEdid(QByteArray edid);

    /**
     * Returns a matrix that can translate into the display's coordinates system
     */
    static QMatrix4x4 logicalToNativeMatrix(const QRect &rect, qreal scale, Transform transform);

Q_SIGNALS:
    void modeChanged();
    void outputChange(const QRegion &damagedRegion);

protected:
    void initWaylandOutputDevice(const QString &name,
                                 const QString &model,
                                 const QString &manufacturer,
                                 const QByteArray &uuid,
                                 const QVector<KWayland::Server::OutputDeviceInterface::Mode> &modes);

    KWayland::Server::XdgOutputInterface* xdgOutput() const {
        return m_xdgOutput;
    }

    KWayland::Server::OutputDeviceInterface* waylandOutputDevice() const {
        return m_waylandOutputDevice;
    }

    QPoint globalPos() const {
        return m_waylandOutputDevice->globalPosition();
    }

    QSize rawPhysicalSize() const {
        return m_physicalSize;
    }
    void setRawPhysicalSize(const QSize &set) {
        m_physicalSize = set;
    }

    void setOrientation(Qt::ScreenOrientation set) {
        m_orientation = set;
    }
    bool internal() const {
        return m_internal;
    }
    void setInternal(bool set) {
        m_internal = set;
    }
    void setDpmsSupported(bool set) {
        m_waylandOutput->setDpmsSupported(set);
    }
    virtual void updateDpms(KWayland::Server::OutputInterface::DpmsMode mode) {
        Q_UNUSED(mode);
    }
    virtual void updateMode(int modeIndex) {
        Q_UNUSED(modeIndex);
    }
    virtual void transform(KWayland::Server::OutputDeviceInterface::Transform transform) {
        Q_UNUSED(transform);
    }
    virtual void updateColorCurves(KWayland::Server::OutputDeviceInterface::ColorCurves colorCurves) {
        Q_UNUSED(colorCurves);
    }

    void setWaylandMode(const QSize &size, int refreshRate);

    QSize orientateSize(const QSize &size) const;

private:
    KWayland::Server::OutputInterface* m_waylandOutput;
    KWayland::Server::XdgOutputInterface* m_xdgOutput;
    KWayland::Server::OutputDeviceInterface* m_waylandOutputDevice;

    KWayland::Server::OutputInterface::DpmsMode m_dpms = KWayland::Server::OutputInterface::DpmsMode::On;

    QSize m_physicalSize;
    Qt::ScreenOrientation m_orientation = Qt::PrimaryOrientation;
    bool m_internal = false;
    bool m_positionSet = false;
};

}

#endif // KWIN_OUTPUT_H
