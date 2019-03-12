#include "ChatUserModel.h"
#include "UserHasher.h"

ChatUserModel::ChatUserModel(QObject *parent) : QObject(parent)
{
   hasher_ = std::make_shared<UserHasher>();
}

void ChatUserModel::addUser(const ChatUserDataPtr &chatUserDataPtr)
{
   if (isChatUserExist(chatUserDataPtr->userId()))
      return;

   chatUserDataListPtr_.push_back(chatUserDataPtr);

   emit chatUserAdded(chatUserDataPtr);
   emit chatUserDataListChanged(chatUserDataListPtr_);
}

void ChatUserModel::removeUser(const ChatUserDataPtr &chatUserDataPtr)
{
   removeByUserId(chatUserDataPtr->userId());
}

void ChatUserModel::removeByUserId(const QString &userId)
{
   ChatUserDataPtr chatUserDataPtr = getUserByUserId(userId);
   if (!chatUserDataPtr)
   {
      return;
   }

   chatUserDataListPtr_.erase(
      std::remove_if(std::begin(chatUserDataListPtr_), std::end(chatUserDataListPtr_),
      [chatUserDataPtr](const ChatUserDataPtr cudPtr)
   {
      return cudPtr && (cudPtr == chatUserDataPtr);
   }));

   emit chatUserRemoved(chatUserDataPtr);
   emit chatUserDataListChanged(chatUserDataListPtr_);
}

bool ChatUserModel::isChatUserExist(const QString &userId) const
{
   ChatUserDataPtr chatUserDataPtr = getUserByUserId(userId);

   if (chatUserDataPtr)
   {
      return true;
   }

   return false;
}

bool ChatUserModel::hasUnreadMessages() const
{
   ChatUserDataPtr chatUserDataPtr;
   foreach( chatUserDataPtr, chatUserDataListPtr_ )
   {
      if (chatUserDataPtr->haveNewMessage()) {
         return true;
      }
   }

   return false;
}

void ChatUserModel::setUserStatus(const QString &userId, const ChatUserData::ConnectionStatus &userStatus)
{
   ChatUserDataPtr chatUserDataPtr = getUserByUserId(userId);

   if (!chatUserDataPtr)
   {
      return;
   }

   chatUserDataPtr->setUserConnectionStatus(userStatus);

   emit chatUserStatusChanged(chatUserDataPtr);
   emit chatUserDataListChanged(chatUserDataListPtr_);
}

void ChatUserModel::setUserState(const QString &userId, const ChatUserData::State &userState)
{
   ChatUserDataPtr chatUserDataPtr = getUserByUserId(userId);

   if (!chatUserDataPtr)
   {
      return;
   }

   chatUserDataPtr->setUserState(userState);

   emit chatUserStateChanged(chatUserDataPtr);
   emit chatUserDataListChanged(chatUserDataListPtr_);
}

void ChatUserModel::setUserHaveNewMessage(const QString &userId, const bool &haveNewMessage) {
   ChatUserDataPtr chatUserDataPtr = getUserByUserId(userId);

   if (!chatUserDataPtr)
   {
      return;
   }

   chatUserDataPtr->setHaveNewMessage(haveNewMessage);

   emit chatUserHaveNewMessageChanged(chatUserDataPtr);
   emit chatUserDataListChanged(chatUserDataListPtr_);
}

ChatUserDataPtr ChatUserModel::getUserByUserId(const QString &userId) const
{
   auto chatUserIt = std::find_if (std::begin(chatUserDataListPtr_), std::end(chatUserDataListPtr_), [userId](const ChatUserDataPtr &chatUserDataPtr)->bool
   {
      return (0 == chatUserDataPtr->userId().compare(userId));
   });

   if (chatUserIt == std::end(chatUserDataListPtr_))
   {
      return ChatUserDataPtr();
   }

   ChatUserDataPtr chatUserDataPtr((*chatUserIt));

   return chatUserDataPtr;
}

ChatUserDataPtr ChatUserModel::getUserByUserIdPrefix(const QString &userIdPrefix) const
{
   auto chatUserIt = std::find_if (std::begin(chatUserDataListPtr_), std::end(chatUserDataListPtr_), [userIdPrefix](const ChatUserDataPtr &chatUserDataPtr)->bool
   {
      return (true == chatUserDataPtr->userId().startsWith(userIdPrefix));
   });

   if (chatUserIt == std::end(chatUserDataListPtr_))
   {
      return ChatUserDataPtr();
   }

   ChatUserDataPtr chatUserDataPtr((*chatUserIt));

   return chatUserDataPtr;
}

ChatUserDataPtr ChatUserModel::getUserByEmail(const QString &email) const
{
   QString userId = QString::fromStdString(hasher_->deriveKey(email.toStdString()));
   auto chatUserIt = std::find_if (std::begin(chatUserDataListPtr_), std::end(chatUserDataListPtr_), [userId](const ChatUserDataPtr &chatUserDataPtr)->bool
   {
      return (0 == chatUserDataPtr->userId().compare(userId));
   });

   if (chatUserIt == std::end(chatUserDataListPtr_))
   {
      return ChatUserDataPtr();
   }

   ChatUserDataPtr chatUserDataPtr((*chatUserIt));

   return chatUserDataPtr;
}

void ChatUserModel::resetModel()
{
   while(!chatUserDataListPtr_.empty())
   {
      ChatUserDataPtr chatUserDataPtr = chatUserDataListPtr_.back();
      chatUserDataListPtr_.pop_back();
      emit chatUserRemoved(chatUserDataPtr);
   }

   emit chatUserDataListChanged(chatUserDataListPtr_);
}

ChatUserDataListPtr ChatUserModel::chatUserDataList() const
{
   return chatUserDataListPtr_;
}

bool ChatUserModel::isChatUserInContacts(const QString &userId) const
{
   ChatUserDataPtr chatUserDataPtr = getUserByUserId(userId);

   if (!chatUserDataPtr)
   {
      return false;
   }

   // any state exept unknown belongs to contacts
   return (ChatUserData::State::Unknown != chatUserDataPtr->userState());
}
