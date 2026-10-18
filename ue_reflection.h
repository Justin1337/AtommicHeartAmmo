// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only Atomic Heart reflection scanner, C++17, Windows x64.
// Layout/signature references: Skorchekd/AtomicHeartMenu, commit
// fe36f468baacc996c252a1d3231d0d20009127bd. See NOTICE and LICENSE.
// Define AH_SCANNER_MAIN in ONE translation unit to build the console program.
#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ahscan {
using Address = std::uint64_t;

// All memory operations are injected through this interface. The test suite
// supplies artificial memory; the Windows implementation uses read access only.
struct Memory {
    virtual ~Memory() = default;
    virtual bool bytes(Address address, void* out, std::size_t size) const = 0;
    template<class T> T read(Address address) const {
        T value{};
        if (!bytes(address, &value, sizeof(value)))
            throw std::runtime_error("Unreadable memory at " + hex(address));
        return value;
    }
    static std::string hex(Address n) {
        std::ostringstream s; s << "0x" << std::hex << std::uppercase << n; return s.str();
    }
};
inline bool pointer(Address p) { return p >= 0x10000 && p < 0x0000800000000000ULL && (p & 7) == 0; }
inline std::string identifier(const std::string& input) {
    std::string out;
    for (unsigned char c : input) out += (std::isalnum(c) && c < 128) || c == '_' ? char(c) : '_';
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(out.begin(), '_');
    // A prefix also prevents C++ keywords and reserved identifiers from leaking
    // into declarations generated from arbitrary game property names.
    return "v_" + out;
}

// Wildcards are -1. Each match is returned: silently selecting the first AOB
// match would produce unreliable globals after a game update.
inline std::vector<std::size_t> matches(const std::vector<std::uint8_t>& data,
                                      const std::vector<int>& pattern) {
    std::vector<std::size_t> result;
    if (pattern.empty() || data.size() < pattern.size()) return result;
    for (std::size_t i = 0; i <= data.size() - pattern.size(); ++i) {
        bool equal = true;
        for (std::size_t j = 0; j < pattern.size(); ++j)
            if (pattern[j] >= 0 && data[i+j] != pattern[j]) { equal = false; break; }
        if (equal) result.push_back(i);
    }
    return result;
}
inline Address ripTarget(const Memory& m, Address instruction) {
    // These signatures start with a seven-byte RIP-relative x64 instruction.
    const auto displacement = m.read<std::int32_t>(instruction + 3);
    return Address(std::int64_t(instruction + 7) + displacement);
}
// FNameEntryHeader bits 1..5 contain a hash, not part of the length.
inline bool validNamePool(const Memory& m, Address pool) {
    try {
        auto block=m.read<Address>(pool+0x10);
        if (!pointer(block)) return false;
        auto header=m.read<std::uint16_t>(block);
        if ((header>>6)!=4 || (header&1)) return false;
        char text[4]{};
        return m.bytes(block+2,text,4) && std::memcmp(text,"None",4)==0;
    } catch(const std::exception&) { return false; }
}
struct Step { Address offset; bool dereference; };
using Chain = std::vector<Step>;
inline Address follow(const Memory& m, Address base, const Chain& chain) {
    for (auto step : chain) {
        if (base > UINT64_MAX - step.offset) throw std::runtime_error("Pointer overflow");
        base += step.offset;
        if (step.dereference) {
            base = m.read<Address>(base);
            if (!pointer(base)) throw std::runtime_error("Invalid pointer in chain");
        }
    }
    return base;
}

