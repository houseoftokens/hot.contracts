#pragma once

#include <eosiolib/asset.hpp>
#include <eosiolib/eosio.hpp>
#include <eosiolib/singleton.hpp>

#include <string>

namespace eosiosystem {
   class system_contract;
}

namespace eosio {

   using std::string;

   class [[eosio::contract("eosio.token")]] token : public contract {
      public:
         using contract::contract;

         [[eosio::action]]
         void create( name   issuer,
                      asset  maximum_supply);

         [[eosio::action]]
         void issue( name to, asset quantity, string memo );

         [[eosio::action]]
         void retire( asset quantity, string memo );

         [[eosio::action]]
         void transfer( name    from,
                        name    to,
                        asset   quantity,
                        string  memo );

         [[eosio::action]]
         void open( name owner, const symbol& symbol, name ram_payer );

         [[eosio::action]]
         void close( name owner, const symbol& symbol );

         [[eosio::action]]
         void bonusfreeze( asset bonus, asset minimum, name collector );

         [[eosio::action]]
         void bonusclear();

         [[eosio::action]]
         void bonus( name to );

         [[eosio::action]]
         void bonusclose( bool force );

         static asset get_supply( name token_contract_account, symbol_code sym_code )
         {
            stats statstable( token_contract_account, sym_code.raw() );
            const auto& st = statstable.get( sym_code.raw() );
            return st.supply;
         }

         static asset get_balance( name token_contract_account, name owner, symbol_code sym_code )
         {
            accounts accountstable( token_contract_account, owner.value );
            const auto& ac = accountstable.get( sym_code.raw() );
            return ac.balance;
         }

         using create_action = eosio::action_wrapper<"create"_n, &token::create>;
         using issue_action = eosio::action_wrapper<"issue"_n, &token::issue>;
         using retire_action = eosio::action_wrapper<"retire"_n, &token::retire>;
         using transfer_action = eosio::action_wrapper<"transfer"_n, &token::transfer>;
         using open_action = eosio::action_wrapper<"open"_n, &token::open>;
         using close_action = eosio::action_wrapper<"close"_n, &token::close>;
         using bonusfreeze_action = eosio::action_wrapper<"bonusfreeze"_n, &token::bonusfreeze>;
         using bonusclear_action = eosio::action_wrapper<"bonusclear"_n, &token::bonusclear>;
         using bonus_action = eosio::action_wrapper<"bonus"_n, &token::bonus>;
         using bonusclose_action = eosio::action_wrapper<"bonusclose"_n, &token::bonusclose>;

      private:

         // account bonus based on meta data of balance
         struct [[eosio::table]] account_bonus_meta {
            name      owner;        // bonus meta owner
            uint64_t  round;        // current bonus round
            int64_t   balance;      // bonus base balnace of *this* round
            asset     bonus;        // uncleared bonus shares

            uint64_t primary_key() const { return owner.value; }
            uint64_t bonus_round_key() const { return round; }
            uint64_t bonus_amount_key() const { return bonus.amount; }
         };
      
         // bonus round infomation
         struct [[eosio::table]] bonus_round {
            uint64_t  id;
            uint64_t  round;        // round num
            bool      clearing;     // whether we are clearing this round
            int64_t   clearbase;    // core asset supply on the time of round freeze
            asset     bonus;        // actual total bonus of this round
            asset     minmum_bonus; // if one got bonus less than this, then it get nothing
            asset     balance;      // dynamic balance during clearing
            name      collector;    // account who got balance after clear

            uint64_t primary_key() const { return id; }
         };

         struct [[eosio::table]] account {
            asset    balance;
            uint64_t primary_key() const { return balance.symbol.code().raw(); }
         };

         struct [[eosio::table]] currency_stats {
            asset    supply;
            asset    max_supply;
            name     issuer;

            uint64_t primary_key() const { return supply.symbol.code().raw(); }
         };

         typedef eosio::multi_index< "accounts"_n, account > accounts;
         typedef eosio::multi_index< "stat"_n, currency_stats > stats;

         typedef eosio::multi_index< "abms"_n, account_bonus_meta,
            indexed_by<"byround"_n, const_mem_fun<account_bonus_meta, uint64_t, &account_bonus_meta::bonus_round_key> >,
            indexed_by<"bybonus"_n, const_mem_fun<account_bonus_meta, uint64_t, &account_bonus_meta::bonus_amount_key> > 
         > abms;
         typedef eosio::multi_index< "brnd"_n, bonus_round > brnd;

         void sub_balance( name owner, asset value );
         void add_balance( name owner, asset value, name ram_payer );
         void on_balance_change(name owner, asset balance, name ram_payer);
         asset calc_bonus(uint64_t balance) const;
   };

} /// namespace eosio
