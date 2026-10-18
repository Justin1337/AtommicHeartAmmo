// Synthetic tests of production feature modules; no real process is accessed.
#include "../game_features.h"
#include "fixture.h"
#include <cassert>
#include <limits>
using namespace ammo;

template<class F>void rejects(F action){
    bool failed=false;try{action();}catch(const std::exception&){failed=true;}assert(failed);
}

// Nonstandard nested offsets catch accidental +8/+12 assumptions. Base/current
// intentionally differ so a one-field implementation cannot pass restoration.
struct EnergyFixture:Fixture {
    Address pc=cls("AHBaseCharacter"),ac=cls("AHBaseCharacterAttributeSet");
    Address data=cls("GameplayAttributeData",0,0x40);
    Address pawn=object(pc,"Player"),set=object(ac,"Attributes");
    Address first=object(cls("RangeWeapon"),"Electro"),second=object(m.read<Address>(first+0x10),"Dominator");
    Address b=set+0x154,c=set+0x16C,cap=set+0x22C;
    Address baseMetadata=0,currentMetadata=0,effect=0,effectProperty=0;
    Address genericBase=set+0x354,genericCurrent=set+0x36C;
    std::unique_ptr<Resolver> resolver;
    std::unique_ptr<RuntimeObjects> runtime;
    std::ostringstream log;
    EnergyFixture(){
        prop(pc,"AttributeSet","ObjectProperty",0xE0,8);m.put<Address>(pawn+0xE0,set);
        baseMetadata=prop(data,"BaseValue","FloatProperty",0x14,4);
        currentMetadata=prop(data,"CurrentValue","FloatProperty",0x2C,4);
        auto energy=prop(ac,"RageResource","StructProperty",0x140,0x40);
        auto maximum=prop(ac,"MaxRageResource","StructProperty",0x200,0x40);
        m.put<Address>(energy+0x78,data);m.put<Address>(maximum+0x78,data);
        m.put<float>(b,4);m.put<float>(c,2);m.put<float>(cap,75);
        prop(ac,"Health","FloatProperty",0x300,4);m.put<float>(set+0x300,33);
        // Reproduce the reported distinction: generic Energy is full while the
        // weapon pool is a separate resource. A V5-style refill cannot pass.
        auto generic=prop(ac,"Energy","StructProperty",0x340,0x40);
        auto genericMax=prop(ac,"MaxEnergy","StructProperty",0x400,0x40);
        m.put<Address>(generic+0x78,data);m.put<Address>(genericMax+0x78,data);
        m.put<float>(genericBase,100);m.put<float>(genericCurrent,100);m.put<float>(set+0x42C,100);
        auto meta=cls("Class");effect=cls("GE_RemoveRageResourceWeapon_C");m.put<Address>(effect+0x10,meta);
        effectProperty=prop(m.read<Address>(first+0x10),"RemoveRageEffect","ClassProperty",0xA0,8);
        m.put<Address>(first+0xA0,effect);m.put<Address>(second+0xA0,effect);
        rebuild();
    }
    void rebuild(){resolver=std::make_unique<Resolver>(m,pool);runtime=std::make_unique<RuntimeObjects>(m,*resolver,0);}
    FeatureContext context(Address weapon=0){return {m,*resolver,*runtime,pawn,weapon?weapon:first,log};}
};

// A tiny extension proves central lifecycle registration independently of the
// two built-in modules; failures must not prevent other modules being restored.
struct Counts {int updates=0,restores=0,discards=0;bool failRestore=false;};
class Probe final:public Feature {
    Counts& counts;std::string title;bool unavailable;
public:
    Probe(Counts& state,std::string name,bool absent=false):counts(state),title(std::move(name)),unavailable(absent){}
    const char* name()const override{return title.c_str();}
    void update(const FeatureContext&)override{++counts.updates;if(unavailable)throw FeatureUnavailable("Test layout absent");}
    bool restore(WritableMemory&)override{++counts.restores;return !counts.failRestore;}
    void discard()override{++counts.discards;}
    bool pending()const override{return counts.updates>counts.restores;}
};