// Bootstrap layouts from the AtomicHeartMenu dump. These describe UE's
// reflection metadata, NOT guessed health/ammo offsets. Changes to this engine
// layout still require a new profile; a generic AOB cannot infer every layout.
struct Profile {
    std::uint32_t objectClass = 0x10, objectName = 0x18;
    std::uint32_t super = 0x48, children = 0x58, structSize = 0x60;
    std::uint32_t fieldType = 0x08, fieldNext = 0x20, fieldName = 0x28;
    std::uint32_t arrayDim = 0x38, elementSize = 0x3C, propertyOffset = 0x4C;
    std::uint32_t propertyExtra = 0x78;
};
struct Property {
    std::string name, type;
    Address metadata = 0, structure = 0;
    std::uint32_t offset = 0, size = 0, dim = 0;
    std::uint8_t byteOffset = 0, byteMask = 0, fieldMask = 0;
};
class Reflection {
    const Memory& m;
public:
    Address names;
    Profile layout;
    Reflection(const Memory& mem, Address pool) : m(mem), names(pool) {}
    std::string name(Address fname) const {
        const auto id = m.read<std::uint32_t>(fname);
        const auto block = id >> 16, entryOffset = id & 0xFFFF;
        if (block >= 8192) throw std::runtime_error("Invalid FName block");
        Address entry = m.read<Address>(names + 0x10 + Address(block)*8);
        if (!pointer(entry)) throw std::runtime_error("Invalid FName block pointer");
        entry += Address(entryOffset)*2;
        const auto header = m.read<std::uint16_t>(entry);
        const auto len = header >> 6;
        if (len < 1 || len > 1024) throw std::runtime_error("Invalid FName length");
        std::string result;
        if (header & 1) {
            // Engine property identifiers are ASCII. Refuse unusual UTF-16 names
            // rather than incorrectly converting or generating invalid C++.
            for (int i=0; i<len; ++i) {
                auto c = m.read<std::uint16_t>(entry+2+Address(i)*2);
                result += c < 128 ? char(c) : '_';
            }
        } else {
            result.resize(len);
            if (!m.bytes(entry+2, result.data(), len)) throw std::runtime_error("Unreadable FName");
        }
        const auto number = m.read<std::uint32_t>(fname+4);
        if (number) result += "_" + std::to_string(number-1);
        return result;
    }
    std::string objectName(Address object) const { return name(object + layout.objectName); }
    Address klass(Address object) const {
        auto k = m.read<Address>(object + layout.objectClass);
        if (!pointer(k)) throw std::runtime_error("Invalid UObject class");
        return k;
    }
    bool isA(Address object, const std::string& wanted) const {
        std::set<Address> seen;
        for (Address k=klass(object); k && seen.size()<64; k=m.read<Address>(k+layout.super)) {
            if (!pointer(k) || !seen.insert(k).second) throw std::runtime_error("Invalid class hierarchy");
            if (objectName(k)==wanted) return true;
        }
        return false;
    }
    std::vector<Property> properties(Address type) const {
        std::vector<Property> out;
        std::set<Address> classes, fields;
        for (Address k=type; k; k=m.read<Address>(k+layout.super)) {
            if (!pointer(k) || classes.size()>=64 || !classes.insert(k).second)
                throw std::runtime_error("Invalid reflection hierarchy");
            const auto containerSize=m.read<std::int32_t>(k+layout.structSize);
            if (containerSize<=0 || containerSize>0x1000000) throw std::runtime_error("Invalid struct size");
            for (Address f=m.read<Address>(k+layout.children); f; f=m.read<Address>(f+layout.fieldNext)) {
                if (!pointer(f) || fields.size()>=8192 || !fields.insert(f).second)
                    throw std::runtime_error("Cyclic/invalid property list");
                Property p; p.metadata=f; p.name=name(f+layout.fieldName);
                Address t=m.read<Address>(f+layout.fieldType);
                if (!pointer(t)) throw std::runtime_error("Invalid property type");
                p.type=name(t);
                const auto off=m.read<std::int32_t>(f+layout.propertyOffset);
                const auto sz=m.read<std::int32_t>(f+layout.elementSize);
                const auto dim=m.read<std::int32_t>(f+layout.arrayDim);
                if (off<0 || sz<=0 || dim<=0 || std::uint64_t(off)+std::uint64_t(sz)*dim>std::uint64_t(containerSize))
                    throw std::runtime_error("Property exceeds its reflected container");
                p.offset=off; p.size=sz; p.dim=dim;
                if (p.type=="StructProperty") p.structure=m.read<Address>(f+layout.propertyExtra);
                if (p.type=="BoolProperty") {
                    p.byteOffset=m.read<std::uint8_t>(f+layout.propertyExtra+1);
                    p.byteMask=m.read<std::uint8_t>(f+layout.propertyExtra+2);
                    p.fieldMask=m.read<std::uint8_t>(f+layout.propertyExtra+3);
                    if (p.byteOffset>=p.size || !p.byteMask || !p.fieldMask)
                        throw std::runtime_error("Invalid reflected bool metadata");
                }
                out.push_back(p);
            }
        }
        return out;
    }
    Property find(Address object, const std::vector<std::string>& candidates) const {
        const auto props=properties(klass(object));
        for (const auto& wanted : candidates)
            for (const auto& p : props) if (p.name==wanted) return p;
        throw std::runtime_error("Required property absent: " + candidates.front());
    }
    Address objectProperty(Address object, const Property& p) const {
        if (p.type!="ObjectProperty" || p.dim!=1 || p.size!=8)
            throw std::runtime_error("Expected a plain UObject pointer: " + p.name);
        auto value=m.read<Address>(object+p.offset);
        if (!pointer(value)) throw std::runtime_error("Object not available: " + p.name);
        return value;
    }
};
struct Node { std::string label; Address address; Chain chain; };

