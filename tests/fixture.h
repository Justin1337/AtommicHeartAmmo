#pragma once
#include "../ammo_core.h"
using namespace ammo;
struct Fake final:WritableMemory{
 std::map<Address,std::uint8_t> memory;int writes=0;mutable int reads=0;Address failAt=0;
 std::vector<Address> written;
 bool bytes(Address a,void* out,std::size_t n)const override{++reads;auto p=static_cast<std::uint8_t*>(out);for(std::size_t i=0;i<n;++i){auto t=memory.find(a+i);if(t==memory.end())return false;p[i]=t->second;}return true;}
 bool write(Address a,const void* in,std::size_t n)override{if(a==failAt)return false;auto p=static_cast<const std::uint8_t*>(in);for(std::size_t i=0;i<n;++i)memory[a+i]=p[i];++writes;written.push_back(a);return true;}
 void zero(Address a,std::size_t n){for(std::size_t i=0;i<n;++i)memory[a+i]=0;}
};
struct Fixture{
 Fake m;Address next=0x400000,pool=0x100000,names=0x200000;std::uint32_t namePos=0;
 std::map<std::string,Address> types;
 Address alloc(std::size_t n=0x100){auto a=next;next+=(n+0xFF)&~Address(0xFF);m.zero(a,n);return a;}
 Fixture(){m.put<Address>(pool+0x10,names);}
 void fname(Address destination,const std::string& name){m.put<std::uint32_t>(destination,namePos/2);m.put<std::uint32_t>(destination+4,0);m.put<std::uint16_t>(names+namePos,static_cast<std::uint16_t>(name.size()<<6));for(std::size_t i=0;i<name.size();++i)m.put<char>(names+namePos+2+i,name[i]);namePos+=static_cast<std::uint32_t>((name.size()+3)&~std::size_t(1));}
 Address cls(const std::string& name,Address super=0,int size=0x2000){auto c=alloc();fname(c+0x18,name);m.put<Address>(c+0x48,super);m.put<std::int32_t>(c+0x60,size);return c;}
 Address object(Address c,const std::string& name,std::size_t size=0x2000){auto o=alloc(size);m.put<Address>(o+0x10,c);m.put<std::int32_t>(o+0xC,static_cast<std::int32_t>(o>>8));fname(o+0x18,name);return o;}
 Address type(const std::string& name){if(!types.count(name)){auto t=alloc();fname(t,name);types[name]=t;}return types[name];}
 Address prop(Address c,const std::string& name,const std::string& kind,int offset,int size){auto p=alloc();m.put<Address>(p+8,type(kind));fname(p+0x28,name);m.put<std::int32_t>(p+0x38,1);m.put<std::int32_t>(p+0x3C,size);m.put<std::int32_t>(p+0x4C,offset);if(c){m.put<Address>(p+0x20,m.read<Address>(c+0x58));m.put<Address>(c+0x58,p);}return p;}
 void slotMap(Address map,const std::vector<SlotEntry>& entries){m.zero(map,80);auto data=alloc(entries.size()*24);m.put<Address>(map,data);m.put<std::int32_t>(map+8,static_cast<int>(entries.size()));m.put<std::int32_t>(map+12,static_cast<int>(entries.size()));m.put<std::int32_t>(map+0x28,static_cast<int>(entries.size()));m.put<std::int32_t>(map+0x2C,128);m.put<std::uint32_t>(map+0x10,(1u<<entries.size())-1);for(std::size_t i=0;i<entries.size();++i){m.put<std::int32_t>(data+i*24,entries[i].key);m.put<Address>(data+i*24+8,entries[i].object);}}
};
