/*
 * PROJECT:     ReactOS Intel PRO/1000 Driver
 * LICENSE:     GPL-2.0+ (https://spdx.org/licenses/GPL-2.0+)
 * PURPOSE:     Hardware specific functions
 * COPYRIGHT:   Copyright 2018 Mark Jansen (mark.jansen@reactos.org)
 */

#include "nic.h"

#include <debug.h>


static USHORT SupportedDevices[] =
{
    0x100f,     // Intel 82545EM (VMWare E1000)
};


static ULONG E1000WriteFlush(IN PE1000_ADAPTER Adapter)
{
    volatile ULONG Value;

    NdisReadRegisterUlong(Adapter->IoBase + E1000_REG_STATUS, &Value);
    return Value;
}

static VOID E1000WriteUlong(IN PE1000_ADAPTER Adapter, IN ULONG Address, IN ULONG Value)
{
    NdisWriteRegisterUlong((PULONG)(Adapter->IoBase + Address), Value);
}

static VOID E1000ReadUlong(IN PE1000_ADAPTER Adapter, IN ULONG Address, OUT PULONG Value)
{
    NdisReadRegisterUlong((PULONG)(Adapter->IoBase + Address), Value);
}

static VOID E1000WriteIoUlong(IN PE1000_ADAPTER Adapter, IN ULONG Address, IN ULONG Value)
{
    NdisRawWritePortUlong((PULONG)(Adapter->IoPort), Address);
    E1000WriteFlush(Adapter);
    NdisRawWritePortUlong((PULONG)(Adapter->IoPort + 4), Value);
}

static BOOLEAN E1000ReadMdic(IN PE1000_ADAPTER Adapter, IN ULONG Address, USHORT *Result)
{
    ULONG ResultAddress;
    ULONG Mdic;
    UINT n;

    if (Address > MAX_PHY_REG_ADDRESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("PHY Address %d is invalid\n", Address));
        return 1;
    }

    Mdic = (Address << E1000_MDIC_REGADD_SHIFT);
    Mdic |= (E1000_MDIC_PHYADD_GIGABIT << E1000_MDIC_PHYADD_SHIFT);
    Mdic |= E1000_MDIC_OP_READ;

    E1000WriteUlong(Adapter, E1000_REG_MDIC, Mdic);

    for (n = 0; n < MAX_PHY_READ_ATTEMPTS; n++)
    {
        NdisStallExecution(50);
        E1000ReadUlong(Adapter, E1000_REG_MDIC, &Mdic);
        if (Mdic & E1000_MDIC_R)
            break;
    }
    if (!(Mdic & E1000_MDIC_R))
    {
        NDIS_DbgPrint(MIN_TRACE, ("MDI Read incomplete\n"));
        return FALSE;
    }
    if (Mdic & E1000_MDIC_E)
    {
        NDIS_DbgPrint(MIN_TRACE, ("MDI Read error\n"));
        return FALSE;
    }

    ResultAddress = (Mdic >> E1000_MDIC_REGADD_SHIFT) & MAX_PHY_REG_ADDRESS;

    if (ResultAddress!= Address)
    {
        /* Add locking? */
        NDIS_DbgPrint(MIN_TRACE, ("MDI Read got wrong address (%d instead of %d)\n",
                                  ResultAddress, Address));
        return FALSE;
    }
    *Result = (USHORT) Mdic;
    return TRUE;
}


static BOOLEAN E1000ReadEeprom(IN PE1000_ADAPTER Adapter, IN UCHAR Address, USHORT *Result)
{
    UINT Value;
    UINT n;

    E1000WriteUlong(Adapter, E1000_REG_EERD, E1000_EERD_START | ((UINT)Address << E1000_EERD_ADDR_SHIFT));

    for (n = 0; n < MAX_EEPROM_READ_ATTEMPTS; ++n)
    {
        NdisStallExecution(5);

        E1000ReadUlong(Adapter, E1000_REG_EERD, &Value);

        if (Value & E1000_EERD_DONE)
            break;
    }
    if (!(Value & E1000_EERD_DONE))
    {
        NDIS_DbgPrint(MIN_TRACE, ("EEPROM Read incomplete\n"));
        return FALSE;
    }
    *Result = (USHORT)(Value >> E1000_EERD_DATA_SHIFT);
    return TRUE;
}

