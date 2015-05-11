/******************************************************************************
 *
 * Copyright(c) 2009-2013  Realtek Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 * The full GNU General Public License is included in this distribution in the
 * file called LICENSE.
 *
 * Contact Information:
 * wlanfae <wlanfae@realtek.com>
 * Realtek Corporation, No. 2, Innovation Road II, Hsinchu Science Park,
 * Hsinchu 300, Taiwan.
 *
 * Larry Finger <Larry.Finger@lwfinger.net>
 *
 *****************************************************************************/

#include "../wifi.h"
#include "../pci.h"
#include "../ps.h"
#include "reg.h"
#include "def.h"
#include "phy.h"
#include "rf.h"
#include "dm.h"
#include "table.h"

static void set_baseband_phy_config(struct ieee80211_hw *hw);
static void set_baseband_agc_config(struct ieee80211_hw *hw);
static void store_pwrindex_offset(struct ieee80211_hw *hw,
				  u32 regaddr, u32 bitmask,
				  u32 data);
static bool check_cond(struct ieee80211_hw *hw, const u32  condition);

static u32 rf_serial_read(struct ieee80211_hw *hw,
			  enum radio_path rfpath, u32 offset)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	struct bb_reg_def *phreg = &rtlphy->phyreg_def[rfpath];
	u32 newoffset;
	u32 tmplong, tmplong2;
	u8 rfpi_enable = 0;
	u32 ret;
	int jj = RF90_PATH_A;
	int kk = RF90_PATH_B;

	offset &= 0xff;
	newoffset = offset;
	if (RT_CANNOT_IO(hw)) {
		RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG, "return all one\n");
		return 0xFFFFFFFF;
	}
	tmplong = rtl_get_bbreg(hw, RFPGA0_XA_HSSIPARAMETER2, MASKDWORD);
	if (rfpath == jj)
		tmplong2 = tmplong;
	else
		tmplong2 = rtl_get_bbreg(hw, phreg->rfhssi_para2, MASKDWORD);
	tmplong2 = (tmplong2 & (~BLSSIREADADDRESS)) |
	    (newoffset << 23) | BLSSIREADEDGE;
	rtl_set_bbreg(hw, RFPGA0_XA_HSSIPARAMETER2, MASKDWORD,
		      tmplong & (~BLSSIREADEDGE));
	mdelay(1);
	rtl_set_bbreg(hw, phreg->rfhssi_para2, MASKDWORD, tmplong2);
	mdelay(2);
	if (rfpath == jj)
		rfpi_enable = (u8) rtl_get_bbreg(hw, RFPGA0_XA_HSSIPARAMETER1,
						 BIT(8));
	else if (rfpath == kk)
		rfpi_enable = (u8) rtl_get_bbreg(hw, RFPGA0_XB_HSSIPARAMETER1,
						 BIT(8));
	if (rfpi_enable)
		ret = rtl_get_bbreg(hw, phreg->rf_rbpi, BLSSIREADBACKDATA);
	else
		ret = rtl_get_bbreg(hw, phreg->rf_rb, BLSSIREADBACKDATA);
	RT_TRACE(rtlpriv, COMP_RF, DBG_TRACE, "RFR-%d Addr[0x%x]= 0x%x\n",
		 rfpath, phreg->rf_rb, ret);
	return ret;
}

static void rf_serial_write(struct ieee80211_hw *hw,
			    enum radio_path rfpath, u32 offset,
			    u32 data)
{
	u32 data_and_addr;
	u32 newoffset;
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	struct bb_reg_def *phreg = &rtlphy->phyreg_def[rfpath];

	if (RT_CANNOT_IO(hw)) {
		RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG, "stop\n");
		return;
	}
	offset &= 0xff;
	newoffset = offset;
	data_and_addr = ((newoffset << 20) | (data & 0x000fffff)) & 0x0fffffff;
	rtl_set_bbreg(hw, phreg->rf3wire_offset, MASKDWORD, data_and_addr);
	RT_TRACE(rtlpriv, COMP_RF, DBG_TRACE, "RFW-%d Addr[0x%x]= 0x%x\n",
		 rfpath, phreg->rf3wire_offset, data_and_addr);
}

static u32 cal_bit_shift(u32 bitmask)
{
	u32 i;

	for (i = 0; i <= 31; i++) {
		if (((bitmask >> i) & 0x1) == 1)
			break;
	}
	return i;
}

static bool config_bb_with_header(struct ieee80211_hw *hw,
				  u8 configtype)
{
	if (configtype == BASEBAND_CONFIG_PHY_REG)
		set_baseband_phy_config(hw);
	else if (configtype == BASEBAND_CONFIG_AGC_TAB)
		set_baseband_agc_config(hw);
	return true;
}

static bool config_bb_with_pgheader(struct ieee80211_hw *hw,
				    u8 configtype)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	int i;
	u32 *table_pg;
	u16 tbl_page_len;
	u32 v1 = 0, v2 = 0;

	tbl_page_len = RTL8188EEPHY_REG_ARRAY_PGLEN;
	table_pg = RTL8188EEPHY_REG_ARRAY_PG;

	if (configtype == BASEBAND_CONFIG_PHY_REG) {
		for (i = 0; i < tbl_page_len; i = i + 3) {
			v1 = table_pg[i];
			v2 = table_pg[i + 1];

			if (v1 < 0xcdcdcdcd) {
				if (table_pg[i] == 0xfe)
					mdelay(50);
				else if (table_pg[i] == 0xfd)
					mdelay(5);
				else if (table_pg[i] == 0xfc)
					mdelay(1);
				else if (table_pg[i] == 0xfb)
					udelay(50);
				else if (table_pg[i] == 0xfa)
					udelay(5);
				else if (table_pg[i] == 0xf9)
					udelay(1);

				store_pwrindex_offset(hw, table_pg[i],
						      table_pg[i + 1],
						      table_pg[i + 2]);
				continue;
			} else {
				if (!check_cond(hw, table_pg[i])) {
					/*don't need the hw_body*/
					i += 2; /* skip the pair of expression*/
					v1 = table_pg[i];
					v2 = table_pg[i + 1];
					while (v2 != 0xDEAD) {
						i += 3;
						v1 = table_pg[i];
						v2 = table_pg[i + 1];
					}
				}
			}
		}
	} else {
		RT_TRACE(rtlpriv, COMP_SEND, DBG_TRACE,
			 "configtype != BaseBand_Config_PHY_REG\n");
	}
	return true;
}

static bool config_parafile(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	struct rtl_efuse *fuse = rtl_efuse(rtl_priv(hw));
	bool rtstatus;

	rtstatus = config_bb_with_header(hw, BASEBAND_CONFIG_PHY_REG);
	if (rtstatus != true) {
		RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG, "Write BB Reg Fail!!");
		return false;
	}

	if (fuse->autoload_failflag == false) {
		rtlphy->pwrgroup_cnt = 0;
		rtstatus = config_bb_with_pgheader(hw, BASEBAND_CONFIG_PHY_REG);
	}
	if (rtstatus != true) {
		RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG, "BB_PG Reg Fail!!");
		return false;
	}
	rtstatus = config_bb_with_header(hw, BASEBAND_CONFIG_AGC_TAB);
	if (rtstatus != true) {
		RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG, "AGC Table Fail\n");
		return false;
	}
	rtlphy->cck_high_power = (bool) (rtl_get_bbreg(hw,
				 RFPGA0_XA_HSSIPARAMETER2, 0x200));

	return true;
}

static void rtl88e_phy_init_bb_rf_register_definition(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	int jj = RF90_PATH_A;
	int kk = RF90_PATH_B;

	rtlphy->phyreg_def[jj].rfintfs = RFPGA0_XAB_RFINTERFACESW;
	rtlphy->phyreg_def[kk].rfintfs = RFPGA0_XAB_RFINTERFACESW;
	rtlphy->phyreg_def[RF90_PATH_C].rfintfs = RFPGA0_XCD_RFINTERFACESW;
	rtlphy->phyreg_def[RF90_PATH_D].rfintfs = RFPGA0_XCD_RFINTERFACESW;

	rtlphy->phyreg_def[jj].rfintfi = RFPGA0_XAB_RFINTERFACERB;
	rtlphy->phyreg_def[kk].rfintfi = RFPGA0_XAB_RFINTERFACERB;
	rtlphy->phyreg_def[RF90_PATH_C].rfintfi = RFPGA0_XCD_RFINTERFACERB;
	rtlphy->phyreg_def[RF90_PATH_D].rfintfi = RFPGA0_XCD_RFINTERFACERB;

	rtlphy->phyreg_def[jj].rfintfo = RFPGA0_XA_RFINTERFACEOE;
	rtlphy->phyreg_def[kk].rfintfo = RFPGA0_XB_RFINTERFACEOE;

	rtlphy->phyreg_def[jj].rfintfe = RFPGA0_XA_RFINTERFACEOE;
	rtlphy->phyreg_def[kk].rfintfe = RFPGA0_XB_RFINTERFACEOE;

	rtlphy->phyreg_def[jj].rf3wire_offset = RFPGA0_XA_LSSIPARAMETER;
	rtlphy->phyreg_def[kk].rf3wire_offset = RFPGA0_XB_LSSIPARAMETER;

	rtlphy->phyreg_def[jj].rflssi_select = rFPGA0_XAB_RFPARAMETER;
	rtlphy->phyreg_def[kk].rflssi_select = rFPGA0_XAB_RFPARAMETER;
	rtlphy->phyreg_def[RF90_PATH_C].rflssi_select = rFPGA0_XCD_RFPARAMETER;
	rtlphy->phyreg_def[RF90_PATH_D].rflssi_select = rFPGA0_XCD_RFPARAMETER;

	rtlphy->phyreg_def[jj].rftxgain_stage = RFPGA0_TXGAINSTAGE;
	rtlphy->phyreg_def[kk].rftxgain_stage = RFPGA0_TXGAINSTAGE;
	rtlphy->phyreg_def[RF90_PATH_C].rftxgain_stage = RFPGA0_TXGAINSTAGE;
	rtlphy->phyreg_def[RF90_PATH_D].rftxgain_stage = RFPGA0_TXGAINSTAGE;

	rtlphy->phyreg_def[jj].rfhssi_para1 = RFPGA0_XA_HSSIPARAMETER1;
	rtlphy->phyreg_def[kk].rfhssi_para1 = RFPGA0_XB_HSSIPARAMETER1;

	rtlphy->phyreg_def[jj].rfhssi_para2 = RFPGA0_XA_HSSIPARAMETER2;
	rtlphy->phyreg_def[kk].rfhssi_para2 = RFPGA0_XB_HSSIPARAMETER2;

	rtlphy->phyreg_def[jj].rfsw_ctrl = RFPGA0_XAB_SWITCHCONTROL;
	rtlphy->phyreg_def[kk].rfsw_ctrl = RFPGA0_XAB_SWITCHCONTROL;
	rtlphy->phyreg_def[RF90_PATH_C].rfsw_ctrl = RFPGA0_XCD_SWITCHCONTROL;
	rtlphy->phyreg_def[RF90_PATH_D].rfsw_ctrl = RFPGA0_XCD_SWITCHCONTROL;

	rtlphy->phyreg_def[jj].rfagc_control1 = ROFDM0_XAAGCCORE1;
	rtlphy->phyreg_def[kk].rfagc_control1 = ROFDM0_XBAGCCORE1;
	rtlphy->phyreg_def[RF90_PATH_C].rfagc_control1 = ROFDM0_XCAGCCORE1;
	rtlphy->phyreg_def[RF90_PATH_D].rfagc_control1 = ROFDM0_XDAGCCORE1;

	rtlphy->phyreg_def[jj].rfagc_control2 = ROFDM0_XAAGCCORE2;
	rtlphy->phyreg_def[kk].rfagc_control2 = ROFDM0_XBAGCCORE2;
	rtlphy->phyreg_def[RF90_PATH_C].rfagc_control2 = ROFDM0_XCAGCCORE2;
	rtlphy->phyreg_def[RF90_PATH_D].rfagc_control2 = ROFDM0_XDAGCCORE2;

	rtlphy->phyreg_def[jj].rfrxiq_imbal = ROFDM0_XARXIQIMBAL;
	rtlphy->phyreg_def[kk].rfrxiq_imbal = ROFDM0_XBRXIQIMBAL;
	rtlphy->phyreg_def[RF90_PATH_C].rfrxiq_imbal = ROFDM0_XCRXIQIMBAL;
	rtlphy->phyreg_def[RF90_PATH_D].rfrxiq_imbal = ROFDM0_XDRXIQIMBAL;

	rtlphy->phyreg_def[jj].rfrx_afe = ROFDM0_XARXAFE;
	rtlphy->phyreg_def[kk].rfrx_afe = ROFDM0_XBRXAFE;
	rtlphy->phyreg_def[RF90_PATH_C].rfrx_afe = ROFDM0_XCRXAFE;
	rtlphy->phyreg_def[RF90_PATH_D].rfrx_afe = ROFDM0_XDRXAFE;

	rtlphy->phyreg_def[jj].rftxiq_imbal = ROFDM0_XATXIQIMBAL;
	rtlphy->phyreg_def[kk].rftxiq_imbal = ROFDM0_XBTXIQIMBAL;
	rtlphy->phyreg_def[RF90_PATH_C].rftxiq_imbal = ROFDM0_XCTXIQIMBAL;
	rtlphy->phyreg_def[RF90_PATH_D].rftxiq_imbal = ROFDM0_XDTXIQIMBAL;

	rtlphy->phyreg_def[jj].rftx_afe = ROFDM0_XATXAFE;
	rtlphy->phyreg_def[kk].rftx_afe = ROFDM0_XBTXAFE;

	rtlphy->phyreg_def[jj].rf_rb = RFPGA0_XA_LSSIREADBACK;
	rtlphy->phyreg_def[kk].rf_rb = RFPGA0_XB_LSSIREADBACK;

	rtlphy->phyreg_def[jj].rf_rbpi = TRANSCEIVEA_HSPI_READBACK;
	rtlphy->phyreg_def[kk].rf_rbpi = TRANSCEIVEB_HSPI_READBACK;
}

