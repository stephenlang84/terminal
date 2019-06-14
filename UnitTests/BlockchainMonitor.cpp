#include "BlockchainMonitor.h"
#include "Wallets/SyncHDWallet.h"
#include "Wallets/SyncWallet.h"


BlockchainMonitor::BlockchainMonitor(const std::shared_ptr<ArmoryConnection> &armory)
   : ArmoryCallbackTarget(armory.get())
{}

uint32_t BlockchainMonitor::waitForNewBlocks(uint32_t targetHeight)
{
   while (!receivedNewBlock_ || (targetHeight && (armory_->topBlock() < targetHeight))) {
      std::this_thread::sleep_for(std::chrono::milliseconds{ 10 });
   }
   receivedNewBlock_ = false;
   return armory_->topBlock();
}

bool BlockchainMonitor::waitForFlag(std::atomic_bool &flag, const std::chrono::milliseconds timeout)
{
   using namespace std::chrono_literals;
   const auto napTime = 10ms;
   for (auto elapsed = 0ms; elapsed < timeout; elapsed += napTime) {
      if (flag) {
         return true;
      }
      std::this_thread::sleep_for(napTime);
   }
   return false;
}

bool BlockchainMonitor::waitForWalletReady(const std::shared_ptr<bs::sync::Wallet> &wallet
   , const std::chrono::milliseconds timeout)
{
   using namespace std::chrono_literals;
   const auto napTime = 10ms;
   for (auto elapsed = 0ms; elapsed < timeout; elapsed += napTime) {
      if (wallet->isReady()) {
         return true;
      }
      std::this_thread::sleep_for(napTime);
   }
   return false;
}

bool BlockchainMonitor::waitForWalletReady(const std::shared_ptr<bs::sync::hd::Wallet> &wallet
   , const std::chrono::milliseconds timeout)
{
   using namespace std::chrono_literals;
   const auto napTime = 30ms;
   for (auto elapsed = 0ms; elapsed < timeout; elapsed += napTime) {
      if (wallet->isReady()) {
         return true;
      }
      std::this_thread::sleep_for(napTime);
   }
   return false;
}

std::vector<bs::TXEntry> BlockchainMonitor::waitForZC()
{
   while (true) {
      try {
         auto&& zcVec = zcQueue_.pop_front();
         return zcVec;
      }
      catch (IsEmpty&)
      {}

      std::this_thread::sleep_for(std::chrono::milliseconds{10});
   }
}

