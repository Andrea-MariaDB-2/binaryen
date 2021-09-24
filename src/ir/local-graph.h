/*
 * Copyright 2017 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_ir_local_graph_h
#define wasm_ir_local_graph_h

#include "support/small_set.h"
#include "wasm.h"

namespace wasm {

//
// Finds the connections between local.gets and local.sets, creating
// a graph of those ties. This is useful for "ssa-style" optimization,
// in which you want to know exactly which sets are relevant for a
// a get, so it is as if each get has just one set, logically speaking
// (see the SSA pass for actually creating new local indexes based
// on this).
//
struct LocalGraph {
  // main API

  // the constructor computes getSetses, the sets affecting each get
  LocalGraph(Function* func);

  // The local.sets relevant for an index or a get. The most common case is to
  // have a single set; after that, to be a phi of 2 items, so we use a small
  // set of size 2 to avoid allocations there.
  // 0: 4.38    47,03
  // 1: 4.11    45,99
  // 2: 3.70    45,88  winner
  // 3: 3.65    45,84
  // 4: 3.67    46,11  DOWNAIDE
  // Unordered is worse.
  typedef SmallSet<LocalSet*, 2> Sets;

  // unordered: 3.7 => 3.55   45,55
  typedef std::unordered_map<LocalGet*, Sets> GetSetses;

  // maybe also worth unordered, but not clear.
  typedef std::unordered_map<Expression*, Expression**> Locations;

  // externally useful information
  GetSetses getSetses; // the sets affecting each get. a nullptr set means the
                       // initial value (0 for a var, the received value for a
                       // param)
  Locations locations; // where each get and set is (for easy replacing)

  // Checks if two gets are equivalent, that is, definitely have the same
  // value.
  bool equivalent(LocalGet* a, LocalGet* b);

  // Optional: compute the influence graphs between sets and gets
  // (useful for algorithms that propagate changes).

  void computeSetInfluences();
  void computeGetInfluences();

  void computeInfluences() {
    computeSetInfluences();
    computeGetInfluences();
  }

  // for each get, the sets whose values are influenced by that get
  // 0: 8.90    70,65
  // 1: 8.8?    70,72
  // 2:         70,31
  // 4: same
  using GetInfluences = SmallUnorderedSet<LocalSet*, 4>;
  std::unordered_map<LocalGet*, GetInfluences> getInfluences;
  // for each set, the gets whose values are influenced by that set
  // 1: 
  using SetInfluences = SmallUnorderedSet<LocalGet*, 3>;
  std::unordered_map<LocalSet*, SetInfluences> setInfluences;

  // Optional: Compute the local indexes that are SSA, in the sense of
  //  * a single set for all the gets for that local index
  //  * the set dominates all the gets (logically implied by the former
  //  property)
  //  * no other set (aside from the zero-init)
  // The third property is not exactly standard SSA, but is useful since we are
  // not in SSA form in our IR. To see why it matters, consider these:
  //
  // x = 0 // zero init
  // [..]
  // x = 10
  // y = x + 20
  // x = 30 // !!!
  // f(y)
  //
  // The !!! line violates that property - it is another set for x, and it may
  // interfere say with replacing f(y) with f(x + 20). Instead, if we know the
  // only other possible set for x is the zero init, then things like the !!!
  // line cannot exist, and it is valid to replace f(y) with f(x + 20). (This
  // could be simpler, but in wasm the zero init always exists.)

  void computeSSAIndexes();

  bool isSSA(Index x);

private:
  Function* func;
  std::set<Index> SSAIndexes;
};

} // namespace wasm

#endif // wasm_ir_local_graph_h
