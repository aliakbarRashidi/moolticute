/******************************************************************************
 **  Copyright (c) Raoul Hecky. All Rights Reserved.
 **
 **  Moolticute is free software; you can redistribute it and/or modify
 **  it under the terms of the GNU General Public License as published by
 **  the Free Software Foundation; either version 3 of the License, or
 **  (at your option) any later version.
 **
 **  Moolticute is distributed in the hope that it will be useful,
 **  but WITHOUT ANY WARRANTY; without even the implied warranty of
 **  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 **  GNU General Public License for more details.
 **
 **  You should have received a copy of the GNU General Public License
 **  along with Foobar; if not, write to the Free Software
 **  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 **
 ******************************************************************************/
#include "WSServerCon.h"
#include "WSServer.h"
#include "version.h"
#include "ParseDomain.h"
#include "MPDeviceBleImpl.h"
#include "HaveIBeenPwned.h"

#include <QCryptographicHash>

WSServerCon::WSServerCon(QWebSocket *conn):
    wsClient(conn),
    clientUid(Common::createUid(QStringLiteral("ws-"))),
    hibp(new HaveIBeenPwned(this))
{
    connect(wsClient, &QWebSocket::textMessageReceived, this, &WSServerCon::processMessage);
    connect(hibp, &HaveIBeenPwned::sendPwnedMessage, this, &WSServerCon::sendHibpNotification);
}

WSServerCon::~WSServerCon()
{
    delete wsClient;
}

void WSServerCon::sendJsonMessage(const QJsonObject &data)
{
    QJsonDocument jdoc(data);
    wsClient->sendTextMessage(jdoc.toJson(QJsonDocument::JsonFormat::Compact));
    // wsClient->flush();
}

void WSServerCon::sendJsonMessageString(const QString &data)
{
    wsClient->sendTextMessage(data);
}

void WSServerCon::processMessage(const QString &message)
{
    QJsonParseError err;
    QJsonDocument jdoc = QJsonDocument::fromJson(message.toUtf8(), &err);

    if (!message.startsWith("{\"ping"))
        qDebug().noquote() << "JSON API recv:" << Common::maskLog(message);

    if (err.error != QJsonParseError::NoError)
    {
        qWarning() << "JSON parse error " << err.errorString();
        return;
    }

    QJsonObject root = jdoc.object();

    /* API that does not require device */
    if (root["msg"] == "show_app")
    {
        //broadcast the message to all clients
        emit notifyAllClients(root);
        return;
    }
    else if (root["msg"] == "get_application_id")
    {
        QJsonObject ores;
        QJsonObject oroot = root;
        ores["application_name"] = "moolticute";
        ores["application_version"] = QStringLiteral(APP_VERSION);
        oroot["data"] = ores;
        oroot["msg"] = "get_application_id";
        sendJsonMessage(oroot);
        return;
    }
    else if (root["msg"] == "show_status_notification_warning")
    {
        QJsonDocument showWarningDoc(root);
        bool isGuiRunning = false;
        emit sendMessageToGUI(showWarningDoc.toJson(), isGuiRunning);
        if (!isGuiRunning)
        {
            qDebug() << "Cannot show status notification warning, because Moolticute is not running";
        }
        return;
    }

    //Strip the data for the progress lambda,
    //uneeded data should not be passed around
    QJsonObject rootStripped = root;
    rootStripped.remove("data");

    //Default progress callback handling
    auto defaultProgressCb = [=](QVariantMap progressData)
    {
        if (!WSServer::Instance()->checkClientExists(this))
            return;

        int total = progressData.value("total", QVariant(-1)).toInt();
        int current = progressData.value("current", QVariant(-1)).toInt();

        if (current > total)
            current = total;

        QJsonObject ores;
        QJsonObject oroot = rootStripped;
        ores["progress_total"] = total;
        ores["progress_current"] = current;

        oroot["msg"] = "progress"; //change msg to avoid breaking of client waiting of the response
        sendJsonMessage(oroot);

        // New progress message
        if (progressData.contains("msg")) {
            ores["progress_message"] = progressData["msg"].toString();
            if (progressData.contains("msg_args"))
            {
                auto message_args = progressData["msg_args"].toList();
                auto message_args_json = QJsonValue::fromVariant(message_args);
                ores["progress_message_args"] = message_args_json;
            }
        }
        oroot["data"] = ores;
        oroot["msg"] = "progress_detailed"; //change msg to avoid breaking of client waiting of the response
        sendJsonMessage(oroot);
    };

    if (!mpdevice)
    {
        sendFailedJson(root, "No device connected");
        return;
    }

    if (checkMemModeEnabled(root))
        return;

    if (mpdevice->isBLE())
    {
        processMessageBLE(root, defaultProgressCb);
    }
    else
    {
        processMessageMini(root, defaultProgressCb);
    }
}