static bool rtl88e_phy_set_sw_chnl_cmdarray(struct swchnlcmd *cmdtable,
					    u32 cmdtableidx, u32 cmdtablesz,
					    enum swchnlcmd_id cmdid,
					    u32 para1, u32 para2, u32 msdelay)
{
	struct swchnlcmd *pcmd;

	if (cmdtable == NULL) {
		RT_ASSERT(false, "cmdtable cannot be NULL.\n");
		return false;
	}

	if (cmdtableidx >= cmdtablesz)
		return false;

	pcmd = cmdtable + cmdtableidx;
	pcmd->cmdid = cmdid;
	pcmd->para1 = para1;
	pcmd->para2 = para2;
	pcmd->msdelay = msdelay;
	return true;
}

static bool chnl_step_by_step(struct ieee80211_hw *hw,
			      u8 channel, u8 *stage, u8 *step,
			      u32 *delay)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	struct swchnlcmd precommoncmd[MAX_PRECMD_CNT];
	u32 precommoncmdcnt;
	struct swchnlcmd postcommoncmd[MAX_POSTCMD_CNT];
	u32 postcommoncmdcnt;
	struct swchnlcmd rfdependcmd[MAX_RFDEPENDCMD_CNT];
	u32 rfdependcmdcnt;
	struct swchnlcmd *currentcmd = NULL;
	u8 rfpath;
	u8 num_total_rfpath = rtlphy->num_total_rfpath;

	precommoncmdcnt = 0;
	rtl88e_phy_set_sw_chnl_cmdarray(precommoncmd, precommoncmdcnt++,
					MAX_PRECMD_CNT,
					CMDID_SET_TXPOWEROWER_LEVEL, 0, 0, 0);
	rtl88e_phy_set_sw_chnl_cmdarray(precommoncmd, precommoncmdcnt++,
					MAX_PRECMD_CNT, CMDID_END, 0, 0, 0);

	postcommoncmdcnt = 0;

	rtl88e_phy_set_sw_chnl_cmdarray(postcommoncmd, postcommoncmdcnt++,
					MAX_POSTCMD_CNT, CMDID_END, 0, 0, 0);

	rfdependcmdcnt = 0;

	RT_ASSERT((channel >= 1 && channel <= 14),
		  "illegal channel for Zebra: %d\n", channel);

	rtl88e_phy_set_sw_chnl_cmdarray(rfdependcmd, rfdependcmdcnt++,
					MAX_RFDEPENDCMD_CNT, CMDID_RF_WRITEREG,
					RF_CHNLBW, channel, 10);

	rtl88e_phy_set_sw_chnl_cmdarray(rfdependcmd, rfdependcmdcnt++,
					MAX_RFDEPENDCMD_CNT, CMDID_END, 0, 0,
					 0);

	do {
		switch (*stage) {
		case 0:
			currentcmd = &precommoncmd[*step];
			break;
		case 1:
			currentcmd = &rfdependcmd[*step];
			break;
		case 2:
			currentcmd = &postcommoncmd[*step];
			break;
		}

		if (currentcmd->cmdid == CMDID_END) {
			if ((*stage) == 2) {
				return true;
			} else {
				(*stage)++;
				(*step) = 0;
				continue;
			}
		}

		switch (currentcmd->cmdid) {
		case CMDID_SET_TXPOWEROWER_LEVEL:
			rtl88e_phy_set_txpower_level(hw, channel);
			break;
		case CMDID_WRITEPORT_ULONG:
			rtl_write_dword(rtlpriv, currentcmd->para1,
					currentcmd->para2);
			break;
		case CMDID_WRITEPORT_USHORT:
			rtl_write_word(rtlpriv, currentcmd->para1,
				       (u16) currentcmd->para2);
			break;
		case CMDID_WRITEPORT_UCHAR:
			rtl_write_byte(rtlpriv, currentcmd->para1,
				       (u8) currentcmd->para2);
			break;
		case CMDID_RF_WRITEREG:
			for (rfpath = 0; rfpath < num_total_rfpath; rfpath++) {
				rtlphy->rfreg_chnlval[rfpath] =
				    ((rtlphy->rfreg_chnlval[rfpath] &
				      0xfffffc00) | currentcmd->para2);

				rtl_set_rfreg(hw, (enum radio_path)rfpath,
					      currentcmd->para1,
					      RFREG_OFFSET_MASK,
					      rtlphy->rfreg_chnlval[rfpath]);
			}
			break;
		default:
			RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG,
				 "switch case not processed\n");
			break;
		}

		break;
	} while (true);

	(*delay) = currentcmd->msdelay;
	(*step)++;
	return false;
}

static long rtl88e_pwr_idx_dbm(struct ieee80211_hw *hw,
			       enum wireless_mode wirelessmode,
			       u8 txpwridx)
{
	long offset;
	long pwrout_dbm;

	switch (wirelessmode) {
	case WIRELESS_MODE_B:
		offset = -7;
		break;
	case WIRELESS_MODE_G:
	case WIRELESS_MODE_N_24G:
		offset = -8;
		break;
	default:
		offset = -8;
		break;
	}
	pwrout_dbm = txpwridx / 2 + offset;
	return pwrout_dbm;
}

static void rtl88e_phy_set_io(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	struct dig_t *dm_digtable = &rtlpriv->dm_digtable;

	RT_TRACE(rtlpriv, COMP_CMD, DBG_TRACE,
		 "--->Cmd(%#x), set_io_inprogress(%d)\n",
		 rtlphy->current_io_type, rtlphy->set_io_inprogress);
	switch (rtlphy->current_io_type) {
	case IO_CMD_RESUME_DM_BY_SCAN:
		dm_digtable->cur_igvalue = rtlphy->initgain_backup.xaagccore1;
		/*rtl92c_dm_write_dig(hw);*/
		rtl88e_phy_set_txpower_level(hw, rtlphy->current_channel);
		rtl_set_bbreg(hw, RCCK0_CCA, 0xff0000, 0x83);
		break;
	case IO_CMD_PAUSE_DM_BY_SCAN:
		rtlphy->initgain_backup.xaagccore1 = dm_digtable->cur_igvalue;
		dm_digtable->cur_igvalue = 0x17;
		rtl_set_bbreg(hw, RCCK0_CCA, 0xff0000, 0x40);
		break;
	default:
		RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG,
			 "switch case not processed\n");
		break;
	}
	rtlphy->set_io_inprogress = false;
	RT_TRACE(rtlpriv, COMP_CMD, DBG_TRACE,
		 "(%#x)\n", rtlphy->current_io_type);
}

u32 rtl88e_phy_query_bb_reg(struct ieee80211_hw *hw, u32 regaddr, u32 bitmask)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 returnvalue, originalvalue, bitshift;

	RT_TRACE(rtlpriv, COMP_RF, DBG_TRACE,
		 "regaddr(%#x), bitmask(%#x)\n", regaddr, bitmask);
	originalvalue = rtl_read_dword(rtlpriv, regaddr);
	bitshift = cal_bit_shift(bitmask);
	returnvalue = (originalvalue & bitmask) >> bitshift;

	RT_TRACE(rtlpriv, COMP_RF, DBG_TRACE,
		 "BBR MASK = 0x%x Addr[0x%x]= 0x%x\n", bitmask,
		 regaddr, originalvalue);

	return returnvalue;
}

void rtl88e_phy_set_bb_reg(struct ieee80211_hw *hw,
			   u32 regaddr, u32 bitmask, u32 data)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 originalvalue, bitshift;

	RT_TRACE(rtlpriv, COMP_RF, DBG_TRACE,
		 "regaddr(%#x), bitmask(%#x),data(%#x)\n",
		 regaddr, bitmask, data);

	if (bitmask != MASKDWORD) {
		originalvalue = rtl_read_dword(rtlpriv, regaddr);
		bitshift = cal_bit_shift(bitmask);
		data = ((originalvalue & (~bitmask)) | (data << bitshift));
	}

	rtl_write_dword(rtlpriv, regaddr, data);

	RT_TRACE(rtlpriv, COMP_RF, DBG_TRACE,
		 "regaddr(%#x), bitmask(%#x), data(%#x)\n",
		 regaddr, bitmask, data);
}

