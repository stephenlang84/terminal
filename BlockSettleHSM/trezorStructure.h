/*

***********************************************************************************
* Copyright (C) 2016 - , BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/
#ifndef TREZORSTRUCTURES_H
#define TREZORSTRUCTURES_H

#include <functional>
#include <QByteArray>
#include <QString>

using AsyncCallBack = std::function<void()>;
using AsyncCallBackCall = std::function<void(QByteArray&&)>;

struct DeviceData {
   QByteArray path_ = {};
   QByteArray vendor_ = {};
   QByteArray product_ = {};
   QByteArray sessionId_ = {};
   QByteArray debug_ = {};
   QByteArray debugSession_ = {};
};

enum class State {
   None = 0,
   Init,
   Enumerated,
   Acquired,
   Released
};

struct DeviceKey{
   QString deviceLabel_;
   QString deviceId_;
   QString vendor_;
};

struct MessageData
{
   int msg_type_ = -1;
   int length_ = -1;
   std::string message_;
};



#endif // TREZORSTRUCTURES_H
