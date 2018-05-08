#include "safenodelist.h"
#include "ui_safenodelist.h"

#include "activesafenode.h"
#include "clientmodel.h"
#include "init.h"
#include "guiutil.h"
#include "safenode-sync.h"
#include "safenodeconfig.h"
#include "safenodeman.h"
#include "sync.h"
#include "wallet/wallet.h"
#include "walletmodel.h"

#include <QTimer>
#include <QMessageBox>

SafenodeList::SafenodeList(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::SafenodeList),
    clientModel(0),
    walletModel(0)
{
    ui->setupUi(this);

    ui->startButton->setEnabled(false);

    int columnAliasWidth = 100;
    int columnAddressWidth = 200;
    int columnProtocolWidth = 60;
    int columnStatusWidth = 80;
    int columnActiveWidth = 130;
    int columnLastSeenWidth = 130;

    ui->tableWidgetMySafenodes->setColumnWidth(0, columnAliasWidth);
    ui->tableWidgetMySafenodes->setColumnWidth(1, columnAddressWidth);
    ui->tableWidgetMySafenodes->setColumnWidth(2, columnProtocolWidth);
    ui->tableWidgetMySafenodes->setColumnWidth(3, columnStatusWidth);
    ui->tableWidgetMySafenodes->setColumnWidth(4, columnActiveWidth);
    ui->tableWidgetMySafenodes->setColumnWidth(5, columnLastSeenWidth);

    ui->tableWidgetSafenodes->setColumnWidth(0, columnAddressWidth);
    ui->tableWidgetSafenodes->setColumnWidth(1, columnProtocolWidth);
    ui->tableWidgetSafenodes->setColumnWidth(2, columnStatusWidth);
    ui->tableWidgetSafenodes->setColumnWidth(3, columnActiveWidth);
    ui->tableWidgetSafenodes->setColumnWidth(4, columnLastSeenWidth);

    ui->tableWidgetMySafenodes->setContextMenuPolicy(Qt::CustomContextMenu);

    QAction *startAliasAction = new QAction(tr("Start alias"), this);
    contextMenu = new QMenu();
    contextMenu->addAction(startAliasAction);
    connect(ui->tableWidgetMySafenodes, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(showContextMenu(const QPoint&)));
    connect(startAliasAction, SIGNAL(triggered()), this, SLOT(on_startButton_clicked()));

    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(updateNodeList()));
    connect(timer, SIGNAL(timeout()), this, SLOT(updateMyNodeList()));
    timer->start(1000);

    fFilterUpdated = false;
    nTimeFilterUpdated = GetTime();
    updateNodeList();
}

SafenodeList::~SafenodeList()
{
    delete ui;
}

void SafenodeList::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if(model) {
        // try to update list when safenode count changes
        connect(clientModel, SIGNAL(strSafenodesChanged(QString)), this, SLOT(updateNodeList()));
    }
}

void SafenodeList::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
}

void SafenodeList::showContextMenu(const QPoint &point)
{
    QTableWidgetItem *item = ui->tableWidgetMySafenodes->itemAt(point);
    if(item) contextMenu->exec(QCursor::pos());
}

void SafenodeList::StartAlias(std::string strAlias)
{
    std::string strStatusHtml;
    strStatusHtml += "<center>Alias: " + strAlias;

    BOOST_FOREACH(CSafenodeConfig::CSafenodeEntry mne, safenodeConfig.getEntries()) {
        if(mne.getAlias() == strAlias) {
            std::string strError;
            CSafenodeBroadcast mnb;

            bool fSuccess = CSafenodeBroadcast::Create(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strError, mnb);

            if(fSuccess) {
                strStatusHtml += "<br>Successfully started safenode.";
                mnodeman.UpdateSafenodeList(mnb);
                mnb.Relay();
                mnodeman.NotifySafenodeUpdates();
            } else {
                strStatusHtml += "<br>Failed to start safenode.<br>Error: " + strError;
            }
            break;
        }
    }
    strStatusHtml += "</center>";

    QMessageBox msg;
    msg.setText(QString::fromStdString(strStatusHtml));
    msg.exec();

    updateMyNodeList(true);
}

