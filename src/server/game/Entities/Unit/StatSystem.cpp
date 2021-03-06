/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
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

#include "Unit.h"
#include "Player.h"
#include "Pet.h"
#include "Creature.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"

inline bool _ModifyUInt32(bool apply, uint32& baseValue, int32& amount)
{
    // If amount is negative, change sign and value of apply.
    if (amount < 0)
    {
        apply = !apply;
        amount = -amount;
    }
    if (apply)
        baseValue += amount;
    else
    {
        // Make sure we do not get uint32 overflow.
        if (amount > int32(baseValue))
            amount = baseValue;
        baseValue -= amount;
    }
    return apply;
}

/*#######################################
########                         ########
########   PLAYERS STAT SYSTEM   ########
########                         ########
#######################################*/

bool Player::UpdateStats(Stats stat)
{
    if (stat > STAT_SPIRIT)
        return false;

    // value = ((base_value * base_pct) + total_value) * total_pct
    float value = 0.0f;

    if (GetStatsModifier()->IsActive())
    {
        UnitMods unitMod = UnitMods(UNIT_MOD_STAT_START + stat);

        if (m_auraModifiersGroup[unitMod][TOTAL_PCT] <= 0.0f)
            value = 0.0f;
        else
        {
            value = m_auraModifiersGroup[unitMod][BASE_VALUE];
            GetStatsModifier()->ApplyModifier(value);
            value += GetCreateStat(stat);
            value *= m_auraModifiersGroup[unitMod][BASE_PCT];
            value += m_auraModifiersGroup[unitMod][TOTAL_VALUE];
            value *= m_auraModifiersGroup[unitMod][TOTAL_PCT];
        }
    }
    else
        value = GetTotalStatValue(stat);

    SetStat(stat, int32(value));

    // <TehKevin> Pets shouldn't get an update -> They take a snapshot at summon
    /*if (stat == STAT_STAMINA || stat == STAT_INTELLECT || stat == STAT_STRENGTH)
    {
        Pet* pet = GetPet();
        if (pet)
            pet->UpdateStats(stat);
    }*/

    switch (stat)
    {
        case STAT_STRENGTH:
            UpdateShieldBlockValue();
            break;
        case STAT_AGILITY:
            UpdateArmor();
            UpdateAllCritPercentages();
            UpdateDodgePercentage();
            break;
        case STAT_STAMINA:   UpdateMaxHealth(); break;
        case STAT_INTELLECT:
            UpdateMaxPower(POWER_MANA);
            UpdateAllSpellCritChances();
            UpdateArmor();                                  //SPELL_AURA_MOD_RESISTANCE_OF_INTELLECT_PERCENT, only armor currently
            break;

        case STAT_SPIRIT:
            break;

        default:
            break;
    }

    if (stat == STAT_STRENGTH)
    {
        UpdateAttackPowerAndDamage(false);
        if (HasAuraTypeWithMiscvalue(SPELL_AURA_MOD_RANGED_ATTACK_POWER_OF_STAT_PERCENT, stat))
            UpdateAttackPowerAndDamage(true);
    }
    else if (stat == STAT_AGILITY)
    {
        UpdateAttackPowerAndDamage(false);
        UpdateAttackPowerAndDamage(true);
    }
    else
    {
        // Need update (exist AP from stat auras)
        if (HasAuraTypeWithMiscvalue(SPELL_AURA_MOD_ATTACK_POWER_OF_STAT_PERCENT, stat))
            UpdateAttackPowerAndDamage(false);
        if (HasAuraTypeWithMiscvalue(SPELL_AURA_MOD_RANGED_ATTACK_POWER_OF_STAT_PERCENT, stat))
            UpdateAttackPowerAndDamage(true);
    }

    UpdateSpellDamageAndHealingBonus();
    UpdateManaRegen();

    // Update ratings in exist SPELL_AURA_MOD_RATING_FROM_STAT and only depends from stat
    uint32 mask = 0;
    AuraEffectList const& modRatingFromStat = GetAuraEffectsByType(SPELL_AURA_MOD_RATING_FROM_STAT);
    for (AuraEffectList::const_iterator i = modRatingFromStat.begin(); i != modRatingFromStat.end(); ++i)
        if (Stats((*i)->GetMiscValueB()) == stat)
            mask |= (*i)->GetMiscValue();
    if (mask)
    {
        for (uint32 rating = 0; rating < MAX_COMBAT_RATING; ++rating)
            if (mask & (1 << rating))
                ApplyRatingMod(CombatRating(rating), 0, true);
    }
    return true;
}

void Player::ApplySpellPowerBonus(int32 amount, bool apply)
{
    apply = _ModifyUInt32(apply, m_baseSpellPower, amount);

    // For speed just update for client
    ApplyModUInt32Value(PLAYER_FIELD_MOD_HEALING_DONE_POS, amount, apply);
    for (int i = SPELL_SCHOOL_HOLY; i < MAX_SPELL_SCHOOL; ++i)
        ApplyModUInt32Value(PLAYER_FIELD_MOD_DAMAGE_DONE_POS + i, amount, apply);
}

void Player::UpdateSpellDamageAndHealingBonus()
{
    // Magic damage modifiers implemented in Unit::SpellDamageBonusDone
    // This information for client side use only
    // Get healing bonus for all schools
    SetStatInt32Value(PLAYER_FIELD_MOD_HEALING_DONE_POS, SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_ALL));
    // Get damage bonus for all schools
    for (int i = SPELL_SCHOOL_HOLY; i < MAX_SPELL_SCHOOL; ++i)
        SetStatInt32Value(PLAYER_FIELD_MOD_DAMAGE_DONE_POS + i, SpellBaseDamageBonusDone(SpellSchoolMask(1 << i)));
}

bool Player::UpdateAllStats()
{
    for (int8 i = STAT_STRENGTH; i < MAX_STATS; ++i)
    {
        float value = 0.0f;

        if (GetStatsModifier()->IsActive())
        {
            UnitMods unitMod = UnitMods(UNIT_MOD_STAT_START + i);

            if (m_auraModifiersGroup[unitMod][TOTAL_PCT] <= 0.0f)
                value = 0.0f;
            else
            {
                value = m_auraModifiersGroup[unitMod][BASE_VALUE];
                GetStatsModifier()->ApplyModifier(value);
                value += GetCreateStat(Stats(i));
                value *= m_auraModifiersGroup[unitMod][BASE_PCT];
                value += m_auraModifiersGroup[unitMod][TOTAL_VALUE];
                value *= m_auraModifiersGroup[unitMod][TOTAL_PCT];
            }
        }
        else
            value = GetTotalStatValue(Stats(i));

        SetStat(Stats(i), int32(value));
    }

    UpdateArmor();
    // calls UpdateAttackPowerAndDamage() in UpdateArmor for SPELL_AURA_MOD_ATTACK_POWER_OF_ARMOR
    UpdateAttackPowerAndDamage(true);
    UpdateMaxHealth();

    for (uint8 i = POWER_MANA; i < MAX_POWERS; ++i)
        UpdateMaxPower(Powers(i));

    UpdateAllRatings();
    UpdateAllCritPercentages();
    UpdateAllSpellCritChances();
    UpdateDefenseBonusesMod();
    UpdateShieldBlockValue();
    UpdateSpellDamageAndHealingBonus();
    UpdateManaRegen();
    UpdateExpertise(BASE_ATTACK);
    UpdateExpertise(OFF_ATTACK);
    RecalculateRating(CR_ARMOR_PENETRATION);
    for (int i = SPELL_SCHOOL_NORMAL; i < MAX_SPELL_SCHOOL; ++i)
        UpdateResistances(i);

    return true;
}

