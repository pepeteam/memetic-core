// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2012 Litecoin Developers
// Copyright (c) 2013 Peercoin Developers
// Copyright (c) 2014 DarkCoin Developers
// Copyright (c) 2014 BlackCoin Developers
// Copyright (c) 2014 Digibyte Developers
// Copyright (c) 2014 DashCoin Developers
// Copyright (c) 2014 NetCoin Developers
// Copyright (c) 2015 Transfercoin Developer
// Copyright (c) 2015-2016 PepeCoin Developers
// Copyright (c) 2016-2019 Memetic / PepeCoin Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>

#include "alert.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "db.h"
#include "init.h"
#include "kernel.h"
#include "net.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "instantx.h"
#include "darksend.h"
#include "masternode.h"
#include "spork.h"
#include "smessage.h"

using namespace std;
using namespace boost;

//
// Global state
//

CCriticalSection cs_setpwalletRegistered;
set<CWallet*> setpwalletRegistered;

CCriticalSection cs_main;

CTxMemPool mempool;

map<uint256, CBlockIndex*> mapBlockIndex;
map<uint256, CPepeMessage> mapPepeMessages;
set<pair<COutPoint, unsigned int> > setStakeSeen;

CBigNum bnProofOfStakeLimit(~uint256(0) >> 20);

unsigned int nStakeMinAge = 6 * 60 * 60; // 6 hours  temporary 1 hr for test
unsigned int nModifierInterval = 2 * 60; // time to elapse before new modifier is computed

int nCoinbaseMaturity = 60;     // 60 blocks until new POW or POS minted coins can be moved
CBlockIndex* pindexGenesisBlock = NULL;
int nBestHeight = -1;

uint256 nBestChainTrust = 0;
uint256 nBestInvalidTrust = 0;

uint256 hashBestChain = 0;
CBlockIndex* pindexBest = NULL;
int64_t nTimeBestReceived = 0;
bool fImporting = false;
bool fReindex = false;
bool fAddrIndex = false;
bool fHaveGUI = false;

struct COrphanBlock {
    uint256 hashBlock;
    uint256 hashPrev;
    std::pair<COutPoint, unsigned int> stake;
    vector<unsigned char> vchBlock;
};
map<uint256, COrphanBlock*> mapOrphanBlocks;
multimap<uint256, COrphanBlock*> mapOrphanBlocksByPrev;
set<pair<COutPoint, unsigned int> > setStakeSeenOrphan;

map<uint256, CTransaction> mapOrphanTransactions;
map<uint256, set<uint256> > mapOrphanTransactionsByPrev;

// Constant stuff for coinbase transactions we create:
CScript COINBASE_FLAGS;

const string strMessageMagic = "PepeCoin Signed Message:\n";

std::set<uint256> setValidatedTx;

//////////////////////////////////////////////////////////////////////////////
//
// dispatching functions
//

// These functions dispatch to one or all registered wallets

namespace {

struct CMainSignals {
    // Notifies listeners of updated transaction data (passing hash, transaction, and optionally the block it is found in.
    boost::signals2::signal<void (const CTransaction &, const CBlock *, bool)> SyncTransaction;
    // Notifies listeners of an erased transaction (currently disabled, requires transaction replacement).
    boost::signals2::signal<void (const uint256 &)> EraseTransaction;
    // Notifies listeners of an updated transaction without new data (for now: a coinbase potentially becoming visible).
    boost::signals2::signal<void (const uint256 &)> UpdatedTransaction;
    // Notifies listeners of a new active block chain.
    boost::signals2::signal<void (const CBlockLocator &)> SetBestChain;
    // Notifies listeners about an inventory item being seen on the network.
    boost::signals2::signal<void (const uint256 &)> Inventory;
    // Tells listeners to broadcast their data.
    boost::signals2::signal<void (bool)> Broadcast;
} g_signals;
}

void RegisterWallet(CWalletInterface* pwalletIn) {
    g_signals.SyncTransaction.connect(boost::bind(&CWalletInterface::SyncTransaction, pwalletIn, _1, _2, _3));
    g_signals.EraseTransaction.connect(boost::bind(&CWalletInterface::EraseFromWallet, pwalletIn, _1));
    g_signals.UpdatedTransaction.connect(boost::bind(&CWalletInterface::UpdatedTransaction, pwalletIn, _1));
    g_signals.SetBestChain.connect(boost::bind(&CWalletInterface::SetBestChain, pwalletIn, _1));
    g_signals.Inventory.connect(boost::bind(&CWalletInterface::Inventory, pwalletIn, _1));
    g_signals.Broadcast.connect(boost::bind(&CWalletInterface::ResendWalletTransactions, pwalletIn, _1));
}

void UnregisterWallet(CWalletInterface* pwalletIn) {
    g_signals.Broadcast.disconnect(boost::bind(&CWalletInterface::ResendWalletTransactions, pwalletIn, _1));
    g_signals.Inventory.disconnect(boost::bind(&CWalletInterface::Inventory, pwalletIn, _1));
    g_signals.SetBestChain.disconnect(boost::bind(&CWalletInterface::SetBestChain, pwalletIn, _1));
    g_signals.UpdatedTransaction.disconnect(boost::bind(&CWalletInterface::UpdatedTransaction, pwalletIn, _1));
    g_signals.EraseTransaction.disconnect(boost::bind(&CWalletInterface::EraseFromWallet, pwalletIn, _1));
    g_signals.SyncTransaction.disconnect(boost::bind(&CWalletInterface::SyncTransaction, pwalletIn, _1, _2, _3));
}

void UnregisterAllWallets() {
    g_signals.Broadcast.disconnect_all_slots();
    g_signals.Inventory.disconnect_all_slots();
    g_signals.SetBestChain.disconnect_all_slots();
    g_signals.UpdatedTransaction.disconnect_all_slots();
    g_signals.EraseTransaction.disconnect_all_slots();
    g_signals.SyncTransaction.disconnect_all_slots();
}

void SyncWithWallets(const CTransaction &tx, const CBlock *pblock, bool fConnect) {
    g_signals.SyncTransaction(tx, pblock, fConnect);
}

void ResendWalletTransactions(bool fForce) {
    g_signals.Broadcast(fForce);
}


//////////////////////////////////////////////////////////////////////////////
//
// Registration of network node signals.
//

void RegisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.ProcessMessages.connect(&ProcessMessages);
    nodeSignals.SendMessages.connect(&SendMessages);
}

void UnregisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.ProcessMessages.disconnect(&ProcessMessages);
    nodeSignals.SendMessages.disconnect(&SendMessages);
}

bool AbortNode(const std::string &strMessage, const std::string &userMessage) {
    strMiscWarning = strMessage;
    LogPrintf("*** %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occured, see debug.log for details") : userMessage,
        "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

int GetHeight()
{
    while(true){
        TRY_LOCK(cs_main, lockMain);
        if(!lockMain) { MilliSleep(50); continue; }
        return pindexBest->nHeight;
    }
}

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CTransaction& tx)
{
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:

    size_t nSize = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);

    if (nSize > 5000)
    {
        LogPrint("mempool", "ignoring large orphan tx (size: %u, hash: %s)\n", nSize, hash.ToString());
        return false;
    }

    mapOrphanTransactions[hash] = tx;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    LogPrint("mempool", "stored orphan tx %s (mapsz %u)\n", hash.ToString(),
        mapOrphanTransactions.size());
    return true;
}

void static EraseOrphanTx(uint256 hash)
{
    map<uint256, CTransaction>::iterator it = mapOrphanTransactions.find(hash);
    if (it == mapOrphanTransactions.end())
        return;
    BOOST_FOREACH(const CTxIn& txin, it->second.vin)
    {
        map<uint256, set<uint256> >::iterator itPrev = mapOrphanTransactionsByPrev.find(txin.prevout.hash);
        if (itPrev == mapOrphanTransactionsByPrev.end())
            continue;
        itPrev->second.erase(hash);
        if (itPrev->second.empty())
            mapOrphanTransactionsByPrev.erase(itPrev);
    }
    mapOrphanTransactions.erase(it);
}

unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    unsigned int nEvicted = 0;
    while (mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        map<uint256, CTransaction>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}

//////////////////////////////////////////////////////////////////////////////
//
// CTransaction and CTxIndex
//

bool CTransaction::ReadFromDisk(CTxDB& txdb, const uint256& hash, CTxIndex& txindexRet)
{
    SetNull();
    if (!txdb.ReadTxIndex(hash, txindexRet))
        return false;
    if (!ReadFromDisk(txindexRet.pos))
        return false;
    return true;
}

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout, CTxIndex& txindexRet)
{
    if (!ReadFromDisk(txdb, prevout.hash, txindexRet))
        return false;
    if (prevout.n >= vout.size())
    {
        SetNull();
        return false;
    }
    return true;
}

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout)
{
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

bool CTransaction::ReadFromDisk(COutPoint prevout)
{
    CTxDB txdb("r");
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

bool IsStandardTx(const CTransaction& tx, string& reason)
{
    if (tx.nVersion > CTransaction::CURRENT_VERSION || tx.nVersion < 1) {
        reason = "version";
        return false;
    }

    // Treat non-final transactions as non-standard to prevent a specific type
    // of double-spend attack, as well as DoS attacks. (if the transaction
    // can't be mined, the attacker isn't expending resources broadcasting it)
    // Basically we don't want to propagate transactions that can't be included in
    // the next block.
    //
    // However, IsFinalTx() is confusing... Without arguments, it uses
    // chainActive.Height() to evaluate nLockTime; when a block is accepted, chainActive.Height()
    // is set to the value of nHeight in the block. However, when IsFinalTx()
    // is called within CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a transaction can
    // be part of the *next* block, we need to call IsFinalTx() with one more
    // than chainActive.Height().
    //
    // Timestamps on the other hand don't get any special treatment, because we
    // can't know what timestamp the next block will have, and there aren't
    // timestamp applications where it matters.
    if (!IsFinalTx(tx, nBestHeight + 1)) {
        reason = "non-final";
        return false;
    }
    // nTime has different purpose from nLockTime but can be used in similar attacks
    if (tx.nTime > FutureDrift(GetAdjustedTime(), nBestHeight + 1)) {
        reason = "time-too-new";
        return false;
    }

    // Extremely large transactions with lots of inputs can cost the network
    // almost as much to process as they cost the sender in fees, because
    // computing signature hashes is O(ninputs*txsize). Limiting transactions
    // to MAX_STANDARD_TX_SIZE mitigates CPU exhaustion attacks.
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    if (sz >= MAX_STANDARD_TX_SIZE) {
        reason = "tx-size";
        return false;
    }

    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        // Biggest 'standard' txin is a 15-of-15 P2SH multisig with compressed
        // keys. (remember the 520 byte limit on redeemScript size) That works
        // out to a (15*(33+1))+3=513 byte redeemScript, 513+1+15*(73+1)+3=1627
        // bytes of scriptSig, which we round off to 1650 bytes for some minor
        // future-proofing. That's also enough to spend a 20-of-20
        // CHECKMULTISIG scriptPubKey, though such a scriptPubKey is not
        // considered standard)
        if (txin.scriptSig.size() > 1650) {
            reason = "scriptsig-size";
            return false;
        }
        if (!txin.scriptSig.IsPushOnly()) {
            reason = "scriptsig-not-pushonly";
            return false;
        }
        if (!txin.scriptSig.HasCanonicalPushes()) {
            reason = "scriptsig-non-canonical-push";
            return false;
        }
    }

    unsigned int nDataOut = 0;

    txnouttype whichType;
    BOOST_FOREACH(const CTxOut& txout, tx.vout) {
        if (!::IsStandard(txout.scriptPubKey, whichType))
        {
            reason = "scriptpubkey";
            return false;
        }
        if (whichType == TX_NULL_DATA)
        {
            nDataOut++;
        } else if (txout.nValue == 0) {
            reason = "dust";
            return false;
        }
        if (!txout.scriptPubKey.HasCanonicalPushes()) {
            reason = "scriptpubkey-non-canonical-push";
            return false;
        }
    }

    // not more than one data txout per non-data txout is permitted
    // only one data txout is permitted too
    if (nDataOut > 1 && nDataOut > tx.vout.size()/2) {
        reason = "multi-op-return";
        return false;
    }

    return true;
}

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    AssertLockHeld(cs_main);
    
    // Time based nLockTime implemented in 0.1.6
    if (tx.nLockTime == 0)
        return true;
    if (nBlockHeight == 0)
        nBlockHeight = nBestHeight;
    if (nBlockTime == 0)
        nBlockTime = GetAdjustedTime();
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        if (!txin.IsFinal())
            return false;
    return true;
}

//
// Check transaction inputs to mitigate two
// potential denial-of-service attacks:
//
// 1. scriptSigs with extra data stuffed into them,
//    not consumed by scriptPubKey (or P2SH script)
// 2. P2SH scripts with a crazy number of expensive
//    CHECKSIG/CHECKMULTISIG operations
//
bool AreInputsStandard(const CTransaction& tx, const MapPrevTx& mapInputs)
{
    if (tx.IsCoinBase())
        return true; // Coinbases don't use vin normally

    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const CTxOut& prev = tx.GetOutputFor(tx.vin[i], mapInputs);

        vector<vector<unsigned char> > vSolutions;
        txnouttype whichType;
        // get the scriptPubKey corresponding to this input:
        const CScript& prevScript = prev.scriptPubKey;
        if (!Solver(prevScript, whichType, vSolutions))
            return false;
        int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);
        if (nArgsExpected < 0)
            return false;

        // Transactions with extra stuff in their scriptSigs are
        // non-standard. Note that this EvalScript() call will
        // be quick, because if there are any operations
        // beside "push data" in the scriptSig
        // IsStandard() will have already returned false
        // and this method isn't called.
        vector<vector<unsigned char> > stack;
        if (!EvalScript(stack, tx.vin[i].scriptSig, tx, i, SCRIPT_VERIFY_NONE, 0))
            return false;

        if (whichType == TX_SCRIPTHASH)
        {
            if (stack.empty())
                return false;
            CScript subscript(stack.back().begin(), stack.back().end());
            vector<vector<unsigned char> > vSolutions2;
            txnouttype whichType2;
            if (Solver(subscript, whichType2, vSolutions2))
            {
                int tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);

                if (whichType2 == TX_SCRIPTHASH)
                    return false;
                if (tmpExpected < 0)
                    return false;

                nArgsExpected += tmpExpected;
            }
            else
            {
                // Any other Script with less than 15 sigops OK:
                unsigned int sigops = subscript.GetSigOpCount(true);
                // ... extra data left on the stack after execution is OK, too:
                return (sigops <= MAX_P2SH_SIGOPS);
            }
        }

        if (stack.size() != (unsigned int)nArgsExpected)
            return false;
    }

    return true;
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    BOOST_FOREACH(const CTxOut& txout, tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const MapPrevTx& inputs)
{
    if (tx.IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const CTxOut& prevout = tx.GetOutputFor(tx.vin[i], inputs);
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
    }
    return nSigOps;
}

int CMerkleTx::SetMerkleBranch(const CBlock* pblock)
{
    AssertLockHeld(cs_main);

    CBlock blockTmp;
    if (pblock == NULL)
    {
        // Load the block this tx is in
        CTxIndex txindex;
        if (!CTxDB("r").ReadTxIndex(GetHash(), txindex))
            return 0;
        if (!blockTmp.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos))
            return 0;
        pblock = &blockTmp;
    }

    // Update the tx's hashBlock
    hashBlock = pblock->GetHash();

    // Locate the transaction
    for (nIndex = 0; nIndex < (int)pblock->vtx.size(); nIndex++)
        if (pblock->vtx[nIndex] == *(CTransaction*)this)
            break;
    if (nIndex == (int)pblock->vtx.size())
    {
        vMerkleBranch.clear();
        nIndex = -1;
        LogPrintf("ERROR: SetMerkleBranch() : couldn't find tx in block\n");
        return 0;
    }

    // Fill in merkle branch
    vMerkleBranch = pblock->GetMerkleBranch(nIndex);

    // Is the tx in a block that's in the main chain
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    return pindexBest->nHeight - pindex->nHeight + 1;
}

double CTransaction::ComputePriority(double dPriorityInputs, unsigned int nTxSize) const
{
    // In order to avoid disincentivizing cleaning up the UTXO set we don't count
    // the constant overhead for each txin and up to 110 bytes of scriptSig (which
    // is enough to cover a compressed pubkey p2sh redemption) for priority.
    // Providing any more cleanup incentive than making additional inputs free would
    // risk encouraging people to create junk outputs to redeem later.
    if (nTxSize == 0)
        nTxSize = ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION);
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        unsigned int offset = 41U + std::min(110U, (unsigned int)txin.scriptSig.size());
        if (nTxSize > offset)
            nTxSize -= offset;
    }
    if (nTxSize == 0) return 0.0;
    return dPriorityInputs / nTxSize;
}

bool CTransaction::CheckTransaction() const
{
    // Basic checks that don't depend on any context
    if (vin.empty())
        return DoS(10, error("CTransaction::CheckTransaction() : vin empty"));
    if (vout.empty())
        return DoS(10, error("CTransaction::CheckTransaction() : vout empty"));
    // Size limits
    if (::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return DoS(100, error("CTransaction::CheckTransaction() : size limits failed"));

    // Check for negative or overflow output values
    int64_t nValueOut = 0;
    for (unsigned int i = 0; i < vout.size(); i++)
    {
        const CTxOut& txout = vout[i];
        if (txout.IsEmpty() && !IsCoinBase() && !IsCoinStake())
            return DoS(100, error("CTransaction::CheckTransaction() : txout empty for user transaction"));
        if (txout.nValue < 0)
            return DoS(100, error("CTransaction::CheckTransaction() : txout.nValue negative"));
        if (txout.nValue > MAX_MONEY)
            return DoS(100, error("CTransaction::CheckTransaction() : txout.nValue too high"));
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return DoS(100, error("CTransaction::CheckTransaction() : txout total out of range"));
    }

    // Check for duplicate inputs
    set<COutPoint> vInOutPoints;
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        if (vInOutPoints.count(txin.prevout))
            return false;
        vInOutPoints.insert(txin.prevout);
    }

    if (IsCoinBase())
    {
        if (vin[0].scriptSig.size() < 2 || vin[0].scriptSig.size() > 100)
            return DoS(100, error("CTransaction::CheckTransaction() : coinbase script size is invalid"));
    }
    else
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
            if (txin.prevout.IsNull())
                return DoS(10, error("CTransaction::CheckTransaction() : prevout is null"));
    }

    return true;
}

int64_t GetMinFee(const CTransaction& tx, unsigned int nBytes, bool fAllowFree, enum GetMinFee_mode mode)
{
    // Base fee is either MIN_TX_FEE or MIN_RELAY_TX_FEE
    int64_t nBaseFee = (mode == GMF_RELAY) ? MIN_RELAY_TX_FEE : MIN_TX_FEE;

    int64_t nMinFee = (1 + (int64_t)nBytes / 1000) * nBaseFee;

    if (fAllowFree)
    {
        // There is a free transaction area in blocks created by most miners,
        // * If we are relaying we allow transactions up to DEFAULT_BLOCK_PRIORITY_SIZE - 1000
        //   to be considered to fall into this category. We don't want to encourage sending
        //   multiple transactions instead of one big transaction to avoid fees.
        // * If we are creating a transaction we allow transactions up to 1,000 bytes
        //   to be considered safe and assume they can likely make it into this section.
        if (nBytes < (mode == GMF_SEND ? 1000 : (DEFAULT_BLOCK_PRIORITY_SIZE - 1000)))
            nMinFee = 0;
    }

    // This code can be removed after enough miners have upgraded to version 0.9.
    // Until then, be safe when sending and require a fee if any output
    // is less than CENT:
    if (nMinFee < nBaseFee && mode == GMF_SEND)
    {
        BOOST_FOREACH(const CTxOut& txout, tx.vout)
            if (txout.nValue < CENT)
                nMinFee = nBaseFee;
    }

    if (!MoneyRange(nMinFee))
        nMinFee = MAX_MONEY;
    return nMinFee;
}


