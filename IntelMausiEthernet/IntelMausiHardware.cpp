/* IntelMausiHardware.cpp -- IntelMausi hardware specific routines.
 *
 * Copyright (c) 2014 Laura Müller <laura-mueller@uni-duesseldorf.de>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * Driver for Intel PCIe gigabit ethernet controllers.
 *
 * This driver is based on Intel's E1000e driver for Linux.
 */


#include "IntelMausiEthernet.h"

#pragma mark --- hardware initialization methods ---

bool IntelMausi::initPCIConfigSpace(IOPCIDevice *provider)
{
    bool result = false;
    
    /* Get vendor and device info. */
    pciDeviceData.vendor = provider->configRead16(kIOPCIConfigVendorID);
    pciDeviceData.device = provider->configRead16(kIOPCIConfigDeviceID);
    pciDeviceData.subsystem_vendor = provider->configRead16(kIOPCIConfigSubSystemVendorID);
    pciDeviceData.subsystem_device = provider->configRead16(kIOPCIConfigSubSystemID);
    pciDeviceData.revision = provider->configRead8(kIOPCIConfigRevisionID);
    
    /* Identify the chipset. */
    if (!intelIdentifyChip())
        goto done;
    
    if (chipType == board_pch_lpt) {
        pciDeviceData.maxSnoop = provider->configRead16(E1000_PCI_LTR_CAP_LPT);
        pciDeviceData.maxNoSnoop = provider->configRead16(E1000_PCI_LTR_CAP_LPT + 2);
    }    
    /* Get the bus information. */
    adapterData.hw.bus.func = pciDevice->getFunctionNumber();
    adapterData.hw.bus.width = e1000_bus_width_pcie_x1;

    /* Enable the device. */
    intelEnablePCIDevice(provider);
    
    baseMap = provider->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    
    if (!baseMap) {
        IOLog("Ethernet [IntelMausi]: region #0 not an MMIO resource, aborting.\n");
        goto done;
    }
    baseAddr = reinterpret_cast<volatile void *>(baseMap->getVirtualAddress());
    adapterData.hw.hw_addr = (u8 __iomem *)baseAddr;
    
    if (adapterData.flags & FLAG_HAS_FLASH) {
        flashMap = provider->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress1);
        
        if (!flashMap) {
            IOLog("Ethernet [IntelMausi]: region #1 not an MMIO resource, aborting.\n");
            goto error;
        }
        flashAddr = reinterpret_cast<volatile void *>(flashMap->getVirtualAddress());
        adapterData.hw.flash_address = (u8 __iomem *)flashAddr;
    }
    result = true;
    
done:
    return result;
    
error:
    RELEASE(baseMap);
    baseMap = NULL;
    adapterData.hw.hw_addr = NULL;
    goto done;
}

void IntelMausi::initPCIPowerManagment(IOPCIDevice *provider, const struct e1000_info *ei)
{
    UInt32 pcieLinkCap;
    UInt16 pcieLinkCtl;
    UInt16 aspmDisable;
    UInt16 pmCap;
    UInt8 pmCapOffset;
    
    /* Setup power management. */
    if (provider->findPCICapability(kIOPCIPowerManagementCapability, &pmCapOffset)) {
        pmCap = provider->configRead16(pmCapOffset + kIOPCIPMCapability);
        DebugLog("Ethernet [IntelMausi]: PCI power management capabilities: 0x%x.\n", pmCap);
        
        if (pmCap & (kPCIPMCPMESupportFromD3Cold | kPCIPMCPMESupportFromD3Hot)) {
            wolCapable = true;
            DebugLog("Ethernet [IntelMausi]: PME# from D3 (cold/hot) supported.\n");
        }
        pciPMCtrlOffset = pmCapOffset + kIOPCIPMControl;
    } else {
        IOLog("Ethernet [IntelMausi]: PCI power management unsupported.\n");
    }
    provider->enablePCIPowerManagement();
    
    /* Get PCIe link information. */
    if (provider->findPCICapability(kIOPCIPCIExpressCapability, &pcieCapOffset)) {
        pcieLinkCap = provider->configRead32(pcieCapOffset + kIOPCIELinkCapability);
        pcieLinkCtl = provider->configRead16(pcieCapOffset + kIOPCIELinkControl);
        DebugLog("Ethernet [IntelMausi]: PCIe link capabilities: 0x%08x, link control: 0x%04x.\n", pcieLinkCap, pcieLinkCtl);
        aspmDisable = 0;
        
        if (ei->flags2 & FLAG2_DISABLE_ASPM_L0S)
            aspmDisable |= kIOPCIELinkCtlL0s;
        
        if (ei->flags2 & FLAG2_DISABLE_ASPM_L1)
            aspmDisable |= kIOPCIELinkCtlL1;

        if (aspmDisable)
            provider->configWrite16(pcieCapOffset + kIOPCIELinkControl, (pcieLinkCtl & ~aspmDisable));

#ifdef DEBUG
        pcieLinkCtl = provider->configRead16(pcieCapOffset + kIOPCIELinkControl);
        
        if (pcieLinkCtl & (kIOPCIELinkCtlASPM | kIOPCIELinkCtlClkReqEn))
            IOLog("Ethernet [IntelMausi]: PCIe ASPM enabled. link control: 0x%04x.\n", pcieLinkCtl);
        else
            IOLog("Ethernet [IntelMausi]: PCIe ASPM disabled. link control: 0x%04x.\n", pcieLinkCtl);
#endif  /* DEBUG */
    }
}

