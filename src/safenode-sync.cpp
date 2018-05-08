// Copyright (c) 2014-2018 The SafeNode developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activesafenode.h"
#include "checkpoints.h"
#include "governance.h"
#include "main.h"
#include "safenode.h"
#include "safenode-payments.h"
#include "safenode-sync.h"
#include "safenodeman.h"
#include "netfulfilledman.h"
#include "spork.h"
#include "util.h"

class CSafenodeSync;
CSafenodeSync safenodeSync;

bool CSafenodeSync::CheckNodeHeight(CNode* pnode, bool fDisconnectStuckNodes)
{
    CNodeStateStats stats;
    if(!GetNodeStateStats(pnode->id, stats) || stats.nCommonHeight == -1 || stats.nSyncHeight == -1) return false; // not enough info about this peer

    // Check blocks and headers, allow a small error margin of 1 block
    if(pCurrentBlockIndex->nHeight - 1 > stats.nCommonHeight) {
        // This peer probably stuck, don't sync any additional data from it
        if(fDisconnectStuckNodes) {
            // Disconnect to free this connection slot for another peer.
            pnode->fDisconnect = true;
            LogPrintf("CSafenodeSync::CheckNodeHeight -- disconnecting from stuck peer, nHeight=%d, nCommonHeight=%d, peer=%d\n",
                        pCurrentBlockIndex->nHeight, stats.nCommonHeight, pnode->id);
        } else {
            LogPrintf("CSafenodeSync::CheckNodeHeight -- skipping stuck peer, nHeight=%d, nCommonHeight=%d, peer=%d\n",
                        pCurrentBlockIndex->nHeight, stats.nCommonHeight, pnode->id);
        }
        return false;
    }
    else if(pCurrentBlockIndex->nHeight < stats.nSyncHeight - 1) {
        // This peer announced more headers than we have blocks currently
        LogPrintf("CSafenodeSync::CheckNodeHeight -- skipping peer, who announced more headers than we have blocks currently, nHeight=%d, nSyncHeight=%d, peer=%d\n",
                    pCurrentBlockIndex->nHeight, stats.nSyncHeight, pnode->id);
        return false;
    }

    return true;
}

bool CSafenodeSync::IsBlockchainSynced(bool fBlockAccepted)
{
    static bool fBlockchainSynced = false;
    static int64_t nTimeLastProcess = GetTime();
    static int nSkipped = 0;
    static bool fFirstBlockAccepted = false;

    // if the last call to this function was more than 60 minutes ago (client was in sleep mode) reset the sync process
    if(GetTime() - nTimeLastProcess > 60*60) {
        Reset();
        fBlockchainSynced = false;
    }

    if(!pCurrentBlockIndex || !pindexBestHeader || fImporting || fReindex) return false;

    if(fBlockAccepted) {
        // this should be only triggered while we are still syncing
        if(!IsSynced()) {
            // we are trying to download smth, reset blockchain sync status
            if(fDebug) LogPrintf("CSafenodeSync::IsBlockchainSynced -- reset\n");
            fFirstBlockAccepted = true;
            fBlockchainSynced = false;
            nTimeLastProcess = GetTime();
            return false;
        }
    } else {
        // skip if we already checked less than 1 tick ago
        if(GetTime() - nTimeLastProcess < SAFENODE_SYNC_TICK_SECONDS) {
            nSkipped++;
            return fBlockchainSynced;
        }
    }

    if(fDebug) LogPrintf("CSafenodeSync::IsBlockchainSynced -- state before check: %ssynced, skipped %d times\n", fBlockchainSynced ? "" : "not ", nSkipped);

    nTimeLastProcess = GetTime();
    nSkipped = 0;

    if(fBlockchainSynced) return true;

    if(fCheckpointsEnabled && pCurrentBlockIndex->nHeight < Checkpoints::GetTotalBlocksEstimate(Params().Checkpoints()))
        return false;

    std::vector<CNode*> vNodesCopy = CopyNodeVector();

    // We have enough peers and assume most of them are synced
    if(vNodesCopy.size() >= SAFENODE_SYNC_ENOUGH_PEERS) {
        // Check to see how many of our peers are (almost) at the same height as we are
        int nNodesAtSameHeight = 0;
        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            // Make sure this peer is presumably at the same height
            if(!CheckNodeHeight(pnode)) continue;
            nNodesAtSameHeight++;
            // if we have decent number of such peers, most likely we are synced now
            if(nNodesAtSameHeight >= SAFENODE_SYNC_ENOUGH_PEERS) {
                LogPrintf("CSafenodeSync::IsBlockchainSynced -- found enough peers on the same height as we are, done\n");
                fBlockchainSynced = true;
                ReleaseNodeVector(vNodesCopy);
                return true;
            }
        }
    }
    ReleaseNodeVector(vNodesCopy);

    // wait for at least one new block to be accepted
    if(!fFirstBlockAccepted) return false;

    // same as !IsInitialBlockDownload() but no cs_main needed here
    int64_t nMaxBlockTime = std::max(pCurrentBlockIndex->GetBlockTime(), pindexBestHeader->GetBlockTime());
    fBlockchainSynced = pindexBestHeader->nHeight - pCurrentBlockIndex->nHeight < 24 * 6 &&
                        GetTime() - nMaxBlockTime < Params().MaxTipAge();

    return fBlockchainSynced;
}

