/*
 * Copyright (C) 2014 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2015 Jakub Kicinski <kubakici@wp.pl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "mt7601u.h"
#include "trace.h"
#include <linux/etherdevice.h>

static void
mt76_mac_process_tx_rate(struct ieee80211_tx_rate *txrate, u16 rate,
			 enum ieee80211_band band)
{
	u8 idx = MT76_GET(MT_TXWI_RATE_MCS, rate);

	txrate->idx = 0;
	txrate->flags = 0;
	txrate->count = 1;

	switch (MT76_GET(MT_TXWI_RATE_PHY_MODE, rate)) {
	case MT_PHY_TYPE_OFDM:
		if (band == IEEE80211_BAND_2GHZ)
			idx += 4;

		txrate->idx = idx;
		return;
	case MT_PHY_TYPE_CCK:
		if (idx >= 8)
			idx -= 8;

		txrate->idx = idx;
		return;
	case MT_PHY_TYPE_HT_GF:
		txrate->flags |= IEEE80211_TX_RC_GREEN_FIELD;
		/* fall through */
	case MT_PHY_TYPE_HT:
		txrate->flags |= IEEE80211_TX_RC_MCS;
		txrate->idx = idx;
		break;
	case MT_PHY_TYPE_VHT:
		txrate->flags |= IEEE80211_TX_RC_VHT_MCS;
		txrate->idx = idx;
		break;
	default:
		WARN_ON(1);
		return;
	}

	switch (MT76_GET(MT_TXWI_RATE_BW, rate)) {
	case MT_PHY_BW_20:
		break;
	case MT_PHY_BW_40:
		txrate->flags |= IEEE80211_TX_RC_40_MHZ_WIDTH;
		break;
	case MT_PHY_BW_80:
		txrate->flags |= IEEE80211_TX_RC_80_MHZ_WIDTH;
		break;
	default:
		WARN_ON(1);
		break;
	}

	if (rate & MT_TXWI_RATE_SGI)
		txrate->flags |= IEEE80211_TX_RC_SHORT_GI;
}

void
mt76_mac_fill_tx_status(struct mt76_dev *dev, struct ieee80211_tx_info *info,
			struct mt76_tx_status *st)
{
	struct ieee80211_tx_rate *rate = info->status.rates;
	int cur_idx, last_rate;
	int i;

	last_rate = min_t(int, st->retry, IEEE80211_TX_MAX_RATES - 1);
	mt76_mac_process_tx_rate(&rate[last_rate], st->rate,
				 dev->chandef.chan->band);
	if (last_rate < IEEE80211_TX_MAX_RATES - 1)
		rate[last_rate + 1].idx = -1;

	cur_idx = rate[last_rate].idx + st->retry;
	for (i = 0; i <= last_rate; i++) {
		rate[i].flags = rate[last_rate].flags;
		rate[i].idx = max_t(int, 0, cur_idx - i);
		rate[i].count = 1;
	}

	if (last_rate > 0)
		rate[last_rate - 1].count = st->retry + 1 - last_rate;

	info->status.ampdu_len = 1;
	info->status.ampdu_ack_len = st->success;

	if (st->pktid & MT_TXWI_PKTID_PROBE)
		info->flags |= IEEE80211_TX_CTL_RATE_CTRL_PROBE;

	if (st->aggr)
		info->flags |= IEEE80211_TX_CTL_AMPDU |
			       IEEE80211_TX_STAT_AMPDU;

	if (!st->ack_req)
		info->flags |= IEEE80211_TX_CTL_NO_ACK;
	else if (st->success)
		info->flags |= IEEE80211_TX_STAT_ACK;
}

