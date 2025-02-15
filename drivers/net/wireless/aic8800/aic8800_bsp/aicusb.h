/**
 * aicwf_usb.h
 *
 * USB function declarations
 *
 * Copyright (C) AICSemi 2018-2020
 */

#ifndef _AICWF_USB_H_
#define _AICWF_USB_H_

#ifdef AICWF_USB_SUPPORT
#include <linux/usb.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>
#include <linux/ieee80211.h>
#include <linux/semaphore.h>
#include "aic_bsp_driver.h"

/* USB Device ID */
#define USB_VENDOR_ID_AIC               0xA69C
#define USB_VENDOR_ID_AIC_V2            0x368B

#define USB_DEVICE_ID_AIC_8800          0x8800
#define USB_DEVICE_ID_AIC_8801          0x8801

#define USB_DEVICE_ID_AIC_8800D80       0x8D80
#define USB_DEVICE_ID_AIC_8800D81       0x8D81
#define USB_DEVICE_ID_AIC_8800D40       0x8D40
#define USB_DEVICE_ID_AIC_8800D41       0x8D41

#define USB_DEVICE_ID_AIC_8800D80X2     0x8D90
#define USB_DEVICE_ID_AIC_8800D81X2     0x8D91

#define AICWF_USB_RX_URBS               (20)
#define AICWF_USB_TX_URBS               (100)
#define AICWF_USB_TX_LOW_WATER          (AICWF_USB_TX_URBS/4)
#define AICWF_USB_TX_HIGH_WATER         (AICWF_USB_TX_LOW_WATER*3)
#define AICWF_USB_MAX_PKT_SIZE          (2048)


typedef enum {
	PRIV_TYPE_DATA         = 0X00,
	PRIV_TYPE_CFG          = 0X10,
	PRIV_TYPE_CFG_CMD_RSP  = 0X11,
	PRIV_TYPE_CFG_DATA_CFM = 0X12,
	PRIV_TYPE_CFG_PRINT    = 0X13
} priv_type;

enum aicwf_usb_state {
	USB_DOWN_ST,
	USB_UP_ST,
	USB_SLEEP_ST
};

struct aicwf_usb_buf {
	struct list_head list;
	struct priv_dev *aicdev;
	struct urb *urb;
	struct sk_buff *skb;
	bool cfm;
};

struct priv_dev {
	struct rwnx_cmd_mgr cmd_mgr;
	struct aicwf_bus *bus_if;
	struct usb_device *udev;
	struct device *dev;
	struct aicwf_rx_priv *rx_priv;
	enum aicwf_usb_state state;

	struct usb_anchor rx_submitted;
	struct work_struct rx_urb_work;

	spinlock_t rx_free_lock;
	spinlock_t tx_free_lock;
	spinlock_t tx_post_lock;
	spinlock_t tx_flow_lock;

	struct list_head rx_free_list;
	struct list_head tx_free_list;
	struct list_head tx_post_list;

	uint bulk_in_pipe;
	uint bulk_out_pipe;

#ifdef CONFIG_USB_MSG_EP
	uint msg_out_pipe;
	uint use_msg_ep;
#endif

	int tx_free_count;
	int tx_post_count;

	struct aicwf_usb_buf usb_tx_buf[AICWF_USB_TX_URBS];
	struct aicwf_usb_buf usb_rx_buf[AICWF_USB_RX_URBS];

	int msg_finished;
	wait_queue_head_t msg_wait;
	ulong msg_busy;
	struct urb *msg_out_urb;

	bool tbusy;
	bool app_cmp;
	u32 fw_version_uint;
};

void *aicbsp_get_drvdata(void *args);
int aicwf_bustx_thread(void *data);
int aicwf_busrx_thread(void *data);
int aicwf_process_rxframes(struct aicwf_rx_priv *rx_priv);
#endif /* AICWF_USB_SUPPORT */

#endif /* _AICWF_USB_H_       */
