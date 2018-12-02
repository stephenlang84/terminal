#include "BSTerminalMainWindow.h"
#include "ui_BSTerminalMainWindow.h"
#include "moc_BSTerminalMainWindow.cpp"

#include <QApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QGuiApplication>
#include <QIcon>
#include <QShortcut>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QToolBar>
#include <QTreeView>

#include <thread>

#include "AboutDialog.h"
#include "AssetManager.h"
#include "AuthAddressDialog.h"
#include "AuthAddressManager.h"
#include "BSMarketDataProvider.h"
#include "BSTerminalSplashScreen.h"
#include "CCFileManager.h"
#include "CCPortfolioModel.h"
#include "CCTokenEntryDialog.h"
#include "CelerAccountInfoDialog.h"
#include "CelerMarketDataProvider.h"
#include "ConfigDialog.h"
#include "ConnectionManager.h"
#include "CreateTransactionDialogAdvanced.h"
#include "CreateTransactionDialogSimple.h"
#include "DialogManager.h"
#include "EnterWalletPassword.h"
#include "HDWallet.h"
#include "HeadlessContainer.h"
#include "LoginWindow.h"
#include "MarketDataProvider.h"
#include "MDAgreementDialog.h"
#include "BSMessageBox.h"
#include "NewAddressDialog.h"
#include "NewWalletDialog.h"
#include "NotificationCenter.h"
#include "OTPFileInfoDialog.h"
#include "OTPImportDialog.h"
#include "OTPManager.h"
#include "QuoteProvider.h"
#include "RequestReplyCommand.h"
#include "SelectWalletDialog.h"
#include "SignContainer.h"
#include "StatusBarView.h"
#include "TabWithShortcut.h"
#include "UiUtils.h"
#include "WalletsManager.h"
#include "ZmqSecuredDataConnection.h"

#include <spdlog/spdlog.h>

BSTerminalMainWindow::BSTerminalMainWindow(const std::shared_ptr<ApplicationSettings>& settings, BSTerminalSplashScreen& splashScreen, QWidget* parent)
   : QMainWindow(parent)
   , ui(new Ui::BSTerminalMainWindow())
   , applicationSettings_(settings)
   , walletsManager_(nullptr)
{
   UiUtils::SetupLocale();

   ui->setupUi(this);

   setupShortcuts();

   if (!applicationSettings_->get<bool>(ApplicationSettings::initialized)) {
      applicationSettings_->SetDefaultSettings(true);
   }

   auto geom = settings->get<QRect>(ApplicationSettings::GUI_main_geometry);
   if (!geom.isEmpty()) {
      setGeometry(geom);
   }

   connect(ui->action_Quit, &QAction::triggered, qApp, &QCoreApplication::quit);

   logMgr_ = std::make_shared<bs::LogManager>([] { KillHeadlessProcess(); });
   logMgr_->add(applicationSettings_->GetLogsConfig());

   logMgr_->logger()->debug("Settings loaded from {}", applicationSettings_->GetSettingsPath().toStdString());

   setupIcon();
   UiUtils::setupIconFont(this);
   NotificationCenter::createInstance(applicationSettings_, ui.get(), sysTrayIcon_, this);

   InitConnections();

   initArmory();

   otpManager_ = std::make_shared<OTPManager>(logMgr_->logger(), applicationSettings_, celerConnection_);
   connect(otpManager_.get(), &OTPManager::SyncCompleted, this, &BSTerminalMainWindow::OnOTPSyncCompleted);

   InitSigningContainer();
   LoadWallets(splashScreen);
   QApplication::processEvents();

   InitAuthManager();
   InitAssets();

   authAddrDlg_ = std::make_shared<AuthAddressDialog>(logMgr_->logger(), authManager_
      , assetManager_, otpManager_, applicationSettings_, this);

   statusBarView_ = std::make_shared<StatusBarView>(armory_, walletsManager_, assetManager_, celerConnection_
      , signContainer_, ui->statusbar);

   InitWalletsView();
   setupToolbar();
   setupMenu();

   ui->widgetTransactions->setEnabled(false);

   if (!signContainer_->Start()) {
      BSMessageBox(BSMessageBox::warning, tr("BlockSettle Signer"), tr("Failed to start local signer process")).exec();
   }

   connectArmory();
   splashScreen.SetProgress(100);

   InitPortfolioView();

   ui->widgetRFQ->initWidgets(mdProvider_, applicationSettings_);

   aboutDlg_ = std::make_shared<AboutDialog>(applicationSettings_->get<QString>(ApplicationSettings::ChangeLog_Base_Url), this);
   auto aboutDlgCb = [this] (int tab) {
      return [this, tab]() {
         aboutDlg_->setTab(tab);
         aboutDlg_->show();
      };
   };
   connect(ui->action_About_BlockSettle, &QAction::triggered, aboutDlgCb(0));
   connect(ui->actionAbout_the_Terminal, &QAction::triggered, aboutDlgCb(1));
   connect(ui->action_Contact_BlockSettle, &QAction::triggered, aboutDlgCb(2));
   connect(ui->action_Version, &QAction::triggered, aboutDlgCb(3));

   // Enable/disable send action when first wallet created/last wallet removed
   connect(walletsManager_.get(), &WalletsManager::walletChanged, this, &BSTerminalMainWindow::updateControlEnabledState);
   connect(walletsManager_.get(), &WalletsManager::newWalletAdded, this, &BSTerminalMainWindow::updateControlEnabledState);

   ui->tabWidget->setCurrentIndex(settings->get<int>(ApplicationSettings::GUI_main_tab));

   ui->widgetTransactions->setAppSettings(applicationSettings_);

   UpdateMainWindowAppearence();
}

