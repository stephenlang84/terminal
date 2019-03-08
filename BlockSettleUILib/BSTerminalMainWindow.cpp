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
#include "ArmoryServersProvider.h"
#include "AssetManager.h"
#include "AuthAddressDialog.h"
#include "AuthAddressManager.h"
#include "AutheIDClient.h"
#include "AuthSignManager.h"
#include "BSMarketDataProvider.h"
#include "BSMessageBox.h"
#include "BSTerminalSplashScreen.h"
#include "CCFileManager.h"
#include "CCPortfolioModel.h"
#include "CCTokenEntryDialog.h"
#include "CelerAccountInfoDialog.h"
#include "ChatWidget.h"
#include "ConnectionManager.h"
#include "CreateTransactionDialogAdvanced.h"
#include "CreateTransactionDialogSimple.h"
#include "DialogManager.h"
#include "HeadlessContainer.h"
#include "LoginWindow.h"
#include "ManageEncryption/EnterWalletPassword.h"
#include "MarketDataProvider.h"
#include "MDAgreementDialog.h"
#include "NewAddressDialog.h"
#include "NewWalletDialog.h"
#include "NotificationCenter.h"
#include "OfflineSigner.h"
#include "QuoteProvider.h"
#include "RequestReplyCommand.h"
#include "SelectWalletDialog.h"
#include "Settings/ConfigDialog.h"
#include "StartupDialog.h"
#include "StatusBarView.h"
#include "TabWithShortcut.h"
#include "TerminalEncryptionDialog.h"
#include "TransactionsViewModel.h"
#include "UiUtils.h"
#include "Wallets/SyncHDWallet.h"
#include "Wallets/SyncWalletsManager.h"
#include "ZMQHelperFunctions.h"
#include "ZmqSecuredDataConnection.h"

#include <spdlog/spdlog.h>

BSTerminalMainWindow::BSTerminalMainWindow(const std::shared_ptr<ApplicationSettings>& settings
   , BSTerminalSplashScreen& splashScreen, QWidget* parent)
   : QMainWindow(parent)
   , ui_(new Ui::BSTerminalMainWindow())
   , applicationSettings_(settings)
{
   UiUtils::SetupLocale();

   ui_->setupUi(this);

   setupShortcuts();

   loginButtonText_ = tr("Login");

   armoryServersProvider_= std::make_shared<ArmoryServersProvider>(applicationSettings_);

   bool licenseAccepted = showStartupDialog();
   if (!licenseAccepted) {
      QTimer::singleShot(0, this, [this](){
         qApp->exit(EXIT_FAILURE);
      });
      return;
   }

   auto geom = settings->get<QRect>(ApplicationSettings::GUI_main_geometry);
   if (!geom.isEmpty()) {
      setGeometry(geom);
   }

   connect(ui_->actionQuit, &QAction::triggered, qApp, &QCoreApplication::quit);
   connect(this, &BSTerminalMainWindow::readyToLogin, this, &BSTerminalMainWindow::onReadyToLogin);

   logMgr_ = std::make_shared<bs::LogManager>([] { KillHeadlessProcess(); });
   logMgr_->add(applicationSettings_->GetLogsConfig());

   logMgr_->logger()->debug("Settings loaded from {}", applicationSettings_->GetSettingsPath().toStdString());

   setupIcon();
   UiUtils::setupIconFont(this);
   NotificationCenter::createInstance(applicationSettings_, ui_.get(), sysTrayIcon_, this);

   InitConnections();

   initArmory();

   walletsMgr_ = std::make_shared<bs::sync::WalletsManager>(logMgr_->logger(), applicationSettings_, armory_);
   authSignManager_ = std::make_shared<AuthSignManager>(logMgr_->logger(), applicationSettings_
      , celerConnection_, connectionManager_);

   if (!applicationSettings_->get<bool>(ApplicationSettings::initialized)) {
      applicationSettings_->SetDefaultSettings(true);
   }

   InitAssets();
   InitSigningContainer();
   InitAuthManager();

   statusBarView_ = std::make_shared<StatusBarView>(armory_, walletsMgr_, assetManager_, celerConnection_
      , signContainer_, ui_->statusbar);

   splashScreen.SetProgress(100);
   splashScreen.close();
   QApplication::processEvents();

   setupToolbar();
   setupMenu();

   ui_->widgetTransactions->setEnabled(false);

   connectSigner();
   connectArmory();

   aboutDlg_ = std::make_shared<AboutDialog>(applicationSettings_->get<QString>(ApplicationSettings::ChangeLog_Base_Url), this);
   auto aboutDlgCb = [this] (int tab) {
      return [this, tab]() {
         aboutDlg_->setTab(tab);
         aboutDlg_->show();
      };
   };
   connect(ui_->actionAboutBlockSettle, &QAction::triggered, aboutDlgCb(0));
   connect(ui_->actionAboutTerminal, &QAction::triggered, aboutDlgCb(1));
   connect(ui_->actionContactBlockSettle, &QAction::triggered, aboutDlgCb(2));
   connect(ui_->actionVersion, &QAction::triggered, aboutDlgCb(3));

   ui_->tabWidget->setCurrentIndex(settings->get<int>(ApplicationSettings::GUI_main_tab));

   ui_->widgetTransactions->setAppSettings(applicationSettings_);

   UpdateMainWindowAppearence();
}

void BSTerminalMainWindow::onMDConnectionDetailsRequired()
{
   GetNetworkSettingsFromPuB([this]() { OnNetworkSettingsLoaded(); } );
}

void BSTerminalMainWindow::LoadCCDefinitionsFromPuB()
{
   if (!ccFileManager_ || ccFileManager_->synchronized()) {
      return;
   }
   const auto &priWallet = walletsMgr_->getPrimaryWallet();
   if (priWallet) {
      const auto &ccGroup = priWallet->getGroup(bs::hd::BlockSettle_CC);
      if (ccGroup && (ccGroup->getNumLeaves() > 0)) {
         ccFileManager_->LoadCCDefinitionsFromPub();
      }
   }
}