bool AcceptToMemoryPool(CTxMemPool& pool, CTransaction &tx, bool fLimitFree,
                        bool* pfMissingInputs, bool fRejectInsaneFee, bool ignoreFees)
{
    AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;

    if (!tx.CheckTransaction())
        return error("AcceptToMemoryPool : CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return tx.DoS(100, error("AcceptToMemoryPool : coinbase as individual tx"));

    // ppcoin: coinstake is also only valid in a block, not as a loose transaction
    if (tx.IsCoinStake())
        return tx.DoS(100, error("AcceptToMemoryPool : coinstake as individual tx"));

    // Rather not work on nonstandard transactions (unless -testnet)
    string reason;
    if (!TestNet() && !IsStandardTx(tx, reason))
        return error("AcceptToMemoryPool : nonstandard transaction: %s",
                     reason);

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash))
        return false;

    // ----------- instantX transaction scanning -----------

    /*BOOST_FOREACH(const CTxIn& in, tx.vin){
        if(mapLockedInputs.count(in.prevout)){
            if(mapLockedInputs[in.prevout] != tx.GetHash()){
                return tx.DoS(0, error("AcceptToMemoryPool : conflicts with existing transaction lock: %s", reason));
            }
        }
    }*/

    // Check for conflicts with in-memory transactions
    {
    LOCK(pool.cs); // protect pool.mapNextTx
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        COutPoint outpoint = tx.vin[i].prevout;
        if (pool.mapNextTx.count(outpoint))
        {
            // Disable replacement feature for now
           // return false;
        }
    }
    }

    {
        CTxDB txdb("r");

        // do we already have it?
        if (txdb.ContainsTx(hash))
            return false;

        // do all inputs exist?
        // Note that this does not check for the presence of actual outputs (see the next check for that),
        // only helps filling in pfMissingInputs (to determine missing vs spent).
        BOOST_FOREACH(const CTxIn txin, tx.vin) {
            if (!txdb.ContainsTx(txin.prevout.hash)) {
                if (pfMissingInputs)
                    *pfMissingInputs = true;
                return false;
            }
        }
        MapPrevTx mapInputs;
        map<uint256, CTxIndex> mapUnused;
        bool fInvalid = false;
        if (!tx.FetchInputs(txdb, mapUnused, false, false, mapInputs, fInvalid))
        {
            if (fInvalid)
                return error("AcceptToMemoryPool : FetchInputs found invalid tx %s", hash.ToString());
            return false;
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (!TestNet() && !AreInputsStandard(tx, mapInputs))
            return error("AcceptToMemoryPool : nonstandard transaction input");

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        unsigned int nSigOps = GetLegacySigOpCount(tx);
        nSigOps += GetP2SHSigOpCount(tx, mapInputs);
        if (nSigOps > MAX_TX_SIGOPS)
            return tx.DoS(0,
                          error("AcceptToMemoryPool : too many sigops %s, %d > %d",
                                hash.ToString(), nSigOps, MAX_TX_SIGOPS));

        int64_t nFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();
        unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);

        // Don't accept it if it can't get into a block
        // but prioritise dstx and don't check fees for it
        if(mapDarksendBroadcastTxes.count(hash)) {
            // Normally we would PrioritiseTransaction But currently it is unimplemented
            // mempool.PrioritiseTransaction(hash, hash.ToString(), 1000, 0.1*COIN);
        } else if(!ignoreFees){
            int64_t txMinFee = GetMinFee(tx, nSize, true, GMF_RELAY);
            if (fLimitFree && nFees < txMinFee)
                return error("AcceptToMemoryPool : not enough fees %s, %d < %d",
                            hash.ToString(),
                            nFees, txMinFee);

            // Continuously rate-limit free transactions
            // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
            // be annoying or make others' transactions take longer to confirm.
            if (fLimitFree && nFees < MIN_RELAY_TX_FEE)
            {
                static CCriticalSection csFreeLimiter;
                static double dFreeCount;
                static int64_t nLastTime;
                int64_t nNow = GetTime();

                LOCK(csFreeLimiter);

                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;
                // -limitfreerelay unit is thousand-bytes-per-minute
                // At default rate it would take over a month to fill 1GB
                if (dFreeCount > GetArg("-limitfreerelay", 15)*10*1000)
                    return error("AcceptableInputs : free transaction rejected by rate limiter");
                LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
                dFreeCount += nSize;
            }
        }

        if (fRejectInsaneFee && nFees > MIN_RELAY_TX_FEE * 100000)          //increase min tx size for POS wallets with many small inputs
            return error("AcceptableInputs: : insane fees %s, %d > %d",
                         hash.ToString(),
                         nFees, MIN_RELAY_TX_FEE * 100000);

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!tx.ConnectInputs(txdb, mapInputs, mapUnused, CDiskTxPos(1,1,1), pindexBest, false, false, STANDARD_SCRIPT_VERIFY_FLAGS))
        {
            return error("AcceptToMemoryPool : ConnectInputs failed %s", hash.ToString());
        }

        // Check again against just the consensus-critical mandatory script
        // verification flags, in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks, however allowing such transactions into the mempool
        // can be exploited as a DoS attack.
        if (!tx.ConnectInputs(txdb, mapInputs, mapUnused, CDiskTxPos(1,1,1), pindexBest, false, false, MANDATORY_SCRIPT_VERIFY_FLAGS))
        {
            return error("AcceptToMemoryPool: : BUG! PLEASE REPORT THIS! ConnectInputs failed against MANDATORY but not STANDARD flags %s", hash.ToString());
        }
    }

    // Store transaction in memory
    pool.addUnchecked(hash, tx);
    setValidatedTx.insert(hash);

    SyncWithWallets(tx, NULL);

    LogPrint("mempool", "AcceptToMemoryPool : accepted %s (poolsz %u)\n",
           hash.ToString(),
           pool.mapTx.size());

    return true;
}

bool AcceptableInputs(CTxMemPool& pool, const CTransaction &txo, bool fLimitFree,
                         bool* pfMissingInputs, bool fRejectInsaneFee, bool isDSTX)
{
    //AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;

    CTransaction tx(txo);
    string reason;

    if (!tx.CheckTransaction())
        return error("AcceptableInputs : CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return tx.DoS(100, error("AcceptableInputs : coinbase as individual tx"));

    // ppcoin: coinstake is also only valid in a block, not as a loose transaction
    if (tx.IsCoinStake())
        return tx.DoS(100, error("AcceptableInputs : coinstake as individual tx"));

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash))
        return false;

    // ----------- instantX transaction scanning -----------

    BOOST_FOREACH(const CTxIn& in, tx.vin){
        if(mapLockedInputs.count(in.prevout)){
            if(mapLockedInputs[in.prevout] != tx.GetHash()){
                return tx.DoS(0, error("AcceptableInputs : conflicts with existing transaction lock: %s", reason));
            }
        }
    }

    // Check for conflicts with in-memory transactions
    {
    LOCK(pool.cs); // protect pool.mapNextTx
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        COutPoint outpoint = tx.vin[i].prevout;
        if (pool.mapNextTx.count(outpoint))
        {
            // Disable replacement feature for now
            return false;
        }
    }
    }

    {
        CTxDB txdb("r");

        // do we already have it?
        if (txdb.ContainsTx(hash))
            return false;

        MapPrevTx mapInputs;
        map<uint256, CTxIndex> mapUnused;
        bool fInvalid = false;
        if (!tx.FetchInputs(txdb, mapUnused, false, false, mapInputs, fInvalid))
        {
            if (fInvalid)
                return error("AcceptableInputs : FetchInputs found invalid tx %s", hash.ToString());
            return false;
        }

        // Check for non-standard pay-to-script-hash in inputs
        //if (!TestNet() && !AreInputsStandard(tx, mapInputs))
          //  return error("AcceptToMemoryPool : nonstandard transaction input");

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        unsigned int nSigOps = GetLegacySigOpCount(tx);
        nSigOps += GetP2SHSigOpCount(tx, mapInputs);
        if (nSigOps > MAX_TX_SIGOPS)
            return tx.DoS(0,
                          error("AcceptableInputs : too many sigops %s, %d > %d",
                                hash.ToString(), nSigOps, MAX_TX_SIGOPS));

        int64_t nFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();
        unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
        int64_t txMinFee = GetMinFee(tx, nSize, true, GMF_RELAY);

        // Don't accept it if it can't get into a block
        if(isDSTX) {
            // Normally we would PrioritiseTransaction But currently it is unimplemented
            // mempool.PrioritiseTransaction(hash, hash.ToString(), 1000, 0.1*COIN);
        } else { // same as !ignoreFees for AcceptToMemoryPool
            if (fLimitFree && nFees < txMinFee)
                return error("AcceptableInputs : not enough fees %s, %d < %d",
                            hash.ToString(),
                            nFees, txMinFee);

            // Continuously rate-limit free transactions
            // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
            // be annoying or make others' transactions take longer to confirm.
            if (fLimitFree && nFees < MIN_RELAY_TX_FEE)
            {
                static CCriticalSection csFreeLimiter;
                static double dFreeCount;
                static int64_t nLastTime;
                int64_t nNow = GetTime();

                LOCK(csFreeLimiter);

                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;
                // -limitfreerelay unit is thousand-bytes-per-minute
                // At default rate it would take over a month to fill 1GB
                if (dFreeCount > GetArg("-limitfreerelay", 15)*10*1000)
                    return error("AcceptableInputs : free transaction rejected by rate limiter");
                LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
                dFreeCount += nSize;
            }
        }

        if (fRejectInsaneFee && nFees > txMinFee * 10000)
            return error("AcceptableInputs: : insane fees %s, %d > %d",
                         hash.ToString(),
                         nFees, MIN_RELAY_TX_FEE * 10000);

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!tx.ConnectInputs(txdb, mapInputs, mapUnused, CDiskTxPos(1,1,1), pindexBest, true, false, STANDARD_SCRIPT_VERIFY_FLAGS, false))
        {
            return error("AcceptableInputs : ConnectInputs failed %s", hash.ToString());
        }
    }


    /*LogPrint("mempool", "AcceptableInputs : accepted %s (poolsz %u)\n",
           hash.ToString(),
           pool.mapTx.size());
    */
    return true;
}


int CMerkleTx::GetDepthInMainChainINTERNAL(CBlockIndex* &pindexRet) const
{
    if (hashBlock == 0 || nIndex == -1)
        return 0;
    AssertLockHeld(cs_main);

    // Find the block it claims to be in
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    // Make sure the merkle branch connects to this block
    if (!fMerkleVerified)
    {
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            return 0;
        fMerkleVerified = true;
    }

    pindexRet = pindex;
    return pindexBest->nHeight - pindex->nHeight + 1;
}


int CMerkleTx::GetDepthInMainChain(CBlockIndex* &pindexRet, bool enableIX) const
{
    AssertLockHeld(cs_main);
    int nResult = GetDepthInMainChainINTERNAL(pindexRet);
    if (nResult == 0 && !mempool.exists(GetHash()))
        return -1; // Not in chain, not in mempool

    return nResult;
}

int CMerkleTx::GetBlocksToMaturity() const
{
    if (!(IsCoinBase() || IsCoinStake()))
        return 0;
    return max(0, (nCoinbaseMaturity+1) - GetDepthInMainChain());
}


bool CMerkleTx::AcceptToMemoryPool(bool fLimitFree, bool fRejectInsaneFee, bool ignoreFees)
{
    return ::AcceptToMemoryPool(mempool, *this, fLimitFree, NULL, fRejectInsaneFee, ignoreFees);
}



bool CWalletTx::AcceptWalletTransaction(CTxDB& txdb)
{
    {
        // Add previous supporting transactions first
        BOOST_FOREACH(CMerkleTx& tx, vtxPrev)
        {
            if (!(tx.IsCoinBase() || tx.IsCoinStake()))
            {
                uint256 hash = tx.GetHash();
                if (!mempool.exists(hash) && !txdb.ContainsTx(hash))
                    tx.AcceptToMemoryPool(false);
            }
        }
        return AcceptToMemoryPool(false);
    }
    return false;
}

bool CWalletTx::AcceptWalletTransaction()
{
    CTxDB txdb("r");
    return AcceptWalletTransaction(txdb);
}

int GetInputAge(CTxIn& vin)
{
    const uint256& prevHash = vin.prevout.hash;
    CTransaction tx;
    uint256 hashBlock;
    bool fFound = GetTransaction(prevHash, tx, hashBlock, false);
    if(fFound)
    {
    if(mapBlockIndex.find(hashBlock) != mapBlockIndex.end())
    {
        return pindexBest->nHeight - mapBlockIndex[hashBlock]->nHeight;
    }
    else
        return 0;
    }
    else
        return 0;
}


int GetInputAgeIX(uint256 nTXHash, CTxIn& vin)
{
    int sigs = 0;
    int nResult = GetInputAge(vin);
    if(nResult < 0) nResult = 0;

    if (nResult < 6){
        std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(nTXHash);
        if (i != mapTxLocks.end()){
            sigs = (*i).second.CountSignatures();
        }
        if(sigs >= INSTANTX_SIGNATURES_REQUIRED){
            return nInstantXDepth+nResult;
        }
    }

    return -1;
}

int GetIXConfirmations(uint256 nTXHash)
{
    int sigs = 0;

    std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(nTXHash);
    if (i != mapTxLocks.end()){
        sigs = (*i).second.CountSignatures();
    }
    if(sigs >= INSTANTX_SIGNATURES_REQUIRED){
        return nInstantXDepth;
    }

    return 0;
}

int CTxIndex::GetDepthInMainChain() const
{
    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(pos.nFile, pos.nBlockPos, false))
        return 0;
    // Find the block in the index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.GetHash());
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;
    return 1 + nBestHeight - pindex->nHeight;
}

// Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool GetTransaction(const uint256 &hash, CTransaction &tx, uint256 &hashBlock, bool fCheckMempool)
{
    {
        if(fCheckMempool) // avoid the mempool check and lock on cs_main if called from the masternode message handler
        {
            LOCK(cs_main);
            {
                if (mempool.lookup(hash, tx))
                {
                    return true;
                }
            }
        }
        CTxDB txdb("r");
        CTxIndex txindex;
        if (tx.ReadFromDisk(txdb, hash, txindex))
        {
            CBlock block;
            if (block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
                hashBlock = block.GetHash();
            return true;
        }
        // look for transaction in disconnected blocks to find orphaned CoinBase and CoinStake transactions
        BOOST_FOREACH(PAIRTYPE(const uint256, CBlockIndex*)& item, mapBlockIndex)
        {
            CBlockIndex* pindex = item.second;
            if (pindex == pindexBest || pindex->pnext != 0)
                continue;
            CBlock block;
            if (!block.ReadFromDisk(pindex))
                continue;
            BOOST_FOREACH(const CTransaction& txOrphan, block.vtx)
            {
                if (txOrphan.GetHash() == hash)
                {
                    tx = txOrphan;
                    return true;
                }
            }
        }
    }
    return false;
}

//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

static CBlockIndex* pblockindexFBBHLast;
CBlockIndex* FindBlockByHeight(int nHeight)
{
    CBlockIndex *pblockindex;
    if (nHeight < nBestHeight / 2)
        pblockindex = pindexGenesisBlock;
    else
        pblockindex = pindexBest;
    if (pblockindexFBBHLast && abs(nHeight - pblockindex->nHeight) > abs(nHeight - pblockindexFBBHLast->nHeight))
        pblockindex = pblockindexFBBHLast;
    while (pblockindex->nHeight > nHeight)
        pblockindex = pblockindex->pprev;
    while (pblockindex->nHeight < nHeight)
        pblockindex = pblockindex->pnext;
    pblockindexFBBHLast = pblockindex;
    return pblockindex;
}

bool CBlock::ReadFromDisk(const CBlockIndex* pindex, bool fReadTransactions)
{
    if (!fReadTransactions)
    {
        *this = pindex->GetBlockHeader();
        return true;
    }
    if (!ReadFromDisk(pindex->nFile, pindex->nBlockPos, fReadTransactions))
        return false;
    if (GetHash() != pindex->GetBlockHash())
        return error("CBlock::ReadFromDisk() : GetHash() doesn't match index");
    return true;
}

uint256 static GetOrphanRoot(const uint256& hash)
{
    map<uint256, COrphanBlock*>::iterator it = mapOrphanBlocks.find(hash);
    if (it == mapOrphanBlocks.end())
        return hash;

    // Work back to the first block in the orphan chain
    do {
        map<uint256, COrphanBlock*>::iterator it2 = mapOrphanBlocks.find(it->second->hashPrev);
        if (it2 == mapOrphanBlocks.end())
            return it->first;
        it = it2;
    } while(true);
}

// ppcoin: find block wanted by given orphan block
uint256 WantedByOrphan(const COrphanBlock* pblockOrphan)
{
    // Work back to the first block in the orphan chain
    while (mapOrphanBlocks.count(pblockOrphan->hashPrev))
        pblockOrphan = mapOrphanBlocks[pblockOrphan->hashPrev];
    return pblockOrphan->hashPrev;
}

// Remove a random orphan block (which does not have any dependent orphans).
void static PruneOrphanBlocks()
{
    if (mapOrphanBlocksByPrev.size() <= (size_t)std::max((int64_t)0, GetArg("-maxorphanblocks", DEFAULT_MAX_ORPHAN_BLOCKS)))
        return;

    // Pick a random orphan block.
    int pos = insecure_rand() % mapOrphanBlocksByPrev.size();
    std::multimap<uint256, COrphanBlock*>::iterator it = mapOrphanBlocksByPrev.begin();
    while (pos--) it++;

    // As long as this block has other orphans depending on it, move to one of those successors.
    do {
        std::multimap<uint256, COrphanBlock*>::iterator it2 = mapOrphanBlocksByPrev.find(it->second->hashBlock);
        if (it2 == mapOrphanBlocksByPrev.end())
            break;
        it = it2;
    } while(1);

    setStakeSeenOrphan.erase(it->second->stake);
    uint256 hash = it->second->hashBlock;
    delete it->second;
    mapOrphanBlocksByPrev.erase(it);
    mapOrphanBlocks.erase(hash);
}

static CBigNum GetProofOfStakeLimit(int nHeight)
{
    return bnProofOfStakeLimit;
}

// miner's coin base reward
int64_t GetProofOfWorkReward(int nHeight, int64_t nFees)
{
int64_t nSubsidy = 20 * COIN;

  if (nHeight <= 1)
  {
    nSubsidy = 0 * COIN;
  }
  else if(nHeight < 4)
  {
    nSubsidy = 281169 * COIN; // PEPE development tendies fund, two 281195 blocks total 562390 PEPE or 3.5% of POW
  }
  else if(nHeight < 100)  // confirm tendies fund
  {
    nSubsidy = 0 * COIN;
  }
  else if(nHeight < 900)
  {
    nSubsidy = 1500 * COIN;
  }
  else if(nHeight < 1300)
  {
    nSubsidy = 1200 * COIN;
  }
  else if(nHeight < 1900)
  {
    nSubsidy = 1050 * COIN;
  }
  else if(nHeight < 2900)
  {
    nSubsidy = 900 * COIN;
  }
  else if(nHeight < 4100)
  {
    nSubsidy = 700 * COIN;
  }
  else if(nHeight < 6680)
  {
    nSubsidy = 400 * COIN;
  }
  else if(nHeight < 9060)
  {
    nSubsidy = 500 * COIN;
  }
  else if(nHeight < 11840)
  {
    nSubsidy = 300 * COIN;
  }
  else if(nHeight < 14420)
  {
    nSubsidy = 200 * COIN;
  }
  else if(nHeight < 17000)
  {
    nSubsidy = 300 * COIN;
  }
  else if(nHeight < 19580)
  {
    nSubsidy = 100 * COIN;
  }
  else if(nHeight < 22160)
  {
    nSubsidy = 200 * COIN;
  }
  else if(nHeight < 24740)
  {
    nSubsidy = 90 * COIN;
  }
  else if(nHeight < 27320)
  {
    nSubsidy = 130 * COIN;
  }
  else if(nHeight < 29900)
  {
    nSubsidy = 80 * COIN;
  }
  else if(nHeight < 32480)
  {
    nSubsidy = 100 * COIN;
  }
  else if(nHeight < 35060)
  {
    nSubsidy = 70 * COIN;
  }
  else if(nHeight < 37640)
  {
    nSubsidy = 100 * COIN;
  }
  else if(nHeight < 40220)
  {
    nSubsidy = 60 * COIN;
  }
  else if(nHeight < 42800)
  {
    nSubsidy = 100 * COIN;
  }
  else if(nHeight < 45380)
  {
    nSubsidy = 50 * COIN;
  }
  else if(nHeight < 47960)
  {
    nSubsidy = 100 * COIN;
  }
  else if(nHeight < 50540)
  {
    nSubsidy = 40 * COIN;
  }
  else if(nHeight < 53120)
  {
    nSubsidy = 100 * COIN;
  }
  else if(nHeight < 55700)
  {
    nSubsidy = 40 * COIN;
  }
  else if(nHeight < 58280)
  {
    nSubsidy = 100 * COIN;
  }
  else if(nHeight < 60860)
  {
    nSubsidy = 40 * COIN;
  }
  else if(nHeight < 63440)
  {
    nSubsidy = 100 * COIN;
  }
  else if(nHeight < 66020)
  {
    nSubsidy = 40 * COIN;
  }
  else if(nHeight < 68600)
  {
    nSubsidy = 100 * COIN;
  }
  else if(nHeight < 71180)
  {
    nSubsidy = 40 * COIN;
  }
  else if(nHeight < 73760)
  {
    nSubsidy = 90 * COIN;
  }
  else if(nHeight < 76340)
  {
    nSubsidy = 30 * COIN;
  }
  else if(nHeight < 78920)
  {
    nSubsidy = 80 * COIN;
  }
  else if(nHeight < 81500)
  {
    nSubsidy = 30 * COIN;
  }
  else if(nHeight < 84080)
  {
    nSubsidy = 70 * COIN;
  }
  else if(nHeight < 86660)
  {
    nSubsidy = 30 * COIN;
  }
  else if(nHeight < 89240)
  {
    nSubsidy = 60 * COIN;
  }
  else if(nHeight < 91820)
  {
    nSubsidy = 30 * COIN;
  }
  else if(nHeight < 94400)
  {
    nSubsidy = 50 * COIN;
  }
  else if(nHeight < 96980)
  {
    nSubsidy = 30 * COIN;
  }
  else if(nHeight < 99560)
  {
    nSubsidy = 50 * COIN;
  }
  else if(nHeight < 102140)
  {
    nSubsidy = 30 * COIN;
  }
  else if(nHeight < 104720)
  {
    nSubsidy = 50 * COIN;
  }
  else if(nHeight < 107300)
  {
    nSubsidy = 20 * COIN;
  }
  else if(nHeight < 109880)
  {
    nSubsidy = 40 * COIN;
  }
  else if(nHeight < 112460)
  {
    nSubsidy = 20 * COIN;
  }
  else if(nHeight < 115040)
  {
    nSubsidy = 30 * COIN;
  }
  else if(nHeight < 117620)
  {
    nSubsidy = 20 * COIN;
  }
  else if(nHeight < 120200)
  {
    nSubsidy = 25 * COIN;
  }
  else if(nHeight < 122780)
  {
    nSubsidy = 15 * COIN;
  }
  else if(nHeight < 125360)
  {
    nSubsidy = 20 * COIN;
  }
  else if(nHeight < 127940)
  {
    nSubsidy = 15 * COIN;
  }
  else if(nHeight < 130421)
  {
    nSubsidy = 20 * COIN;
  }
  else if(nHeight > RESTART_POW_BLOCK && nHeight < PEPE_REBRAND_HEIGHT)
  {
    nSubsidy = 10 * COIN;
    nSubsidy >>= (nHeight / 131400); // block reward halves roughly four times a year a year (every 365/4 days at 1440 blocks per day)
  }
  else if(nHeight >= PEPE_REBRAND_HEIGHT)
  {
  	  // subsidy starts at 7 Coins per block for 43,200 blocks
  	  if(nHeight <= (PEPE_REBRAND_HEIGHT + 43200))
  	  {
  	  	nSubsidy = 7 * COIN;
  	  }
  	  else
  	  {
  	  	nSubsidy = 15 * COIN;  
  	  	nSubsidy >>= ((nHeight - PEPE_REBRAND_HEIGHT) / 525600); // block reward halves once a year
  	  }
    
      if(nHeight == PEPE_REBRAND_PF_HEIGHT)
        nSubsidy += (3 * PEPE_DEV_GRANT);
      if(nHeight == PEPE_KEKDAQ_MID_FIX_HEIGHT)
        nSubsidy += (3 * PEPE_DEV_GRANT_MID);
      if(nHeight == PEPE_IPFSMN_FNL_HEIGHT)
        nSubsidy += (3 * PEPE_DEV_GRANT_FINAL);
      if(nHeight == PEPE_STAKE_CONF_HEIGHT)
        nSubsidy += (3 * PEPE_DEV_GRANT);
      if(nHeight == PEPE_KEKDAQ2_SWAP_HEIGHT)
        nSubsidy += (3 * DEVFEE_OFF_SWAP_FINAL);

  }

  	return nSubsidy + nFees;

}

  int GetPIRRewardPhase(int64_t nHeight)
  {
     int64_t Phase0StartHeight = 9000;
     int phase = (int)( (nHeight-Phase0StartHeight) / PIR_PHASEBLOCKS);
     return min(PIR_PHASES-1, max(0,phase) );
  }


  int64_t GetPIRRewardCoinYear(int64_t nCoinValue, int64_t nHeight)
  {

      // work out which phase rates we should use, based on the block height
      int nPhase = GetPIRRewardPhase(nHeight);

      // find the % band that contains the staked value
      if (nCoinValue >= PIR_THRESHOLDS[PIR_LEVELS-1] * COIN)
          return PIR_RATES[nPhase][PIR_LEVELS-1]  * CENT;

      int nLevel = 0;
      for (int i = 1; i<PIR_LEVELS; i++)
      {
          if (nCoinValue < PIR_THRESHOLDS[i] * COIN)
          {
                  nLevel = i-1;
                  break;
          };
      };

      // interpolate the PSR for this staked value
      // a simple way to interpolate this using integer math is to break the range into 100 slices and find the slice where our coin value lies
      // Rates and Thresholds are integers, CENT and COIN are multiples of 100, so using 100 slices does not introduce any integer math rounding errors

      int64_t nLevelRatePerSlice = (( PIR_RATES[nPhase][nLevel+1] - PIR_RATES[nPhase][nLevel] ) * CENT )  / 100;
      int64_t nLevelValuePerSlice = (( PIR_THRESHOLDS[nLevel+1] - PIR_THRESHOLDS[nLevel] ) * COIN ) / 100;

      int64_t nTestValue = PIR_THRESHOLDS[nLevel] * COIN;

      int64_t nRewardCoinYear = PIR_RATES[nPhase][nLevel] * CENT;
      while (nTestValue < nCoinValue)
      {
          nTestValue += nLevelValuePerSlice;
          nRewardCoinYear += nLevelRatePerSlice;
      };

      return nRewardCoinYear;

  }

  int64_t GetProofOfStakeReward(const CBlockIndex* pindexPrev, int64_t nCoinAge, int64_t nFees, int64_t nCoinValue)
  {
        if((!TestNet() && pindexPrev->nHeight < PEPE_STAKE_V2_SWITCH_HEIGHT) ||
            (TestNet() && pindexPrev->nHeight < PEPE_STAKE_V2_SWITCH_HEIGHT_TESTNET))
        {
            return GetProofOfStakeRewardV1(pindexPrev, nCoinAge, nFees, nCoinValue);
        }
        else if((!TestNet() && pindexPrev->nHeight > PEPE_REBRAND_HEIGHT) ||
        	(TestNet() && pindexPrev->nHeight > PEPE_REBRAND_HEIGHT_TESTNET))
        {
        	return GetProofOfStakeRewardV3(pindexPrev, nCoinAge, nFees, nCoinValue);
        }
        else
        {
            return GetProofOfStakeRewardV2(pindexPrev, nCoinAge, nFees, nCoinValue);
        }
  }

  // PEPE rebrand 7% pos
  int64_t GetProofOfStakeRewardV3(const CBlockIndex* pindexPrev, int64_t nCoinAge, int64_t nFees, int64_t nCoinValue)
  {
        int64_t nHeight = pindexPrev->nHeight;
        int64_t nRewardCoinYear = 7 * CENT; // 7% per year

        // Now calculate the reward
        int64_t nSubsidy = nCoinAge * nRewardCoinYear * 33 / (365 * 33 + 8); //integer equivalent of nCoinAge * nRewardCoinYear / 365.2424242..
     
        if (nHeight >= PEPE_JACKOLANTERN_FORK_HEIGHT)
            nSubsidy = 1 * COIN;    // Minimum 1 PEPE stake return for optimal KEKDAQ functionality.

        if (nHeight >= PEPE_STAKEONLY_HEIGHT)
        {
            // adopt the proof-of-work reward schedule when swap over to stake only
            // this will pick up the current pow reward of 3.25 at fork height with halving
            nSubsidy = 15 * COIN;  
            nSubsidy >>= ((nHeight - PEPE_STAKEONLY_HEIGHT) / 525600); // block reward halves once a year

            if(nSubsidy < 1 * COIN)  // minimum 1 PEPE stake return
                nSubsidy = 1 * COIN;
        }

        if(nHeight+1 == PEPE_REBRAND_PF_HEIGHT)
            nSubsidy += (3 * PEPE_DEV_GRANT);
        if(nHeight+1 == PEPE_KEKDAQ_MID_FIX_HEIGHT)
            nSubsidy += (3 * PEPE_DEV_GRANT_MID);
        if(nHeight+1 == PEPE_IPFSMN_FNL_HEIGHT)
            nSubsidy += (3 * PEPE_DEV_GRANT_FINAL);
        if(nHeight+1 == PEPE_STAKE_CONF_HEIGHT)
            nSubsidy += (3 * PEPE_DEV_GRANT);
        if(nHeight+1 == PEPE_KEKDAQ2_SWAP_HEIGHT)
            nSubsidy += (3 * DEVFEE_OFF_SWAP_FINAL);
        if(nHeight+1 == PEPE_STAKEONLY_HEIGHT)
            nSubsidy += (4 * PEPE_SO_SWAP_GRANT);

        return nSubsidy + nFees;
  }

  // PEPE stake fix
  // Not used anymore
  int64_t GetProofOfStakeRewardV2(const CBlockIndex* pindexPrev, int64_t nCoinAge, int64_t nFees, int64_t nCoinValue)
  {
        int64_t nHeight = pindexPrev->nHeight;
        int64_t nRewardCoinYear = 1 * CENT; // 1% per year


        // PEPE Stake Reward table
        // Amount       Year 1      Year 2      Year 3+
        // 0-10,000      3%          2%          1%
        // 10,000+       4%          3%          2%
        // 50,000+       5%          4%          3%
        // 100,000+      6%          5%          4%

        // What phase/year is the block in?
        int64_t blocksInAYear = 60 * 24 * 365;  // 60s blocks, 24 hours in a day, 365 days in a year
        int64_t nStakeStartHeight = Params().POSStartBlock(); // POS started at block 9000 on mainnet, 20 on testnet
        int nBlockYear = (int)((nHeight - nStakeStartHeight)/blocksInAYear);
        if(nBlockYear == 1)
        {
            // get rate based on amount
            if(nCoinValue <= 10000 * COIN)
                nRewardCoinYear = 3 * CENT; // 3% per year
            else if(nCoinValue <= 50000 * COIN)
                nRewardCoinYear = 4 * CENT; // 4% per year
            else if(nCoinValue < 100000 * COIN)
                nRewardCoinYear = 5 * CENT; // 5% per year
            else
                nRewardCoinYear = 6 * CENT; // 6% per year
        }
        else if(nBlockYear == 2)
        {
            // get rate based on amount
            if(nCoinValue <= 10000 * COIN)
                nRewardCoinYear = 2 * CENT; // 2% per year
            else if(nCoinValue <= 50000 * COIN)
                nRewardCoinYear = 3 * CENT; // 3% per year
            else if(nCoinValue < 100000 * COIN)
                nRewardCoinYear = 4 * CENT; // 4% per year
            else
                nRewardCoinYear = 5 * CENT; // 5% per year
        }
        else // year 3+
        {
            // get rate based on amount
            if(nCoinValue <= 10000 * COIN)
                nRewardCoinYear = 1 * CENT; // 1% per year
            else if(nCoinValue <= 50000 * COIN)
                nRewardCoinYear = 2 * CENT; // 2% per year
            else if(nCoinValue < 100000 * COIN)
                nRewardCoinYear = 3 * CENT; // 3% per year
            else
                nRewardCoinYear = 4 * CENT; // 4% per year
        }

        // Now calculate the reward
        int64_t nSubsidy = nCoinAge * nRewardCoinYear * 33 / (365 * 33 + 8); //integer equivalent of nCoinAge * nRewardCoinYear / 365.2424242..

        if(nHeight > RESTART_POW_BLOCK)
        {

              if (nSubsidy > 10)
                  nSubsidy = 10;

              if (nSubsidy < 0.5)
                  nSubsidy = 0.5;
        }

      return nSubsidy + nFees;
  }

  int64_t GetProofOfStakeRewardV1(const CBlockIndex* pindexPrev, int64_t nCoinAge, int64_t nFees, int64_t nCoinValue)
  {

      int64_t nHeight = pindexPrev->nHeight;
      int64_t nRewardCoinYear = GetPIRRewardCoinYear(nCoinValue, nHeight);

      int64_t nSubsidy = nCoinAge * nRewardCoinYear * 33 / (365 * 33 + 8); //integer equivalent of nCoinAge * nRewardCoinYear / 365.2424242..

      if (nSubsidy > 420)
        nSubsidy = 420;

      if (nSubsidy < 0)
        nSubsidy = 0;

      return nSubsidy + nFees;
  }



static int64_t nTargetTimespan = 10 * 60;  // 10 mins stake difficulty retargeting


// ppcoin: find last block index up to pindex
const CBlockIndex* GetLastBlockIndex(const CBlockIndex* pindex, bool fProofOfStake)
{
    while (pindex && pindex->pprev && (pindex->IsProofOfStake() != fProofOfStake))
        pindex = pindex->pprev;
    return pindex;
}

unsigned int GetNextTargetRequired(const CBlockIndex* pindexLast, bool fProofOfStake)
{
    // if(pindexLast->nHeight >= PEPE_JACKOLANTERN_FORK_HEIGHT)
    //    nTargetTimespan = 2 * 60;

    nTargetTimespan = 10 * 60;  // 10 mins stake difficulty retargeting

    if(pindexLast->nHeight >= PEPE_NTARGETTIMESPAN_UNSTICK_HEIGHT)
        nTargetTimespan = 2 * 60;
    
    // set fork-fix timespan back to 10 minutes for these blocks, to pass POW block sync sticking issues    
    if(pindexLast->nHeight >= 886415 && pindexLast->nHeight <=888057)  
        nTargetTimespan = 10 * 60;

    // POS difficulty timespan set back to 10 minutes to lessen difficulty swings after block 1823000
    if(pindexLast->nHeight >= PEPE_DIFFIMPROVE_HEIGHT )
        nTargetTimespan = 10 * 60;
        
    CBigNum bnTargetLimit = fProofOfStake ? GetProofOfStakeLimit(pindexLast->nHeight) : Params().ProofOfWorkLimit();

    if (pindexLast == NULL || TestNet()) 
        return bnTargetLimit.GetCompact(); // genesis block

    const CBlockIndex* pindexPrev = GetLastBlockIndex(pindexLast, fProofOfStake);
    if (pindexPrev->pprev == NULL)
        return bnTargetLimit.GetCompact(); // first block
    const CBlockIndex* pindexPrevPrev = GetLastBlockIndex(pindexPrev->pprev, fProofOfStake);
    if (pindexPrevPrev->pprev == NULL)
        return bnTargetLimit.GetCompact(); // second block

    int64_t nActualSpacing = pindexPrev->GetBlockTime() - pindexPrevPrev->GetBlockTime();

        if (nActualSpacing < 0)
            nActualSpacing = TARGET_SPACING;

    // ppcoin: target change every block
    // ppcoin: retarget with exponential moving toward target spacing
    CBigNum bnNew;
    bnNew.SetCompact(pindexPrev->nBits);


    int64_t nInterval = nTargetTimespan / TARGET_SPACING;
    bnNew *= ((nInterval - 1) * TARGET_SPACING + nActualSpacing + nActualSpacing);
    bnNew /= ((nInterval + 1) * TARGET_SPACING);


    if (bnNew <= 0 || bnNew > bnTargetLimit)
        bnNew = bnTargetLimit;

    return bnNew.GetCompact();
}


bool CheckProofOfWork(uint256 hash, unsigned int nBits)
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Check range
    if (bnTarget <= 0 || bnTarget > Params().ProofOfWorkLimit())
        return error("CheckProofOfWork() : nBits below minimum work");

    // Check proof of work matches claimed amount
    if (hash > bnTarget.getuint256())
        return error("CheckProofOfWork() : hash doesn't match nBits");

    return true;
}

static int nAskedForBlocks = 0;

bool IsSyncing()
{
    int nSyncSpan = GetArg("-syncspan", 90);    
    return pindexBest->GetBlockTime() < GetTime() - nSyncSpan * 60;
}

void DropNonRespondingSyncPeer()
{
    // detect if the currently connected sync node has not started sending blocks
    // within timeout period, and if so disconnect it so we can try to sync
    // from a new peer       
    BOOST_FOREACH(CNode* pnode, vNodes) {
        if(pnode->fStartSync && !pnode->fDisconnect && IsSyncing())
        {            
            int nSyncTimeout = GetArg("-synctimeout", 60);
            int64_t tNow = GetTimeMillis();
            if (pnode->tGetblocks) {
                if (pnode->tGetblocks > pnode->tBlockInvs && tNow-pnode->tGetblocks > nSyncTimeout * 1000) {
                    LogPrintf("sync peer=%d: Block sync did not start within %d seconds. Disconnecting peer.\n", pnode->id, nSyncTimeout);
                    pnode->fDisconnect = true;
                    pnode->fStartSync = false;
                    nAskedForBlocks = 0;

                    break;
                }
            }
        }
    }
}

bool IsInitialBlockDownload()
{
    //LOCK(cs_main);
    if (pindexBest == NULL || nBestHeight < Checkpoints::GetTotalBlocksEstimate())
        return true;
    static int64_t nLastUpdate;
    static CBlockIndex* pindexLastBest;
    if (pindexBest != pindexLastBest)
    {
        pindexLastBest = pindexBest;
        nLastUpdate = GetTime();
    }
    return (GetTime() - nLastUpdate < 15 &&
            pindexBest->GetBlockTime() < GetTime() - 8 * 60 * 60);
}

// Requires cs_main.
void Misbehaving(NodeId pnode, int howmuch)
{
    if (howmuch == 0)
        return;

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pn, vNodes)
    {
        if(pn->GetId() == pnode)
        {
            pn->nMisbehavior += howmuch;
            int banscore = GetArg("-banscore", 25);
            if (pn->nMisbehavior >= banscore && pn->nMisbehavior - howmuch < banscore)
            {
                LogPrintf("Misbehaving: %s (%d -> %d) BAN THRESHOLD EXCEEDED\n", pn->addrName, pn->nMisbehavior-howmuch, pn->nMisbehavior);
                //pn->fShouldBan = true;
            }
            else
                LogPrintf("Misbehaving: %s (%d -> %d)\n", pn->addrName, pn->nMisbehavior-howmuch, pn->nMisbehavior);

            break;
        }
    }
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (pindexNew->nChainTrust > nBestInvalidTrust)
    {
        nBestInvalidTrust = pindexNew->nChainTrust;
        CTxDB().WriteBestInvalidTrust(CBigNum(nBestInvalidTrust));
    }

    uint256 nBestInvalidBlockTrust = pindexNew->nChainTrust - pindexNew->pprev->nChainTrust;
    uint256 nBestBlockTrust = pindexBest->nHeight != 0 ? (pindexBest->nChainTrust - pindexBest->pprev->nChainTrust) : pindexBest->nChainTrust;

    LogPrintf("InvalidChainFound: invalid block=%s  height=%d  trust=%s  blocktrust=%d  date=%s\n",
      pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
      CBigNum(pindexNew->nChainTrust).ToString(), nBestInvalidBlockTrust.Get64(),
      DateTimeStrFormat("%x %H:%M:%S", pindexNew->GetBlockTime()));
    LogPrintf("InvalidChainFound:  current best=%s  height=%d  trust=%s  blocktrust=%d  date=%s\n",
      hashBestChain.ToString(), nBestHeight,
      CBigNum(pindexBest->nChainTrust).ToString(),
      nBestBlockTrust.Get64(),
      DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()));
}


void CBlock::UpdateTime(const CBlockIndex* pindexPrev)
{
    nTime = max(GetBlockTime(), GetAdjustedTime());
}

bool IsConfirmedInNPrevBlocks(const CTxIndex& txindex, const CBlockIndex* pindexFrom, int nMaxDepth, int& nActualDepth)
{
    for (const CBlockIndex* pindex = pindexFrom; pindex && pindexFrom->nHeight - pindex->nHeight < nMaxDepth; pindex = pindex->pprev)
    {
        if (pindex->nBlockPos == txindex.pos.nBlockPos && pindex->nFile == txindex.pos.nFile)
        {
            nActualDepth = pindexFrom->nHeight - pindex->nHeight;
            return true;
        }
    }
    return false;
}

