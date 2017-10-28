// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Darkcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "core.h"
#include "db.h"
#include "init.h"
#include "activemasternode.h"
#include "masternodeman.h"
#include "masternodeconfig.h"
#include "rpcserver.h"
#include <boost/lexical_cast.hpp>
//#include "amount.h"
#include "util.h"
//#include "utilmoneystr.h"

#include <fstream>
using namespace json_spirit;
using namespace std;

void SendMoney(const CTxDestination &address, CAmount nValue, CWalletTx& wtxNew, AvailableCoinsType coin_type=ALL_COINS)
{
    // Check amount
    if (nValue <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nValue > pwalletMain->GetBalance())
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    string strError;
    if (pwalletMain->IsLocked())
    {
        strError = "Error: Wallet locked, unable to create transaction!";
        LogPrintf("SendMoney() : %s", strError);
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    // Parse pepecoin address
    CScript scriptPubKey = GetScriptForDestination(address);

    // Create and send the transaction
    CReserveKey reservekey(pwalletMain);
    int64_t nFeeRequired;
    std::string sNarr;
    if (!pwalletMain->CreateTransaction(scriptPubKey, nValue, sNarr, wtxNew, reservekey, nFeeRequired, NULL))
    {
        if (nValue + nFeeRequired > pwalletMain->GetBalance())
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds!", FormatMoney(nFeeRequired));
        LogPrintf("SendMoney() : %s\n", strError);
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    if (!pwalletMain->CommitTransaction(wtxNew, reservekey))
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: The transaction was rejected! This might happen if some of the coins in your wallet were already spent, such as if you used a copy of wallet.dat and coins were spent in the copy but not marked as spent here.");
}

Value darksend(const Array& params, bool fHelp)
{
    if (fHelp || params.size() == 0)
        throw runtime_error(
            "darksend <pepecoinaddress> <amount>\n"
            "pepecoinaddress, reset, or auto (AutoDenominate)"
            "<amount> is a real and is rounded to the nearest 0.00000001"
            + HelpRequiringPassphrase());

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    if(params[0].get_str() == "auto"){
        if(fMasterNode)
            return "DarkSend is not supported from masternodes";

        return "DoAutomaticDenominating " + (darkSendPool.DoAutomaticDenominating() ? "successful" : ("failed: " + darkSendPool.GetStatus()));
    }

    if(params[0].get_str() == "reset"){
        darkSendPool.Reset();
        return "successfully reset darksend";
    }

    if (params.size() != 2)
        throw runtime_error(
            "darksend <pepecoinaddress> <amount>\n"
            "pepecoinaddress, denominate, or auto (AutoDenominate)"
            "<amount> is type \"real\" and will be rounded to the nearest 0.1"
            + HelpRequiringPassphrase());

    CPepeCoinAddress address(params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid pepecoin address");

    // Amount
    CAmount nAmount = AmountFromValue(params[1]);

    // Wallet comments
    CWalletTx wtx;
    std::string sNarr;
    if (params.size() > 6 && params[6].type() != null_type && !params[6].get_str().empty())
        sNarr = params[6].get_str();
    
    if (sNarr.length() > 24)
        throw runtime_error("Narration must be 24 characters or less.");
    
    //string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, sNarr, wtx, ONLY_DENOMINATED);
    SendMoney(address.Get(), nAmount, wtx, ONLY_DENOMINATED);
    //if (strError != "")
        //throw JSONRPCError(RPC_WALLET_ERROR, strError);
   
    return wtx.GetHash().GetHex();
}


Value getpoolinfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getpoolinfo\n"
            "Returns an object containing anonymous pool-related information.");

    Object obj;
    obj.push_back(Pair("current_masternode",        mnodeman.GetCurrentMasterNode()->addr.ToString()));
    obj.push_back(Pair("state",        darkSendPool.GetState()));
    obj.push_back(Pair("entries",      darkSendPool.GetEntriesCount()));
    obj.push_back(Pair("entries_accepted",      darkSendPool.GetCountEntriesAccepted()));
    return obj;
}


Value masternode(const Array& params, bool fHelp)
{
    string strCommand;
    if (params.size() >= 1)
        strCommand = params[0].get_str();

    if (fHelp  ||
        (strCommand != "count" && strCommand != "current" && strCommand != "debug" && strCommand != "genkey" && strCommand != "enforce" && strCommand != "list" && strCommand != "list-conf"
        	&& strCommand != "start" && strCommand != "start-alias" && strCommand != "start-many" && strCommand != "status" && strCommand != "stop" && strCommand != "stop-alias"
                && strCommand != "stop-many" && strCommand != "winners" && strCommand != "connect" && strCommand != "outputs" && strCommand != "vote-many" && strCommand != "vote"))
        throw runtime_error(
                "masternode \"command\"... ( \"passphrase\" )\n"
                "Set of commands to execute masternode related actions\n"
                "\nArguments:\n"
                "1. \"command\"        (string or set of strings, required) The command to execute\n"
                "2. \"passphrase\"     (string, optional) The wallet passphrase\n"
                "\nAvailable commands:\n"
                "  count        - Print number of all known masternodes (optional: 'enabled', 'both')\n"
                "  current      - Print info on current masternode winner\n"
                "  debug        - Print masternode status\n"
                "  genkey       - Generate new masternodeprivkey\n"
                "  enforce      - Enforce masternode payments\n"
                "  list         - Print list of all known masternodes (see masternodelist for more info)\n"
                "  list-conf    - Print masternode.conf in JSON format\n"
                "  outputs      - Print masternode compatible outputs\n"
                "  start        - Start masternode configured in pepecoin.conf\n"
                "  start-alias  - Start single masternode by assigned alias configured in masternode.conf\n"
                "  start-many   - Start all masternodes configured in masternode.conf\n"
                "  status       - Print masternode status information\n"
                "  stop         - Stop masternode configured in pepecoin.conf\n"
                "  stop-alias   - Stop single masternode by assigned alias configured in masternode.conf\n"
                "  stop-many    - Stop all masternodes configured in masternode.conf\n"
                "  winners      - Print list of masternode winners\n"
                "  vote-many    - Vote on a PepeCoin initiative\n"
                "  vote         - Vote on a PepeCoin initiative\n"
                );

    if (strCommand == "stop")
    {
        if(!fMasterNode) return "you must set masternode=1 in the configuration";

        if(pwalletMain->IsLocked()) {
            SecureString strWalletPass;
            strWalletPass.reserve(100);

            if (params.size() == 2){
                strWalletPass = params[1].get_str().c_str();
            } else {
                throw runtime_error(
                    "Your wallet is locked, passphrase is required\n");
            }

            if(!pwalletMain->Unlock(strWalletPass)){
                return "incorrect passphrase";
            }
        }

        std::string errorMessage;
        if(!activeMasternode.StopMasterNode(errorMessage)) {
        	return "stop failed: " + errorMessage;
        }
        pwalletMain->Lock();

        if(activeMasternode.status == MASTERNODE_STOPPED) return "successfully stopped masternode";
        if(activeMasternode.status == MASTERNODE_NOT_CAPABLE) return "not capable masternode";

        return "unknown";
    }

    if (strCommand == "stop-alias")
    {
	    if (params.size() < 2){
			throw runtime_error(
			"command needs at least 2 parameters\n");
	    }

	    std::string alias = params[1].get_str().c_str();

    	if(pwalletMain->IsLocked()) {
    		SecureString strWalletPass;
    	    strWalletPass.reserve(100);

			if (params.size() == 3){
				strWalletPass = params[2].get_str().c_str();
			} else {
				throw runtime_error(
				"Your wallet is locked, passphrase is required\n");
			}

			if(!pwalletMain->Unlock(strWalletPass)){
				return "incorrect passphrase";
			}
        }

    	bool found = false;

		Object statusObj;
		statusObj.push_back(Pair("alias", alias));

    	BOOST_FOREACH(CMasternodeConfig::CMasternodeEntry mne, masternodeConfig.getEntries()) {
    		if(mne.getAlias() == alias) {
    			found = true;
    			std::string errorMessage;
    			bool result = activeMasternode.StopMasterNode(mne.getIp(), mne.getPrivKey(), errorMessage);

				statusObj.push_back(Pair("result", result ? "successful" : "failed"));
    			if(!result) {
   					statusObj.push_back(Pair("errorMessage", errorMessage));
   				}
    			break;
    		}
    	}

    	if(!found) {
    		statusObj.push_back(Pair("result", "failed"));
    		statusObj.push_back(Pair("errorMessage", "could not find alias in config. Verify with list-conf."));
    	}

    	pwalletMain->Lock();
    	return statusObj;
    }

    if (strCommand == "stop-many")
    {
    	if(pwalletMain->IsLocked()) {
			SecureString strWalletPass;
			strWalletPass.reserve(100);

			if (params.size() == 2){
				strWalletPass = params[1].get_str().c_str();
			} else {
				throw runtime_error(
				"Your wallet is locked, passphrase is required\n");
			}

			if(!pwalletMain->Unlock(strWalletPass)){
				return "incorrect passphrase";
			}
		}

		int total = 0;
		int successful = 0;
		int fail = 0;


		Object resultsObj;

		BOOST_FOREACH(CMasternodeConfig::CMasternodeEntry mne, masternodeConfig.getEntries()) {
			total++;

			std::string errorMessage;
			bool result = activeMasternode.StopMasterNode(mne.getIp(), mne.getPrivKey(), errorMessage);

			Object statusObj;
			statusObj.push_back(Pair("alias", mne.getAlias()));
			statusObj.push_back(Pair("result", result ? "successful" : "failed"));

			if(result) {
				successful++;
			} else {
				fail++;
				statusObj.push_back(Pair("errorMessage", errorMessage));
			}

			resultsObj.push_back(Pair("status", statusObj));
		}
		pwalletMain->Lock();

		Object returnObj;
		returnObj.push_back(Pair("overall", "Successfully stopped " + boost::lexical_cast<std::string>(successful) + " masternodes, failed to stop " +
				boost::lexical_cast<std::string>(fail) + ", total " + boost::lexical_cast<std::string>(total)));
		returnObj.push_back(Pair("detail", resultsObj));

		return returnObj;

    }

    if (strCommand == "list")
    {
        Array newParams(params.size() - 1);
        std::copy(params.begin() + 1, params.end(), newParams.begin());
        return masternodelist(newParams, fHelp);
    }

    if (strCommand == "count")
    {
        if (params.size() > 2){
            throw runtime_error(
                "too many parameters\n");
        }

        if (params.size() == 2)
        {
            if(params[1] == "enabled") return mnodeman.CountEnabled();
            if(params[1] == "both") return boost::lexical_cast<std::string>(mnodeman.CountEnabled()) + " / " + boost::lexical_cast<std::string>(mnodeman.size());
        }
        return mnodeman.size();
    }

    if (strCommand == "start")
    {
        if(!fMasterNode) return "you must set masternode=1 in the configuration";

        if(pwalletMain->IsLocked()) {
            SecureString strWalletPass;
            strWalletPass.reserve(100);

            if (params.size() == 2){
                strWalletPass = params[1].get_str().c_str();
            } else {
                throw runtime_error(
                    "Your wallet is locked, passphrase is required\n");
            }

            if(!pwalletMain->Unlock(strWalletPass)){
                return "incorrect passphrase";
            }
        }

        if(activeMasternode.status != MASTERNODE_REMOTELY_ENABLED && activeMasternode.status != MASTERNODE_IS_CAPABLE){
            activeMasternode.status = MASTERNODE_NOT_PROCESSED; // TODO: consider better way
            std::string errorMessage;
            activeMasternode.ManageStatus();
            pwalletMain->Lock();
        }

        if(activeMasternode.status == MASTERNODE_REMOTELY_ENABLED) return "masternode started remotely";
        if(activeMasternode.status == MASTERNODE_INPUT_TOO_NEW) return "masternode input must have at least 15 confirmations";
        if(activeMasternode.status == MASTERNODE_STOPPED) return "masternode is stopped";
        if(activeMasternode.status == MASTERNODE_IS_CAPABLE) return "successfully started masternode";
        if(activeMasternode.status == MASTERNODE_NOT_CAPABLE) return "not capable masternode: " + activeMasternode.notCapableReason;
        if(activeMasternode.status == MASTERNODE_SYNC_IN_PROCESS) return "sync in process. Must wait until client is synced to start.";

        return "unknown";
    }

    if (strCommand == "start-alias")
    {
	    if (params.size() < 2){
			throw runtime_error(
			"command needs at least 2 parameters\n");
	    }

	    std::string alias = params[1].get_str().c_str();

    	if(pwalletMain->IsLocked()) {
    		SecureString strWalletPass;
    	    strWalletPass.reserve(100);

			if (params.size() == 3){
				strWalletPass = params[2].get_str().c_str();
			} else {
				throw runtime_error(
				"Your wallet is locked, passphrase is required\n");
			}

			if(!pwalletMain->Unlock(strWalletPass)){
				return "incorrect passphrase";
			}
        }

    	bool found = false;

		Object statusObj;
		statusObj.push_back(Pair("alias", alias));

    	BOOST_FOREACH(CMasternodeConfig::CMasternodeEntry mne, masternodeConfig.getEntries()) {
    		if(mne.getAlias() == alias) {
    			found = true;
    			std::string errorMessage;
                std::string strDonateAddress = "";
                std::string strDonationPercentage = "";

                bool result = activeMasternode.Register(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strDonateAddress, strDonationPercentage, errorMessage);
  
                statusObj.push_back(Pair("result", result ? "successful" : "failed"));
    			if(!result) {
					statusObj.push_back(Pair("errorMessage", errorMessage));
				}
    			break;
    		}
    	}

    	if(!found) {
    		statusObj.push_back(Pair("result", "failed"));
    		statusObj.push_back(Pair("errorMessage", "could not find alias in config. Verify with list-conf."));
    	}

    	pwalletMain->Lock();
    	return statusObj;

    }

    if (strCommand == "start-many")
    {
    	if(pwalletMain->IsLocked()) {
			SecureString strWalletPass;
			strWalletPass.reserve(100);

			if (params.size() == 2){
				strWalletPass = params[1].get_str().c_str();
			} else {
				throw runtime_error(
				"Your wallet is locked, passphrase is required\n");
			}

			if(!pwalletMain->Unlock(strWalletPass)){
				return "incorrect passphrase";
			}
		}

		std::vector<CMasternodeConfig::CMasternodeEntry> mnEntries;
		mnEntries = masternodeConfig.getEntries();

		int total = 0;
		int successful = 0;
		int fail = 0;

		Object resultsObj;

		BOOST_FOREACH(CMasternodeConfig::CMasternodeEntry mne, masternodeConfig.getEntries()) {
			total++;

			std::string errorMessage;
            std::string strDonateAddress = "";
            std::string strDonationPercentage = "";

            bool result = activeMasternode.Register(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strDonateAddress, strDonationPercentage, errorMessage);

			Object statusObj;
			statusObj.push_back(Pair("alias", mne.getAlias()));
			statusObj.push_back(Pair("result", result ? "succesful" : "failed"));

			if(result) {
				successful++;
			} else {
				fail++;
				statusObj.push_back(Pair("errorMessage", errorMessage));
			}

			resultsObj.push_back(Pair("status", statusObj));
		}
		pwalletMain->Lock();

		Object returnObj;
		returnObj.push_back(Pair("overall", "Successfully started " + boost::lexical_cast<std::string>(successful) + " masternodes, failed to start " +
				boost::lexical_cast<std::string>(fail) + ", total " + boost::lexical_cast<std::string>(total)));
		returnObj.push_back(Pair("detail", resultsObj));

		return returnObj;
    }

    if (strCommand == "debug")
    {
        if(activeMasternode.status == MASTERNODE_REMOTELY_ENABLED) return "masternode started remotely";
        if(activeMasternode.status == MASTERNODE_INPUT_TOO_NEW) return "masternode input must have at least 15 confirmations";
        if(activeMasternode.status == MASTERNODE_IS_CAPABLE) return "successfully started masternode";
        if(activeMasternode.status == MASTERNODE_STOPPED) return "masternode is stopped";
        if(activeMasternode.status == MASTERNODE_NOT_CAPABLE) return "not capable masternode: " + activeMasternode.notCapableReason;
        if(activeMasternode.status == MASTERNODE_SYNC_IN_PROCESS) return "sync in process. Must wait until client is synced to start.";

        CTxIn vin = CTxIn();
        CPubKey pubkey = CScript();
        CKey key;
        bool found = activeMasternode.GetMasterNodeVin(vin, pubkey, key);
        if(!found){
            return "Missing masternode input, please look at the documentation for instructions on masternode creation";
        } else {
            return "No problems were found";
        }
    }

    if (strCommand == "create")
    {

        return "Not implemented yet, please look at the documentation for instructions on masternode creation";
    }

    if (strCommand == "current")
    {
        CMasternode* winner = mnodeman.GetCurrentMasterNode(1);
        if(winner) {
            Object obj;
            CScript pubkey;
            pubkey.SetDestination(winner->pubkey.GetID());
            CTxDestination address1;
            ExtractDestination(pubkey, address1);
            CPepeCoinAddress address2(address1);

            obj.push_back(Pair("IP:port",       winner->addr.ToString().c_str()));
            obj.push_back(Pair("protocol",      (int64_t)winner->protocolVersion));
            obj.push_back(Pair("vin",           winner->vin.prevout.hash.ToString().c_str()));
            obj.push_back(Pair("pubkey",        address2.ToString().c_str()));
            obj.push_back(Pair("lastseen",      (int64_t)winner->lastTimeSeen));
            obj.push_back(Pair("activeseconds", (int64_t)(winner->lastTimeSeen - winner->sigTime)));
            return obj;
        }

        return "unknown";
    }

    if (strCommand == "genkey")
    {
        CKey secret;
        secret.MakeNewKey(false);

        return CPepeCoinSecret(secret).ToString();
    }

    if (strCommand == "winners")
    {
        Object obj;
        std::string strMode = "addr";
    
        if (params.size() >= 1) strMode = params[0].get_str();

        for(int nHeight = pindexBest->nHeight-10; nHeight < pindexBest->nHeight+20; nHeight++)
        {
            CScript payee;
            CTxIn vin;
            if(masternodePayments.GetBlockPayee(nHeight, payee, vin)){
                CTxDestination address1;
                ExtractDestination(payee, address1);
                CPepeCoinAddress address2(address1);

                if(strMode == "addr")
                    obj.push_back(Pair(boost::lexical_cast<std::string>(nHeight),       address2.ToString().c_str()));

                if(strMode == "vin")
                    obj.push_back(Pair(boost::lexical_cast<std::string>(nHeight),       vin.ToString().c_str()));

            } else {
                obj.push_back(Pair(boost::lexical_cast<std::string>(nHeight),       ""));
            }
        }

        return obj;
    }

    if(strCommand == "enforce")
    {
        return (uint64_t)enforceMasternodePaymentsTime;
    }

    if(strCommand == "connect")
    {
        std::string strAddress = "";
        if (params.size() == 2){
            strAddress = params[1].get_str().c_str();
        } else {
            throw runtime_error(
                "Masternode address required\n");
        }

        CService addr = CService(strAddress);

        if(ConnectNode((CAddress)addr, NULL, true)){
            return "successfully connected";
        } else {
            return "error connecting";
        }
    }

    if(strCommand == "list-conf")
    {
    	std::vector<CMasternodeConfig::CMasternodeEntry> mnEntries;
    	mnEntries = masternodeConfig.getEntries();

        Object resultObj;

        BOOST_FOREACH(CMasternodeConfig::CMasternodeEntry mne, masternodeConfig.getEntries()) {
    		Object mnObj;
    		mnObj.push_back(Pair("alias", mne.getAlias()));
    		mnObj.push_back(Pair("address", mne.getIp()));
    		mnObj.push_back(Pair("privateKey", mne.getPrivKey()));
    		mnObj.push_back(Pair("txHash", mne.getTxHash()));
    		mnObj.push_back(Pair("outputIndex", mne.getOutputIndex()));
    		resultObj.push_back(Pair("masternode", mnObj));
    	}

    	return resultObj;
    }

    if (strCommand == "outputs"){
        // Find possible candidates
        vector<COutput> possibleCoins = activeMasternode.SelectCoinsMasternode();

        Object obj;
        BOOST_FOREACH(COutput& out, possibleCoins) {
            obj.push_back(Pair(out.tx->GetHash().ToString().c_str(), boost::lexical_cast<std::string>(out.i)));
        }

        return obj;

    }

    if(strCommand == "vote-many")
    {
        std::vector<CMasternodeConfig::CMasternodeEntry> mnEntries;
        mnEntries = masternodeConfig.getEntries();

        if (params.size() != 2)
            throw runtime_error("You can only vote 'yay' or 'nay'");

        std::string vote = params[1].get_str().c_str();
        if(vote != "yay" && vote != "nay") return "You can only vote 'yay' or 'nay'";
        int nVote = 0;
        if(vote == "yay") nVote = 1;
        if(vote == "nay") nVote = -1;

        int success = 0;
        int failed = 0;

        Object resultObj;

        BOOST_FOREACH(CMasternodeConfig::CMasternodeEntry mne, masternodeConfig.getEntries()) {
            std::string errorMessage;
            std::vector<unsigned char> vchMasterNodeSignature;
            std::string strMasterNodeSignMessage;

            CPubKey pubKeyCollateralAddress;
            CKey keyCollateralAddress;
            CPubKey pubKeyMasternode;
            CKey keyMasternode;

            if(!darkSendSigner.SetKey(mne.getPrivKey(), errorMessage, keyMasternode, pubKeyMasternode)){
                printf(" Error upon calling SetKey for %s\n", mne.getAlias().c_str());
                failed++;
                continue;
            }
            
            CMasternode* pmn = mnodeman.Find(pubKeyMasternode);
            if(pmn == NULL)
            {
                printf("Can't find masternode by pubkey for %s\n", mne.getAlias().c_str());
                failed++;
                continue;
            }

            std::string strMessage = pmn->vin.ToString() + boost::lexical_cast<std::string>(nVote);

            if(!darkSendSigner.SignMessage(strMessage, errorMessage, vchMasterNodeSignature, keyMasternode)){
                printf(" Error upon calling SignMessage for %s\n", mne.getAlias().c_str());
                failed++;
                continue;
            }

            if(!darkSendSigner.VerifyMessage(pubKeyMasternode, vchMasterNodeSignature, strMessage, errorMessage)){
                printf(" Error upon calling VerifyMessage for %s\n", mne.getAlias().c_str());
                failed++;
                continue;
            }

            success++;

            //send to all peers
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
                pnode->PushMessage("mvote", pmn->vin, vchMasterNodeSignature, nVote);
        }

        return("Voted successfully " + boost::lexical_cast<std::string>(success) + " time(s) and failed " + boost::lexical_cast<std::string>(failed) + " time(s).");
    }

    if(strCommand == "vote")
    {
        std::vector<CMasternodeConfig::CMasternodeEntry> mnEntries;
        mnEntries = masternodeConfig.getEntries();

        if (params.size() != 2)
            throw runtime_error("You can only vote 'yay' or 'nay'");

        std::string vote = params[1].get_str().c_str();
        if(vote != "yay" && vote != "nay") return "You can only vote 'yay' or 'nay'";
        int nVote = 0;
        if(vote == "yay") nVote = 1;
        if(vote == "nay") nVote = -1;

        // Choose coins to use
        CPubKey pubKeyCollateralAddress;
        CKey keyCollateralAddress;
        CPubKey pubKeyMasternode;
        CKey keyMasternode;

        std::string errorMessage;
        std::vector<unsigned char> vchMasterNodeSignature;
        std::string strMessage = activeMasternode.vin.ToString() + boost::lexical_cast<std::string>(nVote);

        if(!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, keyMasternode, pubKeyMasternode))
            return(" Error upon calling SetKey");

        if(!darkSendSigner.SignMessage(strMessage, errorMessage, vchMasterNodeSignature, keyMasternode))
            return(" Error upon calling SignMessage");

        if(!darkSendSigner.VerifyMessage(pubKeyMasternode, vchMasterNodeSignature, strMessage, errorMessage))
            return(" Error upon calling VerifyMessage");

        //send to all peers
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            pnode->PushMessage("mvote", activeMasternode.vin, vchMasterNodeSignature, nVote);

    }

    if(strCommand == "status")
    {
        std::vector<CMasternodeConfig::CMasternodeEntry> mnEntries;
        mnEntries = masternodeConfig.getEntries();

        CScript pubkey;
        pubkey = GetScriptForDestination(activeMasternode.pubKeyMasternode.GetID());
        CTxDestination address1;
        ExtractDestination(pubkey, address1);
        CPepeCoinAddress address2(address1);

        Object mnObj;
        mnObj.push_back(Pair("vin", activeMasternode.vin.ToString().c_str()));
        mnObj.push_back(Pair("service", activeMasternode.service.ToString().c_str()));
        mnObj.push_back(Pair("status", activeMasternode.status));
        mnObj.push_back(Pair("pubKeyMasternode", address2.ToString().c_str()));
        mnObj.push_back(Pair("notCapableReason", activeMasternode.notCapableReason.c_str()));
        return mnObj;
    }

    return Value::null;
}

