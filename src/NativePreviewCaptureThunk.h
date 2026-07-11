#pragma once
#include <xbyak/xbyak.h>
#include <cstdint>

namespace rock_backpack::native_preview
{
    // At FO4VR B03531, R14 is the renderer (assigned at B02E56 and
    // restored at B03568). Forward it as the third Win64 argument while
    // preserving the existing manager/target arguments and return address.
    // This avoids interposing on Inventory3DFix's offscreen draw call.
    class CaptureThunk final : public Xbyak::CodeGenerator
    {
    public:
        explicit CaptureThunk(std::uintptr_t destination) : CodeGenerator(64)
        {
            Xbyak::Label target;
            mov(r8, r14);
            jmp(ptr[rip + target]);
            L(target);
            dq(destination);
            ready();
        }
    };
}
