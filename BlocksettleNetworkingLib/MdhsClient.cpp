#include "MdhsClient.h"

#include "ApplicationSettings.h"
#include "ConnectionManager.h"
#include "FastLock.h"
#include "RequestReplyCommand.h"

#include "market_data_history.pb.h"

#include <spdlog/logger.h>

// Private Market
const std::string ANT_XBT = "ANT/XBT";
const std::string BLK_XBT = "BLK/XBT";
const std::string BSP_XBT = "BSP/XBT";
const std::string JAN_XBT = "JAN/XBT";
const std::string SCO_XBT = "SCO/XBT";
// Spot XBT
const std::string XBT_EUR = "XBT/EUR";
const std::string XBT_GBP = "XBT/GBP";
const std::string XBT_JPY = "XBT/JPY";
const std::string XBT_SEK = "XBT/SEK";
// Spot FX
const std::string EUR_GBP = "EUR/GBP";
const std::string EUR_JPY = "EUR/JPY";
const std::string EUR_SEK = "EUR/SEK";
const std::string GPB_JPY = "GPB/JPY";
const std::string GBP_SEK = "GBP/SEK";
const std::string JPY_SEK = "JPY/SEK";


MdhsClient::MdhsClient(
	const std::shared_ptr<ApplicationSettings>& appSettings,
	const std::shared_ptr<ConnectionManager>& connectionManager,
	const std::shared_ptr<spdlog::logger>& logger,
	QObject* pParent)
	: QObject(pParent)
	, appSettings_(appSettings)
	, connectionManager_(connectionManager)
	, logger_(logger)
{
}

MdhsClient::~MdhsClient()
{
	FastLock locker(lockCommands_);
	for (auto &cmd : activeCommands_) {
		cmd->DropResult();
	}
}

void MdhsClient::SendRequest(const MarketDataHistoryRequest& request)
{
	const auto apiConnection = connectionManager_->CreateGenoaClientConnection();
	auto command = std::make_shared<RequestReplyCommand>("MdhsClient", apiConnection, logger_);

	command->SetReplyCallback([command, this](const std::string& data) -> bool
	{
		command->CleanupCallbacks();
		FastLock locker(lockCommands_);
		activeCommands_.erase(command);
		return OnDataReceived(data);
	});

	command->SetErrorCallback([command, this](const std::string& message)
	{
		logger_->error("Failed to get history data from mdhs: {}", message);
		command->CleanupCallbacks();
		FastLock locker(lockCommands_);
		activeCommands_.erase(command);
	});

	FastLock locker(lockCommands_);
	activeCommands_.emplace(command);

	if (!command->ExecuteRequest(
		appSettings_->get<std::string>(ApplicationSettings::mdhsHost),
		appSettings_->get<std::string>(ApplicationSettings::mdhsPort),
		request.SerializeAsString()))
	{
		logger_->error("Failed to send request for mdhs.");
		command->CleanupCallbacks();
		FastLock locker(lockCommands_);
		activeCommands_.erase(command);
	}
}

bool MdhsClient::OnDataReceived(const std::string& data)
{
	emit DataReceived(data);
	return true;
}
