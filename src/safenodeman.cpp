// Copyright (c) 2014-2018 The SafeNode developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activesafenode.h"
#include "addrman.h"
#include "darksend.h"
#include "governance.h"
#include "safenode-payments.h"
#include "safenode-sync.h"
#include "safenodeman.h"
#include "netfulfilledman.h"
#include "util.h"

/** Safenode manager */
CSafenodeMan mnodeman;

const std::string CSafenodeMan::SERIALIZATION_VERSION_STRING = "CSafenodeMan-Version-4";

struct CompareLastPaidBlock
{
    bool operator()(const std::pair<int, CSafenode*>& t1,
                    const std::pair<int, CSafenode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

struct CompareScoreMN
{
    bool operator()(const std::pair<int64_t, CSafenode*>& t1,
                    const std::pair<int64_t, CSafenode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->vin < t2.second->vin);
    }
};

CSafenodeIndex::CSafenodeIndex()
    : nSize(0),
      mapIndex(),
      mapReverseIndex()
{}

bool CSafenodeIndex::Get(int nIndex, CTxIn& vinSafenode) const
{
    rindex_m_cit it = mapReverseIndex.find(nIndex);
    if(it == mapReverseIndex.end()) {
        return false;
    }
    vinSafenode = it->second;
    return true;
}

int CSafenodeIndex::GetSafenodeIndex(const CTxIn& vinSafenode) const
{
    index_m_cit it = mapIndex.find(vinSafenode);
    if(it == mapIndex.end()) {
        return -1;
    }
    return it->second;
}

void CSafenodeIndex::AddSafenodeVIN(const CTxIn& vinSafenode)
{
    index_m_it it = mapIndex.find(vinSafenode);
    if(it != mapIndex.end()) {
        return;
    }
    int nNextIndex = nSize;
    mapIndex[vinSafenode] = nNextIndex;
    mapReverseIndex[nNextIndex] = vinSafenode;
    ++nSize;
}

void CSafenodeIndex::Clear()
{
    mapIndex.clear();
    mapReverseIndex.clear();
    nSize = 0;
}
struct CompareByAddr

{
    bool operator()(const CSafenode* t1,
                    const CSafenode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

void CSafenodeIndex::RebuildIndex()
{
    nSize = mapIndex.size();
    for(index_m_it it = mapIndex.begin(); it != mapIndex.end(); ++it) {
        mapReverseIndex[it->second] = it->first;
    }
}

CSafenodeMan::CSafenodeMan()
: cs(),
  vSafenodes(),
  mAskedUsForSafenodeList(),
  mWeAskedForSafenodeList(),
  mWeAskedForSafenodeListEntry(),
  mWeAskedForVerification(),
  mMnbRecoveryRequests(),
  mMnbRecoveryGoodReplies(),
  listScheduledMnbRequestConnections(),
  nLastIndexRebuildTime(0),
  indexSafenodes(),
  indexSafenodesOld(),
  fIndexRebuilt(false),
  fSafenodesAdded(false),
  fSafenodesRemoved(false),
  vecDirtyGovernanceObjectHashes(),
  nLastWatchdogVoteTime(0),
  mapSeenSafenodeBroadcast(),
  mapSeenSafenodePing(),
  nDsqCount(0)
{}

bool CSafenodeMan::Add(CSafenode &mn)
{
    LOCK(cs);

    CSafenode *pmn = Find(mn.vin);
    if (pmn == NULL) {
        LogPrint("safenode", "CSafenodeMan::Add -- Adding new Safenode: addr=%s, %i now\n", mn.addr.ToString(), size() + 1);
        vSafenodes.push_back(mn);
        indexSafenodes.AddSafenodeVIN(mn.vin);
        fSafenodesAdded = true;
        return true;
    }

    return false;
}

void CSafenodeMan::AskForMN(CNode* pnode, const CTxIn &vin)
{
    if(!pnode) return;

    LOCK(cs);

    std::map<COutPoint, std::map<CNetAddr, int64_t> >::iterator it1 = mWeAskedForSafenodeListEntry.find(vin.prevout);
    if (it1 != mWeAskedForSafenodeListEntry.end()) {
        std::map<CNetAddr, int64_t>::iterator it2 = it1->second.find(pnode->addr);
        if (it2 != it1->second.end()) {
            if (GetTime() < it2->second) {
                // we've asked recently, should not repeat too often or we could get banned
                return;
            }
            // we asked this node for this outpoint but it's ok to ask again already
            LogPrintf("CSafenodeMan::AskForMN -- Asking same peer %s for missing safenode entry again: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
        } else {
            // we already asked for this outpoint but not this node
            LogPrintf("CSafenodeMan::AskForMN -- Asking new peer %s for missing safenode entry: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
        }
    } else {
        // we never asked any node for this outpoint
        LogPrintf("CSafenodeMan::AskForMN -- Asking peer %s for missing safenode entry for the first time: %s\n", pnode->addr.ToString(), vin.prevout.ToStringShort());
    }
    mWeAskedForSafenodeListEntry[vin.prevout][pnode->addr] = GetTime() + DSEG_UPDATE_SECONDS;

    pnode->PushMessage(NetMsgType::DSEG, vin);
}

void CSafenodeMan::Check()
{
    LOCK(cs);

    LogPrint("safenode", "CSafenodeMan::Check -- nLastWatchdogVoteTime=%d, IsWatchdogActive()=%d\n", nLastWatchdogVoteTime, IsWatchdogActive());

    BOOST_FOREACH(CSafenode& mn, vSafenodes) {
        mn.Check();
    }
}

void CSafenodeMan::CheckAndRemove()
{
    if(!safenodeSync.IsSafenodeListSynced()) return;

    LogPrintf("CSafenodeMan::CheckAndRemove\n");

    {
        // Need LOCK2 here to ensure consistent locking order because code below locks cs_main
        // in CheckMnbAndUpdateSafenodeList()
        LOCK2(cs_main, cs);

        Check();

        // Remove spent safenodes, prepare structures and make requests to reasure the state of inactive ones
        std::vector<CSafenode>::iterator it = vSafenodes.begin();
        std::vector<std::pair<int, CSafenode> > vecSafenodeRanks;
        // ask for up to MNB_RECOVERY_MAX_ASK_ENTRIES safenode entries at a time
        int nAskForMnbRecovery = MNB_RECOVERY_MAX_ASK_ENTRIES;
        while(it != vSafenodes.end()) {
            CSafenodeBroadcast mnb = CSafenodeBroadcast(*it);
            uint256 hash = mnb.GetHash();
            // If collateral was spent ...
            if ((*it).IsOutpointSpent()) {
                LogPrint("safenode", "CSafenodeMan::CheckAndRemove -- Removing Safenode: %s  addr=%s  %i now\n", (*it).GetStateString(), (*it).addr.ToString(), size() - 1);

                // erase all of the broadcasts we've seen from this txin, ...
                mapSeenSafenodeBroadcast.erase(hash);
                mWeAskedForSafenodeListEntry.erase((*it).vin.prevout);

                // and finally remove it from the list
                it->FlagGovernanceItemsAsDirty();
                it = vSafenodes.erase(it);
                fSafenodesRemoved = true;
            } else {
                bool fAsk = pCurrentBlockIndex &&
                            (nAskForMnbRecovery > 0) &&
                            safenodeSync.IsSynced() &&
                            it->IsNewStartRequired() &&
                            !IsMnbRecoveryRequested(hash);
                if(fAsk) {
                    // this mn is in a non-recoverable state and we haven't asked other nodes yet
                    std::set<CNetAddr> setRequested;
                    // calulate only once and only when it's needed
                    if(vecSafenodeRanks.empty()) {
                        int nRandomBlockHeight = GetRandInt(pCurrentBlockIndex->nHeight);
                        vecSafenodeRanks = GetSafenodeRanks(nRandomBlockHeight);
                    }
                    bool fAskedForMnbRecovery = false;
                    // ask first MNB_RECOVERY_QUORUM_TOTAL safenodes we can connect to and we haven't asked recently
                    for(int i = 0; setRequested.size() < MNB_RECOVERY_QUORUM_TOTAL && i < (int)vecSafenodeRanks.size(); i++) {
                        // avoid banning
                        if(mWeAskedForSafenodeListEntry.count(it->vin.prevout) && mWeAskedForSafenodeListEntry[it->vin.prevout].count(vecSafenodeRanks[i].second.addr)) continue;
                        // didn't ask recently, ok to ask now
                        CService addr = vecSafenodeRanks[i].second.addr;
                        setRequested.insert(addr);
                        listScheduledMnbRequestConnections.push_back(std::make_pair(addr, hash));
                        fAskedForMnbRecovery = true;
                    }
                    if(fAskedForMnbRecovery) {
                        LogPrint("safenode", "CSafenodeMan::CheckAndRemove -- Recovery initiated, safenode=%s\n", it->vin.prevout.ToStringShort());
                        nAskForMnbRecovery--;
                    }
                    // wait for mnb recovery replies for MNB_RECOVERY_WAIT_SECONDS seconds
                    mMnbRecoveryRequests[hash] = std::make_pair(GetTime() + MNB_RECOVERY_WAIT_SECONDS, setRequested);
                }
                ++it;
            }
        }

        // proces replies for SAFENODE_NEW_START_REQUIRED safenodes
        LogPrint("safenode", "CSafenodeMan::CheckAndRemove -- mMnbRecoveryGoodReplies size=%d\n", (int)mMnbRecoveryGoodReplies.size());
        std::map<uint256, std::vector<CSafenodeBroadcast> >::iterator itMnbReplies = mMnbRecoveryGoodReplies.begin();
        while(itMnbReplies != mMnbRecoveryGoodReplies.end()){
            if(mMnbRecoveryRequests[itMnbReplies->first].first < GetTime()) {
                // all nodes we asked should have replied now
                if(itMnbReplies->second.size() >= MNB_RECOVERY_QUORUM_REQUIRED) {
                    // majority of nodes we asked agrees that this mn doesn't require new mnb, reprocess one of new mnbs
                    LogPrint("safenode", "CSafenodeMan::CheckAndRemove -- reprocessing mnb, safenode=%s\n", itMnbReplies->second[0].vin.prevout.ToStringShort());
                    // mapSeenSafenodeBroadcast.erase(itMnbReplies->first);
                    int nDos;
                    itMnbReplies->second[0].fRecovery = true;
                    CheckMnbAndUpdateSafenodeList(NULL, itMnbReplies->second[0], nDos);
                }
                LogPrint("safenode", "CSafenodeMan::CheckAndRemove -- removing mnb recovery reply, safenode=%s, size=%d\n", itMnbReplies->second[0].vin.prevout.ToStringShort(), (int)itMnbReplies->second.size());
                mMnbRecoveryGoodReplies.erase(itMnbReplies++);
            } else {
                ++itMnbReplies;
            }
        }
    }
    {
        // no need for cm_main below
        LOCK(cs);

        std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > >::iterator itMnbRequest = mMnbRecoveryRequests.begin();
        while(itMnbRequest != mMnbRecoveryRequests.end()){
            // Allow this mnb to be re-verified again after MNB_RECOVERY_RETRY_SECONDS seconds
            // if mn is still in SAFENODE_NEW_START_REQUIRED state.
            if(GetTime() - itMnbRequest->second.first > MNB_RECOVERY_RETRY_SECONDS) {
                mMnbRecoveryRequests.erase(itMnbRequest++);
            } else {
                ++itMnbRequest;
            }
        }

        // check who's asked for the Safenode list
        std::map<CNetAddr, int64_t>::iterator it1 = mAskedUsForSafenodeList.begin();
        while(it1 != mAskedUsForSafenodeList.end()){
            if((*it1).second < GetTime()) {
                mAskedUsForSafenodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check who we asked for the Safenode list
        it1 = mWeAskedForSafenodeList.begin();
        while(it1 != mWeAskedForSafenodeList.end()){
            if((*it1).second < GetTime()){
                mWeAskedForSafenodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check which Safenodes we've asked for
        std::map<COutPoint, std::map<CNetAddr, int64_t> >::iterator it2 = mWeAskedForSafenodeListEntry.begin();
        while(it2 != mWeAskedForSafenodeListEntry.end()){
            std::map<CNetAddr, int64_t>::iterator it3 = it2->second.begin();
            while(it3 != it2->second.end()){
                if(it3->second < GetTime()){
                    it2->second.erase(it3++);
                } else {
                    ++it3;
                }
            }
            if(it2->second.empty()) {
                mWeAskedForSafenodeListEntry.erase(it2++);
            } else {
                ++it2;
            }
        }

        std::map<CNetAddr, CSafenodeVerification>::iterator it3 = mWeAskedForVerification.begin();
        while(it3 != mWeAskedForVerification.end()){
            if(it3->second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
                mWeAskedForVerification.erase(it3++);
            } else {
                ++it3;
            }
        }

        // NOTE: do not expire mapSeenSafenodeBroadcast entries here, clean them on mnb updates!

        // remove expired mapSeenSafenodePing
        std::map<uint256, CSafenodePing>::iterator it4 = mapSeenSafenodePing.begin();
        while(it4 != mapSeenSafenodePing.end()){
            if((*it4).second.IsExpired()) {
                LogPrint("safenode", "CSafenodeMan::CheckAndRemove -- Removing expired Safenode ping: hash=%s\n", (*it4).second.GetHash().ToString());
                mapSeenSafenodePing.erase(it4++);
            } else {
                ++it4;
            }
        }

        // remove expired mapSeenSafenodeVerification
        std::map<uint256, CSafenodeVerification>::iterator itv2 = mapSeenSafenodeVerification.begin();
        while(itv2 != mapSeenSafenodeVerification.end()){
            if((*itv2).second.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS){
                LogPrint("safenode", "CSafenodeMan::CheckAndRemove -- Removing expired Safenode verification: hash=%s\n", (*itv2).first.ToString());
                mapSeenSafenodeVerification.erase(itv2++);
            } else {
                ++itv2;
            }
        }

        LogPrintf("CSafenodeMan::CheckAndRemove -- %s\n", ToString());

        if(fSafenodesRemoved) {
            CheckAndRebuildSafenodeIndex();
        }
    }

    if(fSafenodesRemoved) {
        NotifySafenodeUpdates();
    }
}

void CSafenodeMan::Clear()
{
    LOCK(cs);
    vSafenodes.clear();
    mAskedUsForSafenodeList.clear();
    mWeAskedForSafenodeList.clear();
    mWeAskedForSafenodeListEntry.clear();
    mapSeenSafenodeBroadcast.clear();
    mapSeenSafenodePing.clear();
    nDsqCount = 0;
    nLastWatchdogVoteTime = 0;
    indexSafenodes.Clear();
    indexSafenodesOld.Clear();
}

int CSafenodeMan::CountSafenodes(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinSafenodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CSafenode& mn, vSafenodes) {
        if(mn.nProtocolVersion < nProtocolVersion) continue;
        nCount++;
    }

    return nCount;
}

int CSafenodeMan::CountEnabled(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinSafenodePaymentsProto() : nProtocolVersion;

    BOOST_FOREACH(CSafenode& mn, vSafenodes) {
        if(mn.nProtocolVersion < nProtocolVersion || !mn.IsEnabled()) continue;
        nCount++;
    }

    return nCount;
}

/* Only IPv4 safenodes are allowed in 12.1, saving this for later
int CSafenodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    BOOST_FOREACH(CSafenode& mn, vSafenodes)
        if ((nNetworkType == NET_IPV4 && mn.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && mn.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && mn.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}
*/

void CSafenodeMan::DsegUpdate(CNode* pnode)
{
    LOCK(cs);

    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForSafenodeList.find(pnode->addr);
            if(it != mWeAskedForSafenodeList.end() && GetTime() < (*it).second) {
                LogPrintf("CSafenodeMan::DsegUpdate -- we already asked %s for the list; skipping...\n", pnode->addr.ToString());
                return;
            }
        }
    }
    
    pnode->PushMessage(NetMsgType::DSEG, CTxIn());
    int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
    mWeAskedForSafenodeList[pnode->addr] = askAgain;

    LogPrint("safenode", "CSafenodeMan::DsegUpdate -- asked %s for the list\n", pnode->addr.ToString());
}

CSafenode* CSafenodeMan::Find(const CScript &payee)
{
    LOCK(cs);

    BOOST_FOREACH(CSafenode& mn, vSafenodes)
    {
        if(GetScriptForDestination(mn.pubKeyCollateralAddress.GetID()) == payee)
            return &mn;
    }
    return NULL;
}

CSafenode* CSafenodeMan::Find(const CTxIn &vin)
{
    LOCK(cs);

    BOOST_FOREACH(CSafenode& mn, vSafenodes)
    {
        if(mn.vin.prevout == vin.prevout)
            return &mn;
    }
    return NULL;
}

CSafenode* CSafenodeMan::Find(const CPubKey &pubKeySafenode)
{
    LOCK(cs);

    BOOST_FOREACH(CSafenode& mn, vSafenodes)
    {
        if(mn.pubKeySafenode == pubKeySafenode)
            return &mn;
    }
    return NULL;
}

bool CSafenodeMan::Get(const CPubKey& pubKeySafenode, CSafenode& safenode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CSafenode* pMN = Find(pubKeySafenode);
    if(!pMN)  {
        return false;
    }
    safenode = *pMN;
    return true;
}

bool CSafenodeMan::Get(const CTxIn& vin, CSafenode& safenode)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    CSafenode* pMN = Find(vin);
    if(!pMN)  {
        return false;
    }
    safenode = *pMN;
    return true;
}

safenode_info_t CSafenodeMan::GetSafenodeInfo(const CTxIn& vin)
{
    safenode_info_t info;
    LOCK(cs);
    CSafenode* pMN = Find(vin);
    if(!pMN)  {
        return info;
    }
    info = pMN->GetInfo();
    return info;
}

safenode_info_t CSafenodeMan::GetSafenodeInfo(const CPubKey& pubKeySafenode)
{
    safenode_info_t info;
    LOCK(cs);
    CSafenode* pMN = Find(pubKeySafenode);
    if(!pMN)  {
        return info;
    }
    info = pMN->GetInfo();
    return info;
}

bool CSafenodeMan::Has(const CTxIn& vin)
{
    LOCK(cs);
    CSafenode* pMN = Find(vin);
    return (pMN != NULL);
}

//
// Deterministically select the oldest/best safenode to pay on the network
//
CSafenode* CSafenodeMan::GetNextSafenodeInQueueForPayment(bool fFilterSigTime, int& nCount)
{
    if(!pCurrentBlockIndex) {
        nCount = 0;
        return NULL;
    }
    return GetNextSafenodeInQueueForPayment(pCurrentBlockIndex->nHeight, fFilterSigTime, nCount);
}

CSafenode* CSafenodeMan::GetNextSafenodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount)
{
    // Need LOCK2 here to ensure consistent locking order because the GetBlockHash call below locks cs_main
    LOCK2(cs_main,cs);

    CSafenode *pBestSafenode = NULL;
    std::vector<std::pair<int, CSafenode*> > vecSafenodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */

    int nMnCount = CountEnabled();
    BOOST_FOREACH(CSafenode &mn, vSafenodes)
    {
        if(!mn.IsValidForPayment()) continue;

        // //check protocol version
        if(mn.nProtocolVersion < mnpayments.GetMinSafenodePaymentsProto()) continue;

        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if(mnpayments.IsScheduled(mn, nBlockHeight)) continue;

        //it's too new, wait for a cycle
        if(fFilterSigTime && mn.sigTime + (nMnCount*2.6*60) > GetAdjustedTime()) continue;

        //make sure it has at least as many confirmations as there are safenodes
        if(mn.GetCollateralAge() < nMnCount) continue;

        vecSafenodeLastPaid.push_back(std::make_pair(mn.GetLastPaidBlock(), &mn));
    }

    nCount = (int)vecSafenodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if(fFilterSigTime && nCount < nMnCount/3) return GetNextSafenodeInQueueForPayment(nBlockHeight, false, nCount);

    // Sort them low to high
    sort(vecSafenodeLastPaid.begin(), vecSafenodeLastPaid.end(), CompareLastPaidBlock());

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight - 101)) {
        LogPrintf("CSafenode::GetNextSafenodeInQueueForPayment -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight - 101);
        return NULL;
    }
    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = nMnCount/10;
    int nCountTenth = 0;
    arith_uint256 nHighest = 0;
    BOOST_FOREACH (PAIRTYPE(int, CSafenode*)& s, vecSafenodeLastPaid){
        arith_uint256 nScore = s.second->CalculateScore(blockHash);
        if(nScore > nHighest){
            nHighest = nScore;
            pBestSafenode = s.second;
        }
        nCountTenth++;
        if(nCountTenth >= nTenthNetwork) break;
    }
    return pBestSafenode;
}

CSafenode* CSafenodeMan::FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion)
{
    LOCK(cs);

    nProtocolVersion = nProtocolVersion == -1 ? mnpayments.GetMinSafenodePaymentsProto() : nProtocolVersion;

    int nCountEnabled = CountEnabled(nProtocolVersion);
    int nCountNotExcluded = nCountEnabled - vecToExclude.size();

    LogPrintf("CSafenodeMan::FindRandomNotInVec -- %d enabled safenodes, %d safenodes to choose from\n", nCountEnabled, nCountNotExcluded);
    if(nCountNotExcluded < 1) return NULL;

    // fill a vector of pointers
    std::vector<CSafenode*> vpSafenodesShuffled;
    BOOST_FOREACH(CSafenode &mn, vSafenodes) {
        vpSafenodesShuffled.push_back(&mn);
    }

    InsecureRand insecureRand;
    // shuffle pointers
    std::random_shuffle(vpSafenodesShuffled.begin(), vpSafenodesShuffled.end(), insecureRand);
    bool fExclude;

    // loop through
    BOOST_FOREACH(CSafenode* pmn, vpSafenodesShuffled) {
        if(pmn->nProtocolVersion < nProtocolVersion || !pmn->IsEnabled()) continue;
        fExclude = false;
        BOOST_FOREACH(const CTxIn &txinToExclude, vecToExclude) {
            if(pmn->vin.prevout == txinToExclude.prevout) {
                fExclude = true;
                break;
            }
        }
        if(fExclude) continue;
        // found the one not in vecToExclude
        LogPrint("safenode", "CSafenodeMan::FindRandomNotInVec -- found, safenode=%s\n", pmn->vin.prevout.ToStringShort());
        return pmn;
    }

    LogPrint("safenode", "CSafenodeMan::FindRandomNotInVec -- failed\n");
    return NULL;
}

int CSafenodeMan::GetSafenodeRank(const CTxIn& vin, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CSafenode*> > vecSafenodeScores;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return -1;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CSafenode& mn, vSafenodes) {
        if(mn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive) {
            if(!mn.IsEnabled()) continue;
        }
        else {
            if(!mn.IsValidForPayment()) continue;
        }
        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecSafenodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecSafenodeScores.rbegin(), vecSafenodeScores.rend(), CompareScoreMN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CSafenode*)& scorePair, vecSafenodeScores) {
        nRank++;
        if(scorePair.second->vin.prevout == vin.prevout) return nRank;
    }