BOOLEAN E1000ValidateNvmChecksum(IN PE1000_ADAPTER Adapter)
{
    USHORT Checksum = 0, Data;
    UINT n;

    /* 5.6.35 Checksum Word Calculation (Word 3Fh) */
    for (n = 0; n <= E1000_NVM_REG_CHECKSUM; n++)
    {
        if (!E1000ReadEeprom(Adapter, n, &Data))
        {
            return FALSE;
        }
        Checksum += Data;
    }

    if (Checksum != NVM_MAGIC_SUM)
    {
        NDIS_DbgPrint(MIN_TRACE, ("EEPROM has an invalid checksum of 0x%x\n", (ULONG)Checksum));
        return FALSE;
    }

    return TRUE;
}


BOOLEAN
NTAPI
NICRecognizeHardware(
    IN PE1000_ADAPTER Adapter)
{
    UINT n;
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    if (Adapter->VendorID != HW_VENDOR_INTEL)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unknown vendor: 0x%x\n", Adapter->VendorID));
        return FALSE;
    }

    for (n = 0; n < ARRAYSIZE(SupportedDevices); ++n)
    {
        if (SupportedDevices[n] == Adapter->DeviceID)
        {
            return TRUE;
        }
    }

    NDIS_DbgPrint(MIN_TRACE, ("Unknown device: 0x%x\n", Adapter->DeviceID));

    return FALSE;
}

