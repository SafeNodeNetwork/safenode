// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2018 The SafeNode developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SAFENODEMAN_H
#define SAFENODEMAN_H

#include "safenode.h"
#include "sync.h"

using namespace std;

class CSafenodeMan;

extern CSafenodeMan mnodeman;

/**
 * Provides a forward and reverse index between MN vin's and integers.
 *
 * This mapping is normally add-only and is expected to be permanent
 * It is only rebuilt if the size of the index exceeds the expected maximum number
 * of MN's and the current number of known MN's.
 *
 * The external interface to this index is provided via delegation by CSafenodeMan
 */
class CSafenodeIndex
{
public: // Types
    typedef std::map<CTxIn,int> index_m_t;

    typedef index_m_t::iterator index_m_it;

    typedef index_m_t::const_iterator index_m_cit;

    typedef std::map<int,CTxIn> rindex_m_t;

    typedef rindex_m_t::iterator rindex_m_it;

    typedef rindex_m_t::const_iterator rindex_m_cit;

private:
    int                  nSize;

    index_m_t            mapIndex;

    rindex_m_t           mapReverseIndex;

public:
    CSafenodeIndex();

    int GetSize() const {
        return nSize;
    }

    /// Retrieve safenode vin by index
    bool Get(int nIndex, CTxIn& vinSafenode) const;

    /// Get index of a safenode vin
    int GetSafenodeIndex(const CTxIn& vinSafenode) const;

    void AddSafenodeVIN(const CTxIn& vinSafenode);

    void Clear();

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion)
    {
        READWRITE(mapIndex);
        if(ser_action.ForRead()) {
            RebuildIndex();
        }
    }

private:
    void RebuildIndex();

};

class CSafenodeMan
{
public:
    typedef std::map<CTxIn,int> index_m_t;

    typedef index_m_t::iterator index_m_it;

    typedef index_m_t::const_iterator index_m_cit;

private:
    static const int MAX_EXPECTED_INDEX_SIZE = 30000;

    /// Only allow 1 index rebuild per hour
    static const int64_t MIN_INDEX_REBUILD_TIME = 3600;

    static const std::string SERIALIZATION_VERSION_STRING;

    static const int DSEG_UPDATE_SECONDS        = 3 * 60 * 60;

    static const int LAST_PAID_SCAN_BLOCKS      = 100;

    static const int MIN_POSE_PROTO_VERSION     = 70203;
    static const int MAX_POSE_CONNECTIONS       = 10;
    static const int MAX_POSE_RANK              = 10;
    static const int MAX_POSE_BLOCKS            = 10;

    static const int MNB_RECOVERY_QUORUM_TOTAL      = 10;
    static const int MNB_RECOVERY_QUORUM_REQUIRED   = 6;
    static const int MNB_RECOVERY_MAX_ASK_ENTRIES   = 10;
    static const int MNB_RECOVERY_WAIT_SECONDS      = 60;
    static const int MNB_RECOVERY_RETRY_SECONDS     = 3 * 60 * 60;


    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    // Keep track of current block index
    const CBlockIndex *pCurrentBlockIndex;

    // map to hold all MNs
    std::vector<CSafenode> vSafenodes;
    // who's asked for the Safenode list and the last time
    std::map<CNetAddr, int64_t> mAskedUsForSafenodeList;
    // who we asked for the Safenode list and the last time
    std::map<CNetAddr, int64_t> mWeAskedForSafenodeList;
    // which Safenodes we've asked for
    std::map<COutPoint, std::map<CNetAddr, int64_t> > mWeAskedForSafenodeListEntry;
    // who we asked for the safenode verification
    std::map<CNetAddr, CSafenodeVerification> mWeAskedForVerification;

    // these maps are used for safenode recovery from SAFENODE_NEW_START_REQUIRED state
    std::map<uint256, std::pair< int64_t, std::set<CNetAddr> > > mMnbRecoveryRequests;
    std::map<uint256, std::vector<CSafenodeBroadcast> > mMnbRecoveryGoodReplies;
    std::list< std::pair<CService, uint256> > listScheduledMnbRequestConnections;

    int64_t nLastIndexRebuildTime;

    CSafenodeIndex indexSafenodes;

    CSafenodeIndex indexSafenodesOld;

    /// Set when index has been rebuilt, clear when read
    bool fIndexRebuilt;

