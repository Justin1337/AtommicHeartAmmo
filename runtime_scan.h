// SPDX-License-Identifier: GPL-3.0-or-later
// One-time, read-only bootstrap plus live reflection-based object resolution.
// PE header offsets follow Microsoft's PE/COFF specification (README sources).
#pragma once
#include "ammo_core.h"
#include <tuple>
namespace ammo {
inline constexpr std::uint64_t WorkIntervalMs=500;
// Shared by production and tests: early/duplicate timer messages do no work.
class WorkGate {
    std::uint64_t last=0;bool first=true;
public:
    bool due(std::uint64_t now){if(!first && now-last<WorkIntervalMs)return false;first=false;last=now;return true;}
};
struct Section {std::uint32_t rva=0,size=0,flags=0;};
struct Image {
    Address base=0;std::uint32_t size=0,timestamp=0;std::vector<Section> sections;
    bool writableData(Address a,std::size_t n=8)const{
        if(a<base || n>size || a-base>size-n)return false;
        for(auto s:sections)if((s.flags&0x80000000u)&&!(s.flags&0x20000000u)&&a-base>=s.rva&&a-base-s.rva<=s.size&&n<=s.size-(a-base-s.rva))return true;
        return false;
    }
    static Image parse(const Memory& m,Address base,std::uint32_t moduleSize){
        if(!pointer(base)||moduleSize<0x1000||moduleSize>0x80000000u)throw std::runtime_error("Invalid module bounds");
        if(m.read<std::uint16_t>(base)!=0x5A4D)throw std::runtime_error("Invalid DOS magic");
        auto lfanew=m.read<std::int32_t>(base+0x3C);
        if(lfanew<0x40||std::uint32_t(lfanew)>moduleSize-0x100)throw std::runtime_error("Invalid PE offset");
        Address nt=base+lfanew;
        if(m.read<std::uint32_t>(nt)!=0x4550||m.read<std::uint16_t>(nt+4)!=0x8664)throw std::runtime_error("Expected x64 PE image");
        auto count=m.read<std::uint16_t>(nt+6),optionalSize=m.read<std::uint16_t>(nt+20);
        Address optional=nt+24;
        if(count<1||count>96||optionalSize<0x70||m.read<std::uint16_t>(optional)!=0x20B)throw std::runtime_error("Invalid PE optional header");
        auto size=m.read<std::uint32_t>(optional+56),headers=m.read<std::uint32_t>(optional+60);
        auto sectionOffset=std::uint64_t(lfanew)+24+optionalSize;
        if(size!=moduleSize||headers>size||sectionOffset+std::uint64_t(count)*40>headers)throw std::runtime_error("Invalid PE section table");
        Image image{base,size,m.read<std::uint32_t>(nt+8),{}};
        for(unsigned i=0;i<count;++i){
            Address s=base+sectionOffset+Address(i)*40;
            Section item{m.read<std::uint32_t>(s+12),m.read<std::uint32_t>(s+8),m.read<std::uint32_t>(s+36)};
            if(item.size==0)continue;
            if(item.rva<headers||item.rva>=size||item.size>size-item.rva)throw std::runtime_error("Section outside image");
            for(auto other:image.sections)if(item.rva<std::uint64_t(other.rva)+other.size&&other.rva<std::uint64_t(item.rva)+item.size)
                throw std::runtime_error("Overlapping PE sections");
            image.sections.push_back(item);
        }
        if(image.sections.empty())throw std::runtime_error("Empty PE sections");
        return image;
    }
};
struct ScanStats {std::uint64_t bytes=0,reads=0,unreadablePages=0;};
struct GlobalTargets {std::set<Address> names,worlds;ScanStats stats;};
inline GlobalTargets scanGlobals(const Memory& m,const Image& image,std::size_t chunkSize=65536){
    if(chunkSize<4096||chunkSize%4096||chunkSize>1048576)throw std::runtime_error("Invalid scanner chunk size");
    // These signatures identify INSTRUCTIONS, not hardcoded RVAs. disp32 bytes
    // are wildcards; every actual target is computed from that instruction's RIP.
    const std::vector<std::vector<int>> patterns={
        {0x48,0x8D,0x0D,-1,-1,-1,-1,0xE8,-1,-1,-1,-1,0xC6,0x05},
        {0x48,0x89,0x15,-1,-1,-1,-1,0x48,0x8D,0x05,-1,-1,-1,-1,0xC3}};
    GlobalTargets result;std::vector<std::uint8_t> tail;
    auto consume=[&](Address at,const std::vector<std::uint8_t>& bytes){
        std::vector<std::uint8_t> block(tail);auto prefix=block.size();block.insert(block.end(),bytes.begin(),bytes.end());
        const auto* begin=block.data();std::size_t position=0;
        // Both known signatures start in 48. memchr skips nonmatching areas in
        // the LOCAL buffer, reducing per-byte comparisons on large code sections.
        while(position<block.size()){
            auto found=static_cast<const std::uint8_t*>(std::memchr(begin+position,0x48,block.size()-position));
            if(!found)break;
            position=static_cast<std::size_t>(found-begin);
            for(std::size_t kind=0;kind<patterns.size();++kind){
                const auto& p=patterns[kind];if(block.size()-position<p.size())continue;
                bool match=true;for(std::size_t j=1;j<p.size();++j)if(p[j]>=0&&block[position+j]!=p[j]){match=false;break;}
                if(!match)continue;
                std::int32_t displacement=0;std::memcpy(&displacement,begin+position+3,4);
                const Address instruction=at-prefix+position;
                const Address target=Address(std::int64_t(instruction+7)+displacement);
                if(!pointer(target)||!image.writableData(target))continue;
                (kind?result.worlds:result.names).insert(target);
                if(result.names.size()+result.worlds.size()>4096)throw std::runtime_error("Too many global signature targets");
            }
            ++position;
        }
        const auto keep=std::min<std::size_t>(14,block.size());tail.assign(block.end()-keep,block.end());
    };
    for(auto section:image.sections){
        if(!(section.flags&0x20000000u))continue;
        tail.clear(); // No fabricated patterns across different PE sections.
        for(std::uint64_t offset=0;offset<section.size;offset+=chunkSize){
            auto size=std::size_t(std::min<std::uint64_t>(chunkSize,section.size-offset));
            Address at=image.base+section.rva+offset;std::vector<std::uint8_t> data(size);++result.stats.reads;
            if(m.bytes(at,data.data(),size)){result.stats.bytes+=size;consume(at,data);continue;}
            // Recover accessible pages from a partially unreadable large chunk.
            // A failed page breaks overlap; patterns never bridge a memory gap.
            for(std::size_t page=0;page<size;page+=4096){
                std::vector<std::uint8_t> part(std::min<std::size_t>(4096,size-page));++result.stats.reads;
                if(m.bytes(at+page,part.data(),part.size())){result.stats.bytes+=part.size();consume(at+page,part);}
                else{tail.clear();++result.stats.unreadablePages;}
            }
        }
    }
    return result;
}
inline Address onlyTarget(const std::set<Address>& targets,const std::string& name){
    if(targets.size()!=1)throw std::runtime_error(name+": "+std::to_string(targets.size())+" validated targets; no writes");
    return *targets.begin();
}
struct Globals {Address names=0,worldSlot=0;};
inline Globals validateGlobals(const Memory& m,const GlobalTargets& raw){
    std::set<Address> validNames;
    for(auto pool:raw.names)if(validNamePool(m,pool))validNames.insert(pool);
    auto names=onlyTarget(validNames,"GNames");Reflection reflection(m,names);
    std::set<Address> loaded,empty;
    for(auto slot:raw.worlds)try{
        auto object=m.read<Address>(slot);
        if(object==0){empty.insert(slot);continue;}
        if(pointer(object)&&reflection.isA(object,"World")){reflection.find(object,{"OwningGameInstance"});loaded.insert(slot);}
    }catch(const std::exception&){}
    // A single null slot may be provisionally retained during loading. Its World
    // class AND full player path are mandatory before any runtime writes occur.
    if(!loaded.empty())return {names,onlyTarget(loaded,"GWorld")};
    return {names,onlyTarget(empty,"GWorld (waiting for load)")};
}
struct LiveObjects {Address world=0,player=0;};
class RuntimeObjects {
    const Memory& m;Resolver& r;Address slot;
    std::map<Address,std::vector<Property>> structSchemas;
    std::map<std::tuple<Address,std::string,std::string>,std::uint32_t> floatOffsets;
    // Resolve a struct member once per owning class/field/member. Cached values
    // are RELATIVE offsets; object addresses and resource values stay live.
    Address attributeFloat(Address attributes,const std::string& field,const std::string& member){
        auto key=std::make_tuple(r.reflection.klass(attributes),field,member);
        auto cached=floatOffsets.find(key);
        if(cached!=floatOffsets.end())return attributes+cached->second;
        auto p=r.find(attributes,{field});
        if(p.type!="StructProperty"||p.dim!=1||!pointer(p.structure))throw std::runtime_error("Invalid energy attribute struct: "+field);
        auto schema=structSchemas.find(p.structure);
        if(schema==structSchemas.end())schema=structSchemas.emplace(p.structure,r.reflection.properties(p.structure)).first;
        const Property* found=nullptr;
        for(const auto& f:schema->second)if(f.name==member){
            if(found||f.type!="FloatProperty"||f.size!=4||f.dim!=1||std::uint64_t(f.offset)+4>p.size)
                throw std::runtime_error("Invalid attribute float: "+field+"."+member);
            found=&f;
        }
        if(!found)throw std::runtime_error("Missing attribute float: "+field+"."+member);
        auto offset=p.offset+found->offset;floatOffsets.emplace(std::move(key),offset);
        return attributes+offset;
    }
    // Read only a correctly reflected UObject pointer; null is a loading state.
    Address child(Address parent,const std::vector<std::string>& names){
        auto p=r.find(parent,names);
        if(p.type!="ObjectProperty"||p.size!=8||p.dim!=1)throw std::runtime_error("Invalid live object property type");
        auto value=m.read<Address>(parent+p.offset);
        if(!value)throw ResolutionFailure("Objekt noch nicht geladen: "+p.name);
        if(!pointer(value))throw std::runtime_error("Invalid live object pointer");
        return value;
    }
public:
    RuntimeObjects(const Memory& memory,Resolver& resolver,Address worldSlot):m(memory),r(resolver),slot(worldSlot){}
    // Follow the live local-player chain; null during loading is retryable.
    LiveObjects player(){
        auto world=m.read<Address>(slot);
        if(!pointer(world))throw ResolutionFailure("Warte auf geladenen Spielstand");
        if(!r.reflection.isA(world,"World"))throw std::runtime_error("GWorld has incompatible class");
        auto instance=child(world,{"OwningGameInstance"});
        auto local=r.find(instance,{"LocalPlayers"});
        if(local.type!="ArrayProperty"||local.size!=16||local.dim!=1)throw std::runtime_error("Invalid LocalPlayers reflection type");
        auto inner=m.read<Address>(local.metadata+0x78);
        if(r.reflection.name(m.read<Address>(inner+8))!="ObjectProperty"||m.read<std::int32_t>(inner+0x3C)!=8)throw std::runtime_error("Invalid LocalPlayers element type");
        auto data=m.read<Address>(instance+local.offset);auto count=m.read<std::int32_t>(instance+local.offset+8),capacity=m.read<std::int32_t>(instance+local.offset+12);
        if(!pointer(data)||count<1||count>4||capacity<count||capacity>64)throw ResolutionFailure("Kein lokaler Spieler geladen");
        auto controller=child(m.read<Address>(data),{"PlayerController"});Address pawn=0;
        for(const auto& name:{"AcknowledgedPawn","Pawn"})try{pawn=child(controller,{name});if(r.reflection.isA(pawn,"AHBaseCharacter"))break;pawn=0;}catch(const std::exception&){pawn=0;}
        if(!pawn)throw ResolutionFailure("Kein gueltiger Atomic-Heart-Spieler geladen");
        return {world,pawn};
    }
    // Use the character's live attribute object, validated through inheritance.
    Address attributes(Address pawn){
        auto object=child(pawn,{"AttributeSet","BaseCharacterAttributeSet"});
        if(!r.reflection.isA(object,"AHBaseCharacterAttributeSet"))throw std::runtime_error("Invalid AttributeSet class");
        return object;
    }
    // Honour the reflected death bit without assuming its offset or bit number.
    bool dead(Address pawn){
        auto p=r.find(pawn,{"bIsDead"});
        if(p.type!="BoolProperty"||p.size!=1)throw std::runtime_error("Invalid death flag type");
        return (m.read<std::uint8_t>(pawn+p.offset+p.byteOffset)&p.byteMask)!=0;
    }
    // Locate the effect-adjusted float without caching its changing value.
    Address currentFloat(Address attributes,const std::string& field){
        return attributeFloat(attributes,field,"CurrentValue");
    }
    // GAS keeps independent base and current floats. Never assume +8/+12:
    // each nested member is resolved and bounds-checked through metadata.
    Address baseFloat(Address attributes,const std::string& field){
        return attributeFloat(attributes,field,"BaseValue");
    }
};
} // namespace ammo