void BSTerminalMainWindow::GetNetworkSettingsFromPuB(const std::function<void(std::map<NetworkSettingType, std::pair<std::string, unsigned int>>)> &cb)
{
   if (!networkSettings_.empty()) {
      cb(networkSettings_);
      return;
   }

   Blocksettle::Communication::RequestPacket reqPkt;
   reqPkt.set_requesttype(Blocksettle::Communication::GetNetworkSettingsType);
   reqPkt.set_requestdata("");

   const auto connection = connectionManager_->CreateSecuredDataConnection();
   connection->SetServerPublicKey(applicationSettings_->get<std::string>(ApplicationSettings::pubBridgePubKey));
   cmdPuBSettings_ = std::make_shared<RequestReplyCommand>("network_settings", connection, logMgr_->logger());
   const auto &title = tr("Network settings");

   const auto &populateAppSettings = [this](std::map<NetworkSettingType, std::pair<std::string, unsigned int>> settings) {
      for (const auto &setting : settings) {
         switch (setting.first) {
         case NetworkSettingType::Celer:
            applicationSettings_->set(ApplicationSettings::celerHost, QString::fromStdString(setting.second.first));
            applicationSettings_->set(ApplicationSettings::celerPort, setting.second.second);
            break;
         case NetworkSettingType::MarketData:
            applicationSettings_->set(ApplicationSettings::mdServerHost, QString::fromStdString(setting.second.first));
            applicationSettings_->set(ApplicationSettings::mdServerPort, setting.second.second);
            break;
         default:
            logMgr_->logger()->info("[GetNetworkSettingsFromPuB] unknown network setting {}", (int)setting.first);
            break;
         }
      }
   };

   cmdPuBSettings_->SetReplyCallback([this, title, cb, populateAppSettings](const std::string &data) {
      if (data.empty()) {
         showError(title, tr("Empty reply from BlockSettle server"));
      }
      Blocksettle::Communication::GetNetworkSettingsResponse response;
      if (!response.ParseFromString(data)) {
         showError(title, tr("Invalid reply from BlockSettle server"));
         return false;
      }
      for (int i = 0; i < response.settings_size(); ++i) {
         const auto &setting = response.settings(i);
         networkSettings_[static_cast<NetworkSettingType>(setting.type())] = {setting.host(), setting.port()};
      }

      if (networkSettings_.empty()) {
         showError(title, tr("Empty network settings received from BlockSettle server"));
         return false;
      }
      if (networkSettings_.find(NetworkSettingType::Celer) == networkSettings_.end()) {
         showError(title, tr("Missing Celer connection settings"));
         return false;
      }
      if (networkSettings_.find(NetworkSettingType::MarketData) == networkSettings_.end()) {
         showError(title, tr("Missing Market Data server connection settings"));
         return false;
      }
      logMgr_->logger()->debug("[GetNetworkSettingsFromPuB] received {} network settings", networkSettings_.size());
      populateAppSettings(networkSettings_);
      cb(networkSettings_);
      return true;
   });
   cmdPuBSettings_->SetErrorCallback([this, title](const std::string& message) {
      logMgr_->logger()->error("[GetNetworkSettingsFromPuB] error: {}", message);
      showError(title, tr("Failed to obtain network settings from BlockSettle server"));
   });

   if (!cmdPuBSettings_->ExecuteRequest(applicationSettings_->get<std::string>(ApplicationSettings::pubBridgeHost)
      , applicationSettings_->get<std::string>(ApplicationSettings::pubBridgePort)
      , reqPkt.SerializeAsString())) {
      logMgr_->logger()->error("[GetNetworkSettingsFromPuB] failed to send request");
      showError(title, tr("Failed to retrieve network settings due to invalid connection to BlockSettle server"));
   }
}

void BSTerminalMainWindow::postSplashscreenActions()
{
   if (applicationSettings_->get<bool>(ApplicationSettings::SubscribeToMDOnStart)) {
      GetNetworkSettingsFromPuB([this](std::map<NetworkSettingType, std::pair<std::string, unsigned int>> networkSettings) {
         const auto &itSettings = networkSettings.find(NetworkSettingType::MarketData);
         if (itSettings != networkSettings.end()) {
            const auto &setPair = itSettings->second;
            mdProvider_->SubscribeToMD(setPair.first, std::to_string(setPair.second));
         }
         else {
            showError(tr("Market Data Connection"), tr("MD connection settings not found in reply from BlockSettle server"));
         }
      });
   }
}

BSTerminalMainWindow::~BSTerminalMainWindow()
{
   applicationSettings_->set(ApplicationSettings::GUI_main_geometry, geometry());
   applicationSettings_->set(ApplicationSettings::GUI_main_tab, ui->tabWidget->currentIndex());
   applicationSettings_->SaveSettings();

   NotificationCenter::destroyInstance();
   if (signContainer_) {
      signContainer_->Stop();
      signContainer_ = nullptr;
   }
   walletsManager_ = nullptr;
   assetManager_ = nullptr;
   bs::UtxoReservation::destroy();
}

void BSTerminalMainWindow::setupToolbar()
{
   QIcon lockbox_icon = UiUtils::icon(0xe774);
   QIcon offline_icon = UiUtils::icon(0xe77f);
   QIcon create_wallet_icon = UiUtils::icon(0xe67a);
   QIcon wallet_properties_icon = UiUtils::icon(0xe6c3);
   QIcon import_wallet_icon = UiUtils::icon(0xe765);

   action_send_ = new QAction(tr("Create &Transaction"), this);
   connect(action_send_, &QAction::triggered, this, &BSTerminalMainWindow::onSend);
   action_receive_ = new QAction(tr("Generate &Address"), this);
   connect(action_receive_, &QAction::triggered, this, &BSTerminalMainWindow::onReceive);

   action_login_ = new QAction(tr("Login to BlockSettle"), this);
   connect(action_login_, &QAction::triggered, this, &BSTerminalMainWindow::onLogin);

   action_logout_ = new QAction(tr("Logout from BlockSettle"), this);
   connect(action_logout_, &QAction::triggered, this, &BSTerminalMainWindow::onLogout);

   auto toolBar = new QToolBar(this);
   toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
   ui->tabWidget->setCornerWidget(toolBar, Qt::TopRightCorner);

   // send bitcoins
   toolBar->addAction(action_send_);
   // receive bitcoins
   toolBar->addAction(action_receive_);

   action_logout_->setVisible(false);

   connect(ui->pushButtonUser, &QPushButton::clicked, this, &BSTerminalMainWindow::onButtonUserClicked);

   QMenu* trayMenu = new QMenu(this);
   QAction* trayShowAction = trayMenu->addAction(tr("&Open Terminal"));
   connect(trayShowAction, &QAction::triggered, this, &QMainWindow::show);
   trayMenu->addSeparator();

   trayMenu->addAction(action_send_);
   trayMenu->addAction(action_receive_);
   trayMenu->addAction(ui->actionSettings);

   trayMenu->addSeparator();
   trayMenu->addAction(ui->action_Quit);
   sysTrayIcon_->setContextMenu(trayMenu);

   updateControlEnabledState();
}

