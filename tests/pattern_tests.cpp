#include "../runtime_scan.h"
#include "fixture.h"
#include <cassert>
using namespace ammo;
template<class F>void rejects(F action){bool caught=false;try{action();}catch(const std::exception&){caught=true;}assert(caught);}
struct PEFixture:Fixture{
 Address imageBase;std::uint32_t imageSize=0x9000;Address code,data,worldSlot,worldObject;
 PEFixture(Address base=0x140000000ULL,std::uint32_t dataRva=0x1000,std::uint32_t codeRva=0x4000,std::uint32_t stamp=123):imageBase(base){
  // A little-endian PE32+ image with writable data BEFORE executable code.
  // Thus RIP displacements here are negative, not only common positive values.
  m.zero(base,0x400);m.zero(base+dataRva,0x2000);m.zero(base+codeRva,0x3000);
  m.put<std::uint16_t>(base,0x5A4D);m.put<std::int32_t>(base+0x3C,0x80);
  Address nt=base+0x80;m.put<std::uint32_t>(nt,0x4550);m.put<std::uint16_t>(nt+4,0x8664);m.put<std::uint16_t>(nt+6,2);m.put<std::uint32_t>(nt+8,stamp);m.put<std::uint16_t>(nt+20,0xF0);
  Address opt=nt+24;m.put<std::uint16_t>(opt,0x20B);m.put<std::uint32_t>(opt+56,imageSize);m.put<std::uint32_t>(opt+60,0x400);
  Address section=nt+24+0xF0;m.put<std::uint32_t>(section+8,0x2000);m.put<std::uint32_t>(section+12,dataRva);m.put<std::uint32_t>(section+36,0xC0000040);
  section+=40;m.put<std::uint32_t>(section+8,0x3000);m.put<std::uint32_t>(section+12,codeRva);m.put<std::uint32_t>(section+36,0x60000020);
  data=base+dataRva;code=base+codeRva;pool=data+0x100;worldSlot=data+0x300;m.put<Address>(pool+0x10,names);
  // validNamePool inspects entry zero; retain nonzero hash bits in its header.
  auto scratch=alloc();fname(scratch,"None");m.put<std::uint16_t>(names,(4<<6)|0x16);
  auto worldClass=cls("World");prop(worldClass,"OwningGameInstance","ObjectProperty",0x70,8);
  worldObject=object(worldClass,"World_0");m.put<Address>(worldSlot,worldObject);
 }
 void pattern(Address at,bool world,Address target){
  std::vector<std::uint8_t> bytes=world?std::vector<std::uint8_t>{0x48,0x89,0x15,0,0,0,0,0x48,0x8D,0x05,0,0,0,0,0xC3}:std::vector<std::uint8_t>{0x48,0x8D,0x0D,0,0,0,0,0xE8,0,0,0,0,0xC6,0x05};
  auto delta=std::int64_t(target)-std::int64_t(at+7);assert(delta>=INT32_MIN&&delta<=INT32_MAX);auto displacement=static_cast<std::int32_t>(delta);std::memcpy(bytes.data()+3,&displacement,4);
  m.write(at,bytes.data(),bytes.size());
 }
 Image image(){return Image::parse(m,imageBase,imageSize);}
};
int main(){
 PEFixture f;f.pattern(f.code+4090,false,f.pool);f.pattern(f.code+8200,true,f.worldSlot);
 int before=f.m.writes;auto parsed=f.image();auto raw=scanGlobals(f.m,parsed,4096);auto globals=validateGlobals(f.m,raw);
 assert(globals.names==f.pool&&globals.worldSlot==f.worldSlot&&f.m.writes==before);
 assert(raw.stats.bytes==0x3000&&raw.stats.unreadablePages==0);
 // Multiple callsites to one target are fine; data-section decoys are ignored.
 f.pattern(f.code+100,false,f.pool);f.pattern(f.data+0x800,true,f.data+0x900);
 auto repeated=scanGlobals(f.m,f.image(),4096);assert(repeated.names.size()==1&&repeated.worlds.size()==1);
 // Invalid pool decoys are rejected after matching. A second VALID pool must
 // stop bootstrap, rather than select the first hit or use an old fixed RVA.
 Address fake=f.data+0xA00;f.pattern(f.code+200,false,fake);auto decoy=scanGlobals(f.m,f.image());assert(validateGlobals(f.m,decoy).names==f.pool);
 f.m.put<Address>(fake+0x10,f.names);auto ambiguous=scanGlobals(f.m,f.image());rejects([&]{validateGlobals(f.m,ambiguous);});f.m.put<Address>(fake+0x10,0);
 // Different module base, PE timestamp, code placement and globals placement:
 // no old offset header, image size or timestamp participates in resolution.
 PEFixture shifted(0x7FF700000000ULL,0x2000,0x5000,0xDEADBEEF);
 shifted.imageSize=0xA000;shifted.m.put<std::uint32_t>(shifted.imageBase+0x80+24+56,shifted.imageSize);
 shifted.pattern(shifted.code+32,false,shifted.pool);shifted.pattern(shifted.code+64,true,shifted.worldSlot);
 auto moved=validateGlobals(shifted.m,scanGlobals(shifted.m,shifted.image()));assert(moved.names==shifted.pool&&moved.worldSlot==shifted.worldSlot);
 assert(shifted.image().timestamp==0xDEADBEEF);
 // Positive RIP displacement: writable data AFTER code instead.
 PEFixture positive(0x180000000ULL,0x5000,0x1000,999);positive.pattern(positive.code+33,false,positive.pool);positive.pattern(positive.code+88,true,positive.worldSlot);
 assert(validateGlobals(positive.m,scanGlobals(positive.m,positive.image())).names==positive.pool);
 // Changed instruction bytes / incompatible signature => reject, no fallback.
 PEFixture missing;missing.pattern(missing.code+64,false,missing.pool);rejects([&]{validateGlobals(missing.m,scanGlobals(missing.m,missing.image()));});
 // Readable pages on each side of an unreadable page must never be concatenated.
 PEFixture gap;gap.pattern(gap.code+4090,false,gap.pool);gap.pattern(gap.code+8300,false,gap.pool);gap.pattern(gap.code+8400,true,gap.worldSlot);
 for(Address a=gap.code+4096;a<gap.code+8192;++a)gap.m.memory.erase(a);
 auto recovered=scanGlobals(gap.m,gap.image(),65536);assert(recovered.stats.unreadablePages==1);assert(recovered.names.size()==1);assert(validateGlobals(gap.m,recovered).worldSlot==gap.worldSlot);
 // Remove the intact later GNames signature; the split signature is not a hit.
 gap.m.put<std::uint8_t>(gap.code+8300,0);assert(scanGlobals(gap.m,gap.image(),65536).names.empty());
 // Null GWorld is allowed only provisionally. RuntimeObjects must refuse any
 // player access until a valid live World/player chain is available.
 f.m.put<Address>(f.worldSlot,0);auto empty=validateGlobals(f.m,scanGlobals(f.m,f.image()));Resolver resolver(f.m,empty.names);RuntimeObjects live(f.m,resolver,empty.worldSlot);rejects([&]{live.player();});
 f.m.put<Address>(f.worldSlot,f.worldObject);
 // Ambiguous live worlds and ambiguous null slots are both hard failures.
 auto extra=f.data+0xC00;f.m.put<Address>(extra,f.worldObject);f.pattern(f.code+400,true,extra);rejects([&]{validateGlobals(f.m,scanGlobals(f.m,f.image()));});
 f.m.put<Address>(extra,0);f.m.put<Address>(f.worldSlot,0);rejects([&]{validateGlobals(f.m,scanGlobals(f.m,f.image()));});
 // PE validation: corruption, wrong architecture, wrong module bounds, section
 // overflow and overlapping sections are rejected before any code scanning.
 PEFixture bad;bad.m.put<std::uint16_t>(bad.imageBase,0);rejects([&]{bad.image();});bad.m.put<std::uint16_t>(bad.imageBase,0x5A4D);
 bad.m.put<std::uint16_t>(bad.imageBase+0x84,0x14C);rejects([&]{bad.image();});bad.m.put<std::uint16_t>(bad.imageBase+0x84,0x8664);
 rejects([&]{Image::parse(bad.m,bad.imageBase,bad.imageSize+0x1000);});
 auto table=bad.imageBase+0x80+24+0xF0;bad.m.put<std::uint32_t>(table+40+8,0xFFFFFFF0);rejects([&]{bad.image();});bad.m.put<std::uint32_t>(table+40+8,0x3000);
 bad.m.put<std::uint32_t>(table+40+12,0x2000);rejects([&]{bad.image();});
 // Duplicate World references are deduplicated exactly like GNames.
 PEFixture duplicate;duplicate.pattern(duplicate.code+32,false,duplicate.pool);duplicate.pattern(duplicate.code+64,true,duplicate.worldSlot);duplicate.pattern(duplicate.code+96,true,duplicate.worldSlot);
 assert(scanGlobals(duplicate.m,duplicate.image()).worlds.size()==1);
 // Pattern targets in executable or read-only sections are not globals.
 duplicate.pattern(duplicate.code+128,false,duplicate.code+1000);
 assert(scanGlobals(duplicate.m,duplicate.image()).names.size()==1);
 auto dataFlags=duplicate.imageBase+0x80+24+0xF0+36;
 duplicate.m.put<std::uint32_t>(dataFlags,0x40000040);assert(scanGlobals(duplicate.m,duplicate.image()).names.empty());
 // Exercise the actual DEFAULT 64-KiB chunk boundary, not just a page boundary.
 PEFixture large;large.imageSize=0x30000;large.m.put<std::uint32_t>(large.imageBase+0x80+24+56,large.imageSize);
 large.m.put<std::uint32_t>(large.imageBase+0x80+24+0xF0+40+8,0x20000);large.m.zero(large.code,0x20000);
 large.pattern(large.code+65530,false,large.pool);large.pattern(large.code+65570,true,large.worldSlot);
 auto bulk=scanGlobals(large.m,large.image());assert(bulk.stats.reads==2);assert(bulk.stats.bytes==0x20000);
 assert(validateGlobals(large.m,bulk).worldSlot==large.worldSlot);
 // Production scheduling: no repeated work at 30/100 ms, exactly 500 ms floor.
 WorkGate gate;assert(gate.due(1000));assert(!gate.due(1000));assert(!gate.due(1030));assert(!gate.due(1100));assert(!gate.due(1499));assert(gate.due(1500));assert(!gate.due(1501));assert(gate.due(2000));
 std::cout<<"PASS: PE bounds, ASLR and shifted RVAs/timestamps/image sizes, signed RIP, chunk boundaries, gaps/page recovery, decoys, ambiguity, null World deferral, missing patterns, read-only bootstrap, 500 ms scheduler.\n";
}