// The exporter emits only discovered fields; missing properties are never
// silently replaced with old constants. Names precede their hex values.
class Exporter {
    const Reflection& reflection;
    std::ostringstream offsets, chains;
    std::set<std::string> scopes;
public:
    explicit Exporter(const Reflection& r) : reflection(r) {}
    static void constant(std::ostream& out, const std::string& type, const std::string& name, Address value) {
        out << "inline constexpr " << type << " " << name << " = " << Memory::hex(value) << ";\n";
    }
    void chain(const std::string& name, const Chain& value) {
        chains << "inline constexpr std::array<Step, " << value.size() << "> " << name << " = {{\n";
        for (auto s:value) chains << "    {" << Memory::hex(s.offset) << ", " << (s.dereference?"true":"false") << "},\n";
        chains << "}};\n";
    }
    void dump(const Node& node, Address type, int depth=0) {
        if (depth>2 || !scopes.insert(node.label).second) return;
        auto props=reflection.properties(type);
        offsets << "namespace " << node.label << " {\n";
        std::set<std::string> identifiers;
        for (const auto& p:props) {
            auto id=identifier(p.name);
            if (!identifiers.insert(id).second) id += "_at_" + std::to_string(p.offset);
            offsets << "// " << p.type << "; element size=" << p.size << "; array dimension=" << p.dim << "\n";
            constant(offsets,"Offset",id,p.offset);
            if (p.type=="BoolProperty") {
                constant(offsets,"Offset",id+"_ByteOffset",p.byteOffset);
                constant(offsets,"std::uint8_t",id+"_ByteMask",p.byteMask);
                constant(offsets,"std::uint8_t",id+"_FieldMask",p.fieldMask);
            }
            Chain c=node.chain; c.push_back({p.offset,false});
            chain(node.label+"_"+id,c);
        }
        offsets << "}\n";
        for (const auto& p:props) if (p.type=="StructProperty" && pointer(p.structure) && p.dim==1) {
            Chain c=node.chain; c.push_back({p.offset,false});
            dump({node.label+"_"+identifier(p.name),node.address+p.offset,c},p.structure,depth+1);
        }
    }
    std::string finish(Address base, Address worldRva, Address namesRva, std::uint32_t timestamp,
                       std::uint32_t imageSize, const std::vector<Node>& nodes) {
        std::ostringstream out;
        out << "// Generated from the running Atomic Heart process. C++17 / x64.\n"
               "// Pointer values are session snapshots; resolve Chains again after loading.\n"
               "// RVAs are module-relative. Offsets are relative to their named container.\n"
               "// Names starting with v_ are reflected names prefixed for C++ compatibility.\n"
               "// A chain adds each offset, then reads a uint64 pointer ONLY when dereference=true.\n"
               "// For BoolProperty: read at field+ByteOffset and test ByteMask; do not overwrite the whole byte.\n"
               "#pragma once\n#include <cstdint>\n#include <array>\nnamespace AtomicHeart {\n"
               "using Pointer=std::uint64_t; using Offset=std::uint32_t;\n"
               "struct Step { std::uint64_t offset; bool dereference; };\nnamespace Build {\n";
        constant(out,"std::uint32_t","TimeDateStamp",timestamp);
        constant(out,"std::uint32_t","SizeOfImage",imageSize);
        out << "}\nnamespace RVAs {\n";
        constant(out,"Pointer","GWorld",worldRva); constant(out,"Pointer","GNames",namesRva);
        out << "}\nnamespace Pointers {\n"; constant(out,"Pointer","module",base);
        for (const auto& n:nodes) constant(out,"Pointer",n.label,n.address);
        out << "}\nnamespace Offsets {\n" << offsets.str() << "}\nnamespace Chains {\n" << chains.str() << "}\n}\n";
        return out.str();
    }
};
} // namespace ahscan

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
namespace ahscan {
struct Handle {
    HANDLE value=nullptr;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){ if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&)=delete; Handle& operator=(const Handle&)=delete;
};
class ProcessMemory final:public Memory {
    Handle process;
public:
    explicit ProcessMemory(DWORD pid):process(OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION,FALSE,pid)) {
        if (!process.value) throw std::runtime_error("OpenProcess failed: " + std::to_string(GetLastError()));
    }
    bool bytes(Address a,void* out,std::size_t n)const override {
        SIZE_T got=0;
        return ReadProcessMemory(process.value,reinterpret_cast<LPCVOID>(a),out,n,&got) && got==n;
    }
};
struct Target { DWORD pid; Address base; std::uint32_t size; };
inline Target target() {
    Handle list(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    PROCESSENTRY32W p{}; p.dwSize=sizeof(p); std::vector<DWORD> found;
    if (Process32FirstW(list.value,&p)) do {
        if (_wcsicmp(p.szExeFile,L"AtomicHeart-Win64-Shipping.exe")==0) found.push_back(p.th32ProcessID);
    } while(Process32NextW(list.value,&p));
    if (found.size()!=1) throw std::runtime_error("Start exactly one AtomicHeart-Win64-Shipping.exe instance");
    Handle modules(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,found[0]));
    MODULEENTRY32W e{}; e.dwSize=sizeof(e);
    if (!Module32FirstW(modules.value,&e)) throw std::runtime_error("Cannot enumerate x64 modules");
    do {
        if (_wcsicmp(e.szModule,L"AtomicHeart-Win64-Shipping.exe")==0)
            return {found[0],reinterpret_cast<Address>(e.modBaseAddr),e.modBaseSize};
    } while(Module32NextW(modules.value,&e));
    throw std::runtime_error("Main module not found");
}