IOReturn IntelMausi::setPowerStateWakeAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    IntelMausi *ethCtlr = OSDynamicCast(IntelMausi, owner);
    IOPCIDevice *dev;
    UInt16 val16;
    UInt8 offset;
    
    if (ethCtlr) {
        dev = ethCtlr->pciDevice;
        offset = ethCtlr->pciPMCtrlOffset;
        
        val16 = dev->configRead16(offset);
        
        val16 &= ~(kPCIPMCSPowerStateMask | kPCIPMCSPMEStatus | kPCIPMCSPMEEnable);
        val16 |= kPCIPMCSPowerStateD0;
        
        dev->configWrite16(offset, val16);
        
        /* Restore the PCI Command register. */
        ethCtlr->intelEnablePCIDevice(dev);
    }
    return kIOReturnSuccess;
}

IOReturn IntelMausi::setPowerStateSleepAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    IntelMausi *ethCtlr = OSDynamicCast(IntelMausi, owner);
    IOPCIDevice *dev;
    UInt16 val16;
    UInt8 offset;
    
    if (ethCtlr) {
        dev = ethCtlr->pciDevice;
        offset = ethCtlr->pciPMCtrlOffset;
        
        val16 = dev->configRead16(offset);
        
        val16 &= ~(kPCIPMCSPowerStateMask | kPCIPMCSPMEStatus | kPCIPMCSPMEEnable);
        
        if (ethCtlr->adapterData.wol)
            val16 |= (kPCIPMCSPMEStatus | kPCIPMCSPMEEnable | kPCIPMCSPowerStateD3);
        else
            val16 |= kPCIPMCSPowerStateD3;
        
        dev->configWrite16(offset, val16);
    }
    return kIOReturnSuccess;
}

void IntelMausi::intelEEPROMChecks(struct e1000_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	int ret_val;
	u16 buf = 0;
    
	if (hw->mac.type != e1000_82573)
		return;
    
	ret_val = e1000_read_nvm(hw, NVM_INIT_CONTROL2_REG, 1, &buf);
	le16_to_cpus(&buf);
    
	if (!ret_val && (!(buf & (1 << 0)))) {
		/* Deep Smart Power Down (DSPD) */
		IOLog("Ethernet [IntelMausi]: Warning: detected DSPD enabled in EEPROM.\n");
	}
}

void IntelMausi::intelEnableIRQ(UInt32 newMask)
{
    intelWriteMem32(E1000_IMS, newMask);
    intelFlush();
}

void IntelMausi::intelDisableIRQ()
{
    intelWriteMem32(E1000_IMC, 0xFFFFFFFF);
    intelFlush();
}

void IntelMausi::intelEnable()
{
    struct e1000_hw *hw = &adapterData.hw;
    
    e1000_phy_hw_reset(hw);
    
    if (hw->mac.type >= e1000_pch2lan)
		e1000_resume_workarounds_pchlan(hw);
    
    e1000e_power_up_phy(&adapterData);
    
    //intelInitManageabilityPt(&adapterData);
    
    /* If AMT is enabled, let the firmware know that the network
	 * interface is now open and reset the part to a known state.
	 */
	if (adapterData.flags & FLAG_HAS_AMT) {
		e1000e_get_hw_control(&adapterData);
	}
    intelReset(&adapterData);
    
#if DISABLED_CODE
    
	/* DMA latency requirement to workaround jumbo issue */
	pm_qos_add_request(&adapter->netdev->pm_qos_req, PM_QOS_CPU_DMA_LATENCY,
                       PM_QOS_DEFAULT_VALUE);
    
#endif /* DISABLED_CODE */
    
	intelConfigure(&adapterData);
    
	/* From here on the code is the same as e1000e_up() */
	clear_bit(__E1000_DOWN, &adapterData.state);
    
	intelEnableIRQ(intrMask);
    
	adapterData.tx_hang_recheck = false;
    
	hw->mac.get_link_status = true;
}

void IntelMausi::intelDisable()
{
    intelDown(&adapterData, true);
    //e1000_power_down_phy(&adapterData);
    
    /* If AMT is enabled, let the firmware know that the network
     * interface is now closed
     */
    if (adapterData.flags & FLAG_HAS_AMT)
        e1000e_release_hw_control(&adapterData);
}

/**
 * intelConfigure - configure the hardware for Rx and Tx
 * @adapter: private board structure
 **/
void IntelMausi::intelConfigure(struct e1000_adapter *adapter)
{
    setMulticastMode(true);
    intelInitManageabilityPt(adapter);
    
    intelSetupRssHash(adapter);
    intelVlanStripEnable(adapter);
    intelConfigureTx(adapter);
    intelSetupRxControl(adapter);
    intelConfigureRx(adapter);
}

/**
 * intelConfigureTx - Configure Transmit Unit after Reset
 * @adapter: board private structure
 *
 * Configure the Tx unit of the MAC after a reset.
 **/