__le16
mt76_mac_tx_rate_val(struct mt76_dev *dev, const struct ieee80211_tx_rate *rate,
		     u8 *nss_val)
{
	u16 rateval;
	u8 phy, rate_idx;
	u8 nss = 1;
	u8 bw = 0;

	if (rate->flags & IEEE80211_TX_RC_VHT_MCS) {
		rate_idx = rate->idx;
		nss = 1 + (rate->idx >> 4);
		phy = MT_PHY_TYPE_VHT;
		if (rate->flags & IEEE80211_TX_RC_80_MHZ_WIDTH)
			bw = 2;
		else if (rate->flags & IEEE80211_TX_RC_40_MHZ_WIDTH)
			bw = 1;
	} else if (rate->flags & IEEE80211_TX_RC_MCS) {
		rate_idx = rate->idx;
		nss = 1 + (rate->idx >> 3);
		phy = MT_PHY_TYPE_HT;
		if (rate->flags & IEEE80211_TX_RC_GREEN_FIELD)
			phy = MT_PHY_TYPE_HT_GF;
		if (rate->flags & IEEE80211_TX_RC_40_MHZ_WIDTH)
			bw = 1;
	} else {
		const struct ieee80211_rate *r;
		int band = dev->chandef.chan->band;
		u16 val;

		r = &dev->hw->wiphy->bands[band]->bitrates[rate->idx];
		if (rate->flags & IEEE80211_TX_RC_USE_SHORT_PREAMBLE)
			val = r->hw_value_short;
		else
			val = r->hw_value;

		phy = val >> 8;
		rate_idx = val & 0xff;
		bw = 0;
	}

	rateval = MT76_SET(MT_RXWI_RATE_MCS, rate_idx);
	rateval |= MT76_SET(MT_RXWI_RATE_PHY, phy);
	rateval |= MT76_SET(MT_RXWI_RATE_BW, bw);
	if (rate->flags & IEEE80211_TX_RC_SHORT_GI)
		rateval |= MT_RXWI_RATE_SGI;

	*nss_val = nss;
	return cpu_to_le16(rateval);
}

void mt76_mac_wcid_set_rate(struct mt76_dev *dev, struct mt76_wcid *wcid,
			    const struct ieee80211_tx_rate *rate)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	wcid->tx_rate = mt76_mac_tx_rate_val(dev, rate, &wcid->tx_rate_nss);
	wcid->tx_rate_set = true;
	spin_unlock_irqrestore(&dev->lock, flags);
}

void mt7601u_mac_stat(struct work_struct *work)
{
	struct mt76_dev *dev = container_of(work, struct mt76_dev,
					    stat_work.work);
	u32 stat1, stat2;
	struct mt76_tx_status stat;
	struct ieee80211_tx_info info = {};
	struct ieee80211_sta *sta = NULL;
	struct mt76_wcid *wcid = NULL;
	void *msta;
	int cleaned = 0;

	/* TODO: this may be slow. It takes ca. 200us to read a register
	 *	 we can use MCU_RANDOM_READ but later *_ext will have to be
	 *	 matched back to stat_fifo (there is a possibility of a race).
	 *	 To do that perhaps it's possible to put rate into pkt_id and
	 *	 for stats match pkt_id ~= succ_rate + retry.
	 */
	/* Note: carefull with accessing things here - there is no explicit
	 *	 locking!
	 */
	while(1) {
		stat1 = mt7601u_rr(dev, MT_TX_STAT_FIFO);
		if (!(stat1 & MT_TX_STAT_FIFO_VALID))
			break;

		stat2 = mt76_rr(dev, MT_TX_STAT_FIFO_EXT);
		trace_tx_status(stat1, stat2);

		stat.valid = 1;
		stat.success = !!(stat1 & MT_TX_STAT_FIFO_SUCCESS);
		stat.aggr = !!(stat1 & MT_TX_STAT_FIFO_AGGR);
		stat.ack_req = !!(stat1 & MT_TX_STAT_FIFO_ACKREQ);
		stat.pktid = MT76_GET(MT_TX_STAT_FIFO_PID_TYPE, stat1);
		stat.wcid = MT76_GET(MT_TX_STAT_FIFO_WCID, stat1);
		stat.rate = MT76_GET(MT_TX_STAT_FIFO_RATE, stat1);
		stat.retry = MT76_GET(MT_TX_STAT_FIFO_EXT_RETRY, stat2);

		rcu_read_lock();
		if (stat.wcid < ARRAY_SIZE(dev->wcid))
			wcid = rcu_dereference(dev->wcid[stat.wcid]);

		if (wcid) {
			msta = container_of(wcid, struct mt76_sta, wcid);
			sta = container_of(msta, struct ieee80211_sta,
					   drv_priv);
		}

		mt76_mac_fill_tx_status(dev, &info, &stat);
		ieee80211_tx_status_noskb(dev->hw, sta, &info);
		rcu_read_unlock();

		cleaned++;
	}

	trace_tx_status_cleaned(cleaned);

	if (cleaned || !dev->tx_stat_quiting)
		ieee80211_queue_delayed_work(dev->hw, &dev->stat_work,
					     msecs_to_jiffies(20));
	dev->tx_stat_quiting = !cleaned;
}

