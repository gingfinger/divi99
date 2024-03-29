// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2016-2017 The PIVX Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "spork.h"
#include "base58.h"
#include "key.h"
#include <validation.h>
#include <messagesigner.h>
#include <net.h>
#include <protocol.h>
#include <sync.h>
#include <sporkdb.h>
#include <netmessagemaker.h>
#include <consensus/validation.h>

#include <numeric>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/lexical_cast.hpp>

std::map<uint256, CSporkMessage> mapSporks;
CSporkManager sporkManager;

static std::map<int, std::string> mapSporkDefaults = {
    {SPORK_2_SWIFTTX_ENABLED,                "0"},             // ON
    {SPORK_3_SWIFTTX_BLOCK_FILTERING,        "0"},             // ON
    {SPORK_5_INSTANTSEND_MAX_VALUE,          "1000"},          // 1000 PIVX
    {SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT, "1537971708"},    // ON
    {SPORK_9_SUPERBLOCKS_ENABLED,            "4070908800"},    // OFF
    {SPORK_10_MASTERNODE_PAY_UPDATED_NODES,  "4070908800"},    // OFF
    {SPORK_12_RECONSIDER_BLOCKS,             "0"},             // 0 BLOCKS
    {SPORK_14_TX_FEE,                        "10000;300;10000000;1000"}
};

static bool IsMultiValueSpork(int nSporkID)
{
    if(SPORK_13_BLOCK_PAYMENTS == nSporkID ||
            SPORK_15_BLOCK_VALUE == nSporkID ||
            SPORK_16_LOTTERY_TICKET_MIN_VALUE == nSporkID) {
        return true;
    }

    return false;
}

bool CSporkManager::AddActiveSpork(const CSporkMessage &spork)
{
    auto &sporks = mapSporksActive[spork.nSporkID];
    if(IsMultiValueSpork(spork.nSporkID)) {

        auto sporkHash = spork.GetHash();
        auto it = std::find_if(std::begin(sporks), std::end(sporks), [sporkHash](const CSporkMessage &spork) {
            return sporkHash == spork.GetHash();
        });

        if(it != std::end(sporks)) {
            return false;
        }

        sporks.push_back(spork);
        std::sort(std::begin(sporks), std::end(sporks), [](const CSporkMessage &lhs, const CSporkMessage &rhs) {
            return lhs.nTimeSigned < rhs.nTimeSigned;
        });
    }
    else {
        sporks = { spork };
    }

    return true;
}

bool CSporkManager::IsNewerSpork(const CSporkMessage &spork) const
{

    if(mapSporksActive.count(spork.nSporkID)) {
        const auto &sporks = mapSporksActive.at(spork.nSporkID);
        // at this place items have to be sorted
        if (sporks.back().nTimeSigned >= spork.nTimeSigned)
            return false;
    }

    return true;
}

CSporkManager::CSporkManager()
{
}

// DIVI: on startup load spork values from previous session if they exist in the sporkDB
void CSporkManager::LoadSporksFromDB()
{
    for (int i = SPORK_START; i <= SPORK_END; ++i) {
        // Since not all spork IDs are in use, we have to exclude undefined IDs
        std::string strSpork = sporkManager.GetSporkNameByID(i);
        if (strSpork == "Unknown") continue;

        // attempt to read spork from sporkDB
        CSporkMessage spork;
        if (!pSporkDB->ReadSpork(i, spork)) {
            LogPrintf("%s : no previous value for %s found in database\n", __func__, strSpork);
            continue;
        }

        // add spork to memory
        mapSporks[spork.GetHash()] = spork;
        AddActiveSpork(spork);

        if(spork.nSporkID == SPORK_14_TX_FEE)
        {
            ExecuteSpork(spork.nSporkID);
        }

        LogPrintf("%s : loaded spork %s with value %d\n", __func__, sporkManager.GetSporkNameByID(spork.nSporkID), spork.strValue);
    }
}