void BSTerminalMainWindow::setupIcon()
{
   QIcon icon;
   QString iconFormatString = QString::fromStdString(":/ICON_BS_%1");

   for (const int s : {16, 24, 32}) {
      icon.addFile(iconFormatString.arg(s), QSize(s, s));
   }

   setWindowIcon(icon);

   sysTrayIcon_ = std::make_shared<QSystemTrayIcon>(icon, this);
   sysTrayIcon_->setToolTip(windowTitle());
   sysTrayIcon_->show();

   connect(sysTrayIcon_.get(), &QSystemTrayIcon::activated, [this](QSystemTrayIcon::ActivationReason reason) {
      if (reason == QSystemTrayIcon::Context) {
         // Right click, this is handled by the menu, so we don't do anything here.
         return;
      }

      setWindowState(windowState() & ~Qt::WindowMinimized);
      show();
      raise();
      activateWindow();
   });

   connect(qApp, &QCoreApplication::aboutToQuit, sysTrayIcon_.get(), &QSystemTrayIcon::hide);
   connect(qApp, SIGNAL(lastWindowClosed()), sysTrayIcon_.get(), SLOT(hide()));
}

void BSTerminalMainWindow::LoadWallets(BSTerminalSplashScreen& splashScreen)
{
   logMgr_->logger()->debug("Loading wallets");
   splashScreen.SetTipText(tr("Loading wallets"));
   splashScreen.SetProgress(5);

   bs::UtxoReservation::init();

   WalletsManager::load_progress_delegate progressDelegate = [&](int progress)
   {
      splashScreen.SetProgress(progress);
   };

   walletsManager_ = std::make_shared<WalletsManager>(logMgr_->logger(), applicationSettings_, armory_);

   connect(walletsManager_.get(), &WalletsManager::walletsReady, [this] {
      ui->widgetRFQ->SetWalletsManager(walletsManager_);
      ui->widgetRFQReply->SetWalletsManager(walletsManager_);
   });
   connect(walletsManager_.get(), &WalletsManager::info, this, &BSTerminalMainWindow::showInfo);
   connect(walletsManager_.get(), &WalletsManager::error, this, &BSTerminalMainWindow::showError);

   walletsManager_->LoadWallets(applicationSettings_->get<NetworkType>(ApplicationSettings::netType)
      , applicationSettings_->GetHomeDir(), progressDelegate);

   if (signContainer_->opMode() != SignContainer::OpMode::Offline) {
      addrSyncer_ = std::make_shared<HeadlessAddressSyncer>(signContainer_, walletsManager_);
      connect(signContainer_.get(), &SignContainer::UserIdSet, [this] {
         addrSyncer_->SyncWallet(walletsManager_->GetAuthWallet());
      });
   }

   logMgr_->logger()->debug("End of wallets loading");
}

void BSTerminalMainWindow::InitAuthManager()
{
   authManager_ = std::make_shared<AuthAddressManager>(logMgr_->logger(), armory_);
   authManager_->init(applicationSettings_, walletsManager_, otpManager_); //? should be merged to constructor later
   authManager_->SetSigningContainer(signContainer_);    //? ditto

   connect(authManager_.get(), &AuthAddressManager::NeedVerify, this, &BSTerminalMainWindow::openAuthDlgVerify);
   connect(authManager_.get(), &AuthAddressManager::AddrStateChanged, [](const QString &addr, const QString &state) {
      NotificationCenter::notify(bs::ui::NotifyType::AuthAddress, { addr, state });
   });
   connect(authManager_.get(), &AuthAddressManager::ConnectionComplete, this, &BSTerminalMainWindow::onAuthMgrConnComplete);
   connect(authManager_.get(), &AuthAddressManager::AuthWalletCreated, [this](const QString &) {
      if (authAddrDlg_) {
         openAuthManagerDialog();
      }
   });
}

void BSTerminalMainWindow::InitSigningContainer()
{
   signContainer_ = CreateSigner(logMgr_->logger(), applicationSettings_, connectionManager_);
   if (!signContainer_) {
      showError(tr("Signer"), tr("Creation failure"));
      return;
   }
   connect(signContainer_.get(), &SignContainer::ready, this, &BSTerminalMainWindow::SignerReady);
}

void BSTerminalMainWindow::SignerReady()
{
   if (signContainer_->hasUI()) {
      disconnect(signContainer_.get(), &SignContainer::PasswordRequested, this, &BSTerminalMainWindow::onPasswordRequested);
   }
   else {
      connect(signContainer_.get(), &SignContainer::PasswordRequested, this, &BSTerminalMainWindow::onPasswordRequested);
   }

   if (!widgetsInited_) {
      auto quoteProvider = std::make_shared<QuoteProvider>(assetManager_, logMgr_->logger("message"));
      quoteProvider->ConnectToCelerClient(celerConnection_);

      auto dialogManager = std::make_shared<DialogManager>(geometry());

      ui->widgetRFQ->init(logMgr_->logger(), celerConnection_, authManager_, quoteProvider, assetManager_
         , dialogManager, signContainer_, armory_);
      ui->widgetRFQReply->init(logMgr_->logger(), celerConnection_, authManager_, quoteProvider, mdProvider_, assetManager_
         , applicationSettings_, dialogManager, signContainer_, armory_);
      widgetsInited_ = true;
   }
   else {
      signContainer_->SetUserId(BinaryData::CreateFromHex(celerConnection_->userId()));
   }
}

void BSTerminalMainWindow::InitConnections()
{
   connectionManager_ = std::make_shared<ConnectionManager>(logMgr_->logger("message"));
   celerConnection_ = std::make_shared<CelerClient>(connectionManager_);
   connect(celerConnection_.get(), &CelerClient::OnConnectedToServer, this, &BSTerminalMainWindow::onCelerConnected);
   connect(celerConnection_.get(), &CelerClient::OnConnectionClosed, this, &BSTerminalMainWindow::onCelerDisconnected);
   connect(celerConnection_.get(), &CelerClient::OnConnectionError, this, &BSTerminalMainWindow::onCelerConnectionError, Qt::QueuedConnection);

   mdProvider_ = std::make_shared<CelerMarketDataProvider>(connectionManager_, logMgr_->logger("message"), true);

   connect(mdProvider_.get(), &MarketDataProvider::UserWantToConnectToMD, this, &BSTerminalMainWindow::acceptMDAgreement);
}

void BSTerminalMainWindow::acceptMDAgreement(const std::string &host, const std::string &port)
{
   if (!isMDLicenseAccepted()) {
      MDAgreementDialog dlg{this};
      if (dlg.exec() != QDialog::Accepted) {
         return;
      }

      saveUserAcceptedMDLicense();
   }

   mdProvider_->MDLicenseAccepted(host, port);
}

