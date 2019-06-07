#include "HeadlessContainerListener.h"
#include <spdlog/spdlog.h>
#include "CheckRecipSigner.h"
#include "ConnectionManager.h"
#include "CoreHDWallet.h"
#include "CoreWalletsManager.h"
#include "DispatchQueue.h"
#include "ServerConnection.h"
#include "WalletEncryption.h"


using namespace Blocksettle::Communication;

HeadlessContainerListener::HeadlessContainerListener(const std::shared_ptr<spdlog::logger> &logger
   , const std::shared_ptr<bs::core::WalletsManager> &walletsMgr
   , const std::shared_ptr<DispatchQueue> &queue
   , const std::string &walletsPath, NetworkType netType
   , bool wo, const bool &backupEnabled)
   : ServerConnectionListener()
   , logger_(logger)
   , walletsMgr_(walletsMgr)
   , queue_(queue)
   , walletsPath_(walletsPath)
   , backupPath_(walletsPath + "/../backup")
   , netType_(netType)
   , watchingOnly_(wo)
   , backupEnabled_(backupEnabled)
{
}

void HeadlessContainerListener::setCallbacks(HeadlessContainerCallbacks *callbacks)
{
   callbacks_ = callbacks;
}

HeadlessContainerListener::~HeadlessContainerListener() noexcept
{
   disconnect();
}

bool HeadlessContainerListener::disconnect(const std::string &clientId)
{
   headless::RequestPacket packet;
   packet.set_data("");
   packet.set_type(headless::DisconnectionRequestType);
   const auto &serializedPkt = packet.SerializeAsString();

   bool rc = sendData(serializedPkt, clientId);
   if (rc && !clientId.empty()) {
      OnClientDisconnected(clientId);
   }
   return rc;
}

bool HeadlessContainerListener::sendData(const std::string &data, const std::string &clientId)
{
   if (!connection_) {
      return false;
   }

   bool sentOk = false;
   if (clientId.empty()) {
      for (const auto &clientId : connectedClients_) {
         if (connection_->SendDataToClient(clientId, data)) {
            sentOk = true;
         }
      }
   }
   else {
      sentOk = connection_->SendDataToClient(clientId, data);
   }
   return sentOk;
}

void HeadlessContainerListener::SetLimits(const bs::signer::Limits &limits)
{
   limits_ = limits;
}

static std::string toHex(const std::string &binData)
{
   return BinaryData(binData).toHexStr();
}

void HeadlessContainerListener::OnClientConnected(const std::string &clientId)
{
   logger_->debug("[HeadlessContainerListener] client {} connected", toHex(clientId));

   queue_->dispatch([this, clientId] {
      connectedClients_.insert(clientId);
   });
}

void HeadlessContainerListener::OnClientDisconnected(const std::string &clientId)
{
   logger_->debug("[HeadlessContainerListener] client {} disconnected", toHex(clientId));

   queue_->dispatch([this, clientId] {
      connectedClients_.erase(clientId);

      if (callbacks_) {
         callbacks_->clientDisconn(clientId);
      }
   });
}

void HeadlessContainerListener::OnDataFromClient(const std::string &clientId, const std::string &data)
{
   queue_->dispatch([this, clientId, data] {
      headless::RequestPacket packet;
      if (!packet.ParseFromString(data)) {
         logger_->error("[{}] failed to parse request packet", __func__);
         return;
      }

      if (!onRequestPacket(clientId, packet)) {
         packet.set_data("");
         sendData(packet.SerializeAsString(), clientId);
      }
   });
}

void HeadlessContainerListener::OnPeerConnected(const std::string &ip)
{
   logger_->debug("[{}] IP {} connected", __func__, ip);
   queue_->dispatch([this, ip] {
      if (callbacks_) {
         callbacks_->peerConn(ip);
      }
   });
}

void HeadlessContainerListener::OnPeerDisconnected(const std::string &ip)
{
   logger_->debug("[{}] IP {} disconnected", __func__, ip);
   queue_->dispatch([this, ip] {
      if (callbacks_) {
         callbacks_->peerDisconn(ip);
      }
   });
}

bool HeadlessContainerListener::isRequestAllowed(Blocksettle::Communication::headless::RequestType reqType) const
{
   if (watchingOnly_) {
      switch (reqType) {
      case headless::CancelSignTxRequestType:
      case headless::SignTXRequestType:
      case headless::SignPartialTXRequestType:
      case headless::SignPayoutTXRequestType:
      case headless::SignTXMultiRequestType:
      case headless::PasswordRequestType:
      case headless::CreateHDWalletRequestType:
      case headless::SetLimitsRequestType:
         return false;
      default:    break;
      }
   }
   return true;
}

bool HeadlessContainerListener::onRequestPacket(const std::string &clientId, headless::RequestPacket packet)
{
   if (!connection_) {
      logger_->error("[HeadlessContainerListener::{}] connection_ is not set");
      return false;
   }

   connection_->GetClientInfo(clientId);
   if (!isRequestAllowed(packet.type())) {
      logger_->info("[{}] request {} is not applicable at this state", __func__, (int)packet.type());
      return false;
   }

   switch (packet.type()) {
   case headless::HeartbeatType:
      packet.set_data("");
      if (!sendData(packet.SerializeAsString(), clientId)) {
         logger_->error("[HeadlessContainerListener] failed to send response hearbeat packet");
         return false;
      }
      break;

   case headless::AuthenticationRequestType:
      return AuthResponse(clientId, packet);

   case headless::CancelSignTxRequestType:
      return onCancelSignTx(clientId, packet);

   case headless::SignTXRequestType:
      return onSignTXRequest(clientId, packet);

   case headless::SignPartialTXRequestType:
      return onSignTXRequest(clientId, packet, true);

   case headless::SignPayoutTXRequestType:
      return onSignPayoutTXRequest(clientId, packet);

   case headless::SignTXMultiRequestType:
      return onSignMultiTXRequest(clientId, packet);

   case headless::PasswordRequestType:
      return onPasswordReceived(clientId, packet);

   case headless::SetUserIdRequestType:
      return onSetUserId(clientId, packet);

   case headless::CreateHDWalletRequestType:
      return onCreateHDWallet(clientId, packet);

   case headless::DeleteHDWalletRequestType:
      return onDeleteHDWallet(packet);

   case headless::SetLimitsRequestType:
      return onSetLimits(clientId, packet);

   case headless::GetHDWalletInfoRequestType:
      return onGetHDWalletInfo(clientId, packet);

   case headless::DisconnectionRequestType:
      break;

   case headless::SyncWalletInfoType:
      return onSyncWalletInfo(clientId, packet);

   case headless::SyncHDWalletType:
      return onSyncHDWallet(clientId, packet);

   case headless::SyncWalletType:
      return onSyncWallet(clientId, packet);

   case headless::SyncCommentType:
      return onSyncComment(clientId, packet);

   case headless::SyncAddressesType:
      return onSyncAddresses(clientId, packet);

   case headless::ExtendAddressChainType:
      return onExtAddrChain(clientId, packet);

   case headless::ExecCustomDialogRequestType:
      return onExecCustomDialog(clientId, packet);

   default:
      logger_->error("[HeadlessContainerListener] unknown request type {}", packet.type());
      return false;
   }
   return true;
}

