#pragma once

#include <memory>
#include "base/base.hpp"
#include "componentid.h"
#include "exchangeformat.h"
#include "utils/uint256.h"
#include "framework/component.hpp"
#include "chain.h"
#include "config/chainparams.h"
#include "coins.h"
#include "blockindexmanager.h"

class CBlock;

class CDataStream;

enum FlushStateMode
{
    FLUSH_STATE_NONE,
    FLUSH_STATE_IF_NEEDED,
    FLUSH_STATE_PERIODIC,
    FLUSH_STATE_ALWAYS
};

class IChainComponent : public appbase::TComponent<IChainComponent>
{
public:
    virtual ~IChainComponent()
    {
    }

    enum
    {
        ID = CID_BLOCK_CHAIN
    };

    virtual int GetID() const override
    {
        return ID;
    }

    virtual bool ComponentInitialize() = 0;

    virtual bool ComponentStartup() = 0;

    virtual bool ComponentShutdown() = 0;

    virtual const char *whoru() const = 0;

    virtual bool IsImporting() const = 0;

    virtual bool IsReindexing() const = 0;

    virtual bool IsTxIndex() const = 0;

    virtual bool IsLogEvents() = 0;

    virtual bool IsInitialBlockDownload() = 0;

    virtual bool DoesBlockExist(uint256 hash) = 0;

    virtual CBlockIndex *GetBlockIndex(uint256 hash) = 0;

    virtual int GetSpendHeight(const CCoinsViewCache &inputs) = 0;

    virtual int GetActiveChainHeight() = 0;

    virtual bool GetActiveChainTipHash(uint256 &tipHash) = 0;

    virtual CChain &GetActiveChain() = 0;

    virtual std::set<const CBlockIndex *, CompareBlocksByHeight> GetTips() = 0;

    virtual CCoinsView *GetCoinViewDB() = 0;

    virtual CCoinsViewCache *GetCoinsTip() = 0;

    virtual CBlockTreeDB *GetBlockTreeDB() = 0;

    virtual CBlockIndex *GetIndexBestHeader() = 0;

    virtual CBlockIndex *FindForkInGlobalIndex(const CChain &chain, const CBlockLocator &locator) = 0;

    virtual bool ActivateBestChain(CValidationState &state, const CChainParams &chainparams,
                                   std::shared_ptr<const CBlock> pblock) = 0;

    virtual bool VerifyDB(const CChainParams &chainparams, CCoinsView *coinsview, int nCheckLevel, int nCheckDepth) = 0;

    virtual bool ProcessNewBlockHeaders(const std::vector<CBlockHeader> &headers, CValidationState &state,
                                        const CChainParams &chainparams, const CBlockIndex **ppindex,
                                        CBlockHeader *first_invalid) = 0;

    virtual bool ProcessNewBlock(const CChainParams &chainparams, const std::shared_ptr<const CBlock> pblock,
                                 bool fForceProcessing, bool *fNewBlock) = 0;

    virtual bool NetRequestCheckPoint(ExNode *xnode, int height) = 0;

    virtual bool NetReceiveCheckPoint(ExNode *xnode, CDataStream &stream) = 0;

    virtual bool NetRequestBlocks(ExNode *xnode, CDataStream &stream, std::vector<uint256> &blockHashes) = 0;

    virtual bool NetRequestHeaders(ExNode *xnode, CDataStream &stream) = 0;

    virtual bool NetReceiveHeaders(ExNode *xnode, CDataStream &stream) = 0;

    virtual bool NetRequestBlockData(ExNode *xnode, uint256 blockHash, int blockType, void *filter) = 0;

    virtual bool NetReceiveBlockData(ExNode *xnode, CDataStream &stream, uint256 &blockHash) = 0;

    virtual bool NetRequestBlockTxn(ExNode *xnode, CDataStream &stream) = 0;

    virtual bool NetRequestMostRecentCmpctBlock(ExNode *xnode, uint256 bestBlockHint) = 0;

    virtual bool
    ProcessNewBlock(const std::shared_ptr<const CBlock> pblock, bool fForceProcessing, bool *fNewBlock) = 0;

    virtual bool TestBlockValidity(CValidationState &state, const CChainParams &chainparams, const CBlock &block,
                                   CBlockIndex *pindexPrev, bool fCheckPOW, bool fCheckMerkleRoot) = 0;

    virtual void PruneBlockFilesManual(int nManualPruneHeight) = 0;

    virtual bool
    CheckBlock(const CBlock &block, CValidationState &state, const Consensus::Params &consensusParams, bool fCheckPOW,
               bool fCheckMerkleRoot) = 0;

    virtual bool InvalidateBlock(CValidationState &state, const CChainParams &chainparams, CBlockIndex *pindex) = 0;

    virtual bool PreciousBlock(CValidationState &state, const CChainParams &params, CBlockIndex *pindex) = 0;

    virtual void FlushStateToDisk() = 0;

    virtual bool FlushStateToDisk(CValidationState &state, FlushStateMode mode, const CChainParams &chainparams) = 0;

    virtual bool ResetBlockFailureFlags(CBlockIndex *pindex) = 0;

    virtual void UpdateCoins(const CTransaction &tx, CCoinsViewCache &inputs, int nHeight) = 0;

    virtual CAmount GetBlockSubsidy(int nHeight) = 0;

    virtual bool IsSBTCForkEnabled(const int height) = 0;

//    virtual bool IsSBTCContractEnabled(const CBlockIndex *pindex) = 0;

    virtual bool IsSBTCForkContractEnabled(const int height) = 0;

    //add other interface methods here ...
};

#define GET_CHAIN_INTERFACE(ifObj) \
    auto ifObj = GetApp()->FindComponent<IChainComponent>()
