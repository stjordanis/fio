/** FioFee implementation file
 *  Description: FioFee is the smart contract that manages fees.
 *  @author Adam Androulidakis, Casey Gardiner, Ed Rotthoff
 *  @modifedby
 *  @file fio.fee.cpp
 *  @license FIO Foundation ( https://github.com/fioprotocol/fio/blob/master/LICENSE ) Dapix
 */

#include "fio.fee.hpp"
#include "../fio.address/fio.address.hpp"
#include <fio.common/fio.common.hpp>
#include <fio.common/fioerror.hpp>
#include <eosio/native/intrinsics.hpp>
#include <string>
#include <map>

using std::string;

namespace fioio {

    /***
     * This contract maintains fee related actions and data. It provides voting actions which allow the
     * setting of fee amounts for mandatory fees, and the setting of bundle counts for bundled fees
     * by the active block producers. It provides datga structures representing the fee construct within
     * the FIO protocol. It provides the computation of fees from the voting
     * results of block producers.
     */
    class [[eosio::contract("FioFee")]]  FioFee : public eosio::contract {

    private:
        const int MIN_FEE_VOTERS_FOR_MEDIAN = 15;
        fiofee_table fiofees;
        feevoters_table feevoters;
        bundlevoters_table bundlevoters;
        feevotes_table feevotes;
        eosiosystem::top_producers_table topprods;
        eosiosystem::producers_table prods;

        uint32_t update_fees() {
            map<uint64_t, double> producer_fee_multipliers_map;
            vector<uint128_t> fee_hashes; //hashes for endpoints to process.
            vector<string> fee_endpoints;

            int NUMBER_FEEVOTERS_TO_PROCESS = 400;

            //create a map for each top 21 producer and its associated multiplier
            int num_voters = 0;
            auto topprod = topprods.begin();
            while (topprod != topprods.end()) {
                    auto voters_iter = feevoters.find(topprod->producer.value);
                    if (voters_iter != feevoters.end()) {
                        producer_fee_multipliers_map.insert(make_pair(topprod->producer.value, voters_iter->fee_multiplier));
                        num_voters++;
                    }
                topprod++;
            }

            //get all the fees needing processing.
            auto fee = fiofees.begin();
            while (fee != fiofees.end()) {
               if(fee->votes_pending.value()){
                   fee_hashes.push_back(fee->end_point_hash);
                   fee_endpoints.push_back(fee->end_point);
               }
                fee++;
            }

            //400 error if fees to process is empty.
            fio_400_assert(fee_hashes.size() > 0, "compute fees", "compute fees",
                           "No Work.", ErrorNoWork);

            //figure out how many to process this work iteration.
            int feevotestoprocess = (num_voters * fee_hashes.size());
            int numberiterations = ((feevotestoprocess % NUMBER_FEEVOTERS_TO_PROCESS) > 0) ? (feevotestoprocess / NUMBER_FEEVOTERS_TO_PROCESS)+1 : (feevotestoprocess / NUMBER_FEEVOTERS_TO_PROCESS);
            int numberfeestoprocess = (numberiterations == 1) ? fee_hashes.size() : NUMBER_FEEVOTERS_TO_PROCESS/num_voters;

            //only process how many you need to process.
            if(fee_hashes.size() > numberfeestoprocess) {
                fee_hashes.erase(fee_hashes.begin()+numberfeestoprocess,fee_hashes.end());
                fee_endpoints.erase(fee_endpoints.begin()+numberfeestoprocess,fee_endpoints.end());
            }

            auto feevotesbyendpoint = feevotes.get_index<"byendpoint"_n>();
            auto feesbyendpoint = fiofees.get_index<"byendpoint"_n>();
            vector <uint64_t> feevalues;

            for(int i=0;i<fee_hashes.size();i++){
                feevalues.clear();
                auto vote_iter = feevotesbyendpoint.lower_bound(fee_hashes[i]);
                //build fee values.
                while ( vote_iter != feevotesbyendpoint.end()) {
                    if(vote_iter->end_point_hash != fee_hashes[i]){
                        break;
                    }
                    if (producer_fee_multipliers_map.find(vote_iter->block_producer_name.value) !=
                        producer_fee_multipliers_map.end()) {
                        //note -- we protect against both overflow and negative values here, an
                        //overflow error should result computing the dresult,and we check if the
                        //result is negative.
                        const double dresult = producer_fee_multipliers_map[vote_iter->block_producer_name.value] *
                                (double) vote_iter->suf_amount;
                        const uint64_t voted_fee = (uint64_t)(dresult);
                        feevalues.push_back(voted_fee);
                    }
                    vote_iter++;
                }
                //compute median
                int64_t median_fee = compute_median(feevalues, fee_endpoints[i], fee_hashes[i]);
                //set it.
                if (median_fee > 0) {
                    //update the fee.
                    auto fee_iter = feesbyendpoint.find(fee_hashes[i]);
                    if (fee_iter != feesbyendpoint.end()) {
                        feesbyendpoint.modify(fee_iter, _self, [&](struct fiofee &ff) {
                            ff.suf_amount = median_fee;
                            ff.votes_pending.emplace(false);
                        });

                    }
                }
            }
            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

            return fee_hashes.size();
        }