bool HeadlessContainerListener::AuthResponse(const std::string &clientId, headless::RequestPacket packet)
{
   headless::AuthenticationReply response;
   response.set_authticket("");  // no auth tickets after moving to BIP150/151
   response.set_hasui(callbacks_ != nullptr);
   response.set_nettype((netType_ == NetworkType::TestNet) ? headless::TestNetType : headless::MainNetType);

   packet.set_data(response.SerializeAsString());
   return sendData(packet.SerializeAsString(), clientId);
}

bool HeadlessContainerListener::onSignTXRequest(const std::string &clientId, const headless::RequestPacket &packet, bool partial)
{
   const auto reqType = partial ? headless::SignPartialTXRequestType : headless::SignTXRequestType;
   headless::SignTXRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[HeadlessContainerListener] failed to parse SignTXRequest");
      SignTXResponse(clientId, packet.id(), reqType, "failed to parse");
      return false;
   }
   uint64_t inputVal = 0;
   bs::core::wallet::TXSignRequest txSignReq;
   txSignReq.walletId = request.walletid();
   for (int i = 0; i < request.inputs_size(); i++) {
      UTXO utxo;
      utxo.unserialize(request.inputs(i));
      if (utxo.isInitialized()) {
         txSignReq.inputs.push_back(utxo);
         inputVal += utxo.getValue();
         logger_->debug("[{}] UTXO {}, addr {}", __func__, utxo.getTxHash().toHexStr(true), bs::Address::fromUTXO(utxo).display());
      }
   }

   uint64_t outputVal = 0;
   for (int i = 0; i < request.recipients_size(); i++) {
      BinaryData serialized = request.recipients(i);
      const auto recip = ScriptRecipient::deserialize(serialized);
      txSignReq.recipients.push_back(recip);
      outputVal += recip->getValue();
   }
   int64_t value = outputVal;

   txSignReq.fee = request.fee();
   txSignReq.RBF = request.rbf();

   if (!request.unsignedstate().empty()) {
      const BinaryData prevState(request.unsignedstate());
      txSignReq.prevStates.push_back(prevState);
      if (!value) {
         bs::CheckRecipSigner signer(prevState);
         value = signer.spendValue();
         if (txSignReq.change.value) {
            value -= txSignReq.change.value;
         }
      }
   }

   if (request.has_change()) {
      txSignReq.change.address = request.change().address();
      txSignReq.change.index = request.change().index();
      txSignReq.change.value = request.change().value();
   }

   if (!txSignReq.isValid()) {
      logger_->error("[HeadlessContainerListener] invalid SignTXRequest");
      SignTXResponse(clientId, packet.id(), reqType, "missing critical data");
      return false;
   }

   txSignReq.populateUTXOs = request.populateutxos();

   const auto wallet = walletsMgr_->getWalletById(txSignReq.walletId);
   if (!wallet) {
      logger_->error("[HeadlessContainerListener] failed to find wallet {}", txSignReq.walletId);
      SignTXResponse(clientId, packet.id(), reqType, "failed to find wallet " + txSignReq.walletId);
      return false;
   }
   const auto rootWalletId = walletsMgr_->getHDRootForLeaf(txSignReq.walletId)->walletId();

   if ((wallet->type() == bs::core::wallet::Type::Bitcoin)
      && !CheckSpendLimit(value, request.applyautosignrules(), rootWalletId)) {
      SignTXResponse(clientId, packet.id(), reqType, "spend limit exceeded");
      return false;
   }

   const auto onPassword = [this, wallet, txSignReq, rootWalletId, clientId, id = packet.id(), partial
      , reqType, value, autoSign = request.applyautosignrules()
      , keepDuplicatedRecipients = request.keepduplicatedrecipients()] (const SecureBinaryData &pass,
            bool cancelledByUser) {
      try {
         if (cancelledByUser) {
            return;
         }
         logger_->debug("SignTX partial: {}, populate UTXOs: {}", partial, txSignReq.populateUTXOs);
         {
            auto passLock = wallet->lockForEncryption(pass);
            const auto tx = partial ? wallet->signPartialTXRequest(txSignReq)
               : wallet->signTXRequest(txSignReq);
            SignTXResponse(clientId, id, reqType, {}, tx, cancelledByUser);
         }

         onXbtSpent(value, autoSign);
         if (callbacks_) {
            callbacks_->xbtSpent(value, autoSign);
         }
      }
      catch (const std::exception &e) {
         logger_->error("[HeadlessContainerListener] failed to sign {} TX request: {}", partial ? "partial" : "full", e.what());
         SignTXResponse(clientId, id, reqType, std::string("failed to sign: ") + e.what());
         passwords_.erase(wallet->walletId());
         passwords_.erase(rootWalletId);
         if (callbacks_) {
            callbacks_->asDeact(rootWalletId);
         }
      }
   };

   if (!request.password().empty()) {
      logger_->debug("[{}] password: {}", __func__, request.password());
      onPassword(BinaryData::CreateFromHex(request.password()), false);
      return true;
   }

   const std::string prompt = std::string("Outgoing ") + (partial ? "Partial " : "" ) + "Transaction";
   return RequestPasswordIfNeeded(clientId, txSignReq, prompt, onPassword, request.applyautosignrules());
}

bool HeadlessContainerListener::onCancelSignTx(const std::string &, headless::RequestPacket packet)
{
   headless::CancelSignTx request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[HeadlessContainerListener] failed to parse CancelSignTx");
      return false;
   }

   if (callbacks_) {
      callbacks_->cancelTxSign(request.txid());
   }

   return true;
}

bool HeadlessContainerListener::onSignPayoutTXRequest(const std::string &clientId, const headless::RequestPacket &packet)
{
   const auto reqType = headless::SignPayoutTXRequestType;
   headless::SignPayoutTXRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[HeadlessContainerListener] failed to parse SignPayoutTXRequest");
      SignTXResponse(clientId, packet.id(), reqType, "failed to parse");
      return false;
   }

/*   const auto settlWallet = std::dynamic_pointer_cast<bs::core::SettlementWallet>(walletsMgr_->getSettlementWallet());
   if (!settlWallet) {
      logger_->error("[HeadlessContainerListener] Settlement wallet is missing");
      SignTXResponse(clientId, packet.id(), reqType, "no settlement wallet");
      return false;
   }

   const auto &authWallet = walletsMgr_->getAuthWallet();
   if (!authWallet) {
      logger_->error("[HeadlessContainerListener] Auth wallet is missing");
      SignTXResponse(clientId, packet.id(), reqType, "no auth wallet");
      return false;
   }*/

   bs::core::wallet::TXSignRequest txSignReq;