void Player::UpdateResistances(uint32 school)
{
    if (school > SPELL_SCHOOL_NORMAL)
    {
        float value  = GetTotalAuraModValue(UnitMods(UNIT_MOD_RESISTANCE_START + school));
        SetResistance(SpellSchools(school), int32(value));

        // <TehKevin> Pets shouldn't get an update -> They take a snapshot at summon
        /*Pet* pet = GetPet();
        if (pet)
            pet->UpdateResistances(school);*/
    }
    else
        UpdateArmor();
}

void Player::UpdateArmor()
{
    float value = 0.0f;
    UnitMods unitMod = UNIT_MOD_ARMOR;

    value  = GetModifierValue(unitMod, BASE_VALUE);         // base armor (from items)
    value *= GetModifierValue(unitMod, BASE_PCT);           // armor percent from items
    value += GetStat(STAT_AGILITY) * 2.0f;                  // armor bonus from stats
    value += GetModifierValue(unitMod, TOTAL_VALUE);

    //add dynamic flat mods
    AuraEffectList const& mResbyIntellect = GetAuraEffectsByType(SPELL_AURA_MOD_RESISTANCE_OF_STAT_PERCENT);
    for (AuraEffectList::const_iterator i = mResbyIntellect.begin(); i != mResbyIntellect.end(); ++i)
    {
        if ((*i)->GetMiscValue() & SPELL_SCHOOL_MASK_NORMAL)
            value += CalculatePctN(GetStat(Stats((*i)->GetMiscValueB())), (*i)->GetAmount());
    }

    value *= GetModifierValue(unitMod, TOTAL_PCT);

    SetArmor(int32(value));

    // <TehKevin> Pets shouldn't get an update -> They take a snapshot at summon
    /*Pet* pet = GetPet();
    if (pet)
        pet->UpdateArmor();*/

    UpdateAttackPowerAndDamage();                           // armor dependent auras update for SPELL_AURA_MOD_ATTACK_POWER_OF_ARMOR
}

float Player::GetHealthBonusFromStamina()
{
    float stamina = GetStat(STAT_STAMINA);

    float baseStam = stamina < 20 ? stamina : 20;
    float moreStam = stamina - baseStam;

    return baseStam + (moreStam*10.0f);
}

float Player::GetManaBonusFromIntellect()
{
    float intellect = GetStat(STAT_INTELLECT);

    float baseInt = intellect < 20 ? intellect : 20;
    float moreInt = intellect - baseInt;

    return baseInt + (moreInt*15.0f);
}

void Player::UpdateMaxHealth()
{
    UnitMods unitMod = UNIT_MOD_HEALTH;

    float value = GetModifierValue(unitMod, BASE_VALUE) + GetCreateHealth();
    value *= GetModifierValue(unitMod, BASE_PCT);
    value += GetModifierValue(unitMod, TOTAL_VALUE) + GetHealthBonusFromStamina();
    value *= GetModifierValue(unitMod, TOTAL_PCT);

    SetMaxHealth((uint32)value);
}

void Player::UpdateMaxPower(Powers power)
{
    UnitMods unitMod = UnitMods(UNIT_MOD_POWER_START + power);

    float bonusPower = (power == POWER_MANA && GetCreatePowers(power) > 0) ? GetManaBonusFromIntellect() : 0;

    float value = GetModifierValue(unitMod, BASE_VALUE) + GetCreatePowers(power);
    value *= GetModifierValue(unitMod, BASE_PCT);
    value += GetModifierValue(unitMod, TOTAL_VALUE) +  bonusPower;
    value *= GetModifierValue(unitMod, TOTAL_PCT);

    SetMaxPower(power, uint32(value));
}

void Player::ApplyFeralAPBonus(int32 amount, bool apply)
{
    _ModifyUInt32(apply, m_baseFeralAP, amount);
    UpdateAttackPowerAndDamage();
}

