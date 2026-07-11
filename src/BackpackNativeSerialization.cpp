#include "PCH.h"
#include "BackpackStorage.h"

namespace rock_backpack
{
    namespace {
        constexpr std::uint32_t layoutRecord=0x4C494252,deliveryRecord=0x44494252;
        struct Header { std::uint64_t nextId{},nextAppearance{},count{}; };
        struct Item {
            std::uint64_t id{},appearance{};
            std::uint32_t form{},count{},uniqueBase{},x{},y{};
            float weight{};
            std::uint16_t uniqueIndex{};
            std::uint8_t kind{},location{},rotated{},reserved[3]{};
            std::uint32_t nameBytes{};
        };
        struct SavedDelivery { std::uint64_t id{}; std::uint32_t reference{},form{}; };
        static_assert(sizeof(Header)==24 && sizeof(Item)==56 && sizeof(SavedDelivery)==16);
    }
    void BackpackStorage::saveNative(const F4SE::SerializationInterface* serial) const
    {
        if(_native.records.size()>UINT32_MAX) { logger::error("Native inventory layout too large to save"); return; }
        const Header header{_native.nextId,_native.nextAppearance,_native.records.size()};
        if(!serial->OpenRecord(layoutRecord,1) || !serial->WriteRecordData(&header,sizeof(header))) {
            logger::error("Native backpack layout save failed"); return;
        }
        for(const auto& record:_native.records) {
            const auto* e=_native.grid.find(record.id); if(!e) { logger::error("Native backpack layout mismatch"); return; }
            const auto& s=record.observed;
            if(s.name.size()>4096) { logger::error("Native item name exceeds layout record bound"); return; }
            const Item item{record.id,record.appearance,s.form,e->count,s.uniqueBase,e->placement.x,e->placement.y,s.weight,
                s.uniqueIndex,std::uint8_t(e->kind),std::uint8_t(e->placement.location),std::uint8_t(e->placement.rotated),{},std::uint32_t(s.name.size())};
            if(!serial->WriteRecordData(&item,sizeof(item)) ||
                (item.nameBytes && !serial->WriteRecordData(s.name.data(),item.nameBytes))) {
                logger::error("Native backpack layout item save failed"); return;
            }
        }
        if(_pending.id && _pending.bag==kBags) {
            const SavedDelivery delivery{_pending.id,_pending.reference,_nativePendingForm};
            if(!serial->WriteRecord(deliveryRecord,1,&delivery,sizeof(delivery))) logger::error("Native backpack delivery save failed");
        }
    }
    bool BackpackStorage::loadNative(const F4SE::SerializationInterface* serial,std::uint32_t type,std::uint32_t version,std::uint32_t bytes)
    {
        if(type!=layoutRecord && type!=deliveryRecord) return false;
        const auto remap=[&](std::uint32_t id){return id?serial->ResolveFormID(id).value_or(0):0;};
        if(version!=1) { logger::error("Unsupported native backpack layout version {}; native items preserved",version); return true; }
        if(type==deliveryRecord) {
            SavedDelivery delivery;
            if(bytes==sizeof(delivery) && serial->ReadRecordData(&delivery,sizeof(delivery))==sizeof(delivery) && delivery.id) {
                _pending={kBags,delivery.id,remap(delivery.reference)};
                _nativePendingForm=remap(delivery.form); _loadedPending=true;
            }
            return true;
        }
        Header header;
        if(bytes<sizeof(header) || serial->ReadRecordData(&header,sizeof(header))!=sizeof(header) ||
            header.count>(bytes-sizeof(header))/sizeof(Item) || !header.nextId || !header.nextAppearance) return true;
        inventory_mirror::Ledger next; next.nextId=header.nextId; next.nextAppearance=header.nextAppearance;
        std::uint64_t consumed=sizeof(header);
        bool valid=true;
        for(std::uint64_t i=0;i<header.count;++i) {
            Item item;
            if(bytes-consumed<sizeof(item) || serial->ReadRecordData(&item,sizeof(item))!=sizeof(item)) { valid=false; break; }
            consumed+=sizeof(item);
            if(item.nameBytes>4096 || item.nameBytes>bytes-consumed || !item.id || item.id>=header.nextId ||
                !item.appearance || item.appearance>=header.nextAppearance || item.rotated>1 || item.reserved[0] || item.reserved[1] || item.reserved[2] ||
                !std::isfinite(item.weight) || item.weight<0) { valid=false; break; }
            std::string name(item.nameBytes,'\0');
            if(item.nameBytes && serial->ReadRecordData(name.data(),item.nameBytes)!=item.nameBytes) { valid=false; break; }
            consumed+=item.nameBytes;
            const auto form=remap(item.form); if(!form) continue;
            const auto uniqueBase=remap(item.uniqueBase);
            if(item.uniqueBase && !uniqueBase) continue; // never guess a lost pinned identity by name
            inventory_mirror::Record record{item.id,item.appearance};
            record.observed={form,0,item.count,uniqueBase,item.uniqueIndex,storage::Kind(item.kind),std::move(name),item.weight};
            // This is layout only. The first native snapshot supplies counts
            // and determines whether the exact item still exists.
            if(next.grid.insert({item.id,storage::Kind(item.kind),item.count,item.id,
                {storage::savedLocation(item.location),item.x,item.y,item.rotated!=0}})!=storage::Result::Ok) { valid=false; break; }
            next.records.push_back(std::move(record));
        }
        if(valid && consumed==bytes) _native=std::move(next);
        else logger::error("Invalid native backpack layout ignored; actual inventory remains native-owned");
        return true;
    }
}
