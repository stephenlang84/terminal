#ifndef ClientPartyModel_h__
#define ClientPartyModel_h__

#include <QObject>
#include <memory>
#include <unordered_map>
#include <vector>

#include "ChatProtocol/PartyModel.h"
#include "ChatProtocol/ClientParty.h"

namespace spdlog
{
   class logger;
}

namespace Chat
{
   enum class ClientPartyModelError
   {
      DynamicPointerCast,
      UserNameNotFound
   };

   using LoggerPtr = std::shared_ptr<spdlog::logger>;

   class ClientPartyModel : public PartyModel
   {
      Q_OBJECT
   public:
      ClientPartyModel(const LoggerPtr& loggerPtr, QObject* parent = nullptr);
      IdPartyList getIdPartyList() const;
      ClientPartyPtr getPartyByUserName(const std::string& userName);

   signals:
      void error(const ClientPartyModelError& errorCode, const std::string& what = "");

   private slots:
      void handleLocalErrors(const ClientPartyModelError& errorCode, const std::string& what);
   };

   using ClientPartyModelPtr = std::shared_ptr<ClientPartyModel>;

}

#endif // ClientPartyModel_h__