bool CTransaction::DisconnectInputs(CTxDB& txdb)
{
    // Relinquish previous transactions' spent pointers
    if (!IsCoinBase())
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
        {
            COutPoint prevout = txin.prevout;

            // Get prev txindex from disk
            CTxIndex txindex;
            if (!txdb.ReadTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : ReadTxIndex failed");

            if (prevout.n >= txindex.vSpent.size())
                return error("DisconnectInputs() : prevout.n out of range");

            // Mark outpoint as not spent
            txindex.vSpent[prevout.n].SetNull();

            // Write back
            if (!txdb.UpdateTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : UpdateTxIndex failed");
        }
    }

    // Remove transaction from index
    // This can fail if a duplicate of this transaction was in a chain that got
    // reorganized away. This is only possible if this transaction was completely
    // spent, so erasing it would be a no-op anyway.
    txdb.EraseTxIndex(*this);

    return true;
}


bool CTransaction::FetchInputs(CTxDB& txdb, const map<uint256, CTxIndex>& mapTestPool,
                               bool fBlock, bool fMiner, MapPrevTx& inputsRet, bool& fInvalid)
{
    // FetchInputs can return false either because we just haven't seen some inputs
    // (in which case the transaction should be stored as an orphan)
    // or because the transaction is malformed (in which case the transaction should
    // be dropped).  If tx is definitely invalid, fInvalid will be set to true.
    fInvalid = false;

    if (IsCoinBase())
        return true; // Coinbase transactions have no inputs to fetch.

    for (unsigned int i = 0; i < vin.size(); i++)
    {
        COutPoint prevout = vin[i].prevout;
        if (inputsRet.count(prevout.hash))
            continue; // Got it already

        // Read txindex
        CTxIndex& txindex = inputsRet[prevout.hash].first;
        bool fFound = true;
        if ((fBlock || fMiner) && mapTestPool.count(prevout.hash))
        {
            // Get txindex from current proposed changes
            txindex = mapTestPool.find(prevout.hash)->second;
        }
        else
        {
            // Read txindex from txdb
            fFound = txdb.ReadTxIndex(prevout.hash, txindex);
        }
        if (!fFound && (fBlock || fMiner))
            return fMiner ? false : error("FetchInputs() : %s prev tx %s index entry not found", GetHash().ToString(),  prevout.hash.ToString());

        // Read txPrev
        CTransaction& txPrev = inputsRet[prevout.hash].second;
        if (!fFound || txindex.pos == CDiskTxPos(1,1,1))
        {
            // Get prev tx from single transactions in memory
            if (!mempool.lookup(prevout.hash, txPrev))
                return error("FetchInputs() : %s mempool Tx prev not found %s", GetHash().ToString(),  prevout.hash.ToString());
            if (!fFound)
                txindex.vSpent.resize(txPrev.vout.size());
        }
        else
        {
            // Get prev tx from disk
            if (!txPrev.ReadFromDisk(txindex.pos))
                return error("FetchInputs() : %s ReadFromDisk prev tx %s failed", GetHash().ToString(),  prevout.hash.ToString());
        }
    }

    // Make sure all prevout.n indexes are valid:
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const COutPoint prevout = vin[i].prevout;
        assert(inputsRet.count(prevout.hash) != 0);
        const CTxIndex& txindex = inputsRet[prevout.hash].first;
        const CTransaction& txPrev = inputsRet[prevout.hash].second;
        if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
        {
            // Revisit this if/when transaction replacement is implemented and allows
            // adding inputs:
            fInvalid = true;
            return DoS(100, error("FetchInputs() : %s prevout.n out of range %d %u %u prev tx %s\n%s", GetHash().ToString(), prevout.n, txPrev.vout.size(), txindex.vSpent.size(), prevout.hash.ToString(), txPrev.ToString()));
        }
    }

    return true;
}

const CTxOut& CTransaction::GetOutputFor(const CTxIn& input, const MapPrevTx& inputs) const
{
    MapPrevTx::const_iterator mi = inputs.find(input.prevout.hash);
    if (mi == inputs.end())
        throw std::runtime_error("CTransaction::GetOutputFor() : prevout.hash not found");

    const CTransaction& txPrev = (mi->second).second;
    if (input.prevout.n >= txPrev.vout.size())
        throw std::runtime_error("CTransaction::GetOutputFor() : prevout.n out of range");

    return txPrev.vout[input.prevout.n];
}

int64_t CTransaction::GetValueIn(const MapPrevTx& inputs) const
{
    if (IsCoinBase())
        return 0;

    int64_t nResult = 0;
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        nResult += GetOutputFor(vin[i], inputs).nValue;
    }
    return nResult;

}

bool CTransaction::ConnectInputs(CTxDB& txdb, MapPrevTx inputs, map<uint256, CTxIndex>& mapTestPool, const CDiskTxPos& posThisTx,
    const CBlockIndex* pindexBlock, bool fBlock, bool fMiner, unsigned int flags, bool fValidateSig)
{
    // Take over previous transactions' spent pointers
    // fBlock is true when this is called from AcceptBlock when a new best-block is added to the blockchain
    // fMiner is true when called from the internal bitcoin miner
    // ... both are false when called from CTransaction::AcceptToMemoryPool
    if (!IsCoinBase())
    {
        int64_t nValueIn = 0;
        int64_t nFees = 0;
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            COutPoint prevout = vin[i].prevout;
            assert(inputs.count(prevout.hash) > 0);
            CTxIndex& txindex = inputs[prevout.hash].first;
            CTransaction& txPrev = inputs[prevout.hash].second;

            if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
                return DoS(100, error("ConnectInputs() : %s prevout.n out of range %d %u %u prev tx %s\n%s", GetHash().ToString(), prevout.n, txPrev.vout.size(), txindex.vSpent.size(), prevout.hash.ToString(), txPrev.ToString()));

            // If prev is coinbase or coinstake, check that it's matured
            if (txPrev.IsCoinBase() || txPrev.IsCoinStake())
            {
                int nSpendDepth;
                if (IsConfirmedInNPrevBlocks(txindex, pindexBlock, nCoinbaseMaturity, nSpendDepth)){
                    return error("ConnectInputs() : tried to spend %s at depth %d", txPrev.IsCoinBase() ? "coinbase" : "coinstake", nSpendDepth);
                }
            }
            // ppcoin: check transaction timestamp
            if (txPrev.nTime > nTime)
                return DoS(100, error("ConnectInputs() : transaction timestamp earlier than input transaction"));

            // Check for negative or overflow input values
            nValueIn += txPrev.vout[prevout.n].nValue;
            if (!MoneyRange(txPrev.vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                return DoS(100, error("ConnectInputs() : txin values out of range"));

        }
        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            COutPoint prevout = vin[i].prevout;
            assert(inputs.count(prevout.hash) > 0);
            CTxIndex& txindex = inputs[prevout.hash].first;
            CTransaction& txPrev = inputs[prevout.hash].second;

            // Check for conflicts (double-spend)
            // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
            // for an attacker to attempt to split the network.
            if (!txindex.vSpent[prevout.n].IsNull())
                return fMiner ? false : error("ConnectInputs() : %s prev tx already used at %s", GetHash().ToString(), txindex.vSpent[prevout.n].ToString());

       if(fValidateSig)
       {
            // Skip ECDSA signature verification when connecting blocks (fBlock=true)
            // before the last blockchain checkpoint. This is safe because block merkle hashes are
            // still computed and checked, and any change will be caught at the next checkpoint.
            if (!(fBlock && (nBestHeight < Checkpoints::GetTotalBlocksEstimate())))
            {
                // Verify signature
                if (!VerifySignature(txPrev, *this, i, flags, 0))
                {
                    if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                        // Check whether the failure was caused by a
                        // non-mandatory script verification check, such as
                        // non-null dummy arguments;
                        // if so, don't trigger DoS protection to
                        // avoid splitting the network between upgraded and
                        // non-upgraded nodes.
                        if (VerifySignature(txPrev, *this, i, flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, 0))
                            return error("ConnectInputs() : %s non-mandatory VerifySignature failed", GetHash().ToString());
                    }
                    // Failures of other flags indicate a transaction that is
                    // invalid in new blocks, e.g. a invalid P2SH. We DoS ban
                    // such nodes as they are not following the protocol. That
                    // said during an upgrade careful thought should be taken
                    // as to the correct behavior - we may want to continue
                    // peering with non-upgraded nodes even after a soft-fork
                    // super-majority vote has passed.
                    return DoS(100,error("ConnectInputs() : %s VerifySignature failed", GetHash().ToString()));
                }
            }
       }

            // Mark outpoints as spent
            txindex.vSpent[prevout.n] = posThisTx;

            // Write back
            if (fBlock || fMiner)
            {
                mapTestPool[prevout.hash] = txindex;
            }
        }

        if (!IsCoinStake())
        {
            if (nValueIn < GetValueOut())
                return DoS(100, error("ConnectInputs() : %s value in < value out", GetHash().ToString()));

            // Tally transaction fees
            int64_t nTxFee = nValueIn - GetValueOut();
            if (nTxFee < 0)
                return DoS(100, error("ConnectInputs() : %s nTxFee < 0", GetHash().ToString()));

            nFees += nTxFee;
            if (!MoneyRange(nFees))
                return DoS(100, error("ConnectInputs() : nFees out of range"));
        }
    }

    return true;
}

bool CBlock::DisconnectBlock(CTxDB& txdb, CBlockIndex* pindex)
{
    // Disconnect in reverse order
    for (int i = vtx.size()-1; i >= 0; i--)
        if (!vtx[i].DisconnectInputs(txdb))
            return false;

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = 0;
        if (!txdb.WriteBlockIndex(blockindexPrev))
            return error("DisconnectBlock() : WriteBlockIndex failed");
    }

    // ppcoin: clean up wallet after disconnecting coinstake
    BOOST_FOREACH(CTransaction& tx, vtx)
        SyncWithWallets(tx, this, false);

    return true;
}

bool static BuildAddrIndex(const CScript &script, std::vector<uint160>& addrIds)
{
    CScript::const_iterator pc = script.begin();
    CScript::const_iterator pend = script.end();
    std::vector<unsigned char> data;
    opcodetype opcode;
    bool fHaveData = false;
    while (pc < pend) {
        script.GetOp(pc, opcode, data);
        if (0 <= opcode && opcode <= OP_PUSHDATA4 && data.size() >= 8) { // data element
            uint160 addrid = 0;
            if (data.size() <= 20) {
                memcpy(&addrid, &data[0], data.size());
            } else {
                addrid = Hash160(data);
            }
            addrIds.push_back(addrid);
            fHaveData = true;
        }
    }
    if (!fHaveData) {
        uint160 addrid = Hash160(script);
    addrIds.push_back(addrid);
        return true;
    }
    else
    {
    if(addrIds.size() > 0)
        return true;
    else
        return false;
    }
}

bool FindTransactionsByDestination(const CTxDestination &dest, std::vector<uint256> &vtxhash) {
    uint160 addrid = 0;
    const CKeyID *pkeyid = boost::get<CKeyID>(&dest);
    if (pkeyid)
        addrid = static_cast<uint160>(*pkeyid);
    if (!addrid) {
        const CScriptID *pscriptid = boost::get<CScriptID>(&dest);
        if (pscriptid)
            addrid = static_cast<uint160>(*pscriptid);
    }
    if (!addrid)
    {
        LogPrintf("FindTransactionsByDestination(): Couldn't parse dest into addrid\n");
        return false;
    }

    LOCK(cs_main);
    CTxDB txdb("r");
    if(!txdb.ReadAddrIndex(addrid, vtxhash))
    {
        LogPrintf("FindTransactionsByDestination(): txdb.ReadAddrIndex failed\n");
        return false;
    }
    return true;
}

void RebuildAddressIndexFromHeight(int64_t nStartHeight)
{
    LogPrintf("RebuildAddressIndexFromHeight(): nStartHeight: %d\n", nStartHeight);
    uint64_t nStopHeight = pindexBest->nHeight;
    CBlockIndex* pblockAddrIndex = FindBlockByHeight(nStartHeight);
    CTxDB txdbAddr("rw");
    while(pblockAddrIndex && pblockAddrIndex->nHeight <= nStopHeight)
    {
        CBlock pblockAddr;
        if(pblockAddr.ReadFromDisk(pblockAddrIndex, true))
            pblockAddr.RebuildAddressIndex(txdbAddr);
        pblockAddrIndex = pblockAddrIndex->pnext;
    }
    LogPrintf("RebuildAddressIndexFromHeight(): Complete\n");
}

void RebuildAddressIndexForBlock(int64_t nBlockHeight)
{
    LogPrintf("RebuildAddressIndexForBlock(): nBlockHeight: %d\n", nBlockHeight);
    CBlockIndex* pblockAddrIndex = FindBlockByHeight(nBlockHeight);
    CTxDB txdbAddr("rw");
    CBlock pblockAddr;
    if(pblockAddr.ReadFromDisk(pblockAddrIndex, true))
        pblockAddr.RebuildAddressIndex(txdbAddr);

    LogPrintf("RebuildAddressIndexForBlock(): Complete\n");
}

void CBlock::RebuildAddressIndex(CTxDB& txdb)
{
    BOOST_FOREACH(CTransaction& tx, vtx)
    {
        uint256 hashTx = tx.GetHash();
        // inputs
        if(!tx.IsCoinBase())
        {
            MapPrevTx mapInputs;
            map<uint256, CTxIndex> mapQueuedChangesT;
            bool fInvalid;
            if (!tx.FetchInputs(txdb, mapQueuedChangesT, true, false, mapInputs, fInvalid))
                return;

            MapPrevTx::const_iterator mi;
            for(MapPrevTx::const_iterator mi = mapInputs.begin(); mi != mapInputs.end(); ++mi)
            {
                BOOST_FOREACH(const CTxOut &atxout, (*mi).second.second.vout)
                {
                    std::vector<uint160> addrIds;
                    if(BuildAddrIndex(atxout.scriptPubKey, addrIds))
                    {
                        BOOST_FOREACH(uint160 addrId, addrIds)
                        {
                            if(!txdb.WriteAddrIndex(addrId, hashTx))
                                LogPrintf("RebuildAddressIndex(): txins WriteAddrIndex failed addrId: %s txhash: %s\n", addrId.ToString().c_str(), hashTx.ToString().c_str());
                        }
                    }
                }
            }
        }
        // outputs
        BOOST_FOREACH(const CTxOut &atxout, tx.vout) {
            std::vector<uint160> addrIds;
            if(BuildAddrIndex(atxout.scriptPubKey, addrIds))
            {
                BOOST_FOREACH(uint160 addrId, addrIds)
                {
                    if(!txdb.WriteAddrIndex(addrId, hashTx))
                        LogPrintf("RebuildAddressIndex(): txouts WriteAddrIndex failed addrId: %s txhash: %s\n", addrId.ToString().c_str(), hashTx.ToString().c_str());
                }
            }
        }
    }
}

