/* @flow */
// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2015-2017 The PIVX Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>
#include <boost/lexical_cast.hpp>
#include <cmath>
#include <algorithm>

#include <db.h>
#include <policy/policy.h>
#include <consensus/kernel.h>
#include <script/interpreter.h>
#include <timedata.h>
#include <time.h>
#include <streams.h>
#include <chainparams.h>
#include <hash.h>

using namespace std;

static bool fTestNet = false; //Params().NetworkID() == CBaseChainParams::TESTNET;

// Modifier interval: time to elapse before new modifier is computed
// Set to 3-hour for production network and 20-minute for test network
unsigned int nModifierInterval;
static int nStakeTargetSpacing = 60;

unsigned int getIntervalVersion(bool fTestNet)
{
    if (fTestNet)
        return MODIFIER_INTERVAL_TESTNET;
    else
        return MODIFIER_INTERVAL;
}

// Hard checkpoints of stake modifiers to ensure they are deterministic
static std::map<int, unsigned int> mapStakeModifierCheckpoints =
        boost::assign::map_list_of(0, 0xfd11f4e7u);

// Get time weight
int64_t GetWeight(int64_t nIntervalBeginning, int64_t nIntervalEnd)
{
    return nIntervalEnd - nIntervalBeginning - nStakeMinAge;
}

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex)
        return error("GetLastStakeModifier: null pindex");
    while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier())
        pindex = pindex->pprev;
    if (!pindex->GeneratedStakeModifier())
        return error("GetLastStakeModifier: no generation at genesis block");
    nStakeModifier = pindex->nStakeModifier;
    nModifierTime = pindex->GetBlockTime();
    return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert(nSection >= 0 && nSection < 64);
    int64_t a = getIntervalVersion(fTestNet) * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1)));
    return a;
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionInterval()
{
    int64_t nSelectionInterval = 0;
    for (int nSection = 0; nSection < 64; nSection++) {
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    }
    return nSelectionInterval;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(
        vector<pair<int64_t, arith_uint256> >& vSortedByTimestamp,
        map<uint256, const CBlockIndex*>& mapSelectedBlocks,
        int64_t nSelectionIntervalStop,
        uint64_t nStakeModifierPrev,
        const CBlockIndex** pindexSelected)
{
    bool fModifierV2 = false;
    bool fFirstRun = true;
    bool fSelected = false;
    arith_uint256 hashBest = 0;
    *pindexSelected = nullptr;
    for (std::pair<int64_t, arith_uint256> item : vSortedByTimestamp) {
        if (!mapBlockIndex.count(ArithToUint256(item.second)))
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString().c_str());

        const CBlockIndex* pindex = mapBlockIndex[ArithToUint256(item.second)];
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;

        //if the lowest block height (vSortedByTimestamp[0]) is >= switch height, use new modifier calc
        if (fFirstRun){
            fModifierV2 = pindex->nHeight >= Params().GetConsensus().nModifierUpdateBlock;
            fFirstRun = false;
        }

        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;

        // compute the selection hash by hashing an input that is unique to that block
        uint256 hashProof;
        if(fModifierV2)
            hashProof = pindex->GetBlockHash();
        else
            hashProof = pindex->IsProofOfStake() ? uint256() : pindex->GetBlockHash();

        CDataStream ss(SER_GETHASH, 0);
        ss << hashProof << nStakeModifierPrev;
        arith_uint256 hashSelection = UintToArith256(Hash(ss.begin(), ss.end()));

        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
            hashSelection >>= 32;

        if (fSelected && hashSelection < hashBest) {
            hashBest = hashSelection;
            *pindexSelected = pindex;
        } else if (!fSelected) {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = pindex;
        }
    }
    if (gArgs.GetBoolArg("-printstakemodifier", false))
        LogPrintf("SelectBlockFromCandidates: selection hash=%s\n", hashBest.ToString().c_str());
    return fSelected;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
static bool ComputeNextStakeModifierV2(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier)
{
    nStakeModifier = 0;
    fGeneratedStakeModifier = false;
    if (!pindexPrev) {
        fGeneratedStakeModifier = true;
        return true; // genesis block's modifier is 0
    }
    if (pindexPrev->nHeight == 0) {
        //Give a stake modifier to the first block
        fGeneratedStakeModifier = true;
        //        nStakeModifier = uint64_t("stakemodifier");
        nStakeModifier = 8093378;
        return true;
    }

    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
        return error("ComputeNextStakeModifier: unable to get last modifier");

    if (gArgs.GetBoolArg("-printstakemodifier", false))
        LogPrintf("%s: prev modifier= %s time=%s\n", __func__, boost::lexical_cast<std::string>(nStakeModifier).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nModifierTime).c_str());

    if (nModifierTime / getIntervalVersion(fTestNet) >= pindexPrev->GetBlockTime() / getIntervalVersion(fTestNet))
        return true;

    // Sort candidate blocks by timestamp
    vector<pair<int64_t, arith_uint256> > vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * getIntervalVersion(fTestNet) / nStakeTargetSpacing);
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / getIntervalVersion(fTestNet)) * getIntervalVersion(fTestNet) - nSelectionInterval;
    const CBlockIndex* pindex = pindexPrev;

    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart) {
        vSortedByTimestamp.push_back(make_pair(pindex->GetBlockTime(), UintToArith256(pindex->GetBlockHash())));
        pindex = pindex->pprev;
    }

    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;
    std::reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
    std::sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end());

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (size_t nRound = 0; nRound < std::min<size_t>(64u, vSortedByTimestamp.size()); nRound++) {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);

        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex))
            return error("ComputeNextStakeModifier: unable to select block at round %d", nRound);

        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);

        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(make_pair(pindex->GetBlockHash(), pindex));
        if (gArgs.GetBoolArg("-printstakemodifier", false))
            LogPrintf("ComputeNextStakeModifier: selected round %d stop=%s height=%d bit=%d\n",
                      nRound, DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nSelectionIntervalStop).c_str(), pindex->nHeight, pindex->GetStakeEntropyBit());
    }

    // Print selection map for visualization of the selected blocks
    if (gArgs.GetBoolArg("-printstakemodifier", false)) {
        string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected
        strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
        pindex = pindexPrev;
        while (pindex && pindex->nHeight >= nHeightFirstCandidate) {
            // '=' indicates proof-of-stake blocks not selected
            if (pindex->IsProofOfStake())
                strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
            pindex = pindex->pprev;
        }
        for (const std::pair<uint256, const CBlockIndex*> &item : mapSelectedBlocks) {
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake() ? "S" : "W");
        }
        LogPrintf("ComputeNextStakeModifier: selection height [%d, %d] map %s\n", nHeightFirstCandidate, pindexPrev->nHeight, strSelectionMap.c_str());
    }
    if (gArgs.GetBoolArg("-printstakemodifier", false)) {
        LogPrintf("ComputeNextStakeModifier: new modifier=%s time=%s\n", boost::lexical_cast<std::string>(nStakeModifierNew).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexPrev->GetBlockTime()).c_str());
    }

    nStakeModifier = nStakeModifierNew;
    fGeneratedStakeModifier = true;
    return true;
}

