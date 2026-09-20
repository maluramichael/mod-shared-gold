# mod-shared-gold

One wallet for AzerothCore + mod-playerbots: the character you play holds all the gold, your bots never keep any.
Meant for a solo player with his own bot party and `AiPlayerbot.BotCheats` containing `gold` (bots buy, repair and
train for free).

## Behavior

A bot's *master* is `PlayerbotAI::GetMaster()`. Everything below only applies to bots whose master is a real
(non-bot) player in the world and is not the bot itself; masterless random bots are never touched.

| Source | What happens | Toggle |
|--------|--------------|--------|
| Bot loots gold | Paid to the master, `loot->gold` zeroed so nothing is paid twice | `SharedGold.Enable` |
| You loot while grouped **only** with your own bots | You get the whole amount instead of the core's equal split | `SharedGold.GroupAll` |
| Bot vendors an item | Sale price paid to the master (silent, one chat line per item would flood) | `SharedGold.BotSales` |
| Bot turns in a quest with a money reward | Reward paid to the master, one chat line | `SharedGold.BotQuestMoney` |

Groups with another human, or with somebody else's bot, keep the normal split.

### How the sale / quest redirect works

`OnPlayerMoneyChanged` fires inside `Player::ModifyMoney` before the amount is applied and may change it. For a
bot with a master the module sets the amount to `0` (the bot is credited nothing) and calls `ModifyMoney` on the
master instead. Because that hook fires for *every* money change, it only acts on money that follows a marker set
by an earlier hook of the same call stack:

- `OnPlayerCanSellItem` marks "the next credit is a vendor sale" (`HandleSellItemOpcode`); it is only accepted in
  the same world tick and if it does not exceed the item's sell price.
- `OnPlayerQuestComputeXP` marks "the next credit is quest money" (`Player::RewardQuest`); `OnPlayerCompleteQuest`
  clears it. Quest rewards that *cost* money (negative amounts) are ignored.

Markers are `thread_local`, consumed on first use, and a re-entrancy guard keeps the master's own credit from
re-triggering the hook. If the master cannot take the money (gold cap) it stays with the bot instead of vanishing.

mod-playerbots' `SellAction` restores the bot's previous money after a sale when the `gold` cheat is on; since the
bot was never credited that restore is a no-op.

## Configuration

`conf/mod_shared_gold.conf.dist` (copy to `configs/modules/mod_shared_gold.conf`); all default to `1`.

- `SharedGold.Enable` - master switch
- `SharedGold.Announce` - login message
- `SharedGold.GroupAll` - full loot gold when grouped only with your own bots
- `SharedGold.BotSales` - route bot vendor-sale proceeds to the master
- `SharedGold.BotQuestMoney` - route bot quest-reward money to the master

## Limitations

- Deliberately **not** routed, because they can't be told apart from legitimate transfers: gold you trade to a bot,
  mail money, guild-create funding by playerbots, AH proceeds, and kill bounties from mod-money-for-kills. A blanket
  "redirect all bot income" would swallow those. Bounties would need a marker hook fired before mod-money-for-kills
  pays (script order is not guaranteed) or a small patch in that module.
- Sale and quest redirects apply whether or not the bot has the `gold` cheat; without it bots would have no income
  of their own.
- Depends on mod-playerbots (`Playerbots.h`, `PlayerbotAI::GetMaster`) and on the core hooks `OnPlayerMoneyChanged`,
  `OnPlayerCanSellItem`, `OnPlayerQuestComputeXP`, `OnPlayerCompleteQuest`, `OnPlayerBeforeLootMoney`.