void IntelMausi::intelConfigureTx(struct e1000_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	//struct e1000_ring *tx_ring = adapter->tx_ring;
	UInt32 tctl, tarc;
    UInt32 txdctl;
    
    /* Setup the HW Tx Head and Tail descriptor pointers */
    intelInitTxRing();
    
	/* Set the Tx Interrupt Delay register */
	intelWriteMem32(E1000_TIDV, adapter->tx_int_delay);
	/* Tx irq moderation */
	intelWriteMem32(E1000_TADV, adapter->tx_abs_int_delay);
    
    txdctl = intelReadMem32(E1000_TXDCTL(0));

    /* erratum work around: set txdctl the same for both queues */
    intelWriteMem32(E1000_TXDCTL(1), txdctl);

	/* Program the Transmit Control Register */
	tctl = intelReadMem32(E1000_TCTL);
	tctl &= ~E1000_TCTL_CT;
	tctl |= E1000_TCTL_PSP | E1000_TCTL_RTLC |
    (E1000_COLLISION_THRESHOLD << E1000_CT_SHIFT);
    
	/* errata: program both queues to unweighted RR */
	if (adapter->flags & FLAG_TARC_SET_BIT_ZERO) {
		tarc = intelReadMem32(E1000_TARC(0));
		tarc |= 1;
		intelWriteMem32(E1000_TARC(0), tarc);
		tarc = intelReadMem32(E1000_TARC(1));
		tarc |= 1;
		intelWriteMem32(E1000_TARC(1), tarc);
	}
	intelWriteMem32(E1000_TCTL, tctl);
    
	hw->mac.ops.config_collision_dist(hw);
}

/**
 * intelSetupRxControl - configure the receive control registers
 * @adapter: Board private structure
 **/

void IntelMausi::intelSetupRxControl(struct e1000_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 rctl, rfctl;
    
	/* Workaround Si errata on PCHx - configure jumbo frame flow.
	 * If jumbo frames not set, program related MAC/PHY registers
	 * to h/w defaults
	 */
	if (hw->mac.type >= e1000_pch2lan) {
		s32 ret_val;
        
		if (mtu > ETH_DATA_LEN)
			ret_val = e1000_lv_jumbo_workaround_ich8lan(hw, true);
		else
			ret_val = e1000_lv_jumbo_workaround_ich8lan(hw, false);
        
		if (ret_val)
			DebugLog("Ethernet [IntelMausi]: failed to enable/disable jumbo frame workaround mode.\n");
	}
	/* Program MC offset vector base */
	rctl = intelReadMem32(E1000_RCTL);
	rctl &= ~(3 << E1000_RCTL_MO_SHIFT);
	rctl |= E1000_RCTL_BAM | E1000_RCTL_LBM_NO | E1000_RCTL_RDMTS_HALF | (adapter->hw.mac.mc_filter_type << E1000_RCTL_MO_SHIFT);
    
	/* Do not Store bad packets */
	rctl &= ~E1000_RCTL_SBP;
    
	/* Enable Long Packet receive */
	if (mtu <= ETH_DATA_LEN)
		rctl &= ~E1000_RCTL_LPE;
	else
		rctl |= E1000_RCTL_LPE;
    
	/* Some systems expect that the CRC is included in SMBUS traffic. The
	 * hardware strips the CRC before sending to both SMBUS (BMC) and to
	 * host memory when this is enabled
	 */
	if (adapter->flags2 & FLAG2_CRC_STRIPPING)
        rctl |= E1000_RCTL_SECRC;
    
	/* Workaround Si errata on 82577 PHY - configure IPG for jumbos */
	if ((hw->phy.type == e1000_phy_82577) && (rctl & E1000_RCTL_LPE)) {
		u16 phy_data;
        
		e1e_rphy(hw, PHY_REG(770, 26), &phy_data);
		phy_data &= 0xfff8;
		phy_data |= (1 << 2);
		e1e_wphy(hw, PHY_REG(770, 26), phy_data);
        
		e1e_rphy(hw, 22, &phy_data);
		phy_data &= 0x0fff;
		phy_data |= (1 << 14);
		e1e_wphy(hw, 0x10, 0x2823);
		e1e_wphy(hw, 0x11, 0x0003);
		e1e_wphy(hw, 22, phy_data);
	}
    
	/* Set buffer sizes to 2048 */
    //rctl |= (0x2 << E1000_RCTL_FLXB_SHIFT);
    rctl &= ~(E1000_RCTL_SZ_256 | E1000_RCTL_BSEX);
    
	/* Enable Extended Status in all Receive Descriptors */
	rfctl = intelReadMem32(E1000_RFCTL);
    rfctl |= (E1000_RFCTL_NEW_IPV6_EXT_DIS | E1000_RFCTL_IPV6_EX_DIS | E1000_RFCTL_EXTEN | E1000_RFCTL_NFSW_DIS | E1000_RFCTL_NFSR_DIS);
	intelWriteMem32(E1000_RFCTL, rfctl);
    
	intelWriteMem32(E1000_RCTL, rctl);
}

/**
 * intelConfigureRx - Configure Receive Unit after Reset
 * @adapter: board private structure
 *
 * Configure the Rx unit of the MAC after a reset.
 **/
