// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "feature.h"

namespace ammo {
class EnergyFeature final:public Feature {
    PatchGroup changes;
    Address base=0,current=0,maximum=0;
    float lastMaximum=0;
    std::string resourceName,maximumName;
    unsigned samples=0;

    struct Pool {const char* resource;const char* maximum;std::string evidence;};

    // V5 targeted the generic Energy attribute even though Electro's live
    // RemoveRageEffect points at GE_RemoveRageResourceWeapon_C. Resolve the pool
    // from that validated CLASS reference; never guess from a weapon's BP name
    // or silently fall back to an unrelated, already-full Energy attribute.
    static Pool weaponPool(const FeatureContext& c){
        if(!c.resolver.reflection.isA(c.weapon,"RangeWeapon"))
            throw std::runtime_error("Keine bestaetigte Schusswaffe fuer Waffenenergie");
        auto property=c.resolver.find(c.weapon,{"RemoveRageEffect"});
        if(property.type!="ClassProperty"||property.size!=8||property.dim!=1)
            throw std::runtime_error("RemoveRageEffect hat unbekannten Typ");
        auto effect=c.memory.read<Address>(c.weapon+property.offset);
        if(!pointer(effect)||!c.resolver.reflection.isA(effect,"Class"))
            throw std::runtime_error("Kein gueltiger Waffen-Verbrauchseffekt");
        auto name=c.resolver.reflection.objectName(effect);
        if(name!="GE_RemoveRageResourceWeapon_C")
            throw std::runtime_error("Waffenenergie-Pool fuer "+name+" nicht bestaetigt");
        return {"RageResource","MaxRageResource",name};
    }

    // Reject NaN, infinity, negative resources and implausible caps before any
    // remote write. Zero energy itself is valid and is explicitly refillable.
    static bool resource(float value){return std::isfinite(value)&&value>=0&&value<=1000000;}

    // Read all addresses and values first. No partial group is published if a
    // field is absent or has changed type in a new build.
    void prepare(const FeatureContext& c,Address attributes,const Pool& pool){
        auto newBase=c.objects.baseFloat(attributes,pool.resource);
        auto newCurrent=c.objects.currentFloat(attributes,pool.resource);
        auto newMaximum=c.objects.currentFloat(attributes,pool.maximum);
        if(newBase==newCurrent||newBase==newMaximum||newCurrent==newMaximum)
            throw std::runtime_error("Aliased energy fields");
        auto cap=c.memory.read<float>(newMaximum);
        if(!resource(cap)||cap==0)throw std::runtime_error(std::string(pool.maximum)+" ist null oder ungueltig");
        PatchGroup fresh;fresh.owner=Identity::capture(c.memory,attributes);
        fresh.addFloat(c.memory,newBase,cap);
        fresh.addFloat(c.memory,newCurrent,cap);
        c.log<<"Weapon energy pool="<<pool.resource<<" maximum="<<pool.maximum
             <<" evidence=RemoveRageEffect:"<<pool.evidence<<"\n";
        c.log<<pool.resource<<" BEFORE: object="<<Memory::hex(attributes)
             <<" BaseValue@"<<Memory::hex(newBase)<<"="<<c.memory.read<float>(newBase)
             <<" CurrentValue@"<<Memory::hex(newCurrent)<<"="<<c.memory.read<float>(newCurrent)
             <<" "<<pool.maximum<<".CurrentValue@"<<Memory::hex(newMaximum)<<"="<<cap<<"\n";
        changes=std::move(fresh);base=newBase;current=newCurrent;maximum=newMaximum;
        resourceName=pool.resource;maximumName=pool.maximum;lastMaximum=0;samples=0;
    }

public:
    const char* name()const override{return "Energie";}
    bool applicable(const FeatureContext& c)const override{return !c.melee;}

    // Keep the shared player pool across ranged-weapon switches. Refresh its cap
    // every batch: upgrades/buffs need no toggle and no stale target is reused.
    // The base/current write behavior follows upstream SetAttr, but V6 selects
    // the weapon's RageResource pool instead of copying its generic Energy choice.
    void update(const FeatureContext& c)override{
        Address attributes=0;
        Pool pool{};
        try{pool=weaponPool(c);attributes=c.objects.attributes(c.player);}
        catch(const std::exception& e){throw FeatureUnavailable(e.what());}
        if(changes.owner.address!=attributes||changes.patches.empty()||resourceName!=pool.resource){
            if(!restore(c.memory))throw std::runtime_error("Alte Energie konnte nicht wiederhergestellt werden");
            try{prepare(c,attributes,pool);}
            catch(const std::exception& e){throw FeatureUnavailable(e.what());}
        }
        if(!changes.owner.alive(c.memory))throw ResolutionFailure("Energieobjekt wurde ersetzt");
        auto cap=c.memory.read<float>(maximum);
        auto liveBase=c.memory.read<float>(base),liveCurrent=c.memory.read<float>(current);
        if(!resource(cap)||cap==0||!resource(liveBase)||!resource(liveCurrent))
            throw FeatureUnavailable("Energiewerte ungueltig; keine Auffuellung");
        // Change targets only, never the original snapshots. PatchGroup skips
        // unchanged floats, so idle/full energy produces zero remote writes.
        for(auto& patch:changes.patches){patch.target=0;std::memcpy(&patch.target,&cap,sizeof(cap));}
        changes.apply(c.memory);
        if(cap!=lastMaximum){
            c.log<<resourceName<<" AFTER: BaseValue="<<c.memory.read<float>(base)
                 <<" CurrentValue="<<c.memory.read<float>(current)<<" "<<maximumName<<".CurrentValue="<<cap<<"\n";
            lastMaximum=cap;
        }
        // Sparse samples distinguish an idle full pool from an actually drained
        // and refilled pool. At 500 ms, twenty enabled batches are about 10 s.
        // No per-shot/per-tick log flood or additional scan is introduced.
        if(++samples%20==0){
            c.log<<resourceName<<" SAMPLE: beforeBase="<<liveBase<<" beforeCurrent="<<liveCurrent
                 <<" maximum="<<cap<<" refillNeeded="<<(liveBase!=cap||liveCurrent!=cap)
                 <<" afterBase="<<c.memory.read<float>(base)<<" afterCurrent="<<c.memory.read<float>(current)<<"\n";
            c.log.flush();
        }
    }

    // Restore only the selected resource floats; the maximum stays read-only.
    bool restore(WritableMemory& memory)override{
        if(!changes.restore(memory))return false;
        base=current=maximum=0;lastMaximum=0;samples=0;resourceName.clear();maximumName.clear();return true;
    }
    void discard()override{changes.clear();base=current=maximum=0;lastMaximum=0;samples=0;resourceName.clear();maximumName.clear();}
    bool pending()const override{return !changes.patches.empty();}
};

} // namespace ammo
