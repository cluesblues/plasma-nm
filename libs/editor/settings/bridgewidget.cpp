/*
    Copyright 2013 Lukas Tinkl <ltinkl@redhat.com>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) version 3, or any
    later version accepted by the membership of KDE e.V. (or its
    successor approved by the membership of KDE e.V.), which shall
    act as a proxy defined in Section 6 of version 3 of the license.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "bridgewidget.h"
#include "ui_bridge.h"
#include "connectioneditordialog.h"
#include "debug.h"

#include <QDBusPendingReply>

#include <NetworkManagerQt/GenericTypes>
#include <NetworkManagerQt/Connection>
#include <NetworkManagerQt/Settings>
#include <NetworkManagerQt/ConnectionSettings>

#include <KLocalizedString>
#include <KMessageBox>

BridgeWidget::BridgeWidget(const QString & masterUuid, const QString &masterId,
                           const NetworkManager::Setting::Ptr &setting,
                           QWidget* parent, Qt::WindowFlags f):
    SettingWidget(setting, parent, f),
    m_uuid(masterUuid),
    m_id(masterId),
    m_ui(new Ui::BridgeWidget)
{
    m_ui->setupUi(this);

    // Action buttons and menu
    m_menu = new QMenu(this);
    QAction * action = new QAction(i18n("Ethernet"), this);
    action->setData(NetworkManager::ConnectionSettings::Wired);
    m_menu->addAction(action);
    action = new QAction(i18n("VLAN"), this);
    action->setData(NetworkManager::ConnectionSettings::Vlan);
    m_menu->addAction(action);
    action = new QAction(i18n("Wi-Fi"), this);
    action->setData(NetworkManager::ConnectionSettings::Wireless);
    m_menu->addAction(action);
    m_ui->btnAdd->setMenu(m_menu);
    connect(m_menu, &QMenu::triggered, this, &BridgeWidget::addBridge);
    connect(m_ui->btnEdit, &QPushButton::clicked, this, &BridgeWidget::editBridge);
    connect(m_ui->btnDelete, &QPushButton::clicked, this, &BridgeWidget::deleteBridge);

    // bridges
    populateBridges();
    connect(m_ui->bridges, &QListWidget::currentItemChanged, this, &BridgeWidget::currentBridgeChanged);
    connect(m_ui->bridges, &QListWidget::itemDoubleClicked, this, &BridgeWidget::editBridge);

    connect(m_ui->ifaceName, &KLineEdit::textChanged, this, &BridgeWidget::slotWidgetChanged);

    // Connect for setting check
    watchChangedSetting();

    KAcceleratorManager::manage(this);
    KAcceleratorManager::manage(m_menu);

    if (setting) {
        loadConfig(setting);
    }
}

BridgeWidget::~BridgeWidget()
{
    delete m_ui;
}

void BridgeWidget::loadConfig(const NetworkManager::Setting::Ptr &setting)
{
    NetworkManager::BridgeSetting::Ptr bridgeSetting = setting.staticCast<NetworkManager::BridgeSetting>();

    m_ui->ifaceName->setText(bridgeSetting->interfaceName());
    m_ui->agingTime->setValue(bridgeSetting->agingTime());

    const bool stp = bridgeSetting->stp();
    m_ui->stpGroup->setChecked(stp);
    if (stp) {
        m_ui->priority->setValue(bridgeSetting->priority());
        m_ui->forwardDelay->setValue(bridgeSetting->forwardDelay());
        m_ui->helloTime->setValue(bridgeSetting->helloTime());
        m_ui->maxAge->setValue(bridgeSetting->maxAge());
    }
}

QVariantMap BridgeWidget::setting() const
{
    NetworkManager::BridgeSetting setting;
    setting.setInterfaceName(m_ui->ifaceName->text());
    setting.setAgingTime(m_ui->agingTime->value());

    const bool stp = m_ui->stpGroup->isChecked();
    setting.setStp(stp);
    if (stp) {
        setting.setPriority(m_ui->priority->value());
        setting.setForwardDelay(m_ui->forwardDelay->value());
        setting.setHelloTime(m_ui->helloTime->value());
        setting.setMaxAge(m_ui->maxAge->value());
    }

    return setting.toMap();
}

void BridgeWidget::addBridge(QAction *action)
{
    qCDebug(PLASMA_NM) << "Adding bridged connection:" << action->data();
    qCDebug(PLASMA_NM) << "Master UUID:" << m_uuid;
    qCDebug(PLASMA_NM) << "Slave type:" << type();

    NetworkManager::ConnectionSettings::ConnectionType connectionType = static_cast<NetworkManager::ConnectionSettings::ConnectionType>(action->data().toInt());
    NetworkManager::ConnectionSettings::Ptr connectionSettings = NetworkManager::ConnectionSettings::Ptr(new NetworkManager::ConnectionSettings(connectionType));
    connectionSettings->setUuid(NetworkManager::ConnectionSettings::createNewUuid());
    connectionSettings->setMaster(m_uuid);
    connectionSettings->setSlaveType(type());
    connectionSettings->setAutoconnect(false);

    QPointer<ConnectionEditorDialog> bridgeEditor = new ConnectionEditorDialog(connectionSettings);
    connect(bridgeEditor.data(), &ConnectionEditorDialog::accepted,
            [bridgeEditor, this] () {
                qCDebug(PLASMA_NM) << "Saving slave connection";
                // qCDebug(PLASMA_NM) << bridgeEditor->setting();
                QDBusPendingReply<QDBusObjectPath> reply = NetworkManager::addConnection(bridgeEditor->setting());
                QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(reply, this);
                connect(watcher, &QDBusPendingCallWatcher::finished, this, &BridgeWidget::bridgeAddComplete);
            });
    connect(bridgeEditor.data(), &ConnectionEditorDialog::finished,
            [bridgeEditor] () {
                if (bridgeEditor) {
                    bridgeEditor->deleteLater();
                }
            });
    bridgeEditor->setModal(true);
    bridgeEditor->show();
}

void BridgeWidget::currentBridgeChanged(QListWidgetItem *current, QListWidgetItem *previous)
{
    Q_UNUSED(previous)

    m_ui->btnEdit->setEnabled(current);
    m_ui->btnDelete->setEnabled(current);
}

void BridgeWidget::bridgeAddComplete(QDBusPendingCallWatcher * watcher)
{
    QDBusPendingReply<QDBusObjectPath> reply = *watcher;

    if (reply.isValid()) {
        // find the slave connection with matching UUID
        NetworkManager::Connection::Ptr connection = NetworkManager::findConnection(reply.value().path());
        if (connection && connection->settings()->master() == m_uuid) {
            const QString label = QString("%1 (%2)").arg(connection->name()).arg(connection->settings()->typeAsString(connection->settings()->connectionType()));
            QListWidgetItem * slaveItem = new QListWidgetItem(label, m_ui->bridges);
            slaveItem->setData(Qt::UserRole, connection->uuid());
            slotWidgetChanged();
        }
    } else {
        qCWarning(PLASMA_NM) << "Bridged connection not added:" << reply.error().message();
    }
}

void BridgeWidget::editBridge()
{
    QListWidgetItem * currentItem = m_ui->bridges->currentItem();
    if (!currentItem)
        return;

    const QString uuid = currentItem->data(Qt::UserRole).toString();
    NetworkManager::Connection::Ptr connection = NetworkManager::findConnectionByUuid(uuid);

    if (connection) {
        qCDebug(PLASMA_NM) << "Editing bridged connection" << currentItem->text() << uuid;
        QPointer<ConnectionEditorDialog> bridgeEditor = new ConnectionEditorDialog(connection->settings());
        connect(bridgeEditor.data(), &ConnectionEditorDialog::accepted,
                [connection, bridgeEditor, this] () {
                    connection->update(bridgeEditor->setting());
                    connect(connection.data(), &NetworkManager::Connection::updated, this, &BridgeWidget::populateBridges);
                });
        connect(bridgeEditor.data(), &ConnectionEditorDialog::finished,
                [bridgeEditor] () {
                    if (bridgeEditor) {
                        bridgeEditor->deleteLater();
                    }
                });
        bridgeEditor->setModal(true);
        bridgeEditor->show();
    }
}

void BridgeWidget::deleteBridge()
{
    QListWidgetItem * currentItem = m_ui->bridges->currentItem();
    if (!currentItem)
        return;

    const QString uuid = currentItem->data(Qt::UserRole).toString();
    NetworkManager::Connection::Ptr connection = NetworkManager::findConnectionByUuid(uuid);

    if (connection) {
        // qCDebug(PLASMA_NM) << "About to delete bridged connection" << currentItem->text() << uuid;
        if (KMessageBox::questionYesNo(this, i18n("Do you want to remove the connection '%1'?", connection->name()), i18n("Remove Connection"), KStandardGuiItem::remove(),
                                       KStandardGuiItem::no(), QString(), KMessageBox::Dangerous)
                == KMessageBox::Yes) {
            connection->remove();
            delete currentItem;
            slotWidgetChanged();
        }
    }
}

void BridgeWidget::populateBridges()
{
    m_ui->bridges->clear();

    for (const NetworkManager::Connection::Ptr &connection : NetworkManager::listConnections()) {
        NetworkManager::ConnectionSettings::Ptr settings = connection->settings();
        // The mapping from slave to master may be by uuid or name, try our best to
        // figure out if we are master to the slave.
        const QString master = settings->master();
        bool isSlave = ((master == m_uuid) || // by-uuid
                        (!m_id.isEmpty() && master == m_id)); // by-name
        if (isSlave && (settings->slaveType() == type())) {
            const QString label = QString("%1 (%2)").arg(connection->name()).arg(connection->settings()->typeAsString(connection->settings()->connectionType()));
            QListWidgetItem * slaveItem = new QListWidgetItem(label, m_ui->bridges);
            slaveItem->setData(Qt::UserRole, connection->uuid());
        }
    }
}

bool BridgeWidget::isValid() const
{
    return !m_ui->ifaceName->text().isEmpty() && m_ui->bridges->count() > 0;
}