void BSTerminalMainWindow::updateControlEnabledState()
{
   action_send_->setEnabled(walletsManager_->GetWalletsCount() > 0
      && armory_->isOnline());
}

bool BSTerminalMainWindow::isMDLicenseAccepted() const
{
   return applicationSettings_->get<bool>(ApplicationSettings::MDLicenseAccepted);
}

void BSTerminalMainWindow::saveUserAcceptedMDLicense()
{
   applicationSettings_->set(ApplicationSettings::MDLicenseAccepted, true);
}

void BSTerminalMainWindow::InitAssets()
{
   ccFileManager_ = std::make_shared<CCFileManager>(logMgr_->logger(), applicationSettings_
      , otpManager_, connectionManager_);
   assetManager_ = std::make_shared<AssetManager>(logMgr_->logger(), walletsManager_, mdProvider_, celerConnection_);
   assetManager_->init();

   connect(ccFileManager_.get(), &CCFileManager::CCSecurityDef, assetManager_.get(), &AssetManager::onCCSecurityReceived);
   connect(ccFileManager_.get(), &CCFileManager::CCSecurityInfo, walletsManager_.get(), &WalletsManager::onCCSecurityInfo);
   connect(ccFileManager_.get(), &CCFileManager::Loaded, walletsManager_.get(), &WalletsManager::onCCInfoLoaded);
   connect(ccFileManager_.get(), &CCFileManager::LoadingFailed, this, &BSTerminalMainWindow::onCCInfoMissing);

   connect(ccFileManager_.get(), &CCFileManager::CCSecurityId, mdProvider_.get(), &CelerMarketDataProvider::onCCSecurityReceived);
   connect(mdProvider_.get(), &MarketDataProvider::MDUpdate, assetManager_.get(), &AssetManager::onMDUpdate);

   ccFileManager_->LoadSavedCCDefinitions();
   ccFileManager_->LoadCCDefinitionsFromPub();
}

void BSTerminalMainWindow::InitPortfolioView()
{
   portfolioModel_ = std::make_shared<CCPortfolioModel>(walletsManager_, assetManager_, this);
   ui->widgetPortfolio->init(applicationSettings_, mdProvider_, portfolioModel_,
                             signContainer_, armory_, logMgr_->logger("ui"),
                             walletsManager_);
}

void BSTerminalMainWindow::InitWalletsView()
{
   ui->widgetWallets->init(logMgr_->logger("ui"), walletsManager_, signContainer_
      , applicationSettings_, assetManager_, authManager_, armory_);
}

// Initialize widgets related to transactions.
void BSTerminalMainWindow::InitTransactionsView()
{
   ui->widgetExplorer->init(armory_, logMgr_->logger());
   ui->widgetTransactions->init(walletsManager_, armory_, signContainer_,
                                logMgr_->logger("ui"));
   ui->widgetTransactions->setEnabled(true);

   ui->widgetTransactions->SetTransactionsModel(transactionsModel_);
   ui->widgetPortfolio->SetTransactionsModel(transactionsModel_);
}

void BSTerminalMainWindow::onArmoryStateChanged(ArmoryConnection::State newState)
{
   switch(newState)
   {
   case ArmoryConnection::State::Ready:
      QMetaObject::invokeMethod(this, "CompleteUIOnlineView", Qt::QueuedConnection);
      break;
   case ArmoryConnection::State::Connected:
      QMetaObject::invokeMethod(this, "CompleteDBConnection", Qt::QueuedConnection);
      break;
   case ArmoryConnection::State::Offline:
      QMetaObject::invokeMethod(this, "ArmoryIsOffline", Qt::QueuedConnection);
      break;
   case ArmoryConnection::State::Scanning:
   case ArmoryConnection::State::Error:
   case ArmoryConnection::State::Closing:
      break;
   default:    break;
   }
}

void BSTerminalMainWindow::CompleteUIOnlineView()
{
   transactionsModel_ = std::make_shared<TransactionsViewModel>(armory_
                                                               , walletsManager_
                                                         , logMgr_->logger("ui")
                                                                , this);

   InitTransactionsView();
   transactionsModel_->loadAllWallets();

   if (walletsManager_->GetWalletsCount() == 0) {
      createWallet(!walletsManager_->HasPrimaryWallet());
   }
   updateControlEnabledState();
}

void BSTerminalMainWindow::CompleteDBConnection()
{
   logMgr_->logger("ui")->debug("BSTerminalMainWindow::CompleteDBConnection");
   walletsManager_->RegisterSavedWallets();
}

void BSTerminalMainWindow::onReactivate()
{
   show();
}

void BSTerminalMainWindow::UpdateMainWindowAppearence()
{
   if (!applicationSettings_->get<bool>(ApplicationSettings::closeToTray) && isHidden()) {
      setWindowState(windowState() & ~Qt::WindowMinimized);
      show();
      raise();
      activateWindow();
   }

   const auto bsTitle = tr("BlockSettle Terminal [%1]");
   switch (applicationSettings_->get<NetworkType>(ApplicationSettings::netType)) {
   case NetworkType::TestNet:
      setWindowTitle(bsTitle.arg(tr("TESTNET")));
      break;

   case NetworkType::RegTest:
      setWindowTitle(bsTitle.arg(tr("REGTEST")));
      break;

   default:
      setWindowTitle(tr("BlockSettle Terminal"));
      break;
   }
}

void BSTerminalMainWindow::ArmoryIsOffline()
{
   logMgr_->logger("ui")->debug("BSTerminalMainWindow::ArmoryIsOffline");
   walletsManager_->UnregisterSavedWallets();
   connectArmory();
   updateControlEnabledState();
}

void BSTerminalMainWindow::initArmory()
{
   armory_ = std::make_shared<ArmoryConnection>(logMgr_->logger()
      , applicationSettings_->get<std::string>(ApplicationSettings::txCacheFileName), true);
   connect(armory_.get(), &ArmoryConnection::txBroadcastError, [](const QString &txHash, const QString &error) {
      NotificationCenter::notify(bs::ui::NotifyType::BroadcastError, { txHash, error });
   });
   connect(armory_.get(), &ArmoryConnection::zeroConfReceived, this, &BSTerminalMainWindow::onZCreceived, Qt::QueuedConnection);
   connect(armory_.get(), SIGNAL(stateChanged(ArmoryConnection::State)), this, SLOT(onArmoryStateChanged(ArmoryConnection::State)), Qt::QueuedConnection);
}

