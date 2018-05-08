#ifndef SAFENODELIST_H
#define SAFENODELIST_H

#include "safenode.h"
#include "platformstyle.h"
#include "sync.h"
#include "util.h"

#include <QMenu>
#include <QTimer>
#include <QWidget>

#define MY_SAFENODELIST_UPDATE_SECONDS                 60
#define SAFENODELIST_UPDATE_SECONDS                    15
#define SAFENODELIST_FILTER_COOLDOWN_SECONDS            3

namespace Ui {
    class SafenodeList;
}

class ClientModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

/** Safenode Manager page widget */
class SafenodeList : public QWidget
{
    Q_OBJECT

public:
    explicit SafenodeList(const PlatformStyle *platformStyle, QWidget *parent = 0);
    ~SafenodeList();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);
    void StartAlias(std::string strAlias);
    void StartAll(std::string strCommand = "start-all");

private:
    QMenu *contextMenu;
    int64_t nTimeFilterUpdated;
    bool fFilterUpdated;

public Q_SLOTS:
    void updateMySafenodeInfo(QString strAlias, QString strAddr, safenode_info_t& infoMn);
    void updateMyNodeList(bool fForce = false);
    void updateNodeList();

Q_SIGNALS:

private:
    QTimer *timer;
    Ui::SafenodeList *ui;
    ClientModel *clientModel;
    WalletModel *walletModel;

    // Protects tableWidgetSafenodes
    CCriticalSection cs_mnlist;

    // Protects tableWidgetMySafenodes
    CCriticalSection cs_mymnlist;

    QString strCurrentFilter;

private Q_SLOTS:
    void showContextMenu(const QPoint &);
    void on_filterLineEdit_textChanged(const QString &strFilterIn);
    void on_startButton_clicked();
    void on_startAllButton_clicked();
    void on_startMissingButton_clicked();
    void on_tableWidgetMySafenodes_itemSelectionChanged();
    void on_UpdateButton_clicked();
};
#endif // SAFENODELIST_H