    return -1;
}

std::vector<std::pair<int, CSafenode> > CSafenodeMan::GetSafenodeRanks(int nBlockHeight, int nMinProtocol)
{
    std::vector<std::pair<int64_t, CSafenode*> > vecSafenodeScores;
    std::vector<std::pair<int, CSafenode> > vecSafenodeRanks;

    //make sure we know about this block
    uint256 blockHash = uint256();
    if(!GetBlockHash(blockHash, nBlockHeight)) return vecSafenodeRanks;

    LOCK(cs);

    // scan for winner
    BOOST_FOREACH(CSafenode& mn, vSafenodes) {

        if(mn.nProtocolVersion < nMinProtocol || !mn.IsEnabled()) continue;

        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecSafenodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecSafenodeScores.rbegin(), vecSafenodeScores.rend(), CompareScoreMN());

    int nRank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CSafenode*)& s, vecSafenodeScores) {
        nRank++;
        vecSafenodeRanks.push_back(std::make_pair(nRank, *s.second));
    }

    return vecSafenodeRanks;
}

CSafenode* CSafenodeMan::GetSafenodeByRank(int nRank, int nBlockHeight, int nMinProtocol, bool fOnlyActive)
{
    std::vector<std::pair<int64_t, CSafenode*> > vecSafenodeScores;

    LOCK(cs);

    uint256 blockHash;
    if(!GetBlockHash(blockHash, nBlockHeight)) {
        LogPrintf("CSafenode::GetSafenodeByRank -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight);
        return NULL;
    }

    // Fill scores
    BOOST_FOREACH(CSafenode& mn, vSafenodes) {

        if(mn.nProtocolVersion < nMinProtocol) continue;
        if(fOnlyActive && !mn.IsEnabled()) continue;

        int64_t nScore = mn.CalculateScore(blockHash).GetCompact(false);

        vecSafenodeScores.push_back(std::make_pair(nScore, &mn));
    }

    sort(vecSafenodeScores.rbegin(), vecSafenodeScores.rend(), CompareScoreMN());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(int64_t, CSafenode*)& s, vecSafenodeScores){
        rank++;
        if(rank == nRank) {
            return s.second;
        }
    }

    return NULL;
}

