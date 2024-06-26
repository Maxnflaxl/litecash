// Copyright 2018 The Beam Team
// Copyright 2019 - 2022 The LiteCash Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "wallet/common.h"
#include "wallet/wallet_db.h"

#include <condition_variable>
#include <boost/optional.hpp>
#include "utility/logger.h"

#include <future>

namespace beam { namespace wallet
{

    TxID GenerateTxID();

    struct ITransaction
    {
        using Ptr = std::shared_ptr<ITransaction>;
        virtual TxType GetType() const = 0;
        virtual void Update() = 0;
        virtual void Cancel() = 0;
    };


    std::string GetFailureMessage(TxFailureReason reason);

    class TransactionFailedException : public std::runtime_error
    {
    public:
        TransactionFailedException(bool notify, TxFailureReason reason, const char* message = "");
        bool ShouldNofify() const;
        TxFailureReason GetReason() const;
    private:
        bool m_Notify;
        TxFailureReason m_Reason;
    };

    //
    // State machine for managing per transaction negotiations between wallets
    // 
    class BaseTransaction : public ITransaction
    {
    public:
        using Ptr = std::shared_ptr<BaseTransaction>;
        BaseTransaction(INegotiatorGateway& gateway
                      , beam::IWalletDB::Ptr walletDB
                      , const TxID& txID);
        virtual ~BaseTransaction(){}

        const TxID& GetTxID() const;
        void Update() override;
        void Cancel() override;

        static const uint32_t s_ProtoVersion;

        template <typename T>
        bool GetParameter(TxParameterID paramID, T& value) const
        {
            return getTxParameter(*m_WalletDB, GetTxID(), paramID, value);
        }

        template <typename T>
        T GetMandatoryParameter(TxParameterID paramID) const
        {
            T value{};
            if (!getTxParameter(*m_WalletDB, GetTxID(), paramID, value))
            {
                LOG_ERROR() << GetTxID() << " Failed to get parameter: " << (int)paramID;
                throw TransactionFailedException(true, TxFailureReason::FailedToGetParameter);
            }
            return value;
        }

        template <typename T>
        bool SetParameter(TxParameterID paramID, const T& value)
        {
            bool shouldNotifyAboutChanges = ShouldNotifyAboutChanges(paramID);
            return SetParameter(paramID, value, shouldNotifyAboutChanges);
        }

        template <typename T>
        bool SetParameter(TxParameterID paramID, const T& value, bool shouldNotifyAboutChanges)
        {
            return setTxParameter(*m_WalletDB, GetTxID(), paramID, value, shouldNotifyAboutChanges);
        }

        template <typename T>
        void SetState(T state)
        {
            SetParameter(TxParameterID::State, state, true);
        }

        IWalletDB::Ptr GetWalletDB();
        bool IsInitiator() const;
        uint32_t get_PeerVersion() const;
        bool GetTip(Block::SystemState::Full& state) const;
    protected:
        bool CheckExpired();
        bool CheckExternalFailures();
        void ConfirmKernel(const Merkle::Hash& kernelID);
        void UpdateOnNextTip();
        void CompleteTx();
        void RollbackTx();
		void NotifyFailure(TxFailureReason);
        void UpdateTxDescription(TxStatus s);

        void OnFailed(TxFailureReason reason, bool notify = false);


        bool SendTxParameters(SetTxParameter&& msg) const;
        virtual void UpdateImpl() = 0;

        virtual bool ShouldNotifyAboutChanges(TxParameterID paramID) const { return true; };
    protected:

        INegotiatorGateway& m_Gateway;
        beam::IWalletDB::Ptr m_WalletDB;

        TxID m_ID;
        mutable boost::optional<bool> m_IsInitiator;
    };

    class TxBuilder;

    class SimpleTransaction : public BaseTransaction
    {
        enum State : uint8_t
        {
            Initial,
            Invitation,
            PeerConfirmation,
            