// Build a complete barrel with shifted fields and a validated projectile UClass.
// This exercises AmmoFeature's lifecycle, not just the older low-level builder.
Address addBarrels(EnergyFixture& f,Address& other){
    auto range=f.m.read<Address>(f.first+0x10),barrelClass=f.cls("EBBarrel");
    auto barrel=f.object(barrelClass,"FirstBarrel");other=f.object(barrelClass,"OtherBarrel");
    auto meta=f.cls("Class"),bullet=f.cls("EnergyBullet");f.m.put<Address>(bullet+0x10,meta);
    f.prop(range,"Bullet","ClassProperty",0x70,8);f.prop(range,"Barrel","ObjectProperty",0x88,8);
    f.m.put<Address>(f.first+0x70,bullet);f.m.put<Address>(f.second+0x70,bullet);
    f.m.put<Address>(f.first+0x88,barrel);f.m.put<Address>(f.second+0x88,other);
    for(auto name:{"ShootingBlocked","CycleAmmoUnlimited","LoadNext"}){
        int offset=std::string(name)=="ShootingBlocked"?0x101:std::string(name)=="LoadNext"?0x103:0x102;
        auto p=f.prop(barrelClass,name,"BoolProperty",offset,1);
        f.m.put<std::uint8_t>(p+0x79,0);f.m.put<std::uint8_t>(p+0x7A,1);f.m.put<std::uint8_t>(p+0x7B,255);
    }
    f.prop(barrelClass,"CycleAmmoCount","IntProperty",0x110,4);
    f.prop(barrelClass,"CycleAmmoPos","IntProperty",0x114,4);
    f.prop(barrelClass,"ChamberedBullet","ClassProperty",0x120,8);
    for(auto object:{barrel,other}){
        f.m.put<std::uint8_t>(object+0x101,1);f.m.put<std::int32_t>(object+0x114,-1);
    }
    f.rebuild();return barrel;
}

