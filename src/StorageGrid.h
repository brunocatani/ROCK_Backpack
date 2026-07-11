#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>
#include <unordered_map>

namespace rock_backpack::storage
{
    using ItemId = std::uint64_t;
    enum class Kind : std::uint8_t { LongGun, Pistol, Melee, Armor, Aid, Ammo, Junk, Misc, Sniper, Heavy, Throwable, Knife };
    // Values are saved in layouts. RetiredLegHolster (the removed right-hip
    // holster) is never placeable and never reused; savedLocation moves an
    // old save's hip weapon to Overflow, which reconciliation then places
    // in the bag interior.
    enum class Location : std::uint8_t { Interior, LeftRifle, RightRifle, Pistol, RetiredLegHolster, Overflow, Equipped };
    constexpr Location savedLocation(std::uint8_t value)
    { return value==std::uint8_t(Location::RetiredLegHolster)?Location::Overflow:Location(value); }
    constexpr bool validKind(Kind kind) { return unsigned(kind) <= unsigned(Kind::Knife); }
    constexpr bool isMount(Location location) { return location >= Location::LeftRifle && location <= Location::Pistol; }
    constexpr bool isWeapon(Kind kind)
    { return kind == Kind::LongGun || kind == Kind::Pistol || kind == Kind::Melee || kind == Kind::Knife || kind == Kind::Sniper || kind == Kind::Heavy; }
    constexpr Kind weaponKind(bool melee, bool knife, bool pistol, bool sniper, bool heavy, bool thrown)
    { return thrown ? Kind::Throwable : melee ? (knife ? Kind::Knife : Kind::Melee) : heavy ? Kind::Heavy : pistol ? Kind::Pistol : sniper ? Kind::Sniper : Kind::LongGun; }
    struct Footprint { unsigned width{1}, height{1}; };
    constexpr Footprint footprint(Kind kind, bool rotated = false)
    {
        if (kind == Kind::Misc) return {0,0};
        if (kind == Kind::Heavy) return rotated ? Footprint{2,3} : Footprint{3,2};
        const unsigned cells = kind == Kind::Sniper ? 4 : kind == Kind::LongGun || kind == Kind::Melee ? 3 :
            kind == Kind::Pistol || kind == Kind::Knife || kind == Kind::Armor ? 2 : 1;
        return rotated ? Footprint{1, cells} : Footprint{cells, 1};
    }
    constexpr bool accepts(Location slot, Kind kind)
    {
        if (!validKind(kind) ||
            static_cast<unsigned>(slot) > static_cast<unsigned>(Location::Equipped)) return false;
        if(slot==Location::Equipped) return isWeapon(kind);
        if(slot==Location::Overflow) return true;
        // Each backpack mount holds one exact, non-thrown weapon.
        if (slot == Location::RightRifle || slot==Location::LeftRifle || slot == Location::Pistol) return isWeapon(kind);
        return slot == Location::Interior;
    }
    struct Placement
    {
        Location location{Location::Interior};
        unsigned x{}, y{};
        bool rotated{};
        bool operator==(const Placement&) const = default;
    };
    struct Entry
    {
        ItemId id{};
        Kind kind{Kind::Misc};
        std::uint32_t count{1};
        // A nonzero key is assigned only after native stack compatibility has
        // been established. It is not a base-form or appearance-hash shortcut.
        std::uint64_t stackKey{};
        Placement placement;
        bool busy{};
    };
    enum class Result { Ok, Missing, Stale, Busy, Occupied, Outside, Incompatible, Full, InvalidQuantity };
    inline constexpr unsigned kInitialColumns = 10, kInitialRows = 8;