NDIS_STATUS
NTAPI
NICInitializeAdapterResources(
    IN PE1000_ADAPTER Adapter,
    IN PNDIS_RESOURCE_LIST ResourceList)
{
    UINT n;
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    for (n = 0; n < ResourceList->Count; n++)
    {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR ResourceDescriptor = ResourceList->PartialDescriptors + n;

        switch (ResourceDescriptor->Type)
        {
        case CmResourceTypePort:
            ASSERT(Adapter->IoPortAddress == 0);
            ASSERT(ResourceDescriptor->u.Port.Start.HighPart == 0);

            Adapter->IoPortAddress = ResourceDescriptor->u.Port.Start.LowPart;
            Adapter->IoPortLength = ResourceDescriptor->u.Port.Length;

            NDIS_DbgPrint(MID_TRACE, ("I/O port range is %p to %p\n",
                                      Adapter->IoPortAddress,
                                      Adapter->IoPortAddress + Adapter->IoPortLength));
            break;
        case CmResourceTypeInterrupt:
            ASSERT(Adapter->InterruptVector == 0);
            ASSERT(Adapter->InterruptLevel == 0);

            Adapter->InterruptVector = ResourceDescriptor->u.Interrupt.Vector;
            Adapter->InterruptLevel = ResourceDescriptor->u.Interrupt.Level;
            Adapter->InterruptShared = (ResourceDescriptor->ShareDisposition == CmResourceShareShared);
            Adapter->InterruptFlags = ResourceDescriptor->Flags;

            NDIS_DbgPrint(MID_TRACE, ("IRQ vector is %d\n", Adapter->InterruptVector));
            break;
        case CmResourceTypeMemory:
            /* Internal registers and memories (including PHY) */
            if (ResourceDescriptor->u.Memory.Length ==  (128 * 1024))
            {
                ASSERT(Adapter->IoAddress.LowPart == 0);
                ASSERT(ResourceDescriptor->u.Port.Start.HighPart == 0);


                Adapter->IoAddress.QuadPart = ResourceDescriptor->u.Memory.Start.QuadPart;
                Adapter->IoLength = ResourceDescriptor->u.Memory.Length;
                NDIS_DbgPrint(MID_TRACE, ("Memory range is %I64x to %I64x\n",
                                          Adapter->IoAddress.QuadPart,
                                          Adapter->IoAddress.QuadPart + Adapter->IoLength));
            }
            break;

        default:
            NDIS_DbgPrint(MIN_TRACE, ("Unrecognized resource type: 0x%x\n", ResourceDescriptor->Type));
            break;
        }
    }

    if (Adapter->IoAddress.QuadPart == 0 || Adapter->IoPortAddress == 0 || Adapter->InterruptVector == 0)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Adapter didn't receive enough resources\n"));
        return NDIS_STATUS_RESOURCES;
    }

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICAllocateIoResources(
    IN PE1000_ADAPTER Adapter)
{
    NDIS_STATUS Status;
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    Status = NdisMRegisterIoPortRange((PVOID*)&Adapter->IoPort,
                                      Adapter->AdapterHandle,
                                      Adapter->IoPortAddress,
                                      Adapter->IoPortLength);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        NDIS_DbgPrint(MIN_TRACE, ("Unable to register IO port range (0x%x)\n", Status));
        return NDIS_STATUS_RESOURCES;
    }

    Status = NdisMMapIoSpace((PVOID*)&Adapter->IoBase,
                             Adapter->AdapterHandle,
                             Adapter->IoAddress,
                             Adapter->IoLength);

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICRegisterInterrupts(
    IN PE1000_ADAPTER Adapter)
{
    NDIS_STATUS Status;
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    Status = NdisMRegisterInterrupt(&Adapter->Interrupt,
                                    Adapter->AdapterHandle,
                                    Adapter->InterruptVector,
                                    Adapter->InterruptLevel,
                                    TRUE, // We always want ISR calls
                                    Adapter->InterruptShared,
                                    (Adapter->InterruptFlags & CM_RESOURCE_INTERRUPT_LATCHED) ?
                                    NdisInterruptLatched : NdisInterruptLevelSensitive);

    if (Status == NDIS_STATUS_SUCCESS)
    {
        Adapter->InterruptRegistered = TRUE;
    }

    return Status;
}