void mt7601u_mac_set_protection(struct mt7601u_dev *dev, bool legacy_prot,
				int ht_mode)
{
	int mode = ht_mode & IEEE80211_HT_OP_MODE_PROTECTION;
	bool non_gf = !!(ht_mode & IEEE80211_HT_OP_MODE_NON_GF_STA_PRSNT);
	bool non_ht = !!(ht_mode & IEEE80211_HT_OP_MODE_NON_HT_STA_PRSNT);
	u32 prot[6];
	bool ht_rts[4] = {};
	int i;

	printk("[prot transition] mode:%04hx bgprot:%d non-gf:%d non-ht:%d\n",
	       mode, legacy_prot, non_gf, non_ht);

	/* TODO: vendor sets the legacy protection on during connect and
	 *	 leaves it on - later updates touch only n-modes.
	 */
	prot[0] = MT_PROT_NAV_SHORT |
		  MT_PROT_TXOP_ALLOW_ALL |
		  MT_PROT_RTS_THR_EN;
	/* TODO: for legacy mode if min_rate is OFDM, set prot_rate to OFDM_6 */
	prot[1] = prot[0];
	if (legacy_prot)
		prot[1] |= MT_PROT_CTRL_CTS2SELF;

	prot[2] = prot[4] = MT_PROT_NAV_SHORT | MT_PROT_TXOP_ALLOW_BW20;
	prot[3] = prot[5] = MT_PROT_NAV_SHORT | MT_PROT_TXOP_ALLOW_ALL;

	if (legacy_prot) {
		prot[2] |= MT_PROT_RATE_CCK_11;
		prot[3] |= MT_PROT_RATE_CCK_11;
		prot[4] |= MT_PROT_RATE_CCK_11;
		prot[5] |= MT_PROT_RATE_CCK_11;
	} else {
		prot[2] |= MT_PROT_RATE_OFDM_24;
		prot[3] |= MT_PROT_RATE_DUP_OFDM_24;
		prot[4] |= MT_PROT_RATE_OFDM_24;
		prot[5] |= MT_PROT_RATE_DUP_OFDM_24;
	}

	switch (mode) {
	case IEEE80211_HT_OP_MODE_PROTECTION_NONE:
		break;

	case IEEE80211_HT_OP_MODE_PROTECTION_NONMEMBER:
		ht_rts[0] = ht_rts[1] = ht_rts[2] = ht_rts[3] = true;
		break;

	case IEEE80211_HT_OP_MODE_PROTECTION_20MHZ:
		ht_rts[1] = ht_rts[3] = true;
		break;

	case IEEE80211_HT_OP_MODE_PROTECTION_NONHT_MIXED:
		ht_rts[0] = ht_rts[1] = ht_rts[2] = ht_rts[3] = true;
		break;
	}

	if (non_gf)
		ht_rts[2] = ht_rts[3] = true;

	/* TODO: vendor also turns ht_rts on for:
	 *	 AP - when there are "bad Atheros" clients
	 *	 STA - when there are BA sessions active.
	 */

	for (i = 0; i < 4; i++)
		if (ht_rts[i])
			prot[i + 2] |= MT_PROT_CTRL_RTS_CTS;

	for (i = 0; i < 6; i++)
		mt7601u_wr(dev, MT_CCK_PROT_CFG + i * 4, prot[i]);
}

