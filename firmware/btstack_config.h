#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// sw2bridge: BLE central only (Switch 2 controller), no classic, no SMP pairing
// (PROTOCOL.md: "Normal Bluetooth SMP pairing is not required").
//
// sm_init() is still required (GATT client's IRK-lookup gate), but an
// unsolicited SMP Security Request must never auto-start pairing: the Pro
// answers pairing with "Pairing Not Supported" and drops the link.
#define ENABLE_EXPLICIT_PAIRING_ON_SECURITY_REQUEST 1

#undef ENABLE_CLASSIC

// BLE central + GATT client (we scan/connect/subscribe).
// ENABLE_BLE is provided by the pico_btstack_ble build (-DENABLE_BLE=1).
#ifndef ENABLE_BLE
#define ENABLE_BLE
#endif
#define ENABLE_LE_CENTRAL
#define ENABLE_GATT_CLIENT
#undef ENABLE_LE_PERIPHERAL

// att_db_util.c is compiled unconditionally by pico_btstack_ble even though
// this firmware never runs an ATT server.
#define MAX_ATT_DB_SIZE 200

// memory / connections
#define MAX_NR_HCI_CONNECTIONS 1
#define MAX_NR_HCI_ACL_PACKETS 6
#define MAX_NR_L2CAP_CHANNELS 1
#define MAX_NR_L2CAP_SERVICES 1
#define MAX_NR_GATT_CLIENTS 1
#define MAX_NR_WHITELIST_ENTRIES 1
#define MAX_NR_SM_LOOKUP_ENTRIES 1
#define MAX_NR_LE_DEVICE_DB_ENTRIES 1

// CYW43 HCI transport requires pre-buffer space for packet header.
// Common input report notifications carry 63-byte GATT values -> needs ATT MTU
// >= 66. HCI ACL payload comfortably larger.
#define HCI_ACL_PAYLOAD_SIZE 1021
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4

#define NVM_NUM_LINK_KEYS 0
#define NVM_NUM_DEVICE_DB_ENTRIES 4

#define HAVE_EMBEDDED_TIME_MS
#define ENABLE_PRINTF_HEXDUMP

#endif
