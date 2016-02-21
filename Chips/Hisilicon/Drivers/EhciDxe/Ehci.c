/** @file  
  The Ehci controller driver.

  EhciDxe driver is responsible for managing the behavior of EHCI controller. 
  It implements the interfaces of monitoring the status of all ports and transferring 
  Control, Bulk, Interrupt and Isochronous requests to Usb2.0 device.

  Note that EhciDxe driver is enhanced to guarantee that the EHCI controller get attached
  to the EHCI controller before a UHCI or OHCI driver attaches to the companion UHCI or 
  OHCI controller.  This way avoids the control transfer on a shared port between EHCI 
  and companion host controller when UHCI or OHCI gets attached earlier than EHCI and a 
  USB 2.0 device inserts.

Copyright (c) 2006 - 2014, Intel Corporation. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

Based on the files under MdeModulePkg/Bus/Pci/EhciDxe/

**/


#include "Ehci.h"

//
// Two arrays used to translate the EHCI port state (change)
// to the UEFI protocol's port state (change).
//
USB_PORT_STATE_MAP  mUsbPortStateMap[] = {
  {PORTSC_CONN,     USB_PORT_STAT_CONNECTION},
  {PORTSC_ENABLED,  USB_PORT_STAT_ENABLE},
  {PORTSC_SUSPEND,  USB_PORT_STAT_SUSPEND},
  {PORTSC_OVERCUR,  USB_PORT_STAT_OVERCURRENT},
  {PORTSC_RESET,    USB_PORT_STAT_RESET},
  {PORTSC_POWER,    USB_PORT_STAT_POWER},
  {PORTSC_OWNER,    USB_PORT_STAT_OWNER}
};

USB_PORT_STATE_MAP  mUsbPortChangeMap[] = {
  {PORTSC_CONN_CHANGE,    USB_PORT_STAT_C_CONNECTION},
  {PORTSC_ENABLE_CHANGE,  USB_PORT_STAT_C_ENABLE},
  {PORTSC_OVERCUR_CHANGE, USB_PORT_STAT_C_OVERCURRENT}
};

EFI_DRIVER_BINDING_PROTOCOL
gEhciDriverBinding = {
  EhcDriverBindingSupported,
  EhcDriverBindingStart,
  EhcDriverBindingStop,
  0x30,
  NULL,
  NULL
};

EFI_HANDLE      gGlobalController = NULL;
BOOLEAN         gEhciAleadyInit = FALSE;

EFI_CPU_ARCH_PROTOCOL      *gCpu;

typedef struct {
  EFI_PHYSICAL_ADDRESS              HostAddress;
  EFI_PHYSICAL_ADDRESS              DeviceAddress;
  UINTN                             NumberOfBytes;
  EFI_PCI_IO_PROTOCOL_OPERATION     Operation;
  BOOLEAN                           DoubleBuffer;
} MEM_MAP_INFO_INSTANCE;

VOID *
UncachedAllocatePagesBelow4G(
  IN UINTN  Pages
  ) 
{
  // Work around
#if 0
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  PhysicalAddress;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR   Descriptor;

  if (Pages == 0) {
    return NULL;
  }

  PhysicalAddress = 0xffffffff;
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  EfiBootServicesData,
                  Pages,
                  &PhysicalAddress
                  );
  
  if (EFI_ERROR (Status)) {    
    return NULL;
  }
  
  Status = gDS->GetMemorySpaceDescriptor (PhysicalAddress, &Descriptor);
  if (!EFI_ERROR (Status)) {
    // We are making an assumption that all of memory has the same default attributes
    //gAttributes = Descriptor.Attributes;
  }

  Status = gDS->SetMemorySpaceAttributes (PhysicalAddress, EFI_PAGES_TO_SIZE (Pages), EFI_MEMORY_WC);
  if (EFI_ERROR (Status)) {    
    return NULL;
  }
  
  (VOID)WriteBackInvalidateDataCacheRange((VOID *)(UINTN)PhysicalAddress, EFI_PAGES_TO_SIZE(Pages));
  
  return  (VOID *)(UINTN)PhysicalAddress;
#else
  return UncachedAllocatePages (Pages);
#endif
}