void WSServerCon::sendFailedJson(QJsonObject obj, QString errstr, int errCode)
{
    QJsonObject odata;
    odata["failed"] = true;
    if (!errstr.isEmpty())
        odata["error_message"] = errstr;
    if (errCode != -999)
        odata["error_code"] = errCode;
    obj["data"] = odata;
    sendJsonMessage(obj);
}

void WSServerCon::resetDevice(MPDevice *dev)
{
    mpdevice = dev;

    if (!mpdevice)
    {
        sendJsonMessage({{ "msg", "mp_disconnected" }});
        return;
    }

    sendJsonMessage({{ "msg", "mp_connected" }});

    //Whenever mp status changes, send state update to client
    connect(mpdevice, &MPDevice::statusChanged, this, &WSServerCon::statusChanged);

    connect(mpdevice, SIGNAL(keyboardLayoutChanged(int)), this, SLOT(sendKeyboardLayout()));
    connect(mpdevice, SIGNAL(lockTimeoutEnabledChanged(bool)), this, SLOT(sendLockTimeoutEnabled()));
    connect(mpdevice, SIGNAL(lockTimeoutChanged(int)), this, SLOT(sendLockTimeout()));
    connect(mpdevice, SIGNAL(screensaverChanged(bool)), this, SLOT(sendScreensaver()));
    connect(mpdevice, SIGNAL(userRequestCancelChanged(bool)), this, SLOT(sendUserRequestCancel()));
    connect(mpdevice, SIGNAL(userInteractionTimeoutChanged(int)), this, SLOT(sendUserInteractionTimeout()));
    connect(mpdevice, SIGNAL(flashScreenChanged(bool)), this, SLOT(sendFlashScreen()));
    connect(mpdevice, SIGNAL(offlineModeChanged(bool)), this, SLOT(sendOfflineMode()));
    connect(mpdevice, SIGNAL(tutorialEnabledChanged(bool)), this, SLOT(sendTutorialEnabled()));
    connect(mpdevice, SIGNAL(memMgmtModeChanged(bool)), this, SLOT(sendMemMgmtMode()));
    connect(mpdevice, SIGNAL(flashMbSizeChanged(int)), this, SLOT(sendVersion()));
    connect(mpdevice, SIGNAL(hwVersionChanged(QString)), this, SLOT(sendVersion()));
    connect(mpdevice, SIGNAL(serialNumberChanged(quint32)), this, SLOT(sendVersion()));
    connect(mpdevice, SIGNAL(screenBrightnessChanged(int)), this, SLOT(sendScreenBrightness()));
    connect(mpdevice, SIGNAL(knockEnabledChanged(bool)), this, SLOT(sendKnockEnabled()));
    connect(mpdevice, SIGNAL(knockSensitivityChanged(int)), this, SLOT(sendKnockSensitivity()));
    connect(mpdevice, SIGNAL(randomStartingPinChanged(bool)), this, SLOT(sendRandomStartingPin()));
    connect(mpdevice, SIGNAL(hashDisplayChanged(bool)), this, SLOT(sendHashDisplayEnabled()));
    connect(mpdevice, SIGNAL(lockUnlockModeChanged(int)), this, SLOT(sendLockUnlockMode()));

    connect(mpdevice, SIGNAL(keyAfterLoginSendEnableChanged(bool)), this, SLOT(sendKeyAfterLoginSendEnable()));
    connect(mpdevice, SIGNAL(keyAfterLoginSendChanged(int)), this, SLOT(sendKeyAfterLoginSend()));
    connect(mpdevice, SIGNAL(keyAfterPassSendEnableChanged(bool)), this, SLOT(sendKeyAfterPassSendEnable()));
    connect(mpdevice, SIGNAL(keyAfterPassSendChanged(int)), this, SLOT(sendKeyAfterPassSend()));
    connect(mpdevice, SIGNAL(delayAfterKeyEntryEnableChanged(bool)), this, SLOT(sendDelayAfterKeyEntryEnable()));
    connect(mpdevice, SIGNAL(delayAfterKeyEntryChanged(int)), this, SLOT(sendDelayAfterKeyEntry()));

    connect(mpdevice, SIGNAL(uidChanged(qint64)), this, SLOT(sendDeviceUID()));

    connect(mpdevice, &MPDevice::filesCacheChanged, this, &WSServerCon::sendFilesCache);

    connect(mpdevice, &MPDevice::dbChangeNumbersChanged, this, &WSServerCon::sendCardDbMetadata);

}

void WSServerCon::statusChanged()
{
    qDebug() << "Update client status changed: " << this;
    if (!mpdevice)
        return;
    sendJsonMessage({{ "msg", "status_changed" },
                     { "data", Common::MPStatusString[mpdevice->get_status()] }});
}

