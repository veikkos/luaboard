/*
 * ESP32 mock platform - the NimBLE host API (GAP central + GATT client).
 *
 * Only what the firmware uses, with the names, types and fields of the real
 * NimBLE headers (ble_hs.h pulls in ble_gap.h, ble_gatt.h, ble_uuid.h, ...
 * there too), so the same code builds against both. Behind it the mock runs
 * a host thread that answers like the real stack, from simulated devices
 * (sim_hw.h) or the computer's own Bluetooth.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- nimble/ble.h ------------------------------------------------------ */
#define BLE_ADDR_PUBLIC      (0x00)
#define BLE_ADDR_RANDOM      (0x01)
#define BLE_ADDR_PUBLIC_ID   (0x02)
#define BLE_ADDR_RANDOM_ID   (0x03)

typedef struct {
    uint8_t type;
    uint8_t val[6];      /* little-endian: val[5] is the first byte as written */
} ble_addr_t;

#define BLE_ERR_CONN_SPVN_TMO        0x08
#define BLE_ERR_REM_USER_CONN_TERM   0x13
#define BLE_ERR_CONN_TERM_LOCAL      0x16
#define BLE_ERR_CONN_ESTABLISHMENT   0x3e

/* ---- host/ble_hs.h: errors ---------------------------------------------- */
#define BLE_HS_FOREVER              INT32_MAX
#define BLE_HS_EALREADY             2
#define BLE_HS_EINVAL               3
#define BLE_HS_ENOMEM               6
#define BLE_HS_ENOTCONN             7
#define BLE_HS_EAPP                 9
#define BLE_HS_ETIMEOUT             13
#define BLE_HS_EDONE                14
#define BLE_HS_EBUSY                15
#define BLE_HS_ENOTSYNCED           22
#define BLE_HS_EAUTHEN              23
#define BLE_HS_ESTORE_CAP           27
#define BLE_HS_EENCRYPT             25
#define BLE_HS_ERR_ATT_BASE         0x100
#define BLE_HS_ATT_ERR(x)           ((x) ? BLE_HS_ERR_ATT_BASE + (x) : 0)
#define BLE_HS_ERR_HCI_BASE         0x200
#define BLE_HS_HCI_ERR(x)           ((x) ? BLE_HS_ERR_HCI_BASE + (x) : 0)

/* ---- host/ble_att.h ------------------------------------------------------ */
#define BLE_ATT_ERR_INVALID_HANDLE          0x01
#define BLE_ATT_ERR_READ_NOT_PERMITTED      0x02
#define BLE_ATT_ERR_WRITE_NOT_PERMITTED     0x03
#define BLE_ATT_ERR_INSUFFICIENT_AUTHEN     0x05
#define BLE_ATT_ERR_REQ_NOT_SUPPORTED       0x06
#define BLE_ATT_ERR_INSUFFICIENT_ENC        0x0f
#define BLE_ATT_ERR_UNLIKELY                0x0e

/* ---- host/ble_uuid.h ---------------------------------------------------- */
enum {
    BLE_UUID_TYPE_16 = 16,
    BLE_UUID_TYPE_32 = 32,
    BLE_UUID_TYPE_128 = 128,
};

typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef struct { ble_uuid_t u; uint32_t value; } ble_uuid32_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;   /* little-endian */
typedef union {
    ble_uuid_t u;
    ble_uuid16_t u16;
    ble_uuid32_t u32;
    ble_uuid128_t u128;
} ble_uuid_any_t;

#define BLE_UUID_STR_LEN (37)
int ble_uuid_cmp(const ble_uuid_t *uuid1, const ble_uuid_t *uuid2);
int ble_uuid_init_from_buf(ble_uuid_any_t *uuid, const void *buf, size_t len);

/* ---- os/os_mbuf.h, host/ble_hs_mbuf.h ---------------------------------- */
/* A received attribute value. The real one is a chain of buffers; the
   firmware only reads it through these two. */
struct os_mbuf_pkthdr { uint16_t omp_len; };
struct os_mbuf {
    struct os_mbuf_pkthdr hdr;
    uint8_t data[512];
};
#define OS_MBUF_PKTLEN(__om) ((__om)->hdr.omp_len)
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *flat, uint16_t max_len, uint16_t *out_copy_len);