void mt7601u_mac_set_short_preamble(struct mt7601u_dev *dev, bool short_preamb)
{
	if (short_preamb)
		mt76_set(dev, MT_AUTO_RSP_CFG, MT_AUTO_RSP_PREAMB_SHORT);
	else
		mt76_clear(dev, MT_AUTO_RSP_CFG, MT_AUTO_RSP_PREAMB_SHORT);
}

void mt7601u_mac_config_tsf(struct mt7601u_dev *dev, bool enable, int interval)
{
	u32 val = mt7601u_rr(dev, MT_BEACON_TIME_CFG);

	val &= ~(MT_BEACON_TIME_CFG_TIMER_EN |
		 MT_BEACON_TIME_CFG_SYNC_MODE |
		 MT_BEACON_TIME_CFG_TBTT_EN);

	if (!enable) {
		mt7601u_wr(dev, MT_BEACON_TIME_CFG, val);
		return;
	}

	val &= ~MT_BEACON_TIME_CFG_INTVAL;
	val |= MT76_SET(MT_BEACON_TIME_CFG_INTVAL, interval << 4) |
		MT_BEACON_TIME_CFG_TIMER_EN |
		MT_BEACON_TIME_CFG_SYNC_MODE |
		MT_BEACON_TIME_CFG_TBTT_EN;
}

static void mt7601u_check_mac_err(struct mt7601u_dev *dev)
{
	u32 val = mt7601u_rr(dev, 0x10f4);

	if (!(val & BIT(29)) || !(val & (BIT(7) | BIT(5))))
		return;

	printk("Warning: MAC specific condition occured\n");

	mt76_set(dev, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_RESET_CSR);
	udelay(10);
	mt76_clear(dev, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_RESET_CSR);
}

void mt7601u_mac_work(struct work_struct *work)
{
	struct mt76_dev *dev = container_of(work, struct mt76_dev,
					    mac_work.work);
	u32 val;
	int i;

	mt7601u_check_mac_err(dev);

	/* TODO: use MCU burst read */
#define read_stats(addr, s1, s2) ({					\
			val = mt76_rr(dev, addr);			\
			dev->stats.s1 += val & 0xffff;			\
			dev->stats.s2 += val >> 16;			\
		})
	read_stats(MT_RX_STA_CNT0, rx_crc_err, rx_phy_err);
	read_stats(MT_RX_STA_CNT1, rx_false_cca, rx_plcp_err);
	read_stats(MT_RX_STA_CNT2, rx_fifo_overflow, rx_duplicate);

	read_stats(MT_TX_STA_CNT0, tx_fail_cnt, tx_bcn_cnt);
	read_stats(MT_TX_STA_CNT1, tx_success, tx_retransmit);
	read_stats(MT_TX_STA_CNT2, tx_zero_len, tx_underflow);

	read_stats(MT_TX_AGG_CNT0, non_aggr_tx, aggr_tx);

	for (i = 0; i < 16; i++) {
		val = mt76_rr(dev, MT_TX_AGG_CNT(i));
		dev->stats.aggr_size[i * 2] += val & 0xffff;
		dev->stats.aggr_size[i * 2 + 1] += val >> 16;
	}

	read_stats(MT_MPDU_DENSITY_CNT, tx_zero_len_del, rx_zero_len_del);
#undef read_stats

	ieee80211_queue_delayed_work(dev->hw, &dev->mac_work, 5 * HZ);
}

void mt7601u_mac_wcid_setup(struct mt76_dev *dev, u8 idx, u8 vif_idx, u8 *mac)
{
	u8 zmac[ETH_ALEN] = {};
	u32 attr;

	attr = MT76_SET(MT_WCID_ATTR_BSS_IDX, vif_idx & 7) |
	       MT76_SET(MT_WCID_ATTR_BSS_IDX_EXT, !!(vif_idx & 8));

	mt76_wr(dev, MT_WCID_ATTR(idx), attr);

	if (mac)
		memcpy(zmac, mac, sizeof(zmac));

	mt7601u_addr_wr(dev, MT_WCID_ADDR(idx), zmac);
}

