#ifndef CHAT_WIDGET_H
#define CHAT_WIDGET_H

#include <memory>
#include <QPointer>
#include <QWidget>
#include "ChatProtocol/ChatClientService.h"
#include "ChatProtocol/ClientParty.h"
#include "OtcTypes.h"

class QItemSelection;

class AbstractChatWidgetState;
class AuthAddressManager;
class ArmoryConnection;
class ChatPartiesTreeModel;
class OTCRequestViewModel;
class SignContainer;
class WalletsM;
class ChatOTCHelper;
class OTCWindowsManager;
class MarketDataProvider;
class AssetManager;
class BaseCelerClient;

namespace Ui {
   class ChatWidget;
}

namespace bs {
   namespace sync {
      class WalletsManager;
   }
}

class ChatWidget : public QWidget
{
   Q_OBJECT

public:
   explicit ChatWidget(QWidget* parent = nullptr);
   ~ChatWidget() override;

   void init(const std::shared_ptr<ConnectionManager>& connectionManager
      , const std::shared_ptr<ApplicationSettings>& appSettings
      , const Chat::ChatClientServicePtr& chatClientServicePtr
      , const std::shared_ptr<spdlog::logger>& loggerPtr
      , const std::shared_ptr<bs::sync::WalletsManager> &walletsMgr
      , const std::shared_ptr<AuthAddressManager> &authManager
      , const std::shared_ptr<ArmoryConnection> &armory
      , const std::shared_ptr<SignContainer> &signContainer
      , const std::shared_ptr<MarketDataProvider>& mdProvider
      , const std::shared_ptr<AssetManager>& assetManager
      , const std::shared_ptr<BaseCelerClient> &celerClient
   );

   std::string login(const std::string& email, const std::string& jwt, const ZmqBipNewKeyCb&);

   bs::network::otc::Peer *currentPeer() const;

protected:
   void showEvent(QShowEvent* e) override;
   bool eventFilter(QObject* sender, QEvent* event) override;

public slots:
   void onProcessOtcPbMessage(const std::string& data);
   void onSendOtcMessage(const std::string& contactId, const BinaryData& data);
   void onSendOtcPublicMessage(const BinaryData& data);

   void onNewChatMessageTrayNotificationClicked(const QString& partyId);
   void onUpdateOTCShield();

private slots:
   void onPartyModelChanged();
   void onLogin();
   void onLogout();
   void onSendMessage();
   void onMessageRead(const std::string& partyId, const std::string& messageId);
   void onSendArrived(const Chat::MessagePtrList& messagePtr);
   void onClientPartyStatusChanged(const Chat::ClientPartyPtr& clientPartyPtr);
   void onMessageStateChanged(const std::string& partyId, const std::string& message_id, const int party_message_state);
   void onUserListClicked(const QModelIndex& index);
   void onActivatePartyId(const QString& partyId);
   void onActivateGlobalPartyId();
   void onActivateCurrentPartyId();
   void onRegisterNewChangingRefresh();
   void onShowUserRoom(const QString& userHash);
   void onContactFriendRequest(const QString& userHash);
   void onSetDisplayName(const std::string& partyId, const std::string& contactName);
   void onUserPublicKeyChanged(const Chat::UserPublicKeyInfoList& userPublicKeyInfoList);
   void onConfirmContactNewKeyData(const Chat::UserPublicKeyInfoList& userPublicKeyInfoList, bool bForceUpdateAllUsers);

   void onOtcRequestCurrentChanged(const QModelIndex &current, const QModelIndex &previous);

   void onContactRequestAcceptClicked(const std::string& partyId);
   void onContactRequestRejectClicked(const std::string& partyId);
   void onContactRequestSendClicked(const std::string& partyId);
   void onContactRequestCancelClicked(const std::string& partyId);

   void onNewPartyRequest(const std::string& userName);
   void onRemovePartyRequest(const std::string& partyId);

   void onOtcUpdated(const bs::network::otc::Peer *peer);
   void onOtcPublicUpdated();
   void onOTCPeerError(const bs::network::otc::Peer *peer, const std::string &errorMsg);

   void onOtcRequestSubmit();
   void onOtcResponseAccept();
   void onOtcResponseUpdate();
   void onOtcQuoteRequestSubmit();
   void onOtcQuoteResponseSubmit();
   void onOtcPullOrRejectCurrent();
   void onOtcPullOrReject(const std::string& contactId, bs::network::otc::PeerType type);

signals:
   // OTC
   void sendOtcPbMessage(const std::string& data);
   void chatRoomChanged();
   void requestPrimaryWalletCreation();

private:
   friend class AbstractChatWidgetState;
   friend class ChatLogOutState;
   friend class IdleState;
   friend class PrivatePartyInitState;
   friend class PrivatePartyUninitState;
   friend class PrivatePartyRequestedOutgoingState;
   friend class PrivatePartyRequestedIncomingState;
   friend class PrivatePartyRejectedState;

   template <typename stateType, typename = typename std::enable_if<std::is_base_of<AbstractChatWidgetState, stateType>::value>::type>
      void changeState(std::function<void(void)>&& transitionChanges = []() {})
      {
         // Exit previous state
         stateCurrent_.reset();

         // Enter new state
         transitionChanges();
         stateCurrent_ = std::make_unique<stateType>(this);
         stateCurrent_->applyState();
      }

protected:
   std::unique_ptr<AbstractChatWidgetState> stateCurrent_;

private:
   void chatTransition(const Chat::ClientPartyPtr& clientPartyPtr);

   QScopedPointer<Ui::ChatWidget> ui_;
   Chat::ChatClientServicePtr    chatClientServicePtr_;
   OTCRequestViewModel* otcRequestViewModel_ = nullptr;
   QPointer<ChatOTCHelper> otcHelper_{};
   std::shared_ptr<spdlog::logger>  loggerPtr_;
   std::shared_ptr<BaseCelerClient> celerClient_;
   std::shared_ptr<ChatPartiesTreeModel> chatPartiesTreeModel_;
   std::shared_ptr<OTCWindowsManager> otcWindowsManager_{};

   std::string ownUserId_;
   std::string currentPartyId_;
   QMap<std::string, QString> draftMessages_;
   bool bNeedRefresh_ = false;
};

#endif // CHAT_WIDGET_H