bool CBlock::ConnectBlock(CTxDB& txdb, CBlockIndex* pindex, bool fJustCheck)
{
    // Check it again in case a previous version let a bad block in, but skip BlockSig checking
    if (!CheckBlock(!fJustCheck, !fJustCheck, false))
        return false;

    unsigned int flags = SCRIPT_VERIFY_NOCACHE;

    //// issue here: it doesn't know the version
    unsigned int nTxPos;
    if (fJustCheck)
        // FetchInputs treats CDiskTxPos(1,1,1) as a special "refer to memorypool" indicator
        // Since we're just checking the block and not actually connecting it, it might not (and probably shouldn't) be on the disk to get the transaction from
        nTxPos = 1;
    else
        nTxPos = pindex->nBlockPos + ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) - (2 * GetSizeOfCompactSize(0)) + GetSizeOfCompactSize(vtx.size());

    map<uint256, CTxIndex> mapQueuedChanges;
    int64_t nFees = 0;
    int64_t nValueIn = 0;
    int64_t nValueOut = 0;
    int64_t nStakeReward = 0;
    unsigned int nSigOps = 0;
    int nInputs = 0;

    BOOST_FOREACH(CTransaction& tx, vtx)
    {
        uint256 hashTx = tx.GetHash();
        nInputs += tx.vin.size();
        nSigOps += GetLegacySigOpCount(tx);

        if (nSigOps > MAX_BLOCK_SIGOPS)
            return DoS(100, error("ConnectBlock() : too many sigops"));

        CDiskTxPos posThisTx(pindex->nFile, pindex->nBlockPos, nTxPos);
        if (!fJustCheck)
            nTxPos += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);

        MapPrevTx mapInputs;
        if (tx.IsCoinBase())
            nValueOut += tx.GetValueOut();
        else
        {
            bool fInvalid;
            if (!tx.FetchInputs(txdb, mapQueuedChanges, true, false, mapInputs, fInvalid))
                return false;

            // Add in sigops done by pay-to-script-hash inputs;
            // this is to prevent a "rogue miner" from creating
            // an incredibly-expensive-to-validate block.
            nSigOps += GetP2SHSigOpCount(tx, mapInputs);
            if (nSigOps > MAX_BLOCK_SIGOPS)
                return DoS(100, error("ConnectBlock() : too many sigops"));

            int64_t nTxValueIn = tx.GetValueIn(mapInputs);
            int64_t nTxValueOut = tx.GetValueOut();
            nValueIn += nTxValueIn;
            nValueOut += nTxValueOut;
            if (!tx.IsCoinStake())
                nFees += nTxValueIn - nTxValueOut;
            if (tx.IsCoinStake())
                nStakeReward = nTxValueOut - nTxValueIn;


            if (!tx.ConnectInputs(txdb, mapInputs, mapQueuedChanges, posThisTx, pindex, true, false, flags))
                return false;
        }

        mapQueuedChanges[hashTx] = CTxIndex(posThisTx, tx.vout.size());
    }


    if (IsProofOfWork())
    {
        int64_t nReward = GetProofOfWorkReward(pindex->nHeight, nFees);
        // Check coinbase reward
        if (vtx[0].GetValueOut() > nReward)
            return DoS(50, error("ConnectBlock() : coinbase reward exceeded (actual=%d vs calculated=%d)",
                   vtx[0].GetValueOut(),
                   nReward));

        // If after rebrand hardfork, check that dev rewards are present
        if(pindex->nHeight >= PEPE_REBRAND_PF_HEIGHT)
            if(!CheckDevRewards(vtx[0], pindex->nHeight, nReward, nFees))
                return error("ConnectBlock(): check proof-of-work failed for block, dev rewards mising.");
    }
    if (IsProofOfStake())
    {
        // ppcoin: coin stake tx earns reward instead of paying fee
        uint64_t nCoinAge;
        int64_t nCoinValue;
        if (!vtx[1].GetCoinAge(txdb, pindex->pprev, nCoinAge, nCoinValue))
            return error("ConnectBlock() : %s unable to get coin age for coinstake", vtx[1].GetHash().ToString());

        int64_t nCalculatedStakeReward = GetProofOfStakeReward(pindex->pprev, nCoinAge, nFees, nCoinValue);

        if (nStakeReward > nCalculatedStakeReward)
            return DoS(100, error("ConnectBlock() : coinstake pays too much(actual=%d vs calculated=%d)", nStakeReward, nCalculatedStakeReward));

        // If after rebrand hardfork, check that dev rewards are present
        if(pindex->nHeight >= PEPE_REBRAND_PF_HEIGHT)
            if(!CheckDevRewards(vtx[1], pindex->nHeight, nStakeReward, nFees))
                return error("ConnectBlock(): check proof-of-stake failed for block, dev rewards mising.");
    }

    // ----------- masternode payments -----------

    bool MasternodePayments = false;                    
    if ( Params().NetworkID() == CChainParams::TESTNET ){
        if (pindex->nHeight > PEPE_KEKDAQ_MID_HEIGHT_TESTNET ){
            MasternodePayments = true;
        }
    }else{
        if (pindex->nHeight > PEPE_KEKDAQ_MID_FIX_HEIGHT){
            MasternodePayments = true;
        }
    }

    if(MasternodePayments == true)
    {
        CAmount masternodePaymentAmount = 0;
        if(IsProofOfStake())
        {            
            int64_t nDevReward = 0.02 * nStakeReward; // 2% per dev reward

            if (pindex->nHeight > PEPE_REBRAND_PF_HEIGHT)
                nDevReward = 0.04 * nStakeReward; // 4% per dev reward
            if (pindex->nHeight == PEPE_REBRAND_PF_HEIGHT)
                nDevReward = PEPE_DEV_GRANT;
            if (pindex->nHeight == PEPE_KEKDAQ_MID_FIX_HEIGHT)
                nDevReward = PEPE_DEV_GRANT_MID;
            if (pindex->nHeight == PEPE_IPFSMN_FNL_HEIGHT)
                nDevReward = PEPE_DEV_GRANT_FINAL;
            if (pindex->nHeight == PEPE_STAKE_CONF_HEIGHT)
                nDevReward = PEPE_DEV_GRANT;
            if (pindex->nHeight == PEPE_KEKDAQ2_SWAP_HEIGHT)
                nDevReward = DEVFEE_OFF_SWAP_FINAL;
            if (pindex->nHeight == PEPE_STAKEONLY_HEIGHT)
                nDevReward = PEPE_SO_SWAP_GRANT;

            int64_t nTotalDevRewards = 3 * nDevReward;
            if(pindex->nHeight >= PEPE_STAKEONLY_HEIGHT)
                nTotalDevRewards = 4 * nDevReward;
            int64_t nMnRewardBase = nStakeReward - nTotalDevRewards;
            masternodePaymentAmount = GetMasternodePayment(pindex->nHeight, nMnRewardBase);
        }
        else if(IsProofOfWork())
        {
            masternodePaymentAmount = GetMasternodePayment(pindex->nHeight, vtx[0].GetValueOut());
        }
            
        bool fIsInitialDownload = IsInitialBlockDownload();

        // If we don't already have its previous block, skip masternode payment step
        if (!fIsInitialDownload && pindex != NULL)
        {
            bool foundPaymentAmount = false;
            bool foundPayee = false;

            CScript payee;
            if(!masternodePayments.GetBlockPayee(pindex->nHeight, payee) || payee == CScript()){
                foundPayee = true; //doesn't require a specific payee
                
                if(fDebug) { LogPrintf("CheckBlock() : Using non-specific masternode payments %d\n", pindex->nHeight); }
            }

            int vtxIndex = 0;
            if(IsProofOfStake())
                vtxIndex = 1;

            for (unsigned int i = 0; i < vtx[vtxIndex].vout.size(); i++) {
                if(vtx[vtxIndex].vout[i].nValue == masternodePaymentAmount )
                    foundPaymentAmount = true;
                if(vtx[vtxIndex].vout[i].scriptPubKey == payee )
                    foundPayee = true;
            }

            if(!foundPaymentAmount || !foundPayee) {
                CTxDestination address1;
                ExtractDestination(payee, address1);
                CBitcoinAddress address2(address1);

                if(fDebug) { LogPrintf("CheckBlock() : Couldn't find masternode payment(%d|%d) or payee(%d|%s) nHeight %d. \n", foundPaymentAmount, masternodePaymentAmount, foundPayee, address2.ToString().c_str(), pindex->nHeight); }
                return DoS(1, error("CheckBlock() : Couldn't find masternode payment or payee"));
            } else {
                if(fDebug) { LogPrintf("CheckBlock() : Found masternode payment %d\n", pindexBest->nHeight+1); }
            }
        } else {
            if(fDebug) { LogPrintf("CheckBlock() : Is initial download, skipping masternode payment check %d\n", pindex->nHeight); }
        }
    } else {
        if(fDebug) { LogPrintf("CheckBlock() : Skipping masternode payment check - nHeight %d Hash %s\n", pindex->nHeight, GetHash().ToString().c_str()); }
    }


    // ppcoin: track money supply and mint amount info
    pindex->nMint = nValueOut - nValueIn + nFees;
    pindex->nMoneySupply = (pindex->pprev? pindex->pprev->nMoneySupply : 0) + nValueOut - nValueIn;
    if (!txdb.WriteBlockIndex(CDiskBlockIndex(pindex)))
        return error("Connect() : WriteBlockIndex for pindex failed");

    if (fJustCheck)
        return true;

    // Write queued txindex changes
    for (map<uint256, CTxIndex>::iterator mi = mapQueuedChanges.begin(); mi != mapQueuedChanges.end(); ++mi)
    {
        if (!txdb.UpdateTxIndex((*mi).first, (*mi).second))
            return error("ConnectBlock() : UpdateTxIndex failed");
    }

    if(GetBoolArg("-addrindex", false) && !fJustCheck)
    {
        CBitcoinAddress addrDevOne;
        addrDevOne.SetString(DecodeBase64(PEPE_REBRAND_DEV_1));
        CScript payeeDevOne = GetScriptForDestination(addrDevOne.Get());
        CBitcoinAddress addrDevTwo;
        addrDevTwo.SetString(DecodeBase64(PEPE_REBRAND_DEV_2));
        CScript payeeDevTwo = GetScriptForDestination(addrDevTwo.Get());
        CBitcoinAddress addrDevThree;
        addrDevThree.SetString(DecodeBase64(PEPE_REBRAND_DEV_3));
        CScript payeeDevThree = GetScriptForDestination(addrDevThree.Get());

        CTxDB txdbaddr("rw");
        // Write Address Index
        BOOST_FOREACH(CTransaction& tx, vtx)
        {
            uint256 hashTx = tx.GetHash();
            // inputs
            if(!tx.IsCoinBase())
            {
                MapPrevTx mapInputs;
                map<uint256, CTxIndex> mapQueuedChangesT;
                bool fInvalid;
                if (!tx.FetchInputs(txdbaddr, mapQueuedChangesT, true, false, mapInputs, fInvalid))
                    return false;

                MapPrevTx::const_iterator mi;
                for(MapPrevTx::const_iterator mi = mapInputs.begin(); mi != mapInputs.end(); ++mi)
                {
                    BOOST_FOREACH(const CTxOut &atxout, (*mi).second.second.vout)
                    {
                        std::vector<uint160> addrIds;
                        
                        if(atxout.scriptPubKey != payeeDevOne && atxout.scriptPubKey != payeeDevTwo && atxout.scriptPubKey != payeeDevThree)
                        {
                            if(BuildAddrIndex(atxout.scriptPubKey, addrIds))
                            {
                                BOOST_FOREACH(uint160 addrId, addrIds)
                                {
                                    if(!txdbaddr.WriteAddrIndex(addrId, hashTx))
                                        LogPrintf("ConnectBlock(): txins WriteAddrIndex failed addrId: %s txhash: %s\n", addrId.ToString().c_str(), hashTx.ToString().c_str());
                                }
                            }
                        }
                    }
                }
            }

            // outputs
            BOOST_FOREACH(const CTxOut &atxout, tx.vout)
            {
                if(atxout.scriptPubKey != payeeDevOne && atxout.scriptPubKey != payeeDevTwo && atxout.scriptPubKey != payeeDevThree)
                {
                    std::vector<uint160> addrIds;
                    if(BuildAddrIndex(atxout.scriptPubKey, addrIds))
                    {
                        BOOST_FOREACH(uint160 addrId, addrIds)
                        {
                            if(!txdbaddr.WriteAddrIndex(addrId, hashTx))
                                LogPrintf("ConnectBlock(): txouts WriteAddrIndex failed addrId: %s txhash: %s\n", addrId.ToString().c_str(), hashTx.ToString().c_str());
                        }
                    }
                }
            }
        }
    }

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = pindex->GetBlockHash();
        if (!txdb.WriteBlockIndex(blockindexPrev))
            return error("ConnectBlock() : WriteBlockIndex failed");
    }

    // Watch for transactions paying to me
    BOOST_FOREACH(CTransaction& tx, vtx)
        SyncWithWallets(tx, this);

    return true;
}

bool static Reorganize(CTxDB& txdb, CBlockIndex* pindexNew)
{
    LogPrintf("REORGANIZE\n");

    // Find the fork
    CBlockIndex* pfork = pindexBest;
    CBlockIndex* plonger = pindexNew;
    while (pfork != plonger)
    {
        while (plonger->nHeight > pfork->nHeight)
            if (!(plonger = plonger->pprev))
                return error("Reorganize() : plonger->pprev is null");
        if (pfork == plonger)
            break;
        if (!(pfork = pfork->pprev))
            return error("Reorganize() : pfork->pprev is null");
    }

    // List of what to disconnect
    vector<CBlockIndex*> vDisconnect;
    for (CBlockIndex* pindex = pindexBest; pindex != pfork; pindex = pindex->pprev)
        vDisconnect.push_back(pindex);

    // List of what to connect
    vector<CBlockIndex*> vConnect;
    for (CBlockIndex* pindex = pindexNew; pindex != pfork; pindex = pindex->pprev)
        vConnect.push_back(pindex);
    reverse(vConnect.begin(), vConnect.end());

    LogPrintf("REORGANIZE: Disconnect %u blocks; %s..%s\n", vDisconnect.size(), pfork->GetBlockHash().ToString(), pindexBest->GetBlockHash().ToString());
    LogPrintf("REORGANIZE: Connect %u blocks; %s..%s\n", vConnect.size(), pfork->GetBlockHash().ToString(), pindexNew->GetBlockHash().ToString());

    // Disconnect shorter branch
    list<CTransaction> vResurrect;
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
    {
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("Reorganize() : ReadFromDisk for disconnect failed");
        if (!block.DisconnectBlock(txdb, pindex))
            return error("Reorganize() : DisconnectBlock %s failed", pindex->GetBlockHash().ToString());

        // Queue memory transactions to resurrect.
        // We only do this for blocks after the last checkpoint (reorganisation before that
        // point should only happen with -reindex/-loadblock, or a misbehaving peer.
        BOOST_REVERSE_FOREACH(const CTransaction& tx, block.vtx)
            if (!(tx.IsCoinBase() || tx.IsCoinStake()) && pindex->nHeight > Checkpoints::GetTotalBlocksEstimate())
                vResurrect.push_front(tx);
    }

    // Connect longer branch
    vector<CTransaction> vDelete;
    for (unsigned int i = 0; i < vConnect.size(); i++)
    {
        CBlockIndex* pindex = vConnect[i];
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("Reorganize() : ReadFromDisk for connect failed");
        if (!block.ConnectBlock(txdb, pindex))
        {
            // Invalid block
            return error("Reorganize() : ConnectBlock %s failed", pindex->GetBlockHash().ToString());
        }

        // Queue memory transactions to delete
        BOOST_FOREACH(const CTransaction& tx, block.vtx)
            vDelete.push_back(tx);
    }
    if (!txdb.WriteHashBestChain(pindexNew->GetBlockHash()))
        return error("Reorganize() : WriteHashBestChain failed");

    // Make sure it's successfully written to disk before changing memory structure
    if (!txdb.TxnCommit())
        return error("Reorganize() : TxnCommit failed");

    // Disconnect shorter branch
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
        if (pindex->pprev)
            pindex->pprev->pnext = NULL;

    // Connect longer branch
    BOOST_FOREACH(CBlockIndex* pindex, vConnect)
        if (pindex->pprev)
            pindex->pprev->pnext = pindex;

    // Resurrect memory transactions that were in the disconnected branch
    BOOST_FOREACH(CTransaction& tx, vResurrect)
        AcceptToMemoryPool(mempool, tx, false, NULL);

    // Delete redundant memory transactions that are in the connected branch
    BOOST_FOREACH(CTransaction& tx, vDelete) {
        mempool.remove(tx);
        mempool.removeConflicts(tx);
    }

    LogPrintf("REORGANIZE: done\n");

    return true;
}


// Called from inside SetBestChain: attaches a block to the new best chain being built
bool CBlock::SetBestChainInner(CTxDB& txdb, CBlockIndex *pindexNew)
{
    uint256 hash = GetHash();

    // Adding to current best branch
    if (!ConnectBlock(txdb, pindexNew) || !txdb.WriteHashBestChain(hash))
    {
        txdb.TxnAbort();
        InvalidChainFound(pindexNew);
        return false;
    }
    if (!txdb.TxnCommit())
        return error("SetBestChain() : TxnCommit failed");

    // Add to current best branch
    pindexNew->pprev->pnext = pindexNew;

    // Delete redundant memory transactions
    BOOST_FOREACH(CTransaction& tx, vtx)
        mempool.remove(tx);

    return true;
}

bool CBlock::SetBestChain(CTxDB& txdb, CBlockIndex* pindexNew)
{
    uint256 hash = GetHash();

    if (!txdb.TxnBegin())
        return error("SetBestChain() : TxnBegin failed");

    if (pindexGenesisBlock == NULL && hash == Params().HashGenesisBlock())
    {
        txdb.WriteHashBestChain(hash);
        if (!txdb.TxnCommit())
            return error("SetBestChain() : TxnCommit failed");
        pindexGenesisBlock = pindexNew;
    }
    else if (hashPrevBlock == hashBestChain)
    {
        if (!SetBestChainInner(txdb, pindexNew))
            return error("SetBestChain() : SetBestChainInner failed");
    }
    else
    {
        // the first block in the new chain that will cause it to become the new best chain
        CBlockIndex *pindexIntermediate = pindexNew;

        // list of blocks that need to be connected afterwards
        std::vector<CBlockIndex*> vpindexSecondary;

        // Reorganize is costly in terms of db load, as it works in a single db transaction.
        // Try to limit how much needs to be done inside
        while (pindexIntermediate->pprev && pindexIntermediate->pprev->nChainTrust > pindexBest->nChainTrust)
        {
            vpindexSecondary.push_back(pindexIntermediate);
            pindexIntermediate = pindexIntermediate->pprev;
        }

        if (!vpindexSecondary.empty())
            LogPrintf("Postponing %u reconnects\n", vpindexSecondary.size());

        // Switch to new best branch
        if (!Reorganize(txdb, pindexIntermediate))
        {
            txdb.TxnAbort();
            InvalidChainFound(pindexNew);
            return error("SetBestChain() : Reorganize failed");
        }

        // Connect further blocks
        BOOST_REVERSE_FOREACH(CBlockIndex *pindex, vpindexSecondary)
        {
            CBlock block;
            if (!block.ReadFromDisk(pindex))
            {
                LogPrintf("SetBestChain() : ReadFromDisk failed\n");
                break;
            }
            if (!txdb.TxnBegin()) {
                LogPrintf("SetBestChain() : TxnBegin 2 failed\n");
                break;
            }
            // errors now are not fatal, we still did a reorganisation to a new chain in a valid way
            if (!block.SetBestChainInner(txdb, pindex))
                break;
        }
    }

    // Update best block in wallet (so we can detect restored wallets)
    bool fIsInitialDownload = IsInitialBlockDownload();
    if ((pindexNew->nHeight % 20160) == 0 || (!fIsInitialDownload && (pindexNew->nHeight % 144) == 0))
    {
        const CBlockLocator locator(pindexNew);
        g_signals.SetBestChain(locator);
    }

    // New best block
    hashBestChain = hash;
    pindexBest = pindexNew;
    pblockindexFBBHLast = NULL;
    nBestHeight = pindexBest->nHeight;
    nBestChainTrust = pindexNew->nChainTrust;
    nTimeBestReceived = GetTime();
    mempool.AddTransactionsUpdated(1);

    // remove some unneeded log messages to reduce disk thrashing
    /* 
        LogPrintf("SetBestChain: new best=%s  height=%d  trust=%s  blocktrust=%d  date=%s\n",
      hashBestChain.ToString(), nBestHeight,
      CBigNum(nBestChainTrust).ToString(),
      nBestBlockTrust.Get64(),
      DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()));
    */

    // Check the version of the last 100 blocks to see if we need to upgrade:
    if (!fIsInitialDownload)
    {
        int nUpgraded = 0;
        const CBlockIndex* pindex = pindexBest;
        for (int i = 0; i < 100 && pindex != NULL; i++)
        {
            if (pindex->nVersion > CBlock::CURRENT_VERSION)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            LogPrintf("SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, (int)CBlock::CURRENT_VERSION);
        if (nUpgraded > 100/2)
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: This version is obsolete, upgrade required!");
    }

    std::string strCmd = GetArg("-blocknotify", "");

    if (!fIsInitialDownload && !strCmd.empty())
    {
        boost::replace_all(strCmd, "%s", hashBestChain.GetHex());
        boost::thread t(runCommand, strCmd); // thread runs free
    }

    return true;
}

// ppcoin: total coin age spent in transaction, in the unit of coin-days.
// Only those coins meeting minimum age requirement counts. As those
// transactions not in main chain are not currently indexed so we
// might not find out about their coin age. Older transactions are
// guaranteed to be in main chain by sync-checkpoint. This rule is
// introduced to help nodes establish a consistent view of the coin
// age (trust score) of competing branches.
bool CTransaction::GetCoinAge(CTxDB& txdb, const CBlockIndex* pindexPrev, uint64_t& nCoinAge, int64_t& nCoinValue) const
{
    CBigNum bnCentSecond = 0;  // coin age in the unit of cent-seconds
    nCoinAge = 0;
    nCoinValue = 0;
    int nStakeMinConfirmations = 360;
    if((pindexPrev->nHeight+1) > PEPE_STAKE_WINTER_SWITCH_HEIGHT)
        nStakeMinConfirmations = 60;
    if((pindexPrev->nHeight+1) > PEPE_KEKDAQ_MID_FIX_HEIGHT)
        nStakeMinConfirmations = 600;
    if((pindexPrev->nHeight+1) > PEPE_STAKE_CONF_HEIGHT)
        nStakeMinConfirmations = 360;
    if((pindexPrev->nHeight+1) > PEPE_STAKE_CONF_TWEAK)
        nStakeMinConfirmations = 600;

    if (IsCoinBase())
        return true;

    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        // First try finding the previous transaction in database
        CTransaction txPrev;
        CTxIndex txindex;
        if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
            continue;  // previous transaction not in main chain
        if (nTime < txPrev.nTime)
            return false;  // Transaction timestamp violation

        int nSpendDepth;

        if (IsConfirmedInNPrevBlocks(txindex, pindexPrev, nStakeMinConfirmations - 1, nSpendDepth))
        {
            LogPrint("coinage", "coin age skip nSpendDepth=%d\n", nSpendDepth + 1);
            continue; // only count coins meeting min confirmations requirement
        }

        int64_t nValueIn = txPrev.vout[txin.prevout.n].nValue;
        bnCentSecond += CBigNum(nValueIn) * (nTime-txPrev.nTime) / CENT;
        nCoinValue += nValueIn;


        LogPrint("coinage", "coin age nValueIn=%d nTimeDiff=%d bnCentSecond=%s\n", nValueIn, nTime - txPrev.nTime, bnCentSecond.ToString());
    }

    CBigNum bnCoinDay = bnCentSecond * CENT / COIN / (24 * 60 * 60);
    LogPrint("coinage", "coin age bnCoinDay=%s\n", bnCoinDay.ToString());
    nCoinAge = bnCoinDay.getuint64();
    return true;
}

bool CBlock::AddToBlockIndex(unsigned int nFile, unsigned int nBlockPos, const uint256& hashProof)
{
    AssertLockHeld(cs_main);

    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return error("AddToBlockIndex() : %s already exists", hash.ToString());

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(nFile, nBlockPos, *this);
    if (!pindexNew)
        return error("AddToBlockIndex() : new CBlockIndex failed");
    pindexNew->phashBlock = &hash;
    map<uint256, CBlockIndex*>::iterator miPrev = mapBlockIndex.find(hashPrevBlock);
    if (miPrev != mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
    }

    // ppcoin: compute chain trust score
    pindexNew->nChainTrust = (pindexNew->pprev ? pindexNew->pprev->nChainTrust : 0) + pindexNew->GetBlockTrust();

    // ppcoin: compute stake entropy bit for stake modifier
    if (!pindexNew->SetStakeEntropyBit(GetStakeEntropyBit()))
        return error("AddToBlockIndex() : SetStakeEntropyBit() failed");

    // Record proof hash value
    pindexNew->hashProof = hashProof;

    // ppcoin: compute stake modifier
    uint64_t nStakeModifier = 0;
    bool fGeneratedStakeModifier = false;
    if (!ComputeNextStakeModifier(pindexNew->pprev, nStakeModifier, fGeneratedStakeModifier))
        return error("AddToBlockIndex() : ComputeNextStakeModifier() failed");
    pindexNew->SetStakeModifier(nStakeModifier, fGeneratedStakeModifier);
    pindexNew->bnStakeModifierV2 = ComputeStakeModifierV2(pindexNew->pprev, IsProofOfWork() ? hash : vtx[1].vin[0].prevout.hash);

    // Add to mapBlockIndex
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    if (pindexNew->IsProofOfStake())
        setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));
    pindexNew->phashBlock = &((*mi).first);

    // Write to disk block index
    CTxDB txdb;
    if (!txdb.TxnBegin())
        return false;
    txdb.WriteBlockIndex(CDiskBlockIndex(pindexNew));
    if (!txdb.TxnCommit())
        return false;

    // New best
    if (pindexNew->nChainTrust > nBestChainTrust)
        if (!SetBestChain(txdb, pindexNew))
            return false;

    if (pindexNew == pindexBest)
    {
        // Notify UI to display prev block's coinbase if it was ours
        static uint256 hashPrevBestCoinBase;
        g_signals.UpdatedTransaction(hashPrevBestCoinBase);
        hashPrevBestCoinBase = vtx[0].GetHash();
    }

    return true;
}