void CSafenodeMan::ProcessSafenodeConnections()
{
    //we don't care about this for regtest
    if(Params().NetworkIDString() == CBaseChainParams::REGTEST) return;

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes) {
        if(pnode->fSafenode) {
            if(darkSendPool.pSubmittedToSafenode != NULL && pnode->addr == darkSendPool.pSubmittedToSafenode->addr) continue;
            LogPrintf("Closing Safenode connection: peer=%d, addr=%s\n", pnode->id, pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    }
}

std::pair<CService, std::set<uint256> > CSafenodeMan::PopScheduledMnbRequestConnection()
{
    LOCK(cs);
    if(listScheduledMnbRequestConnections.empty()) {
        return std::make_pair(CService(), std::set<uint256>());
    }

    std::set<uint256> setResult;

    listScheduledMnbRequestConnections.sort();
    std::pair<CService, uint256> pairFront = listScheduledMnbRequestConnections.front();

    // squash hashes from requests with the same CService as the first one into setResult
    std::list< std::pair<CService, uint256> >::iterator it = listScheduledMnbRequestConnections.begin();
    while(it != listScheduledMnbRequestConnections.end()) {
        if(pairFront.first == it->first) {
            setResult.insert(it->second);
            it = listScheduledMnbRequestConnections.erase(it);
        } else {
            // since list is sorted now, we can be sure that there is no more hashes left
            // to ask for from this addr
            break;
        }
    }
    return std::make_pair(pairFront.first, setResult);
}


void CSafenodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if(fLiteMode) return; // disable all SafeNode specific functionality
    if(!safenodeSync.IsBlockchainSynced()) return;

    if (strCommand == NetMsgType::MNANNOUNCE) { //Safenode Broadcast

        CSafenodeBroadcast mnb;
        vRecv >> mnb;

        pfrom->setAskFor.erase(mnb.GetHash());

        LogPrint("safenode", "MNANNOUNCE -- Safenode announce, safenode=%s\n", mnb.vin.prevout.ToStringShort());

        int nDos = 0;

        if (CheckMnbAndUpdateSafenodeList(pfrom, mnb, nDos)) {
            // use announced Safenode as a peer
            addrman.Add(CAddress(mnb.addr), pfrom->addr, 2*60*60);
        } else if(nDos > 0) {
            Misbehaving(pfrom->GetId(), nDos);
        }

        if(fSafenodesAdded) {
            NotifySafenodeUpdates();
        }
    } else if (strCommand == NetMsgType::MNPING) { //Safenode Ping

        CSafenodePing mnp;
        vRecv >> mnp;

        uint256 nHash = mnp.GetHash();

        pfrom->setAskFor.erase(nHash);

        LogPrint("safenode", "MNPING -- Safenode ping, safenode=%s\n", mnp.vin.prevout.ToStringShort());

        // Need LOCK2 here to ensure consistent locking order because the CheckAndUpdate call below locks cs_main
        LOCK2(cs_main, cs);

        if(mapSeenSafenodePing.count(nHash)) return; //seen
        mapSeenSafenodePing.insert(std::make_pair(nHash, mnp));

        LogPrint("safenode", "MNPING -- Safenode ping, safenode=%s new\n", mnp.vin.prevout.ToStringShort());

        // see if we have this Safenode
        CSafenode* pmn = mnodeman.Find(mnp.vin);

        // too late, new MNANNOUNCE is required
        if(pmn && pmn->IsNewStartRequired()) return;

        int nDos = 0;
        if(mnp.CheckAndUpdate(pmn, false, nDos)) return;

        if(nDos > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDos);
        } else if(pmn != NULL) {
            // nothing significant failed, mn is a known one too
            return;
        }

        // something significant is broken or mn is unknown,
        // we might have to ask for a safenode entry once
        AskForMN(pfrom, mnp.vin);

    } else if (strCommand == NetMsgType::DSEG) { //Get Safenode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after safenode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!safenodeSync.IsSynced()) return;

        CTxIn vin;
        vRecv >> vin;

        LogPrint("safenode", "DSEG -- Safenode list, safenode=%s\n", vin.prevout.ToStringShort());

        LOCK(cs);

        if(vin == CTxIn()) { //only should ask for this once
            //local network
            bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());

            if(!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForSafenodeList.find(pfrom->addr);
                if (i != mAskedUsForSafenodeList.end()){
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        Misbehaving(pfrom->GetId(), 34);
                        LogPrintf("DSEG -- peer already asked me for the list, peer=%d\n", pfrom->id);
                        return;
                    }
                }
                int64_t askAgain = GetTime() + DSEG_UPDATE_SECONDS;
                mAskedUsForSafenodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok

        int nInvCount = 0;

        BOOST_FOREACH(CSafenode& mn, vSafenodes) {
            if (vin != CTxIn() && vin != mn.vin) continue; // asked for specific vin but we are not there yet
            if (mn.addr.IsRFC1918() || mn.addr.IsLocal()) continue; // do not send local network safenode
            if (mn.IsUpdateRequired()) continue; // do not send outdated safenodes

            LogPrint("safenode", "DSEG -- Sending Safenode entry: safenode=%s  addr=%s\n", mn.vin.prevout.ToStringShort(), mn.addr.ToString());
            CSafenodeBroadcast mnb = CSafenodeBroadcast(mn);
            uint256 hash = mnb.GetHash();
            pfrom->PushInventory(CInv(MSG_SAFENODE_ANNOUNCE, hash));
            pfrom->PushInventory(CInv(MSG_SAFENODE_PING, mn.lastPing.GetHash()));
            nInvCount++;

            if (!mapSeenSafenodeBroadcast.count(hash)) {
                mapSeenSafenodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));
            }

            if (vin == mn.vin) {
                LogPrintf("DSEG -- Sent 1 Safenode inv to peer %d\n", pfrom->id);
                return;
            }
        }

        if(vin == CTxIn()) {
            pfrom->PushMessage(NetMsgType::SYNCSTATUSCOUNT, SAFENODE_SYNC_LIST, nInvCount);
            LogPrintf("DSEG -- Sent %d Safenode invs to peer %d\n", nInvCount, pfrom->id);
            return;
        }
        // smth weird happen - someone asked us for vin we have no idea about?
        LogPrint("safenode", "DSEG -- No invs sent to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::MNVERIFY) { // Safenode Verify

        // Need LOCK2 here to ensure consistent locking order because the all functions below call GetBlockHash which locks cs_main
        LOCK2(cs_main, cs);

        CSafenodeVerification mnv;
        vRecv >> mnv;

        if(mnv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            SendVerifyReply(pfrom, mnv);
        } else if (mnv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some safenode
            ProcessVerifyReply(pfrom, mnv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some safenode which verified another one
            ProcessVerifyBroadcast(pfrom, mnv);
        }
    }
}