// The stake modifier used to hash for a stake kernel is chosen as the stake
// modifier about a selection interval later than the coin generating the kernel
bool GetKernelStakeModifier(uint256 hashBlockFrom, unsigned int nMaxHeight, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    nStakeModifier = 0;
    if (!mapBlockIndex.count(hashBlockFrom))
        return error("GetKernelStakeModifier() : block not indexed");
    const CBlockIndex* pindexFrom = mapBlockIndex[hashBlockFrom];
    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->GetBlockTime();
    int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();
    const CBlockIndex* pindex = pindexFrom;
    CBlockIndex* pindexNext = chainActive[pindexFrom->nHeight + 1];

    // loop to find the stake modifier later by a selection interval
    while (nStakeModifierTime < pindexFrom->GetBlockTime() + nStakeModifierSelectionInterval) {
        if (!pindexNext) {
            // Should never happen
            nStakeModifierHeight = pindexFrom->nHeight;
            nStakeModifierTime = pindexFrom->GetBlockTime();
            if(pindex->GeneratedStakeModifier())
            {
                nStakeModifier = pindex->nStakeModifier;
            }
            return true;
        }

        pindex = pindexNext;
        pindexNext = (pindexNext->nHeight + 1 < nMaxHeight) ? chainActive[pindexNext->nHeight + 1] : nullptr;
        if (pindex->GeneratedStakeModifier()) {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
    }

    nStakeModifier = pindex->nStakeModifier;
    return true;
}

uint256 stakeHash(unsigned int nTimeTx, CDataStream ss, unsigned int prevoutIndex, uint256 prevoutHash, unsigned int nTimeBlockFrom)
{
    //Divi will hash in the transaction hash and the index number in order to make sure each hash is unique
    ss << nTimeBlockFrom << prevoutIndex << prevoutHash << nTimeTx;
    return Hash(ss.begin(), ss.end());
}

//test hash vs target
bool stakeTargetHit(uint256 hashProofOfStake, uint64_t nValueIn, arith_uint256 bnTargetPerCoinDay, uint64_t nTimeWeight)
{
    arith_uint256 bnCoinDayWeight = arith_uint256(nValueIn) * nTimeWeight / COIN / 400;

    // Now check if proof-of-stake hash meets target protocol
    return (UintToArith256(hashProofOfStake) < bnCoinDayWeight * bnTargetPerCoinDay);
}

//instead of looping outside and reinitializing variables many times, we will give a nTimeTx and also search interval so that we can do all the hashing here
static bool CheckStakeKernelHashV2(unsigned int nBlockHeight,
                                   unsigned int nBits, const CBlock blockFrom, const CTransaction &txPrev,
                                   const COutPoint prevout, unsigned int& nTimeTx,
                                   unsigned int nHashDrift, bool fCheck, uint256& hashProofOfStake, bool fPrintProofOfStake)
{
    //assign new variables to make it easier to read
    int64_t nValueIn = txPrev.vout[prevout.n].nValue;
    unsigned int nTimeBlockFrom = blockFrom.GetBlockTime();

    if (nTimeTx < nTimeBlockFrom) // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    if (nTimeBlockFrom + nStakeMinAge > nTimeTx) // Min age requirement
        return error("CheckStakeKernelHash() : min age violation - nTimeBlockFrom=%d nStakeMinAge=%d nTimeTx=%d", nTimeBlockFrom, nStakeMinAge, nTimeTx);

    //grab difficulty
    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    //grab stake modifier
    uint64_t nStakeModifier = 0;
    int nStakeModifierHeight = 0;
    int64_t nStakeModifierTime = 0;
    if (!GetKernelStakeModifier(blockFrom.GetHash(), nBlockHeight, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake)) {
        LogPrintf("CheckStakeKernelHash(): failed to get kernel stake modifier\n");
        return false;
    }

    //create data stream once instead of repeating it in the loop
    CDataStream ss(SER_GETHASH, 0);
    ss << nStakeModifier;

    //get the stake weight - weight is equal to coin amount
    int64_t nTimeWeight = std::min<int64_t>(nTimeTx - nTimeBlockFrom, nStakeMaxAge - nStakeMinAge);

    //if wallet is simply checking to make sure a hash is valid
    if (fCheck) {
        hashProofOfStake = stakeHash(nTimeTx, ss, prevout.n, prevout.hash, nTimeBlockFrom);
        return stakeTargetHit(hashProofOfStake, nValueIn, bnTargetPerCoinDay, nTimeWeight);
    }

    bool fSuccess = false;
    unsigned int nTryTime = 0;
    int nHeightStart = chainActive.Height();
    for (unsigned int i = 0; i < nHashDrift; i++) //iterate the hashing
    {
        //new block came in, move on
        if (chainActive.Height() != nHeightStart)
            break;

        //hash this iteration
        nTryTime = nTimeTx - i;
        hashProofOfStake = stakeHash(nTryTime, ss, prevout.n, prevout.hash, nTimeBlockFrom);

        // if stake hash does not meet the target then continue to next iteration
        if (!stakeTargetHit(hashProofOfStake, nValueIn, bnTargetPerCoinDay, nTimeWeight))
            continue;

        fSuccess = true; // if we make it this far then we have successfully created a stake hash
        nTimeTx = nTryTime;

        if (fPrintProofOfStake) {
            LogPrintf("CheckStakeKernelHash() : using modifier %s at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
                      boost::lexical_cast<std::string>(nStakeModifier).c_str(), nStakeModifierHeight,
                      DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nStakeModifierTime).c_str(),
                      mapBlockIndex[blockFrom.GetHash()]->nHeight,
                    DateTimeStrFormat("%Y-%m-%d %H:%M:%S", blockFrom.GetBlockTime()).c_str());
            LogPrintf("CheckStakeKernelHash() : pass protocol=%s modifier=%s nTimeBlockFrom=%u prevoutHash=%s nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
                      "0.3",
                      boost::lexical_cast<std::string>(nStakeModifier).c_str(),
                      nTimeBlockFrom, prevout.hash.ToString().c_str(), nTimeBlockFrom, prevout.n, nTryTime,
                      hashProofOfStake.ToString().c_str());
        }
        break;
    }

    mapHashedBlocks.clear();
    mapHashedBlocks[chainActive.Tip()->nHeight] = GetTime(); //store a time stamp of when we last hashed on this block
    return fSuccess;
}