void CSporkManager::ProcessSpork(CNode* pfrom, CValidationState &state, const std::string& strCommand, CDataStream& vRecv, CConnman *connman)
{
    const CNetMsgMaker msgMaker(pfrom->GetSendVersion());
    if (strCommand == NetMsgType::SPORK) {

        CSporkMessage spork;
        vRecv >> spork;

        uint256 hash = spork.GetHash();


        std::string strLogMsg;
        {
            LOCK(cs_main);
            if(!chainActive.Tip()) return;
            strLogMsg = strprintf("SPORK -- hash: %s id: %d value: %10d bestHeight: %d peer=%d", hash.ToString(), spork.nSporkID, spork.strValue, chainActive.Height(), pfrom->GetId());
        }

        if(IsNewerSpork(spork)) {
            LogPrintf("%s new\n", strLogMsg);
        } else if(!IsMultiValueSpork(spork.nSporkID)) {
            LogPrint(BCLog::SPORK, "%s seen\n", strLogMsg);
            pfrom->nSporksSynced++;
            return;
        }

        if(!spork.CheckSignature(sporkPubKey)) {
            LOCK(cs_main);
            LogPrintf("CSporkManager::ProcessSpork -- ERROR: invalid signature\n");
            state.DoS(100, false, REJECT_INVALID, "spork-invalid-sig");
            return;
        }

        pfrom->nSporksSynced++;

        mapSporks[hash] = spork;

        if(AddActiveSpork(spork)) {
            pSporkDB->WriteSpork(spork.nSporkID, spork);

            //does a task if needed
            ExecuteSpork(spork.nSporkID);
        }

        spork.Relay(connman);

    } else if (strCommand == NetMsgType::GETSPORKS) {

        for(auto &&entry : mapSporksActive) {
            for(auto &&spork : entry.second) {
                connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::SPORK, spork));

            }
        }

    } else if(strCommand == NetMsgType::SPORKCOUNT) {
        int nSporkCount = 0;
        vRecv >> nSporkCount;
        pfrom->SetSporkCount(nSporkCount);
        connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::GETSPORKS));
    }

}

void CSporkManager::ExecuteSpork(int nSporkID)
{
    auto strValue = mapSporksActive.at(nSporkID).front().strValue;
    //correct fork via spork technology
    switch (nSporkID) {
    case SPORK_12_RECONSIDER_BLOCKS: {
        int64_t nValue = 0;
        try
        {
            nValue = boost::lexical_cast<int64_t>(strValue);
        }
        catch(boost::bad_lexical_cast &)
        {
        }

        if(nValue <= 0) {
            return;
        }

        // allow to reprocess 24h of blocks max, which should be enough to resolve any issues
        int64_t nMaxBlocks = 576;
        // this potentially can be a heavy operation, so only allow this to be executed once per 10 minutes
        int64_t nTimeout = 10 * 60;

        static int64_t nTimeExecuted = 0; // i.e. it was never executed before

        if(GetTime() - nTimeExecuted < nTimeout) {
            LogPrint(BCLog::SPORK, "CSporkManager::ExecuteSpork -- ERROR: Trying to reconsider blocks, too soon - %d/%d\n", GetTime() - nTimeExecuted, nTimeout);
            return;
        }

        if(nValue > nMaxBlocks) {
            LogPrintf("CSporkManager::ExecuteSpork -- ERROR: Trying to reconsider too many blocks %d/%d\n", strValue, nMaxBlocks);
            return;
        }


        LogPrintf("CSporkManager::ExecuteSpork -- Reconsider Last %d Blocks\n", nValue);

        ReprocessBlocks(nValue);
        nTimeExecuted = GetTime();
        break;
    }
    case SPORK_14_TX_FEE: {
        auto txFeeSpork = TxFeeSporkValue::FromString(strValue);

        if (!txFeeSpork.IsValid())
        {
            LogPrintf("CSporkManager::ExecuteSpork -- ERROR: Invalid transaction fee spork value\n");
            return;
        }

        minRelayTxFee = CFeeRate(txFeeSpork.nMinFeePerKb);
        maxTxFee = txFeeSpork.nMaxFee;
        nTxFeeSizeMultiplier = txFeeSpork.nTxSizeMultiplier;
        nTxFeeValueMultiplier = txFeeSpork.nTxValueMultiplier;
        break;
    }
    default:
        break;
    }
}

void CSporkManager::ExecuteMultiValueSpork(int nSporkID)
{
}

