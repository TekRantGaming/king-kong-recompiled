// Random Xenos programs for the differential test (translated code on a GPU against the
// reference interpreter). Unlike the corpus's fuzz group, which aims at the translator's
// robustness, these aim at comparable results: every instruction class with random operands,
// swizzles and modifiers, predication, address-register and loop-relative addressing, nested
// loops, structured and crossing jumps, calls, vertex fetches of every format, and texture
// fetches of every dimension, ending in exports that carry the results out.
#pragma once

#include <cstdint>

#include "corpus.h"

// One program (a container, or bare microcode when `raw`), deterministic in `seed`.
CorpusShader randomProgram(uint32_t seed, bool vertex, bool raw);