// Scan readable chunks of executable PE sections. A failed page discards the
// overlap, preventing false signatures formed across inaccessible memory gaps.
inline std::set<Address> scanTargets(const Memory& m,Address base,const IMAGE_NT_HEADERS64& nt,
                                    const std::vector<int>& pattern) {
    std::set<Address> targets;
    auto dos=m.read<IMAGE_DOS_HEADER>(base);
    Address sections=base+dos.e_lfanew+4+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;
    for (unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
        auto s=m.read<IMAGE_SECTION_HEADER>(sections+Address(i)*sizeof(IMAGE_SECTION_HEADER));
        if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        if (s.VirtualAddress>=nt.OptionalHeader.SizeOfImage) continue;
        const std::uint64_t size=std::min<std::uint64_t>(s.Misc.VirtualSize,nt.OptionalHeader.SizeOfImage-s.VirtualAddress);
        std::vector<std::uint8_t> tail;
        for (std::uint64_t offset=0;offset<size;offset+=4096) {
            std::size_t count=std::size_t(std::min<std::uint64_t>(4096,size-offset));
            std::vector<std::uint8_t> block(tail); auto old=block.size(); block.resize(old+count);
            Address at=base+s.VirtualAddress+offset;
            if (!m.bytes(at,block.data()+old,count)) { tail.clear(); continue; }
            for (auto hit:matches(block,pattern)) {
                auto address=at-old+hit;
                auto candidate=ripTarget(m,address);
                if (candidate>=base && candidate+8<=base+nt.OptionalHeader.SizeOfImage) targets.insert(candidate);
            }
            auto keep=std::min(pattern.size()-1,block.size());
            tail.assign(block.end()-keep,block.end());
        }
    }
    return targets;
}
inline Address unique(const std::set<Address>& candidates,const std::string& label) {
    if (candidates.size()!=1) throw std::runtime_error(label+": expected one validated result, got "+std::to_string(candidates.size()));
    return *candidates.begin();
}
inline int run(int argc,char** argv) {
    static_assert(sizeof(void*)==8,"Compile for x64");
    // Explicit overrides are RVAs, never absolute addresses. They still undergo
    // structural validation; old repository constants are NOT automatic fallbacks.
    Address worldOverride=0,namesOverride=0;
    std::string output="AtomicHeartOffsets.h";
    for(int i=1;i<argc;++i) {
        std::string flag=argv[i];
        if (++i>=argc) throw std::runtime_error("Option requires a value");
        if(flag=="--output") output=argv[i];
        else if(flag=="--gworld-rva" || flag=="--gnames-rva") {
            std::string value=argv[i]; std::size_t consumed=0;
            auto number=std::stoull(value,&consumed,0);
            if(consumed!=value.size()) throw std::runtime_error("Invalid numeric RVA");
            (flag=="--gworld-rva"?worldOverride:namesOverride)=number;
        } else throw std::runtime_error("Unknown option: "+flag);
    }
    const auto t=target(); ProcessMemory m(t.pid);
    auto dos=m.read<IMAGE_DOS_HEADER>(t.base);
    if(dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000) throw std::runtime_error("Invalid DOS header");
    auto nt=m.read<IMAGE_NT_HEADERS64>(t.base+dos.e_lfanew);
    if(nt.Signature!=IMAGE_NT_SIGNATURE || nt.FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
       nt.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage!=t.size ||
       !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections>96) throw std::runtime_error("Invalid x64 PE header");
    if(worldOverride>=t.size || namesOverride>=t.size) throw std::runtime_error("RVA outside module");
    std::cout << "Reading engine globals...\n";
    const std::vector<int> namePattern={0x48,0x8D,0x0D,-1,-1,-1,-1,0xE8,-1,-1,-1,-1,0xC6,0x05};
    const std::vector<int> worldPattern={0x48,0x89,0x15,-1,-1,-1,-1,0x48,0x8D,0x05,-1,-1,-1,-1,0xC3};
    auto names=namesOverride?std::set<Address>{t.base+namesOverride}:scanTargets(m,t.base,nt,namePattern);
    std::set<Address> validNames;
    for(auto candidate:names) if(validNamePool(m,candidate)) validNames.insert(candidate);
    auto pool=unique(validNames,"GNames"); Reflection r(m,pool);
    auto worlds=worldOverride?std::set<Address>{t.base+worldOverride}:scanTargets(m,t.base,nt,worldPattern);
    std::set<Address> validWorlds;
    for(auto slot:worlds) try {
        auto w=m.read<Address>(slot);
        if(pointer(w) && r.isA(w,"World")) { r.find(w,{"OwningGameInstance"}); validWorlds.insert(slot); }
    } catch(const std::exception&) {}
    auto slot=unique(validWorlds,"GWorld (load a save first)");
    Node world{"world",m.read<Address>(slot),{{slot-t.base,true}}};
    auto child=[&](const Node& parent,const std::string& label,const std::vector<std::string>& aliases) {
        auto p=r.find(parent.address,aliases); auto chain=parent.chain; chain.push_back({p.offset,true});
        return Node{label,r.objectProperty(parent.address,p),chain};
    };
    Node gi=child(world,"gameInstance",{"OwningGameInstance"});
    auto lp=r.find(gi.address,{"LocalPlayers"});
    if(lp.type!="ArrayProperty" || lp.size!=16 || lp.dim!=1) throw std::runtime_error("Invalid LocalPlayers TArray metadata");
    auto inner=m.read<Address>(lp.metadata+r.layout.propertyExtra);
    if(!pointer(inner) || r.name(m.read<Address>(inner+r.layout.fieldType))!="ObjectProperty") throw std::runtime_error("Invalid LocalPlayers inner type");
    auto count=m.read<std::int32_t>(gi.address+lp.offset+8),capacity=m.read<std::int32_t>(gi.address+lp.offset+12);
    if(count<1 || count>4 || capacity<count || capacity>64) throw std::runtime_error("No valid local player");
    Chain localChain=gi.chain; localChain.push_back({lp.offset,true}); localChain.push_back({0,true});
    Node local{"localPlayer",follow(m,t.base,localChain),localChain};
    Node controller=child(local,"controller",{"PlayerController"});
    Node player;
    try { player=child(controller,"player",{"AcknowledgedPawn","Pawn"}); }
    catch(const std::exception&) { player=child(controller,"player",{"Pawn"}); }
    if(!r.isA(player.address,"AHBaseCharacter")) throw std::runtime_error("Pawn is not AHBaseCharacter; load gameplay");
    std::vector<Node> nodes={world,gi,local,controller,player};
    // Optional nodes must be absent, rather than fabricated, when a weapon is
    // holstered or a component has not been instantiated in the current level.
    auto optional=[&](const Node& parent,const std::string& label,
                      const std::vector<std::string>& aliases,const std::string& expectedClass) {
        try {
            // Prefer documented field names, then discover the field by its live
            // object's class. This handles renamed Blueprint member properties.
            Node result;
            try { result=child(parent,label,aliases); }
            catch(const std::exception&) {
                std::vector<Node> candidates;
                for(const auto& property:r.properties(r.klass(parent.address))) {
                    if(property.type!="ObjectProperty" || property.dim!=1) continue;
                    try {
                        auto value=r.objectProperty(parent.address,property);
                        if(r.isA(value,expectedClass)) {
                            auto chain=parent.chain; chain.push_back({property.offset,true});
                            candidates.push_back({label,value,chain});
                        }
                    } catch(const std::exception&) {}
                }
                if(candidates.size()!=1) throw std::runtime_error("No unique live "+expectedClass+" property");
                result=candidates.front();
            }
            if(!r.isA(result.address,expectedClass)) throw std::runtime_error("Unexpected class for "+label);
            nodes.push_back(result); std::cout<<label<<" found\n";
        } catch(const std::exception& e) { std::cout<<label<<" omitted: "<<e.what()<<"\n"; }
    };
    optional(player,"attributes",{"AttributeSet","BaseCharacterAttributeSet"},"AHBaseCharacterAttributeSet");
    optional(player,"inventory",{"InventoryPlayer","Inventory"},"AHInventoryPlayer");
    optional(player,"weapon",{"CurrentWeapon","WeaponInHands","EquippedWeapon","CurrentSlotWeapon"},"BaseWeapon");
    auto weapon=std::find_if(nodes.begin(),nodes.end(),[](const Node& n){return n.label=="weapon";});
    if(weapon!=nodes.end()) { auto copy=*weapon; optional(copy,"barrel",{"Barrel","EBBarrel"},"EBBarrel"); }
    Exporter exporter(r);
    for(const auto& n:nodes) { exporter.chain(n.label,n.chain); exporter.dump(n,r.klass(n.address)); }
    // Reject a moving snapshot before producing an apparently coherent header.
    // This detects changes, but is not an atomic snapshot of the entire process.
    for(const auto& n:nodes) if(follow(m,t.base,n.chain)!=n.address) throw std::runtime_error("Object changed during scan; repeat while paused");
    auto result=exporter.finish(t.base,slot-t.base,pool-t.base,nt.FileHeader.TimeDateStamp,t.size,nodes);
    // Assemble fully in memory first. Scan failures never replace an old header.
    // MoveFileEx then publishes the completed file atomically on the same volume.
    const auto temp=output+".tmp";
    { std::ofstream f(temp,std::ios::binary|std::ios::trunc); f.exceptions(std::ios::badbit|std::ios::failbit); f<<result; f.close(); }
    if(!MoveFileExA(temp.c_str(),output.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot publish output header");
    std::cout<<"Written: "<<output<<"\n";
    return 0;
}
} // namespace ahscan
#ifdef AH_SCANNER_MAIN
int main(int argc,char** argv) {
    try { return ahscan::run(argc,argv); }
    catch(const std::exception& e) { std::cerr<<"Scan failed: "<<e.what()<<"\nNo new header generated.\n"; return 1; }
}
#endif
#endif // _WIN32
