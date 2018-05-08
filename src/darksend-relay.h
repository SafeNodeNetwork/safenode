
// Copyright (c) 2014-2018 The SafeNode developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DARKSEND_RELAY_H
#define DARKSEND_RELAY_H

#include "main.h"
#include "activesafenode.h"
#include "safenodeman.h"


class CDarkSendRelay
{
public:
    CTxIn vinSafenode;
    vector<unsigned char> vchSig;
    vector<unsigned char> vchSig2;
    int nBlockHeight;
    int nRelayType;
    CTxIn in;
    CTxOut out;

    CDarkSendRelay();
    CDarkSendRelay(CTxIn& vinSafenodeIn, vector<unsigned char>& vchSigIn, int nBlockHeightIn, int nRelayTypeIn, CTxIn& in2, CTxOut& out2);
    
    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(vinSafenode);
        READWRITE(vchSig);
        READWRITE(vchSig2);
        READWRITE(nBlockHeight);
        READWRITE(nRelayType);
        READWRITE(in);
        READWRITE(out);
    }

    std::string ToString();

    bool Sign(std::string strSharedKey);
    bool VerifyMessage(std::string strSharedKey);
    void Relay();
    void RelayThroughNode(int nRank);
};



#endif