    /// Set when safenodes are added, cleared when CGovernanceManager is notified
    bool fSafenodesAdded;

    /// Set when safenodes are removed, cleared when CGovernanceManager is notified
    bool fSafenodesRemoved;

    std::vector<uint256> vecDirtyGovernanceObjectHashes;

    int64_t nLastWatchdogVoteTime;

    friend class CSafenodeSync;

public:
    // Keep track of all broadcasts I've seen
    std::map<uint256, std::pair<int64_t, CSafenodeBroadcast> > mapSeenSafenodeBroadcast;
    // Keep track of all pings I've seen
    std::map<uint256, CSafenodePing> mapSeenSafenodePing;
    // Keep track of all verifications I've seen
    std::map<uint256, CSafenodeVerification> mapSeenSafenodeVerification;
    // keep track of dsq count to prevent safenodes from gaming darksend queue
    int64_t nDsqCount;


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        LOCK(cs);
        std::string strVersion;
        if(ser_action.ForRead()) {
            READWRITE(strVersion);
        }
        else {
            strVersion = SERIALIZATION_VERSION_STRING; 
            READWRITE(strVersion);
        }

        READWRITE(vSafenodes);
        READWRITE(mAskedUsForSafenodeList);
        READWRITE(mWeAskedForSafenodeList);
        READWRITE(mWeAskedForSafenodeListEntry);
        READWRITE(mMnbRecoveryRequests);
        READWRITE(mMnbRecoveryGoodReplies);
        READWRITE(nLastWatchdogVoteTime);
        READWRITE(nDsqCount);