void CSafenodeSync::Fail()
{
    nTimeLastFailure = GetTime();
    nRequestedSafenodeAssets = SAFENODE_SYNC_FAILED;
}

void CSafenodeSync::Reset()
{
    nRequestedSafenodeAssets = SAFENODE_SYNC_INITIAL;
    nRequestedSafenodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
    nTimeLastSafenodeList = GetTime();
    nTimeLastPaymentVote = GetTime();
    nTimeLastGovernanceItem = GetTime();
    nTimeLastFailure = 0;
    nCountFailures = 0;
}

std::string CSafenodeSync::GetAssetName()
{
    switch(nRequestedSafenodeAssets)
    {
        case(SAFENODE_SYNC_INITIAL):      return "SAFENODE_SYNC_INITIAL";
        case(SAFENODE_SYNC_SPORKS):       return "SAFENODE_SYNC_SPORKS";
        case(SAFENODE_SYNC_LIST):         return "SAFENODE_SYNC_LIST";
        case(SAFENODE_SYNC_MNW):          return "SAFENODE_SYNC_MNW";
        case(SAFENODE_SYNC_GOVERNANCE):   return "SAFENODE_SYNC_GOVERNANCE";
        case(SAFENODE_SYNC_FAILED):       return "SAFENODE_SYNC_FAILED";
        case SAFENODE_SYNC_FINISHED:      return "SAFENODE_SYNC_FINISHED";
        default:                            return "UNKNOWN";
    }
}

