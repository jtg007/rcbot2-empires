// Unit tests for research- and budget-aware vehicle loadout selection (bot_empires_vehicle_loadout.h).
#include "bot_empires_vehicle_loadout.h"

#include <cstdlib>
#include <iostream>

using namespace EmpiresCommanderEconomy;
using namespace EmpiresVehicleLoadout;

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition)
	{
		std::cerr << "FAIL: " << message << "\n";
		std::exit(1);
	}
}
}

int main()
{
	bool none[kResearchItemCount] = {};

	// 1. Unresearched baseline matches legacy hardcoded purchases exactly.
	{
		const Loadout nfLight = ChoosePurchaseLoadout(true, false, none, -1.0f);
		Require(nfLight.chassis == 2 && nfLight.cannonSlot == 0 && nfLight.cannonWeapon == 12 &&
			nfLight.mgSlot == 1 && nfLight.mgWeapon == 0 && nfLight.armorType == 0 &&
			nfLight.armorLevels[0] == 2 && nfLight.engine == 13 && nfLight.estimatedCost == 274 &&
			nfLight.totalWeight <= nfLight.maxWeight, "VL-1 NF unresearched light tank baseline");

		const Loadout beAfv = ChoosePurchaseLoadout(false, false, none, -1.0f);
		Require(beAfv.chassis == 10 && beAfv.cannonSlot == 1 && beAfv.cannonWeapon == 12 &&
			beAfv.mgSlot == 0 && beAfv.mgWeapon == 0 && beAfv.armorType == 0 &&
			beAfv.armorLevels[0] == 2 && beAfv.engine == 13 && beAfv.estimatedCost == 224 &&
			beAfv.totalWeight <= beAfv.maxWeight, "VL-2 BE unresearched AFV baseline");

		const Loadout nfJeep = ChoosePurchaseLoadout(true, true, none, -1.0f);
		const Loadout beJeep = ChoosePurchaseLoadout(false, true, none, -1.0f);
		Require(nfJeep.chassis == 0 && nfJeep.engine == 1 && nfJeep.cannonSlot == -1 && nfJeep.isJeep(),
			"VL-3 NF jeep baseline");
		Require(beJeep.chassis == 8 && beJeep.engine == 0 && beJeep.cannonSlot == -1 && beJeep.isJeep(),
			"VL-4 BE jeep baseline");
	}

	// 2. Medium Tank researched -> picks medium chassis with slot 0 cannon, slot 1 Med Chain Gun.
	{
		bool med[kResearchItemCount] = {};
		med[kResearchMediumTank] = true;
		const Loadout nfMed = ChoosePurchaseLoadout(true, false, med, 600.0f);
		Require(nfMed.chassis == 3 && nfMed.cannonSlot == 0 && nfMed.mgSlot == 1 &&
			nfMed.mgWeapon == kWeaponMedChainGun && nfMed.engine == kEngineMedStd &&
			nfMed.armorLevels[0] == 4 && nfMed.totalWeight <= nfMed.maxWeight,
			"VL-5 NF medium tank");

		const Loadout beMed = ChoosePurchaseLoadout(false, false, med, 600.0f);
		Require(beMed.chassis == 11 && beMed.cannonSlot == 0 && beMed.mgSlot == 1 &&
			beMed.mgWeapon == kWeaponMedChainGun && beMed.engine == kEngineMedStd &&
			beMed.totalWeight <= beMed.maxWeight,
			"VL-6 BE medium tank");

		// Low budget falls back to light tank.
		const Loadout fallback = ChoosePurchaseLoadout(true, false, med, 300.0f);
		Require(fallback.chassis == 2, "VL-7 tight budget falls back from medium to light");
	}

	// 3. Heavy Tank + Composite + Gas Turbine -> full endgame build; BE Heavy uses slot 2 for MG.
	{
		bool full[kResearchItemCount] = {};
		full[kResearchMediumTank] = true;
		full[kResearchHeavyTank] = true;
		full[kResearchCompositeArmor] = true;
		full[kResearchGasTurbineEngine] = true;

		const Loadout nfHeavy = ChoosePurchaseLoadout(true, false, full, 2500.0f);
		Require(nfHeavy.chassis == 5 && nfHeavy.cannonSlot == 0 && nfHeavy.mgSlot == 1 &&
			nfHeavy.armorType == kArmorComposite && nfHeavy.engine == kEngineHeavyGas &&
			nfHeavy.totalWeight <= nfHeavy.maxWeight,
			"VL-8 NF heavy full tech");

		const Loadout beHeavy = ChoosePurchaseLoadout(false, false, full, 2500.0f);
		Require(beHeavy.chassis == 13 && beHeavy.cannonSlot == 0 && beHeavy.mgSlot == 2 &&
			beHeavy.armorType == kArmorComposite && beHeavy.engine == kEngineHeavyGas &&
			beHeavy.totalWeight <= beHeavy.maxWeight,
			"VL-9 BE heavy full tech (MG in slot 2)");

		// Mid budget with full tech: buys Medium + Gas Turbine + Composite instead of unaffordable Heavy.
		const Loadout mid = ChoosePurchaseLoadout(true, false, full, 950.0f);
		Require(mid.chassis == 3 && mid.engine == kEngineMedGas && mid.armorType == kArmorComposite,
			"VL-10 mid budget buys upgraded medium instead of unaffordable heavy");
	}

	// 4. Pad refit preserves chassis and respects allowUpgrades & budget.
	{
		bool tech[kResearchItemCount] = {};
		tech[kResearchCompositeArmor] = true;
		tech[kResearchGasTurbineEngine] = true;

		const Loadout freeRefit = ChooseRefitLoadout(false, 10, tech, false, 1000.0f);
		Require(freeRefit.chassis == 10 && freeRefit.armorType == kArmorPlain && freeRefit.engine == kEngineLightStd,
			"VL-11 ammo/free refit keeps plain+std");

		const Loadout upRefit = ChooseRefitLoadout(false, 10, tech, true, 500.0f);
		Require(upRefit.chassis == 10 && upRefit.armorType == kArmorComposite && upRefit.engine == kEngineLightGas,
			"VL-12 upgraded refit applies composite+gas on existing chassis");

		// Medium and Heavy chassis are refittable and preserve their chassis & slot layout.
		Require(IsRefittableTankChassis(2) && IsRefittableTankChassis(3) && IsRefittableTankChassis(5) &&
			IsRefittableTankChassis(10) && IsRefittableTankChassis(11) && IsRefittableTankChassis(13) &&
			!IsRefittableTankChassis(0) && !IsRefittableTankChassis(1) &&
			!IsRefittableTankChassis(8) && !IsRefittableTankChassis(9),
			"VL-13 IsRefittableTankChassis covers all 6 tank chassis only");

		const Loadout nfMedRefit = ChooseRefitLoadout(true, 3, tech, false, 1000.0f);
		Require(nfMedRefit.chassis == 3 && nfMedRefit.cannonSlot == 0 && nfMedRefit.mgSlot == 1 &&
			nfMedRefit.armorType == kArmorPlain && nfMedRefit.engine == kEngineMedStd &&
			nfMedRefit.totalWeight <= nfMedRefit.maxWeight,
			"VL-14 NF medium ammo refit");

		const Loadout beHeavyRefit = ChooseRefitLoadout(false, 13, tech, true, 1500.0f);
		Require(beHeavyRefit.chassis == 13 && beHeavyRefit.cannonSlot == 0 && beHeavyRefit.mgSlot == 2 &&
			beHeavyRefit.armorType == kArmorComposite && beHeavyRefit.engine == kEngineHeavyGas &&
			beHeavyRefit.totalWeight <= beHeavyRefit.maxWeight,
			"VL-15 BE heavy upgraded refit");
	}

	// 5. Exhaustive weight invariant across all tech & team combinations.
	for (int team = 0; team < 2; ++team)
	{
		for (int mask = 0; mask < 16; ++mask)
		{
			bool tech[kResearchItemCount] = {};
			tech[kResearchMediumTank] = (mask & 1) != 0;
			tech[kResearchHeavyTank] = (mask & 2) != 0;
			tech[kResearchCompositeArmor] = (mask & 4) != 0;
			tech[kResearchGasTurbineEngine] = (mask & 8) != 0;
			for (float budget : {-1.0f, 250.0f, 500.0f, 1000.0f, 2500.0f})
			{
				const Loadout l = ChoosePurchaseLoadout(team == 0, false, tech, budget);
				Require(l.totalWeight <= l.maxWeight, "VL-INV weight within chassis max");
				Require(l.cannonSlot >= 0 && l.mgSlot >= 0 && l.cannonSlot != l.mgSlot,
					"VL-INV distinct valid weapon slots");
			}
		}
	}

	std::cout << "PASS vehicle loadout policy\n";
	return 0;
}