//   txSignReq.walletId = authWallet->walletId();
   UTXO utxo;
   utxo.unserialize(request.input());
   if (utxo.isInitialized()) {
      txSignReq.inputs.push_back(utxo);
   }

   BinaryData serialized = request.recipient();
   const auto recip = ScriptRecipient::deserialize(serialized);
   txSignReq.recipients.push_back(recip);

   const bs::Address authAddr(request.authaddress());
   const BinaryData settlementId = request.settlementid();

   const auto rootWalletId = walletsMgr_->getPrimaryWallet()->walletId();
   //walletsMgr_->getHDRootForLeaf(authWallet->walletId())->walletId();

   const auto onAuthPassword = [this, clientId, id = packet.id(), txSignReq, /*authWallet,*/ authAddr
      , /*settlWallet,*/ settlementId, reqType, rootWalletId](const SecureBinaryData &pass,
            bool cancelledByUser) {
/*      if (!authWallet->encryptionTypes().empty() && pass.isNull()) {
         logger_->error("[HeadlessContainerListener] no password for encrypted auth wallet");
         SignTXResponse(clientId, id, reqType, "password required, but empty received");
      }*/

      bs::core::KeyPair authKeys;// = authWallet->getKeyPairFor(authAddr, pass);
      if (authKeys.privKey.isNull() || authKeys.pubKey.isNull()) {
         logger_->error("[HeadlessContainerListener] failed to get priv/pub keys for {}", authAddr.display());
         SignTXResponse(clientId, id, reqType, "no auth priv/pub keys found");
//         passwords_.erase(authWallet->walletId());
         passwords_.erase(rootWalletId);
         if (callbacks_) {
            callbacks_->asDeact(rootWalletId);
         }
         return;
      }

/*      try {
         const auto tx = settlWallet->signPayoutTXRequest(txSignReq, authKeys, settlementId);
         SignTXResponse(clientId, id, reqType, {}, tx, cancelledByUser);
      }
      catch (const std::exception &e) {
         logger_->error("[HeadlessContainerListener] failed to sign PayoutTX request: {}", e.what());
         SignTXResponse(clientId, id, reqType, std::string("failed to sign: ") + e.what());
      }*/
   };

/*   if (!request.password().empty()) {
      onAuthPassword(BinaryData::CreateFromHex(request.password()), false);
      return true;
   }*/

   std::stringstream ssPrompt;
   ssPrompt << "Signing pay-out transaction for " << std::fixed
      << std::setprecision(8) << utxo.getValue() / BTCNumericTypes::BalanceDivider
      << " XBT:\n Settlement ID: " << settlementId.toHexStr();

   return RequestPasswordIfNeeded(clientId, txSignReq, ssPrompt.str(), onAuthPassword, request.applyautosignrules());
}

bool HeadlessContainerListener::onSignMultiTXRequest(const std::string &clientId, const headless::RequestPacket &packet)
{
   const auto reqType = headless::SignTXMultiRequestType;
   headless::SignTXMultiRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[HeadlessContainerListener] failed to parse SignTXMultiRequest");
      SignTXResponse(clientId, packet.id(), reqType, "failed to parse");
      return false;
   }

   bs::core::wallet::TXMultiSignRequest txMultiReq;
   bs::core::WalletMap walletMap;
   txMultiReq.prevState = request.signerstate();
   for (int i = 0; i < request.walletids_size(); i++) {
      const auto &wallet = walletsMgr_->getWalletById(request.walletids(i));
      if (!wallet) {
         logger_->error("[HeadlessContainerListener] failed to find wallet with id {}", request.walletids(i));
         SignTXResponse(clientId, packet.id(), reqType, "failed to find wallet " + request.walletids(i));
         return false;
      }
      walletMap[wallet->walletId()] = wallet;
   }

   const std::string prompt("Signing multi-wallet input (auth revoke) transaction");

   const auto cbOnAllPasswords = [this, txMultiReq, walletMap, clientId, reqType, id=packet.id()]
                                 (const std::unordered_map<std::string, SecureBinaryData> &walletPasswords) {
      try {
         const auto tx = bs::core::SignMultiInputTX(txMultiReq, walletPasswords, walletMap);
         SignTXResponse(clientId, id, reqType, {}, tx);
      }
      catch (const std::exception &e) {
         logger_->error("[HeadlessContainerListener] failed to sign multi TX request: {}", e.what());
         SignTXResponse(clientId, id, reqType, std::string("failed to sign: ") + e.what());
      }
   };
   return RequestPasswordsIfNeeded(++reqSeqNo_, clientId, txMultiReq, walletMap, prompt, cbOnAllPasswords);
}

void HeadlessContainerListener::SignTXResponse(const std::string &clientId, unsigned int id, headless::RequestType reqType
   , const std::string &error, const BinaryData &tx, bool cancelledByUser)
{
   headless::SignTXReply response;
   if (tx.isNull()) {
      response.set_error(error);
   }
   else {
      response.set_signedtx(tx.toBinStr());
   }
   response.set_cancelledbyuser(cancelledByUser);

   headless::RequestPacket packet;
   packet.set_id(id);
   packet.set_type(reqType);
   packet.set_data(response.SerializeAsString());

   if (!sendData(packet.SerializeAsString(), clientId)) {
      logger_->error("[HeadlessContainerListener] failed to send response signTX packet");
   }
   if (callbacks_) {
      callbacks_->txSigned(tx);
   }
}

bool HeadlessContainerListener::onPasswordReceived(const std::string &clientId, headless::RequestPacket &packet)
{
   headless::PasswordReply response;
   if (!response.ParseFromString(packet.data())) {
      logger_->error("[HeadlessContainerListener] failed to parse PasswordReply");
      return false;
   }
   if (response.walletid().empty()) {
      logger_->error("[HeadlessContainerListener] walletId is empty in PasswordReply");
      return false;
   }
   const auto password = BinaryData::CreateFromHex(response.password());
   if (!password.isNull()) {
      passwords_[response.walletid()] = password;
   }

   passwordReceived(clientId, response.walletid(), password, response.cancelledbyuser());
   return true;
}

void HeadlessContainerListener::passwordReceived(const std::string &clientId, const std::string &walletId,
   const SecureBinaryData &password, bool cancelledByUser)
{
   const auto cbsIt = passwordCallbacks_.find(walletId);
   if (cbsIt != passwordCallbacks_.end()) {
      for (const auto &cb : cbsIt->second) {
         cb(password, cancelledByUser);
      }
      passwordCallbacks_.erase(cbsIt);
   }
   if (autoSignPwdReqs_.find(walletId) != autoSignPwdReqs_.end()) {
      autoSignPwdReqs_.erase(walletId);
      activateAutoSign(clientId, walletId, password);
   }
}

