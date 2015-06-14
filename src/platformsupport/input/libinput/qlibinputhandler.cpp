/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the plugins module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qlibinputhandler_p.h"
#include "qlibinputpointer_p.h"
#include "qlibinputkeyboard_p.h"
#include "qlibinputtouch_p.h"

#include <libudev.h>
#include <libinput.h>
#include <QtCore/QLoggingCategory>
#include <QtCore/QSocketNotifier>
#include <QtCore/private/qcore_unix_p.h>
#include <private/qguiapplication_p.h>
#include <private/qinputdevicemanager_p_p.h>

QT_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(qLcInput, "qt.qpa.input")

static int liOpen(const char *path, int flags, void *user_data)
{
    Q_UNUSED(user_data);
    return qt_safe_open(path, flags);
}

static void liClose(int fd, void *user_data)
{
    Q_UNUSED(user_data);
    qt_safe_close(fd);
}

static const struct libinput_interface liInterface = {
    liOpen,
    liClose
};

static void liLogHandler(libinput *libinput, libinput_log_priority priority, const char *format, va_list args)
{
    Q_UNUSED(libinput);
    Q_UNUSED(priority);

    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), format, args);
    if (n > 0) {
        if (buf[n - 1] == '\n')
            buf[n - 1] = '\0';
        qCDebug(qLcInput, "libinput: %s", buf);
    }
}

QLibInputHandler::QLibInputHandler(const QString &key, const QString &spec)
{
    Q_UNUSED(key);
    Q_UNUSED(spec);

    m_udev = udev_new();
    if (!m_udev)
        qFatal("Failed to get udev context for libinput");

    m_li = libinput_udev_create_context(&liInterface, Q_NULLPTR, m_udev);
    if (!m_li)
        qFatal("Failed to get libinput context");

    libinput_log_set_handler(m_li, liLogHandler);
    if (qLcInput().isDebugEnabled())
        libinput_log_set_priority(m_li, LIBINPUT_LOG_PRIORITY_DEBUG);

    if (libinput_udev_assign_seat(m_li, "seat0"))
        qFatal("Failed to assign seat");

    m_liFd = libinput_get_fd(m_li);
    m_notifier.reset(new QSocketNotifier(m_liFd, QSocketNotifier::Read));
    connect(m_notifier.data(), SIGNAL(activated(int)), SLOT(onReadyRead()));

    m_pointer.reset(new QLibInputPointer);
    m_keyboard.reset(new QLibInputKeyboard);
    m_touch.reset(new QLibInputTouch);

    connect(QGuiApplicationPrivate::inputDeviceManager(), SIGNAL(cursorPositionChangeRequested(QPoint)),
            this, SLOT(onCursorPositionChangeRequested(QPoint)));

    // Process the initial burst of DEVICE_ADDED events.
    onReadyRead();
}

QLibInputHandler::~QLibInputHandler()
{
    if (m_li)
        libinput_unref(m_li);

    if (m_udev)
        udev_unref(m_udev);
}

void QLibInputHandler::onReadyRead()
{
    if (libinput_dispatch(m_li)) {
        qWarning("libinput_dispatch failed");
        return;
    }

    libinput_event *ev;
    while ((ev = libinput_get_event(m_li)) != Q_NULLPTR) {
        processEvent(ev);
        libinput_event_destroy(ev);
    }
}

void QLibInputHandler::processEvent(libinput_event *ev)
{
    libinput_event_type type = libinput_event_get_type(ev);
    libinput_device *dev = libinput_event_get_device(ev);

    switch (type) {
    case LIBINPUT_EVENT_DEVICE_ADDED:
    {
        // This is not just for hotplugging, it is also called for each input
        // device libinput reads from on startup. Hence it is suitable for doing
        // touch device registration.
        const char *sysname = libinput_device_get_sysname(dev); // node name without path
        const char *name = libinput_device_get_name(dev);
        emit deviceAdded(QString::fromUtf8(sysname), QString::fromUtf8(name));

        QInputDeviceManagerPrivate *inputManagerPriv = QInputDeviceManagerPrivate::get(
            QGuiApplicationPrivate::inputDeviceManager());
        if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_TOUCH)) {
            m_touch->registerDevice(dev);
            int &count(m_devCount[QInputDeviceManager::DeviceTypeTouch]);
            ++count;
            inputManagerPriv->setDeviceCount(QInputDeviceManager::DeviceTypeTouch, count);
        }
        if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER)) {
            int &count(m_devCount[QInputDeviceManager::DeviceTypePointer]);
            ++count;
            inputManagerPriv->setDeviceCount(QInputDeviceManager::DeviceTypePointer, count);
        }
        if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
            int &count(m_devCount[QInputDeviceManager::DeviceTypeKeyboard]);
            ++count;
            inputManagerPriv->setDeviceCount(QInputDeviceManager::DeviceTypeKeyboard, count);
        }
        break;
    }
    case LIBINPUT_EVENT_DEVICE_REMOVED:
    {
        const char *sysname = libinput_device_get_sysname(dev);
        const char *name = libinput_device_get_name(dev);
        emit deviceRemoved(QString::fromUtf8(sysname), QString::fromUtf8(name));

        QInputDeviceManagerPrivate *inputManagerPriv = QInputDeviceManagerPrivate::get(
            QGuiApplicationPrivate::inputDeviceManager());
        if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_TOUCH)) {
            m_touch->unregisterDevice(dev);
            int &count(m_devCount[QInputDeviceManager::DeviceTypeTouch]);
            --count;
            inputManagerPriv->setDeviceCount(QInputDeviceManager::DeviceTypeTouch, count);
        }
        if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER)) {
            int &count(m_devCount[QInputDeviceManager::DeviceTypePointer]);
            --count;
            inputManagerPriv->setDeviceCount(QInputDeviceManager::DeviceTypePointer, count);
        }
        if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
            int &count(m_devCount[QInputDeviceManager::DeviceTypeKeyboard]);
            --count;
            inputManagerPriv->setDeviceCount(QInputDeviceManager::DeviceTypeKeyboard, count);
        }
        break;
    }
    case LIBINPUT_EVENT_POINTER_BUTTON:
        m_pointer->processButton(libinput_event_get_pointer_event(ev));
        break;
    case LIBINPUT_EVENT_POINTER_MOTION:
        m_pointer->processMotion(libinput_event_get_pointer_event(ev));
        break;
    case LIBINPUT_EVENT_POINTER_AXIS:
        m_pointer->processAxis(libinput_event_get_pointer_event(ev));
        break;
    case LIBINPUT_EVENT_KEYBOARD_KEY:
        m_keyboard->processKey(libinput_event_get_keyboard_event(ev));
        break;
    case LIBINPUT_EVENT_TOUCH_DOWN:
        m_touch->processTouchDown(libinput_event_get_touch_event(ev));
        break;
    case LIBINPUT_EVENT_TOUCH_MOTION:
        m_touch->processTouchMotion(libinput_event_get_touch_event(ev));
        break;
    case LIBINPUT_EVENT_TOUCH_UP:
        m_touch->processTouchUp(libinput_event_get_touch_event(ev));
        break;
    case LIBINPUT_EVENT_TOUCH_CANCEL:
        m_touch->processTouchCancel(libinput_event_get_touch_event(ev));
        break;
    case LIBINPUT_EVENT_TOUCH_FRAME:
        m_touch->processTouchFrame(libinput_event_get_touch_event(ev));
        break;
    default:
        break;
    }
}

void QLibInputHandler::onCursorPositionChangeRequested(const QPoint &pos)
{
    m_pointer->setPos(pos);
}

QT_END_NAMESPACE