        /*******
        * This method will compute the median fee from the fee votes that are cast.
        *
        * @param feevalues
        * @param fee_endpoint
        * @param fee_endpoint_hash
        */
        int64_t
        compute_median(vector <uint64_t> feevalues, const string &fee_endpoint, const uint128_t &fee_endpoint_hash) {
            //one more time
            if (feevalues.size() >= MIN_FEE_VOTERS_FOR_MEDIAN) {
                uint64_t median_fee = 0;
                //sort it.
                sort(feevalues.begin(), feevalues.end());
                //if the number of values is odd use the middle one.
                if ((feevalues.size() % 2) == 1) {
                    const int useIdx = (feevalues.size() / 2);
                    median_fee = feevalues[useIdx];
                } else {//even number in the list. use the middle 2
                    const int useIdx = (feevalues.size() / 2) - 1;
                    median_fee = (feevalues[useIdx] + feevalues[useIdx + 1]) / 2;
                }
                return median_fee;
            }
            return -1;
        }

    public:
        using contract::contract;

        FioFee(name s, name code, datastream<const char *> ds)
                : contract(s, code, ds),
                  fiofees(_self, _self.value),
                  bundlevoters(_self, _self.value),
                  feevoters(_self, _self.value),
                  feevotes(_self, _self.value),
                  topprods(SYSTEMACCOUNT, SYSTEMACCOUNT.value),
                  prods(SYSTEMACCOUNT,SYSTEMACCOUNT.value){
        }