u32 rtl88e_phy_query_rf_reg(struct ieee80211_hw *hw,
			    enum radio_path rfpath, u32 regaddr, u32 bitmask)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 original_value, readback_value, bitshift;
	unsigned long flags;

	RT_TRACE(rtlpriv, COMP_RF, DBG_TRACE,
		 "regaddr(%#x), rfpath(%#x), bitmask(%#x)\n",
		 regaddr, rfpath, bitmask);

	spin_lock_irqsave(&rtlpriv->locks.rf_lock, flags);


	original_value = rf_serial_read(hw, rfpath, regaddr);
	bitshift = cal_bit_shift(bitmask);
	readback_value = (original_value & bitmask) >> bitshift;

	spin_unlock_irqrestore(&rtlpriv->locks.rf_lock, flags);

	RT_TRACE(rtlpriv, COMP_RF, DBG_TRACE,
		 "regaddr(%#x), rfpath(%#x), bitmask(%#x), original_value(%#x)\n",
		  regaddr, rfpath, bitmask, original_value);

	return readback_value;
}

void rtl88e_phy_set_rf_reg(struct ieee80211_hw *hw,
			   enum radio_path rfpath,
			   u32 regaddr, u32 bitmask, u32 data)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 original_value, bitshift;
	unsigned long flags;

	RT_TRACE(rtlpriv, COMP_RF, DBG_TRACE,
		 "regaddr(%#x), bitmask(%#x), data(%#x), rfpath(%#x)\n",
		  regaddr, bitmask, data, rfpath);

	spin_lock_irqsave(&rtlpriv->locks.rf_lock, flags);

	if (bitmask != RFREG_OFFSET_MASK) {
			original_value = rf_serial_read(hw, rfpath, regaddr);
			bitshift = cal_bit_shift(bitmask);
			data = ((original_value & (~bitmask)) |
				(data << bitshift));
		}

	rf_serial_write(hw, rfpath, regaddr, data);


	spin_unlock_irqrestore(&rtlpriv->locks.rf_lock, flags);

	RT_TRACE(rtlpriv, COMP_RF, DBG_TRACE,
		 "regaddr(%#x), bitmask(%#x), data(%#x), rfpath(%#x)\n",
		 regaddr, bitmask, data, rfpath);
}

static bool config_mac_with_header(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 i;
	u32 arraylength;
	u32 *ptrarray;

	RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE, "Read Rtl8188EMACPHY_Array\n");
	arraylength = RTL8188EEMAC_1T_ARRAYLEN;
	ptrarray = RTL8188EEMAC_1T_ARRAY;
	RT_TRACE(rtlpriv, COMP_INIT, DBG_LOUD,
		 "Img:RTL8188EEMAC_1T_ARRAY LEN %d\n", arraylength);
	for (i = 0; i < arraylength; i = i + 2)
		rtl_write_byte(rtlpriv, ptrarray[i], (u8) ptrarray[i + 1]);
	return true;
}

bool rtl88e_phy_mac_config(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	bool rtstatus = config_mac_with_header(hw);

	rtl_write_byte(rtlpriv, 0x04CA, 0x0B);
	return rtstatus;
}

bool rtl88e_phy_bb_config(struct ieee80211_hw *hw)
{
	bool rtstatus = true;
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u16 regval;
	u8 reg_hwparafile = 1;
	u32 tmp;
	rtl88e_phy_init_bb_rf_register_definition(hw);
	regval = rtl_read_word(rtlpriv, REG_SYS_FUNC_EN);
	rtl_write_word(rtlpriv, REG_SYS_FUNC_EN,
		       regval | BIT(13) | BIT(0) | BIT(1));

	rtl_write_byte(rtlpriv, REG_RF_CTRL, RF_EN | RF_RSTB | RF_SDMRSTB);
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN,
		       FEN_PPLL | FEN_PCIEA | FEN_DIO_PCIE |
		       FEN_BB_GLB_RSTN | FEN_BBRSTB);
	tmp = rtl_read_dword(rtlpriv, 0x4c);
	rtl_write_dword(rtlpriv, 0x4c, tmp | BIT(23));
	if (reg_hwparafile == 1)
		rtstatus = config_parafile(hw);
	return rtstatus;
}

bool rtl88e_phy_rf_config(struct ieee80211_hw *hw)
{
	return rtl88e_phy_rf6052_config(hw);
}

static bool check_cond(struct ieee80211_hw *hw,
				    const u32  condition)
{
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	struct rtl_efuse *fuse = rtl_efuse(rtl_priv(hw));
	u32 _board = fuse->board_type; /*need efuse define*/
	u32 _interface = rtlhal->interface;
	u32 _platform = 0x08;/*SupportPlatform */
	u32 cond = condition;

	if (condition == 0xCDCDCDCD)
		return true;

	cond = condition & 0xFF;
	if ((_board & cond) == 0 && cond != 0x1F)
		return false;

	cond = condition & 0xFF00;
	cond = cond >> 8;
	if ((_interface & cond) == 0 && cond != 0x07)
		return false;

	cond = condition & 0xFF0000;
	cond = cond >> 16;
	if ((_platform & cond) == 0 && cond != 0x0F)
		return false;
	return true;
}

static void _rtl8188e_config_rf_reg(struct ieee80211_hw *hw,
				    u32 addr, u32 data, enum radio_path rfpath,
				    u32 regaddr)
{
	if (addr == 0xffe) {
		mdelay(50);
	} else if (addr == 0xfd) {
		mdelay(5);
	} else if (addr == 0xfc) {
		mdelay(1);
	} else if (addr == 0xfb) {
		udelay(50);
	} else if (addr == 0xfa) {
		udelay(5);
	} else if (addr == 0xf9) {
		udelay(1);
	} else {
		rtl_set_rfreg(hw, rfpath, regaddr,
			      RFREG_OFFSET_MASK,
			      data);
		udelay(1);
	}
}

static void rtl88_config_s(struct ieee80211_hw *hw,
	u32 addr, u32 data)
{
	u32 content = 0x1000; /*RF Content: radio_a_txt*/
	u32 maskforphyset = (u32)(content & 0xE000);

	_rtl8188e_config_rf_reg(hw, addr, data, RF90_PATH_A,
				addr | maskforphyset);
}

static void _rtl8188e_config_bb_reg(struct ieee80211_hw *hw,
				    u32 addr, u32 data)
{
	if (addr == 0xfe) {
		mdelay(50);
	} else if (addr == 0xfd) {
		mdelay(5);
	} else if (addr == 0xfc) {
		mdelay(1);
	} else if (addr == 0xfb) {
		udelay(50);
	} else if (addr == 0xfa) {
		udelay(5);
	} else if (addr == 0xf9) {
		udelay(1);
	} else {
		rtl_set_bbreg(hw, addr, MASKDWORD, data);
		udelay(1);
	}
}


#define NEXT_PAIR(v1, v2, i)				\
	do {						\
		i += 2; v1 = array_table[i];		\
		v2 = array_table[i + 1];		\
	} while (0)

static void set_baseband_agc_config(struct ieee80211_hw *hw)
{
	int i;
	u32 *array_table;
	u16 arraylen;
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 v1 = 0, v2 = 0;

	arraylen = RTL8188EEAGCTAB_1TARRAYLEN;
	array_table = RTL8188EEAGCTAB_1TARRAY;

	for (i = 0; i < arraylen; i += 2) {
		v1 = array_table[i];
		v2 = array_table[i + 1];
		if (v1 < 0xCDCDCDCD) {
			rtl_set_bbreg(hw, array_table[i], MASKDWORD,
				      array_table[i + 1]);
			udelay(1);
			continue;
		} else {/*This line is the start line of branch.*/
			if (!check_cond(hw, array_table[i])) {
				/*Discard the following (offset, data) pairs*/
				NEXT_PAIR(v1, v2, i);
				while (v2 != 0xDEAD && v2 != 0xCDEF &&
				       v2 != 0xCDCD && i < arraylen - 2) {
					NEXT_PAIR(v1, v2, i);
				}
				i -= 2; /* compensate for loop's += 2*/
			} else {
				/* Configure matched pairs and skip to end */
				NEXT_PAIR(v1, v2, i);
				while (v2 != 0xDEAD && v2 != 0xCDEF &&
				       v2 != 0xCDCD && i < arraylen - 2) {
					rtl_set_bbreg(hw, array_table[i],
						      MASKDWORD,
						      array_table[i + 1]);
					udelay(1);
					NEXT_PAIR(v1, v2, i);
				}

				while (v2 != 0xDEAD && i < arraylen - 2)
					NEXT_PAIR(v1, v2, i);
			}
		}
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "The agctab_array_table[0] is %x Rtl818EEPHY_REGArray[1] is %x\n",
			 array_table[i],
			 array_table[i + 1]);
	}
}

static void set_baseband_phy_config(struct ieee80211_hw *hw)
{
	int i;
	u32 *array_table;
	u16 arraylen;
	u32 v1 = 0, v2 = 0;

	arraylen = RTL8188EEPHY_REG_1TARRAYLEN;
	array_table = RTL8188EEPHY_REG_1TARRAY;

	for (i = 0; i < arraylen; i += 2) {
		v1 = array_table[i];
		v2 = array_table[i + 1];
		if (v1 < 0xcdcdcdcd) {
			_rtl8188e_config_bb_reg(hw, v1, v2);
		} else {/*This line is the start line of branch.*/
			if (!check_cond(hw, array_table[i])) {
				/*Discard the following (offset, data) pairs*/
				NEXT_PAIR(v1, v2, i);
				while (v2 != 0xDEAD &&
				       v2 != 0xCDEF &&
				       v2 != 0xCDCD && i < arraylen - 2)
					NEXT_PAIR(v1, v2, i);
				i -= 2; /* prevent from for-loop += 2*/
			} else {
				/* Configure matched pairs and skip to end */
				NEXT_PAIR(v1, v2, i);
				while (v2 != 0xDEAD &&
				       v2 != 0xCDEF &&
				       v2 != 0xCDCD && i < arraylen - 2) {
					_rtl8188e_config_bb_reg(hw, v1, v2);
					NEXT_PAIR(v1, v2, i);
				}

				while (v2 != 0xDEAD && i < arraylen - 2)
					NEXT_PAIR(v1, v2, i);
			}
		}
	}
}