Value masternodelist(const Array& params, bool fHelp)
{
    std::string strMode = "status";
    std::string strFilter = "";

    if (params.size() >= 1) strMode = params[0].get_str();
    if (params.size() == 2) strFilter = params[1].get_str();

    if (fHelp ||
            (strMode != "activeseconds" && strMode != "donation" && strMode != "full" && strMode != "lastseen" && strMode != "protocol" 
                && strMode != "pubkey" && strMode != "rank" && strMode != "status" && strMode != "addr" && strMode != "votes" && strMode != "lastpaid"))
    {
        throw runtime_error(
                "masternodelist ( \"mode\" \"filter\" )\n"
                "Get a list of masternodes in different modes\n"
                "\nArguments:\n"
                "1. \"mode\"      (string, optional/required to use filter, defaults = status) The mode to run list in\n"
                "2. \"filter\"    (string, optional) Filter results. Partial match by IP by default in all modes, additional matches in some modes\n"
                "\nAvailable modes:\n"
                "  activeseconds  - Print number of seconds masternode recognized by the network as enabled\n"
                "  donation       - Show donation settings\n"
                "  full           - Print info in format 'status protocol pubkey vin lastseen activeseconds' (can be additionally filtered, partial match)\n"
                "  lastseen       - Print timestamp of when a masternode was last seen on the network\n"
                "  protocol       - Print protocol of a masternode (can be additionally filtered, exact match)\n"
                "  pubkey         - Print public key associated with a masternode (can be additionally filtered, partial match)\n"
                "  rank           - Print rank of a masternode based on current block\n"
                "  status         - Print masternode status: ENABLED / EXPIRED / VIN_SPENT / REMOVE / POS_ERROR (can be additionally filtered, partial match)\n"
                "  addr            - Print ip address associated with a masternode (can be additionally filtered, partial match)\n"
                "  votes          - Print all masternode votes for a PepeCoin initiative (can be additionally filtered, partial match)\n"
                "  lastpaid       - The last time a node was paid on the network\n"
                );
    }

    Object obj;
    if (strMode == "rank") {
        std::vector<pair<int, CMasternode> > vMasternodeRanks = mnodeman.GetMasternodeRanks(pindexBest->nHeight);
        BOOST_FOREACH(PAIRTYPE(int, CMasternode)& s, vMasternodeRanks) {
            std::string strVin = s.second.vin.prevout.ToStringShort();
            if(strFilter !="" && strVin.find(strFilter) == string::npos) continue;
            obj.push_back(Pair(strVin,       s.first));
        }
    } else {
        std::vector<CMasternode> vMasternodes = mnodeman.GetFullMasternodeVector();
        BOOST_FOREACH(CMasternode& mn, vMasternodes) {
            std::string strVin = mn.vin.prevout.ToStringShort();
            if (strMode == "activeseconds") {
                if(strFilter !="" && strVin.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strVin,       (int64_t)(mn.lastTimeSeen - mn.sigTime)));
            } else if (strMode == "donation") {
                CTxDestination address1;
                ExtractDestination(mn.donationAddress, address1);
                CPepeCoinAddress address2(address1);

                if(strFilter !="" && address2.ToString().find(strFilter) == string::npos &&
                    strVin.find(strFilter) == string::npos) continue;

                std::string strOut = "";

                if(mn.donationPercentage != 0){
                    strOut = address2.ToString().c_str();
                    strOut += ":";
                    strOut += boost::lexical_cast<std::string>(mn.donationPercentage);
                }
                obj.push_back(Pair(strVin,       strOut.c_str()));
            } else if (strMode == "full") {
                CScript pubkey;
                pubkey.SetDestination(mn.pubkey.GetID());
                CTxDestination address1;
                ExtractDestination(pubkey, address1);
                CPepeCoinAddress address2(address1);

                std::ostringstream addrStream;
                addrStream << setw(21) << strVin;

                std::ostringstream stringStream;
                stringStream << setw(10) <<
                               mn.Status() << " " <<
                               mn.protocolVersion << " " <<
                               address2.ToString() << " " <<
                               mn.addr.ToString() << " " <<
                               mn.lastTimeSeen << " " << setw(8) <<
                               (mn.lastTimeSeen - mn.sigTime) << " " <<
                               mn.nLastPaid;
                std::string output = stringStream.str();
                stringStream << " " << strVin;
                if(strFilter !="" && stringStream.str().find(strFilter) == string::npos &&
                        strVin.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(addrStream.str(), output));
            } else if (strMode == "lastseen") {
                if(strFilter !="" && strVin.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strVin,       (int64_t)mn.lastTimeSeen));
            } else if (strMode == "protocol") {
                if(strFilter !="" && strFilter != boost::lexical_cast<std::string>(mn.protocolVersion) &&
                    strVin.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strVin,       (int64_t)mn.protocolVersion));
            } else if (strMode == "pubkey") {
                CScript pubkey;
                pubkey.SetDestination(mn.pubkey.GetID());
                CTxDestination address1;
                ExtractDestination(pubkey, address1);
                CPepeCoinAddress address2(address1);

                if(strFilter !="" && address2.ToString().find(strFilter) == string::npos &&
                    strVin.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strVin,       address2.ToString().c_str()));
            } else if(strMode == "status") {
                std::string strStatus = mn.Status();
                if(strFilter !="" && strVin.find(strFilter) == string::npos && strStatus.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strVin,       strStatus.c_str()));
            } else if (strMode == "addr") {
                if(strFilter !="" && mn.vin.prevout.hash.ToString().find(strFilter) == string::npos &&
                    strVin.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strVin,       mn.addr.ToString().c_str()));
            } else if(strMode == "votes"){
                std::string strStatus = "ABSTAIN";

                //voting lasts 30 days, ignore the last vote if it was older than that
                if((GetAdjustedTime() - mn.lastVote) < (60*60*30*24))
                {
                    if(mn.nVote == -1) strStatus = "NAY";
                    if(mn.nVote == 1) strStatus = "YAY";
                }

                if(strFilter !="" && (strVin.find(strFilter) == string::npos && strStatus.find(strFilter) == string::npos)) continue;
                obj.push_back(Pair(strVin,       strStatus.c_str()));
            } else if(strMode == "lastpaid"){
                if(strFilter !="" && mn.vin.prevout.hash.ToString().find(strFilter) == string::npos &&
                    strVin.find(strFilter) == string::npos) continue;
                obj.push_back(Pair(strVin,      (int64_t)mn.nLastPaid));
            }
        }
    }
    return obj;

}