bool CBlock::CheckBlock(bool fCheckPOW, bool fCheckMerkleRoot, bool fCheckSig) const
{
    // These are checks that are independent of context
    // that can be verified before saving an orphan block.

    // Size limits
    if (vtx.empty() || vtx.size() > MAX_BLOCK_SIZE || ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return DoS(100, error("CheckBlock() : size limits failed"));

    // Check proof of work matches claimed amount
    if (fCheckPOW && IsProofOfWork() && !CheckProofOfWork(GetPoWHash(), nBits))
        return DoS(50, error("CheckBlock() : proof of work failed"));

    // Check timestamp
    int nHeight = 0;
    map<uint256, CBlockIndex*>::iterator miPrev = mapBlockIndex.find(hashPrevBlock);
    if (miPrev != mapBlockIndex.end())
    {
        nHeight = (*miPrev).second->nHeight + 1;
    }

    if (GetBlockTime() > FutureDrift(GetAdjustedTime(), nHeight))
    {
        LogPrintf("CheckBlock(): block timestamp too far in the future for block height: %d\n", nHeight);
        return error("CheckBlock() : block timestamp too far in the future");
    }

    // First transaction must be coinbase, the rest must not be
    if (vtx.empty() || !vtx[0].IsCoinBase())
        return DoS(100, error("CheckBlock() : first tx is not coinbase"));
    for (unsigned int i = 1; i < vtx.size(); i++)
        if (vtx[i].IsCoinBase())
            return DoS(100, error("CheckBlock() : more than one coinbase"));

    if (IsProofOfStake())
    {
        // Coinbase output should be empty if proof-of-stake block
        if (vtx[0].vout.size() != 1 || !vtx[0].vout[0].IsEmpty())
            return DoS(100, error("CheckBlock() : coinbase output not empty for proof-of-stake block"));

        // Second transaction must be coinstake, the rest must not be
        if (vtx.empty() || !vtx[1].IsCoinStake())
            return DoS(100, error("CheckBlock() : second tx is not coinstake"));
        for (unsigned int i = 2; i < vtx.size(); i++)
            if (vtx[i].IsCoinStake())
                return DoS(100, error("CheckBlock() : more than one coinstake"));
    }

    // Check proof-of-stake block signature
    if (fCheckSig && !CheckBlockSignature())
        return DoS(100, error("CheckBlock() : bad proof-of-stake block signature"));

    // Check transactions
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        if (!tx.CheckTransaction())
            return DoS(tx.nDoS, error("CheckBlock() : CheckTransaction failed"));

        // ppcoin: check transaction timestamp
        if (GetBlockTime() < (int64_t)tx.nTime)
            return DoS(50, error("CheckBlock() : block timestamp earlier than transaction timestamp"));
    }

    // Check for duplicate txids. This is caught by ConnectInputs(),
    // but catching it earlier avoids a potential DoS attack:
    set<uint256> uniqueTx;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        uniqueTx.insert(tx.GetHash());
    }
    if (uniqueTx.size() != vtx.size())
        return DoS(100, error("CheckBlock() : duplicate transaction"));

    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        nSigOps += GetLegacySigOpCount(tx);
    }
    if (nSigOps > MAX_BLOCK_SIGOPS)
        return DoS(100, error("CheckBlock() : out-of-bounds SigOpCount"));

    // Check merkle root
    if (fCheckMerkleRoot && hashMerkleRoot != BuildMerkleTree())
        return DoS(100, error("CheckBlock() : hashMerkleRoot mismatch"));


    return true;
}

bool CBlock::AcceptBlock()
{
    AssertLockHeld(cs_main);

    // Remove for BIP-0034 FORK
    if (nVersion > CURRENT_VERSION)
        return DoS(100, error("AcceptBlock() : reject unknown block version %d", nVersion));

    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return error("AcceptBlock() : block already in mapBlockIndex");

    // Get prev block index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashPrevBlock);
    if (mi == mapBlockIndex.end())
        return DoS(1, error("AcceptBlock() : prev block not found"));
    CBlockIndex* pindexPrev = (*mi).second;
    int nHeight = pindexPrev->nHeight+1;

    if (IsProofOfWork() && ((nHeight > STOP_POW_BLOCK && nHeight < RESTART_POW_BLOCK) || nHeight >= PEPE_STAKEONLY_HEIGHT))
        return DoS(100, error("AcceptBlock() : reject proof-of-work at height %d", nHeight));

    if (IsProofOfStake() && nHeight < Params().POSStartBlock() )
        return DoS(100, error("AcceptBlock() : reject proof-of-stake at height <= %d", nHeight));

    // Check coinbase timestamp
    if (GetBlockTime() > FutureDrift((int64_t)vtx[0].nTime, nHeight) && IsProofOfStake())
        return DoS(50, error("AcceptBlock() : coinbase timestamp is too early"));

    // Check coinstake timestamp
    if (IsProofOfStake() && !CheckCoinStakeTimestamp(nHeight, GetBlockTime(), (int64_t)vtx[1].nTime))
        return DoS(50, error("AcceptBlock() : coinstake timestamp violation nTimeBlock=%d nTimeTx=%u", GetBlockTime(), vtx[1].nTime));

    // Check proof-of-work or proof-of-stake
    if (nBits != GetNextTargetRequired(pindexPrev, IsProofOfStake()) &&
        hash != uint256("0x73f988a95293b060f1b4034c5e62a6e4439090e5d19bf078f3c90548671c5a82") && // exception for block 886907 past hard-fork stick
        hash != uint256("0x9b345b188b1fb5cbd535666cf678b53577b58481765705592d1d3c5986ed6ac6")) // exception for block 886416 past hard-fork stick
        
        return DoS(1, error("AcceptBlock() : incorrect %s", IsProofOfWork() ? "proof-of-work" : "proof-of-stake"));

    // Check timestamp against prev
    if (GetBlockTime() <= pindexPrev->GetPastTimeLimit() || FutureDrift(GetBlockTime(), nHeight) < pindexPrev->GetBlockTime())
        return error("AcceptBlock() : block's timestamp is too early");

    // Check that all transactions are finalized
    BOOST_FOREACH(const CTransaction& tx, vtx)
        if (!IsFinalTx(tx, nHeight, GetBlockTime()))
            return DoS(10, error("AcceptBlock() : contains a non-final transaction"));

    // Check that the block chain matches the known block chain up to a checkpoint
    if (!Checkpoints::CheckHardened(nHeight, hash))
        return DoS(10, error("AcceptBlock() : rejected by hardened checkpoint lock-in at %d", nHeight));

    uint256 hashProof;
    // Verify hash target and signature of coinstake tx
    if (IsProofOfStake())
    {
        uint256 targetProofOfStake;
        if (!CheckProofOfStake(pindexPrev, vtx[1], nBits, hashProof, targetProofOfStake))
        {
            return error("AcceptBlock() : check proof-of-stake failed for block %s", hash.ToString());
        }
    }
    // PoW is checked in CheckBlock()
    if (IsProofOfWork())
    {
        hashProof = GetPoWHash();
    }

    // Check that the block satisfies synchronized checkpoint
    if (!Checkpoints::CheckSync(nHeight))
        return error("AcceptBlock() : rejected by synchronized checkpoint");

    // Enforce rule that the coinbase starts with serialized block height
    CScript expect = CScript() << nHeight;
    if (vtx[0].vin[0].scriptSig.size() < expect.size() ||
        !std::equal(expect.begin(), expect.end(), vtx[0].vin[0].scriptSig.begin()))
        return DoS(100, error("AcceptBlock() : block height mismatch in coinbase"));

    // Write block to history file
    if (!CheckDiskSpace(::GetSerializeSize(*this, SER_DISK, CLIENT_VERSION)))
        return error("AcceptBlock() : out of disk space");
    unsigned int nFile = -1;
    unsigned int nBlockPos = 0;
    if (!WriteToDisk(nFile, nBlockPos))
        return error("AcceptBlock() : WriteToDisk failed");
    if (!AddToBlockIndex(nFile, nBlockPos, hashProof))
        return error("AcceptBlock() : AddToBlockIndex failed");

    // Relay inventory, but don't relay old inventory during initial block download
    int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate();
    if (hashBestChain == hash)
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            if (nBestHeight > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate))
                pnode->PushInventory(CInv(MSG_BLOCK, hash));
    }

    return true;
}

bool CBlock::CheckDevRewards(CTransaction tx, int64_t nHeight, int64_t nReward, int64_t nFees)
{
    // Check that the transaction contains
    // the 3 dev reward outputs with the
    // appropriate % of the total nReward for the block

    int64_t nDevReward = 0.02 * nReward; // 2% per dev reward

    if (nHeight > PEPE_REBRAND_PF_HEIGHT)
        nDevReward = 0.04 * nReward; // 4% per dev reward
    if (nHeight == PEPE_REBRAND_PF_HEIGHT)
        nDevReward = PEPE_DEV_GRANT;
    if (nHeight == PEPE_KEKDAQ_MID_FIX_HEIGHT)
        nDevReward = PEPE_DEV_GRANT_MID;
    if (nHeight == PEPE_IPFSMN_FNL_HEIGHT)
        nDevReward = PEPE_DEV_GRANT_FINAL;
    if (nHeight == PEPE_STAKE_CONF_HEIGHT)
        nDevReward = PEPE_DEV_GRANT;
    if (nHeight == PEPE_KEKDAQ2_SWAP_HEIGHT)
        nDevReward = DEVFEE_OFF_SWAP_FINAL; 
    if (nHeight == PEPE_STAKEONLY_HEIGHT)
        nDevReward = PEPE_SO_SWAP_GRANT;             
        
    int64_t nTotalDevRewards = 3 * nDevReward;
    if(nHeight >= PEPE_STAKEONLY_HEIGHT)
        nTotalDevRewards = 4 * nDevReward;

    int64_t nFoundDevRewards = 0;
    
    CBitcoinAddress addrDevOne;
    addrDevOne.SetString(DecodeBase64(PEPE_REBRAND_DEV_1));
    CScript payeeDevOne = GetScriptForDestination(addrDevOne.Get());
    CBitcoinAddress addrDevTwo;
    addrDevTwo.SetString(DecodeBase64(PEPE_REBRAND_DEV_2));
    CScript payeeDevTwo = GetScriptForDestination(addrDevTwo.Get());
    CBitcoinAddress addrDevThree;
    addrDevThree.SetString(DecodeBase64(PEPE_REBRAND_DEV_3));
    CScript payeeDevThree = GetScriptForDestination(addrDevThree.Get());
    CBitcoinAddress addrDevFour;
    addrDevFour.SetString(DecodeBase64(PEPE_DEV_4));
    CScript payeeDevFour = GetScriptForDestination(addrDevFour.Get());

    bool bFoundDevOne = false;
    bool bFoundDevTwo = false;
    bool bFoundDevThree = false;
    bool bFoundDevFour = false;
    for(unsigned int i=0; i<tx.vout.size(); i++)
    {
        txnouttype type;
        vector<CTxDestination> addresses;
        int nRequired;

        if (ExtractDestinations(tx.vout[i].scriptPubKey, type, addresses, nRequired))
        {
            BOOST_FOREACH(const CTxDestination& addr, addresses)
            {    
                CBitcoinAddress addrFound = CBitcoinAddress(addr);
    
                if(addrFound == addrDevOne)
                {
                    if(tx.vout[i].nValue >= nDevReward)
                    {
                        bFoundDevOne = true;
                        nFoundDevRewards += nDevReward;
                    }
                }

                if(addrFound == addrDevTwo)
                {
                    if(tx.vout[i].nValue >= nDevReward)
                    {
                        bFoundDevTwo = true;
                        nFoundDevRewards += nDevReward;
                    }
                }

                if(addrFound == addrDevThree)
                {
                    if(tx.vout[i].nValue >= nDevReward)
                    {
                        bFoundDevThree = true;
                        nFoundDevRewards += nDevReward;
                    }
                }

                if(addrFound == addrDevFour)
                {
                    if(tx.vout[i].nValue >= nDevReward)
                    {
                        bFoundDevFour = true;
                        nFoundDevRewards += nDevReward;
                    }
                }
            }
        }
    }

    if(nHeight >= PEPE_STAKEONLY_HEIGHT)
    {
        if((bFoundDevOne && bFoundDevTwo && bFoundDevThree && bFoundDevFour) && (nFoundDevRewards >= nTotalDevRewards))
            return true;
        else
            return false;
    }
    else
    {
        if((bFoundDevOne && bFoundDevTwo && bFoundDevThree) && (nFoundDevRewards >= nTotalDevRewards))
            return true;
        else
            return false;
    }
}

uint256 CBlockIndex::GetBlockTrust() const
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    if (bnTarget <= 0)
        return 0;

    return ((CBigNum(1)<<256) / (bnTarget+1)).getuint256();
}

void PushGetBlocks(CNode* pnode, CBlockIndex* pindexBegin, uint256 hashEnd)
{
    // Filter out duplicate requests
    if (pindexBegin == pnode->pindexLastGetBlocksBegin && hashEnd == pnode->hashLastGetBlocksEnd)
        return;
    pnode->pindexLastGetBlocksBegin = pindexBegin;
    pnode->hashLastGetBlocksEnd = hashEnd;

    pnode->PushMessage("getblocks", CBlockLocator(pindexBegin), hashEnd);
}

bool static IsCanonicalBlockSignature(CBlock* pblock)
{
    if (pblock->IsProofOfWork()) {
        return pblock->vchBlockSig.empty();
    }

    return IsDERSignature(pblock->vchBlockSig, false);
}

bool ProcessBlock(CNode* pfrom, CBlock* pblock)
{
    AssertLockHeld(cs_main);

    // Check for duplicate
    uint256 hash = pblock->GetHash();
    if (mapBlockIndex.count(hash))
        return error("ProcessBlock() : already have block %d %s", mapBlockIndex[hash]->nHeight, hash.ToString());
    if (mapOrphanBlocks.count(hash))
        return error("ProcessBlock() : already have block (orphan) %s", hash.ToString());

    // ppcoin: check proof-of-stake
    // Limited duplicity on stake: prevents block flood attack
    // Duplicate stake allowed only when there is orphan child block
    if (!fReindex && !fImporting && pblock->IsProofOfStake() && setStakeSeen.count(pblock->GetProofOfStake()) && !mapOrphanBlocksByPrev.count(hash))
        return error("ProcessBlock() : duplicate proof-of-stake (%s, %d) for block %s", pblock->GetProofOfStake().first.ToString(), pblock->GetProofOfStake().second, hash.ToString());

    if (pblock->hashPrevBlock != hashBestChain)
    {
        // Extra checks to prevent "fill up memory by spamming with bogus blocks"
        const CBlockIndex* pcheckpoint = Checkpoints::AutoSelectSyncCheckpoint();
        int64_t deltaTime = pblock->GetBlockTime() - pcheckpoint->nTime;
        if (deltaTime < 0)
        {
            if (pfrom)
                pfrom->Misbehaving(1);
            return error("ProcessBlock() : block with timestamp before last checkpoint");
        }
    }

    // Block signature can be malleated in such a way that it increases block size up to maximum allowed by protocol
    // For now we just strip garbage from newly received blocks
    if (!IsCanonicalBlockSignature(pblock)) {
        return error("ProcessBlock(): bad block signature encoding");
    }

    // Preliminary checks
    if (!pblock->CheckBlock())
        return error("ProcessBlock() : CheckBlock FAILED");

    // If we don't already have its previous block, shunt it off to holding area until we get it
    if (!mapBlockIndex.count(pblock->hashPrevBlock))
    {
        if(IsSyncing())
        {
            pfrom->fDisconnect = true;
            pfrom->fStartSync = false;
            nAskedForBlocks = 0;
            return error("ProcessBlock(): ORPHAN detected, but we syncing.  Disconnecting from peer.\n");
        }

        LogPrintf("ProcessBlock: ORPHAN BLOCK %lu, prev=%s\n", (unsigned long)mapOrphanBlocks.size(), pblock->hashPrevBlock.ToString());

        // Accept orphans as long as there is a node to request its parents from
        if (pfrom) {
            // ppcoin: check proof-of-stake
            if (pblock->IsProofOfStake())
            {
                // Limited duplicity on stake: prevents block flood attack
                // Duplicate stake allowed only when there is orphan child block
                if (setStakeSeenOrphan.count(pblock->GetProofOfStake()) && !mapOrphanBlocksByPrev.count(hash))
                    return error("ProcessBlock() : duplicate proof-of-stake (%s, %d) for orphan block %s", pblock->GetProofOfStake().first.ToString(), pblock->GetProofOfStake().second, hash.ToString());
            }
            PruneOrphanBlocks();
            COrphanBlock* pblock2 = new COrphanBlock();
            {
                CDataStream ss(SER_DISK, CLIENT_VERSION);
                ss << *pblock;
                pblock2->vchBlock = std::vector<unsigned char>(ss.begin(), ss.end());
            }
            pblock2->hashBlock = hash;
            pblock2->hashPrev = pblock->hashPrevBlock;
            pblock2->stake = pblock->GetProofOfStake();
            mapOrphanBlocks.insert(make_pair(hash, pblock2));
            mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrev, pblock2));
            if (pblock->IsProofOfStake())
                setStakeSeenOrphan.insert(pblock->GetProofOfStake());

            // Ask this guy to fill in what we're missing
            PushGetBlocks(pfrom, pindexBest, GetOrphanRoot(hash));
            // ppcoin: getblocks may not obtain the ancestor block rejected
            // earlier by duplicate-stake check so we ask for it again directly
            if (!IsInitialBlockDownload())
                pfrom->AskFor(CInv(MSG_BLOCK, WantedByOrphan(pblock2)));
        }
        return true;
    }

    // Store to disk
    if (!pblock->AcceptBlock())
        return error("ProcessBlock() : AcceptBlock FAILED");

    // Recursively process any orphan blocks that depended on this one
    vector<uint256> vWorkQueue;
    vWorkQueue.push_back(hash);
    for (unsigned int i = 0; i < vWorkQueue.size(); i++)
    {
        uint256 hashPrev = vWorkQueue[i];
        for (multimap<uint256, COrphanBlock*>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
             mi != mapOrphanBlocksByPrev.upper_bound(hashPrev);
             ++mi)
        {
            CBlock block;
            {
                CDataStream ss(mi->second->vchBlock, SER_DISK, CLIENT_VERSION);
                ss >> block;
            }
            block.BuildMerkleTree();
            if (block.AcceptBlock())
                vWorkQueue.push_back(mi->second->hashBlock);
            mapOrphanBlocks.erase(mi->second->hashBlock);
            setStakeSeenOrphan.erase(block.GetProofOfStake());
            delete mi->second;
        }
        mapOrphanBlocksByPrev.erase(hashPrev);
    }

    // Cache any found pepe messages
    if(GetBoolArg("-messagewall", false))
    {
        CTxDB txdb("rw");
        BOOST_FOREACH(const CTransaction& tx, pblock->vtx)
        {
            BOOST_FOREACH(const CTxOut vout, tx.vout)
            {
                if(vout.scriptPubKey.size() > 0 && vout.scriptPubKey[0] == OP_RETURN)
                {
                    std::vector<unsigned char> vch(vout.scriptPubKey.begin()+2,vout.scriptPubKey.end());                
                    std::string astring(vch.begin(), vch.end());
                    CPepeMessage pmsg;
                    pmsg.nTime = tx.nTime;
                    pmsg.msg = astring;
                    txdb.WritePepeMessage(pmsg.GetHash(), pmsg);
                    if(mapPepeMessages.count(pmsg.GetHash()) == 0)
                        mapPepeMessages.insert(make_pair(pmsg.GetHash(), pmsg));
                }
            }
        }
    }   

    int nShift = (pblock->nBits >> 24) & 0xff;
    double dDiff = (double)0x0000ffff / (double)(pblock->nBits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }
    LogPrintf("ProcessBlock: ACCEPTED Height: %d Difficulty: %d\n", mapBlockIndex[hash]->nHeight, dDiff);

#ifdef ENABLE_WALLET
    pwalletMain->fCachedBalanceNeedsUpdating = true;  // update cached balance in case coins came out of stake with the accepted block
#endif

    return true;
}

#ifdef ENABLE_WALLET
// novacoin: attempt to generate suitable proof-of-stake
bool CBlock::SignBlock(CWallet& wallet, int64_t nFees)
{
    // if we are trying to sign
    //    something except proof-of-stake block template
    if (!vtx[0].vout[0].IsEmpty())
        return false;

    // if we are trying to sign
    //    a complete proof-of-stake block
    if (IsProofOfStake())
        return true;

    static int64_t nLastCoinStakeSearchTime = GetAdjustedTime(); // startup timestamp

    CKey key;
    CTransaction txCoinStake;
    txCoinStake.nTime &= ~STAKE_TIMESTAMP_MASK;

    int64_t nSearchTime = txCoinStake.nTime; // search to current time

    if (nSearchTime > nLastCoinStakeSearchTime)
    {
        int64_t nSearchInterval = 1;
        if (wallet.CreateCoinStake(wallet, nBits, nSearchInterval, nFees, txCoinStake, key))
        {
            if (txCoinStake.nTime >= pindexBest->GetPastTimeLimit()+1)
            {
                // make sure coinstake would meet timestamp protocol
                //    as it would be the same as the block timestamp
                vtx[0].nTime = nTime = txCoinStake.nTime;

                // we have to make sure that we have no future timestamps in
                //    our transactions set
                for (vector<CTransaction>::iterator it = vtx.begin(); it != vtx.end();)
                    if (it->nTime > nTime) { it = vtx.erase(it); } else { ++it; }

                vtx.insert(vtx.begin() + 1, txCoinStake);
                hashMerkleRoot = BuildMerkleTree();

                // append a signature to our block
                return key.Sign(GetHash(), vchBlockSig);
            }
        }
        nLastCoinStakeSearchInterval = nSearchTime - nLastCoinStakeSearchTime;
        nLastCoinStakeSearchTime = nSearchTime;
    }

    return false;
}
#endif

