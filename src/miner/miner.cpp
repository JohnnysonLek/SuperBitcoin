// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Super Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "miner.h"

#include "wallet/amount.h"
#include "chaincontrol/chain.h"
#include "config/chainparams.h"
#include "chaincontrol/coins.h"
#include "config/consensus.h"
#include "sbtccore/block/merkle.h"
#include "chaincontrol/validation.h"
#include "hash.h"
#include "block/validation.h"
#include "p2p/net.h"
#include "wallet/feerate.h"
#include "sbtccore/transaction/policy.h"
#include "pow.h"
#include "transaction/transaction.h"
#include "script/standard.h"
#include "timedata.h"
#include "mempool/txmempool.h"
#include "utils/util.h"
#include "utils/utilmoneystr.h"
#include "framework/validationinterface.h"
#include "interface/ichaincomponent.h"
#include "interface/imempoolcomponent.h"
#include "chaincontrol/utils.h"

#include <algorithm>
#include <queue>
#include <utility>

SET_CPP_SCOPED_LOG_CATEGORY(CID_MINER);

//////////////////////////////////////////////////////////////////////////////
//
// Super BitcoinMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest fee rate of a transaction combined with all
// its ancestors.

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockWeight = 0;

std::vector<unsigned char>
GenerateCoinbaseCommitment(CBlock &block, const CBlockIndex *pindexPrev, const Consensus::Params &consensusParams)
{
    std::vector<unsigned char> commitment;
    int commitpos = GetWitnessCommitmentIndex(block);
    std::vector<unsigned char> ret(32, 0x00);
    if (consensusParams.vDeployments[Consensus::DEPLOYMENT_SEGWIT].nTimeout != 0)
    {
        if (commitpos == -1)
        {
            uint256 witnessroot = BlockWitnessMerkleRoot(block, nullptr);
            CHash256().Write(witnessroot.begin(), 32).Write(ret.data(), 32).Finalize(witnessroot.begin());
            CTxOut out;
            out.nValue = 0;
            out.scriptPubKey.resize(38);
            out.scriptPubKey[0] = OP_RETURN;
            out.scriptPubKey[1] = 0x24;
            out.scriptPubKey[2] = 0xaa;
            out.scriptPubKey[3] = 0x21;
            out.scriptPubKey[4] = 0xa9;
            out.scriptPubKey[5] = 0xed;
            memcpy(&out.scriptPubKey[6], witnessroot.begin(), 32);
            commitment = std::vector<unsigned char>(out.scriptPubKey.begin(), out.scriptPubKey.end());
            CMutableTransaction tx(*block.vtx[0]);
            tx.vout.push_back(out);
            block.vtx[0] = MakeTransactionRef(std::move(tx));
        }
    }
    UpdateUncommittedBlockStructures(block, pindexPrev, consensusParams);
    return commitment;
}

int64_t UpdateTime(CBlockHeader *pblock, const Consensus::Params &consensusParams, const CBlockIndex *pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast() + 1, GetAdjustedTime());

    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks)
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);

    return nNewTime - nOldTime;
}

BlockAssembler::Options::Options()
{
    blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    nBlockMaxWeight = DEFAULT_BLOCK_MAX_WEIGHT;
}

BlockAssembler::BlockAssembler(const CChainParams &params, const Options &options) : chainparams(params)
{
    blockMinFeeRate = options.blockMinFeeRate;
    // Limit weight to between 4K and MAX_BLOCK_WEIGHT-4K for sanity:
    nBlockMaxWeight = std::max<size_t>(4000, std::min<size_t>(MAX_BLOCK_WEIGHT - 4000, options.nBlockMaxWeight));
}