void BSTerminalMainWindow::GetNetworkSettingsFromPuB(const std::function<void()> &cb)
{
   if (networkSettings_.isSet) {
      cb();
      return;
   }

   Blocksettle::Communication::RequestPacket reqPkt;
   reqPkt.set_requesttype(Blocksettle::Communication::GetNetworkSettingsType);
   reqPkt.set_requestdata("");

   const auto &title = tr("Network settings");
   const auto connection = connectionManager_->CreateSecuredDataConnection();
   BinaryData inSrvPubKey(applicationSettings_->get<std::string>(ApplicationSettings::pubBridgePubKey));
   if (!connection->SetServerPublicKey(inSrvPubKey)) {
      showError(title, tr("Failed to set PuB connection public key"));
      return;
   }
   cmdPuBSettings_ = std::make_shared<RequestReplyCommand>("network_settings", connection, logMgr_->logger());

   const auto &populateAppSettings = [this](NetworkSettings settings) {
      if (!settings.celer.host.empty()) {
         applicationSettings_->set(ApplicationSettings::celerHost, QString::fromStdString(settings.celer.host));
         applicationSettings_->set(ApplicationSettings::celerPort, settings.celer.port);
      }
      if (!settings.marketData.host.empty()) {
         applicationSettings_->set(ApplicationSettings::mdServerHost, QString::fromStdString(settings.marketData.host));
         applicationSettings_->set(ApplicationSettings::mdServerPort, settings.marketData.port);
      }
#ifndef NDEBUG
	  QString chost = applicationSettings_->get<QString>(ApplicationSettings::chatServerHost);
	  QString cport = applicationSettings_->get<QString>(ApplicationSettings::chatServerPort);
	  if (!settings.chat.host.empty()) {
		  if (chost.isEmpty())
			applicationSettings_->set(ApplicationSettings::chatServerHost, QString::fromStdString(settings.chat.host));
		  if (cport.isEmpty())
			applicationSettings_->set(ApplicationSettings::chatServerPort, settings.chat.port);
	  }
#else
	  if (!settings.chat.host.empty()) {
		  applicationSettings_->set(ApplicationSettings::chatServerHost, QString::fromStdString(settings.chat.host));
		  applicationSettings_->set(ApplicationSettings::chatServerPort, settings.chat.port);
	  }
#endif // NDEBUG
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

      if (response.has_celer()) {
         networkSettings_.celer = { response.celer().host(), response.celer().port() };
         networkSettings_.isSet = true;
      }
      else {
         showError(title, tr("Missing Celer connection settings"));
         return false;
      }

      if (response.has_marketdata()) {
         networkSettings_.marketData = { response.marketdata().host(), response.marketdata().port() };
         networkSettings_.isSet = true;
      }
      else {
         showError(title, tr("Missing MD connection settings"));
         return false;
      }

      if (response.has_mdhs()) {
         networkSettings_.mdhs = { response.mdhs().host(), response.mdhs().port() };
         networkSettings_.isSet = true;
      }
      // else {
         // showError(title, tr("Missing MDHS connection settings"));
         // return false;
      // }

      if (response.has_chat()) {
         networkSettings_.chat = { response.chat().host(), response.chat().port() };
         networkSettings_.isSet = true;
      }
      else {
         showError(title, tr("Missing Chat connection settings"));
         return false;
      }

      populateAppSettings(networkSettings_);
      cb();
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

void BSTerminalMainWindow::OnNetworkSettingsLoaded()
{
   mdProvider_->SetConnectionSettings(applicationSettings_->get<std::string>(ApplicationSettings::mdServerHost)
      , applicationSettings_->get<std::string>(ApplicationSettings::mdServerPort));
}

void BSTerminalMainWindow::postSplashscreenActions()
{
   if (applicationSettings_->get<bool>(ApplicationSettings::SubscribeToMDOnStart)) {
      mdProvider_->SubscribeToMD();
   }
}

BSTerminalMainWindow::~BSTerminalMainWindow()
{
   applicationSettings_->set(ApplicationSettings::GUI_main_geometry, geometry());
   applicationSettings_->set(ApplicationSettings::GUI_main_tab, ui_->tabWidget->currentIndex());
   applicationSettings_->SaveSettings();

   NotificationCenter::destroyInstance();
   if (signContainer_) {
      signContainer_->Stop();
      signContainer_.reset();
   }
   walletsMgr_.reset();
   assetManager_.reset();
   bs::UtxoReservation::destroy();
}

void BSTerminalMainWindow::setupToolbar()
{
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
   ui_->tabWidget->setCornerWidget(toolBar, Qt::TopRightCorner);

   // send bitcoins
   toolBar->addAction(action_send_);
   // receive bitcoins
   toolBar->addAction(action_receive_);

   action_logout_->setVisible(false);

   connect(ui_->pushButtonUser, &QPushButton::clicked, this, &BSTerminalMainWindow::onButtonUserClicked);

   QMenu* trayMenu = new QMenu(this);
   QAction* trayShowAction = trayMenu->addAction(tr("&Open Terminal"));
   connect(trayShowAction, &QAction::triggered, this, &QMainWindow::show);
   trayMenu->addSeparator();

   trayMenu->addAction(action_send_);
   trayMenu->addAction(action_receive_);
   trayMenu->addAction(ui_->actionSettings);

   trayMenu->addSeparator();
   trayMenu->addAction(ui_->actionQuit);
   sysTrayIcon_->setContextMenu(trayMenu);
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

void BSTerminalMainWindow::LoadWallets()
{
   logMgr_->logger()->debug("Loading wallets");

   bs::UtxoReservation::init();

   connect(walletsMgr_.get(), &bs::sync::WalletsManager::walletsReady, [this] {
      ui_->widgetRFQ->setWalletsManager(walletsMgr_);
      ui_->widgetRFQReply->setWalletsManager(walletsMgr_);
   });
   connect(walletsMgr_.get(), &bs::sync::WalletsManager::walletsSynchronized, [this] {
      updateControlEnabledState();

      connect(armory_.get(), &ArmoryConnection::stateChanged, this, [this](ArmoryConnection::State state) {
         if (!initialWalletCreateDialogShown_) {
            if (state == ArmoryConnection::State::Connected && walletsMgr_ && walletsMgr_->hdWalletsCount() == 0) {
               initialWalletCreateDialogShown_ = true;
               QMetaObject::invokeMethod(this, "createWallet", Qt::QueuedConnection, Q_ARG(bool, true));
            }
         }
      });
      QTimer::singleShot(5000, this, [this](){
         if (!initialWalletCreateDialogShown_ && !armoryKeyDialogShown_) {
            if (walletsMgr_ && walletsMgr_->hdWalletsCount() == 0) {
               initialWalletCreateDialogShown_ = true;
               QMetaObject::invokeMethod(this, "createWallet", Qt::QueuedConnection, Q_ARG(bool, true));
            }
         }
      });

      if (readyToRegisterWallets_) {
         readyToRegisterWallets_ = false;
         walletsMgr_->registerWallets();
      }
   });
   connect(walletsMgr_.get(), &bs::sync::WalletsManager::info, this, &BSTerminalMainWindow::showInfo);
   connect(walletsMgr_.get(), &bs::sync::WalletsManager::error, this, &BSTerminalMainWindow::showError);

   // Enable/disable send action when first wallet created/last wallet removed
   connect(walletsMgr_.get(), &bs::sync::WalletsManager::walletChanged, this
      , &BSTerminalMainWindow::updateControlEnabledState);
   connect(walletsMgr_.get(), &bs::sync::WalletsManager::newWalletAdded, this
      , &BSTerminalMainWindow::updateControlEnabledState);

   const auto &progressDelegate = [this](int cur, int total) {
//      const int progress = cur * (100 / total);
//      splashScreen.SetProgress(progress);
      logMgr_->logger()->debug("Loaded wallet {} of {}", cur, total);
   };
   walletsMgr_->syncWallets(progressDelegate);
}

void BSTerminalMainWindow::InitAuthManager()
{
   authManager_ = std::make_shared<AuthAddressManager>(logMgr_->logger(), armory_);
   authManager_->init(applicationSettings_, walletsMgr_, authSignManager_, signContainer_);

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

std::shared_ptr<SignContainer> BSTerminalMainWindow::createSigner()
{
   std::shared_ptr<SignContainer> retPtr;
   auto runMode = static_cast<SignContainer::OpMode>(applicationSettings_->get<int>(ApplicationSettings::signerRunMode));
   auto signerHost = applicationSettings_->get<QString>(ApplicationSettings::signerHost);
   const auto signerPort = applicationSettings_->get<QString>(ApplicationSettings::signerPort);
   SecureBinaryData signerPubKey;

   if ((runMode == SignContainer::OpMode::Local)
      && SignerConnectionExists(QLatin1String("127.0.0.1"), signerPort)) {
      if (BSMessageBox(BSMessageBox::messageBoxType::question, tr("Signer Local Connection")
         , tr("Another Signer (or some other program occupying port %1) is running. Would you like to continue connecting to it?").arg(signerPort)
         , tr("If you wish to continue using GUI signer running on the same host, just select Remote Signer in settings and configure local connection")
         , this).exec() == QDialog::Rejected) {
         return retPtr;
      }
      runMode = SignContainer::OpMode::Remote;
      signerHost = QLatin1String("127.0.0.1");
   }

   if (signerPubKey.isNull()) {
      const auto pubKeyPath = applicationSettings_->get<QString>(ApplicationSettings::zmqLocalSignerPubKeyFilePath);

      if (!bs::network::readZmqKeyFile(pubKeyPath, signerPubKey, true, logMgr_->logger())) {
         logMgr_->logger()->warn("[BSTerminalMainWindow::InitSigningContainer] failed to load local signer key");
         BSMessageBox(BSMessageBox::messageBoxType::warning
            , tr("Signer Local Connection")
            , tr("Could not load local signer key.")
            , tr("BS terminal is missing connection encryption key for local signer process. File expected to be at %1").arg(pubKeyPath)
            , this).exec();
         return retPtr;
      }
   }

   retPtr = CreateSigner(logMgr_->logger(), applicationSettings_, runMode
      , signerHost, connectionManager_, armoryServersProvider_);
   return retPtr;
}

bool BSTerminalMainWindow::InitSigningContainer()
{
   signContainer_ = createSigner();

   if (!signContainer_) {
      showError(tr("BlockSettle Signer"), tr("BlockSettle Signer creation failure"));
      return false;
   }
   connect(signContainer_.get(), &SignContainer::ready, this, &BSTerminalMainWindow::SignerReady, Qt::QueuedConnection);
   connect(signContainer_.get(), &SignContainer::connectionError, this, &BSTerminalMainWindow::onSignerConnError, Qt::QueuedConnection);

   walletsMgr_->setSignContainer(signContainer_);

   return true;
}

void BSTerminalMainWindow::SignerReady()
{
   if (signContainer_->hasUI()) {
      disconnect(signContainer_.get(), &SignContainer::PasswordRequested, this, &BSTerminalMainWindow::onPasswordRequested);
   }
   else {
      connect(signContainer_.get(), &SignContainer::PasswordRequested, this, &BSTerminalMainWindow::onPasswordRequested);
   }

   LoadWallets();

   if (!widgetsInited_) {
      authAddrDlg_ = std::make_shared<AuthAddressDialog>(logMgr_->logger(), authManager_
         , assetManager_, applicationSettings_, this);

      InitWalletsView();
      InitPortfolioView();

      ui_->widgetRFQ->initWidgets(mdProvider_, applicationSettings_);

      auto quoteProvider = std::make_shared<QuoteProvider>(assetManager_, logMgr_->logger("message"));
      quoteProvider->ConnectToCelerClient(celerConnection_);

      auto dialogManager = std::make_shared<DialogManager>(geometry());

      ui_->widgetRFQ->init(logMgr_->logger(), celerConnection_, authManager_, quoteProvider, assetManager_
         , dialogManager, signContainer_, armory_, connectionManager_);
      ui_->widgetRFQReply->init(logMgr_->logger(), celerConnection_, authManager_, quoteProvider, mdProvider_, assetManager_
         , applicationSettings_, dialogManager, signContainer_, armory_, connectionManager_);

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

   mdProvider_ = std::make_shared<BSMarketDataProvider>(connectionManager_, logMgr_->logger("message"));

   connect(mdProvider_.get(), &MarketDataProvider::UserWantToConnectToMD, this, &BSTerminalMainWindow::acceptMDAgreement);
   connect(mdProvider_.get(), &MarketDataProvider::WaitingForConnectionDetails, this, &BSTerminalMainWindow::onMDConnectionDetailsRequired);

   InitChatView();
}

void BSTerminalMainWindow::acceptMDAgreement()
{
   if (!isMDLicenseAccepted()) {
      MDAgreementDialog dlg{this};
      if (dlg.exec() != QDialog::Accepted) {
         return;
      }

      saveUserAcceptedMDLicense();
   }

   mdProvider_->MDLicenseAccepted();
}

void BSTerminalMainWindow::updateControlEnabledState()
{
   action_send_->setEnabled(walletsMgr_->hdWalletsCount() > 0
      && armory_->isOnline() && signContainer_);
}

bool BSTerminalMainWindow::isMDLicenseAccepted() const
{
   return applicationSettings_->get<bool>(ApplicationSettings::MDLicenseAccepted);
}

void BSTerminalMainWindow::saveUserAcceptedMDLicense()
{
   applicationSettings_->set(ApplicationSettings::MDLicenseAccepted, true);
}

bool BSTerminalMainWindow::showStartupDialog()
{
   bool wasInitialized = applicationSettings_->get<bool>(ApplicationSettings::initialized);
   if (wasInitialized) {
     return true;
   }

 #ifdef _WIN32
   // Read registry value in case it was set with installer. Could be used only on Windows for now.
   QSettings settings(QLatin1String("HKEY_CURRENT_USER\\Software\\blocksettle\\blocksettle"), QSettings::NativeFormat);
   bool showLicense = !settings.value(QLatin1String("license_accepted"), false).toBool();
 #else
   bool showLicense = true;
 #endif // _WIN32

   StartupDialog startupDialog(showLicense);
   startupDialog.init(applicationSettings_, armoryServersProvider_);
   int result = startupDialog.exec();

   if (result == QDialog::Rejected) {
      hide();
      return false;
   }
   return true;
}

void BSTerminalMainWindow::InitAssets()
{
   ccFileManager_ = std::make_shared<CCFileManager>(logMgr_->logger(), applicationSettings_
      , authSignManager_, connectionManager_);
   assetManager_ = std::make_shared<AssetManager>(logMgr_->logger(), walletsMgr_, mdProvider_, celerConnection_);
   assetManager_->init();

   connect(ccFileManager_.get(), &CCFileManager::CCSecurityDef, assetManager_.get(), &AssetManager::onCCSecurityReceived);
   connect(ccFileManager_.get(), &CCFileManager::CCSecurityInfo, walletsMgr_.get(), &bs::sync::WalletsManager::onCCSecurityInfo);
   connect(ccFileManager_.get(), &CCFileManager::Loaded, walletsMgr_.get(), &bs::sync::WalletsManager::onCCInfoLoaded);
   connect(ccFileManager_.get(), &CCFileManager::LoadingFailed, this, &BSTerminalMainWindow::onCCInfoMissing);

   connect(mdProvider_.get(), &MarketDataProvider::MDUpdate, assetManager_.get(), &AssetManager::onMDUpdate);

   if (!ccFileManager_->hasLocalFile()) {
      logMgr_->logger()->info("Request for CC definitions from Public Bridge");
      ccFileManager_->LoadCCDefinitionsFromPub();
   }
   else {
      ccFileManager_->LoadSavedCCDefinitions();
   }
}

void BSTerminalMainWindow::InitPortfolioView()
{
   portfolioModel_ = std::make_shared<CCPortfolioModel>(walletsMgr_, assetManager_, this);
   ui_->widgetPortfolio->init(applicationSettings_, mdProvider_, portfolioModel_,
                             signContainer_, armory_, logMgr_->logger("ui"),
                             walletsMgr_);
}

void BSTerminalMainWindow::InitWalletsView()
{
   ui_->widgetWallets->init(logMgr_->logger("ui"), walletsMgr_, signContainer_
      , applicationSettings_, connectionManager_, assetManager_, authManager_, armory_);
}

void BSTerminalMainWindow::InitChatView()
{
   ui_->widgetChat->init(connectionManager_, applicationSettings_, logMgr_->logger("chat"));

   //connect(ui_->widgetChat, &ChatWidget::LoginFailed, this, &BSTerminalMainWindow::onAutheIDFailed);
}

// Initialize widgets related to transactions.
void BSTerminalMainWindow::InitTransactionsView()
{
   ui_->widgetExplorer->init(armory_, logMgr_->logger());
   ui_->widgetTransactions->init(walletsMgr_, armory_, signContainer_,
                                logMgr_->logger("ui"));
   ui_->widgetTransactions->setEnabled(true);

   ui_->widgetTransactions->SetTransactionsModel(transactionsModel_);
   ui_->widgetPortfolio->SetTransactionsModel(transactionsModel_);
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
   if (!transactionsModel_) {
      transactionsModel_ = std::make_shared<TransactionsViewModel>(armory_
         , walletsMgr_, logMgr_->logger("ui"), this);

      InitTransactionsView();
      transactionsModel_->loadAllWallets();
   }
   updateControlEnabledState();
}

void BSTerminalMainWindow::CompleteDBConnection()
{
   logMgr_->logger("ui")->debug("BSTerminalMainWindow::CompleteDBConnection");
   if (walletsMgr_ && walletsMgr_->hdWalletsCount()) {
      walletsMgr_->registerWallets();
   }
   readyToRegisterWallets_ = true;
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

   setWindowTitle(tr("BlockSettle Terminal"));

//   const auto bsTitle = tr("BlockSettle Terminal [%1]");
//   switch (applicationSettings_->get<NetworkType>(ApplicationSettings::netType)) {
//   case NetworkType::TestNet:
//      setWindowTitle(bsTitle.arg(tr("TESTNET")));
//      break;

//   case NetworkType::RegTest:
//      setWindowTitle(bsTitle.arg(tr("REGTEST")));
//      break;

//   default:
//      setWindowTitle(tr("BlockSettle Terminal"));
//      break;
//   }
}

bool BSTerminalMainWindow::isUserLoggedIn() const
{
   return (celerConnection_ && celerConnection_->IsConnected());
}

bool BSTerminalMainWindow::isArmoryConnected() const
{
   return armory_->state() == ArmoryConnection::State::Ready;
}

void BSTerminalMainWindow::ArmoryIsOffline()
{
   logMgr_->logger("ui")->debug("BSTerminalMainWindow::ArmoryIsOffline");
   if (walletsMgr_) {
      walletsMgr_->unregisterWallets();
   }
   connectArmory();
   updateControlEnabledState();
   // XXX: disabled until armory connection is stable in terminal
   // updateLoginActionState();
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
   ArmorySettings currentArmorySettings = armoryServersProvider_->getArmorySettings();
   armoryServersProvider_->setConnectedArmorySettings(currentArmorySettings);
   armory_->setupConnection(currentArmorySettings
      , [this](const BinaryData& srvPubKey, const std::string& srvIPPort) {
      std::shared_ptr<std::promise<bool>> promiseObj = std::make_shared<std::promise<bool>>();
      std::future<bool> futureObj = promiseObj->get_future();
      QMetaObject::invokeMethod(this, "showArmoryServerPrompt", Qt::QueuedConnection
         , Q_ARG(BinaryData, srvPubKey), Q_ARG(std::string, srvIPPort)
         , Q_ARG(std::shared_ptr<std::promise<bool>>, promiseObj));
      bool result = futureObj.get();

      // stop armory connection loop if server key was rejected
      armory_->needsBreakConnectionLoop_.store(!result);
      armory_->setState(ArmoryConnection::State::Canceled);
      return result;
   });
}

void BSTerminalMainWindow::connectSigner()
{
   if (!signContainer_) {
      return;
   }

   if(!signContainer_->Start()) {
      BSMessageBox(BSMessageBox::warning, tr("BlockSettle Signer Connection")
         , tr("Failed to start signer connection.")).exec();
   }
}

bool BSTerminalMainWindow::createWallet(bool primary, bool reportSuccess)
{
   if (primary && (walletsMgr_->hdWalletsCount() > 0)) {
      auto wallet = walletsMgr_->getHDWallet(0);
      if (wallet->isPrimary()) {
         return true;
      }
      BSMessageBox qry(BSMessageBox::question, tr("Create primary wallet"), tr("Promote to primary wallet")
         , tr("In order to execute trades and take delivery of XBT and Equity Tokens, you are required to"
            " have a Primary Wallet which supports the sub-wallets required to interact with the system.")
         .arg(QString::fromStdString(wallet->name())), this);
      if (qry.exec() == QDialog::Accepted) {
         wallet->createGroup(bs::hd::CoinType::BlockSettle_Auth);
         return true;
      }
      return false;
   }

   if (!signContainer_->isOffline()) {
      NewWalletDialog newWalletDialog(true, applicationSettings_, this);
      if (newWalletDialog.exec() != QDialog::Accepted) {
         return false;
      }

      if (newWalletDialog.isCreate()) {
         return ui_->widgetWallets->CreateNewWallet(reportSuccess);
      }
      else if (newWalletDialog.isImport()) {
         return ui_->widgetWallets->ImportNewWallet(reportSuccess);
      }

      return false;
   } else {
      return ui_->widgetWallets->ImportNewWallet(reportSuccess);
   }
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

void BSTerminalMainWindow::onSignerConnError(const QString &err)
{
   showError(tr("Signer connection error"), tr("Signer connection error details: %1").arg(err));
}

void BSTerminalMainWindow::onReceive()
{
   const auto defWallet = walletsMgr_->getDefaultWallet();
   std::string selWalletId = defWallet ? defWallet->walletId() : std::string{};
   if (ui_->tabWidget->currentWidget() == ui_->widgetWallets) {
      auto wallets = ui_->widgetWallets->getSelectedWallets();
      if (!wallets.empty()) {
         selWalletId = wallets[0]->walletId();
      } else {
         wallets = ui_->widgetWallets->getFirstWallets();

         if (!wallets.empty()) {
            selWalletId = wallets[0]->walletId();
         }
      }
   }
   SelectWalletDialog *selectWalletDialog = new SelectWalletDialog(walletsMgr_, selWalletId, this);
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
   CreateTransactionDialogAdvanced advancedDialog{armory_, walletsMgr_
      , signContainer_, true, logMgr_->logger("ui"), nullptr, this};
   advancedDialog.setOfflineDir(applicationSettings_->get<QString>(ApplicationSettings::signerOfflineDir));

   if (!selectedWalletId.empty()) {
      advancedDialog.SelectWallet(selectedWalletId);
   }

   advancedDialog.exec();
}

void BSTerminalMainWindow::onSend()
{
   std::string selectedWalletId;

   if (ui_->tabWidget->currentWidget() == ui_->widgetWallets) {
      const auto &wallets = ui_->widgetWallets->getSelectedWallets();
      if (wallets.size() == 1) {
         selectedWalletId = wallets[0]->walletId();
      }
   }

   if (QGuiApplication::keyboardModifiers() & Qt::ShiftModifier) {
      createAdvancedTxDialog(selectedWalletId);
   } else {
      if (applicationSettings_->get<bool>(ApplicationSettings::AdvancedTxDialogByDefault)) {
         createAdvancedTxDialog(selectedWalletId);
      } else {
         CreateTransactionDialogSimple dlg{armory_, walletsMgr_, signContainer_
            , logMgr_->logger("ui"),
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

   ui_->menuFile->insertAction(ui_->actionSettings, action_login_);
   ui_->menuFile->insertAction(ui_->actionSettings, action_logout_);

   ui_->menuFile->insertSeparator(action_login_);
   ui_->menuFile->insertSeparator(ui_->actionSettings);

   connect(ui_->actionCreateNewWallet, &QAction::triggered, [ww = ui_->widgetWallets]{ ww->CreateNewWallet(); });
   connect(ui_->actionAuthenticationAddresses, &QAction::triggered, this, &BSTerminalMainWindow::openAuthManagerDialog);
   connect(ui_->actionSettings, &QAction::triggered, this, [=]() { openConfigDialog(); });
   connect(ui_->actionAccountInformation, &QAction::triggered, this, &BSTerminalMainWindow::openAccountInfoDialog);
   connect(ui_->actionEnterColorCoinToken, &QAction::triggered, this, &BSTerminalMainWindow::openCCTokenDialog);

   onUserLoggedOut();

#ifndef Q_OS_MAC
   ui_->horizontalFrame->hide();

   ui_->menubar->setCornerWidget(ui_->pushButtonUser);
#endif
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
   ConfigDialog configDialog(applicationSettings_, armoryServersProvider_, this);
   connect(&configDialog, &ConfigDialog::reconnectArmory, this, &BSTerminalMainWindow::onArmoryNeedsReconnect);
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
   if (walletsMgr_->hasPrimaryWallet() || createWallet(true, false)) {
      CCTokenEntryDialog dialog(walletsMgr_, ccFileManager_, signContainer_, this);
      dialog.exec();
   }
}

void BSTerminalMainWindow::loginToCeler(const std::string& username, const std::string& password)
{
   const std::string host = applicationSettings_->get<std::string>(ApplicationSettings::celerHost);
   const std::string port = applicationSettings_->get<std::string>(ApplicationSettings::celerPort);

   if (host.empty() || port.empty()) {
      logMgr_->logger("ui")->error("[BSTerminalMainWindow::loginToCeler] missing network settings for App server");
      showError(tr("Connection error"), tr("Missing network settings for Blocksettle Server"));
      return;
   }

   if (!celerConnection_->LoginToServer(host, port, username, password)) {
      logMgr_->logger("ui")->error("[BSTerminalMainWindow::loginToCeler] login failed");
      showError(tr("Connection error"), tr("Login failed"));
   } else {
      auto userName = QString::fromStdString(username);
      currentUserLogin_ = userName;
      ui_->widgetWallets->setUsername(userName);
      action_logout_->setVisible(false);
      action_login_->setEnabled(false);
   }
}

void BSTerminalMainWindow::onLogin()
{
   LoadCCDefinitionsFromPuB();

   GetNetworkSettingsFromPuB([this]() {
      OnNetworkSettingsLoaded();
      emit readyToLogin();
   });
}

void BSTerminalMainWindow::onReadyToLogin()
{
   LoginWindow loginDialog(logMgr_->logger("autheID"), applicationSettings_, connectionManager_, this);

   if (loginDialog.exec() == QDialog::Accepted) {
      currentUserLogin_ = loginDialog.getUsername();
      auto id = ui_->widgetChat->login(currentUserLogin_.toStdString(), loginDialog.getJwt());
      setLoginButtonText(currentUserLogin_);

#ifndef PRODUCTION_BUILD
      // TODO: uncomment this section once we have armory connection
      // if (isArmoryConnected()) {
         loginToCeler(loginDialog.getUsername().toStdString()
            , "Welcome1234");
      // } else {
         // logMgr_->logger()->debug("[BSTerminalMainWindow::onReadyToLogin] armory disconnected. Could not login to celer.");
      // }
#endif
   }
}

void BSTerminalMainWindow::onLogout()
{
   ui_->widgetWallets->setUsername(QString());
   ui_->widgetChat->logout();

   if (celerConnection_->IsConnected()) {
      celerConnection_->CloseConnection();
   }
   
   setLoginButtonText(loginButtonText_);
}

void BSTerminalMainWindow::onUserLoggedIn()
{
   ui_->actionAccountInformation->setEnabled(true);
   ui_->actionAuthenticationAddresses->setEnabled(true);
   ui_->actionOneTimePassword->setEnabled(true);
   ui_->actionEnterColorCoinToken->setEnabled(true);

   ui_->actionDeposits->setEnabled(true);
   ui_->actionWithdrawalRequest->setEnabled(true);
   ui_->actionLinkAdditionalBankAccount->setEnabled(true);

   authManager_->ConnectToPublicBridge(connectionManager_, celerConnection_);
   ccFileManager_->ConnectToCelerClient(celerConnection_);

   const auto userId = BinaryData::CreateFromHex(celerConnection_->userId());
   if (signContainer_) {
      signContainer_->SetUserId(userId);
   }
   walletsMgr_->setUserId(userId);

   setLoginButtonText(currentUserLogin_);

   if (!mdProvider_->IsConnectionActive()) {
      mdProvider_->SubscribeToMD();
   }
}

void BSTerminalMainWindow::onUserLoggedOut()
{
   ui_->actionAccountInformation->setEnabled(false);
   ui_->actionAuthenticationAddresses->setEnabled(false);
   ui_->actionEnterColorCoinToken->setEnabled(false);
   ui_->actionOneTimePassword->setEnabled(false);

   ui_->actionDeposits->setEnabled(false);
   ui_->actionWithdrawalRequest->setEnabled(false);
   ui_->actionLinkAdditionalBankAccount->setEnabled(false);

   if (signContainer_) {
      signContainer_->SetUserId(BinaryData{});
   }
   if (walletsMgr_) {
      walletsMgr_->setUserId(BinaryData{});
   }
   if (authManager_) {
      authManager_->OnDisconnectedFromCeler();
   }
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
      logMgr_->logger("ui")->debug("[BSTerminalMainWindow::onCelerConnectionError] login failed. Probably user do not have BS matching account");
      break;
   }

   if (!mdProvider_->IsConnectionActive()) {
      mdProvider_->SubscribeToMD();
   }
}

void BSTerminalMainWindow::createAuthWallet()
{
   if (celerConnection_->tradingAllowed()) {
      if (!walletsMgr_->hasPrimaryWallet() && !createWallet(true)) {
         return;
      }

      if (!walletsMgr_->getAuthWallet()) {
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
      if (!walletsMgr_->hasPrimaryWallet() && !createWallet(true)) {
         return;
      }
      if (!walletsMgr_->hasSettlementWallet()) {
         BSMessageBox createSettlReq(BSMessageBox::question, tr("Create settlement wallet")
            , tr("Settlement wallet missing")
            , tr("You don't have Settlement wallet, yet. Do you wish to create it?")
            , this);
         if (createSettlReq.exec() == QDialog::Accepted) {
            const auto title = tr("Settlement wallet");
            if (walletsMgr_->createSettlementWallet()) {
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

struct BSTerminalMainWindow::TxInfo {
   Tx       tx;
   uint32_t txTime;
   int64_t  value;
   std::shared_ptr<bs::sync::Wallet>   wallet;
   bs::sync::Transaction::Direction    direction;
   QString  mainAddress;
};

void BSTerminalMainWindow::onZCreceived(const std::vector<bs::TXEntry> entries)
{
   if (entries.empty()) {
      return;
   }
   for (const auto &entry : entries) {
      const auto &cbTx = [this, id = entry.id, txTime = entry.txTime, value = entry.value](Tx tx) {
         const auto wallet = walletsMgr_->getWalletById(id);
         if (!wallet) {
            return;
         }
         auto txInfo = new TxInfo { tx, txTime, value, wallet, bs::sync::Transaction::Direction::Unknown, QString() };
         const auto &cbDir = [this, txInfo] (bs::sync::Transaction::Direction dir, std::vector<bs::Address>) {
            txInfo->direction = dir;
            if (!txInfo->mainAddress.isEmpty() && txInfo->wallet) {
               showZcNotification(txInfo);
               delete txInfo;
            }
         };
         const auto &cbMainAddr = [this, txInfo] (QString mainAddr, int addrCount) {
            txInfo->mainAddress = mainAddr;
            if ((txInfo->direction != bs::sync::Transaction::Direction::Unknown) && txInfo->wallet) {
               showZcNotification(txInfo);
               delete txInfo;
            }
         };
         walletsMgr_->getTransactionDirection(tx, id, cbDir);
         walletsMgr_->getTransactionMainAddress(tx, id, (value > 0), cbMainAddr);
      };
      armory_->getTxByHash(entry.txHash, cbTx);
   }
}

void BSTerminalMainWindow::showZcNotification(const TxInfo *txInfo)
{
   QStringList lines;
   lines << tr("Date: %1").arg(UiUtils::displayDateTime(txInfo->txTime));
   lines << tr("TX: %1 %2 %3").arg(tr(bs::sync::Transaction::toString(txInfo->direction)))
      .arg(txInfo->wallet->displayTxValue(txInfo->value)).arg(txInfo->wallet->displaySymbol());
   lines << tr("Wallet: %1").arg(QString::fromStdString(txInfo->wallet->name()));
   lines << txInfo->mainAddress;

   const auto &title = tr("New blockchain transaction");
   NotificationCenter::notify(bs::ui::NotifyType::BlockchainTX, { title, lines.join(tr("\n")) });
}

void BSTerminalMainWindow::showRunInBackgroundMessage()
{
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
   ui_->pushButtonUser->setText(text);

#ifndef Q_OS_MAC
   ui_->menubar->adjustSize();
#endif
}

void BSTerminalMainWindow::onPasswordRequested(const bs::hd::WalletInfo &walletInfo, std::string prompt)
{
   SignContainer::PasswordType password;
   bool cancelledByUser = true;

   if (walletInfo.rootId().isEmpty()) {
      logMgr_->logger("ui")->error("[onPasswordRequested] can\'t ask password for empty wallet id");
   } else {
      QString walletName;
      const auto wallet = walletsMgr_->getWalletById(walletInfo.rootId().toStdString());
      if (wallet != nullptr) {
         // do we need to get name of root wallet?
         walletName = QString::fromStdString(wallet->name());
      } else {
         const auto hdWallet = walletsMgr_->getHDWalletById(walletInfo.rootId().toStdString());
         walletName = QString::fromStdString(hdWallet->name());
      }

      // pass to dialog root wallet id and root name
      bs::hd::WalletInfo walletInfoCopy = walletInfo;
      if (!walletName.isEmpty()) {
         const auto &rootWallet = walletsMgr_->getHDRootForLeaf(walletInfo.rootId().toStdString());
         if (rootWallet) {
            walletInfoCopy.setRootId(rootWallet->walletId());
            walletInfoCopy.setName(rootWallet->name());
         }

         EnterWalletPassword passwordDialog(AutheIDClient::SignWallet, this);
         passwordDialog.init(walletInfoCopy, applicationSettings_, connectionManager_, WalletKeyWidget::UseType::RequestAuthAsDialog
                             , QString::fromStdString(prompt), logMgr_->logger("ui"));

         if (passwordDialog.exec() == QDialog::Accepted) {
            password = passwordDialog.resultingKey();
            cancelledByUser = false;
         }
         else {
            logMgr_->logger("ui")->debug("[onPasswordRequested] user rejected to enter password for wallet {} ( {} )"
               , walletInfo.rootId().toStdString(), walletName.toStdString());
         }
      } else {
         logMgr_->logger("ui")->error("[onPasswordRequested] can\'t find wallet with id {}", walletInfo.rootId().toStdString());
      }
   }

   signContainer_->SendPassword(walletInfo.rootId().toStdString(), password, cancelledByUser);
}

void BSTerminalMainWindow::onCCInfoMissing()
{ }   // do nothing here since we don't know if user will need Private Market before logon to Celer

void BSTerminalMainWindow::setupShortcuts()
{
   auto overviewTabShortcut = new QShortcut(QKeySequence(QString::fromStdString("Ctrl+1")), this);
   overviewTabShortcut->setContext(Qt::WindowShortcut);
   connect(overviewTabShortcut, &QShortcut::activated, [this](){ ui_->tabWidget->setCurrentIndex(0);});

   auto tradingTabShortcut = new QShortcut(QKeySequence(QString::fromStdString("Ctrl+2")), this);
   tradingTabShortcut->setContext(Qt::WindowShortcut);
   connect(tradingTabShortcut, &QShortcut::activated, [this](){ ui_->tabWidget->setCurrentIndex(1);});

   auto dealingTabShortcut = new QShortcut(QKeySequence(QString::fromStdString("Ctrl+3")), this);
   dealingTabShortcut->setContext(Qt::WindowShortcut);
   connect(dealingTabShortcut, &QShortcut::activated, [this](){ ui_->tabWidget->setCurrentIndex(2);});

   auto walletsTabShortcutt = new QShortcut(QKeySequence(QString::fromStdString("Ctrl+4")), this);
   walletsTabShortcutt->setContext(Qt::WindowShortcut);
   connect(walletsTabShortcutt, &QShortcut::activated, [this](){ ui_->tabWidget->setCurrentIndex(3);});

   auto transactionsTabShortcut = new QShortcut(QKeySequence(QString::fromStdString("Ctrl+5")), this);
   transactionsTabShortcut->setContext(Qt::WindowShortcut);
   connect(transactionsTabShortcut, &QShortcut::activated, [this](){ ui_->tabWidget->setCurrentIndex(4);});

   auto alt_1 = new QShortcut(QKeySequence(QString::fromLatin1("Alt+1")), this);
   alt_1->setContext(Qt::WindowShortcut);
   connect(alt_1, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui_->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Alt_1);
      }
   );

   auto alt_2 = new QShortcut(QKeySequence(QString::fromLatin1("Alt+2")), this);
   alt_2->setContext(Qt::WindowShortcut);
   connect(alt_2, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui_->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Alt_2);
      }
   );

   auto alt_3 = new QShortcut(QKeySequence(QString::fromLatin1("Alt+3")), this);
   alt_3->setContext(Qt::WindowShortcut);
   connect(alt_3, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui_->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Alt_3);
      }
   );

   auto ctrl_s = new QShortcut(QKeySequence(QString::fromLatin1("Ctrl+S")), this);
   ctrl_s->setContext(Qt::WindowShortcut);
   connect(ctrl_s, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui_->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Ctrl_S);
      }
   );

   auto ctrl_p = new QShortcut(QKeySequence(QString::fromLatin1("Ctrl+P")), this);
   ctrl_p->setContext(Qt::WindowShortcut);
   connect(ctrl_p, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui_->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Ctrl_P);
      }
   );

   auto ctrl_q = new QShortcut(QKeySequence(QString::fromLatin1("Ctrl+Q")), this);
   ctrl_q->setContext(Qt::WindowShortcut);
   connect(ctrl_q, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui_->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Ctrl_Q);
      }
   );

   auto alt_s = new QShortcut(QKeySequence(QString::fromLatin1("Alt+S")), this);
   alt_s->setContext(Qt::WindowShortcut);
   connect(alt_s, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui_->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Alt_S);
      }
   );

   auto alt_b = new QShortcut(QKeySequence(QString::fromLatin1("Alt+B")), this);
   alt_b->setContext(Qt::WindowShortcut);
   connect(alt_b, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui_->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Alt_B);
      }
   );

   auto alt_p = new QShortcut(QKeySequence(QString::fromLatin1("Alt+P")), this);
   alt_p->setContext(Qt::WindowShortcut);
   connect(alt_p, &QShortcut::activated, [this]() {
         static_cast<TabWithShortcut*>(ui_->tabWidget->currentWidget())->shortcutActivated(
            TabWithShortcut::ShortcutType::Alt_P);
      }
   );
}