        /*********
         * This action provides the ability to set a fee vote by a block producer.
         * the user submits a list of feevalue objects each of which contains a suf amount
         * (to which a multiplier will later be applied, and an endpoint name which must
         * match one of the endpoint names in the fees table.
         * @param fee_values this is a vector of feevalue objects each has the
         *                     endpoint name and desired fee in FIO SUF
         * @param actor this is the string rep of the fio account for the user making the call, a block producer.
         */
        // @abi action
        [[eosio::action]]
        void setfeevote(const vector <feevalue> &fee_values, const int64_t &max_fee, const string &actor) {

            name aactor = name(actor.c_str());
            require_auth(aactor);

            bool dbgout = false;

            auto prodsbyowner = prods.get_index<"byowner"_n>();
            //check that the producer is active block producer
            fio_400_assert(((prodsbyowner.find(aactor.value) != prodsbyowner.end())), "actor", actor,
                           " Not an active BP",
                           ErrorFioNameNotReg);

            fio_400_assert(max_fee >= 0, "max_fee", to_string(max_fee), "Invalid fee value",
                           ErrorMaxFeeInvalid);

            const uint32_t nowtime = now();

            //check the submitted fee values.
            for (auto &feeval : fee_values) {
                //check the endpoint exists for this fee
                const uint128_t endPointHash = string_to_uint128_hash(feeval.end_point.c_str());

                auto feesbyendpoint = fiofees.get_index<"byendpoint"_n>();
                auto fees_iter = feesbyendpoint.find(endPointHash);

                fio_400_assert(fees_iter != feesbyendpoint.end(), "end_point", feeval.end_point,
                               "invalid end_point", ErrorEndpointNotFound);

                fio_400_assert(feeval.value >= 0, "fee_value", feeval.end_point,
                               "invalid fee value", ErrorFeeInvalid);

                //get all the votes made by this actor. go through the list
                //and find the fee vote to update.
                auto feevotesbybpname = feevotes.get_index<"bybpname"_n>();
                auto votebyname_iter = feevotesbybpname.lower_bound(aactor.value);

                uint64_t idtoremove;
                bool found = false;
                bool timeviolation = false;
                while (votebyname_iter != feevotesbybpname.end())
                {
                    if (votebyname_iter->block_producer_name.value != aactor.value) {
                        //if the bp name changes we have exited the items of interest, so quit.
                        break;
                    }

                    if (votebyname_iter->end_point_hash == endPointHash) {
                        //check the time of the last update, remove and replace if
                        //its been longer than the time between votes
                        const uint32_t lastupdate = votebyname_iter->lastvotetimestamp;

                        //be silent if the time between votes has not yet elapsed.
                        if (lastupdate > (nowtime - TIME_BETWEEN_FEE_VOTES_SECONDS)) {
                            timeviolation = true;
                            break;
                        } else {
                            idtoremove = votebyname_iter->id;
                            found = true;
                            break;
                        }
                    }
                    votebyname_iter++;
                }

                if (found) {
                    auto myiter = feevotes.find(idtoremove);
                    if(myiter != feevotes.end()) {
                        feevotes.erase(myiter);
                    }
                }

                if (!timeviolation) {
                    feevotes.emplace(aactor, [&](struct feevote &fv) {
                        fv.id = feevotes.available_primary_key();
                        fv.block_producer_name = aactor;
                        fv.end_point = feeval.end_point;
                        fv.end_point_hash = endPointHash;
                        fv.suf_amount = feeval.value;
                        fv.lastvotetimestamp = nowtime;
                    });

                    feesbyendpoint.modify(fees_iter, _self, [&](struct fiofee &a) {
                        a.votes_pending.emplace(true);
                    });
                } else {
                    fio_400_assert(false, "", "", "Too soon since last call", ErrorTimeViolation);
                }
            }

            //begin new fees, logic for Mandatory fees.
            uint128_t endpoint_hash = string_to_uint128_hash("submit_fee_ratios");

            auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();
            auto fee_iter = fees_by_endpoint.find(endpoint_hash);
            //if the fee isnt found for the endpoint, then 400 error.
            fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", "submit_fee_ratios",
                           "FIO fee not found for endpoint", ErrorNoEndpoint);

            uint64_t reg_amount = fee_iter->suf_amount;
            uint64_t fee_type = fee_iter->type;

            //if its not a mandatory fee then this is an error.
            fio_400_assert(fee_type == 0, "fee_type", to_string(fee_type),
                           "submit_fee_ratios unexpected fee type for endpoint submit_fee_ratios, expected 0",
                           ErrorNoEndpoint);

            fio_400_assert(max_fee >= (int64_t)reg_amount, "max_fee", to_string(max_fee), "Fee exceeds supplied maximum.",
                           ErrorMaxFeeExceeded);

            fio_fees(aactor, asset(reg_amount, FIOSYMBOL));
            processrewardsnotpid(reg_amount, get_self());
            //end new fees, logic for Mandatory fees.

            const string response_string = string("{\"status\": \"OK\"}");


