/*
 * Shared Gold Module for AzerothCore with Playerbots
 *
 * One wallet: the character you play. Two cases are redirected to it:
 *  1. A playerbot loots gold -> it goes to the bot's master instead.
 *  2. You loot gold while grouped ONLY with your own bots -> you get the whole
 *     amount instead of the core's equal split (which would strand the bots'
 *     shares in their own wallets). Groups that contain another human, or a
 *     bot owned by someone else, keep the normal split.
 * Money is credited exactly once and loot->gold is zeroed, so the core's own
 * payout/split that runs afterwards pays out 0 - nothing is counted twice.
 *  3. A bot's vendor-sale proceeds go to its master (SharedGold.BotSales).
 *  4. A bot's quest-reward money goes to its master (SharedGold.BotQuestMoney).
 *     Both are done in OnPlayerMoneyChanged, which fires inside Player::ModifyMoney BEFORE the
 *     bot is credited, so the amount is simply set to 0 there (the bot never holds it) and paid to
 *     the master instead. To avoid touching unrelated income (trade from the master, guild-create
 *     funding, bounties, mail, ...) the hook only acts on money that follows a marker set by an
 *     earlier hook in the very same call (OnPlayerCanSellItem / OnPlayerQuestComputeXP).
 */

#include "Config.h"
#include "Player.h"
#include "Group.h"
#include "GroupReference.h"
#include "ScriptMgr.h"
#include "Chat.h"
#include "GameTime.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"

#include <sstream>

static bool SharedGoldEnabled = true;
static bool SharedGoldAnnounce = true;
static bool SharedGoldGroupAll = true;
static bool SharedGoldBotSales = true;
static bool SharedGoldBotQuestMoney = true;

// Markers for "the very next positive ModifyMoney of this player is a sale / a quest reward".
// Both are set and consumed inside one call stack (HandleSellItemOpcode / Player::RewardQuest), so they are
// thread_local: worlds with several map-update threads must not see each other's markers.
static thread_local ObjectGuid PendingSaleGuid;
static thread_local uint32 PendingSaleMax = 0;    // upper bound of the sale price, sanity check
static thread_local int64 PendingSaleTickMs = 0;  // world tick the marker was set in (drops stale markers)
static thread_local ObjectGuid PendingQuestGuid;
static thread_local bool RedirectingMoney = false; // re-entrancy guard while crediting the master

static int64 SharedGoldNowMs()
{
    return GameTime::GetGameTimeMS().count();
}

static std::string FormatMoney(uint32 money)
{
    uint32 const gold = money / 10000;
    uint32 const silver = (money % 10000) / 100;
    uint32 const copper = money % 100;

    std::ostringstream out;
    if (gold)
        out << " " << gold << " gold";
    if (silver)
        out << " " << silver << " silver";
    if (copper || (!gold && !silver))
        out << " " << copper << " copper";
    return out.str();
}

// True when every other member of the player's group is a bot mastered by this player
// (and there is at least one), i.e. the group is just "me and my bots".
static bool IsGroupOnlyMyBots(Player* player)
{
    Group* group = player->GetGroup();
    if (!group)
        return false;

    uint32 myBots = 0;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        Player* member = itr->GetSource();
        if (!member || member == player)
            continue;

        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI || memberAI->GetMaster() != player)
            return false; // another human, or someone else's bot -> keep the normal split

        ++myBots;
    }

    return myBots > 0;
}

// The real player a bot's income should be paid to, or nullptr when the bot has no such master
// (masterless random bot, master is the bot itself, master is another bot, master not in world).
static Player* GetIncomeMaster(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return nullptr;

    Player* master = botAI->GetMaster();
    if (!master || master == bot || !master->IsInWorld() || !master->GetSession() || GET_PLAYERBOT_AI(master))
        return nullptr;

    return master;
}

class SharedGoldPlayerScript : public PlayerScript
{
public:
    SharedGoldPlayerScript() : PlayerScript("SharedGoldPlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        if (SharedGoldEnabled && SharedGoldAnnounce)
        {
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the |cff4CFF00Shared Gold|r module. Bot loot goes to you!");
        }
    }

    // Marker: a bot is about to vendor an item. The core credits the sale price with ModifyMoney later
    // in the same HandleSellItemOpcode call (mod-playerbots SellAction restores the bot's old money
    // afterwards when the "gold" cheat is on, which is why the proceeds vanished before).
    bool OnPlayerCanSellItem(Player* player, Item* item, Creature* /*creature*/) override
    {
        PendingSaleGuid = ObjectGuid::Empty;

        if (SharedGoldEnabled && SharedGoldBotSales && item && GetIncomeMaster(player))
        {
            if (ItemTemplate const* proto = item->GetTemplate())
            {
                if (proto->SellPrice)
                {
                    PendingSaleGuid = player->GetGUID();
                    PendingSaleMax = proto->SellPrice * item->GetCount();
                    PendingSaleTickMs = SharedGoldNowMs();
                }
            }
        }

        return true;
    }

