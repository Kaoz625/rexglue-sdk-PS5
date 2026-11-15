/**
 * @file        ppc.h
 * @brief       PPC recompilation support -- umbrella header
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cmath>

// fmadd/fmsub/fnmadd/fnmsub (and their single forms) in recompiled code. Exact by default (std::fma, one
// rounding, as on Xenon). REX_PPC_UNFUSED_FMA: a*b+c, two roundings - for hosts without hardware FMA such as
// the Xbox One's Jaguar (btver2), where the C runtime's software fma() took ~70% of a game thread and held
// Army of Two at 8 fps (x360-native, 29 Oct 2026).
#ifndef REX_PPC_FMA
#if defined(REX_PPC_UNFUSED_FMA) && REX_PPC_UNFUSED_FMA
#define REX_PPC_FMA(a, b, c) ((a) * (b) + (c))
#else
#define REX_PPC_FMA(a, b, c) std::fma((a), (b), (c))
#endif
#endif

#include <rex/hook.h>
#include <rex/types.h>

#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/ppc/intrinsics.h>
#include <rex/ppc/stack.h>

// Consumer-facing using declarations
using rex::ppc::FindPPCFuncByName;
using rex::ppc::GetPPCFuncRegistry;
using rex::ppc::GuestToHostFunction;
using rex::ppc::HostToGuestFunction;
using rex::ppc::ImportFunction;
