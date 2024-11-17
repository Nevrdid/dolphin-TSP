// Copyright 2020 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/Expression.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fmt/format.h>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// https://github.com/zserge/expr/ is a C program and sorta valid C++.
// When included in a C++ program, it's treated as a C++ code, and it may cause
// issues: <cmath> may already be included, if so, including <math.h> may
// not do anything. <math.h> is obligated to put its functions in the global
// namespace, while <cmath> may or may not. The C code we're interpreting as
// C++ won't call functions by their qualified names. The code may work anyway
// if <cmath> puts its functions in the global namespace, or if the functions
// are actually macros that expand inline, both of which are common.
// NetBSD 10.0 i386 is an exception, and we need `using` there.
using std::isinf;
using std::isnan;
#include <expr.h>

#include "Common/BitUtils.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/Core.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

template <typename T>
static T HostRead(const Core::CPUThreadGuard& guard, u32 address);

template <typename T>
static void HostWrite(const Core::CPUThreadGuard& guard, T var, u32 address);

template <>
u8 HostRead(const Core::CPUThreadGuard& guard, u32 address)
{
  return PowerPC::MMU::HostRead_U8(guard, address);
}

template <>
u16 HostRead(const Core::CPUThreadGuard& guard, u32 address)
{
  return PowerPC::MMU::HostRead_U16(guard, address);
}

template <>
u32 HostRead(const Core::CPUThreadGuard& guard, u32 address)
{
  return PowerPC::MMU::HostRead_U32(guard, address);
}

template <>
u64 HostRead(const Core::CPUThreadGuard& guard, u32 address)
{
  return PowerPC::MMU::HostRead_U64(guard, address);
}

template <>
void HostWrite(const Core::CPUThreadGuard& guard, u8 var, u32 address)
{
  PowerPC::MMU::HostWrite_U8(guard, var, address);
}

template <>
void HostWrite(const Core::CPUThreadGuard& guard, u16 var, u32 address)
{
  PowerPC::MMU::HostWrite_U16(guard, var, address);
}

template <>
void HostWrite(const Core::CPUThreadGuard& guard, u32 var, u32 address)
{
  PowerPC::MMU::HostWrite_U32(guard, var, address);
}

template <>
void HostWrite(const Core::CPUThreadGuard& guard, u64 var, u32 address)
{
  PowerPC::MMU::HostWrite_U64(guard, var, address);
}

template <typename T, typename U = T>
static double HostReadFunc(expr_func* f, vec_expr_t* args, void* c)
{
  if (vec_len(args) != 1)
    return 0;
  const u32 address = static_cast<u32>(expr_eval(&vec_nth(args, 0)));

  Core::CPUThreadGuard guard(Core::System::GetInstance());
  return Common::BitCast<T>(HostRead<U>(guard, address));
}

template <typename T, typename U = T>
static double HostWriteFunc(expr_func* f, vec_expr_t* args, void* c)
{
  if (vec_len(args) != 2)
    return 0;
  const T var = static_cast<T>(expr_eval(&vec_nth(args, 0)));
  const u32 address = static_cast<u32>(expr_eval(&vec_nth(args, 1)));

  Core::CPUThreadGuard guard(Core::System::GetInstance());
  HostWrite<U>(guard, Common::BitCast<U>(var), address);
  return var;
}

template <typename T, typename U = T>
static double CastFunc(expr_func* f, vec_expr_t* args, void* c)
{
  if (vec_len(args) != 1)
    return 0;
  return Common::BitCast<T>(static_cast<U>(expr_eval(&vec_nth(args, 0))));
}

static double CallstackFunc(expr_func* f, vec_expr_t* args, void* c)
{
  if (vec_len(args) != 1)
    return 0;

  std::vector<Dolphin_Debugger::CallstackEntry> stack;
  {
    Core::CPUThreadGuard guard(Core::System::GetInstance());
    const bool success = Dolphin_Debugger::GetCallstack(guard, stack);
    if (!success)
      return 0;
  }

  double num = expr_eval(&vec_nth(args, 0));
  if (!std::isnan(num))
  {
    u32 address = static_cast<u32>(num);
    return std::any_of(stack.begin(), stack.end(),
                       [address](const auto& s) { return s.vAddress == address; });
  }

  const char* cstr = expr_get_str(&vec_nth(args, 0));
  if (cstr != nullptr)
  {
    return std::any_of(stack.begin(), stack.end(),
                       [cstr](const auto& s) { return s.Name.find(cstr) != std::string::npos; });
  }

  return 0;
}

