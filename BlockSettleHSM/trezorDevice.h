/*

***********************************************************************************
* Copyright (C) 2016 - , BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/
#ifndef TREZORDEVICE_H
#define TREZORDEVICE_H

#include "trezorStructure.h"
#include <QObject>
#include <QNetworkReply>
#include <QPointer>

// Trezor interface (source - https://github.com/trezor/trezor-common/tree/master/protob)
#include "trezor/generated_proto/messages-management.pb.h"
#include "trezor/generated_proto/messages-common.pb.h"
#include "trezor/generated_proto/messages-bitcoin.pb.h"
#include "trezor/generated_proto/messages.pb.h"


class ConnectionManager;
class QNetworkRequest;
class TrezorClient;

class TrezorDevice : public QObject
{
   Q_OBJECT

public:
   TrezorDevice(const std::shared_ptr<ConnectionManager>& connectionManager_, QPointer<TrezorClient> client, QObject* parent = nullptr);
   ~TrezorDevice() override;

   DeviceKey deviceKey() const;

   void init(AsyncCallBack&& cb = nullptr);
   void getPublicKey(AsyncCallBack&& cb = nullptr);
   void setMatrixPin(const std::string& pin);
   void cancel();

signals:
   void publicKeyReady();
   void requestPinMatrix();
   void requestHSMPass();

private:
   void makeCall(const google::protobuf::Message &msg);

   void handleMessage(const MessageData& data);
   bool parseResponse(google::protobuf::Message &msg, const MessageData& data);

private:
   std::shared_ptr<ConnectionManager> connectionManager_{};
   QPointer<TrezorClient> client_{};
   hw::trezor::messages::management::Features features_{};

   std::unordered_map<int, AsyncCallBack> awaitingCallback_;

};

#endif // TREZORDEVICE_H
