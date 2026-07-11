#include "NativePreviewCaptureThunk.h"
#include <iostream>
#include <cstdlib>

namespace
{
    std::uintptr_t managerSeen{}, rendererSeen{};
    std::uint32_t targetSeen{}, calls{};
    void capture(void* manager, std::uint32_t target, void* renderer) noexcept
    { managerSeen = reinterpret_cast<std::uintptr_t>(manager); targetSeen = target; rendererSeen = reinterpret_cast<std::uintptr_t>(renderer); ++calls; }
}
int main()
{
    rock_backpack::native_preview::CaptureThunk thunk(reinterpret_cast<std::uintptr_t>(&capture));
    // Reproduce the native call boundary: R14 holds the renderer; RCX/EDX
    // carry the original arguments. The third argument is deliberately stale.
    struct Caller : Xbyak::CodeGenerator
    {
        explicit Caller(const void* target) : CodeGenerator(128)
        {
            push(r14); sub(rsp, 32);
            mov(r14, 0x123456789ABCDEF0ull);
            mov(rcx, 0x1020304050607080ull); mov(edx, 0x87); xor_(r8, r8);
            mov(rax, reinterpret_cast<std::uintptr_t>(target)); call(rax);
            mov(rax, r14); add(rsp, 32); pop(r14); ret(); ready();
        }
    } caller(thunk.getCode());
    const auto preserved = caller.getCode<std::uintptr_t(*)()>()();
    if (calls != 1 || managerSeen != 0x1020304050607080ull || targetSeen != 0x87 ||
        rendererSeen != 0x123456789ABCDEF0ull || preserved != rendererSeen) {
        std::cerr << "Capture forwarding corrupted the native arguments, R14, or call count\n";
        return EXIT_FAILURE;
    }
    std::cout << "Native capture thunk preserves Win64 arguments and R14; forwards exactly once.\n";
}
