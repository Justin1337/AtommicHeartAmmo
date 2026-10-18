#include "../runtime_scan.h"
#include "fixture.h"
#include <cassert>
using namespace ammo;
template<class F>void rejects(F action){bool failed=false;try{action();}catch(const std::exception&){failed=true;}assert(failed);}
int main(){
 Fixture f;
 // All gameplay offsets intentionally differ from the supplied original dump.
 // The only unchanged values are bootstrap UE reflection metadata layouts.
 auto wc=f.cls("World"),gc=f.cls("GameInstance"),lc=f.cls("LocalPlayer"),cc=f.cls("PlayerController"),pc=f.cls("AHBaseCharacter");
 auto ac=f.cls("AHBaseCharacterAttributeSet"),attributeStruct=f.cls("GameplayAttributeData",0,0x40);
 auto world=f.object(wc,"World"),instance=f.object(gc,"GameInstance"),local=f.object(lc,"LocalPlayer"),controller=f.object(cc,"Controller"),pawn=f.object(pc,"Player"),attributes=f.object(ac,"Attributes");
 auto slot=f.alloc(8);f.m.put<Address>(slot,world);
 f.prop(wc,"OwningGameInstance","ObjectProperty",0x70,8);f.m.put<Address>(world+0x70,instance);
 auto players=f.prop(gc,"LocalPlayers","ArrayProperty",0x88,16),inner=f.prop(0,"Inner","ObjectProperty",0,8);f.m.put<Address>(players+0x78,inner);
 auto array=f.alloc(8);f.m.put<Address>(array,local);f.m.put<Address>(instance+0x88,array);f.m.put<std::int32_t>(instance+0x90,1);f.m.put<std::int32_t>(instance+0x94,1);
 f.prop(lc,"PlayerController","ObjectProperty",0x68,8);f.m.put<Address>(local+0x68,controller);
 f.prop(cc,"AcknowledgedPawn","ObjectProperty",0x98,8);f.m.put<Address>(controller+0x98,pawn);
 auto death=f.prop(pc,"bIsDead","BoolProperty",0xE0,1);f.m.put<std::uint8_t>(death+0x79,0);f.m.put<std::uint8_t>(death+0x7A,4);f.m.put<std::uint8_t>(death+0x7B,4);
 f.prop(pc,"AttributeSet","ObjectProperty",0xF0,8);f.m.put<Address>(pawn+0xF0,attributes);
 f.prop(attributeStruct,"CurrentValue","FloatProperty",0x24,4);
 auto max=f.prop(ac,"MaxEnergy","StructProperty",0x100,0x40),energy=f.prop(ac,"Energy","StructProperty",0x180,0x40);
 f.m.put<Address>(max+0x78,attributeStruct);f.m.put<Address>(energy+0x78,attributeStruct);f.m.put<float>(attributes+0x124,75);f.m.put<float>(attributes+0x1A4,10);
 f.prop(pc,"CurrentActiveWeaponSlot","IntProperty",0xA0,4);f.m.put<std::int32_t>(pawn+0xA0,2);
 auto base=f.cls("BaseWeapon"),range=f.cls("RangeWeapon",base),weapon=f.object(range,"Gun");
 f.prop(pc,"CurrentWeapon","ObjectProperty",0xB0,8);f.m.put<Address>(pawn+0xB0,weapon);
 Resolver r(f.m,f.pool);RuntimeObjects runtime(f.m,r,slot);int before=f.m.writes;
 auto live=runtime.player();assert(live.world==world&&live.player==pawn);assert(runtime.attributes(pawn)==attributes);
 assert(!runtime.dead(pawn));assert(runtime.currentFloat(attributes,"Energy")==attributes+0x1A4);assert(runtime.currentFloat(attributes,"MaxEnergy")==attributes+0x124);
 std::ostringstream trace;assert(r.selectedWeapon(pawn,&trace)==weapon);assert(trace.str().find("Active weapon index=2")!=std::string::npos);assert(f.m.writes==before);
 f.m.put<std::uint8_t>(pawn+0xE0,4);assert(runtime.dead(pawn));
 // Empty and null objects are waitable, whereas bad types/arrays fail closed.
 f.m.put<Address>(world+0x70,0);rejects([&]{runtime.player();});f.m.put<Address>(world+0x70,instance);
 f.m.put<std::int32_t>(instance+0x90,0);rejects([&]{runtime.player();});f.m.put<std::int32_t>(instance+0x90,1);
 f.m.put<std::int32_t>(instance+0x94,0);rejects([&]{runtime.player();});f.m.put<std::int32_t>(instance+0x94,1);
 f.m.put<Address>(controller+0x98,0);
 f.prop(cc,"Pawn","ObjectProperty",0xC0,8);f.m.put<Address>(controller+0xC0,pawn);
 Resolver newResolver(f.m,f.pool);RuntimeObjects fallback(f.m,newResolver,slot);assert(fallback.player().player==pawn);
 f.m.put<Address>(slot,0);rejects([&]{runtime.player();});f.m.put<Address>(slot,weapon);rejects([&]{runtime.player();});f.m.put<Address>(slot,world);
 // A class-field offset can shift on a fresh process/update without touching
 // any original header. Its newly reflected offset is used by a fresh resolver.
 f.m.put<std::int32_t>(energy+0x4C,0x1D0);f.m.put<float>(attributes+0x1F4,25);
 Resolver afterUpdate(f.m,f.pool);RuntimeObjects updated(f.m,afterUpdate,slot);
 assert(updated.currentFloat(attributes,"Energy")==attributes+0x1F4);
 std::cout<<"PASS: shifted live player/attribute/death/index offsets, shifted nested floats, reflected pawn fallback, no writes during resolution, null/invalid chain gates.\n";
}