void CSafenodeSync::SwitchToNextAsset()
{
    switch(nRequestedSafenodeAssets)
    {
        case(SAFENODE_SYNC_FAILED):
            throw std::runtime_error("Can't switch to next asset from failed, should use Reset() first!");
            break;
        case(SAFENODE_SYNC_INITIAL):
            ClearFulfilledRequests();
            nRequestedSafenodeAssets = SAFENODE_SYNC_SPORKS;
            LogPrintf("CSafenodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case(SAFENODE_SYNC_SPORKS):
            nTimeLastSafenodeList = GetTime();
            nRequestedSafenodeAssets = SAFENODE_SYNC_LIST;
            LogPrintf("CSafenodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case(SAFENODE_SYNC_LIST):
            nTimeLastPaymentVote = GetTime();
            nRequestedSafenodeAssets = SAFENODE_SYNC_MNW;
            LogPrintf("CSafenodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case(SAFENODE_SYNC_MNW):
            nTimeLastGovernanceItem = GetTime();
            nRequestedSafenodeAssets = SAFENODE_SYNC_GOVERNANCE;
            LogPrintf("CSafenodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
            break;
        case(SAFENODE_SYNC_GOVERNANCE):
            LogPrintf("CSafenodeSync::SwitchToNextAsset -- Sync has finished\n");
            nRequestedSafenodeAssets = SAFENODE_SYNC_FINISHED;
            uiInterface.NotifyAdditionalDataSyncProgressChanged(1);
            //try to activate our safenode if possible
            activeSafenode.ManageState();

            TRY_LOCK(cs_vNodes, lockRecv);
            if(!lockRecv) return;

            BOOST_FOREACH(CNode* pnode, vNodes) {
                netfulfilledman.AddFulfilledRequest(pnode->addr, "full-sync");
            }

            break;
    }
    nRequestedSafenodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
}

std::string CSafenodeSync::GetSyncStatus()
{
    switch (safenodeSync.nRequestedSafenodeAssets) {
        case SAFENODE_SYNC_INITIAL:       return _("Synchronization pending...");
        case SAFENODE_SYNC_SPORKS:        return _("Synchronizing sporks...");
        case SAFENODE_SYNC_LIST:          return _("Synchronizing safenodes...");
        case SAFENODE_SYNC_MNW:           return _("Synchronizing safenode payments...");
        case SAFENODE_SYNC_GOVERNANCE:    return _("Synchronizing governance objects...");
        case SAFENODE_SYNC_FAILED:        return _("Synchronization failed");
        case SAFENODE_SYNC_FINISHED:      return _("Synchronization finished");
        default:                            return "";
    }
}

void CSafenodeSync::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (strCommand == NetMsgType::SYNCSTATUSCOUNT) { //Sync status count

        //do not care about stats if sync process finished or failed
        if(IsSynced() || IsFailed()) return;

        int nItemID;
        int nCount;
        vRecv >> nItemID >> nCount;

        LogPrintf("SYNCSTATUSCOUNT -- got inventory count: nItemID=%d  nCount=%d  peer=%d\n", nItemID, nCount, pfrom->id);
    }
}

void CSafenodeSync::ClearFulfilledRequests()
{
    TRY_LOCK(cs_vNodes, lockRecv);
    if(!lockRecv) return;

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "spork-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "safenode-list-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "safenode-payment-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "governance-sync");
        netfulfilledman.RemoveFulfilledRequest(pnode->addr, "full-sync");
    }
}

void CSafenodeSync::ProcessTick()
{
    static int nTick = 0;
    if(nTick++ % SAFENODE_SYNC_TICK_SECONDS != 0) return;
    if(!pCurrentBlockIndex) return;

    //the actual count of safenodes we have currently
    int nMnCount = mnodeman.CountSafenodes();

    if(fDebug) LogPrintf("CSafenodeSync::ProcessTick -- nTick %d nMnCount %d\n", nTick, nMnCount);

    // RESET SYNCING INCASE OF FAILURE
    {
        if(IsSynced()) {
            /*
                Resync if we lost all safenodes from sleep/wake or failed to sync originally
            */
            if(nMnCount == 0) {
                LogPrintf("CSafenodeSync::ProcessTick -- WARNING: not enough data, restarting sync\n");
                Reset();
            } else {
                std::vector<CNode*> vNodesCopy = CopyNodeVector();
                governance.RequestGovernanceObjectVotes(vNodesCopy);
                ReleaseNodeVector(vNodesCopy);
                return;
            }
        }

        //try syncing again
        if(IsFailed()) {
            if(nTimeLastFailure + (1*60) < GetTime()) { // 1 minute cooldown after failed sync
                Reset();
            }
            return;
        }
    }

    // INITIAL SYNC SETUP / LOG REPORTING
    double nSyncProgress = double(nRequestedSafenodeAttempt + (nRequestedSafenodeAssets - 1) * 8) / (8*4);
    LogPrintf("CSafenodeSync::ProcessTick -- nTick %d nRequestedSafenodeAssets %d nRequestedSafenodeAttempt %d nSyncProgress %f\n", nTick, nRequestedSafenodeAssets, nRequestedSafenodeAttempt, nSyncProgress);
    uiInterface.NotifyAdditionalDataSyncProgressChanged(nSyncProgress);

    // sporks synced but blockchain is not, wait until we're almost at a recent block to continue
    if(Params().NetworkIDString() != CBaseChainParams::REGTEST &&
            !IsBlockchainSynced() && nRequestedSafenodeAssets > SAFENODE_SYNC_SPORKS)
    {
        LogPrintf("CSafenodeSync::ProcessTick -- nTick %d nRequestedSafenodeAssets %d nRequestedSafenodeAttempt %d -- blockchain is not synced yet\n", nTick, nRequestedSafenodeAssets, nRequestedSafenodeAttempt);
        nTimeLastSafenodeList = GetTime();
        nTimeLastPaymentVote = GetTime();
        nTimeLastGovernanceItem = GetTime();
        return;
    }

    if(nRequestedSafenodeAssets == SAFENODE_SYNC_INITIAL ||
        (nRequestedSafenodeAssets == SAFENODE_SYNC_SPORKS && IsBlockchainSynced()))
    {
        SwitchToNextAsset();
    }

    std::vector<CNode*> vNodesCopy = CopyNodeVector();

    BOOST_FOREACH(CNode* pnode, vNodesCopy)
    {
        // Don't try to sync any data from outbound "safenode" connections -
        // they are temporary and should be considered unreliable for a sync process.
        // Inbound connection this early is most likely a "safenode" connection
        // initialted from another node, so skip it too.
        if(pnode->fSafenode || (fSafeNode && pnode->fInbound)) continue;

        // QUICK MODE (REGTEST ONLY!)
        if(Params().NetworkIDString() == CBaseChainParams::REGTEST)
        {
            if(nRequestedSafenodeAttempt <= 2) {
                pnode->PushMessage(NetMsgType::GETSPORKS); //get current network sporks
            } else if(nRequestedSafenodeAttempt < 4) {
                mnodeman.DsegUpdate(pnode);
            } else if(nRequestedSafenodeAttempt < 6) {
                int nMnCount = mnodeman.CountSafenodes();
                pnode->PushMessage(NetMsgType::SAFENODEPAYMENTSYNC, nMnCount); //sync payment votes
                SendGovernanceSyncRequest(pnode);
            } else {
                nRequestedSafenodeAssets = SAFENODE_SYNC_FINISHED;
            }
            nRequestedSafenodeAttempt++;
            ReleaseNodeVector(vNodesCopy);
            return;
        }

        // NORMAL NETWORK MODE - TESTNET/MAINNET
        {
            if(netfulfilledman.HasFulfilledRequest(pnode->addr, "full-sync")) {
                // We already fully synced from this node recently,
                // disconnect to free this connection slot for another peer.
                pnode->fDisconnect = true;
                LogPrintf("CSafenodeSync::ProcessTick -- disconnecting from recently synced peer %d\n", pnode->id);
                continue;
            }

            // SPORK : ALWAYS ASK FOR SPORKS AS WE SYNC (we skip this mode now)

            if(!netfulfilledman.HasFulfilledRequest(pnode->addr, "spork-sync")) {
                // only request once from each peer
                netfulfilledman.AddFulfilledRequest(pnode->addr, "spork-sync");
                // get current network sporks
                pnode->PushMessage(NetMsgType::GETSPORKS);
                LogPrintf("CSafenodeSync::ProcessTick -- nTick %d nRequestedSafenodeAssets %d -- requesting sporks from peer %d\n", nTick, nRequestedSafenodeAssets, pnode->id);
                continue; // always get sporks first, switch to the next node without waiting for the next tick
            }

            // MNLIST : SYNC SAFENODE LIST FROM OTHER CONNECTED CLIENTS

            if(nRequestedSafenodeAssets == SAFENODE_SYNC_LIST) {
                LogPrint("safenode", "CSafenodeSync::ProcessTick -- nTick %d nRequestedSafenodeAssets %d nTimeLastSafenodeList %lld GetTime() %lld diff %lld\n", nTick, nRequestedSafenodeAssets, nTimeLastSafenodeList, GetTime(), GetTime() - nTimeLastSafenodeList);
                // check for timeout first
                if(nTimeLastSafenodeList < GetTime() - SAFENODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrintf("CSafenodeSync::ProcessTick -- nTick %d nRequestedSafenodeAssets %d -- timeout\n", nTick, nRequestedSafenodeAssets);
                    if (nRequestedSafenodeAttempt == 0) {
                        LogPrintf("CSafenodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // there is no way we can continue without safenode list, fail here and try later
                        Fail();
                        ReleaseNodeVector(vNodesCopy);
                        return;
                    }
                    SwitchToNextAsset();
                    ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // only request once from each peer
                if(netfulfilledman.HasFulfilledRequest(pnode->addr, "safenode-list-sync")) continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "safenode-list-sync");

                if (pnode->nVersion < mnpayments.GetMinSafenodePaymentsProto()) continue;
                nRequestedSafenodeAttempt++;

                mnodeman.DsegUpdate(pnode);

                ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }

            // MNW : SYNC SAFENODE PAYMENT VOTES FROM OTHER CONNECTED CLIENTS

            if(nRequestedSafenodeAssets == SAFENODE_SYNC_MNW) {
                LogPrint("mnpayments", "CSafenodeSync::ProcessTick -- nTick %d nRequestedSafenodeAssets %d nTimeLastPaymentVote %lld GetTime() %lld diff %lld\n", nTick, nRequestedSafenodeAssets, nTimeLastPaymentVote, GetTime(), GetTime() - nTimeLastPaymentVote);
                // check for timeout first
                // This might take a lot longer than SAFENODE_SYNC_TIMEOUT_SECONDS minutes due to new blocks,
                // but that should be OK and it should timeout eventually.
                if(nTimeLastPaymentVote < GetTime() - SAFENODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrintf("CSafenodeSync::ProcessTick -- nTick %d nRequestedSafenodeAssets %d -- timeout\n", nTick, nRequestedSafenodeAssets);
                    if (nRequestedSafenodeAttempt == 0) {
                        LogPrintf("CSafenodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // probably not a good idea to proceed without winner list
                        Fail();
                        ReleaseNodeVector(vNodesCopy);
                        return;
                    }
                    SwitchToNextAsset();
                    ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // check for data
                // if mnpayments already has enough blocks and votes, switch to the next asset
                // try to fetch data from at least two peers though
                if(nRequestedSafenodeAttempt > 1 && mnpayments.IsEnoughData()) {
                    LogPrintf("CSafenodeSync::ProcessTick -- nTick %d nRequestedSafenodeAssets %d -- found enough data\n", nTick, nRequestedSafenodeAssets);
                    SwitchToNextAsset();
                    ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // only request once from each peer
                if(netfulfilledman.HasFulfilledRequest(pnode->addr, "safenode-payment-sync")) continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "safenode-payment-sync");

                if(pnode->nVersion < mnpayments.GetMinSafenodePaymentsProto()) continue;
                nRequestedSafenodeAttempt++;

                // ask node for all payment votes it has (new nodes will only return votes for future payments)
                pnode->PushMessage(NetMsgType::SAFENODEPAYMENTSYNC, mnpayments.GetStorageLimit());
                // ask node for missing pieces only (old nodes will not be asked)
                mnpayments.RequestLowDataPaymentBlocks(pnode);

                ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }

            // GOVOBJ : SYNC GOVERNANCE ITEMS FROM OUR PEERS

            if(nRequestedSafenodeAssets == SAFENODE_SYNC_GOVERNANCE) {
                LogPrint("gobject", "CSafenodeSync::ProcessTick -- nTick %d nRequestedSafenodeAssets %d nTimeLastGovernanceItem %lld GetTime() %lld diff %lld\n", nTick, nRequestedSafenodeAssets, nTimeLastGovernanceItem, GetTime(), GetTime() - nTimeLastGovernanceItem);

                // check for timeout first
                if(GetTime() - nTimeLastGovernanceItem > SAFENODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrintf("CSafenodeSync::ProcessTick -- nTick %d nRequestedSafenodeAssets %d -- timeout\n", nTick, nRequestedSafenodeAssets);
                    if(nRequestedSafenodeAttempt == 0) {
                        LogPrintf("CSafenodeSync::ProcessTick -- WARNING: failed to sync %s\n", GetAssetName());
                        // it's kind of ok to skip this for now, hopefully we'll catch up later?
                    }
                    SwitchToNextAsset();
                    ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // only request obj sync once from each peer, then request votes on per-obj basis
                if(netfulfilledman.HasFulfilledRequest(pnode->addr, "governance-sync")) {
                    int nObjsLeftToAsk = governance.RequestGovernanceObjectVotes(pnode);
                    static int64_t nTimeNoObjectsLeft = 0;
                    // check for data
                    if(nObjsLeftToAsk == 0) {
                        static int nLastTick = 0;
                        static int nLastVotes = 0;
                        if(nTimeNoObjectsLeft == 0) {
                            // asked all objects for votes for the first time
                            nTimeNoObjectsLeft = GetTime();
                        }
                        // make sure the condition below is checked only once per tick
                        if(nLastTick == nTick) continue;
                        if(GetTime() - nTimeNoObjectsLeft > SAFENODE_SYNC_TIMEOUT_SECONDS &&
                            governance.GetVoteCount() - nLastVotes < std::max(int(0.0001 * nLastVotes), SAFENODE_SYNC_TICK_SECONDS)
                        ) {
                            // We already asked for all objects, waited for SAFENODE_SYNC_TIMEOUT_SECONDS
                            // after that and less then 0.01% or SAFENODE_SYNC_TICK_SECONDS
                            // (i.e. 1 per second) votes were recieved during the last tick.
                            // We can be pretty sure that we are done syncing.
                            LogPrintf("CSafenodeSync::ProcessTick -- nTick %d nRequestedSafenodeAssets %d -- asked for all objects, nothing to do\n", nTick, nRequestedSafenodeAssets);
                            // reset nTimeNoObjectsLeft to be able to use the same condition on resync
                            nTimeNoObjectsLeft = 0;
                            SwitchToNextAsset();
                            ReleaseNodeVector(vNodesCopy);
                            return;
                        }
                        nLastTick = nTick;
                        nLastVotes = governance.GetVoteCount();
                    }
                    continue;
                }
                netfulfilledman.AddFulfilledRequest(pnode->addr, "governance-sync");

                if (pnode->nVersion < MIN_GOVERNANCE_PEER_PROTO_VERSION) continue;
                nRequestedSafenodeAttempt++;

                SendGovernanceSyncRequest(pnode);

                ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }
        }
    }
    // looped through all nodes, release them
    ReleaseNodeVector(vNodesCopy);
}

void CSafenodeSync::SendGovernanceSyncRequest(CNode* pnode)
{
    if(pnode->nVersion >= GOVERNANCE_FILTER_PROTO_VERSION) {
        CBloomFilter filter;
        filter.clear();

        pnode->PushMessage(NetMsgType::MNGOVERNANCESYNC, uint256(), filter);
    }
    else {
        pnode->PushMessage(NetMsgType::MNGOVERNANCESYNC, uint256());
    }
}

void CSafenodeSync::UpdatedBlockTip(const CBlockIndex *pindex)
{
    pCurrentBlockIndex = pindex;
}