// BlackCoin kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + blockFrom.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coins one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   blockFrom.nTime: slightly scrambles computation
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
static bool CheckStakeKernelHashV3(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t blockFromTime, CAmount prevoutValue, const COutPoint& prevout, unsigned int nTimeBlock, uint256& hashProofOfStake, bool fPrintProofOfStake)
{
    if (nTimeBlock < blockFromTime)  // Transaction timestamp violation
        return error("%s : nTime violation", __func__);

    if (blockFromTime + nStakeMinAge > nTimeBlock) // Min age requirement
        return error("%s : min age violation - nTimeBlockFrom=%d nStakeMinAge=%d nTimeTx=%d", __func__, blockFromTime, nStakeMinAge, nTimeBlock);

    // Base target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    int64_t nValueIn = prevoutValue;
    arith_uint256 bnWeight = arith_uint256(nValueIn);
    bnTarget *= bnWeight;

    uint256 nStakeModifier = pindexPrev->hashStakeModifierV3;

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    ss << nStakeModifier;
    ss << blockFromTime << prevout.hash << prevout.n << nTimeBlock;
    hashProofOfStake = Hash(ss.begin(), ss.end());

    if (fPrintProofOfStake)
    {
        LogPrint(BCLog::KERNEL, "%s : check modifier=%s nTimeBlockFrom=%u nPrevout=%u nTimeBlock=%u hashProof=%s\n", __func__,
                 nStakeModifier.GetHex().c_str(),
                 blockFromTime, prevout.n, nTimeBlock,
                 hashProofOfStake.ToString());
    }

    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) > bnTarget)
        return false;

    LogPrint(BCLog::KERNEL, "%s : check modifier=%s nTimeBlockFrom=%u nPrevout=%u nTimeBlock=%u hashProof=%s\n", __func__,
             nStakeModifier.GetHex().c_str(),
             blockFromTime, prevout.n, nTimeBlock,
             hashProofOfStake.ToString());

    return true;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(const CBlock &block, uint256& hashProofOfStake)
{
    const CTransactionRef &tx = block.vtx[1];
    if (!tx->IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx->GetHash().ToString().c_str());

    if(tx->vin.size() > MAX_KERNEL_COMBINED_INPUTS) {
        return error("CheckProofOfStake() : invalid amount of stake inputs, current: %d, max: %d", tx->vin.size(), MAX_KERNEL_COMBINED_INPUTS);
    }

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx->vin[0];

    // First try finding the previous transaction in database
    uint256 hashBlock;
    CTransactionRef txPrev;
    if (!GetTransaction(txin.prevout.hash, txPrev, Params().GetConsensus(), hashBlock, true))
        return error("CheckProofOfStake() : INFO: read txPrev failed");

    const CScript &kernelScript = txPrev->vout[txin.prevout.n].scriptPubKey;
    bool hasMinStakeAmount = false;

    auto nValidInputs = std::count_if(std::begin(tx->vin), std::end(tx->vin),
                                      [&kernelScript, &hasMinStakeAmount](const CTxIn &txIn) {
        CTransactionRef txPrev;
        uint256 hashBlock;
        if(GetTransaction(txIn.prevout.hash, txPrev, Params().GetConsensus(), hashBlock, true)) {
            auto transactionOut = txPrev->vout[txIn.prevout.n];

            if (!hasMinStakeAmount && transactionOut.nValue >= MIN_STAKING_AMOUNT)
                hasMinStakeAmount = true;

            return transactionOut.scriptPubKey == kernelScript;
        }

        return false;
    });

    if (!hasMinStakeAmount && ShouldCheckForMinStakeAmount(chainActive.Tip()->nHeight + 1, Params().GetConsensus()))
        return error("CheckProofOfStake() : Amount of stake less than the required minimum of %d.", MIN_STAKING_AMOUNT);

    if(nValidInputs != tx->vin.size()) {
        return error("CheckProofOfStake() : Invalid inputs for stake total inputs: %d vs valid inputs %d", tx->vin.size(), nValidInputs);
    }

    //verify signature and script
    CTxOut prevTxOut = txPrev->vout[txin.prevout.n];
    if (!VerifyScript(txin.scriptSig, txPrev->vout[txin.prevout.n].scriptPubKey, &txin.scriptWitness, STANDARD_SCRIPT_VERIFY_FLAGS, TransactionSignatureChecker(tx.get(), 0, prevTxOut.nValue)))
        return error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx->GetHash().ToString().c_str());

    CBlockIndex* pindex = NULL;
    BlockMap::iterator it = mapBlockIndex.find(hashBlock);
    if (it != mapBlockIndex.end())
        pindex = it->second;
    else
        return error("CheckProofOfStake() : read block failed");

    // Read block header
    CBlock blockprev;
    if (!ReadBlockFromDisk(blockprev, pindex->GetBlockPos(), Params().GetConsensus()))
        return error("CheckProofOfStake(): INFO: failed to find block");


    auto fIt = mapBlockIndex.find(block.GetHash()); // we will use this value to determine how long we should scan for stake modifier.
    unsigned int nBlockHeight = fIt != std::end(mapBlockIndex) ? fIt->second->nHeight : chainActive.Tip()->nHeight + 1;

    unsigned int nInterval = 0;
    unsigned int nTime = block.nTime;
    if (!CheckStakeKernelHash(mapBlockIndex[block.hashPrevBlock], block.nBits, blockprev, *txPrev, txin.prevout, nBlockHeight, nTime, nInterval, true, hashProofOfStake, false))
        return error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s \n", tx->GetHash().ToString().c_str(), hashProofOfStake.ToString().c_str()); // may occur during initial download or if behind on block chain sync

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    // v0.3 protocol
    return (nTimeBlock == nTimeTx);
}

