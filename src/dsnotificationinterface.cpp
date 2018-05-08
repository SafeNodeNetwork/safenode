// Copyright (c) 2014-2018 The SafeNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "dsnotificationinterface.h"
#include "darksend.h"
#include "instantx.h"
#include "governance.h"
#include "safenodeman.h"
#include "safenode-payments.h"
#include "safenode-sync.h"

CDSNotificationInterface::CDSNotificationInterface()
{
}

CDSNotificationInterface::~CDSNotificationInterface()
{
}

void CDSNotificationInterface::UpdatedBlockTip(const CBlockIndex *pindex)
{
    mnodeman.UpdatedBlockTip(pindex);
    darkSendPool.UpdatedBlockTip(pindex);
    instantsend.UpdatedBlockTip(pindex);
    mnpayments.UpdatedBlockTip(pindex);
    governance.UpdatedBlockTip(pindex);
    safenodeSync.UpdatedBlockTip(pindex);
}

void CDSNotificationInterface::SyncTransaction(const CTransaction &tx, const CBlock *pblock)
{
    instantsend.SyncTransaction(tx, pblock);
}