void WSServerCon::sendInitialStatus()
{
    //Sends initial status to any new connected client
    //is any mp connected? and if true send mp state too

    if (!mpdevice)
        sendJsonMessage({{ "msg", "mp_disconnected" }});
    else
    {
        sendJsonMessage({{ "msg", "mp_connected" }});
        sendJsonMessage({{ "msg", "status_changed" },
                         { "data", Common::MPStatusString[mpdevice->get_status()] }});
        sendKeyboardLayout();
        sendLockTimeoutEnabled();
        sendLockTimeout();
        sendScreensaver();
        sendUserRequestCancel();
        sendUserInteractionTimeout();
        sendFlashScreen();
        sendOfflineMode();
        sendTutorialEnabled();
        sendMemMgmtMode();
        sendVersion();
        sendScreenBrightness();
        sendKnockEnabled();
        sendKnockSensitivity();
        sendRandomStartingPin();
        sendHashDisplayEnabled();
        sendLockUnlockMode();
        sendKeyAfterLoginSendEnable();
        sendKeyAfterLoginSend();
        sendKeyAfterPassSendEnable();
        sendKeyAfterPassSend();
        sendDelayAfterKeyEntryEnable();
        sendDelayAfterKeyEntry();
        sendCardDbMetadata();
    }
}

void WSServerCon::sendKeyboardLayout()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "keyboard_layout" },
                        { "value", mpdevice->get_keyboardLayout() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendLockTimeoutEnabled()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "lock_timeout_enabled" },
                        { "value", mpdevice->get_lockTimeoutEnabled() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendLockTimeout()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "lock_timeout" },
                        { "value", mpdevice->get_lockTimeout() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendScreensaver()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "screensaver" },
                        { "value", mpdevice->get_screensaver() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendUserRequestCancel()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "user_request_cancel" },
                        { "value", mpdevice->get_userRequestCancel() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendUserInteractionTimeout()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "user_interaction_timeout" },
                        { "value", mpdevice->get_userInteractionTimeout() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendFlashScreen()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "flash_screen" },
                        { "value", mpdevice->get_flashScreen() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendOfflineMode()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "offline_mode" },
                        { "value", mpdevice->get_offlineMode() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendTutorialEnabled()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "tutorial_enabled" },
                        { "value", mpdevice->get_tutorialEnabled() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendScreenBrightness()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "screen_brightness" },
                        { "value", mpdevice->get_screenBrightness() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendKnockEnabled()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "knock_enabled" },
                        { "value", mpdevice->get_knockEnabled() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendKnockSensitivity()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "knock_sensitivity" },
                        { "value", mpdevice->get_knockSensitivity() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}


void WSServerCon::sendRandomStartingPin()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "random_starting_pin" },
                        { "value", mpdevice->get_randomStartingPin() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendHashDisplayEnabled()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "hash_display" },
                        { "value", mpdevice->get_hashDisplay() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendLockUnlockMode()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "lock_unlock_mode" },
                        { "value", mpdevice->get_lockUnlockMode() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendKeyAfterLoginSendEnable()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "key_after_login_enabled" },
                        { "value", mpdevice->get_keyAfterLoginSendEnable() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendKeyAfterLoginSend()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "key_after_login" },
                        { "value", mpdevice->get_keyAfterLoginSend() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendKeyAfterPassSendEnable()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "key_after_pass_enabled" },
                        { "value", mpdevice->get_keyAfterPassSendEnable() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendKeyAfterPassSend()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "key_after_pass" },
                        { "value", mpdevice->get_keyAfterPassSend() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendDelayAfterKeyEntryEnable()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "delay_after_key_enabled" },
                        { "value", mpdevice->get_delayAfterKeyEntryEnable() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendDelayAfterKeyEntry()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "parameter", "delay_after_key" },
                        { "value", mpdevice->get_delayAfterKeyEntry() }};
    sendJsonMessage({{ "msg", "param_changed" }, { "data", data }});
}

void WSServerCon::sendMemMgmtMode()
{
    if (!mpdevice)
        return;
    sendJsonMessage({{ "msg", "memorymgmt_changed" },
                     { "data", mpdevice->get_memMgmtMode() }});

    QJsonArray logins;
    foreach (MPNode *n, mpdevice->getLoginNodes())
    {
        logins.append(n->toJson());
    }

    QJsonArray datas;
    foreach (MPNode *n, mpdevice->getDataNodes())
    {
        datas.append(n->toJson());
    }

    QJsonObject jdata;
    jdata["login_nodes"] = logins;
    jdata["data_nodes"] = datas;

    sendJsonMessage({{ "msg", "memorymgmt_data" },
                     { "data", jdata }});
}