void IntelMausi::intelConfigureRx(struct e1000_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	//struct e1000_ring *rx_ring = adapter->rx_ring;
	u32 rctl, rxcsum, ctrl_ext;
    
	/* disable receives while setting up the descriptors */
	rctl = intelReadMem32(E1000_RCTL);
    
	intelFlush();
	usleep_range(10000, 20000);
    
	/* set the Receive Delay Timer Register */
	intelWriteMem32(E1000_RDTR, adapter->rx_int_delay);
    
	/* irq moderation */
	intelWriteMem32(E1000_RADV, adapter->rx_abs_int_delay);
    
    /* Set interrupt throttle value. */
    intelWriteMem32(E1000_ITR, intrThrValue);
    
    /* Auto-Mask interrupts upon ICR access. */
	ctrl_ext = intelReadMem32(E1000_CTRL_EXT);
	ctrl_ext |= E1000_CTRL_EXT_IAME;
	intelWriteMem32(E1000_IAM, 0xffffffff);
	intelWriteMem32(E1000_CTRL_EXT, ctrl_ext);
	e1e_flush();
    
	/* Setup the HW Rx Head and Tail Descriptor Pointers and
	 * the Base and Length of the Rx Descriptor Ring
	 */
    intelInitRxRing();
    
	/* Enable Receive Checksum Offload for TCP and UDP */
	rxcsum = intelReadMem32(E1000_RXCSUM);
    rxcsum |= E1000_RXCSUM_TUOFL;
	intelWriteMem32(E1000_RXCSUM, rxcsum);
    
	/* With jumbo frames, excessive C-state transition latencies result
	 * in dropped transactions.
	 */
	if (mtu > ETH_DATA_LEN) {
		//u32 lat = ((intelReadMem32(E1000_PBA) & E1000_PBA_RXA_MASK) * 1024 - adapter->max_frame_size) * 8 / 1000;
        
		if (adapter->flags & FLAG_IS_ICH) {
			u32 rxdctl = intelReadMem32(E1000_RXDCTL(0));
            
			intelWriteMem32(E1000_RXDCTL(0), rxdctl | 0x3);
		}
        
		//pm_qos_update_request(&adapter->netdev->pm_qos_req, lat);
	} else {
		//pm_qos_update_request(&adapter->netdev->pm_qos_req, PM_QOS_DEFAULT_VALUE);
	}
	intelWriteMem32(E1000_RCTL, rctl);
}

/**
 * intelDown - quiesce the device and optionally reset the hardware
 * @adapter: board private structure
 * @reset: boolean flag to reset the hardware or not
 */
void IntelMausi::intelDown(struct e1000_adapter *adapter, bool reset)
{
	struct e1000_hw *hw = &adapter->hw;
	UInt32 tctl, rctl;
    
	/* signal that we're down so the interrupt handler does not
	 * reschedule our watchdog timer
	 */
	set_bit(__E1000_DOWN, &adapter->state);
    
	/* disable receives in the hardware */
	rctl = intelReadMem32(E1000_RCTL);
    rctl &= ~E1000_RCTL_EN;
	intelWriteMem32(E1000_RCTL, rctl);
    
	/* flush and sleep below */
    
	/* disable transmits in the hardware */
	tctl = intelReadMem32(E1000_TCTL);
	tctl &= ~E1000_TCTL_EN;
	intelWriteMem32(E1000_TCTL, tctl);
    
	/* flush both disables and wait for them to finish */
	intelFlush();
	usleep_range(10000, 20000);
    
	intelDisableIRQ();
    updateStatistics(adapter);
    clearDescriptors();
    
	adapter->link_speed = 0;
	adapter->link_duplex = 0;
    
	/* Disable Si errata workaround on PCHx for jumbo frame flow */
	if ((hw->mac.type >= e1000_pch2lan) && (mtu > ETH_DATA_LEN) && e1000_lv_jumbo_workaround_ich8lan(hw, false))
		DebugLog("Ethernet [IntelMausi]: failed to disable jumbo frame workaround mode\n");
    
	if (reset)
		intelReset(adapter);
}

void IntelMausi::intelInitManageabilityPt(struct e1000_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 manc, manc2h, mdef, i, j;
    
	if (!(adapter->flags & FLAG_MNG_PT_ENABLED))
		return;
    
	manc = intelReadMem32(E1000_MANC);
    
	/* enable receiving management packets to the host. this will probably
	 * generate destination unreachable messages from the host OS, but
	 * the packets will be handled on SMBUS
	 */
	manc |= E1000_MANC_EN_MNG2HOST;
	manc2h = intelReadMem32(E1000_MANC2H);
    
	switch (hw->mac.type) {
        default:
            manc2h |= (E1000_MANC2H_PORT_623 | E1000_MANC2H_PORT_664);
            break;
        case e1000_82574:
        case e1000_82583:
            /* Check if IPMI pass-through decision filter already exists;
             * if so, enable it.
             */
            for (i = 0, j = 0; i < 8; i++) {
                mdef = intelReadMem32(E1000_MDEF(i));
                
                /* Ignore filters with anything other than IPMI ports */
                if (mdef & ~(E1000_MDEF_PORT_623 | E1000_MDEF_PORT_664))
                    continue;
                
                /* Enable this decision filter in MANC2H */
                if (mdef)
                    manc2h |= (1 << i);
                
                j |= mdef;
            }
            
            if (j == (E1000_MDEF_PORT_623 | E1000_MDEF_PORT_664))
                break;
            
            /* Create new decision filter in an empty filter */
            for (i = 0, j = 0; i < 8; i++)
                if (intelReadMem32(E1000_MDEF(i)) == 0) {
                    intelWriteMem32(E1000_MDEF(i), (E1000_MDEF_PORT_623 | E1000_MDEF_PORT_664));
                    manc2h |= (1 << 1);
                    j++;
                    break;
                }
            
            if (!j)
                IOLog("Ethernet [IntelMausi]: Unable to create IPMI pass-through filter.\n");
            break;
	}
	intelWriteMem32(E1000_MANC2H, manc2h);
	intelWriteMem32(E1000_MANC, manc);
}

/**
 * intelReset - bring the hardware into a known good state
 *
 * This function boots the hardware and enables some settings that
 * require a configuration cycle of the hardware - those cannot be
 * set/changed during runtime. After reset the device needs to be
 * properly configured for Rx, Tx etc.
 */