static void store_pwrindex_offset(struct ieee80211_hw *hw,
				  u32 regaddr, u32 bitmask,
				  u32 data)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);

	if (regaddr == RTXAGC_A_RATE18_06) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][0] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][0] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][0]);
	}
	if (regaddr == RTXAGC_A_RATE54_24) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][1] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][1] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][1]);
	}
	if (regaddr == RTXAGC_A_CCK1_MCS32) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][6] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][6] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][6]);
	}
	if (regaddr == RTXAGC_B_CCK11_A_CCK2_11 && bitmask == 0xffffff00) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][7] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][7] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][7]);
	}
	if (regaddr == RTXAGC_A_MCS03_MCS00) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][2] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][2] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][2]);
	}
	if (regaddr == RTXAGC_A_MCS07_MCS04) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][3] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][3] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][3]);
	}
	if (regaddr == RTXAGC_A_MCS11_MCS08) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][4] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][4] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][4]);
	}
	if (regaddr == RTXAGC_A_MCS15_MCS12) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][5] = data;
		if (get_rf_type(rtlphy) == RF_1T1R)
			rtlphy->pwrgroup_cnt++;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][5] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][5]);
	}
	if (regaddr == RTXAGC_B_RATE18_06) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][8] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][8] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][8]);
	}
	if (regaddr == RTXAGC_B_RATE54_24) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][9] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][9] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][9]);
	}
	if (regaddr == RTXAGC_B_CCK1_55_MCS32) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][14] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][14] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][14]);
	}
	if (regaddr == RTXAGC_B_CCK11_A_CCK2_11 && bitmask == 0x000000ff) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][15] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][15] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][15]);
	}
	if (regaddr == RTXAGC_B_MCS03_MCS00) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][10] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][10] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][10]);
	}
	if (regaddr == RTXAGC_B_MCS07_MCS04) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][11] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][11] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][11]);
	}
	if (regaddr == RTXAGC_B_MCS11_MCS08) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][12] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][12] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][12]);
	}
	if (regaddr == RTXAGC_B_MCS15_MCS12) {
		rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][13] = data;
		RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
			 "MCSTxPowerLevelOriginalOffset[%d][13] = 0x%x\n",
			 rtlphy->pwrgroup_cnt,
			 rtlphy->mcs_offset[rtlphy->pwrgroup_cnt][13]);
		if (get_rf_type(rtlphy) != RF_1T1R)
			rtlphy->pwrgroup_cnt++;
	}
}

#define READ_NEXT_RF_PAIR(v1, v2, i)		\
	do {					\
		i += 2; v1 = a_table[i];	\
		v2 = a_table[i + 1];		\
	} while (0)

bool rtl88e_phy_config_rf_with_headerfile(struct ieee80211_hw *hw,
					  enum radio_path rfpath)
{
	int i;
	u32 *a_table;
	u16 a_len;
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	u32 v1 = 0, v2 = 0;

	a_len = RTL8188EE_RADIOA_1TARRAYLEN;
	a_table = RTL8188EE_RADIOA_1TARRAY;
	RT_TRACE(rtlpriv, COMP_INIT, DBG_LOUD,
		 "Radio_A:RTL8188EE_RADIOA_1TARRAY %d\n", a_len);
	RT_TRACE(rtlpriv, COMP_INIT, DBG_LOUD, "Radio No %x\n", rfpath);
	switch (rfpath) {
	case RF90_PATH_A:
		for (i = 0; i < a_len; i = i + 2) {
			v1 = a_table[i];
			v2 = a_table[i + 1];
			if (v1 < 0xcdcdcdcd) {
				rtl88_config_s(hw, v1, v2);
			} else {/*This line is the start line of branch.*/
				if (!check_cond(hw, a_table[i])) {
					/* Discard the following (offset, data)
					 * pairs
					 */
					READ_NEXT_RF_PAIR(v1, v2, i);
					while (v2 != 0xDEAD && v2 != 0xCDEF &&
					       v2 != 0xCDCD && i < a_len - 2)
						READ_NEXT_RF_PAIR(v1, v2, i);
					i -= 2; /* prevent from for-loop += 2*/
				} else {
					/* Configure matched pairs and skip to
					 * end of if-else.
					 */
					READ_NEXT_RF_PAIR(v1, v2, i);
					while (v2 != 0xDEAD && v2 != 0xCDEF &&
					       v2 != 0xCDCD && i < a_len - 2) {
						rtl88_config_s(hw, v1, v2);
						READ_NEXT_RF_PAIR(v1, v2, i);
					}

					while (v2 != 0xDEAD && i < a_len - 2)
						READ_NEXT_RF_PAIR(v1, v2, i);
				}
			}
		}

		if (rtlhal->oem_id == RT_CID_819x_HP)
			rtl88_config_s(hw, 0x52, 0x7E4BD);

		break;

	case RF90_PATH_B:
	case RF90_PATH_C:
	case RF90_PATH_D:
	default:
		RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG,
			 "switch case not processed\n");
		break;
	}
	return true;
}

void rtl88e_phy_get_hw_reg_originalvalue(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);

	rtlphy->default_initialgain[0] = rtl_get_bbreg(hw, ROFDM0_XAAGCCORE1,
						       MASKBYTE0);
	rtlphy->default_initialgain[1] = rtl_get_bbreg(hw, ROFDM0_XBAGCCORE1,
						       MASKBYTE0);
	rtlphy->default_initialgain[2] = rtl_get_bbreg(hw, ROFDM0_XCAGCCORE1,
						       MASKBYTE0);
	rtlphy->default_initialgain[3] = rtl_get_bbreg(hw, ROFDM0_XDAGCCORE1,
						       MASKBYTE0);

	RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
		 "Default initial gain (c50 = 0x%x, c58 = 0x%x, c60 = 0x%x, c68 = 0x%x\n",
		  rtlphy->default_initialgain[0],
		  rtlphy->default_initialgain[1],
		  rtlphy->default_initialgain[2],
		  rtlphy->default_initialgain[3]);

	rtlphy->framesync = rtl_get_bbreg(hw, ROFDM0_RXDETECTOR3,
					  MASKBYTE0);
	rtlphy->framesync_c34 = rtl_get_bbreg(hw, ROFDM0_RXDETECTOR2,
					      MASKDWORD);

	RT_TRACE(rtlpriv, COMP_INIT, DBG_TRACE,
		 "Default framesync (0x%x) = 0x%x\n",
		 ROFDM0_RXDETECTOR3, rtlphy->framesync);
}

void rtl88e_phy_get_txpower_level(struct ieee80211_hw *hw, long *powerlevel)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	u8 level;
	long dbm;

	level = rtlphy->cur_cck_txpwridx;
	dbm = rtl88e_pwr_idx_dbm(hw, WIRELESS_MODE_B, level);
	level = rtlphy->cur_ofdm24g_txpwridx;
	if (rtl88e_pwr_idx_dbm(hw, WIRELESS_MODE_G, level) > dbm)
		dbm = rtl88e_pwr_idx_dbm(hw, WIRELESS_MODE_G, level);
	level = rtlphy->cur_ofdm24g_txpwridx;
	if (rtl88e_pwr_idx_dbm(hw, WIRELESS_MODE_N_24G, level) > dbm)
		dbm = rtl88e_pwr_idx_dbm(hw, WIRELESS_MODE_N_24G, level);
	*powerlevel = dbm;
}

static void _rtl88e_get_txpower_index(struct ieee80211_hw *hw, u8 channel,
				      u8 *cckpower, u8 *ofdm, u8 *bw20_pwr,
				      u8 *bw40_pwr)
{
	struct rtl_efuse *fuse = rtl_efuse(rtl_priv(hw));
	u8 i = (channel - 1);
	u8 rf_path = 0;
	int jj = RF90_PATH_A;
	int kk = RF90_PATH_B;

	for (rf_path = 0; rf_path < 2; rf_path++) {
		if (rf_path == jj) {
			cckpower[jj] = fuse->txpwrlevel_cck[jj][i];
			if (fuse->txpwr_ht20diff[jj][i] > 0x0f) /*-8~7 */
				bw20_pwr[jj] = fuse->txpwrlevel_ht40_1s[jj][i] -
					(~(fuse->txpwr_ht20diff[jj][i]) + 1);
			else
				bw20_pwr[jj] = fuse->txpwrlevel_ht40_1s[jj][i] +
					 fuse->txpwr_ht20diff[jj][i];
			if (fuse->txpwr_legacyhtdiff[jj][i] > 0xf)
				ofdm[jj] = fuse->txpwrlevel_ht40_1s[jj][i] -
					(~(fuse->txpwr_legacyhtdiff[jj][i])+1);
			else
				ofdm[jj] = fuse->txpwrlevel_ht40_1s[jj][i] +
					   fuse->txpwr_legacyhtdiff[jj][i];
			bw40_pwr[jj] = fuse->txpwrlevel_ht40_1s[jj][i];

		} else if (rf_path == kk) {
			cckpower[kk] = fuse->txpwrlevel_cck[kk][i];
			bw20_pwr[kk] = fuse->txpwrlevel_ht40_1s[kk][i] +
				       fuse->txpwr_ht20diff[kk][i];
			ofdm[kk] = fuse->txpwrlevel_ht40_1s[kk][i] +
					fuse->txpwr_legacyhtdiff[kk][i];
			bw40_pwr[kk] = fuse->txpwrlevel_ht40_1s[kk][i];
		}
	}
}

static void _rtl88e_ccxpower_index_check(struct ieee80211_hw *hw,
					 u8 channel, u8 *cckpower,
					 u8 *ofdm, u8 *bw20_pwr,
					 u8 *bw40_pwr)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);

	rtlphy->cur_cck_txpwridx = cckpower[0];
	rtlphy->cur_ofdm24g_txpwridx = ofdm[0];
	rtlphy->cur_bw20_txpwridx = bw20_pwr[0];
	rtlphy->cur_bw40_txpwridx = bw40_pwr[0];
}

void rtl88e_phy_set_txpower_level(struct ieee80211_hw *hw, u8 channel)
{
	struct rtl_efuse *fuse = rtl_efuse(rtl_priv(hw));
	u8 cckpower[MAX_TX_COUNT]  = {0}, ofdm[MAX_TX_COUNT] = {0};
	u8 bw20_pwr[MAX_TX_COUNT] = {0}, bw40_pwr[MAX_TX_COUNT] = {0};

	if (fuse->txpwr_fromeprom == false)
		return;
	_rtl88e_get_txpower_index(hw, channel, &cckpower[0], &ofdm[0],
				  &bw20_pwr[0], &bw40_pwr[0]);
	_rtl88e_ccxpower_index_check(hw, channel, &cckpower[0], &ofdm[0],
				     &bw20_pwr[0], &bw40_pwr[0]);
	rtl88e_phy_rf6052_set_cck_txpower(hw, &cckpower[0]);
	rtl88e_phy_rf6052_set_ofdm_txpower(hw, &ofdm[0], &bw20_pwr[0],
					   &bw40_pwr[0], channel);
}

void rtl88e_phy_scan_operation_backup(struct ieee80211_hw *hw, u8 operation)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	enum io_type iotype;

	if (!is_hal_stop(rtlhal)) {
		switch (operation) {
		case SCAN_OPT_BACKUP:
			iotype = IO_CMD_PAUSE_DM_BY_SCAN;
			rtlpriv->cfg->ops->set_hw_reg(hw,
						      HW_VAR_IO_CMD,
						      (u8 *)&iotype);
			break;
		case SCAN_OPT_RESTORE:
			iotype = IO_CMD_RESUME_DM_BY_SCAN;
			rtlpriv->cfg->ops->set_hw_reg(hw,
						      HW_VAR_IO_CMD,
						      (u8 *)&iotype);
			break;
		default:
			RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG,
				 "Unknown Scan Backup operation.\n");
			break;
		}
	}
}

