// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2018 The SafeNode developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activesafenode.h"
#include "consensus/validation.h"
#include "darksend.h"
#include "init.h"
#include "governance.h"
#include "safenode.h"
#include "safenode-payments.h"
#include "safenode-sync.h"
#include "safenodeman.h"
#include "util.h"

#include <boost/lexical_cast.hpp>


CSafenode::CSafenode() :
    vin(),
    addr(),
    pubKeyCollateralAddress(),
    pubKeySafenode(),
    lastPing(),
    vchSig(),
    sigTime(GetAdjustedTime()),
    nLastDsq(0),
    nTimeLastChecked(0),
    nTimeLastPaid(0),
    nTimeLastWatchdogVote(0),
    nActiveState(SAFENODE_ENABLED),
    nCacheCollateralBlock(0),
    nBlockLastPaid(0),
    nProtocolVersion(PROTOCOL_VERSION),
    nPoSeBanScore(0),
    nPoSeBanHeight(0),
    fAllowMixingTx(true),
    fUnitTest(false)
{}

CSafenode::CSafenode(CService addrNew, CTxIn vinNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeySafenodeNew, int nProtocolVersionIn) :
    vin(vinNew),
    addr(addrNew),
    pubKeyCollateralAddress(pubKeyCollateralAddressNew),
    pubKeySafenode(pubKeySafenodeNew),
    lastPing(),
    vchSig(),
    sigTime(GetAdjustedTime()),
    nLastDsq(0),
    nTimeLastChecked(0),
    nTimeLastPaid(0),
    nTimeLastWatchdogVote(0),
    nActiveState(SAFENODE_ENABLED),
    nCacheCollateralBlock(0),
    nBlockLastPaid(0),
    nProtocolVersion(nProtocolVersionIn),
    nPoSeBanScore(0),
    nPoSeBanHeight(0),
    fAllowMixingTx(true),
    fUnitTest(false)
{}

CSafenode::CSafenode(const CSafenode& other) :
    vin(other.vin),
    addr(other.addr),
    pubKeyCollateralAddress(other.pubKeyCollateralAddress),
    pubKeySafenode(other.pubKeySafenode),
    lastPing(other.lastPing),
    vchSig(other.vchSig),
    sigTime(other.sigTime),
    nLastDsq(other.nLastDsq),
    nTimeLastChecked(other.nTimeLastChecked),
    nTimeLastPaid(other.nTimeLastPaid),
    nTimeLastWatchdogVote(other.nTimeLastWatchdogVote),
    nActiveState(other.nActiveState),
    nCacheCollateralBlock(other.nCacheCollateralBlock),
    nBlockLastPaid(other.nBlockLastPaid),
    nProtocolVersion(other.nProtocolVersion),
    nPoSeBanScore(other.nPoSeBanScore),
    nPoSeBanHeight(other.nPoSeBanHeight),
    fAllowMixingTx(other.fAllowMixingTx),
    fUnitTest(other.fUnitTest)
{}

CSafenode::CSafenode(const CSafenodeBroadcast& mnb) :
    vin(mnb.vin),
    addr(mnb.addr),
    pubKeyCollateralAddress(mnb.pubKeyCollateralAddress),
    pubKeySafenode(mnb.pubKeySafenode),
    lastPing(mnb.lastPing),
    vchSig(mnb.vchSig),
    sigTime(mnb.sigTime),
    nLastDsq(0),
    nTimeLastChecked(0),
    nTimeLastPaid(0),
    nTimeLastWatchdogVote(mnb.sigTime),
    nActiveState(mnb.nActiveState),
    nCacheCollateralBlock(0),
    nBlockLastPaid(0),
    nProtocolVersion(mnb.nProtocolVersion),
    nPoSeBanScore(0),
    nPoSeBanHeight(0),
    fAllowMixingTx(true),
    fUnitTest(false)
{}

//
// When a new safenode broadcast is sent, update our information
//
bool CSafenode::UpdateFromNewBroadcast(CSafenodeBroadcast& mnb)
{
    if(mnb.sigTime <= sigTime && !mnb.fRecovery) return false;

    pubKeySafenode = mnb.pubKeySafenode;
    sigTime = mnb.sigTime;
    vchSig = mnb.vchSig;
    nProtocolVersion = mnb.nProtocolVersion;
    addr = mnb.addr;
    nPoSeBanScore = 0;
    nPoSeBanHeight = 0;
    nTimeLastChecked = 0;
    int nDos = 0;
    if(mnb.lastPing == CSafenodePing() || (mnb.lastPing != CSafenodePing() && mnb.lastPing.CheckAndUpdate(this, true, nDos))) {
        lastPing = mnb.lastPing;
        mnodeman.mapSeenSafenodePing.insert(std::make_pair(lastPing.GetHash(), lastPing));
    }
    // if it matches our Safenode privkey...
    if(fSafeNode && pubKeySafenode == activeSafenode.pubKeySafenode) {
        nPoSeBanScore = -SAFENODE_POSE_BAN_MAX_SCORE;
        if(nProtocolVersion == PROTOCOL_VERSION) {
            // ... and PROTOCOL_VERSION, then we've been remotely activated ...
            activeSafenode.ManageState();
        } else {
            // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
            // but also do not ban the node we get this message from
            LogPrintf("CSafenode::UpdateFromNewBroadcast -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", nProtocolVersion, PROTOCOL_VERSION);
            return false;
        }
    }
    return true;
}