void HeadlessContainerListener::passwordReceived(const std::string &walletId,
   const SecureBinaryData &password, bool cancelledByUser)
{
   passwordReceived({}, walletId, password, cancelledByUser);
}

bool HeadlessContainerListener::RequestPasswordIfNeeded(const std::string &clientId
   , const bs::core::wallet::TXSignRequest &txReq
   , const std::string &prompt, const PasswordReceivedCb &cb, bool autoSign)
{
   const auto &wallet = walletsMgr_->getWalletById(txReq.walletId);
   if (!wallet) {
      logger_->error("[{}] failed to find wallet {}", __func__, txReq.walletId);
      return false;
   }
   bool needPassword = !wallet->encryptionTypes().empty();
   SecureBinaryData password;
   std::string walletId = wallet->walletId();
   if (needPassword && autoSign) {
      const auto &hdRoot = walletsMgr_->getHDRootForLeaf(walletId);
      if (hdRoot) {
         walletId = hdRoot->walletId();
      }
      const auto passwordIt = passwords_.find(walletId);
      if (passwordIt != passwords_.end()) {
         needPassword = false;
         password = passwordIt->second;
      }
   }
   if (!needPassword) {
      if (cb) {
         cb(password, false);
      }
      return true;
   }

   return RequestPassword(clientId, txReq, prompt, cb);
}

bool HeadlessContainerListener::RequestPasswordsIfNeeded(int reqId, const std::string &clientId
   , const bs::core::wallet::TXMultiSignRequest &txMultiReq, const bs::core::WalletMap &walletMap
   , const std::string &prompt, const PasswordsReceivedCb &cb)
{
   TempPasswords tempPasswords;
   for (const auto &wallet : walletMap) {
      const auto &walletId = wallet.first;
      const auto &rootWallet = walletsMgr_->getHDRootForLeaf(walletId);
      const auto &rootWalletId = rootWallet->walletId();

      tempPasswords.rootLeaves[rootWalletId].insert(walletId);
      tempPasswords.reqWalletIds.insert(walletId);

      if (!rootWallet->encryptionTypes().empty()) {
         const auto cbWalletPass = [this, reqId, cb, rootWalletId](const SecureBinaryData &password, bool) {
            auto &tempPasswords = tempPasswords_[reqId];
            const auto &walletsIt = tempPasswords.rootLeaves.find(rootWalletId);
            if (walletsIt == tempPasswords.rootLeaves.end()) {
               return;
            }
            for (const auto &walletId : walletsIt->second) {
               tempPasswords.passwords[walletId] = password;
            }
            if (tempPasswords.passwords.size() == tempPasswords.reqWalletIds.size()) {
               cb(tempPasswords.passwords);
               tempPasswords_.erase(reqId);
            }
         };

         bs::core::wallet::TXSignRequest txReq;
         txReq.walletId = rootWallet->walletId();
         RequestPassword(clientId, txReq, prompt, cbWalletPass);
      }
      else {
         tempPasswords.passwords[walletId] = {};
      }
   }
   if (tempPasswords.reqWalletIds.size() == tempPasswords.passwords.size()) {
      cb(tempPasswords.passwords);
   }
   else {
      tempPasswords_[reqId] = tempPasswords;
   }
   return true;
}

bool HeadlessContainerListener::RequestPassword(const std::string &clientId, const bs::core::wallet::TXSignRequest &txReq
   , const std::string &prompt, const PasswordReceivedCb &cb)
{
   if (cb) {
      auto &callbacks = passwordCallbacks_[txReq.walletId];
      callbacks.push_back(cb);
      if (callbacks.size() > 1) {
         return true;
      }
   }

   if (callbacks_) {
      callbacks_->pwd(txReq, prompt);
      return true;
   }
   else {
      headless::PasswordRequest request;
      if (!prompt.empty()) {
         request.set_prompt(prompt);
      }
      if (!txReq.walletId.empty()) {
         request.set_walletid(txReq.walletId);
         const auto &wallet = walletsMgr_->getWalletById(txReq.walletId);
         std::vector<bs::wallet::EncryptionType> encTypes;
         std::vector<SecureBinaryData> encKeys;
         bs::wallet::KeyRank keyRank = { 0, 0 };
         if (wallet) {
            encTypes = wallet->encryptionTypes();
            encKeys = wallet->encryptionKeys();
            keyRank = wallet->encryptionRank();
         }
         else {
            const auto &rootWallet = walletsMgr_->getHDWalletById(txReq.walletId);
            if (rootWallet) {
               encTypes = rootWallet->encryptionTypes();
               encKeys = rootWallet->encryptionKeys();
               keyRank = rootWallet->encryptionRank();
            }
         }

         for (const auto &encType : encTypes) {
            request.add_enctypes(static_cast<uint32_t>(encType));
         }
         for (const auto &encKey : encKeys) {
            request.add_enckeys(encKey.toBinStr());
         }
         request.set_rankm(keyRank.first);
      }

      headless::RequestPacket packet;
      packet.set_type(headless::PasswordRequestType);
      packet.set_data(request.SerializeAsString());
      return sendData(packet.SerializeAsString(), clientId);
   }
}

bool HeadlessContainerListener::onSetUserId(const std::string &clientId, headless::RequestPacket &packet)
{
   headless::SetUserIdRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[HeadlessContainerListener] failed to parse SetUserIdRequest");
      return false;
   }


   headless::RequestPacket response;
   response.set_type(headless::SetUserIdRequestType);
   sendData(response.SerializeAsString(), clientId);
   return true;
}

