// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../runtime_scan.h"
#include <memory>

namespace ammo {
// Borrowed live context, valid for ONE validated 500-ms batch. Features must not
// retain references to it or do their own weapon/World graph traversal.
struct FeatureContext {
    WritableMemory& memory;
    Resolver& resolver;
    RuntimeObjects& objects;
    Address player,weapon;
    std::ostream& log;
    bool melee=false;
};

// Only preparation/read failures may use this exception. Write failures escape
// normally so the trainer disables the batch and attempts complete restoration.
struct FeatureUnavailable:std::runtime_error {using std::runtime_error::runtime_error;};

// Extension contract: update applies one batch; restore is retryable; discard
// forgets stale state WITHOUT writing (process exit / player or World change).
class Feature {
public:
    virtual ~Feature()=default;
    virtual const char* name()const=0;
    // New modules run for all contexts by default (e.g. a future display-only
    // module may remain useful while melee is selected). Each opts out itself.
    virtual bool applicable(const FeatureContext&)const{return true;}
    virtual void update(const FeatureContext& context)=0;
    virtual bool restore(WritableMemory& memory)=0;
    virtual void discard()=0;
    virtual bool pending()const=0;
};

} // namespace ammo
