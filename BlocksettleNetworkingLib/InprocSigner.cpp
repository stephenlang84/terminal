#include "InprocSigner.h"
#include <spdlog/spdlog.h>
#include "Address.h"
#include "CoreWalletsManager.h"
#include "CoreHDWallet.h"
#include "Wallets/SyncHDWallet.h"
#include "Wallets/SyncSettlementWallet.h"

InprocSigner::InprocSigner(const std::shared_ptr<bs::core::WalletsManager> &mgr
   , const std::shared_ptr<spdlog::logger> &logger, const std::string &walletsPath
   , NetworkType netType)
   : SignContainer(logger, SignContainer::OpMode::LocalInproc)
   , walletsMgr_(mgr), walletsPath_(walletsPath), netType_(netType)
{ }

bool InprocSigner::Start()
{
   const auto &cbLoadProgress = [this](int cur, int total) {
      logger_->debug("[InprocSigner::Start] loading wallets: {} of {}", cur, total);
   };
   walletsMgr_->loadWallets(netType_, walletsPath_, cbLoadProgress);
   emit ready();
   return true;
}

SignContainer::RequestId InprocSigner::signTXRequest(const bs::core::wallet::TXSignRequest &txSignReq,
   bool, TXSignMode mode, const PasswordType &password, bool)
{
   if (!txSignReq.isValid()) {
      logger_->error("[{}] Invalid TXSignRequest", __func__);
      return 0;
   }
   const auto wallet = walletsMgr_->getWalletById(txSignReq.walletId);
   if (!wallet) {
      logger_->error("[{}] failed to find wallet with id {}", __func__, txSignReq.walletId);
      return 0;
   }

   const auto reqId = seqId_++;
   bs::core::wallet::TXSignRequest req;
   req.inputs = txSignReq.inputs;
   req.recipients = txSignReq.recipients;
   req.change.address = txSignReq.change.address;
   req.change.index = txSignReq.change.index;
   req.change.value = txSignReq.change.value;
   req.fee = txSignReq.fee;
   req.comment = txSignReq.comment;
   req.RBF = txSignReq.RBF;
   req.walletId = txSignReq.walletId;
   try {
      BinaryData signedTx;
      if (mode == TXSignMode::Full) {
         signedTx = wallet->signTXRequest(req, password);
      } else {
         signedTx = wallet->signPartialTXRequest(req, password);
      }
      QTimer::singleShot(1, [this, reqId, signedTx] {emit TXSigned(reqId, signedTx, {}, false); });
   }
   catch (const std::exception &e) {
      QTimer::singleShot(1, [this, reqId, e] { emit TXSigned(reqId, {}, e.what(), false); });
   }
   return reqId;  //stub
}

SignContainer::RequestId InprocSigner::createHDLeaf(const std::string &rootWalletId, const bs::hd::Path &path
   , const std::vector<bs::wallet::PasswordData> &pwdData)
{
   const RequestId reqId = seqId_++;
   return reqId;  //stub
}

SignContainer::RequestId InprocSigner::createHDWallet(const std::string &name, const std::string &desc
   , bool primary, const bs::core::wallet::Seed &seed, const std::vector<bs::wallet::PasswordData> &pwdData
   , bs::wallet::KeyRank keyRank)
{
   const auto wallet = walletsMgr_->createWallet(name, desc, seed, walletsPath_, primary, pwdData, keyRank);
   const RequestId reqId = seqId_++;
   const auto hdWallet = std::make_shared<bs::sync::hd::Wallet>(wallet->walletId(), wallet->name()
      , wallet->description(), std::shared_ptr<InprocSigner>(this), logger_);
   QTimer::singleShot(1, [this, reqId, hdWallet] { emit HDWalletCreated(reqId, hdWallet); });
   return reqId;
}

SignContainer::RequestId InprocSigner::createSetttlementWallet()
{
   const RequestId reqId = seqId_++;
   auto wallet = walletsMgr_->getSettlementWallet();
   if (!wallet) {
      wallet = walletsMgr_->createSettlementWallet(netType_, walletsPath_);
   }
   const auto settlWallet = std::make_shared<bs::sync::SettlementWallet>(wallet->walletId(), wallet->name()
      , wallet->description(), std::shared_ptr<InprocSigner>(this), logger_);
   QTimer::singleShot(1, [this, reqId, settlWallet] { emit SettlementWalletCreated(reqId, settlWallet); });
   return reqId;
}