void Player::UpdateAttackPowerAndDamage(bool ranged)
{
    float val2 = 0.0f;
    float level = float(getLevel());

    UnitMods unitMod = ranged ? UNIT_MOD_ATTACK_POWER_RANGED : UNIT_MOD_ATTACK_POWER;

    uint16 index = UNIT_FIELD_ATTACK_POWER;
    uint16 index_mod = UNIT_FIELD_ATTACK_POWER_MODS;
    uint16 index_mult = UNIT_FIELD_ATTACK_POWER_MULTIPLIER;

    if (ranged)
    {
        index = UNIT_FIELD_RANGED_ATTACK_POWER;
        index_mod = UNIT_FIELD_RANGED_ATTACK_POWER_MODS;
        index_mult = UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER;

        switch (getClass())
        {
            case CLASS_HUNTER:
                val2 = level * 2.0f + GetStat(STAT_AGILITY) - 10.0f;
                break;
            case CLASS_ROGUE:
                val2 = level + GetStat(STAT_AGILITY) - 10.0f;
                break;
            case CLASS_WARRIOR:
                val2 = level + GetStat(STAT_AGILITY) - 10.0f;
                break;
            case CLASS_DRUID:
                switch (GetShapeshiftForm())
                {
                    case FORM_CAT:
                    case FORM_BEAR:
                    case FORM_DIREBEAR:
                        val2 = 0.0f; break;
                    default:
                        val2 = GetStat(STAT_AGILITY) - 10.0f; break;
                }
                break;
            default: val2 = GetStat(STAT_AGILITY) - 10.0f; break;
        }
    }
    else
    {
        switch (getClass())
        {
            case CLASS_WARRIOR:
                val2 = level * 3.0f + GetStat(STAT_STRENGTH) * 2.0f - 20.0f;
                break;
            case CLASS_PALADIN:
                val2 = level * 3.0f + GetStat(STAT_STRENGTH) * 2.0f - 20.0f;
                break;
            case CLASS_DEATH_KNIGHT:
                val2 = level * 3.0f + GetStat(STAT_STRENGTH) * 2.0f - 20.0f;
                break;
            case CLASS_ROGUE:
                val2 = level * 2.0f + GetStat(STAT_STRENGTH) + GetStat(STAT_AGILITY) - 20.0f;
                break;
            case CLASS_HUNTER:
                val2 = level * 2.0f + GetStat(STAT_STRENGTH) + GetStat(STAT_AGILITY) - 20.0f;
                break;
            case CLASS_SHAMAN:
                val2 = level * 2.0f + GetStat(STAT_STRENGTH) + GetStat(STAT_AGILITY) - 20.0f;
                break;
            case CLASS_DRUID:
            {
                // Check if Predatory Strikes is skilled
                float mLevelMult = 0.0f;
                float weapon_bonus = 0.0f;
                if (IsInFeralForm())
                {
                    Unit::AuraEffectList const& mDummy = GetAuraEffectsByType(SPELL_AURA_DUMMY);
                    for (Unit::AuraEffectList::const_iterator itr = mDummy.begin(); itr != mDummy.end(); ++itr)
                    {
                        AuraEffect* aurEff = *itr;
                        if (aurEff->GetSpellInfo()->SpellIconID == 1563)
                        {
                            switch (aurEff->GetEffIndex())
                            {
                                case 0: // Predatory Strikes (effect 0)
                                    mLevelMult = CalculatePctN(1.0f, aurEff->GetAmount());
                                    break;
                                case 1: // Predatory Strikes (effect 1)
                                    if (Item* mainHand = m_items[EQUIPMENT_SLOT_MAINHAND])
                                    {
                                        // also gains % attack power from equipped weapon
                                        ItemTemplate const* proto = mainHand->GetTemplate();
                                        if (!proto)
                                            continue;

                                        weapon_bonus = CalculatePctN(float(proto->getFeralBonus()), aurEff->GetAmount());
                                    }
                                    break;
                                default:
                                    break;
                            }
                        }
                    }
                }

                switch (GetShapeshiftForm())
                {
                    case FORM_CAT:
                        val2 = getLevel() * (mLevelMult + 2.0f) + GetStat(STAT_STRENGTH) * 2.0f + GetStat(STAT_AGILITY) - 20.0f + weapon_bonus + m_baseFeralAP;
                        break;
                    case FORM_BEAR:
                    case FORM_DIREBEAR:
                        val2 = getLevel() * (mLevelMult + 3.0f) + GetStat(STAT_STRENGTH) * 2.0f - 20.0f + weapon_bonus + m_baseFeralAP;
                        break;
                    case FORM_MOONKIN:
                        val2 = getLevel() * (mLevelMult + 1.5f) + GetStat(STAT_STRENGTH) * 2.0f - 20.0f + m_baseFeralAP;
                        break;
                    default:
                        val2 = GetStat(STAT_STRENGTH) * 2.0f - 20.0f;
                        break;
                }
                break;
            }
            case CLASS_MAGE:
                val2 = GetStat(STAT_STRENGTH) - 10.0f;
                break;
            case CLASS_PRIEST:
                val2 = GetStat(STAT_STRENGTH) - 10.0f;
                break;
            case CLASS_WARLOCK:
                val2 = GetStat(STAT_STRENGTH) - 10.0f;
                break;
        }
    }

    SetModifierValue(unitMod, BASE_VALUE, val2);

    float base_attPower = GetModifierValue(unitMod, BASE_VALUE) * GetModifierValue(unitMod, BASE_PCT);
    float attPowerMod = GetModifierValue(unitMod, TOTAL_VALUE);
    GetStatsModifier()->ApplyModifier(attPowerMod);

    //add dynamic flat mods
    if (ranged)
    {
        if ((getClassMask() & CLASSMASK_WAND_USERS) == 0)
        {
            AuraEffectList const& mRAPbyStat = GetAuraEffectsByType(SPELL_AURA_MOD_RANGED_ATTACK_POWER_OF_STAT_PERCENT);
            for (AuraEffectList::const_iterator i = mRAPbyStat.begin(); i != mRAPbyStat.end(); ++i)
                attPowerMod += CalculatePctN(GetStat(Stats((*i)->GetMiscValue())), (*i)->GetAmount());
        }
    }
    else
    {
        AuraEffectList const& mAPbyStat = GetAuraEffectsByType(SPELL_AURA_MOD_ATTACK_POWER_OF_STAT_PERCENT);
        for (AuraEffectList::const_iterator i = mAPbyStat.begin(); i != mAPbyStat.end(); ++i)
            attPowerMod += CalculatePctN(GetStat(Stats((*i)->GetMiscValue())), (*i)->GetAmount());

        AuraEffectList const& mAPbyArmor = GetAuraEffectsByType(SPELL_AURA_MOD_ATTACK_POWER_OF_ARMOR);
        for (AuraEffectList::const_iterator iter = mAPbyArmor.begin(); iter != mAPbyArmor.end(); ++iter)
            // always: ((*i)->GetModifier()->m_miscvalue == 1 == SPELL_SCHOOL_MASK_NORMAL)
            attPowerMod += int32(GetArmor() / (*iter)->GetAmount());
    }

    float attPowerMultiplier = GetModifierValue(unitMod, TOTAL_PCT) - 1.0f;

    SetInt32Value(index, (uint32)base_attPower);    //UNIT_FIELD_(RANGED)_ATTACK_POWER field
    SetInt32Value(index_mod, (uint32)attPowerMod);  //UNIT_FIELD_(RANGED)_ATTACK_POWER_MODS field
    SetFloatValue(index_mult, attPowerMultiplier);   //UNIT_FIELD_(RANGED)_ATTACK_POWER_MULTIPLIER field

    // <TehKevin> Pets shouldn't get an update -> They take a snapshot at summon
    //Pet* pet = GetPet();

    //automatically update weapon damage after attack power modification
    if (ranged)
    {
        UpdateDamagePhysical(RANGED_ATTACK);

        /*if (pet && pet->isHunterPet()) // At ranged attack change for hunter pet
            pet->UpdateAttackPowerAndDamage();*/
    }
    else
    {
        UpdateDamagePhysical(BASE_ATTACK);
        if (CanDualWield() && haveOffhandWeapon())           //allow update offhand damage only if player knows DualWield Spec and has equipped offhand weapon
            UpdateDamagePhysical(OFF_ATTACK);
        if (getClass() == CLASS_SHAMAN || getClass() == CLASS_PALADIN)                      // mental quickness
            UpdateSpellDamageAndHealingBonus();

        /*if (pet && pet->IsPetGhoul()) // At ranged attack change for hunter pet
            pet->UpdateAttackPowerAndDamage();*/
    }
}