void BSTerminalMainWindow::connectArmory()
{
   armory_->setupConnection(applicationSettings_->GetArmorySettings());
}

bool BSTerminalMainWindow::createWallet(bool primary, bool reportSuccess)
{
   if (primary && (walletsManager_->GetHDWalletsCount() > 0)) {
      auto wallet = walletsManager_->GetHDWallet(0);
      if (wallet->isPrimary()) {
         return true;
      }
      BSMessageBox qry(BSMessageBox::question, tr("Create primary wallet"), tr("Promote to primary wallet")
         , tr("In order to execute trades and take delivery of XBT and Equity Tokens, you are required to"
            " have a Primary Wallet which supports the sub-wallets required to interact with the system.")
         .arg(QString::fromStdString(wallet->getName())), this);
      if (qry.exec() == QDialog::Accepted) {
         wallet->createGroup(bs::hd::CoinType::BlockSettle_Auth);
         return true;
      }
      return false;
   }

   NewWalletDialog newWalletDialog(true, this);
   if (!newWalletDialog.exec()) {
      return false;
   }

   if (newWalletDialog.isCreate()) {
      return ui->widgetWallets->CreateNewWallet(primary, reportSuccess);
   }
   else if (newWalletDialog.isImport()) {
      return ui->widgetWallets->ImportNewWallet(primary, reportSuccess);
   }
   return false;
}

void BSTerminalMainWindow::showInfo(const QString &title, const QString &text)
{
   BSMessageBox(BSMessageBox::info, title, text).exec();
}

void BSTerminalMainWindow::showError(const QString &title, const QString &text)
{
   QMetaObject::invokeMethod(this, [this, title, text] {
      BSMessageBox(BSMessageBox::critical, title, text, this).exec();
   });
}

void BSTerminalMainWindow::onReceive()
{
   const auto &defWallet = walletsManager_->GetDefaultWallet();
   std::string selWalletId = defWallet ? defWallet->GetWalletId() : std::string{};
   if (ui->tabWidget->currentWidget() == ui->widgetWallets) {
      auto wallets = ui->widgetWallets->GetSelectedWallets();
      if (!wallets.empty()) {
         selWalletId = wallets[0]->GetWalletId();
      } else {
         wallets = ui->widgetWallets->GetFirstWallets();

         if (!wallets.empty()) {
            selWalletId = wallets[0]->GetWalletId();
         }
      }
   }
   SelectWalletDialog *selectWalletDialog = new SelectWalletDialog(walletsManager_, selWalletId, this);
   selectWalletDialog->exec();

   if (selectWalletDialog->result() == QDialog::Rejected) {
      return;
   }

   NewAddressDialog* newAddressDialog = new NewAddressDialog(selectWalletDialog->getSelectedWallet()
      , signContainer_, selectWalletDialog->isNestedSegWitAddress(), this);
   newAddressDialog->show();
}

void BSTerminalMainWindow::createAdvancedTxDialog(const std::string &selectedWalletId)
{
   CreateTransactionDialogAdvanced advancedDialog{armory_, walletsManager_,
                                                  signContainer_, true,
                                                  logMgr_->logger("ui"), this};
   advancedDialog.setOfflineDir(applicationSettings_->get<QString>(ApplicationSettings::signerOfflineDir));

   if (!selectedWalletId.empty()) {
      advancedDialog.SelectWallet(selectedWalletId);
   }

   advancedDialog.exec();
}

void BSTerminalMainWindow::onSend()
{
   std::string selectedWalletId;

   if (ui->tabWidget->currentWidget() == ui->widgetWallets) {
      const auto &wallets = ui->widgetWallets->GetSelectedWallets();
      if (wallets.size() == 1) {
         selectedWalletId = wallets[0]->GetWalletId();
      }
   }

   if (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) {
      createAdvancedTxDialog(selectedWalletId);
   } else {
      if (applicationSettings_->get<bool>(ApplicationSettings::AdvancedTxDialogByDefault)) {
         createAdvancedTxDialog(selectedWalletId);
      } else {
         CreateTransactionDialogSimple dlg{armory_, walletsManager_,
                                           signContainer_, logMgr_->logger("ui"),
                                           this};
         dlg.setOfflineDir(applicationSettings_->get<QString>(ApplicationSettings::signerOfflineDir));

         if (!selectedWalletId.empty()) {
            dlg.SelectWallet(selectedWalletId);
         }

         dlg.exec();

         if ((dlg.result() == QDialog::Accepted) && dlg.userRequestedAdvancedDialog()) {
            auto advancedDialog = dlg.CreateAdvancedDialog();

            advancedDialog->exec();
         }
      }
   }
}

void BSTerminalMainWindow::setupMenu()
{
   // menu role erquired for OSX only, to place it to first menu item
   action_login_->setMenuRole(QAction::ApplicationSpecificRole);
   action_logout_->setMenuRole(QAction::ApplicationSpecificRole);

   ui->menuFile->insertAction(ui->actionSettings, action_login_);
   ui->menuFile->insertAction(ui->actionSettings, action_logout_);

   ui->menuFile->insertSeparator(action_login_);
   ui->menuFile->insertSeparator(ui->actionSettings);

   connect(ui->action_Create_New_Wallet, &QAction::triggered, [ww = ui->widgetWallets]{ ww->CreateNewWallet(false); });
   connect(ui->actionAuthentication_Addresses, &QAction::triggered, this, &BSTerminalMainWindow::openAuthManagerDialog);
   connect(ui->action_One_time_Password, &QAction::triggered, this, &BSTerminalMainWindow::openOTPDialog);
   connect(ui->actionSettings, &QAction::triggered, this, [=]() { openConfigDialog(); });
   connect(ui->actionAccount_Information, &QAction::triggered, this, &BSTerminalMainWindow::openAccountInfoDialog);
   connect(ui->actionEnter_Color_Coin_token, &QAction::triggered, this, &BSTerminalMainWindow::openCCTokenDialog);

   onUserLoggedOut();

#ifndef Q_OS_MAC
   ui->horizontalFrame->hide();

   ui->menubar->setCornerWidget(ui->pushButtonUser);
#endif
}

void BSTerminalMainWindow::openOTPDialog()
{
   if (otpManager_->CurrentUserHaveOTP()) {
      OTPFileInfoDialog dialog(logMgr_->logger("ui"), otpManager_, applicationSettings_, this);
      dialog.exec();
   } else {
      OTPImportDialog(logMgr_->logger("ui"), otpManager_, celerConnection_->userName()
         , applicationSettings_, this).exec();
   }
}