bool HeadlessContainerListener::CreateHDLeaf(const std::string &clientId, unsigned int id, const headless::NewHDLeaf &request
   , const std::vector<bs::wallet::PasswordData> &pwdData)
{
   const auto hdWallet = walletsMgr_->getHDWalletById(request.rootwalletid());
   if (!hdWallet) {
      logger_->error("[HeadlessContainerListener] failed to find root HD wallet by id {}", request.rootwalletid());
      CreateHDWalletResponse(clientId, id, "no root HD wallet");
      return false;
   }
   const auto path = bs::hd::Path::fromString(request.path());
   if ((path.length() != 3) || !path.isAbolute()) {
      logger_->error("[HeadlessContainerListener] invalid path {} at HD wallet creation", request.path());
      CreateHDWalletResponse(clientId, id, "invalid path");
      return false;
   }

   auto& password = pwdData[0].password;
   if (backupEnabled_)
      walletsMgr_->backupWallet(hdWallet, backupPath_);

   const auto onPassword = 
      [this, hdWallet, path, clientId, id]
      (const SecureBinaryData &pass, bool cancelledByUser)
   {
      if (!hdWallet->encryptionTypes().empty() && pass.isNull())
      {
         logger_->error("[HeadlessContainerListener] no password for encrypted wallet");
         CreateHDWalletResponse(clientId, id, "password required, but empty received");
      }

      //what is this horror?
      const auto groupIndex = static_cast<bs::hd::CoinType>(path.get(1));
      auto group = hdWallet->getGroup(groupIndex);
      if (!group)
         group = hdWallet->createGroup(groupIndex);

      const auto leafIndex = path.get(2);
      auto leaf = group->getLeafByPath(leafIndex);

      if (leaf == nullptr) 
      {
         hdWallet->lockForEncryption(pass);
         leaf = group->createLeaf(leafIndex);

         if (leaf == nullptr)
         {
            logger_->error("[HeadlessContainerListener] failed to create/get leaf {}", path.toString());
            CreateHDWalletResponse(clientId, id, "failed to create leaf");
            return;
         }
      }

      auto assetPtr = leaf->getRootAsset();
      CreateHDWalletResponse(clientId, id, "",
         assetPtr->getPubKey()->getUncompressedKey(), 
         assetPtr->getChaincode());
   };

   if (!hdWallet->encryptionTypes().empty()) {
      if (!password.isNull()) {
         onPassword(password, false);
      }
      else {
         bs::core::wallet::TXSignRequest txReq;
         txReq.walletId = hdWallet->walletId();
         return RequestPassword(clientId, txReq, "Creating a wallet " + txReq.walletId, onPassword);
      }
   }
   else {
      onPassword({}, false);
   }
   return true;
}

bool HeadlessContainerListener::CreateHDWallet(const std::string &clientId, unsigned int id, const headless::NewHDWallet &request
   , NetworkType netType, const std::vector<bs::wallet::PasswordData> &pwdData, bs::wallet::KeyRank keyRank)
{
   if (netType != netType_) {
      CreateHDWalletResponse(clientId, id, "network type mismatch");
      return false;
   }
   std::shared_ptr<bs::core::hd::Wallet> wallet;
   try {
      auto seed = request.privatekey().empty() ? bs::core::wallet::Seed(request.seed(), netType)
         : bs::core::wallet::Seed(request.privatekey(), netType);
      wallet = walletsMgr_->createWallet(request.name(), request.description(),
         seed, walletsPath_, pwdData.begin()->password, request.primary());
   }
   catch (const std::exception &e) {
      CreateHDWalletResponse(clientId, id, e.what());
      return false;
   }
   if (!wallet) {
      CreateHDWalletResponse(clientId, id, "creation failed");
      return false;
   }
   try 
   {
      const auto woWallet = wallet->createWatchingOnly();
      if (!woWallet) 
      {
         CreateHDWalletResponse(clientId, id, "failed to create watching-only copy");
         return false;
      }

      CreateHDWalletResponse(clientId, id, woWallet->walletId(), {}, {}, woWallet);
   }
   catch (const std::exception &e) {
      CreateHDWalletResponse(clientId, id, e.what());
      return false;
   }
   return true;
}

bool HeadlessContainerListener::onCreateHDWallet(const std::string &clientId, headless::RequestPacket &packet)
{
   // Not used anymore, use SignAdaptor instead
   return false;
}

void HeadlessContainerListener::CreateHDWalletResponse(const std::string &clientId, unsigned int id
   , const std::string &errorOrWalletId, const BinaryData &pubKey, const BinaryData &chainCode
   , const std::shared_ptr<bs::core::hd::Wallet> &wallet)
{
   logger_->debug("[HeadlessContainerListener] CreateHDWalletResponse: {}", errorOrWalletId);
   headless::CreateHDWalletResponse response;
   if (!pubKey.isNull() && !chainCode.isNull()) {
      auto leaf = response.mutable_leaf();
      leaf->set_walletid(errorOrWalletId);
   }
   else if (wallet) {
      auto wlt = response.mutable_wallet();
      wlt->set_name(wallet->name());
      wlt->set_description(wallet->description());
      wlt->set_walletid(wallet->walletId());
      wlt->set_nettype((wallet->networkType() == NetworkType::TestNet) ? headless::TestNetType : headless::MainNetType);
      for (const auto &group : wallet->getGroups()) {
         auto grp = wlt->add_groups();
         grp->set_path(group->path().toString());
         for (const auto &leaf : group->getLeaves()) {
            auto wLeaf = wlt->add_leaves();
            wLeaf->set_path(leaf->path().toString());
            wLeaf->set_walletid(leaf->walletId());
         }
      }
   }
   else {
      response.set_error(errorOrWalletId);
   }

   headless::RequestPacket packet;
   packet.set_id(id);
   packet.set_type(headless::CreateHDWalletRequestType);
   packet.set_data(response.SerializeAsString());

   if (!sendData(packet.SerializeAsString(), clientId)) {
      logger_->error("[HeadlessContainerListener] failed to send response CreateHDWallet packet");
   }
}

bool HeadlessContainerListener::onDeleteHDWallet(headless::RequestPacket &packet)
{
   // Not used anymore, use SignAdaptor instead
   return false;
}

bool HeadlessContainerListener::onSetLimits(const std::string &clientId, headless::RequestPacket &packet)
{
   headless::SetLimitsRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[HeadlessContainerListener] failed to parse SetLimitsRequest");
      AutoSignActiveResponse(clientId, {}, false, "request parse error", packet.id());
      return false;
   }
   if (request.rootwalletid().empty()) {
      logger_->error("[HeadlessContainerListener] no wallet specified in SetLimitsRequest");
      AutoSignActiveResponse(clientId, request.rootwalletid(), false, "invalid request", packet.id());
      return false;
   }
   if (!request.activateautosign()) {
      deactivateAutoSign(clientId, request.rootwalletid());
      return true;
   }

   if (!request.password().empty()) {
      activateAutoSign(clientId, request.rootwalletid(), BinaryData::CreateFromHex(request.password()));
   }
   else {
      const auto &wallet = walletsMgr_->getHDWalletById(request.rootwalletid());
      if (!wallet) {
         logger_->error("[HeadlessContainerListener] failed to find root wallet by id {} (to activate auto-sign)"
            , request.rootwalletid());
         AutoSignActiveResponse(clientId, request.rootwalletid(), false, "missing wallet", packet.id());
         return false;
      }
      if (!wallet->encryptionTypes().empty() && !isAutoSignActive(request.rootwalletid())) {
         addPendingAutoSignReq(request.rootwalletid());
         if (callbacks_) {
            bs::core::wallet::TXSignRequest txReq;
            txReq.walletId = request.rootwalletid();
            txReq.autoSign = true;
            callbacks_->pwd(txReq, {});
         }
      }
      else {
         if (callbacks_) {
            callbacks_->asAct(request.rootwalletid());
         }
         AutoSignActiveResponse(clientId, request.rootwalletid(), true, {}, packet.id());
      }
   }
   return true;
}