void BSTerminalMainWindow::onButtonUserClicked() {
   if (ui_->pushButtonUser->text() == loginButtonText_) {
      onLogin();
   } else {
      if (BSMessageBox(BSMessageBox::question, tr("User Logout"), tr("You are about to logout")
         , tr("Do you want to continue?")).exec() == QDialog::Accepted)
      onLogout();
   }
}

void BSTerminalMainWindow::showArmoryServerPrompt(const BinaryData &srvPubKey, const std::string &srvIPPort, std::shared_ptr<std::promise<bool>> promiseObj)
{
   armoryKeyDialogShown_ = true;
   QList<ArmoryServer> servers = armoryServersProvider_->servers();
   int serverIndex = armoryServersProvider_->indexOfIpPort(srvIPPort);
   if (serverIndex >= 0) {
      ArmoryServer server = servers.at(serverIndex);

      if (server.armoryDBKey.isEmpty()) {
         BSMessageBox *box = new BSMessageBox(BSMessageBox::question
                          , tr("ArmoryDB Key Import")
                          , tr("Do you wish to import the following ArmoryDB Key?")
                          , tr("Address: %1\n"
                               "Port: %2\n"
                               "Key: %3")
                                    .arg(QString::fromStdString(srvIPPort).split(QStringLiteral(":")).at(0))
                                    .arg(QString::fromStdString(srvIPPort).split(QStringLiteral(":")).at(1))
                                    .arg(QString::fromLatin1(QByteArray::fromStdString(srvPubKey.toBinStr()).toHex()))
                          , this);
         box->setMinimumSize(600, 150);
         box->setMaximumSize(600, 150);

         bool answer = (box->exec() == QDialog::Accepted);
         box->deleteLater();

         if (answer) {
            armoryServersProvider_->addKey(srvIPPort, srvPubKey);
         }

         promiseObj->set_value(true);
      }
      else if (server.armoryDBKey != QString::fromLatin1(QByteArray::fromStdString(srvPubKey.toBinStr()).toHex())) {
         BSMessageBox *box = new BSMessageBox(BSMessageBox::warning
                          , tr("ArmoryDB Key")
                          , tr("ArmoryDB Key was changed.\n"
                               "Do you wish to proceed connection and save new key?")
                          , tr("Address: %1\n"
                               "Port: %2\n"
                               "Old Key: %3\n"
                               "New Key: %4")
                                    .arg(QString::fromStdString(srvIPPort).split(QStringLiteral(":")).at(0))
                                    .arg(QString::fromStdString(srvIPPort).split(QStringLiteral(":")).at(1))
                                    .arg(QString::fromLatin1(QByteArray::fromStdString(srvPubKey.toBinStr()).toHex()))
                                    .arg(QString::fromLatin1(QByteArray::fromStdString(srvPubKey.toBinStr()).toHex()))
                          , this);
         box->setMinimumSize(600, 150);
         box->setMaximumSize(600, 150);
         box->setCancelVisible(true);

         bool answer = (box->exec() == QDialog::Accepted);
         box->deleteLater();

         if (answer) {
            armoryServersProvider_->addKey(srvIPPort, srvPubKey);
         }

         promiseObj->set_value(answer);
      }
      else {
         promiseObj->set_value(true);
      }
   }
   else {
      // server not in the list - added directly to ini config
      promiseObj->set_value(true);
   }
}

void BSTerminalMainWindow::onArmoryNeedsReconnect()
{
   disconnect(statusBarView_.get(), 0, 0, 0);
   statusBarView_->deleteLater();
   QApplication::processEvents();

   initArmory();
   LoadWallets();

   QApplication::processEvents();

   statusBarView_ = std::make_shared<StatusBarView>(armory_, walletsMgr_, assetManager_, celerConnection_
      , signContainer_, ui_->statusbar);

   InitWalletsView();


   widgetsInited_ = false;
   InitSigningContainer();
   InitAuthManager();

   connectSigner();
   connectArmory();
}