bool CBlock::CheckBlockSignature() const
{
    if (IsProofOfWork())
        return vchBlockSig.empty();

    if (vchBlockSig.empty())
        return false;

    vector<valtype> vSolutions;
    txnouttype whichType;

    const CTxOut& txout = vtx[1].vout[1];

    if (!Solver(txout.scriptPubKey, whichType, vSolutions))
        return false;

    if (whichType == TX_PUBKEY)
    {
        valtype& vchPubKey = vSolutions[0];
        return CPubKey(vchPubKey).Verify(GetHash(), vchBlockSig);
    }

    return false;
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
    uint64_t nFreeBytesAvailable = filesystem::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
    {
        string strMessage = _("Error: Disk space is low!");
        strMiscWarning = strMessage;
        LogPrintf("*** %s\n", strMessage);
        uiInterface.ThreadSafeMessageBox(strMessage, "", CClientUIInterface::MSG_ERROR);
        StartShutdown();
        return false;
    }
    return true;
}

static filesystem::path BlockFilePath(unsigned int nFile)
{
    string strBlockFn = strprintf("blk%04u.dat", nFile);
    return GetDataDir() / strBlockFn;
}

FILE* OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char* pszMode)
{
    if ((nFile < 1) || (nFile == (unsigned int) -1))
        return NULL;
    FILE* file = fopen(BlockFilePath(nFile).string().c_str(), pszMode);
    if (!file)
        return NULL;
    if (nBlockPos != 0 && !strchr(pszMode, 'a') && !strchr(pszMode, 'w'))
    {
        if (fseek(file, nBlockPos, SEEK_SET) != 0)
        {
            fclose(file);
            return NULL;
        }
    }
    return file;
}

static unsigned int nCurrentBlockFile = 1;

FILE* AppendBlockFile(unsigned int& nFileRet)
{
    nFileRet = 0;
    while (true)
    {
        FILE* file = OpenBlockFile(nCurrentBlockFile, 0, "ab");
        if (!file)
            return NULL;
        if (fseek(file, 0, SEEK_END) != 0)
            return NULL;
        // FAT32 file size max 4GB, fseek and ftell max 2GB, so we must stay under 2GB
        if (ftell(file) < (long)(0x7F000000 - MAX_SIZE))
        {
            nFileRet = nCurrentBlockFile;
            return file;
        }
        fclose(file);
        nCurrentBlockFile++;
    }
}

bool LoadBlockIndex(bool fAllowNew)
{
    LOCK(cs_main);

    if (TestNet())
    {
        nCoinbaseMaturity = 10; // test maturity is 10 blocks
    }

    //
    // Load block index
    //
    CTxDB txdb("cr+");
    if (!txdb.LoadBlockIndex())
        return false;

    //
    // Init with genesis block
    //
    if (mapBlockIndex.empty())
    {
        if (!fAllowNew)
            return false;

        CBlock &block = const_cast<CBlock&>(Params().GenesisBlock());
        // Start new block file
        unsigned int nFile;
        unsigned int nBlockPos;
        if (!block.WriteToDisk(nFile, nBlockPos))
            return error("LoadBlockIndex() : writing genesis block to disk failed");
        if (!block.AddToBlockIndex(nFile, nBlockPos, Params().HashGenesisBlock()))
            return error("LoadBlockIndex() : genesis block not accepted");
    }

    return true;
}

bool LoadPepeMessages()
{
    LOCK(cs_main);
    CTxDB txdb("cr+");
    LogPrintf("main LoadPepeMessages\n");
    if(!txdb.LoadPepeMessages())
        return false;

    return true;
}

void PrintBlockTree()
{
    AssertLockHeld(cs_main);
    // pre-compute tree structure
    map<CBlockIndex*, vector<CBlockIndex*> > mapNext;
    for (map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex* pindex = (*mi).second;
        mapNext[pindex->pprev].push_back(pindex);
        // test
        //while (rand() % 3 == 0)
        //    mapNext[pindex->pprev].push_back(pindex);
    }

    vector<pair<int, CBlockIndex*> > vStack;
    vStack.push_back(make_pair(0, pindexGenesisBlock));

    int nPrevCol = 0;
    while (!vStack.empty())
    {
        int nCol = vStack.back().first;
        CBlockIndex* pindex = vStack.back().second;
        vStack.pop_back();

        // print split or gap
        if (nCol > nPrevCol)
        {
            for (int i = 0; i < nCol-1; i++)
                LogPrintf("| ");
            LogPrintf("|\\\n");
        }
        else if (nCol < nPrevCol)
        {
            for (int i = 0; i < nCol; i++)
                LogPrintf("| ");
            LogPrintf("|\n");
       }
        nPrevCol = nCol;

        // print columns
        for (int i = 0; i < nCol; i++)
            LogPrintf("| ");

        // print item
        CBlock block;
        block.ReadFromDisk(pindex);
        LogPrintf("%d (%u,%u) %s  %08x  %s  mint %7s  tx %u",
            pindex->nHeight,
            pindex->nFile,
            pindex->nBlockPos,
            block.GetHash().ToString(),
            block.nBits,
            DateTimeStrFormat("%x %H:%M:%S", block.GetBlockTime()),
            FormatMoney(pindex->nMint),
            block.vtx.size());

        // put the main time-chain first
        vector<CBlockIndex*>& vNext = mapNext[pindex];
        for (unsigned int i = 0; i < vNext.size(); i++)
        {
            if (vNext[i]->pnext)
            {
                swap(vNext[0], vNext[i]);
                break;
            }
        }

        // iterate children
        for (unsigned int i = 0; i < vNext.size(); i++)
            vStack.push_back(make_pair(nCol+i, vNext[i]));
    }
}

