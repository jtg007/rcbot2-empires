#ifndef RCBOT_EMPIRES_VEHICLE_LOADOUT_H
#define RCBOT_EMPIRES_VEHICLE_LOADOUT_H

#include "bot_empires_commander_economy.h"

// Research- and budget-aware vehicle loadout selection (TODO 5.2).
// Engine-free so it can be unit tested (bot_empires_vehicle_loadout_test.cpp).
// Part indices, weights, costs, and weapon_slot layouts come from Empires 2.39
// scripts/vehicle_*.txt (scripts_000.vpk).
namespace EmpiresVehicleLoadout
{
// Armor indices (scripts/vehicle_armor.txt).
constexpr int kArmorPlain = 0;     // cost 8/plate, wt 15/plate, no research
constexpr int kArmorComposite = 1; // cost 40/plate, wt 15/plate, Composite_Armor (34)

// Weapon indices (scripts/vehicle_weapons.txt).
constexpr int kWeaponStdMg = 0;        // type 0, size 1, cost 30, wt 30, no research
constexpr int kWeaponMedChainGun = 7;  // type 0, size 2, cost 60, wt 50, no research
constexpr int kWeaponStdCannon = 12;   // type 1, size 1, cost 50, wt 50, no research

// Engine indices (scripts/vehicle_engines.txt).
constexpr int kEngineImpJeep = 0;     // type 4, team IMP, cost 10, wt 10
constexpr int kEngineNfJeep = 1;      // type 4, team NF,  cost 10, wt 10
constexpr int kEngineApcStd = 7;      // type 3, APC Standard, cost 20, wt 30
constexpr int kEngineLightStd = 13;   // type 5, cost 20, wt 30
constexpr int kEngineLightGas = 14;   // type 5, cost 75, wt 20, Gas_Turbine_Engine (35)
constexpr int kEngineMedStd = 19;     // type 6, cost 30, wt 30
constexpr int kEngineMedGas = 20;     // type 6, cost 90, wt 20, Gas_Turbine_Engine (35)
constexpr int kEngineHeavyStd = 25;   // type 7, cost 40, wt 30
constexpr int kEngineHeavyGas = 26;   // type 7, cost 150, wt 20, Gas_Turbine_Engine (35)

struct Loadout
{
	int chassis = 2;
	int cannonSlot = 0;       // -1 when none (jeep)
	int cannonWeapon = kWeaponStdCannon;
	int mgSlot = 1;           // -1 when none (jeep)
	int mgWeapon = kWeaponStdMg;
	int armorType = kArmorPlain;
	int armorLevels[4] = {2, 2, 2, 2}; // front, rear, left, right
	int engine = kEngineLightStd;
	int estimatedCost = 0;
	int totalWeight = 0;
	int maxWeight = 260;