void InprocSigner::syncWalletInfo(const std::function<void(std::vector<bs::sync::WalletInfo>)> &cb)
{
   std::vector<bs::sync::WalletInfo> result;
   for (size_t i = 0; i < walletsMgr_->getHDWalletsCount(); ++i) {
      const auto hdWallet = walletsMgr_->getHDWallet(i);
      result.push_back({ bs::sync::WalletFormat::HD, hdWallet->walletId(), hdWallet->name()
         , hdWallet->description(), hdWallet->networkType() });
   }
   const auto settlWallet = walletsMgr_->getSettlementWallet();
   if (settlWallet) {
      result.push_back({ bs::sync::WalletFormat::Settlement, settlWallet->walletId(), settlWallet->name()
         , settlWallet->description(), settlWallet->networkType() });
   }
   cb(result);
}

void InprocSigner::syncHDWallet(const std::string &id, const std::function<void(bs::sync::HDWalletData)> &cb)
{
   bs::sync::HDWalletData result;
   const auto hdWallet = walletsMgr_->getHDWalletById(id);
   if (hdWallet) {
      for (const auto &group : hdWallet->getGroups()) {
         bs::sync::HDWalletData::Group groupData;
         groupData.type = static_cast<bs::hd::CoinType>(group->index());

         for (const auto &leaf : group->getLeaves()) {
            groupData.leaves.push_back({ leaf->walletId(), leaf->index() });
         }
         result.groups.push_back(groupData);
      }
   }
   else {
      logger_->error("[{}] failed to find HD wallet with id {}", __func__, id);
   }
   cb(result);
}

void InprocSigner::syncWallet(const std::string &id, const std::function<void(bs::sync::WalletData)> &cb)
{
   bs::sync::WalletData result;
   const auto wallet = walletsMgr_->getWalletById(id);
   if (wallet) {
      result.encryptionTypes = wallet->encryptionTypes();
      result.encryptionKeys = wallet->encryptionKeys();
      result.encryptionRank = wallet->encryptionRank();
      result.netType = wallet->networkType();

      for (const auto &addr : wallet->getUsedAddressList()) {
         const auto index = wallet->getAddressIndex(addr);
         const auto comment = wallet->getAddressComment(addr);
         result.addresses.push_back({index, addr.getType(), addr, comment});
      }
      for (const auto &addr : wallet->getPooledAddressList()) {
         const auto index = wallet->getAddressIndex(addr);
         result.addrPool.push_back({ index, addr.getType(), addr });
      }
      for (const auto &txComment : wallet->getAllTxComments()) {
         result.txComments.push_back({txComment.first, txComment.second});
      }
   }
   cb(result);
}

void InprocSigner::syncAddressComment(const std::string &walletId, const bs::Address &addr, const std::string &comment)
{
   const auto wallet = walletsMgr_->getWalletById(walletId);
   if (wallet) {
      wallet->setAddressComment(addr, comment);
   }
}

void InprocSigner::syncTxComment(const std::string &walletId, const BinaryData &txHash, const std::string &comment)
{
   const auto wallet = walletsMgr_->getWalletById(walletId);
   if (wallet) {
      wallet->setTransactionComment(txHash, comment);
   }
}

void InprocSigner::syncNewAddress(const std::string &walletId, const std::string &index, AddressEntryType aet
   , const std::function<void(const bs::Address &)> &cb)
{
   const auto &cbAddrs = [cb](const std::vector<std::pair<bs::Address, std::string>> &outAddrs) {
      if (outAddrs.size() == 1) {
         cb(outAddrs[0].first);
      }
      else {
         cb({});
      }
   };
   syncNewAddresses(walletId, { {index, aet} }, cbAddrs);
}

void InprocSigner::syncNewAddresses(const std::string &walletId
   , const std::vector<std::pair<std::string, AddressEntryType>> &inData
   , const std::function<void(const std::vector<std::pair<bs::Address, std::string>> &)> &cb)
{
   std::vector<std::pair<bs::Address, std::string>> result;
   result.reserve(inData.size());
   const auto wallet = walletsMgr_->getWalletById(walletId);
   if (wallet) {
      for (const auto &in : inData) {
         result.push_back({ wallet->createAddressWithIndex(in.first, in.second), in.first });
      }
   }
   cb(result);
}