static std::optional<std::string> ReadStringArg(const Core::CPUThreadGuard& guard, expr* e)
{
  double num = expr_eval(e);
  if (!std::isnan(num))
  {
    u32 address = static_cast<u32>(num);
    return PowerPC::MMU::HostGetString(guard, address);
  }

  const char* cstr = expr_get_str(e);
  if (cstr != nullptr)
  {
    return std::string(cstr);
  }

  return std::nullopt;
}

static double StreqFunc(expr_func* f, vec_expr_t* args, void* c)
{
  if (vec_len(args) != 2)
    return 0;

  std::array<std::string, 2> strs;
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  for (int i = 0; i < 2; i++)
  {
    std::optional<std::string> arg = ReadStringArg(guard, &vec_nth(args, i));
    if (arg == std::nullopt)
      return 0;

    strs[i] = std::move(*arg);
  }

  return strs[0] == strs[1];
}

static std::array<expr_func, 23> g_expr_funcs{{
    // For internal storage and comparisons, everything is auto-converted to Double.
    // If u64 ints are added, this could produce incorrect results.
    {"read_u8", HostReadFunc<u8>},
    {"read_s8", HostReadFunc<s8, u8>},
    {"read_u16", HostReadFunc<u16>},
    {"read_s16", HostReadFunc<s16, u16>},
    {"read_u32", HostReadFunc<u32>},
    {"read_s32", HostReadFunc<s32, u32>},
    {"read_f32", HostReadFunc<float, u32>},
    {"read_f64", HostReadFunc<double, u64>},
    {"write_u8", HostWriteFunc<u8>},
    {"write_u16", HostWriteFunc<u16>},
    {"write_u32", HostWriteFunc<u32>},
    {"write_f32", HostWriteFunc<float, u32>},
    {"write_f64", HostWriteFunc<double, u64>},
    {"u8", CastFunc<u8>},
    {"s8", CastFunc<s8, u8>},
    {"u16", CastFunc<u16>},
    {"s16", CastFunc<s16, u16>},
    {"u32", CastFunc<u32>},
    {"s32", CastFunc<s32, u32>},
    {"callstack", CallstackFunc},
    {"streq", StreqFunc},
    {},
}};

void ExprDeleter::operator()(expr* expression) const
{
  expr_destroy(expression, nullptr);
}

void ExprVarListDeleter::operator()(expr_var_list* vars) const
{
  // Free list elements
  expr_destroy(nullptr, vars);
  // Free list object
  delete vars;
}

