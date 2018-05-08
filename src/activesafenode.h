// Copyright (c) 2014-2018 The SafeNode developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ACTIVESAFENODE_H
#define ACTIVESAFENODE_H

#include "net.h"
#include "key.h"
#include "wallet/wallet.h"

class CActiveSafenode;

static const int ACTIVE_SAFENODE_INITIAL          = 0; // initial state
static const int ACTIVE_SAFENODE_SYNC_IN_PROCESS  = 1;
static const int ACTIVE_SAFENODE_INPUT_TOO_NEW    = 2;
static const int ACTIVE_SAFENODE_NOT_CAPABLE      = 3;
static const int ACTIVE_SAFENODE_STARTED          = 4;

extern CActiveSafenode activeSafenode;

// Responsible for activating the Safenode and pinging the network
class CActiveSafenode
{
public:
    enum safenode_type_enum_t {
        SAFENODE_UNKNOWN = 0,
        SAFENODE_REMOTE  = 1,
        SAFENODE_LOCAL   = 2
    };

private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    safenode_type_enum_t eType;

    bool fPingerEnabled;

    /// Ping Safenode
    bool SendSafenodePing();

public:
    // Keys for the active Safenode
    CPubKey pubKeySafenode;
    CKey keySafenode;

    // Initialized while registering Safenode
    CTxIn vin;
    CService service;

    int nState; // should be one of ACTIVE_SAFENODE_XXXX
    std::string strNotCapableReason;

    CActiveSafenode()
        : eType(SAFENODE_UNKNOWN),
          fPingerEnabled(false),
          pubKeySafenode(),
          keySafenode(),
          vin(),
          service(),
          nState(ACTIVE_SAFENODE_INITIAL)
    {}

    /// Manage state of active Safenode
    void ManageState();

    std::string GetStateString() const;
    std::string GetStatus() const;
    std::string GetTypeString() const;

private:
    void ManageStateInitial();
    void ManageStateRemote();
    void ManageStateLocal();
};

#endif