static BlockAssembler::Options DefaultOptions(const CChainParams &params)
{
    // Block resource limits
    // If neither -blockmaxsize or -blockmaxweight is given, limit to DEFAULT_BLOCK_MAX_*
    // If only one is given, only restrict the specified resource.
    // If both are given, restrict both.
    BlockAssembler::Options options;
    options.nBlockMaxWeight = Args().GetArg<uint32_t>("-blockmaxweight", DEFAULT_BLOCK_MAX_WEIGHT);
    if (Args().IsArgSet("-blockmintxfee"))
    {
        CAmount n = 0;
        ParseMoney(Args().GetArg<std::string>("-blockmintxfee", ""), n);
        options.blockMinFeeRate = CFeeRate(n);
    } else
    {
        options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    }
    return options;
}

BlockAssembler::BlockAssembler(const CChainParams &params) : BlockAssembler(params, DefaultOptions(params))
{
}

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockWeight = 4000;
    nBlockSigOpsCost = 400;
    fIncludeWitness = false;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}

//sbtc-vm
void BlockAssembler::RebuildRefundTransaction(uint256 hashStateRoot, uint256 hashUTXORoot)
{

//    int refundtx = 0;
//    GET_CHAIN_INTERFACE(ifChainObj);

    CMutableTransaction contrTx(originalRewardTx);
//    contrTx.vout[refundtx].nValue = nFees + ifChainObj->GetBlockSubsidy(nHeight);
//    contrTx.vout[refundtx].nValue -= bceResult.refundSender;
    if (!(hashStateRoot.IsNull() || hashUTXORoot.IsNull()))
    {
        CScript scriptPubKey =
                CScript() << ParseHex(hashStateRoot.GetHex().c_str()) << ParseHex(hashUTXORoot.GetHex().c_str())
                          << OP_VM_STATE;

        contrTx.vout[0].scriptPubKey = scriptPubKey;
        contrTx.vout[0].nValue = 0;
    }

    //note, this will need changed for MPoS
    int i = contrTx.vout.size();
    contrTx.vout.resize(contrTx.vout.size() + bceResult.refundOutputs.size());
    for (CTxOut &vout : bceResult.refundOutputs)
    {
        contrTx.vout[i] = vout;
        i++;
    }
    pblock->vtx[1] = MakeTransactionRef(std::move(contrTx));
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript &scriptPubKeyIn, bool fMineWitnessTx)
{
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if (!pblocktemplate.get())
        return nullptr;
    pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOpsCost.push_back(-1); // updated at end

    GET_TXMEMPOOL_INTERFACE(ifTxMempoolObj);
    LOCK2(cs_main, ifTxMempoolObj->GetMemPool().cs);
    GET_CHAIN_INTERFACE(ifChainObj);
    CBlockIndex *pindexPrev = ifChainObj->GetActiveChain().Tip();
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = Args().GetArg<int32_t>("-blockversion", pblock->nVersion);

    pblock->nTime = GetAdjustedTime();
    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();

    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                      ? nMedianTimePast
                      : pblock->GetBlockTime();

    // Decide whether to include witness transactions
    // This is only needed in case the witness softfork activation is reverted
    // (which would require a very deep reorganization) or when
    // -promiscuousmempoolflags is used.
    // replace this with a call to main to assess validity of a mempool
    // transaction (which in most cases can be a no-op).
    fIncludeWitness = IsWitnessEnabled(pindexPrev, chainparams.GetConsensus()) && fMineWitnessTx;

    nLastBlockTx = nBlockTx;
    nLastBlockWeight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    CMutableTransaction coinbaseTxBak;

    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vout.resize(1);
    coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;
    coinbaseTx.vout[0].nValue = nFees + ifChainObj->GetBlockSubsidy(nHeight);
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    coinbaseTxBak = coinbaseTx; //sbtc-vm
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));

    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    //    addPackageTxs(nPackagesSelected, nDescendantsUpdated);

    //////////////////////////////////////////////////////// sbtc-vm
    GET_CONTRACT_INTERFACE(ifContractObj);
    minGasPrice = ifContractObj->GetMinGasPrice(nHeight);
    if (Args().IsArgSet("-staker-min-tx-gas-price"))
    {
        CAmount stakerMinGasPrice;
        string strMinxGasPrice = "";
        strMinxGasPrice = Args().GetArg("-staker-min-tx-gas-price", strMinxGasPrice);
        if (ParseMoney(strMinxGasPrice, stakerMinGasPrice))
        {
            minGasPrice = std::max(minGasPrice, (uint64_t)stakerMinGasPrice);
        }
    }
    hardBlockGasLimit = ifContractObj->GetBlockGasLimit(nHeight);
    softBlockGasLimit = Args().GetArg("-staker-soft-block-gas-limit", hardBlockGasLimit);
    softBlockGasLimit = std::min(softBlockGasLimit, hardBlockGasLimit);
    txGasLimit = Args().GetArg("-staker-max-tx-gas-limit", softBlockGasLimit);

    //    nBlockMaxSize = blockSizeDGP ? blockSizeDGP : nBlockMaxSize;

    uint256 oldHashStateRoot, oldHashUTXORoot;
    ifContractObj->GetState(oldHashStateRoot, oldHashUTXORoot);

    bool enablecontract = false;
    enablecontract = ifChainObj->IsSBTCForkContractEnabled(pindexPrev->nHeight);

    // Create second transaction.
    if(enablecontract)
    {
        CMutableTransaction coinbase2;
        coinbase2.vin.resize(2);
        coinbase2.vin[0].prevout.SetNull();
        coinbase2.vin[1].prevout.SetNull();
        coinbase2.vout.resize(1);

        if (oldHashStateRoot.IsNull())
        {
            oldHashStateRoot = DEFAULT_HASH_STATE_ROOT;
        }
        if (oldHashUTXORoot.IsNull())
        {
            oldHashUTXORoot = DEFAULT_HASH_UTXO_ROOT;
        }
        CScript scriptPubKey =
                CScript() << ParseHex(oldHashStateRoot.GetHex().c_str()) << ParseHex(oldHashUTXORoot.GetHex().c_str())
                          << OP_VM_STATE;
        coinbase2.vout[0].scriptPubKey = scriptPubKey;
        coinbase2.vout[0].nValue = 0;
        coinbase2.vin[0].scriptSig = CScript() << nHeight << OP_0;
        coinbase2.vin[1].scriptSig = CScript() << nHeight << OP_0;
        originalRewardTx = coinbase2;
        pblock->vtx.emplace_back();
        pblock->vtx[1] = MakeTransactionRef(std::move(coinbase2));
    }

    //    addPriorityTxs(minGasPrice);
    uint256 hashStateRoot, hashUTXORoot;
    addPackageTxs(nPackagesSelected, nDescendantsUpdated);
    ifContractObj->GetState(hashStateRoot, hashUTXORoot);
    if (pindexPrev->nHeight + 1 > Params().GetConsensus().SBTCContractForkHeight)
    {
        if (hashStateRoot.IsNull())
        {
            hashStateRoot = DEFAULT_HASH_STATE_ROOT;
        }
        if (hashUTXORoot.IsNull())
        {
            hashUTXORoot = DEFAULT_HASH_UTXO_ROOT;
        }
    }
    //    pblock->hashStateRoot = hashStateRoot;
    //    pblock->hashUTXORoot = hashUTXORoot;
    ifContractObj->UpdateState(oldHashStateRoot, oldHashUTXORoot);

    //this should already be populated by AddBlock in case of contracts, but if no contracts
    //then it won't get populated
    if(enablecontract) {
        RebuildRefundTransaction(hashStateRoot, hashUTXORoot);
    }

    coinbaseTxBak.vout[0].nValue = nFees + ifChainObj->GetBlockSubsidy(nHeight);
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTxBak));
    ////////////////////////////////////////////////////////

    int64_t nTime1 = GetTimeMicros();

    pblocktemplate->vchCoinbaseCommitment = GenerateCoinbaseCommitment(*pblock, pindexPrev, chainparams.GetConsensus());
    pblocktemplate->vTxFees[0] = -nFees;

    NLogFormat("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d", GetBlockWeight(*pblock), nBlockTx,
               nFees, nBlockSigOpsCost);

    // Fill in header
    pblock->hashPrevBlock = pindexPrev->GetBlockHash();
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
    pblock->nNonce = 0;
    pblocktemplate->vTxSigOpsCost[0] = WITNESS_SCALE_FACTOR * (*pblock->vtx[0]).GetLegacySigOpCount();

    CValidationState state;
    if (!ifChainObj->TestBlockValidity(state, chainparams, *pblock, pindexPrev, false, false))
    {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
    }
    int64_t nTime2 = GetTimeMicros();

    NLogFormat(
            "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)",
            0.001 * (nTime1 - nTimeStart), nPackagesSelected, nDescendantsUpdated, 0.001 * (nTime2 - nTime1),
            0.001 * (nTime2 - nTimeStart));

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries &testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end();)
    {
        // Only test txs not already in the block
        if (inBlock.count(*iit))
        {
            testSet.erase(iit++);
        } else
        {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOpsCost)
{
    // TODO: switch to weight-based accounting for packages instead of vsize-based accounting.
    if (nBlockWeight + WITNESS_SCALE_FACTOR * packageSize >= nBlockMaxWeight)
        return false;
    if (nBlockSigOpsCost + packageSigOpsCost >= MAX_BLOCK_SIGOPS_COST)
        return false;
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - premature witness (in case segwit transactions are added to mempool before
//   segwit activation)
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries &package)
{
    for (const CTxMemPool::txiter it : package)
    {
        //        GET_VERIFY_INTERFACE(ifVerifyObj);
        if (!it->GetTx().IsFinalTx(nHeight, nLockTimeCutoff))
            return false;
        if (!fIncludeWitness && it->GetTx().HasWitness())
            return false;
    }
    return true;
}

//sbtc-vm
bool BlockAssembler::AttemptToAddContractToBlock(CTxMemPool::txiter iter, uint64_t minGasPrice)
{
    uint256 oldHashStateRoot, oldHashUTXORoot;
    GET_CONTRACT_INTERFACE(ifContractObj);
    ifContractObj->GetState(oldHashStateRoot, oldHashUTXORoot);
    // operate on local vars first, then later apply to `this`
    uint64_t nBlockWeight = this->nBlockWeight;
    uint64_t nBlockSigOpsCost = this->nBlockSigOpsCost;

    ByteCodeExecResult testExecResult;
    if (!ifContractObj->RunContractTx(iter->GetTx(), NULL, pblock, minGasPrice, hardBlockGasLimit, softBlockGasLimit,
                                      txGasLimit, bceResult.usedGas, testExecResult))
    {
        ifContractObj->UpdateState(oldHashStateRoot, oldHashUTXORoot);
        return false;
    }

    NLogFormat("AttemptToAddContractToBlock3=====");
    if (bceResult.usedGas + testExecResult.usedGas > softBlockGasLimit)
    {
        //if this transaction could cause block gas limit to be exceeded, then don't add it
        ifContractObj->UpdateState(oldHashStateRoot, oldHashUTXORoot);
        return false;
    }
    NLogFormat("AttemptToAddContractToBlock4=====");
    //apply contractTx costs to local state
    nBlockWeight += iter->GetTxWeight();
    nBlockSigOpsCost += iter->GetSigOpCost();
    //apply value-transfer txs to local state
    for (CTransaction &t : testExecResult.valueTransfers)
    {
        nBlockWeight += GetTransactionWeight(t);
        nBlockSigOpsCost += t.GetLegacySigOpCount();
    }

    int proofTx = 1; // 0

    //calculate sigops from new refund/proof tx

    //first, subtract old proof tx
    nBlockSigOpsCost -= (*pblock->vtx[proofTx]).GetLegacySigOpCount();

    // manually rebuild refundtx
    CMutableTransaction contrTx(*pblock->vtx[proofTx]);
    //note, this will need changed for MPoS
    int i = contrTx.vout.size();
    contrTx.vout.resize(contrTx.vout.size() + testExecResult.refundOutputs.size());
    for (CTxOut &vout : testExecResult.refundOutputs)
    {
        contrTx.vout[i] = vout;
        i++;
    }
    CTransaction txNewConst(contrTx);
    nBlockSigOpsCost += txNewConst.GetLegacySigOpCount();
    //all contract costs now applied to local state

    //Check if block will be too big or too expensive with this contract execution
    if (nBlockSigOpsCost * WITNESS_SCALE_FACTOR > (uint64_t)MAX_BLOCK_SIGOPS_COST ||
        nBlockWeight > MAX_BLOCK_WEIGHT)
    {//sbtc-vm
        //contract will not be added to block, so revert state to before we tried
        ifContractObj->UpdateState(oldHashStateRoot, oldHashUTXORoot);
        return false;
    }

    //block is not too big, so apply the contract execution and it's results to the actual block

    //apply local bytecode to global bytecode state
    bceResult.usedGas += testExecResult.usedGas;
    bceResult.refundSender += testExecResult.refundSender;
    bceResult.refundOutputs.insert(bceResult.refundOutputs.end(), testExecResult.refundOutputs.begin(),
                                   testExecResult.refundOutputs.end());
    bceResult.valueTransfers = std::move(testExecResult.valueTransfers);

    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    this->nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    this->nBlockSigOpsCost += iter->GetSigOpCost();

    CAmount gasRefunds = 0;
    for (CTxOut refundVout : testExecResult.refundOutputs)
    {
        gasRefunds += refundVout.nValue;  //one contract tx, need to refund gas
    }

    CAmount tmpFee = (iter->GetFee() - gasRefunds);
    nFees += tmpFee;   //  xiaofei

    inBlock.insert(iter);
    for (CTransaction &t : bceResult.valueTransfers)
    {
        pblock->vtx.emplace_back(MakeTransactionRef(std::move(t)));
        this->nBlockWeight += GetTransactionWeight(t);
        this->nBlockSigOpsCost += t.GetLegacySigOpCount();
        ++nBlockTx;
    }
    //calculate sigops from new refund/proof tx
    this->nBlockSigOpsCost -= (*pblock->vtx[proofTx]).GetLegacySigOpCount();
    oldHashStateRoot.SetNull();// not update hashroot this moment
    oldHashUTXORoot.SetNull();
    RebuildRefundTransaction(oldHashStateRoot, oldHashUTXORoot);
    this->nBlockSigOpsCost += (*pblock->vtx[proofTx]).GetLegacySigOpCount();

    bceResult.valueTransfers.clear();

    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(iter->GetSigOpCost());
    nBlockWeight += iter->GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += iter->GetSigOpCost();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = Args().GetArg<bool>("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority)
    {
        NLogFormat("fee %s txid %s",
                   CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                   iter->GetTx().GetHash().ToString());
    }
}

int BlockAssembler::UpdatePackagesForAdded(const CTxMemPool::setEntries &alreadyAdded,
                                           indexed_modified_transaction_set &mapModifiedTx)
{
    GET_TXMEMPOOL_INTERFACE(ifTxMempoolObj);
    CTxMemPool &mempool = ifTxMempoolObj->GetMemPool();

    int nDescendantsUpdated = 0;
    for (const CTxMemPool::txiter it : alreadyAdded)
    {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants)
        {
            if (alreadyAdded.count(desc))
                continue;
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end())
            {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCostWithAncestors -= it->GetSigOpCost();
                mapModifiedTx.insert(modEntry);
            } else
            {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }
    return nDescendantsUpdated;
}

// Skip entries in mapTx that are already in a block or are present
// in mapModifiedTx (which implies that the mapTx ancestor state is
// stale due to ancestor inclusion in the block)
// Also skip transactions that we've already failed to add. This can happen if
// we consider a transaction in mapModifiedTx and it fails: we can then
// potentially consider it again while walking mapTx.  It's currently
// guaranteed to fail again, but as a belt-and-suspenders check we put it in
// failedTx and avoid re-evaluation, since the re-evaluation would be using
// cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx,
                                    CTxMemPool::setEntries &failedTx)
{
    GET_TXMEMPOOL_INTERFACE(ifTxMempoolObj);
    assert (it != ifTxMempoolObj->GetMemPool().mapTx.end());
    return mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it);
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries &package, CTxMemPool::txiter entry,
                                  std::vector<CTxMemPool::txiter> &sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs(int &nPackagesSelected, int &nDescendantsUpdated)
{
    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    GET_TXMEMPOOL_INTERFACE(ifTxMempoolObj);
    CTxMemPool &mempool = ifTxMempoolObj->GetMemPool();
    CTxMemPool::indexed_transaction_set::index<ancestor_score_or_gas_price>::type::iterator mi = mempool.mapTx.get<ancestor_score_or_gas_price>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != mempool.mapTx.get<ancestor_score_or_gas_price>().end() || !mapModifiedTx.empty())
    {
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != mempool.mapTx.get<ancestor_score_or_gas_price>().end() &&
            SkipMapTxEntry(mempool.mapTx.project<0>(mi), mapModifiedTx, failedTx))
        {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score_or_gas_price>().begin();
        if (mi == mempool.mapTx.get<ancestor_score_or_gas_price>().end())
        {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else
        {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score_or_gas_price>().end() &&
                CompareModifiedEntry()(*modit, CTxMemPoolModifiedEntry(iter)))
            {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else
            {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOpsCost = iter->GetSigOpCostWithAncestors();
        if (fUsingModified)
        {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOpsCost = modit->nSigOpCostWithAncestors;
        }

        if (packageFees < blockMinFeeRate.GetFee(packageSize))
        {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOpsCost))
        {
            if (fUsingModified)
            {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score_or_gas_price>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight >
                                                                 nBlockMaxWeight - 4000)
            {
                // Give up if we're close to full and haven't succeeded in a while
                break;
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final
        if (!TestPackageTransactions(ancestors))
        {
            if (fUsingModified)
            {
                mapModifiedTx.get<ancestor_score_or_gas_price>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, iter, sortedEntries);

        bool wasAdded = true;//sbtc-vm
        for (size_t i = 0; i < sortedEntries.size(); ++i)
        {
            if (!wasAdded)   //sbtc-vm
            {
                //if out of time, or earlier ancestor failed, then skip the rest of the transactions
                mapModifiedTx.erase(sortedEntries[i]);
                wasAdded = false;
                continue;
            }
            //sbtc-vm
            const CTransaction &tx = sortedEntries[i]->GetTx();
            if (tx.HasCreateOrCall())
            {
                ILogFormat("addPackageTxs run contracttx=====");
                wasAdded = AttemptToAddContractToBlock(sortedEntries[i], minGasPrice);
                if (!wasAdded)
                {
                    ILogFormat("addPackageTxs wasAdded=false");
                    if (fUsingModified)
                    {
                        //this only needs to be done once to mark the whole package (everything in sortedEntries) as failed
                        mapModifiedTx.get<ancestor_score_or_gas_price>().erase(modit);
                        failedTx.insert(iter);
                    }
                }
            } else
            {
                AddToBlock(sortedEntries[i]);
            }
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        if (!wasAdded)
        { //sbtc-vm
            //skip UpdatePackages if a transaction failed to be added (match TestPackage logic)
            continue;
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
}

void IncrementExtraNonce(CBlock *pblock, const CBlockIndex *pindexPrev, unsigned int &nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight + 1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}