void BSTerminalMainWindow::openAuthManagerDialog()
{
   openAuthDlgVerify(QString());
}

void BSTerminalMainWindow::openAuthDlgVerify(const QString &addrToVerify)
{
   if (authManager_->HaveAuthWallet()) {
      authAddrDlg_->show();
      QApplication::processEvents();
      authAddrDlg_->setAddressToVerify(addrToVerify);
   } else {
      createAuthWallet();
   }
}

void BSTerminalMainWindow::openConfigDialog()
{
   ConfigDialog configDialog(applicationSettings_, walletsManager_, assetManager_, this);
   configDialog.exec();

   UpdateMainWindowAppearence();
}

void BSTerminalMainWindow::openAccountInfoDialog()
{
   CelerAccountInfoDialog dialog(celerConnection_, this);
   dialog.exec();
}

void BSTerminalMainWindow::openCCTokenDialog()
{
   if (!otpManager_->CurrentUserHaveOTP()) {
      BSMessageBox createOtpReq(BSMessageBox::question, tr("One-Time Password")
         , tr("Import one-time password")
         , tr("Would you like to import your OTP at this time?")
         , tr("BlockSettle has sent a one-time password to your registered postal address. The OTP is used "
            "to confirm your identity and to establish a secure channel through which communication can occur.")
         , this);
      if (createOtpReq.exec() == QDialog::Accepted) {
         OTPImportDialog otpDialog(logMgr_->logger("ui"), otpManager_, celerConnection_->userName()
            , applicationSettings_, this);
         if (otpDialog.exec() != QDialog::Accepted) {
            return;
         }
      }
      else {
         return;
      }
   }
   if (walletsManager_->HasPrimaryWallet() || createWallet(true, false)) {
      CCTokenEntryDialog dialog(walletsManager_, ccFileManager_, signContainer_, this);
      dialog.exec();
   }
}

void BSTerminalMainWindow::onLogin()
{
   GetNetworkSettingsFromPuB([this](std::map<NetworkSettingType, std::pair<std::string, unsigned int>>) {});

   LoginWindow loginDialog(applicationSettings_, this);

   if (loginDialog.exec() == QDialog::Accepted) {
      const std::string host = applicationSettings_->get<std::string>(ApplicationSettings::celerHost);
      const std::string port = applicationSettings_->get<std::string>(ApplicationSettings::celerPort);
      const std::string username = loginDialog.getUsername().toStdString();
      const std::string password = loginDialog.getPassword().toStdString();

      if (!celerConnection_->LoginToServer(host, port, username, password)) {
         logMgr_->logger("ui")->error("[BSTerminalMainWindow::onLogin] LoginToServer failed");
      } else {
         ui->widgetWallets->setUsername(QString::fromStdString(username));
         action_logout_->setVisible(false);
         action_login_->setEnabled(false);

         // set button text to this temporary text until the login
         // completes and button text is changed to the username
         setLoginButtonText(tr("Logging in..."));
      }
   }
}

void BSTerminalMainWindow::onLogout()
{
   ui->widgetWallets->setUsername(QString());
   celerConnection_->CloseConnection();
}

void BSTerminalMainWindow::onUserLoggedIn()
{
   ui->actionAccount_Information->setEnabled(true);
   ui->actionAuthentication_Addresses->setEnabled(true);
   ui->action_One_time_Password->setEnabled(true);
   ui->actionEnter_Color_Coin_token->setEnabled(true);

   ui->actionDeposits->setEnabled(true);
   ui->actionWithdrawal_Request->setEnabled(true);
   ui->actionLink_Additional_Bank_Account->setEnabled(true);

   authManager_->ConnectToPublicBridge(connectionManager_, celerConnection_);
   ccFileManager_->ConnectToCelerClient(celerConnection_);

   const auto userId = BinaryData::CreateFromHex(celerConnection_->userId());
   signContainer_->SetUserId(userId);
   walletsManager_->SetUserId(userId);

   setLoginButtonText(QString::fromStdString(celerConnection_->userName()));

   if (!mdProvider_->IsConnectionActive()) {
      mdProvider_->SubscribeToMD(applicationSettings_->get<std::string>(ApplicationSettings::mdServerHost)
         , applicationSettings_->get<std::string>(ApplicationSettings::mdServerPort));
   }
}

void BSTerminalMainWindow::onUserLoggedOut()
{
   ui->actionAccount_Information->setEnabled(false);
   ui->actionAuthentication_Addresses->setEnabled(false);
   ui->actionEnter_Color_Coin_token->setEnabled(false);
   ui->action_One_time_Password->setEnabled(false);

   ui->actionDeposits->setEnabled(false);
   ui->actionWithdrawal_Request->setEnabled(false);
   ui->actionLink_Additional_Bank_Account->setEnabled(false);

   signContainer_->SetUserId(BinaryData{});
   walletsManager_->SetUserId(BinaryData{});
   authManager_->OnDisconnectedFromCeler();
   setLoginButtonText(tr("user.name"));
}

void BSTerminalMainWindow::onCelerConnected()
{
   action_login_->setVisible(false);
   action_logout_->setVisible(true);

   onUserLoggedIn();
}

void BSTerminalMainWindow::onCelerDisconnected()
{
   action_logout_->setVisible(false);
   action_login_->setEnabled(true);
   action_login_->setVisible(true);

   onUserLoggedOut();
   celerConnection_->CloseConnection();
}

void BSTerminalMainWindow::onCelerConnectionError(int errorCode)
{
   switch(errorCode)
   {
   case CelerClient::LoginError:
      BSMessageBox loginErrorBox(BSMessageBox::critical, tr("Login failed"), tr("Login failed"), tr("Invalid username/password pair"), this);
      loginErrorBox.exec();
      break;
   }
}

void BSTerminalMainWindow::createAuthWallet()
{
   if (celerConnection_->tradingAllowed()) {
      if (!walletsManager_->HasPrimaryWallet() && !createWallet(true)) {
         return;
      }

      if (authManager_->HaveOTP() && !walletsManager_->GetAuthWallet()) {
         BSMessageBox createAuthReq(BSMessageBox::question, tr("Authentication Wallet")
            , tr("Create Authentication Wallet")
            , tr("You don't have a sub-wallet in which to hold Authentication Addresses. Would you like to create one?")
            , this);
         if (createAuthReq.exec() == QDialog::Accepted) {
            authManager_->CreateAuthWallet();
         }
      }
   }
}

