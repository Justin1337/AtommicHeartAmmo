// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "feature.h"

namespace ammo {
class AmmoFeature final:public Feature {
    PatchGroup changes;
    Identity selected;
public:
    const char* name()const override{return "Munition";}
    bool applicable(const FeatureContext& c)const override{return !c.melee;}

    // Reuse the working barrel preparation; resolve its LIVE component once per
    // batch in case it was replaced without replacing the weapon actor.
    void update(const FeatureContext& c)override{
        Address liveBarrel=0;bool prepared=false;
        try{liveBarrel=c.resolver.barrel(c.weapon);}
        catch(const std::exception& e){throw FeatureUnavailable(e.what());}
        if(selected.address!=c.weapon||!selected.alive(c.memory)||changes.owner.address!=liveBarrel){
            if(!restore(c.memory))throw std::runtime_error("Alter Lauf konnte nicht wiederhergestellt werden");
            // Preparation is read-only. Assign only a fully validated group.
            try{
                auto identity=Identity::capture(c.memory,c.weapon);
                auto fresh=makeBarrelPatch(c.resolver,c.memory,c.weapon,liveBarrel,c.log);
                changes=std::move(fresh);selected=identity;prepared=true;
            }catch(const std::exception& e){throw FeatureUnavailable(e.what());}
        }
        if(!selected.alive(c.memory))throw ResolutionFailure("Waffe wurde ersetzt");
        changes.apply(c.memory);
        // Sample once with labels; full graph dumps are manual diagnostics.
        if(prepared)logBarrelState(c.resolver,c.memory,liveBarrel,c.log,"AFTER");
    }

    // Reverse-order rollback preserves neighbouring packed bool bits.
    bool restore(WritableMemory& memory)override{
        if(!changes.restore(memory))return false;
        selected={};return true;
    }
    void discard()override{changes.clear();selected={};}
    bool pending()const override{return !changes.patches.empty();}
};

} // namespace ammo
