/****************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** This file is part of the QtGui module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial Usage
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights.  These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
** If you have questions regarding the use of this file, please contact
** Nokia at qt-info@nokia.com.
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef QCHANNELSOCKET_P_H
#define QCHANNELSOCKET_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <QtNetwork/qabstractsocket.h>
#include <QtCore/qiodevice.h>
#include <QtCore/qlist.h>
#include <QtCore/qshareddata.h>

#include <sstream>

#include "nbb.h"

extern "C" {
#include <sys/types.h>
};

QT_BEGIN_NAMESPACE

//class QChannelSocketPrivate;


class Q_GUI_EXPORT QChannelSocket : public QIODevice
{
    Q_OBJECT
public:
    enum SocketState {
        UnconnectedState,
        HostLookupState,
        ConnectingState,
        ConnectedState,
        BoundState,
        ListeningState,
        ClosingState
    };
    bool isSequential() const;

    QChannelSocket(QObject * = 0);
    virtual ~QChannelSocket();

    qint64 bytesAvailable() const;
    qint64 bytesToWrite() const;

    void emitReadyRead();

    /*
    qint64 write(const char * data, qint64 maxSize);
    qint64 write(const QByteArray & byteArray);
    */
    
    //qint64 read(char * data, qint64 maxSize);
    QByteArray read(qint64 maxSize);

    // Required for proper QAbstractSocket casting
    int socketDescriptor();
    virtual bool setSocketDescriptor(int socketDescriptor, QAbstractSocket::SocketState socketState = QAbstractSocket::ConnectedState, QAbstractSocket::OpenMode openMode = ReadWrite);
    QAbstractSocket::SocketState state();
    bool flush();

protected:
    qint64 readData(char * data, qint64 maxSize);
    qint64 writeData (const char * data, qint64 maxSize);
    // Unique socket name for use with NBB
    const char *getSocketName(void);

private:
    QChannelSocket(const QChannelSocket &);
    QChannelSocket & operator=(const QChannelSocket &);
    int slotNumber;
    QAbstractSocket::SocketState sockState;
    std::stringstream socketName;

Q_SIGNALS:
    // TODO: make readyRead actually work properly. Necessary.
    // This signal is emitted once every time new data is available for reading from the device. It will only be emitted again once new data is available, such as when a new payload of network data has arrived on your network socket, or when a new block of data has been appended to your device.
    void readyRead();
    // This one is easy, and implemented
    void bytesWritten(qint64 bytes);
    // These other two are more optional
    void disconnected();
    void error(QAbstractSocket::SocketError);

    //QChannelSocketPrivate * d;
};

QT_END_NAMESPACE

#endif // QCHANNELSOCKET_P_H
