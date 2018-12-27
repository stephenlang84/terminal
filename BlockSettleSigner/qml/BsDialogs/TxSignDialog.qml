import QtQuick 2.9
import QtQuick.Layouts 1.0
import QtQuick.Controls 2.2

import com.blocksettle.TXInfo 1.0
import com.blocksettle.AutheIDClient 1.0
import com.blocksettle.AuthSignWalletObject 1.0
import com.blocksettle.WalletInfo 1.0
import com.blocksettle.QSeed 1.0
import com.blocksettle.QPasswordData 1.0
import com.blocksettle.NsWallet.namespace 1.0

import "../StyledControls"
import "../BsControls"
import "../js/helper.js" as JsHelper

CustomTitleDialogWindow {
    property string prompt
    property WalletInfo walletInfo: WalletInfo{}
    property TXInfo txInfo
    property QPasswordData passwordData: QPasswordData{}
    property bool   acceptable: walletInfo.encType === NsWallet.Password ? tfPassword.text : true
    property bool   cancelledByUser: false
    property AuthSignWalletObject  authSign
    //property string encKey

    title: qsTr("Wallet Password Confirmation")

//    function confirmClicked() {
//        if (txInfo.walletInfo.encType === NsWallet.Password) {
//            //password = JsHelper.toHex(tfPassword.text)
//            passwordData.textPassword = tfPassword.text
//            passwordData.encType = NsWallet.Password
//        }
//        acceptAnimated()
//    }

//    onTxInfoChanged: {
//        console.log("QML onTxInfoChanged")
//        if (txInfo.walletInfo.encType === NsWallet.Auth) {
//            JsHelper.requesteIdAuth(AutheIDClient.SignWallet
//                                    , walletInfo
//                                    , function(pd){
//                                        passwordData = pd
//                                        acceptAnimated()
//                                    })


////            authSign = authProxy.signWallet(AutheIDClient.SignWallet, prompt,
////                                            txInfo.walletInfo.rootId, txInfo.walletInfo.encKey)

////            authSign.succeeded.connect(function(encKey_, password_) {
////                console.log("authSign.succeeded.connect " + encKey_)
////                console.log("authSign.succeeded.connect " + password_)

////                acceptable = true
////                passwordData.binaryPassword =
////                encKey = encKey_
////                seed.password = password_
////                acceptAnimated()
////            })
////            authSign.failed.connect(function(text) {
////                rejectAnimated()
////            })
//        }
//    }

    Connections {
        target: qmlAppObj

        onCancelSignTx: {
            if (txId === txInfo.txId) {
                rejectAnimated()
            }
        }
    }

    cContentItem: ColumnLayout {
        spacing: 10

        CustomLabel {
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            visible: !txInfo.nbInputs && txInfo.walletInfo.name.length
            text: qsTr("Wallet %1").arg(txInfo.walletInfo.name)
        }

        GridLayout {
            id: gridDashboard
            visible: txInfo.nbInputs
            columns: 2
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            rowSpacing: 0

            CustomHeader {
                Layout.fillWidth: true
                Layout.columnSpan: 2
                text: qsTr("Details")
                Layout.preferredHeight: 25
            }

            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Sending Wallet")
            }
            CustomLabelValue {
                text: txInfo.walletInfo.name
                Layout.alignment: Qt.AlignRight
            }

            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("No. of Inputs")
            }
            CustomLabelValue {
                text: txInfo.nbInputs
                Layout.alignment: Qt.AlignRight
            }

            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Receiving Address(es)")
                verticalAlignment: Text.AlignTop
                Layout.fillHeight: true
            }
            ColumnLayout{
                spacing: 0
                Layout.leftMargin: 0
                Layout.rightMargin: 0
                Repeater {
                    model: txInfo.recvAddresses
                    CustomLabelValue {
                        text: modelData
                        Layout.alignment: Qt.AlignRight
                    }
                }
            }

            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Transaction Size")
            }
            CustomLabelValue {
                text: txInfo.txVirtSize
                Layout.alignment: Qt.AlignRight
            }

            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Input Amount")
            }
            CustomLabelValue {
                text: txInfo.inputAmount.toFixed(8)
                Layout.alignment: Qt.AlignRight
            }

            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Return Amount")
            }
            CustomLabelValue {
                text: txInfo.changeAmount.toFixed(8)
                Layout.alignment: Qt.AlignRight
            }

            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Transaction Fee")
            }
            CustomLabelValue {
                text: txInfo.fee.toFixed(8)
                Layout.alignment: Qt.AlignRight
            }

            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Transaction Amount")
            }
            CustomLabelValue {
                text: txInfo.total.toFixed(8)
                Layout.alignment: Qt.AlignRight
            }
        }

        RowLayout {
            spacing: 5
            Layout.fillWidth: true
            Layout.leftMargin: 10
            Layout.rightMargin: 10

            CustomHeader {
                Layout.fillWidth: true
                text: qsTr("Password Confirmation")
                Layout.preferredHeight: 25
            }
        }

        RowLayout {
            spacing: 5
            Layout.fillWidth: true
            Layout.leftMargin: 10
            Layout.rightMargin: 10

            CustomLabel {
                visible: prompt.length
                Layout.minimumWidth: 110
                Layout.preferredWidth: 110
                Layout.maximumWidth: 110
                Layout.fillWidth: true
                text: prompt
                elide: Label.ElideRight
            }

            CustomPasswordTextInput {
                id: tfPassword
                visible: txInfo.walletInfo.encType === NsWallet.Password
                focus: true
                placeholderText: qsTr("Password")
                echoMode: TextField.Password
                Layout.fillWidth: true
            }

            CustomLabel {
                id: labelAuth
                visible: txInfo.walletInfo.encType === NsWallet.Auth
                text: authSign.status
            }
        }

        ColumnLayout {
            spacing: 5
            Layout.fillWidth: true
            Layout.leftMargin: 10
            Layout.rightMargin: 10

            Timer {
                id: timer
                property real timeLeft: 300
                interval: 500
                running: true
                repeat: true
                onTriggered: {
                    timeLeft -= 0.5
                    if (timeLeft <= 0) {
                        stop()
                        // assume non signed tx is cancelled tx
                        cancelledByUser = true
                        rejectAnimated()
                    }
                }
                signal expired()
            }

            CustomLabel {
                text: qsTr("On completion just press [Enter] or [Return]")
                Layout.fillWidth: true
            }
            CustomLabelValue {
                text: qsTr("%1 seconds left").arg(timer.timeLeft.toFixed((0)))
                Layout.fillWidth: true
            }

            CustomProgressBar {
                Layout.minimumHeight: 6
                Layout.preferredHeight: 6
                Layout.maximumHeight: 6
                Layout.bottomMargin: 10
                Layout.fillWidth: true
                to: 120
                value: timer.timeLeft
            }
        }

    }

    cFooterItem: RowLayout {
        CustomButtonBar {
            Layout.fillWidth: true

            CustomButton {
                text: qsTr("Cancel")
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                onClicked: {
                    cancelledByUser = true
                    rejectAnimated()
                }
            }

            CustomButtonPrimary {
                text: qsTr("CONFIRM")
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                enabled: tfPassword.text.length || acceptable
                id: confirmButton
                onClicked: {
                    //confirmClicked();

                    console.log("TxSignDialog.qml onClicked walletId " + txInfo.walletInfo.walletId)
                    console.log("TxSignDialog.qml onClicked encType " + txInfo.walletInfo.encType)
                    console.log("TxSignDialog.qml onClicked walletInfo " + txInfo.walletInfo)

                    if (txInfo.walletInfo.encType === NsWallet.Password) {
                        //password = JsHelper.toHex(tfPassword.text)
                        passwordData.textPassword = tfPassword.text
                        passwordData.encType = NsWallet.Password
                        acceptAnimated()
                    }
                    else if (txInfo.walletInfo.encType === NsWallet.Auth) {
                        JsHelper.requesteIdAuth(AutheIDClient.SignWallet
                                                , walletInfo
                                                , function(pd){
                                                    passwordData = pd
                                                    acceptAnimated()
                                                })

                    }
                    else {
                        passwordData.encType = NsWallet.Unencrypted
                        acceptAnimated()
                    }
                }
            }
        }
    }
}