void Player::UpdateShieldBlockValue()
{
    SetUInt32Value(PLAYER_SHIELD_BLOCK, GetShieldBlockValue());
}

void Player::CalculateMinMaxDamage(WeaponAttackType attType, bool normalized, bool addTotalPct, float& min_damage, float& max_damage)
{
    UnitMods unitMod;

    switch (attType)
    {
        case BASE_ATTACK:
        default:
            unitMod = UNIT_MOD_DAMAGE_MAINHAND;
            break;
        case OFF_ATTACK:
            unitMod = UNIT_MOD_DAMAGE_OFFHAND;
            break;
        case RANGED_ATTACK:
            unitMod = UNIT_MOD_DAMAGE_RANGED;
            break;
    }

    float att_speed = GetAPMultiplier(attType, normalized);

    float base_value  = GetModifierValue(unitMod, BASE_VALUE) + GetTotalAttackPowerValue(attType)/ 14.0f * att_speed;
    float base_pct    = GetModifierValue(unitMod, BASE_PCT);
    float total_value = GetModifierValue(unitMod, TOTAL_VALUE);
    float total_pct   = addTotalPct ? GetModifierValue(unitMod, TOTAL_PCT) : 1.0f;

    float weapon_mindamage = GetWeaponDamageRange(attType, MINDAMAGE);
    float weapon_maxdamage = GetWeaponDamageRange(attType, MAXDAMAGE);

    GetStatsModifier()->ApplyModifier(weapon_mindamage);
    GetStatsModifier()->ApplyModifier(weapon_maxdamage);

    if (IsInFeralForm())                                    //check if player is druid and in cat or bear forms
    {
        uint8 lvl = getLevel();
        if (lvl > 60)
            lvl = 60;

        weapon_mindamage = lvl*0.85f*att_speed;
        weapon_maxdamage = lvl*1.25f*att_speed;
    }
    else if (!CanUseAttackType(attType))      //check if player not in form but still can't use (disarm case)
    {
        //cannot use ranged/off attack, set values to 0
        if (attType != BASE_ATTACK)
        {
            min_damage = 0;
            max_damage = 0;
            return;
        }
        weapon_mindamage = BASE_MINDAMAGE;
        weapon_maxdamage = BASE_MAXDAMAGE;
    }
    else if (attType == RANGED_ATTACK)                       //add ammo DPS to ranged damage
    {
        weapon_mindamage += GetAmmoDPS() * att_speed;
        weapon_maxdamage += GetAmmoDPS() * att_speed;
    }

    min_damage = ((base_value + weapon_mindamage) * base_pct + total_value) * total_pct;
    max_damage = ((base_value + weapon_maxdamage) * base_pct + total_value) * total_pct;
}

void Player::UpdateDamagePhysical(WeaponAttackType attType)
{
    float mindamage;
    float maxdamage;

    CalculateMinMaxDamage(attType, false, true, mindamage, maxdamage);

    switch (attType)
    {
        case BASE_ATTACK:
        default:
            SetStatFloatValue(UNIT_FIELD_MINDAMAGE, mindamage);
            SetStatFloatValue(UNIT_FIELD_MAXDAMAGE, maxdamage);
            break;
        case OFF_ATTACK:
            SetStatFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE, mindamage);
            SetStatFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE, maxdamage);
            break;
        case RANGED_ATTACK:
            SetStatFloatValue(UNIT_FIELD_MINRANGEDDAMAGE, mindamage);
            SetStatFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE, maxdamage);
            break;
    }
}

void Player::UpdateDefenseBonusesMod()
{
    UpdateBlockPercentage();
    UpdateParryPercentage();
    UpdateDodgePercentage();
}

void Player::UpdateBlockPercentage()
{
    // No block
    float value = 0.0f;
    if (CanBlock())
    {
        // Base value
        value = 5.0f;
        // Modify value from defense skill
        value += (int32(GetDefenseSkillValue()) - int32(GetMaxSkillValueForLevel())) * 0.04f;
        // Increase from SPELL_AURA_MOD_BLOCK_PERCENT aura
        value += GetTotalAuraModifier(SPELL_AURA_MOD_BLOCK_PERCENT);
        // Increase from rating
        value += GetRatingBonusValue(CR_BLOCK);
        value = value < 0.0f ? 0.0f : value;
    }
    SetStatFloatValue(PLAYER_BLOCK_PERCENTAGE, value);
}

void Player::UpdateCritPercentage(WeaponAttackType attType)
{
    BaseModGroup modGroup;
    uint16 index;
    CombatRating cr;

    switch (attType)
    {
        case OFF_ATTACK:
            modGroup = OFFHAND_CRIT_PERCENTAGE;
            index = PLAYER_OFFHAND_CRIT_PERCENTAGE;
            cr = CR_CRIT_MELEE;
            break;
        case RANGED_ATTACK:
            modGroup = RANGED_CRIT_PERCENTAGE;
            index = PLAYER_RANGED_CRIT_PERCENTAGE;
            cr = CR_CRIT_RANGED;
            break;
        case BASE_ATTACK:
        default:
            modGroup = CRIT_PERCENTAGE;
            index = PLAYER_CRIT_PERCENTAGE;
            cr = CR_CRIT_MELEE;
            break;
    }

    float value = GetRatingBonusValue(cr);
    GetStatsModifier()->ApplyModifier(value);
    value += GetTotalPercentageModValue(modGroup);

    // Modify crit from weapon skill and maximized defense skill of same level victim difference
    value += (int32(GetWeaponSkillValue(attType)) - int32(GetMaxSkillValueForLevel())) * 0.04f;
    value = value < 0.0f ? 0.0f : value;
    SetStatFloatValue(index, value);
}

void Player::UpdateAllCritPercentages()
{
    float value = GetMeleeCritFromAgility();

    SetBaseModValue(CRIT_PERCENTAGE, PCT_MOD, value);
    SetBaseModValue(OFFHAND_CRIT_PERCENTAGE, PCT_MOD, value);
    SetBaseModValue(RANGED_CRIT_PERCENTAGE, PCT_MOD, value);

    UpdateCritPercentage(BASE_ATTACK);
    UpdateCritPercentage(OFF_ATTACK);
    UpdateCritPercentage(RANGED_ATTACK);
}

const float m_diminishing_k[MAX_CLASSES] =
{
    0.9560f,  // Warrior
    0.9560f,  // Paladin
    0.9880f,  // Hunter
    0.9880f,  // Rogue
    0.9830f,  // Priest
    0.9560f,  // DK
    0.9880f,  // Shaman
    0.9830f,  // Mage
    0.9830f,  // Warlock
    0.0f,     // ??
    0.9720f   // Druid
};