        READWRITE(mapSeenSafenodeBroadcast);
        READWRITE(mapSeenSafenodePing);
        READWRITE(indexSafenodes);
        if(ser_action.ForRead() && (strVersion != SERIALIZATION_VERSION_STRING)) {
            Clear();
        }
    }

    CSafenodeMan();

    /// Add an entry
    bool Add(CSafenode &mn);

    /// Ask (source) node for mnb
    void AskForMN(CNode *pnode, const CTxIn &vin);
    void AskForMnb(CNode *pnode, const uint256 &hash);

    /// Check all Safenodes
    void Check();

    /// Check all Safenodes and remove inactive
    void CheckAndRemove();

    /// Clear Safenode vector
    void Clear();

    /// Count Safenodes filtered by nProtocolVersion.
    /// Safenode nProtocolVersion should match or be above the one specified in param here.
    int CountSafenodes(int nProtocolVersion = -1);
    /// Count enabled Safenodes filtered by nProtocolVersion.
    /// Safenode nProtocolVersion should match or be above the one specified in param here.
    int CountEnabled(int nProtocolVersion = -1);

    /// Count Safenodes by network type - NET_IPV4, NET_IPV6, NET_TOR
    // int CountByIP(int nNetworkType);

    void DsegUpdate(CNode* pnode);

    /// Find an entry
    CSafenode* Find(const CScript &payee);
    CSafenode* Find(const CTxIn& vin);
    CSafenode* Find(const CPubKey& pubKeySafenode);

    /// Versions of Find that are safe to use from outside the class
    bool Get(const CPubKey& pubKeySafenode, CSafenode& safenode);
    bool Get(const CTxIn& vin, CSafenode& safenode);

    /// Retrieve safenode vin by index
    bool Get(int nIndex, CTxIn& vinSafenode, bool& fIndexRebuiltOut) {
        LOCK(cs);
        fIndexRebuiltOut = fIndexRebuilt;
        return indexSafenodes.Get(nIndex, vinSafenode);
    }

    bool GetIndexRebuiltFlag() {
        LOCK(cs);
        return fIndexRebuilt;
    }

    /// Get index of a safenode vin
    int GetSafenodeIndex(const CTxIn& vinSafenode) {
        LOCK(cs);
        return indexSafenodes.GetSafenodeIndex(vinSafenode);
    }

    /// Get old index of a safenode vin
    int GetSafenodeIndexOld(const CTxIn& vinSafenode) {
        LOCK(cs);
        return indexSafenodesOld.GetSafenodeIndex(vinSafenode);
    }

    /// Get safenode VIN for an old index value
    bool GetSafenodeVinForIndexOld(int nSafenodeIndex, CTxIn& vinSafenodeOut) {
        LOCK(cs);
        return indexSafenodesOld.Get(nSafenodeIndex, vinSafenodeOut);
    }

    /// Get index of a safenode vin, returning rebuild flag
    int GetSafenodeIndex(const CTxIn& vinSafenode, bool& fIndexRebuiltOut) {
        LOCK(cs);
        fIndexRebuiltOut = fIndexRebuilt;
        return indexSafenodes.GetSafenodeIndex(vinSafenode);
    }

    void ClearOldSafenodeIndex() {
        LOCK(cs);
        indexSafenodesOld.Clear();
        fIndexRebuilt = false;
    }

    bool Has(const CTxIn& vin);

    safenode_info_t GetSafenodeInfo(const CTxIn& vin);

    safenode_info_t GetSafenodeInfo(const CPubKey& pubKeySafenode);

    /// Find an entry in the safenode list that is next to be paid
    CSafenode* GetNextSafenodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount);
    /// Same as above but use current block height
    CSafenode* GetNextSafenodeInQueueForPayment(bool fFilterSigTime, int& nCount);

    /// Find a random entry
    CSafenode* FindRandomNotInVec(const std::vector<CTxIn> &vecToExclude, int nProtocolVersion = -1);

    std::vector<CSafenode> GetFullSafenodeVector() { return vSafenodes; }

    std::vector<std::pair<int, CSafenode> > GetSafenodeRanks(int nBlockHeight = -1, int nMinProtocol=0);
    int GetSafenodeRank(const CTxIn &vin, int nBlockHeight, int nMinProtocol=0, bool fOnlyActive=true);
    CSafenode* GetSafenodeByRank(int nRank, int nBlockHeight, int nMinProtocol=0, bool fOnlyActive=true);

    void ProcessSafenodeConnections();
    std::pair<CService, std::set<uint256> > PopScheduledMnbRequestConnection();

    void ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

    void DoFullVerificationStep();
    void CheckSameAddr();
    bool SendVerifyRequest(const CAddress& addr, const std::vector<CSafenode*>& vSortedByAddr);
    void SendVerifyReply(CNode* pnode, CSafenodeVerification& mnv);
    void ProcessVerifyReply(CNode* pnode, CSafenodeVerification& mnv);
    void ProcessVerifyBroadcast(CNode* pnode, const CSafenodeVerification& mnv);

    /// Return the number of (unique) Safenodes
    int size() { return vSafenodes.size(); }

    std::string ToString() const;

    /// Update safenode list and maps using provided CSafenodeBroadcast
    void UpdateSafenodeList(CSafenodeBroadcast mnb);
    /// Perform complete check and only then update list and maps
    bool CheckMnbAndUpdateSafenodeList(CNode* pfrom, CSafenodeBroadcast mnb, int& nDos);
    bool IsMnbRecoveryRequested(const uint256& hash) { return mMnbRecoveryRequests.count(hash); }

    void UpdateLastPaid();

    void CheckAndRebuildSafenodeIndex();

    void AddDirtyGovernanceObjectHash(const uint256& nHash)
    {
        LOCK(cs);
        vecDirtyGovernanceObjectHashes.push_back(nHash);
    }

    std::vector<uint256> GetAndClearDirtyGovernanceObjectHashes()
    {
        LOCK(cs);
        std::vector<uint256> vecTmp = vecDirtyGovernanceObjectHashes;
        vecDirtyGovernanceObjectHashes.clear();
        return vecTmp;;
    }

    bool IsWatchdogActive();
    void UpdateWatchdogVoteTime(const CTxIn& vin);
    bool AddGovernanceVote(const CTxIn& vin, uint256 nGovernanceObjectHash);
    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    void CheckSafenode(const CTxIn& vin, bool fForce = false);
    void CheckSafenode(const CPubKey& pubKeySafenode, bool fForce = false);

    int GetSafenodeState(const CTxIn& vin);
    int GetSafenodeState(const CPubKey& pubKeySafenode);

    bool IsSafenodePingedWithin(const CTxIn& vin, int nSeconds, int64_t nTimeToCheckAt = -1);
    void SetSafenodeLastPing(const CTxIn& vin, const CSafenodePing& mnp);

    void UpdatedBlockTip(const CBlockIndex *pindex);

    /**
     * Called to notify CGovernanceManager that the safenode index has been updated.
     * Must be called while not holding the CSafenodeMan::cs mutex
     */
    void NotifySafenodeUpdates();

};

#endif