            if (SETFEEVOTERAM > 0) {
                action(
                        permission_level{SYSTEMACCOUNT, "active"_n},
                        "eosio"_n,
                        "incram"_n,
                        std::make_tuple(aactor, SETFEEVOTERAM)
                ).send();
            }

            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

            send_response(response_string.c_str());
        }

        /**********
      * This method will update the fees based upon the present votes made by producers.
      */
        [[eosio::action]]
        void updatefees() {
            uint32_t numberprocessed = update_fees();
            const string response_string = string("{\"status\": \"OK\",\"fees_processed\":") +
                                           to_string(numberprocessed) + string("}");
            send_response(response_string.c_str());
        }

       /********
        * This action allows block producers to vote for the number of transactions that will be permitted
        * for free in the FIO bundled transaction model.
        * @param bundled_transactions the number of bundled transactions per FIO user.
        * @param actor the block producer actor that is presently voting, the signer of this tx.
        */
        // @abi action
        [[eosio::action]]
        void bundlevote(
                int64_t bundled_transactions,
                const string &actor
        ) {
            const name aactor = name(actor.c_str());
            require_auth(aactor);

            fio_400_assert(((topprods.find(aactor.value) != topprods.end())), "actor", actor,
                           " Not an active BP",
                           ErrorFioNameNotReg);

            fio_400_assert(bundled_transactions > 0, "bundled_transactions", to_string(bundled_transactions),
                           " Must be positive",
                           ErrorFioNameNotReg);

            const uint32_t nowtime = now();

            auto voter_iter = bundlevoters.find(aactor.value);
            if (voter_iter != bundlevoters.end()) //update if it exists
            {
                const uint32_t lastupdate = voter_iter->lastvotetimestamp;
                if (lastupdate <= (nowtime - TIME_BETWEEN_VOTES_SECONDS)) {
                    bundlevoters.modify(voter_iter, _self, [&](struct bundlevoter &a) {
                        a.block_producer_name = aactor;
                        a.bundledbvotenumber = bundled_transactions;
                        a.lastvotetimestamp = nowtime;
                    });
                } else {
                    fio_400_assert(false, "", "", "Too soon since last call", ErrorTimeViolation);
                }
            } else {
                bundlevoters.emplace(aactor, [&](struct bundlevoter &f) {
                    f.block_producer_name = aactor;
                    f.bundledbvotenumber = bundled_transactions;
                    f.lastvotetimestamp = nowtime;
                });
            }

            const string response_string = string("{\"status\": \"OK\"}");

            if (BUNDLEVOTERAM > 0) {
                action(
                        permission_level{SYSTEMACCOUNT, "active"_n},
                        "eosio"_n,
                        "incram"_n,
                        std::make_tuple(aactor, BUNDLEVOTERAM)
                ).send();
            }

            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

            send_response(response_string.c_str());
        }


