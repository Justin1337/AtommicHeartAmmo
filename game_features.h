// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Feature registry and the one registration point for the built-in modules.
#include "features/ammo.h"
#include "features/energy.h"

namespace ammo {
// Central registry. A new module implements Feature and is registered with add;
// the GUI, timer, connection, F10 and cleanup code require no feature-specific edit.
class FeatureSet {
    struct Entry {std::unique_ptr<Feature> feature;std::string lastReport;};
    std::vector<Entry> entries;
public:
    // Registration owns the module for the lifetime of the trainer.
    void add(std::unique_ptr<Feature> feature){
        if(!feature)throw std::invalid_argument("Null feature");
        for(const auto& entry:entries)if(std::string(entry.feature->name())==feature->name())throw std::invalid_argument("Duplicate feature name");
        entries.push_back({std::move(feature),{}});
    }

    // Unsupported modules remain isolated; actual write/read failures abort the
    // batch. Reports/logs change only on state changes, avoiding 500-ms spam.
    std::string update(const FeatureContext& c){
        std::string result;
        for(auto& entry:entries){
            std::string report;
            try{
                if(entry.feature->applicable(c)){
                    entry.feature->update(c);report=std::string(entry.feature->name())+" aktiv";
                }else{
                    if(!entry.feature->restore(c.memory))throw std::runtime_error("Pausieren: Wiederherstellung fehlgeschlagen");
                    report=std::string(entry.feature->name())+" pausiert";
                }
            }
            catch(const FeatureUnavailable& e){
                if(!entry.feature->restore(c.memory))throw std::runtime_error(std::string(entry.feature->name())+": Wiederherstellung fehlgeschlagen");
                report=std::string(entry.feature->name())+" nicht verfuegbar: "+e.what();
            }
            if(report!=entry.lastReport){c.log<<"Feature: "<<report<<"\n";entry.lastReport=report;}
            if(!result.empty())result+=" | ";
            result+=report;
        }
        return result.empty()?"Keine Funktionen registriert":result;
    }

    // Try EVERY module even when one restore fails. Its state is retained for
    // the next cleanup attempt; successes are safe to call repeatedly.
    bool restore(WritableMemory& memory){
        bool okay=true;
        for(auto it=entries.rbegin();it!=entries.rend();++it){
            try{if(!it->feature->restore(memory))okay=false;}
            catch(const std::exception&){okay=false;}
            it->lastReport.clear();
        }
        return okay;
    }
    // Stale contexts must never trigger restoration writes.
    void discard(){for(auto& entry:entries){entry.feature->discard();entry.lastReport.clear();}}
    bool pending()const{for(const auto& entry:entries)if(entry.feature->pending())return true;return false;}
};

// The single registration point. Add another Feature here to extend F10's set.
inline FeatureSet defaultFeatures(){
    FeatureSet features;
    features.add(std::make_unique<AmmoFeature>());
    features.add(std::make_unique<EnergyFeature>());
    return features;
}
} // namespace ammo