void WSServerCon::sendVersion()
{
    if (!mpdevice)
        return;
    QJsonObject data = {{ "hw_version", mpdevice->get_hwVersion() },
                        { "flash_size", mpdevice->get_flashMbSize() }};
    data["hw_serial"] = static_cast<qint64>(mpdevice->get_serialNumber());
    if (mpdevice->isBLE())
    {
        data["hw_version"] = "ble";
        if (auto bleImpl = mpdevice->ble())
        {
            data["aux_mcu_version"] = bleImpl->get_auxMCUVersion();
            data["main_mcu_version"] = bleImpl->get_mainMCUVersion();
        }
    }
    sendJsonMessage({{ "msg", "version_changed" }, { "data", data }});
}

void WSServerCon::sendDeviceUID()
{
    if (!mpdevice)
        return;
    sendJsonMessage({{ "msg", "device_uid" },
                     { "data", QJsonObject{ {"uid", mpdevice->get_uid()} } }
                    });
}

void WSServerCon::sendFilesCache()
{
    if (!mpdevice->hasFilesCache())
    {
        qDebug() << "There is fo files cache to send";
        return;
    }

    auto deviceStatus = mpdevice->get_status();
    if (deviceStatus != Common::Unlocked)
    {
        qDebug() << "It's an unknown smartcard or it's locked, no need to search for files cache";
        return;
    }

    qDebug() << "Sending files cache";
    QJsonObject oroot = { {"msg", "files_cache_list"} };
    QJsonArray array;
    for (QVariantMap item : mpdevice->getFilesCache())
        array.append(QJsonDocument::fromVariant(item).object());

    oroot["data"] = array;
    oroot["sync"] = mpdevice->isFilesCacheInSync();
    sendJsonMessage(oroot);
}

void WSServerCon::sendCardDbMetadata()
{
    qDebug() << "Send card db metadata";
    QByteArray cpz = mpdevice->get_cardCPZ();
    int credentialsCn = mpdevice->get_credentialsDbChangeNumber();
    int dataCn = mpdevice->get_dataDbChangeNumber();
    if (cpz.isEmpty())
    {
        qDebug() << "There is no card data to be send.";
        return;
    }
    else
    {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData("mooltipass");
        hash.addData(cpz);
        QString cardId = hash.result().toHex();

        QJsonObject oroot = { {"msg", "card_db_metadata"} };
        QJsonObject data;
        data.insert("cardId", cardId);
        data.insert("credentialsDbChangeNumber", credentialsCn);
        data.insert("dataDbChangeNumber", dataCn);
        oroot["data"] = data;

        sendJsonMessage(oroot);
        qDebug() << "Sended card db metadata";
    }
}

void WSServerCon::sendHibpNotification(QString message)
{
    QJsonObject oroot = { {"msg", "send_hibp"} };
    QJsonObject data;
    data.insert("message", message);
    oroot["data"] = data;

    bool isGuiRunning;
    emit sendMessageToGUI(QJsonDocument(oroot).toJson(QJsonDocument::JsonFormat::Compact), isGuiRunning);

    if (!isGuiRunning)
    {
        qDebug() << "Cannot send pwned notification to GUI: " << message;
    }
}

void WSServerCon::processParametersSet(const QJsonObject &data)
{
    if (!mpdevice)
        return;
    if (data.contains("keyboard_layout"))
        mpdevice->updateKeyboardLayout(data["keyboard_layout"].toInt());
    if (data.contains("lock_timeout_enabled"))
        mpdevice->updateLockTimeoutEnabled(data["lock_timeout_enabled"].toBool());
    if (data.contains("lock_timeout"))
        mpdevice->updateLockTimeout(data["lock_timeout"].toInt());
    if (data.contains("screensaver"))
        mpdevice->updateScreensaver(data["screensaver"].toBool());
    if (data.contains("user_request_cancel"))
        mpdevice->updateUserRequestCancel(data["user_request_cancel"].toBool());
    if (data.contains("user_interaction_timeout"))
        mpdevice->updateUserInteractionTimeout(data["user_interaction_timeout"].toInt());
    if (data.contains("flash_screen"))
        mpdevice->updateFlashScreen(data["flash_screen"].toBool());
    if (data.contains("offline_mode"))
        mpdevice->updateOfflineMode(data["offline_mode"].toBool());
    if (data.contains("tutorial_enabled"))
        mpdevice->updateTutorialEnabled(data["tutorial_enabled"].toBool());
    if (data.contains("screen_brightness"))
        mpdevice->updateScreenBrightness(data["screen_brightness"].toInt());
    if (data.contains("knock_enabled"))
        mpdevice->updateKnockEnabled(data["knock_enabled"].toBool());
    if (data.contains("knock_sensitivity"))
        mpdevice->updateKnockSensitivity(data["knock_sensitivity"].toInt());
    if (data.contains("random_starting_pin"))
        mpdevice->updateRandomStartingPin(data["random_starting_pin"].toBool());
    if (data.contains("hash_display"))
        mpdevice->updateHashDisplay(data["hash_display"].toBool());
    if (data.contains("lock_unlock_mode"))
        mpdevice->updateLockUnlockMode(data["lock_unlock_mode"].toInt());
    if (data.contains("key_after_login_enabled"))
         mpdevice->updateKeyAfterLoginSendEnable(data["key_after_login_enabled"].toBool());
    if (data.contains("key_after_login"))
         mpdevice->updateKeyAfterLoginSend(data["key_after_login"].toInt());
    if (data.contains("key_after_pass_enabled"))
         mpdevice->updateKeyAfterPassSendEnable(data["key_after_pass_enabled"].toBool());
    if (data.contains("key_after_pass"))
         mpdevice->updateKeyAfterPassSend(data["key_after_pass"].toInt());
    if (data.contains("delay_after_key_enabled"))
         mpdevice->updateDelayAfterKeyEntryEnable( data["delay_after_key_enabled"].toBool());
    if (data.contains("delay_after_key"))
         mpdevice->updateDelayAfterKeyEntry(data["delay_after_key"].toInt());

    //reload parameters from device after changed all params, this will trigger
    //websocket update of clients too
    mpdevice->loadParameters();
}