void BSTerminalMainWindow::onAuthMgrConnComplete()
{
   if (celerConnection_->tradingAllowed()) {
      if (!walletsManager_->HasPrimaryWallet() && !createWallet(true)) {
         return;
      }
      if (!walletsManager_->HasSettlementWallet()) {
         BSMessageBox createSettlReq(BSMessageBox::question, tr("Create settlement wallet")
            , tr("Settlement wallet missing")
            , tr("You don't have Settlement wallet, yet. Do you wish to create it?")
            , this);
         if (createSettlReq.exec() == QDialog::Accepted) {
            const auto title = tr("Settlement wallet");
            if (walletsManager_->CreateSettlementWallet(applicationSettings_->GetHomeDir())) {
               BSMessageBox(BSMessageBox::success, title, tr("Settlement wallet successfully created")).exec();
            } else {
               showError(title, tr("Failed to create settlement wallet"));
               return;
            }
         }
         else {
            return;
         }
      }

      createAuthWallet();
   }
   else {
      logMgr_->logger("ui")->debug("Trading not allowed");
   }
}

void BSTerminalMainWindow::onZCreceived(ArmoryConnection::ReqIdType reqId)
{
   const auto &entries = armory_->getZCentries(reqId);
   if (entries.empty()) {
      return;
   }
   for (const auto& led : entries) {
      const auto &cbTx = [this, id = led.getID(), txTime = led.getTxTime(), value = led.getValue()](Tx tx) {
         const auto &wallet = walletsManager_->GetWalletById(id);
         if (!wallet) {
            return;
         }
         auto txInfo = new TxInfo { tx, txTime, value, wallet, bs::Transaction::Direction::Unknown, QString() };
         const auto &cbDir = [this, txInfo] (bs::Transaction::Direction dir) {
            txInfo->direction = dir;
            if (!txInfo->mainAddress.isEmpty() && txInfo->wallet) {
               showZcNotification(*txInfo);
               delete txInfo;
            }
         };
         const auto &cbMainAddr = [this, txInfo] (QString mainAddr) {
            txInfo->mainAddress = mainAddr;
            if ((txInfo->direction != bs::Transaction::Direction::Unknown) && txInfo->wallet) {
               showZcNotification(*txInfo);
               delete txInfo;
            }
         };
         walletsManager_->GetTransactionDirection(tx, wallet, cbDir);
         walletsManager_->GetTransactionMainAddress(tx, wallet, (value > 0), cbMainAddr);
      };
      armory_->getTxByHash(led.getTxHash(), cbTx);
   }
}

void BSTerminalMainWindow::showZcNotification(const TxInfo &txInfo)
{
   QStringList lines;
   lines << tr("Date: %1").arg(UiUtils::displayDateTime(txInfo.txTime));
   lines << tr("TX: %1 %2 %3").arg(tr(bs::Transaction::toString(txInfo.direction)))
      .arg(txInfo.wallet->displayTxValue(txInfo.value)).arg(txInfo.wallet->displaySymbol());
   lines << tr("Wallet: %1").arg(QString::fromStdString(txInfo.wallet->GetWalletName()));
   lines << txInfo.mainAddress;

   const auto &title = tr("New blockchain transaction");
   NotificationCenter::notify(bs::ui::NotifyType::BlockchainTX, { title, lines.join(tr("\n")) });
}

void BSTerminalMainWindow::showRunInBackgroundMessage()
{
   qDebug() << "showMessage" << sysTrayIcon_->isVisible();
   sysTrayIcon_->showMessage(tr("BlockSettle is running"), tr("BlockSettle Terminal is running in the backgroud. Click the tray icon to open the main window."), QSystemTrayIcon::Information);
}

void BSTerminalMainWindow::closeEvent(QCloseEvent* event)
{
   if (applicationSettings_->get<bool>(ApplicationSettings::closeToTray)) {
      hide();
      event->ignore();
   }
   else {
      QMainWindow::closeEvent(event);
      QApplication::exit();
   }
}

void BSTerminalMainWindow::changeEvent(QEvent* e)
{
   switch (e->type())
   {
      case QEvent::WindowStateChange:
      {
         if (this->windowState() & Qt::WindowMinimized)
         {
            if (applicationSettings_->get<bool>(ApplicationSettings::minimizeToTray))
            {
               QTimer::singleShot(0, this, &QMainWindow::hide);
            }
         }

         break;
      }
      default:
         break;
   }

   QMainWindow::changeEvent(e);
}

void BSTerminalMainWindow::setLoginButtonText(const QString& text)
{
   ui->pushButtonUser->setText(text);

#ifndef Q_OS_MAC
   ui->menubar->adjustSize();
#endif
}

void BSTerminalMainWindow::onPasswordRequested(std::string walletId, std::string prompt
   , std::vector<bs::wallet::EncryptionType> encTypes, std::vector<SecureBinaryData> encKeys
   , bs::wallet::KeyRank keyRank)
{
   SignContainer::PasswordType password;
   bool cancelledByUser = true;

   if (walletId.empty()) {
      logMgr_->logger("ui")->error("[onPasswordRequested] can\'t ask password for empty wallet id");
   } else {
      QString walletName;
      const auto wallet = walletsManager_->GetWalletById(walletId);
      if (wallet != nullptr) {
         // do we need to get name of root wallet?
         walletName = QString::fromStdString(wallet->GetWalletName());
      } else {
         const auto hdWallet = walletsManager_->GetHDWalletById(walletId);
         walletName = QString::fromStdString(hdWallet->getName());
      }

      if (!walletName.isEmpty()) {
         const auto &rootWallet = walletsManager_->GetHDRootForLeaf(walletId);

         EnterWalletPassword passwordDialog(MobileClientRequest::SignWallet, this);
         passwordDialog.init(rootWallet ? rootWallet->getWalletId() : walletId
            , keyRank, encTypes, encKeys, applicationSettings_, QString::fromStdString(prompt));
         if (passwordDialog.exec() == QDialog::Accepted) {
            password = passwordDialog.getPassword();
            cancelledByUser = false;
         }
         else {
            logMgr_->logger("ui")->debug("[onPasswordRequested] user rejected to enter password for wallet {} ( {} )"
               , walletId, walletName.toStdString());
         }
      } else {
         logMgr_->logger("ui")->error("[onPasswordRequested] can\'t find wallet with id {}", walletId);
      }
   }

   signContainer_->SendPassword(walletId, password, cancelledByUser);
}

