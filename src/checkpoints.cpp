// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp> // for 'map_list_of()'
#include <boost/foreach.hpp>

#include "checkpoints.h"

#include "txdb.h"
#include "main.h"
#include "uint256.h"


static int nCheckpointSpan = 100;

namespace Checkpoints
{
    typedef std::map<int, uint256> MapCheckpoints;

    //
    // What makes a good checkpoint block?
    // + Is surrounded by blocks with reasonable timestamps
    //   (no blocks before with a timestamp after, none after with
    //    timestamp before)
    // + Contains no strange transactionsg
    //
    static MapCheckpoints mapCheckpoints =
        boost::assign::map_list_of
        (0,	Params().HashGenesisBlock() )
        (14, uint256("0xeb1a88f40f84c13d061a4ededd50f03977cb815f459b44f0acf8bbc3593cd57e"))
        (38, uint256("0xdb2420ff33b119de3bf0eda27c3e4653aa13a63b21009fb2412b3e45dce9a136"))
        (776, uint256("0x19fae8ae0a8c77a239ab8551b57f02cf0ac584a6eaab2b4cc9195745839b24b6"))
        (1863, uint256("0xb9ca005de3783c7be996fc95901394dd08a2213e480015f32ef671e9ed47c7b1"))
        (2578, uint256("0x2984f39527006fe0206a0ae76717f7f193b3007e61cb39e9befab3902c74bb2b"))
        (4381, uint256("0x2e52f5d4bd18f4a413697dfd4913728bfb2a2f680377d7096f31c1399df0ece7"))
        (6347, uint256("0x256b0d65bf31f0a1a2cdaefae07e1b832b8bfe339248af070338a85880cf6f8d"))
        (9139, uint256("0x01d892847b2c609ea2ff06c804fc36cf784da590ca5657a0661970ddc2ff61d2"))
        (13782, uint256("0xe0be7d54614fb55329e8a0a9d2865da0ccca54d032fbc30d3a551644d8be9292"))
        (15420, uint256("0x2a592c35d8b7e26a8d642f2b3e27fb586884b5f12fee6f52b09a1fea5edf2fa1"))
        (19909, uint256("0xf9c933d941b39698dea5078bd5270cc4af0af11b6302c51ca87cf2a13c1cafdc"))
        (24938, uint256("0xb33bd09e56e696e5e0547845c9646d2cefc04ef25e1b90bb9018d9fb89f788da"))
        (28308, uint256("0x77b1e4d7a29655dc33c38d32fa833407f5ad59c7dee76fcc78231387e7311360"))
        (32000, uint256("0x17f909ef335c27bf44655377622927122c5b6824815a867d52cd31d5aaac2873"))
        (48398, uint256("0xdfe376d27cf8c6f11beaa29f4a3966f1011847b9af91ff81fbe69fc03178294b"))
        (71320, uint256("0x4298a1d06c0effb8a6f15205bb5e6d7c6ea3d6ef68a16a40ef9480c49a3cf212"))
        (219777, uint256("0xe9f0929405524b5a3b2888c392351c902a11b7a167b3e12d63720b62d344230f"))
        (333777, uint256("0x45bff45918baa567e50568a5e0218e9796b9ef8ea3bdb44e1e6c17f95e687542"))
        (447777, uint256("0x307f3cb162219d6763439851f4c81ae6d0b867adccc97c6cc8d962b67f0de43a"))
        (549333, uint256("0xfd66916698a7cb627b8187be3852b9df59d23a73e57d2e03eeee2da06a068bed"))   
        (580500, uint256("0xe0dec5f9a0f267fabf46413c331b9a8fc36ddac260a09afa0d0ecbbc574efff6"))
        (581777, uint256("0xf578368df2ca18fe517931d2a51c9cc2a6c5e00b168be4773786ce80e33ed47e"))     
        (600000, uint256("0x69374e4dd79947ac885587b80a2730efef922772b01494885e95c533f415c46a"))
        (736820, uint256("0x40beb7ceb66dd20e22854a77e020bc76bf401c92e3d1e455d711067ab0de0d81"))
        (738522, uint256("0x8cfc6234f870351ab359a5f8d116af4e0203a50de8a111016f081902da2a782e"))
        (746200, uint256("0xd5c3e2c2bf888cc9b3a594fafa8a67c8696bedb39daa2a28c475fe88b64d663f"))
        (834600, uint256("0x0cd92f0d45ceb2c69c8d7845a584e69668cce098c01d3e1ec04af285afe2d378"))
        (850000, uint256("0x010f9fd8aba13696829b5b6003770b8e947df06eaf507de592b4cc3bc9b04192"))
        (885000, uint256("0x0fcb255ff69a157d13ff94bce583f2993b9bc1ed385ecb0d027c95b29c03b222"))
        (889777, uint256("0x86a3ad3af9a93f4a5c5c6d5e020f1a323af4cd1d43fee72d6747177e71320d9e"))
        (982877, uint256("0x5f2d4d7ab746a3373b82a45ec194d854688dff19b354038d71f410ed7bb15ea9"))
        (984066, uint256("0x3497b6650d00fce42f448c1731327ad31616f073b4855331d4bc83923c11312a"))
        (1034505, uint256("0x638a260d83c15e99959b08b1814cbf376136571900f8ba525c66deb8a8965167"))
        (1090777, uint256("0x4c68bdbb07f106476bda8f98417c7ded56d26b8a46a4cd5d102e65076e63d293"))
        (1118701, uint256("0x43481ac2839e6f4ab94d14d7fd683789d4f5a493cbef39f9cc7dcf9348163564"))
        (1790400, uint256("0x0ce318476cf4676e2452c71c0c6b9570f73fc21a332b19b4a0bf6bc391a40f01"))
        (1804422, uint256("0x338dffee2646d7a3a56daa33823b2c617f33d6a9d5ee8dc10f1a5c080c88f11f"))
        (1805024, uint256("0x4d9e9e8011b6267d73b31b4bc3b3f6ae36a7d5e19bdc5e93fef954b2395ac4d3"))
        (1807255, uint256("0xc740d0099fee03a40f7af9a9696dbe64488c87222f2f392b35c180c16b83c0ca"))
        (1809112, uint256("0xa9b12d22d0d153ad26e6a809972d7f57208c08020361284a6c3c0bddc40a167c"))
        (1810063, uint256("0x2821f1bc68322b2bf7fddf34beb3c8ee937f4f1096caad404e8b993f4aa204e2"))
        (1811850, uint256("0x783b25b45ba71a59bdab2e62367ed9de8d7beb6ddcdd2042b29e3b2461865946"))
        (1812077, uint256("0x675f544141a3d2e95b5313d28819fb7370fec4573e101a168b481598d2f7d617"))
        (1813001, uint256("0x9414f13f7aa7a7fab80fe8186949495acb5311f99e311062b6179a7dd6f6bc9f"))
        (1814411, uint256("0x9b37845256869b5dfea201b702fb66ac3897a60b21c5982068ada1bf17617070"))
    ;

