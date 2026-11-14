// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 */

#include <linux/usb.h>
#include <linux/ieee80211.h>
#include <linux/firmware.h>
#include "mt76.h"

#define XONE_DONGLE_NUM_IN_URBS 12
#define XONE_DONGLE_NUM_OUT_URBS 12

#define XONE_DONGLE_LEN_CMD_PKT 0x0654
#define XONE_DONGLE_LEN_WLAN_PKT 0x8400

#define XONE_DONGLE_MAX_CLIENTS 16

#define CONFIG_PM 1

/* autosuspend delay in ms */
//GK: Reduced to 6s instead of 60s
#define XONE_DONGLE_SUSPEND_DELAY 6000
#define XONE_DONGLE_WITH_CLIENT_SUSPEND_DELAY 900000

#define XONE_DONGLE_PAIRING_TIMEOUT 60 // seconds
#define XONE_DONGLE_PAIR_SCAN_INTERVAL msecs_to_jiffies(2000)
#define XONE_DONGLE_PWR_OFF_TIMEOUT msecs_to_jiffies(5000)
#define XONE_DONGLE_FW_REQ_TIMEOUT_MS 3000
#define XONE_DONGLE_FW_REQ_RETRIES 11 // 30 seconds
#define XONE_DONGLE_FW_LOAD_RETRIES 3

enum xone_dongle_queue {
	XONE_DONGLE_QUEUE_DATA = 0x00,
	XONE_DONGLE_QUEUE_AUDIO = 0x02,
};

enum xone_dongle_fw_state {
	XONE_DONGLE_FW_STATE_PENDING,
	XONE_DONGLE_FW_STATE_STOP_LOADING,
	XONE_DONGLE_FW_STATE_ERROR,
	XONE_DONGLE_FW_STATE_READY,
};

struct xone_dongle_skb_cb {
	struct xone_dongle *dongle;
	struct urb *urb;
};

struct xone_dongle_client {
	struct xone_dongle *dongle;
	u8 wcid;
	u8 address[ETH_ALEN];
	bool encryption_enabled;

	struct gip_adapter *adapter;
};

enum xone_dongle_event_type {
	XONE_DONGLE_EVT_ADD_CLIENT,
	XONE_DONGLE_EVT_REMOVE_CLIENT,
	XONE_DONGLE_EVT_PAIR_CLIENT,
	XONE_DONGLE_EVT_ENABLE_PAIRING,
	XONE_DONGLE_EVT_ENABLE_ENCRYPTION,
};

struct xone_dongle_event {
	enum xone_dongle_event_type type;

	struct xone_dongle *dongle;
	u8 address[ETH_ALEN];
	u8 wcid;

	struct work_struct work;
};

struct xone_dongle {
	struct xone_mt76 mt;

	struct usb_anchor urbs_in_idle;
	struct usb_anchor urbs_in_busy;
	struct usb_anchor urbs_out_idle;
	struct usb_anchor urbs_out_busy;

	/* serializes pairing changes */
	struct mutex pairing_lock;
	struct delayed_work pairing_work;
	struct delayed_work pairing_scan_work;
	bool pairing;
	unsigned long last_wlan_rx;
	u8 pairing_scan_idx;

	/* serializes access to clients array */
	spinlock_t clients_lock;
	struct xone_dongle_client *clients[XONE_DONGLE_MAX_CLIENTS];
	atomic_t client_count;
	wait_queue_head_t disconnect_wait;

	struct workqueue_struct *event_wq;
	struct work_struct load_fw_work;

	enum xone_dongle_fw_state fw_state;
	u16 vendor;
	u16 product;
};

static int xone_dongle_power_off_client(struct xone_dongle *dongle, int index, bool silent);
static int xone_dongle_power_off_clients(struct xone_dongle *dongle);