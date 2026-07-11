#pragma once
#include "NativePreviewAddresses.h"
#include "PreviewRecovery.h"
#include "RE/Bethesda/BSGeometry.h"
#include "RE/NetImmerse/NiNode.h"
#include <array>

namespace rock_backpack::native_preview
{
    // One owned offscreen pass. All pointers are borrowed from its retained
    // model/accumulator and are retired before native scene destruction.
    class LightingScope
    {
    public:
        bool begin(void* renderer)
        {
            if (_accumulator) return false;
            auto* model = member<RE::NiAVObject*>(renderer, 0x98);
            auto* accumulator = member<void*>(renderer, 0x1B0);
            if (!model || !accumulator) return false;
            std::array<RE::NiAVObject*, preview_recovery::kMaximumNodes> stack{};
            std::size_t size = 1, visited = 0;
            stack[0] = model;
            _count = _geometries = 0;
            while (size) {
                auto* object = stack[--size];
                if (!object) continue;
                if (++visited > preview_recovery::kMaximumNodes) return false;
                if (auto* node = object->IsNode()) {
                    const auto& children = node->GetRuntimeData().children;
                    if (children.size() > stack.size() - size) return false;
                    for (const auto& child : children) if (child) stack[size++] = child.get();
                } else if (auto* geometry = object->IsGeometry()) {
                    ++_geometries;
                    for (const auto& property : geometry->GetRuntimeData().properties) {
                        auto* pointer = property.get();
                        // Native BSLightingShaderProperty constructor installs
                        // this VR vtable. The flat header's subclass offsets
                        // (including baseTechniqueID) are not used.
                        if (!pointer || member<std::uintptr_t>(pointer, 0) != REL::Module::get().base() + 0x30A5C18 ||
                            !(member<std::uint64_t>(pointer, 0x30) & (1ull << 7))) continue;
                        if (_count == _environment.size()) return false;
                        _environment[_count++] = pointer;
                    }
                }
            }
            // Collect once, then deduplicate in O(n log n); large weapon
            // assemblies must not turn each preview pass into a quadratic scan.
            _count=preview_recovery::distinctProperties(_environment,_count);
            _accumulator = accumulator;
            _registration = member<bool>(accumulator, 0xF669);
            _mode = member<std::uint32_t>(accumulator, 0xF688);
            // The menu forward path has no world reflection context. Apply
            // this only to our preview's pass, through the native flag setter
            // so its render-pass cache is invalidated; restore the bit below.
            for (std::size_t i = 0; i < _count; ++i) setEnvironment(_environment[i], false);
            member<bool>(accumulator, 0xF669) = true;
            member<std::uint32_t>(accumulator, 0xF688) = 0x19;
            return true;
        }
        void restore()
        {
            if (!_accumulator) { _environment.fill(nullptr); _count = 0; return; }
            for (std::size_t i = 0; i < _count; ++i) setEnvironment(_environment[i], true);
            member<bool>(_accumulator, 0xF669) = _registration;
            member<std::uint32_t>(_accumulator, 0xF688) = _mode;
            _accumulator = nullptr; _environment.fill(nullptr); _count = 0;
        }
        bool owns(void* accumulator) const { return _accumulator && _accumulator == accumulator; }
        std::size_t environmentCount() const { return _count; }
        std::size_t geometryCount() const { return _geometries; }
    private:
        template<class T> static T& member(void* object, std::size_t offset)
        { return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset); }
        static void setEnvironment(void* property, bool enabled)
        {
            const auto fn = reinterpret_cast<void(*)(void*, std::uint32_t, bool)>(REL::Module::get().base() + shaderFlag.rva);
            fn(property, 7, enabled);
        }
        void* _accumulator{};
        std::array<void*, preview_recovery::kMaximumProperties> _environment{};
        std::size_t _count{}, _geometries{};
        bool _registration{};
        std::uint32_t _mode{};
    };
}