void IntelMausi::intelReset(struct e1000_adapter *adapter)
{
	struct e1000_mac_info *mac = &adapter->hw.mac;
	struct e1000_fc_info *fc = &adapter->hw.fc;
	struct e1000_hw *hw = &adapter->hw;
	u32 tx_space, min_tx_space, min_rx_space;
	u32 pba = adapter->pba;
	u16 hwm;
    
	/* reset Packet Buffer Allocation to default */
	intelWriteMem32(E1000_PBA, pba);
    
	if (adapter->max_frame_size > ETH_FRAME_LEN + ETH_FCS_LEN) {
		/* To maintain wire speed transmits, the Tx FIFO should be
		 * large enough to accommodate two full transmit packets,
		 * rounded up to the next 1KB and expressed in KB.  Likewise,
		 * the Rx FIFO should be large enough to accommodate at least
		 * one full receive packet and is similarly rounded up and
		 * expressed in KB.
		 */
		pba = intelReadMem32(E1000_PBA);
		/* upper 16 bits has Tx packet buffer allocation size in KB */
		tx_space = pba >> 16;
		/* lower 16 bits has Rx packet buffer allocation size in KB */
		pba &= 0xffff;
		/* the Tx fifo also stores 16 bytes of information about the Tx
		 * but don't include ethernet FCS because hardware appends it
		 */
		min_tx_space = (adapter->max_frame_size +
                        sizeof(struct e1000_tx_desc) - ETH_FCS_LEN) * 2;
		min_tx_space = ALIGN(min_tx_space, 1024);
		min_tx_space >>= 10;
		/* software strips receive CRC, so leave room for it */
		min_rx_space = adapter->max_frame_size;
		min_rx_space = ALIGN(min_rx_space, 1024);
		min_rx_space >>= 10;
        
		/* If current Tx allocation is less than the min Tx FIFO size,
		 * and the min Tx FIFO size is less than the current Rx FIFO
		 * allocation, take space away from current Rx allocation
		 */
		if ((tx_space < min_tx_space) &&
		    ((min_tx_space - tx_space) < pba)) {
			pba -= min_tx_space - tx_space;
            
			/* if short on Rx space, Rx wins and must trump Tx
			 * adjustment
			 */
			if (pba < min_rx_space)
				pba = min_rx_space;
		}
		intelWriteMem32(E1000_PBA, pba);
	}
    
	/* flow control settings
	 *
	 * The high water mark must be low enough to fit one full frame
	 * (or the size used for early receive) above it in the Rx FIFO.
	 * Set it to the lower of:
	 * - 90% of the Rx FIFO size, and
	 * - the full Rx FIFO size minus one full frame
	 */
	if (adapter->flags & FLAG_DISABLE_FC_PAUSE_TIME)
		fc->pause_time = 0xFFFF;
	else
		fc->pause_time = E1000_FC_PAUSE_TIME;
	fc->send_xon = true;
	fc->current_mode = fc->requested_mode;
    
	switch (hw->mac.type) {
        case e1000_ich9lan:
        case e1000_ich10lan:
            if (mtu > ETH_DATA_LEN) {
                pba = 14;
                intelWriteMem32(E1000_PBA, pba);
                fc->high_water = 0x2800;
                fc->low_water = fc->high_water - 8;
                break;
            }
            /* fall-through */
        default:
            hwm = min(((pba << 10) * 9 / 10),
                      ((pba << 10) - adapter->max_frame_size));
            
            fc->high_water = hwm & E1000_FCRTH_RTH;	/* 8-byte granularity */
            fc->low_water = fc->high_water - 8;
            break;
        case e1000_pchlan:
            /* Workaround PCH LOM adapter hangs with certain network
             * loads.  If hangs persist, try disabling Tx flow control.
             */
            if (mtu > ETH_DATA_LEN) {
                fc->high_water = 0x3500;
                fc->low_water = 0x1500;
            } else {
                fc->high_water = 0x5000;
                fc->low_water = 0x3000;
            }
            fc->refresh_time = 0x1000;
            break;
        case e1000_pch2lan:
        case e1000_pch_lpt:
            fc->refresh_time = 0x0400;
            
            if (mtu <= ETH_DATA_LEN) {
                fc->high_water = 0x05C20;
                fc->low_water = 0x05048;
                fc->pause_time = 0x0650;
                break;
            }
            
            pba = 14;
            intelWriteMem32(E1000_PBA, pba);
            fc->high_water = ((pba << 10) * 9 / 10) & E1000_FCRTH_RTH;
            fc->low_water = ((pba << 10) * 8 / 10) & E1000_FCRTL_RTL;
            break;
	}
    
	/* Alignment of Tx data is on an arbitrary byte boundary with the
	 * maximum size per Tx descriptor limited only to the transmit
	 * allocation of the packet buffer minus 96 bytes with an upper
	 * limit of 24KB due to receive synchronization limitations.
	 */
	adapter->tx_fifo_limit = min_t(u32, ((intelReadMem32(E1000_PBA) >> 16) << 10) - 96,
                                   24 << 10);
    
    /* Set interrupt throttle value. */
    intelWriteMem32(E1000_ITR, intrThrValue);
    
	/* Allow time for pending master requests to run */
	mac->ops.reset_hw(hw);
    
	/* For parts with AMT enabled, let the firmware know
	 * that the network interface is in control
	 */
	if (adapter->flags & FLAG_HAS_AMT)
		e1000e_get_hw_control(adapter);
    
	intelWriteMem32(E1000_WUC, 0);
    
	if (mac->ops.init_hw(hw))
		IOLog("Ethernet [IntelMausi]: Hardware Error.\n");
    
	//e1000_update_mng_vlan(adapter);
    
	/* Enable h/w to recognize an 802.1Q VLAN Ethernet packet */
	intelWriteMem32(E1000_VET, ETH_P_8021Q);
    
	intelResetAdaptive(hw);
    
	/* initialize systim and reset the ns time counter */
	//e1000e_config_hwtstamp(adapter, &adapter->hwtstamp_config);
    
	/* Set EEE advertisement as appropriate */
	if (adapter->flags2 & FLAG2_HAS_EEE) {
		s32 ret_val;
		u16 adv_addr;
        
		switch (hw->phy.type) {
            case e1000_phy_82579:
                adv_addr = I82579_EEE_ADVERTISEMENT;
                break;
            case e1000_phy_i217:
                adv_addr = I217_EEE_ADVERTISEMENT;
                break;
            default:
                IOLog("Ethernet [IntelMausi]: Invalid PHY type setting EEE advertisement.\n");
                return;
		}
        
		ret_val = hw->phy.ops.acquire(hw);
        
		if (ret_val) {
			IOLog("Ethernet [IntelMausi]: EEE advertisement - unable to acquire PHY.\n");
			return;
		}
		e1000_write_emi_reg_locked(hw, adv_addr, hw->dev_spec.ich8lan.eee_disable ? 0 : adapter->eee_advert);
		hw->phy.ops.release(hw);
	}
	e1000_get_phy_info(hw);
    
	if ((adapter->flags & FLAG_HAS_SMART_POWER_DOWN) && !(adapter->flags & FLAG_SMART_POWER_DOWN)) {
		u16 phy_data = 0;
		/* speed up time to link by disabling smart power down, ignore
		 * the return value of this function because there is nothing
		 * different we would do if it failed
		 */
		e1e_rphy(hw, IGP02E1000_PHY_POWER_MGMT, &phy_data);
		phy_data &= ~IGP02E1000_PM_SPD;
		e1e_wphy(hw, IGP02E1000_PHY_POWER_MGMT, phy_data);
	}
}