/* ---- host/ble_gap.h ----------------------------------------------------- */
#define BLE_GAP_EVENT_CONNECT               0
#define BLE_GAP_EVENT_DISCONNECT            1
#define BLE_GAP_EVENT_DISC                  7
#define BLE_GAP_EVENT_DISC_COMPLETE         8
#define BLE_GAP_EVENT_ENC_CHANGE            10
#define BLE_GAP_EVENT_PASSKEY_ACTION        11
#define BLE_GAP_EVENT_NOTIFY_RX             12
#define BLE_GAP_EVENT_MTU                   15
#define BLE_GAP_EVENT_REPEAT_PAIRING        17

#define BLE_GAP_REPEAT_PAIRING_RETRY        1
#define BLE_GAP_REPEAT_PAIRING_IGNORE       2

#define BLE_HCI_ADV_RPT_EVTYPE_ADV_IND      (0)
#define BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP     (4)

struct ble_gap_disc_params {
    uint16_t itvl;
    uint16_t window;
    uint8_t filter_policy;
    uint8_t limited:1;
    uint8_t passive:1;
    uint8_t filter_duplicates:1;
    uint8_t disable_observer_mode:1;
};

struct ble_gap_disc_desc {
    uint8_t event_type;
    uint8_t length_data;
    ble_addr_t addr;
    int8_t rssi;
    const uint8_t *data;
    ble_addr_t direct_addr;
};

struct ble_gap_sec_state {
    unsigned encrypted:1;
    unsigned authenticated:1;
    unsigned bonded:1;
    unsigned key_size:5;
    unsigned authorize:1;
};

struct ble_gap_conn_desc {
    struct ble_gap_sec_state sec_state;
    ble_addr_t our_id_addr;
    ble_addr_t peer_id_addr;
    ble_addr_t our_ota_addr;
    ble_addr_t peer_ota_addr;
    uint16_t conn_handle;
    uint16_t conn_itvl;
    uint16_t conn_latency;
    uint16_t supervision_timeout;
    uint8_t role;
    uint8_t master_clock_accuracy;
};

struct ble_gap_conn_params;     /* NULL: the stack's defaults (all the mock knows) */

struct ble_gap_event {
    uint8_t type;
    union {
        struct { int status; uint16_t conn_handle; } connect;
        struct { int reason; struct ble_gap_conn_desc conn; } disconnect;
        struct ble_gap_disc_desc disc;
        struct { int reason; } disc_complete;
        struct { int status; uint16_t conn_handle; } enc_change;
        struct { uint16_t conn_handle; } passkey;
        struct {
            struct os_mbuf *om;
            uint16_t attr_handle;
            uint16_t conn_handle;
            uint8_t indication:1;
        } notify_rx;
        struct { uint16_t conn_handle; uint16_t channel_id; uint16_t value; } mtu;
        struct { uint16_t conn_handle; } repeat_pairing;
    };
};

typedef int ble_gap_event_fn(struct ble_gap_event *event, void *arg);

int ble_gap_disc(uint8_t own_addr_type, int32_t duration_ms,
                 const struct ble_gap_disc_params *disc_params,
                 ble_gap_event_fn *cb, void *cb_arg);
int ble_gap_disc_cancel(void);
int ble_gap_disc_active(void);
int ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer_addr,
                    int32_t duration_ms,
                    const struct ble_gap_conn_params *params,
                    ble_gap_event_fn *cb, void *cb_arg);
int ble_gap_conn_cancel(void);
int ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason);
int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *out_desc);
int ble_gap_security_initiate(uint16_t conn_handle);
/** Forget the bond with a peer (and drop the connection to it, if any). */
int ble_gap_unpair(const ble_addr_t *peer_addr);

/* ---- host/ble_gatt.h ---------------------------------------------------- */
#define BLE_GATT_DSC_CLT_CFG_UUID16                     0x2902

#define BLE_GATT_CHR_PROP_BROADCAST                     0x01
#define BLE_GATT_CHR_PROP_READ                          0x02
#define BLE_GATT_CHR_PROP_WRITE_NO_RSP                  0x04
#define BLE_GATT_CHR_PROP_WRITE                         0x08
#define BLE_GATT_CHR_PROP_NOTIFY                        0x10
#define BLE_GATT_CHR_PROP_INDICATE                      0x20
#define BLE_GATT_CHR_PROP_AUTH_SIGN_WRITE               0x40
#define BLE_GATT_CHR_PROP_EXTENDED                      0x80

