#include "PCH.h"
#include "NativePipboyInventory.h"
#include "BackpackStorage.h"
#include <RE/Bethesda/IMenu.h>
#include <RPS/Runtime/HookChain.h>

namespace rock_backpack::native_pipboy
{
    namespace {
        using Value=RE::Scaleform::GFx::Value;
        using Params=RE::Scaleform::GFx::FunctionHandler::Params;
        using Call=void(*)(RE::IMenu*,const Params&);
        using Advance=void(*)(RE::IMenu*,float,std::uint64_t);
        Call originalCall{};
        Advance originalAdvance{};
        bool warned{},reported{}; // native menu/UI thread only
        constexpr std::array<const char*,7> indexes{"0","1","2","3","4","5","6"};
        constexpr const char* savedLabels="ROCKBackpackOriginalLabels";
        bool member(const Value& object,const char* name,Value& result)
        { return object.IsObject() && object.GetMember(name,&result); }
        std::optional<unsigned> number(const Value& value)
        {
            if(value.IsUInt()) return value.GetUInt();
            if(value.IsInt() && value.GetInt()>=0) return unsigned(value.GetInt());
            if(value.IsNumber() && value.GetNumber()>=0 && value.GetNumber()<=UINT32_MAX) return unsigned(value.GetNumber());
            return {};
        }
        bool inventoryPage(RE::IMenu* menu,Value& data,Value& page)
        {
            Value index;
            return menu && member(menu->menuObj,"DataObj",data) && member(data,"CurrentPage",index) && number(index)==1 &&
                member(menu->menuObj,"CurrentPage",page) && page.IsObject();
        }
        void redrawHeader(RE::IMenu* menu)
        {
            Value header;
            if(member(menu->menuObj,"Header_mc",header)) {
                (void)header.SetMember("_prevTabIndex",Value(std::int32_t(-1)));
                (void)header.Invoke("SetIsDirty");
            }
        }
        bool restrictPage(RE::IMenu* menu)
        {
            Value data,page,tabs;
            if(!inventoryPage(menu,data,page)) return false;
            if(!member(page,"TabNames",tabs) || !tabs.IsArray() || tabs.GetArraySize()!=7) {
                if(!warned) { warned=true; logger::warn("Pip-Boy Misc-only view unavailable: inventory tab schema differs; native navigation preserved"); }
                return false;
            }
            const bool enabled=BackpackStorage::get().nativeUiActive();
            Value selected;
            if(!member(data,"CurrentTab",selected) || !number(selected)) return false;
            const bool changeTab=enabled && number(selected)!=3;
            if(changeTab) {
                Value argument(std::uint32_t(3)),allowed;
                // Preserve the page's native modal/inspection gate. Retry on a
                // later advance after the current interaction has finished.
                if(!page.Invoke("CanSwitchTabs",&allowed,&argument,1) || !allowed.IsBoolean() || !allowed.GetBoolean()) return false;
            }
            Value originals;
            const bool masked=member(tabs,savedLabels,originals) && originals.IsArray() && originals.GetArraySize()==7;
            if(!enabled && !masked) return false;
            std::array<Value,7> labels;
            bool changed{};
            if(!enabled || !masked) {
            for(unsigned i=0;i<labels.size();++i) {
                const auto* name=indexes[i];
                if(!member(masked?originals:tabs,name,labels[i]) || !labels[i].IsString()) return false;
            }
            if(enabled && !masked) {
                if(!menu->uiMovie) return false;
                menu->uiMovie->CreateArray(&originals);
                for(const auto& value:labels) if(!originals.PushBack(value)) return false;
                if(!tabs.SetMember(savedLabels,originals)) return false;
            }
            for(unsigned i=0;i<labels.size();++i) {
                const auto* name=indexes[i];
                Value current; if(!member(tabs,name,current) || !current.IsString()) return false;
                const Value desired=enabled && i!=3?Value(""):labels[i];
                if(std::strcmp(current.GetString(),desired.GetString())) {
                    if(!tabs.SetMember(name,desired)) return false;
                    changed=true;
                }
            }
            if(!enabled) { (void)tabs.SetMember(savedLabels,Value()); if(changed) redrawHeader(menu); return false; }
            }
            if(changeTab) {
                Value argument(std::uint32_t(3));
                Params params{}; params.movie=menu->uiMovie.get(); params.self=&menu->menuObj;
                params.args=&argument; params.argCount=1; params.userData=reinterpret_cast<void*>(4);
                // Use the native onNewTab callback. It updates the native
                // remembered tab AND inventory filter; a GFx-only edit doesn't.
                originalCall(menu,params); changed=true;
            }
            if(changed) redrawHeader(menu);
            if(!reported) { reported=true; logger::info("Pip-Boy Misc-only tab mask applied; other pages retained"); }
            return true;
        }
        void call(RE::IMenu* menu,const Params& args) noexcept
        {
            try {
                if(reinterpret_cast<std::uintptr_t>(args.userData)==4 && args.argCount==1 && args.args &&
                    BackpackStorage::get().nativeUiActive() && restrictPage(menu) && number(args.args[0])!=3) return;
            } catch(...) {
                if(!warned) { warned=true; logger::warn("Pip-Boy inventory filtering failed; original navigation retained"); }
            }
            originalCall(menu,args);
            try { (void)restrictPage(menu); } catch(...) {
                if(!warned) { warned=true; logger::warn("Pip-Boy inventory presentation update failed"); }
            }
        }
        void advance(RE::IMenu* menu,float delta,std::uint64_t time) noexcept
        {
            try { (void)restrictPage(menu); } catch(...) {
                if(!warned) { warned=true; logger::warn("Pip-Boy inventory preparation failed; native menu retained"); }
            }
            originalAdvance(menu,delta,time);
            // An asynchronously loaded inventory page can first appear here.
            try { (void)restrictPage(menu); } catch(...) {
                if(!warned) { warned=true; logger::warn("Pip-Boy inventory page could not be filtered"); }
            }
        }
    }
    bool install()
    {
        if(originalCall) return true;
        const auto base=REL::Module::get().base();
        // Two independent vtable stores: B9F73B/B9F749 constructor and
        // B9FC2E/B9FC38 destructor. Native AdvanceMovie forwards float XMM1
        // and R8 time to 2198D40, which accesses IMenu::uiMovie at +40.
        // Either slot may already hold another plugin's function (ROCK replaces
        // both); Backpack then calls through it.
        const auto callSlot=base+0x2D58420,advanceSlot=base+0x2D58438;
        const auto callResolved=RPS::Runtime::HookChain::resolveSlot(callSlot,base+0xBA0F30);
        const auto advanceResolved=RPS::Runtime::HookChain::resolveSlot(advanceSlot,base+0xBA09D0);
        if(!callResolved || !advanceResolved) {
            logger::warn("Pip-Boy slots hold unrecognised code; Misc-only presentation was not installed"); return false;
        }
        for(const auto* resolved:{&callResolved,&advanceResolved}) if(resolved->foreign())
            logger::info("Pip-Boy menu slot already replaced by {}; Backpack calls through it",resolved->owner);
        originalCall=reinterpret_cast<Call>(callResolved.callee);
        originalAdvance=reinterpret_cast<Advance>(advanceResolved.callee);
        REL::safe_write(callSlot,reinterpret_cast<std::uintptr_t>(&call));
        REL::safe_write(advanceSlot,reinterpret_cast<std::uintptr_t>(&advance));
        return true;
    }
}