void SafenodeList::StartAll(std::string strCommand)
{
    int nCountSuccessful = 0;
    int nCountFailed = 0;
    std::string strFailedHtml;

    BOOST_FOREACH(CSafenodeConfig::CSafenodeEntry mne, safenodeConfig.getEntries()) {
        std::string strError;
        CSafenodeBroadcast mnb;

        int32_t nOutputIndex = 0;
        if(!ParseInt32(mne.getOutputIndex(), &nOutputIndex)) {
            continue;
        }

        CTxIn txin = CTxIn(uint256S(mne.getTxHash()), nOutputIndex);

        if(strCommand == "start-missing" && mnodeman.Has(txin)) continue;

        bool fSuccess = CSafenodeBroadcast::Create(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strError, mnb);

        if(fSuccess) {
            nCountSuccessful++;
            mnodeman.UpdateSafenodeList(mnb);
            mnb.Relay();
            mnodeman.NotifySafenodeUpdates();
        } else {
            nCountFailed++;
            strFailedHtml += "\nFailed to start " + mne.getAlias() + ". Error: " + strError;
        }
    }
    pwalletMain->Lock();

    std::string returnObj;
    returnObj = strprintf("Successfully started %d safenodes, failed to start %d, total %d", nCountSuccessful, nCountFailed, nCountFailed + nCountSuccessful);
    if (nCountFailed > 0) {
        returnObj += strFailedHtml;
    }

    QMessageBox msg;
    msg.setText(QString::fromStdString(returnObj));
    msg.exec();

    updateMyNodeList(true);
}

void SafenodeList::updateMySafenodeInfo(QString strAlias, QString strAddr, safenode_info_t& infoMn)
{
    bool fOldRowFound = false;
    int nNewRow = 0;

    for(int i = 0; i < ui->tableWidgetMySafenodes->rowCount(); i++) {
        if(ui->tableWidgetMySafenodes->item(i, 0)->text() == strAlias) {
            fOldRowFound = true;
            nNewRow = i;
            break;
        }
    }

    if(nNewRow == 0 && !fOldRowFound) {
        nNewRow = ui->tableWidgetMySafenodes->rowCount();
        ui->tableWidgetMySafenodes->insertRow(nNewRow);
    }

    QTableWidgetItem *aliasItem = new QTableWidgetItem(strAlias);
    QTableWidgetItem *addrItem = new QTableWidgetItem(infoMn.fInfoValid ? QString::fromStdString(infoMn.addr.ToString()) : strAddr);
    QTableWidgetItem *protocolItem = new QTableWidgetItem(QString::number(infoMn.fInfoValid ? infoMn.nProtocolVersion : -1));
    QTableWidgetItem *statusItem = new QTableWidgetItem(QString::fromStdString(infoMn.fInfoValid ? CSafenode::StateToString(infoMn.nActiveState) : "MISSING"));
    QTableWidgetItem *activeSecondsItem = new QTableWidgetItem(QString::fromStdString(DurationToDHMS(infoMn.fInfoValid ? (infoMn.nTimeLastPing - infoMn.sigTime) : 0)));
    QTableWidgetItem *lastSeenItem = new QTableWidgetItem(QString::fromStdString(DateTimeStrFormat("%Y-%m-%d %H:%M",
                                                                                                   infoMn.fInfoValid ? infoMn.nTimeLastPing + QDateTime::currentDateTime().offsetFromUtc() : 0)));
    QTableWidgetItem *pubkeyItem = new QTableWidgetItem(QString::fromStdString(infoMn.fInfoValid ? CBitcoinAddress(infoMn.pubKeyCollateralAddress.GetID()).ToString() : ""));

    ui->tableWidgetMySafenodes->setItem(nNewRow, 0, aliasItem);
    ui->tableWidgetMySafenodes->setItem(nNewRow, 1, addrItem);
    ui->tableWidgetMySafenodes->setItem(nNewRow, 2, protocolItem);
    ui->tableWidgetMySafenodes->setItem(nNewRow, 3, statusItem);
    ui->tableWidgetMySafenodes->setItem(nNewRow, 4, activeSecondsItem);
    ui->tableWidgetMySafenodes->setItem(nNewRow, 5, lastSeenItem);
    ui->tableWidgetMySafenodes->setItem(nNewRow, 6, pubkeyItem);
}

