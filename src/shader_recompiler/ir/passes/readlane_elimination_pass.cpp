// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <unordered_map>
#include <queue>
#include "common/logging/log.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

// golf-fix: SGPR spills to VGPR lanes (v_writelane / v_readlane) whose VGPR also carries an
// unrelated value on some control flow path could not be eliminated. The fallback emitted
// WriteLane as 0, so every reload of a spilled SGPR read 0 (black eyes/mouth in
// Everybody's Golf). Values reaching the readlane from a non-WriteLane producer are treated
// as undefined, matching what the original compiler assumed for spill slots.
static thread_local IR::Inst* g_golf_undef{};
static thread_local IR::Block* g_golf_entry{};

static IR::Value GolfUndef() {
    if (!g_golf_undef) {
        g_golf_undef = &*g_golf_entry->PrependNewInst(g_golf_entry->begin(), IR::Opcode::UndefU32);
    }
    return IR::Value{g_golf_undef};
}

static IR::Inst* SearchChain(IR::Inst* inst, u32 lane) {
    while (inst->GetOpcode() == IR::Opcode::WriteLane) {
        if (inst->Arg(2).U32() == lane) {
            // We found a possible write lane source, return it.
            return inst;
        }
        inst = inst->Arg(0).Inst();
    }
    return inst;
}

static bool IsPossibleToEliminate(IR::Inst* inst, u32 lane) {
    // Breadth-first search visiting the right most arguments first
    boost::container::small_vector<IR::Inst*, 16> visited;
    std::queue<IR::Inst*> queue;
    queue.push(inst);
    bool found_write = false;
    bool found_foreign = false;
    IR::Opcode foreign_op{};

    while (!queue.empty()) {
        // Pop one instruction from the queue
        IR::Inst* inst{queue.front()};
        queue.pop();

        // If it's a WriteLane search for possible candidates
        if (inst = SearchChain(inst, lane); inst->GetOpcode() == IR::Opcode::WriteLane) {
            // We found a possible write lane source, stop looking here.
            found_write = true;
            continue;
        }
        // A select between two VGPR values (EXEC-masked write): follow both sides.
        if (inst->GetOpcode() == IR::Opcode::SelectU32) {
            for (size_t arg = 1; arg <= 2; ++arg) {
                const auto arg_value{inst->Arg(arg)};
                if (arg_value.IsImmediate()) {
                    continue;
                }
                IR::Inst* arg_inst{arg_value.Inst()};
                if (std::ranges::find(visited, arg_inst) == visited.end()) {
                    visited.push_back(arg_inst);
                    queue.push(arg_inst);
                }
            }
            continue;
        }
        // Other producers: the spill slot holds an unrelated value on that path.
        if (inst->GetOpcode() != IR::Opcode::Phi && inst->GetOpcode() != IR::Opcode::UndefU32) {
            if (!found_foreign) {
                foreign_op = inst->GetOpcode();
            }
            found_foreign = true;
            continue;
        }
        for (size_t arg = inst->NumArgs(); arg--;) {
            auto arg_value{inst->Arg(arg)};
            if (arg_value.IsImmediate()) {
                continue;
            }
            // Queue instruction if it hasn't been visited
            IR::Inst* arg_inst{arg_value.Inst()};
            if (std::ranges::find(visited, arg_inst) == visited.end()) {
                visited.push_back(arg_inst);
                queue.push(arg_inst);
            }
        }
    }
    if (found_foreign) {
        static std::atomic<int> golf_logged{0};
        if (golf_logged < 32) {
            ++golf_logged;
            LOG_ERROR(Render_Recompiler,
                      "golf-fix: readlane lane {} chain has foreign producer {} (writelane found: {})",
                      lane, IR::NameOf(foreign_op), found_write);
        }
    }
    return found_write;
}

using PhiMap = std::unordered_map<IR::Inst*, IR::Value>;

