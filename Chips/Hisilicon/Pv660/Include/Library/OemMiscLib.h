/** @file
*
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


#ifndef _OEM_MISC_LIB_H_
#define _OEM_MISC_LIB_H_


#include <PlatformArch.h>
#include <Library/HwMemInitLib.h>
#include <BootLine.h>
#include <Library/OemDevicePath.h>

#define I2C_PORT0   0
#define I2C_PORT1   1
#define I2C_INVALIDPORT   0xFF

#define ETH_NUM             2
#define ETH_MODTYPE_GE      0
#define ETH_MODTYPE_10GE    1
#define ETH_INVALID_MOD     0xFF


#define I2C_SLAVEADDR_DCP       0x3E  
#define I2C_SLAVEADDR_FRU       0x52
#define I2C_SLAVEADDR_SFP_CONV  0x50
#define I2C_SLAVEADDR_SFP_PHY   0x56
#define I2C_SLAVEADDR_SFP_ENHA  0x51
#define I2C_SLAVEADDR_PCA9545   0x70

#define I2C_SLAVEADDR_CK420     0x69

#define CK420_SPREAD_ENABLE     0x9f 
#define CK420_SPREAD_DISABLE    0x9e  
#define CK420_SPREAD_REG        0x1   
#define CK420_MIN_REG           0x0  
#define CK420_MAX_REG           0x9  
#define CK420_MAX_REG_NUM       0xa   
#define PLL_BACKUP

#define INVAILD_BOOT_OPTION    0xFFFF

EFI_STATUS OemPreStartBootOptionAction(UINT16 BootOptionId);
EFI_STATUS OemBootOrderSeting(IN OUT UINT16* BootOrderFinal, UINTN BootOrderSize, BOOLEAN *IsFlashBootFirst, BOOLEAN *FlashBootSupport);
EFI_STATUS OemGetSataBootNum(UINTN SataDesSize);
EFI_STATUS OemGetPXEBootNum(UINTN PXESize);

#define ETH_MAX_PORT          8   
#define ETH_DEBUG_PORT0       6  
#define ETH_DEBUG_PORT1       7  

#define ETH_SPEED_10M     6
#define ETH_SPEED_100M    7
#define ETH_SPEED_1000M   8
#define ETH_SPEED_10KM    9
#define ETH_HALF_DUPLEX   0  
#define ETH_FULL_DUPLEX   1   

#define ETH_GDD_ID                          0x001378e0    
#define ETH_PHY_BCM5241_ID                  0x0143bc30      
#define ETH_PHY_MVL88E1145_ID               0x01410cd0      
#define ETH_PHY_MVL88E1119_ID               0x01410e80       
#define ETH_PHY_MVL88E1512_ID               0x01410dd0       
#define ETH_PHY_MVL88E1543_ID               0x01410ea0     
#define ETH_PHY_NLP3142_ID                  0x00000412       

#define ETH_INVALID                         0xffffffff

typedef struct EthProductDesc{
    UINT32 Valid;   
    UINT32 Speed;
    UINT32 Duplex;
    UINT32 PhyId;
    UINT32 PhyAddr;
}ETH_PRODUCT_DESC;
typedef struct _I2C_SLAVE_ADDR{
    UINT8  Port;
    UINT8  DevAddr;
}I2C_SLAVE_ADDR;

typedef union
{
  struct
  {
    UINT32   SclId    :  8;
    UINT32   Cpu      :  8;
    UINT32   Result   :  8;
    UINT32   Reserved :  8;
  } cpu_bist;
  struct
  {
    UINT32   Result   :  8;
    UINT32   Reserved :  24;
  } clock;        
  struct
  {
    UINT32   Result   :  8;
    UINT32   Reserved :  24;
  } cpld;         
  struct
  {
    UINT32   Result   :  8;
    UINT32   Reserved :  24;
  } bios_crc;     
  struct
  {
    UINT32 Socket    :   8;
    UINT32 Channel   :   8;
    UINT32 Dimm      :   8;
    UINT32 Result    :   8;
  } dimm_absent;  
  struct
  {
    UINT32 Socket    :   8;
    UINT32 Channel   :   8;
    UINT32 Dimm      :   8;
    UINT32 Result    :   8;
  } dimm_isolate;  
  struct
  {
    UINT32   Bank     :  8;
    UINT32   Tag_Data :  8; //0:tag,  1:data
    UINT32   Result   :  8;
    UINT32   Reserved :  8;
  } l3_bist;

  UINT32    Data;
} U_SELF_TEST_PARA;

typedef enum
{
    SELF_TEST_CPU_BIST  = 0,
    SELF_TEST_CLOCK,    
    SELF_TEST_CPLD,
    SELF_TEST_BIOS_CRC,
    SELF_TEST_DIMM_ABSENT,
    SELF_TEST_DIMM_ISOLATE,
    SELT_TEST_L3CACHE,

    SELF_TEST_BUTT
} E_SELF_TEST_ITEM;

typedef VOID (*SELF_TEST_REPORT) (IN UINT32 SelfTestPara);
typedef enum
{
    RESET_CAUSE_KEYPRESS       = 0x1,  
    RESET_CAUSE_WDT            = 0x2,  
    RESET_CAUSE_SOFT           = 0x3,  
    RESET_CAUSE_SHUT           = 0x4,  
    RESET_CAUSE_UNKNOWN        = 0x6,   
    RESET_CAUSE_TEMP           = 0x7, 
    RESET_CAUSE_POWER          = 0xa,  
    RESET_CAUSE_MCE            = 0xb, 
    RESET_CAUSE_BIOS_SWITCH    = 0xc,  
    RESET_CAUSE_CPLD_UPDATA    = 0xd,  
    RESET_CAUSE_BIOS           = 0xf,   
    RESET_CAUSE_SOFT_FAIL      = 0xa0,  
    RESET_CAUSE_WDT_FAIL       = 0xa1, 
    RESET_CAUSE_KEYPRESS_FAIL  = 0xa2, 
    RESET_CAUSE_SES            = 0xa3,  

    RESET_CAUSE_CPLD_BUTT      = 0xff
} E_RESET_CAUSE;

VOID OemPostStartIndicator (VOID);
VOID OemPostEndIndicator (VOID);

extern I2C_SLAVE_ADDR  DimmSpdAddr[MAX_SOCKET][MAX_CHANNEL][MAX_DIMM];

BOOLEAN OemIsSocketPresent (UINTN Socket);

EFI_STATUS OemSelfTestReport(IN E_SELF_TEST_ITEM Item, IN U_SELF_TEST_PARA Para);
VOID OemSetSelfTestFailFlagAndAlarm(VOID);
UINT32 OemGetCurrentBiosChannel(VOID);
EFI_STATUS OemCheckCpld(VOID);
EFI_STATUS OemCheckClock(VOID);

E_RESET_CAUSE OemGetCurrentResetCause(VOID);
E_RESET_CAUSE OemGetPer2ResetCause(VOID);
E_RESET_CAUSE OemGetPer3ResetCause(VOID);
UINT32 OemIsWarmBoot();
VOID OemBiosSwitch(UINT32 Master);

VOID CoreSelectBoot(VOID);

VOID SetCpldBootDeviceID(IN UINT8 Value);
UINT8 ReadCpldBootDeviceID(void);
VOID SetCpldBootDeviceCount(IN UINT8 Value);
UINT8 ReadCpldBootDeviceCount(void);

VOID OemWriteProtectSet(BOOLEAN val);
BOOLEAN OemIsSpiFlashWriteProtected(VOID);
VOID OemPcieResetAndOffReset(void);
VOID OpenAlarmLed(VOID);
VOID OemPcieCardReset(UINT32 Reset);
EFI_STATUS OemCk420Read(UINT16 InfoOffset, UINT32 ulLength, UINT8 *pBuf);
EFI_STATUS OemCk420Write(UINT16 InfoOffset, UINT32 ulLength, UINT8 *pBuf);
UINT32  SelDdrFreq(pGBL_DATA pGblData);

VOID BoardInformation(void);

UINT8 OemAhciStartPort(VOID);
UINT32 OemGetDefaultSetupData(VOID *Setup);

VOID CpldRamWriteCpuBist(IN UINT32 SelfTestPara);
VOID CpldRamWriteClockCheck(IN UINT32 SelfTestPara);
VOID CpldRamWriteCpldCheck(IN UINT32 SelfTestPara);
VOID CpldRamWriteBiosCrcCheck(IN UINT32 SelfTestPara);
VOID CpldRamWriteDimmAbsent(IN UINT32 SelfTestPara);
VOID CpldRamWriteDimmIsolate(IN UINT32 SelfTestPara);


extern CHAR8 *sTokenList[];
UINT32 OemEthFindFirstSP();
ETH_PRODUCT_DESC *OemEthInit(UINT32 port);

VOID InitMarginLog();
VOID GetMarginRamInfo(UINT16 AreaOffset, UINT8 *Data, UINT8 Bytes);
VOID SetMarginRamInfo(UINT16 Addr, UINT8 *Data, UINT8 Bytes);
BOOLEAN OemPreMarginTestInit();
VOID OemScrubFlagConfig(pGBL_DATA pGblData);
UINTN OemGetSocketNumber(VOID);
UINTN OemGetDdrChannel (VOID);
UINTN OemGetDimmSlot(UINTN Socket, UINTN Channel);
BOOLEAN OemIsLoadDefault();

BOOLEAN OemIsMpBoot();

BOOLEAN OemIsInitEth(UINT32 Port);
#endif