static int GetActivationHeightHelper(int nSporkID, const std::string strValue)
{
    int nActivationTime = 0;

    if(nSporkID == SPORK_13_BLOCK_PAYMENTS) {
        nActivationTime = BlockPaymentSporkValue::FromString(strValue).nActivationBlockHeight;
    }
    else if(nSporkID == SPORK_15_BLOCK_VALUE) {
        nActivationTime = BlockSubsiditySporkValue::FromString(strValue).nActivationBlockHeight;
    }
    else if(nSporkID == SPORK_16_LOTTERY_TICKET_MIN_VALUE) {
        nActivationTime = LotteryTicketMinValueSporkValue::FromString(strValue).nActivationBlockHeight;
    }
    else {
        return nActivationTime;
    }

    return nActivationTime;
}

bool CSporkManager::UpdateSpork(int nSporkID, std::string strValue, CConnman *connman)
{
    CSporkMessage spork = CSporkMessage(nSporkID, strValue, GetAdjustedTime());

    if(!IsNewerSpork(spork))
        return false;

    if(IsMultiValueSpork(nSporkID)) {
        if(GetActivationHeightHelper(nSporkID, strValue) < chainActive.Height() + 10) {
            return false;
        }
    }

    if(spork.Sign(sporkPrivKey, sporkPubKey)) {
        spork.Relay(connman);
        mapSporks[spork.GetHash()] = spork;
        AddActiveSpork(spork);
        return true;
    }

    return false;
}

int CSporkManager::GetActiveSporkCount() const
{
    return std::accumulate(std::begin(mapSporksActive), std::end(mapSporksActive),  0, [](int accum, const std::pair<int, std::vector<CSporkMessage>> &item) {
        return accum + item.second.size();
    });
}

// grab the spork, otherwise say it's off
bool CSporkManager::IsSporkActive(int nSporkID)
{
    // Multi value sporks cannot be active, but they can store value
    bool fMultiValue = IsMultiValueSpork(nSporkID);

    if(fMultiValue) {
        return mapSporksActive.count(nSporkID);
    }

    std::string r;

    if(mapSporksActive.count(nSporkID)){
        r = mapSporksActive[nSporkID].front().strValue; // always one value
    } else if (mapSporkDefaults.count(nSporkID)) {
        r = mapSporkDefaults[nSporkID];
    } else {
        LogPrint(BCLog::SPORK, "CSporkManager::IsSporkActive -- Unknown Spork ID %d\n", nSporkID);
        r = "4070908800"; // 2099-1-1 i.e. off by default
    }

    return boost::lexical_cast<int64_t>(r) < GetAdjustedTime();
}

std::vector<CSporkMessage> CSporkManager::GetMultiValueSpork(int nSporkID) const
{
    if (IsMultiValueSpork(nSporkID) && mapSporksActive.count(nSporkID)) {
        return mapSporksActive.at(nSporkID);
    }

    return std::vector<CSporkMessage>();
}

// grab the value of the spork on the network, or the default
std::string CSporkManager::GetSporkValue(int nSporkID) const
{
    if(IsMultiValueSpork(nSporkID)) {
        auto values = GetMultiValueSpork(nSporkID);
        return values.empty() ? std::string() : values.back().strValue;
    }

    if (mapSporksActive.count(nSporkID)) {
        return mapSporksActive.at(nSporkID).front().strValue;
    }

    if (mapSporkDefaults.count(nSporkID)) {
        return mapSporkDefaults[nSporkID];
    }

    LogPrint(BCLog::SPORK, "CSporkManager::GetSporkValue -- Unknown Spork ID %d\n", nSporkID);
    return std::string();
}