void SafenodeList::updateMyNodeList(bool fForce)
{
    TRY_LOCK(cs_mymnlist, fLockAcquired);
    if(!fLockAcquired) {
        return;
    }
    static int64_t nTimeMyListUpdated = 0;

    // automatically update my safenode list only once in MY_SAFENODELIST_UPDATE_SECONDS seconds,
    // this update still can be triggered manually at any time via button click
    int64_t nSecondsTillUpdate = nTimeMyListUpdated + MY_SAFENODELIST_UPDATE_SECONDS - GetTime();
    ui->secondsLabel->setText(QString::number(nSecondsTillUpdate));

    if(nSecondsTillUpdate > 0 && !fForce) return;
    nTimeMyListUpdated = GetTime();

    ui->tableWidgetSafenodes->setSortingEnabled(false);
    BOOST_FOREACH(CSafenodeConfig::CSafenodeEntry mne, safenodeConfig.getEntries()) {
        int32_t nOutputIndex = 0;
        if(!ParseInt32(mne.getOutputIndex(), &nOutputIndex)) {
            continue;
        }

        CTxIn txin = CTxIn(uint256S(mne.getTxHash()), nOutputIndex);

        safenode_info_t infoMn = mnodeman.GetSafenodeInfo(txin);

        updateMySafenodeInfo(QString::fromStdString(mne.getAlias()), QString::fromStdString(mne.getIp()), infoMn);
    }
    ui->tableWidgetSafenodes->setSortingEnabled(true);

    // reset "timer"
    ui->secondsLabel->setText("0");
}

void SafenodeList::updateNodeList()
{
    TRY_LOCK(cs_mnlist, fLockAcquired);
    if(!fLockAcquired) {
        return;
    }

    static int64_t nTimeListUpdated = GetTime();

    // to prevent high cpu usage update only once in SAFENODELIST_UPDATE_SECONDS seconds
    // or SAFENODELIST_FILTER_COOLDOWN_SECONDS seconds after filter was last changed
    int64_t nSecondsToWait = fFilterUpdated
                            ? nTimeFilterUpdated - GetTime() + SAFENODELIST_FILTER_COOLDOWN_SECONDS
                            : nTimeListUpdated - GetTime() + SAFENODELIST_UPDATE_SECONDS;

    if(fFilterUpdated) ui->countLabel->setText(QString::fromStdString(strprintf("Please wait... %d", nSecondsToWait)));
    if(nSecondsToWait > 0) return;

    nTimeListUpdated = GetTime();
    fFilterUpdated = false;

    QString strToFilter;
    ui->countLabel->setText("Updating...");
    ui->tableWidgetSafenodes->setSortingEnabled(false);
    ui->tableWidgetSafenodes->clearContents();
    ui->tableWidgetSafenodes->setRowCount(0);
    std::vector<CSafenode> vSafenodes = mnodeman.GetFullSafenodeVector();

    BOOST_FOREACH(CSafenode& mn, vSafenodes)
    {
        // populate list
        // Address, Protocol, Status, Active Seconds, Last Seen, Pub Key
        QTableWidgetItem *addressItem = new QTableWidgetItem(QString::fromStdString(mn.addr.ToString()));
        QTableWidgetItem *protocolItem = new QTableWidgetItem(QString::number(mn.nProtocolVersion));
        QTableWidgetItem *statusItem = new QTableWidgetItem(QString::fromStdString(mn.GetStatus()));
        QTableWidgetItem *activeSecondsItem = new QTableWidgetItem(QString::fromStdString(DurationToDHMS(mn.lastPing.sigTime - mn.sigTime)));
        QTableWidgetItem *lastSeenItem = new QTableWidgetItem(QString::fromStdString(DateTimeStrFormat("%Y-%m-%d %H:%M", mn.lastPing.sigTime + QDateTime::currentDateTime().offsetFromUtc())));
        QTableWidgetItem *pubkeyItem = new QTableWidgetItem(QString::fromStdString(CBitcoinAddress(mn.pubKeyCollateralAddress.GetID()).ToString()));

        if (strCurrentFilter != "")
        {
            strToFilter =   addressItem->text() + " " +
                            protocolItem->text() + " " +
                            statusItem->text() + " " +
                            activeSecondsItem->text() + " " +
                            lastSeenItem->text() + " " +
                            pubkeyItem->text();
            if (!strToFilter.contains(strCurrentFilter)) continue;
        }

        ui->tableWidgetSafenodes->insertRow(0);
        ui->tableWidgetSafenodes->setItem(0, 0, addressItem);
        ui->tableWidgetSafenodes->setItem(0, 1, protocolItem);
        ui->tableWidgetSafenodes->setItem(0, 2, statusItem);
        ui->tableWidgetSafenodes->setItem(0, 3, activeSecondsItem);
        ui->tableWidgetSafenodes->setItem(0, 4, lastSeenItem);
        ui->tableWidgetSafenodes->setItem(0, 5, pubkeyItem);
    }

    ui->countLabel->setText(QString::number(ui->tableWidgetSafenodes->rowCount()));
    ui->tableWidgetSafenodes->setSortingEnabled(true);
}

