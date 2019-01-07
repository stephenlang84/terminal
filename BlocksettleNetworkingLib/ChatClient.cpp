#include "ChatClient.h"
#include "ChatProtocol.h"

#include <spdlog/spdlog.h>
#include "botan/base64.h"

#include "ZmqSecuredDataConnection.h"
#include "ConnectionManager.h"
#include "ApplicationSettings.h"
#include "EncryptUtils.h"

#include <QDateTime>


ChatClient::ChatClient(const std::shared_ptr<ConnectionManager>& connectionManager
                  , const std::shared_ptr<ApplicationSettings> &appSettings
                  , const std::shared_ptr<spdlog::logger>& logger)

   : connectionManager_(connectionManager)
   , appSettings_(appSettings)
   , logger_(logger)
{
   heartbeatTimer_.setInterval(30 * 1000);
   heartbeatTimer_.setSingleShot(false);
   connect(&heartbeatTimer_, &QTimer::timeout, this, &ChatClient::sendHeartbeat);
   heartbeatTimer_.start();
}

ChatClient::~ChatClient()
{
   if (loggedIn_) {
      logout();
   }
}

std::string ChatClient::loginToServer(const std::string& email, const std::string& jwt)
{
   if (connection_) {
      logger_->error("[ChatClient::loginToServer] connecting with not purged connection");
      return std::string();
   }

   auto bytesHash = autheid::getSHA256(email.c_str(), email.size());
   currentUserId_ = QString::fromStdString(autheid::base64Encode(bytesHash).substr(0, 8)).toLower().toStdString();

   connection_ = connectionManager_->CreateSecuredDataConnection();
   BinaryData inSrvPubKey(appSettings_->get<std::string>(ApplicationSettings::chatServerPubKey));
   connection_->SetServerPublicKey(inSrvPubKey);
   if (!connection_->openConnection(appSettings_->get<std::string>(ApplicationSettings::chatServerHost)
                            , appSettings_->get<std::string>(ApplicationSettings::chatServerPort), this))
   {
      logger_->error("[ChatClient::loginToServer] failed to open ZMQ data connection");
      connection_.reset();
   }

   auto loginRequest = std::make_shared<Chat::LoginRequest>("", currentUserId_, jwt);
   sendRequest(loginRequest);

   return currentUserId_;
}

void ChatClient::OnLoginReturned(const Chat::LoginResponse &response)
{
   if (response.getStatus() == Chat::LoginResponse::Status::LoginOk) {
      loggedIn_ = true;
      auto request = std::make_shared<Chat::MessagesRequest>("", currentUserId_, currentUserId_);
      sendRequest(request);
   }
   else {
      loggedIn_ = false;
      emit LoginFailed();
   }
}

void ChatClient::logout()
{
   loggedIn_ = false;

   if (!connection_) {
      logger_->error("[ChatClient::logout] Disconnected already.");
      return;
   }

   auto request = std::make_shared<Chat::LogoutRequest>("", currentUserId_, "");
   sendRequest(request);

   currentUserId_.clear();
   connection_.reset();
}

void ChatClient::sendRequest(const std::shared_ptr<Chat::Request>& request)
{
   auto requestData = request->getData();

   logger_->debug("[ChatClient::sendRequest] \"{}\"", requestData.c_str());

   if (!connection_->isActive())
   {
      logger_->error("Connection is not alive!");
   }

   connection_->send(requestData);
}

void ChatClient::sendHeartbeat()
{
   if (loggedIn_ && connection_->isActive()) {
      sendRequest(std::make_shared<Chat::HeartbeatPingRequest>(currentUserId_));
   }
}

void ChatClient::OnHeartbeatPong(const Chat::HeartbeatPongResponse &response)
{
   logger_->debug("[ChatClient::OnHeartbeatPong] {}", response.getData());
}

void ChatClient::OnUsersList(const Chat::UsersListResponse &response)
{
   logger_->debug("Received users list from server: {}", response.getData());
   switch (response.command()) {
   case Chat::UsersListResponse::Command::Replace:
      emit UsersReplace(response.getDataList());
      break;
   case Chat::UsersListResponse::Command::Add:
      emit UsersAdd(response.getDataList());
      break;
   case Chat::UsersListResponse::Command::Delete:
      emit UsersDel(response.getDataList());
      break;
   }
}

void ChatClient::OnMessages(const Chat::MessagesResponse &response)
{
   logger_->debug("Received messages from server: {}", response.getData());

   emit MessagesUpdate(response.getDataList());
}

void ChatClient::OnDataReceived(const std::string& data)
{
   logger_->debug("[ChatClient::OnDataReceived] {}", data);

   auto response = Chat::Response::fromJSON(data);
   response->handle(*this);
}

void ChatClient::OnConnected()
{
   logger_->debug("[ChatClient::OnConnected]");
}

void ChatClient::OnDisconnected()
{
   logger_->debug("[ChatClient::OnDisconnected]");
}

void ChatClient::OnError(DataConnectionError errorCode)
{
   logger_->debug("[ChatClient::OnError] {}", errorCode);
}

void ChatClient::onSendMessage(const QString &message, const QString &receiver)
{
   logger_->debug("[ChatClient::sendMessage] {}", message.toStdString());

   Chat::MessageData msg(QString::fromStdString(currentUserId_), receiver
                    , QDateTime::currentDateTimeUtc(), message);

   auto request = std::make_shared<Chat::SendMessageRequest>("", msg.toJsonString());
   sendRequest(request);
}