float Player::GetMissPercentageFromDefence() const
{
    float const miss_cap[MAX_CLASSES] =
    {
        16.00f,     // Warrior //correct
        16.00f,     // Paladin //correct
        16.00f,     // Hunter  //?
        16.00f,     // Rogue   //?
        16.00f,     // Priest  //?
        16.00f,     // DK      //correct
        16.00f,     // Shaman  //?
        16.00f,     // Mage    //?
        16.00f,     // Warlock //?
        0.0f,       // ??
        16.00f      // Druid   //?
    };

    float diminishing = 0.0f, nondiminishing = 0.0f;
    // Modify value from defense skill (only bonus from defense rating diminishes)
    nondiminishing += (GetSkillValue(SKILL_DEFENSE) - GetMaxSkillValueForLevel()) * 0.04f;
    diminishing += (int32(GetRatingBonusValue(CR_DEFENSE_SKILL))) * 0.04f;

    // apply diminishing formula to diminishing miss chance
    uint32 pclass = getClass()-1;
    return nondiminishing + (diminishing * miss_cap[pclass] / (diminishing + miss_cap[pclass] * m_diminishing_k[pclass]));
}

void Player::UpdateParryPercentage()
{
    const float parry_cap[MAX_CLASSES] =
    {
        47.003525f,     // Warrior
        47.003525f,     // Paladin
        145.560408f,    // Hunter
        145.560408f,    // Rogue
        0.0f,           // Priest
        47.003525f,     // DK
        145.560408f,    // Shaman
        0.0f,           // Mage
        0.0f,           // Warlock
        0.0f,           // ??
        0.0f            // Druid
    };

    // No parry
    float value = 0.0f;
    uint32 pclass = getClass()-1;
    if (CanParry() && parry_cap[pclass] > 0.0f)
    {
        float nondiminishing  = 5.0f;
        // Parry from rating
        float diminishing = GetRatingBonusValue(CR_PARRY);
        // Modify value from defense skill (only bonus from defense rating diminishes)
        nondiminishing += (GetSkillValue(SKILL_DEFENSE) - GetMaxSkillValueForLevel()) * 0.04f;
        diminishing += (int32(GetRatingBonusValue(CR_DEFENSE_SKILL))) * 0.04f;
        // Parry from SPELL_AURA_MOD_PARRY_PERCENT aura
        nondiminishing += GetTotalAuraModifier(SPELL_AURA_MOD_PARRY_PERCENT);
        // apply diminishing formula to diminishing parry chance
        value = nondiminishing + diminishing * parry_cap[pclass] / (diminishing + parry_cap[pclass] * m_diminishing_k[pclass]);
        value = value < 0.0f ? 0.0f : value;
    }
    SetStatFloatValue(PLAYER_PARRY_PERCENTAGE, value);
}

void Player::UpdateDodgePercentage()
{
    const float dodge_cap[MAX_CLASSES] =
    {
        88.129021f,     // Warrior
        88.129021f,     // Paladin
        145.560408f,    // Hunter
        145.560408f,    // Rogue
        150.375940f,    // Priest
        88.129021f,     // DK
        145.560408f,    // Shaman
        150.375940f,    // Mage
        150.375940f,    // Warlock
        0.0f,           // ??
        116.890707f     // Druid
    };

    float diminishing = 0.0f, nondiminishing = 0.0f;
    GetDodgeFromAgility(diminishing, nondiminishing);
    // Modify value from defense skill (only bonus from defense rating diminishes)
    nondiminishing += (GetSkillValue(SKILL_DEFENSE) - GetMaxSkillValueForLevel()) * 0.04f;
    diminishing += (int32(GetRatingBonusValue(CR_DEFENSE_SKILL))) * 0.04f;
    // Dodge from SPELL_AURA_MOD_DODGE_PERCENT aura
    nondiminishing += GetTotalAuraModifier(SPELL_AURA_MOD_DODGE_PERCENT);
    // Dodge from rating
    diminishing += GetRatingBonusValue(CR_DODGE);
    // apply diminishing formula to diminishing dodge chance
    uint32 pclass = getClass()-1;
    float value = nondiminishing + (diminishing * dodge_cap[pclass] / (diminishing + dodge_cap[pclass] * m_diminishing_k[pclass]));

    value = value < 0.0f ? 0.0f : value;
    SetStatFloatValue(PLAYER_DODGE_PERCENTAGE, value);
}

void Player::UpdateSpellCritChance(uint32 school)
{
    // For normal school set zero crit chance
    if (school == SPELL_SCHOOL_NORMAL)
    {
        SetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1, 0.0f);
        return;
    }
    // For others recalculate it from:
    float crit = 0.0f;
    // Crit from Intellect
    crit += GetSpellCritFromIntellect();
    // Increase crit from SPELL_AURA_MOD_SPELL_CRIT_CHANCE
    crit += GetTotalAuraModifier(SPELL_AURA_MOD_SPELL_CRIT_CHANCE);
    // Increase crit from SPELL_AURA_MOD_CRIT_PCT
    crit += GetTotalAuraModifier(SPELL_AURA_MOD_CRIT_PCT);
    // Increase crit by school from SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL
    crit += GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL, 1<<school);
    // Increase crit from spell crit ratings
    crit += GetRatingBonusValue(CR_CRIT_SPELL) * GetStatsModifier()->GetModifier();

    // Store crit value
    SetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + school, crit);
}

void Player::UpdateArmorPenetration(int32 amount)
{
    // Store Rating Value
    GetStatsModifier()->ApplyModifier(amount);
    SetUInt32Value(PLAYER_FIELD_COMBAT_RATING_1 + CR_ARMOR_PENETRATION, amount);
}

void Player::UpdateMeleeHitChances()
{
    m_modMeleeHitChance = (float)GetTotalAuraModifier(SPELL_AURA_MOD_HIT_CHANCE);
    m_modMeleeHitChance += GetRatingBonusValue(CR_HIT_MELEE);
}

void Player::UpdateRangedHitChances()
{
    m_modRangedHitChance = (float)GetTotalAuraModifier(SPELL_AURA_MOD_HIT_CHANCE);
    m_modRangedHitChance += GetRatingBonusValue(CR_HIT_RANGED);
}

void Player::UpdateSpellHitChances()
{
    m_modSpellHitChance = (float)GetTotalAuraModifier(SPELL_AURA_MOD_SPELL_HIT_CHANCE);
    m_modSpellHitChance += GetRatingBonusValue(CR_HIT_SPELL);
}

void Player::UpdateAllSpellCritChances()
{
    for (int i = SPELL_SCHOOL_NORMAL; i < MAX_SPELL_SCHOOL; ++i)
        UpdateSpellCritChance(i);
}

