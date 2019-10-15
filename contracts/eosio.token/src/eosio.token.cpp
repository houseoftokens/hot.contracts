#include <eosio.token/eosio.token.hpp>

#define HOT_CORE_SYMBOL (symbol("HOT", 6))
#define HOT_BONUS_SCOPE 0
#define HOT_BONUS_ACT_PER_ROUND 8

namespace eosio {

void token::create( name   issuer,
                    asset  maximum_supply )
{
    require_auth( _self );

    auto sym = maximum_supply.symbol;
    check( sym.is_valid(), "invalid symbol name" );
    check( maximum_supply.is_valid(), "invalid supply");
    check( maximum_supply.amount > 0, "max-supply must be positive");

    stats statstable( _self, sym.code().raw() );
    auto existing = statstable.find( sym.code().raw() );
    check( existing == statstable.end(), "token with symbol already exists" );

    statstable.emplace( _self, [&]( auto& s ) {
       s.supply.symbol = maximum_supply.symbol;
       s.max_supply    = maximum_supply;
       s.issuer        = issuer;
    });
}


void token::issue( name to, asset quantity, string memo )
{
    auto sym = quantity.symbol;
    check( sym.is_valid(), "invalid symbol name" );
    check( memo.size() <= 256, "memo has more than 256 bytes" );

    stats statstable( _self, sym.code().raw() );
    auto existing = statstable.find( sym.code().raw() );
    check( existing != statstable.end(), "token with symbol does not exist, create token before issue" );
    const auto& st = *existing;

    require_auth( st.issuer );
    check( quantity.is_valid(), "invalid quantity" );
    check( quantity.amount > 0, "must issue positive quantity" );

    check( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
    check( quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

    statstable.modify( st, same_payer, [&]( auto& s ) {
       s.supply += quantity;
    });

    add_balance( st.issuer, quantity, st.issuer );

    if( to != st.issuer ) {
      SEND_INLINE_ACTION( *this, transfer, { {st.issuer, "active"_n} },
                          { st.issuer, to, quantity, memo }
      );
    }
}

void token::retire( asset quantity, string memo )
{
    auto sym = quantity.symbol;
    check( sym.is_valid(), "invalid symbol name" );
    check( memo.size() <= 256, "memo has more than 256 bytes" );

    stats statstable( _self, sym.code().raw() );
    auto existing = statstable.find( sym.code().raw() );
    check( existing != statstable.end(), "token with symbol does not exist" );
    const auto& st = *existing;

    require_auth( st.issuer );
    check( quantity.is_valid(), "invalid quantity" );
    check( quantity.amount > 0, "must retire positive quantity" );

    check( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );

    statstable.modify( st, same_payer, [&]( auto& s ) {
       s.supply -= quantity;
    });

    sub_balance( st.issuer, quantity );
}

void token::transfer( name    from,
                      name    to,
                      asset   quantity,
                      string  memo )
{
    check( from != to, "cannot transfer to self" );
    require_auth( from );
    check( is_account( to ), "to account does not exist");
    auto sym = quantity.symbol.code();
    stats statstable( _self, sym.raw() );
    const auto& st = statstable.get( sym.raw() );

    require_recipient( from );
    require_recipient( to );

    check( quantity.is_valid(), "invalid quantity" );
    check( quantity.amount > 0, "must transfer positive quantity" );
    check( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
    check( memo.size() <= 256, "memo has more than 256 bytes" );

    auto payer = has_auth( to ) ? to : from;

    sub_balance( from, quantity );
    add_balance( to, quantity, payer );
}

void token::sub_balance( name owner, asset value ) {
    accounts from_acnts( _self, owner.value );
    
    const auto& from = from_acnts.get( value.symbol.code().raw(), "no balance object found" );
    check( from.balance.amount >= value.amount, "overdrawn balance" );
    asset balance;
    from_acnts.modify( from, owner, [&]( auto& a ) {
       a.balance -= value;
       balance = a.balance;
    });
    on_balance_change(owner, balance, owner);
}

void token::add_balance( name owner, asset value, name ram_payer )
{
   accounts to_acnts( _self, owner.value );
   auto to = to_acnts.find( value.symbol.code().raw() );
   asset balance;
   if( to == to_acnts.end() ) {
      to_acnts.emplace( ram_payer, [&]( auto& a ){
        a.balance = value;
        balance = a.balance;
      });
   } else {
      to_acnts.modify( to, same_payer, [&]( auto& a ) {
        a.balance += value;
        balance = a.balance;
      });      
   }
   on_balance_change(owner, balance, ram_payer);
}

void token::on_balance_change(name owner, asset balance, name ram_payer)
{
   // only update on core asset balance changing
   if ( balance.symbol != HOT_CORE_SYMBOL ) {
      return;
   }
   // find newest round number
   brnd bonus_rounds( _self, HOT_BONUS_SCOPE );
   uint64_t round_num = 0;
   auto it_latest = bonus_rounds.begin();
   if (it_latest != bonus_rounds.end()) {
      round_num = it_latest->round;
   }

   // update bonus meta
   abms to_abms( _self, HOT_BONUS_SCOPE );
   auto it_to = to_abms.find( owner.value );
   if ( it_to == to_abms.end() ) {
      // if no abms record, create a new one
      to_abms.emplace( ram_payer, [&]( auto &m ) {
         m.owner = owner;
         m.round = round_num;
         m.balance = balance.amount;
         m.bonus = asset();
      });
   } else {
      // compare current round number to abms
      if ( it_to->round + 1 == round_num ) {
         // new round is started since last update
         to_abms.modify( it_to, same_payer, [&]( auto &m ) {   
            m.bonus = calc_bonus( m.balance );
            m.round = round_num;
            m.balance = balance.amount;
         });
      } else if ( it_to->round == round_num ) {
         // update abms record
         to_abms.modify( it_to, same_payer, [&]( auto &m ) {
            m.balance = balance.amount;
         });
      } else {
         // this should not happen
         check( false, "abms round number should <= current round number" );
      }
   }
}

// caculate bonus of last round
asset token::calc_bonus(uint64_t balance) const {
   brnd bonus_rounds( _self, HOT_BONUS_SCOPE );
   auto it = bonus_rounds.begin();
   check( it != bonus_rounds.end(), "bonus round not found" );
   check( it->clearing, "calc_bonus should be called during clearing" );
   uint128_t bonus_val = uint128_t(it->bonus.amount) * uint128_t(balance);
   bonus_val /= uint128_t(it->clearbase);
   // asset bonus = it->bonus * balance / it->clearbase;
   asset bonus = it->bonus;
   bonus.amount = uint64_t(bonus_val);
   return bonus;
}

void token::open( name owner, const symbol& symbol, name ram_payer )
{
   require_auth( ram_payer );

   check( is_account( owner ), "owner account does not exist" );

   auto sym_code_raw = symbol.code().raw();
   stats statstable( _self, sym_code_raw );
   const auto& st = statstable.get( sym_code_raw, "symbol does not exist" );
   check( st.supply.symbol == symbol, "symbol precision mismatch" );

   accounts acnts( _self, owner.value );
   auto it = acnts.find( sym_code_raw );
   if( it == acnts.end() ) {
      acnts.emplace( ram_payer, [&]( auto& a ){
        a.balance = asset{0, symbol};
      });
      on_balance_change(owner, asset{0, symbol}, ram_payer);
   }
}

void token::close( name owner, const symbol& symbol )
{
   require_auth( owner );
   accounts acnts( _self, owner.value );
   auto it = acnts.find( symbol.code().raw() );
   check( it != acnts.end(), "Balance row already deleted or never existed. Action won't have any effect." );
   check( it->balance.amount == 0, "Cannot close because the balance is not zero." );
   acnts.erase( it );
}

void token::bonusfreeze( asset bonus, asset minimum, name collector )
{
   check( bonus.is_valid(), "invalid bonus quantity" );
   check( minimum.is_valid(), "invalid minimum quantity" );
   check( bonus.symbol == minimum.symbol, "bonus and minimum should be the same token" );
   check( bonus.amount > 0, "bonus amount should greator than 0" );
   check( bonus.amount > minimum.amount, "bonus amount should greator than minimum amount" );
   check( is_account( collector ), "collector account does not exist");

   // check existance of bonus token
   stats stat_bonus( _self, bonus.symbol.code().raw() );
   auto it_stat_bonus = stat_bonus.find( bonus.symbol.code().raw() );
   check( it_stat_bonus != stat_bonus.end(), "token with bonus symbol does not exist, cannot freeze bonus" );
   const auto& st = *it_stat_bonus;
   require_auth( st.issuer );

   // get current CORE ASSET supply
   stats stat_core( _self, HOT_CORE_SYMBOL.code().raw() );
   auto it_stat_core = stat_core.find( HOT_CORE_SYMBOL.code().raw() );
   check( it_stat_core != stat_core.end(), "core asset token does not exist, cannot freeze bonus" );
   uint64_t supply = it_stat_core->supply.amount;

   // update round info
   brnd bonus_rounds( _self, HOT_BONUS_SCOPE );
   auto it_br = bonus_rounds.begin();
   if ( it_br == bonus_rounds.end() ) {
      // this is the first time freeze
      bonus_rounds.emplace(st.issuer, [&]( auto &br ) {
         br.id = 1;
         br.round = 1;
         br.clearing = true;
         br.clearbase = supply;
         br.bonus = bonus;
         br.minmum_bonus = minimum;
         br.balance = bonus;
         br.collector = collector;
      });
   } else {
      check(!it_br->clearing, "in process of clearing, cannot freeze bonus");
      bonus_rounds.modify( *it_br, same_payer, [&]( auto &br ) {
         br.round += 1;
         br.clearing = true;
         br.clearbase = supply;
         br.bonus = bonus;
         br.minmum_bonus = minimum;
         br.balance = bonus;
         br.collector = collector;
      });
   }
}

void token::bonusclear()
{
   brnd bonus_rounds( _self, HOT_BONUS_SCOPE );
   auto it_br = bonus_rounds.begin();
   check( it_br != bonus_rounds.end(), "bonus round not found" );
   check( it_br->clearing, "bonus round has not been frozen yet" );

   stats statstable( _self, it_br->bonus.symbol.code().raw() );
   auto existing = statstable.find( it_br->bonus.symbol.code().raw() );
   check( existing != statstable.end(), "token with bonus symbol does not exist, cannot do bonus clear" );
   const auto& st = *existing;
   require_auth( st.issuer );

   std::vector<name> bonus_accs;
   bool done_clear = false;
   abms to_abms( _self, HOT_BONUS_SCOPE );

   // check account whose balance does not update during freeze
   auto idx_rnd = to_abms.get_index<"byround"_n>();
   for ( auto it = idx_rnd.begin(); it != idx_rnd.end() && it->round < it_br->round; ++it ) {
      // maximum action per transaction
      if ( bonus_accs.size() >= HOT_BONUS_ACT_PER_ROUND ) {
         break;
      }
      bonus_accs.push_back(it->owner);
   }

   if ( bonus_accs.size() < HOT_BONUS_ACT_PER_ROUND ) {
      // check account whose balance has updated during freeze
      auto index = to_abms.get_index<"bybonus"_n>();
      for (auto it = index.rbegin(); bonus_accs.size() < HOT_BONUS_ACT_PER_ROUND; ++it) {
         if (it == index.rend() || it->bonus.amount == 0) {
            // done clear
            done_clear = true;
            break;
         }
         bonus_accs.push_back(it->owner);
      }
   }

   // send acctions for bonus
   for (auto acc = bonus_accs.begin(); acc != bonus_accs.end(); ++acc) {
      SEND_INLINE_ACTION( *this, bonus, { {st.issuer, "active"_n} }, { *acc } );
   }

   // we are done clear
   if (done_clear) {
      auto it_br = bonus_rounds.begin();
      check( it_br != bonus_rounds.end(), "bonus round not found" );
      if (it_br->balance.amount > 0) {
         // send all left bonus to issuer
         SEND_INLINE_ACTION( *this, bonusclose, { {st.issuer, "active"_n} }, { false } );
      }
      // // mark this round as finished
      // bonus_rounds.modify( *it_br, same_payer, [&]( auto& br ) {
      //    br.clearing = false;
      //    br.clearbase = 0;
      //    br.bonus = asset();
      //    br.minmum_bonus = asset();
      //    br.balance = asset();
      //    br.collector = name();
      // });
   }
}

void token::bonus(name to )
{
   brnd bonus_rounds( _self, HOT_BONUS_SCOPE );
   auto it_br = bonus_rounds.begin();
   check( it_br != bonus_rounds.end(), "bonus round not found, could not bonus" );
   check( it_br->clearing, "bonus round not frozen yet, could not bonus" );

   stats statstable( _self, it_br->bonus.symbol.code().raw() );
   auto existing = statstable.find( it_br->bonus.symbol.code().raw() );
   check( existing != statstable.end(), "token with bonus symbol does not exist, create token before bonus" );
   const auto& st = *existing;
   require_auth( st.issuer );

   abms to_abms( _self, HOT_BONUS_SCOPE );
   auto it_abms = to_abms.find( to.value );
   check( it_abms != to_abms.end(), "abms not found, could not bonus" );

   if ( it_abms->round + 1 == it_br->round ) {
      // not balance update happen after freeze
      to_abms.modify( it_abms, same_payer, [&]( auto &m ) {   
         m.bonus = calc_bonus( m.balance );
         m.round = it_br->round;
      });
      // fetch abms after modfication
      it_abms = to_abms.find( to.value );
      check( it_abms != to_abms.end(), "abms not found, could not bonus" );
   } else if (it_abms->round != it_br->round) {
      check(false, "round number not match, this should not happen");
   }

   check( it_br->bonus.symbol == it_abms->bonus.symbol, "bonus symbol should be the same" );
   // check( it_abms->bonus.amount > 0, "abms bonus should greater than 0" );

   auto real_bonus = it_abms->bonus;
   to_abms.modify( it_abms, same_payer, [&]( auto& a ) {
      a.bonus = asset();
   });

   // if bonus is less than minimum bonus amount, do not bonus
   if (real_bonus.amount < it_br->minmum_bonus.amount) {
      return;
   }
   // run actual bonus
   check( real_bonus.amount <= st.max_supply.amount - st.supply.amount, "bonus quantity exceeds available supply");

   // update round balance
   bonus_rounds.modify( *it_br, same_payer, [&]( auto &br ) {
      br.balance -= real_bonus;
   });

   // following logic likes issue
   statstable.modify( st, same_payer, [&]( auto& s ) {
      s.supply += real_bonus;
   });

   add_balance( st.issuer, real_bonus, st.issuer );

   if( to != st.issuer ) {
      SEND_INLINE_ACTION( *this, transfer, { {st.issuer, "active"_n} },
            { st.issuer, to, real_bonus, "bonus" }
      );
   }
}

void token::bonusclose( bool force ) {
   brnd bonus_rounds( _self, HOT_BONUS_SCOPE );
   auto it_br = bonus_rounds.begin();
   check( it_br != bonus_rounds.end(), "bonus round not found" );
   check( it_br->clearing, "bonus round has not been frozen yet" );

   stats statstable( _self, it_br->bonus.symbol.code().raw() );
   auto existing = statstable.find( it_br->bonus.symbol.code().raw() );
   check( existing != statstable.end(), "token with bonus symbol does not exist, create token before bonusclose" );
   const auto& st = *existing;

   if ( !force ) {
      require_auth( st.issuer );
   } else {
      // super user auth required
      require_auth( _self );
   }

   abms to_abms( _self, HOT_BONUS_SCOPE );
   auto idx_rnd = to_abms.get_index<"byround"_n>();
   auto it = idx_rnd.find(it_br->round - 1);
   check( it == idx_rnd.end(), "when bonus close, there should be no abms round smaller than current round number" );
   
   auto index = to_abms.get_index<"bybonus"_n>();
   auto it_bn = index.rbegin();
   if ( it_bn != index.rend() ) {
      check( it_bn->bonus.amount <= 0, "when bonus close, there should be no abms with bonus greater than 0" );
   }

   auto real_bonus = it_br->balance;
   auto collector = it_br->collector;
   bonus_rounds.modify( *it_br, same_payer, [&]( auto& br ) {
      br.clearing = false;
      br.clearbase = 0;
      br.bonus = asset();
      br.minmum_bonus = asset();
      br.balance = asset();
      br.collector = name();
   });

   if ( real_bonus.amount > 0 ) {
      add_balance( st.issuer, real_bonus, st.issuer );
      if( collector != st.issuer ) {
         SEND_INLINE_ACTION( *this, transfer, { {st.issuer, "active"_n} },
               { st.issuer, collector, real_bonus, "bonus close" }
         );
      }      
   }
}

} /// namespace eosio

EOSIO_DISPATCH( eosio::token, (create)(issue)(transfer)(open)(close)(retire)(bonusfreeze)(bonusclear)(bonus)(bonusclose) )
