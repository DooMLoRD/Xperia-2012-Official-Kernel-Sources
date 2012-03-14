/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2009  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2009 The Android Open Source Project
 *  Copyright (C) 2011 Sony Ericsson Mobile Communications AB.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <private/android_filesystem_config.h>
#include <sys/prctl.h>
#include <linux/capability.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#ifdef SEMC_BLUETOOTH_STE
#include <bluetooth/hci_lib.h>
#include "adapter.h"
#endif

/* Set UID to bluetooth w/ CAP_NET_RAW, CAP_NET_ADMIN and CAP_NET_BIND_SERVICE
 * (Android's init.rc does not yet support applying linux capabilities) */
void android_set_aid_and_cap() {
	prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
	setuid(AID_BLUETOOTH);

	struct __user_cap_header_struct header;
	struct __user_cap_data_struct cap;
	header.version = _LINUX_CAPABILITY_VERSION;
	header.pid = 0;
	cap.effective = cap.permitted = 1 << CAP_NET_RAW |
					1 << CAP_NET_ADMIN |
					1 << CAP_NET_BIND_SERVICE;
	cap.inheritable = 0;
	capset(&header, &cap);
}

#if defined(BOARD_HAVE_BLUETOOTH_BCM) || defined(SEMC_BLUETOOTH_TI) || \
		defined(SEMC_BLUETOOTH_STE)

#ifdef BOARD_HAVE_BLUETOOTH_BCM
static int write_flush_timeout(int fd, uint16_t handle,
        unsigned int timeout_ms) {
    uint16_t timeout = (timeout_ms * 1000) / 625;  // timeout units of 0.625ms
    unsigned char hci_write_flush_cmd[] = {
        0x01,               // HCI command packet
        0x28, 0x0C,         // HCI_Write_Automatic_Flush_Timeout
        0x04,               // Length
        0x00, 0x00,         // Handle
        0x00, 0x00,         // Timeout
    };

    hci_write_flush_cmd[4] = (uint8_t)handle;
    hci_write_flush_cmd[5] = (uint8_t)(handle >> 8);
    hci_write_flush_cmd[6] = (uint8_t)timeout;
    hci_write_flush_cmd[7] = (uint8_t)(timeout >> 8);

    int ret = write(fd, hci_write_flush_cmd, sizeof(hci_write_flush_cmd));
    if (ret < 0) {
        error("write(): %s (%d)]", strerror(errno), errno);
        return -1;
    } else if (ret != sizeof(hci_write_flush_cmd)) {
        error("write(): unexpected length %d", ret);
        return -1;
    }
    return 0;
}

static int vendor_high_priority(int fd, uint16_t handle) {
    unsigned char hci_sleep_cmd[] = {
        0x01,               // HCI command packet
        0x57, 0xfc,         // HCI_Write_High_Priority_Connection
        0x02,               // Length
        0x00, 0x00          // Handle
    };

    hci_sleep_cmd[4] = (uint8_t)handle;
    hci_sleep_cmd[5] = (uint8_t)(handle >> 8);

    int ret = write(fd, hci_sleep_cmd, sizeof(hci_sleep_cmd));
    if (ret < 0) {
        error("write(): %s (%d)]", strerror(errno), errno);
        return -1;
    } else if (ret != sizeof(hci_sleep_cmd)) {
        error("write(): unexpected length %d", ret);
        return -1;
    }
    return 0;
}
#endif

#ifdef SEMC_BLUETOOTH_TI
static int write_flush_timeout(int fd, uint16_t handle,
        unsigned int timeout_ms) {
    // do not use flush timeout on TI hardware
    return 0;
}

static int vendor_high_priority(int fd, uint16_t handle) {
    // set of flow spec parameters recommended by TI
    unsigned char hci_flow_spec_cmd[] = {
        0x01,                   // HCI command packet
        0x10, 0x08,             // HCI_Flow_Specification
        0x15,                   // Length
        0x00, 0x00,             // Handle
        0x00,                   // Flags(reserved)
        0x00,                   // Flow direction=outgoing
        0x02,                   // Service type=guaranteed
        0xa8, 0x61, 0x00, 0x00, // Token rate=25000
        0x4d, 0x01, 0x00, 0x00, // Token bucket size=333
        0xa8, 0x61, 0x00, 0x00, // Peak bandwidth=25000
        0xc8, 0x32, 0x00, 0x00, // Access latency=13000
    };

    hci_flow_spec_cmd[4] = (uint8_t)handle;
    hci_flow_spec_cmd[5] = (uint8_t)(handle >> 8);

    int ret = write(fd, hci_flow_spec_cmd, sizeof(hci_flow_spec_cmd));
    if (ret < 0) {
        error("write(): %s (%d)]", strerror(errno), errno);
        return -1;
    } else if (ret != sizeof(hci_flow_spec_cmd)) {
        error("write(): unexpected length %d", ret);
        return -1;
    }
    return 0;
}
#endif

#ifdef SEMC_BLUETOOTH_STE
static int write_flush_timeout(int fd, uint16_t handle,
		unsigned int timeout_ms) {
	// do not use flush timeout on ST-E hardware
	return 0;
}