        /**********
         * This action will create a new feevoters record if the specified block producer does not yet exist in the
         * feevoters table,
         * it will verify that the producer making the request is a present active block producer, it will update the
         * feevoters record if a pre-existing feevoters record exists.
         * @param multiplier this is the multiplier that will be applied to all fee votes for this producer before
         * computing the median fee.
         * @param actor this is the block producer voter.
         */
        // @abi action
        [[eosio::action]]
        void setfeemult(
                const double &multiplier,
                const int64_t &max_fee,
                const string &actor
        ) {

            const name aactor = name(actor.c_str());
            require_auth(aactor);

            auto prodsbyowner = prods.get_index<"byowner"_n>();
            //check that the producer is active block producer
            fio_400_assert(((prodsbyowner.find(aactor.value) != prodsbyowner.end())), "actor", actor,
                           " Not an active BP",
                           ErrorFioNameNotReg);

            fio_400_assert(multiplier > 0, "multiplier", to_string(multiplier),
                           " Must be positive",
                           ErrorFioNameNotReg);

            fio_400_assert(max_fee >= 0, "max_fee", to_string(max_fee), "Invalid fee value",
                           ErrorMaxFeeInvalid);

            const uint32_t nowtime = now();

            auto voter_iter = feevoters.find(aactor.value);
            if (voter_iter != feevoters.end())
            {
                const uint32_t lastupdate = voter_iter->lastvotetimestamp;
                if (lastupdate <= (nowtime - 120)) {
                    feevoters.modify(voter_iter, _self, [&](struct feevoter &a) {
                        a.block_producer_name = aactor;
                        a.fee_multiplier = multiplier;
                        a.lastvotetimestamp = nowtime;
                    });
                } else {
                    fio_400_assert(false, "", "", "Too soon since last call", ErrorTimeViolation);
                }
            } else {
                feevoters.emplace(aactor, [&](struct feevoter &f) {
                    f.block_producer_name = aactor;
                    f.fee_multiplier = multiplier;
                    f.lastvotetimestamp = nowtime;
                });
            }

            //get all voted fees and set votes pending.
            auto feevotesbybpname = feevotes.get_index<"bybpname"_n>();
            auto votebyname_iter = feevotesbybpname.lower_bound(aactor.value);
            auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();

            while (votebyname_iter != feevotesbybpname.end()) {
                if (votebyname_iter->block_producer_name.value != aactor.value) {
                    //if the bp name changes we have exited the items of interest, so quit.
                    break;
                } else {
                    auto fee_iter = fees_by_endpoint.find(votebyname_iter->end_point_hash);
                    fio_400_assert((fee_iter != fees_by_endpoint.end()), "end point", votebyname_iter->end_point,
                                   " Fee lookup error",
                                   ErrorNoFeesFoundForEndpoint);
                    fees_by_endpoint.modify(fee_iter, _self, [&](struct fiofee &a) {
                        a.votes_pending.emplace(true);
                    });

                }
                votebyname_iter++;
            }


            //begin new fees, logic for Mandatory fees.
            uint128_t endpoint_hash = string_to_uint128_hash("submit_fee_multiplier");

            auto fee_iter = fees_by_endpoint.find(endpoint_hash);
            //if the fee isnt found for the endpoint, then 400 error.
            fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", "submit_fee_multiplier",
                           "FIO fee not found for endpoint", ErrorNoEndpoint);

            uint64_t reg_amount = fee_iter->suf_amount;
            uint64_t fee_type = fee_iter->type;

            //if its not a mandatory fee then this is an error.
            fio_400_assert(fee_type == 0, "fee_type", to_string(fee_type),
                           "submit_fee_multiplier unexpected fee type for endpoint submit_fee_multiplier, expected 0",
                           ErrorNoEndpoint);

            fio_400_assert(max_fee >= (int64_t)reg_amount, "max_fee", to_string(max_fee), "Fee exceeds supplied maximum.",
                           ErrorMaxFeeExceeded);

            fio_fees(aactor, asset(reg_amount, FIOSYMBOL));
            processrewardsnotpid(reg_amount, get_self());
            //end new fees, logic for Mandatory fees.

                const string response_string = string("{\"status\": \"OK\"}");

            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