struct ble_gatt_error { uint16_t status; uint16_t att_handle; };
struct ble_gatt_svc { uint16_t start_handle; uint16_t end_handle; ble_uuid_any_t uuid; };
struct ble_gatt_attr { uint16_t handle; uint16_t offset; struct os_mbuf *om; };
struct ble_gatt_chr { uint16_t def_handle; uint16_t val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };

typedef int ble_gatt_mtu_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                            uint16_t mtu, void *arg);
typedef int ble_gatt_disc_svc_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                                 const struct ble_gatt_svc *service, void *arg);
typedef int ble_gatt_attr_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg);
typedef int ble_gatt_chr_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg);
typedef int ble_gatt_dsc_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                            uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);

int ble_gattc_exchange_mtu(uint16_t conn_handle, ble_gatt_mtu_fn *cb, void *cb_arg);
int ble_gattc_disc_all_svcs(uint16_t conn_handle, ble_gatt_disc_svc_fn *cb, void *cb_arg);
int ble_gattc_disc_all_chrs(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle,
                            ble_gatt_chr_fn *cb, void *cb_arg);
int ble_gattc_disc_all_dscs(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle,
                            ble_gatt_dsc_fn *cb, void *cb_arg);
int ble_gattc_read(uint16_t conn_handle, uint16_t attr_handle, ble_gatt_attr_fn *cb, void *cb_arg);
int ble_gattc_write_flat(uint16_t conn_handle, uint16_t attr_handle, const void *data,
                         uint16_t data_len, ble_gatt_attr_fn *cb, void *cb_arg);
int ble_gattc_write_no_rsp_flat(uint16_t conn_handle, uint16_t attr_handle,
                                const void *data, uint16_t data_len);

/* ---- host/ble_sm.h ------------------------------------------------------ */
#define BLE_SM_PAIR_KEY_DIST_ENC                0x01
#define BLE_SM_PAIR_KEY_DIST_ID                 0x02
#define BLE_SM_IO_CAP_NO_IO                     0x03

/* ---- host/ble_store.h --------------------------------------------------- */
#define BLE_STORE_OBJ_TYPE_OUR_SEC           1
#define BLE_STORE_OBJ_TYPE_PEER_SEC          2
#define BLE_STORE_OBJ_TYPE_CCCD              3
#define BLE_STORE_OBJ_TYPE_PEER_ADDR         6
#define BLE_STORE_OBJ_TYPE_CSFC              8
#define BLE_STORE_EVENT_OVERFLOW             1
#define BLE_STORE_EVENT_FULL                 2

struct ble_store_value_sec { ble_addr_t peer_addr; };
struct ble_store_value_cccd { ble_addr_t peer_addr; uint16_t chr_val_handle; uint16_t flags; };
struct ble_store_value_csfc { ble_addr_t peer_addr; };
union ble_store_value {
    struct ble_store_value_sec sec;
    struct ble_store_value_cccd cccd;
    struct ble_store_value_csfc csfc;
};

struct ble_store_status_event {
    int event_code;
    union {
        struct { int obj_type; const union ble_store_value *value; } overflow;
        struct { int obj_type; uint16_t conn_handle; } full;
    };
};

int ble_store_util_status_rr(struct ble_store_status_event *event, void *arg);
int ble_store_util_delete_peer(const ble_addr_t *peer_id_addr);
/** The bonded peers, oldest first. */
int ble_store_util_bonded_peers(ble_addr_t *out_peer_id_addrs, int *out_num_peers, int max_peers);

/* ---- host/ble_hs_id.h --------------------------------------------------- */
int ble_hs_id_infer_auto(int privacy, uint8_t *out_addr_type);

/* ---- host/ble_hs.h: configuration ---------------------------------------- */
typedef void ble_hs_reset_fn(int reason);
typedef void ble_hs_sync_fn(void);
typedef int ble_store_status_fn(struct ble_store_status_event *event, void *arg);

struct ble_hs_cfg {
    uint8_t sm_io_cap;
    unsigned sm_bonding:1;
    unsigned sm_mitm:1;
    unsigned sm_sc:1;
    uint8_t sm_our_key_dist;
    uint8_t sm_their_key_dist;
    ble_hs_reset_fn *reset_cb;
    ble_hs_sync_fn *sync_cb;
    ble_store_status_fn *store_status_cb;
    void *store_status_arg;
};
extern struct ble_hs_cfg ble_hs_cfg;

#ifdef __cplusplus
}
#endif