            InvitationConfirmation,
            Registration,

            KernelConfirmation,
            OutputsConfirmation
        };
    public:
        SimpleTransaction(INegotiatorGateway& gateway
                        , beam::IWalletDB::Ptr walletDB
                        , const TxID& txID);
    private:
        TxType GetType() const override;
        void UpdateImpl() override;
        bool ShouldNotifyAboutChanges(TxParameterID paramID) const override;
        void SendInvitation(const TxBuilder& builder, bool isSender);
        void ConfirmInvitation(const TxBuilder& builder, bool sendUtxos);
        void ConfirmTransaction(const TxBuilder& builder, bool sendUtxos);
        void NotifyTransactionRegistered();
        bool IsSelfTx() const;
        State GetState() const;
    private:
        io::AsyncEvent::Ptr m_CompletedEvent;
        std::future<void> m_OutputsFuture;
    };

    class TxBuilder
    {
    public:
        TxBuilder(BaseTransaction& tx, const AmountList& amount, Amount fee);

        void SelectInputs();
        void AddChange();
        void GenerateNewCoin(Amount amount, bool bChange);
        void AddOutput(Amount amount, bool bChange);
        void CreateOutputs();
        bool FinalizeOutputs();
        bool LoadKernel();
        bool HasKernelID() const;
        Output::Ptr CreateOutput(Amount amount, bool bChange);
        void CreateKernel();
        bool GenerateBlindingExcess();
        void GenerateNonce();
        ECC::Point::Native GetPublicExcess() const;
        ECC::Point::Native GetPublicNonce() const;
        bool GetInitialTxParams();
        bool GetPeerPublicExcessAndNonce();
        bool GetPeerSignature();
        bool GetPeerInputsAndOutputs();
        void FinalizeSignature();
        Transaction::Ptr CreateTransaction();
        void SignPartial();
        bool IsPeerSignatureValid() const;

        Amount GetAmount() const;
        const AmountList& GetAmountList() const;
        Amount GetFee() const;
        Height GetLifetime() const;
        Height GetMinHeight() const;
        Height GetMaxHeight() const;
        const std::vector<Input::Ptr>& GetInputs() const;
        const std::vector<Output::Ptr>& GetOutputs() const;
        const ECC::Scalar::Native& GetOffset() const;
        const ECC::Scalar::Native& GetPartialSignature() const;
        const TxKernel& GetKernel() const;
        const Merkle::Hash& GetKernelID() const;
        void StoreKernelID();
        std::string GetKernelIDString() const;
        bool UpdateMaxHeight();
        bool IsAcceptableMaxHeight() const;

        const std::vector<Coin>& GetCoins() const;
    private:
        BaseTransaction& m_Tx;

        // input
        AmountList m_AmountList;
        Amount m_Fee;
        Amount m_Change;
        Height m_Lifetime;
        Height m_MinHeight;
        Height m_MaxHeight;
        std::vector<Input::Ptr> m_Inputs;
        std::vector<Output::Ptr> m_Outputs;
        ECC::Scalar::Native m_BlindingExcess; // goes to kernel
        ECC::Scalar::Native m_Offset; // goes to offset

        std::vector<Coin> m_Coins;

        // peer values
        ECC::Scalar::Native m_PartialSignature;
        ECC::Point::Native m_PeerPublicNonce;
        ECC::Point::Native m_PeerPublicExcess;
        std::vector<Input::Ptr> m_PeerInputs;
        std::vector<Output::Ptr> m_PeerOutputs;
        ECC::Scalar::Native m_PeerOffset;
        Height m_PeerMaxHeight;

        // deduced values,
        TxKernel::Ptr m_Kernel;
        ECC::Scalar::Native m_PeerSignature;
        ECC::Hash::Value m_Message;
        ECC::Signature::MultiSig m_MultiSig;

        mutable boost::optional<Merkle::Hash> m_KernelID;
    };
}}