Expression::Expression(std::string_view text, ExprPointer ex, ExprVarListPointer vars)
    : m_text(text), m_expr(std::move(ex)), m_vars(std::move(vars))
{
  using LookupKV = std::pair<std::string_view, Expression::VarBinding>;
  static constexpr auto sorted_lookup = []() consteval
  {
    // using enum Expression::VarBindingType;
    std::array<std::pair<std::basic_string_view<char>, Expression::VarBinding>, 148> unsorted_lookup = {{
        {"r0", {Expression::VarBindingType::GPR, 0}},
        {"r1", {Expression::VarBindingType::GPR, 1}},
        {"r2", {Expression::VarBindingType::GPR, 2}},
        {"r3", {Expression::VarBindingType::GPR, 3}},
        {"r4", {Expression::VarBindingType::GPR, 4}},
        {"r5", {Expression::VarBindingType::GPR, 5}},
        {"r6", {Expression::VarBindingType::GPR, 6}},
        {"r7", {Expression::VarBindingType::GPR, 7}},
        {"r8", {Expression::VarBindingType::GPR, 8}},
        {"r9", {Expression::VarBindingType::GPR, 9}},
        {"r10", {Expression::VarBindingType::GPR, 10}},
        {"r11", {Expression::VarBindingType::GPR, 11}},
        {"r12", {Expression::VarBindingType::GPR, 12}},
        {"r13", {Expression::VarBindingType::GPR, 13}},
        {"r14", {Expression::VarBindingType::GPR, 14}},
        {"r15", {Expression::VarBindingType::GPR, 15}},
        {"r16", {Expression::VarBindingType::GPR, 16}},
        {"r17", {Expression::VarBindingType::GPR, 17}},
        {"r18", {Expression::VarBindingType::GPR, 18}},
        {"r19", {Expression::VarBindingType::GPR, 19}},
        {"r20", {Expression::VarBindingType::GPR, 20}},
        {"r21", {Expression::VarBindingType::GPR, 21}},
        {"r22", {Expression::VarBindingType::GPR, 22}},
        {"r23", {Expression::VarBindingType::GPR, 23}},
        {"r24", {Expression::VarBindingType::GPR, 24}},
        {"r25", {Expression::VarBindingType::GPR, 25}},
        {"r26", {Expression::VarBindingType::GPR, 26}},
        {"r27", {Expression::VarBindingType::GPR, 27}},
        {"r28", {Expression::VarBindingType::GPR, 28}},
        {"r29", {Expression::VarBindingType::GPR, 29}},
        {"r30", {Expression::VarBindingType::GPR, 30}},
        {"r31", {Expression::VarBindingType::GPR, 31}},
        {"f0", {Expression::VarBindingType::FPR, 0}},
        {"f1", {Expression::VarBindingType::FPR, 1}},
        {"f2", {Expression::VarBindingType::FPR, 2}},
        {"f3", {Expression::VarBindingType::FPR, 3}},
        {"f4", {Expression::VarBindingType::FPR, 4}},
        {"f5", {Expression::VarBindingType::FPR, 5}},
        {"f6", {Expression::VarBindingType::FPR, 6}},
        {"f7", {Expression::VarBindingType::FPR, 7}},
        {"f8", {Expression::VarBindingType::FPR, 8}},
        {"f9", {Expression::VarBindingType::FPR, 9}},
        {"f10", {Expression::VarBindingType::FPR, 10}},
        {"f11", {Expression::VarBindingType::FPR, 11}},
        {"f12", {Expression::VarBindingType::FPR, 12}},
        {"f13", {Expression::VarBindingType::FPR, 13}},
        {"f14", {Expression::VarBindingType::FPR, 14}},
        {"f15", {Expression::VarBindingType::FPR, 15}},
        {"f16", {Expression::VarBindingType::FPR, 16}},
        {"f17", {Expression::VarBindingType::FPR, 17}},
        {"f18", {Expression::VarBindingType::FPR, 18}},
        {"f19", {Expression::VarBindingType::FPR, 19}},
        {"f20", {Expression::VarBindingType::FPR, 20}},
        {"f21", {Expression::VarBindingType::FPR, 21}},
        {"f22", {Expression::VarBindingType::FPR, 22}},
        {"f23", {Expression::VarBindingType::FPR, 23}},
        {"f24", {Expression::VarBindingType::FPR, 24}},
        {"f25", {Expression::VarBindingType::FPR, 25}},
        {"f26", {Expression::VarBindingType::FPR, 26}},
        {"f27", {Expression::VarBindingType::FPR, 27}},
        {"f28", {Expression::VarBindingType::FPR, 28}},
        {"f29", {Expression::VarBindingType::FPR, 29}},
        {"f30", {Expression::VarBindingType::FPR, 30}},
        {"f31", {Expression::VarBindingType::FPR, 31}},
        {"pc", {Expression::VarBindingType::PCtr}},
        {"msr", {Expression::VarBindingType::MSR}},
        {"xer", {Expression::VarBindingType::SPR, SPR_XER}},
        {"lr", {Expression::VarBindingType::SPR, SPR_LR}},
        {"ctr", {Expression::VarBindingType::SPR, SPR_CTR}},
        {"dsisr", {Expression::VarBindingType::SPR, SPR_DSISR}},
        {"dar", {Expression::VarBindingType::SPR, SPR_DAR}},
        {"dec", {Expression::VarBindingType::SPR, SPR_DEC}},
        {"sdr1", {Expression::VarBindingType::SPR, SPR_SDR}},
        {"srr0", {Expression::VarBindingType::SPR, SPR_SRR0}},
        {"srr1", {Expression::VarBindingType::SPR, SPR_SRR1}},
        {"tbl", {Expression::VarBindingType::SPR, SPR_TL}},
        {"tbu", {Expression::VarBindingType::SPR, SPR_TU}},
        {"pvr", {Expression::VarBindingType::SPR, SPR_PVR}},
        {"sprg0", {Expression::VarBindingType::SPR, SPR_SPRG0}},
        {"sprg1", {Expression::VarBindingType::SPR, SPR_SPRG1}},
        {"sprg2", {Expression::VarBindingType::SPR, SPR_SPRG2}},
        {"sprg3", {Expression::VarBindingType::SPR, SPR_SPRG3}},
        {"ear", {Expression::VarBindingType::SPR, SPR_EAR}},
        {"ibat0u", {Expression::VarBindingType::SPR, SPR_IBAT0U}},
        {"ibat0l", {Expression::VarBindingType::SPR, SPR_IBAT0L}},
        {"ibat1u", {Expression::VarBindingType::SPR, SPR_IBAT1U}},
        {"ibat1l", {Expression::VarBindingType::SPR, SPR_IBAT1L}},
        {"ibat2u", {Expression::VarBindingType::SPR, SPR_IBAT2U}},
        {"ibat2l", {Expression::VarBindingType::SPR, SPR_IBAT2L}},
        {"ibat3u", {Expression::VarBindingType::SPR, SPR_IBAT3U}},
        {"ibat3l", {Expression::VarBindingType::SPR, SPR_IBAT3L}},
        {"ibat4u", {Expression::VarBindingType::SPR, SPR_IBAT4U}},
        {"ibat4l", {Expression::VarBindingType::SPR, SPR_IBAT4L}},
        {"ibat5u", {Expression::VarBindingType::SPR, SPR_IBAT5U}},
        {"ibat5l", {Expression::VarBindingType::SPR, SPR_IBAT5L}},
        {"ibat6u", {Expression::VarBindingType::SPR, SPR_IBAT6U}},
        {"ibat6l", {Expression::VarBindingType::SPR, SPR_IBAT6L}},
        {"ibat7u", {Expression::VarBindingType::SPR, SPR_IBAT7U}},
        {"ibat7l", {Expression::VarBindingType::SPR, SPR_IBAT7L}},
        {"dbat0u", {Expression::VarBindingType::SPR, SPR_DBAT0U}},
        {"dbat0l", {Expression::VarBindingType::SPR, SPR_DBAT0L}},
        {"dbat1u", {Expression::VarBindingType::SPR, SPR_DBAT1U}},
        {"dbat1l", {Expression::VarBindingType::SPR, SPR_DBAT1L}},
        {"dbat2u", {Expression::VarBindingType::SPR, SPR_DBAT2U}},
        {"dbat2l", {Expression::VarBindingType::SPR, SPR_DBAT2L}},
        {"dbat3u", {Expression::VarBindingType::SPR, SPR_DBAT3U}},
        {"dbat3l", {Expression::VarBindingType::SPR, SPR_DBAT3L}},
        {"dbat4u", {Expression::VarBindingType::SPR, SPR_DBAT4U}},
        {"dbat4l", {Expression::VarBindingType::SPR, SPR_DBAT4L}},
        {"dbat5u", {Expression::VarBindingType::SPR, SPR_DBAT5U}},
        {"dbat5l", {Expression::VarBindingType::SPR, SPR_DBAT5L}},
        {"dbat6u", {Expression::VarBindingType::SPR, SPR_DBAT6U}},
        {"dbat6l", {Expression::VarBindingType::SPR, SPR_DBAT6L}},
        {"dbat7u", {Expression::VarBindingType::SPR, SPR_DBAT7U}},
        {"dbat7l", {Expression::VarBindingType::SPR, SPR_DBAT7L}},
        {"gqr0", {Expression::VarBindingType::SPR, SPR_GQR0 + 0}},
        {"gqr1", {Expression::VarBindingType::SPR, SPR_GQR0 + 1}},
        {"gqr2", {Expression::VarBindingType::SPR, SPR_GQR0 + 2}},
        {"gqr3", {Expression::VarBindingType::SPR, SPR_GQR0 + 3}},
        {"gqr4", {Expression::VarBindingType::SPR, SPR_GQR0 + 4}},
        {"gqr5", {Expression::VarBindingType::SPR, SPR_GQR0 + 5}},
        {"gqr6", {Expression::VarBindingType::SPR, SPR_GQR0 + 6}},
        {"gqr7", {Expression::VarBindingType::SPR, SPR_GQR0 + 7}},
        {"hid0", {Expression::VarBindingType::SPR, SPR_HID0}},
        {"hid1", {Expression::VarBindingType::SPR, SPR_HID1}},
        {"hid2", {Expression::VarBindingType::SPR, SPR_HID2}},
        {"hid4", {Expression::VarBindingType::SPR, SPR_HID4}},
        {"iabr", {Expression::VarBindingType::SPR, SPR_IABR}},
        {"dabr", {Expression::VarBindingType::SPR, SPR_DABR}},
        {"wpar", {Expression::VarBindingType::SPR, SPR_WPAR}},
        {"dmau", {Expression::VarBindingType::SPR, SPR_DMAU}},
        {"dmal", {Expression::VarBindingType::SPR, SPR_DMAL}},
        {"ecid_u", {Expression::VarBindingType::SPR, SPR_ECID_U}},
        {"ecid_m", {Expression::VarBindingType::SPR, SPR_ECID_M}},
        {"ecid_l", {Expression::VarBindingType::SPR, SPR_ECID_L}},
        {"usia", {Expression::VarBindingType::SPR, SPR_USIA}},
        {"sia", {Expression::VarBindingType::SPR, SPR_SIA}},
        {"l2cr", {Expression::VarBindingType::SPR, SPR_L2CR}},
        {"ictc", {Expression::VarBindingType::SPR, SPR_ICTC}},
        {"mmcr0", {Expression::VarBindingType::SPR, SPR_MMCR0}},
        {"mmcr1", {Expression::VarBindingType::SPR, SPR_MMCR1}},
        {"pmc1", {Expression::VarBindingType::SPR, SPR_PMC1}},
        {"pmc2", {Expression::VarBindingType::SPR, SPR_PMC2}},
        {"pmc3", {Expression::VarBindingType::SPR, SPR_PMC3}},
        {"pmc4", {Expression::VarBindingType::SPR, SPR_PMC4}},
        {"thrm1", {Expression::VarBindingType::SPR, SPR_THRM1}},
        {"thrm2", {Expression::VarBindingType::SPR, SPR_THRM2}},
        {"thrm3", {Expression::VarBindingType::SPR, SPR_THRM3}},
    }};
    std::ranges::sort(unsorted_lookup, {}, &LookupKV::first);
    return unsorted_lookup;
  }
  ();
  static_assert(std::ranges::adjacent_find(sorted_lookup, {}, &LookupKV::first) ==
                    sorted_lookup.end(),
                "Expression: Sorted lookup should not contain duplicate keys.");
  for (auto* v = m_vars->head; v != nullptr; v = v->next)
  {
    const auto iter = std::ranges::lower_bound(sorted_lookup, v->name, {}, &LookupKV::first);
    if (iter != sorted_lookup.end() && iter->first == v->name)
      m_binds.emplace_back(iter->second);
    else
      m_binds.emplace_back();
  }
}

