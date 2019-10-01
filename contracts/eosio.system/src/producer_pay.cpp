#include <eosio.system/eosio.system.hpp>

#include <eosio.token/eosio.token.hpp>

namespace eosiosystem {

   const int64_t  min_pervote_daily_pay = 100'0000;
   const int64_t  min_activated_stake   = 150'000'000'0000;
   const double   continuous_rate       = 0.04879;          // 5% annual rate
   const double   perblock_rate         = 0.0025;           // 0.25%
   const double   standby_rate          = 0.0075;           // 0.75%
   const uint32_t blocks_per_year       = 52*7*24*2*3600;   // half seconds per year
   const uint32_t seconds_per_year      = 52*7*24*3600;
   const uint32_t blocks_per_day        = 2 * 24 * 3600;
   const uint32_t blocks_per_hour       = 2 * 3600;
   const int64_t  useconds_per_day      = 24 * 3600 * int64_t(1000000);
   const int64_t  useconds_per_year     = seconds_per_year*1000000ll;

   void system_contract::onblock( ignore<block_header> ) {
      using namespace eosio;

      require_auth(_self);

      block_timestamp timestamp;
      name producer;
      _ds >> timestamp >> producer;

      // _gstate2.last_block_num is not used anywhere in the system contract code anymore.
      // Although this field is deprecated, we will continue updating it for now until the last_block_num field
      // is eventually completely removed, at which point this line can be removed.
      _gstate2.last_block_num = timestamp;

      /** until activated stake crosses this threshold no new rewards are paid */
      if( _gstate.total_activated_stake < min_activated_stake || get_producers_size() < 4 )
         return;

      if( _gstate.last_pervote_bucket_fill == time_point() )  /// start the presses
         _gstate.last_pervote_bucket_fill = current_time_point();


      /**
       * At startup the initial producer may not be one that is registered / elected
       * and therefore there may be no producer object for them.
       */
      auto prod = _producers.find( producer.value );
      if ( prod != _producers.end() ) {
         _gstate.total_unpaid_blocks++;
         _producers.modify( prod, same_payer, [&](auto& p ) {
               p.unpaid_blocks++;
         });
      }

      /// only update block producers once every minute, block_timestamp is in half seconds
      if( timestamp.slot - _gstate.last_producer_schedule_update.slot > 120 ) {
         update_elected_producers( timestamp );

         if( (timestamp.slot - _gstate.last_name_close.slot) > blocks_per_day ) {
            name_bid_table bids(_self, _self.value);
            auto idx = bids.get_index<"highbid"_n>();
            auto highest = idx.lower_bound( std::numeric_limits<uint64_t>::max()/2 );
            if( highest != idx.end() &&
                highest->high_bid > 0 &&
                (current_time_point() - highest->last_bid_time) > microseconds(useconds_per_day) &&
                _gstate.thresh_activated_stake_time > time_point() &&
                (current_time_point() - _gstate.thresh_activated_stake_time) > microseconds(14 * useconds_per_day)
            ) {
               _gstate.last_name_close = timestamp;
               channel_namebid_to_rex( highest->high_bid );
               idx.modify( highest, same_payer, [&]( auto& b ){
                  b.high_bid = -b.high_bid;
               });
            }
         }
      }
   }

   using namespace eosio;

   int16_t system_contract::get_producers_size() {
       int16_t count = 0;
       auto idx = _producers.get_index<"prototalvote"_n>();

       for ( auto it = idx.cbegin(); it != idx.cend(); ++it ) {
          ++ count;
       }
       return count;
   }




   //发起提案，只有gnode才可以发起提案。
   // type 1: add bp 2: remove bp 3: switch consensus
   void system_contract::newproposal( const name owner, const name account, uint32_t block_height, int16_t type, int16_t status) {
       require_auth( owner );
       const auto ct = current_time_point();

       auto prod3 = _producers3.find( owner.value );
       check(prod3 != _producers3.end(), "only governance node can new proposal");
       if(type == 1) {
           check(owner == account, "can not add other account to bp");
       }

       INLINE_ACTION_SENDER(eosio::token, transfer)(
          token_account, { {owner, active_permission} },
          { owner, prop_account, asset(15000, core_symbol()), "transfer 1.5000 EON to new proposal" }
       );

       uint64_t id = _proposals.available_primary_key();

       _proposals.emplace(_self, [&](auto &info) {
           info.id = id;
           info.owner = owner;
           info.account = account;
           info.start_time = ct;
           if(type == 1 || type == 2) {
               info.end_time = ct + microseconds(useconds_per_day * 15);
           } else {
               info.end_time = ct + microseconds(useconds_per_day * 30);
           }
           info.block_height = block_height;
           info.type = type;
           info.is_satisfy = false;
           info.status = 0;
           info.total_yeas     = 0;
           info.total_nays     = 0;
       });

   }