/**
 * intelPowerDownPhy - Power down the PHY
 *
 * Power down the PHY so no link is implied when interface is down.
 * The PHY cannot be powered down if management or WoL is active.
 */
void IntelMausi::intelPowerDownPhy(struct e1000_adapter *adapter)
{
	if (adapter->hw.phy.ops.power_down)
		adapter->hw.phy.ops.power_down(&adapter->hw);
}

/**
 *  intelEnableMngPassThru - Check if management passthrough is needed
 *  @hw: pointer to the HW structure
 *
 *  Verifies the hardware needs to leave interface enabled so that frames can
 *  be directed to and from the management interface.
 **/
bool IntelMausi::intelEnableMngPassThru(struct e1000_hw *hw)
{
	u32 manc;
	u32 fwsm, factps;
    
	manc = intelReadMem32(E1000_MANC);
    
	if (!(manc & E1000_MANC_RCV_TCO_EN))
		return false;
    
	if (hw->mac.has_fwsm) {
		fwsm = intelReadMem32(E1000_FWSM);
		factps = intelReadMem32(E1000_FACTPS);
        
		if (!(factps & E1000_FACTPS_MNGCG) &&
		    ((fwsm & E1000_FWSM_MODE_MASK) ==
		     (e1000_mng_mode_pt << E1000_FWSM_MODE_SHIFT)))
			return true;
	} else if ((hw->mac.type == e1000_82574) ||
               (hw->mac.type == e1000_82583)) {
		u16 data;
		s32 ret_val;
        
		factps = intelReadMem32(E1000_FACTPS);
		ret_val = e1000_read_nvm(hw, NVM_INIT_CONTROL2_REG, 1, &data);
		if (ret_val)
			return false;
        
		if (!(factps & E1000_FACTPS_MNGCG) &&
		    ((data & E1000_NVM_INIT_CTRL2_MNGM) ==
		     (e1000_mng_mode_pt << 13)))
			return true;
	} else if ((manc & E1000_MANC_SMBUS_EN) &&
               !(manc & E1000_MANC_ASF_EN)) {
		return true;
	}
    
	return false;
}

/**
 *  intelResetAdaptive - Reset Adaptive Interframe Spacing
 *  @hw: pointer to the HW structure
 *
 *  Reset the Adaptive Interframe Spacing throttle to default values.
 **/
void IntelMausi::intelResetAdaptive(struct e1000_hw *hw)
{
	struct e1000_mac_info *mac = &hw->mac;
    
	if (!mac->adaptive_ifs) {
		DebugLog("Ethernet [IntelMausi]: Not in Adaptive IFS mode!\n");
		return;
	}
	mac->current_ifs_val = 0;
	mac->ifs_min_val = IFS_MIN;
	mac->ifs_max_val = IFS_MAX;
	mac->ifs_step_size = IFS_STEP;
	mac->ifs_ratio = IFS_RATIO;
    
	mac->in_ifs_mode = false;
	intelWriteMem32(E1000_AIT, 0);
}

/**
 *  intelUpdateAdaptive - Update Adaptive Interframe Spacing
 *  @hw: pointer to the HW structure
 *
 *  Update the Adaptive Interframe Spacing Throttle value based on the
 *  time between transmitted packets and time between collisions.
 **/
