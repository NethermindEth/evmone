// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2020 The evmone Authors.
// SPDX-License-Identifier: Apache-2.0

#include "baseline.hpp"
#include "baseline_instruction_table.hpp"
#include "execution_state.hpp"
#include "instructions.hpp"
#include "vm.hpp"
#include <evmc/instructions.h>
#include <memory>

#ifdef _MSC_VER
#define CASE(OPCODE) case OP_##OPCODE:
#else
#define CASE(OPCODE)  \
    case OP_##OPCODE: \
        asm("#" #OPCODE);
#endif


namespace evmone::baseline
{
CodeAnalysis analyze(const uint8_t* code, size_t code_size)
{
    // To find if op is any PUSH opcode (OP_PUSH1 <= op <= OP_PUSH32)
    // it can be noticed that OP_PUSH32 is INT8_MAX (0x7f) therefore
    // static_cast<int8_t>(op) <= OP_PUSH32 is always true and can be skipped.
    static_assert(OP_PUSH32 == std::numeric_limits<int8_t>::max());

    CodeAnalysis::JumpdestMap map(code_size);  // Allocate and init bitmap with zeros.
    size_t i = 0;
    while (i < code_size)
    {
        const auto op = code[i];
        if (static_cast<int8_t>(op) >= OP_PUSH1)  // If any PUSH opcode (see explanation above).
            i += op - size_t{OP_PUSH1 - 1};       // Skip PUSH data.
        else if (INTX_UNLIKELY(op == OP_JUMPDEST))
            map[i] = true;
        ++i;
    }

    // i is the needed code size including the last push data (can be bigger than code_size).
    // Using "raw" new operator instead of std::make_unique() to get uninitialized array.
    std::unique_ptr<uint8_t[]> padded_code{new uint8_t[i + 1]};  // +1 for the final STOP.
    std::copy_n(code, code_size, padded_code.get());
    padded_code[i] = OP_STOP;  // Set final STOP at the code end.

    // TODO: Using fixed-size padding of 33, the padded code buffer and jumpdest bitmap can be
    //       created with single allocation.

    return CodeAnalysis{std::move(padded_code), std::move(map)};
}

namespace
{
const uint8_t* op_jump(
    ExecutionState& state, const CodeAnalysis::JumpdestMap& jumpdest_map) noexcept
{
    const auto dst = state.stack.pop();
    if (dst >= jumpdest_map.size() || !jumpdest_map[static_cast<size_t>(dst)])
    {
        state.status = EVMC_BAD_JUMP_DESTINATION;
        return &state.code[0] + state.code.size();
    }

    return &state.code[static_cast<size_t>(dst)];
}

template <size_t Len>
inline const uint8_t* load_push(ExecutionState& state, const uint8_t* code) noexcept
{
    uint8_t buffer[Len];
    // This valid because code is padded with garbage to satisfy push data read pass the code end.
    std::memcpy(buffer, code, Len);
    state.stack.push(intx::be::load<intx::uint256>(buffer));
    return code + Len;
}

template <evmc_status_code StatusCode>
inline void op_return(ExecutionState& state) noexcept
{
    const auto offset = state.stack[0];
    const auto size = state.stack[1];

    if (!check_memory(state, offset, size))
    {
        state.status = EVMC_OUT_OF_GAS;
        return;
    }

    state.output_offset = static_cast<size_t>(offset);  // Can be garbage if size is 0.
    state.output_size = static_cast<size_t>(size);
    state.status = StatusCode;
}

inline evmc_status_code check_requirements(
    const InstructionTable& instruction_table, ExecutionState& state, uint8_t op) noexcept
{
    const auto metrics = instruction_table[op];

    if (INTX_UNLIKELY(metrics.gas_cost == instr::undefined))
        return EVMC_UNDEFINED_INSTRUCTION;

    if (INTX_UNLIKELY((state.gas_left -= metrics.gas_cost) < 0))
        return EVMC_OUT_OF_GAS;

    const auto stack_size = state.stack.size();
    if (INTX_UNLIKELY(stack_size == Stack::limit))
    {
        if (metrics.can_overflow_stack)
            return EVMC_STACK_OVERFLOW;
    }
    else if (INTX_UNLIKELY(stack_size < metrics.stack_height_required))
        return EVMC_STACK_UNDERFLOW;

    return EVMC_SUCCESS;
}

template <bool TracingEnabled>
evmc_result execute(const VM& vm, ExecutionState& state, const CodeAnalysis& analysis) noexcept
{
    // Use padded code.
    state.code = {analysis.padded_code.get(), state.code.size()};

    auto* tracer = vm.get_tracer();
    if constexpr (TracingEnabled)
        tracer->notify_execution_start(state.rev, *state.msg, state.code);

    const auto& instruction_table = get_baseline_instruction_table(state.rev);

    const auto* const code = state.code.data();
    auto pc = code;
    while (true)  // Guaranteed to terminate because padded code ends with STOP.
    {
        if constexpr (TracingEnabled)
        {
            const auto offset = static_cast<uint32_t>(pc - code);
            if (offset < state.code.size())  // Skip STOP from code padding.
                tracer->notify_instruction_start(offset, state);
        }

        const auto op = *pc;
        const auto status = check_requirements(instruction_table, state, op);
        if (status != EVMC_SUCCESS)
        {
            state.status = status;
            goto exit;
        }

        switch (op)
        {
            CASE(STOP)
            goto exit;
            CASE(ADD)
            add(state.stack);
            break;
            CASE(MUL)
            mul(state.stack);
            break;
            CASE(SUB)
            sub(state.stack);
            break;
            CASE(DIV)
            div(state.stack);
            break;
            CASE(SDIV)
            sdiv(state.stack);
            break;
            CASE(MOD)
            mod(state.stack);
            break;
            CASE(SMOD)
            smod(state.stack);
            break;
            CASE(ADDMOD)
            addmod(state.stack);
            break;
            CASE(MULMOD)
            mulmod(state.stack);
            break;
            CASE(EXP)
            {
                const auto status_code = exp(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(SIGNEXTEND)
            signextend(state.stack);
            break;

            CASE(LT)
            lt(state.stack);
            break;
            CASE(GT)
            gt(state.stack);
            break;
            CASE(SLT)
            slt(state.stack);
            break;
            CASE(SGT)
            sgt(state.stack);
            break;
            CASE(EQ)
            eq(state.stack);
            break;
            CASE(ISZERO)
            iszero(state.stack);
            break;
            CASE(AND)
            and_(state.stack);
            break;
            CASE(OR)
            or_(state.stack);
            break;
            CASE(XOR)
            xor_(state.stack);
            break;
            CASE(NOT)
            not_(state.stack);
            break;
            CASE(BYTE)
            byte(state.stack);
            break;
            CASE(SHL)
            shl(state.stack);
            break;
            CASE(SHR)
            shr(state.stack);
            break;
            CASE(SAR)
            sar(state.stack);
            break;

            CASE(KECCAK256)
            {
                const auto status_code = keccak256(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }

            CASE(ADDRESS)
            address(state);
            break;
            CASE(BALANCE)
            {
                const auto status_code = balance(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(ORIGIN)
            origin(state);
            break;
            CASE(CALLER)
            caller(state);
            break;
            CASE(CALLVALUE)
            callvalue(state);
            break;
            CASE(CALLDATALOAD)
            calldataload(state);
            break;
            CASE(CALLDATASIZE)
            calldatasize(state);
            break;
            CASE(CALLDATACOPY)
            {
                const auto status_code = calldatacopy(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(CODESIZE)
            codesize(state);
            break;
            CASE(CODECOPY)
            {
                const auto status_code = codecopy(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(GASPRICE)
            gasprice(state);
            break;
            CASE(EXTCODESIZE)
            {
                const auto status_code = extcodesize(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(EXTCODECOPY)
            {
                const auto status_code = extcodecopy(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(RETURNDATASIZE)
            returndatasize(state);
            break;
            CASE(RETURNDATACOPY)
            {
                const auto status_code = returndatacopy(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(EXTCODEHASH)
            {
                const auto status_code = extcodehash(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(BLOCKHASH)
            blockhash(state);
            break;
            CASE(COINBASE)
            coinbase(state);
            break;
            CASE(TIMESTAMP)
            timestamp(state);
            break;
            CASE(NUMBER)
            number(state);
            break;
            CASE(DIFFICULTY)
            difficulty(state);
            break;
            CASE(GASLIMIT)
            gaslimit(state);
            break;
            CASE(CHAINID)
            chainid(state);
            break;
            CASE(SELFBALANCE)
            selfbalance(state);
            break;
            CASE(BASEFEE)
            basefee(state);
            break;

            CASE(POP)
            pop(state.stack);
            break;
            CASE(MLOAD)
            {
                const auto status_code = mload(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(MSTORE)
            {
                const auto status_code = mstore(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(MSTORE8)
            {
                const auto status_code = mstore8(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }

            CASE(JUMP)
            pc = op_jump(state, analysis.jumpdest_map);
            continue;
            CASE(JUMPI)
            if (state.stack[1] != 0)
            {
                pc = op_jump(state, analysis.jumpdest_map);
            }
            else
            {
                state.stack.pop();
                ++pc;
            }
            state.stack.pop();
            continue;

            CASE(PC)
            state.stack.push(pc - code);
            break;
            CASE(MSIZE)
            msize(state);
            break;
            CASE(SLOAD)
            {
                const auto status_code = sload(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(SSTORE)
            {
                const auto status_code = sstore(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(GAS)
            state.stack.push(state.gas_left);
            break;
            CASE(JUMPDEST)
            break;

            CASE(PUSH1)
            pc = load_push<1>(state, pc + 1);
            continue;

            CASE(PUSH2)
            pc = load_push<2>(state, pc + 1);
            continue;

            CASE(PUSH3)
            pc = load_push<3>(state, pc + 1);
            continue;
            CASE(PUSH4)
            pc = load_push<4>(state, pc + 1);
            continue;
            CASE(PUSH5)
            pc = load_push<5>(state, pc + 1);
            continue;
            CASE(PUSH6)
            pc = load_push<6>(state, pc + 1);
            continue;
            CASE(PUSH7)
            pc = load_push<7>(state, pc + 1);
            continue;
            CASE(PUSH8)
            pc = load_push<8>(state, pc + 1);
            continue;
            CASE(PUSH9)
            pc = load_push<9>(state, pc + 1);
            continue;
            CASE(PUSH10)
            pc = load_push<10>(state, pc + 1);
            continue;
            CASE(PUSH11)
            pc = load_push<11>(state, pc + 1);
            continue;
            CASE(PUSH12)
            pc = load_push<12>(state, pc + 1);
            continue;
            CASE(PUSH13)
            pc = load_push<13>(state, pc + 1);
            continue;
            CASE(PUSH14)
            pc = load_push<14>(state, pc + 1);
            continue;
            CASE(PUSH15)
            pc = load_push<15>(state, pc + 1);
            continue;
            CASE(PUSH16)
            pc = load_push<16>(state, pc + 1);
            continue;
            CASE(PUSH17)
            pc = load_push<17>(state, pc + 1);
            continue;
            CASE(PUSH18)
            pc = load_push<18>(state, pc + 1);
            continue;
            CASE(PUSH19)
            pc = load_push<19>(state, pc + 1);
            continue;
            CASE(PUSH20)
            pc = load_push<20>(state, pc + 1);
            continue;
            CASE(PUSH21)
            pc = load_push<21>(state, pc + 1);
            continue;
            CASE(PUSH22)
            pc = load_push<22>(state, pc + 1);
            continue;
            CASE(PUSH23)
            pc = load_push<23>(state, pc + 1);
            continue;
            CASE(PUSH24)
            pc = load_push<24>(state, pc + 1);
            continue;
            CASE(PUSH25)
            pc = load_push<25>(state, pc + 1);
            continue;
            CASE(PUSH26)
            pc = load_push<26>(state, pc + 1);
            continue;
            CASE(PUSH27)
            pc = load_push<27>(state, pc + 1);
            continue;
            CASE(PUSH28)
            pc = load_push<28>(state, pc + 1);
            continue;
            CASE(PUSH29)
            pc = load_push<29>(state, pc + 1);
            continue;
            CASE(PUSH30)
            pc = load_push<30>(state, pc + 1);
            continue;
            CASE(PUSH31)
            pc = load_push<31>(state, pc + 1);
            continue;
            CASE(PUSH32)
            pc = load_push<32>(state, pc + 1);
            continue;

            CASE(DUP1)
            dup<1>(state.stack);
            break;
            CASE(DUP2)
            dup<2>(state.stack);
            break;
            CASE(DUP3)
            dup<3>(state.stack);
            break;
            CASE(DUP4)
            dup<4>(state.stack);
            break;
            CASE(DUP5)
            dup<5>(state.stack);
            break;
            CASE(DUP6)
            dup<6>(state.stack);
            break;
            CASE(DUP7)
            dup<7>(state.stack);
            break;
            CASE(DUP8)
            dup<8>(state.stack);
            break;
            CASE(DUP9)
            dup<9>(state.stack);
            break;
            CASE(DUP10)
            dup<10>(state.stack);
            break;
            CASE(DUP11)
            dup<11>(state.stack);
            break;
            CASE(DUP12)
            dup<12>(state.stack);
            break;
            CASE(DUP13)
            dup<13>(state.stack);
            break;
            CASE(DUP14)
            dup<14>(state.stack);
            break;
            CASE(DUP15)
            dup<15>(state.stack);
            break;
            CASE(DUP16)
            dup<16>(state.stack);
            break;

            CASE(SWAP1)
            swap<1>(state.stack);
            break;
            CASE(SWAP2)
            swap<2>(state.stack);
            break;
            CASE(SWAP3)
            swap<3>(state.stack);
            break;
            CASE(SWAP4)
            swap<4>(state.stack);
            break;
            CASE(SWAP5)
            swap<5>(state.stack);
            break;
            CASE(SWAP6)
            swap<6>(state.stack);
            break;
            CASE(SWAP7)
            swap<7>(state.stack);
            break;
            CASE(SWAP8)
            swap<8>(state.stack);
            break;
            CASE(SWAP9)
            swap<9>(state.stack);
            break;
            CASE(SWAP10)
            swap<10>(state.stack);
            break;
            CASE(SWAP11)
            swap<11>(state.stack);
            break;
            CASE(SWAP12)
            swap<12>(state.stack);
            break;
            CASE(SWAP13)
            swap<13>(state.stack);
            break;
            CASE(SWAP14)
            swap<14>(state.stack);
            break;
            CASE(SWAP15)
            swap<15>(state.stack);
            break;
            CASE(SWAP16)
            swap<16>(state.stack);
            break;

            CASE(LOG0)
            {
                const auto status_code = log(state, 0);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(LOG1)
            {
                const auto status_code = log(state, 1);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(LOG2)
            {
                const auto status_code = log(state, 2);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(LOG3)
            {
                const auto status_code = log(state, 3);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(LOG4)
            {
                const auto status_code = log(state, 4);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }

            CASE(CREATE)
            {
                const auto status_code = create<EVMC_CREATE>(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(CALL)
            {
                const auto status_code = call<EVMC_CALL>(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(CALLCODE)
            {
                const auto status_code = call<EVMC_CALLCODE>(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(RETURN)
            op_return<EVMC_SUCCESS>(state);
            goto exit;
            CASE(DELEGATECALL)
            {
                const auto status_code = call<EVMC_DELEGATECALL>(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(STATICCALL)
            {
                const auto status_code = call<EVMC_CALL, true>(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(CREATE2)
            {
                const auto status_code = create<EVMC_CREATE2>(state);
                if (status_code != EVMC_SUCCESS)
                {
                    state.status = status_code;
                    goto exit;
                }
                break;
            }
            CASE(REVERT)
            op_return<EVMC_REVERT>(state);
            goto exit;
            CASE(INVALID)
            state.status = EVMC_INVALID_INSTRUCTION;
            goto exit;
            CASE(SELFDESTRUCT)
            state.status = selfdestruct(state);
            goto exit;
        default:
            INTX_UNREACHABLE();
        }

        ++pc;
    }

exit:
    const auto gas_left =
        (state.status == EVMC_SUCCESS || state.status == EVMC_REVERT) ? state.gas_left : 0;

    const auto result = evmc::make_result(state.status, gas_left,
        state.output_size != 0 ? &state.memory[state.output_offset] : nullptr, state.output_size);

    if constexpr (TracingEnabled)
        tracer->notify_execution_end(result);

    return result;
}
}  // namespace

evmc_result execute(const VM& vm, ExecutionState& state, const CodeAnalysis& analysis) noexcept
{
    if (INTX_UNLIKELY(vm.get_tracer() != nullptr))
        return execute<true>(vm, state, analysis);

    return execute<false>(vm, state, analysis);
}

evmc_result execute(evmc_vm* c_vm, const evmc_host_interface* host, evmc_host_context* ctx,
    evmc_revision rev, const evmc_message* msg, const uint8_t* code, size_t code_size) noexcept
{
    auto vm = static_cast<VM*>(c_vm);
    const auto jumpdest_map = analyze(code, code_size);
    auto state = std::make_unique<ExecutionState>(*msg, rev, *host, ctx, code, code_size);
    return execute(*vm, *state, jumpdest_map);
}
}  // namespace evmone::baseline