std::optional<Expression> Expression::TryParse(std::string_view text)
{
  ExprVarListPointer vars{new expr_var_list{}};
  ExprPointer ex{expr_create(text.data(), text.length(), vars.get(), g_expr_funcs.data())};
  if (!ex)
    return std::nullopt;

  return Expression{text, std::move(ex), std::move(vars)};
}

double Expression::Evaluate(Core::System& system) const
{
  SynchronizeBindings(system, SynchronizeDirection::From);

  double result = expr_eval(m_expr.get());

  SynchronizeBindings(system, SynchronizeDirection::To);

  Reporting(result);

  return result;
}

void Expression::SynchronizeBindings(Core::System& system, SynchronizeDirection dir) const
{
  auto& ppc_state = system.GetPPCState();
  auto bind = m_binds.begin();
  for (auto* v = m_vars->head; v != nullptr; v = v->next, ++bind)
  {
    switch (bind->type)
    {
    case VarBindingType::Zero:
      if (dir == SynchronizeDirection::From)
        v->value = 0;
      break;
    case VarBindingType::GPR:
      if (dir == SynchronizeDirection::From)
        v->value = static_cast<double>(ppc_state.gpr[bind->index]);
      else
        ppc_state.gpr[bind->index] = static_cast<u32>(static_cast<s64>(v->value));
      break;
    case VarBindingType::FPR:
      if (dir == SynchronizeDirection::From)
        v->value = ppc_state.ps[bind->index].PS0AsDouble();
      else
        ppc_state.ps[bind->index].SetPS0(v->value);
      break;
    case VarBindingType::SPR:
      if (dir == SynchronizeDirection::From)
        v->value = static_cast<double>(ppc_state.spr[bind->index]);
      else
        ppc_state.spr[bind->index] = static_cast<u32>(static_cast<s64>(v->value));
      break;
    case VarBindingType::PCtr:
      if (dir == SynchronizeDirection::From)
        v->value = static_cast<double>(ppc_state.pc);
      break;
    case VarBindingType::MSR:
      if (dir == SynchronizeDirection::From)
        v->value = static_cast<double>(ppc_state.msr.Hex);
      else
        ppc_state.msr.Hex = static_cast<u32>(static_cast<s64>(v->value));
      break;
    }
  }
}

void Expression::Reporting(const double result) const
{
  bool is_nan = std::isnan(result);
  std::string message;

  for (auto* v = m_vars->head; v != nullptr; v = v->next)
  {
    if (std::isnan(v->value))
      is_nan = true;

    fmt::format_to(std::back_inserter(message), "  {}={}", v->name, v->value);
  }

  if (is_nan)
  {
    message.append("\nBreakpoint condition encountered a NaN");
    Core::DisplayMessage("Breakpoint condition has encountered a NaN.", 2000);
  }

  if (result != 0.0 || is_nan)
    NOTICE_LOG_FMT(MEMMAP, "Breakpoint condition returned: {}. Vars:{}", result, message);
}

std::string Expression::GetText() const
{
  return m_text;
}