int main(){
    {
        EnergyFixture f;EnergyFeature energy;auto context=f.context();
        int before=f.m.writes;energy.update(context);
        assert(f.m.read<float>(f.b)==75&&f.m.read<float>(f.c)==75);
        assert(f.m.writes==before+2);
        assert(f.m.read<float>(f.cap)==75&&f.m.read<float>(f.set+0x300)==33);
        // Full/idle: zero writes, and cached nested lookups need only klass read.
        before=f.m.writes;energy.update(context);assert(f.m.writes==before);
        int reads=f.m.reads;assert(f.runtime->baseFloat(f.set,"RageResource")==f.b);assert(f.m.reads==reads+1);
        // Both depleted floats are repaired; live cap follows upgrades and buffs.
        f.m.put<float>(f.b,0);f.m.put<float>(f.c,0);before=f.m.writes;
        energy.update(context);assert(f.m.writes==before+2);
        f.m.put<float>(f.cap,150);before=f.m.writes;energy.update(context);
        assert(f.m.writes==before+2&&f.m.read<float>(f.b)==150&&f.m.read<float>(f.c)==150);
        assert(f.log.str().find("MaxRageResource.CurrentValue=150")!=std::string::npos);
        // Weapon switch keeps shared energy and original snapshots; no rollback
        // to the pre-toggle value occurs in the middle of the switch.
        auto switched=f.context(f.second);before=f.m.writes;energy.update(switched);
        assert(f.m.writes==before);
        assert(energy.restore(f.m));assert(f.m.read<float>(f.b)==4&&f.m.read<float>(f.c)==2);
        assert(f.m.read<float>(f.cap)==150&&f.m.read<float>(f.set+0x300)==33);
        before=f.m.writes;assert(energy.restore(f.m));assert(f.m.writes==before);
        // A first-time zero snapshot is valid and must restore to zero.
        f.m.put<float>(f.b,0);f.m.put<float>(f.c,0);energy.update(context);
        assert(energy.restore(f.m));assert(f.m.read<float>(f.b)==0&&f.m.read<float>(f.c)==0);
    }
    {
        EnergyFixture f;FeatureSet features;features.add(std::make_unique<EnergyFeature>());
        auto context=f.context();features.update(context);
        // A zero maximum is a waitable per-feature failure, never an arbitrary
        // replacement amount. Original energy is restored and retry can recover.
        f.m.put<float>(f.cap,0);
        auto status=features.update(context);assert(status.find("nicht verfuegbar")!=std::string::npos);
        assert(f.m.read<float>(f.b)==4&&f.m.read<float>(f.c)==2);
        f.m.put<float>(f.cap,60);assert(features.update(context)=="Energie aktiv");
        assert(f.m.read<float>(f.b)==60&&f.m.read<float>(f.c)==60);
        assert(features.restore(f.m));
    }
    for(float invalid:{-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),1000001.f}){
        EnergyFixture f;EnergyFeature energy;auto context=f.context();f.m.put<float>(f.cap,invalid);
        int before=f.m.writes;rejects([&]{energy.update(context);});assert(f.m.writes==before);
    }
    {
        EnergyFixture f;EnergyFeature energy;auto context=f.context();energy.update(context);
        // A live resource cannot silently become a bit-pattern write target.
        f.m.put<float>(f.b,std::numeric_limits<float>::quiet_NaN());int before=f.m.writes;
        rejects([&]{energy.update(context);});assert(f.m.writes==before);assert(energy.restore(f.m));
    }
    {
        EnergyFixture f;EnergyFeature energy;
        // Wrong type and bounds fail during read-only preparation. A second
        // fixture below removes BaseValue: old CurrentValue-only code must fail.
        f.m.put<std::int32_t>(f.baseMetadata+0x3C,8);f.rebuild();auto context=f.context();
        int before=f.m.writes;rejects([&]{energy.update(context);});assert(f.m.writes==before);
    }
    {
        EnergyFixture f;EnergyFeature energy;
        f.m.put<std::int32_t>(f.baseMetadata+0x4C,0x40);f.rebuild();auto context=f.context();
        int before=f.m.writes;rejects([&]{energy.update(context);});assert(f.m.writes==before);
    }
    {
        EnergyFixture f;EnergyFeature energy;
        f.fname(f.baseMetadata+0x28,"MissingBase");f.rebuild();auto context=f.context();
        int before=f.m.writes;rejects([&]{energy.update(context);});assert(f.m.writes==before);
    }
    {
        EnergyFixture f;EnergyFeature energy;auto context=f.context();
        // A second-field write failure leaves the first write restorable; failed
        // restoration state is kept until the remote write succeeds again.
        f.m.failAt=f.c;rejects([&]{energy.update(context);});assert(f.m.read<float>(f.b)==75);
        assert(!energy.restore(f.m));assert(energy.pending());
        f.m.failAt=0;assert(energy.restore(f.m));assert(f.m.read<float>(f.b)==4&&f.m.read<float>(f.c)==2);
    }
    {
        EnergyFixture f;EnergyFeature energy;auto context=f.context();energy.update(context);
        // Recycled address: no writes or restoration to the replacement object.
        f.m.put<std::int32_t>(f.set+0xC,123);int before=f.m.writes;
        rejects([&]{energy.update(context);});assert(f.m.writes==before);
        assert(!energy.restore(f.m));assert(f.m.writes==before);
    }
    {
        EnergyFixture f;EnergyFeature energy;auto context=f.context();energy.update(context);
        // Discard on World/player change is strictly write-free.
        int before=f.m.writes;energy.discard();assert(!energy.pending()&&f.m.writes==before);
    }
    {
        EnergyFixture f;EnergyFeature energy;auto context=f.context();energy.update(context);
        // New live AttributeSet gets its own snapshot, not addresses cached for
        // the prior object. Schema offsets may be cached; values must not be.
        auto replacement=f.object(f.ac,"NewAttributes");
        f.m.put<float>(replacement+0x154,8);f.m.put<float>(replacement+0x16C,6);f.m.put<float>(replacement+0x22C,90);
        f.m.put<Address>(f.pawn+0xE0,replacement);energy.update(context);
        assert(f.m.read<float>(f.b)==4&&f.m.read<float>(f.c)==2);
        assert(f.m.read<float>(replacement+0x154)==90&&f.m.read<float>(replacement+0x16C)==90);
        assert(energy.restore(f.m));assert(f.m.read<float>(replacement+0x154)==8&&f.m.read<float>(replacement+0x16C)==6);
    }
    {
        EnergyFixture f;FeatureSet features;Counts a,b;
        features.add(std::make_unique<Probe>(a,"Extra"));features.add(std::make_unique<Probe>(b,"Absent",true));
        auto context=f.context();auto report=features.update(context);
        assert(a.updates==1&&b.updates==1&&b.restores==1);
        assert(report.find("Extra aktiv")!=std::string::npos&&report.find("Absent nicht verfuegbar")!=std::string::npos);
        auto log=f.log.str();features.update(context);assert(f.log.str()==log);
        b.failRestore=true;assert(!features.restore(f.m));assert(a.restores==1);
        b.failRestore=false;assert(features.restore(f.m));
        features.discard();assert(a.discards==1&&b.discards==1);
        rejects([&]{features.add(std::make_unique<Probe>(a,"Extra"));});
        rejects([&]{features.add(nullptr);});
    }
    {
        // The actual default registry can refill energy even if this synthetic
        // weapon has no EBBarrel. No false claim that ammunition was activated.
        EnergyFixture f;auto features=defaultFeatures();auto context=f.context();
        auto report=features.update(context);
        assert(report.find("Munition nicht verfuegbar")!=std::string::npos);
        assert(report.find("Energie aktiv")!=std::string::npos);
        assert(f.m.read<float>(f.b)==75&&f.m.read<float>(f.c)==75);assert(features.restore(f.m));
    }
    {
        EnergyFixture f;Address other=0;auto barrel=addBarrels(f,other);
        auto features=defaultFeatures();Counts extra;features.add(std::make_unique<Probe>(extra,"Extension"));
        auto context=f.context();auto report=features.update(context);
        assert(report=="Munition aktiv | Energie aktiv | Extension aktiv");
        assert(f.m.read<std::int32_t>(barrel+0x110)==1&&f.m.read<std::uint8_t>(barrel+0x101)==0);
        assert(f.m.read<float>(f.b)==75&&f.m.read<float>(f.c)==75);
        int before=f.m.writes;features.update(context);assert(f.m.writes==before);
        // The actor stays but its barrel changes. Restore old component, update
        // the live replacement; energy retains its independent original snapshot.
        f.m.put<Address>(f.first+0x88,other);features.update(context);
        assert(f.m.read<std::int32_t>(barrel+0x110)==0&&f.m.read<std::uint8_t>(barrel+0x101)==1);
        assert(f.m.read<std::int32_t>(other+0x110)==1&&f.m.read<float>(f.c)==75);
        auto switched=f.context(f.second);features.update(switched);
        assert(f.m.read<float>(f.c)==75);
        // Modules opt out on melee individually. A newly registered general
        // feature continues updating without a new branch in trainer.cpp.
        auto melee=f.context();melee.melee=true;int updates=extra.updates;
        report=features.update(melee);
        assert(report=="Munition pausiert | Energie pausiert | Extension aktiv");
        assert(extra.updates==updates+1);
        assert(f.m.read<float>(f.b)==4&&f.m.read<float>(f.c)==2);
        assert(f.m.read<std::int32_t>(other+0x110)==0&&f.m.read<std::uint8_t>(other+0x101)==1);
        before=f.m.writes;features.update(melee);assert(f.m.writes==before);
        assert(features.restore(f.m));
    }
    {
        // Exact conceptual regression from log (3): both old Energy values are
        // 100, Electro still has no weapon resource. Only RageResource changes.
        EnergyFixture f;EnergyFeature energy;auto context=f.context();
        f.m.put<float>(f.b,0);f.m.put<float>(f.c,0);int before=f.m.writes;
        energy.update(context);assert(f.m.writes==before+2);
        assert(f.m.read<float>(f.b)==75&&f.m.read<float>(f.c)==75);
        assert(f.m.read<float>(f.genericBase)==100&&f.m.read<float>(f.genericCurrent)==100);
        assert(f.log.str().find("pool=RageResource")!=std::string::npos);
        assert(f.log.str().find("GE_RemoveRageResourceWeapon_C")!=std::string::npos);
        assert(energy.restore(f.m));
        // Even a non-full generic Energy value must not be changed as a side effect.
        f.m.put<float>(f.genericBase,42);f.m.put<float>(f.genericCurrent,40);
        energy.update(context);assert(f.m.read<float>(f.genericBase)==42&&f.m.read<float>(f.genericCurrent)==40);
        assert(energy.restore(f.m));
    }
    for(int invalid=0;invalid<5;++invalid){
        EnergyFixture f;EnergyFeature energy;
        if(invalid==0)f.m.put<Address>(f.first+0xA0,0);
        if(invalid==1)f.m.put<Address>(f.first+0xA0,3);
        if(invalid==2)f.fname(f.effect+0x18,"GE_RemoveHealth_C");
        if(invalid==3)f.m.put<Address>(f.effectProperty+8,f.type("ObjectProperty"));
        if(invalid==4)f.m.put<Address>(f.effect+0x10,f.cls("DataAsset"));
        f.rebuild();auto context=f.context();int before=f.m.writes;
        rejects([&]{energy.update(context);});assert(f.m.writes==before);
        assert(f.m.read<float>(f.b)==4&&f.m.read<float>(f.c)==2);
    }
    {
        // A changed consumption reference on the same actor must be revalidated;
        // it cannot keep an old, apparently active resource patch alive.
        EnergyFixture f;FeatureSet features;features.add(std::make_unique<EnergyFeature>());
        auto context=f.context();features.update(context);
        f.m.put<Address>(f.first+0xA0,0);
        assert(features.update(context).find("nicht verfuegbar")!=std::string::npos);
        assert(f.m.read<float>(f.b)==4&&f.m.read<float>(f.c)==2);
    }
    {
        // The added diagnostic is sparse: no sample before batch 20, then one
        // before/after sample records a real refill without logging every tick.
        EnergyFixture f;EnergyFeature energy;auto context=f.context();
        for(int i=0;i<19;++i)energy.update(context);
        assert(f.log.str().find(" SAMPLE:")==std::string::npos);
        f.m.put<float>(f.b,0);f.m.put<float>(f.c,0);int before=f.m.writes;
        energy.update(context);assert(f.m.writes==before+2);
        assert(f.log.str().find("RageResource SAMPLE: beforeBase=0 beforeCurrent=0 maximum=75 refillNeeded=1 afterBase=75 afterCurrent=75")!=std::string::npos);
        assert(energy.restore(f.m));
    }
    std::cout<<"PASS: weapon-effect-selected RageResource vs full generic Energy, null/unknown/mistyped/changed consumption effects, both reflected resource floats, zero-energy refill, live cap refresh, shared weapon pool, zero idle writes, nested-offset cache, independent snapshots, invalid values/layouts, recycled identity, write-free discard, partial rollback/retry, AttributeSet replacement, extension registration and isolated unsupported modules.\n";
}
