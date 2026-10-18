// Tests use synthetic memory; they do not attach to or modify any process.
#include "../ammo_core.h"
#include <cassert>
#include <limits>
using namespace ammo;
struct Fake final:WritableMemory {
 std::map<Address,std::uint8_t> data;Address failAt=0;int writes=0;
 bool bytes(Address a,void* out,std::size_t n)const override{auto p=static_cast<std::uint8_t*>(out);for(std::size_t i=0;i<n;++i){auto it=data.find(a+i);if(it==data.end())return false;p[i]=it->second;}return true;}
 bool write(Address a,const void* src,std::size_t n)override{if(a==failAt)return false;auto p=static_cast<const std::uint8_t*>(src);for(std::size_t i=0;i<n;++i)data[a+i]=p[i];++writes;return true;}
 void zero(Address a,std::size_t n){for(std::size_t i=0;i<n;++i)data[a+i]=0;}
};
template<class F>void fails(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}assert(caught);}
int main(){
 Fake m;constexpr Address object=0x10000;m.zero(object,0x100);
 m.put<Address>(object+0x10,0x20000);m.put<std::int32_t>(object+0xC,123);m.put<Address>(object+0x18,0x1234);m.put<Address>(object+0x20,0x30000);
 m.put<std::uint8_t>(object+0x40,0xA0);m.put<std::int32_t>(object+0x44,7);
 Property boolField;boolField.type="BoolProperty";boolField.offset=0x40;boolField.size=boolField.dim=1;boolField.byteMask=boolField.fieldMask=4;
 PatchGroup group;group.owner=Identity::capture(m,object);group.addBool(m,object,boolField);group.addInt(m,object+0x44,9999);group.apply(m);
 assert(m.read<std::uint8_t>(object+0x40)==0xA4);assert(m.read<std::int32_t>(object+0x44)==9999);
 // A game update to another packed bool must survive restoration.
 m.put<std::uint8_t>(object+0x40,0xE4);assert(group.restore(m));assert(m.read<std::uint8_t>(object+0x40)==0xE0);assert(m.read<std::int32_t>(object+0x44)==7);
 // Native bool uses FieldMask FF and ByteMask 01, not an all-bits-set true.
 boolField.fieldMask=255;boolField.byteMask=1;m.put<std::uint8_t>(object+0x40,0);
 group.owner=Identity::capture(m,object);group.addBool(m,object,boolField);group.apply(m);assert(m.read<std::uint8_t>(object+0x40)==1);assert(group.restore(m));assert(m.read<std::uint8_t>(object+0x40)==0);
 // Recycled memory and destruction flags block both writes and restoration.
 group.owner=Identity::capture(m,object);group.addInt(m,object+0x44,9999);m.put<std::int32_t>(object+0xC,124);
 int before=m.writes;fails([&]{group.apply(m);});assert(!group.restore(m));assert(m.writes==before);m.put<std::int32_t>(object+0xC,123);
 m.put<std::uint32_t>(object+8,0x8000);fails([&]{Identity::capture(m,object);});m.put<std::uint32_t>(object+8,0);
 // Partial update failure: earlier fields can still be restored, and failed
 // restorations remain retryable until the underlying access recovers.
 group.owner=Identity::capture(m,object);group.addBool(m,object,boolField);group.addInt(m,object+0x44,9999);m.failAt=object+0x44;fails([&]{group.apply(m);});assert(!group.restore(m));assert(!group.patches.empty());m.failAt=0;assert(group.restore(m));assert(m.read<std::int32_t>(object+0x44)==7);
 // Shared energy is finite and is returned to its saved current value.
 m.put<float>(object+0x50,20);group.owner=Identity::capture(m,object);group.addFloat(m,object+0x50,100);group.apply(m);assert(m.read<float>(object+0x50)==100);assert(group.restore(m));assert(m.read<float>(object+0x50)==20);
 fails([&]{group.addFloat(m,object+0x50,std::numeric_limits<float>::infinity());});
 // Real sparse map shape: an unallocated entry deliberately has the selected
 // key and a garbage value. Only the allocated slot is accepted.
 constexpr Address map=0x40000,data=0x50000;m.zero(map,0x50);m.zero(data,3*24);
 m.put<Address>(map,data);m.put<std::int32_t>(map+8,3);m.put<std::int32_t>(map+12,3);
 m.put<std::int32_t>(map+0x28,3);m.put<std::int32_t>(map+0x2C,128);m.put<std::int32_t>(map+0x34,1);m.put<std::uint32_t>(map+0x10,5);
 m.put<std::int32_t>(data,1);m.put<Address>(data+8,0x60000);
 m.put<std::int32_t>(data+24,2);m.put<Address>(data+32,1);
 m.put<std::int32_t>(data+48,2);m.put<Address>(data+56,0x70000);
 assert(slotMapValue(m,map,4,2)==0x70000);assert(slotMapValue(m,map,1,2)==0x70000);
 fails([&]{slotMapValue(m,map,8,2);});fails([&]{slotMapValue(m,map,4,3);});
 // Duplicate allocated keys, allocation-count mismatch, and invalid heap bits.
 m.put<std::int32_t>(data,2);fails([&]{slotMapValue(m,map,4,2);});m.put<std::int32_t>(data,1);
 m.put<std::int32_t>(map+0x34,0);fails([&]{slotMapValue(m,map,4,2);});m.put<std::int32_t>(map+0x34,1);
 constexpr Address bits=0x80000;m.put<Address>(map+0x20,bits);m.put<std::uint32_t>(bits,5);assert(slotMapValue(m,map,4,2)==0x70000);
 m.put<Address>(map+0x20,3);fails([&]{slotMapValue(m,map,4,2);});
 // Zero-only refill: preserve existing positive ammunition and do not keep
 // restoring snapshots that have never been changed by this trainer.
 m.put<std::int32_t>(object+0x44,0);group.owner=Identity::capture(m,object);
 group.addMinimumInt(m,object+0x44,1);group.apply(m);assert(m.read<std::int32_t>(object+0x44)==1);
 int writesAfterFloor=m.writes;group.apply(m);assert(m.writes==writesAfterFloor);
 assert(group.restore(m));assert(m.read<std::int32_t>(object+0x44)==0);
 m.put<std::int32_t>(object+0x44,7);group.owner=Identity::capture(m,object);group.addMinimumInt(m,object+0x44,1);
 int writesBeforePositive=m.writes;group.apply(m);assert(m.writes==writesBeforePositive);
 m.put<std::int32_t>(object+0x44,6);assert(group.restore(m));assert(m.read<std::int32_t>(object+0x44)==6);
 // Class-reference data writes preserve all 64 pointer bits, fill only null,
 // and do not overwrite a new chamber reference placed there by the game.
 constexpr Address bullet=0x12345678000ULL,otherBullet=0x12345679000ULL;
 m.put<Address>(object+0x58,0);group.owner=Identity::capture(m,object);group.addNullPointer(m,object+0x58,bullet);group.apply(m);
 assert(m.read<Address>(object+0x58)==bullet);assert(group.restore(m));assert(m.read<Address>(object+0x58)==0);
 group.owner=Identity::capture(m,object);group.addNullPointer(m,object+0x58,bullet);group.apply(m);m.put<Address>(object+0x58,otherBullet);
 int writesBeforeOther=m.writes;group.apply(m);assert(m.writes==writesBeforeOther);assert(group.restore(m));assert(m.read<Address>(object+0x58)==otherBullet);
 std::cout<<"PASS: bit masks, native bools, live identities, destruction flags, partial write rollback/retry, zero-to-one floor, 64-bit chamber pointers, energy, sparse map holes, duplicate keys, bounds, inline/heap allocation bits.\n";
}
