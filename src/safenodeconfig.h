// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2018 The SafeNode developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SRC_SAFENODECONFIG_H_
#define SRC_SAFENODECONFIG_H_

class CSafenodeConfig;
extern CSafenodeConfig safenodeConfig;

class CSafenodeConfig
{

public:

    class CSafenodeEntry {

    private:
        std::string alias;
        std::string ip;
        std::string privKey;
        std::string txHash;
        std::string outputIndex;
    public:

        CSafenodeEntry(std::string alias, std::string ip, std::string privKey, std::string txHash, std::string outputIndex) {
            this->alias = alias;
            this->ip = ip;
            this->privKey = privKey;
            this->txHash = txHash;
            this->outputIndex = outputIndex;
        }

        const std::string& getAlias() const {
            return alias;
        }

        void setAlias(const std::string& alias) {
            this->alias = alias;
        }

        const std::string& getOutputIndex() const {
            return outputIndex;
        }

        void setOutputIndex(const std::string& outputIndex) {
            this->outputIndex = outputIndex;
        }

        const std::string& getPrivKey() const {
            return privKey;
        }

        void setPrivKey(const std::string& privKey) {
            this->privKey = privKey;
        }

        const std::string& getTxHash() const {
            return txHash;
        }

        void setTxHash(const std::string& txHash) {
            this->txHash = txHash;
        }

        const std::string& getIp() const {
            return ip;
        }

        void setIp(const std::string& ip) {
            this->ip = ip;
        }
    };

    CSafenodeConfig() {
        entries = std::vector<CSafenodeEntry>();
    }

    void clear();
    bool read(std::string& strErr);
    void add(std::string alias, std::string ip, std::string privKey, std::string txHash, std::string outputIndex);

    std::vector<CSafenodeEntry>& getEntries() {
        return entries;
    }

    int getCount() {
        return (int)entries.size();
    }

private:
    std::vector<CSafenodeEntry> entries;


};


#endif /* SRC_SAFENODECONFIG_H_ */