bool HeadlessContainerListener::onGetHDWalletInfo(const std::string &clientId, headless::RequestPacket &packet)
{
   headless::GetHDWalletInfoRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[HeadlessContainerListener] failed to parse GetHDWalletInfoRequest");
      GetHDWalletInfoResponse(clientId, packet.id(), {}, nullptr, "failed to parse request");
      return false;
   }
   const auto &wallet = walletsMgr_->getHDWalletById(request.rootwalletid());
   if (!wallet) {
      logger_->error("[HeadlessContainerListener] failed to find wallet for id {}", request.rootwalletid());
      GetHDWalletInfoResponse(clientId, packet.id(), request.rootwalletid(), nullptr, "failed to find wallet");
      return false;
   }
   GetHDWalletInfoResponse(clientId, packet.id(), request.rootwalletid(), wallet);
   return true;
}

void HeadlessContainerListener::GetHDWalletInfoResponse(const std::string &clientId, unsigned int id
   , const std::string &walletId, const std::shared_ptr<bs::core::hd::Wallet> &wallet, const std::string &error)
{
   headless::GetHDWalletInfoResponse response;
   if (!error.empty()) {
      response.set_error(error);
   }
   if (wallet) {
      for (const auto &encType : wallet->encryptionTypes()) {
         response.add_enctypes(static_cast<uint32_t>(encType));
      }
      for (const auto &encKey : wallet->encryptionKeys()) {
         response.add_enckeys(encKey.toBinStr());
      }
      response.set_rankm(wallet->encryptionRank().first);
      response.set_rankn(wallet->encryptionRank().second);
   }
   if (!walletId.empty()) {
      response.set_rootwalletid(walletId);
   }

   headless::RequestPacket packet;
   packet.set_id(id);
   packet.set_type(headless::GetHDWalletInfoRequestType);
   packet.set_data(response.SerializeAsString());

   if (!sendData(packet.SerializeAsString(), clientId)) {
      logger_->error("[HeadlessContainerListener::{}] failed to send to {}", __func__
         , BinaryData(clientId).toHexStr());
   }
}

bool HeadlessContainerListener::onChangePassword(const std::string &clientId
   , headless::RequestPacket &packet)
{
/*   headless::ChangePasswordRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[HeadlessContainerListener] failed to parse ChangePasswordRequest");
      ChangePasswordResponse(clientId, packet.id(), {}, false);
      return false;
   }
   const auto &wallet = walletsMgr_->getHDWalletById(request.rootwalletid());
   if (!wallet) {
      logger_->error("[HeadlessContainerListener] failed to find wallet for id {}", request.rootwalletid());
      ChangePasswordResponse(clientId, packet.id(), request.rootwalletid(), false);
      return false;
   }
   std::vector<bs::wallet::PasswordData> pwdData;
   for (int i = 0; i < request.newpassword_size(); ++i) {
      const auto &pwd = request.newpassword(i);
      pwdData.push_back({ BinaryData::CreateFromHex(pwd.password())
         , static_cast<bs::wallet::EncryptionType>(pwd.enctype()), pwd.enckey()});
   }
   bs::wallet::KeyRank keyRank = {request.rankm(), request.rankn()};

   bool result = wallet->changePassword(pwdData.begin()->password);

   if (!result) {
      logger_->error("[HeadlessContainerListener] failed to change password for wallet {}", request.rootwalletid());
      ChangePasswordResponse(clientId, packet.id(), request.rootwalletid(), false);
      return false;
   }
   logger_->info("Changed password for wallet {} (id: {})", wallet->name(), wallet->walletId());
   ChangePasswordResponse(clientId, packet.id(), request.rootwalletid(), true);
   return true;*/

   return false;
}

void HeadlessContainerListener::ChangePasswordResponse(const std::string &clientId, unsigned int id
   , const std::string &walletId, bool ok)
{
/*   headless::ChangePasswordResponse response;
   response.set_rootwalletid(walletId);
   response.set_success(ok);

   headless::RequestPacket packet;
   packet.set_id(id);
   packet.set_authticket(authTicket(clientId).toBinStr());
   packet.set_type(headless::ChangePasswordRequestType);
   packet.set_data(response.SerializeAsString());

   if (!sendData(packet.SerializeAsString(), clientId)) {
      logger_->error("[HeadlessContainerListener] failed to send ChangePassword response");
   }*/
}

void HeadlessContainerListener::AutoSignActiveResponse(const std::string &clientId, const std::string &walletId
   , bool active, const std::string &error, unsigned int id)
{
   headless::SetLimitsResponse response;
   response.set_rootwalletid(walletId);
   response.set_autosignactive(active);
   if (!error.empty()) {
      response.set_error(error);
   }

   headless::RequestPacket packet;
   packet.set_id(id);
   packet.set_type(headless::SetLimitsRequestType);
   packet.set_data(response.SerializeAsString());

   if (!sendData(packet.SerializeAsString(), clientId)) {
      if (clientId.empty()) {
         logger_->warn("[HeadlessContainerListener] failed to multicast SetLimits response");
      }
      else {
         logger_->error("[HeadlessContainerListener] failed to send SetLimits response");
      }
   }
}

bool HeadlessContainerListener::CheckSpendLimit(uint64_t value, bool autoSign, const std::string &walletId)
{
   if (autoSign) {
      if (value > limits_.autoSignSpendXBT) {
         logger_->warn("[HeadlessContainerListener] requested auto-sign spend {} exceeds limit {}", value
            , limits_.autoSignSpendXBT);
         deactivateAutoSign({}, walletId, "spend limit reached");
         return false;
      }
   }
   else {
      if (value > limits_.manualSpendXBT) {
         logger_->warn("[HeadlessContainerListener] requested manual spend {} exceeds limit {}", value
            , limits_.manualSpendXBT);
         return false;
      }
   }
   return true;
}

void HeadlessContainerListener::onXbtSpent(int64_t value, bool autoSign)
{
   if (autoSign) {
      limits_.autoSignSpendXBT -= value;
      logger_->debug("[HeadlessContainerListener] new auto-sign spend limit =  {}", limits_.autoSignSpendXBT);
   }
   else {
      limits_.manualSpendXBT -= value;
      logger_->debug("[HeadlessContainerListener] new manual spend limit =  {}", limits_.manualSpendXBT);
   }
}

void HeadlessContainerListener::activateAutoSign(const std::string &clientId, const std::string &walletId
   , const SecureBinaryData &password)
{
   logger_->info("Activate AutoSign for {}", walletId);

   const auto &wallet = walletId.empty() ? walletsMgr_->getPrimaryWallet() : walletsMgr_->getHDWalletById(walletId);
   if (!wallet) {
      deactivateAutoSign({}, walletId, "wallet missing");
      return;
   }
   if (!wallet->encryptionTypes().empty()) {
      if (password.isNull()) {
         // This will happen when user cancels autosign.
         // Do not send reason text in this case, because it's used as an error message is set.
         deactivateAutoSign({}, walletId, {});
         return;
      }

      throw std::runtime_error("disabled 2");
      /*const auto decrypted = wallet->getRootNode(password);
      if (!decrypted) {
         deactivateAutoSign({}, walletId, "failed to decrypt root node");
         return;
      }*/
   }
   passwords_[wallet->walletId()] = password;
   if (callbacks_) {
      callbacks_->asAct(wallet->walletId());
   }
   AutoSignActiveResponse(clientId, wallet->walletId(), true);
}