QString WSServerCon::getRequestId(const QJsonValue &v)
{
    if (v.isDouble())
        return QString::number(v.toInt());
    return v.toString();
}

void WSServerCon::processMessageMini(QJsonObject root, const MPDeviceProgressCb &cbProgress)
{
    if (root["msg"] == "param_set")
    {
        processParametersSet(root["data"].toObject());
    }
    else if (root["msg"] == "start_memorymgmt")
    {
        QJsonObject o = root["data"].toObject();

        WSServer::Instance()->setMemLockedClient(clientUid);

        //send command to start MMM
        mpdevice->startMemMgmtMode(o["want_data"].toBool(),
                cbProgress,
                [=](bool success, int errCode, QString errMsg)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                QJsonObject oroot = root;
                oroot["msg"] = "failed_memorymgmt";
                sendFailedJson(oroot, errMsg, errCode);
            }
        });
    }
    else if (root["msg"] == "exit_memorymgmt")
    {
        //send command to exit MMM
        mpdevice->exitMemMgmtMode();
    }
    else if (root["msg"] == "start_memcheck")
    {
        //start integrity check
        mpdevice->startIntegrityCheck(
                    [=](bool success, int freeBlocks, int totalBlocks, QString errstr)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            QJsonObject oroot = root;
            oroot["msg"] = "memcheck";

            if (!success)
            {
                sendFailedJson(oroot, errstr);
                return;
            }

            QJsonObject ores;
            ores["memcheck_status"] = "done"; //TODO: add return info here about the result of memcheck?
            ores["free_blocks"] = freeBlocks;
            ores["total_blocks"] = totalBlocks;
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        },
        cbProgress);
    }
    else if (root["msg"] == "ask_password" ||
             root["msg"] == "get_credential")
    {
        QJsonObject o = root["data"].toObject();

        QString reqid;
        if (o.contains("request_id"))
            reqid = QStringLiteral("%1-%2").arg(clientUid).arg(getRequestId(o["request_id"]));

        mpdevice->getCredential(o["service"].toString(), o["login"].toString(), o["fallback_service"].toString(),
                reqid,
                [=](bool success, QString errstr, const QString &service, const QString &login, const QString &pass, const QString &desc)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QSettings s;
            if (s.value("settings/enable_hibp_check").toBool())
            {
                QString formatString = service + ": " + login + ": ";
                formatString += HIBP_COMPROMISED_FORMAT;
                hibp->isPasswordPwned(pass, formatString);
            }
            QJsonObject ores;
            QJsonObject oroot = root;
            ores["service"] = service;
            ores["login"] = login;
            ores["password"] = pass;
            if (mpdevice && mpdevice->isFw12()) //only add description for fw > 1.2
                ores["description"] = desc;
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        });
    }
    else if (root["msg"] == "set_credential")
    {
        QJsonObject o = root["data"].toObject();
        QString loginName = o["login"].toString();
        bool isMsgContainsExtInfo = o.contains("extension_version") || o.contains("mc_cli_version");
        bool isGuiRunning = false;
        if (loginName.isEmpty() && isMsgContainsExtInfo && !o.contains("saveLoginConfirmed"))
        {
            root["msg"] = "request_login";
            QJsonDocument requestLoginDoc(root);
            emit sendMessageToGUI(requestLoginDoc.toJson(), isGuiRunning);
            if (isGuiRunning)
            {
                return;
            }
            qDebug() << "GUI is not running, saving credential with empty login";
        }

        QString originalService = o["service"].toString();
        ParseDomain url(originalService);
        QSettings s;
        bool isSubdomainSelectionEnabled = s.value("settings/enable_subdomain_selection").toBool() && url.isWebsite();
        bool isManualCredential = o.contains("saveManualCredential");
        if (!url.subdomain().isEmpty() && isMsgContainsExtInfo && isSubdomainSelectionEnabled && !isManualCredential && !o.contains("saveDomainConfirmed"))
        {
            root["msg"] = "request_domain";
            o["domain"] = url.getFullDomain();
            o["subdomain"] = url.getFullSubdomain();
            root["data"] = o;
            QJsonDocument requestLoginDoc(root);
            emit sendMessageToGUI(requestLoginDoc.toJson(), isGuiRunning);
            if (isGuiRunning)
            {
                return;
            }
            qDebug() << "GUI is not running, saving credential with subdomain";
        }

        if (!o.contains("saveDomainConfirmed") && url.isWebsite())
        {
            o["service"] = url.getFullDomain();
        }

        if (isManualCredential)
        {
            o["service"] = url.getManuallyEnteredDomainName(originalService);
        }

        const QJsonDocument credDetectedDoc(QJsonObject{{ "msg", "credential_detected" }});
        emit sendMessageToGUI(credDetectedDoc.toJson(QJsonDocument::JsonFormat::Compact), isGuiRunning);

        if (s.value("settings/enable_hibp_check").toBool())
        {
            QString formatString = o["service"].toString() + ": " + loginName + ": ";
            formatString += HIBP_COMPROMISED_FORMAT;
            hibp->isPasswordPwned(o["password"].toString(), formatString);
        }

        mpdevice->setCredential(o["service"].toString(), o["login"].toString(),
                o["password"].toString(), o["description"].toString(), o.contains("description"),
                [=](bool success, QString errstr)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject ores = o;
            QJsonObject oroot = root;
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        });
    }
    else if (root["msg"] == "del_credential")
    {
        QJsonObject o = root["data"].toObject();
        mpdevice->delCredentialAndLeave(o["service"].toString(), o["login"].toString(),
                cbProgress,
                [=](bool success, QString errstr)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject oroot = root;
            oroot["data"] = QJsonObject({{ "success", true }});
            sendJsonMessage(oroot);
        });
    }
    else if (root["msg"] == "request_device_uid")
    {
        QJsonObject o = root["data"].toObject();
        const QByteArray key = o.value("key").toString().toUtf8().simplified();
        mpdevice->getUID(key);
    }

    else if (root["msg"] == "get_random_numbers")
    {
        mpdevice->getRandomNumber([=](bool success, QString errstr, const QByteArray &rndNums)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject oroot = root;
            QJsonArray arr;
            for (int i = 0;i < rndNums.size();i++)
                arr.append(static_cast<quint8>(rndNums.at(i)));
            oroot["data"] = arr;
            sendJsonMessage(oroot);
        });
    }
    else if (root["msg"] == "cancel_request")
    {
        QJsonObject o = root["data"].toObject();
        QString reqid;
        if (o.contains("request_id"))
            reqid = QStringLiteral("%1-%2").arg(clientUid).arg(getRequestId(o["request_id"]));

        mpdevice->cancelUserRequest(reqid);
    }
    else if (root["msg"] == "get_data_node")
    {
        QJsonObject o = root["data"].toObject();
        QString reqid;
        if (o.contains("request_id"))
            reqid = QStringLiteral("%1-%2").arg(clientUid).arg(getRequestId(o["request_id"]));

        mpdevice->getDataNode(o["service"].toString(), o["fallback_service"].toString(),
                reqid,
                [=](bool success, QString errstr, const QString &service, const QByteArray &dataNode)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject ores;
            QJsonObject oroot = root;
            ores["service"] = service;
            ores["node_data"] = QString(dataNode.toBase64());
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        },
        cbProgress);
    }
    else if (root["msg"] == "set_data_node")
    {
        QJsonObject o = root["data"].toObject();
        QString service = o["service"].toString();
        QByteArray data = QByteArray::fromBase64(o["node_data"].toString().toLocal8Bit());
        if (data.isEmpty())
        {
            sendFailedJson(root, "node_data is empty");
            return;
        }

        int maxSize = MP_MAX_FILE_SIZE;
        if (service.toLower() == MC_SSH_SERVICE)
            maxSize = MP_MAX_SSH_SIZE;
        if (data.size() > maxSize)
        {
            sendFailedJson(root, "data is too big to be stored in device");
            return;
        }

        mpdevice->setDataNode(service, data,
                [=](bool success, QString errstr)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject ores;
            ores["service"] = service;
            QJsonObject oroot = root;
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        },
        cbProgress);
    }
    else if (root["msg"] == "delete_data_nodes")
    {
        QJsonObject o = root["data"].toObject();

        if (!mpdevice->get_memMgmtMode())
        {
            sendFailedJson(root, "Not in memory management mode");
            return;
        }

        QJsonArray jarr = o["services"].toArray();
        QStringList services;
        for (int i = 0;i < jarr.size();i++)
            services.append(jarr[i].toString());

        mpdevice->deleteDataNodesAndLeave(services,
                [=](bool success, QString errstr)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject oroot = root;
            oroot["data"] = QJsonObject({{ "success", true }});
            sendJsonMessage(oroot);
        },
        cbProgress);
    }
    else if (root["msg"] == "credential_exists")
    {
        QJsonObject o = root["data"].toObject();

        QString reqid;
        if (o.contains("request_id"))
            reqid = QStringLiteral("%1-%2").arg(clientUid).arg(getRequestId(o["request_id"]));

        mpdevice->serviceExists(false, o["service"].toString(),
                reqid,
                [=](bool success, QString errstr, const QString &service, bool exists)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject ores;
            QJsonObject oroot = root;
            ores["service"] = service;
            ores["exists"] = exists;
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        });
    }
    else if (root["msg"] == "data_node_exists")
    {
        QJsonObject o = root["data"].toObject();

        QString reqid;
        if (o.contains("request_id"))
            reqid = QStringLiteral("%1-%2").arg(clientUid).arg(getRequestId(o["request_id"]));

        mpdevice->serviceExists(true, o["service"].toString(),
                reqid,
                [=](bool success, QString errstr, const QString &service, bool exists)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject ores;
            QJsonObject oroot = root;
            ores["service"] = service;
            ores["exists"] = exists;
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        });
    }
    else if (root["msg"] == "set_credentials")
    {
        if (!mpdevice->get_memMgmtMode())
        {
            sendFailedJson(root, "Not in memory management mode");
            return;
        }

        mpdevice->setMMCredentials(
                    root["data"].toArray(),
                    false,
                    cbProgress,
                    [=](bool success, QString errstr)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject ores;
            QJsonObject oroot = root;
            ores["success"] = "true";
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        });
    }
    else if (root["msg"] == "export_database")
    {
        QString encryptionMethod  = "none";
        if (root.contains("data"))
        {
            QJsonObject o = root["data"].toObject();
            encryptionMethod = o.value("encryption").toString();
        }

        mpdevice->exportDatabase(encryptionMethod,
                                 [=](bool success, QString errstr, QByteArray fileData)
        {
            qDebug() << "send exported DB on WS: success:" << success
                     << ", fileData size:" << fileData.size()
                     << ", errstr:" << errstr;

            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject ores;
            QJsonObject oroot = root;
            ores["file_data"] = QString(fileData.toBase64());
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        },
        cbProgress);
    }
    else if (root["msg"] == "import_database")
    {
        QJsonObject o = root["data"].toObject();

        QByteArray data = QByteArray::fromBase64(o["file_data"].toString().toLocal8Bit());
        if (data.isEmpty())
        {
            sendFailedJson(root, "file_data is empty");
            return;
        }

        mpdevice->importDatabase(data, o["no_delete"].toBool(),
                    [=](bool success, QString errstr)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject ores;
            QJsonObject oroot = root;
            ores["success"] = "true";
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        },
        cbProgress);
    }
    else if (root["msg"] == "import_csv")
    {
        mpdevice->importFromCSV(
                    root["data"].toArray(),
                    cbProgress,
                    [=](bool success, QString errstr)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject ores;
            QJsonObject oroot = root;
            ores["success"] = "true";
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        });
    }
    else if (root["msg"] == "refresh_files_cache")
    {
        mpdevice->updateFilesCache();
    }
    else if (root["msg"] == "list_files_cache")
    {
        sendFilesCache();
    }
    else if (root["msg"] == "reset_card")
    {
        mpdevice->resetSmartCard([=](bool success, QString errstr)
        {
            if (!WSServer::Instance()->checkClientExists(this))
                return;

            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject ores;
            QJsonObject oroot = root;
            ores["success"] = "true";
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        }
        );
    }
    else if (root["msg"] == "lock_device")
    {
        mpdevice->lockDevice([this, root](bool success, QString errstr)
        {
            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            QJsonObject ores;
            QJsonObject oroot = root;
            ores["success"] = "true";
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        });
    }
}