void mt7601u_mac_set_ampdu_factor(struct mt76_dev *dev,
				  struct ieee80211_sta_ht_cap *cap)
{
	/* TODO: ugly hack - this should be set to max of all the stas
	 * perhaps just set to the max value on init
	 */
	mt7601u_wr(dev, MT_MAX_LEN_CFG, 0xa0fff |
		   MT76_SET(MT_MAX_LEN_CFG_AMPDU, cap->ampdu_factor));
}

static void
mt76_mac_process_rate(struct ieee80211_rx_status *status, u16 rate)
{
	u8 idx = MT76_GET(MT_RXWI_RATE_MCS, rate);

	/* TODO: not sure about math in this switch */
	switch (MT76_GET(MT_RXWI_RATE_PHY, rate)) {
	case MT_PHY_TYPE_OFDM:
		if (WARN_ON(idx >= 8))
			idx = 0;
		idx += 4;

		status->rate_idx = idx;
		return;
	case MT_PHY_TYPE_CCK:
		if (idx >= 8) {
			idx -= 8;
			status->flag |= RX_FLAG_SHORTPRE;
		}

		if (idx >= 4)
			idx = 0;

		status->rate_idx = idx;
		return;
	case MT_PHY_TYPE_HT_GF:
		status->flag |= RX_FLAG_HT_GF;
		/* fall through */
	case MT_PHY_TYPE_HT:
		status->flag |= RX_FLAG_HT;
		status->rate_idx = idx;
		break;
	default:
		WARN_ON(1);
		return;
	}

	if (rate & MT_RXWI_RATE_SGI)
		status->flag |= RX_FLAG_SHORT_GI;

	if (rate & MT_RXWI_RATE_STBC)
		status->flag |= 1 << RX_FLAG_STBC_SHIFT;

	if (rate & MT_RXWI_RATE_BW)
		status->flag |= RX_FLAG_40MHZ;
}

static void
mt7601u_rx_monitor_beacon(struct mt76_dev *dev, struct mt7601u_rxwi *rxwi,
		       u16 rate)
{
	spin_lock_bh(&dev->last_beacon.lock);
	dev->last_beacon.freq_off = rxwi->freq_off;
	dev->last_beacon.phy_mode = MT76_GET(MT_RXWI_RATE_PHY, rate);
	spin_unlock_bh(&dev->last_beacon.lock);
}

static int mt7601u_rx_is_our_beacon(struct mt76_dev *dev, struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	return ieee80211_is_beacon(hdr->frame_control) &&
		ether_addr_equal(hdr->addr2, dev->bssid);
}

int mt76_mac_process_rx(struct mt76_dev *dev, struct sk_buff *skb, void *rxi)
{
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
	struct mt7601u_rxwi *rxwi = rxi;
	u32 ctl = le32_to_cpu(rxwi->ctl);
	u16 rate = le16_to_cpu(rxwi->rate);
	int len, rssi;

	if (rxwi->rxinfo & cpu_to_le32(MT_RXINFO_L2PAD))
		mt76_remove_hdr_pad(skb);

	if (rxwi->rxinfo & cpu_to_le32(MT_RXINFO_DECRYPT)) {
		status->flag |= RX_FLAG_DECRYPTED;
		status->flag |= RX_FLAG_IV_STRIPPED | RX_FLAG_MMIC_STRIPPED;
	}

	len = MT76_GET(MT_RXWI_CTL_MPDU_LEN, ctl);
	skb_trim(skb, len);

	status->chains = BIT(0);
	rssi = mt7601u_phy_get_rssi(dev, rxwi, rate);
	status->chain_signal[0] = status->signal = rssi;
	status->freq = dev->chandef.chan->center_freq;
	status->band = dev->chandef.chan->band;

	mt76_mac_process_rate(status, rate);

	/* TODO: avg rssi has no locking */
	/* TODO: avg rssi approaches real value from above (starts as 0) */
	if (mt7601u_rx_is_our_beacon(dev, skb)) {
		mt7601u_rx_monitor_beacon(dev, rxwi, rate);
		dev->avg_rssi = (dev->avg_rssi * 15) / 16 + (rssi << 8);
	} else if (rxwi->rxinfo & cpu_to_le32(MT_RXINFO_U2M))
		dev->avg_rssi = (dev->avg_rssi * 15) / 16 + (rssi << 8);

	return 0;
}