void HeadlessContainerListener::deactivateAutoSign(const std::string &clientId, const std::string &walletId
   , const std::string &reason)
{
   logger_->info("Deactivate AutoSign for {} ({})", walletId, reason);

   if (walletId.empty()) {
      passwords_.clear();
   }
   else {
      passwords_.erase(walletId);
   }
   if (callbacks_) {
      callbacks_->asDeact(walletId);
   }
   AutoSignActiveResponse(clientId, walletId, false, reason);
}

bool HeadlessContainerListener::isAutoSignActive(const std::string &walletId) const
{
   if (walletId.empty()) {
      return !passwords_.empty();
   }
   return (passwords_.find(walletId) != passwords_.end());
}

void HeadlessContainerListener::addPendingAutoSignReq(const std::string &walletId)
{
   if (walletId.empty()) {
      autoSignPwdReqs_.insert(walletsMgr_->getPrimaryWallet()->walletId());
   }
   else {
      autoSignPwdReqs_.insert(walletId);
   }
}

void HeadlessContainerListener::walletsListUpdated()
{
   logger_->debug("send WalletsListUpdatedType message");

   headless::RequestPacket packet;
   packet.set_type(headless::WalletsListUpdatedType);
   sendData(packet.SerializeAsString());
}

void HeadlessContainerListener::resetConnection(ServerConnection *connection)
{
   connection_ = connection;
}

static headless::NetworkType mapFrom(NetworkType netType)
{
   switch (netType) {
   case NetworkType::MainNet: return headless::MainNetType;
   case NetworkType::TestNet:
   default:    return headless::TestNetType;
   }
}

bool HeadlessContainerListener::onSyncWalletInfo(const std::string &clientId, headless::RequestPacket packet)
{
   headless::SyncWalletInfoResponse response;

   for (size_t i = 0; i < walletsMgr_->getHDWalletsCount(); ++i) {
      const auto hdWallet = walletsMgr_->getHDWallet(i);
      auto walletData = response.add_wallets();
      walletData->set_format(headless::WalletFormatHD);
      walletData->set_id(hdWallet->walletId());
      walletData->set_name(hdWallet->name());
      walletData->set_description(hdWallet->description());
      walletData->set_nettype(mapFrom(hdWallet->networkType()));
      walletData->set_watching_only(hdWallet->isWatchingOnly());
   }

   packet.set_data(response.SerializeAsString());
   return sendData(packet.SerializeAsString(), clientId);
}

bool HeadlessContainerListener::onSyncHDWallet(const std::string &clientId, headless::RequestPacket packet)
{
   headless::SyncWalletRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[{}] failed to parse request", __func__);
      return false;
   }

   headless::SyncHDWalletResponse response;
   const auto hdWallet = walletsMgr_->getHDWalletById(request.walletid());
   if (hdWallet) {
      response.set_walletid(hdWallet->walletId());
      for (const auto &group : hdWallet->getGroups()) {
         auto groupData = response.add_groups();
         groupData->set_type(group->index());
         groupData->set_ext_only(hdWallet->isExtOnly());

         for (const auto &leaf : group->getLeaves()) {
            auto leafData = groupData->add_leaves();
            leafData->set_id(leaf->walletId());
            leafData->set_index(leaf->index());
         }
      }
   } else {
      logger_->error("[{}] failed to find HD wallet with id {}", __func__, request.walletid());
      return false;
   }

   packet.set_data(response.SerializeAsString());
   return sendData(packet.SerializeAsString(), clientId);
}

static headless::EncryptionType mapFrom(bs::wallet::EncryptionType encType)
{
   switch (encType) {
   case bs::wallet::EncryptionType::Password:   return headless::EncryptionTypePassword;
   case bs::wallet::EncryptionType::Auth:       return headless::EncryptionTypeAutheID;
   case bs::wallet::EncryptionType::Unencrypted:
   default:       return headless::EncryptionTypeUnencrypted;
   }
}

bool HeadlessContainerListener::onSyncWallet(const std::string &clientId, headless::RequestPacket packet)
{
   headless::SyncWalletRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[{}] failed to parse request", __func__);
      return false;
   }

   const auto wallet = walletsMgr_->getWalletById(request.walletid());
   if (!wallet) {
      logger_->error("[{}] failed to find wallet with id {}", __func__, request.walletid());
      return false;
   }

   const auto &lbdSend = [this, wallet, id=packet.id(), clientId]
   {
      headless::SyncWalletResponse response;

      response.set_walletid(wallet->walletId());
      for (const auto &encType : wallet->encryptionTypes()) {
         response.add_encryptiontypes(mapFrom(encType));
      }
      for (const auto &encKey : wallet->encryptionKeys()) {
         response.add_encryptionkeys(encKey.toBinStr());
      }
      auto keyrank = response.mutable_keyrank();
      keyrank->set_m(wallet->encryptionRank().first);
      keyrank->set_n(wallet->encryptionRank().second);

      response.set_nettype(mapFrom(wallet->networkType()));
      response.set_highest_ext_index(wallet->getExtAddressCount());
      response.set_highest_int_index(wallet->getIntAddressCount());

      for (const auto &addr : wallet->getUsedAddressList()) {
         const auto comment = wallet->getAddressComment(addr);
         const auto index = wallet->getAddressIndex(addr);
         auto addrData = response.add_addresses();
         addrData->set_address(addr.display());
         addrData->set_index(index);
         if (!comment.empty()) {
            addrData->set_comment(comment);
         }
      }
      const auto &pooledAddresses = wallet->getPooledAddressList();
      unsigned int poolAddrCnt = 0;
      for (const auto &addr : pooledAddresses) {
         const auto index = wallet->getAddressIndex(addr);
         auto addrData = response.add_addrpool();
         addrData->set_address(addr.display());
         addrData->set_index(index);
      }
      for (const auto &txComment : wallet->getAllTxComments()) {
         auto txCommData = response.add_txcomments();
         txCommData->set_txhash(txComment.first.toBinStr());
         txCommData->set_comment(txComment.second);
      }

      headless::RequestPacket packet;
      packet.set_id(id);
      packet.set_data(response.SerializeAsString());
      packet.set_type(headless::SyncWalletType);
      sendData(packet.SerializeAsString(), clientId);
   };
   std::thread(lbdSend).detach();
   return true;
}