void WSServerCon::processMessageBLE(QJsonObject root, const MPDeviceProgressCb &cbProgress)
{
    //Ble related commands
    MPDeviceBleImpl *bleImpl = mpdevice->ble();
    if (nullptr == bleImpl)
    {
        return;
    }

    if (root["msg"] == "get_debug_platinfo")
    {
        bleImpl->getDebugPlatInfo([this, root, bleImpl](bool success, QString errstr, QByteArray data)
        {
            if (!success)
            {
                sendFailedJson(root, errstr);
                return;
            }

            auto platInfo = bleImpl->calcDebugPlatInfo(data);
            QJsonObject ores;
            QJsonObject oroot = root;
            ores["aux_major"] = platInfo[0];
            ores["aux_minor"] = platInfo[1];
            ores["main_major"] = platInfo[2];
            ores["main_minor"] = platInfo[3];
            ores["success"] = "true";
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        });
    }
    else if (root["msg"] == "flash_mcu")
    {
        QJsonObject o = root["data"].toObject();
        bleImpl->flashMCU(o["type"].toString(), [this, root](bool success, QString errstr)
        {
            if (!success)
            {
                qCritical() << errstr;
                sendFailedJson(root, errstr);
                return;
            }
        });
    }
    else if (root["msg"] == "upload_bundle")
    {
        QJsonObject o = root["data"].toObject();
        bleImpl->uploadBundle(o["file"].toString(), [this, root](bool success, QString errstr)
        {
            QJsonObject ores;
            QJsonObject oroot = root;
            ores["success"] = success;
            if (!success)
            {
                qCritical() << errstr;
            }
            oroot["data"] = ores;
            sendJsonMessage(oroot);
        }, cbProgress);
    }
    else if (root["msg"] == "fetch_data")
    {
        QJsonObject o = root["data"].toObject();
        auto type = static_cast<Common::FetchType>(o["type"].toInt());
        const auto cmd = Common::FetchType::ACCELEROMETER == type ?
                    MPCmd::CMD_DBG_GET_ACC_32_SAMPLES : MPCmd::GET_RANDOM_NUMBER;
        bleImpl->fetchData(o["file"].toString(), cmd);
    }
    else if (root["msg"] == "stop_fetch_data")
    {
        bleImpl->stopFetchData();
    }
    else if (root["msg"] == "ask_password" ||
             root["msg"] == "get_credential")
    {
        QJsonObject o = root["data"].toObject();
        QString service = o["service"].toString();
        QString login = o["login"].toString();
        bleImpl->getCredential(service, login,
                [this, root, bleImpl, service, login](bool success, QString errstr, QByteArray data)
                {
                    if (!success)
                    {
                        sendFailedJson(root, errstr);
                        return;
                    }

                    auto cred = bleImpl->retrieveCredentialFromResponse(data, service, login);

                    QSettings s;
                    if (s.value("settings/enable_hibp_check").toBool())
                    {
                        QString formatString = service + ": " + login + ": ";
                        formatString += HIBP_COMPROMISED_FORMAT;
                        hibp->isPasswordPwned(cred.get(BleCredential::CredAttr::PASSWORD), formatString);
                    }
                    QJsonObject ores;
                    QJsonObject oroot = root;
                    ores["service"] = service;
                    ores["login"] = cred.get(BleCredential::CredAttr::LOGIN);
                    ores["desc"] = cred.get(BleCredential::CredAttr::DESCRIPTION);
                    ores["third"] = cred.get(BleCredential::CredAttr::THIRD);
                    ores["password"] = cred.get(BleCredential::CredAttr::PASSWORD);
                    oroot["data"] = ores;
                    sendJsonMessage(oroot);
                });
    }
    else if (root["msg"] == "set_credential")
    {
        QJsonObject o = root["data"].toObject();
        QString loginName = o["login"].toString();
        QString originalService = o["service"].toString();
        ParseDomain url(originalService);
        QSettings s;
        bool isManualCredential = o.contains("saveManualCredential");
        if (isManualCredential)
        {
            o["service"] = url.getManuallyEnteredDomainName(originalService);
        }
        else
        {
            o["service"] = url.getFullSubdomain();
        }

        const QJsonDocument credDetectedDoc(QJsonObject{{ "msg", "credential_detected" }});
        bool isGuiRunning;
        emit sendMessageToGUI(credDetectedDoc.toJson(QJsonDocument::JsonFormat::Compact), isGuiRunning);

        if (s.value("settings/enable_hibp_check").toBool())
        {
            QString formatString = o["service"].toString() + ": " + loginName + ": ";
            formatString += HIBP_COMPROMISED_FORMAT;
            hibp->isPasswordPwned(o["password"].toString(), formatString);
        }

        bleImpl->storeCredential(BleCredential{o["service"].toString(), o["login"].toString(),
                                               o["description"].toString(), "", o["password"].toString()},
                                 [=](bool success, QString errstr)
                                 {
                                     if (!WSServer::Instance()->checkClientExists(this))
                                         return;

                                     if (!success)
                                     {
                                         sendFailedJson(root, errstr);
                                         return;
                                     }

                                     QJsonObject ores = o;
                                     QJsonObject oroot = root;
                                     oroot["data"] = ores;
                                     sendJsonMessage(oroot);
                                 });
    }
    else
    {
        qDebug() << root["msg"] << " message have not implemented yet for BLE";
    }
}

bool WSServerCon::checkMemModeEnabled(const QJsonObject &root)
{
    if (WSServer::Instance()->isMemModeLocked(clientUid))
    {
        sendFailedJson(root, "Device is in memory management mode");
        return true;
    }

    return false;
}
