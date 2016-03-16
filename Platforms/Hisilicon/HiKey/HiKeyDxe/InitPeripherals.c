/** @file
*
*  Copyright (c) 2015, Linaro Ltd. All rights reserved.
*  Copyright (c) 2015, Hisilicon Ltd. All rights reserved.
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


#include <Library/UefiLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Hi6220.h>

#include "Hi6220RegsPeri.h"
#include "HiKeyDxeInternal.h"

STATIC
VOID
UsbPhyInit (
  IN VOID
  )
{
  EFI_STATUS    Status;
  UINT32        Value, Data;
  UINTN         VariableSize;
  CHAR16        UsbType[USB_TYPE_LENGTH];

  VariableSize = USB_TYPE_LENGTH * sizeof (CHAR16);
  Status = gRT->GetVariable (
		(CHAR16 *)L"DwUsbType",
		&gDwUsbTypeVariableGuid,
		NULL,
		&VariableSize,
		&UsbType
          );

  if (EFI_ERROR(Status))
      DEBUG ((EFI_D_ERROR, "Variable Not Found\n"));

  //setup clock
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_CLKEN0, BIT4);
  do {
       Value = MmioRead32 (PERI_CTRL_BASE + SC_PERIPH_CLKSTAT0);
  } while ((Value & BIT4) == 0);

  //setup phy
  Data = RST0_USBOTG_BUS | RST0_POR_PICOPHY |
           RST0_USBOTG | RST0_USBOTG_32K;
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_RSTDIS0, Data);
  do {
      Value = MmioRead32 (PERI_CTRL_BASE + SC_PERIPH_RSTSTAT0);
      Value &= Data;
  } while (Value);

  Value = MmioRead32 (PERI_CTRL_BASE + SC_PERIPH_CTRL4);
  Value &= ~(CTRL4_PICO_SIDDQ | CTRL4_FPGA_EXT_PHY_SEL |
             CTRL4_OTG_PHY_SEL);
  Value |=  CTRL4_PICO_VBUSVLDEXT | CTRL4_PICO_VBUSVLDEXTSEL;
  MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_CTRL4, Value);
  MicroSecondDelay (1000);

  if (!StrCmp(UsbType, (const CHAR16 *)L"device")) {
         Value = MmioRead32 (PERI_CTRL_BASE + SC_PERIPH_CTRL5);
         Value &= ~CTRL5_PICOPHY_BC_MODE;
         MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_CTRL5, Value);
         MicroSecondDelay (20000);
  } else if (!StrCmp(UsbType, (const CHAR16 *)L"host")) {
         /*CTRL5*/
         Data = MmioRead32 (PERI_CTRL_BASE + SC_PERIPH_CTRL5);
         Data &= ~CTRL5_PICOPHY_BC_MODE;
         Data |= CTRL5_USBOTG_RES_SEL | CTRL5_PICOPHY_ACAENB |
                 CTRL5_PICOPHY_VDATDETENB | CTRL5_PICOPHY_DCDENB;
         MmioWrite32 (PERI_CTRL_BASE + SC_PERIPH_CTRL5, Data);
         MicroSecondDelay (20000);
         MmioWrite32 (PERI_CTRL_BASE + 0x018, 0x70533483); //EYE_PATTERN

         MicroSecondDelay (5000);
  } else {
         DEBUG ((EFI_D_ERROR, "Unknown USB Type set, only possible values 'device' and 'host'\n"));
  }
}

EFI_STATUS
HiKeyInitPeripherals (
  IN VOID
  )
{
  UINT32     Data, Bits;

  /* make I2C0/I2C1/I2C2/SPI0 out of reset */
  Bits = PERIPH_RST3_I2C0 | PERIPH_RST3_I2C1 | PERIPH_RST3_I2C2 | \
	 PERIPH_RST3_SSP;
  MmioWrite32 (SC_PERIPH_RSTDIS3, Bits);

  do {
    Data = MmioRead32 (SC_PERIPH_RSTSTAT3);
  } while (Data & Bits);

  UsbPhyInit ();

  return EFI_SUCCESS;
}