void Player::UpdateExpertise(WeaponAttackType attack)
{
    if (attack == RANGED_ATTACK)
        return;

    int32 expertise = int32(GetRatingBonusValue(CR_EXPERTISE));

    Item* weapon = GetWeaponForAttack(attack, true);

    AuraEffectList const& expAuras = GetAuraEffectsByType(SPELL_AURA_MOD_EXPERTISE);
    for (AuraEffectList::const_iterator itr = expAuras.begin(); itr != expAuras.end(); ++itr)
    {
        // item neutral spell
        if ((*itr)->GetSpellInfo()->EquippedItemClass == -1)
            expertise += (*itr)->GetAmount();
        // item dependent spell
        else if (weapon && weapon->IsFitToSpellRequirements((*itr)->GetSpellInfo()))
            expertise += (*itr)->GetAmount();
    }

    if (expertise < 0)
        expertise = 0;

    switch (attack)
    {
        case BASE_ATTACK: SetUInt32Value(PLAYER_EXPERTISE, expertise);         break;
        case OFF_ATTACK:  SetUInt32Value(PLAYER_OFFHAND_EXPERTISE, expertise); break;
        default: break;
    }
}

void Player::ApplyManaRegenBonus(int32 amount, bool apply)
{
    _ModifyUInt32(apply, m_baseManaRegen, amount);
    UpdateManaRegen();
}

void Player::ApplyHealthRegenBonus(int32 amount, bool apply)
{
    _ModifyUInt32(apply, m_baseHealthRegen, amount);
}

void Player::UpdateManaRegen()
{
    float Intellect = GetStat(STAT_INTELLECT);
    // Mana regen from spirit and intellect
    float power_regen = sqrt(Intellect) * OCTRegenMPPerSpirit();
    // Apply PCT bonus from SPELL_AURA_MOD_POWER_REGEN_PERCENT aura on spirit base regen
    power_regen *= GetTotalAuraMultiplierByMiscValue(SPELL_AURA_MOD_POWER_REGEN_PERCENT, POWER_MANA);

    // Mana regen from SPELL_AURA_MOD_POWER_REGEN aura
    float power_regen_mp5 = (GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_POWER_REGEN, POWER_MANA) + m_baseManaRegen) / 5.0f;

    // Get bonus from SPELL_AURA_MOD_MANA_REGEN_FROM_STAT aura
    AuraEffectList const& regenAura = GetAuraEffectsByType(SPELL_AURA_MOD_MANA_REGEN_FROM_STAT);
    for (AuraEffectList::const_iterator i = regenAura.begin(); i != regenAura.end(); ++i)
    {
        power_regen_mp5 += GetStat(Stats((*i)->GetMiscValue())) * (*i)->GetAmount() / 500.0f;
    }

    // Set regen rate in cast state apply only on spirit based regen
    int32 modManaRegenInterrupt = GetTotalAuraModifier(SPELL_AURA_MOD_MANA_REGEN_INTERRUPT);
    if (modManaRegenInterrupt > 100)
        modManaRegenInterrupt = 100;
    SetStatFloatValue(UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER, power_regen_mp5 + CalculatePctN(power_regen, modManaRegenInterrupt));

    SetStatFloatValue(UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER, power_regen_mp5 + power_regen);
}

void Player::UpdateRuneRegen(RuneType rune)
{
    if (rune >= NUM_RUNE_TYPES)
        return;

    uint32 cooldown = 0;

    for (uint32 i = 0; i < MAX_RUNES; ++i)
        if (GetBaseRune(i) == rune)
        {
            cooldown = GetRuneBaseCooldown(i);
            break;
        }

    if (cooldown <= 0)
        return;

    float regen = float(1 * IN_MILLISECONDS) / float(cooldown);
    SetFloatValue(PLAYER_RUNE_REGEN_1 + uint8(rune), regen);
}

void Player::_ApplyAllStatBonuses()
{
    SetCanModifyStats(false);

    _ApplyAllAuraStatMods();
    _ApplyAllItemMods();

    SetCanModifyStats(true);

    UpdateAllStats();
}

void Player::_RemoveAllStatBonuses()
{
    SetCanModifyStats(false);

    _RemoveAllItemMods();
    _RemoveAllAuraStatMods();

    SetCanModifyStats(true);

    UpdateAllStats();
}

/*#######################################
########                         ########
########    MOBS STAT SYSTEM     ########
########                         ########
#######################################*/

bool Creature::UpdateStats(Stats /*stat*/)
{
    return true;
}

bool Creature::UpdateAllStats()
{
    UpdateMaxHealth();
    UpdateAttackPowerAndDamage();
    UpdateAttackPowerAndDamage(true);

    for (uint8 i = POWER_MANA; i < MAX_POWERS; ++i)
        UpdateMaxPower(Powers(i));

    for (int8 i = SPELL_SCHOOL_NORMAL; i < MAX_SPELL_SCHOOL; ++i)
        UpdateResistances(i);

    return true;
}

void Creature::UpdateResistances(uint32 school)
{
    if (school > SPELL_SCHOOL_NORMAL)
    {
        float value  = GetTotalAuraModValue(UnitMods(UNIT_MOD_RESISTANCE_START + school));
        SetResistance(SpellSchools(school), int32(value));
    }
    else
        UpdateArmor();
}

void Creature::UpdateArmor()
{
    float value = GetTotalAuraModValue(UNIT_MOD_ARMOR);
    SetArmor(int32(value));
}

void Creature::UpdateMaxHealth()
{
    float value = GetTotalAuraModValue(UNIT_MOD_HEALTH);
    SetMaxHealth((uint32)value);
}

void Creature::UpdateMaxPower(Powers power)
{
    UnitMods unitMod = UnitMods(UNIT_MOD_POWER_START + power);

    float value  = GetTotalAuraModValue(unitMod);
    SetMaxPower(power, uint32(value));
}

void Creature::UpdateAttackPowerAndDamage(bool ranged)
{
    UnitMods unitMod = ranged ? UNIT_MOD_ATTACK_POWER_RANGED : UNIT_MOD_ATTACK_POWER;

    uint16 index = UNIT_FIELD_ATTACK_POWER;
    uint16 index_mod = UNIT_FIELD_ATTACK_POWER_MODS;
    uint16 index_mult = UNIT_FIELD_ATTACK_POWER_MULTIPLIER;

    if (ranged)
    {
        index = UNIT_FIELD_RANGED_ATTACK_POWER;
        index_mod = UNIT_FIELD_RANGED_ATTACK_POWER_MODS;
        index_mult = UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER;
    }

    float base_attPower  = GetModifierValue(unitMod, BASE_VALUE) * GetModifierValue(unitMod, BASE_PCT);
    float attPowerMod = GetModifierValue(unitMod, TOTAL_VALUE);
    float attPowerMultiplier = GetModifierValue(unitMod, TOTAL_PCT) - 1.0f;

    SetInt32Value(index, (uint32)base_attPower);            //UNIT_FIELD_(RANGED)_ATTACK_POWER field
    SetInt32Value(index_mod, (uint32)attPowerMod);          //UNIT_FIELD_(RANGED)_ATTACK_POWER_MODS field
    SetFloatValue(index_mult, attPowerMultiplier);          //UNIT_FIELD_(RANGED)_ATTACK_POWER_MULTIPLIER field

    //automatically update weapon damage after attack power modification
    if (ranged)
        UpdateDamagePhysical(RANGED_ATTACK);
    else
    {
        UpdateDamagePhysical(BASE_ATTACK);
        UpdateDamagePhysical(OFF_ATTACK);
    }
}