void rtl88e_phy_set_bw_mode_callback(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	u8 reg_bw_opmode;
	u8 reg_prsr_rsc;

	RT_TRACE(rtlpriv, COMP_SCAN, DBG_TRACE,
		 "Switch to %s bandwidth\n",
		 rtlphy->current_chan_bw == HT_CHANNEL_WIDTH_20 ?
		 "20MHz" : "40MHz");

	if (is_hal_stop(rtlhal)) {
		rtlphy->set_bwmode_inprogress = false;
		return;
	}

	reg_bw_opmode = rtl_read_byte(rtlpriv, REG_BWOPMODE);
	reg_prsr_rsc = rtl_read_byte(rtlpriv, REG_RRSR + 2);

	switch (rtlphy->current_chan_bw) {
	case HT_CHANNEL_WIDTH_20:
		reg_bw_opmode |= BW_OPMODE_20MHZ;
		rtl_write_byte(rtlpriv, REG_BWOPMODE, reg_bw_opmode);
		break;
	case HT_CHANNEL_WIDTH_20_40:
		reg_bw_opmode &= ~BW_OPMODE_20MHZ;
		rtl_write_byte(rtlpriv, REG_BWOPMODE, reg_bw_opmode);
		reg_prsr_rsc =
		    (reg_prsr_rsc & 0x90) | (mac->cur_40_prime_sc << 5);
		rtl_write_byte(rtlpriv, REG_RRSR + 2, reg_prsr_rsc);
		break;
	default:
		RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG,
			 "unknown bandwidth: %#X\n", rtlphy->current_chan_bw);
		break;
	}

	switch (rtlphy->current_chan_bw) {
	case HT_CHANNEL_WIDTH_20:
		rtl_set_bbreg(hw, RFPGA0_RFMOD, BRFMOD, 0x0);
		rtl_set_bbreg(hw, RFPGA1_RFMOD, BRFMOD, 0x0);
	/*	rtl_set_bbreg(hw, RFPGA0_ANALOGPARAMETER2, BIT(10), 1);*/
		break;
	case HT_CHANNEL_WIDTH_20_40:
		rtl_set_bbreg(hw, RFPGA0_RFMOD, BRFMOD, 0x1);
		rtl_set_bbreg(hw, RFPGA1_RFMOD, BRFMOD, 0x1);

		rtl_set_bbreg(hw, RCCK0_SYSTEM, BCCK_SIDEBAND,
			      (mac->cur_40_prime_sc >> 1));
		rtl_set_bbreg(hw, ROFDM1_LSTF, 0xC00, mac->cur_40_prime_sc);
		/*rtl_set_bbreg(hw, RFPGA0_ANALOGPARAMETER2, BIT(10), 0);*/

		rtl_set_bbreg(hw, 0x818, (BIT(26) | BIT(27)),
			      (mac->cur_40_prime_sc ==
			       HAL_PRIME_CHNL_OFFSET_LOWER) ? 2 : 1);
		break;
	default:
		RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG,
			 "unknown bandwidth: %#X\n", rtlphy->current_chan_bw);
		break;
	}
	rtl88e_phy_rf6052_set_bandwidth(hw, rtlphy->current_chan_bw);
	rtlphy->set_bwmode_inprogress = false;
	RT_TRACE(rtlpriv, COMP_SCAN, DBG_LOUD, "\n");
}

void rtl88e_phy_set_bw_mode(struct ieee80211_hw *hw,
			    enum nl80211_channel_type ch_type)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	u8 tmp_bw = rtlphy->current_chan_bw;

	if (rtlphy->set_bwmode_inprogress)
		return;
	rtlphy->set_bwmode_inprogress = true;
	if ((!is_hal_stop(rtlhal)) && !(RT_CANNOT_IO(hw))) {
		rtl88e_phy_set_bw_mode_callback(hw);
	} else {
		RT_TRACE(rtlpriv, COMP_ERR, DBG_WARNING,
			 "FALSE driver sleep or unload\n");
		rtlphy->set_bwmode_inprogress = false;
		rtlphy->current_chan_bw = tmp_bw;
	}
}

void rtl88e_phy_sw_chnl_callback(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	u32 delay;

	RT_TRACE(rtlpriv, COMP_SCAN, DBG_TRACE,
		 "switch to channel%d\n", rtlphy->current_channel);
	if (is_hal_stop(rtlhal))
		return;
	do {
		if (!rtlphy->sw_chnl_inprogress)
			break;
		if (!chnl_step_by_step(hw, rtlphy->current_channel,
				       &rtlphy->sw_chnl_stage,
				       &rtlphy->sw_chnl_step, &delay)) {
			if (delay > 0)
				mdelay(delay);
			else
				continue;
		} else {
			rtlphy->sw_chnl_inprogress = false;
		}
		break;
	} while (true);
	RT_TRACE(rtlpriv, COMP_SCAN, DBG_TRACE, "\n");
}

u8 rtl88e_phy_sw_chnl(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));

	if (rtlphy->sw_chnl_inprogress)
		return 0;
	if (rtlphy->set_bwmode_inprogress)
		return 0;
	RT_ASSERT((rtlphy->current_channel <= 14),
		  "WIRELESS_MODE_G but channel>14");
	rtlphy->sw_chnl_inprogress = true;
	rtlphy->sw_chnl_stage = 0;
	rtlphy->sw_chnl_step = 0;
	if (!(is_hal_stop(rtlhal)) && !(RT_CANNOT_IO(hw))) {
		rtl88e_phy_sw_chnl_callback(hw);
		RT_TRACE(rtlpriv, COMP_CHAN, DBG_LOUD,
			 "sw_chnl_inprogress false schdule workitem current channel %d\n",
			 rtlphy->current_channel);
		rtlphy->sw_chnl_inprogress = false;
	} else {
		RT_TRACE(rtlpriv, COMP_CHAN, DBG_LOUD,
			 "sw_chnl_inprogress false driver sleep or unload\n");
		rtlphy->sw_chnl_inprogress = false;
	}
	return 1;
}

static u8 _rtl88e_phy_path_a_iqk(struct ieee80211_hw *hw, bool config_pathb)
{
	u32 reg_eac, reg_e94, reg_e9c;
	u8 result = 0x00;

	rtl_set_bbreg(hw, 0xe30, MASKDWORD, 0x10008c1c);
	rtl_set_bbreg(hw, 0xe34, MASKDWORD, 0x30008c1c);
	rtl_set_bbreg(hw, 0xe38, MASKDWORD, 0x8214032a);
	rtl_set_bbreg(hw, 0xe3c, MASKDWORD, 0x28160000);

	rtl_set_bbreg(hw, 0xe4c, MASKDWORD, 0x00462911);
	rtl_set_bbreg(hw, 0xe48, MASKDWORD, 0xf9000000);
	rtl_set_bbreg(hw, 0xe48, MASKDWORD, 0xf8000000);

	mdelay(IQK_DELAY_TIME);

	reg_eac = rtl_get_bbreg(hw, 0xeac, MASKDWORD);
	reg_e94 = rtl_get_bbreg(hw, 0xe94, MASKDWORD);
	reg_e9c = rtl_get_bbreg(hw, 0xe9c, MASKDWORD);

	if (!(reg_eac & BIT(28)) &&
	    (((reg_e94 & 0x03FF0000) >> 16) != 0x142) &&
	    (((reg_e9c & 0x03FF0000) >> 16) != 0x42))
		result |= 0x01;
	return result;
}

static u8 _rtl88e_phy_path_b_iqk(struct ieee80211_hw *hw)
{
	u32 reg_eac, reg_eb4, reg_ebc, reg_ec4, reg_ecc;
	u8 result = 0x00;

	rtl_set_bbreg(hw, 0xe60, MASKDWORD, 0x00000002);
	rtl_set_bbreg(hw, 0xe60, MASKDWORD, 0x00000000);
	mdelay(IQK_DELAY_TIME);
	reg_eac = rtl_get_bbreg(hw, 0xeac, MASKDWORD);
	reg_eb4 = rtl_get_bbreg(hw, 0xeb4, MASKDWORD);
	reg_ebc = rtl_get_bbreg(hw, 0xebc, MASKDWORD);
	reg_ec4 = rtl_get_bbreg(hw, 0xec4, MASKDWORD);
	reg_ecc = rtl_get_bbreg(hw, 0xecc, MASKDWORD);

	if (!(reg_eac & BIT(31)) &&
	    (((reg_eb4 & 0x03FF0000) >> 16) != 0x142) &&
	    (((reg_ebc & 0x03FF0000) >> 16) != 0x42))
		result |= 0x01;
	else
		return result;
	if (!(reg_eac & BIT(30)) &&
	    (((reg_ec4 & 0x03FF0000) >> 16) != 0x132) &&
	    (((reg_ecc & 0x03FF0000) >> 16) != 0x36))
		result |= 0x02;
	return result;
}

static u8 _rtl88e_phy_path_a_rx_iqk(struct ieee80211_hw *hw, bool config_pathb)
{
	u32 reg_eac, reg_e94, reg_e9c, reg_ea4, u32temp;
	u8 result = 0x00;
	int jj = RF90_PATH_A;

	/*Get TXIMR Setting*/
	/*Modify RX IQK mode table*/
	rtl_set_bbreg(hw, RFPGA0_IQK, MASKDWORD, 0x00000000);
	rtl_set_rfreg(hw, jj, RF_WE_LUT, RFREG_OFFSET_MASK, 0x800a0);
	rtl_set_rfreg(hw, jj, RF_RCK_OS, RFREG_OFFSET_MASK, 0x30000);
	rtl_set_rfreg(hw, jj, RF_TXPA_G1, RFREG_OFFSET_MASK, 0x0000f);
	rtl_set_rfreg(hw, jj, RF_TXPA_G2, RFREG_OFFSET_MASK, 0xf117b);
	rtl_set_bbreg(hw, RFPGA0_IQK, MASKDWORD, 0x80800000);

	/*IQK Setting*/
	rtl_set_bbreg(hw, RTX_IQK, MASKDWORD, 0x01007c00);
	rtl_set_bbreg(hw, RRX_IQK, MASKDWORD, 0x81004800);

	/*path a IQK setting*/
	rtl_set_bbreg(hw, RTX_IQK_TONE_A, MASKDWORD, 0x10008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_A, MASKDWORD, 0x30008c1c);
	rtl_set_bbreg(hw, RTX_IQK_PI_A, MASKDWORD, 0x82160804);
	rtl_set_bbreg(hw, RRX_IQK_PI_A, MASKDWORD, 0x28160000);

	/*LO calibration Setting*/
	rtl_set_bbreg(hw, RIQK_AGC_RSP, MASKDWORD, 0x0046a911);
	/*one shot, path A LOK & iqk*/
	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xf9000000);
	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xf8000000);

	mdelay(IQK_DELAY_TIME);

	reg_eac = rtl_get_bbreg(hw, RRX_POWER_AFTER_IQK_A_2, MASKDWORD);
	reg_e94 = rtl_get_bbreg(hw, RTX_POWER_BEFORE_IQK_A, MASKDWORD);
	reg_e9c = rtl_get_bbreg(hw, RTX_POWER_AFTER_IQK_A, MASKDWORD);


	if (!(reg_eac & BIT(28)) &&
	    (((reg_e94 & 0x03FF0000) >> 16) != 0x142) &&
	    (((reg_e9c & 0x03FF0000) >> 16) != 0x42))
		result |= 0x01;
	else
		return result;

	u32temp = 0x80007C00 | (reg_e94&0x3FF0000)  |
		  ((reg_e9c&0x3FF0000) >> 16);
	rtl_set_bbreg(hw, RTX_IQK, MASKDWORD, u32temp);
	/*RX IQK*/
	/*Modify RX IQK mode table*/
	rtl_set_bbreg(hw, RFPGA0_IQK, MASKDWORD, 0x00000000);
	rtl_set_rfreg(hw, jj, RF_WE_LUT, RFREG_OFFSET_MASK, 0x800a0);
	rtl_set_rfreg(hw, jj, RF_RCK_OS, RFREG_OFFSET_MASK, 0x30000);
	rtl_set_rfreg(hw, jj, RF_TXPA_G1, RFREG_OFFSET_MASK, 0x0000f);
	rtl_set_rfreg(hw, jj, RF_TXPA_G2, RFREG_OFFSET_MASK, 0xf7ffa);
	rtl_set_bbreg(hw, RFPGA0_IQK, MASKDWORD, 0x80800000);

	/*IQK Setting*/
	rtl_set_bbreg(hw, RRX_IQK, MASKDWORD, 0x01004800);

	/*path a IQK setting*/
	rtl_set_bbreg(hw, RTX_IQK_TONE_A, MASKDWORD, 0x30008c1c);
	rtl_set_bbreg(hw, RRX_IQK_TONE_A, MASKDWORD, 0x10008c1c);
	rtl_set_bbreg(hw, RTX_IQK_PI_A, MASKDWORD, 0x82160c05);
	rtl_set_bbreg(hw, RRX_IQK_PI_A, MASKDWORD, 0x28160c05);

	/*LO calibration Setting*/
	rtl_set_bbreg(hw, RIQK_AGC_RSP, MASKDWORD, 0x0046a911);
	/*one shot, path A LOK & iqk*/
	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xf9000000);
	rtl_set_bbreg(hw, RIQK_AGC_PTS, MASKDWORD, 0xf8000000);

	mdelay(IQK_DELAY_TIME);

	reg_eac = rtl_get_bbreg(hw, RRX_POWER_AFTER_IQK_A_2, MASKDWORD);
	reg_e94 = rtl_get_bbreg(hw, RTX_POWER_BEFORE_IQK_A, MASKDWORD);
	reg_e9c = rtl_get_bbreg(hw, RTX_POWER_AFTER_IQK_A, MASKDWORD);
	reg_ea4 = rtl_get_bbreg(hw, RRX_POWER_BEFORE_IQK_A_2, MASKDWORD);

	if (!(reg_eac & BIT(27)) &&
	    (((reg_ea4 & 0x03FF0000) >> 16) != 0x132) &&
	    (((reg_eac & 0x03FF0000) >> 16) != 0x36))
		result |= 0x02;
	return result;
}