// Verification of safenodes via unique direct requests.

void CSafenodeMan::DoFullVerificationStep()
{
    if(activeSafenode.vin == CTxIn()) return;
    if(!safenodeSync.IsSynced()) return;

    std::vector<std::pair<int, CSafenode> > vecSafenodeRanks = GetSafenodeRanks(pCurrentBlockIndex->nHeight - 1, MIN_POSE_PROTO_VERSION);

    // Need LOCK2 here to ensure consistent locking order because the SendVerifyRequest call below locks cs_main
    // through GetHeight() signal in ConnectNode
    LOCK2(cs_main, cs);

    int nCount = 0;

    int nMyRank = -1;
    int nRanksTotal = (int)vecSafenodeRanks.size();

    // send verify requests only if we are in top MAX_POSE_RANK
    std::vector<std::pair<int, CSafenode> >::iterator it = vecSafenodeRanks.begin();
    while(it != vecSafenodeRanks.end()) {
        if(it->first > MAX_POSE_RANK) {
            LogPrint("safenode", "CSafenodeMan::DoFullVerificationStep -- Must be in top %d to send verify request\n",
                        (int)MAX_POSE_RANK);
            return;
        }
        if(it->second.vin == activeSafenode.vin) {
            nMyRank = it->first;
            LogPrint("safenode", "CSafenodeMan::DoFullVerificationStep -- Found self at rank %d/%d, verifying up to %d safenodes\n",
                        nMyRank, nRanksTotal, (int)MAX_POSE_CONNECTIONS);
            break;
        }
        ++it;
    }

    // edge case: list is too short and this safenode is not enabled
    if(nMyRank == -1) return;

    // send verify requests to up to MAX_POSE_CONNECTIONS safenodes
    // starting from MAX_POSE_RANK + nMyRank and using MAX_POSE_CONNECTIONS as a step
    int nOffset = MAX_POSE_RANK + nMyRank - 1;
    if(nOffset >= (int)vecSafenodeRanks.size()) return;

    std::vector<CSafenode*> vSortedByAddr;
    BOOST_FOREACH(CSafenode& mn, vSafenodes) {
        vSortedByAddr.push_back(&mn);
    }

    sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

    it = vecSafenodeRanks.begin() + nOffset;
    while(it != vecSafenodeRanks.end()) {
        if(it->second.IsPoSeVerified() || it->second.IsPoSeBanned()) {
            LogPrint("safenode", "CSafenodeMan::DoFullVerificationStep -- Already %s%s%s safenode %s address %s, skipping...\n",
                        it->second.IsPoSeVerified() ? "verified" : "",
                        it->second.IsPoSeVerified() && it->second.IsPoSeBanned() ? " and " : "",
                        it->second.IsPoSeBanned() ? "banned" : "",
                        it->second.vin.prevout.ToStringShort(), it->second.addr.ToString());
            nOffset += MAX_POSE_CONNECTIONS;
            if(nOffset >= (int)vecSafenodeRanks.size()) break;
            it += MAX_POSE_CONNECTIONS;
            continue;
        }
        LogPrint("safenode", "CSafenodeMan::DoFullVerificationStep -- Verifying safenode %s rank %d/%d address %s\n",
                    it->second.vin.prevout.ToStringShort(), it->first, nRanksTotal, it->second.addr.ToString());
        if(SendVerifyRequest((CAddress)it->second.addr, vSortedByAddr)) {
            nCount++;
            if(nCount >= MAX_POSE_CONNECTIONS) break;
        }
        nOffset += MAX_POSE_CONNECTIONS;
        if(nOffset >= (int)vecSafenodeRanks.size()) break;
        it += MAX_POSE_CONNECTIONS;
    }

    LogPrint("safenode", "CSafenodeMan::DoFullVerificationStep -- Sent verification requests to %d safenodes\n", nCount);
}