static IR::Value GetRealValue(PhiMap& phi_map, IR::Inst* inst, u32 lane) {
    // If this is a WriteLane op search the chain for a possible candidate.
    if (inst = SearchChain(inst, lane); inst->GetOpcode() == IR::Opcode::WriteLane) {
        return inst->Arg(1);
    }

    // If this is a phi, duplicate it and populate its arguments with real values.
    if (inst->GetOpcode() == IR::Opcode::Phi) {
        // We are in a phi cycle, use the already duplicated phi.
        const auto [it, is_new_phi] = phi_map.try_emplace(inst);
        if (!is_new_phi) {
            return it->second;
        }

        // Create new phi and insert it right before the old one.
        const auto insert_point = IR::Block::InstructionList::s_iterator_to(*inst);
        IR::Block* block = inst->GetParent();
        IR::Inst* const new_phi{&*block->PrependNewInst(insert_point, IR::Opcode::Phi)};
        new_phi->SetFlags(IR::Type::U32);
        it->second = IR::Value{new_phi};

        // Gather all arguments.
        boost::container::small_vector<IR::Value, 5> phi_args;
        for (size_t arg_index = 0; arg_index < inst->NumArgs(); arg_index++) {
            IR::Value arg = inst->Arg(arg_index);
            if (!arg.IsImmediate()) {
                arg = GetRealValue(phi_map, arg.Inst(), lane);
            }
            phi_args.push_back(arg);
        }
        for (size_t arg_index = 0; arg_index < inst->NumArgs(); arg_index++) {
            new_phi->AddPhiOperand(inst->PhiBlock(arg_index), phi_args[arg_index]);
        }
        return IR::Value{new_phi};
    }
    if (inst->GetOpcode() == IR::Opcode::UndefU32) {
        return IR::Value{inst};
    }
    if (inst->GetOpcode() == IR::Opcode::SelectU32) {
        const auto [it, is_new] = phi_map.try_emplace(inst);
        if (!is_new) {
            return it->second;
        }
        IR::Value sides[2];
        for (size_t i = 0; i < 2; ++i) {
            const IR::Value arg = inst->Arg(i + 1);
            sides[i] = arg.IsImmediate() ? arg : GetRealValue(phi_map, arg.Inst(), lane);
        }
        const auto insert_point = IR::Block::InstructionList::s_iterator_to(*inst);
        IR::Block* block = inst->GetParent();
        IR::Inst* const sel{&*block->PrependNewInst(insert_point, IR::Opcode::SelectU32,
                                                    {inst->Arg(0), sides[0], sides[1]})};
        phi_map[inst] = IR::Value{sel};
        return IR::Value{sel};
    }
    return GolfUndef();
}

void ReadLaneEliminationPass(IR::Program& program) {
    g_golf_undef = nullptr;
    g_golf_entry = program.post_order_blocks.back();
    PhiMap phi_map;
    std::vector<IR::Inst*> worklist;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::ReadLane) {
                continue;
            }
            if (!inst.Arg(1).IsImmediate()) {
                continue;
            }

            const u32 lane = inst.Arg(1).U32();
            IR::Inst* prod = inst.Arg(0).Inst();
            IR::Value replacement{};

            // Check simple case of no control flow and phis
            if (prod = SearchChain(prod, lane); prod->GetOpcode() == IR::Opcode::WriteLane) {
                replacement = prod->Arg(1);
            } else if ((prod->GetOpcode() == IR::Opcode::Phi ||
                        prod->GetOpcode() == IR::Opcode::SelectU32) &&
                       IsPossibleToEliminate(prod, lane)) {
                replacement = GetRealValue(phi_map, prod, lane);
                for (const auto& [_, phi_value] : phi_map) {
                    if (!phi_value.IsImmediate() &&
                        phi_value.Inst()->GetOpcode() == IR::Opcode::Phi) {
                        worklist.push_back(phi_value.Inst());
                    }
                }
                phi_map.clear();
            }
            if (replacement.IsEmpty()) {
                static std::atomic<int> golf_failed{0};
                if (golf_failed < 32) {
                    ++golf_failed;
                    LOG_ERROR(Render_Recompiler,
                              "golf-fix: readlane lane {} not eliminated, source {}", lane,
                              IR::NameOf(prod->GetOpcode()));
                }
                continue;
            }

            for (auto& [user, operand] : inst.Uses()) {
                if (user->GetOpcode() == IR::Opcode::Phi) {
                    worklist.push_back(user);
                }
            }
            inst.ReplaceUsesWith(replacement);
        }
    }
    // Attempt to resolve phis that have become trivial
    while (!worklist.empty()) {
        IR::Inst* phi = worklist.back();
        worklist.pop_back();

        if (phi->GetOpcode() == IR::Opcode::Void) {
            continue;
        }

        IR::Value same;
        bool non_trivial = false;
        for (size_t i = 0; i < phi->NumArgs(); ++i) {
            const IR::Value op{phi->Arg(i)};
            if (op == same || op == IR::Value{phi}) {
                // Unique value or self-reference
                continue;
            }
            if (!same.IsEmpty()) {
                // The phi merges at least two values: not trivial
                non_trivial = true;
                break;
            }
            same = op;
        }
        if (non_trivial) {
            continue;
        }

        IR::Block* block = phi->GetParent();
        ASSERT(!same.IsEmpty());

        // Add phi users to worklist since they may have become trivial
        for (const auto& [user, operand] : phi->Uses()) {
            if (user->GetOpcode() == IR::Opcode::Phi && user != phi) {
                worklist.push_back(user);
            }
        }
        phi->ReplaceUsesWithAndRemove(same);
        auto it = IR::Block::InstructionList::s_iterator_to(*phi);
        block->Instructions().erase(it);
    }
}

} // namespace Shader::Optimization