    // TestNet has no checkpoints
    static MapCheckpoints mapCheckpointsTestnet;

    bool CheckHardened(int nHeight, const uint256& hash)
    {
        MapCheckpoints& checkpoints = (TestNet() ? mapCheckpointsTestnet : mapCheckpoints);

        MapCheckpoints::const_iterator i = checkpoints.find(nHeight);
        if (i == checkpoints.end()) return true;
        return hash == i->second;
    }

    int GetTotalBlocksEstimate()
    {
        MapCheckpoints& checkpoints = (TestNet() ? mapCheckpointsTestnet : mapCheckpoints);

        if (checkpoints.empty())
            return 0;
        return checkpoints.rbegin()->first;
    }

    CBlockIndex* GetLastCheckpoint(const std::map<uint256, CBlockIndex*>& mapBlockIndex)
    {
        MapCheckpoints& checkpoints = (TestNet() ? mapCheckpointsTestnet : mapCheckpoints);

        BOOST_REVERSE_FOREACH(const MapCheckpoints::value_type& i, checkpoints)
        {
            const uint256& hash = i.second;
            std::map<uint256, CBlockIndex*>::const_iterator t = mapBlockIndex.find(hash);
            if (t != mapBlockIndex.end())
                return t->second;
        }
        return NULL;
    }

    // Automatically select a suitable sync-checkpoint
    const CBlockIndex* AutoSelectSyncCheckpoint()
    {
        // tighten automatic checkpointing to 10 blocks in past for reorg fix attempt
        if (pindexBest->nHeight >= PEPE_JACKOLANTERN_FORK_HEIGHT)
            nCheckpointSpan = 10;

        // set autocheckpointing to 25 blocks max to limit forced resyncs
        if (pindexBest->nHeight >= PEPE_STAKE_CONF_TWEAK)
            nCheckpointSpan = 25;


        const CBlockIndex *pindex = pindexBest;
        // Search backward for a block within max span and maturity window
        while (pindex->pprev && pindex->nHeight + nCheckpointSpan > pindexBest->nHeight)
            pindex = pindex->pprev;
        return pindex;
    }

    // Check against synchronized checkpoint
    bool CheckSync(int nHeight)
    {
        const CBlockIndex* pindexSync = AutoSelectSyncCheckpoint();
        if (nHeight <= pindexSync->nHeight){
            return false;
        }
        return true;
    }
}