// Get stake modifier checksum
uint64_t GetStakeModifierChecksum(const CBlockIndex* pindex)
{
    assert(pindex->pprev || pindex->GetBlockHash() == Params().GetConsensus().hashGenesisBlock);
    // Hash previous checksum with flags, hashProofOfStake and nStakeModifier
    CDataStream ss(SER_GETHASH, 0);
    if (pindex->pprev)
        ss << pindex->pprev->nStakeModifierChecksum;
    ss << pindex->nFlags << pindex->hashProofOfStake << pindex->nStakeModifier;
    arith_uint256 hashChecksum = UintToArith256(Hash(ss.begin(), ss.end()));
    hashChecksum >>= (256 - 32);
    return hashChecksum.GetLow64();
}

// Check stake modifier hard checkpoints
static bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum)
{
    if (fTestNet) return true; // Testnet has no checkpoints
    if (mapStakeModifierCheckpoints.count(nHeight)) {
        return nStakeModifierChecksum == mapStakeModifierCheckpoints[nHeight];
    }
    return true;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifierV3(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return uint256();  // genesis block's modifier is 0

    CDataStream ss(SER_GETHASH, 0);
    ss << kernel << pindexPrev->nStakeModifier;
    return Hash(ss.begin(), ss.end());
}

bool ComputeAndSetStakeModifier(CBlockIndex *pindexNew, const Consensus::Params &consensus)
{
    if(IsWitnessEnabled(pindexNew->nHeight, Params().GetConsensus()))
    {
        auto kernel = pindexNew->IsProofOfWork() ? pindexNew->GetBlockHash() : pindexNew->prevoutStake.hash;
        pindexNew->hashStakeModifierV3 = ComputeStakeModifierV3(pindexNew->pprev, kernel);
    }
    else
    {
        // ppcoin: compute stake modifier
        uint64_t nStakeModifier = 0;
        bool fGeneratedStakeModifier = false;
        if (!ComputeNextStakeModifierV2(pindexNew->pprev, nStakeModifier, fGeneratedStakeModifier))
            LogPrintf("AcceptProofOfStakeBlock() : ComputeNextStakeModifier() failed \n");
        pindexNew->SetStakeModifier(nStakeModifier, fGeneratedStakeModifier);
        pindexNew->nStakeModifierChecksum = GetStakeModifierChecksum(pindexNew);
        if (!CheckStakeModifierCheckpoints(pindexNew->nHeight, pindexNew->nStakeModifierChecksum))
            LogPrintf("AcceptProofOfStakeBlock() : Rejected by stake modifier checkpoint height=%d, modifier=%s \n", pindexNew->nHeight, std::to_string(nStakeModifier));
    }

    return true;
}

bool CheckStakeKernelHash(CBlockIndex *pindexPrev, unsigned int nBits, const CBlock &blockFrom, const CTransaction &txPrev, const COutPoint prevout, unsigned int nBlockHeight, unsigned int &nTimeTx, unsigned int nHashDrift, bool fCheck, uint256 &hashProofOfStake, bool fPrintProofOfStake)
{
    return true;
    return IsWitnessEnabled(pindexPrev->nHeight + 1, Params().GetConsensus()) ?
                CheckStakeKernelHashV3(pindexPrev, nBits, blockFrom.nTime, txPrev.vout[prevout.n].nValue, prevout, nTimeTx, hashProofOfStake, fPrintProofOfStake) :
                CheckStakeKernelHashV2(nBlockHeight, nBits, blockFrom, txPrev, prevout, nTimeTx, nHashDrift, fCheck, hashProofOfStake, fPrintProofOfStake);
}
