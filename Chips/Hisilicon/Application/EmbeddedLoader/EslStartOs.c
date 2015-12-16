/** @file
*
*  Copyright (c) 2011-2015, ARM Limited. All rights reserved.
*  Copyright (c) 2015, Hisilicon Limited. All rights reserved.
*  Copyright (c) 2015, Linaro Limited. All rights reserved.
*
*  This program and the accompanying materials
*  are licensed and made available under the terms and conditions of the BSD License
*  which accompanies this distribution.  The full text of the license may be found at
*  http://opensource.org/licenses/bsd-license.php
*
*  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
*  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*
**/

#include "CustomLoader.h"

EFI_STATUS
EFIAPI
EslStartOsEntry (
  IN EFI_HANDLE          ImageHandle,
  IN EFI_SYSTEM_TABLE*   SystemTable
  )
{
    EFI_STATUS Status;
    UINT64     Reg_Value;
    ESL_LINUX LinuxKernel = (ESL_LINUX)(0x80000); 

    if(!PcdGet32(PcdIsMPBoot))
    {
        DEBUG((EFI_D_ERROR,"Update FDT\n"));
        Status = EFIFdtUpdate(0x06000000);
        if(EFI_ERROR(Status))
        {
            DEBUG((EFI_D_ERROR,"EFIFdtUpdate ERROR\n"));
            goto Exit;
        }
    }
    DEBUG((EFI_D_ERROR, "[%a]:[%dL] Start to boot Linux\n", __FUNCTION__, __LINE__));

    SmmuConfigForLinux();

    ITSCONFIG();

    if(PcdGet32(PcdIsMPBoot))
    {
        *(volatile UINT32 *)(0x60016220)   = 0x7;
        *(volatile UINT32 *)(0x60016230)   = 0x40016260;
        *(volatile UINT32 *)(0x60016234)   = 0X0;
        *(volatile UINT32 *)(0x60016238)   = 0x60016260;
        *(volatile UINT32 *)(0x6001623C)   = 0x400;
        *(volatile UINT32 *)(0x60016240)   = 0x40016260;
        *(volatile UINT32 *)(0x60016244)   = 0x400;

        *(volatile UINT32 *)(0x40016220)   = 0x7;
        *(volatile UINT32 *)(0x40016230)   = 0x60016260;
        *(volatile UINT32 *)(0x40016234)   = 0X0;
        *(volatile UINT32 *)(0x40016238)   = 0x60016260;
        *(volatile UINT32 *)(0x4001623C)   = 0x400;
        *(volatile UINT32 *)(0x40016240)   = 0x40016260;
        *(volatile UINT32 *)(0x40016244)   = 0x400;

        *(volatile UINT32 *)(0x60016220 + S1_BASE)   = 0x7;
        *(volatile UINT32 *)(0x60016230 + S1_BASE)   = 0x40016260;
        *(volatile UINT32 *)(0x60016234 + S1_BASE)   = 0X0;
        *(volatile UINT32 *)(0x60016238 + S1_BASE)   = 0x60016260;
        *(volatile UINT32 *)(0x6001623C + S1_BASE)   = 0x0;
        *(volatile UINT32 *)(0x60016240 + S1_BASE)   = 0x40016260;
        *(volatile UINT32 *)(0x60016244 + S1_BASE)   = 0x400;

        *(volatile UINT32 *)(0x40016220 + S1_BASE)   = 0x7;
        *(volatile UINT32 *)(0x40016230 + S1_BASE)   = 0x60016260;
        *(volatile UINT32 *)(0x40016234 + S1_BASE)   = 0X0;
        *(volatile UINT32 *)(0x40016238 + S1_BASE)   = 0x60016260;
        *(volatile UINT32 *)(0x4001623C + S1_BASE)   = 0x400;
        *(volatile UINT32 *)(0x40016240 + S1_BASE)   = 0x40016260;
        *(volatile UINT32 *)(0x40016244 + S1_BASE)   = 0x0;
    }
    
    Status = ShutdownUefiBootServices ();
    if(EFI_ERROR(Status)) 
    {
        DEBUG((EFI_D_ERROR,"ERROR: Can not shutdown UEFI boot services. Status=0x%X\n", Status));
        goto Exit;
    }

    //
    // Switch off interrupts, caches, mmu, etc
    //
    Status = PreparePlatformHardware ();
    ASSERT_EFI_ERROR(Status);

    *(volatile UINT32*)0xFFF8 = 0x0;
    *(volatile UINT32*)0xFFFC = 0x0;
    asm("DSB SY");
    asm("ISB");

    if (!PcdGet64 (PcdTrustedFirmwareEnable))
    {
        StartupAp();
    }

    Reg_Value = ArmReadCpuExCr();
    DEBUG((EFI_D_ERROR,"CPUECTLR_EL1 = 0x%llx\n",Reg_Value));

    MN_CONFIG ();

    DEBUG((EFI_D_ERROR, "[%a]:[%dL] Start to jump Linux kernel\n", __FUNCTION__, __LINE__));

    LinuxKernel (0x06000000,0,0,0);
    
    // Kernel should never exit
    // After Life services are not provided
    ASSERT(FALSE);
    Status = EFI_ABORTED;

Exit:
    // Only be here if we fail to start Linux
    Print (L"ERROR  : Can not start the kernel. Status=%r\n", Status);

    // Free Runtimee Memory (kernel and FDT)
    return Status;
}

