/*
 * Copyright 2021 WebAssembly Community Group participants
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

//
// Refines the types of locals where possible. That is, if a local is assigned
// types that are more specific than the local's declared type, refine the
// declared type. This can then potentially unlock optimizations later when the
// local is used, as we have more type info. (However, it may also increase code
// size in theory, if we end up declaring more types - TODO investigate.)
//

#include <ir/find_all.h>
#include <ir/linear-execution.h>
#include <ir/local-graph.h>
#include <ir/utils.h>
#include <pass.h>
#include <wasm.h>

namespace wasm {

struct LocalSubtyping : public WalkerPass<PostWalker<LocalSubtyping>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new LocalSubtyping(); }

  void doWalkFunction(Function* func) {
    if (!getModule()->features.hasGC()) {
      return;
    }

    auto numLocals = func->getNumLocals();

    // Compute the local graph. We need to get the list of gets and sets for
    // each local, so that we can do the analysis. For non-nullable locals, we
    // also need to know when the default value of a local is used: if so then
    // we cannot change that type, as if we change the local type to
    // non-nullable then we'd be accessing the default, which is not allowed.
    //
    // TODO: Optimize this, as LocalGraph computes more than we need, and on
    //       more locals than we need.
    LocalGraph localGraph(func);

    // For each local index, compute all the the sets and gets.
    std::vector<std::vector<LocalSet*>> setsForLocal(numLocals);
    std::vector<std::vector<LocalGet*>> getsForLocal(numLocals);

    for (auto& kv : localGraph.locations) {
      auto* curr = kv.first;
      if (auto* set = curr->dynCast<LocalSet>()) {
        setsForLocal[set->index].push_back(set);
      } else {
        auto* get = curr->cast<LocalGet>();
        getsForLocal[get->index].push_back(get);
      }
    }

    // Find which vars use the default value, if we allow non-nullable locals.
    //
    // If that feature is not enabled, then we can safely assume that the
    // default is never used - the default would be a null value, and the type
    // of the null does not really matter as all nulls compare equally, so we do
    // not need to worry.
    std::unordered_set<Index> usesDefault;

    if (getModule()->features.hasGCNNLocals()) {
      for (auto& kv : localGraph.getSetses) {
        auto* get = kv.first;
        auto& sets = kv.second;
        auto index = get->index;
        if (func->isVar(index) &&
            std::any_of(sets.begin(), sets.end(), [&](LocalSet* set) {
              return set == nullptr;
            })) {
          usesDefault.insert(index);
        }
      }
    }

    auto varBase = func->getVarIndexBase();

    // Keep iterating while we find things to change. There can be chains like
    // X -> Y -> Z where one change enables more. Note that we are O(N^2) on
    // that atm, but it is a rare pattern as general optimizations
    // (SimplifyLocals and CoalesceLocals) break up such things; also, even if
    // we tracked changes more carefully we'd have the case of nested tees
    // where we could still be O(N^2), so we'd need something more complex here
    // involving topological sorting. Leave that for if the need arises.

    // TODO: handle cycles of X -> Y -> X etc.

    bool more;

    do {
      more = false;

      // First, refinalize which will recompute least upper bounds on ifs and
      // blocks, etc., potentially finding a more specific type. Note that
      // that utility does not tell us if it changed anything, so we depend on
      // the next step for knowing if there is more work to do.
      ReFinalize().walkFunctionInModule(func, getModule());

      // Second, find vars whose actual applied values allow a more specific
      // type.

      for (Index i = varBase; i < numLocals; i++) {
        // Find all the types assigned to the var, and compute the optimal LUB.
        std::unordered_set<Type> types;
        for (auto* set : setsForLocal[i]) {
          types.insert(set->value->type);
        }
        if (types.empty()) {
          // Nothing is assigned to this local (other opts will remove it).
          continue;
        }

        auto oldType = func->getLocalType(i);
        auto newType = Type::getLeastUpperBound(types);
        assert(newType != Type::none); // in valid wasm there must be a LUB

        // Remove non-nullability if we disallow that in locals.
        if (newType.isNonNullable()) {
          // As mentioned earlier, even if we allow non-nullability, there may
          // be a problem if the default value - a null - is used. In that case,
          // remove non-nullability as well.
          if (!getModule()->features.hasGCNNLocals() || usesDefault.count(i)) {
            newType = Type(newType.getHeapType(), Nullable);
          }
        } else if (!newType.isDefaultable()) {
          // Aside from the case we just handled of allowed non-nullability, we
          // cannot put anything else in a local that does not have a default
          // value.
          continue;
        }

        if (newType != oldType) {
          // We found a more specific type!
          assert(Type::isSubType(newType, oldType));
          func->vars[i - varBase] = newType;
          more = true;

          // Update gets and tees.
          for (auto* get : getsForLocal[i]) {
            get->type = newType;
          }

          // NB: These tee updates will not be needed if the type of tees
          //     becomes that of their value, in the spec.
          for (auto* set : setsForLocal[i]) {
            if (set->isTee()) {
              set->type = newType;
              set->finalize();
            }
          }
        }
      }
    } while (more);
  }
};

Pass* createLocalSubtypingPass() { return new LocalSubtyping(); }

} // namespace wasm