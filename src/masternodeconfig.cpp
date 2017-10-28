#include "net.h"
#include "masternodeconfig.h"
#include "util.h"
#include <base58.h>

CMasternodeConfig masternodeConfig;


void CMasternodeConfig::add(std::string alias, std::string ip, std::string privKey, std::string txHash, std::string outputIndex, std::string donationAddress, std::string donationPercent) {
    CMasternodeEntry cme(alias, ip, privKey, txHash, outputIndex, donationAddress, donationPercent);
    entries.push_back(cme);
}

bool CMasternodeConfig::read(boost::filesystem::path path) {
    boost::filesystem::ifstream streamConfig(GetMasternodeConfigFile());
    if (!streamConfig.good()) {
        return true; // No masternode.conf file is OK
    }

    for(std::string line; std::getline(streamConfig, line); )
    {
        if(line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        std::string alias, ip, privKey, txHash, outputIndex, donation, donationAddress, donationPercent;
        if (!(iss >> alias >> ip >> privKey >> txHash >> outputIndex >> donation)) {
            donationAddress = "";
            donationPercent = "";
            iss.str(line);
            iss.clear();
            if (!(iss >> alias >> ip >> privKey >> txHash >> outputIndex)) {
                LogPrintf("Could not parse masternode.conf line: %s\n", line.c_str());
                streamConfig.close();
                return false;
            }
        } else {
            size_t pos = donation.find_first_of(":");
            if(pos == string::npos) { // no ":" found
                donationPercent = "100";
                donationAddress = donation;
            } else {
                donationPercent = donation.substr(pos + 1);
                donationAddress = donation.substr(0, pos);
            }
            CBitcoinAddress address(donationAddress);
            if (!address.IsValid()) {
                LogPrintf("Invalid PEPE address in masternode.conf line: %s\n", line.c_str());
                streamConfig.close();
                return false;
            }
        }

        if(Params().NetworkID() == CChainParams::MAIN){
            if(CService(ip).GetPort() != 17170) {
                LogPrintf("Invalid port detected in masternode.conf: %s (must be 17170 for mainnet)\n", line.c_str());
                streamConfig.close();
                return false;
            }
        } else if(CService(ip).GetPort() == 17170) {
            LogPrintf("Invalid port detected in masternode.conf: %s (17170 must be only on mainnet)\n", line.c_str());
            streamConfig.close();
            return false;
        }

        if (!(CService(ip).IsIPv4() && CService(ip).IsRoutable())) {
            LogPrintf("Invalid Address detected in masternode.conf: %s (IPV4 ONLY) \n", line.c_str());
            streamConfig.close();
            return false;
        }

        add(alias, ip, privKey, txHash, outputIndex, donationAddress, donationPercent);
    }

    streamConfig.close();
    return true;
}