void Creature::UpdateDamagePhysical(WeaponAttackType attType)
{
    UnitMods unitMod;
    switch (attType)
    {
        case BASE_ATTACK:
        default:
            unitMod = UNIT_MOD_DAMAGE_MAINHAND;
            break;
        case OFF_ATTACK:
            unitMod = UNIT_MOD_DAMAGE_OFFHAND;
            break;
        case RANGED_ATTACK:
            unitMod = UNIT_MOD_DAMAGE_RANGED;
            break;
    }

    //float att_speed = float(GetAttackTime(attType))/1000.0f;

    float weapon_mindamage = GetWeaponDamageRange(attType, MINDAMAGE);
    float weapon_maxdamage = GetWeaponDamageRange(attType, MAXDAMAGE);

    /* difference in AP between current attack power and base value from DB */
    float att_pwr_change = GetTotalAttackPowerValue(attType) - GetCreatureTemplate()->attackpower;
    float base_value  = GetModifierValue(unitMod, BASE_VALUE) + (att_pwr_change * GetAPMultiplier(attType, false) / 14.0f);
    float base_pct    = GetModifierValue(unitMod, BASE_PCT);
    float total_value = GetModifierValue(unitMod, TOTAL_VALUE);
    float total_pct   = GetModifierValue(unitMod, TOTAL_PCT);
    float dmg_multiplier = GetCreatureTemplate()->dmg_multiplier;

    if (!CanUseAttackType(attType))
    {
        weapon_mindamage = 0;
        weapon_maxdamage = 0;
    }

    float mindamage = ((base_value + weapon_mindamage) * dmg_multiplier * base_pct + total_value) * total_pct;
    float maxdamage = ((base_value + weapon_maxdamage) * dmg_multiplier * base_pct + total_value) * total_pct;

    switch (attType)
    {
        case BASE_ATTACK:
        default:
            SetStatFloatValue(UNIT_FIELD_MINDAMAGE, mindamage);
            SetStatFloatValue(UNIT_FIELD_MAXDAMAGE, maxdamage);
            break;
        case OFF_ATTACK:
            SetStatFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE, mindamage);
            SetStatFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE, maxdamage);
            break;
        case RANGED_ATTACK:
            SetStatFloatValue(UNIT_FIELD_MINRANGEDDAMAGE, mindamage);
            SetStatFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE, maxdamage);
            break;
    }
}

/*#######################################
########                         ########
########  GUARDIAN STAT SYSTEM   ########
########                         ########
#######################################*/

bool Guardian::UpdateStats(Stats stat)
{
    if (stat >= MAX_STATS)
        return false;

    int32 value = GetTotalStatValue(stat);
    SetStat(stat, value);

    sLog->outDebug(LOG_FILTER_PETS, "Guardian::UpdateStats: Set stat %u to %u",
                   stat, value);

    switch (stat)
    {
        case STAT_STRENGTH:         UpdateAttackPowerAndDamage();        break;
        case STAT_AGILITY:          UpdateArmor();                       break;
        case STAT_STAMINA:          UpdateMaxHealth();                   break;
        case STAT_INTELLECT:        UpdateMaxPower(POWER_MANA);          break;
        case STAT_SPIRIT:
        default:
            break;
    }

    return true;
}

bool Guardian::UpdateAllStats()
{
    for (uint8 i = STAT_STRENGTH; i < MAX_STATS; ++i)
        UpdateStats(Stats(i));

    for (uint8 i = POWER_MANA; i < MAX_POWERS; ++i)
        UpdateMaxPower(Powers(i));

    for (uint8 i = SPELL_SCHOOL_NORMAL; i < MAX_SPELL_SCHOOL; ++i)
        UpdateResistances(i);

    UpdateHitRating();

    return true;
}

void Guardian::UpdateResistances(uint32 school)
{
    if (school > SPELL_SCHOOL_NORMAL)
    {
        float value  = GetTotalAuraModValue(UnitMods(UNIT_MOD_RESISTANCE_START + school));
        SetResistance(SpellSchools(school), int32(value));
    }
    else
        UpdateArmor();
}

void Guardian::UpdateArmor()
{
    float value = 0.0f;
    UnitMods unitMod = UNIT_MOD_ARMOR;

    value  = GetModifierValue(unitMod, BASE_VALUE);
    value *= GetModifierValue(unitMod, BASE_PCT);
    value += GetStat(STAT_AGILITY) * 2.0f;
    value += GetModifierValue(unitMod, TOTAL_VALUE);
    value *= GetModifierValue(unitMod, TOTAL_PCT);

    SetArmor(int32(value));
}

void Guardian::UpdateMaxHealth()
{
    UnitMods unitMod = UNIT_MOD_HEALTH;
    float value = 0.0f;
    float multiplicator = 10.0f;

    float stamina = GetStat(STAT_STAMINA);
    stamina -= GetCreateStat(STAT_STAMINA);

    if (ownerScaling)
        multiplicator = ownerScaling->GetScaling(STAT_IDENTIFIER_UNIT_MOD, unitMod);

    value = GetModifierValue(unitMod, BASE_VALUE) + GetCreateHealth();
    value *= GetModifierValue(unitMod, BASE_PCT);
    value += GetModifierValue(unitMod, TOTAL_VALUE);

    value += stamina * multiplicator;
    value *= GetModifierValue(unitMod, TOTAL_PCT);

    sLog->outDebug(LOG_FILTER_PETS, "Guardian::UpdateMaxHealth to %f with CreateHealth %u",
                   value, GetCreateHealth());

    SetMaxHealth(uint32(value));
}

void Guardian::UpdateMaxPower(Powers power)
{
    UnitMods unitMod = UnitMods(UNIT_MOD_POWER_START + power);
    float addValue = (power == POWER_MANA) ? GetStat(STAT_INTELLECT) - GetCreateStat(STAT_INTELLECT) : 0.0f;
    float multiplicator = 15.0f;
    float value = 0.0f;

    if (ownerScaling)
        multiplicator = ownerScaling->GetScaling(STAT_IDENTIFIER_UNIT_MOD, unitMod);

    value = GetModifierValue(unitMod, BASE_VALUE) + GetCreatePowers(power);
    value *= GetModifierValue(unitMod, BASE_PCT);
    value += GetModifierValue(unitMod, TOTAL_VALUE);

    value += addValue * multiplicator;
    value *= GetModifierValue(unitMod, TOTAL_PCT);

    SetMaxPower(power, uint32(value));
}