static void fill_iqk(struct ieee80211_hw *hw, bool iqk_ok, long result[][8],
		     u8 final, bool btxonly)
{
	u32 oldval_0, x, tx0_a, reg;
	long y, tx0_c;

	if (final == 0xFF) {
		return;
	} else if (iqk_ok) {
		oldval_0 = (rtl_get_bbreg(hw, ROFDM0_XATXIQIMBAL,
					  MASKDWORD) >> 22) & 0x3FF;
		x = result[final][0];
		if ((x & 0x00000200) != 0)
			x = x | 0xFFFFFC00;
		tx0_a = (x * oldval_0) >> 8;
		rtl_set_bbreg(hw, ROFDM0_XATXIQIMBAL, 0x3FF, tx0_a);
		rtl_set_bbreg(hw, ROFDM0_ECCATHRES, BIT(31),
			      ((x * oldval_0 >> 7) & 0x1));
		y = result[final][1];
		if ((y & 0x00000200) != 0)
			y |= 0xFFFFFC00;
		tx0_c = (y * oldval_0) >> 8;
		rtl_set_bbreg(hw, ROFDM0_XCTXAFE, 0xF0000000,
			      ((tx0_c & 0x3C0) >> 6));
		rtl_set_bbreg(hw, ROFDM0_XATXIQIMBAL, 0x003F0000,
			      (tx0_c & 0x3F));
		rtl_set_bbreg(hw, ROFDM0_ECCATHRES, BIT(29),
			      ((y * oldval_0 >> 7) & 0x1));
		if (btxonly)
			return;
		reg = result[final][2];
		rtl_set_bbreg(hw, ROFDM0_XARXIQIMBAL, 0x3FF, reg);
		reg = result[final][3] & 0x3F;
		rtl_set_bbreg(hw, ROFDM0_XARXIQIMBAL, 0xFC00, reg);
		reg = (result[final][3] >> 6) & 0xF;
		rtl_set_bbreg(hw, 0xca0, 0xF0000000, reg);
	}
}

static void save_adda_reg(struct ieee80211_hw *hw,
			  const u32 *addareg, u32 *backup,
			  u32 registernum)
{
	u32 i;

	for (i = 0; i < registernum; i++)
		backup[i] = rtl_get_bbreg(hw, addareg[i], MASKDWORD);
}

static void save_mac_reg(struct ieee80211_hw *hw, const u32 *macreg,
			 u32 *macbackup)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 i;

	for (i = 0; i < (IQK_MAC_REG_NUM - 1); i++)
		macbackup[i] = rtl_read_byte(rtlpriv, macreg[i]);
	macbackup[i] = rtl_read_dword(rtlpriv, macreg[i]);
}

static void reload_adda(struct ieee80211_hw *hw, const u32 *addareg,
		        u32 *backup, u32 reg_num)
{
	u32 i;

	for (i = 0; i < reg_num; i++)
		rtl_set_bbreg(hw, addareg[i], MASKDWORD, backup[i]);
}

static void reload_mac(struct ieee80211_hw *hw, const u32 *macreg,
		       u32 *macbackup)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 i;

	for (i = 0; i < (IQK_MAC_REG_NUM - 1); i++)
		rtl_write_byte(rtlpriv, macreg[i], (u8) macbackup[i]);
	rtl_write_dword(rtlpriv, macreg[i], macbackup[i]);
}

static void _rtl88e_phy_path_adda_on(struct ieee80211_hw *hw,
				     const u32 *addareg, bool is_patha_on,
				     bool is2t)
{
	u32 pathon;
	u32 i;

	pathon = is_patha_on ? 0x04db25a4 : 0x0b1b25a4;
	if (false == is2t) {
		pathon = 0x0bdb25a0;
		rtl_set_bbreg(hw, addareg[0], MASKDWORD, 0x0b1b25a0);
	} else {
		rtl_set_bbreg(hw, addareg[0], MASKDWORD, pathon);
	}

	for (i = 1; i < IQK_ADDA_REG_NUM; i++)
		rtl_set_bbreg(hw, addareg[i], MASKDWORD, pathon);
}

static void _rtl88e_phy_mac_setting_calibration(struct ieee80211_hw *hw,
						const u32 *macreg,
						u32 *macbackup)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	u32 i = 0;

	rtl_write_byte(rtlpriv, macreg[i], 0x3F);

	for (i = 1; i < (IQK_MAC_REG_NUM - 1); i++)
		rtl_write_byte(rtlpriv, macreg[i],
			       (u8) (macbackup[i] & (~BIT(3))));
	rtl_write_byte(rtlpriv, macreg[i], (u8) (macbackup[i] & (~BIT(5))));
}

static void _rtl88e_phy_path_a_standby(struct ieee80211_hw *hw)
{
	rtl_set_bbreg(hw, 0xe28, MASKDWORD, 0x0);
	rtl_set_bbreg(hw, 0x840, MASKDWORD, 0x00010000);
	rtl_set_bbreg(hw, 0xe28, MASKDWORD, 0x80800000);
}

static void _rtl88e_phy_pi_mode_switch(struct ieee80211_hw *hw, bool pi_mode)
{
	u32 mode;

	mode = pi_mode ? 0x01000100 : 0x01000000;
	rtl_set_bbreg(hw, 0x820, MASKDWORD, mode);
	rtl_set_bbreg(hw, 0x828, MASKDWORD, mode);
}

static bool sim_comp(struct ieee80211_hw *hw, long result[][8], u8 c1, u8 c2)
{
	u32 i, j, diff, bitmap, bound;
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));

	u8 final[2] = {0xFF, 0xFF};
	bool bresult = true, is2t = IS_92C_SERIAL(rtlhal->version);

	if (is2t)
		bound = 8;
	else
		bound = 4;

	bitmap = 0;

	for (i = 0; i < bound; i++) {
		diff = (result[c1][i] > result[c2][i]) ?
		       (result[c1][i] - result[c2][i]) :
		       (result[c2][i] - result[c1][i]);

		if (diff > MAX_TOLERANCE) {
			if ((i == 2 || i == 6) && !bitmap) {
				if (result[c1][i] + result[c1][i + 1] == 0)
					final[(i / 4)] = c2;
				else if (result[c2][i] + result[c2][i + 1] == 0)
					final[(i / 4)] = c1;
				else
					bitmap = bitmap | (1 << i);
			} else {
				bitmap = bitmap | (1 << i);
			}
		}
	}

	if (bitmap == 0) {
		for (i = 0; i < (bound / 4); i++) {
			if (final[i] != 0xFF) {
				for (j = i * 4; j < (i + 1) * 4 - 2; j++)
					result[3][j] = result[final[i]][j];
				bresult = false;
			}
		}
		return bresult;
	} else if (!(bitmap & 0x0F)) {
		for (i = 0; i < 4; i++)
			result[3][i] = result[c1][i];
		return false;
	} else if (!(bitmap & 0xF0) && is2t) {
		for (i = 4; i < 8; i++)
			result[3][i] = result[c1][i];
		return false;
	} else {
		return false;
	}
}