            send_response(response_string.c_str());
        }


        // @abi action
        [[eosio::action]]
        void mandatoryfee(
                const string &end_point,
                const name &account,
                const int64_t &max_fee
        ) {
            require_auth(account);
            //begin new fees, logic for Mandatory fees.
            const uint128_t endpoint_hash = fioio::string_to_uint128_hash(end_point.c_str());

            auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();

            auto fee_iter = fees_by_endpoint.find(endpoint_hash);
            //if the fee isnt found for the endpoint, then 400 error.
            fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", "register_producer",
                           "FIO fee not found for endpoint", ErrorNoEndpoint);

            const uint64_t reg_amount = fee_iter->suf_amount;
            const uint64_t fee_type = fee_iter->type;

            //if its not a mandatory fee then this is an error.
            fio_400_assert(fee_type == 0, "fee_type", to_string(fee_type),
                           "register_producer unexpected fee type for endpoint register_producer, expected 0",
                           ErrorNoEndpoint);

            fio_400_assert(max_fee >= (int64_t)reg_amount, "max_fee", to_string(max_fee), "Fee exceeds supplied maximum.",
                           ErrorMaxFeeExceeded);

            fio_fees(account, asset(reg_amount, FIOSYMBOL));
            processrewardsnotpid(reg_amount, get_self());

            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

        }

        // @abi action
        [[eosio::action]]
        void bytemandfee(
                const string &end_point,
                const name &account,
                const int64_t &max_fee,
                const int64_t &bytesize
        ) {

            require_auth(account);
            //begin new fees, logic for Mandatory fees.
            const uint128_t endpoint_hash = fioio::string_to_uint128_hash(end_point.c_str());

            auto fees_by_endpoint = fiofees.get_index<"byendpoint"_n>();

            auto fee_iter = fees_by_endpoint.find(endpoint_hash);
            //if the fee isnt found for the endpoint, then 400 error.
            fio_400_assert(fee_iter != fees_by_endpoint.end(), "endpoint_name", "register_producer",
                           "FIO fee not found for endpoint", ErrorNoEndpoint);

            uint64_t reg_amount = fee_iter->suf_amount;
            uint64_t remv = bytesize % 1000;
            uint64_t divv = bytesize / 1000;
            if (remv > 0 ){
                divv ++;
            }

            reg_amount = divv * reg_amount;

            const uint64_t fee_type = fee_iter->type;

            //if its not a mandatory fee then this is an error.
            fio_400_assert(fee_type == 0, "fee_type", to_string(fee_type),
                           "register_producer unexpected fee type for endpoint register_producer, expected 0",
                           ErrorNoEndpoint);

            fio_400_assert(max_fee >= (int64_t)reg_amount, "max_fee", to_string(max_fee), "Fee exceeds supplied maximum.",
                           ErrorMaxFeeExceeded);

            fio_fees(account, asset(reg_amount, FIOSYMBOL));
            processrewardsnotpid(reg_amount, get_self());
            //end new fees, logic for Mandatory fees.
        }

        /*******
         * This action will create a new fee on the FIO protocol.
         * @param end_point  this is the api endpoint name associated with the fee
         * @param type consult feetype, mandatory is 0 bundle eligible is 1
         * @param suf_amount this is the fee amount in FIO SUFs
         */
        // @abi action
        [[eosio::action]]
        void createfee(
                string end_point,
                int64_t type,
                int64_t suf_amount
        ) {
            require_auth(_self);

            fio_400_assert(suf_amount >= 0, "suf_amount", to_string(suf_amount),
                           " invalid suf amount",
                           ErrorFeeInvalid);

            const uint128_t endPointHash = string_to_uint128_hash(end_point.c_str());
            const uint64_t fee_id = fiofees.available_primary_key();

            auto feesbyendpoint = fiofees.get_index<"byendpoint"_n>();
            auto fees_iter = feesbyendpoint.find(endPointHash);

            if (fees_iter != feesbyendpoint.end())
            {
                    feesbyendpoint.modify(fees_iter, _self, [&](struct fiofee &a) {
                        a.type = type;
                        a.suf_amount = suf_amount;
                        //leave votes_pending as is, if votes are pending they need processed.
                    });
            } else {
                fiofees.emplace(get_self(), [&](struct fiofee &f) {
                    f.fee_id = fee_id;
                    f.end_point = end_point;
                    f.end_point_hash = endPointHash;
                    f.type = type;
                    f.suf_amount = suf_amount;
                    f.votes_pending.emplace(false);
                });
            }
            fio_400_assert(transaction_size() <= MAX_TRX_SIZE, "transaction_size", std::to_string(transaction_size()),
              "Transaction is too large", ErrorTransactionTooLarge);

        }
    };

    EOSIO_DISPATCH(FioFee, (setfeevote)(bundlevote)(setfeemult)(updatefees)
                           (mandatoryfee)(bytemandfee)(createfee)
    )
}
