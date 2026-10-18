// SPDX-License-Identifier: GPL-3.0-or-later
// Native external trainer: remote data reads/writes only; no injection or hooks.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include "game_features.h"
#include <memory>
#include <filesystem>
#include <future>
#include <chrono>
namespace {
using namespace ammo;
class GameMemory final:public WritableMemory {
    ahscan::Handle process;
public:
    // Open one external process handle; no remote execution rights are requested.
    explicit GameMemory(DWORD pid):process(OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|PROCESS_VM_WRITE|PROCESS_VM_OPERATION,FALSE,pid)){
        if(!process.value)throw std::runtime_error("OpenProcess: "+std::to_string(GetLastError()));
    }
    // Accept only complete remote reads, never partially initialized values.
    bool bytes(Address a,void* out,std::size_t n)const override{SIZE_T got=0;return ReadProcessMemory(process.value,reinterpret_cast<LPCVOID>(a),out,n,&got)&&got==n;}
    // Report partial writes as failures so PatchGroup retains rollback state.
    bool write(Address a,const void* data,std::size_t n)override{SIZE_T done=0;return WriteProcessMemory(process.value,reinterpret_cast<LPVOID>(a),data,n,&done)&&done==n;}
    // Check the open process, avoiding PID reuse when detecting game exit.
    bool running()const{DWORD code=0;return GetExitCodeProcess(process.value,&code)&&code==STILL_ACTIVE;}
};
// Convert UTF-8 status text to Windows Unicode for all controls.
std::wstring wide(const std::string& s){
    int count=MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,nullptr,0);if(count<=0)return L"Fehler";
    std::wstring out(static_cast<std::size_t>(count),0);MultiByteToWideChar(CP_UTF8,0,s.c_str(),-1,out.data(),count);out.pop_back();return out;
}
class Trainer {
    std::unique_ptr<GameMemory> m;std::unique_ptr<Resolver> r;
    std::unique_ptr<RuntimeObjects> objects;
    struct Connection {std::unique_ptr<GameMemory> memory;Address base;Globals globals;ScanStats stats;std::uint32_t timestamp,imageSize;};
    std::future<Connection> pending;DWORD pendingPid=0,failedPid=0;
    Globals globals;WorkGate work;
    Address world=0,player=0,weapon=0;
    FeatureSet features=defaultFeatures();ULONGLONG lastConnect=0;
    std::ofstream log;std::string previous,lastResolutionError;
    // Read-only linked-object diagnostics. Alias pointers are deduplicated so
    // RageComponent/BaseRageComponent do not dump the same object twice.
    void describeLinks(Address source,const std::vector<std::string>& names){
        std::set<Address> seen;
        for(const auto& name:names)try{
            auto property=r->find(source,{name});
            if(property.type=="ObjectProperty"&&property.size==8&&property.dim==1){
                auto object=m->read<Address>(source+property.offset);
                if(pointer(object)&&seen.insert(object).second){
                    log<<"=== "<<name<<" ===\n";r->describe(object,log);
                }
            }
        }catch(const std::exception& e){log<<name<<": "<<e.what()<<"\n";}
    }
    // Restore all modules only while the captured World/player context is live.
    bool cleanup(){
        if(!m)return true;
        if(!m->running()){features.discard();return true;}
        if(!features.pending())return true;
        // A load screen can invalidate the chain before tick sees a new world.
        // Do not restore through any pointer from that previous context.
        try { auto live=objects->player();if(live.world!=world || live.player!=player){features.discard();return false;} }
        catch(const std::exception&){features.discard();return false;}
        return features.restore(*m);
    }
    // Poll one background, read-only bootstrap; construct live resolvers on success.
    bool connect(){
        if(pending.valid()){
            if(pending.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)return false;
            try{
                auto found=pending.get();
                auto newResolver=std::make_unique<Resolver>(*found.memory,found.globals.names);
                auto newObjects=std::make_unique<RuntimeObjects>(*found.memory,*newResolver,found.globals.worldSlot);
                m=std::move(found.memory);r=std::move(newResolver);objects=std::move(newObjects);
                globals=found.globals;
                log<<"Pattern bootstrap: module="<<Memory::hex(found.base)<<" GNames="<<Memory::hex(globals.names)<<" GWorld slot="<<Memory::hex(globals.worldSlot)
                   <<" timestamp="<<Memory::hex(found.timestamp)<<" imageSize="<<Memory::hex(found.imageSize)
                   <<" scannedBytes="<<found.stats.bytes<<" reads="<<found.stats.reads<<" unreadablePages="<<found.stats.unreadablePages<<"\n";
                status("Verbunden per Pattern-Suche. Zyklus 500 ms; F10 zum Einschalten.");return true;
            }catch(...){failedPid=pendingPid;throw;}
        }
        auto now=GetTickCount64();if(now-lastConnect<1500)return false;lastConnect=now;
        auto target=ahscan::target();
        // Unsupported patterns are not rescanned every tick. Retry explicitly
        // with the rescan button or automatically when a NEW process starts.
        if(target.pid==failedPid)return false;
        pendingPid=target.pid;
        pending=std::async(std::launch::async,[target](){
            auto memory=std::make_unique<GameMemory>(target.pid);
            auto image=Image::parse(*memory,target.base,target.size);
            auto raw=scanGlobals(*memory,image);
            auto resolved=validateGlobals(*memory,raw);
            return Connection{std::move(memory),target.base,resolved,raw.stats,image.timestamp,image.size};
        });
        status("Einmalige Pattern-Suche laeuft im Hintergrund...");return false;
    }
public:
    bool enabled=false;std::string message="Warte auf Atomic Heart...",weaponName="-";
    // Place the append-only session log beside the executable.
    Trainer(){wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);log.open(std::filesystem::path(path).parent_path()/"AtomicHeartAmmo.log",std::ios::app);log<<"\nExternal trainer v6 started (weapon RageResource, modular features, 500 ms work loop)\n";}
    // Normal shutdown provides a final restoration attempt.
    ~Trainer(){if(m&&m->running())cleanup();}
    // Update UI state and log only transitions, keeping the idle path quiet.
    void status(const std::string& text){message=text;if(text!=previous){log<<text<<"\n";log.flush();previous=text;}}
    // F10 is immediate; pending restoration blocks re-enabling safely.
    void toggle(){
        if(enabled){enabled=false;if(cleanup())status("Unendliche Munition AUS.");else status("Aus; Wiederherstellung wird erneut versucht.");}
        else{if(!m){status(pending.valid()?"Pattern-Suche laeuft noch; danach F10 druecken.":"Zuerst Spiel starten und Spielstand laden.");return;}if(!cleanup()){status("Wiederherstellung offen; Einschalten blockiert.");return;}enabled=true;weapon=0;lastResolutionError.clear();status("AN: suche aktuelle Waffe...");}
    }
    // Explicit diagnostics are read-only and include actual energy float values.
    void diagnose(){
        if(!r||!player){status("Diagnose: kein geladener Spieler");return;}
        log<<"\n=== V6 RESOLVER / ENERGY DIAGNOSTIC ===\n";
        try {auto selected=r->selectedWeapon(player,&log);
            log<<"Resolved weapon="<<Memory::hex(selected)<<" via "<<r->lastRoute<<"\n";
            r->describe(selected,log);
            // Read-only extra detail for weapon-specific energy costs. Shared
            // data assets are never modified by diagnostics or feature code.
            describeLinks(selected,{"BaseWeaponAttack","WeaponSettings"});
            try {r->describe(r->barrel(selected),log);}catch(const std::exception& e){log<<"Barrel: "<<e.what()<<"\n";}
        }catch(const std::exception& e){log<<"Resolution result: "<<e.what()<<"\n";}
        describeLinks(player,{"RageComponent","BaseRageComponent"});
        try{
            auto set=objects->attributes(player);
            log<<"=== Energy attributes object="<<Memory::hex(set)<<" ===\n";
            for(const auto& field:{"RageResource","MaxRageResource","Energy","MaxEnergy"}){
                auto b=objects->baseFloat(set,field),c=objects->currentFloat(set,field);
                log<<field<<".BaseValue@"<<Memory::hex(b)<<"="<<m->read<float>(b)
                   <<" CurrentValue@"<<Memory::hex(c)<<"="<<m->read<float>(c)<<"\n";
            }
        }catch(const std::exception& e){log<<"Energy diagnostic: "<<e.what()<<"\n";}
        log<<"=== END DIAGNOSTIC ===\n";log.flush();
        status("V6-Diagnose in AtomicHeartAmmo.log gespeichert.");
    }
    // Release changes before discarding bootstrap/schema caches.
    void rescan(){
        if(pending.valid()){status("Pattern-Suche laeuft bereits; bitte kurz warten.");return;}
        enabled=false;if(!cleanup()){status("Wiederherstellung noch offen; Scan nicht neu gestartet.");return;}
        objects.reset();r.reset();m.reset();failedPid=0;lastConnect=0;world=player=weapon=0;weaponName="-";
        status("Neuverbindung angefordert; Pattern-Suche startet beim naechsten Zyklus.");
    }
    // Shared 500-ms scheduler: validate context, select once, update modules.
    void tick(){
      if(!work.due(GetTickCount64()))return;
      try{
        if(m&&!m->running()){enabled=false;features.discard();objects.reset();r.reset();m.reset();world=player=weapon=0;weaponName="-";status("Spiel beendet. Warte auf Neustart...");}
        if(!m && !connect())return;
        auto live=objects->player();auto newWorld=live.world,newPlayer=live.player;
        // Old level pointers are discarded, never written after a context change.
        if((world&&world!=newWorld)||(player&&player!=newPlayer)){features.discard();weapon=0;enabled=false;objects.reset();r=std::make_unique<Resolver>(*m,globals.names);objects=std::make_unique<RuntimeObjects>(*m,*r,globals.worldSlot);status("Spieler/Level gewechselt. F10 erneut druecken.");}
        world=newWorld;player=newPlayer;if(!r->reflection.isA(player,"AHBaseCharacter"))throw std::runtime_error("Kein gueltiger Spieler");
        if(!enabled){if(features.pending())cleanup();return;}
        if(objects->dead(player)){enabled=false;cleanup();weapon=0;status("Spieler tot. Funktion ausgeschaltet.");return;}
        // One full selection per batch; feature modules share this result.
        auto current=r->selectedWeapon(player);
        if(current!=weapon){
            weapon=current;weaponName=r->reflection.objectName(weapon);
            log<<"Selected weapon "<<weaponName<<" via "<<r->lastRoute<<"\n";
            lastResolutionError.clear();
        }
        FeatureContext context{*m,*r,*objects,player,weapon,log,r->melee(weapon)};
        status("AN: "+features.update(context)+".");
    }catch(const ResolutionFailure& e){
        // A holster/reload/swap is transient. Restore existing modifications,
        // then wait for an unambiguous selection before applying fresh ones.
        if(m)cleanup();weapon=0;weaponName="-";
        if(enabled&&player&&lastResolutionError!=e.what()){
            lastResolutionError=e.what();diagnose();
        }
        status(std::string(enabled?"AN, warte: ":"Warte: ")+e.what());
    }catch(const std::exception& e){enabled=false;if(m)cleanup();weapon=0;weaponName="-";status(std::string("Aus: ")+e.what());}}
    // WM_CLOSE can report restoration failures before the window is destroyed.
    bool stop(){enabled=false;return cleanup();}
};
std::unique_ptr<Trainer> trainer;HWND stateLabel=nullptr,weaponLabel=nullptr,toggleButton=nullptr;HFONT font=nullptr;bool hotkey=false;
// Copy cached trainer state into the small native menu; no process traversal.
void refresh(){SetWindowTextW(stateLabel,wide(trainer->message).c_str());SetWindowTextW(weaponLabel,wide("Waffe: "+trainer->weaponName).c_str());SetWindowTextW(toggleButton,trainer->enabled?L"Munition AUS [F10]":L"Munition AN [F10]");}
// Route native UI, timer, F10 and close events to the trainer coordinator.
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM w,LPARAM l){
    switch(message){
    case WM_CREATE:{
        font=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        auto control=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int width,int height,int id){auto h=CreateWindowW(cls,text,WS_CHILD|WS_VISIBLE|style,x,y,width,height,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return h;};
        control(L"STATIC",L"Atomic Heart | Externe Munitionshilfe",0,22,20,510,25,0);
        control(L"STATIC",L"F10: ein/aus. Pattern-Suche; Zyklus 500 ms.",0,22,52,510,22,0);
        stateLabel=control(L"STATIC",L"Warte auf Spiel...",0,22,90,510,66,0);weaponLabel=control(L"STATIC",L"Waffe: -",SS_PATHELLIPSIS,22,162,510,30,0);
        toggleButton=control(L"BUTTON",L"Munition AN [F10]",BS_PUSHBUTTON,22,207,160,42,101);
        control(L"BUTTON",L"Diagnose",BS_PUSHBUTTON,196,207,160,42,102);
        control(L"BUTTON",L"Neu scannen",BS_PUSHBUTTON,370,207,157,42,103);
        hotkey=RegisterHotKey(window,1,MOD_NOREPEAT,VK_F10)!=FALSE;
        if(!hotkey)MessageBoxW(window,L"F10 ist bereits belegt. Bitte nutze den Menueknopf.",L"Hotkey",MB_OK|MB_ICONINFORMATION);
        SetTimer(window,1,static_cast<UINT>(WorkIntervalMs),nullptr);return 0;}
    case WM_TIMER:trainer->tick();refresh();return 0;
    case WM_HOTKEY:if(w==1){trainer->toggle();refresh();}return 0;
    case WM_COMMAND:if(LOWORD(w)==101)trainer->toggle();else if(LOWORD(w)==102)trainer->diagnose();else if(LOWORD(w)==103)trainer->rescan();refresh();return 0;
    case WM_CLOSE:KillTimer(window,1);if(!trainer->stop())MessageBoxW(window,L"Nicht alle Originalwerte konnten wiederhergestellt werden. Spiel vor erneutem Versuch neu starten.",L"Wiederherstellung",MB_OK|MB_ICONWARNING);DestroyWindow(window);return 0;
    case WM_DESTROY:if(hotkey)UnregisterHotKey(window,1);if(font)DeleteObject(font);PostQuitMessage(0);return 0;
    }return DefWindowProcW(window,message,w,l);
}
}
// Own the single-instance guard and the native Windows message loop.
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show){
    static_assert(sizeof(void*)==8,"Build x64");HANDLE mutex=CreateMutexW(nullptr,TRUE,L"Local\\AtomicHeartAmmoExternalSingleInstance");
    if(!mutex||GetLastError()==ERROR_ALREADY_EXISTS){if(mutex)CloseHandle(mutex);MessageBoxW(nullptr,L"Der Trainer laeuft bereits.",L"Atomic Heart",MB_OK);return 1;}
    trainer=std::make_unique<Trainer>();WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=instance;cls.lpszClassName=L"AHExternalAmmoWindow";cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&cls);
    auto window=CreateWindowW(cls.lpszClassName,L"Atomic Heart - Infinite Ammo v6 (extern)",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,566,310,nullptr,nullptr,instance,nullptr);
    if(!window){trainer.reset();CloseHandle(mutex);return 1;}ShowWindow(window,show);MSG msg{};int result;
    while((result=GetMessageW(&msg,nullptr,0,0))>0){TranslateMessage(&msg);DispatchMessageW(&msg);}trainer.reset();ReleaseMutex(mutex);CloseHandle(mutex);return result<0?1:0;
}
