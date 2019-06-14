#ifndef __BLOCKCHAIN_MONITOR_H__
#define __BLOCKCHAIN_MONITOR_H__

#include <atomic>
#include <memory>
#include <vector>
#include "ArmoryConnection.h"
#include "BinaryData.h"
#include "ClientClasses.h"
#include "ThreadSafeClasses.h"


namespace bs {
   namespace sync {
      namespace hd {
         class Wallet;
      }
      class Wallet;
   }
}

class BlockchainMonitor : public ArmoryCallbackTarget
{
public:
   BlockchainMonitor(const std::shared_ptr<ArmoryConnection> &);

   uint32_t waitForNewBlocks(uint32_t targetHeight = 0);
   std::vector<bs::TXEntry> waitForZC(void);
   bool waitForWalletReady(const std::shared_ptr<bs::sync::Wallet> &
      , const std::chrono::milliseconds timeout = std::chrono::milliseconds{ 10000 });
   bool waitForWalletReady(const std::shared_ptr<bs::sync::hd::Wallet> &
      , const std::chrono::milliseconds timeout = std::chrono::milliseconds{ 10000 });

   static bool waitForFlag(std::atomic_bool &
      , const std::chrono::milliseconds timeout = std::chrono::milliseconds{10000});

private:
   void onNewBlock(unsigned int) override {
      receivedNewBlock_ = true;
   }
   void onZCReceived(const std::vector<bs::TXEntry> &zcs) override {
      auto zcCopy = zcs;
      zcQueue_.push_back(std::move(zcCopy));
   }

private:
   std::atomic_bool  receivedNewBlock_{ false };
   Queue<std::vector<bs::TXEntry>> zcQueue_;
};

#endif // __BLOCKCHAIN_MONITOR_H__