void IntelMausi::intelUpdateAdaptive(struct e1000_hw *hw)
{
	struct e1000_mac_info *mac = &hw->mac;
    
	if (!mac->adaptive_ifs) {
		DebugLog("Ethernet [IntelMausi]: Not in Adaptive IFS mode!\n");
		return;
	}
    
	if ((mac->collision_delta * mac->ifs_ratio) > mac->tx_packet_delta) {
		if (mac->tx_packet_delta > MIN_NUM_XMITS) {
			mac->in_ifs_mode = true;
			if (mac->current_ifs_val < mac->ifs_max_val) {
				if (!mac->current_ifs_val)
					mac->current_ifs_val = mac->ifs_min_val;
				else
					mac->current_ifs_val += mac->ifs_step_size;
                
				intelWriteMem32(E1000_AIT, mac->current_ifs_val);
			}
		}
	} else {
		if (mac->in_ifs_mode && (mac->tx_packet_delta <= MIN_NUM_XMITS)) {
			mac->current_ifs_val = 0;
			mac->in_ifs_mode = false;
			intelWriteMem32(E1000_AIT, 0);
		}
	}
}

/**
 * intelVlanStripDisable - helper to disable HW VLAN stripping
 * @adapter: board private structure to initialize
 **/
void IntelMausi::intelVlanStripDisable(struct e1000_adapter *adapter)
{
	UInt32 ctrl;
    
	/* disable VLAN tag insert/strip */
	ctrl = intelReadMem32(E1000_CTRL);
	ctrl &= ~E1000_CTRL_VME;
	intelWriteMem32(E1000_CTRL, ctrl);
}

/**
 * intelVlanStripEnable - helper to enable HW VLAN stripping
 * @adapter: board private structure to initialize
 **/
void IntelMausi::intelVlanStripEnable(struct e1000_adapter *adapter)
{
	UInt32 ctrl;
    
	/* enable VLAN tag insert/strip */
	ctrl = intelReadMem32(E1000_CTRL);
	ctrl |= E1000_CTRL_VME;
	intelWriteMem32(E1000_CTRL, ctrl);
}

static const u32 rsskey[10] = {
    0xda565a6d, 0xc20e5b25, 0x3d256741, 0xb08fa343, 0xcb2bcad0,
    0xb4307bae, 0xa32dcb77, 0x0cf23080, 0x3bb7426a, 0xfa01acbe
};

void IntelMausi::intelSetupRssHash(struct e1000_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	u32 mrqc, rxcsum;
	int i;
    
	for (i = 0; i < 10; i++)
		ew32(RSSRK(i), rsskey[i]);
    
	/* Direct all traffic to queue 0 */
	for (i = 0; i < 32; i++)
		ew32(RETA(i), 0);
    
	/* Disable raw packet checksumming so that RSS hash is placed in
	 * descriptor on writeback.
	 */
	rxcsum = er32(RXCSUM);
	rxcsum |= E1000_RXCSUM_PCSD;
    
	ew32(RXCSUM, rxcsum);
    
	mrqc = (E1000_MRQC_RSS_FIELD_IPV4 |
            E1000_MRQC_RSS_FIELD_IPV4_TCP |
            E1000_MRQC_RSS_FIELD_IPV6 |
            E1000_MRQC_RSS_FIELD_IPV6_TCP |
            E1000_MRQC_RSS_FIELD_IPV6_TCP_EX);
    
    mrqc |= 0x01;
	ew32(MRQC, mrqc);
}


/* Reset the NIC in case a tx deadlock or a pci error occurred. timerSource and txQueue
 * are stopped immediately but will be restarted by checkLinkStatus() when the link has
 * been reestablished.
 */

void IntelMausi::intelRestart()
{
    /* Stop and cleanup txQueue. Also set the link status to down. */
    txQueue->stop();
    txQueue->flush();
    linkUp = false;
    setLinkStatus(kIONetworkLinkValid);
    
    /* Reset NIC and cleanup both descriptor rings. */
    intelDisableIRQ();
	intelReset(&adapterData);
    
    clearDescriptors();
    rxCleanedCount = rxNextDescIndex = 0;
    deadlockWarn = 0;
    forceReset = false;
    enableEEE = false;
    adapterData.phy_hang_count = 0;

    /* Reinitialize NIC. */
    intelConfigure(&adapterData);
    
    /* From here on the code is the same as e1000e_up() */
	clear_bit(__E1000_DOWN, &adapterData.state);
    
	intelEnableIRQ(intrMask);
    
	adapterData.tx_hang_recheck = false;
    
	adapterData.hw.mac.get_link_status = true;
}

void IntelMausi::intelInitTxRing()
{
	intelWriteMem32(E1000_TDBAL(0), (txPhyAddr & 0xffffffff));
	intelWriteMem32(E1000_TDBAH(0), (txPhyAddr >> 32));
	intelWriteMem32(E1000_TDLEN(0), kTxDescSize);
	intelWriteMem32(E1000_TDH(0), 0);
	intelWriteMem32(E1000_TDT(0), 0);
    txNextDescIndex = txDirtyIndex = txCleanBarrierIndex = 0;
    txNumFreeDesc = kNumTxDesc;
}

void IntelMausi::intelInitRxRing()
{
	intelWriteMem32(E1000_RDBAL(0), (rxPhyAddr & 0xffffffff));
	intelWriteMem32(E1000_RDBAH(0), (rxPhyAddr >> 32));
	intelWriteMem32(E1000_RDLEN(0), kRxDescSize);
	intelWriteMem32(E1000_RDH(0), 0);
    
    if (adapterData.flags2 & FLAG2_PCIM2PCI_ARBITER_WA)
        intelUpdateRxDescTail(kRxLastDesc);
    else
        intelWriteMem32(E1000_RDT(0), kRxLastDesc);
    
    rxCleanedCount = rxNextDescIndex = 0;
}

