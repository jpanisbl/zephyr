/*
 * Copyright (c) 2022 Nordic Semiconductor
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/l2cap.h>

#include "babblekit/testcase.h"
#include "babblekit/flags.h"

extern enum bst_result_t bst_result;

static struct bt_conn *default_conn;

#define PSM 0x80

DEFINE_FLAG_STATIC(is_connected);
DEFINE_FLAG_STATIC(chan_connected);
DEFINE_FLAG_STATIC(data_received);

#define DATA_BYTE_VAL  0xBB

/* L2CAP channel buffer pool */
NET_BUF_POOL_DEFINE(buf_pool, 1, BT_L2CAP_SDU_BUF_SIZE(16), CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static void chan_connected_cb(struct bt_l2cap_chan *l2cap_chan)
{
	struct net_buf *buf;
	int err;

	/* Send data immediately on L2CAP connection */
	buf = net_buf_alloc(&buf_pool, K_NO_WAIT);
	if (!buf) {
		TEST_FAIL("Buffer allocation failed");
	}

	(void)net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
	(void)net_buf_add_u8(buf, DATA_BYTE_VAL);

	/* Try to send data */
	err = bt_l2cap_chan_send(l2cap_chan, buf);
	if (err < 0) {
		TEST_FAIL("Could not send data, error %d", err);
	}

	SET_FLAG(chan_connected);
}

static void chan_disconnected_cb(struct bt_l2cap_chan *l2cap_chan)
{
	(void)l2cap_chan;

	UNSET_FLAG(chan_connected);
}

static int chan_recv_cb(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	(void)chan;

	if ((buf->len != 1) || (buf->data[0] != DATA_BYTE_VAL)) {
		TEST_FAIL("Unexpected data received");
	}

	SET_FLAG(data_received);

	return 0;
}

static const struct bt_l2cap_chan_ops l2cap_ops = {
	.connected = chan_connected_cb,
	.disconnected = chan_disconnected_cb,
	.recv = chan_recv_cb,
};

static struct bt_l2cap_le_chan channel;

static int accept(struct bt_conn *conn, struct bt_l2cap_server *server,
		  struct bt_l2cap_chan **l2cap_chan)
{
	channel.chan.ops = &l2cap_ops;
	*l2cap_chan = &channel.chan;

	return 0;
}

static struct bt_l2cap_server server = {
	.accept = accept,
	.sec_level = BT_SECURITY_L1,
	.psm = PSM,
};

static void connect_l2cap_channel(void)
{
	struct bt_l2cap_chan *chans[] = {&channel.chan, NULL};
	int err;

	channel.chan.ops = &l2cap_ops;

	if (IS_ENABLED(CONFIG_BT_L2CAP_ECRED)) {
		err = bt_l2cap_ecred_chan_connect(default_conn, chans, server.psm);
		if (err) {
			TEST_FAIL("Failed to send ecred connection request (err %d)", err);
		}
	} else {
		err = bt_l2cap_chan_connect(default_conn, &channel.chan, server.psm);
		if (err) {
			TEST_FAIL("Failed to send connection request (err %d)", err);
		}
	}
}

static void register_l2cap_server(void)
{
	int err;

	err = bt_l2cap_server_register(&server);
	if (err < 0) {
		TEST_FAIL("Failed to get free server (err %d)", err);
		return;
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		TEST_FAIL("Failed to connect (err %d)", err);
		bt_conn_unref(default_conn);
		default_conn = NULL;
		return;
	}

	default_conn = bt_conn_ref(conn);

	SET_FLAG(is_connected);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (default_conn != conn) {
		TEST_FAIL("Connection mismatch %p %p)", default_conn, conn);
		return;
	}

	bt_conn_unref(default_conn);
	default_conn = NULL;
	UNSET_FLAG(is_connected);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	struct bt_le_conn_param *param;
	int err;

	err = bt_le_scan_stop();
	if (err) {
		TEST_FAIL("Failed to stop scanning (err %d)", err);
		return;
	}

	param = BT_LE_CONN_PARAM_DEFAULT;
	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, param, &default_conn);
	if (err) {
		TEST_FAIL("Failed to create connection (err %d)", err);
		return;
	}
}

static void test_peripheral_main(void)
{
	int err;
	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	};

	err = bt_enable(NULL);
	if (err != 0) {
		TEST_FAIL("Bluetooth init failed (err %d)", err);
		return;
	}

	register_l2cap_server();

	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err != 0) {
		TEST_FAIL("Advertising failed to start (err %d)", err);
		return;
	}

	WAIT_FOR_FLAG(is_connected);

	WAIT_FOR_FLAG(chan_connected);

	WAIT_FOR_FLAG(data_received);

	WAIT_FOR_FLAG_UNSET(is_connected);

TEST_PASS("Test passed");
}

static void test_central_main(void)
{
	int err;

	err = bt_enable(NULL);
	if (err != 0) {
		TEST_FAIL("Bluetooth init failed (err %d)", err);
	}

	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, device_found);
	if (err != 0) {
		TEST_FAIL("Scanning failed to start (err %d)", err);
	}

	WAIT_FOR_FLAG(is_connected);

	connect_l2cap_channel();
	WAIT_FOR_FLAG(chan_connected);

	WAIT_FOR_FLAG(data_received);

	err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	if (err) {
		TEST_FAIL("Failed to disconnect (err %d)", err);
		return;
	}

	WAIT_FOR_FLAG_UNSET(is_connected);

	TEST_PASS("Test passed");
}

static const struct bst_test_instance test_def[] = {
	{
		.test_id = "peripheral",
		.test_descr = "Peripheral",
		.test_main_f = test_peripheral_main,
	},
	{
		.test_id = "central",
		.test_descr = "Central",
		.test_main_f = test_central_main,
	},
	BSTEST_END_MARKER,
};

struct bst_test_list *test_main_l2cap_send_on_connect_install(struct bst_test_list *tests)
{
	return bst_add_tests(tests, test_def);
}
