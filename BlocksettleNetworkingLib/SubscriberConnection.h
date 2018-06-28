#ifndef __SUBSCRIBER_CONNECTION_H__
#define __SUBSCRIBER_CONNECTION_H__

#include "ZmqContext.h"

#include <atomic>
#include <thread>

class SubscriberConnectionListener
{
public:
   SubscriberConnectionListener() = default;
   virtual ~SubscriberConnectionListener() noexcept = default;

   SubscriberConnectionListener(const SubscriberConnectionListener&) = delete;
   SubscriberConnectionListener& operator = (const SubscriberConnectionListener&) = delete;

   SubscriberConnectionListener(SubscriberConnectionListener&&) = delete;
   SubscriberConnectionListener& operator = (SubscriberConnectionListener&&) = delete;

   virtual void OnDataReceived(const std::string& data) = 0;
   virtual void OnConnected() = 0;
   virtual void OnDisconnected() = 0;
};

class SubscriberConnection
{
public:
   SubscriberConnection(const std::shared_ptr<spdlog::logger>& logger
      , const std::shared_ptr<ZmqContext>& context);
   ~SubscriberConnection() noexcept;

   SubscriberConnection(const SubscriberConnection&) = delete;
   SubscriberConnection& operator = (const SubscriberConnection&) = delete;

   SubscriberConnection(SubscriberConnection&&) = delete;
   SubscriberConnection& operator = (SubscriberConnection&&) = delete;

   bool ConnectToPublisher(const std::string& host, const std::string& port, SubscriberConnectionListener* listener);

private:
   void stopListen();

   void listenFunction();

   enum SocketIndex {
      ControlSocketIndex = 0,
      StreamSocketIndex,
      MonitorSocketIndex
   };

   enum InternalCommandCode {
      CommandStop = 0
   };

   bool isActive() const;

   bool recvData();

private:
   std::shared_ptr<spdlog::logger>  logger_;
   std::shared_ptr<ZmqContext>      context_;

   std::string                      connectionName_;
   bool                             isConnected_ = false;

   ZmqContext::sock_ptr             dataSocket_;
   ZmqContext::sock_ptr             threadMasterSocket_;
   ZmqContext::sock_ptr             threadSlaveSocket_;
   ZmqContext::sock_ptr             monSocket_;

   std::thread                      listenThread_;
   SubscriberConnectionListener*    listener_ = nullptr;
};

#endif // __SUBSCRIBER_CONNECTION_H__