EFI_STATUS
EFIAPI
MemMap (
  IN     EFI_PCI_IO_PROTOCOL            *This,
  IN     EFI_PCI_IO_PROTOCOL_OPERATION  Operation,
  IN     VOID                           *HostAddress,
  IN OUT UINTN                          *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS           *DeviceAddress,
  OUT    VOID                           **Mapping
  )
{
  EFI_STATUS                      Status;
  MEM_MAP_INFO_INSTANCE           *Map;
  VOID                            *Buffer;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR GcdDescriptor;
  
  if (HostAddress == NULL || NumberOfBytes == NULL || DeviceAddress == NULL || Mapping == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((UINT32)Operation >= EfiPciIoOperationMaximum) {
    return EFI_INVALID_PARAMETER;
  }

  *DeviceAddress = ConvertToPhysicalAddress (HostAddress);

  // Remember range so we can flush on the other side
  Map = AllocatePool (sizeof (MEM_MAP_INFO_INSTANCE));
  if (Map == NULL) {
    return  EFI_OUT_OF_RESOURCES;
  }  

  *Mapping = Map;

  if ((((UINTN)HostAddress & (EFI_PAGE_SIZE - 1)) != 0) ||
      ((*NumberOfBytes % EFI_PAGE_SIZE) != 0)) {
    
    // Get the cacheability of the region
    Status = gDS->GetMemorySpaceDescriptor (*DeviceAddress, &GcdDescriptor);    
    if (EFI_ERROR(Status)) {
      return Status;
    }
    
    // If the mapped buffer is not an uncached buffer
    if ( (GcdDescriptor.Attributes != EFI_MEMORY_WC) &&
         (GcdDescriptor.Attributes != EFI_MEMORY_UC) )
    {        
      //
      // If the buffer does not fill entire cache lines we must double buffer into
      // uncached memory. Device (PCI) address becomes uncached page.
      //
      Map->DoubleBuffer  = TRUE;
      Buffer = UncachedAllocatePagesBelow4G(EFI_SIZE_TO_PAGES (*NumberOfBytes));
      
      if (Buffer == NULL) {
        return EFI_OUT_OF_RESOURCES;
      }

      (VOID)CopyMem(Buffer, HostAddress, *NumberOfBytes);

      *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)Buffer;
    } else {
      Map->DoubleBuffer  = FALSE;
    }
  } else {
    Map->DoubleBuffer  = FALSE;

    // Flush the Data Cache (should not have any effect if the memory region is uncached)
    gCpu->FlushDataCache (gCpu, *DeviceAddress, *NumberOfBytes, EfiCpuFlushTypeWriteBackInvalidate);

    Status = gDS->SetMemorySpaceAttributes (*DeviceAddress & ~(BASE_4KB - 1), ALIGN_VALUE (*NumberOfBytes, BASE_4KB), EFI_MEMORY_WC);
    //ASSERT_EFI_ERROR (Status);
    if (EFI_ERROR (Status)) {    
      DEBUG((EFI_D_ERROR, "[%a]:[%dL] SetMemorySpaceAttributes Fail. %r\n", __FUNCTION__, __LINE__, Status));
    }
  }

  Map->HostAddress   = (UINTN)HostAddress;
  Map->DeviceAddress = *DeviceAddress;
  Map->NumberOfBytes = *NumberOfBytes;
  Map->Operation     = Operation;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MemUnmap (
  IN  VOID                         *Mapping
  )
{
  MEM_MAP_INFO_INSTANCE *Map;

  if (Mapping == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Map = (MEM_MAP_INFO_INSTANCE *)Mapping;

  if (Map->DoubleBuffer) { 
    if ((Map->Operation == EfiPciIoOperationBusMasterWrite) || (Map->Operation == EfiPciIoOperationBusMasterCommonBuffer)) {
      (VOID)CopyMem((VOID *)(UINTN)Map->HostAddress, (VOID *)(UINTN)Map->DeviceAddress, Map->NumberOfBytes);   
    }

    if((VOID *)(UINTN)Map->DeviceAddress != NULL) {
      UncachedFreePages ((VOID *)(UINTN)Map->DeviceAddress, EFI_SIZE_TO_PAGES (Map->NumberOfBytes));
    }
    

  } else {
    if (Map->Operation == EfiPciIoOperationBusMasterWrite) {
      //
      // Make sure we read buffer from uncached memory and not the cache
      //
      gCpu->FlushDataCache (gCpu, Map->HostAddress, Map->NumberOfBytes, EfiCpuFlushTypeInvalidate);
    }
  }

  FreePool (Map);

  return EFI_SUCCESS;
}


/**
  Retrieves the capability of root hub ports.

  @param  This                  This EFI_USB_HC_PROTOCOL instance.
  @param  MaxSpeed              Max speed supported by the controller.
  @param  PortNumber            Number of the root hub ports.
  @param  Is64BitCapable        Whether the controller supports 64-bit memory
                                addressing.

  @retval EFI_SUCCESS           Host controller capability were retrieved successfully.
  @retval EFI_INVALID_PARAMETER Either of the three capability pointer is NULL.

**/
EFI_STATUS
EFIAPI
EhcGetCapability (
  IN  EFI_USB2_HC_PROTOCOL  *This,
  OUT UINT8                 *MaxSpeed,
  OUT UINT8                 *PortNumber,
  OUT UINT8                 *Is64BitCapable
  )
{
  USB2_HC_DEV             *Ehc;
  EFI_TPL                 OldTpl;

  if ((MaxSpeed == NULL) || (PortNumber == NULL) || (Is64BitCapable == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl          = gBS->RaiseTPL (EHC_TPL);
  Ehc             = EHC_FROM_THIS (This);

  *MaxSpeed       = EFI_USB_SPEED_HIGH;
  *PortNumber     = (UINT8) (Ehc->HcStructParams & HCSP_NPORTS);
  *Is64BitCapable = (UINT8) (Ehc->HcCapParams & HCCP_64BIT);

  DEBUG ((EFI_D_ERROR, "EhcGetCapability: %d ports, 64 bit %d\n", *PortNumber, *Is64BitCapable));

  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}


/**
  Provides software reset for the USB host controller.

  @param  This                  This EFI_USB2_HC_PROTOCOL instance.
  @param  Attributes            A bit mask of the reset operation to perform.

  @retval EFI_SUCCESS           The reset operation succeeded.
  @retval EFI_INVALID_PARAMETER Attributes is not valid.
  @retval EFI_UNSUPPOURTED      The type of reset specified by Attributes is
                                not currently supported by the host controller.
  @retval EFI_DEVICE_ERROR      Host controller isn't halted to reset.

**/
EFI_STATUS
EFIAPI
EhcReset (
  IN EFI_USB2_HC_PROTOCOL *This,
  IN UINT16               Attributes
  )
{
  USB2_HC_DEV             *Ehc;
  EFI_TPL                 OldTpl;
  EFI_STATUS              Status;
  UINT32                  DbgCtrlStatus;

  Ehc = EHC_FROM_THIS (This);

  if (Ehc->DevicePath != NULL) {
    //
    // Report Status Code to indicate reset happens
    //
    REPORT_STATUS_CODE_WITH_DEVICE_PATH (
      EFI_PROGRESS_CODE,
      (EFI_IO_BUS_USB | EFI_IOB_PC_RESET),
      Ehc->DevicePath
      );
  }

  OldTpl  = gBS->RaiseTPL (EHC_TPL);

  switch (Attributes) {
  case EFI_USB_HC_RESET_GLOBAL:
  //
  // Flow through, same behavior as Host Controller Reset
  //
  case EFI_USB_HC_RESET_HOST_CONTROLLER:
    //
    // Host Controller must be Halt when Reset it
    //
    if (Ehc->DebugPortNum != 0) {
      DbgCtrlStatus = EhcReadDbgRegister(Ehc, 0);
      if ((DbgCtrlStatus & (USB_DEBUG_PORT_IN_USE | USB_DEBUG_PORT_OWNER)) == (USB_DEBUG_PORT_IN_USE | USB_DEBUG_PORT_OWNER)) {
        Status = EFI_SUCCESS;
        goto ON_EXIT;
      }
    }

    if (!EhcIsHalt (Ehc)) {
      Status = EhcHaltHC (Ehc, EHC_GENERIC_TIMEOUT);

      if (EFI_ERROR (Status)) {
        Status = EFI_DEVICE_ERROR;
        goto ON_EXIT;
      }
    }

    //
    // Clean up the asynchronous transfers, currently only
    // interrupt supports asynchronous operation.
    //
    EhciDelAllAsyncIntTransfers (Ehc);
    EhcAckAllInterrupt (Ehc);
    EhcFreeSched (Ehc);

    Status = EhcResetHC (Ehc, EHC_RESET_TIMEOUT);

    if (EFI_ERROR (Status)) {
      goto ON_EXIT;
    }

    Status = EhcInitHC (Ehc);
    break;

  case EFI_USB_HC_RESET_GLOBAL_WITH_DEBUG:
  case EFI_USB_HC_RESET_HOST_WITH_DEBUG:
    Status = EFI_UNSUPPORTED;
    break;

  default:
    Status = EFI_INVALID_PARAMETER;
  }

ON_EXIT:
  DEBUG ((EFI_D_INFO, "EhcReset: exit status %r\n", Status));
  gBS->RestoreTPL (OldTpl);
  return Status;
}


/**
  Retrieve the current state of the USB host controller.

  @param  This                   This EFI_USB2_HC_PROTOCOL instance.
  @param  State                  Variable to return the current host controller
                                 state.

  @retval EFI_SUCCESS            Host controller state was returned in State.
  @retval EFI_INVALID_PARAMETER  State is NULL.
  @retval EFI_DEVICE_ERROR       An error was encountered while attempting to
                                 retrieve the host controller's current state.

**/
EFI_STATUS
EFIAPI
EhcGetState (
  IN   EFI_USB2_HC_PROTOCOL  *This,
  OUT  EFI_USB_HC_STATE      *State
  )
{
  EFI_TPL                 OldTpl;
  USB2_HC_DEV             *Ehc;

  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl  = gBS->RaiseTPL (EHC_TPL);
  Ehc     = EHC_FROM_THIS (This);

  if (EHC_REG_BIT_IS_SET (Ehc, EHC_USBSTS_OFFSET, USBSTS_HALT)) {
    *State = EfiUsbHcStateHalt;
  } else {
    *State = EfiUsbHcStateOperational;
  }

  gBS->RestoreTPL (OldTpl);

  DEBUG ((EFI_D_ERROR, "EhcGetState: current state %d\n", *State));
  return EFI_SUCCESS;
}


/**
  Sets the USB host controller to a specific state.

  @param  This                  This EFI_USB2_HC_PROTOCOL instance.
  @param  State                 The state of the host controller that will be set.

  @retval EFI_SUCCESS           The USB host controller was successfully placed
                                in the state specified by State.
  @retval EFI_INVALID_PARAMETER State is invalid.
  @retval EFI_DEVICE_ERROR      Failed to set the state due to device error.

**/
EFI_STATUS
EFIAPI
EhcSetState (
  IN EFI_USB2_HC_PROTOCOL *This,
  IN EFI_USB_HC_STATE     State
  )
{
  USB2_HC_DEV             *Ehc;
  EFI_TPL                 OldTpl;
  EFI_STATUS              Status;
  EFI_USB_HC_STATE        CurState;

  Status = EhcGetState (This, &CurState);

  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  if (CurState == State) {
    return EFI_SUCCESS;
  }

  OldTpl  = gBS->RaiseTPL (EHC_TPL);
  Ehc     = EHC_FROM_THIS (This);

  switch (State) {
  case EfiUsbHcStateHalt:
    Status = EhcHaltHC (Ehc, EHC_GENERIC_TIMEOUT);
    break;

  case EfiUsbHcStateOperational:
    if (EHC_REG_BIT_IS_SET (Ehc, EHC_USBSTS_OFFSET, USBSTS_SYS_ERROR)) {
      Status = EFI_DEVICE_ERROR;
      break;
    }

    //
    // Software must not write a one to this field unless the host controller
    // is in the Halted state. Doing so will yield undefined results.
    // refers to Spec[EHCI1.0-2.3.1]
    //
    if (!EHC_REG_BIT_IS_SET (Ehc, EHC_USBSTS_OFFSET, USBSTS_HALT)) {
      Status = EFI_DEVICE_ERROR;
      break;
    }

    Status = EhcRunHC (Ehc, EHC_GENERIC_TIMEOUT);
    break;

  case EfiUsbHcStateSuspend:
    Status = EFI_UNSUPPORTED;
    break;

  default:
    Status = EFI_INVALID_PARAMETER;
  }

  DEBUG ((EFI_D_INFO, "EhcSetState: exit status %r\n", Status));
  gBS->RestoreTPL (OldTpl);
  return Status;
}


/**
  Retrieves the current status of a USB root hub port.

  @param  This                  This EFI_USB2_HC_PROTOCOL instance.
  @param  PortNumber            The root hub port to retrieve the state from.
                                This value is zero-based.
  @param  PortStatus            Variable to receive the port state.

  @retval EFI_SUCCESS           The status of the USB root hub port specified.
                                by PortNumber was returned in PortStatus.
  @retval EFI_INVALID_PARAMETER PortNumber is invalid.
  @retval EFI_DEVICE_ERROR      Can't read register.

**/
EFI_STATUS
EFIAPI
EhcGetRootHubPortStatus (
  IN   EFI_USB2_HC_PROTOCOL  *This,
  IN   UINT8                 PortNumber,
  OUT  EFI_USB_PORT_STATUS   *PortStatus
  )
{
  USB2_HC_DEV             *Ehc;
  EFI_TPL                 OldTpl;
  UINT32                  Offset;
  UINT32                  State;
  UINT32                  TotalPort;
  UINTN                   Index;
  UINTN                   MapSize;
  EFI_STATUS              Status;
  UINT32                  DbgCtrlStatus;

  if (PortStatus == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl    = gBS->RaiseTPL (EHC_TPL);

  Ehc       = EHC_FROM_THIS (This);
  Status    = EFI_SUCCESS;

  TotalPort = (Ehc->HcStructParams & HCSP_NPORTS);

  if (PortNumber >= TotalPort) {
    Status = EFI_INVALID_PARAMETER;
    goto ON_EXIT;
  }

  Offset                        = (UINT32) (EHC_PORT_STAT_OFFSET + (4 * PortNumber));
  PortStatus->PortStatus        = 0;
  PortStatus->PortChangeStatus  = 0;

  if ((Ehc->DebugPortNum != 0) && (PortNumber == (Ehc->DebugPortNum - 1))) {
    DbgCtrlStatus = EhcReadDbgRegister(Ehc, 0);
    if ((DbgCtrlStatus & (USB_DEBUG_PORT_IN_USE | USB_DEBUG_PORT_OWNER)) == (USB_DEBUG_PORT_IN_USE | USB_DEBUG_PORT_OWNER)) {
      goto ON_EXIT;
    }
  }

  State                         = EhcReadOpReg (Ehc, Offset);

  //
  // Identify device speed. If in K state, it is low speed.
  // If the port is enabled after reset, the device is of
  // high speed. The USB bus driver should retrieve the actual
  // port speed after reset.
  //
  if (EHC_BIT_IS_SET (State, PORTSC_LINESTATE_K)) {
    PortStatus->PortStatus |= USB_PORT_STAT_LOW_SPEED;

  } else if (EHC_BIT_IS_SET (State, PORTSC_ENABLED)) {
    PortStatus->PortStatus |= USB_PORT_STAT_HIGH_SPEED;
  }

  //
  // Convert the EHCI port/port change state to UEFI status
  //
  MapSize = sizeof (mUsbPortStateMap) / sizeof (USB_PORT_STATE_MAP);

  for (Index = 0; Index < MapSize; Index++) {
    if (EHC_BIT_IS_SET (State, mUsbPortStateMap[Index].HwState)) {
      PortStatus->PortStatus = (UINT16) (PortStatus->PortStatus | mUsbPortStateMap[Index].UefiState);
    }
  }

  MapSize = sizeof (mUsbPortChangeMap) / sizeof (USB_PORT_STATE_MAP);

  for (Index = 0; Index < MapSize; Index++) {
    if (EHC_BIT_IS_SET (State, mUsbPortChangeMap[Index].HwState)) {
      PortStatus->PortChangeStatus = (UINT16) (PortStatus->PortChangeStatus | mUsbPortChangeMap[Index].UefiState);
    }
  }

ON_EXIT:
  gBS->RestoreTPL (OldTpl);
  return Status;
}


/**
  Sets a feature for the specified root hub port.

  @param  This                  This EFI_USB2_HC_PROTOCOL instance.
  @param  PortNumber            Root hub port to set.
  @param  PortFeature           Feature to set.

  @retval EFI_SUCCESS           The feature specified by PortFeature was set.
  @retval EFI_INVALID_PARAMETER PortNumber is invalid or PortFeature is invalid.
  @retval EFI_DEVICE_ERROR      Can't read register.

**/
EFI_STATUS
EFIAPI
EhcSetRootHubPortFeature (
  IN  EFI_USB2_HC_PROTOCOL  *This,
  IN  UINT8                 PortNumber,
  IN  EFI_USB_PORT_FEATURE  PortFeature
  )
{
  USB2_HC_DEV             *Ehc;
  EFI_TPL                 OldTpl;
  UINT32                  Offset;
  UINT32                  State;
  UINT32                  TotalPort;
  EFI_STATUS              Status;

  OldTpl    = gBS->RaiseTPL (EHC_TPL);
  Ehc       = EHC_FROM_THIS (This);
  Status    = EFI_SUCCESS;

  TotalPort = (Ehc->HcStructParams & HCSP_NPORTS);

  if (PortNumber >= TotalPort) {
    Status = EFI_INVALID_PARAMETER;
    goto ON_EXIT;
  }

  Offset  = (UINT32) (EHC_PORT_STAT_OFFSET + (4 * PortNumber));
  State   = EhcReadOpReg (Ehc, Offset);

  //
  // Mask off the port status change bits, these bits are
  // write clean bit
  //
  State &= ~PORTSC_CHANGE_MASK;

  switch (PortFeature) {
  case EfiUsbPortEnable:
    //
    // Sofeware can't set this bit, Port can only be enable by
    // EHCI as a part of the reset and enable
    //
    State |= PORTSC_ENABLED;
    EhcWriteOpReg (Ehc, Offset, State);
    break;

  case EfiUsbPortSuspend:
    State |= PORTSC_SUSPEND;
    EhcWriteOpReg (Ehc, Offset, State);
    break;

  case EfiUsbPortReset:
    //
    // Make sure Host Controller not halt before reset it
    //
    if (EhcIsHalt (Ehc)) {
      Status = EhcRunHC (Ehc, EHC_GENERIC_TIMEOUT);

      if (EFI_ERROR (Status)) {
        DEBUG ((EFI_D_ERROR, "EhcSetRootHubPortFeature :failed to start HC - %r\n", Status));
        break;
      }
    }

    //
    // Set one to PortReset bit must also set zero to PortEnable bit
    //
    State |= PORTSC_RESET;
    State &= ~PORTSC_ENABLED;
    EhcWriteOpReg (Ehc, Offset, State);
    break;

  case EfiUsbPortPower:
    //
    // Set port power bit when PPC is 1
    //
    if ((Ehc->HcCapParams & HCSP_PPC) == HCSP_PPC) {
      State |= PORTSC_POWER;
      EhcWriteOpReg (Ehc, Offset, State);
    }
    break;

  case EfiUsbPortOwner:
    State |= PORTSC_OWNER;
    EhcWriteOpReg (Ehc, Offset, State);
    break;

  default:
    Status = EFI_INVALID_PARAMETER;
  }

ON_EXIT:
  DEBUG ((EFI_D_INFO, "EhcSetRootHubPortFeature: exit status %r\n", Status));

  gBS->RestoreTPL (OldTpl);
  return Status;
}


/**
  Clears a feature for the specified root hub port.

  @param  This                  A pointer to the EFI_USB2_HC_PROTOCOL instance.
  @param  PortNumber            Specifies the root hub port whose feature is
                                requested to be cleared.
  @param  PortFeature           Indicates the feature selector associated with the
                                feature clear request.

  @retval EFI_SUCCESS           The feature specified by PortFeature was cleared
                                for the USB root hub port specified by PortNumber.
  @retval EFI_INVALID_PARAMETER PortNumber is invalid or PortFeature is invalid.
  @retval EFI_DEVICE_ERROR      Can't read register.

**/
EFI_STATUS
EFIAPI
EhcClearRootHubPortFeature (
  IN  EFI_USB2_HC_PROTOCOL  *This,
  IN  UINT8                 PortNumber,
  IN  EFI_USB_PORT_FEATURE  PortFeature
  )
{
  USB2_HC_DEV             *Ehc;
  EFI_TPL                 OldTpl;
  UINT32                  Offset;
  UINT32                  State;
  UINT32                  TotalPort;
  EFI_STATUS              Status;

  OldTpl    = gBS->RaiseTPL (EHC_TPL);
  Ehc       = EHC_FROM_THIS (This);
  Status    = EFI_SUCCESS;

  TotalPort = (Ehc->HcStructParams & HCSP_NPORTS);

  if (PortNumber >= TotalPort) {
    Status = EFI_INVALID_PARAMETER;
    goto ON_EXIT;
  }

  Offset  = EHC_PORT_STAT_OFFSET + (4 * PortNumber);
  State   = EhcReadOpReg (Ehc, Offset);
  State &= ~PORTSC_CHANGE_MASK;

  switch (PortFeature) {
  case EfiUsbPortEnable:
    //
    // Clear PORT_ENABLE feature means disable port.
    //
    State &= ~PORTSC_ENABLED;
    EhcWriteOpReg (Ehc, Offset, State);
    break;

  case EfiUsbPortSuspend:
    //
    // A write of zero to this bit is ignored by the host
    // controller. The host controller will unconditionally
    // set this bit to a zero when:
    //   1. software sets the Forct Port Resume bit to a zero from a one.
    //   2. software sets the Port Reset bit to a one frome a zero.
    //
    State &= ~PORSTSC_RESUME;
    EhcWriteOpReg (Ehc, Offset, State);
    break;

  case EfiUsbPortReset:
    //
    // Clear PORT_RESET means clear the reset signal.
    //
    State &= ~PORTSC_RESET;
    EhcWriteOpReg (Ehc, Offset, State);
    break;

  case EfiUsbPortOwner:
    //
    // Clear port owner means this port owned by EHC
    //
    State &= ~PORTSC_OWNER;
    EhcWriteOpReg (Ehc, Offset, State);
    break;

  case EfiUsbPortConnectChange:
    //
    // Clear connect status change
    //
    State |= PORTSC_CONN_CHANGE;
    EhcWriteOpReg (Ehc, Offset, State);
    break;

  case EfiUsbPortEnableChange:
    //
    // Clear enable status change
    //
    State |= PORTSC_ENABLE_CHANGE;
    EhcWriteOpReg (Ehc, Offset, State);
    break;

  case EfiUsbPortOverCurrentChange:
    //
    // Clear PortOverCurrent change
    //
    State |= PORTSC_OVERCUR_CHANGE;
    EhcWriteOpReg (Ehc, Offset, State);
    break;

  case EfiUsbPortPower:
    //
    // Clear port power bit when PPC is 1
    //
    if ((Ehc->HcCapParams & HCSP_PPC) == HCSP_PPC) {
      State &= ~PORTSC_POWER;
      EhcWriteOpReg (Ehc, Offset, State);
    }
    break;
  case EfiUsbPortSuspendChange:
  case EfiUsbPortResetChange:
    //
    // Not supported or not related operation
    //
    break;

  default:
    Status = EFI_INVALID_PARAMETER;
    break;
  }

ON_EXIT:
  DEBUG ((EFI_D_INFO, "EhcClearRootHubPortFeature: exit status %r\n", Status));
  gBS->RestoreTPL (OldTpl);
  return Status;
}


/**
  Submits control transfer to a target USB device.

  @param  This                  This EFI_USB2_HC_PROTOCOL instance.
  @param  DeviceAddress         The target device address.
  @param  DeviceSpeed           Target device speed.
  @param  MaximumPacketLength   Maximum packet size the default control transfer
                                endpoint is capable of sending or receiving.
  @param  Request               USB device request to send.
  @param  TransferDirection     Specifies the data direction for the data stage
  @param  Data                  Data buffer to be transmitted or received from USB
                                device.
  @param  DataLength            The size (in bytes) of the data buffer.
  @param  TimeOut               Indicates the maximum timeout, in millisecond.
  @param  Translator            Transaction translator to be used by this device.
  @param  TransferResult        Return the result of this control transfer.

  @retval EFI_SUCCESS           Transfer was completed successfully.
  @retval EFI_OUT_OF_RESOURCES  The transfer failed due to lack of resources.
  @retval EFI_INVALID_PARAMETER Some parameters are invalid.
  @retval EFI_TIMEOUT           Transfer failed due to timeout.
  @retval EFI_DEVICE_ERROR      Transfer failed due to host controller or device error.

**/
EFI_STATUS
EFIAPI
EhcControlTransfer (
  IN  EFI_USB2_HC_PROTOCOL                *This,
  IN  UINT8                               DeviceAddress,
  IN  UINT8                               DeviceSpeed,
  IN  UINTN                               MaximumPacketLength,
  IN  EFI_USB_DEVICE_REQUEST              *Request,
  IN  EFI_USB_DATA_DIRECTION              TransferDirection,
  IN  OUT VOID                            *Data,
  IN  OUT UINTN                           *DataLength,
  IN  UINTN                               TimeOut,
  IN  EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  OUT UINT32                              *TransferResult
  )
{
  USB2_HC_DEV             *Ehc;
  URB                     *Urb;
  EFI_TPL                 OldTpl;
  UINT8                   Endpoint;
  EFI_STATUS              Status;

  //
  // Validate parameters
  //
  if ((Request == NULL) || (TransferResult == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((TransferDirection != EfiUsbDataIn) &&
      (TransferDirection != EfiUsbDataOut) &&
      (TransferDirection != EfiUsbNoData)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((TransferDirection == EfiUsbNoData) &&
      ((Data != NULL) || (*DataLength != 0))) {
    return EFI_INVALID_PARAMETER;
  }

  if ((TransferDirection != EfiUsbNoData) &&
     ((Data == NULL) || (*DataLength == 0))) {
    return EFI_INVALID_PARAMETER;
  }

  if ((MaximumPacketLength != 8)  && (MaximumPacketLength != 16) &&
      (MaximumPacketLength != 32) && (MaximumPacketLength != 64)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((DeviceSpeed == EFI_USB_SPEED_LOW) && (MaximumPacketLength != 8)) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl          = gBS->RaiseTPL (EHC_TPL);
  Ehc             = EHC_FROM_THIS (This);

  Status          = EFI_DEVICE_ERROR;
  *TransferResult = EFI_USB_ERR_SYSTEM;

  if (EhcIsHalt (Ehc) || EhcIsSysError (Ehc)) {
    DEBUG ((EFI_D_ERROR, "EhcControlTransfer: HC halted at entrance\n"));

    EhcAckAllInterrupt (Ehc);
    goto ON_EXIT;
  }

  EhcAckAllInterrupt (Ehc);

  //
  // Create a new URB, insert it into the asynchronous
  // schedule list, then poll the execution status.
  //
  //
  // Encode the direction in address, although default control
  // endpoint is bidirectional. EhcCreateUrb expects this
  // combination of Ep addr and its direction.
  //
  Endpoint = (UINT8) (0 | ((TransferDirection == EfiUsbDataIn) ? 0x80 : 0));
  Urb = EhcCreateUrb (
          Ehc,
          DeviceAddress,
          Endpoint,
          DeviceSpeed,
          0,
          MaximumPacketLength,
          Translator,
          EHC_CTRL_TRANSFER,
          Request,
          Data,
          *DataLength,
          NULL,
          NULL,
          1
          );

  if (Urb == NULL) {
    DEBUG ((EFI_D_ERROR, "EhcControlTransfer: failed to create URB"));

    Status = EFI_OUT_OF_RESOURCES;
    goto ON_EXIT;
  }

  EhcLinkQhToAsync (Ehc, Urb->Qh);
  Status = EhcExecTransfer (Ehc, Urb, TimeOut);
  EhcUnlinkQhFromAsync (Ehc, Urb->Qh);

  //
  // Get the status from URB. The result is updated in EhcCheckUrbResult
  // which is called by EhcExecTransfer
  //
  *TransferResult = Urb->Result;
  *DataLength     = Urb->Completed;

  if (*TransferResult == EFI_USB_NOERROR) {
    Status = EFI_SUCCESS;
  }

  EhcAckAllInterrupt (Ehc);
  EhcFreeUrb (Ehc, Urb);

ON_EXIT:
  gBS->RestoreTPL (OldTpl);

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EhcControlTransfer: error - %r, transfer - %x\n", Status, *TransferResult));
  }

  return Status;
}


/**
  Submits bulk transfer to a bulk endpoint of a USB device.

  @param  This                  This EFI_USB2_HC_PROTOCOL instance.
  @param  DeviceAddress         Target device address.
  @param  EndPointAddress       Endpoint number and its direction in bit 7.
  @param  DeviceSpeed           Device speed, Low speed device doesn't support bulk
                                transfer.
  @param  MaximumPacketLength   Maximum packet size the endpoint is capable of
                                sending or receiving.
  @param  DataBuffersNumber     Number of data buffers prepared for the transfer.
  @param  Data                  Array of pointers to the buffers of data to transmit
                                from or receive into.
  @param  DataLength            The lenght of the data buffer.
  @param  DataToggle            On input, the initial data toggle for the transfer;
                                On output, it is updated to to next data toggle to
                                use of the subsequent bulk transfer.
  @param  TimeOut               Indicates the maximum time, in millisecond, which
                                the transfer is allowed to complete.
  @param  Translator            A pointr to the transaction translator data.
  @param  TransferResult        A pointer to the detailed result information of the
                                bulk transfer.

  @retval EFI_SUCCESS           The transfer was completed successfully.
  @retval EFI_OUT_OF_RESOURCES  The transfer failed due to lack of resource.
  @retval EFI_INVALID_PARAMETER Some parameters are invalid.
  @retval EFI_TIMEOUT           The transfer failed due to timeout.
  @retval EFI_DEVICE_ERROR      The transfer failed due to host controller error.

**/
EFI_STATUS
EFIAPI
EhcBulkTransfer (
  IN  EFI_USB2_HC_PROTOCOL                *This,
  IN  UINT8                               DeviceAddress,
  IN  UINT8                               EndPointAddress,
  IN  UINT8                               DeviceSpeed,
  IN  UINTN                               MaximumPacketLength,
  IN  UINT8                               DataBuffersNumber,
  IN  OUT VOID                            *Data[EFI_USB_MAX_BULK_BUFFER_NUM],
  IN  OUT UINTN                           *DataLength,
  IN  OUT UINT8                           *DataToggle,
  IN  UINTN                               TimeOut,
  IN  EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  OUT UINT32                              *TransferResult
  )
{
  USB2_HC_DEV             *Ehc;
  URB                     *Urb;
  EFI_TPL                 OldTpl;
  EFI_STATUS              Status;

  //
  // Validate the parameters
  //
  if ((DataLength == NULL) || (*DataLength == 0) ||
      (Data == NULL) || (Data[0] == NULL) || (TransferResult == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((*DataToggle != 0) && (*DataToggle != 1)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((DeviceSpeed == EFI_USB_SPEED_LOW) ||
      ((DeviceSpeed == EFI_USB_SPEED_FULL) && (MaximumPacketLength > 64)) ||
      ((EFI_USB_SPEED_HIGH == DeviceSpeed) && (MaximumPacketLength > 512))) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl          = gBS->RaiseTPL (EHC_TPL);
  Ehc             = EHC_FROM_THIS (This);

  *TransferResult = EFI_USB_ERR_SYSTEM;
  Status          = EFI_DEVICE_ERROR;

  if (EhcIsHalt (Ehc) || EhcIsSysError (Ehc)) {
    DEBUG ((EFI_D_ERROR, "EhcBulkTransfer: HC is halted\n"));

    EhcAckAllInterrupt (Ehc);
    goto ON_EXIT;
  }

  EhcAckAllInterrupt (Ehc);

  //
  // Create a new URB, insert it into the asynchronous
  // schedule list, then poll the execution status.
  //
  Urb = EhcCreateUrb (
          Ehc,
          DeviceAddress,
          EndPointAddress,
          DeviceSpeed,
          *DataToggle,
          MaximumPacketLength,
          Translator,
          EHC_BULK_TRANSFER,
          NULL,
          Data[0],
          *DataLength,
          NULL,
          NULL,
          1
          );

  if (Urb == NULL) {
    DEBUG ((EFI_D_ERROR, "EhcBulkTransfer: failed to create URB\n"));

    Status = EFI_OUT_OF_RESOURCES;
    goto ON_EXIT;
  }

  EhcLinkQhToAsync (Ehc, Urb->Qh);
  Status = EhcExecTransfer (Ehc, Urb, TimeOut);
  EhcUnlinkQhFromAsync (Ehc, Urb->Qh);

  *TransferResult = Urb->Result;
  *DataLength     = Urb->Completed;
  *DataToggle     = Urb->DataToggle;

  if (*TransferResult == EFI_USB_NOERROR) {
    Status = EFI_SUCCESS;
  }

  EhcAckAllInterrupt (Ehc);
  EhcFreeUrb (Ehc, Urb);

ON_EXIT:
  //Ehc->PciIo->Flush (Ehc->PciIo);
  gBS->RestoreTPL (OldTpl);

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EhcBulkTransfer: error - %r, transfer - %x\n", Status, *TransferResult));
  }

  return Status;
}


/**
  Submits an asynchronous interrupt transfer to an
  interrupt endpoint of a USB device.

  @param  This                  This EFI_USB2_HC_PROTOCOL instance.
  @param  DeviceAddress         Target device address.
  @param  EndPointAddress       Endpoint number and its direction encoded in bit 7
  @param  DeviceSpeed           Indicates device speed.
  @param  MaximumPacketLength   Maximum packet size the target endpoint is capable
  @param  IsNewTransfer         If TRUE, to submit an new asynchronous interrupt
                                transfer If FALSE, to remove the specified
                                asynchronous interrupt.
  @param  DataToggle            On input, the initial data toggle to use; on output,
                                it is updated to indicate the next data toggle.
  @param  PollingInterval       The he interval, in milliseconds, that the transfer
                                is polled.
  @param  DataLength            The length of data to receive at the rate specified
                                by  PollingInterval.
  @param  Translator            Transaction translator to use.
  @param  CallBackFunction      Function to call at the rate specified by
                                PollingInterval.
  @param  Context               Context to CallBackFunction.

  @retval EFI_SUCCESS           The request has been successfully submitted or canceled.
  @retval EFI_INVALID_PARAMETER Some parameters are invalid.
  @retval EFI_OUT_OF_RESOURCES  The request failed due to a lack of resources.
  @retval EFI_DEVICE_ERROR      The transfer failed due to host controller error.

**/
EFI_STATUS
EFIAPI
EhcAsyncInterruptTransfer (
  IN  EFI_USB2_HC_PROTOCOL                  * This,
  IN  UINT8                                 DeviceAddress,
  IN  UINT8                                 EndPointAddress,
  IN  UINT8                                 DeviceSpeed,
  IN  UINTN                                 MaximumPacketLength,
  IN  BOOLEAN                               IsNewTransfer,
  IN  OUT UINT8                             *DataToggle,
  IN  UINTN                                 PollingInterval,
  IN  UINTN                                 DataLength,
  IN  EFI_USB2_HC_TRANSACTION_TRANSLATOR    * Translator,
  IN  EFI_ASYNC_USB_TRANSFER_CALLBACK       CallBackFunction,
  IN  VOID                                  *Context OPTIONAL
  )
{
  USB2_HC_DEV             *Ehc;
  URB                     *Urb;
  EFI_TPL                 OldTpl;
  EFI_STATUS              Status;
  UINT8                   *Data;

  //
  // Validate parameters
  //
  if (!EHCI_IS_DATAIN (EndPointAddress)) {
    return EFI_INVALID_PARAMETER;
  }

  if (IsNewTransfer) {
    if (DataLength == 0) {
      return EFI_INVALID_PARAMETER;
    }

    if ((*DataToggle != 1) && (*DataToggle != 0)) {
      return EFI_INVALID_PARAMETER;
    }

    if ((PollingInterval > 255) || (PollingInterval < 1)) {
      return EFI_INVALID_PARAMETER;
    }
  }

  OldTpl  = gBS->RaiseTPL (EHC_TPL);
  Ehc     = EHC_FROM_THIS (This);

  //
  // Delete Async interrupt transfer request. DataToggle will return
  // the next data toggle to use.
  //
  if (!IsNewTransfer) {
    Status = EhciDelAsyncIntTransfer (Ehc, DeviceAddress, EndPointAddress, DataToggle);

    DEBUG ((EFI_D_ERROR, "EhcAsyncInterruptTransfer: remove old transfer - %r\n", Status));
    goto ON_EXIT;
  }

  Status = EFI_SUCCESS;

  if (EhcIsHalt (Ehc) || EhcIsSysError (Ehc)) {
    DEBUG ((EFI_D_ERROR, "EhcAsyncInterruptTransfer: HC is halt\n"));
    EhcAckAllInterrupt (Ehc);

    Status = EFI_DEVICE_ERROR;
    goto ON_EXIT;
  }

  EhcAckAllInterrupt (Ehc);

  Data = AllocatePool (DataLength);

  if (Data == NULL) {
    DEBUG ((EFI_D_ERROR, "EhcAsyncInterruptTransfer: failed to allocate buffer\n"));

    Status = EFI_OUT_OF_RESOURCES;
    goto ON_EXIT;
  }

  Urb = EhcCreateUrb (
          Ehc,
          DeviceAddress,
          EndPointAddress,
          DeviceSpeed,
          *DataToggle,
          MaximumPacketLength,
          Translator,
          EHC_INT_TRANSFER_ASYNC,
          NULL,
          Data,
          DataLength,
          CallBackFunction,
          Context,
          PollingInterval
          );

  if (Urb == NULL) {
    DEBUG ((EFI_D_ERROR, "EhcAsyncInterruptTransfer: failed to create URB\n"));

    gBS->FreePool (Data);
    Status = EFI_OUT_OF_RESOURCES;
    goto ON_EXIT;
  }

  //
  // New asynchronous transfer must inserted to the head.
  // Check the comments in EhcMoniteAsyncRequests
  //
  EhcLinkQhToPeriod (Ehc, Urb->Qh);
  InsertHeadList (&Ehc->AsyncIntTransfers, &Urb->UrbList);

ON_EXIT:
  //Ehc->PciIo->Flush (Ehc->PciIo);
  gBS->RestoreTPL (OldTpl);

  return Status;
}


/**
  Submits synchronous interrupt transfer to an interrupt endpoint
  of a USB device.

  @param  This                  This EFI_USB2_HC_PROTOCOL instance.
  @param  DeviceAddress         Target device address.
  @param  EndPointAddress       Endpoint number and its direction encoded in bit 7
  @param  DeviceSpeed           Indicates device speed.
  @param  MaximumPacketLength   Maximum packet size the target endpoint is capable
                                of sending or receiving.
  @param  Data                  Buffer of data that will be transmitted to  USB
                                device or received from USB device.
  @param  DataLength            On input, the size, in bytes, of the data buffer; On
                                output, the number of bytes transferred.
  @param  DataToggle            On input, the initial data toggle to use; on output,
                                it is updated to indicate the next data toggle.
  @param  TimeOut               Maximum time, in second, to complete.
  @param  Translator            Transaction translator to use.
  @param  TransferResult        Variable to receive the transfer result.

  @return EFI_SUCCESS           The transfer was completed successfully.
  @return EFI_OUT_OF_RESOURCES  The transfer failed due to lack of resource.
  @return EFI_INVALID_PARAMETER Some parameters are invalid.
  @return EFI_TIMEOUT           The transfer failed due to timeout.
  @return EFI_DEVICE_ERROR      The failed due to host controller or device error

**/
EFI_STATUS
EFIAPI
EhcSyncInterruptTransfer (
  IN  EFI_USB2_HC_PROTOCOL                *This,
  IN  UINT8                               DeviceAddress,
  IN  UINT8                               EndPointAddress,
  IN  UINT8                               DeviceSpeed,
  IN  UINTN                               MaximumPacketLength,
  IN  OUT VOID                            *Data,
  IN  OUT UINTN                           *DataLength,
  IN  OUT UINT8                           *DataToggle,
  IN  UINTN                               TimeOut,
  IN  EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  OUT UINT32                              *TransferResult
  )
{
  USB2_HC_DEV             *Ehc;
  EFI_TPL                 OldTpl;
  URB                     *Urb;
  EFI_STATUS              Status;

  //
  // Validates parameters
  //
  if ((DataLength == NULL) || (*DataLength == 0) ||
      (Data == NULL) || (TransferResult == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!EHCI_IS_DATAIN (EndPointAddress)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((*DataToggle != 1) && (*DataToggle != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (((DeviceSpeed == EFI_USB_SPEED_LOW) && (MaximumPacketLength != 8))  ||
      ((DeviceSpeed == EFI_USB_SPEED_FULL) && (MaximumPacketLength > 64)) ||
      ((DeviceSpeed == EFI_USB_SPEED_HIGH) && (MaximumPacketLength > 3072))) {
    return EFI_INVALID_PARAMETER;
  }

  OldTpl          = gBS->RaiseTPL (EHC_TPL);
  Ehc             = EHC_FROM_THIS (This);

  *TransferResult = EFI_USB_ERR_SYSTEM;
  Status          = EFI_DEVICE_ERROR;

  if (EhcIsHalt (Ehc) || EhcIsSysError (Ehc)) {
    DEBUG ((EFI_D_ERROR, "EhcSyncInterruptTransfer: HC is halt\n"));

    EhcAckAllInterrupt (Ehc);
    goto ON_EXIT;
  }

  EhcAckAllInterrupt (Ehc);

  Urb = EhcCreateUrb (
          Ehc,
          DeviceAddress,
          EndPointAddress,
          DeviceSpeed,
          *DataToggle,
          MaximumPacketLength,
          Translator,
          EHC_INT_TRANSFER_SYNC,
          NULL,
          Data,
          *DataLength,
          NULL,
          NULL,
          1
          );

  if (Urb == NULL) {
    DEBUG ((EFI_D_ERROR, "EhcSyncInterruptTransfer: failed to create URB\n"));

    Status = EFI_OUT_OF_RESOURCES;
    goto ON_EXIT;
  }

  EhcLinkQhToPeriod (Ehc, Urb->Qh);
  Status = EhcExecTransfer (Ehc, Urb, TimeOut);
  EhcUnlinkQhFromPeriod (Ehc, Urb->Qh);

  *TransferResult = Urb->Result;
  *DataLength     = Urb->Completed;
  *DataToggle     = Urb->DataToggle;

  if (*TransferResult == EFI_USB_NOERROR) {
    Status = EFI_SUCCESS;
  }

ON_EXIT:
  //Ehc->PciIo->Flush (Ehc->PciIo);
  gBS->RestoreTPL (OldTpl);

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EhcSyncInterruptTransfer: error - %r, transfer - %x\n", Status, *TransferResult));
  }

  return Status;
}


/**
  Submits isochronous transfer to a target USB device.

  @param  This                 This EFI_USB2_HC_PROTOCOL instance.
  @param  DeviceAddress        Target device address.
  @param  EndPointAddress      End point address with its direction.
  @param  DeviceSpeed          Device speed, Low speed device doesn't support this
                               type.
  @param  MaximumPacketLength  Maximum packet size that the endpoint is capable of
                               sending or receiving.
  @param  DataBuffersNumber    Number of data buffers prepared for the transfer.
  @param  Data                 Array of pointers to the buffers of data that will
                               be transmitted to USB device or received from USB
                               device.
  @param  DataLength           The size, in bytes, of the data buffer.
  @param  Translator           Transaction translator to use.
  @param  TransferResult       Variable to receive the transfer result.

  @return EFI_UNSUPPORTED      Isochronous transfer is unsupported.

**/
EFI_STATUS
EFIAPI
EhcIsochronousTransfer (
  IN  EFI_USB2_HC_PROTOCOL                *This,
  IN  UINT8                               DeviceAddress,
  IN  UINT8                               EndPointAddress,
  IN  UINT8                               DeviceSpeed,
  IN  UINTN                               MaximumPacketLength,
  IN  UINT8                               DataBuffersNumber,
  IN  OUT VOID                            *Data[EFI_USB_MAX_ISO_BUFFER_NUM],
  IN  UINTN                               DataLength,
  IN  EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  OUT UINT32                              *TransferResult
  )
{
  return EFI_UNSUPPORTED;
}


/**
  Submits Async isochronous transfer to a target USB device.

  @param  This                 This EFI_USB2_HC_PROTOCOL instance.
  @param  DeviceAddress        Target device address.
  @param  EndPointAddress      End point address with its direction.
  @param  DeviceSpeed          Device speed, Low speed device doesn't support this
                               type.
  @param  MaximumPacketLength  Maximum packet size that the endpoint is capable of
                               sending or receiving.
  @param  DataBuffersNumber    Number of data buffers prepared for the transfer.
  @param  Data                 Array of pointers to the buffers of data that will
                               be transmitted to USB device or received from USB
                               device.
  @param  DataLength           The size, in bytes, of the data buffer.
  @param  Translator           Transaction translator to use.
  @param  IsochronousCallBack  Function to be called when the transfer complete.
  @param  Context              Context passed to the call back function as
                               parameter.

  @return EFI_UNSUPPORTED      Isochronous transfer isn't supported.

**/
EFI_STATUS
EFIAPI
EhcAsyncIsochronousTransfer (
  IN  EFI_USB2_HC_PROTOCOL                *This,
  IN  UINT8                               DeviceAddress,
  IN  UINT8                               EndPointAddress,
  IN  UINT8                               DeviceSpeed,
  IN  UINTN                               MaximumPacketLength,
  IN  UINT8                               DataBuffersNumber,
  IN  OUT VOID                            *Data[EFI_USB_MAX_ISO_BUFFER_NUM],
  IN  UINTN                               DataLength,
  IN  EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
  IN  EFI_ASYNC_USB_TRANSFER_CALLBACK     IsochronousCallBack,
  IN  VOID                                *Context
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Entry point for EFI drivers.

  @param  ImageHandle       EFI_HANDLE.
  @param  SystemTable       EFI_SYSTEM_TABLE.

  @return EFI_SUCCESS       Success.
          EFI_DEVICE_ERROR  Fail.

**/
EFI_STATUS
EFIAPI
EhcDriverEntryPoint (
  IN EFI_HANDLE           ImageHandle,
  IN EFI_SYSTEM_TABLE     *SystemTable
  )
{
  EFI_STATUS                Status;
  EFI_DEV_PATH              EndNode;
  EFI_DEV_PATH              Node;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath = NULL;  

  DResetUsb ();
  MicroSecondDelay(1000);

  // Get the Cpu protocol for later use
  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&gCpu);
  if(EFI_ERROR(Status))
  {
      DEBUG((EFI_D_ERROR, "[%a]:[%dL] LocateProtocol gEfiCpuArchProtocolGuid. %r\n", __FUNCTION__, __LINE__, Status));
  }
 
  //
  // Install driver model protocol(s).
  //
  Status = EfiLibInstallDriverBindingComponentName2 (
             ImageHandle,
             SystemTable,
             &gEhciDriverBinding,
             ImageHandle,
             &gEhciComponentName,
             &gEhciComponentName2
             );
  if(EFI_ERROR(Status))
  {
      DEBUG((EFI_D_ERROR, "[%a]:[%dL] InstallProtocolInterface fail. %r\n", __FUNCTION__, __LINE__, Status));
  }

  (void)ZeroMem (&Node, sizeof (Node));
  Node.DevPath.Type = HARDWARE_DEVICE_PATH;
  Node.DevPath.SubType = HW_PCI_DP;
  (void)SetDevicePathNodeLength (&Node.DevPath, sizeof (PCI_DEVICE_PATH));
  // Make USB controller device path different from built-in SATA controller
  Node.Pci.Function = 1;
  Node.Pci.Device = 0;

  SetDevicePathEndNode (&EndNode.DevPath);
  
  DevicePath = AppendDevicePathNode (&EndNode.DevPath, &Node.DevPath);

  Status = gBS->InstallProtocolInterface (
                  &gGlobalController,
                  &gEfiDevicePathProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  DevicePath
                  );  
  if(EFI_ERROR(Status))
  {
      DEBUG((EFI_D_ERROR, "[%a]:[%dL] InstallProtocolInterface fail. %r\n", __FUNCTION__, __LINE__, Status));
  }

  return Status;
}


/**
  Test to see if this driver supports ControllerHandle. Any
  ControllerHandle that has Usb2HcProtocol installed will
  be supported.

  @param  This                 Protocol instance pointer.
  @param  Controller           Handle of device to test.
  @param  RemainingDevicePath  Not used.

  @return EFI_SUCCESS          This driver supports this device.
  @return EFI_UNSUPPORTED      This driver does not support this device.

**/
EFI_STATUS
EFIAPI
EhcDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  Controller,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath
  )
{
  EFI_STATUS                    Status;
  EFI_DEVICE_PATH_PROTOCOL      *ParentDevicePath;

  if(gGlobalController != Controller) {
    return EFI_UNSUPPORTED;
  }

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID *) &ParentDevicePath,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    //
    // EFI_ALREADY_STARTED is also an error
    //
    return Status;
  }
  //
  // Close the protocol because we don't use it here
  //
  gBS->CloseProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  This->DriverBindingHandle,
                  Controller
                  );

  return EFI_SUCCESS;
}

/**
  Get the usb debug port related information.

  @param  Ehc                The EHCI device.

  @retval RETURN_SUCCESS     Get debug port number, bar and offset successfully.
  @retval Others             The usb host controller does not supported usb debug port capability.

**/
EFI_STATUS
EhcGetUsbDebugPortInfo (
  IN  USB2_HC_DEV     *Ehc
 )
{
  UINT16              DebugPort;

  DebugPort = 0x2000;
  
  Ehc->DebugPortOffset = DebugPort & 0x1FFF;
  Ehc->DebugPortBarNum = (UINT8)((DebugPort >> 13) - 1);
  Ehc->DebugPortNum    = (UINT8)((Ehc->HcStructParams & 0x00F00000) >> 20);

  return EFI_SUCCESS;
}


/**
  Create and initialize a USB2_HC_DEV.

  @param  PciIo                  The PciIo on this device.
  @param  DevicePath             The device path of host controller.
  @param  OriginalPciAttributes  Original PCI attributes.

  @return  The allocated and initialized USB2_HC_DEV structure if created,
           otherwise NULL.

**/
USB2_HC_DEV *
EhcCreateUsb2Hc (
  IN EFI_PCI_IO_PROTOCOL       *PciIo,
  IN EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN UINT64                    OriginalPciAttributes
  )
{
  USB2_HC_DEV             *Ehc;
  EFI_STATUS              Status;

  Ehc = AllocateZeroPool (sizeof (USB2_HC_DEV));

  if (Ehc == NULL) {
    return NULL;
  }

  //
  // Init EFI_USB2_HC_PROTOCOL interface and private data structure
  //
  Ehc->Signature                        = USB2_HC_DEV_SIGNATURE;

  Ehc->Usb2Hc.GetCapability             = EhcGetCapability;
  Ehc->Usb2Hc.Reset                     = EhcReset;
  Ehc->Usb2Hc.GetState                  = EhcGetState;
  Ehc->Usb2Hc.SetState                  = EhcSetState;
  Ehc->Usb2Hc.ControlTransfer           = EhcControlTransfer;
  Ehc->Usb2Hc.BulkTransfer              = EhcBulkTransfer;
  Ehc->Usb2Hc.AsyncInterruptTransfer    = EhcAsyncInterruptTransfer;
  Ehc->Usb2Hc.SyncInterruptTransfer     = EhcSyncInterruptTransfer;
  Ehc->Usb2Hc.IsochronousTransfer       = EhcIsochronousTransfer;
  Ehc->Usb2Hc.AsyncIsochronousTransfer  = EhcAsyncIsochronousTransfer;
  Ehc->Usb2Hc.GetRootHubPortStatus      = EhcGetRootHubPortStatus;
  Ehc->Usb2Hc.SetRootHubPortFeature     = EhcSetRootHubPortFeature;
  Ehc->Usb2Hc.ClearRootHubPortFeature   = EhcClearRootHubPortFeature;
  Ehc->Usb2Hc.MajorRevision             = 0x2;
  Ehc->Usb2Hc.MinorRevision             = 0x0;

  Ehc->PciIo                 = PciIo;
  Ehc->DevicePath            = DevicePath;
  Ehc->OriginalPciAttributes = OriginalPciAttributes;
  Ehc->UsbMemBase            = PlatformGetEhciBase ();

  InitializeListHead (&Ehc->AsyncIntTransfers);

  Ehc->HcStructParams = EhcReadCapRegister (Ehc, EHC_HCSPARAMS_OFFSET);
  Ehc->HcCapParams    = EhcReadCapRegister (Ehc, EHC_HCCPARAMS_OFFSET);
  Ehc->CapLen         = EhcReadCapRegister (Ehc, EHC_CAPLENGTH_OFFSET) & 0x0FF;

  DEBUG ((EFI_D_ERROR, "EhcCreateUsb2Hc: capability length %d\n", Ehc->CapLen));
 
  //
  // EHCI Controllers with a CapLen of 0 are ignored.
  //
  if (Ehc->CapLen == 0) {
    gBS->FreePool (Ehc);
    return NULL;
  }
  
  (VOID)EhcGetUsbDebugPortInfo (Ehc);

  //
  // Create AsyncRequest Polling Timer
  //
  Status = gBS->CreateEvent (
                  EVT_TIMER | EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  EhcMonitorAsyncRequests,
                  Ehc,
                  &Ehc->PollTimer
                  );

  if (EFI_ERROR (Status)) {
    gBS->FreePool (Ehc);
    return NULL;
  }

  return Ehc;
}

/**
  One notified function to stop the Host Controller when gBS->ExitBootServices() called.

  @param  Event                   Pointer to this event
  @param  Context                 Event handler private data

**/
VOID
EFIAPI
EhcExitBootService (
  EFI_EVENT                      Event,
  VOID                           *Context
  )

{
  USB2_HC_DEV       *Ehc;
  EFI_STATUS        Status;

  Ehc = (USB2_HC_DEV *) Context;

  //
  // Reset the Host Controller
  //
  Status = EhcResetHC (Ehc, EHC_RESET_TIMEOUT);
  if(EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "EhcResetHC Failed. %r\n", __FUNCTION__, __LINE__, Status));
  }  
}


/**
  Starting the Usb EHCI Driver.

  @param  This                 Protocol instance pointer.
  @param  Controller           Handle of device to test.
  @param  RemainingDevicePath  Not used.

  @return EFI_SUCCESS          supports this device.
  @return EFI_UNSUPPORTED      do not support this device.
  @return EFI_DEVICE_ERROR     cannot be started due to device Error.
  @return EFI_OUT_OF_RESOURCES cannot allocate resources.

**/
EFI_STATUS
EFIAPI
EhcDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  Controller,
  IN EFI_DEVICE_PATH_PROTOCOL    *RemainingDevicePath
  )
{
  EFI_STATUS                Status;
  USB2_HC_DEV               *Ehc;
  UINT32                    State;
  EFI_DEVICE_PATH_PROTOCOL  *HcDevicePath;

  if(gEhciAleadyInit != FALSE) {
    return EFI_SUCCESS;
  }
  gEhciAleadyInit = TRUE;

  //
  // Open Device Path Protocol for on USB host controller
  //
  HcDevicePath = NULL;
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **) &HcDevicePath,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
				  
  if (EFI_ERROR (Status)) {
    goto ON_EXIT;
  }

  //
  // Create then install USB2_HC_PROTOCOL
  //
  Ehc = EhcCreateUsb2Hc (NULL, HcDevicePath, 0);

  if (Ehc == NULL) {
    DEBUG ((EFI_D_ERROR, "EhcDriverBindingStart: failed to create USB2_HC\n"));

    Status = EFI_OUT_OF_RESOURCES;
    goto ON_EXIT;
  }

  Status = gBS->InstallProtocolInterface (
                  &Controller,
                  &gEfiUsb2HcProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &Ehc->Usb2Hc
                  );

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EhcDriverBindingStart: failed to install USB2_HC Protocol\n"));
    goto FREE_POOL;
  }

  //
  // Robustnesss improvement such as for Duet platform
  // Default is not required.
  //
  if (FeaturePcdGet (PcdTurnOffUsbLegacySupport)) {
    EhcClearLegacySupport (Ehc);
  }

  if (Ehc->DebugPortNum != 0) {
    State = EhcReadDbgRegister(Ehc, 0);
    if ((State & (USB_DEBUG_PORT_IN_USE | USB_DEBUG_PORT_OWNER)) != (USB_DEBUG_PORT_IN_USE | USB_DEBUG_PORT_OWNER)) {
      (VOID)EhcResetHC (Ehc, EHC_RESET_TIMEOUT);
    }
  }

  Status = EhcInitHC (Ehc);

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EhcDriverBindingStart: failed to init host controller\n"));
    goto UNINSTALL_USBHC;
  }

  //
  // Start the asynchronous interrupt monitor
  //
  Status = gBS->SetTimer (Ehc->PollTimer, TimerPeriodic, EHC_ASYNC_POLL_INTERVAL);

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EhcDriverBindingStart: failed to start async interrupt monitor\n"));

    (VOID)EhcHaltHC (Ehc, EHC_GENERIC_TIMEOUT);
    goto UNINSTALL_USBHC;
  }

  //
  // Create event to stop the HC when exit boot service.
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  EhcExitBootService,
                  Ehc,
                  &gEfiEventExitBootServicesGuid,
                  &Ehc->ExitBootServiceEvent
                  );
  if (EFI_ERROR (Status)) {
    goto UNINSTALL_USBHC;
  }

  //
  // Install the component name protocol, don't fail the start
  // because of something for display.
  //
  AddUnicodeString2 (
    "eng",
    gEhciComponentName.SupportedLanguages,
    &Ehc->ControllerNameTable,
    L"Enhanced Host Controller (USB 2.0)",
    TRUE
    );
  AddUnicodeString2 (
    "en",
    gEhciComponentName2.SupportedLanguages,
    &Ehc->ControllerNameTable,
    L"Enhanced Host Controller (USB 2.0)",
    FALSE
    );


  DEBUG ((EFI_D_ERROR, "EhcDriverBindingStart: EHCI started for controller @ %p\n", Controller));
  return EFI_SUCCESS;

UNINSTALL_USBHC:
  gBS->UninstallProtocolInterface (
         Controller,
         &gEfiUsb2HcProtocolGuid,
         &Ehc->Usb2Hc
         );

FREE_POOL:
  EhcFreeSched (Ehc);
  gBS->CloseEvent (Ehc->PollTimer);
  gBS->FreePool (Ehc);

ON_EXIT:
  return Status;
}


/**
  Stop this driver on ControllerHandle. Support stoping any child handles
  created by this driver.

  @param  This                 Protocol instance pointer.
  @param  Controller           Handle of device to stop driver on.
  @param  NumberOfChildren     Number of Children in the ChildHandleBuffer.
  @param  ChildHandleBuffer    List of handles for the children we need to stop.

  @return EFI_SUCCESS          Success.
  @return EFI_DEVICE_ERROR     Fail.

**/
EFI_STATUS
EFIAPI
EhcDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE                  Controller,
  IN UINTN                       NumberOfChildren,
  IN EFI_HANDLE                  *ChildHandleBuffer
  )
{
  EFI_STATUS            Status;
  EFI_USB2_HC_PROTOCOL  *Usb2Hc;
  USB2_HC_DEV           *Ehc;

  gEhciAleadyInit = FALSE;

  //
  // Test whether the Controller handler passed in is a valid
  // Usb controller handle that should be supported, if not,
  // return the error status directly
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiUsb2HcProtocolGuid,
                  (VOID **) &Usb2Hc,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Ehc   = EHC_FROM_THIS (Usb2Hc);

  Status = gBS->UninstallProtocolInterface (
                  Controller,
                  &gEfiUsb2HcProtocolGuid,
                  Usb2Hc
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Stop AsyncRequest Polling timer then stop the EHCI driver
  // and uninstall the EHCI protocl.
  //
  gBS->SetTimer (Ehc->PollTimer, TimerCancel, EHC_ASYNC_POLL_INTERVAL);
  (VOID)EhcHaltHC (Ehc, EHC_GENERIC_TIMEOUT);

  if (Ehc->PollTimer != NULL) {
    gBS->CloseEvent (Ehc->PollTimer);
  }

  if (Ehc->ExitBootServiceEvent != NULL) {
    gBS->CloseEvent (Ehc->ExitBootServiceEvent);
  }

  EhcFreeSched (Ehc);

  if (Ehc->ControllerNameTable != NULL) {
    FreeUnicodeStringTable (Ehc->ControllerNameTable);
  }

  //
  // Disable routing of all ports to EHCI controller, so all ports are 
  // routed back to the UHCI or OHCI controller.
  //
  EhcClearOpRegBit (Ehc, EHC_CONFIG_FLAG_OFFSET, CONFIGFLAG_ROUTE_EHC);

  FreePool (Ehc);

  return EFI_SUCCESS;
}