void IntelMausi::intelUpdateTxDescTail(UInt32 index)
{
    struct e1000_hw *hw = &adapterData.hw;
    s32 ret = __ew32_prepare(hw);
    
    intelWriteMem32(E1000_TDT(0),index);
    
    if (!ret && (index != intelReadMem32(E1000_TDT(0)))) {
        UInt32 tctl = intelReadMem32(E1000_TCTL);
        
        intelWriteMem32(E1000_TCTL, tctl & ~E1000_TCTL_EN);
        forceReset = true;
        
        IOLog("Ethernet [IntelMausi]: ME firmware caused invalid TDT - resetting.\n");
    }
}

void IntelMausi::intelUpdateRxDescTail(UInt32 index)
{
    struct e1000_hw *hw = &adapterData.hw;
    SInt32 ret = __ew32_prepare(hw);
    
    intelWriteMem32(E1000_RDT(0),index);
    
    if (!ret && (index != intelReadMem32(E1000_RDT(0)))) {
        UInt32 rctl = intelReadMem32(E1000_RCTL);
        
        intelWriteMem32(E1000_RCTL, rctl & ~E1000_RCTL_EN);
        forceReset = true;
        
        IOLog("Ethernet [IntelMausi]: ME firmware caused invalid RDT - resetting.\n");
    }
}

inline void IntelMausi::intelEnablePCIDevice(IOPCIDevice *provider)
{
    UInt16 cmdReg;
    
    cmdReg = provider->configRead16(kIOPCIConfigCommand);
    cmdReg |= (kIOPCICommandBusMaster | kIOPCICommandMemorySpace | kIOPCICommandMemWrInvalidate);
    cmdReg &= ~kIOPCICommandIOSpace;
	provider->configWrite16(kIOPCIConfigCommand, cmdReg);
}

bool IntelMausi::intelCheckLink(struct e1000_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	bool link_active = false;
	s32 ret_val = 0;
    
	/* get_link_status is set on LSC (link status) interrupt or
	 * Rx sequence error interrupt.  get_link_status will stay
	 * false until the check_for_link establishes link
	 * for copper adapters ONLY
	 */
	switch (hw->phy.media_type) {
        case e1000_media_type_copper:
            if (hw->mac.get_link_status) {
                ret_val = hw->mac.ops.check_for_link(hw);
                link_active = !hw->mac.get_link_status;
            } else {
                link_active = true;
            }
            break;
        case e1000_media_type_fiber:
            ret_val = hw->mac.ops.check_for_link(hw);
            link_active = !!(intelReadMem32(E1000_STATUS) & E1000_STATUS_LU);
            break;
        case e1000_media_type_internal_serdes:
            ret_val = hw->mac.ops.check_for_link(hw);
            link_active = adapter->hw.mac.serdes_has_link;
            break;
        default:
        case e1000_media_type_unknown:
            break;
	}
	if ((ret_val == E1000_ERR_PHY) && (hw->phy.type == e1000_phy_igp_3) &&
	    (intelReadMem32(E1000_CTRL) & E1000_PHY_CTRL_GBE_DISABLE)) {
		/* See e1000_kmrn_lock_loss_workaround_ich8lan() */
		IOLog("Ethernet [IntelMausi]: Gigabit has been disabled, downgrading speed.\n");
	}
	return link_active;
}

/**
 * intelPhyReadStatus - Update the PHY register status snapshot
 * @adapter: board private structure
 **/
void IntelMausi::intelPhyReadStatus(struct e1000_adapter *adapter)
{
	struct e1000_hw *hw = &adapter->hw;
	struct e1000_phy_regs *phy = &adapter->phy_regs;
    
	if ((intelReadMem32(E1000_STATUS) & E1000_STATUS_LU) &&
	    (adapter->hw.phy.media_type == e1000_media_type_copper)) {
		int ret_val;
        
		ret_val = e1e_rphy(hw, MII_BMCR, &phy->bmcr);
		ret_val |= e1e_rphy(hw, MII_BMSR, &phy->bmsr);
		ret_val |= e1e_rphy(hw, MII_ADVERTISE, &phy->advertise);
		ret_val |= e1e_rphy(hw, MII_LPA, &phy->lpa);
		ret_val |= e1e_rphy(hw, MII_EXPANSION, &phy->expansion);
		ret_val |= e1e_rphy(hw, MII_CTRL1000, &phy->ctrl1000);
		ret_val |= e1e_rphy(hw, MII_STAT1000, &phy->stat1000);
		ret_val |= e1e_rphy(hw, MII_ESTATUS, &phy->estatus);
        
		if (ret_val)
			IOLog("Ethernet [IntelMausi]: Error reading PHY register\n");
	} else {
		/* Do not read PHY registers if link is not up
		 * Set values to typical power-on defaults
		 */
		phy->bmcr = (BMCR_SPEED1000 | BMCR_ANENABLE | BMCR_FULLDPLX);
		phy->bmsr = (BMSR_100FULL | BMSR_100HALF | BMSR_10FULL |
                     BMSR_10HALF | BMSR_ESTATEN | BMSR_ANEGCAPABLE |
                     BMSR_ERCAP);
		phy->advertise = (ADVERTISE_PAUSE_ASYM | ADVERTISE_PAUSE_CAP |
                          ADVERTISE_ALL | ADVERTISE_CSMA);
		phy->lpa = 0;
		phy->expansion = EXPANSION_ENABLENPAGE;
		phy->ctrl1000 = ADVERTISE_1000FULL;
		phy->stat1000 = 0;
		phy->estatus = (ESTATUS_1000_TFULL | ESTATUS_1000_THALF);
	}
}