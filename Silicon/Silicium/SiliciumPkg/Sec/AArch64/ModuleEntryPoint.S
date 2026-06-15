//
// Copyright (c) 2011 - 2025, Arm Limited. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause-Patent
//

#include <AsmMacroLib.h>

#include <Library/ArmLib.h>

//
// SCTRL_EL1 Bits
//
.set CTRL_SPAN_BIT, (1 << 23)

ASM_FUNC (_ModuleEntryPoint)
  // === DIAG: paint cont_splash 0x5C000000 GREEN to prove SEC entry reached ===
  movz  x20, #0x0000
  movk  x20, #0x5C00, lsl #16     // x20 = 0x5C000000 (framebuffer)
  movz  w21, #0xFF00
  movk  w21, #0xFF00, lsl #16     // w21 = 0xFF00FF00 (green)
  movz  x22, #0x0000
  movk  x22, #0x0040, lsl #16     // x22 = 0x00400000 (word count)
7:str   w21, [x20], #4
  subs  x22, x22, #1
  b.ne  7b
  // === end DIAG ===

  // Do Platform Specific Initialization
  bl    ASM_PFX (PlatformAssemblyInitialize)

  // Disable Interrupts
  bl    ASM_PFX (ArmDisableInterrupts)

  // Disable MMU and CPU Caches
  bl    ASM_PFX (ArmDisableCachesAndMmu)

  // Invalidate I-Cache
  bl    ASM_PFX (ArmInvalidateInstructionCache)

  // Invalidate TLB
  bl    ASM_PFX (ArmInvalidateTlb)

  // Disable SPAN
  EL1_OR_EL2 (x0)
1:mrs   x0, sctlr_el1
  orr   x0, x0, #CTRL_SPAN_BIT
  msr   sctlr_el1, x0
2:nop

  // Enable Floating Point
  bl    ASM_PFX (ArmEnableVFP)

  // Setup Exception Vector
  EL1_OR_EL2 (x0)
1:bl    ASM_PFX (SetupExceptionLevel1)
  b     ASM_PFX (MainEntryPoint)
2:bl    ASM_PFX (SetupExceptionLevel2)
  b     ASM_PFX (MainEntryPoint)

ASM_PFX (MainEntryPoint):
  // Get Stack Details
  MOV64 (x8, FixedPcdGet64 (PcdCPUCoresStackBase))
  MOV32 (x9, FixedPcdGet32 (PcdCPUCorePrimaryStackSize))

  // Set Stack End Address
  add   x7, x8, x9

  // Set Stack Pointer
  mov   sp, x7

  // Clear Stack
  mov   x6, xzr
0:stp   x6, x6, [x8], #16
  cmp   x8, x7
  b.lt  0b

  // Re-Enable CPU Caches
  bl    ASM_PFX (ArmEnableInstructionCache)
  bl    ASM_PFX (ArmEnableDataCache)

  // Pass Parameters
  mov   x0, x8
  mov   x1, x9

  // Set Jump Location
  ldr   x2, =ASM_PFX (CEntryPoint)

  // Clear Frame pointer
  mov   x29, xzr

  // Jump to C Code
  blr   x2
