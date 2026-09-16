#pragma once

// A disassembler for the debugger's code view.
//
// Covers the instruction classes a GBA game actually contains. Anything it does
// not recognise is shown as raw hex with a leading "?", which is honest and
// obvious rather than a plausible-looking wrong mnemonic.

#include <cstdint>
#include <string>

namespace gba::host {

std::string disasmArm(uint32_t op, uint32_t pc);
std::string disasmThumb(uint16_t op, uint32_t pc);

}  // namespace gba::host
