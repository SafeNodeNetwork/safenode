// Copyright (c) 2014-2018 The SafeNode developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activesafenode.h"
#include "safenode.h"
#include "safenode-sync.h"
#include "safenodeman.h"
#include "protocol.h"

extern CWallet* pwalletMain;

// Keep track of the active Safenode
CActiveSafenode activeSafenode;

void CActiveSafenode::ManageState()
{
    LogPrint("safenode", "CActiveSafenode::ManageState -- Start\n");
    if(!fSafeNode) {
        LogPrint("safenode", "CActiveSafenode::ManageState -- Not a safenode, returning\n");
        return;
    }

    if(Params().NetworkIDString() != CBaseChainParams::REGTEST && !safenodeSync.IsBlockchainSynced()) {
        nState = ACTIVE_SAFENODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveSafenode::ManageState -- %s: %s\n", GetStateString(), GetStatus());
        return;
    }

    if(nState == ACTIVE_SAFENODE_SYNC_IN_PROCESS) {
        nState = ACTIVE_SAFENODE_INITIAL;
    }

    LogPrint("safenode", "CActiveSafenode::ManageState -- status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);

    if(eType == SAFENODE_UNKNOWN) {
        ManageStateInitial();
    }

    if(eType == SAFENODE_REMOTE) {
        ManageStateRemote();
    } else if(eType == SAFENODE_LOCAL) {
        // Try Remote Start first so the started local safenode can be restarted without recreate safenode broadcast.
        ManageStateRemote();
        if(nState != ACTIVE_SAFENODE_STARTED)
            ManageStateLocal();
    }

    SendSafenodePing();
}

std::string CActiveSafenode::GetStateString() const
{
    switch (nState) {
        case ACTIVE_SAFENODE_INITIAL:         return "INITIAL";
        case ACTIVE_SAFENODE_SYNC_IN_PROCESS: return "SYNC_IN_PROCESS";
        case ACTIVE_SAFENODE_INPUT_TOO_NEW:   return "INPUT_TOO_NEW";
        case ACTIVE_SAFENODE_NOT_CAPABLE:     return "NOT_CAPABLE";
        case ACTIVE_SAFENODE_STARTED:         return "STARTED";
        default:                                return "UNKNOWN";
    }
}

std::string CActiveSafenode::GetStatus() const
{
    switch (nState) {
        case ACTIVE_SAFENODE_INITIAL:         return "Node just started, not yet activated";
        case ACTIVE_SAFENODE_SYNC_IN_PROCESS: return "Sync in progress. Must wait until sync is complete to start Safenode";
        case ACTIVE_SAFENODE_INPUT_TOO_NEW:   return strprintf("Safenode input must have at least %d confirmations", Params().GetConsensus().nSafenodeMinimumConfirmations);
        case ACTIVE_SAFENODE_NOT_CAPABLE:     return "Not capable safenode: " + strNotCapableReason;
        case ACTIVE_SAFENODE_STARTED:         return "Safenode successfully started";
        default:                                return "Unknown";
    }
}

std::string CActiveSafenode::GetTypeString() const
{
    std::string strType;
    switch(eType) {
    case SAFENODE_UNKNOWN:
        strType = "UNKNOWN";
        break;
    case SAFENODE_REMOTE:
        strType = "REMOTE";
        break;
    case SAFENODE_LOCAL:
        strType = "LOCAL";
        break;
    default:
        strType = "UNKNOWN";
        break;
    }
    return strType;
}

bool CActiveSafenode::SendSafenodePing()
{
    if(!fPingerEnabled) {
        LogPrint("safenode", "CActiveSafenode::SendSafenodePing -- %s: safenode ping service is disabled, skipping...\n", GetStateString());
        return false;
    }

    if(!mnodeman.Has(vin)) {
        strNotCapableReason = "Safenode not in safenode list";
        nState = ACTIVE_SAFENODE_NOT_CAPABLE;
        LogPrintf("CActiveSafenode::SendSafenodePing -- %s: %s\n", GetStateString(), strNotCapableReason);
        return false;
    }

    CSafenodePing mnp(vin);
    if(!mnp.Sign(keySafenode, pubKeySafenode)) {
        LogPrintf("CActiveSafenode::SendSafenodePing -- ERROR: Couldn't sign Safenode Ping\n");
        return false;
    }

    // Update lastPing for our safenode in Safenode list
    if(mnodeman.IsSafenodePingedWithin(vin, SAFENODE_MIN_MNP_SECONDS, mnp.sigTime)) {
        LogPrintf("CActiveSafenode::SendSafenodePing -- Too early to send Safenode Ping\n");
        return false;
    }

    mnodeman.SetSafenodeLastPing(vin, mnp);

    LogPrintf("CActiveSafenode::SendSafenodePing -- Relaying ping, collateral=%s\n", vin.ToString());
    mnp.Relay();

    return true;
}

void CActiveSafenode::ManageStateInitial()
{
    LogPrint("safenode", "CActiveSafenode::ManageStateInitial -- status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);

    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        nState = ACTIVE_SAFENODE_NOT_CAPABLE;
        strNotCapableReason = "Safenode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("CActiveSafenode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    bool fFoundLocal = false;
    {
        LOCK(cs_vNodes);

        // First try to find whatever local address is specified by externalip option
        fFoundLocal = GetLocal(service) && CSafenode::IsValidNetAddr(service);
        if(!fFoundLocal) {
            // nothing and no live connections, can't do anything for now
            if (vNodes.empty()) {
                nState = ACTIVE_SAFENODE_NOT_CAPABLE;
                strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
                LogPrintf("CActiveSafenode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
                return;
            }
            // We have some peers, let's try to find our local address from one of them
            BOOST_FOREACH(CNode* pnode, vNodes) {
                if (pnode->fSuccessfullyConnected && pnode->addr.IsIPv4()) {
                    fFoundLocal = GetLocal(service, &pnode->addr) && CSafenode::IsValidNetAddr(service);
                    if(fFoundLocal) break;
                }
            }
        }
    }

    if(!fFoundLocal) {
        nState = ACTIVE_SAFENODE_NOT_CAPABLE;
        strNotCapableReason = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("CActiveSafenode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if(service.GetPort() != mainnetDefaultPort) {
            nState = ACTIVE_SAFENODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Invalid port: %u - only %d is supported on mainnet.", service.GetPort(), mainnetDefaultPort);
            LogPrintf("CActiveSafenode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    } else if(service.GetPort() == mainnetDefaultPort) {
        nState = ACTIVE_SAFENODE_NOT_CAPABLE;
        strNotCapableReason = strprintf("Invalid port: %u - %d is only supported on mainnet.", service.GetPort(), mainnetDefaultPort);
        LogPrintf("CActiveSafenode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    LogPrintf("CActiveSafenode::ManageStateInitial -- Checking inbound connection to '%s'\n", service.ToString());

    if(!ConnectNode((CAddress)service, NULL, true)) {
        nState = ACTIVE_SAFENODE_NOT_CAPABLE;
        strNotCapableReason = "Could not connect to " + service.ToString();
        LogPrintf("CActiveSafenode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Default to REMOTE
    eType = SAFENODE_REMOTE;

    // Check if wallet funds are available
    if(!pwalletMain) {
        LogPrintf("CActiveSafenode::ManageStateInitial -- %s: Wallet not available\n", GetStateString());
        return;
    }

    if(pwalletMain->IsLocked()) {
        LogPrintf("CActiveSafenode::ManageStateInitial -- %s: Wallet is locked\n", GetStateString());
        return;
    }

    if(pwalletMain->GetBalance() < 10000*COIN) {
        LogPrintf("CActiveSafenode::ManageStateInitial -- %s: Wallet balance is < 10000 SXN\n", GetStateString());
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    // If collateral is found switch to LOCAL mode
    if(pwalletMain->GetSafenodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        eType = SAFENODE_LOCAL;
    }

    LogPrint("safenode", "CActiveSafenode::ManageStateInitial -- End status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);
}

void CActiveSafenode::ManageStateRemote()
{
    LogPrint("safenode", "CActiveSafenode::ManageStateRemote -- Start status = %s, type = %s, pinger enabled = %d, pubKeySafenode.GetID() = %s\n",
             GetStatus(), fPingerEnabled, GetTypeString(), pubKeySafenode.GetID().ToString());

    mnodeman.CheckSafenode(pubKeySafenode);
    safenode_info_t infoMn = mnodeman.GetSafenodeInfo(pubKeySafenode);
    if(infoMn.fInfoValid) {
        if(infoMn.nProtocolVersion != PROTOCOL_VERSION) {
            nState = ACTIVE_SAFENODE_NOT_CAPABLE;
            strNotCapableReason = "Invalid protocol version";
            LogPrintf("CActiveSafenode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(service != infoMn.addr) {
            nState = ACTIVE_SAFENODE_NOT_CAPABLE;
            strNotCapableReason = "Broadcasted IP doesn't match our external address. Make sure you issued a new broadcast if IP of this safenode changed recently.";
            LogPrintf("CActiveSafenode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(!CSafenode::IsValidStateForAutoStart(infoMn.nActiveState)) {
            nState = ACTIVE_SAFENODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Safenode in %s state", CSafenode::StateToString(infoMn.nActiveState));
            LogPrintf("CActiveSafenode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if(nState != ACTIVE_SAFENODE_STARTED) {
            LogPrintf("CActiveSafenode::ManageStateRemote -- STARTED!\n");
            vin = infoMn.vin;
            service = infoMn.addr;
            fPingerEnabled = true;
            nState = ACTIVE_SAFENODE_STARTED;
        }
    }
    else {
        nState = ACTIVE_SAFENODE_NOT_CAPABLE;
        strNotCapableReason = "Safenode not in safenode list";
        LogPrintf("CActiveSafenode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
    }
}

void CActiveSafenode::ManageStateLocal()
{
    LogPrint("safenode", "CActiveSafenode::ManageStateLocal -- status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);
    if(nState == ACTIVE_SAFENODE_STARTED) {
        return;
    }

    // Choose coins to use
    CPubKey pubKeyCollateral;
    CKey keyCollateral;

    if(pwalletMain->GetSafenodeVinAndKeys(vin, pubKeyCollateral, keyCollateral)) {
        int nInputAge = GetInputAge(vin);
        if(nInputAge < Params().GetConsensus().nSafenodeMinimumConfirmations){
            nState = ACTIVE_SAFENODE_INPUT_TOO_NEW;
            strNotCapableReason = strprintf(_("%s - %d confirmations"), GetStatus(), nInputAge);
            LogPrintf("CActiveSafenode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        {
            LOCK(pwalletMain->cs_wallet);
            pwalletMain->LockCoin(vin.prevout);
        }

        CSafenodeBroadcast mnb;
        std::string strError;
        if(!CSafenodeBroadcast::Create(vin, service, keyCollateral, pubKeyCollateral, keySafenode, pubKeySafenode, strError, mnb)) {
            nState = ACTIVE_SAFENODE_NOT_CAPABLE;
            strNotCapableReason = "Error creating mastenode broadcast: " + strError;
            LogPrintf("CActiveSafenode::ManageStateLocal -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }

        fPingerEnabled = true;
        nState = ACTIVE_SAFENODE_STARTED;

        //update to safenode list
        LogPrintf("CActiveSafenode::ManageStateLocal -- Update Safenode List\n");
        mnodeman.UpdateSafenodeList(mnb);
        mnodeman.NotifySafenodeUpdates();

        //send to all peers
        LogPrintf("CActiveSafenode::ManageStateLocal -- Relay broadcast, vin=%s\n", vin.ToString());
        mnb.Relay();
    }
}