	bool isJeep() const { return chassis == 0 || chassis == 8; }
	bool isApc() const { return chassis == 1 || chassis == 9; }
};

struct ChassisProfile
{
	int chassis;
	int chassisCost;
	int maxWeight;
	int cannonSlot;
	int mgSlot;
	int mgWeapon;
	int mgCost;
	int mgWeight;
	int armorLevels[4];
	int stdEngine;
	int stdEngineCost;
	int gasEngine;
	int gasEngineCost;
};

inline ChassisProfile ProfileForChassis(bool nf, int chassis)
{
	// NF: 2 Light, 3 Med, 5 Heavy. BE: 10 AFV, 11 Med, 13 Heavy.
	// Slot types from scripts/vehicle_*.txt:
	//   NF Light (2):  slot 0 cannon (s1), slot 1 MG (s1)
	//   BE AFV   (10): slot 0 MG (s1),     slot 1 cannon (s1)
	//   NF Med   (3):  slot 0 cannon (s2), slot 1 MG (s2)
	//   BE Med   (11): slot 0 cannon (s2), slot 1 MG (s2)
	//   NF Heavy (5):  slot 0 cannon (s3), slot 1 MG (s3)
	//   BE Heavy (13): slot 0 cannon (s3), slot 1 cannon (s3), slot 2 MG (s3)
	if (chassis == 5)
		return {5, 820, 580, 0, 1, kWeaponMedChainGun, 60, 50, {6, 4, 5, 5}, kEngineHeavyStd, 40, kEngineHeavyGas, 150};
	if (chassis == 13)
		return {13, 850, 550, 0, 2, kWeaponMedChainGun, 60, 50, {6, 4, 5, 5}, kEngineHeavyStd, 40, kEngineHeavyGas, 150};
	if (chassis == 3)
		return {3, 170, 435, 0, 1, kWeaponMedChainGun, 60, 50, {4, 3, 3, 3}, kEngineMedStd, 30, kEngineMedGas, 90};
	if (chassis == 11)
		return {11, 220, 380, 0, 1, kWeaponMedChainGun, 60, 50, {4, 3, 3, 3}, kEngineMedStd, 30, kEngineMedGas, 90};
	if (chassis == 10 || (!nf && chassis != 2))
		return {10, 60, 320, 1, 0, kWeaponStdMg, 30, 30, {2, 2, 2, 2}, kEngineLightStd, 20, kEngineLightGas, 75};
	return {2, 110, 260, 0, 1, kWeaponStdMg, 30, 30, {2, 2, 2, 2}, kEngineLightStd, 20, kEngineLightGas, 75};
}

inline Loadout MakeJeepLoadout(bool nf)
{
	Loadout out;
	out.chassis = nf ? 0 : 8;
	out.cannonSlot = -1;
	out.cannonWeapon = -1;
	out.mgSlot = -1;
	out.mgWeapon = -1;
	out.armorType = kArmorPlain;
	out.armorLevels[0] = out.armorLevels[1] = out.armorLevels[2] = out.armorLevels[3] = 0;
	out.engine = nf ? kEngineNfJeep : kEngineImpJeep;
	out.estimatedCost = (nf ? 50 : 40) + 10;
	out.totalWeight = 10;
	out.maxWeight = 28;
	return out;
}

inline Loadout MakeApcLoadout(bool nf)
{
	// NF APC (1): cost 200, maxWeight 265; BE APC (9): cost 240, maxWeight 265.
	// Slot 0 = T0/S3 (MG), engine 7 = APC Standard.
	Loadout out;
	out.chassis = nf ? 1 : 9;
	out.cannonSlot = -1;
	out.cannonWeapon = -1;
	out.mgSlot = 0;
	out.mgWeapon = kWeaponMedChainGun;
	out.armorType = kArmorPlain;
	out.armorLevels[0] = out.armorLevels[1] = out.armorLevels[2] = out.armorLevels[3] = 2;
	out.engine = kEngineApcStd;
	out.estimatedCost = (nf ? 200 : 240) + 60 + 20 + 8 * 8;
	out.totalWeight = 50 + 30 + 8 * 15;
	out.maxWeight = 265;
	return out;
}

inline Loadout BuildTankLoadout(const ChassisProfile& p, bool useGasEngine, bool useCompositeArmor)
{
	Loadout out;
	out.chassis = p.chassis;
	out.cannonSlot = p.cannonSlot;
	out.cannonWeapon = kWeaponStdCannon;
	out.mgSlot = p.mgSlot;
	out.mgWeapon = p.mgWeapon;
	out.armorType = useCompositeArmor ? kArmorComposite : kArmorPlain;
	int plates = 0;
	for (int i = 0; i < 4; ++i)
	{
		out.armorLevels[i] = p.armorLevels[i];
		plates += p.armorLevels[i];
	}
	out.engine = useGasEngine ? p.gasEngine : p.stdEngine;
	const int engineCost = useGasEngine ? p.gasEngineCost : p.stdEngineCost;
	const int engineWeight = useGasEngine ? 20 : 30;
	const int armorPlateCost = useCompositeArmor ? 40 : 8;
	out.estimatedCost = p.chassisCost + 50 + p.mgCost + engineCost + plates * armorPlateCost;
	out.totalWeight = 50 + p.mgWeight + engineWeight + plates * 15;
	out.maxWeight = p.maxWeight;
	return out;
}

// Choose the best affordable chassis and parts for a new purchase.
// researchDone may be nullptr (treated as no research unlocked).
// availableBudget < 0 means unlimited (e.g. debug buy or unknown team pool).
inline Loadout ChoosePurchaseLoadout(bool nf, bool wantJeep,
	const bool researchDone[EmpiresCommanderEconomy::kResearchItemCount], float availableBudget,
	bool wantApc = false)
{
	if (wantJeep)
		return MakeJeepLoadout(nf);
	if (wantApc)
	{
		const Loadout apc = MakeApcLoadout(nf);
		if (availableBudget < 0.0f || static_cast<float>(apc.estimatedCost) <= availableBudget)
			return apc;
	}

	const bool hasMed = researchDone != nullptr && researchDone[EmpiresCommanderEconomy::kResearchMediumTank];
	const bool hasHeavy = researchDone != nullptr && researchDone[EmpiresCommanderEconomy::kResearchHeavyTank];
	const bool hasComp = researchDone != nullptr && researchDone[EmpiresCommanderEconomy::kResearchCompositeArmor];
	const bool hasGas = researchDone != nullptr && researchDone[EmpiresCommanderEconomy::kResearchGasTurbineEngine];
	const bool unlimited = availableBudget < 0.0f;

	// Pick the highest researched chassis whose base build fits the budget.
	ChassisProfile chosen = ProfileForChassis(nf, nf ? 2 : 10);
	if (hasHeavy)
	{
		const ChassisProfile heavy = ProfileForChassis(nf, nf ? 5 : 13);
		const Loadout baseHeavy = BuildTankLoadout(heavy, false, false);
		if (unlimited || static_cast<float>(baseHeavy.estimatedCost) <= availableBudget)
			chosen = heavy;
	}
	if (chosen.chassis != (nf ? 5 : 13) && hasMed)
	{
		const ChassisProfile med = ProfileForChassis(nf, nf ? 3 : 11);
		const Loadout baseMed = BuildTankLoadout(med, false, false);
		if (unlimited || static_cast<float>(baseMed.estimatedCost) <= availableBudget)
			chosen = med;
	}

	// Layer researched engine and armor upgrades when affordable.
	Loadout current = BuildTankLoadout(chosen, false, false);
	bool useGas = false;
	if (hasGas)
	{
		const Loadout withGas = BuildTankLoadout(chosen, true, false);
		if (unlimited || static_cast<float>(withGas.estimatedCost) <= availableBudget)
		{
			useGas = true;
			current = withGas;
		}
	}
	if (hasComp)
	{
		const Loadout withComp = BuildTankLoadout(chosen, useGas, true);
		if (unlimited || static_cast<float>(withComp.estimatedCost) <= availableBudget)
			current = withComp;
	}
	return current;
}

// Choose a refit loadout for an existing vehicle on a repair pad (chassis cannot change).
// When allowUpgrades is false (e.g. ammo-only trip or tight funds), stays on plain/standard
// so the refit costs 0 difference against a base vehicle.
inline Loadout ChooseRefitLoadout(bool nf, int currentChassis,
	const bool researchDone[EmpiresCommanderEconomy::kResearchItemCount], bool allowUpgrades, float availableBudget)
{
	if (currentChassis == 0 || currentChassis == 8)
		return MakeJeepLoadout(currentChassis == 0);
	if (currentChassis == 1 || currentChassis == 9)
		return MakeApcLoadout(currentChassis == 1);

	const ChassisProfile p = ProfileForChassis(nf, currentChassis);
	const Loadout base = BuildTankLoadout(p, false, false);
	if (!allowUpgrades || researchDone == nullptr)
		return base;

	const bool hasComp = researchDone[EmpiresCommanderEconomy::kResearchCompositeArmor];
	const bool hasGas = researchDone[EmpiresCommanderEconomy::kResearchGasTurbineEngine];
	const bool unlimited = availableBudget < 0.0f;

	bool useGas = false;
	bool useComp = false;
	if (hasGas)
	{
		const Loadout withGas = BuildTankLoadout(p, true, false);
		const int delta = withGas.estimatedCost - base.estimatedCost;
		if (unlimited || static_cast<float>(delta) <= availableBudget)
			useGas = true;
	}
	if (hasComp)
	{
		const Loadout withComp = BuildTankLoadout(p, useGas, true);
		const int delta = withComp.estimatedCost - base.estimatedCost;
		if (unlimited || static_cast<float>(delta) <= availableBudget)
			useComp = true;
	}
	return BuildTankLoadout(p, useGas, useComp);
}
} // namespace EmpiresVehicleLoadout

#endif