// This function tries to find safenodes with the same addr,
// find a verified one and ban all the other. If there are many nodes
// with the same addr but none of them is verified yet, then none of them are banned.
// It could take many times to run this before most of the duplicate nodes are banned.

void CSafenodeMan::CheckSameAddr()
{
    if(!safenodeSync.IsSynced() || vSafenodes.empty()) return;

    std::vector<CSafenode*> vBan;
    std::vector<CSafenode*> vSortedByAddr;

    {
        LOCK(cs);

        CSafenode* pprevSafenode = NULL;
        CSafenode* pverifiedSafenode = NULL;

        BOOST_FOREACH(CSafenode& mn, vSafenodes) {
            vSortedByAddr.push_back(&mn);
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

        BOOST_FOREACH(CSafenode* pmn, vSortedByAddr) {
            // check only (pre)enabled safenodes
            if(!pmn->IsEnabled() && !pmn->IsPreEnabled()) continue;
            // initial step
            if(!pprevSafenode) {
                pprevSafenode = pmn;
                pverifiedSafenode = pmn->IsPoSeVerified() ? pmn : NULL;
                continue;
            }
            // second+ step
            if(pmn->addr == pprevSafenode->addr) {
                if(pverifiedSafenode) {
                    // another safenode with the same ip is verified, ban this one
                    vBan.push_back(pmn);
                } else if(pmn->IsPoSeVerified()) {
                    // this safenode with the same ip is verified, ban previous one
                    vBan.push_back(pprevSafenode);
                    // and keep a reference to be able to ban following safenodes with the same ip
                    pverifiedSafenode = pmn;
                }
            } else {
                pverifiedSafenode = pmn->IsPoSeVerified() ? pmn : NULL;
            }
            pprevSafenode = pmn;
        }
    }

    // ban duplicates
    BOOST_FOREACH(CSafenode* pmn, vBan) {
        LogPrintf("CSafenodeMan::CheckSameAddr -- increasing PoSe ban score for safenode %s\n", pmn->vin.prevout.ToStringShort());
        pmn->IncreasePoSeBanScore();
    }
}

bool CSafenodeMan::SendVerifyRequest(const CAddress& addr, const std::vector<CSafenode*>& vSortedByAddr)
{
    if(netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        // we already asked for verification, not a good idea to do this too often, skip it
        LogPrint("safenode", "CSafenodeMan::SendVerifyRequest -- too many requests, skipping... addr=%s\n", addr.ToString());
        return false;
    }

    CNode* pnode = ConnectNode(addr, NULL, true);
    if(pnode == NULL) {
        LogPrintf("CSafenodeMan::SendVerifyRequest -- can't connect to node to verify it, addr=%s\n", addr.ToString());
        return false;
    }

    netfulfilledman.AddFulfilledRequest(addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request");
    // use random nonce, store it and require node to reply with correct one later
    CSafenodeVerification mnv(addr, GetRandInt(999999), pCurrentBlockIndex->nHeight - 1);
    mWeAskedForVerification[addr] = mnv;
    LogPrintf("CSafenodeMan::SendVerifyRequest -- verifying node using nonce %d addr=%s\n", mnv.nonce, addr.ToString());
    pnode->PushMessage(NetMsgType::MNVERIFY, mnv);

    return true;
}

void CSafenodeMan::SendVerifyReply(CNode* pnode, CSafenodeVerification& mnv)
{
    // only safenodes can sign this, why would someone ask regular node?
    if(!fSafeNode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        return;
    }

    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply")) {
        // peer should not ask us that often
        LogPrintf("SafenodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        LogPrintf("SafenodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    std::string strMessage = strprintf("%s%d%s", activeSafenode.service.ToString(false), mnv.nonce, blockHash.ToString());

    if(!darkSendSigner.SignMessage(strMessage, mnv.vchSig1, activeSafenode.keySafenode)) {
        LogPrintf("SafenodeMan::SendVerifyReply -- SignMessage() failed\n");
        return;
    }

    std::string strError;

    if(!darkSendSigner.VerifyMessage(activeSafenode.pubKeySafenode, mnv.vchSig1, strMessage, strError)) {
        LogPrintf("SafenodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
        return;
    }

    pnode->PushMessage(NetMsgType::MNVERIFY, mnv);
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-reply");
}

void CSafenodeMan::ProcessVerifyReply(CNode* pnode, CSafenodeVerification& mnv)
{
    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if(!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-request")) {
        LogPrintf("CSafenodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nonce != mnv.nonce) {
        LogPrintf("CSafenodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nonce, mnv.nonce, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if(mWeAskedForVerification[pnode->addr].nBlockHeight != mnv.nBlockHeight) {
        LogPrintf("CSafenodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d\n",
                    mWeAskedForVerification[pnode->addr].nBlockHeight, mnv.nBlockHeight, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("SafenodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    // we already verified this address, why node is spamming?
    if(netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done")) {
        LogPrintf("CSafenodeMan::ProcessVerifyReply -- ERROR: already verified %s recently\n", pnode->addr.ToString());
        Misbehaving(pnode->id, 20);
        return;
    }

    {
        LOCK(cs);

        CSafenode* prealSafenode = NULL;
        std::vector<CSafenode*> vpSafenodesToBan;
        std::vector<CSafenode>::iterator it = vSafenodes.begin();
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(false), mnv.nonce, blockHash.ToString());
        while(it != vSafenodes.end()) {
            if((CAddress)it->addr == pnode->addr) {
                if(darkSendSigner.VerifyMessage(it->pubKeySafenode, mnv.vchSig1, strMessage1, strError)) {
                    // found it!
                    prealSafenode = &(*it);
                    if(!it->IsPoSeVerified()) {
                        it->DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::MNVERIFY)+"-done");

                    // we can only broadcast it if we are an activated safenode
                    if(activeSafenode.vin == CTxIn()) continue;
                    // update ...
                    mnv.addr = it->addr;
                    mnv.vin1 = it->vin;
                    mnv.vin2 = activeSafenode.vin;
                    std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(false), mnv.nonce, blockHash.ToString(),
                                            mnv.vin1.prevout.ToStringShort(), mnv.vin2.prevout.ToStringShort());
                    // ... and sign it
                    if(!darkSendSigner.SignMessage(strMessage2, mnv.vchSig2, activeSafenode.keySafenode)) {
                        LogPrintf("SafenodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                        return;
                    }

                    std::string strError;

                    if(!darkSendSigner.VerifyMessage(activeSafenode.pubKeySafenode, mnv.vchSig2, strMessage2, strError)) {
                        LogPrintf("SafenodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                        return;
                    }

                    mWeAskedForVerification[pnode->addr] = mnv;
                    mnv.Relay();

                } else {
                    vpSafenodesToBan.push_back(&(*it));
                }
            }
            ++it;
        }
        // no real safenode found?...
        if(!prealSafenode) {
            // this should never be the case normally,
            // only if someone is trying to game the system in some way or smth like that
            LogPrintf("CSafenodeMan::ProcessVerifyReply -- ERROR: no real safenode found for addr %s\n", pnode->addr.ToString());
            Misbehaving(pnode->id, 20);
            return;
        }
        LogPrintf("CSafenodeMan::ProcessVerifyReply -- verified real safenode %s for addr %s\n",
                    prealSafenode->vin.prevout.ToStringShort(), pnode->addr.ToString());
        // increase ban score for everyone else
        BOOST_FOREACH(CSafenode* pmn, vpSafenodesToBan) {
            pmn->IncreasePoSeBanScore();
            LogPrint("safenode", "CSafenodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        prealSafenode->vin.prevout.ToStringShort(), pnode->addr.ToString(), pmn->nPoSeBanScore);
        }
        LogPrintf("CSafenodeMan::ProcessVerifyBroadcast -- PoSe score increased for %d fake safenodes, addr %s\n",
                    (int)vpSafenodesToBan.size(), pnode->addr.ToString());
    }
}

void CSafenodeMan::ProcessVerifyBroadcast(CNode* pnode, const CSafenodeVerification& mnv)
{
    std::string strError;

    if(mapSeenSafenodeVerification.find(mnv.GetHash()) != mapSeenSafenodeVerification.end()) {
        // we already have one
        return;
    }
    mapSeenSafenodeVerification[mnv.GetHash()] = mnv;

    // we don't care about history
    if(mnv.nBlockHeight < pCurrentBlockIndex->nHeight - MAX_POSE_BLOCKS) {
        LogPrint("safenode", "SafenodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d\n",
                    pCurrentBlockIndex->nHeight, mnv.nBlockHeight, pnode->id);
        return;
    }

    if(mnv.vin1.prevout == mnv.vin2.prevout) {
        LogPrint("safenode", "SafenodeMan::ProcessVerifyBroadcast -- ERROR: same vins %s, peer=%d\n",
                    mnv.vin1.prevout.ToStringShort(), pnode->id);
        // that was NOT a good idea to cheat and verify itself,
        // ban the node we received such message from
        Misbehaving(pnode->id, 100);
        return;
    }

    uint256 blockHash;
    if(!GetBlockHash(blockHash, mnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("SafenodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d\n", mnv.nBlockHeight, pnode->id);
        return;
    }

    int nRank = GetSafenodeRank(mnv.vin2, mnv.nBlockHeight, MIN_POSE_PROTO_VERSION);

    if (nRank == -1) {
        LogPrint("safenode", "CSafenodeMan::ProcessVerifyBroadcast -- Can't calculate rank for safenode %s\n",
                    mnv.vin2.prevout.ToStringShort());
        return;
    }

    if(nRank > MAX_POSE_RANK) {
        LogPrint("safenode", "CSafenodeMan::ProcessVerifyBroadcast -- Mastrernode %s is not in top %d, current rank %d, peer=%d\n",
                    mnv.vin2.prevout.ToStringShort(), (int)MAX_POSE_RANK, nRank, pnode->id);
        return;
    }

    {
        LOCK(cs);

        std::string strMessage1 = strprintf("%s%d%s", mnv.addr.ToString(false), mnv.nonce, blockHash.ToString());
        std::string strMessage2 = strprintf("%s%d%s%s%s", mnv.addr.ToString(false), mnv.nonce, blockHash.ToString(),
                                mnv.vin1.prevout.ToStringShort(), mnv.vin2.prevout.ToStringShort());

        CSafenode* pmn1 = Find(mnv.vin1);
        if(!pmn1) {
            LogPrintf("CSafenodeMan::ProcessVerifyBroadcast -- can't find safenode1 %s\n", mnv.vin1.prevout.ToStringShort());
            return;
        }

        CSafenode* pmn2 = Find(mnv.vin2);
        if(!pmn2) {
            LogPrintf("CSafenodeMan::ProcessVerifyBroadcast -- can't find safenode2 %s\n", mnv.vin2.prevout.ToStringShort());
            return;
        }

        if(pmn1->addr != mnv.addr) {
            LogPrintf("CSafenodeMan::ProcessVerifyBroadcast -- addr %s do not match %s\n", mnv.addr.ToString(), pnode->addr.ToString());
            return;
        }

        if(darkSendSigner.VerifyMessage(pmn1->pubKeySafenode, mnv.vchSig1, strMessage1, strError)) {
            LogPrintf("SafenodeMan::ProcessVerifyBroadcast -- VerifyMessage() for safenode1 failed, error: %s\n", strError);
            return;
        }

        if(darkSendSigner.VerifyMessage(pmn2->pubKeySafenode, mnv.vchSig2, strMessage2, strError)) {
            LogPrintf("SafenodeMan::ProcessVerifyBroadcast -- VerifyMessage() for safenode2 failed, error: %s\n", strError);
            return;
        }

        if(!pmn1->IsPoSeVerified()) {
            pmn1->DecreasePoSeBanScore();
        }
        mnv.Relay();

        LogPrintf("CSafenodeMan::ProcessVerifyBroadcast -- verified safenode %s for addr %s\n",
                    pmn1->vin.prevout.ToStringShort(), pnode->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        BOOST_FOREACH(CSafenode& mn, vSafenodes) {
            if(mn.addr != mnv.addr || mn.vin.prevout == mnv.vin1.prevout) continue;
            mn.IncreasePoSeBanScore();
            nCount++;
            LogPrint("safenode", "CSafenodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                        mn.vin.prevout.ToStringShort(), mn.addr.ToString(), mn.nPoSeBanScore);
        }
        LogPrintf("CSafenodeMan::ProcessVerifyBroadcast -- PoSe score incresed for %d fake safenodes, addr %s\n",
                    nCount, pnode->addr.ToString());
    }
}

std::string CSafenodeMan::ToString() const
{
    std::ostringstream info;

    info << "Safenodes: " << (int)vSafenodes.size() <<
            ", peers who asked us for Safenode list: " << (int)mAskedUsForSafenodeList.size() <<
            ", peers we asked for Safenode list: " << (int)mWeAskedForSafenodeList.size() <<
            ", entries in Safenode list we asked for: " << (int)mWeAskedForSafenodeListEntry.size() <<
            ", safenode index size: " << indexSafenodes.GetSize() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}

void CSafenodeMan::UpdateSafenodeList(CSafenodeBroadcast mnb)
{
    LOCK(cs);
    mapSeenSafenodePing.insert(std::make_pair(mnb.lastPing.GetHash(), mnb.lastPing));
    mapSeenSafenodeBroadcast.insert(std::make_pair(mnb.GetHash(), std::make_pair(GetTime(), mnb)));

    LogPrintf("CSafenodeMan::UpdateSafenodeList -- safenode=%s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());

    CSafenode* pmn = Find(mnb.vin);
    if(pmn == NULL) {
        CSafenode mn(mnb);
        if(Add(mn)) {
            safenodeSync.AddedSafenodeList();
        }
    } else {
        CSafenodeBroadcast mnbOld = mapSeenSafenodeBroadcast[CSafenodeBroadcast(*pmn).GetHash()].second;
        if(pmn->UpdateFromNewBroadcast(mnb)) {
            safenodeSync.AddedSafenodeList();
            mapSeenSafenodeBroadcast.erase(mnbOld.GetHash());
        }
    }
}

bool CSafenodeMan::CheckMnbAndUpdateSafenodeList(CNode* pfrom, CSafenodeBroadcast mnb, int& nDos)
{
    // Need LOCK2 here to ensure consistent locking order because the SimpleCheck call below locks cs_main
    LOCK2(cs_main, cs);

    nDos = 0;
    LogPrint("safenode", "CSafenodeMan::CheckMnbAndUpdateSafenodeList -- safenode=%s\n", mnb.vin.prevout.ToStringShort());

    uint256 hash = mnb.GetHash();
    if(mapSeenSafenodeBroadcast.count(hash) && !mnb.fRecovery) { //seen
        LogPrint("safenode", "CSafenodeMan::CheckMnbAndUpdateSafenodeList -- safenode=%s seen\n", mnb.vin.prevout.ToStringShort());
        // less then 2 pings left before this MN goes into non-recoverable state, bump sync timeout
        if(GetTime() - mapSeenSafenodeBroadcast[hash].first > SAFENODE_NEW_START_REQUIRED_SECONDS - SAFENODE_MIN_MNP_SECONDS * 2) {
            LogPrint("safenode", "CSafenodeMan::CheckMnbAndUpdateSafenodeList -- safenode=%s seen update\n", mnb.vin.prevout.ToStringShort());
            mapSeenSafenodeBroadcast[hash].first = GetTime();
            safenodeSync.AddedSafenodeList();
        }
        // did we ask this node for it?
        if(pfrom && IsMnbRecoveryRequested(hash) && GetTime() < mMnbRecoveryRequests[hash].first) {
            LogPrint("safenode", "CSafenodeMan::CheckMnbAndUpdateSafenodeList -- mnb=%s seen request\n", hash.ToString());
            if(mMnbRecoveryRequests[hash].second.count(pfrom->addr)) {
                LogPrint("safenode", "CSafenodeMan::CheckMnbAndUpdateSafenodeList -- mnb=%s seen request, addr=%s\n", hash.ToString(), pfrom->addr.ToString());
                // do not allow node to send same mnb multiple times in recovery mode
                mMnbRecoveryRequests[hash].second.erase(pfrom->addr);
                // does it have newer lastPing?
                if(mnb.lastPing.sigTime > mapSeenSafenodeBroadcast[hash].second.lastPing.sigTime) {
                    // simulate Check
                    CSafenode mnTemp = CSafenode(mnb);
                    mnTemp.Check();
                    LogPrint("safenode", "CSafenodeMan::CheckMnbAndUpdateSafenodeList -- mnb=%s seen request, addr=%s, better lastPing: %d min ago, projected mn state: %s\n", hash.ToString(), pfrom->addr.ToString(), (GetTime() - mnb.lastPing.sigTime)/60, mnTemp.GetStateString());
                    if(mnTemp.IsValidStateForAutoStart(mnTemp.nActiveState)) {
                        // this node thinks it's a good one
                        LogPrint("safenode", "CSafenodeMan::CheckMnbAndUpdateSafenodeList -- safenode=%s seen good\n", mnb.vin.prevout.ToStringShort());
                        mMnbRecoveryGoodReplies[hash].push_back(mnb);
                    }
                }
            }
        }
        return true;
    }
    mapSeenSafenodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), mnb)));

    LogPrint("safenode", "CSafenodeMan::CheckMnbAndUpdateSafenodeList -- safenode=%s new\n", mnb.vin.prevout.ToStringShort());

    if(!mnb.SimpleCheck(nDos)) {
        LogPrint("safenode", "CSafenodeMan::CheckMnbAndUpdateSafenodeList -- SimpleCheck() failed, safenode=%s\n", mnb.vin.prevout.ToStringShort());
        return false;
    }

    // search Safenode list
    CSafenode* pmn = Find(mnb.vin);
    if(pmn) {
        CSafenodeBroadcast mnbOld = mapSeenSafenodeBroadcast[CSafenodeBroadcast(*pmn).GetHash()].second;
        if(!mnb.Update(pmn, nDos)) {
            LogPrint("safenode", "CSafenodeMan::CheckMnbAndUpdateSafenodeList -- Update() failed, safenode=%s\n", mnb.vin.prevout.ToStringShort());
            return false;
        }
        if(hash != mnbOld.GetHash()) {
            mapSeenSafenodeBroadcast.erase(mnbOld.GetHash());
        }
    } else {
        if(mnb.CheckOutpoint(nDos)) {
            Add(mnb);
            safenodeSync.AddedSafenodeList();
            // if it matches our Safenode privkey...
            if(fSafeNode && mnb.pubKeySafenode == activeSafenode.pubKeySafenode) {
                mnb.nPoSeBanScore = -SAFENODE_POSE_BAN_MAX_SCORE;
                if(mnb.nProtocolVersion == PROTOCOL_VERSION) {
                    // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                    LogPrintf("CSafenodeMan::CheckMnbAndUpdateSafenodeList -- Got NEW Safenode entry: safenode=%s  sigTime=%lld  addr=%s\n",
                                mnb.vin.prevout.ToStringShort(), mnb.sigTime, mnb.addr.ToString());
                    activeSafenode.ManageState();
                } else {
                    // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                    // but also do not ban the node we get this message from
                    LogPrintf("CSafenodeMan::CheckMnbAndUpdateSafenodeList -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", mnb.nProtocolVersion, PROTOCOL_VERSION);
                    return false;
                }
            }
            mnb.Relay();
        } else {
            LogPrintf("CSafenodeMan::CheckMnbAndUpdateSafenodeList -- Rejected Safenode entry: %s  addr=%s\n", mnb.vin.prevout.ToStringShort(), mnb.addr.ToString());
            return false;
        }
    }

    return true;
}

void CSafenodeMan::UpdateLastPaid()
{
    LOCK(cs);

    if(fLiteMode) return;
    if(!pCurrentBlockIndex) return;

    static bool IsFirstRun = true;
    // Do full scan on first run or if we are not a safenode
    // (MNs should update this info on every block, so limited scan should be enough for them)
    int nMaxBlocksToScanBack = (IsFirstRun || !fSafeNode) ? mnpayments.GetStorageLimit() : LAST_PAID_SCAN_BLOCKS;

    // LogPrint("mnpayments", "CSafenodeMan::UpdateLastPaid -- nHeight=%d, nMaxBlocksToScanBack=%d, IsFirstRun=%s\n",
    //                         pCurrentBlockIndex->nHeight, nMaxBlocksToScanBack, IsFirstRun ? "true" : "false");

    BOOST_FOREACH(CSafenode& mn, vSafenodes) {
        mn.UpdateLastPaid(pCurrentBlockIndex, nMaxBlocksToScanBack);
    }

    // every time is like the first time if winners list is not synced
    IsFirstRun = !safenodeSync.IsWinnersListSynced();
}

void CSafenodeMan::CheckAndRebuildSafenodeIndex()
{
    LOCK(cs);

    if(GetTime() - nLastIndexRebuildTime < MIN_INDEX_REBUILD_TIME) {
        return;
    }

    if(indexSafenodes.GetSize() <= MAX_EXPECTED_INDEX_SIZE) {
        return;
    }

    if(indexSafenodes.GetSize() <= int(vSafenodes.size())) {
        return;
    }

    indexSafenodesOld = indexSafenodes;
    indexSafenodes.Clear();
    for(size_t i = 0; i < vSafenodes.size(); ++i) {
        indexSafenodes.AddSafenodeVIN(vSafenodes[i].vin);
    }

    fIndexRebuilt = true;
    nLastIndexRebuildTime = GetTime();
}

void CSafenodeMan::UpdateWatchdogVoteTime(const CTxIn& vin)
{
    LOCK(cs);
    CSafenode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->UpdateWatchdogVoteTime();
    nLastWatchdogVoteTime = GetTime();
}

bool CSafenodeMan::IsWatchdogActive()
{
    LOCK(cs);
    // Check if any safenodes have voted recently, otherwise return false
    // Changed behaviour to avoid WATCHDOG issues
    return false;//(GetTime() - nLastWatchdogVoteTime) <= SAFENODE_WATCHDOG_MAX_SECONDS;
}

bool CSafenodeMan::AddGovernanceVote(const CTxIn& vin, uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    CSafenode* pMN = Find(vin);
    if(!pMN)  {
        return false;
    }
    pMN->AddGovernanceVote(nGovernanceObjectHash);
    return true;
}

void CSafenodeMan::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    BOOST_FOREACH(CSafenode& mn, vSafenodes) {
        mn.RemoveGovernanceObject(nGovernanceObjectHash);
    }
}

void CSafenodeMan::CheckSafenode(const CTxIn& vin, bool fForce)
{
    LOCK(cs);
    CSafenode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->Check(fForce);
}

void CSafenodeMan::CheckSafenode(const CPubKey& pubKeySafenode, bool fForce)
{
    LOCK(cs);
    CSafenode* pMN = Find(pubKeySafenode);
    if(!pMN)  {
        return;
    }
    pMN->Check(fForce);
}

int CSafenodeMan::GetSafenodeState(const CTxIn& vin)
{
    LOCK(cs);
    CSafenode* pMN = Find(vin);
    if(!pMN)  {
        return CSafenode::SAFENODE_NEW_START_REQUIRED;
    }
    return pMN->nActiveState;
}

int CSafenodeMan::GetSafenodeState(const CPubKey& pubKeySafenode)
{
    LOCK(cs);
    CSafenode* pMN = Find(pubKeySafenode);
    if(!pMN)  {
        return CSafenode::SAFENODE_NEW_START_REQUIRED;
    }
    return pMN->nActiveState;
}

bool CSafenodeMan::IsSafenodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CSafenode* pMN = Find(vin);
    if(!pMN) {
        return false;
    }
    return pMN->IsPingedWithin(nSeconds, nTimeToCheckAt);
}

void CSafenodeMan::SetSafenodeLastPing(const CTxIn& vin, const CSafenodePing& mnp)
{
    LOCK(cs);
    CSafenode* pMN = Find(vin);
    if(!pMN)  {
        return;
    }
    pMN->lastPing = mnp;
    mapSeenSafenodePing.insert(std::make_pair(mnp.GetHash(), mnp));

    CSafenodeBroadcast mnb(*pMN);
    uint256 hash = mnb.GetHash();
    if(mapSeenSafenodeBroadcast.count(hash)) {
        mapSeenSafenodeBroadcast[hash].second.lastPing = mnp;
    }
}

void CSafenodeMan::UpdatedBlockTip(const CBlockIndex *pindex)
{
    pCurrentBlockIndex = pindex;
    LogPrint("safenode", "CSafenodeMan::UpdatedBlockTip -- pCurrentBlockIndex->nHeight=%d\n", pCurrentBlockIndex->nHeight);

    CheckSameAddr();

    if(fSafeNode) {
        // normal wallet does not need to update this every block, doing update on rpc call should be enough
        UpdateLastPaid();
    }
}

void CSafenodeMan::NotifySafenodeUpdates()
{
    // Avoid double locking
    bool fSafenodesAddedLocal = false;
    bool fSafenodesRemovedLocal = false;
    {
        LOCK(cs);
        fSafenodesAddedLocal = fSafenodesAdded;
        fSafenodesRemovedLocal = fSafenodesRemoved;
    }

    if(fSafenodesAddedLocal) {
        governance.CheckSafenodeOrphanObjects();
        governance.CheckSafenodeOrphanVotes();
    }
    if(fSafenodesRemovedLocal) {
        governance.UpdateCachesAndClean();
    }

    LOCK(cs);
    fSafenodesAdded = false;
    fSafenodesRemoved = false;
}