NDIS_STATUS
NTAPI
NICUnregisterInterrupts(
    IN PE1000_ADAPTER Adapter)
{
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    if (Adapter->InterruptRegistered)
    {
        NdisMDeregisterInterrupt(&Adapter->Interrupt);
        Adapter->InterruptRegistered = FALSE;
    }

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICReleaseIoResources(
    IN PE1000_ADAPTER Adapter)
{
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    if (Adapter->IoPort)
    {
        NdisMDeregisterIoPortRange(Adapter->AdapterHandle,
                                   Adapter->IoPortAddress,
                                   Adapter->IoPortLength,
                                   Adapter->IoPort);
    }

    if (Adapter->IoBase)
    {
        NdisMUnmapIoSpace(Adapter->AdapterHandle, Adapter->IoBase, Adapter->IoLength);
    }


    return NDIS_STATUS_SUCCESS;
}


NDIS_STATUS
NTAPI
NICPowerOn(
    IN PE1000_ADAPTER Adapter)
{
    NDIS_STATUS Status;
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    Status = NICSoftReset(Adapter);
    if (Status != NDIS_STATUS_SUCCESS)
    {
        return Status;
    }

    if (!E1000ValidateNvmChecksum(Adapter))
    {
        return NDIS_STATUS_INVALID_DATA;
    }

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICSoftReset(
    IN PE1000_ADAPTER Adapter)
{
    ULONG Value, ResetAttempts;
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    //em_get_hw_control(adapter);

    NICDisableInterrupts(Adapter);
    E1000WriteUlong(Adapter, E1000_REG_RCTL, 0);
    E1000ReadUlong(Adapter, E1000_REG_CTRL, &Value);
    /* Write this using IO port, some devices cannot ack this otherwise */
    E1000WriteIoUlong(Adapter, E1000_REG_CTRL, Value | E1000_CTRL_RST);


    for (ResetAttempts = 0; ResetAttempts < MAX_RESET_ATTEMPTS; ResetAttempts++)
    {
        NdisStallExecution(100);
        E1000ReadUlong(Adapter, E1000_REG_CTRL, &Value);

        if (!(Value & E1000_CTRL_RST))
        {
            NDIS_DbgPrint(MAX_TRACE, ("Device is back (%u)\n", ResetAttempts));

            NICDisableInterrupts(Adapter);
            /* Clear out interrupts */
            E1000ReadUlong(Adapter, E1000_REG_ICR, &Value);

            //NdisWriteRegisterUlong(Adapter->IoBase + E1000_REG_WUFC, 0);
            //NdisWriteRegisterUlong(Adapter->IoBase + E1000_REG_VET, E1000_VET_VLAN);

            return NDIS_STATUS_SUCCESS;
        }
    }

    NDIS_DbgPrint(MIN_TRACE, ("Device did not recover\n"));
    return NDIS_STATUS_FAILURE;
}

NDIS_STATUS
NTAPI
NICEnableTxRx(
    IN PE1000_ADAPTER Adapter)
{
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICDisableTxRx(
    IN PE1000_ADAPTER Adapter)
{
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICGetPermanentMacAddress(
    IN PE1000_ADAPTER Adapter,
    OUT PUCHAR MacAddress)
{
    USHORT AddrWord;
    UINT n;

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    /* Should we read from RAL/RAH first? */
    for (n = 0; n < (IEEE_802_ADDR_LENGTH / 2); ++n)
    {
        if (!E1000ReadEeprom(Adapter, (UCHAR)n, &AddrWord))
            return NDIS_STATUS_FAILURE;
        Adapter->PermanentMacAddress[n * 2 + 0] = AddrWord & 0xff;
        Adapter->PermanentMacAddress[n * 2 + 1] = (AddrWord >> 8) & 0xff;
    }

    NDIS_DbgPrint(MIN_TRACE, ("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                              Adapter->PermanentMacAddress[0],
                              Adapter->PermanentMacAddress[1],
                              Adapter->PermanentMacAddress[2],
                              Adapter->PermanentMacAddress[3],
                              Adapter->PermanentMacAddress[4],
                              Adapter->PermanentMacAddress[5]));
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICUpdateMulticastList(
    IN PE1000_ADAPTER Adapter)
{
    UINT n;
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    for (n = 0; n < MAXIMUM_MULTICAST_ADDRESSES; ++n)
    {
        ULONG Ral = *(ULONG *)Adapter->MulticastList[n].MacAddress;
        ULONG Rah = *(USHORT *)&Adapter->MulticastList[n].MacAddress[4];

        if (Rah || Ral)
        {
            Rah |= E1000_RAH_AV;

            E1000WriteUlong(Adapter, E1000_REG_RAL + (8*n), Ral);
            E1000WriteUlong(Adapter, E1000_REG_RAH + (8*n), Rah);
        }
        else
        {
            E1000WriteUlong(Adapter, E1000_REG_RAH + (8*n), 0);
            E1000WriteUlong(Adapter, E1000_REG_RAL + (8*n), 0);
        }
    }

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICApplyPacketFilter(
    IN PE1000_ADAPTER Adapter)
{
    ULONG FilterMask = 0;

    E1000ReadUlong(Adapter, E1000_REG_RCTL, &FilterMask);

    FilterMask &= ~E1000_RCTL_FILTER_BITS;

    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_ALL_MULTICAST)
    {
        /* Multicast Promiscuous Enabled */
        FilterMask |= E1000_RCTL_MPE;
    }
    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_PROMISCUOUS)
    {
        /* Unicast Promiscuous Enabled */
        FilterMask |= E1000_RCTL_UPE;
        /* Multicast Promiscuous Enabled */
        FilterMask |= E1000_RCTL_MPE;
    }
    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_MAC_FRAME)
    {
        /* Pass MAC Control Frames */
        FilterMask |= E1000_RCTL_PMCF;
    }
    if (Adapter->PacketFilter & NDIS_PACKET_TYPE_BROADCAST)
    {
        /* Broadcast Accept Mode */
        FilterMask |= E1000_RCTL_BAM;
    }

    E1000WriteUlong(Adapter, E1000_REG_RCTL, FilterMask);

    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICApplyInterruptMask(
    IN PE1000_ADAPTER Adapter)
{
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    E1000WriteUlong(Adapter, E1000_REG_IMS, Adapter->InterruptMask);
    return NDIS_STATUS_SUCCESS;
}

NDIS_STATUS
NTAPI
NICDisableInterrupts(
    IN PE1000_ADAPTER Adapter)
{
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    E1000WriteUlong(Adapter, E1000_REG_IMC, ~0);
    return NDIS_STATUS_SUCCESS;
}

USHORT
NTAPI
NICInterruptRecognized(
    IN PE1000_ADAPTER Adapter,
    OUT PBOOLEAN InterruptRecognized)
{
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    return 0;
}

VOID
NTAPI
NICAcknowledgeInterrupts(
    IN PE1000_ADAPTER Adapter)
{
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));
}

VOID
NTAPI
NICUpdateLinkStatus(
    IN PE1000_ADAPTER Adapter)
{
    ULONG SpeedIndex;
    USHORT PhyStatus;
    static ULONG SpeedValues[] = { 10, 100, 1000, 1000 };

    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

#if 0
    /* This does not work */
    E1000ReadUlong(Adapter, E1000_REG_STATUS, &DeviceStatus);
    E1000ReadUlong(Adapter, E1000_REG_STATUS, &DeviceStatus);
    Adapter->MediaState = (DeviceStatus & E1000_STATUS_LU) ? NdisMediaStateConnected : NdisMediaStateDisconnected;
    SpeedIndex = (DeviceStatus & E1000_STATUS_SPEEDMASK) >> E1000_STATUS_SPEEDSHIFT;
    Adapter->LinkSpeedMbps = SpeedValues[SpeedIndex];
#else
    /* Link bit can be sticky on some boards, read it twice */
    if (!E1000ReadMdic(Adapter, E1000_PHY_STATUS, &PhyStatus))
        NdisStallExecution(100);

    Adapter->MediaState = NdisMediaStateDisconnected;
    Adapter->LinkSpeedMbps = 0;

    if (!E1000ReadMdic(Adapter, E1000_PHY_STATUS, &PhyStatus))
        return;

    if (!(PhyStatus & E1000_PS_LINK_STATUS))
        return;

    Adapter->MediaState = NdisMediaStateConnected;

    if (E1000ReadMdic(Adapter, E1000_PHY_SPECIFIC_STATUS, &PhyStatus))
    {
        if (PhyStatus & E1000_PSS_SPEED_AND_DUPLEX)
        {
            SpeedIndex = (PhyStatus & E1000_PSS_SPEEDMASK) >> E1000_PSS_SPEEDSHIFT;
            Adapter->LinkSpeedMbps = SpeedValues[SpeedIndex];
        }
        else
        {
            NDIS_DbgPrint(MIN_TRACE, ("Speed and duplex not yet resolved, retry?.\n"));
        }
    }
#endif
}

NDIS_STATUS
NTAPI
NICTransmitPacket(
    IN PE1000_ADAPTER Adapter,
    IN UCHAR TxDesc,
    IN ULONG PhysicalAddress,
    IN ULONG Length)
{
    NDIS_DbgPrint(MAX_TRACE, ("Called.\n"));

    return NDIS_STATUS_FAILURE;
}