static bdaddr_t get_local_bdaddr(int dev_id) {
	bdaddr_t local;

	hci_devba(dev_id, &local);

	return local;
}

static int hci_flow_spec(int fd, uint16_t handle, uint8_t qos) {
	bdaddr_t bdaddr;
	// set of EDR flow spec parameters measured when enabling QoS for ST-E
	unsigned char hci_flow_spec_cmd[] = {
		0x01,                   // HCI command packet
		0x10, 0x08,             // HCI_Flow_Specification
		0x15,                   // Length
		0x00, 0x00,             // Handle
		0x00,                   // Flags (reserved)
		0x00,                   // Flow direction (outgoing)
		0x02,                   // Service type (guaranteed)
		0x64, 0x00, 0x00, 0x00, // Token rate (100)
		0xa0, 0x02, 0x00, 0x00, // Token bucket size (672)
		0x64, 0x00, 0x00, 0x00, // Peak bandwidth (100)
		0x50, 0xc3, 0x00, 0x00, // Access latency (50000)
	};

	hci_flow_spec_cmd[4] = (uint8_t)handle;
	hci_flow_spec_cmd[5] = (uint8_t)(handle >> 8);

	if (!qos) {
		memset(&hci_flow_spec_cmd[6], 0x00, sizeof(hci_flow_spec_cmd)-6);
	} else {
		bdaddr = get_local_bdaddr(0); // hci0
		if (!adapter_device_is_edr(&bdaddr, handle)) {
			/* Remote device does not support EDR, change to BR values. */
			hci_flow_spec_cmd[21] = 0x04; // Access latency (42500)
			hci_flow_spec_cmd[22] = 0xA6;
		}
	}

	int ret = write(fd, hci_flow_spec_cmd, sizeof(hci_flow_spec_cmd));
	if (ret < 0) {
		error("write(): %s (%d)]", strerror(errno), errno);
		return -1;
	} else if (ret != sizeof(hci_flow_spec_cmd)) {
		error("write(): unexpected length %d", ret);
		return -1;
	}
	return 0;
}

static int hci_vs_ext_flow_spec(int fd, uint16_t handle, uint8_t qos) {
	bdaddr_t bdaddr;
	// set of extended EDR flow spec parameters measured when enabling QoS for ST-E
	unsigned char hci_vs_ext_flow_spec_cmd[] = {
		0x01,                   // HCI command packet
		0xd5, 0xfc,             // HCI_VS_Extended_Flow_Specification
		0x0b,                   // Length
		0x00, 0x00,             // Handle
		0x50, 0x00,             // Service interval (80)
		0xb8, 0x0c,             // Outgoing service window (3256)
		0x00, 0x00,             // Incoming service window
		0x9a,                   // CQAE (2DH3)
		0xa0, 0x02,             // Packet size (672)
	};

	hci_vs_ext_flow_spec_cmd[4] = (uint8_t)handle;
	hci_vs_ext_flow_spec_cmd[5] = (uint8_t)(handle >> 8);

	if (!qos) {
		memset(&hci_vs_ext_flow_spec_cmd[6], 0x00, sizeof(hci_vs_ext_flow_spec_cmd)-6);
		hci_vs_ext_flow_spec_cmd[12] = 0x8E; // CQAE (DM5)
	} else {
		bdaddr = get_local_bdaddr(0); // hci0
		if (!adapter_device_is_edr(&bdaddr, handle)) {
			/* Remote device does not support EDR, change to BR values. */
			hci_vs_ext_flow_spec_cmd[6] = 0x44; // Service interval (68)
			hci_vs_ext_flow_spec_cmd[7] = 0x00;
			hci_vs_ext_flow_spec_cmd[8] = 0xE8; // Outgoing service window (1512)
			hci_vs_ext_flow_spec_cmd[9] = 0x05;
			hci_vs_ext_flow_spec_cmd[12] = 0x8E; // CQAE (DH5)
		}
	}

	int ret = write(fd, hci_vs_ext_flow_spec_cmd, sizeof(hci_vs_ext_flow_spec_cmd));
	if (ret < 0) {
		error("write(): %s (%d)]", strerror(errno), errno);
		return -1;
	} else if (ret != sizeof(hci_vs_ext_flow_spec_cmd)) {
		error("write(): unexpected length %d", ret);
		return -1;
	}
	return 0;
}

static int vendor_high_priority(int fd, uint16_t handle, uint32_t link_mode) {
	// enable QoS
	bdaddr_t bdaddr = get_local_bdaddr(0); // hci0

	if (link_mode & HCI_LM_MASTER) {
		adapter_device_set_qos_role(&bdaddr, handle, 0);
		return hci_vs_ext_flow_spec(fd, handle, 1);
	}
	else {
		adapter_device_set_qos_role(&bdaddr, handle, 1);
		return hci_flow_spec(fd, handle, 1);
	}
}