    // Marker: a bot's quest is being rewarded. Fires in Player::RewardQuest right before the quest money
    // is credited; OnPlayerCompleteQuest (last statement of RewardQuest) clears it again.
    void OnPlayerQuestComputeXP(Player* player, Quest const* /*quest*/, uint32& /*xpValue*/) override
    {
        PendingQuestGuid = ObjectGuid::Empty;

        if (SharedGoldEnabled && SharedGoldBotQuestMoney && GetIncomeMaster(player))
            PendingQuestGuid = player->GetGUID();
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* /*quest*/) override
    {
        if (PendingQuestGuid && PendingQuestGuid == player->GetGUID())
            PendingQuestGuid = ObjectGuid::Empty;
    }

    // Fires inside Player::ModifyMoney before the money is applied; `amount` may be changed.
    // Setting it to 0 means the bot is credited nothing, so the master gets it instead - once.
    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        if (amount <= 0 || RedirectingMoney || (!PendingSaleGuid && !PendingQuestGuid))
            return;

        ObjectGuid const guid = player->GetGUID();
        bool isQuest = false;

        if (PendingQuestGuid && guid == PendingQuestGuid)
        {
            isQuest = true;
            PendingQuestGuid = ObjectGuid::Empty;   // consume: only the first credit belongs to the quest reward
        }
        else if (PendingSaleGuid && guid == PendingSaleGuid)
        {
            bool const valid = uint32(amount) <= PendingSaleMax && PendingSaleTickMs == SharedGoldNowMs();
            PendingSaleGuid = ObjectGuid::Empty;    // consume
            if (!valid)
                return;
        }
        else
            return;

        if ((isQuest && !SharedGoldBotQuestMoney) || (!isQuest && !SharedGoldBotSales) || !SharedGoldEnabled)
            return;

        Player* master = GetIncomeMaster(player);
        if (!master)
            return;

        uint32 const gold = uint32(amount);

        RedirectingMoney = true;
        bool const paid = master->ModifyMoney(int32(gold), false);
        RedirectingMoney = false;

        // If the master cannot take it (gold cap), leave it with the bot rather than destroying it.
        if (!paid)
            return;

        amount = 0;

        // Sales are silent (one message per item would flood the chat); quest rewards are worth a line.
        if (isQuest)
        {
            std::ostringstream msg;
            msg << "|cff4CFF00[Shared Gold]|r " << player->GetName() << " earned" << FormatMoney(gold) << " from a quest for you.";
            ChatHandler(master->GetSession()).SendSysMessage(msg.str().c_str());
        }
    }

    // Called before money is looted - we can modify the loot here
    void OnPlayerBeforeLootMoney(Player* player, Loot* loot) override
    {
        if (!SharedGoldEnabled || !loot || loot->gold == 0)
            return;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(player);

        // Case 1: a bot loots -> pay its master.
        if (botAI)
        {
            Player* master = botAI->GetMaster();
            if (!master || master == player)
                return;

            uint32 const gold = loot->gold;
            master->ModifyMoney(gold);

            std::ostringstream msg;
            msg << "|cff4CFF00[Shared Gold]|r " << player->GetName() << " looted" << FormatMoney(gold) << " for you.";
            ChatHandler(master->GetSession()).SendSysMessage(msg.str().c_str());

            // Zero it so neither the bot nor the core's group split pays it out again.
            loot->gold = 0;
            return;
        }

        // Case 2: the real player loots while grouped only with his own bots -> keep it all.
        if (SharedGoldGroupAll && IsGroupOnlyMyBots(player))
        {
            uint32 const gold = loot->gold;
            player->ModifyMoney(gold);

            // The core's own "you loot X" notice will show 0 once loot->gold is zeroed, so say what we did.
            std::ostringstream msg;
            msg << "|cff4CFF00[Shared Gold]|r You looted" << FormatMoney(gold) << " (whole share, no split with your bots).";
            ChatHandler(player->GetSession()).SendSysMessage(msg.str().c_str());

            loot->gold = 0;
        }
    }
};

class SharedGoldWorldScript : public WorldScript
{
public:
    SharedGoldWorldScript() : WorldScript("SharedGoldWorldScript") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        SharedGoldEnabled = sConfigMgr->GetOption<bool>("SharedGold.Enable", true);
        SharedGoldAnnounce = sConfigMgr->GetOption<bool>("SharedGold.Announce", true);
        SharedGoldGroupAll = sConfigMgr->GetOption<bool>("SharedGold.GroupAll", true);
        SharedGoldBotSales = sConfigMgr->GetOption<bool>("SharedGold.BotSales", true);
        SharedGoldBotQuestMoney = sConfigMgr->GetOption<bool>("SharedGold.BotQuestMoney", true);
    }
};

void AddSharedGoldScripts()
{
    new SharedGoldPlayerScript();
    new SharedGoldWorldScript();
}