void Guardian::UpdateAttackPowerAndDamage(bool ranged)
{
    if (ranged)
        return;

    float val = 0.0f;
    UnitMods unitMod = UNIT_MOD_ATTACK_POWER;

    val = 2 * GetStat(STAT_STRENGTH) - 20.0f;
    SetModifierValue(UNIT_MOD_ATTACK_POWER, BASE_VALUE, val);

    // In BASE_VALUE of UNIT_MOD_ATTACK_POWER for creatures we store data of meleeattackpower field in DB
    float base_attPower = GetModifierValue(unitMod, BASE_VALUE) * GetModifierValue(unitMod, BASE_PCT);
    float attPowerMod = GetModifierValue(unitMod, TOTAL_VALUE);
    float attPowerMultiplier = GetModifierValue(unitMod, TOTAL_PCT) - 1.0f;

    // UNIT_FIELD_(RANGED)_ATTACK_POWER field
    SetInt32Value(UNIT_FIELD_ATTACK_POWER, (int32)base_attPower);
    // UNIT_FIELD_(RANGED)_ATTACK_POWER_MODS field
    SetInt32Value(UNIT_FIELD_ATTACK_POWER_MODS, (int32)attPowerMod);
    // UNIT_FIELD_(RANGED)_ATTACK_POWER_MULTIPLIER field
    SetFloatValue(UNIT_FIELD_ATTACK_POWER_MULTIPLIER, attPowerMultiplier);

    // Automatically update weapon damage after attack power modification
    UpdateDamagePhysical(BASE_ATTACK);
}

void Guardian::UpdateDamagePhysical(WeaponAttackType attType)
{
    if (attType > BASE_ATTACK)
        return;

    UnitMods unitMod = UNIT_MOD_DAMAGE_MAINHAND;

    float bonusDamage = 0.0f;
    float bonus = 0.0f;
    float coef = 14.0f;

    if (ownerScaling)
    {
        bonusDamage = ownerScaling->CalculateScaling(STAT_IDENTIFIER_UNIT_MOD, unitMod,
                                                     GetOwnerSpellPower(GetEntry() == ENTRY_TREANT ? SPELL_SCHOOL_NATURE : SPELL_SCHOOL_FIRE));
        bonus = ownerScaling->CalculateScaling(STAT_IDENTIFIER_ATTACK_POWER, attType,
                                               GetOwnerSpellPower(GetEntry() == ENTRY_TREANT ? SPELL_SCHOOL_NATURE : SPELL_SCHOOL_FIRE));
        coef = ownerScaling->GetScaling(STAT_IDENTIFIER_UNIT_MOD, UNIT_MOD_ATTACK_POWER);
    }

    float att_speed = float(GetBaseAttackTime(BASE_ATTACK)) / 1000.0f;

    float base_value  = GetModifierValue(unitMod, BASE_VALUE) + ((GetTotalAttackPowerValue(attType) + bonus) / coef * att_speed) + bonusDamage;
    float base_pct    = GetModifierValue(unitMod, BASE_PCT);
    float total_value = GetModifierValue(unitMod, TOTAL_VALUE);
    float total_pct   = GetModifierValue(unitMod, TOTAL_PCT);

    float weapon_mindamage = GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE);
    float weapon_maxdamage = GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE);

    float mindamage = ((base_value + weapon_mindamage) * base_pct + total_value) * total_pct;
    float maxdamage = ((base_value + weapon_maxdamage) * base_pct + total_value) * total_pct;

    SetStatFloatValue(UNIT_FIELD_MINDAMAGE, mindamage);
    SetStatFloatValue(UNIT_FIELD_MAXDAMAGE, maxdamage);
}

void Guardian::UpdateHitRating()
{
    if (ownerScaling)
    {
        // Melee - Hit
        m_modMeleeHitChance = ownerScaling->CalculateScaling(STAT_IDENTIFIER_COMBAT_RATING, CR_HIT_MELEE, GetOwnerCombatRating(CR_HIT_MELEE));

        // Ranged - Hit
        m_modRangedHitChance = ownerScaling->CalculateScaling(STAT_IDENTIFIER_COMBAT_RATING, CR_HIT_MELEE, GetOwnerCombatRating(CR_HIT_RANGED));

        // Spell - Hit
        m_modSpellHitChance = ownerScaling->CalculateScaling(STAT_IDENTIFIER_COMBAT_RATING, CR_HIT_MELEE, GetOwnerCombatRating(CR_HIT_SPELL));
    }
}

void Guardian::UpdateHasteRating()
{
    // Only pet which has an haste scaling aura
    if (IsPetGhoul())
        return;

    float hasteRates[3];
    memset(&hasteRates, 1.0f, sizeof(hasteRates));

    if (ownerScaling)
    {
        hasteRates[0] = ownerScaling->CalculateScaling(STAT_IDENTIFIER_COMBAT_RATING, CR_HASTE_MELEE, GetOwnerCombatRating(CR_HASTE_MELEE));
        hasteRates[1] = ownerScaling->CalculateScaling(STAT_IDENTIFIER_COMBAT_RATING, CR_HASTE_RANGED, GetOwnerCombatRating(CR_HASTE_RANGED));
        hasteRates[2] = ownerScaling->CalculateScaling(STAT_IDENTIFIER_COMBAT_RATING, CR_HASTE_SPELL, GetOwnerCombatRating(CR_HASTE_SPELL));
    }

    // Melee - Haste
    UpdateHaste(CTYPE_BASE, hasteRates[0]);
    UpdateHaste(CTYPE_OFF, hasteRates[0]);

    // Ranged - Haste
    UpdateHaste(CTYPE_RANGED, hasteRates[1]);

    // Spell - Haste
    UpdateHaste(CTYPE_CAST_TIME, hasteRates[2]);
    UpdateHaste(CTYPE_SPELL, hasteRates[2]);
}

void Guardian::SetBonusDamage(int32 damage)
{
    bonusSpellDamage = damage;
    UpdateVisualBonusDamage();
}

void Guardian::UpdateVisualBonusDamage()
{
    if (GetOwner()->GetTypeId() == TYPEID_PLAYER)
    {
        float bonusDamage = bonusSpellDamage;
        bonusDamage += GetTotalAuraModifier(SPELL_AURA_MOD_DAMAGE_DONE);
        GetOwner()->SetInt32Value(PLAYER_PET_SPELL_POWER, int32(bonusDamage));
    }
}