static int vendor_normal_priority(int fd, uint16_t handle) {
	// disable QoS
	bdaddr_t bdaddr = get_local_bdaddr(0); // hci0

	if (adapter_device_get_qos_role(&bdaddr, handle) == 0) {
		return hci_vs_ext_flow_spec(fd, handle, 0);
	}
	else {
		return hci_flow_spec(fd, handle, 0);
	}
}
#endif

static int get_hci_sock() {
    int sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    struct sockaddr_hci addr;
    int opt;

    if(sock < 0) {
        error("Can't create raw HCI socket!");
        return -1;
    }

    opt = 1;
    if (setsockopt(sock, SOL_HCI, HCI_DATA_DIR, &opt, sizeof(opt)) < 0) {
        error("Error setting data direction\n");
        return -1;
    }

    /* Bind socket to the HCI device */
    addr.hci_family = AF_BLUETOOTH;
    addr.hci_dev = 0;  // hci0
    if(bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        error("Can't attach to device hci0. %s(%d)\n",
             strerror(errno),
             errno);
        return -1;
    }
    return sock;
}

static int get_acl_handle(int fd, bdaddr_t *bdaddr) {
    int i;
    int ret = -1;
    struct hci_conn_list_req *conn_list;
    struct hci_conn_info *conn_info;
    int max_conn = 10;

    conn_list = malloc(max_conn * (
            sizeof(struct hci_conn_list_req) + sizeof(struct hci_conn_info)));
    if (!conn_list) {
        error("Out of memory in %s\n", __FUNCTION__);
        return -1;
    }

    conn_list->dev_id = 0;  /* hardcoded to HCI device 0 */
    conn_list->conn_num = max_conn;

    if (ioctl(fd, HCIGETCONNLIST, (void *)conn_list)) {
        error("Failed to get connection list\n");
        goto out;
    }

    for (i=0; i < conn_list->conn_num; i++) {
        conn_info = &conn_list->conn_info[i];
        if (conn_info->type == ACL_LINK &&
                !memcmp((void *)&conn_info->bdaddr, (void *)bdaddr,
                sizeof(bdaddr_t))) {
            ret = conn_info->handle;
            goto out;
        }
    }
    ret = 0;

out:
    free(conn_list);
    return ret;
}

#ifdef SEMC_BLUETOOTH_STE
static int get_link_info(int fd, bdaddr_t *bdaddr,
		uint16_t *acl_handle, uint32_t *link_mode) {
	int i;
	int ret = -1;
	struct hci_conn_list_req *conn_list;
	struct hci_conn_info *conn_info;
	int max_conn = 10;

	conn_list = malloc(max_conn * (
			sizeof(struct hci_conn_list_req) + sizeof(struct hci_conn_info)));
	if (!conn_list) {
		error("Out of memory in %s\n", __FUNCTION__);
		return -1;
	}

	conn_list->dev_id = 0;  /* hardcoded to HCI device 0 */
	conn_list->conn_num = max_conn;

	if (ioctl(fd, HCIGETCONNLIST, (void *)conn_list)) {
		error("Failed to get connection list\n");
		goto out;
	}

	for (i=0; i < conn_list->conn_num; i++) {
		conn_info = &conn_list->conn_info[i];
		if (conn_info->type == ACL_LINK &&
				!memcmp((void *)&conn_info->bdaddr, (void *)bdaddr,
				sizeof(bdaddr_t))) {
			*acl_handle = conn_info->handle;
			*link_mode = conn_info->link_mode;
			ret = 1;
			goto out;
		}
	}
	ret = 0;

out:
	free(conn_list);
	return ret;
}

int android_set_normal_priority(bdaddr_t *ba) {
	int ret;
	int fd = get_hci_sock();
	int acl_handle;

	if (fd < 0)
		return fd;

	acl_handle = get_acl_handle(fd, ba);
	if (acl_handle < 0) {
		ret = acl_handle;
		goto out;
	}

	ret = vendor_normal_priority(fd, acl_handle);

out:
	close(fd);

	return ret;
}

int android_set_high_priority(bdaddr_t *ba) {
	int ret;
	int fd = get_hci_sock();
	uint16_t acl_handle;
	uint32_t link_mode;

	if (fd < 0)
		return fd;

	ret = get_link_info(fd, ba, &acl_handle, &link_mode);
	if (ret < 0)
		goto out;

	ret = vendor_high_priority(fd, acl_handle, link_mode);
	if (ret < 0)
		goto out;
	ret = write_flush_timeout(fd, acl_handle, 200);

out:
	close(fd);

	return ret;
}

#else

/* Request that the ACL link to a given Bluetooth connection be high priority,
 * for improved coexistance support
 */
int android_set_high_priority(bdaddr_t *ba) {
    int ret;
    int fd = get_hci_sock();
    int acl_handle;

    if (fd < 0)
        return fd;

    acl_handle = get_acl_handle(fd, ba);
    if (acl_handle < 0) {
        ret = acl_handle;
        goto out;
    }

    ret = vendor_high_priority(fd, acl_handle);
    if (ret < 0)
        goto out;
    ret = write_flush_timeout(fd, acl_handle, 200);

out:
    close(fd);

    return ret;
}
#endif

#else

int android_set_high_priority(bdaddr_t *ba) {
    return 0;
}

#endif