   // 抵押EON成为governance node, 可以发起提案
   void system_contract::staketognode( const name owner ) {
       require_auth( owner );
       const auto ct = current_time_point();

       auto prod3 = _producers3.find( owner.value );
       check(prod3 == _producers3.end(), "account already in _producers3");

       INLINE_ACTION_SENDER(eosio::token, transfer)(
          token_account, { {owner, active_permission} },
          { owner, bpstk_account, asset(10000, core_symbol()), "stake 1.0000 EON to governance node" }
       );



       prod3 = _producers3.emplace( owner, [&]( producer_info3& info  ) {
            info.owner          = owner;
            info.bp_staked      = 10000;
            info.stake_time     = ct;
            info.total_yeas     = 0;
            info.total_nays     = 0;
            info.is_bp          = false;
            info.status         = 0;
       });
   }
   // 不增发
   void system_contract::claimrewards( const name owner ) {
      require_auth( owner );

      const auto& prod = _producers.get( owner.value );
      check( prod.active(), "producer does not have an active key" );

      check( _gstate.total_activated_stake >= min_activated_stake,
                    "cannot claim rewards until the chain is activated (at least 15% of all tokens participate in voting)" );

      const auto ct = current_time_point();

      check( ct - prod.last_claim_time > microseconds(useconds_per_day), "already claimed rewards within past day" );

      const asset token_supply   = eosio::token::get_supply(token_account, core_symbol().code() );
      const auto usecs_since_last_fill = (ct - _gstate.last_pervote_bucket_fill).count();


      auto prod2 = _producers2.find( owner.value );

      /// New metric to be used in pervote pay calculation. Instead of vote weight ratio, we combine vote weight and
      /// time duration the vote weight has been held into one metric.
      const auto last_claim_plus_3days = prod.last_claim_time + microseconds(3 * useconds_per_day);

      bool crossed_threshold       = (last_claim_plus_3days <= ct);
      bool updated_after_threshold = true;
      if ( prod2 != _producers2.end() ) {
         updated_after_threshold = (last_claim_plus_3days <= prod2->last_votepay_share_update);
      } else {
         prod2 = _producers2.emplace( owner, [&]( producer_info2& info  ) {
            info.owner                     = owner;
            info.last_votepay_share_update = ct;
         });
      }

      // Note: updated_after_threshold implies cross_threshold (except if claiming rewards when the producers2 table row did not exist).
      // The exception leads to updated_after_threshold to be treated as true regardless of whether the threshold was crossed.
      // This is okay because in this case the producer will not get paid anything either way.
      // In fact it is desired behavior because the producers votes need to be counted in the global total_producer_votepay_share for the first time.

      int64_t producer_per_block_pay = 0;
      if( _gstate.total_unpaid_blocks > 0 ) {
         producer_per_block_pay = _gstate.reward_pre_block * prod.unpaid_blocks;
      }

      // _gstate.pervote_bucket      -= producer_per_vote_pay;
      _gstate.perblock_bucket     -= producer_per_block_pay;
      _gstate.total_unpaid_blocks -= prod.unpaid_blocks;

      // update_total_votepay_share( ct, -new_votepay_share, (updated_after_threshold ? prod.total_votes : 0.0) );

      _producers.modify( prod, same_payer, [&](auto& p) {
         p.last_claim_time = ct;
         p.unpaid_blocks   = 0;
      });

      if( producer_per_block_pay > 0 ) {
         INLINE_ACTION_SENDER(eosio::token, transfer)(
            token_account, { {blkpay_account, active_permission}, {owner, active_permission} },
            { blkpay_account, owner, asset(producer_per_block_pay, core_symbol()), std::string("producer block pay") }
         );
      }
   }

} //namespace eosiosystem
