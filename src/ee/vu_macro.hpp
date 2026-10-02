#pragma once

#include "gt4recomp/ee_decode.hpp"
#include "gt4recomp/ee_state.hpp"

namespace gt4recomp::ee {

// Executes one VU0 macro instruction; returns false when the operation is not
// a macro form. Defined in vu_macro.cpp next to its semantic helpers, which
// mirror the reference emulator's VU macro table.
bool execute_vu_macro(const DecodedInstruction& instruction, GuestState& state);

} // namespace gt4recomp::ee
