// Regression fixture: actual UE metadata in synthetic remote memory. No game
// functions are mocked; Resolver walks the exact FName/FProperty/TMap graph.
#include "../ammo_core.h"
#include <cassert>
using namespace ammo;
#include "fixture.h"
int main(){
 Fixture f;auto base=f.cls("BaseWeapon"),range=f.cls("RangeWeapon",base),playerClass=f.cls("AHBaseCharacter"),slotClass=f.cls("WeaponSlotComponent");
 auto weapon=f.object(range,"Shotgun"),other=f.object(range,"Pistol"),player=f.object(playerClass,"Player"),slot=f.object(slotClass,"WeaponSlot");
 f.prop(playerClass,"CurrentActiveWeaponSlot","IntProperty",0x15F8,4);
 auto map=f.prop(playerClass,"CharacterSlotsComponents","MapProperty",0xBB0,80);
 auto key=f.prop(0,"Key","IntProperty",0,4),value=f.prop(0,"Value","ObjectProperty",8,8);
 f.m.put<Address>(map+0x78,key);f.m.put<Address>(map+0x80,value);
 f.prop(slotClass,"CurrentItem","ObjectProperty",0x30,8);f.m.put<Address>(slot+0x30,weapon);
 f.slotMap(player+0xBB0,{{7,slot}});f.m.put<std::int32_t>(player+0x15F8,0);
 // Regression for the reported log: active index 0, NO map key 0.
 // The old implementation fails; the new resolver follows CurrentItem.
 bool oldFailed=false;try{slotMapValue(f.m,player+0xBB0,4,0);}catch(const std::exception&){oldFailed=true;}assert(oldFailed);
 Resolver resolver(f.m,f.pool);std::ostringstream trace;int before=f.m.writes;
 assert(resolver.selectedWeapon(player,&trace)==weapon);assert(f.m.writes==before);
 assert(trace.str().find("key=7")!=std::string::npos);assert(trace.str().find("value=0")!=std::string::npos);assert(resolver.lastRoute.find("CurrentItem")!=std::string::npos);
 // Changing only the active item pointer changes the result immediately.
 f.m.put<Address>(slot+0x30,other);assert(resolver.selectedWeapon(player)==other);
 // No guessing when two slot components both claim a current weapon.
 auto slot2=f.object(slotClass,"OtherSlot");f.m.put<Address>(slot2+0x30,weapon);f.slotMap(player+0xBB0,{{7,slot},{8,slot2}});
 bool ambiguous=false;try{resolver.selectedWeapon(player);}catch(const ResolutionFailure&){ambiguous=true;}assert(ambiguous);
 // A temporary null/holstered item is an explicitly retryable selection failure.
 f.slotMap(player+0xBB0,{{7,slot}});f.m.put<Address>(slot+0x30,0);
 bool missing=false;try{resolver.selectedWeapon(player);}catch(const ResolutionFailure&){missing=true;}assert(missing);
 // Positive ranged/melee identification; a zero generic count is not sufficient
 // authorization to seed an unknown weapon type.
 auto melee=f.cls("MeleeWeapon",base);auto axe=f.object(melee,"Axe");assert(resolver.melee(axe));assert(!resolver.melee(weapon));
 assert(resolver.reflection.isA(weapon,"RangeWeapon"));assert(!resolver.reflection.isA(axe,"RangeWeapon"));
 // The refill class must come from an existing, validated ClassProperty.
 auto metaClass=f.cls("Class");auto bulletClass=f.cls("AHBullet");f.m.put<Address>(bulletClass+0x10,metaClass);
 f.prop(range,"Bullet","ClassProperty",0x80,8);f.m.put<Address>(weapon+0x80,bulletClass);
 Resolver updated(f.m,f.pool);assert(updated.bulletClass(weapon,0)==bulletClass);
 f.m.put<Address>(weapon+0x80,3);assert(updated.bulletClass(weapon,0)==0);
 // Exact candidate regression observed in the user's V2 log: the visible
 // shotgun had rank 1, while an invisible axe received a wrong array-index 3.
 assert(chooseWeapon({{0x27B6194D9C0ULL,1,true,"player.Children[0]"},
                      {0x27B4D03A060ULL,0,false,"player.Children[1]"},
                      {0x27B4D03A060ULL,3,false,"Equipment.Items[1]"}})==0x27B6194D9C0ULL);
 bool twoVisible=false;try{chooseWeapon({{weapon,1,true,"a"},{other,5,true,"b"}});}catch(const ResolutionFailure&){twoVisible=true;}assert(twoVisible);
 // Complete external barrel-state transaction, including a zero-ammo/click
 // state. Merely showing count+chamber=2 is not sufficient in this fixture.
 auto barrelClass=f.cls("EBBarrel",0,0x800),barrelObject=f.object(barrelClass,"Barrel");
 auto addBool=[&](const std::string& name,int offset){auto p=f.prop(barrelClass,name,"BoolProperty",offset,1);f.m.put<std::uint8_t>(p+0x79,0);f.m.put<std::uint8_t>(p+0x7A,1);f.m.put<std::uint8_t>(p+0x7B,255);};
 addBool("ShootingBlocked",0x519);addBool("CycleAmmoUnlimited",0x531);addBool("LoadNext",0x570);
 f.prop(barrelClass,"CycleAmmoCount","IntProperty",0x548,4);f.prop(barrelClass,"CycleAmmoPos","IntProperty",0x54C,4);
 f.prop(barrelClass,"ChamberedBullet","ClassProperty",0x560,8);
 f.m.put<Address>(weapon+0x80,bulletClass);f.m.put<std::uint8_t>(barrelObject+0x519,1);f.m.put<std::int32_t>(barrelObject+0x54C,-1);
 Resolver fireResolver(f.m,f.pool);std::ostringstream fireLog;
 auto patch=makeBarrelPatch(fireResolver,f.m,weapon,barrelObject,fireLog);
 assert(f.m.read<std::uint8_t>(barrelObject+0x519)==1); // no changes before apply
 patch.apply(f.m);
 assert(f.m.read<std::int32_t>(barrelObject+0x548)==1);
 assert(f.m.read<Address>(barrelObject+0x560)==bulletClass);
 assert(f.m.read<std::uint8_t>(barrelObject+0x519)==0); // actual block released
 assert(f.m.read<std::uint8_t>(barrelObject+0x570)==1); // next-round state enabled
 assert(f.m.read<std::uint8_t>(barrelObject+0x531)==1);
 assert(f.m.read<std::int32_t>(barrelObject+0x54C)==0);
 assert(patch.restore(f.m));
 assert(f.m.read<std::uint8_t>(barrelObject+0x519)==1);
 assert(f.m.read<std::uint8_t>(barrelObject+0x570)==0);
 assert(f.m.read<std::int32_t>(barrelObject+0x54C)==-1);
 assert(f.m.read<Address>(barrelObject+0x560)==0);
 assert(f.m.read<std::int32_t>(barrelObject+0x548)==0);
 bool meleeSeed=false;try{makeBarrelPatch(fireResolver,f.m,axe,barrelObject,fireLog);}catch(const std::exception&){meleeSeed=true;}assert(meleeSeed);
 std::cout<<"PASS: index != map key regression, live item swap, read-only selection, full diagnostics, ambiguity refusal, transient null, melee/range classification, visible-shotgun regression, complete blocked/load-next/chamber cycle transaction.\n";
}
