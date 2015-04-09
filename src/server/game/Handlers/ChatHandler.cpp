/*
 * Copyright (C) 2015 TheSatriaCore <http://www.TheSatria.Com>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Common.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "GuildMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "DatabaseEnv.h"
#include "CellImpl.h"
#include "Chat.h"
#include "ChannelMgr.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
#include "Language.h"
#include "Log.h"
#include "Opcodes.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "Util.h"
#include "ScriptMgr.h"
#include "AccountMgr.h"

void WorldSession::HandleMessagechatOpcode(WorldPacket& recvData)
{
    uint32 type;
    uint32 lang;

    recvData >> type;
    recvData >> lang;

    if (type >= MAX_CHAT_MSG_TYPE)
    {
        sLog->outError(LOG_FILTER_NETWORKIO, "CHAT: Wrong message type received: %u", type);
        recvData.rfinish();
        return;
    }

    Player* sender = GetPlayer();

    //sLog->outDebug("CHAT: packet received. type %u, lang %u", type, lang);

    // prevent talking at unknown language (cheating)
    LanguageDesc const* langDesc = GetLanguageDescByID(lang);
    if (!langDesc)
    {
        SendNotification(LANG_UNKNOWN_LANGUAGE);
        recvData.rfinish();
        return;
    }
    if (langDesc->skill_id != 0 && !sender->HasSkill(langDesc->skill_id))
    {
        // also check SPELL_AURA_COMPREHEND_LANGUAGE (client offers option to speak in that language)
        Unit::AuraEffectList const& langAuras = sender->GetAuraEffectsByType(SPELL_AURA_COMPREHEND_LANGUAGE);
        bool foundAura = false;
        for (Unit::AuraEffectList::const_iterator i = langAuras.begin(); i != langAuras.end(); ++i)
        {
            if ((*i)->GetMiscValue() == int32(lang))
            {
                foundAura = true;
                break;
            }
        }
        if (!foundAura)
        {
            SendNotification(LANG_NOT_LEARNED_LANGUAGE);
            recvData.rfinish();
            return;
        }
    }

    if (lang == LANG_ADDON)
    {
        // LANG_ADDON is only valid for the following message types
        switch (type)
        {
            case CHAT_MSG_PARTY:
            case CHAT_MSG_RAID:
            case CHAT_MSG_GUILD:
            case CHAT_MSG_BATTLEGROUND:
            case CHAT_MSG_WHISPER:
                if (sWorld->getBoolConfig(CONFIG_CHATLOG_ADDON))
                {
                    std::string msg = "";
                    recvData >> msg;

                    if (msg.empty())
                        return;

                    sScriptMgr->OnPlayerChat(sender, uint32(CHAT_MSG_ADDON), lang, msg);
                }

                // Disabled addon channel?
                if (!sWorld->getBoolConfig(CONFIG_ADDON_CHANNEL))
                    return;
                break;
            default:
                sLog->outError(LOG_FILTER_NETWORKIO, "Player %s (GUID: %u) sent a chatmessage with an invalid language/message type combination",
                                                     GetPlayer()->GetName().c_str(), GetPlayer()->GetGUIDLow());

                recvData.rfinish();
                return;
        }
    }
    // LANG_ADDON should not be changed nor be affected by flood control
    else
    {
        // send in universal language if player in .gmon mode (ignore spell effects)
        if (sender->isGameMaster())
            lang = LANG_UNIVERSAL;
        else
        {
            Unit::AuraEffectList const& ModLangAuras = sender->GetAuraEffectsByType(SPELL_AURA_MOD_LANGUAGE);
            if (!ModLangAuras.empty())
                lang = ModLangAuras.front()->GetMiscValue();
            else if (HasPermission(RBAC_PERM_TWO_SIDE_INTERACTION_CHAT))
                lang = LANG_UNIVERSAL;
            else
            {
                switch (type)
                {
                    case CHAT_MSG_PARTY:
                    case CHAT_MSG_PARTY_LEADER:
                    case CHAT_MSG_RAID:
                    case CHAT_MSG_RAID_LEADER:
                    case CHAT_MSG_RAID_WARNING:
                        // allow two side chat at group channel if two side group allowed
                        if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP))
                            lang = LANG_UNIVERSAL;
                        break;
                    case CHAT_MSG_GUILD:
                    case CHAT_MSG_OFFICER:
                        // allow two side chat at guild channel if two side guild allowed
                        if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD))
                            lang = LANG_UNIVERSAL;
                        break;
                }
            }
        }

        if (!sender->CanSpeak())
        {
            std::string timeStr = secsToTimeString(m_muteTime - time(NULL));
            SendNotification(GetTrinityString(LANG_WAIT_BEFORE_SPEAKING), timeStr.c_str());
            recvData.rfinish(); // Prevent warnings
            return;
        }

        if (type != CHAT_MSG_AFK && type != CHAT_MSG_DND)
            sender->UpdateSpeakTime();
    }

    if (sender->HasAura(1852) && type != CHAT_MSG_WHISPER)
    {
        SendNotification(GetTrinityString(LANG_GM_SILENCE), sender->GetName().c_str());
        recvData.rfinish();
        return;
    }

    std::string to, channel, msg, color;
    bool ignoreChecks = false;
    switch (type)
    {
        case CHAT_MSG_SAY:
        case CHAT_MSG_EMOTE:
        case CHAT_MSG_YELL:
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
        case CHAT_MSG_GUILD:
        case CHAT_MSG_OFFICER:
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
        case CHAT_MSG_RAID_WARNING:
        case CHAT_MSG_BATTLEGROUND:
        case CHAT_MSG_BATTLEGROUND_LEADER:
            recvData >> msg;
            break;
        case CHAT_MSG_WHISPER:
            recvData >> to;
            recvData >> msg;
            break;
        case CHAT_MSG_CHANNEL:
            recvData >> channel;
            recvData >> msg;
            break;
        case CHAT_MSG_AFK:
        case CHAT_MSG_DND:
            recvData >> msg;
            ignoreChecks = true;
            break;
    }

    if (!ignoreChecks)
    {
        if (msg.empty())
            return;

        if (ChatHandler(this).ParseCommands(msg.c_str()))
            return;

        if (lang != LANG_ADDON)
        {
            // Strip invisible characters for non-addon messages
            if (sWorld->getBoolConfig(CONFIG_CHAT_FAKE_MESSAGE_PREVENTING))
                stripLineInvisibleChars(msg);

            if (sWorld->getIntConfig(CONFIG_CHAT_STRICT_LINK_CHECKING_SEVERITY) && !ChatHandler(this).isValidChatMessage(msg.c_str()))
            {
                sLog->outError(LOG_FILTER_NETWORKIO, "Player %s (GUID: %u) sent a chatmessage with an invalid link: %s", GetPlayer()->GetName().c_str(),
                    GetPlayer()->GetGUIDLow(), msg.c_str());

                if (sWorld->getIntConfig(CONFIG_CHAT_STRICT_LINK_CHECKING_KICK))
                    KickPlayer();

                return;
            }
        }
    }

    switch (type)
    {
        case CHAT_MSG_SAY:					
        case CHAT_MSG_EMOTE:
        case CHAT_MSG_YELL:
        {
            if (sender->getLevel() < sWorld->getIntConfig(CONFIG_CHAT_SAY_LEVEL_REQ))
            {
                SendNotification(GetTrinityString(LANG_SAY_REQ), sWorld->getIntConfig(CONFIG_CHAT_SAY_LEVEL_REQ));
                return;
            }
			
			if (type == CHAT_MSG_SAY)
		{
                switch (sender->GetSession()->GetSecurity())
            {	
				
                /*case SEC_PLAYER:
                    color = "|cffFFFFFF";
                    break;*/

				case SEC_VIP:
                    color =  "|cffB6FF00";
                    break;

                case SEC_MODERATOR:
				case SEC_GAMEMASTER:
				case SEC_HEAD_GM:
				if (GetPlayer()->isGameMaster()==TRUE)
					{				
                    color =  "|cff0094FF";
					}
					{
					if (GetPlayer()->isGameMaster()==FALSE)
					color = "|cffFFFFFF";
					}
                    break;

                case SEC_DEV:
				if (GetPlayer()->isGameMaster()==TRUE)
					{				
                    color =  "|cffB200FF";
					if (GetPlayer()->isGameMaster()==FALSE)
					color = "|cffFFFFFF";
					}
                    break;

                case SEC_ADMINISTRATOR:
				case SEC_HEAD_ADMIN:
				if (GetPlayer()->isGameMaster()==TRUE)
					{
                    color =  "|cff00FFFF";
					if (GetPlayer()->isGameMaster()==FALSE)
					color = "|cffFFFFFF";
					}
                    break;
				
				case SEC_CO:
				case SEC_STAFF:
				case SEC_OWNER:
				if (GetPlayer()->isGameMaster()==TRUE)
					{
                    color =  "|cffFF0000";
					if (GetPlayer()->isGameMaster()==FALSE)
					color = "|cffFFFFFF";
					}
                    break;				
				
            }
			sender->Say(color + msg, Language(lang));
        }
		
			else if (type == CHAT_MSG_EMOTE)
                sender->TextEmote(msg);
            else if (type == CHAT_MSG_YELL)
                sender->Yell(msg, Language(lang));
		} break;
        case CHAT_MSG_WHISPER:
        {
            if (!normalizePlayerName(to))
            {
                SendPlayerNotFoundNotice(to);
                break;
            }

            Player* receiver = sObjectAccessor->FindPlayerByName(to);
            if (!receiver || (!receiver->isAcceptWhispers() && receiver->GetSession()->HasPermission(RBAC_PERM_CAN_FILTER_WHISPERS) && !receiver->IsInWhisperWhiteList(sender->GetGUID())))
            {
			// If Fake WHO List system on then show player DND
                if (sWorld->getBoolConfig(CONFIG_FAKE_WHO_LIST))
                {
                    ChatHandler(sender->GetSession()).PSendSysMessage(LANG_FAKE_NOT_DISTURB);
                }
                else
                {
                    SendPlayerNotFoundNotice(to);
                }
                 return;
             }
            if (!sender->isGameMaster() && sender->getLevel() < sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ) && !receiver->IsInWhisperWhiteList(sender->GetGUID()))
            {
                SendNotification(GetTrinityString(LANG_WHISPER_REQ), sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ));
                return;
            }

            if (GetPlayer()->GetTeam() != receiver->GetTeam() && !HasPermission(RBAC_PERM_TWO_SIDE_INTERACTION_CHAT) && !receiver->IsInWhisperWhiteList(sender->GetGUID()))
            {
                SendWrongFactionNotice();
                return;
            }

            if (GetPlayer()->HasAura(1852) && !receiver->isGameMaster())
            {
                SendNotification(GetTrinityString(LANG_GM_SILENCE), GetPlayer()->GetName().c_str());
                return;
            }

            // If player is a Gamemaster and doesn't accept whisper, we auto-whitelist every player that the Gamemaster is talking to
            // We also do that if a player is under the required level for whispers.
            if (receiver->getLevel() < sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ) ||
                (HasPermission(RBAC_PERM_CAN_FILTER_WHISPERS) && !sender->isAcceptWhispers() && !sender->IsInWhisperWhiteList(receiver->GetGUID())))
                sender->AddWhisperWhiteList(receiver->GetGUID());

            GetPlayer()->Whisper(msg, lang, receiver->GetGUID());
        } break;
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
        {
            // if player is in battleground, he cannot say to battleground members by /p
            Group* group = GetPlayer()->GetOriginalGroup();
            if (!group)
            {
                group = _player->GetGroup();
                if (!group || group->isBGGroup())
                    return;
            }

            if (type == CHAT_MSG_PARTY_LEADER && !group->IsLeader(_player->GetGUID()))
                return;

            sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, group);

            WorldPacket data;
            ChatHandler::FillMessageData(&data, this, uint8(type), lang, NULL, 0, msg.c_str(), NULL);
            group->BroadcastPacket(&data, false, group->GetMemberGroup(GetPlayer()->GetGUID()));
        } break;
        case CHAT_MSG_GUILD:
        {
            if (GetPlayer()->GetGuildId())
            {
                if (Guild* guild = sGuildMgr->GetGuildById(GetPlayer()->GetGuildId()))
                {
                    sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, guild);

                    guild->BroadcastToGuild(this, false, msg, lang == LANG_ADDON ? LANG_ADDON : LANG_UNIVERSAL);
                }
            }
        } break;
        case CHAT_MSG_OFFICER:
        {
            char message[1024];
			switch(GetPlayer()->GetSession()->GetSecurity())
			{
			case SEC_PLAYER: // normal player, non-vip	1				
					snprintf(message, 1024, "|cffFF6549World |TInterface\\PvPRankBadges\\PvPRank01:13:13|t]|cff00E21A[ Player ]|cff00E2E2[%s]:|cffFFFF00 %s", GetPlayer()->GetName().c_str(), msg.c_str());					
					break;

			case SEC_VIP: // VIP2
					snprintf(message, 1024, "|cffFF6549World |TInterface\\PvPRankBadges\\PvPRank03:13:13|t]|cff00E21A[ V.I.P ]|cff00E2E2[%s]:|cffB6FF00 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					break;
					
			case SEC_MODERATOR: // TRIAL GM3
					if (GetPlayer()->isGameMaster()==TRUE)
					{
					snprintf(message, 1024, "|cffFF6549World |TInterface\\ChatFrame\\UI-ChatIcon-Blizz.blp:0:2:0:-3|t |cff00E21A[ Trial GM ]|cff00E2E2[%s]:|cff0094FF %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}{
					if (GetPlayer()->isGameMaster()==FALSE)
					snprintf(message, 1024, "|cffFF6549World |cff00E2E2[%s]:|cffFFFF00 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}
					break;
					
			case SEC_GAMEMASTER: // GM/SGM4
					if (GetPlayer()->isGameMaster()==TRUE)
					{
					snprintf(message, 1024, "|cffFF6549World |TInterface\\ChatFrame\\UI-ChatIcon-Blizz.blp:0:2:0:-3|t |cff00E21A[ GM ]|cff00E2E2[%s]:|cff0094FF %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}{
					if (GetPlayer()->isGameMaster()==FALSE)
					snprintf(message, 1024, "|cffFF6549World |cff00E2E2[%s]:|cffFFFF00 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}
					break;
					
			 case SEC_HEAD_GM: // ADMIN5
					if (GetPlayer()->isGameMaster()==TRUE)
					{
					snprintf(message, 1024,  "|cffFF6549World |TInterface\\ChatFrame\\UI-ChatIcon-Blizz.blp:0:2:0:-3|t |cff00E21A[ Head GM ]|cff00E2E2[%s]:|cff0094FF %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}{
					if (GetPlayer()->isGameMaster()==FALSE)
					snprintf(message, 1024, "|cffFF6549World |cff00E2E2[%s]:|cffFFFF00 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}
					break;
				
			case SEC_DEV: // DEV6
					if (GetPlayer()->isGameMaster()==TRUE)
					{
					snprintf(message, 1024,  "|cffFF6549World |TInterface\\ChatFrame\\UI-ChatIcon-Blizz.blp:0:2:0:-3|t |cff00E21A[ Developer ]|cff00E2E2[%s]:|cffB200FF %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}{
					if (GetPlayer()->isGameMaster()==FALSE)
					snprintf(message, 1024, "|cffFF6549World |cff00E2E2[%s]:|cffFFFF00 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}
					break;
			
						
			case SEC_ADMINISTRATOR: // ADMIN8
					if (GetPlayer()->isGameMaster()==TRUE)
					{
					snprintf(message, 1024,  "|cffFF6549World |TInterface\\ChatFrame\\UI-ChatIcon-Blizz.blp:0:2:0:-3|t |cff00E21A[ Admin ]|cff00E2E2[%s]:|cff00FFFF %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}{
					if (GetPlayer()->isGameMaster()==FALSE)
					snprintf(message, 1024, "|cffFF6549World |cff00E2E2[%s]:|cffFFFF00 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}
					break;
					
			case SEC_HEAD_ADMIN: // ADMIN9
					if (GetPlayer()->isGameMaster()==TRUE)
					{
					snprintf(message, 1024,  "|cffFF6549World |TInterface\\ChatFrame\\UI-ChatIcon-Blizz.blp:0:2:0:-3|t |cff00E21A[ Head Admin ]|cff00E2E2[%s]:|cff00FFFF %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}{
					if (GetPlayer()->isGameMaster()==FALSE)
					snprintf(message, 1024, "|cffFF6549World |cff00E2E2[%s]:|cffFFFF00 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}
					break;

			case SEC_STAFF: // ADMIN10
					if (GetPlayer()->isGameMaster()==TRUE)
					{
					snprintf(message, 1024,  "|cffFF6549World |TInterface\\ChatFrame\\UI-ChatIcon-Blizz.blp:0:2:0:-3|t |cff00E21A[ Head Of Staffs ]|cff00E2E2[%s]:|cffFF0000 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}{
					if (GetPlayer()->isGameMaster()==FALSE)
					snprintf(message, 1024, "|cffFF6549World |cff00E2E2[%s]:|cffFFFF00 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}
					break;
			
			case SEC_CO: // ADMIN10
					if (GetPlayer()->isGameMaster()==TRUE)
					{
					snprintf(message, 1024,  "|cffFF6549World |TInterface\\ChatFrame\\UI-ChatIcon-Blizz.blp:0:2:0:-3|t |cff00E21A[ Co-Owner ]|cff00E2E2[%s]:|cffFF0000 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}{
					if (GetPlayer()->isGameMaster()==FALSE)
					snprintf(message, 1024, "|cffFF6549World |cff00E2E2[%s]:|cffFFFF00 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}
					break;			
					
			case SEC_OWNER: // ADMIN11
					if (GetPlayer()->isGameMaster()==TRUE)
					{
					snprintf(message, 1024,  "|cffFF6549World |TInterface\\ChatFrame\\UI-ChatIcon-Blizz.blp:0:2:0:-3|t |cff00E21A[ Owner ]|cff00E2E2[%s]:|cffFF0000 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}{
					if (GetPlayer()->isGameMaster()==FALSE)
					snprintf(message, 1024, "|cffFF6549World |cff00E2E2[%s]:|cffFFFF00 %s", GetPlayer()->GetName().c_str(), msg.c_str());
					}
					break;
										
			}
			sWorld->SendGlobalText(message, NULL);
        } break;
		
        case CHAT_MSG_RAID:
        {
            // if player is in battleground, he cannot say to battleground members by /ra
            Group* group = GetPlayer()->GetOriginalGroup();
            if (!group)
            {
                group = GetPlayer()->GetGroup();
                if (!group || group->isBGGroup() || !group->isRaidGroup())
                    return;
            }

            sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, group);

            WorldPacket data;
            ChatHandler::FillMessageData(&data, this, CHAT_MSG_RAID, lang, "", 0, msg.c_str(), NULL);
            group->BroadcastPacket(&data, false);
        } break;
        case CHAT_MSG_RAID_LEADER:
        {
            // if player is in battleground, he cannot say to battleground members by /ra
            Group* group = GetPlayer()->GetOriginalGroup();
            if (!group)
            {
                group = GetPlayer()->GetGroup();
                if (!group || group->isBGGroup() || !group->isRaidGroup() || !group->IsLeader(_player->GetGUID()))
                    return;
            }

            sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, group);

            WorldPacket data;
            ChatHandler::FillMessageData(&data, this, CHAT_MSG_RAID_LEADER, lang, "", 0, msg.c_str(), NULL);
            group->BroadcastPacket(&data, false);
        } break;
        case CHAT_MSG_RAID_WARNING:
        {
            Group* group = GetPlayer()->GetGroup();
            if (!group || !group->isRaidGroup() || !(group->IsLeader(GetPlayer()->GetGUID()) || group->IsAssistant(GetPlayer()->GetGUID())) || group->isBGGroup())
                return;

            sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, group);

            WorldPacket data;
            //in battleground, raid warning is sent only to players in battleground - code is ok
            ChatHandler::FillMessageData(&data, this, CHAT_MSG_RAID_WARNING, lang, "", 0, msg.c_str(), NULL);
            group->BroadcastPacket(&data, false);
        } break;
        case CHAT_MSG_BATTLEGROUND:
        {
            //battleground raid is always in Player->GetGroup(), never in GetOriginalGroup()
            Group* group = GetPlayer()->GetGroup();
            if (!group || !group->isBGGroup())
                return;

            sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, group);

            WorldPacket data;
            ChatHandler::FillMessageData(&data, this, CHAT_MSG_BATTLEGROUND, lang, "", 0, msg.c_str(), NULL);
            group->BroadcastPacket(&data, false);
        } break;
        case CHAT_MSG_BATTLEGROUND_LEADER:
        {
            // battleground raid is always in Player->GetGroup(), never in GetOriginalGroup()
            Group* group = GetPlayer()->GetGroup();
            if (!group || !group->isBGGroup() || !group->IsLeader(GetPlayer()->GetGUID()))
                return;

            sScriptMgr->OnPlayerChat(GetPlayer(), type, lang, msg, group);

            WorldPacket data;
            ChatHandler::FillMessageData(&data, this, CHAT_MSG_BATTLEGROUND_LEADER, lang, "", 0, msg.c_str(), NULL);
            group->BroadcastPacket(&data, false);
        } break;
        case CHAT_MSG_CHANNEL:
        {
            if (!HasPermission(RBAC_PERM_SKIP_CHECK_CHAT_CHANNEL_REQ))
            {
                if (_player->getLevel() < sWorld->getIntConfig(CONFIG_CHAT_CHANNEL_LEVEL_REQ))
                {
                    SendNotification(GetTrinityString(LANG_CHANNEL_REQ), sWorld->getIntConfig(CONFIG_CHAT_CHANNEL_LEVEL_REQ));
                    return;
                }
            }

            if (ChannelMgr* cMgr = ChannelMgr::forTeam(_player->GetTeam()))
            {
                if (Channel* chn = cMgr->GetChannel(channel, _player))
                {
                    sScriptMgr->OnPlayerChat(_player, type, lang, msg, chn);
                    chn->Say(_player->GetGUID(), msg.c_str(), lang);
                }
            }
        } break;
        case CHAT_MSG_AFK:
        {
            if (!_player->isInCombat())
            {
                if (_player->isAFK())                       // Already AFK
                {
                    if (msg.empty())
                        _player->ToggleAFK();               // Remove AFK
                    else
                        _player->autoReplyMsg = msg;        // Update message
                }
                else                                        // New AFK mode
                {
                    _player->autoReplyMsg = msg.empty() ? GetTrinityString(LANG_PLAYER_AFK_DEFAULT) : msg;

                    if (_player->isDND())
                        _player->ToggleDND();

                    _player->ToggleAFK();
                }

                sScriptMgr->OnPlayerChat(_player, type, lang, msg);
            }
            break;
        }
        case CHAT_MSG_DND:
        {
            if (_player->isDND())                           // Already DND
            {
                if (msg.empty())
                    _player->ToggleDND();                   // Remove DND
                else
                    _player->autoReplyMsg = msg;            // Update message
            }
            else                                            // New DND mode
            {
                _player->autoReplyMsg = msg.empty() ? GetTrinityString(LANG_PLAYER_DND_DEFAULT) : msg;

                if (_player->isAFK())
                    _player->ToggleAFK();

                _player->ToggleDND();
            }

            sScriptMgr->OnPlayerChat(_player, type, lang, msg);
            break;
        }
        default:
            sLog->outError(LOG_FILTER_NETWORKIO, "CHAT: unknown message type %u, lang: %u", type, lang);
            break;
    }
}

void WorldSession::HandleEmoteOpcode(WorldPacket& recvData)
{
    if (!GetPlayer()->IsAlive() || GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        return;

    uint32 emote;
    recvData >> emote;
    sScriptMgr->OnPlayerEmote(GetPlayer(), emote);
    GetPlayer()->HandleEmoteCommand(emote);
}

namespace Trinity
{
    class EmoteChatBuilder
    {
        public:
            EmoteChatBuilder(Player const& player, uint32 text_emote, uint32 emote_num, Unit const* target)
                : i_player(player), i_text_emote(text_emote), i_emote_num(emote_num), i_target(target) {}

            void operator()(WorldPacket& data, LocaleConstant loc_idx)
            {
                std::string const name(i_target ? i_target->GetNameForLocaleIdx(loc_idx) : "");
                uint32 namlen = name.size();

                data.Initialize(SMSG_TEXT_EMOTE, 20 + namlen);
                data << i_player.GetGUID();
                data << uint32(i_text_emote);
                data << uint32(i_emote_num);
                data << uint32(namlen);
                if (namlen > 1)
                    data << name;
                else
                    data << uint8(0x00);
            }

        private:
            Player const& i_player;
            uint32        i_text_emote;
            uint32        i_emote_num;
            Unit const*   i_target;
    };
}                                                           // namespace Trinity

void WorldSession::HandleTextEmoteOpcode(WorldPacket& recvData)
{
    if (!GetPlayer()->IsAlive())
        return;

    if (!GetPlayer()->CanSpeak())
    {
        std::string timeStr = secsToTimeString(m_muteTime - time(NULL));
        SendNotification(GetTrinityString(LANG_WAIT_BEFORE_SPEAKING), timeStr.c_str());
        return;
    }

    uint32 text_emote, emoteNum;
    uint64 guid;

    recvData >> text_emote;
    recvData >> emoteNum;
    recvData >> guid;

    sScriptMgr->OnPlayerTextEmote(GetPlayer(), text_emote, emoteNum, guid);

    EmotesTextEntry const* em = sEmotesTextStore.LookupEntry(text_emote);
    if (!em)
        return;

    uint32 emote_anim = em->textid;

    switch (emote_anim)
    {
        case EMOTE_STATE_SLEEP:
        case EMOTE_STATE_SIT:
        case EMOTE_STATE_KNEEL:
        case EMOTE_ONESHOT_NONE:
            break;
        default:
            // Only allow text-emotes for "dead" entities (feign death included)
            if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
                break;
            GetPlayer()->HandleEmoteCommand(emote_anim);
            break;
    }

    Unit* unit = ObjectAccessor::GetUnit(*_player, guid);

    CellCoord p = Trinity::ComputeCellCoord(GetPlayer()->GetPositionX(), GetPlayer()->GetPositionY());

    Cell cell(p);
    cell.SetNoCreate();

    Trinity::EmoteChatBuilder emote_builder(*GetPlayer(), text_emote, emoteNum, unit);
    Trinity::LocalizedPacketDo<Trinity::EmoteChatBuilder > emote_do(emote_builder);
    Trinity::PlayerDistWorker<Trinity::LocalizedPacketDo<Trinity::EmoteChatBuilder > > emote_worker(GetPlayer(), sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE), emote_do);
    TypeContainerVisitor<Trinity::PlayerDistWorker<Trinity::LocalizedPacketDo<Trinity::EmoteChatBuilder> >, WorldTypeMapContainer> message(emote_worker);
    cell.Visit(p, message, *GetPlayer()->GetMap(), *GetPlayer(), sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE));

    GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_DO_EMOTE, text_emote, 0, unit);

    //Send scripted event call
    if (unit && unit->GetTypeId() == TYPEID_UNIT && ((Creature*)unit)->AI())
        ((Creature*)unit)->AI()->ReceiveEmote(GetPlayer(), text_emote);
}

void WorldSession::HandleChatIgnoredOpcode(WorldPacket& recvData)
{
    uint64 iguid;
    uint8 unk;
    //sLog->outDebug(LOG_FILTER_PACKETIO, "WORLD: Received CMSG_CHAT_IGNORED");

    recvData >> iguid;
    recvData >> unk;                                       // probably related to spam reporting

    Player* player = ObjectAccessor::FindPlayer(iguid);
    if (!player || !player->GetSession())
        return;

    WorldPacket data;
    ChatHandler::FillMessageData(&data, this, CHAT_MSG_IGNORED, LANG_UNIVERSAL, NULL, GetPlayer()->GetGUID(), GetPlayer()->GetName().c_str(), NULL);
    player->GetSession()->SendPacket(&data);
}

void WorldSession::HandleChannelDeclineInvite(WorldPacket &recvPacket)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "Opcode %u", recvPacket.GetOpcode());
}

void WorldSession::SendPlayerNotFoundNotice(std::string const& name)
{
    WorldPacket data(SMSG_CHAT_PLAYER_NOT_FOUND, name.size()+1);
    data << name;
    SendPacket(&data);
}

void WorldSession::SendPlayerAmbiguousNotice(std::string const& name)
{
    WorldPacket data(SMSG_CHAT_PLAYER_AMBIGUOUS, name.size()+1);
    data << name;
    SendPacket(&data);
}

void WorldSession::SendWrongFactionNotice()
{
    WorldPacket data(SMSG_CHAT_WRONG_FACTION, 0);
    SendPacket(&data);
}

void WorldSession::SendChatRestrictedNotice(ChatRestrictionType restriction)
{
    WorldPacket data(SMSG_CHAT_RESTRICTED, 1);
    data << uint8(restriction);
    SendPacket(&data);
}
