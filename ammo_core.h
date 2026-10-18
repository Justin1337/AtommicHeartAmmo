// SPDX-License-Identifier: GPL-3.0-or-later
// External trainer core. UE metadata references and credits: see NOTICE.
#pragma once
#include "ue_reflection.h"
#include <cmath>
namespace ammo {
using namespace ahscan;
struct WritableMemory : Memory {
    virtual bool write(Address, const void*, std::size_t) = 0;
    template<class T> void put(Address a, T value) {
        if (!write(a,&value,sizeof(value))) throw std::runtime_error("Write failed at "+hex(a));
    }
};
// Live object identity is rechecked before each group of writes/restores. The
// name/index/class/outer tuple also prevents most recycled-address mistakes.
// These checks reduce races; remote memory access is not an atomic transaction.
struct Identity {
    Address address=0, klass=0, name=0, outer=0; std::int32_t index=-1;
    static Identity capture(const Memory& m, Address a) {
        if (!pointer(a)) throw std::runtime_error("Invalid UObject address");
        if (m.read<std::uint32_t>(a+8)&0x18000) throw std::runtime_error("Object is being destroyed");
        Identity id{a,m.read<Address>(a+0x10),m.read<Address>(a+0x18),m.read<Address>(a+0x20),m.read<std::int32_t>(a+0xC)};
        if(!pointer(id.klass) || id.index<0) throw std::runtime_error("Invalid UObject identity");
        return id;
    }
    bool alive(const Memory& m)const {
        try { auto x=capture(m,address); return x.klass==klass && x.name==name && x.outer==outer && x.index==index; }
        catch(const std::exception&) { return false; }
    }
};
struct Patch {
    Address address=0; std::uint64_t original=0,target=0; std::uint8_t size=4,mask=0xFF; bool touched=false; bool minimum=false,ifNull=false;
};
class PatchGroup {
public:
    Identity owner; std::vector<Patch> patches;
    // Forget state without touching remote memory after a context change.
    void clear(){owner={};patches.clear();}
    // Capture a validated 32-bit ammunition value before staging its replacement.
    void addInt(WritableMemory& m,Address a,std::int32_t value) {
        auto old=m.read<std::int32_t>(a);
        if(old<0 || old>10000000) throw std::runtime_error("Implausible ammo count");
        patches.push_back({a,static_cast<std::uint32_t>(old),static_cast<std::uint32_t>(value),4,0xFF,false});
    }
    // An exhausted cycle may use -1 as its sentinel. Unlike ammo quantities,
    // that is a valid old position and must be restorable as signed int32 bits.
    void addCyclePosition(WritableMemory& m,Address a) {
        auto old=m.read<std::int32_t>(a);
        if(old < -1 || old > 10000000)throw std::runtime_error("Invalid cycle position");
        patches.push_back({a,static_cast<std::uint32_t>(old),0,4,0xFF,false});
    }
    // Preserve float bits separately from their mutable refill target.
    void addFloat(WritableMemory& m,Address a,float value) {
        auto old=m.read<float>(a);
        if(!std::isfinite(old) || old<0 || !std::isfinite(value) || value<=0 || value>1000000)
            throw std::runtime_error("Invalid energy values");
        Patch p;p.address=a;std::memcpy(&p.original,&old,4);std::memcpy(&p.target,&value,4);patches.push_back(p);
    }
    // Capture the reflected bool mask, including native bool and packed fields.
    void addBool(WritableMemory& m,Address object,const Property& p,bool enabled=true) {
        if(p.type!="BoolProperty" || p.size!=1 || p.dim!=1) throw std::runtime_error("Expected reflected bool");
        auto a=object+p.offset+p.byteOffset;
        patches.push_back({a,m.read<std::uint8_t>(a),enabled?std::uint64_t(p.byteMask):0,1,p.fieldMask,false});
    }
    // A floor is not a replacement of an inventory total: only the reflected
    // barrel cycle counter is raised when it is zero. Positive values survive.
    void addMinimumInt(WritableMemory& m,Address a,std::int32_t floor) {
        addInt(m,a,floor);patches.back().minimum=true;
    }
    // Only UClass pointers supplied by the resolver are accepted here. This is
    // a DATA pointer write, not a function call or remote code allocation.
    void addNullPointer(WritableMemory& m,Address a,Address value) {
        if(!pointer(value))throw std::runtime_error("Invalid bullet class pointer");
        auto original=m.read<Address>(a);
        if(original && !pointer(original))throw std::runtime_error("Invalid chambered bullet pointer");
        Patch p;p.address=a;p.original=original;p.target=value;p.size=8;p.ifNull=true;
        patches.push_back(p);
    }
    // Apply only changed fields after verifying the owning object's identity.
    void apply(WritableMemory& m) {
        if(!owner.alive(m)) throw std::runtime_error("Target object changed");
        for(auto& p:patches) {
            // Read each field once. Unchanged values need no remote write;
            // touched remains true if an EARLIER cycle changed that field.
            if(p.size==1){
                auto current=m.read<std::uint8_t>(p.address);
                auto target=static_cast<std::uint8_t>((current&~p.mask)|(p.target&p.mask));
                if(current==target)continue;
                p.touched=true; // A failed write may still have changed a prefix.
                m.put<std::uint8_t>(p.address,target);
            } else if(p.size==8){
                auto current=m.read<Address>(p.address);
                if((p.ifNull&&current!=0)||current==p.target)continue;
                p.touched=true;m.put<Address>(p.address,p.target);
            } else {
                auto current=m.read<std::uint32_t>(p.address);
                if(p.minimum){
                    auto count=static_cast<std::int32_t>(current);
                    if(count<0||count>10000000)throw std::runtime_error("Invalid live ammo count");
                    if(count>=static_cast<std::int32_t>(p.target))continue;
                }
                if(current==static_cast<std::uint32_t>(p.target))continue;
                p.touched=true;m.put<std::uint32_t>(p.address,static_cast<std::uint32_t>(p.target));
            }
        }
    }
    // Roll back touched fields in reverse order; retain failed writes for retry.
    bool restore(WritableMemory& m) {
        if(patches.empty()) return true;
        if(!owner.alive(m)){clear();return false;}
        bool okay=true;
        for(auto it=patches.rbegin();it!=patches.rend();++it) if(it->touched) try {
            auto& p=*it;
            if(p.size==1){auto current=m.read<std::uint8_t>(p.address);m.put<std::uint8_t>(p.address,(current&~p.mask)|(p.original&p.mask));}
            else if(p.size==8){if(m.read<Address>(p.address)==p.target)m.put<Address>(p.address,p.original);}
            else m.put<std::uint32_t>(p.address,static_cast<std::uint32_t>(p.original));
            p.touched=false;
        } catch(const std::exception&){okay=false;}
        // Preserve failed entries so the UI can retry cleanup on the next tick.
        if(okay) clear();
        return okay;
    }
};

// Read the ACTUAL allocated entries of a UE4.27 TMap<integer,UObject*>.
// TSet element = aligned TPair<key,pointer> + two int32 hash indices (24 bytes).
// TSparseArray uses allocation bits: removed entries are NOT valid objects.
// Container layout checked against Dumper-7 UnrealContainers.h (see README).
struct SlotEntry { std::int32_t key; Address object; };
inline std::vector<SlotEntry> slotMapEntries(const Memory& m,Address map,int keySize) {
    if(keySize!=1 && keySize!=4) throw std::runtime_error("Unsupported weapon slot key");
    auto data=m.read<Address>(map);auto count=m.read<std::int32_t>(map+8),capacity=m.read<std::int32_t>(map+12);
    auto bitCount=m.read<std::int32_t>(map+0x28),bitCapacity=m.read<std::int32_t>(map+0x2C);
    auto free=m.read<std::int32_t>(map+0x34);
    if(count<0 || count>256 || capacity<count || capacity>4096 || bitCount<count || bitCount>4096 ||
       bitCapacity<bitCount || bitCapacity>65536 || free<0 || free>count)
        throw std::runtime_error("Unsupported/invalid sparse slot map");
    if(count==0) throw std::runtime_error("Weapon slot map is empty");
    if(!pointer(data)) throw std::runtime_error("Invalid slot array");
    auto secondary=m.read<Address>(map+0x20);
    if(secondary && !pointer(secondary)) throw std::runtime_error("Invalid allocation bit pointer");
    if(!secondary && bitCount>128) throw std::runtime_error("Inline allocation bits overflow");
    auto bits=secondary?secondary:map+0x10;
    std::vector<SlotEntry> result;int allocated=0;
    for(int i=0;i<count;++i){
        auto word=m.read<std::uint32_t>(bits+Address(i/32)*4);
        if(!(word&(std::uint32_t(1)<<(i%32))))continue;
        ++allocated;auto pair=data+Address(i)*24;
        int key=keySize==1?int(m.read<std::uint8_t>(pair)):m.read<std::int32_t>(pair);
        auto value=m.read<Address>(pair+8);
        if(pointer(value))result.push_back({key,value});
    }
    if(allocated!=count-free) throw std::runtime_error("Sparse map allocation count mismatch");
    return result;
}
// Retain the strict lookup for regression tests and callers that genuinely
// have a MAP KEY. CurrentActiveWeaponSlot is no longer passed to it.
inline Address slotMapValue(const Memory& m,Address map,int keySize,std::int32_t selected) {
    Address result=0;int hits=0;
    for(auto entry:slotMapEntries(m,map,keySize))if(entry.key==selected){result=entry.object;++hits;}
    if(hits!=1)throw std::runtime_error("No unique active weapon slot");
    return result;
}
struct ResolutionFailure:std::runtime_error { using std::runtime_error::runtime_error; };
struct WeaponEvidence { Address weapon; int rank; bool visibleOwned; std::string route; };
inline Address chooseWeapon(const std::vector<WeaponEvidence>& evidence) {
    std::set<Address> visible;
    for(const auto& e:evidence)if(e.visibleOwned)visible.insert(e.weapon);
    if(visible.size()==1)return *visible.begin();
    if(visible.size()>1)throw ResolutionFailure("Mehrere sichtbare Spielerwaffen; keine Aenderung");
    int best=0;std::set<Address> candidates;
    for(const auto& e:evidence) {
        if(e.rank>best){best=e.rank;candidates.clear();}
        if(e.rank==best && e.rank>=4)candidates.insert(e.weapon);
    }
    if(candidates.size()==1)return *candidates.begin();
    throw ResolutionFailure("Waffe nicht eindeutig: "+std::to_string(candidates.size())+" aktive Kandidaten");
}
class Resolver {
    const Memory& m;
    std::map<Address,std::vector<Property>> schemas;
    // Semantic active-item links are stronger evidence than array positions.
    static bool activeLink(const std::string& name) {
        static const std::set<std::string> names={"CurrentWeapon","WeaponInHands","EquippedWeapon","CurrentItem","ActiveItem","ActiveWeapon","EquippedItem","ItemInHands","CurrentSlotItem","CurrentSlotWeapon"};
        return names.count(name)!=0;
    }
    // Read exactly the reflected bool bit; neighbouring flags are independent.
    bool boolValue(Address object,const Property& p)const {
        return (m.read<std::uint8_t>(object+p.offset+p.byteOffset)&p.byteMask)!=0;
    }
    // Accept explicit equipped/selected state only under known property names.
    bool activeMarker(Address object) {
        for(const auto& p:properties(object))if(p.type=="BoolProperty" &&
           (p.name=="bIsEquipped" || p.name=="bIsSelected" || p.name=="bIsCurrentWeapon" || p.name=="bWeaponInHands" || p.name=="bIsCurrentSlot"))
            if(boolValue(object,p))return true;
        return false;
    }
    // A uniquely visible player-owned mesh disambiguates hidden inventory items.
    bool visibleOwned(Address object,Address player) {
        try {
            auto owner=reflection.objectProperty(object,find(object,{"Owner"}));
            if(owner!=player && m.read<Address>(owner+0x20)!=player)return false;
            if(boolValue(object,find(object,{"bHidden"})))return false;
            auto mesh=reflection.objectProperty(object,find(object,{"Mesh"}));
            return boolValue(mesh,find(mesh,{"bVisible"})) && !boolValue(mesh,find(mesh,{"bHiddenInGame"}));
        }catch(const std::exception&){return false;}
    }
public:
    Reflection reflection;
    std::string lastRoute;
    Resolver(const Memory& mem,Address names):m(mem),reflection(mem,names){}
    // Cache immutable reflection schemas by class, not live gameplay values.
    const std::vector<Property>& properties(Address object){
        auto klass=reflection.klass(object);auto it=schemas.find(klass);
        if(it==schemas.end())it=schemas.emplace(klass,reflection.properties(klass)).first;
        return it->second;
    }
    // Resolve known aliases in priority order, never guess a numeric offset.
    Property find(Address object,const std::vector<std::string>& names){
        auto& all=properties(object);
        for(const auto& name:names)for(const auto& p:all)if(p.name==name)return p;
        throw std::runtime_error("Missing field: "+names.front());
    }
    // Validate map key/value metadata before reading allocated slot components.
    std::vector<SlotEntry> slots(Address player,std::ostream* trace=nullptr) {
        auto map=find(player,{"CharacterSlotsComponents"});
        if(map.type!="MapProperty" || map.size!=80)throw std::runtime_error("Unsupported CharacterSlotsComponents layout");
        auto key=m.read<Address>(map.metadata+0x78),value=m.read<Address>(map.metadata+0x80);
        auto keyType=reflection.name(m.read<Address>(key+8)),valueType=reflection.name(m.read<Address>(value+8));
        if(trace)*trace<<"Map address="<<Memory::hex(player+map.offset)<<" keyType="<<keyType<<" keySize="<<m.read<std::int32_t>(key+0x3C)<<" valueType="<<valueType
                       <<" data="<<Memory::hex(m.read<Address>(player+map.offset))<<" slots="<<m.read<std::int32_t>(player+map.offset+8)
                       <<" allocatedBits="<<m.read<std::int32_t>(player+map.offset+0x28)<<" free="<<m.read<std::int32_t>(player+map.offset+0x34)<<"\n";
        if((keyType!="IntProperty" && keyType!="ByteProperty" && keyType!="EnumProperty") ||
           valueType!="ObjectProperty" || m.read<std::int32_t>(value+0x3C)!=8)
            throw std::runtime_error("Unexpected slot map key/value types");
        return slotMapEntries(m,player+map.offset,m.read<std::int32_t>(key+0x3C));
    }
    Address selectedWeapon(Address player,std::ostream* trace=nullptr) {
        // The old version equated a weapon index with a character-slot category.
        // Instead inspect EVERY allocated slot and the equipment component.
        // Only semantic active-item links, explicit slot indices/markers, or a
        // uniquely visible owned mesh qualify a weapon for writes.
        auto activeIndex=find(player,{"CurrentActiveWeaponSlot"});
        if(activeIndex.type!="IntProperty" || activeIndex.size!=4)throw std::runtime_error("Invalid active weapon index type");
        const auto selected=m.read<std::int32_t>(player+activeIndex.offset);
        if(trace)*trace<<"Active weapon index="<<selected<<" (not assumed to be a map key)\n";
        struct Visit {Address object;int rank,depth;std::string route;};
        std::vector<Visit> queue={{player,0,0,"player"}};
        try {for(auto entry:slots(player,trace)){
            queue.push_back({entry.object,0,1,"CharacterSlotsComponents[key="+std::to_string(entry.key)+"]"});
            if(trace)*trace<<"Slot map key="<<entry.key<<" object="<<Memory::hex(entry.object)<<"\n";
        }}catch(const std::exception& e){if(trace)*trace<<"Slot map: "<<e.what()<<"\n";}
        std::map<Address,int> visited;std::vector<WeaponEvidence> evidence;
        for(std::size_t at=0;at<queue.size() && at<64;++at) {
            auto node=queue[at];
            if(!pointer(node.object) || node.depth>3)continue;
            auto seen=visited.find(node.object);
            if(seen!=visited.end() && seen->second>=node.rank)continue;
            visited[node.object]=node.rank;
            try {
                if(trace){*trace<<"Route "<<node.route<<"\n";describe(node.object,*trace);}
                int rank=node.rank;
                if(activeMarker(node.object))rank=std::max(rank,5);
                if(reflection.isA(node.object,"BaseWeapon")) {
                    bool visible=visibleOwned(node.object,player);
                    if(visible)rank=std::max(rank,1);
                    evidence.push_back({node.object,rank,visible,node.route});continue;
                }
                for(const auto& p:properties(node.object)) {
                    if(p.type=="ObjectProperty" && p.dim==1 && p.size==8) {
                        // At the player root only follow item/equipment links;
                        // never crawl unrelated AI, camera or interaction actors.
                        if(node.object==player && !activeLink(p.name) && p.name!="EquipmentComponent")continue;
                        if(p.name=="Owner" || p.name=="Instigator" || p.name=="Controller")continue;
                        try {auto value=reflection.objectProperty(node.object,p);
                            queue.push_back({value,activeLink(p.name)?std::max(rank,4):rank,node.depth+1,node.route+"."+p.name});
                        }catch(const std::exception&){}
                    }
                    if(p.type=="ArrayProperty" && p.size==16 && p.dim==1 &&
                       (p.name=="Weapons" || p.name=="WeaponSlots" || p.name=="Items" || (p.name=="Children" && node.object==player))) {
                        try {
                            auto inner=m.read<Address>(p.metadata+0x78);
                            if(reflection.name(m.read<Address>(inner+8))!="ObjectProperty" || m.read<std::int32_t>(inner+0x3C)!=8)continue;
                            auto data=m.read<Address>(node.object+p.offset);int count=m.read<std::int32_t>(node.object+p.offset+8),capacity=m.read<std::int32_t>(node.object+p.offset+12);
                            if(!pointer(data)||count<0||count>64||capacity<count||capacity>256)continue;
                            for(int i=0;i<count;++i){int indexRank=rank; // array position is NOT evidence of active selection
                                queue.push_back({m.read<Address>(data+Address(i)*8),indexRank,node.depth+1,node.route+"."+p.name+"["+std::to_string(i)+"]"});}
                        }catch(const std::exception&){}
                    }
                }
            }catch(const std::exception& e){if(trace)*trace<<"Skipped: "<<e.what()<<"\n";}
        }
        if(trace)for(const auto& e:evidence)*trace<<"Weapon candidate "<<Memory::hex(e.weapon)<<" rank="<<e.rank<<" visibleOwned="<<e.visibleOwned<<" route="<<e.route<<"\n";
        auto result=chooseWeapon(evidence);lastRoute.clear();
        for(const auto& e:evidence)if(e.weapon==result){lastRoute=e.route;break;}
        return result;
    }
    // Accept exactly one live EBBarrel linked from the selected weapon.
    Address barrel(Address weapon){
        std::set<Address> found;
        for(const auto& p:properties(weapon))if(p.type=="ObjectProperty" && p.size==8 && p.dim==1){
            try{auto object=reflection.objectProperty(weapon,p);if(reflection.isA(object,"EBBarrel"))found.insert(object);}catch(const std::exception&){}
        }
        if(found.size()!=1)throw std::runtime_error("No unique EBBarrel for selected weapon");
        return *found.begin();
    }
    Address bulletClass(Address weapon,Address barrelObject) {
        // These are class REFERENCES already owned by the game. No projectile
        // object is fabricated and no memory is allocated in the target process.
        try {auto p=find(weapon,{"Bullet"});
            if(p.type=="ClassProperty" && p.size==8){auto v=m.read<Address>(weapon+p.offset);if(pointer(v)&&reflection.isA(v,"Class"))return v;}
        }catch(const std::exception&){}
        try {auto p=find(barrelObject,{"Ammo"});
            if(p.type=="ArrayProperty" && p.size==16){
                auto inner=m.read<Address>(p.metadata+0x78);
                if(reflection.name(m.read<Address>(inner+8))!="ClassProperty")return 0;
                int count=m.read<std::int32_t>(barrelObject+p.offset+8),capacity=m.read<std::int32_t>(barrelObject+p.offset+12);
                auto data=m.read<Address>(barrelObject+p.offset);
                if(count>0&&count<=64&&capacity>=count&&capacity<=1024&&pointer(data)){
                    auto value=m.read<Address>(data);if(pointer(value)&&reflection.isA(value,"Class"))return value;
                }
            }
        }catch(const std::exception&){}
        return 0;
    }
    bool melee(Address weapon) {
        // Positive identification of a ranged weapon takes precedence over
        // uncertain melee class names. Unknown BaseWeapon types are not seeded.
        return reflection.isA(weapon,"MeleeWeapon") || reflection.isA(weapon,"BaseMeleeWeapon");
    }
    // Explicit read-only diagnostics, including float values needed for energy.
    void describe(Address object,std::ostream& out){
        out<<"Object "<<Memory::hex(object)<<" "<<reflection.objectName(object)<<" class="<<reflection.objectName(reflection.klass(object))<<"\n";
        for(const auto& p:properties(object)) {
            out<<p.name<<" = "<<Memory::hex(p.offset)<<" // "<<p.type<<" size="<<p.size;
            try {
                if(p.type=="IntProperty")out<<" value="<<m.read<std::int32_t>(object+p.offset);
                else if(p.type=="FloatProperty"&&p.size==4)out<<" value="<<m.read<float>(object+p.offset);
                else if(p.type=="BoolProperty")out<<" value="<<boolValue(object,p);
                else if(p.type=="ObjectProperty" || p.type=="ClassProperty"){
                    auto value=m.read<Address>(object+p.offset);out<<" value="<<Memory::hex(value);
                    if(pointer(value))out<<" name="<<reflection.objectName(value)<<" class="<<reflection.objectName(reflection.klass(value));
                }else if(p.type=="ArrayProperty")out<<" data="<<Memory::hex(m.read<Address>(object+p.offset))<<" count="<<m.read<std::int32_t>(object+p.offset+8);
            }catch(const std::exception&){out<<" unavailable";}
            out<<"\n";
        }
    }
};
// Compact before/after state with semantic labels; called only when preparing a
// barrel. It reuses cached schemas and performs no writes or graph traversal.
inline void logBarrelState(Resolver& r,const Memory& m,Address object,std::ostream& log,const char* phase){
    auto count=r.find(object,{"CycleAmmoCount"}),position=r.find(object,{"CycleAmmoPos"});
    auto chamber=r.find(object,{"ChamberedBullet"});
    auto blocked=r.find(object,{"bShootingBlocked","ShootingBlocked"});
    auto load=r.find(object,{"bLoadNext","LoadNext"});
    auto unlimited=r.find(object,{"bCycleAmmoUnlimited","CycleAmmoUnlimited"});
    auto flag=[&](const Property& p){return (m.read<std::uint8_t>(object+p.offset+p.byteOffset)&p.byteMask)!=0;};
    log<<"Barrel "<<phase<<": object="<<Memory::hex(object)
       <<" CycleAmmoCount="<<m.read<std::int32_t>(object+count.offset)
       <<" CycleAmmoPos="<<m.read<std::int32_t>(object+position.offset)
       <<" ChamberedBullet="<<Memory::hex(m.read<Address>(object+chamber.offset))
       <<" CycleAmmoUnlimited="<<flag(unlimited)<<" ShootingBlocked="<<flag(blocked)
       <<" LoadNext="<<flag(load)<<"\n";
}
// Build the complete external data update as a testable unit. The old version
// changed only counters/chamber, which could update the HUD while leaving the
// EBBarrel shooting state blocked. The final write releases that specific gate.
// Player input, ability tags, reload cooldowns and animation flags stay untouched.
inline PatchGroup makeBarrelPatch(Resolver& r,WritableMemory& m,Address weapon,Address object,std::ostream& log) {
    if(r.melee(weapon) || !r.reflection.isA(weapon,"RangeWeapon") || !r.reflection.isA(object,"EBBarrel"))
        throw std::runtime_error("Unconfirmed ranged weapon/barrel");
    PatchGroup fresh;fresh.owner=Identity::capture(m,object);
    auto unlimited=r.find(object,{"bCycleAmmoUnlimited","CycleAmmoUnlimited"});
    auto count=r.find(object,{"CycleAmmoCount"});
    auto position=r.find(object,{"CycleAmmoPos"});
    auto load=r.find(object,{"bLoadNext","LoadNext"});
    auto blocked=r.find(object,{"bShootingBlocked","ShootingBlocked"});
    auto chamber=r.find(object,{"ChamberedBullet"});
    if(count.type!="IntProperty" || count.size!=4 || count.dim!=1 ||
       position.type!="IntProperty" || position.size!=4 || position.dim!=1 ||
       chamber.type!="ClassProperty" || chamber.size!=8 || chamber.dim!=1)
        throw std::runtime_error("Unsupported ammo state field layout");
    auto bullet=r.bulletClass(weapon,object);
    if(!bullet)throw std::runtime_error("No validated projectile class; cannot prepare shot");
    // Stage ammunition before LoadNext/unblocking; reverse-order restoration
    // restores the block first. External access is still not atomically synced
    // with the game thread, as documented in README_DE.md.
    fresh.addNullPointer(m,object+chamber.offset,bullet);
    fresh.addMinimumInt(m,object+count.offset,1);
    fresh.addCyclePosition(m,object+position.offset);
    fresh.addBool(m,object,unlimited,true);
    fresh.addBool(m,object,load,true);
    fresh.addBool(m,object,blocked,false);
    logBarrelState(r,m,object,log,"BEFORE");
    return fresh;
}
} // namespace ammo