static void _rtl88e_phy_iq_calibrate(struct ieee80211_hw *hw,
				     long result[][8], u8 t, bool is2t)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	u32 i;
	u8 patha_ok, pathb_ok;
	const u32 adda_reg[IQK_ADDA_REG_NUM] = {
		0x85c, 0xe6c, 0xe70, 0xe74,
		0xe78, 0xe7c, 0xe80, 0xe84,
		0xe88, 0xe8c, 0xed0, 0xed4,
		0xed8, 0xedc, 0xee0, 0xeec
	};
	const u32 iqk_mac_reg[IQK_MAC_REG_NUM] = {
		0x522, 0x550, 0x551, 0x040
	};
	const u32 iqk_bb_reg[IQK_BB_REG_NUM] = {
		ROFDM0_TRXPATHENABLE, ROFDM0_TRMUXPAR, RFPGA0_XCD_RFINTERFACESW,
		0xb68, 0xb6c, 0x870, 0x860, 0x864, 0x800
	};
	const u32 retrycount = 2;

	if (t == 0) {
		save_adda_reg(hw, adda_reg, rtlphy->adda_backup, 16);
		save_mac_reg(hw, iqk_mac_reg, rtlphy->iqk_mac_backup);
		save_adda_reg(hw, iqk_bb_reg, rtlphy->iqk_bb_backup,
			      IQK_BB_REG_NUM);
	}
	_rtl88e_phy_path_adda_on(hw, adda_reg, true, is2t);
	if (t == 0) {
		rtlphy->rfpi_enable = (u8) rtl_get_bbreg(hw,
					   RFPGA0_XA_HSSIPARAMETER1, BIT(8));
	}

	if (!rtlphy->rfpi_enable)
		_rtl88e_phy_pi_mode_switch(hw, true);
	/*BB Setting*/
	rtl_set_bbreg(hw, 0x800, BIT(24), 0x00);
	rtl_set_bbreg(hw, 0xc04, MASKDWORD, 0x03a05600);
	rtl_set_bbreg(hw, 0xc08, MASKDWORD, 0x000800e4);
	rtl_set_bbreg(hw, 0x874, MASKDWORD, 0x22204000);

	rtl_set_bbreg(hw, 0x870, BIT(10), 0x01);
	rtl_set_bbreg(hw, 0x870, BIT(26), 0x01);
	rtl_set_bbreg(hw, 0x860, BIT(10), 0x00);
	rtl_set_bbreg(hw, 0x864, BIT(10), 0x00);

	if (is2t) {
		rtl_set_bbreg(hw, 0x840, MASKDWORD, 0x00010000);
		rtl_set_bbreg(hw, 0x844, MASKDWORD, 0x00010000);
	}
	_rtl88e_phy_mac_setting_calibration(hw, iqk_mac_reg,
					    rtlphy->iqk_mac_backup);
	rtl_set_bbreg(hw, 0xb68, MASKDWORD, 0x0f600000);
	if (is2t)
		rtl_set_bbreg(hw, 0xb6c, MASKDWORD, 0x0f600000);

	rtl_set_bbreg(hw, 0xe28, MASKDWORD, 0x80800000);
	rtl_set_bbreg(hw, 0xe40, MASKDWORD, 0x01007c00);
	rtl_set_bbreg(hw, 0xe44, MASKDWORD, 0x81004800);
	for (i = 0; i < retrycount; i++) {
		patha_ok = _rtl88e_phy_path_a_iqk(hw, is2t);
		if (patha_ok == 0x01) {
			RT_TRACE(rtlpriv, COMP_INIT, DBG_LOUD,
				 "Path A Tx IQK Success!!\n");
			result[t][0] = (rtl_get_bbreg(hw, 0xe94, MASKDWORD) &
					0x3FF0000) >> 16;
			result[t][1] = (rtl_get_bbreg(hw, 0xe9c, MASKDWORD) &
					0x3FF0000) >> 16;
			break;
		}
	}

	for (i = 0; i < retrycount; i++) {
		patha_ok = _rtl88e_phy_path_a_rx_iqk(hw, is2t);
		if (patha_ok == 0x03) {
			RT_TRACE(rtlpriv, COMP_INIT, DBG_LOUD,
				 "Path A Rx IQK Success!!\n");
			result[t][2] = (rtl_get_bbreg(hw, 0xea4, MASKDWORD) &
					0x3FF0000) >> 16;
			result[t][3] = (rtl_get_bbreg(hw, 0xeac, MASKDWORD) &
					0x3FF0000) >> 16;
			break;
		} else {
			RT_TRACE(rtlpriv, COMP_INIT, DBG_LOUD,
				 "Path a RX iqk fail!!!\n");
		}
	}

	if (0 == patha_ok) {
		RT_TRACE(rtlpriv, COMP_INIT, DBG_LOUD,
			 "Path A IQK Success!!\n");
	}
	if (is2t) {
		_rtl88e_phy_path_a_standby(hw);
		_rtl88e_phy_path_adda_on(hw, adda_reg, false, is2t);
		for (i = 0; i < retrycount; i++) {
			pathb_ok = _rtl88e_phy_path_b_iqk(hw);
			if (pathb_ok == 0x03) {
				result[t][4] = (rtl_get_bbreg(hw,
						0xeb4, MASKDWORD) &
						0x3FF0000) >> 16;
				result[t][5] =
				    (rtl_get_bbreg(hw, 0xebc, MASKDWORD) &
						   0x3FF0000) >> 16;
				result[t][6] =
				    (rtl_get_bbreg(hw, 0xec4, MASKDWORD) &
						   0x3FF0000) >> 16;
				result[t][7] =
				    (rtl_get_bbreg(hw, 0xecc, MASKDWORD) &
						   0x3FF0000) >> 16;
				break;
			} else if (i == (retrycount - 1) && pathb_ok == 0x01) {
				result[t][4] = (rtl_get_bbreg(hw,
						0xeb4, MASKDWORD) &
						0x3FF0000) >> 16;
			}
			result[t][5] = (rtl_get_bbreg(hw, 0xebc, MASKDWORD) &
					0x3FF0000) >> 16;
		}
	}

	rtl_set_bbreg(hw, 0xe28, MASKDWORD, 0);

	if (t != 0) {
		if (!rtlphy->rfpi_enable)
			_rtl88e_phy_pi_mode_switch(hw, false);
		reload_adda(hw, adda_reg, rtlphy->adda_backup, 16);
		reload_mac(hw, iqk_mac_reg, rtlphy->iqk_mac_backup);
		reload_adda(hw, iqk_bb_reg, rtlphy->iqk_bb_backup,
			    IQK_BB_REG_NUM);

		rtl_set_bbreg(hw, 0x840, MASKDWORD, 0x00032ed3);
		if (is2t)
			rtl_set_bbreg(hw, 0x844, MASKDWORD, 0x00032ed3);
		rtl_set_bbreg(hw, 0xe30, MASKDWORD, 0x01008c00);
		rtl_set_bbreg(hw, 0xe34, MASKDWORD, 0x01008c00);
	}
	RT_TRACE(rtlpriv, COMP_INIT, DBG_LOUD, "88ee IQK Finish!!\n");
}

static void _rtl88e_phy_lc_calibrate(struct ieee80211_hw *hw, bool is2t)
{
	u8 tmpreg;
	u32 rf_a_mode = 0, rf_b_mode = 0, lc_cal;
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	int jj = RF90_PATH_A;
	int kk = RF90_PATH_B;

	tmpreg = rtl_read_byte(rtlpriv, 0xd03);

	if ((tmpreg & 0x70) != 0)
		rtl_write_byte(rtlpriv, 0xd03, tmpreg & 0x8F);
	else
		rtl_write_byte(rtlpriv, REG_TXPAUSE, 0xFF);

	if ((tmpreg & 0x70) != 0) {
		rf_a_mode = rtl_get_rfreg(hw, jj, 0x00, MASK12BITS);

		if (is2t)
			rf_b_mode = rtl_get_rfreg(hw, kk, 0x00,
						  MASK12BITS);

		rtl_set_rfreg(hw, jj, 0x00, MASK12BITS,
			      (rf_a_mode & 0x8FFFF) | 0x10000);

		if (is2t)
			rtl_set_rfreg(hw, kk, 0x00, MASK12BITS,
				      (rf_b_mode & 0x8FFFF) | 0x10000);
	}
	lc_cal = rtl_get_rfreg(hw, jj, 0x18, MASK12BITS);

	rtl_set_rfreg(hw, jj, 0x18, MASK12BITS, lc_cal | 0x08000);

	mdelay(100);

	if ((tmpreg & 0x70) != 0) {
		rtl_write_byte(rtlpriv, 0xd03, tmpreg);
		rtl_set_rfreg(hw, jj, 0x00, MASK12BITS, rf_a_mode);

		if (is2t)
			rtl_set_rfreg(hw, kk, 0x00, MASK12BITS,
				      rf_b_mode);
	} else {
		rtl_write_byte(rtlpriv, REG_TXPAUSE, 0x00);
	}
	RT_TRACE(rtlpriv, COMP_INIT, DBG_LOUD, "\n");
}

static void rfpath_switch(struct ieee80211_hw *hw,
			  bool bmain, bool is2t)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_hal *rtlhal = rtl_hal(rtl_priv(hw));
	struct rtl_efuse *fuse = rtl_efuse(rtl_priv(hw));
	RT_TRACE(rtlpriv, COMP_INIT, DBG_LOUD, "\n");

	if (is_hal_stop(rtlhal)) {
		u8 u1btmp;
		u1btmp = rtl_read_byte(rtlpriv, REG_LEDCFG0);
		rtl_write_byte(rtlpriv, REG_LEDCFG0, u1btmp | BIT(7));
		rtl_set_bbreg(hw, rFPGA0_XAB_RFPARAMETER, BIT(13), 0x01);
	}
	if (is2t) {
		if (bmain)
			rtl_set_bbreg(hw, RFPGA0_XB_RFINTERFACEOE,
				      BIT(5) | BIT(6), 0x1);
		else
			rtl_set_bbreg(hw, RFPGA0_XB_RFINTERFACEOE,
				      BIT(5) | BIT(6), 0x2);
	} else {
		rtl_set_bbreg(hw, RFPGA0_XAB_RFINTERFACESW, BIT(8) | BIT(9), 0);
		rtl_set_bbreg(hw, 0x914, MASKLWORD, 0x0201);

		/* We use the RF definition of MAIN and AUX, left antenna and
		 * right antenna repectively.
		 * Default output at AUX.
		 */
		if (bmain) {
			rtl_set_bbreg(hw, RFPGA0_XA_RFINTERFACEOE, BIT(14) |
				      BIT(13) | BIT(12), 0);
			rtl_set_bbreg(hw, RFPGA0_XB_RFINTERFACEOE, BIT(5) |
				      BIT(4) | BIT(3), 0);
			if (fuse->antenna_div_type == CGCS_RX_HW_ANTDIV)
				rtl_set_bbreg(hw, RCONFIG_RAM64X16, BIT(31), 0);
		} else {
			rtl_set_bbreg(hw, RFPGA0_XA_RFINTERFACEOE, BIT(14) |
				      BIT(13) | BIT(12), 1);
			rtl_set_bbreg(hw, RFPGA0_XB_RFINTERFACEOE, BIT(5) |
				      BIT(4) | BIT(3), 1);
			if (fuse->antenna_div_type == CGCS_RX_HW_ANTDIV)
				rtl_set_bbreg(hw, RCONFIG_RAM64X16, BIT(31), 1);
		}
	}
}

#undef IQK_ADDA_REG_NUM
#undef IQK_DELAY_TIME