bool HeadlessContainerListener::onSyncComment(const std::string &clientId, headless::RequestPacket packet)
{
   headless::SyncCommentRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[{}] failed to parse request", __func__);
      return false;
   }
   const auto wallet = walletsMgr_->getWalletById(request.walletid());
   if (!wallet) {
      logger_->error("[{}] failed to find wallet with id {}", __func__, request.walletid());
      return false;
   }
   bool rc = false;
   if (!request.address().empty()) {
      rc = wallet->setAddressComment(request.address(), request.comment());
      logger_->debug("[{}] comment for address {} is set: {}", __func__, request.address(), rc);
   }
   else {
      rc = wallet->setTransactionComment(request.txhash(), request.comment());
      logger_->debug("[{}] comment for TX {} is set: {}", __func__, request.txhash(), rc);
   }
   return rc;
}

void HeadlessContainerListener::SyncAddrsResponse(const std::string &clientId
   , unsigned int id, const std::string &walletId, bs::sync::SyncState state)
{
   headless::SyncAddressesResponse response;
   response.set_wallet_id(walletId);
   headless::SyncState respState = headless::SyncState_Failure;
   switch (state) {
   case bs::sync::SyncState::Success:
      respState = headless::SyncState_Success;
      break;
   case bs::sync::SyncState::NothingToDo:
      respState = headless::SyncState_NothingToDo;
      break;
   case bs::sync::SyncState::Failure:
      respState = headless::SyncState_Failure;
      break;
   }
   response.set_state(respState);

   headless::RequestPacket packet;
   packet.set_id(id);
   packet.set_data(response.SerializeAsString());
   packet.set_type(headless::SyncAddressesType);
   sendData(packet.SerializeAsString(), clientId);
}

bool HeadlessContainerListener::onSyncAddresses(const std::string &clientId, headless::RequestPacket packet)
{
   headless::SyncAddressesRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[{}] failed to parse request", __func__);
      return false;
   }
   const auto wallet = walletsMgr_->getWalletById(request.wallet_id());
   if (wallet == nullptr) {
      SyncAddrsResponse(clientId, packet.id(), request.wallet_id(), bs::sync::SyncState::Failure);
      logger_->error("[{}] wallet with ID {} not found", __func__, request.wallet_id());
      return false;
   }

   std::set<BinaryData> addrSet;
   for (int i = 0; i < request.addresses_size(); ++i) {
      addrSet.insert(request.addresses(i));
   }

   //resolve the path and address type for addrSet
   std::map<BinaryData, std::pair<bs::hd::Path, AddressEntryType>> parsedMap;
   try {
      parsedMap = std::move(wallet->indexPathAndTypes(addrSet));
   } catch (AccountException &e) {
      //failure to find even on of the addresses means the wallet chain needs 
      //extended further
      SyncAddrsResponse(clientId, packet.id(), request.wallet_id(), bs::sync::SyncState::Failure);
      logger_->error("[{}] failed to find indices for {} addresses in {}: {}"
         , __func__, addrSet.size(), request.wallet_id(), e.what());
      return false;
   }

   //order addresses by path
   typedef std::map<bs::hd::Path, std::pair<BinaryData, AddressEntryType>> pathMapping;
   std::map<bs::hd::Path::Elem, pathMapping> mapByPath;

   for (auto& parsedPair : parsedMap) {
      auto elem = parsedPair.second.first.get(-2);
      auto& mapping = mapByPath[elem];

      auto addrPair = std::make_pair(parsedPair.first, parsedPair.second.second);
      mapping.insert(std::make_pair(parsedPair.second.first, addrPair));
   }

   //strip out addresses using the wallet's default type
   for (auto& mapping : mapByPath) {
      auto& addrMap = mapping.second;
      auto iter = addrMap.begin();
      while (iter != addrMap.end()) {
         if (iter->second.second == AddressEntryType_Default) {
            auto eraseIter = iter++;

            /*
            Do not erase this default address if it's the last one in
            the map. The default address type is not a significant piece
            of data to synchronize a wallet's used address chain length,
            however the last instantiated address is relevant, regardless
            of its type
            */

            if (iter != addrMap.end())
               addrMap.erase(eraseIter);

            continue;
         }
         ++iter;
      }
   }

   //request each chain for the relevant address types
   bool update = false;
   for (auto& mapping : mapByPath) {
      for (auto& pathPair : mapping.second) {
         auto resultPair = wallet->synchronizeUsedAddressChain(
            pathPair.first.toString(), pathPair.second.second);
         update |= resultPair.second;
      }
   }

   if (update)
      SyncAddrsResponse(clientId, packet.id(), request.wallet_id(), bs::sync::SyncState::Success);
   else
      SyncAddrsResponse(clientId, packet.id(), request.wallet_id(), bs::sync::SyncState::NothingToDo);
   return true;
}

bool HeadlessContainerListener::onExtAddrChain(const std::string &clientId, headless::RequestPacket packet)
{
   headless::ExtendAddressChainRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[{}] failed to parse request", __func__);
      return false;
   }
   const auto wallet = walletsMgr_->getWalletById(request.wallet_id());
   if (wallet == nullptr) {
      logger_->error("[{}] wallet with ID {} not found", __func__, request.wallet_id());
      return false;
   }

   const auto &lbdSend = [this, wallet, request, id=packet.id(), clientId] {
      headless::ExtendAddressChainResponse response;
      response.set_wallet_id(wallet->walletId());

      auto&& newAddrVec = wallet->extendAddressChain(request.count(), request.ext_int());
      for (const auto &addr : newAddrVec) {
         auto &&index = wallet->getAddressIndex(addr);
         auto addrData = response.add_addresses();
         addrData->set_address(addr.display());
         addrData->set_index(index);
         logger_->debug("[{}] {} = {}", __func__, index, addr.display());
      }

      headless::RequestPacket packet;
      packet.set_id(id);
      packet.set_data(response.SerializeAsString());
      packet.set_type(headless::ExtendAddressChainType);
      sendData(packet.SerializeAsString(), clientId);
      logger_->debug("[{}] data sent", __func__);
   };
   std::thread(lbdSend).detach();
   return true;
}

bool HeadlessContainerListener::onExecCustomDialog(const std::string &clientId, headless::RequestPacket packet)
{
   headless::CustomDialogRequest request;
   if (!request.ParseFromString(packet.data())) {
      logger_->error("[HeadlessContainerListener] failed to parse CustomDialogRequest");
      return false;
   }

   if (callbacks_) {
      callbacks_->customDialog(request.dialogname(), request.variantdata());

//      QByteArray ba = QByteArray::fromStdString(request.variantdata());
//      QDataStream ds(&ba, QIODevice::ReadOnly);
//      QVariant data;
//      ds >> data;

//      cbCustomDialog_(QString::fromStdString(request.dialogname()), data);
   }
   return true;
}
