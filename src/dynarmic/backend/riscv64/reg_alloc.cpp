/* This file is part of the dynarmic project.
 * Copyright (c) 2024 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/riscv64/reg_alloc.h"

#include <algorithm>
#include <array>

#include <mcl/assert.hpp>
#include <mcl/stdint.hpp>

namespace Dynarmic::Backend::RV64 {

constexpr size_t spill_offset = offsetof(StackLayout, spill);
constexpr size_t spill_slot_size = sizeof(decltype(StackLayout::spill)::value_type);

static bool IsValuelessType(IR::Type type) {
    switch (type) {
    case IR::Type::Table:
        return true;
    default:
        return false;
    }
}

IR::Type Argument::GetType() const {
    return value.GetType();
}

bool Argument::IsImmediate() const {
    return value.IsImmediate();
}

bool Argument::GetImmediateU1() const {
    return value.GetU1();
}

u8 Argument::GetImmediateU8() const {
    const u64 imm = value.GetImmediateAsU64();
    ASSERT(imm < 0x100);
    return u8(imm);
}

u16 Argument::GetImmediateU16() const {
    const u64 imm = value.GetImmediateAsU64();
    ASSERT(imm < 0x10000);
    return u16(imm);
}

u32 Argument::GetImmediateU32() const {
    const u64 imm = value.GetImmediateAsU64();
    ASSERT(imm < 0x100000000);
    return u32(imm);
}

u64 Argument::GetImmediateU64() const {
    return value.GetImmediateAsU64();
}

IR::Cond Argument::GetImmediateCond() const {
    ASSERT(IsImmediate() && GetType() == IR::Type::Cond);
    return value.GetCond();
}

IR::AccType Argument::GetImmediateAccType() const {
    ASSERT(IsImmediate() && GetType() == IR::Type::AccType);
    return value.GetAccType();
}

bool HostLocInfo::Contains(const IR::Inst* value) const {
    return std::find(values.begin(), values.end(), value) != values.end();
}

RegAlloc::ArgumentInfo RegAlloc::GetArgumentInfo(IR::Inst* inst) {
    ArgumentInfo ret = {Argument{*this}, Argument{*this}, Argument{*this}, Argument{*this}};
    for (size_t i = 0; i < inst->NumArgs(); i++) {
        const IR::Value arg = inst->GetArg(i);
        ret[i].value = arg;
        if (!arg.IsImmediate() && !IsValuelessType(arg.GetType())) {
            ASSERT_MSG(ValueLocation(arg.GetInst()), "argument must already been defined");
            ValueInfo(arg.GetInst()).accumulated_uses++;
        }
    }
    return ret;
}

bool RegAlloc::IsValueLive(IR::Inst* inst) const {
    return !!ValueLocation(inst);
}

template<bool is_fpr>
u32 RegAlloc::RealizeReadImpl(const IR::Inst* value) {
    constexpr HostLoc::Kind required_kind = is_fpr ? HostLoc::Kind::Fpr : HostLoc::Kind::Gpr;

    const auto current_location = ValueLocation(value);
    ASSERT(current_location);

    if (current_location->kind == required_kind) {
        ValueInfo(*current_location).realized = true;
        return current_location->index;
    }

    ASSERT(!ValueInfo(*current_location).realized);
    ASSERT(!ValueInfo(*current_location).locked);

    if constexpr (is_fpr) {
        const u32 new_location_index = AllocateRegister(fprs, fpr_order);
        SpillFpr(new_location_index);

        switch (current_location->kind) {
        case HostLoc::Kind::Gpr:
            as.FMV_D_X(biscuit::FPR{new_location_index}, biscuit::GPR(current_location->index));
            break;
        case HostLoc::Kind::Fpr:
            ASSERT_FALSE("Logic error");
            break;
        case HostLoc::Kind::Spill:
            as.FLD(biscuit::FPR{new_location_index}, spill_offset + new_location_index * spill_slot_size, biscuit::sp);
            break;
        }

        fprs[new_location_index] = std::exchange(ValueInfo(*current_location), {});
        fprs[new_location_index].realized = true;
        return new_location_index;
    } else {
        const u32 new_location_index = AllocateRegister(gprs, gpr_order);
        SpillGpr(new_location_index);

        switch (current_location->kind) {
        case HostLoc::Kind::Gpr:
            ASSERT_FALSE("Logic error");
            break;
        case HostLoc::Kind::Fpr:
            as.FMV_X_D(biscuit::GPR(new_location_index), biscuit::FPR{current_location->index});
            // ASSERT size fits
            break;
        case HostLoc::Kind::Spill:
            as.LD(biscuit::GPR{new_location_index}, spill_offset + new_location_index * spill_slot_size, biscuit::sp);
            break;
        }

        gprs[new_location_index] = std::exchange(ValueInfo(*current_location), {});
        gprs[new_location_index].realized = true;
        return new_location_index;
    }
}

template<bool is_fpr>
u32 RegAlloc::RealizeWriteImpl(const IR::Inst* value) {
    ASSERT(!ValueLocation(value));

    const auto setup_location = [&](HostLocInfo& info) {
        info = {};
        info.values.emplace_back(value);
        info.locked = true;
        info.realized = true;
        info.expected_uses += value->UseCount();
    };

    if constexpr (is_fpr) {
        const u32 new_location_index = AllocateRegister(fprs, fpr_order);
        SpillFpr(new_location_index);
        setup_location(fprs[new_location_index]);
        return new_location_index;
    } else {
        const u32 new_location_index = AllocateRegister(gprs, gpr_order);
        SpillGpr(new_location_index);
        setup_location(gprs[new_location_index]);
        return new_location_index;
    }
}

template u32 RegAlloc::RealizeReadImpl<true>(const IR::Inst* value);
template u32 RegAlloc::RealizeReadImpl<false>(const IR::Inst* value);
template u32 RegAlloc::RealizeWriteImpl<true>(const IR::Inst* value);
template u32 RegAlloc::RealizeWriteImpl<false>(const IR::Inst* value);

void RegAlloc::Unlock(HostLoc host_loc) {
    HostLocInfo& info = ValueInfo(host_loc);
    if (!info.realized) {
        return;
    }

    if (info.accumulated_uses == info.expected_uses) {
        info = {};
    } else {
        info.realized = false;
        info.locked = false;
    }
}

u32 RegAlloc::AllocateRegister(const std::array<HostLocInfo, 32>& regs, const std::vector<u32>& order) const {
    const auto empty = std::find_if(order.begin(), order.end(), [&](u32 i) { return regs[i].values.empty() && !regs[i].locked; });
    if (empty != order.end()) {
        return *empty;
    }

    std::vector<u32> candidates;
    std::copy_if(order.begin(), order.end(), std::back_inserter(candidates), [&](u32 i) { return !regs[i].locked; });

    // TODO: LRU
    std::uniform_int_distribution<size_t> dis{0, candidates.size() - 1};
    return candidates[dis(rand_gen)];
}

void RegAlloc::SpillGpr(u32 index) {
    ASSERT(!gprs[index].locked && !gprs[index].realized);
    if (gprs[index].values.empty()) {
        return;
    }
    const u32 new_location_index = FindFreeSpill();
    as.SD(biscuit::GPR{index}, spill_offset + new_location_index * spill_slot_size, biscuit::sp);
    spills[new_location_index] = std::exchange(gprs[index], {});
}

void RegAlloc::SpillFpr(u32 index) {
    ASSERT(!fprs[index].locked && !fprs[index].realized);
    if (fprs[index].values.empty()) {
        return;
    }
    const u32 new_location_index = FindFreeSpill();
    as.FSD(biscuit::FPR{index}, spill_offset + new_location_index * spill_slot_size, biscuit::sp);
    spills[new_location_index] = std::exchange(fprs[index], {});
}

u32 RegAlloc::FindFreeSpill() const {
    const auto iter = std::find_if(spills.begin(), spills.end(), [](const HostLocInfo& info) { return info.values.empty(); });
    ASSERT_MSG(iter != spills.end(), "All spill locations are full");
    return static_cast<u32>(iter - spills.begin());
}

std::optional<HostLoc> RegAlloc::ValueLocation(const IR::Inst* value) const {
    const auto contains_value = [value](const HostLocInfo& info) {
        return info.Contains(value);
    };

    if (const auto iter = std::find_if(gprs.begin(), gprs.end(), contains_value); iter != gprs.end()) {
        return HostLoc{HostLoc::Kind::Gpr, static_cast<u32>(iter - gprs.begin())};
    }
    if (const auto iter = std::find_if(fprs.begin(), fprs.end(), contains_value); iter != fprs.end()) {
        return HostLoc{HostLoc::Kind::Fpr, static_cast<u32>(iter - fprs.begin())};
    }
    if (const auto iter = std::find_if(spills.begin(), spills.end(), contains_value); iter != spills.end()) {
        return HostLoc{HostLoc::Kind::Spill, static_cast<u32>(iter - spills.begin())};
    }
    return std::nullopt;
}

HostLocInfo& RegAlloc::ValueInfo(HostLoc host_loc) {
    switch (host_loc.kind) {
    case HostLoc::Kind::Gpr:
        return gprs[static_cast<size_t>(host_loc.index)];
    case HostLoc::Kind::Fpr:
        return fprs[static_cast<size_t>(host_loc.index)];
    case HostLoc::Kind::Spill:
        return spills[static_cast<size_t>(host_loc.index)];
    }
    ASSERT_FALSE("RegAlloc::ValueInfo: Invalid HostLoc::Kind");
}

HostLocInfo& RegAlloc::ValueInfo(const IR::Inst* value) {
    const auto contains_value = [value](const HostLocInfo& info) {
        return info.Contains(value);
    };

    if (const auto iter = std::find_if(gprs.begin(), gprs.end(), contains_value)) {
        return *iter;
    }
    if (const auto iter = std::find_if(fprs.begin(), fprs.end(), contains_value)) {
        return *iter;
    }
    if (const auto iter = std::find_if(spills.begin(), spills.end(), contains_value)) {
        return *iter;
    }
    ASSERT_FALSE("RegAlloc::ValueInfo: Value not found");
}

}  // namespace Dynarmic::Backend::RV64