static enum mt76_cipher_type
mt76_mac_get_key_info(struct ieee80211_key_conf *key, u8 *key_data)
{
	memset(key_data, 0, 32);
	if (!key)
		return MT_CIPHER_NONE;

	if (key->keylen > 32)
		return MT_CIPHER_NONE;

	memcpy(key_data, key->key, key->keylen);

	switch(key->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		return MT_CIPHER_WEP40;
	case WLAN_CIPHER_SUITE_WEP104:
		return MT_CIPHER_WEP104;
	case WLAN_CIPHER_SUITE_TKIP:
		return MT_CIPHER_TKIP;
	case WLAN_CIPHER_SUITE_CCMP:
		return MT_CIPHER_AES_CCMP;
	default:
		return MT_CIPHER_NONE;
	}
}

int mt76_mac_wcid_set_key(struct mt76_dev *dev, u8 idx,
			  struct ieee80211_key_conf *key)
{
	enum mt76_cipher_type cipher;
	u8 key_data[32];
	u8 iv_data[8];
	u32 val;

	cipher = mt76_mac_get_key_info(key, key_data);
	if (cipher == MT_CIPHER_NONE && key)
		return -EINVAL;

	printk("setting key for idx:%02hhx\n", idx);

	mt76_wr_copy(dev, MT_WCID_KEY(idx), key_data, sizeof(key_data));

	memset(iv_data, 0, sizeof(iv_data));
	if (key) {
		iv_data[3] = key->keyidx << 6;
		if (cipher >= MT_CIPHER_TKIP) {
			/* CHANGED: start with 1 to comply with spec,
			 *	    (see comment on common/cmm_wpa.c:4291).
			 */
			iv_data[0] |= 1;
			iv_data[3] |= 0x20;
		}
	}
	mt76_wr_copy(dev, MT_WCID_IV(idx), iv_data, sizeof(iv_data));

	/* CHANGED: move attr updates after all key info is set */
	/* CHANGED: don't use rmw for value changes to save urbs */
	val = mt7601u_rr(dev, MT_WCID_ATTR(idx));
	val &= ~MT_WCID_ATTR_PKEY_MODE & ~MT_WCID_ATTR_PKEY_MODE_EXT;
	val |= MT76_SET(MT_WCID_ATTR_PKEY_MODE, cipher & 7) |
	       MT76_SET(MT_WCID_ATTR_PKEY_MODE_EXT, cipher >> 3);
	val &= ~MT_WCID_ATTR_PAIRWISE;
	val |= MT_WCID_ATTR_PAIRWISE *
		!!(key && key->flags & IEEE80211_KEY_FLAG_PAIRWISE);
	mt7601u_wr(dev, MT_WCID_ATTR(idx), val);

	return 0;
}

int mt76_mac_shared_key_setup(struct mt76_dev *dev, u8 vif_idx, u8 key_idx,
			      struct ieee80211_key_conf *key)
{
	enum mt76_cipher_type cipher;
	u8 key_data[32];
	u32 val;

	cipher = mt76_mac_get_key_info(key, key_data);
	if (cipher == MT_CIPHER_NONE && key)
		return -EINVAL;

	printk("setting key for vif_idx:%02hhx key_idx:%02hhx\n",
	       vif_idx, key_idx);

	mt76_wr_copy(dev, MT_SKEY(vif_idx, key_idx),
		     key_data, sizeof(key_data));

	val = mt76_rr(dev, MT_SKEY_MODE(vif_idx));
	val &= ~(MT_SKEY_MODE_MASK << MT_SKEY_MODE_SHIFT(vif_idx, key_idx));
	val |= cipher << MT_SKEY_MODE_SHIFT(vif_idx, key_idx);
	mt76_wr(dev, MT_SKEY_MODE(vif_idx), val);

	return 0;
}