//
// Deterministically calculate a given "score" for a Safenode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
arith_uint256 CSafenode::CalculateScore(const uint256& blockHash)
{
    uint256 aux = ArithToUint256(UintToArith256(vin.prevout.hash) + vin.prevout.n);

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << blockHash;
    arith_uint256 hash2 = UintToArith256(ss.GetHash());

    CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
    ss2 << blockHash;
    ss2 << aux;
    arith_uint256 hash3 = UintToArith256(ss2.GetHash());

    return (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);
}

void CSafenode::Check(bool fForce)
{
    LOCK(cs);

    if(ShutdownRequested()) return;

    if(!fForce && (GetTime() - nTimeLastChecked < SAFENODE_CHECK_SECONDS)) return;
    nTimeLastChecked = GetTime();

    LogPrint("safenode", "CSafenode::Check -- Safenode %s is in %s state\n", vin.prevout.ToStringShort(), GetStateString());

    //once spent, stop doing the checks
    if(IsOutpointSpent()) return;

    int nHeight = 0;
    if(!fUnitTest) {
        TRY_LOCK(cs_main, lockMain);
        if(!lockMain) return;

        CCoins coins;
        if(!pcoinsTip->GetCoins(vin.prevout.hash, coins) ||
           (unsigned int)vin.prevout.n>=coins.vout.size() ||
           coins.vout[vin.prevout.n].IsNull()) {
            nActiveState = SAFENODE_OUTPOINT_SPENT;
            LogPrint("safenode", "CSafenode::Check -- Failed to find Safenode UTXO, safenode=%s\n", vin.prevout.ToStringShort());
            return;
        }

        nHeight = chainActive.Height();
    }

    if(IsPoSeBanned()) {
        if(nHeight < nPoSeBanHeight) return; // too early?
        // Otherwise give it a chance to proceed further to do all the usual checks and to change its state.
        // Safenode still will be on the edge and can be banned back easily if it keeps ignoring mnverify
        // or connect attempts. Will require few mnverify messages to strengthen its position in mn list.
        LogPrintf("CSafenode::Check -- Safenode %s is unbanned and back in list now\n", vin.prevout.ToStringShort());
        DecreasePoSeBanScore();
    } else if(nPoSeBanScore >= SAFENODE_POSE_BAN_MAX_SCORE) {
        nActiveState = SAFENODE_POSE_BAN;
        // ban for the whole payment cycle
        nPoSeBanHeight = nHeight + mnodeman.size();
        LogPrintf("CSafenode::Check -- Safenode %s is banned till block %d now\n", vin.prevout.ToStringShort(), nPoSeBanHeight);
        return;
    }

    int nActiveStatePrev = nActiveState;
    bool fOurSafenode = fSafeNode && activeSafenode.pubKeySafenode == pubKeySafenode;

                   // safenode doesn't meet payment protocol requirements ...
    bool fRequireUpdate = nProtocolVersion < mnpayments.GetMinSafenodePaymentsProto() ||
                   // or it's our own node and we just updated it to the new protocol but we are still waiting for activation ...
                   (fOurSafenode && nProtocolVersion < PROTOCOL_VERSION);

    if(fRequireUpdate) {
        nActiveState = SAFENODE_UPDATE_REQUIRED;
        if(nActiveStatePrev != nActiveState) {
            LogPrint("safenode", "CSafenode::Check -- Safenode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
        }
        return;
    }

    // keep old safenodes on start, give them a chance to receive updates...
    bool fWaitForPing = !safenodeSync.IsSafenodeListSynced() && !IsPingedWithin(SAFENODE_MIN_MNP_SECONDS);

    if(fWaitForPing && !fOurSafenode) {
        // ...but if it was already expired before the initial check - return right away
        if(IsExpired() || IsWatchdogExpired() || IsNewStartRequired()) {
            LogPrint("safenode", "CSafenode::Check -- Safenode %s is in %s state, waiting for ping\n", vin.prevout.ToStringShort(), GetStateString());
            return;
        }
    }

    // don't expire if we are still in "waiting for ping" mode unless it's our own safenode
    if(!fWaitForPing || fOurSafenode) {

        if(!IsPingedWithin(SAFENODE_NEW_START_REQUIRED_SECONDS)) {
            nActiveState = SAFENODE_NEW_START_REQUIRED;
            if(nActiveStatePrev != nActiveState) {
                LogPrint("safenode", "CSafenode::Check -- Safenode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }

        bool fWatchdogActive = safenodeSync.IsSynced() && mnodeman.IsWatchdogActive();
        bool fWatchdogExpired = (fWatchdogActive && ((GetTime() - nTimeLastWatchdogVote) > SAFENODE_WATCHDOG_MAX_SECONDS));

        LogPrint("safenode", "CSafenode::Check -- outpoint=%s, nTimeLastWatchdogVote=%d, GetTime()=%d, fWatchdogExpired=%d\n",
                vin.prevout.ToStringShort(), nTimeLastWatchdogVote, GetTime(), fWatchdogExpired);

        if(fWatchdogExpired) {
            nActiveState = SAFENODE_WATCHDOG_EXPIRED;
            if(nActiveStatePrev != nActiveState) {
                LogPrint("safenode", "CSafenode::Check -- Safenode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }

        if(!IsPingedWithin(SAFENODE_EXPIRATION_SECONDS)) {
            nActiveState = SAFENODE_EXPIRED;
            if(nActiveStatePrev != nActiveState) {
                LogPrint("safenode", "CSafenode::Check -- Safenode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
            }
            return;
        }
    }

    if(lastPing.sigTime - sigTime < SAFENODE_MIN_MNP_SECONDS) {
        nActiveState = SAFENODE_PRE_ENABLED;
        if(nActiveStatePrev != nActiveState) {
            LogPrint("safenode", "CSafenode::Check -- Safenode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
        }
        return;
    }

    nActiveState = SAFENODE_ENABLED; // OK
    if(nActiveStatePrev != nActiveState) {
        LogPrint("safenode", "CSafenode::Check -- Safenode %s is in %s state now\n", vin.prevout.ToStringShort(), GetStateString());
    }
}

bool CSafenode::IsValidNetAddr()
{
    return IsValidNetAddr(addr);
}

bool CSafenode::IsValidNetAddr(CService addrIn)
{
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().NetworkIDString() == CBaseChainParams::REGTEST ||
            (addrIn.IsIPv4() && IsReachable(addrIn) && addrIn.IsRoutable());
}

safenode_info_t CSafenode::GetInfo()
{
    safenode_info_t info;
    info.vin = vin;
    info.addr = addr;
    info.pubKeyCollateralAddress = pubKeyCollateralAddress;
    info.pubKeySafenode = pubKeySafenode;
    info.sigTime = sigTime;
    info.nLastDsq = nLastDsq;
    info.nTimeLastChecked = nTimeLastChecked;
    info.nTimeLastPaid = nTimeLastPaid;
    info.nTimeLastWatchdogVote = nTimeLastWatchdogVote;
    info.nTimeLastPing = lastPing.sigTime;
    info.nActiveState = nActiveState;
    info.nProtocolVersion = nProtocolVersion;
    info.fInfoValid = true;
    return info;
}

std::string CSafenode::StateToString(int nStateIn)
{
    switch(nStateIn) {
        case SAFENODE_PRE_ENABLED:            return "PRE_ENABLED";
        case SAFENODE_ENABLED:                return "ENABLED";
        case SAFENODE_EXPIRED:                return "EXPIRED";
        case SAFENODE_OUTPOINT_SPENT:         return "OUTPOINT_SPENT";
        case SAFENODE_UPDATE_REQUIRED:        return "UPDATE_REQUIRED";
        case SAFENODE_WATCHDOG_EXPIRED:       return "WATCHDOG_EXPIRED";
        case SAFENODE_NEW_START_REQUIRED:     return "NEW_START_REQUIRED";
        case SAFENODE_POSE_BAN:               return "POSE_BAN";
        default:                                return "UNKNOWN";
    }
}

std::string CSafenode::GetStateString() const
{
    return StateToString(nActiveState);
}

std::string CSafenode::GetStatus() const
{
    // TODO: return smth a bit more human readable here
    return GetStateString();
}

int CSafenode::GetCollateralAge()
{
    int nHeight;
    {
        TRY_LOCK(cs_main, lockMain);
        if(!lockMain || !chainActive.Tip()) return -1;
        nHeight = chainActive.Height();
    }

    if (nCacheCollateralBlock == 0) {
        int nInputAge = GetInputAge(vin);
        if(nInputAge > 0) {
            nCacheCollateralBlock = nHeight - nInputAge;
        } else {
            return nInputAge;
        }
    }

    return nHeight - nCacheCollateralBlock;
}

void CSafenode::UpdateLastPaid(const CBlockIndex *pindex, int nMaxBlocksToScanBack)
{
    if(!pindex) return;

    const CBlockIndex *BlockReading = pindex;

    CScript mnpayee = GetScriptForDestination(pubKeyCollateralAddress.GetID());
    // LogPrint("safenode", "CSafenode::UpdateLastPaidBlock -- searching for block with payment to %s\n", vin.prevout.ToStringShort());

    LOCK(cs_mapSafenodeBlocks);

    for (int i = 0; BlockReading && BlockReading->nHeight > nBlockLastPaid && i < nMaxBlocksToScanBack; i++) {
        if(mnpayments.mapSafenodeBlocks.count(BlockReading->nHeight) &&
            mnpayments.mapSafenodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2))
        {
            CBlock block;
            if(!ReadBlockFromDisk(block, BlockReading, Params().GetConsensus())) // shouldn't really happen
                continue;

            CAmount nSafenodePayment = GetSafenodePayment(BlockReading->nHeight, block.vtx[0].GetValueOut());

            BOOST_FOREACH(CTxOut txout, block.vtx[0].vout)
                if(mnpayee == txout.scriptPubKey && nSafenodePayment == txout.nValue) {
                    nBlockLastPaid = BlockReading->nHeight;
                    nTimeLastPaid = BlockReading->nTime;
                    LogPrint("safenode", "CSafenode::UpdateLastPaidBlock -- searching for block with payment to %s -- found new %d\n", vin.prevout.ToStringShort(), nBlockLastPaid);
                    return;
                }
        }

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    // Last payment for this safenode wasn't found in latest mnpayments blocks
    // or it was found in mnpayments blocks but wasn't found in the blockchain.
    // LogPrint("safenode", "CSafenode::UpdateLastPaidBlock -- searching for block with payment to %s -- keeping old %d\n", vin.prevout.ToStringShort(), nBlockLastPaid);
}

bool CSafenodeBroadcast::Create(std::string strService, std::string strKeySafenode, std::string strTxHash, std::string strOutputIndex, std::string& strErrorRet, CSafenodeBroadcast &mnbRet, bool fOffline)
{
    CTxIn txin;
    CPubKey pubKeyCollateralAddressNew;
    CKey keyCollateralAddressNew;
    CPubKey pubKeySafenodeNew;
    CKey keySafenodeNew;

    //need correct blocks to send ping
    if(!fOffline && !safenodeSync.IsBlockchainSynced()) {
        strErrorRet = "Sync in progress. Must wait until sync is complete to start Safenode";
        LogPrintf("CSafenodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if(!darkSendSigner.GetKeysFromSecret(strKeySafenode, keySafenodeNew, pubKeySafenodeNew)) {
        strErrorRet = strprintf("Invalid safenode key %s", strKeySafenode);
        LogPrintf("CSafenodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if(!pwalletMain->GetSafenodeVinAndKeys(txin, pubKeyCollateralAddressNew, keyCollateralAddressNew, strTxHash, strOutputIndex)) {
        strErrorRet = strprintf("Could not allocate txin %s:%s for safenode %s", strTxHash, strOutputIndex, strService);
        LogPrintf("CSafenodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    CService service = CService(strService);
    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(service.GetPort() != mainnetDefaultPort) {
            strErrorRet = strprintf("Invalid port %u for safenode %s, only %d is supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort);
            LogPrintf("CSafenodeBroadcast::Create -- %s\n", strErrorRet);
            return false;
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        strErrorRet = strprintf("Invalid port %u for safenode %s, %d is the only supported on mainnet.", service.GetPort(), strService, mainnetDefaultPort);
        LogPrintf("CSafenodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    return Create(txin, CService(strService), keyCollateralAddressNew, pubKeyCollateralAddressNew, keySafenodeNew, pubKeySafenodeNew, strErrorRet, mnbRet);
}

bool CSafenodeBroadcast::Create(CTxIn txin, CService service, CKey keyCollateralAddressNew, CPubKey pubKeyCollateralAddressNew, CKey keySafenodeNew, CPubKey pubKeySafenodeNew, std::string &strErrorRet, CSafenodeBroadcast &mnbRet)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    LogPrint("safenode", "CSafenodeBroadcast::Create -- pubKeyCollateralAddressNew = %s, pubKeySafenodeNew.GetID() = %s\n",
             CBitcoinAddress(pubKeyCollateralAddressNew.GetID()).ToString(),
             pubKeySafenodeNew.GetID().ToString());


    CSafenodePing mnp(txin);
    if(!mnp.Sign(keySafenodeNew, pubKeySafenodeNew)) {
        strErrorRet = strprintf("Failed to sign ping, safenode=%s", txin.prevout.ToStringShort());
        LogPrintf("CSafenodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CSafenodeBroadcast();
        return false;
    }

    mnbRet = CSafenodeBroadcast(service, txin, pubKeyCollateralAddressNew, pubKeySafenodeNew, PROTOCOL_VERSION);

    if(!mnbRet.IsValidNetAddr()) {
        strErrorRet = strprintf("Invalid IP address, safenode=%s", txin.prevout.ToStringShort());
        LogPrintf("CSafenodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CSafenodeBroadcast();
        return false;
    }

    mnbRet.lastPing = mnp;
    if(!mnbRet.Sign(keyCollateralAddressNew)) {
        strErrorRet = strprintf("Failed to sign broadcast, safenode=%s", txin.prevout.ToStringShort());
        LogPrintf("CSafenodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CSafenodeBroadcast();
        return false;
    }

    return true;
}

bool CSafenodeBroadcast::SimpleCheck(int& nDos)
{
    nDos = 0;

    // make sure addr is valid
    if(!IsValidNetAddr()) {
        LogPrintf("CSafenodeBroadcast::SimpleCheck -- Invalid addr, rejected: safenode=%s  addr=%s\n",
                    vin.prevout.ToStringShort(), addr.ToString());
        return false;
    }

    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CSafenodeBroadcast::SimpleCheck -- Signature rejected, too far into the future: safenode=%s\n", vin.prevout.ToStringShort());
        nDos = 1;
        return false;
    }

    // empty ping or incorrect sigTime/unknown blockhash
    if(lastPing == CSafenodePing() || !lastPing.SimpleCheck(nDos)) {
        // one of us is probably forked or smth, just mark it as expired and check the rest of the rules
        nActiveState = SAFENODE_EXPIRED;
    }

    if(nProtocolVersion < mnpayments.GetMinSafenodePaymentsProto()) {
        LogPrintf("CSafenodeBroadcast::SimpleCheck -- ignoring outdated Safenode: safenode=%s  nProtocolVersion=%d\n", vin.prevout.ToStringShort(), nProtocolVersion);
        return false;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    if(pubkeyScript.size() != 25) {
        LogPrintf("CSafenodeBroadcast::SimpleCheck -- pubKeyCollateralAddress has the wrong size\n");
        nDos = 100;
        return false;
    }

    CScript pubkeyScript2;
    pubkeyScript2 = GetScriptForDestination(pubKeySafenode.GetID());

    if(pubkeyScript2.size() != 25) {
        LogPrintf("CSafenodeBroadcast::SimpleCheck -- pubKeySafenode has the wrong size\n");
        nDos = 100;
        return false;
    }

    if(!vin.scriptSig.empty()) {
        LogPrintf("CSafenodeBroadcast::SimpleCheck -- Ignore Not Empty ScriptSig %s\n",vin.ToString());
        nDos = 100;
        return false;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(addr.GetPort() != mainnetDefaultPort) return false;
    } else if(addr.GetPort() == mainnetDefaultPort) return false;

    return true;
}

bool CSafenodeBroadcast::Update(CSafenode* pmn, int& nDos)
{
    nDos = 0;

    if(pmn->sigTime == sigTime && !fRecovery) {
        // mapSeenSafenodeBroadcast in CSafenodeMan::CheckMnbAndUpdateSafenodeList should filter legit duplicates
        // but this still can happen if we just started, which is ok, just do nothing here.
        return false;
    }

    // this broadcast is older than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    if(pmn->sigTime > sigTime) {
        LogPrintf("CSafenodeBroadcast::Update -- Bad sigTime %d (existing broadcast is at %d) for Safenode %s %s\n",
                      sigTime, pmn->sigTime, vin.prevout.ToStringShort(), addr.ToString());
        return false;
    }

    pmn->Check();

    // safenode is banned by PoSe
    if(pmn->IsPoSeBanned()) {
        LogPrintf("CSafenodeBroadcast::Update -- Banned by PoSe, safenode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    // IsVnAssociatedWithPubkey is validated once in CheckOutpoint, after that they just need to match
    if(pmn->pubKeyCollateralAddress != pubKeyCollateralAddress) {
        LogPrintf("CSafenodeBroadcast::Update -- Got mismatched pubKeyCollateralAddress and vin\n");
        nDos = 33;
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CSafenodeBroadcast::Update -- CheckSignature() failed, safenode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    // if ther was no safenode broadcast recently or if it matches our Safenode privkey...
    if(!pmn->IsBroadcastedWithin(SAFENODE_MIN_MNB_SECONDS) || (fSafeNode && pubKeySafenode == activeSafenode.pubKeySafenode)) {
        // take the newest entry
        LogPrintf("CSafenodeBroadcast::Update -- Got UPDATED Safenode entry: addr=%s\n", addr.ToString());
        if(pmn->UpdateFromNewBroadcast((*this))) {
            pmn->Check();
            Relay();
        }
        safenodeSync.AddedSafenodeList();
    }

    return true;
}

bool CSafenodeBroadcast::CheckOutpoint(int& nDos)
{
    // we are a safenode with the same vin (i.e. already activated) and this mnb is ours (matches our Safenode privkey)
    // so nothing to do here for us
    if(fSafeNode && vin.prevout == activeSafenode.vin.prevout && pubKeySafenode == activeSafenode.pubKeySafenode) {
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CSafenodeBroadcast::CheckOutpoint -- CheckSignature() failed, safenode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    {
        TRY_LOCK(cs_main, lockMain);
        if(!lockMain) {
            // not mnb fault, let it to be checked again later
            LogPrint("safenode", "CSafenodeBroadcast::CheckOutpoint -- Failed to aquire lock, addr=%s", addr.ToString());
            mnodeman.mapSeenSafenodeBroadcast.erase(GetHash());
            return false;
        }

        CCoins coins;
        if(!pcoinsTip->GetCoins(vin.prevout.hash, coins) ||
           (unsigned int)vin.prevout.n>=coins.vout.size() ||
           coins.vout[vin.prevout.n].IsNull()) {
            LogPrint("safenode", "CSafenodeBroadcast::CheckOutpoint -- Failed to find Safenode UTXO, safenode=%s\n", vin.prevout.ToStringShort());
            return false;
        }

        if(coins.vout[vin.prevout.n].nValue != 2500 * COIN) {
            LogPrint("safenode", "CSafenodeBroadcast::CheckOutpoint -- Safenode UTXO should have 2500 SXN, safenode=%s\n", vin.prevout.ToStringShort());
            return false;
        }

        if(chainActive.Height() - coins.nHeight + 1 < Params().GetConsensus().nSafenodeMinimumConfirmations) {
            LogPrintf("CSafenodeBroadcast::CheckOutpoint -- Safenode UTXO must have at least %d confirmations, safenode=%s\n",
                    Params().GetConsensus().nSafenodeMinimumConfirmations, vin.prevout.ToStringShort());
            // maybe we miss few blocks, let this mnb to be checked again later
            mnodeman.mapSeenSafenodeBroadcast.erase(GetHash());
            return false;
        }
    }

    LogPrint("safenode", "CSafenodeBroadcast::CheckOutpoint -- Safenode UTXO verified\n");

    // make sure the vout that was signed is related to the transaction that spawned the Safenode
    //  - this is expensive, so it's only done once per Safenode
    if(!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubKeyCollateralAddress)) {
        LogPrintf("CSafenodeMan::CheckOutpoint -- Got mismatched pubKeyCollateralAddress and vin\n");
        nDos = 33;
        return false;
    }

    // verify that sig time is legit in past
    // should be at least not earlier than block when 2500 SXN tx got nSafenodeMinimumConfirmations
    uint256 hashBlock = uint256();
    CTransaction tx2;
    GetTransaction(vin.prevout.hash, tx2, Params().GetConsensus(), hashBlock, true);
    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex* pMNIndex = (*mi).second; // block for 2500 SXN tx -> 1 confirmation
            CBlockIndex* pConfIndex = chainActive[pMNIndex->nHeight + Params().GetConsensus().nSafenodeMinimumConfirmations - 1]; // block where tx got nSafenodeMinimumConfirmations
            if(pConfIndex->GetBlockTime() > sigTime) {
                LogPrintf("CSafenodeBroadcast::CheckOutpoint -- Bad sigTime %d (%d conf block is at %d) for Safenode %s %s\n",
                          sigTime, Params().GetConsensus().nSafenodeMinimumConfirmations, pConfIndex->GetBlockTime(), vin.prevout.ToStringShort(), addr.ToString());
                return false;
            }
        }
    }

    return true;
}

bool CSafenodeBroadcast::Sign(CKey& keyCollateralAddress)
{
    std::string strError;
    std::string strMessage;

    sigTime = GetAdjustedTime();

    strMessage = addr.ToString(false) + boost::lexical_cast<std::string>(sigTime) +
                    pubKeyCollateralAddress.GetID().ToString() + pubKeySafenode.GetID().ToString() +
                    boost::lexical_cast<std::string>(nProtocolVersion);

    if(!darkSendSigner.SignMessage(strMessage, vchSig, keyCollateralAddress)) {
        LogPrintf("CSafenodeBroadcast::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!darkSendSigner.VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)) {
        LogPrintf("CSafenodeBroadcast::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CSafenodeBroadcast::CheckSignature(int& nDos)
{
    std::string strMessage;
    std::string strError = "";
    nDos = 0;

    strMessage = addr.ToString(false) + boost::lexical_cast<std::string>(sigTime) +
                    pubKeyCollateralAddress.GetID().ToString() + pubKeySafenode.GetID().ToString() +
                    boost::lexical_cast<std::string>(nProtocolVersion);

    LogPrint("safenode", "CSafenodeBroadcast::CheckSignature -- strMessage: %s  pubKeyCollateralAddress address: %s  sig: %s\n", strMessage, CBitcoinAddress(pubKeyCollateralAddress.GetID()).ToString(), EncodeBase64(&vchSig[0], vchSig.size()));

    if(!darkSendSigner.VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError)){
        LogPrintf("CSafenodeBroadcast::CheckSignature -- Got bad Safenode announce signature, error: %s\n", strError);
        nDos = 100;
        return false;
    }

    return true;
}

void CSafenodeBroadcast::Relay()
{
    CInv inv(MSG_SAFENODE_ANNOUNCE, GetHash());
    RelayInv(inv);
}

CSafenodePing::CSafenodePing(CTxIn& vinNew)
{
    LOCK(cs_main);
    if (!chainActive.Tip() || chainActive.Height() < 12) return;

    vin = vinNew;
    blockHash = chainActive[chainActive.Height() - 12]->GetBlockHash();
    sigTime = GetAdjustedTime();
    vchSig = std::vector<unsigned char>();
}

bool CSafenodePing::Sign(CKey& keySafenode, CPubKey& pubKeySafenode)
{
    std::string strError;
    std::string strSafeNodeSignMessage;

    sigTime = GetAdjustedTime();
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);

    if(!darkSendSigner.SignMessage(strMessage, vchSig, keySafenode)) {
        LogPrintf("CSafenodePing::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!darkSendSigner.VerifyMessage(pubKeySafenode, vchSig, strMessage, strError)) {
        LogPrintf("CSafenodePing::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CSafenodePing::CheckSignature(CPubKey& pubKeySafenode, int &nDos)
{
    std::string strMessage = vin.ToString() + blockHash.ToString() + boost::lexical_cast<std::string>(sigTime);
    std::string strError = "";
    nDos = 0;

    if(!darkSendSigner.VerifyMessage(pubKeySafenode, vchSig, strMessage, strError)) {
        LogPrintf("CSafenodePing::CheckSignature -- Got bad Safenode ping signature, safenode=%s, error: %s\n", vin.prevout.ToStringShort(), strError);
        nDos = 33;
        return false;
    }
    return true;
}

bool CSafenodePing::SimpleCheck(int& nDos)
{
    // don't ban by default
    nDos = 0;

    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CSafenodePing::SimpleCheck -- Signature rejected, too far into the future, safenode=%s\n", vin.prevout.ToStringShort());
        nDos = 1;
        return false;
    }

    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if (mi == mapBlockIndex.end()) {
            LogPrint("safenode", "CSafenodePing::SimpleCheck -- Safenode ping is invalid, unknown block hash: safenode=%s blockHash=%s\n", vin.prevout.ToStringShort(), blockHash.ToString());
            // maybe we stuck or forked so we shouldn't ban this node, just fail to accept this ping
            // TODO: or should we also request this block?
            return false;
        }
    }
    LogPrint("safenode", "CSafenodePing::SimpleCheck -- Safenode ping verified: safenode=%s  blockHash=%s  sigTime=%d\n", vin.prevout.ToStringShort(), blockHash.ToString(), sigTime);
    return true;
}

bool CSafenodePing::CheckAndUpdate(CSafenode* pmn, bool fFromNewBroadcast, int& nDos)
{
    // don't ban by default
    nDos = 0;

    if (!SimpleCheck(nDos)) {
        return false;
    }

    if (pmn == NULL) {
        LogPrint("safenode", "CSafenodePing::CheckAndUpdate -- Couldn't find Safenode entry, safenode=%s\n", vin.prevout.ToStringShort());
        return false;
    }

    if(!fFromNewBroadcast) {
        if (pmn->IsUpdateRequired()) {
            LogPrint("safenode", "CSafenodePing::CheckAndUpdate -- safenode protocol is outdated, safenode=%s\n", vin.prevout.ToStringShort());
            return false;
        }

        if (pmn->IsNewStartRequired()) {
            LogPrint("safenode", "CSafenodePing::CheckAndUpdate -- safenode is completely expired, new start is required, safenode=%s\n", vin.prevout.ToStringShort());
            return false;
        }
    }

    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if ((*mi).second && (*mi).second->nHeight < chainActive.Height() - 24) {
            LogPrintf("CSafenodePing::CheckAndUpdate -- Safenode ping is invalid, block hash is too old: safenode=%s  blockHash=%s\n", vin.prevout.ToStringShort(), blockHash.ToString());
            // nDos = 1;
            return false;
        }
    }

    LogPrint("safenode", "CSafenodePing::CheckAndUpdate -- New ping: safenode=%s  blockHash=%s  sigTime=%d\n", vin.prevout.ToStringShort(), blockHash.ToString(), sigTime);

    // LogPrintf("mnping - Found corresponding mn for vin: %s\n", vin.prevout.ToStringShort());
    // update only if there is no known ping for this safenode or
    // last ping was more then SAFENODE_MIN_MNP_SECONDS-60 ago comparing to this one
    if (pmn->IsPingedWithin(SAFENODE_MIN_MNP_SECONDS - 60, sigTime)) {
        LogPrint("safenode", "CSafenodePing::CheckAndUpdate -- Safenode ping arrived too early, safenode=%s\n", vin.prevout.ToStringShort());
        //nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }

    if (!CheckSignature(pmn->pubKeySafenode, nDos)) return false;

    // so, ping seems to be ok

    // if we are still syncing and there was no known ping for this mn for quite a while
    // (NOTE: assuming that SAFENODE_EXPIRATION_SECONDS/2 should be enough to finish mn list sync)
    if(!safenodeSync.IsSafenodeListSynced() && !pmn->IsPingedWithin(SAFENODE_EXPIRATION_SECONDS/2)) {
        // let's bump sync timeout
        LogPrint("safenode", "CSafenodePing::CheckAndUpdate -- bumping sync timeout, safenode=%s\n", vin.prevout.ToStringShort());
        safenodeSync.AddedSafenodeList();
    }

    // let's store this ping as the last one
    LogPrint("safenode", "CSafenodePing::CheckAndUpdate -- Safenode ping accepted, safenode=%s\n", vin.prevout.ToStringShort());
    pmn->lastPing = *this;

    // and update mnodeman.mapSeenSafenodeBroadcast.lastPing which is probably outdated
    CSafenodeBroadcast mnb(*pmn);
    uint256 hash = mnb.GetHash();
    if (mnodeman.mapSeenSafenodeBroadcast.count(hash)) {
        mnodeman.mapSeenSafenodeBroadcast[hash].second.lastPing = *this;
    }

    pmn->Check(true); // force update, ignoring cache
    if (!pmn->IsEnabled()) return false;

    LogPrint("safenode", "CSafenodePing::CheckAndUpdate -- Safenode ping acceepted and relayed, safenode=%s\n", vin.prevout.ToStringShort());
    Relay();

    return true;
}

void CSafenodePing::Relay()
{
    CInv inv(MSG_SAFENODE_PING, GetHash());
    RelayInv(inv);
}

void CSafenode::AddGovernanceVote(uint256 nGovernanceObjectHash)
{
    if(mapGovernanceObjectsVotedOn.count(nGovernanceObjectHash)) {
        mapGovernanceObjectsVotedOn[nGovernanceObjectHash]++;
    } else {
        mapGovernanceObjectsVotedOn.insert(std::make_pair(nGovernanceObjectHash, 1));
    }
}

void CSafenode::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
{
    std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.find(nGovernanceObjectHash);
    if(it == mapGovernanceObjectsVotedOn.end()) {
        return;
    }
    mapGovernanceObjectsVotedOn.erase(it);
}

void CSafenode::UpdateWatchdogVoteTime()
{
    LOCK(cs);
    nTimeLastWatchdogVote = GetTime();
}

/**
*   FLAG GOVERNANCE ITEMS AS DIRTY
*
*   - When safenode come and go on the network, we must flag the items they voted on to recalc it's cached flags
*
*/
void CSafenode::FlagGovernanceItemsAsDirty()
{
    std::vector<uint256> vecDirty;
    {
        std::map<uint256, int>::iterator it = mapGovernanceObjectsVotedOn.begin();
        while(it != mapGovernanceObjectsVotedOn.end()) {
            vecDirty.push_back(it->first);
            ++it;
        }
    }
    for(size_t i = 0; i < vecDirty.size(); ++i) {
        mnodeman.AddDirtyGovernanceObjectHash(vecDirty[i]);
    }
}