void BSTerminalMainWindow::OnOTPSyncCompleted()
{
   if (otpManager_->CurrentUserHaveOTP()) {
      if (!otpManager_->IsCurrentOTPLatest()) {
         BSMessageBox removeOtpQuestion(BSMessageBox::question, tr("OTP outdated")
            , tr("Your OTP is outdated")
            , tr("Do you want to remove outdated OTP?")
            , tr("Looks like new OTP was generated for your account. All future requests signed by your local OTP will be rejected.")
            , this);

         if (removeOtpQuestion.exec() == QDialog::Accepted) {
            if (otpManager_->RemoveOTPForCurrentUser()) {
               BSMessageBox(BSMessageBox::info, tr("OTP file removed"), tr("Old OTP file was removed."), this).exec();
            } else {
               BSMessageBox(BSMessageBox::critical, tr("OTP file not removed"), tr("Terminal failed to remove OTP file."), this).exec();
            }
         }
      } else if (otpManager_->CountAdvancingRequired()) {
         BSMessageBox otpDialog(BSMessageBox::question, tr("One-Time Password")
            , tr("Update one-time password counter")
            , tr("Would you like to update your OTP usage counter at this time?")
            , tr("Looks like you have used your OTP on another machine. Local OTP usage counter should be advanced or your requests will be rejected.")
            , this);

         if (otpDialog.exec() == QDialog::Accepted) {
            OTPFileInfoDialog(logMgr_->logger("ui"), otpManager_, applicationSettings_, this).exec();
         }
      }
   } else if (celerConnection_->tradingAllowed() ) {
      BSMessageBox createOtpReq(BSMessageBox::question, tr("One-Time Password")
         , tr("Import one-time password")
         , tr("Would you like to import your OTP at this time?")
         , tr("BlockSettle has sent a one-time password to your registered postal address. The OTP is used "
            "to confirm your identity and to establish a secure channel through which communication can occur.")
         , this);
      if (createOtpReq.exec() == QDialog::Accepted) {
         OTPImportDialog(logMgr_->logger("ui"), otpManager_, celerConnection_->userName()
            , applicationSettings_, this).exec();
      }
   }
}

void BSTerminalMainWindow::onCCInfoMissing()
{ }   // do nothing here since we don't know if user will need Private Market before logon to Celer

void BSTerminalMainWindow::setupShortcuts()
{
   auto overviewTabShortcut = new QShortcut(QKeySequence(QString::fromStdString("Ctrl+1")), this);
   overviewTabShortcut->setContext(Qt::WindowShortcut);
   connect(overviewTabShortcut, &QShortcut::activated, [this](){ ui->tabWidget->setCurrentIndex(0);});

   auto tradingTabShortcut = new QShortcut(QKeySequence(QString::fromStdString("Ctrl+2")), this);
   tradingTabShortcut->setContext(Qt::WindowShortcut);
   connect(tradingTabShortcut, &QShortcut::activated, [this](){ ui->tabWidget->setCurrentIndex(1);});

   auto dealingTabShortcut = new QShortcut(QKeySequence(QString::fromStdString("Ctrl+3")), this);
   dealingTabShortcut->setContext(Qt::WindowShortcut);
   connect(dealingTabShortcut, &QShortcut::activated, [this](){ ui->tabWidget->setCurrentIndex(2);});

   auto walletsTabShortcutt = new QShortcut(QKeySequence(QString::fromStdString("Ctrl+4")), this);
   walletsTabShortcutt->setContext(Qt::WindowShortcut);
   connect(walletsTabShortcutt, &QShortcut::activated, [this](){ ui->tabWidget->setCurrentIndex(3);});

   auto transactionsTabShortcut = new QShortcut(QKeySequence(QString::fromStdString("Ctrl+5")), this);
   transactionsTabShortcut->setContext(Qt::WindowShortcut);
   connect(transactionsTabShortcut, &QShortcut::activated, [this](){ ui->tabWidget->setCurrentIndex(4);});

   auto alt_1 = new QShortcut(QKeySequence(QString::fromLatin1("Alt+1")), this);
   alt_1->setContext(Qt::WindowShortcut);
   connect(alt_1, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Alt_1);
      }
   );

   auto alt_2 = new QShortcut(QKeySequence(QString::fromLatin1("Alt+2")), this);
   alt_2->setContext(Qt::WindowShortcut);
   connect(alt_2, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Alt_2);
      }
   );

   auto alt_3 = new QShortcut(QKeySequence(QString::fromLatin1("Alt+3")), this);
   alt_3->setContext(Qt::WindowShortcut);
   connect(alt_3, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Alt_3);
      }
   );

   auto ctrl_s = new QShortcut(QKeySequence(QString::fromLatin1("Ctrl+S")), this);
   ctrl_s->setContext(Qt::WindowShortcut);
   connect(ctrl_s, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Ctrl_S);
      }
   );

   auto ctrl_p = new QShortcut(QKeySequence(QString::fromLatin1("Ctrl+P")), this);
   ctrl_p->setContext(Qt::WindowShortcut);
   connect(ctrl_p, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Ctrl_P);
      }
   );

   auto ctrl_q = new QShortcut(QKeySequence(QString::fromLatin1("Ctrl+Q")), this);
   ctrl_q->setContext(Qt::WindowShortcut);
   connect(ctrl_q, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Ctrl_Q);
      }
   );

   auto alt_s = new QShortcut(QKeySequence(QString::fromLatin1("Alt+S")), this);
   alt_s->setContext(Qt::WindowShortcut);
   connect(alt_s, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Alt_S);
      }
   );

   auto alt_b = new QShortcut(QKeySequence(QString::fromLatin1("Alt+B")), this);
   alt_b->setContext(Qt::WindowShortcut);
   connect(alt_b, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Alt_B);
      }
   );

   auto alt_p = new QShortcut(QKeySequence(QString::fromLatin1("Alt+P")), this);
   alt_p->setContext(Qt::WindowShortcut);
   connect(alt_p, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Alt_P);
      }
   );
}

void BSTerminalMainWindow::onButtonUserClicked() {
   if (ui->pushButtonUser->text() == tr("user.name")) {
      onLogin();
   } else {
      if (BSMessageBox(BSMessageBox::question, tr("User Logout"), tr("You are about to logout")
         , tr("Do you want to continue?")).exec() == QDialog::Accepted)
      onLogout();
   }
}