void SafenodeList::on_filterLineEdit_textChanged(const QString &strFilterIn)
{
    strCurrentFilter = strFilterIn;
    nTimeFilterUpdated = GetTime();
    fFilterUpdated = true;
    ui->countLabel->setText(QString::fromStdString(strprintf("Please wait... %d", SAFENODELIST_FILTER_COOLDOWN_SECONDS)));
}

void SafenodeList::on_startButton_clicked()
{
    std::string strAlias;
    {
        LOCK(cs_mymnlist);
        // Find selected node alias
        QItemSelectionModel* selectionModel = ui->tableWidgetMySafenodes->selectionModel();
        QModelIndexList selected = selectionModel->selectedRows();

        if(selected.count() == 0) return;

        QModelIndex index = selected.at(0);
        int nSelectedRow = index.row();
        strAlias = ui->tableWidgetMySafenodes->item(nSelectedRow, 0)->text().toStdString();
    }

    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this, tr("Confirm safenode start"),
        tr("Are you sure you want to start safenode %1?").arg(QString::fromStdString(strAlias)),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if(retval != QMessageBox::Yes) return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    if(encStatus == walletModel->Locked || encStatus == walletModel->UnlockedForMixingOnly) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());

        if(!ctx.isValid()) return; // Unlock wallet was cancelled

        StartAlias(strAlias);
        return;
    }

    StartAlias(strAlias);
}

void SafenodeList::on_startAllButton_clicked()
{
    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this, tr("Confirm all safenodes start"),
        tr("Are you sure you want to start ALL safenodes?"),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if(retval != QMessageBox::Yes) return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    if(encStatus == walletModel->Locked || encStatus == walletModel->UnlockedForMixingOnly) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());

        if(!ctx.isValid()) return; // Unlock wallet was cancelled

        StartAll();
        return;
    }

    StartAll();
}

void SafenodeList::on_startMissingButton_clicked()
{

    if(!safenodeSync.IsSafenodeListSynced()) {
        QMessageBox::critical(this, tr("Command is not available right now"),
            tr("You can't use this command until safenode list is synced"));
        return;
    }

    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(this,
        tr("Confirm missing safenodes start"),
        tr("Are you sure you want to start MISSING safenodes?"),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if(retval != QMessageBox::Yes) return;

    WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();

    if(encStatus == walletModel->Locked || encStatus == walletModel->UnlockedForMixingOnly) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());

        if(!ctx.isValid()) return; // Unlock wallet was cancelled

        StartAll("start-missing");
        return;
    }

    StartAll("start-missing");
}

void SafenodeList::on_tableWidgetMySafenodes_itemSelectionChanged()
{
    if(ui->tableWidgetMySafenodes->selectedItems().count() > 0) {
        ui->startButton->setEnabled(true);
    }
}

void SafenodeList::on_UpdateButton_clicked()
{
    updateMyNodeList(true);
}