void rtl88e_phy_iq_calibrate(struct ieee80211_hw *hw, bool recovery)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	long result[4][8];
	u8 i, final;
	bool patha_ok;
	long reg_e94, reg_e9c, reg_ea4, reg_eb4, reg_ebc, reg_tmp = 0;
	bool is12simular, is13simular, is23simular;
	u32 iqk_bb_reg[9] = {
		ROFDM0_XARXIQIMBAL,
		ROFDM0_XBRXIQIMBAL,
		ROFDM0_ECCATHRES,
		ROFDM0_AGCRSSITABLE,
		ROFDM0_XATXIQIMBAL,
		ROFDM0_XBTXIQIMBAL,
		ROFDM0_XCTXAFE,
		ROFDM0_XDTXAFE,
		ROFDM0_RXIQEXTANTA
	};

	if (recovery) {
		reload_adda(hw, iqk_bb_reg, rtlphy->iqk_bb_backup, 9);
		return;
	}

	memset(result, 0, 32 * sizeof(long));
	final = 0xff;
	patha_ok = false;
	is12simular = false;
	is23simular = false;
	is13simular = false;
	for (i = 0; i < 3; i++) {
		if (get_rf_type(rtlphy) == RF_2T2R)
			_rtl88e_phy_iq_calibrate(hw, result, i, true);
		else
			_rtl88e_phy_iq_calibrate(hw, result, i, false);
		if (i == 1) {
			is12simular = sim_comp(hw, result, 0, 1);
			if (is12simular) {
				final = 0;
				break;
			}
		}
		if (i == 2) {
			is13simular = sim_comp(hw, result, 0, 2);
			if (is13simular) {
				final = 0;
				break;
			}
			is23simular = sim_comp(hw, result, 1, 2);
			if (is23simular) {
				final = 1;
			} else {
				for (i = 0; i < 8; i++)
					reg_tmp += result[3][i];

				if (reg_tmp != 0)
					final = 3;
				else
					final = 0xFF;
			}
		}
	}
	for (i = 0; i < 4; i++) {
		reg_e94 = result[i][0];
		reg_e9c = result[i][1];
		reg_ea4 = result[i][2];
		reg_eb4 = result[i][4];
		reg_ebc = result[i][5];
	}
	if (final != 0xff) {
		reg_e94 = result[final][0];
		rtlphy->reg_e94 = reg_e94;
		reg_e9c = result[final][1];
		rtlphy->reg_e9c = reg_e9c;
		reg_ea4 = result[final][2];
		reg_eb4 = result[final][4];
		rtlphy->reg_eb4 = reg_eb4;
		reg_ebc = result[final][5];
		rtlphy->reg_ebc = reg_ebc;
		patha_ok = true;
	} else {
		rtlphy->reg_e94 = 0x100;
		rtlphy->reg_eb4 = 0x100;
		rtlphy->reg_ebc = 0x0;
		rtlphy->reg_e9c = 0x0;
	}
	if (reg_e94 != 0) /*&&(reg_ea4 != 0) */
		fill_iqk(hw, patha_ok, result, final, (reg_ea4 == 0));
	if (final != 0xFF) {
		for (i = 0; i < IQK_MATRIX_REG_NUM; i++)
			rtlphy->iqk_matrix[0].value[0][i] = result[final][i];
		rtlphy->iqk_matrix[0].iqk_done = true;
	}
	save_adda_reg(hw, iqk_bb_reg, rtlphy->iqk_bb_backup, 9);
}

void rtl88e_phy_lc_calibrate(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	struct rtl_hal *rtlhal = &(rtlpriv->rtlhal);
	bool start_conttx = false, singletone = false;
	u32 timeout = 2000, timecount = 0;

	if (start_conttx || singletone)
		return;

	while (rtlpriv->mac80211.act_scanning && timecount < timeout) {
		udelay(50);
		timecount += 50;
	}

	rtlphy->lck_inprogress = true;
	RTPRINT(rtlpriv, FINIT, INIT_IQK,
		"LCK:Start!!! currentband %x delay %d ms\n",
		 rtlhal->current_bandtype, timecount);

	_rtl88e_phy_lc_calibrate(hw, false);

	rtlphy->lck_inprogress = false;
}

void rtl88e_phy_set_rfpath_switch(struct ieee80211_hw *hw, bool bmain)
{
	rfpath_switch(hw, bmain, false);
}

bool rtl88e_phy_set_io_cmd(struct ieee80211_hw *hw, enum io_type iotype)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_phy *rtlphy = &(rtlpriv->phy);
	bool postprocessing = false;

	RT_TRACE(rtlpriv, COMP_CMD, DBG_TRACE,
		 "-->IO Cmd(%#x), set_io_inprogress(%d)\n",
		 iotype, rtlphy->set_io_inprogress);
	do {
		switch (iotype) {
		case IO_CMD_RESUME_DM_BY_SCAN:
			RT_TRACE(rtlpriv, COMP_CMD, DBG_TRACE,
				 "[IO CMD] Resume DM after scan.\n");
			postprocessing = true;
			break;
		case IO_CMD_PAUSE_DM_BY_SCAN:
			RT_TRACE(rtlpriv, COMP_CMD, DBG_TRACE,
				 "[IO CMD] Pause DM before scan.\n");
			postprocessing = true;
			break;
		default:
			RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG,
				 "switch case not processed\n");
			break;
		}
	} while (false);
	if (postprocessing && !rtlphy->set_io_inprogress) {
		rtlphy->set_io_inprogress = true;
		rtlphy->current_io_type = iotype;
	} else {
		return false;
	}
	rtl88e_phy_set_io(hw);
	RT_TRACE(rtlpriv, COMP_CMD, DBG_TRACE, "IO Type(%#x)\n", iotype);
	return true;
}

static void rtl88ee_phy_set_rf_on(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);

	rtl_write_byte(rtlpriv, REG_SPS0_CTRL, 0x2b);
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN, 0xE3);
	/*rtl_write_byte(rtlpriv, REG_APSD_CTRL, 0x00);*/
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN, 0xE2);
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN, 0xE3);
	rtl_write_byte(rtlpriv, REG_TXPAUSE, 0x00);
}

static void _rtl88ee_phy_set_rf_sleep(struct ieee80211_hw *hw)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	int jj = RF90_PATH_A;

	rtl_write_byte(rtlpriv, REG_TXPAUSE, 0xFF);
	rtl_set_rfreg(hw, jj, 0x00, RFREG_OFFSET_MASK, 0x00);
	rtl_write_byte(rtlpriv, REG_SYS_FUNC_EN, 0xE2);
	rtl_write_byte(rtlpriv, REG_SPS0_CTRL, 0x22);
}

static bool _rtl88ee_phy_set_rf_power_state(struct ieee80211_hw *hw,
					    enum rf_pwrstate rfpwr_state)
{
	struct rtl_priv *rtlpriv = rtl_priv(hw);
	struct rtl_pci_priv *pcipriv = rtl_pcipriv(hw);
	struct rtl_mac *mac = rtl_mac(rtl_priv(hw));
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	struct rtl8192_tx_ring *ring = NULL;
	bool bresult = true;
	u8 i, queue_id;

	switch (rfpwr_state) {
	case ERFON:{
		if ((ppsc->rfpwr_state == ERFOFF) &&
		    RT_IN_PS_LEVEL(ppsc, RT_RF_OFF_LEVL_HALT_NIC)) {
			bool rtstatus;
			u32 init = 0;
			do {
				init++;
				RT_TRACE(rtlpriv, COMP_RF, DBG_DMESG,
					 "IPS Set eRf nic enable\n");
				rtstatus = rtl_ps_enable_nic(hw);
			} while ((rtstatus != true) && (init < 10));
			RT_CLEAR_PS_LEVEL(ppsc,
					  RT_RF_OFF_LEVL_HALT_NIC);
		} else {
			RT_TRACE(rtlpriv, COMP_RF, DBG_DMESG,
				 "Set ERFON sleeped:%d ms\n",
				 jiffies_to_msecs(jiffies - ppsc->
						  last_sleep_jiffies));
			ppsc->last_awake_jiffies = jiffies;
			rtl88ee_phy_set_rf_on(hw);
		}
		if (mac->link_state == MAC80211_LINKED)
			rtlpriv->cfg->ops->led_control(hw, LED_CTL_LINK);
		else
			rtlpriv->cfg->ops->led_control(hw, LED_CTL_NO_LINK);
		break; }
	case ERFOFF:{
		for (queue_id = 0, i = 0;
		     queue_id < RTL_PCI_MAX_TX_QUEUE_COUNT;) {
			ring = &pcipriv->dev.tx_ring[queue_id];
			if (skb_queue_len(&ring->queue) == 0) {
				queue_id++;
				continue;
			} else {
				RT_TRACE(rtlpriv, COMP_ERR, DBG_WARNING,
					 "eRf Off/Sleep: %d times TcbBusyQueue[%d] =%d before doze!\n",
					 (i + 1), queue_id,
					 skb_queue_len(&ring->queue));

				udelay(10);
				i++;
			}
			if (i >= MAX_DOZE_WAITING_TIMES_9x) {
				RT_TRACE(rtlpriv, COMP_ERR, DBG_WARNING,
					 "\n ERFSLEEP: %d times TcbBusyQueue[%d] = %d !\n",
					  MAX_DOZE_WAITING_TIMES_9x,
					  queue_id,
					  skb_queue_len(&ring->queue));
				break;
			}
		}
		if (ppsc->reg_rfps_level & RT_RF_OFF_LEVL_HALT_NIC) {
			RT_TRACE(rtlpriv, COMP_RF, DBG_DMESG,
				 "IPS Set eRf nic disable\n");
			rtl_ps_disable_nic(hw);
			RT_SET_PS_LEVEL(ppsc, RT_RF_OFF_LEVL_HALT_NIC);
		} else {
			if (ppsc->rfoff_reason == RF_CHANGE_BY_IPS) {
				rtlpriv->cfg->ops->led_control(hw,
						LED_CTL_NO_LINK);
			} else {
				rtlpriv->cfg->ops->led_control(hw,
						LED_CTL_POWER_OFF);
			}
		}
		break; }
	case ERFSLEEP:{
		if (ppsc->rfpwr_state == ERFOFF)
			break;
		for (queue_id = 0, i = 0;
		     queue_id < RTL_PCI_MAX_TX_QUEUE_COUNT;) {
			ring = &pcipriv->dev.tx_ring[queue_id];
			if (skb_queue_len(&ring->queue) == 0) {
				queue_id++;
				continue;
			} else {
				RT_TRACE(rtlpriv, COMP_ERR, DBG_WARNING,
					 "eRf Off/Sleep: %d times TcbBusyQueue[%d] =%d before doze!\n",
					 (i + 1), queue_id,
					 skb_queue_len(&ring->queue));

				udelay(10);
				i++;
			}
			if (i >= MAX_DOZE_WAITING_TIMES_9x) {
				RT_TRACE(rtlpriv, COMP_ERR, DBG_WARNING,
					 "\n ERFSLEEP: %d times TcbBusyQueue[%d] = %d !\n",
					 MAX_DOZE_WAITING_TIMES_9x,
					 queue_id,
					 skb_queue_len(&ring->queue));
				break;
			}
		}
		RT_TRACE(rtlpriv, COMP_RF, DBG_DMESG,
			 "Set ERFSLEEP awaked:%d ms\n",
			 jiffies_to_msecs(jiffies - ppsc->last_awake_jiffies));
		ppsc->last_sleep_jiffies = jiffies;
		_rtl88ee_phy_set_rf_sleep(hw);
		break; }
	default:
		RT_TRACE(rtlpriv, COMP_ERR, DBG_EMERG,
			 "switch case not processed\n");
		bresult = false;
		break;
	}
	if (bresult)
		ppsc->rfpwr_state = rfpwr_state;
	return bresult;
}

bool rtl88e_phy_set_rf_power_state(struct ieee80211_hw *hw,
				   enum rf_pwrstate rfpwr_state)
{
	struct rtl_ps_ctl *ppsc = rtl_psc(rtl_priv(hw));
	bool bresult;

	if (rfpwr_state == ppsc->rfpwr_state)
		return false;
	bresult = _rtl88ee_phy_set_rf_power_state(hw, rfpwr_state);
	return bresult;
}