    class Grid
    {
    public:
        explicit Grid(unsigned columns = kInitialColumns, unsigned rows = kInitialRows) : _columns(columns), _rows(rows) {}
        unsigned columns() const { return _columns; }
        unsigned rows() const { return _rows; }
        std::span<const Entry> entries() const { return _entries; }
        // Only spatial entries participate in frame-time hit tests. The unlimited
        // miscellaneous list must not increase the cost of scanning grid cells.
        std::span<const std::size_t> spatialIndices() const { return _spatial; }
        void reserveForInsert() {
            if(_entries.size()==_entries.capacity()) {
                const auto capacity=(std::max)(std::size_t(16),_entries.capacity()*2);
                _entries.reserve(capacity); _byId.reserve(capacity);
            }
            if(_spatial.size()==_spatial.capacity()) _spatial.reserve((std::max)(std::size_t(16),_spatial.capacity()*2));
        }
        const Entry* find(ItemId id) const
        {
            const auto it=_byId.find(id);
            return it==_byId.end()?nullptr:&_entries[it->second];
        }
        const Entry* at(Location location, unsigned x = 0, unsigned y = 0) const
        {
            for (const auto index : _spatial) {
                const auto& entry = _entries[index];
                const auto& p = entry.placement;
                if (p.location != location) continue;
                const auto size = footprint(entry.kind, p.rotated);
                if (location != Location::Interior || (x >= p.x && x < p.x + size.width && y >= p.y && y < p.y + size.height)) return &entry;
            }
            return nullptr;
        }
        // A pending native unequip reserves its future cell until the engine
        // confirms the transition. This is not an inventory item or saved data.
        const std::optional<Entry>& reservation() const { return _reservation; }
        bool reserveInterior(Kind kind,Placement placement) {
            if(_reservation || placement.location!=Location::Interior || fits(kind,placement)!=Result::Ok) return false;
            _reservation=Entry{0,kind,1,0,placement}; return true;
        }
        void clearReservation() { _reservation.reset(); }
        Result fits(Kind kind, const Placement& placement, ItemId ignore = 0) const
        {
            if (!accepts(placement.location, kind)) return Result::Incompatible;
            if (kind == Kind::Misc || placement.location==Location::Overflow || placement.location==Location::Equipped)
                return !placement.x && !placement.y && !placement.rotated ? Result::Ok : Result::Outside;
            if (placement.location != Location::Interior) {
                if (placement.x || placement.y || placement.rotated) return Result::Outside;
                const auto* other = at(placement.location);
                return other && other->id != ignore ? Result::Occupied : Result::Ok;
            }
            const auto size = footprint(kind, placement.rotated);
            if (!_columns || !_rows || _columns > 32 || _rows > 32 || size.width > _columns || size.height > _rows ||
                placement.x > _columns - size.width || placement.y > _rows - size.height) return Result::Outside;
            if(_reservation) {
                const auto& p=_reservation->placement;
                const auto reserved=footprint(_reservation->kind,p.rotated);
                if(placement.x<p.x+reserved.width && p.x<placement.x+size.width &&
                    placement.y<p.y+reserved.height && p.y<placement.y+size.height) return Result::Occupied;
            }
            for (unsigned y = placement.y; y < placement.y + size.height; ++y)
                for (unsigned x = placement.x; x < placement.x + size.width; ++x)
                    if (const auto* other = at(Location::Interior, x, y); other && other->id != ignore) return Result::Occupied;
            return Result::Ok;
        }
        std::optional<Placement> firstFit(Kind kind, ItemId ignore = 0) const
        {
            if (kind == Kind::Misc) return Placement{};
            if (_columns > 32 || _rows > 32) return {};
            for (const bool rotated : {false, true})
                for (unsigned y = 0; y < _rows; ++y)
                    for (unsigned x = 0; x < _columns; ++x) {
                        Placement p{Location::Interior, x, y, rotated};
                        if (fits(kind, p, ignore) == Result::Ok) return p;
                    }
            return {};
        }
        Result insert(const Entry& entry)
        {
            if (!entry.id || find(entry.id) || !entry.count) return Result::Stale;
            if ((isMount(entry.placement.location) || entry.placement.location==Location::Equipped) && entry.count != 1) return Result::InvalidQuantity;
            const auto result = fits(entry.kind, entry.placement);
            if (result == Result::Ok) {
                reserveForInsert();
                _byId.emplace(entry.id,_entries.size());
                _entries.push_back(entry); indexSpatial(_entries.size()-1);
            }
            return result;
        }
        Result move(ItemId id, Placement placement)
        {
            auto* entry = edit(id);
            if (!entry) return Result::Missing;
            if (entry->busy) return Result::Busy;
            if ((isMount(placement.location) || placement.location==Location::Equipped) && entry->count != 1) return Result::InvalidQuantity;
            const auto result = fits(entry->kind, placement, id);
            if (result == Result::Ok) { _spatial.reserve(_spatial.size()+1); entry->placement = placement; indexSpatial(std::size_t(entry-_entries.data())); }
            return result;
        }
        bool setBusy(ItemId id, bool busy)
        { auto* entry = edit(id); if (!entry) return false; entry->busy = busy; return true; }
        bool setStackKey(ItemId id, std::uint64_t key)
        { auto* entry = edit(id); if (!entry) return false; entry->stackKey = key; return true; }
        bool erase(ItemId id)
        {
            const auto* entry=find(id); if(!entry) return false;
            const auto index=std::size_t(entry-_entries.data());
            _byId.erase(id);
            _entries.erase(_entries.begin()+index);
            for(auto i=index;i<_entries.size();++i) _byId.find(_entries[i].id)->second=i;
            std::erase(_spatial,index); for(auto& current:_spatial) if(current>index) --current;
            return true;
        }
        Result split(ItemId id, ItemId newId, std::uint32_t quantity, Placement destination)
        {
            const auto* entry = find(id);
            if (!entry || !newId || find(newId)) return Result::Missing;
            if (entry->busy) return Result::Busy;
            if (!quantity || quantity >= entry->count || !entry->stackKey || destination.location != Location::Interior) return Result::InvalidQuantity;
            Entry next = *entry; next.id = newId; next.count = quantity; next.placement = destination;
            const auto result = insert(next);
            if (result == Result::Ok) edit(id)->count -= quantity;
            return result;
        }
        Result merge(ItemId source, ItemId destination)
        {
            const auto* from = find(source); auto* to = edit(destination);
            if (!from || !to || source == destination) return Result::Missing;
            if (from->busy || to->busy) return Result::Busy;
            if (!from->stackKey || from->stackKey != to->stackKey || from->kind != to->kind ||
                from->placement.location != Location::Interior || to->placement.location != Location::Interior) return Result::Incompatible;
            if (from->count > (std::numeric_limits<std::uint32_t>::max)() - to->count) return Result::InvalidQuantity;
            to->count += from->count; erase(source); return Result::Ok;
        }
        bool setCount(ItemId id, std::uint32_t count)
        {
            auto* entry = edit(id);
            if (!entry || !count || ((isMount(entry->placement.location) || entry->placement.location==Location::Equipped) && count != 1)) return false;
            entry->count = count; return true;
        }
    private:
        void indexSpatial(std::size_t index)
        {
            std::erase(_spatial,index);
            const auto& entry=_entries[index];
            if (entry.kind != Kind::Misc && entry.placement.location!=Location::Overflow && entry.placement.location!=Location::Equipped) _spatial.push_back(index);
        }
        Entry* edit(ItemId id) { return const_cast<Entry*>(find(id)); }
        unsigned _columns, _rows;
        std::optional<Entry> _reservation;
        std::vector<Entry> _entries;
        std::unordered_map<ItemId,std::size_t> _byId;
        std::vector<std::size_t> _spatial;
    };
}