int CSporkManager::GetSporkIDByName(const std::string& strName)
{
    if (strName == "SPORK_2_SWIFTTX_ENABLED")                   return SPORK_2_SWIFTTX_ENABLED;
    if (strName == "SPORK_3_SWIFTTX_BLOCK_FILTERING")           return SPORK_3_SWIFTTX_BLOCK_FILTERING;
    if (strName == "SPORK_5_INSTANTSEND_MAX_VALUE")             return SPORK_5_INSTANTSEND_MAX_VALUE;
    if (strName == "SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT")    return SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT;
    if (strName == "SPORK_9_SUPERBLOCKS_ENABLED")               return SPORK_9_SUPERBLOCKS_ENABLED;
    if (strName == "SPORK_10_MASTERNODE_PAY_UPDATED_NODES")     return SPORK_10_MASTERNODE_PAY_UPDATED_NODES;
    if (strName == "SPORK_12_RECONSIDER_BLOCKS")                return SPORK_12_RECONSIDER_BLOCKS;
    if (strName == "SPORK_13_BLOCK_PAYMENTS")                   return SPORK_13_BLOCK_PAYMENTS;
    if (strName == "SPORK_14_TX_FEE")                           return SPORK_14_TX_FEE;
    if (strName == "SPORK_15_BLOCK_VALUE")                      return SPORK_15_BLOCK_VALUE;
    if (strName == "SPORK_16_LOTTERY_TICKET_MIN_VALUE")         return SPORK_16_LOTTERY_TICKET_MIN_VALUE;

    LogPrint(BCLog::SPORK, "CSporkManager::GetSporkIDByName -- Unknown Spork name '%s'\n", strName);
    return -1;
}

std::string CSporkManager::GetSporkNameByID(int nSporkID)
{
    switch (nSporkID) {
    case SPORK_2_SWIFTTX_ENABLED:                   return "SPORK_2_SWIFTTX_ENABLED";
    case SPORK_3_SWIFTTX_BLOCK_FILTERING:           return "SPORK_3_SWIFTTX_BLOCK_FILTERING";
    case SPORK_5_INSTANTSEND_MAX_VALUE:             return "SPORK_5_INSTANTSEND_MAX_VALUE";
    case SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT:    return "SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT";
    case SPORK_9_SUPERBLOCKS_ENABLED:               return "SPORK_9_SUPERBLOCKS_ENABLED";
    case SPORK_10_MASTERNODE_PAY_UPDATED_NODES:     return "SPORK_10_MASTERNODE_PAY_UPDATED_NODES";
    case SPORK_12_RECONSIDER_BLOCKS:                return "SPORK_12_RECONSIDER_BLOCKS";
    case SPORK_13_BLOCK_PAYMENTS:                   return "SPORK_13_BLOCK_PAYMENTS";
    case SPORK_14_TX_FEE:                           return "SPORK_14_TX_FEE";
    case SPORK_15_BLOCK_VALUE:                      return "SPORK_15_BLOCK_VALUE";
    case SPORK_16_LOTTERY_TICKET_MIN_VALUE:         return "SPORK_16_LOTTERY_TICKET_MIN_VALUE";
    default:
        LogPrint(BCLog::SPORK, "CSporkManager::GetSporkNameByID -- Unknown Spork ID %d\n", nSporkID);
        return "Unknown";
    }
}

bool CSporkManager::SetSporkAddress(const std::string& strPubKey) {
    CPubKey pubkeynew(ParseHex(strPubKey));
    if (!pubkeynew.IsValid()) {
        LogPrintf("CSporkManager::SetSporkAddress -- Failed to parse spork address\n");
        return false;
    }

    sporkPubKey = pubkeynew;

    return true;
}

bool CSporkManager::SetPrivKey(const std::string& strPrivKey)
{
    CKey key;
    CPubKey pubKey;
    if(!CMessageSigner::GetKeysFromSecret(strPrivKey, key, pubKey)) {
        LogPrintf("CSporkManager::SetPrivKey -- Failed to parse private key\n");
        return false;
    }

    if (pubKey.GetID() != sporkPubKey.GetID()) {
        LogPrintf("CSporkManager::SetPrivKey -- New private key does not belong to spork address\n");
        return false;
    }

    CSporkMessage spork;
    if (spork.Sign(key, sporkPubKey)) {
        // Test signing successful, proceed
        LogPrintf("CSporkManager::SetPrivKey -- Successfully initialized as spork signer\n");

        sporkPrivKey = key;
        return true;
    } else {
        LogPrintf("CSporkManager::SetPrivKey -- Test signing failed\n");
        return false;
    }
}

uint256 CSporkMessage::GetHash() const
{
    return SerializeHash(*this);
}

uint256 CSporkMessage::GetSignatureHash() const
{
    return GetHash();
}