bool LoadExternalBlockFile(FILE* fileIn)
{
    int64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    {
        try {
            CAutoFile blkdat(fileIn, SER_DISK, CLIENT_VERSION);
            unsigned int nPos = 0;
            while (nPos != (unsigned int)-1 && blkdat.good())
            {
                boost::this_thread::interruption_point();
                unsigned char pchData[65536];
                do {
                    fseek(blkdat, nPos, SEEK_SET);
                    int nRead = fread(pchData, 1, sizeof(pchData), blkdat);
                    if (nRead <= 8)
                    {
                        nPos = (unsigned int)-1;
                        break;
                    }
                    void* nFind = memchr(pchData, Params().MessageStart()[0], nRead+1-MESSAGE_START_SIZE);
                    if (nFind)
                    {
                        if (memcmp(nFind, Params().MessageStart(), MESSAGE_START_SIZE)==0)
                        {
                            nPos += ((unsigned char*)nFind - pchData) + MESSAGE_START_SIZE;
                            break;
                        }
                        nPos += ((unsigned char*)nFind - pchData) + 1;
                    }
                    else
                        nPos += sizeof(pchData) - MESSAGE_START_SIZE + 1;
                    boost::this_thread::interruption_point();
                } while(true);
                if (nPos == (unsigned int)-1)
                    break;
                fseek(blkdat, nPos, SEEK_SET);
                unsigned int nSize;
                blkdat >> nSize;
                if (nSize > 0 && nSize <= MAX_BLOCK_SIZE)
                {
                    CBlock block;
                    blkdat >> block;
                    LOCK(cs_main);
                    if (ProcessBlock(NULL,&block))
                    {
                        nLoaded++;
                        nPos += 4 + nSize;
                    }
                }
            }
        }
        catch (std::exception &e) {
            LogPrintf("%s() : Deserialize or I/O error caught during load\n",
                   __PRETTY_FUNCTION__);
        }
    }
    LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

struct CImportingNow
{
    CImportingNow() {
        assert(fImporting == false);
        fImporting = true;
    }

    ~CImportingNow() {
        assert(fImporting == true);
        fImporting = false;
    }
};

void ThreadImport(std::vector<boost::filesystem::path> vImportFiles)
{
    RenameThread("pepecoin-loadblk");

    CImportingNow imp;

    // -loadblock=
    BOOST_FOREACH(boost::filesystem::path &path, vImportFiles) {
        FILE *file = fopen(path.string().c_str(), "rb");
        if (file)
            LoadExternalBlockFile(file);
    }

    // hardcoded $DATADIR/bootstrap.dat
    filesystem::path pathBootstrap = GetDataDir() / "bootstrap.dat";
    if (filesystem::exists(pathBootstrap)) {
        FILE *file = fopen(pathBootstrap.string().c_str(), "rb");
        if (file) {
            filesystem::path pathBootstrapOld = GetDataDir() / "bootstrap.dat.old";
            LoadExternalBlockFile(file);
            RenameOver(pathBootstrap, pathBootstrapOld);
        }
    }
}










//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

extern map<uint256, CAlert> mapAlerts;
extern CCriticalSection cs_mapAlerts;

string GetWarnings(string strFor)
{
    int nPriority = 0;
    string strStatusBar;
    string strRPC;

    if (GetBoolArg("-testsafemode", false))
        strRPC = "test";

    if (!CLIENT_VERSION_IS_RELEASE)
        strStatusBar = _("This is a pre-release test build - use at your own risk - do not use for mining or merchant applications");

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "")
    {
        nPriority = 1000;
        strStatusBar = strMiscWarning;
    }

    // Alerts
    {
        LOCK(cs_mapAlerts);
        BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
        {
            const CAlert& alert = item.second;
            if (alert.AppliesToMe() && alert.nPriority > nPriority)
            {
                nPriority = alert.nPriority;
                strStatusBar = alert.strStatusBar;
                if (nPriority > 1000)
                    strRPC = strStatusBar;
            }
        }
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings() : invalid parameter");
    return "error";
}








//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static AlreadyHave(CTxDB& txdb, const CInv& inv)
{
    switch (inv.type)
    {
    case MSG_DSTX:
        return mapDarksendBroadcastTxes.count(inv.hash);
    case MSG_TX:
        {
        bool txInMap = false;
        txInMap = mempool.exists(inv.hash);
        return txInMap ||
               mapOrphanTransactions.count(inv.hash) ||
               txdb.ContainsTx(inv.hash);
        }

    case MSG_BLOCK:
        return mapBlockIndex.count(inv.hash) ||
               mapOrphanBlocks.count(inv.hash);
    case MSG_TXLOCK_REQUEST:
        return mapTxLockReq.count(inv.hash) ||
               mapTxLockReqRejected.count(inv.hash);
    case MSG_TXLOCK_VOTE:
        return mapTxLockVote.count(inv.hash);
    case MSG_SPORK:
        return mapSporks.count(inv.hash);
    case MSG_MASTERNODE_WINNER:
        return mapSeenMasternodeVotes.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}




void static ProcessGetData(CNode* pfrom)
{
    // only proceed if we can get a lock
    // if we can't, don't block and leave it in the queue for next time
    /*TRY_LOCK(cs_main, lockMain);
    if (!lockMain)
        return;*/

    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();

    vector<CInv> vNotFound;

    LOCK(cs_main);

    while (it != pfrom->vRecvGetData.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        const CInv &inv = *it;
        {
            boost::this_thread::interruption_point();
            it++;

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
            {
                // Send block from disk
                map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(inv.hash);
                if (mi != mapBlockIndex.end())
                {
                    CBlock block;
                    block.ReadFromDisk((*mi).second);
                    pfrom->PushMessage("block", block);

                    // Trigger them to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue)
                    {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, hashBestChain));
                        pfrom->PushMessage("inv", vInv);
                        pfrom->hashContinue = 0;
                    }
                }
            }
            else if (inv.IsKnownType())
            {
                if(fDebug) LogPrintf("ProcessGetData -- Starting \n");
                // Send stream from relay memory
                bool pushed = false;
                {
                    LOCK(cs_mapRelay);
                    map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    if (mi != mapRelay.end()) {
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                        if(fDebug) LogPrintf("ProcessGetData -- pushed = true Rest will fail\n");
                        pushed = true;
                    }
                }

                if (!pushed && inv.type == MSG_TX) {

                    CTransaction tx;
                    if (mempool.lookup(inv.hash, tx)) {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << tx;
                        pfrom->PushMessage("tx", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TXLOCK_VOTE) {
                    if(mapTxLockVote.count(inv.hash)){
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapTxLockVote[inv.hash];
                        pfrom->PushMessage("txlvote", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TXLOCK_REQUEST) {
                    if(mapTxLockReq.count(inv.hash)){
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapTxLockReq[inv.hash];
                        pfrom->PushMessage("txlreq", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_SPORK) {
                    if(mapSporks.count(inv.hash)){
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapSporks[inv.hash];
                        pfrom->PushMessage("spork", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_MASTERNODE_WINNER) {
                    if(mapSeenMasternodeVotes.count(inv.hash)){
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapSeenMasternodeVotes[inv.hash];
                        pfrom->PushMessage("mnw", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_DSTX) {
                    if(mapDarksendBroadcastTxes.count(inv.hash)){
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss <<
                            mapDarksendBroadcastTxes[inv.hash].tx <<
                            mapDarksendBroadcastTxes[inv.hash].vin <<
                            mapDarksendBroadcastTxes[inv.hash].vchSig <<
                            mapDarksendBroadcastTxes[inv.hash].sigTime;

                        pfrom->PushMessage("dstx", ss);
                        pushed = true;
                    }
                }
                if (!pushed) {
                    vNotFound.push_back(inv);
                }
            }

            // Track requests for our stuff.
            g_signals.Inventory(inv.hash);

            if (inv.type == MSG_BLOCK  || inv.type == MSG_FILTERED_BLOCK)
                break;
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        pfrom->PushMessage("notfound", vNotFound);
    }
}

bool static ProcessMessage(CNode* pfrom, string strCommand, CDataStream& vRecv, int64_t nTimeReceived)
{
    RandAddSeedPerfmon();
    LogPrint("net", "received: %s (%u bytes) peer=%s\n", strCommand, vRecv.size(), pfrom->addr.ToString());
    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        LogPrintf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }

    if (strCommand == "version")
    {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
        {
            pfrom->Misbehaving(1);
            return false;
        }

        int64_t nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64_t nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;
        if (pfrom->nVersion < MIN_PEER_PROTO_VERSION)
        {
            // disconnect from peers older than this proto version
            LogPrintf("partner %s using obsolete version %i; disconnecting\n", pfrom->addr.ToString(), pfrom->nVersion);
            pfrom->fDisconnect = true;
        pfrom->Misbehaving(100); // just ban them
            return false;
        }

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty())
            vRecv >> pfrom->strSubVer;
        if (!vRecv.empty())
            vRecv >> pfrom->nStartingHeight;
        
        pfrom->fRelayTxes = true;

        pfrom->cleanSubVer = SanitizeString(pfrom->strSubVer);

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1)
        {
            LogPrintf("connected to self at %s, disconnecting\n", pfrom->addr.ToString());
            pfrom->fDisconnect = true;
            return true;
        }

        pfrom->addrLocal = addrMe;
        if (pfrom->fInbound && addrMe.IsRoutable())
        {
            SeenLocal(addrMe);
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        // Change version
        pfrom->PushMessage("verack");
        pfrom->ssSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (!fNoListen && !IsInitialBlockDownload())
            {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                if (addr.IsRoutable())
                {
                    pfrom->PushAddress(addr);
                } else if (IsPeerAddrLocalGood(pfrom)) {
                    addr.SetIP(pfrom->addrLocal);
                    pfrom->PushAddress(addr);
                }
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000)
            {
                pfrom->PushMessage("getaddr");
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        } else {
            if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom)
            {
                addrman.Add(addrFrom, addrFrom);
                addrman.Good(addrFrom);
            }
        }

        //---
        // Ask the first connected node for block updates
        
        if (!pfrom->fClient && !pfrom->fOneShot && !pfrom->fDisconnect &&
            (pfrom->nStartingHeight > nBestHeight) &&
            (pfrom->nVersion < NOBLKS_VERSION_START ||
             pfrom->nVersion > NOBLKS_VERSION_END) &&
             (nAskedForBlocks < 1 || vNodes.size() <= 1))
        {
            pfrom->fStartSync = true;
            nAskedForBlocks++;            
            PushGetBlocks(pfrom, pindexBest, uint256(0));
            pfrom->tGetblocks = GetTimeMillis();
        }
        //---

        // Relay alerts
        {
            LOCK(cs_mapAlerts);
            BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
                item.second.RelayTo(pfrom);
        }

        pfrom->fSuccessfullyConnected = true;

        LogPrintf("receive version message: version %d, blocks=%d, us=%s, them=%s, peer=%s\n", pfrom->nVersion, pfrom->nStartingHeight, addrMe.ToString(), addrFrom.ToString(), pfrom->addr.ToString());

        if (GetBoolArg("-synctime", true))
            AddTimeData(pfrom->addr, nTime);
    }


    else if (pfrom->nVersion == 0)
    {
        // Must have a version message before anything else
        pfrom->Misbehaving(1);
        return false;
    }


    else if (strCommand == "verack")
    {
        pfrom->SetRecvVersion(min(pfrom->nVersion, PROTOCOL_VERSION));
    }


    else if (strCommand == "addr")
    {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
            return true;
        if (vAddr.size() > 1000)
        {
            pfrom->Misbehaving(20);
            return error("message addr size() = %u", vAddr.size());
        }

        // Store the new addresses
        vector<CAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;
        BOOST_FOREACH(CAddress& addr, vAddr)
        {
            boost::this_thread::interruption_point();

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the setAddrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint64_t hashAddr = addr.GetHash();
                    uint256 hashRand = hashSalt ^ (hashAddr<<32) ^ ((GetTime()+hashAddr)/(24*60*60));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    multimap<uint256, CNode*> mapMix;
                    BOOST_FOREACH(CNode* pnode, vNodes)
                    {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    for (multimap<uint256, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot)
            pfrom->fDisconnect = true;
    }

    else if (strCommand == "inv")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            pfrom->Misbehaving(20);
            return error("message inv size() = %u", vInv.size());
        }
        
        // find last block in inv vector
        unsigned int nLastBlock = (unsigned int)(-1);
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
            if (vInv[vInv.size() - 1 - nInv].type == MSG_BLOCK) {
                nLastBlock = vInv.size() - 1 - nInv;
                break;
            }
        }
        
        LOCK(cs_main);
        int nBlocksGet = 0;

        CTxDB txdb("r");

        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            const CInv &inv = vInv[nInv];

            boost::this_thread::interruption_point();
            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = AlreadyHave(txdb, inv);
            LogPrint("net", "  got inventory: %s  %s\n", inv.ToString(), fAlreadyHave ? "have" : "new");

            if (!fAlreadyHave) {
                if (!fImporting)
                    if (inv.type == MSG_BLOCK) {
                        if (pfrom->tGetblocks > pfrom->tBlockInvs || !IsSyncing()) {
                            pfrom->AskFor(inv);
                            nBlocksGet++;
                        }
                    } else
                        pfrom->AskFor(inv);                                 
            } else if (inv.type == MSG_BLOCK && mapOrphanBlocks.count(inv.hash)) {
                PushGetBlocks(pfrom, pindexBest, GetOrphanRoot(inv.hash));
            } else if (nInv == nLastBlock) {
                // In case we are on a very long side-chain, it is possible that we already have
                // the last block in an inv bundle sent in response to getblocks. Try to detect
                // this situation and push another getblocks to continue.
                PushGetBlocks(pfrom, mapBlockIndex[inv.hash], uint256(0));
                if (fDebug)
                    LogPrintf("force request: %s\n", inv.ToString());
            }


            // Track requests for our stuff
            g_signals.Inventory(inv.hash);
        }

        if (nBlocksGet)
            pfrom->tBlockInvs = GetTimeMillis();
    }


    else if (strCommand == "getdata")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            pfrom->Misbehaving(20);
            return error("message getdata size() = %u", vInv.size());
        }

        if (fDebug || (vInv.size() != 1))
            LogPrint("net", "received getdata (%u invsz)\n", vInv.size());

        if ((fDebug && vInv.size() > 0) || (vInv.size() == 1))
            LogPrint("net", "received getdata for: %s\n", vInv[0].ToString());

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        ProcessGetData(pfrom);
    }


    else if (strCommand == "getblocks")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        CBlockIndex* pindex = locator.GetBlockIndex();

        // Send the rest of the chain
        if (pindex)
            pindex = pindex->pnext;
        int nLimit = 500;
        LogPrint("net", "getblocks %d to %s limit %d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString(), nLimit);
        for (; pindex; pindex = pindex->pnext)
        {
            if (pindex->GetBlockHash() == hashStop)
            {
                LogPrint("net", "  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0)
            {
                // When this block is requested, we'll send an inv that'll make them
                // getblocks the next batch of inventory.
                LogPrint("net", "  getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }

    else if (strCommand == "getheaders")
    {
        /*CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;
        LOCK(cs_main);
        CBlockIndex* pindex = NULL;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = locator.GetBlockIndex();
            if (pindex)
                pindex = pindex->pnext;
        }
        vector<CBlock> vHeaders;
        int nLimit = 500;
        LogPrint("net", "getheaders %d to %s\n", (pindex ? pindex->nHeight : -1), hashStop.ToString());
        for (; pindex; pindex = pindex->pnext)
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        pfrom->PushMessage("headers", vHeaders);*/
    }


    else if (strCommand == "tx")
    {
        vector<uint256> vWorkQueue;
        vector<uint256> vEraseQueue;
        CTransaction tx;
        vRecv >> tx;
        CTxDB txdb("r");    

        CInv inv(MSG_TX, tx.GetHash());
    // Check for recently rejected (and do other quick existence checks)
            if (AlreadyHave(txdb, inv))
                return true;

        pfrom->AddInventoryKnown(inv);

        LOCK(cs_main);

        bool fMissingInputs = false;

        mapAlreadyAskedFor.erase(inv);

        if (AcceptToMemoryPool(mempool, tx, true, &fMissingInputs))
        {
            RelayTransaction(tx, inv.hash);
            vWorkQueue.push_back(inv.hash);
            vEraseQueue.push_back(inv.hash);

            // Recursively process any orphan transactions that depended on this one
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                map<uint256, set<uint256> >::iterator itByPrev = mapOrphanTransactionsByPrev.find(vWorkQueue[i]);
                if (itByPrev == mapOrphanTransactionsByPrev.end())
                    continue;
                for (set<uint256>::iterator mi = itByPrev->second.begin();
                     mi != itByPrev->second.end();
                     ++mi)
                {
                    const uint256& orphanTxHash = *mi;
                    CTransaction& orphanTx = mapOrphanTransactions[orphanTxHash];
                    bool fMissingInputs2 = false;

                    if (AcceptToMemoryPool(mempool, orphanTx, true, &fMissingInputs2))
                    {
                        LogPrint("mempool", "   accepted orphan tx %s\n", orphanTxHash.ToString());
                        RelayTransaction(orphanTx, orphanTxHash);
                        vWorkQueue.push_back(orphanTxHash);
                        vEraseQueue.push_back(orphanTxHash);
                    }
                    else if (!fMissingInputs2)
                    {
                        // invalid or too-little-fee orphan
                        vEraseQueue.push_back(orphanTxHash);
                        LogPrint("mempool", "   removed orphan tx %s\n", orphanTxHash.ToString());
                    }
                }
            }

            BOOST_FOREACH(uint256 hash, vEraseQueue)
                EraseOrphanTx(hash);
        }
        else if (fMissingInputs)
        {
            AddOrphanTx(tx);

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nEvicted = LimitOrphanTxSize(MAX_ORPHAN_TRANSACTIONS);
            if (nEvicted > 0)
                LogPrint("mempool", "mapOrphan overflow, removed %u tx\n", nEvicted);
        }
        if (tx.nDoS) pfrom->Misbehaving(tx.nDoS);
    }

    else if (strCommand == "block" && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        int size = vRecv.size();
        CBlock block;
        vRecv >> block;
        uint256 hashBlock = block.GetHash();

        //LogPrint("net", "received block %s\n", hashBlock.ToString());

        CInv inv(MSG_BLOCK, hashBlock);

        pfrom->AddInventoryKnown(inv);

        int timetodownload = GetTimeMillis() - pfrom->tBlockRecved/1000;
        if (timetodownload > 1000)
            LogPrint("net", "received block %s (%u bytes, %us, %uB/s) peer=%d\n",
              inv.hash.ToString(), size, timetodownload / 1000, size * 1000 / timetodownload, pfrom->id);
        else
            LogPrint("net", "received block %s (%u bytes) peer=%d\n", inv.hash.ToString(), size, pfrom->id);

        LOCK(cs_main);

        if (ProcessBlock(pfrom, &block))
        {
            mapAlreadyAskedFor.erase(inv);
            pfrom->tBlockRecved = GetTimeMillis();
        }
        if (block.nDoS) pfrom->Misbehaving(block.nDoS);
        if (fSecMsgEnabled)
            SecureMsgScanBlock(block);
    }

    // This asymmetric behavior for inbound and outbound connections was introduced
    // to prevent a fingerprinting attack: an attacker can send specific fake addresses
    // to users' AddrMan and later request them by sending getaddr messages.
    // Making users (which are behind NAT and can only make outgoing connections) ignore
    // getaddr message mitigates the attack.
    else if ((strCommand == "getaddr") && (pfrom->fInbound))
    {
        // Don't return addresses older than nCutOff timestamp
        int64_t nCutOff = GetTime() - (nNodeLifespan * 24 * 60 * 60);
        pfrom->vAddrToSend.clear();
        vector<CAddress> vAddr = addrman.GetAddr();
        BOOST_FOREACH(const CAddress &addr, vAddr)
            if(addr.nTime > nCutOff)
                pfrom->PushAddress(addr);
    }


    else if (strCommand == "mempool")
    {
        LOCK(cs_main);

        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);
        vector<CInv> vInv;
        for (unsigned int i = 0; i < vtxid.size(); i++) {
            CInv inv(MSG_TX, vtxid[i]);
            vInv.push_back(inv);
            if (i == (MAX_INV_SZ - 1))
                    break;
        }
        if (vInv.size() > 0)
            pfrom->PushMessage("inv", vInv);
    }


    else if (strCommand == "ping")
    {
        if (pfrom->nVersion > BIP0031_VERSION)
        {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            pfrom->PushMessage("pong", nonce);
        }
    }


    else if (strCommand == "pong")
    {
        int64_t pingUsecEnd = nTimeReceived;
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;

        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (pfrom->nPingNonceSent != 0) {
                if (nonce == pfrom->nPingNonceSent) {
                    // Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                    if (pingUsecTime > 0) {
                        // Successful ping time measurement, replace previous
                        pfrom->nPingUsecTime = pingUsecTime;
                    } else {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                } else {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0) {
                        // This is most likely a bug in another implementation somewhere, cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            } else {
                sProblem = "Unsolicited pong without ping";
            }
        } else {
            // This is most likely a bug in another implementation somewhere, cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty())) {
            LogPrint("net", "pong %s %s: %s, %x expected, %x received, %zu bytes\n"
                , pfrom->addr.ToString()
                , pfrom->strSubVer
                , sProblem
                , pfrom->nPingNonceSent
                , nonce
                , nAvail);
        }
        if (bPingFinished) {
            pfrom->nPingNonceSent = 0;
        }
    }


    else if (strCommand == "alert")
    {
        CAlert alert;
        vRecv >> alert;

        uint256 alertHash = alert.GetHash();
        if (pfrom->setKnown.count(alertHash) == 0)
        {
            if (alert.ProcessAlert())
            {
                // Relay
                pfrom->setKnown.insert(alertHash);
                {
                    LOCK(cs_vNodes);
                    BOOST_FOREACH(CNode* pnode, vNodes)
                        alert.RelayTo(pnode);
                }
            }
            else {
                // Small DoS penalty so peers that send us lots of
                // duplicate/expired/invalid-signature/whatever alerts
                // eventually get banned.
                // This isn't a Misbehaving(100) (immediate ban) because the
                // peer might be an older or different implementation with
                // a different signature key, etc.
                pfrom->Misbehaving(10);
            }
        }
    }


    else
    {               
        if(IsSyncing() && !pfrom->fStartSync)
        {
            pfrom->fDisconnect = true;        
        }
        else if(!fLiteMode && !IsSyncing())
        {           
            if (fSecMsgEnabled)
                SecureMsgReceiveData(pfrom, strCommand, vRecv);

            ProcessMessageDarksend(pfrom, strCommand, vRecv);
            ProcessMessageMasternode(pfrom, strCommand, vRecv);
            ProcessMessageInstantX(pfrom, strCommand, vRecv);
            ProcessSpork(pfrom, strCommand, vRecv);
        }

        // Ignore unknown commands for extensibility
    }


    // Update the last seen time for this node's address
    if (pfrom->fNetworkNode)
        if (strCommand == "version" || strCommand == "addr" || strCommand == "inv" || strCommand == "getdata" || strCommand == "ping")
            AddressCurrentlyConnected(pfrom->addr);


    return true;
}

// requires LOCK(cs_vRecvMsg)
bool ProcessMessages(CNode* pfrom)
{
    //if (fDebug)
    //    LogPrintf("ProcessMessages(%zu messages)\n", pfrom->vRecvMsg.size());

    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fOk = true;    

    if (!pfrom->vRecvGetData.empty())
        ProcessGetData(pfrom);

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty()) return fOk;

    std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
    while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end()) {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize())
            break;

        // get next message
        CNetMessage& msg = *it;
        CMessageHeader& hdr = msg.hdr;
        unsigned int nMessageSize = hdr.nMessageSize;
        string strCommand = hdr.GetCommand();

        //if (fDebug)
        //    LogPrintf("ProcessMessages(message %u msgsz, %zu bytes, complete:%s)\n",
        //            msg.hdr.nMessageSize, msg.vRecv.size(),
        //            msg.complete() ? "Y" : "N");

        if (msg.nDataPos != msg.nLastDataPos) {
            if (strCommand == "block") {
                if (msg.nLastDataPos == 0) {
                    pfrom->tBlockRecvStart = GetTimeMillis();                
                    if (pfrom->tBlockRecved)
                        LogPrint("net", "%ums later, ", pfrom->tBlockRecvStart - pfrom->tBlockRecved);
                    LogPrint("net", "incoming block (%u bytes) peer=%d\n", nMessageSize, pfrom->id);
                }
                pfrom->tBlockRecving = GetTimeMillis();
            }
            msg.nLastDataPos = msg.nDataPos;
        }

        // end, if an incomplete message is found
        if (!msg.complete())
            break;

        // at this point, any failure means we can delete the current message
        it++;

        // Scan for message start
        if (memcmp(msg.hdr.pchMessageStart, Params().MessageStart(), MESSAGE_START_SIZE) != 0) {
            LogPrintf("\n\nPROCESSMESSAGE: INVALID MESSAGESTART\n\n");
            fOk = false;
            break;
        }

        // Read header
        //CMessageHeader& hdr = msg.hdr;
        if (!hdr.IsValid())
        {
            LogPrintf("\n\nPROCESSMESSAGE: ERRORS IN HEADER %s\n\n\n", strCommand);
            continue;
        }
        //string strCommand = hdr.GetCommand();

        // Message size
        //unsigned int nMessageSize = hdr.nMessageSize;

        // Checksum
        CDataStream& vRecv = msg.vRecv;
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        if (nChecksum != hdr.nChecksum)
        {
            LogPrintf("ProcessMessages(%s, %u bytes) : CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n",
               strCommand, nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        // Process message
        bool fRet = false;
        try
        {
            fRet = ProcessMessage(pfrom, strCommand, vRecv, msg.nTime);
            boost::this_thread::interruption_point();
        }
        catch (std::ios_base::failure& e)
        {
            if (strstr(e.what(), "end of data"))
            {
                // Allow exceptions from under-length message on vRecv
                LogPrintf("ProcessMessages(%s, %u bytes) : Exception '%s' caught, normally caused by a message being shorter than its stated length\n", strCommand, nMessageSize, e.what());
            }
            else if (strstr(e.what(), "size too large"))
            {
                // Allow exceptions from over-long size
                LogPrintf("ProcessMessages(%s, %u bytes) : Exception '%s' caught\n", strCommand, nMessageSize, e.what());
            }
            else
            {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        }
        catch (boost::thread_interrupted) {
            throw;
        }
        catch (std::exception& e) {
            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
            LogPrintf("ProcessMessage(%s, %u bytes) FAILED\n", strCommand, nMessageSize);

        break;
    }

    // In case the connection got shut down, its receive buffer was wiped
    if (!pfrom->fDisconnect)
        pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);

    return fOk;
}


bool SendMessages(CNode* pto, bool fSendTrickle)
{
    TRY_LOCK(cs_main, lockMain);
    if (lockMain) {
        // Don't send anything until we get their version message
        if (pto->nVersion == 0)
            return true;

        //
        // Message: ping
        //
        bool pingSend = false;
        if (pto->fPingQueued) {
            // RPC ping request by user
            pingSend = true;
        }
        if (pto->nPingNonceSent == 0 && pto->nPingUsecStart + PING_INTERVAL * 1000000 < GetTimeMicros()) {
            // Ping automatically sent as a latency probe & keepalive.
            pingSend = true;
        }
        if (pingSend) {
            uint64_t nonce = 0;
            while (nonce == 0) {
                GetRandBytes((unsigned char*)&nonce, sizeof(nonce));
            }
            pto->fPingQueued = false;
            pto->nPingUsecStart = GetTimeMicros();
            if (pto->nVersion > BIP0031_VERSION) {
                pto->nPingNonceSent = nonce;
                pto->PushMessage("ping", nonce);
            } else {
                // Peer is too old to support ping command with nonce, pong will never arrive.
                pto->nPingNonceSent = 0;
                pto->PushMessage("ping");
            }
        }

        //TRY_LOCK(cs_main, lockMain); // Acquire cs_main for IsInitialBlockDownload() and CNodeState()
        //if (!lockMain)
        //    return true;

        // Start block sync
     /*   if (pto->fStartSync && !fImporting && !fReindex) {
            pto->fStartSync = false;
            PushGetBlocks(pto, pindexBest, uint256(0));
        }*/

        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!fReindex && !fImporting && !IsInitialBlockDownload())
        {
            ResendWalletTransactions();
        }

        // Address refresh broadcast
        static int64_t nLastRebroadcast;
        if (!IsInitialBlockDownload() && (GetTime() - nLastRebroadcast > 24 * 60 * 60))
        {
            {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    // Periodically clear setAddrKnown to allow refresh broadcasts
                    if (nLastRebroadcast)
                        pnode->setAddrKnown.clear();

                    // Rebroadcast our address
                    if (!fNoListen)
                    {
                        CAddress addr = GetLocalAddress(&pnode->addr);
                        if (addr.IsRoutable())
                            pnode->PushAddress(addr);
                    }
                }
            }
            nLastRebroadcast = GetTime();
        }

        //
        // Message: addr
        //
        if (fSendTrickle)
        {
            vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            BOOST_FOREACH(const CAddress& addr, pto->vAddrToSend)
            {
                // returns true if wasn't already contained in the set
                if (pto->setAddrKnown.insert(addr).second)
                {
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        pto->PushMessage("addr", vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage("addr", vAddr);
        }


        //
        // Message: inventory
        //
        vector<CInv> vInv;
        vector<CInv> vInvWait;
        {
            LOCK(pto->cs_inventory);
            vInv.reserve(pto->vInventoryToSend.size());
            vInvWait.reserve(pto->vInventoryToSend.size());
            BOOST_FOREACH(const CInv& inv, pto->vInventoryToSend)
            {
                if (pto->setInventoryKnown.count(inv))
                    continue;

            // --- Don't trickle tx's, network is too small and results in transaction delays ---
                // trickle out tx inv to protect privacy
               /* if (inv.type == MSG_TX && !fSendTrickle)
                {
                    // 1/4 of tx invs blast to all immediately
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint256 hashRand = inv.hash ^ hashSalt;
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    bool fTrickleWait = ((hashRand & 3) != 0);

                    if (fTrickleWait)
                    {
                        vInvWait.push_back(inv);
                        continue;
                    }
                }*/

                // returns true if wasn't already contained in the set
                if (pto->setInventoryKnown.insert(inv).second)
                {
                    vInv.push_back(inv);
                    if (vInv.size() >= 1000)
                    {
                        pto->PushMessage("inv", vInv);
                        vInv.clear();
                    }
                }
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
            pto->PushMessage("inv", vInv);

        // Detect stalled peers.
        int nSyncTimeout = GetArg("-synctimeout", 60);
        int64_t tNow = GetTimeMillis();
        if (pto->tGetblocks && IsSyncing()) {
            if (pto->tBlockRecving > pto->tBlockRecved) {
                if (tNow-pto->tBlockRecving > nSyncTimeout * 1000) {
                    LogPrintf("sync peer=%d: Block download stalled for over %d seconds.\n", pto->id, nSyncTimeout);
                    pto->fDisconnect = true;
                    pto->fStartSync = false;
                    nAskedForBlocks = 0;
                }
            } else if (pto->tGetblocks > pto->tBlockInvs && tNow-pto->tGetblocks > nSyncTimeout * 1000) {
                LogPrintf("sync peer=%d: No invs of new blocks received within %d seconds.\n", pto->id, nSyncTimeout);
                pto->fDisconnect = true;
                pto->fStartSync = false;
                nAskedForBlocks = 0;
            } else if (IsSyncing() && pto->tBlockRecved && tNow-pto->tBlockRecved > nSyncTimeout * 1000) {
                LogPrintf("sync peer=%d: No block reception for over %d seconds.\n", pto->id, nSyncTimeout);
                pto->fDisconnect = true;                
                pto->fStartSync = false;
                nAskedForBlocks = 0;
            } else if (pto->tGetdataBlock > pto->tBlockRecving && tNow-pto->tGetdataBlock > nSyncTimeout * 1000) {
                LogPrintf("sync peer=%d: No block download started for over %d seconds.\n", pto->id, nSyncTimeout);
                pto->fDisconnect = true;
                pto->fStartSync = false;
                nAskedForBlocks = 0;
            }
        }

        //
        // Message: getdata
        //
        vector<CInv> vGetData;
        int64_t nNow = GetTime() * 1000000;
        CTxDB txdb("r");
        while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(txdb, inv))
            {
                if (fDebug)
                    LogPrint("net", "sending getdata: %s\n", inv.ToString());
                vGetData.push_back(inv);
                if (!pto->tGetdataBlock)
                    pto->tGetdataBlock = GetTimeMillis();
                if (vGetData.size() >= 1000)
                {
                    pto->PushMessage("getdata", vGetData);
                    vGetData.clear();
                }
                mapAlreadyAskedFor[inv] = nNow;
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            pto->PushMessage("getdata", vGetData);

        if (fSecMsgEnabled)
            SecureMsgSendData(pto, fSendTrickle); // should be in cs_main?

    }
    return true;
}



int64_t GetMasternodePayment(int nHeight, int64_t blockValue)
{
    int64_t ret = blockValue * 0.375; // 37.5% Masternodes pay + staking

    return ret;
}

void RescanPepeMessages()
{
    // scan the entire block index looking for messages
    CBlockIndex* pindex = pindexBest;
    CTxDB txdb("rw");
    mapPepeMessages.clear();
    while(pindex != NULL)
    {
        //LogPrintf("entered while loop\n");
        CBlock block;
        if(pindex != NULL && block.ReadFromDisk(pindex))
        {
           // LogPrintf("Read block from disk\n");
            BOOST_FOREACH(const CTransaction& tx, block.vtx)
            {
                //LogPrintf("getMessages found tx\n");
                BOOST_FOREACH(const CTxOut vout, tx.vout)
                {
                   // LogPrintf("getMessages found txout\n");
                    if(vout.scriptPubKey.size() > 0 && vout.scriptPubKey[0] == OP_RETURN)
                    {
                       // LogPrintf("found op_return\n");
                        std::vector<unsigned char> vch(vout.scriptPubKey.begin()+2,vout.scriptPubKey.end());
                        //LogPrintf("casting to astring\n");
                        //std::string astring(reinterpret_cast<char*>(&vch[0]), vch.size());
                        std::string astring(vch.begin(), vch.end());
                        CPepeMessage pmsg;
                        pmsg.nTime = tx.nTime;
                        pmsg.msg = astring;
                        txdb.WritePepeMessage(pmsg.GetHash(), pmsg);
                        if(mapPepeMessages.count(pmsg.GetHash()) == 0)
                            mapPepeMessages.insert(make_pair(pmsg.GetHash(), pmsg));
                    }
                }
            }
        } 

        pindex = pindex->pprev;
    }
}