bool CSporkMessage::Sign(const CKey& key, const CPubKey &sporkPubKey)
{
    if (!key.IsValid()) {
        LogPrintf("CSporkMessage::Sign -- signing key is not valid\n");
        return false;
    }

    std::string strError = "";

    std::string strMessage = boost::lexical_cast<std::string>(nSporkID) + strValue + boost::lexical_cast<std::string>(nTimeSigned);

    if(!CMessageSigner::SignMessage(strMessage, vchSig, key, CPubKey::InputScriptType::SPENDP2PKH)) {
        LogPrintf("CSporkMessage::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!CMessageSigner::VerifyMessage(sporkPubKey.GetID(), vchSig, strMessage, strError)) {
        LogPrintf("CSporkMessage::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CSporkMessage::CheckSignature(const CPubKey& pubKey) const
{
    std::string strError = "";


    std::string strMessage = boost::lexical_cast<std::string>(nSporkID) + boost::lexical_cast<std::string>(strValue) + boost::lexical_cast<std::string>(nTimeSigned);

    if (!CMessageSigner::VerifyMessage(pubKey.GetID(), vchSig, strMessage, strError)){
        LogPrintf("CSporkMessage::CheckSignature -- VerifyHash() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

void CSporkMessage::Relay(CConnman *connman)
{
    CInv inv(MSG_SPORK, GetHash());
    connman->RelayInv(inv);
}

template <class T>
static std::string BuildDataPayload(T values)
{
    std::vector<std::string> strValues;

    std::transform(std::begin(values), std::end(values), std::back_inserter(strValues), [](int value) {
        return boost::lexical_cast<std::string>(value);
    });

    return boost::algorithm::join(strValues, ";");
}

static std::vector<int> ParseDataPayload(std::string strDataPayload)
{
    std::vector<std::string> vecTokens;
    boost::algorithm::split(vecTokens, strDataPayload, boost::is_any_of(";"));
    std::vector<int> vecParsedValues;

    try
    {
        for(auto &&token : vecTokens) {
            vecParsedValues.emplace_back(boost::lexical_cast<int>(token));
        }
    }
    catch(boost::bad_lexical_cast &)
    {

    }

    return vecParsedValues;
}

BlockPaymentSporkValue::BlockPaymentSporkValue() :
    SporkMultiValue(),
    nStakeReward(0),
    nMasternodeReward(0),
    nTreasuryReward(0),
    nProposalsReward(0),
    nCharityReward(0)
{
}

BlockPaymentSporkValue::BlockPaymentSporkValue(int nStakeRewardIn, int nMasternodeRewardIn, int nTreasuryRewardIn,
                                               int nProposalsRewardIn, int nCharityRewardIn, int nActivationBlockHeightIn) :
    SporkMultiValue(nActivationBlockHeightIn),
    nStakeReward(nStakeRewardIn),
    nMasternodeReward(nMasternodeRewardIn),
    nTreasuryReward(nTreasuryRewardIn),
    nProposalsReward(nProposalsRewardIn),
    nCharityReward(nCharityRewardIn)
{

}

BlockPaymentSporkValue BlockPaymentSporkValue::FromString(std::string strData)
{
    std::vector<int> vecParsedValues = ParseDataPayload(strData);

    if(vecParsedValues.size() != 6) {
        return BlockPaymentSporkValue();
    }

    return BlockPaymentSporkValue(vecParsedValues.at(0), vecParsedValues.at(1), vecParsedValues.at(2),
                                  vecParsedValues.at(3), vecParsedValues.at(4), vecParsedValues.at(5));
}

bool BlockPaymentSporkValue::IsValid() const
{
    auto values = {
        nStakeReward,
        nMasternodeReward,
        nTreasuryReward,
        nProposalsReward,
        nCharityReward
    };

    return SporkMultiValue::IsValid() &&
            std::accumulate(std::begin(values), std::end(values), 0) == 100;
}

std::string BlockPaymentSporkValue::ToString() const
{
    auto values = {
        nStakeReward,
        nMasternodeReward,
        nTreasuryReward,
        nProposalsReward,
        nCharityReward,
        nActivationBlockHeight
    };

    return BuildDataPayload(values);
}

BlockSubsiditySporkValue::BlockSubsiditySporkValue() :
    SporkMultiValue(),
    nBlockSubsidity(-1)

{

}

BlockSubsiditySporkValue::BlockSubsiditySporkValue(int nBlockSubsidityIn, int nActivationBlockHeightIn) :
    SporkMultiValue(nActivationBlockHeightIn),
    nBlockSubsidity(nBlockSubsidityIn)
{

}

BlockSubsiditySporkValue BlockSubsiditySporkValue::FromString(std::string strData)
{
    std::vector<int> vecParsedValues = ParseDataPayload(strData);

    if(vecParsedValues.size() != 2) {
        return BlockSubsiditySporkValue();
    }

    return BlockSubsiditySporkValue(vecParsedValues.at(0), vecParsedValues.at(1));
}

bool BlockSubsiditySporkValue::IsValid() const
{
    return SporkMultiValue::IsValid() && nBlockSubsidity >= 0;
}

std::string BlockSubsiditySporkValue::ToString() const
{
    return BuildDataPayload(std::vector<int> { nBlockSubsidity, nActivationBlockHeight });
}

SporkMultiValue::SporkMultiValue(int nActivationBlockHeightIn) :
    nActivationBlockHeight(nActivationBlockHeightIn)
{

}

SporkMultiValue::~SporkMultiValue()
{

}

bool SporkMultiValue::IsValid() const
{
    return nActivationBlockHeight > 0;
}

LotteryTicketMinValueSporkValue::LotteryTicketMinValueSporkValue() :
    SporkMultiValue(),
    nEntryTicketValue(0)
{

}

LotteryTicketMinValueSporkValue::LotteryTicketMinValueSporkValue(int nEntryTicketValueIn, int nActivationBlockHeight) :
    SporkMultiValue(nActivationBlockHeight),
    nEntryTicketValue(nEntryTicketValueIn)
{

}

LotteryTicketMinValueSporkValue LotteryTicketMinValueSporkValue::FromString(std::string strData)
{
    std::vector<int> vecParsedValues = ParseDataPayload(strData);

    if(vecParsedValues.size() != 2) {
        return LotteryTicketMinValueSporkValue();
    }

    return LotteryTicketMinValueSporkValue(vecParsedValues.at(0), vecParsedValues.at(1));
}

bool LotteryTicketMinValueSporkValue::IsValid() const
{
    return SporkMultiValue::IsValid() && nEntryTicketValue > 0;
}

std::string LotteryTicketMinValueSporkValue::ToString() const
{
    return BuildDataPayload(std::vector<int> { nEntryTicketValue, nActivationBlockHeight });
}

TxFeeSporkValue::TxFeeSporkValue() :
    nTxValueMultiplier(-1),
    nTxSizeMultiplier(-1),
    nMaxFee(-1),
    nMinFeePerKb(-1)
{

}

TxFeeSporkValue::TxFeeSporkValue(int nTxValueMultiplierIn, int nTxSizeMultiplierIn, int nMaxFeeIn,
                                 int nMinFeePerKbIn) :
    nTxValueMultiplier(nTxValueMultiplierIn),
    nTxSizeMultiplier(nTxSizeMultiplierIn),
    nMaxFee(nMaxFeeIn),
    nMinFeePerKb(nMinFeePerKbIn)
{
}

TxFeeSporkValue TxFeeSporkValue::FromString(std::string strData)
{
    auto vecParsedValues = ParseDataPayload(strData);

    if(vecParsedValues.size() != 4)
        return TxFeeSporkValue{};

    return TxFeeSporkValue{vecParsedValues.at(0), vecParsedValues.at(1), vecParsedValues.at(2),
                           vecParsedValues.at(3)};
}

bool TxFeeSporkValue::IsValid() const
{
    return nTxValueMultiplier > 0 && nTxSizeMultiplier > 0 && nMaxFee > 0 && nMinFeePerKb > 0;
}

std::string TxFeeSporkValue::ToString() const
{
    auto values = {
        nTxValueMultiplier,
        nTxSizeMultiplier,
        nMaxFee,
        nMinFeePerKb
    };

    return BuildDataPayload(values);
}
