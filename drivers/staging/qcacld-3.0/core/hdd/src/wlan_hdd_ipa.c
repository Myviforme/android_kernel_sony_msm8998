/*
 * Copyright (c) 2013-2018, 2021 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * DOC: wlan_hdd_ipa.c
 *
 * WLAN HDD and ipa interface implementation
 */

#ifdef IPA_OFFLOAD

/* Include Files */
#ifdef CONFIG_IPA_WDI_UNIFIED_API
#include <linux/ipa_wdi3.h>
#else
#include <linux/ipa.h>
#endif

#include <wlan_hdd_includes.h>
#include <wlan_hdd_ipa.h>

#include <linux/etherdevice.h>
#include <linux/atomic.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/list.h>
#include <linux/debugfs.h>
#include <linux/inetdevice.h>
#include <linux/ip.h>
#include <wlan_hdd_softap_tx_rx.h>
#include <ol_txrx_osif_api.h>
#include <ol_txrx.h>
#include <cdp_txrx_peer_ops.h>

#include "cds_sched.h"

#include "wma.h"
#include "wma_api.h"
#include "wal_rx_desc.h"

#include "cdp_txrx_ipa.h"

#define HDD_IPA_DESC_BUFFER_RATIO          4
#define HDD_IPA_IPV4_NAME_EXT              "_ipv4"
#define HDD_IPA_IPV6_NAME_EXT              "_ipv6"

#define HDD_IPA_RX_INACTIVITY_MSEC_DELAY   1000
#define HDD_IPA_UC_WLAN_8023_HDR_SIZE      14
/* WDI TX and RX PIPE */
#define HDD_IPA_UC_NUM_WDI_PIPE            2
#define HDD_IPA_UC_MAX_PENDING_EVENT       33

#define HDD_IPA_UC_DEBUG_DUMMY_MEM_SIZE    32000
#define HDD_IPA_UC_RT_DEBUG_PERIOD         300
#define HDD_IPA_UC_RT_DEBUG_BUF_COUNT      30
#define HDD_IPA_UC_RT_DEBUG_FILL_INTERVAL  10000

#define HDD_IPA_WLAN_HDR_DES_MAC_OFFSET    0
#define HDD_IPA_MAX_IFACE                  MAX_IPA_IFACE
#define HDD_IPA_MAX_SYSBAM_PIPE            4
#define HDD_IPA_RX_PIPE                    HDD_IPA_MAX_IFACE
#define HDD_IPA_ENABLE_MASK                BIT(0)
#define HDD_IPA_PRE_FILTER_ENABLE_MASK     BIT(1)
#define HDD_IPA_IPV6_ENABLE_MASK           BIT(2)
#define HDD_IPA_RM_ENABLE_MASK             BIT(3)
#define HDD_IPA_CLK_SCALING_ENABLE_MASK    BIT(4)
#define HDD_IPA_UC_ENABLE_MASK             BIT(5)
#define HDD_IPA_UC_STA_ENABLE_MASK         BIT(6)
#define HDD_IPA_REAL_TIME_DEBUGGING        BIT(8)

#define HDD_IPA_MAX_PENDING_EVENT_COUNT    20

#define IPA_WLAN_RX_SOFTIRQ_THRESH 32

#define HDD_IPA_MAX_BANDWIDTH 800

#define HDD_IPA_UC_STAT_LOG_RATE 10

enum hdd_ipa_uc_op_code {
	HDD_IPA_UC_OPCODE_TX_SUSPEND = 0,
	HDD_IPA_UC_OPCODE_TX_RESUME = 1,
	HDD_IPA_UC_OPCODE_RX_SUSPEND = 2,
	HDD_IPA_UC_OPCODE_RX_RESUME = 3,
	HDD_IPA_UC_OPCODE_STATS = 4,
#ifdef FEATURE_METERING
	HDD_IPA_UC_OPCODE_SHARING_STATS = 5,
	HDD_IPA_UC_OPCODE_QUOTA_RSP = 6,
	HDD_IPA_UC_OPCODE_QUOTA_IND = 7,
#endif
	HDD_IPA_UC_OPCODE_UC_READY = 8,
	/* keep this last */
	HDD_IPA_UC_OPCODE_MAX
};

/**
 * enum - Reason codes for stat query
 *
 * @HDD_IPA_UC_STAT_REASON_NONE: Initial value
 * @HDD_IPA_UC_STAT_REASON_DEBUG: For debug/info
 * @HDD_IPA_UC_STAT_REASON_BW_CAL: For bandwidth calibration
 * @HDD_IPA_UC_STAT_REASON_DUMP_INFO: For debug info dump
 */
enum {
	HDD_IPA_UC_STAT_REASON_NONE,
	HDD_IPA_UC_STAT_REASON_DEBUG,
	HDD_IPA_UC_STAT_REASON_BW_CAL
};

/**
 * enum hdd_ipa_rm_state - IPA resource manager state
 * @HDD_IPA_RM_RELEASED:      PROD pipe resource released
 * @HDD_IPA_RM_GRANT_PENDING: PROD pipe resource requested but not granted yet
 * @HDD_IPA_RM_GRANTED:       PROD pipe resource granted
 */
enum hdd_ipa_rm_state {
	HDD_IPA_RM_RELEASED,
	HDD_IPA_RM_GRANT_PENDING,
	HDD_IPA_RM_GRANTED,
};

struct llc_snap_hdr {
	uint8_t dsap;
	uint8_t ssap;
	uint8_t resv[4];
	__be16 eth_type;
} __packed;

/**
 * struct hdd_ipa_tx_hdr - header type which IPA should handle to TX packet
 * @eth:      ether II header
 * @llc_snap: LLC snap header
 *
 */
struct hdd_ipa_tx_hdr {
	struct ethhdr eth;
	struct llc_snap_hdr llc_snap;
} __packed;

/**
 * struct frag_header - fragment header type registered to IPA hardware
 * @length:    fragment length
 * @reserved1: Reserved not used
 * @reserved2: Reserved not used
 *
 */
#ifdef QCA_WIFI_3_0
struct frag_header {
	uint16_t length;
	uint32_t reserved1;
	uint32_t reserved2;
} __packed;
#else
struct frag_header {
	uint32_t
		length:16,
		reserved16:16;
	uint32_t reserved32;
} __packed;

#endif

/**
 * struct ipa_header - ipa header type registered to IPA hardware
 * @vdev_id:  vdev id
 * @reserved: Reserved not used
 *
 */
struct ipa_header {
	uint32_t
		vdev_id:8,	/* vdev_id field is LSB of IPA DESC */
		reserved:24;
} __packed;

/**
 * struct hdd_ipa_uc_tx_hdr - full tx header registered to IPA hardware
 * @frag_hd: fragment header
 * @ipa_hd:  ipa header
 * @eth:     ether II header
 *
 */
struct hdd_ipa_uc_tx_hdr {
	struct frag_header frag_hd;
	struct ipa_header ipa_hd;
	struct ethhdr eth;
} __packed;

/**
 * struct hdd_ipa_cld_hdr - IPA CLD Header
 * @reserved: reserved fields
 * @iface_id: interface ID
 * @sta_id: Station ID
 *
 * Packed 32-bit structure
 * +----------+----------+--------------+--------+
 * | Reserved | QCMAP ID | interface id | STA ID |
 * +----------+----------+--------------+--------+
 */
struct hdd_ipa_cld_hdr {
	uint8_t reserved[2];
	uint8_t iface_id;
	uint8_t sta_id;
} __packed;

struct hdd_ipa_rx_hdr {
	struct hdd_ipa_cld_hdr cld_hdr;
	struct ethhdr eth;
} __packed;

struct hdd_ipa_pm_tx_cb {
	bool exception;
	hdd_adapter_t *adapter;
	struct hdd_ipa_iface_context *iface_context;
	struct ipa_rx_data *ipa_tx_desc;
};

struct hdd_ipa_uc_rx_hdr {
	struct ethhdr eth;
} __packed;

struct hdd_ipa_sys_pipe {
	uint32_t conn_hdl;
	uint8_t conn_hdl_valid;
	struct ipa_sys_connect_params ipa_sys_params;
};

struct hdd_ipa_iface_stats {
	uint64_t num_tx;
	uint64_t num_tx_drop;
	uint64_t num_tx_err;
	uint64_t num_tx_cac_drop;
	uint64_t num_rx_ipa_excep;
};

struct hdd_ipa_priv;

struct hdd_ipa_iface_context {
	struct hdd_ipa_priv *hdd_ipa;
	hdd_adapter_t *adapter;
	void *tl_context;

	enum ipa_client_type cons_client;
	enum ipa_client_type prod_client;

	uint8_t iface_id;       /* This iface ID */
	uint8_t sta_id;         /* This iface station ID */
	qdf_spinlock_t interface_lock;
	uint32_t ifa_address;
	struct hdd_ipa_iface_stats stats;
};

struct hdd_ipa_stats {
	uint32_t event[IPA_WLAN_EVENT_MAX];
	uint64_t num_send_msg;
	uint64_t num_free_msg;

	uint64_t num_rm_grant;
	uint64_t num_rm_release;
	uint64_t num_rm_grant_imm;
	uint64_t num_cons_perf_req;
	uint64_t num_prod_perf_req;

	uint64_t num_rx_drop;

	uint64_t num_tx_desc_q_cnt;
	uint64_t num_tx_desc_error;
	uint64_t num_tx_comp_cnt;
	uint64_t num_tx_queued;
	uint64_t num_tx_dequeued;
	uint64_t num_max_pm_queue;

	uint64_t num_rx_excep;
	uint64_t num_tx_fwd_ok;
	uint64_t num_tx_fwd_err;
};

struct ipa_uc_stas_map {
	bool is_reserved;
	uint8_t sta_id;
};
struct op_msg_type {
	uint8_t msg_t;
	uint8_t rsvd;
	uint16_t op_code;
	uint16_t len;
	uint16_t rsvd_snd;
};

struct ipa_uc_fw_stats {
	uint32_t tx_comp_ring_base;
	uint32_t tx_comp_ring_size;
	uint32_t tx_comp_ring_dbell_addr;
	uint32_t tx_comp_ring_dbell_ind_val;
	uint32_t tx_comp_ring_dbell_cached_val;
	uint32_t tx_pkts_enqueued;
	uint32_t tx_pkts_completed;
	uint32_t tx_is_suspend;
	uint32_t tx_reserved;
	uint32_t rx_ind_ring_base;
	uint32_t rx_ind_ring_size;
	uint32_t rx_ind_ring_dbell_addr;
	uint32_t rx_ind_ring_dbell_ind_val;
	uint32_t rx_ind_ring_dbell_ind_cached_val;
	uint32_t rx_ind_ring_rdidx_addr;
	uint32_t rx_ind_ring_rd_idx_cached_val;
	uint32_t rx_refill_idx;
	uint32_t rx_num_pkts_indicated;
	uint32_t rx_buf_refilled;
	uint32_t rx_num_ind_drop_no_space;
	uint32_t rx_num_ind_drop_no_buf;
	uint32_t rx_is_suspend;
	uint32_t rx_reserved;
};

struct ipa_uc_pending_event {
	qdf_list_node_t node;
	hdd_adapter_t *adapter;
	enum ipa_wlan_event type;
	uint8_t sta_id;
	uint8_t mac_addr[QDF_MAC_ADDR_SIZE];
	bool is_loading;
};

/**
 * struct uc_rm_work_struct
 * @work: uC RM work
 * @event: IPA RM event
 */
struct uc_rm_work_struct {
	struct work_struct work;
	enum ipa_rm_event event;
};

/**
 * struct uc_op_work_struct
 * @work: uC OP work
 * @msg: OP message
 */
struct uc_op_work_struct {
	struct work_struct work;
	struct op_msg_type *msg;
};

/**
 * struct uc_rt_debug_info
 * @time: system time
 * @ipa_excep_count: IPA exception packet count
 * @rx_drop_count: IPA Rx drop packet count
 * @net_sent_count: IPA Rx packet sent to network stack count
 * @rx_discard_count: IPA Rx discard packet count
 * @tx_fwd_ok_count: IPA Tx forward success packet count
 * @tx_fwd_count: IPA Tx forward packet count
 * @rx_destructor_call: IPA Rx packet destructor count
 */
struct uc_rt_debug_info {
	uint64_t time;
	uint64_t ipa_excep_count;
	uint64_t rx_drop_count;
	uint64_t net_sent_count;
	uint64_t rx_discard_count;
	uint64_t tx_fwd_ok_count;
	uint64_t tx_fwd_count;
	uint64_t rx_destructor_call;
};

/**
 * struct hdd_ipa_tx_desc
 * @link: link to list head
 * @priv: pointer to priv list entry
 * @id: Tx desc idex
 * @ipa_tx_desc_ptr: pointer to IPA Tx descriptor
 */
struct hdd_ipa_tx_desc {
	struct list_head link;
	void *priv;
	uint32_t id;
	struct ipa_rx_data *ipa_tx_desc_ptr;
};

#ifdef FEATURE_METERING
struct ipa_uc_sharing_stats {
	uint64_t ipv4_rx_packets;
	uint64_t ipv4_rx_bytes;
	uint64_t ipv6_rx_packets;
	uint64_t ipv6_rx_bytes;
	uint64_t ipv4_tx_packets;
	uint64_t ipv4_tx_bytes;
	uint64_t ipv6_tx_packets;
	uint64_t ipv6_tx_bytes;
};

struct ipa_uc_quota_rsp {
	uint8_t success;
	uint8_t reserved[3];
	uint32_t quota_lo;		/* quota limit low bytes */
	uint32_t quota_hi;		/* quota limit high bytes */
};

struct ipa_uc_quota_ind {
	uint64_t quota_bytes;		/* quota limit in bytes */
};
#endif

struct hdd_ipa_priv {
	struct hdd_ipa_sys_pipe sys_pipe[HDD_IPA_MAX_SYSBAM_PIPE];
	struct hdd_ipa_iface_context iface_context[HDD_IPA_MAX_IFACE];
	uint8_t num_iface;
	enum hdd_ipa_rm_state rm_state;
	/*
	 * IPA driver can send RM notifications with IRQ disabled so using qdf
	 * APIs as it is taken care gracefully. Without this, kernel would throw
	 * an warning if spin_lock_bh is used while IRQ is disabled
	 */
	qdf_spinlock_t rm_lock;
	struct uc_rm_work_struct uc_rm_work;
	struct uc_op_work_struct uc_op_work[HDD_IPA_UC_OPCODE_MAX];
	qdf_wake_lock_t wake_lock;
	struct delayed_work wake_lock_work;
	bool wake_lock_released;

	enum ipa_client_type prod_client;

	atomic_t tx_ref_cnt;
	qdf_nbuf_queue_t pm_queue_head;
	struct work_struct pm_work;
	qdf_spinlock_t pm_lock;
	bool suspended;

	qdf_spinlock_t q_lock;

	struct work_struct mcc_work;
	struct list_head pend_desc_head;
	uint16_t tx_desc_size;
	struct hdd_ipa_tx_desc *tx_desc_list;
	struct list_head free_tx_desc_head;

	hdd_context_t *hdd_ctx;
	struct hdd_ipa_stats stats;

	struct notifier_block ipv4_notifier;
	uint32_t curr_prod_bw;
	uint32_t curr_cons_bw;

	uint8_t activated_fw_pipe;
	uint8_t sap_num_connected_sta;
	uint8_t sta_connected;
	uint32_t tx_pipe_handle;
	uint32_t rx_pipe_handle;
	bool resource_loading;
	bool resource_unloading;
	bool pending_cons_req;
	struct ipa_uc_stas_map assoc_stas_map[WLAN_MAX_STA_COUNT];
	qdf_list_t pending_event;
	qdf_mutex_t event_lock;
	bool ipa_pipes_down;
	uint32_t ipa_tx_packets_diff;
	uint32_t ipa_rx_packets_diff;
	uint32_t ipa_p_tx_packets;
	uint32_t ipa_p_rx_packets;
	uint32_t stat_req_reason;
	uint64_t ipa_tx_forward;
	uint64_t ipa_rx_discard;
	uint64_t ipa_rx_net_send_count;
	uint64_t ipa_rx_internal_drop_count;
	uint64_t ipa_rx_destructor_count;
	qdf_mc_timer_t rt_debug_timer;
	struct uc_rt_debug_info rt_bug_buffer[HDD_IPA_UC_RT_DEBUG_BUF_COUNT];
	unsigned int rt_buf_fill_index;
	struct ipa_wdi_in_params cons_pipe_in;
	struct ipa_wdi_in_params prod_pipe_in;
	bool uc_loaded;
	bool wdi_enabled;
	qdf_mc_timer_t rt_debug_fill_timer;
	qdf_mutex_t rt_debug_lock;
	qdf_mutex_t ipa_lock;
	struct ol_txrx_ipa_resources ipa_resource;
	/* IPA UC doorbell registers paddr */
	qdf_dma_addr_t tx_comp_doorbell_dmaaddr;
	qdf_dma_addr_t rx_ready_doorbell_dmaaddr;
	uint8_t vdev_to_iface[CSR_ROAM_SESSION_MAX];
	bool vdev_offload_enabled[CSR_ROAM_SESSION_MAX];
#ifdef FEATURE_METERING
	struct ipa_uc_sharing_stats ipa_sharing_stats;
	struct ipa_uc_quota_rsp ipa_quota_rsp;
	struct ipa_uc_quota_ind ipa_quota_ind;
	struct completion ipa_uc_sharing_stats_comp;
	struct completion ipa_uc_set_quota_comp;
#endif
	struct completion ipa_resource_comp;

	uint32_t wdi_version;
};

#define HDD_IPA_WLAN_FRAG_HEADER        sizeof(struct frag_header)
#define HDD_IPA_WLAN_IPA_HEADER         sizeof(struct ipa_header)
#define HDD_IPA_WLAN_CLD_HDR_LEN        sizeof(struct hdd_ipa_cld_hdr)
#define HDD_IPA_UC_WLAN_CLD_HDR_LEN     0
#define HDD_IPA_WLAN_TX_HDR_LEN         sizeof(struct hdd_ipa_tx_hdr)
#define HDD_IPA_UC_WLAN_TX_HDR_LEN      sizeof(struct hdd_ipa_uc_tx_hdr)
#define HDD_IPA_WLAN_RX_HDR_LEN         sizeof(struct hdd_ipa_rx_hdr)
#define HDD_IPA_UC_WLAN_RX_HDR_LEN      sizeof(struct hdd_ipa_uc_rx_hdr)
#define HDD_IPA_UC_WLAN_HDR_DES_MAC_OFFSET \
	(HDD_IPA_WLAN_FRAG_HEADER + HDD_IPA_WLAN_IPA_HEADER)

#define HDD_IPA_GET_IFACE_ID(_data) \
	(((struct hdd_ipa_cld_hdr *) (_data))->iface_id)

#define HDD_IPA_LOG(LVL, fmt, args ...) \
	QDF_TRACE(QDF_MODULE_ID_HDD, LVL, \
		  "%s:%d: "fmt, __func__, __LINE__, ## args)

#define HDD_IPA_DP_LOG(LVL, fmt, args...) \
	QDF_TRACE(QDF_MODULE_ID_HDD_DATA, LVL, \
		  "%s:%d: "fmt, __func__, __LINE__, ## args)

#define HDD_IPA_DBG_DUMP(_lvl, _prefix, _buf, _len) \
	do { \
		QDF_TRACE(QDF_MODULE_ID_HDD_DATA, _lvl, "%s:", _prefix); \
		QDF_TRACE_HEX_DUMP(QDF_MODULE_ID_HDD_DATA, _lvl, _buf, _len); \
	} while (0)

#define HDD_IPA_IS_CONFIG_ENABLED(_hdd_ctx, _mask) \
	(((_hdd_ctx)->config->IpaConfig & (_mask)) == (_mask))

#define HDD_BW_GET_DIFF(_x, _y) (unsigned long)((ULONG_MAX - (_y)) + (_x) + 1)

#if defined(QCA_WIFI_3_0) && defined(CONFIG_IPA3)
#define HDD_IPA_WDI2_SET(pipe_in, ipa_ctxt, osdev) \
do { \
	pipe_in.u.ul.rdy_ring_rp_va = \
		ipa_ctxt->ipa_resource.rx_proc_done_idx->vaddr; \
	pipe_in.u.ul.rdy_comp_ring_base_pa = \
		qdf_mem_get_dma_addr(osdev, \
			&ipa_ctxt->ipa_resource.rx2_rdy_ring->mem_info);\
	pipe_in.u.ul.rdy_comp_ring_size = \
		ipa_ctxt->ipa_resource.rx2_rdy_ring->mem_info.size; \
	pipe_in.u.ul.rdy_comp_ring_wp_pa = \
		qdf_mem_get_dma_addr(osdev, \
			&ipa_ctxt->ipa_resource.rx2_proc_done_idx->mem_info); \
	pipe_in.u.ul.rdy_comp_ring_wp_va = \
		ipa_ctxt->ipa_resource.rx2_proc_done_idx->vaddr; \
} while (0)

#define IPA_RESOURCE_READY(ipa_resource, osdev) \
	((0 == qdf_mem_get_dma_addr(osdev, &ipa_resource->ce_sr->mem_info)) || \
	 (0 == qdf_mem_get_dma_addr(osdev, &ipa_resource->tx_comp_ring->mem_info)) || \
	 (0 == qdf_mem_get_dma_addr(osdev, &ipa_resource->rx_rdy_ring->mem_info)) || \
	 (0 == qdf_mem_get_dma_addr(osdev, &ipa_resource->rx2_rdy_ring->mem_info)))

#define HDD_IPA_WDI2_SET_SMMU() \
do { \
	qdf_mem_copy(&pipe_in.u.ul_smmu.rdy_comp_ring, \
		     &ipa_res->rx2_rdy_ring->sgtable, \
		     sizeof(sgtable_t)); \
	pipe_in.u.ul_smmu.rdy_comp_ring_size = \
		ipa_res->rx2_rdy_ring->mem_info.size; \
	pipe_in.u.ul_smmu.rdy_comp_ring_wp_pa = \
		ipa_res->rx2_proc_done_idx->mem_info.pa; \
	pipe_in.u.ul_smmu.rdy_comp_ring_wp_va = \
		ipa_res->rx2_proc_done_idx->vaddr; \
} while (0)
#else
/* Do nothing */
#define HDD_IPA_WDI2_SET(pipe_in, ipa_ctxt, osdev)
#define HDD_IPA_WDI2_SET_SMMU()

#define IPA_RESOURCE_READY(ipa_resource, osdev) \
	((0 == qdf_mem_get_dma_addr(osdev, &ipa_resource->ce_sr->mem_info)) || \
	 (0 == qdf_mem_get_dma_addr(osdev, &ipa_resource->tx_comp_ring->mem_info)) || \
	 (0 == qdf_mem_get_dma_addr(osdev, &ipa_resource->rx_rdy_ring->mem_info)))

#endif /* IPA3 */

#define HDD_IPA_DBG_DUMP_RX_LEN 32
#define HDD_IPA_DBG_DUMP_TX_LEN 48

static struct hdd_ipa_adapter_2_client {
	enum ipa_client_type cons_client;
	enum ipa_client_type prod_client;
} hdd_ipa_adapter_2_client[] = {
	{
		IPA_CLIENT_WLAN2_CONS, IPA_CLIENT_WLAN1_PROD
	}, {
		IPA_CLIENT_WLAN3_CONS, IPA_CLIENT_WLAN1_PROD
	}, {
		IPA_CLIENT_WLAN4_CONS, IPA_CLIENT_WLAN1_PROD
	},
};

/* For Tx pipes, use Ethernet-II Header format */
#ifdef QCA_WIFI_3_0
struct hdd_ipa_uc_tx_hdr ipa_uc_tx_hdr = {
	{
		0x0000,
		0x00000000,
		0x00000000
	},
	{
		0x00000000
	},
	{
		{0x00, 0x03, 0x7f, 0xaa, 0xbb, 0xcc},
		{0x00, 0x03, 0x7f, 0xdd, 0xee, 0xff},
		0x0008
	}
};
#else
struct hdd_ipa_uc_tx_hdr ipa_uc_tx_hdr = {
	{
		0x00000000,
		0x00000000
	},
	{
		0x00000000
	},
	{
		{0x00, 0x03, 0x7f, 0xaa, 0xbb, 0xcc},
		{0x00, 0x03, 0x7f, 0xdd, 0xee, 0xff},
		0x0008
	}
};
#endif

#ifdef FEATURE_METERING
#define IPA_UC_SHARING_STATES_WAIT_TIME	500
#define IPA_UC_SET_QUOTA_WAIT_TIME	500
#endif

#define IPA_RESOURCE_COMP_WAIT_TIME	100

static struct hdd_ipa_priv *ghdd_ipa;

/* Local Function Prototypes */
static void hdd_ipa_i2w_cb(void *priv, enum ipa_dp_evt_type evt,
			   unsigned long data);
static void hdd_ipa_w2i_cb(void *priv, enum ipa_dp_evt_type evt,
			   unsigned long data);
static void hdd_ipa_msg_free_fn(void *buff, uint32_t len, uint32_t type);

static void hdd_ipa_cleanup_iface(struct hdd_ipa_iface_context *iface_context);
static void hdd_ipa_uc_proc_pending_event(struct hdd_ipa_priv *hdd_ipa,
					  bool is_loading);
static int hdd_ipa_uc_enable_pipes(struct hdd_ipa_priv *hdd_ipa);
static int hdd_ipa_wdi_init(struct hdd_ipa_priv *hdd_ipa);
static void hdd_ipa_send_pkt_to_tl(struct hdd_ipa_iface_context *iface_context,
		struct ipa_rx_data *ipa_tx_desc);
static int hdd_ipa_setup_sys_pipe(struct hdd_ipa_priv *hdd_ipa);

/**
 * hdd_ipa_uc_get_db_paddr() - Get Doorbell physical address
 * @db_paddr: Doorbell physical address given by IPA
 * @client: IPA client type
 *
 * Query doorbell physical address from IPA
 * IPA will give physical address for TX COMP and RX READY
 *
 * Return: None
 */
static void hdd_ipa_uc_get_db_paddr(qdf_dma_addr_t *db_paddr,
		enum ipa_client_type client)
{
	struct ipa_wdi_db_params dbpa;

	dbpa.client = client;
	ipa_uc_wdi_get_dbpa(&dbpa);
	*db_paddr = dbpa.uc_door_bell_pa;
}

/**
 * hdd_ipa_uc_loaded_uc_cb() - IPA UC loaded event callback
 * @priv_ctxt: hdd ipa local context
 *
 * Will be called by IPA context.
 * It's atomic context, then should be scheduled to kworker thread
 *
 * Return: None
 */
static void hdd_ipa_uc_loaded_uc_cb(void *priv_ctxt)
{
	struct hdd_ipa_priv *hdd_ipa;
	struct op_msg_type *msg;
	struct uc_op_work_struct *uc_op_work;

	if (priv_ctxt == NULL) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "Invalid IPA context");
		return;
	}

	hdd_ipa = (struct hdd_ipa_priv *)priv_ctxt;

	uc_op_work = &hdd_ipa->uc_op_work[HDD_IPA_UC_OPCODE_UC_READY];

	if (!list_empty(&uc_op_work->work.entry)) {
		/* uc_op_work is not initialized yet */
		hdd_ipa->uc_loaded = true;
		return;
	}

	msg = (struct op_msg_type *)qdf_mem_malloc(sizeof(*msg));
	if (!msg) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "op_msg allocation fails");
		return;
	}

	msg->op_code = HDD_IPA_UC_OPCODE_UC_READY;

	/* When the same uC OPCODE is already pended, just return */
	if (uc_op_work->msg)
		goto done;

	uc_op_work->msg = msg;
	schedule_work(&uc_op_work->work);

	/* work handler will free the msg buffer */
	return;

done:
	qdf_mem_free(msg);
}

/**
 * hdd_ipa_uc_send_wdi_control_msg() - Set WDI control message
 * @ctrl: WDI control value
 *
 * Send WLAN_WDI_ENABLE for ctrl = true and WLAN_WDI_DISABLE otherwise.
 *
 * Return: 0 on message send to ipa, -1 on failure
 */
static int hdd_ipa_uc_send_wdi_control_msg(bool ctrl)
{
	struct ipa_msg_meta meta;
	struct ipa_wlan_msg *ipa_msg;
	int ret = 0;

	/* WDI enable message to IPA */
	meta.msg_len = sizeof(*ipa_msg);
	ipa_msg = qdf_mem_malloc(meta.msg_len);
	if (ipa_msg == NULL) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"msg allocation failed");
		return -ENOMEM;
	}

	if (ctrl == true)
		meta.msg_type = WLAN_WDI_ENABLE;
	else
		meta.msg_type = WLAN_WDI_DISABLE;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
		    "ipa_send_msg(Evt:%d)", meta.msg_type);
	ret = ipa_send_msg(&meta, ipa_msg, hdd_ipa_msg_free_fn);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"ipa_send_msg(Evt:%d)-fail=%d",
			meta.msg_type,  ret);
		qdf_mem_free(ipa_msg);
	}
	return ret;
}

/**
 * hdd_ipa_is_enabled() - Is IPA enabled?
 * @hdd_ctx: Global HDD context
 *
 * Return: true if IPA is enabled, false otherwise
 */
bool hdd_ipa_is_enabled(hdd_context_t *hdd_ctx)
{
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx, HDD_IPA_ENABLE_MASK);
}

/**
 * hdd_ipa_uc_is_enabled() - Is IPA uC offload enabled?
 * @hdd_ctx: Global HDD context
 *
 * Return: true if IPA uC offload is enabled, false otherwise
 */
bool hdd_ipa_uc_is_enabled(hdd_context_t *hdd_ctx)
{
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx, HDD_IPA_UC_ENABLE_MASK);
}

/**
 * hdd_ipa_uc_sta_is_enabled() - Is STA mode IPA uC offload enabled?
 * @hdd_ctx: Global HDD context
 *
 * Return: true if STA mode IPA uC offload is enabled, false otherwise
 */
static inline bool hdd_ipa_uc_sta_is_enabled(hdd_context_t *hdd_ctx)
{
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx, HDD_IPA_UC_STA_ENABLE_MASK);
}


/**
 * hdd_ipa_uc_sta_only_offload_is_enabled()
 *
 * STA only IPA offload is needed on MDM platforms to support
 * tethering scenarios in STA-SAP configurations when SAP is idle.
 *
 * Currently in STA-SAP configurations, IPA pipes are enabled only
 * when a wifi client is connected to SAP.
 *
 * Impact of this API is only limited to when IPA pipes are enabled
 * and disabled. To take effect, HDD_IPA_UC_STA_ENABLE_MASK needs to
 * set to 1.
 *
 * Return: true if MDM_PLATFORM is defined, false otherwise
 */
#ifdef MDM_PLATFORM
static inline bool hdd_ipa_uc_sta_only_offload_is_enabled(void)
{
	return true;
}
#else
static inline bool hdd_ipa_uc_sta_only_offload_is_enabled(void)
{
	return false;
}
#endif

/**
 * hdd_ipa_uc_sta_reset_sta_connected() - Reset sta_connected flag
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: None
 */
static inline void hdd_ipa_uc_sta_reset_sta_connected(
		struct hdd_ipa_priv *hdd_ipa)
{
	qdf_mutex_acquire(&hdd_ipa->ipa_lock);
	hdd_ipa->sta_connected = 0;
	qdf_mutex_release(&hdd_ipa->ipa_lock);
}

/**
 * hdd_ipa_is_pre_filter_enabled() - Is IPA pre-filter enabled?
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: true if pre-filter is enabled, otherwise false
 */
static inline bool hdd_ipa_is_pre_filter_enabled(hdd_context_t *hdd_ctx)
{
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx,
					 HDD_IPA_PRE_FILTER_ENABLE_MASK);
}

/**
 * hdd_ipa_is_ipv6_enabled() - Is IPA IPv6 enabled?
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: true if IPv6 is enabled, otherwise false
 */
static inline bool hdd_ipa_is_ipv6_enabled(hdd_context_t *hdd_ctx)
{
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx, HDD_IPA_IPV6_ENABLE_MASK);
}

/**
 * hdd_ipa_is_rt_debugging_enabled() - Is IPA real-time debug enabled?
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: true if resource manager is enabled, otherwise false
 */
static inline bool hdd_ipa_is_rt_debugging_enabled(hdd_context_t *hdd_ctx)
{
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx, HDD_IPA_REAL_TIME_DEBUGGING);
}

/**
 * hdd_ipa_is_fw_wdi_actived() - Are FW WDI pipes activated?
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: true if FW WDI pipes activated, otherwise false
 */
bool hdd_ipa_is_fw_wdi_actived(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa = hdd_ctx->hdd_ipa;

	if (!hdd_ipa)
		return false;

	return (HDD_IPA_UC_NUM_WDI_PIPE == hdd_ipa->activated_fw_pipe);
}

#ifdef FEATURE_METERING
/**
 * __hdd_ipa_wdi_meter_notifier_cb() - WLAN to IPA callback handler.
 * IPA calls to get WLAN stats or set quota limit.
 * @priv: pointer to private data registered with IPA (we register a
 *»       pointer to the global IPA context)
 * @evt: the IPA event which triggered the callback
 * @data: data associated with the event
 *
 * Return: None
 */
static void __hdd_ipa_wdi_meter_notifier_cb(enum ipa_wdi_meter_evt_type evt,
					  void *data)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	hdd_adapter_t *adapter = NULL;
	struct ipa_get_wdi_sap_stats *wdi_sap_stats;
	struct ipa_set_wifi_quota *ipa_set_quota;
	int ret = 0;

	if (wlan_hdd_validate_context(hdd_ipa->hdd_ctx))
		return;

	adapter = hdd_get_adapter(hdd_ipa->hdd_ctx, QDF_STA_MODE);

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "event=%d", evt);

	switch (evt) {
	case IPA_GET_WDI_SAP_STATS:
		/* fill-up ipa_get_wdi_sap_stats structure after getting
		 * ipa_uc_fw_stats from FW
		 */
		wdi_sap_stats = data;

		if (hdd_validate_adapter(adapter)) {
				hdd_err("IPA uC share stats failed - invalid adapter");
			wdi_sap_stats->stats_valid = 0;
			return;
		}

		INIT_COMPLETION(hdd_ipa->ipa_uc_sharing_stats_comp);
		hdd_ipa_uc_sharing_stats_request(adapter,
					     wdi_sap_stats->reset_stats);
		ret = wait_for_completion_timeout(
			&hdd_ipa->ipa_uc_sharing_stats_comp,
			msecs_to_jiffies(IPA_UC_SHARING_STATES_WAIT_TIME));
		if (!ret) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
					"IPA uC share stats request timed out");
			wdi_sap_stats->stats_valid = 0;
		} else {
			wdi_sap_stats->stats_valid = 1;

			wdi_sap_stats->ipv4_rx_packets =
				hdd_ipa->ipa_sharing_stats.ipv4_rx_packets;
			wdi_sap_stats->ipv4_rx_bytes =
				hdd_ipa->ipa_sharing_stats.ipv4_rx_bytes;
			wdi_sap_stats->ipv6_rx_packets =
				hdd_ipa->ipa_sharing_stats.ipv6_rx_packets;
			wdi_sap_stats->ipv6_rx_bytes =
				hdd_ipa->ipa_sharing_stats.ipv6_rx_bytes;
			wdi_sap_stats->ipv4_tx_packets =
				hdd_ipa->ipa_sharing_stats.ipv4_tx_packets;
			wdi_sap_stats->ipv4_tx_bytes =
				hdd_ipa->ipa_sharing_stats.ipv4_tx_bytes;
			wdi_sap_stats->ipv6_tx_packets =
				hdd_ipa->ipa_sharing_stats.ipv6_tx_packets;
			wdi_sap_stats->ipv6_tx_bytes =
				hdd_ipa->ipa_sharing_stats.ipv6_tx_bytes;
			HDD_IPA_DP_LOG(QDF_TRACE_LEVEL_DEBUG,
				"%s:%d,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
				"IPA_GET_WDI_SAP_STATS",
				wdi_sap_stats->stats_valid,
				wdi_sap_stats->ipv4_rx_packets,
				wdi_sap_stats->ipv4_rx_bytes,
				wdi_sap_stats->ipv6_rx_packets,
				wdi_sap_stats->ipv6_rx_bytes,
				wdi_sap_stats->ipv4_tx_packets,
				wdi_sap_stats->ipv4_tx_bytes,
				wdi_sap_stats->ipv6_tx_packets,
				wdi_sap_stats->ipv6_tx_bytes);
		}
		break;
	case IPA_SET_WIFI_QUOTA:
		/* Get ipa_set_wifi_quota structure from IPA and pass to FW
		 * through quota_exceeded field in ipa_uc_fw_stats
		 */
		ipa_set_quota = data;

		if (hdd_validate_adapter(adapter)) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "IPA uC set quota failed - invalid adapter");
			ipa_set_quota->set_valid = 0;
			return;
		}

		INIT_COMPLETION(hdd_ipa->ipa_uc_set_quota_comp);
		hdd_ipa_uc_set_quota(adapter, ipa_set_quota->set_quota,
				     ipa_set_quota->quota_bytes);

		ret = wait_for_completion_timeout(
				&hdd_ipa->ipa_uc_set_quota_comp,
				msecs_to_jiffies(IPA_UC_SET_QUOTA_WAIT_TIME));
		if (!ret) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
					"IPA uC set quota request timed out");
			ipa_set_quota->set_valid = 0;
		} else {
			ipa_set_quota->quota_bytes =
				((uint64_t)(hdd_ipa->ipa_quota_rsp.quota_hi)
				  <<32)|hdd_ipa->ipa_quota_rsp.quota_lo;
			ipa_set_quota->set_valid =
				hdd_ipa->ipa_quota_rsp.success;
		}

		HDD_IPA_DP_LOG(QDF_TRACE_LEVEL_DEBUG, "SET_QUOTA: %llu, %d",
			       ipa_set_quota->quota_bytes,
			       ipa_set_quota->set_valid);
		break;
	}
}

/**
 * hdd_ipa_wdi_meter_notifier_cb() - WLAN to IPA callback handler.
 * IPA calls to get WLAN stats or set quota limit.
 * @priv: pointer to private data registered with IPA (we register a
 *»       pointer to the global IPA context)
 * @evt: the IPA event which triggered the callback
 * @data: data associated with the event
 *
 * Return: None
 */
static void hdd_ipa_wdi_meter_notifier_cb(enum ipa_wdi_meter_evt_type evt,
					  void *data)
{
	cds_ssr_protect(__func__);
	__hdd_ipa_wdi_meter_notifier_cb(evt, data);
	cds_ssr_unprotect(__func__);
}

#else /* FEATURE_METERING */
static void hdd_ipa_wdi_init_metering(struct hdd_ipa_priv *ipa_ctxt, void *in)
{
}
#endif /* FEATURE_METERING */

#ifdef CONFIG_IPA_WDI_UNIFIED_API

/**
 * hdd_ipa_is_rm_enabled() - Is IPA resource manager enabled?
 * @hdd_ipa: Global HDD IPA context
 *
 * IPA RM is deprecated and IPA PM is involved. WLAN driver
 * has no control over IPA PM and thus we could regard IPA
 * RM as always enabled for power efficiency.
 *
 * Return: true
 */
static inline bool hdd_ipa_is_rm_enabled(hdd_context_t *hdd_ctx)
{
	return true;
}

/**
 * hdd_ipa_is_clk_scaling_enabled() - Is IPA clock scaling enabled?
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: true if clock scaling is enabled, otherwise false
 */
static inline bool hdd_ipa_is_clk_scaling_enabled(hdd_context_t *hdd_ctx)
{
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx,
					 HDD_IPA_CLK_SCALING_ENABLE_MASK);
}

/*
 * TODO: Get WDI version through FW capabilities
 */
#ifdef QCA_WIFI_3_0
static inline void hdd_ipa_wdi_get_wdi_version(struct hdd_ipa_priv *hdd_ipa)
{
	hdd_ipa->wdi_version = IPA_WDI_2;
}
#else
static inline void hdd_ipa_wdi_get_wdi_version(struct hdd_ipa_priv *hdd_ipa)
{
	hdd_ipa->wdi_version = IPA_WDI_1;
}
#endif

#ifdef QCA_LL_TX_FLOW_CONTROL_V2
static bool hdd_ipa_wdi_is_mcc_mode_enabled(struct hdd_ipa_priv *hdd_ipa)
{
	return false;
}
#else
static bool hdd_ipa_wdi_is_mcc_mode_enabled(struct hdd_ipa_priv *hdd_ipa)
{
	return hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx);
}
#endif

#ifdef FEATURE_METERING
static void hdd_ipa_wdi_init_metering(struct hdd_ipa_priv *ipa_ctxt, void *in)
{
	struct ipa_wdi_init_in_params *wdi3_in;

	wdi3_in = (struct ipa_wdi_init_in_params *)in;
	wdi3_in->wdi_notify = hdd_ipa_wdi_meter_notifier_cb;

	init_completion(&ipa_ctxt->ipa_uc_sharing_stats_comp);
	init_completion(&ipa_ctxt->ipa_uc_set_quota_comp);
}
#endif

static int hdd_ipa_wdi_init(struct hdd_ipa_priv *hdd_ipa)
{
	struct ipa_wdi_init_in_params in;
	struct ipa_wdi_init_out_params out;
	int ret;

	hdd_ipa->uc_loaded = false;

	in.wdi_version = hdd_ipa->wdi_version;
	in.notify = hdd_ipa_uc_loaded_uc_cb;
	in.priv = (void *)hdd_ipa;
	hdd_ipa_wdi_init_metering(hdd_ipa, (void *)&in);

	ret = ipa_wdi_init(&in, &out);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"ipa_wdi_init failed with ret=%d", ret);
		return -EPERM;
	}

	if (out.is_uC_ready) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "IPA uC READY");
		hdd_ipa->uc_loaded = true;
	} else {
		ret = -EACCES;
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"IPA uC NOT READY ret=%d", ret);
	}

	return ret;
}

static int hdd_ipa_wdi_cleanup(void)
{
	int ret;

	ret = ipa_wdi_cleanup();
	if (ret)
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"ipa_wdi_cleanup failed ret=%d", ret);
	return ret;
}

static int hdd_ipa_wdi_conn_pipes(struct hdd_ipa_priv *hdd_ipa,
		struct ol_txrx_ipa_resources *ipa_res)
{
	hdd_context_t *hdd_ctx = (hdd_context_t *)hdd_ipa->hdd_ctx;
	qdf_device_t osdev = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);
	struct ipa_wdi_conn_in_params *in;
	struct ipa_wdi_conn_out_params out;
	struct ipa_wdi_pipe_setup_info *info;
	struct ipa_wdi_pipe_setup_info_smmu *info_smmu;
	struct ipa_ep_cfg *tx_cfg;
	struct ipa_ep_cfg *rx_cfg;
	int ret;
	int i;

	if (qdf_unlikely(NULL == osdev)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "osdev is NULL");
		return QDF_STATUS_E_FAILURE;
	}

	in = qdf_mem_malloc(sizeof(*in));
	if (!in) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"failed to alloc ipa_wdi_conn_in_params");
		return -ENOMEM;
	}

	/* IPA RX exception packets callback */
	in->notify = hdd_ipa_w2i_cb;
	in->priv = hdd_ctx->hdd_ipa;

	if (hdd_ipa_wdi_is_mcc_mode_enabled(hdd_ipa)) {
		in->num_sys_pipe_needed = HDD_IPA_MAX_IFACE;
		for (i = 0; i < in->num_sys_pipe_needed; i++)
			memcpy(&in->sys_in[i],
					&hdd_ipa->sys_pipe[i].ipa_sys_params,
					sizeof(struct ipa_sys_connect_params));
	} else {
		in->num_sys_pipe_needed = 0;
	}

	if (qdf_mem_smmu_s1_enabled(osdev))
		in->is_smmu_enabled = true;
	else
		in->is_smmu_enabled = false;

	if (in->is_smmu_enabled) {
		tx_cfg = &in->u_tx.tx_smmu.ipa_ep_cfg;
		rx_cfg = &in->u_rx.rx_smmu.ipa_ep_cfg;
	} else {
		tx_cfg = &in->u_tx.tx.ipa_ep_cfg;
		rx_cfg = &in->u_rx.rx.ipa_ep_cfg;
	}

	tx_cfg->nat.nat_en = IPA_BYPASS_NAT;
	tx_cfg->hdr.hdr_len = HDD_IPA_UC_WLAN_TX_HDR_LEN;
	tx_cfg->hdr.hdr_ofst_pkt_size_valid = 1;
	tx_cfg->hdr.hdr_ofst_pkt_size = 0;
	tx_cfg->hdr.hdr_additional_const_len =
		HDD_IPA_UC_WLAN_8023_HDR_SIZE;
	tx_cfg->hdr_ext.hdr_little_endian = true;
	tx_cfg->mode.mode = IPA_BASIC;

	rx_cfg->nat.nat_en = IPA_BYPASS_NAT;
	rx_cfg->hdr.hdr_len = HDD_IPA_UC_WLAN_RX_HDR_LEN;
	rx_cfg->hdr.hdr_ofst_pkt_size_valid = 1;
	rx_cfg->hdr.hdr_ofst_pkt_size = 0;
	rx_cfg->hdr.hdr_additional_const_len =
		HDD_IPA_UC_WLAN_8023_HDR_SIZE;
	rx_cfg->hdr_ext.hdr_little_endian = true;
	rx_cfg->hdr.hdr_ofst_metadata_valid = 0;
	rx_cfg->hdr.hdr_metadata_reg_valid = 1;
	rx_cfg->mode.mode = IPA_BASIC;

	if (in->is_smmu_enabled) {
		/* TX */
		info_smmu = &in->u_tx.tx_smmu;
		info_smmu->client = IPA_CLIENT_WLAN1_CONS;

		qdf_mem_copy(&info_smmu->transfer_ring_base,
				&ipa_res->tx_comp_ring->sgtable,
				sizeof(sgtable_t));
		info_smmu->transfer_ring_size =
			ipa_res->tx_comp_ring->mem_info.size;

		qdf_mem_copy(&info_smmu->event_ring_base,
				&ipa_res->ce_sr->sgtable, sizeof(sgtable_t));
		info_smmu->event_ring_size = ipa_res->ce_sr_ring_size;
		info_smmu->event_ring_doorbell_pa = ipa_res->ce_reg_paddr;
		info_smmu->num_pkt_buffers = ipa_res->tx_num_alloc_buffer;

		/* RX */
		info_smmu = &in->u_rx.rx_smmu;
		info_smmu->client = IPA_CLIENT_WLAN1_PROD;

		qdf_mem_copy(&info_smmu->transfer_ring_base,
				&ipa_res->rx_rdy_ring->sgtable,
				sizeof(sgtable_t));
		info_smmu->transfer_ring_size =
			ipa_res->rx_rdy_ring->mem_info.size;
		info_smmu->transfer_ring_doorbell_pa =
			ipa_res->rx_proc_done_idx->mem_info.pa;

		if (hdd_ipa->wdi_version == IPA_WDI_2) {
			qdf_mem_copy(&info_smmu->event_ring_base,
				     &ipa_res->rx2_rdy_ring->sgtable,
				     sizeof(sgtable_t));
			info_smmu->event_ring_size =
				ipa_res->rx2_rdy_ring->mem_info.size;
			info_smmu->event_ring_doorbell_pa =
				ipa_res->rx2_proc_done_idx->mem_info.pa;
		}
	} else {
		/* TX */
		info = &in->u_tx.tx;

		info->client = IPA_CLIENT_WLAN1_CONS;

		info->transfer_ring_base_pa = qdf_mem_get_dma_addr(osdev,
				&ipa_res->tx_comp_ring->mem_info);
		info->transfer_ring_size =
			ipa_res->tx_comp_ring->mem_info.size;

		info->event_ring_base_pa = qdf_mem_get_dma_addr(osdev,
				&ipa_res->ce_sr->mem_info);
		info->event_ring_size = ipa_res->ce_sr_ring_size;
		info->event_ring_doorbell_pa = ipa_res->ce_reg_paddr;
		info->num_pkt_buffers = ipa_res->tx_num_alloc_buffer;

		/* RX */
		info = &in->u_rx.rx;

		info->client = IPA_CLIENT_WLAN1_PROD;

		info->transfer_ring_base_pa =
			ipa_res->rx_rdy_ring->mem_info.pa;
		info->transfer_ring_size =
			ipa_res->rx_rdy_ring->mem_info.size;
		info->transfer_ring_doorbell_pa =
			ipa_res->rx_proc_done_idx->mem_info.pa;

		if (hdd_ipa->wdi_version == IPA_WDI_2) {
			info->event_ring_base_pa = qdf_mem_get_dma_addr(osdev,
					&ipa_res->rx2_rdy_ring->mem_info);
			info->event_ring_size =
				ipa_res->rx2_rdy_ring->mem_info.size;
			info->event_ring_doorbell_pa =
				qdf_mem_get_dma_addr(osdev,
					&ipa_res->rx2_proc_done_idx->mem_info);
		}
	}

	ret = ipa_wdi_conn_pipes(in, &out);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"ipa_wdi_conn_pipes failed ret=%d", ret);
		qdf_mem_free(in);
		return ret;
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
			"out.tx_uc_db_pa 0x%x out.rx_uc_db_pa 0x%x",
			out.tx_uc_db_pa, out.rx_uc_db_pa);

	hdd_ipa->tx_comp_doorbell_dmaaddr = out.tx_uc_db_pa;
	hdd_ipa->rx_ready_doorbell_dmaaddr = out.rx_uc_db_pa;

	qdf_mem_free(in);

	return 0;
}

static int hdd_ipa_wdi_disconn_pipes(struct hdd_ipa_priv *hdd_ipa)
{
	int ret;

	ret = ipa_wdi_disconn_pipes();
	if (ret)
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"ipa_wdi_disconn_pipes failed ret=%d", ret);
	return ret;
}

static int hdd_ipa_wdi_reg_intf(struct hdd_ipa_priv *hdd_ipa,
		struct hdd_ipa_iface_context *iface_context)
{
	hdd_adapter_t *adapter = iface_context->adapter;
	struct ipa_wdi_reg_intf_in_params in;
	struct hdd_ipa_uc_tx_hdr uc_tx_hdr;
	struct hdd_ipa_uc_tx_hdr uc_tx_hdr_v6;
	int ret;

	memcpy(&uc_tx_hdr, &ipa_uc_tx_hdr, HDD_IPA_UC_WLAN_TX_HDR_LEN);
	memcpy(&uc_tx_hdr.eth.h_source, adapter->dev->dev_addr, ETH_ALEN);
	uc_tx_hdr.ipa_hd.vdev_id = iface_context->adapter->sessionId;

	in.netdev_name = adapter->dev->name;
	in.hdr_info[IPA_IP_v4].hdr = (u8 *)&uc_tx_hdr;
	in.hdr_info[IPA_IP_v4].hdr_len = HDD_IPA_UC_WLAN_TX_HDR_LEN;
	in.hdr_info[IPA_IP_v4].dst_mac_addr_offset =
		HDD_IPA_UC_WLAN_HDR_DES_MAC_OFFSET;
	in.hdr_info[IPA_IP_v4].hdr_type = IPA_HDR_L2_ETHERNET_II;

	if (hdd_ipa_is_ipv6_enabled(hdd_ipa->hdd_ctx)) {
		memcpy(&uc_tx_hdr_v6, &ipa_uc_tx_hdr,
				HDD_IPA_UC_WLAN_TX_HDR_LEN);
		memcpy(&uc_tx_hdr_v6.eth.h_source, adapter->dev->dev_addr,
				ETH_ALEN);
		uc_tx_hdr_v6.ipa_hd.vdev_id = iface_context->adapter->sessionId;
		uc_tx_hdr_v6.eth.h_proto = cpu_to_be16(ETH_P_IPV6);

		in.netdev_name = adapter->dev->name;
		in.hdr_info[IPA_IP_v6].hdr = (u8 *)&uc_tx_hdr_v6;
		in.hdr_info[IPA_IP_v6].hdr_len = HDD_IPA_UC_WLAN_TX_HDR_LEN;
		in.hdr_info[IPA_IP_v6].dst_mac_addr_offset =
			HDD_IPA_UC_WLAN_HDR_DES_MAC_OFFSET;
		in.hdr_info[IPA_IP_v6].hdr_type = IPA_HDR_L2_ETHERNET_II;
	}

	in.alt_dst_pipe = iface_context->cons_client;
	in.is_meta_data_valid = 1;
	in.meta_data = htonl(iface_context->adapter->sessionId << 16);
	in.meta_data_mask = htonl(0x00FF0000);

	ret = ipa_wdi_reg_intf(&in);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"ipa_wdi_reg_intf failed ret=%d", ret);
		return ret;
	}

	return 0;
}

static int hdd_ipa_wdi_dereg_intf(struct hdd_ipa_priv *hdd_ipa,
		const char *devname)
{
	int ret;

	ret = ipa_wdi_dereg_intf(devname);
	if (ret)
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"ipa_wdi_dereg_intf failed ret=%d", ret);
	return ret;
}

static int hdd_ipa_wdi_enable_pipes(struct hdd_ipa_priv *hdd_ipa)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	int ret;

	if (!pdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "pdev is NULL");
		ret = QDF_STATUS_E_FAILURE;
		return ret;
	}

	/* Map IPA SMMU for all Rx hash table */
	ret = ol_txrx_rx_hash_smmu_map(pdev, true);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "IPA SMMU map failed ret=%d", ret);
		return ret;
	}

	ret = ipa_wdi_enable_pipes();
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "ipa_wdi_enable_pipes failed ret=%d", ret);

		if (ol_txrx_rx_hash_smmu_map(pdev, false)) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "IPA SMMU unmap failed");
		}
		return ret;
	}

	return 0;
}

static int hdd_ipa_wdi_disable_pipes(struct hdd_ipa_priv *hdd_ipa)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	int ret;

	if (!pdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "pdev is NULL");
		ret = QDF_STATUS_E_FAILURE;
		return ret;
	}

	ret = ipa_wdi_disable_pipes();
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "ipa_wdi_disable_pipes failed ret=%d", ret);
		return ret;
	}

	/* Unmap IPA SMMU for all Rx hash table */
	ret = ol_txrx_rx_hash_smmu_map(pdev, false);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "IPA SMMU unmap failed");
		return ret;
	}

	return 0;
}

static inline int hdd_ipa_wdi_setup_sys_pipe(struct hdd_ipa_priv *hdd_ipa,
		struct ipa_sys_connect_params *sys, uint32_t *handle)
{
	return 0;
}

static inline int hdd_ipa_wdi_teardown_sys_pipe(struct hdd_ipa_priv *hdd_ipa,
		uint32_t handle)
{
	return 0;
}

static int hdd_ipa_wdi_rm_set_perf_profile(struct hdd_ipa_priv *hdd_ipa,
		int client, uint32_t max_supported_bw_mbps)
{
	struct ipa_wdi_perf_profile profile;

	profile.client = client;
	profile.max_supported_bw_mbps = max_supported_bw_mbps;

	return ipa_wdi_set_perf_profile(&profile);
}

static inline int hdd_ipa_wdi_rm_request_resource(struct hdd_ipa_priv *hdd_ipa,
		enum ipa_rm_resource_name res_name)
{
	return 0;
}

static inline int hdd_ipa_wdi_rm_release_resource(struct hdd_ipa_priv *hdd_ipa,
		enum ipa_rm_resource_name res_name)
{
	return 0;
}

static inline int hdd_ipa_wdi_setup_rm(struct hdd_ipa_priv *hdd_ipa)
{
	return 0;
}

static inline int hdd_ipa_wdi_destroy_rm(struct hdd_ipa_priv *hdd_ipa)
{
	return 0;
}

static inline int hdd_ipa_wdi_rm_request(struct hdd_ipa_priv *hdd_ipa)
{
	return 0;
}

static inline int hdd_ipa_wdi_rm_try_release(struct hdd_ipa_priv *hdd_ipa)
{
	return 0;
}

static inline int hdd_ipa_wdi_rm_notify_completion(enum ipa_rm_event event,
		enum ipa_rm_resource_name resource_name)
{
	return 0;
}

static inline bool hdd_ipa_is_rm_released(struct hdd_ipa_priv *hdd_ipa)
{
	return true;
}

/**
 * hdd_ipa_pm_flush() - flush queued packets
 * @work: pointer to the scheduled work
 *
 * Called during PM resume to send packets to TL which were queued
 * while host was in the process of suspending.
 *
 * Return: None
 */
static void hdd_ipa_pm_flush(struct work_struct *work)
{
	struct hdd_ipa_priv *hdd_ipa = container_of(work,
						    struct hdd_ipa_priv,
						    pm_work);
	struct hdd_ipa_pm_tx_cb *pm_tx_cb = NULL;
	qdf_nbuf_t skb;
	uint32_t dequeued = 0;

	qdf_spin_lock_bh(&hdd_ipa->pm_lock);
	while (((skb = qdf_nbuf_queue_remove(&hdd_ipa->pm_queue_head))
								!= NULL)) {
		qdf_spin_unlock_bh(&hdd_ipa->pm_lock);

		pm_tx_cb = (struct hdd_ipa_pm_tx_cb *)skb->cb;
		dequeued++;
		if (pm_tx_cb->exception) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
				"Flush Exception");
			if (pm_tx_cb->adapter->dev)
				hdd_softap_hard_start_xmit(skb,
					  pm_tx_cb->adapter->dev);
			else
				dev_kfree_skb_any(skb);
		} else {
			hdd_ipa_send_pkt_to_tl(pm_tx_cb->iface_context,
				       pm_tx_cb->ipa_tx_desc);
		}
		qdf_spin_lock_bh(&hdd_ipa->pm_lock);
	}
	qdf_spin_unlock_bh(&hdd_ipa->pm_lock);

	hdd_ipa->stats.num_tx_dequeued += dequeued;
	if (dequeued > hdd_ipa->stats.num_max_pm_queue)
		hdd_ipa->stats.num_max_pm_queue = dequeued;
}

int hdd_ipa_uc_smmu_map(bool map, uint32_t num_buf, qdf_mem_info_t *buf_arr)
{
	if (!num_buf) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "No buffers to map/unmap");
		return 0;
	}

	if (map)
		return ipa_wdi_create_smmu_mapping(num_buf,
			   (struct ipa_wdi_buffer_info *)buf_arr);
	else
		return ipa_wdi_release_smmu_mapping(num_buf,
			   (struct ipa_wdi_buffer_info *)buf_arr);
}
#else /* CONFIG_IPA_WDI_UNIFIED_API */

/**
 * hdd_ipa_is_rm_enabled() - Is IPA resource manager enabled?
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: true if resource manager is enabled, otherwise false
 */
static inline bool hdd_ipa_is_rm_enabled(hdd_context_t *hdd_ctx)
{
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx, HDD_IPA_RM_ENABLE_MASK);
}

/**
 * hdd_ipa_is_clk_scaling_enabled() - Is IPA clock scaling enabled?
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: true if clock scaling is enabled, otherwise false
 */
static inline bool hdd_ipa_is_clk_scaling_enabled(hdd_context_t *hdd_ctx)
{
	return HDD_IPA_IS_CONFIG_ENABLED(hdd_ctx,
					 HDD_IPA_CLK_SCALING_ENABLE_MASK |
					 HDD_IPA_RM_ENABLE_MASK);
}

static inline void hdd_ipa_wdi_get_wdi_version(struct hdd_ipa_priv *hdd_ipa)
{
}

#ifdef FEATURE_METERING
static void hdd_ipa_wdi_init_metering(struct hdd_ipa_priv *ipa_ctxt, void *in)
{
	struct ipa_wdi_in_params *wdi_in;

	wdi_in = (struct ipa_wdi_in_params *)in;
	wdi_in->wdi_notify = hdd_ipa_wdi_meter_notifier_cb;

	init_completion(&ipa_ctxt->ipa_uc_sharing_stats_comp);
	init_completion(&ipa_ctxt->ipa_uc_set_quota_comp);
}
#endif

static int hdd_ipa_wdi_init(struct hdd_ipa_priv *hdd_ipa)
{
	struct ipa_wdi_uc_ready_params uc_ready_param;
	int ret = 0;

	hdd_ipa->uc_loaded = false;
	uc_ready_param.priv = (void *)hdd_ipa;
	uc_ready_param.notify = hdd_ipa_uc_loaded_uc_cb;
	if (ipa_uc_reg_rdyCB(&uc_ready_param)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"UC Ready CB register fail");
		return -EPERM;
	}

	if (true == uc_ready_param.is_uC_ready) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO, "UC Ready");
		hdd_ipa->uc_loaded = true;
	} else {
		ret = -EACCES;
	}

	return ret;
}

static int hdd_ipa_wdi_cleanup(void)
{
	int ret;

	ret = ipa_uc_dereg_rdyCB();
	if (ret)
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"UC Ready CB deregister fail");
	return ret;
}

static int hdd_ipa_wdi_conn_pipes(struct hdd_ipa_priv *hdd_ipa,
		struct ol_txrx_ipa_resources *ipa_res)
{
	hdd_context_t *hdd_ctx = (hdd_context_t *)hdd_ipa->hdd_ctx;
	struct ipa_wdi_in_params pipe_in;
	struct ipa_wdi_out_params pipe_out;
	QDF_STATUS stat = QDF_STATUS_SUCCESS;
	qdf_device_t osdev = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);
	int ret;

	if (qdf_unlikely(NULL == osdev)) {
		QDF_TRACE(QDF_MODULE_ID_HDD_DATA, QDF_TRACE_LEVEL_ERROR,
			  "%s: osdev is NULL", __func__);
		stat = QDF_STATUS_E_FAILURE;
		goto fail_return;
	}


	qdf_mem_zero(&hdd_ipa->cons_pipe_in, sizeof(struct ipa_wdi_in_params));
	qdf_mem_zero(&hdd_ipa->prod_pipe_in, sizeof(struct ipa_wdi_in_params));
	qdf_mem_zero(&pipe_in, sizeof(struct ipa_wdi_in_params));
	qdf_mem_zero(&pipe_out, sizeof(struct ipa_wdi_out_params));

	/* TX PIPE */
	pipe_in.sys.ipa_ep_cfg.nat.nat_en = IPA_BYPASS_NAT;
	pipe_in.sys.ipa_ep_cfg.hdr.hdr_len = HDD_IPA_UC_WLAN_TX_HDR_LEN;
	pipe_in.sys.ipa_ep_cfg.hdr.hdr_ofst_pkt_size_valid = 1;
	pipe_in.sys.ipa_ep_cfg.hdr.hdr_ofst_pkt_size = 0;
	pipe_in.sys.ipa_ep_cfg.hdr.hdr_additional_const_len =
		HDD_IPA_UC_WLAN_8023_HDR_SIZE;
	pipe_in.sys.ipa_ep_cfg.mode.mode = IPA_BASIC;
	pipe_in.sys.client = IPA_CLIENT_WLAN1_CONS;
	pipe_in.sys.desc_fifo_sz = hdd_ctx->config->IpaDescSize;
	pipe_in.sys.priv = hdd_ctx->hdd_ipa;
	pipe_in.sys.ipa_ep_cfg.hdr_ext.hdr_little_endian = true;
	pipe_in.sys.notify = hdd_ipa_i2w_cb;
	if (!hdd_ipa_is_rm_enabled(hdd_ctx)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
				"IPA RM DISABLED, IPA AWAKE");
		pipe_in.sys.keep_ipa_awake = true;
	}

	pipe_in.smmu_enabled = qdf_mem_smmu_s1_enabled(osdev);
	if (pipe_in.smmu_enabled) {
		qdf_mem_copy(&pipe_in.u.dl_smmu.comp_ring,
			     &ipa_res->tx_comp_ring->sgtable,
			     sizeof(sgtable_t));

		qdf_mem_copy(&pipe_in.u.dl_smmu.ce_ring,
			     &ipa_res->ce_sr->sgtable,
			     sizeof(sgtable_t));
		pipe_in.u.dl_smmu.comp_ring_size =
			ipa_res->tx_comp_ring->mem_info.size;
		pipe_in.u.dl_smmu.ce_ring_size =
			ipa_res->ce_sr_ring_size;
		pipe_in.u.dl_smmu.ce_door_bell_pa =
			ipa_res->ce_reg_paddr;
		pipe_in.u.dl_smmu.num_tx_buffers =
			ipa_res->tx_num_alloc_buffer;
	} else {
		pipe_in.u.dl.comp_ring_base_pa =
			qdf_mem_get_dma_addr(osdev,
				&ipa_res->tx_comp_ring->mem_info);
		pipe_in.u.dl.ce_ring_base_pa =
			qdf_mem_get_dma_addr(osdev,
				&ipa_res->ce_sr->mem_info);
		pipe_in.u.dl.comp_ring_size =
			ipa_res->tx_comp_ring->mem_info.size;
		pipe_in.u.dl.ce_door_bell_pa = ipa_res->ce_reg_paddr;
		pipe_in.u.dl.ce_ring_size =
			ipa_res->ce_sr_ring_size;
		pipe_in.u.dl.num_tx_buffers =
			ipa_res->tx_num_alloc_buffer;
	}

	qdf_mem_copy(&hdd_ipa->cons_pipe_in, &pipe_in,
		     sizeof(struct ipa_wdi_in_params));

	/* Connect WDI IPA PIPE */
	ret = ipa_connect_wdi_pipe(&hdd_ipa->cons_pipe_in, &pipe_out);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"ipa_connect_wdi_pipe failed for Tx: ret=%d",
			ret);
		stat = QDF_STATUS_E_FAILURE;
		goto fail_return;
	}

	/* Micro Controller Doorbell register */
	hdd_ipa->tx_comp_doorbell_dmaaddr = pipe_out.uc_door_bell_pa;

	/* WLAN TX PIPE Handle */
	hdd_ipa->tx_pipe_handle = pipe_out.clnt_hdl;

	if (hdd_ipa->tx_pipe_handle == 0) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"TX Handle zero");
		QDF_BUG(0);
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
		"CONS DB pipe out 0x%x TX PIPE Handle 0x%x",
		(unsigned int)pipe_out.uc_door_bell_pa,
		hdd_ipa->tx_pipe_handle);
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
		"TX : CRBPA 0x%x, CRS %d, CERBPA 0x%x, CEDPA 0x%x,"
		" CERZ %d, NB %d, CDBPAD 0x%x",
		(unsigned int)pipe_in.u.dl.comp_ring_base_pa,
		pipe_in.u.dl.comp_ring_size,
		(unsigned int)pipe_in.u.dl.ce_ring_base_pa,
		(unsigned int)pipe_in.u.dl.ce_door_bell_pa,
		pipe_in.u.dl.ce_ring_size,
		pipe_in.u.dl.num_tx_buffers,
		(unsigned int)hdd_ipa->tx_comp_doorbell_dmaaddr);

	/* RX PIPE */
	pipe_in.sys.ipa_ep_cfg.nat.nat_en = IPA_BYPASS_NAT;
	pipe_in.sys.ipa_ep_cfg.hdr.hdr_len = HDD_IPA_UC_WLAN_RX_HDR_LEN;
	pipe_in.sys.ipa_ep_cfg.hdr.hdr_ofst_metadata_valid = 0;
	pipe_in.sys.ipa_ep_cfg.hdr.hdr_metadata_reg_valid = 1;
	pipe_in.sys.ipa_ep_cfg.mode.mode = IPA_BASIC;
	pipe_in.sys.client = IPA_CLIENT_WLAN1_PROD;
	pipe_in.sys.desc_fifo_sz = hdd_ctx->config->IpaDescSize +
				   sizeof(struct sps_iovec);
	pipe_in.sys.notify = hdd_ipa_w2i_cb;
	if (!hdd_ipa_is_rm_enabled(hdd_ctx)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
				"IPA RM DISABLED, IPA AWAKE");
		pipe_in.sys.keep_ipa_awake = true;
	}

	pipe_in.smmu_enabled = qdf_mem_smmu_s1_enabled(osdev);
	if (pipe_in.smmu_enabled) {
		qdf_mem_copy(&pipe_in.u.ul_smmu.rdy_ring,
			     &ipa_res->rx_rdy_ring->sgtable,
			     sizeof(sgtable_t));
		pipe_in.u.ul_smmu.rdy_ring_size =
			ipa_res->rx_rdy_ring->mem_info.size;
		pipe_in.u.ul_smmu.rdy_ring_rp_pa =
			ipa_res->rx_proc_done_idx->mem_info.pa;

		pipe_in.u.ul_smmu.rdy_ring_rp_va =
			ipa_res->rx_proc_done_idx->vaddr;

		HDD_IPA_WDI2_SET_SMMU();
	} else {
		pipe_in.u.ul.rdy_ring_base_pa =
			ipa_res->rx_rdy_ring->mem_info.pa;
		pipe_in.u.ul.rdy_ring_size =
			ipa_res->rx_rdy_ring->mem_info.size;
		pipe_in.u.ul.rdy_ring_rp_pa =
			ipa_res->rx_proc_done_idx->mem_info.pa;
		HDD_IPA_WDI2_SET(pipe_in, hdd_ipa, osdev);
	}

	hdd_ipa_wdi_init_metering(hdd_ipa, (void *)&pipe_in);

	qdf_mem_copy(&hdd_ipa->prod_pipe_in, &pipe_in,
		     sizeof(struct ipa_wdi_in_params));

	ret = ipa_connect_wdi_pipe(&hdd_ipa->prod_pipe_in, &pipe_out);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"ipa_connect_wdi_pipe failed for Rx: ret=%d",
			ret);
		stat = QDF_STATUS_E_FAILURE;
		ret = ipa_disconnect_wdi_pipe(hdd_ipa->tx_pipe_handle);
		if (ret)
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "disconnect failed for TX: ret=%d",
				    ret);
		goto fail_return;
	}
	hdd_ipa->rx_ready_doorbell_dmaaddr = pipe_out.uc_door_bell_pa;
	hdd_ipa->rx_pipe_handle = pipe_out.clnt_hdl;
	if (hdd_ipa->rx_pipe_handle == 0) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"RX Handle zero");
		QDF_BUG(0);
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
		"PROD DB pipe out 0x%x RX PIPE Handle 0x%x",
		(unsigned int)pipe_out.uc_door_bell_pa,
		hdd_ipa->rx_pipe_handle);
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
		"RX : RRBPA 0x%x, RRS %d, PDIPA 0x%x, RDY_DB_PAD 0x%x",
		(unsigned int)pipe_in.u.ul.rdy_ring_base_pa,
		pipe_in.u.ul.rdy_ring_size,
		(unsigned int)pipe_in.u.ul.rdy_ring_rp_pa,
		(unsigned int)hdd_ipa->rx_ready_doorbell_dmaaddr);

fail_return:
	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO, "exit: stat=%d", stat);
	return stat;
}

static int hdd_ipa_wdi_disconn_pipes(struct hdd_ipa_priv *hdd_ipa)
{
	int ret;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
		    "Disconnect TX PIPE tx_pipe_handle=0x%x",
		     hdd_ipa->tx_pipe_handle);
	ret = ipa_disconnect_wdi_pipe(hdd_ipa->tx_pipe_handle);
	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
		    "Disconnect RX PIPE rx_pipe_handle=0x%x",
		     hdd_ipa->rx_pipe_handle);
	ret = ipa_disconnect_wdi_pipe(hdd_ipa->rx_pipe_handle);

	return ret;
}

/**
 * hdd_remove_ipa_header() - Remove a specific header from IPA
 * @name: Name of the header to be removed
 *
 * Return: None
 */
static void hdd_ipa_remove_header(char *name)
{
	struct ipa_ioc_get_hdr hdrlookup;
	int ret = 0, len;
	struct ipa_ioc_del_hdr *ipa_hdr;

	qdf_mem_zero(&hdrlookup, sizeof(hdrlookup));
	strlcpy(hdrlookup.name, name, sizeof(hdrlookup.name));
	ret = ipa_get_hdr(&hdrlookup);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "Hdr deleted already %s, %d",
			    name, ret);
		return;
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "hdl: 0x%x", hdrlookup.hdl);
	len = sizeof(struct ipa_ioc_del_hdr) + sizeof(struct ipa_hdr_del) * 1;
	ipa_hdr = (struct ipa_ioc_del_hdr *)qdf_mem_malloc(len);
	if (ipa_hdr == NULL) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "ipa_hdr allocation failed");
		return;
	}
	ipa_hdr->num_hdls = 1;
	ipa_hdr->commit = 0;
	ipa_hdr->hdl[0].hdl = hdrlookup.hdl;
	ipa_hdr->hdl[0].status = -1;
	ret = ipa_del_hdr(ipa_hdr);
	if (ret != 0)
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "Delete header failed: %d",
			    ret);

	qdf_mem_free(ipa_hdr);
}

/**
 * wlan_ipa_add_hdr() - Add IPA Tx header
 * @ipa_hdr: pointer to IPA header addition parameters
 *
 * Call IPA API to add IPA Tx header descriptor
 * and dump Tx header struct
 *
 * Return: 0 for success, non-zero for failure
 */
static int wlan_ipa_add_hdr(struct ipa_ioc_add_hdr *ipa_hdr)
{
	int ret;

	hdd_debug("==== IPA Tx Header ====\n"
			"name: %s\n"
			"hdr_len: %d\n"
			"type: %d\n"
			"is_partial: %d\n"
			"hdr_hdl: 0x%x\n"
			"status: %d\n"
			"is_eth2_ofst_valid: %d\n"
			"eth2_ofst: %d\n",
			ipa_hdr->hdr[0].name,
			ipa_hdr->hdr[0].hdr_len,
			ipa_hdr->hdr[0].type,
			ipa_hdr->hdr[0].is_partial,
			ipa_hdr->hdr[0].hdr_hdl,
			ipa_hdr->hdr[0].status,
			ipa_hdr->hdr[0].is_eth2_ofst_valid,
			ipa_hdr->hdr[0].eth2_ofst);

	HDD_IPA_DBG_DUMP(QDF_TRACE_LEVEL_DEBUG, "hdr:",
			ipa_hdr->hdr[0].hdr, HDD_IPA_UC_WLAN_TX_HDR_LEN);

	ret = ipa_add_hdr(ipa_hdr);
	return ret;
}

/* For Tx pipes, use 802.3 Header format */
static struct hdd_ipa_tx_hdr ipa_tx_hdr = {
	{
		{0xDE, 0xAD, 0xBE, 0xEF, 0xFF, 0xFF},
		{0xDE, 0xAD, 0xBE, 0xEF, 0xFF, 0xFF},
		0x00            /* length can be zero */
	},
	{
		/* LLC SNAP header 8 bytes */
		0xaa, 0xaa,
		{0x03, 0x00, 0x00, 0x00},
		0x0008          /* type value(2 bytes) ,filled by wlan  */
		/* 0x0800 - IPV4, 0x86dd - IPV6 */
	}
};

/**
 * hdd_ipa_add_header_info() - Add IPA header for a given interface
 * @hdd_ipa: Global HDD IPA context
 * @iface_context: Interface-specific HDD IPA context
 * @mac_addr: Interface MAC address
 *
 * Return: 0 on success, negativer errno value on error
 */
static int hdd_ipa_add_header_info(struct hdd_ipa_priv *hdd_ipa,
				   struct hdd_ipa_iface_context *iface_context,
				   uint8_t *mac_addr)
{
	hdd_adapter_t *adapter = iface_context->adapter;
	char *ifname;
	struct ipa_ioc_add_hdr *ipa_hdr = NULL;
	int ret = -EINVAL;
	struct hdd_ipa_tx_hdr *tx_hdr = NULL;
	struct hdd_ipa_uc_tx_hdr *uc_tx_hdr = NULL;

	ifname = adapter->dev->name;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "Add Partial hdr: %s, %pM",
		    ifname, mac_addr);

	/* dynamically allocate the memory to add the hdrs */
	ipa_hdr = qdf_mem_malloc(sizeof(struct ipa_ioc_add_hdr)
				 + sizeof(struct ipa_hdr_add));
	if (!ipa_hdr) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "%s: ipa_hdr allocation failed", ifname);
		ret = -ENOMEM;
		goto end;
	}

	ipa_hdr->commit = 0;
	ipa_hdr->num_hdrs = 1;

	if (hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
		uc_tx_hdr = (struct hdd_ipa_uc_tx_hdr *)ipa_hdr->hdr[0].hdr;
		memcpy(uc_tx_hdr, &ipa_uc_tx_hdr, HDD_IPA_UC_WLAN_TX_HDR_LEN);
		memcpy(uc_tx_hdr->eth.h_source, mac_addr, ETH_ALEN);
		uc_tx_hdr->ipa_hd.vdev_id = iface_context->adapter->sessionId;
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
			"ifname=%s, vdev_id=%d",
			ifname, uc_tx_hdr->ipa_hd.vdev_id);
		snprintf(ipa_hdr->hdr[0].name, IPA_RESOURCE_NAME_MAX, "%s%s",
				ifname, HDD_IPA_IPV4_NAME_EXT);
		ipa_hdr->hdr[0].hdr_len = HDD_IPA_UC_WLAN_TX_HDR_LEN;
		ipa_hdr->hdr[0].type = IPA_HDR_L2_ETHERNET_II;
		ipa_hdr->hdr[0].is_partial = 1;
		ipa_hdr->hdr[0].hdr_hdl = 0;
		ipa_hdr->hdr[0].is_eth2_ofst_valid = 1;
		ipa_hdr->hdr[0].eth2_ofst = HDD_IPA_UC_WLAN_HDR_DES_MAC_OFFSET;

		ret = wlan_ipa_add_hdr(ipa_hdr);
	} else {
		tx_hdr = (struct hdd_ipa_tx_hdr *)ipa_hdr->hdr[0].hdr;

		/* Set the Source MAC */
		memcpy(tx_hdr, &ipa_tx_hdr, HDD_IPA_WLAN_TX_HDR_LEN);
		memcpy(tx_hdr->eth.h_source, mac_addr, ETH_ALEN);

		snprintf(ipa_hdr->hdr[0].name, IPA_RESOURCE_NAME_MAX, "%s%s",
				ifname, HDD_IPA_IPV4_NAME_EXT);
		ipa_hdr->hdr[0].hdr_len = HDD_IPA_WLAN_TX_HDR_LEN;
		ipa_hdr->hdr[0].is_partial = 1;
		ipa_hdr->hdr[0].hdr_hdl = 0;
		ipa_hdr->hdr[0].is_eth2_ofst_valid = 1;
		ipa_hdr->hdr[0].eth2_ofst = HDD_IPA_WLAN_HDR_DES_MAC_OFFSET;

		/* Set the type to IPV4 in the header */
		tx_hdr->llc_snap.eth_type = cpu_to_be16(ETH_P_IP);

		ret = ipa_add_hdr(ipa_hdr);
	}
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "%s: IPv4 add hdr failed: %d", ifname, ret);
		goto end;
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "%s: IPv4 hdr_hdl: 0x%x",
		    ipa_hdr->hdr[0].name, ipa_hdr->hdr[0].hdr_hdl);

	if (hdd_ipa_is_ipv6_enabled(hdd_ipa->hdd_ctx)) {
		snprintf(ipa_hdr->hdr[0].name, IPA_RESOURCE_NAME_MAX, "%s%s",
			 ifname, HDD_IPA_IPV6_NAME_EXT);

		if (hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
			uc_tx_hdr =
				(struct hdd_ipa_uc_tx_hdr *)ipa_hdr->hdr[0].hdr;
			uc_tx_hdr->eth.h_proto = cpu_to_be16(ETH_P_IPV6);
			ret = wlan_ipa_add_hdr(ipa_hdr);
		} else {
			/* Set the type to IPV6 in the header */
			tx_hdr = (struct hdd_ipa_tx_hdr *)ipa_hdr->hdr[0].hdr;
			tx_hdr->llc_snap.eth_type = cpu_to_be16(ETH_P_IPV6);
			ret = ipa_add_hdr(ipa_hdr);
		}

		if (ret) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "%s: IPv6 add hdr failed: %d", ifname, ret);
			goto clean_ipv4_hdr;
		}

		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "%s: IPv6 hdr_hdl: 0x%x",
			    ipa_hdr->hdr[0].name, ipa_hdr->hdr[0].hdr_hdl);
	}

	qdf_mem_free(ipa_hdr);

	return ret;

clean_ipv4_hdr:
	snprintf(ipa_hdr->hdr[0].name, IPA_RESOURCE_NAME_MAX, "%s%s",
		 ifname, HDD_IPA_IPV4_NAME_EXT);
	hdd_ipa_remove_header(ipa_hdr->hdr[0].name);
end:
	if (ipa_hdr)
		qdf_mem_free(ipa_hdr);

	return ret;
}

/**
 * hdd_ipa_register_interface() - register IPA interface
 * @hdd_ipa: Global IPA context
 * @iface_context: Per-interface IPA context
 *
 * Return: 0 on success, negative errno on error
 */
static int hdd_ipa_register_interface(struct hdd_ipa_priv *hdd_ipa,
				      struct hdd_ipa_iface_context
				      *iface_context)
{
	struct ipa_tx_intf tx_intf;
	struct ipa_rx_intf rx_intf;
	struct ipa_ioc_tx_intf_prop *tx_prop = NULL;
	struct ipa_ioc_rx_intf_prop *rx_prop = NULL;
	char *ifname = iface_context->adapter->dev->name;

	char ipv4_hdr_name[IPA_RESOURCE_NAME_MAX];
	char ipv6_hdr_name[IPA_RESOURCE_NAME_MAX];

	int num_prop = 1;
	int ret = 0;

	if (hdd_ipa_is_ipv6_enabled(hdd_ipa->hdd_ctx))
		num_prop++;

	/* Allocate TX properties for TOS categories, 1 each for IPv4 & IPv6 */
	tx_prop =
		qdf_mem_malloc(sizeof(struct ipa_ioc_tx_intf_prop) * num_prop);
	if (!tx_prop) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "tx_prop allocation failed");
		goto register_interface_fail;
	}

	/* Allocate RX properties, 1 each for IPv4 & IPv6 */
	rx_prop =
		qdf_mem_malloc(sizeof(struct ipa_ioc_rx_intf_prop) * num_prop);
	if (!rx_prop) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "rx_prop allocation failed");
		goto register_interface_fail;
	}

	qdf_mem_zero(&tx_intf, sizeof(tx_intf));
	qdf_mem_zero(&rx_intf, sizeof(rx_intf));

	snprintf(ipv4_hdr_name, IPA_RESOURCE_NAME_MAX, "%s%s",
		 ifname, HDD_IPA_IPV4_NAME_EXT);
	snprintf(ipv6_hdr_name, IPA_RESOURCE_NAME_MAX, "%s%s",
		 ifname, HDD_IPA_IPV6_NAME_EXT);

	rx_prop[IPA_IP_v4].ip = IPA_IP_v4;
	rx_prop[IPA_IP_v4].src_pipe = iface_context->prod_client;
	rx_prop[IPA_IP_v4].hdr_l2_type = IPA_HDR_L2_ETHERNET_II;
	rx_prop[IPA_IP_v4].attrib.attrib_mask = IPA_FLT_META_DATA;

	/*
	 * Interface ID is 3rd byte in the CLD header. Add the meta data and
	 * mask to identify the interface in IPA hardware
	 */
	rx_prop[IPA_IP_v4].attrib.meta_data =
		htonl(iface_context->adapter->sessionId << 16);
	rx_prop[IPA_IP_v4].attrib.meta_data_mask = htonl(0x00FF0000);

	rx_intf.num_props++;
	if (hdd_ipa_is_ipv6_enabled(hdd_ipa->hdd_ctx)) {
		rx_prop[IPA_IP_v6].ip = IPA_IP_v6;
		rx_prop[IPA_IP_v6].src_pipe = iface_context->prod_client;
		rx_prop[IPA_IP_v6].hdr_l2_type = IPA_HDR_L2_ETHERNET_II;
		rx_prop[IPA_IP_v4].attrib.attrib_mask = IPA_FLT_META_DATA;
		rx_prop[IPA_IP_v4].attrib.meta_data =
			htonl(iface_context->adapter->sessionId << 16);
		rx_prop[IPA_IP_v4].attrib.meta_data_mask = htonl(0x00FF0000);

		rx_intf.num_props++;
	}

	tx_prop[IPA_IP_v4].ip = IPA_IP_v4;
	tx_prop[IPA_IP_v4].hdr_l2_type = IPA_HDR_L2_ETHERNET_II;
	tx_prop[IPA_IP_v4].dst_pipe = IPA_CLIENT_WLAN1_CONS;
	tx_prop[IPA_IP_v4].alt_dst_pipe = iface_context->cons_client;
	strlcpy(tx_prop[IPA_IP_v4].hdr_name, ipv4_hdr_name,
			IPA_RESOURCE_NAME_MAX);
	tx_intf.num_props++;

	if (hdd_ipa_is_ipv6_enabled(hdd_ipa->hdd_ctx)) {
		tx_prop[IPA_IP_v6].ip = IPA_IP_v6;
		tx_prop[IPA_IP_v6].hdr_l2_type = IPA_HDR_L2_ETHERNET_II;
		tx_prop[IPA_IP_v6].dst_pipe = IPA_CLIENT_WLAN1_CONS;
		tx_prop[IPA_IP_v6].alt_dst_pipe = iface_context->cons_client;
		strlcpy(tx_prop[IPA_IP_v6].hdr_name, ipv6_hdr_name,
				IPA_RESOURCE_NAME_MAX);
		tx_intf.num_props++;
	}

	tx_intf.prop = tx_prop;
	rx_intf.prop = rx_prop;

	/* Call the ipa api to register interface */
	ret = ipa_register_intf(ifname, &tx_intf, &rx_intf);

register_interface_fail:
	qdf_mem_free(tx_prop);
	qdf_mem_free(rx_prop);
	return ret;
}

static int hdd_ipa_wdi_reg_intf(struct hdd_ipa_priv *hdd_ipa,
		struct hdd_ipa_iface_context *iface_context)
{
	int ret;

	ret = hdd_ipa_add_header_info(hdd_ipa, iface_context,
				      iface_context->adapter->dev->dev_addr);

	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"ipa add header failed ret=%d", ret);
		return ret;
	}

	/* Configure the TX and RX pipes filter rules */
	ret = hdd_ipa_register_interface(hdd_ipa, iface_context);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "IPA WDI reg intf failed ret=%d", ret);
		ret = -EFAULT;
		return ret;
	}

	return 0;
}

static int hdd_ipa_wdi_dereg_intf(struct hdd_ipa_priv *hdd_ipa,
		const char *devname)
{
	char name_ipa[IPA_RESOURCE_NAME_MAX];
	int ret;

	/* Remove the headers */
	snprintf(name_ipa, IPA_RESOURCE_NAME_MAX, "%s%s", devname,
			HDD_IPA_IPV4_NAME_EXT);
	hdd_ipa_remove_header(name_ipa);

	if (hdd_ipa_is_ipv6_enabled(hdd_ipa->hdd_ctx)) {
		snprintf(name_ipa, IPA_RESOURCE_NAME_MAX, "%s%s", devname,
				HDD_IPA_IPV6_NAME_EXT);
		hdd_ipa_remove_header(name_ipa);
	}

	/* unregister the interface with IPA */
	ret = ipa_deregister_intf(devname);
	if (ret)
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
			    "%s: ipa_deregister_intf fail: %d", devname, ret);
	return ret;
}

static int hdd_ipa_wdi_enable_pipes(struct hdd_ipa_priv *hdd_ipa)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	int result;

	if (!pdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "pdev is NULL");
		result = QDF_STATUS_E_FAILURE;
		return result;
	}

	/* Map IPA SMMU for all Rx hash table */
	result = ol_txrx_rx_hash_smmu_map(pdev, true);
	if (result) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "IPA SMMU map failed ret=%d", result);
		return result;
	}
	/* ACTIVATE TX PIPE */
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
			"Enable TX PIPE(tx_pipe_handle=%d)",
			 hdd_ipa->tx_pipe_handle);
	result = ipa_enable_wdi_pipe(hdd_ipa->tx_pipe_handle);
	if (result) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Enable TX PIPE fail, code %d",
			     result);
		goto smmu_unmap;
	}

	result = ipa_resume_wdi_pipe(hdd_ipa->tx_pipe_handle);
	if (result) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Resume TX PIPE fail, code %d",
			     result);
		goto smmu_unmap;
	}

	/* ACTIVATE RX PIPE */
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
			"Enable RX PIPE(rx_pipe_handle=%d)",
			 hdd_ipa->rx_pipe_handle);
	result = ipa_enable_wdi_pipe(hdd_ipa->rx_pipe_handle);
	if (result) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Enable RX PIPE fail, code %d",
			     result);
		goto smmu_unmap;
	}

	result = ipa_resume_wdi_pipe(hdd_ipa->rx_pipe_handle);
	if (result) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Resume RX PIPE fail, code %d",
			     result);
		goto smmu_unmap;
	}

	return 0;

smmu_unmap:
	if (ol_txrx_rx_hash_smmu_map(pdev, false)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "IPA SMMU unmap failed");
	}

	return result;
}

static int hdd_ipa_wdi_disable_pipes(struct hdd_ipa_priv *hdd_ipa)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	int result;

	if (!pdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "pdev is NULL");
		result = QDF_STATUS_E_FAILURE;
		return result;
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "Disable RX PIPE");
	result = ipa_suspend_wdi_pipe(hdd_ipa->rx_pipe_handle);
	if (result) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Suspend RX PIPE fail, code %d", result);
		return result;
	}

	result = ipa_disable_wdi_pipe(hdd_ipa->rx_pipe_handle);
	if (result) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Disable RX PIPE fail, code %d", result);
		return result;
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "Disable TX PIPE");
	result = ipa_suspend_wdi_pipe(hdd_ipa->tx_pipe_handle);
	if (result) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Suspend TX PIPE fail, code %d", result);
		return result;
	}

	result = ipa_disable_wdi_pipe(hdd_ipa->tx_pipe_handle);
	if (result) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Disable TX PIPE fail, code %d", result);
		return result;
	}

	/* Unmap IPA SMMU for all Rx hash table */
	result = ol_txrx_rx_hash_smmu_map(pdev, false);
	if (result) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "IPA SMMU unmap failed");
		return result;
	}

	return 0;
}

static int hdd_ipa_wdi_setup_sys_pipe(struct hdd_ipa_priv *hdd_ipa,
		struct ipa_sys_connect_params *sys, uint32_t *handle)
{
	return ipa_setup_sys_pipe(sys, handle);
}

static int hdd_ipa_wdi_teardown_sys_pipe(struct hdd_ipa_priv *hdd_ipa,
		uint32_t handle)
{
	return ipa_teardown_sys_pipe(handle);
}

static int hdd_ipa_wdi_rm_set_perf_profile(struct hdd_ipa_priv *hdd_ipa,
		int client, uint32_t max_supported_bw_mbps)
{
	enum ipa_rm_resource_name resource_name;
	struct ipa_rm_perf_profile profile;

	if (client == IPA_CLIENT_WLAN1_PROD) {
		resource_name = IPA_RM_RESOURCE_WLAN_PROD;
	} else if (client == IPA_CLIENT_WLAN1_CONS) {
		resource_name = IPA_RM_RESOURCE_WLAN_CONS;
	} else {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"not supported client: %d", client);
		return -EINVAL;
	}

	profile.max_supported_bandwidth_mbps = max_supported_bw_mbps;

	return ipa_rm_set_perf_profile(resource_name, &profile);
}

static int hdd_ipa_wdi_rm_request_resource(struct hdd_ipa_priv *hdd_ipa,
		enum ipa_rm_resource_name res_name)
{
	return ipa_rm_request_resource(res_name);
}

static int hdd_ipa_wdi_rm_release_resource(struct hdd_ipa_priv *hdd_ipa,
		enum ipa_rm_resource_name res_name)
{
	return ipa_rm_release_resource(res_name);
}

/**
 * hdd_ipa_init_uc_rm_work - init ipa uc resource manager work
 * @work: struct work_struct
 * @work_handler: work_handler
 *
 * Return: none
 */
static void hdd_ipa_init_uc_rm_work(struct work_struct *work,
					work_func_t work_handler)
{
	INIT_WORK(work, work_handler);
}

/**
 * hdd_ipa_wake_lock_timer_func() - Wake lock work handler
 * @work: scheduled work
 *
 * When IPA resources are released in hdd_ipa_wdi_rm_try_release() we do
 * not want to immediately release the wake lock since the system
 * would then potentially try to suspend when there is a healthy data
 * rate.  Deferred work is scheduled and this function handles the
 * work.  When this function is called, if the IPA resource is still
 * released then we release the wake lock.
 *
 * Return: None
 */
static void hdd_ipa_wake_lock_timer_func(struct work_struct *work)
{
	struct hdd_ipa_priv *hdd_ipa = container_of(to_delayed_work(work),
						    struct hdd_ipa_priv,
						    wake_lock_work);

	qdf_spin_lock_bh(&hdd_ipa->rm_lock);

	if (hdd_ipa->rm_state != HDD_IPA_RM_RELEASED)
		goto end;

	hdd_ipa->wake_lock_released = true;
	qdf_wake_lock_release(&hdd_ipa->wake_lock,
			      WIFI_POWER_EVENT_WAKELOCK_IPA);

end:
	qdf_spin_unlock_bh(&hdd_ipa->rm_lock);
}

/**
 * hdd_ipa_wdi_rm_request() - Request resource from IPA
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: 0 on success, negative errno on error
 */
static int hdd_ipa_wdi_rm_request(struct hdd_ipa_priv *hdd_ipa)
{
	int ret = 0;

	if (!hdd_ipa_is_rm_enabled(hdd_ipa->hdd_ctx))
		return 0;

	qdf_spin_lock_bh(&hdd_ipa->rm_lock);

	switch (hdd_ipa->rm_state) {
	case HDD_IPA_RM_GRANTED:
		qdf_spin_unlock_bh(&hdd_ipa->rm_lock);
		return 0;
	case HDD_IPA_RM_GRANT_PENDING:
		qdf_spin_unlock_bh(&hdd_ipa->rm_lock);
		return -EINPROGRESS;
	case HDD_IPA_RM_RELEASED:
		hdd_ipa->rm_state = HDD_IPA_RM_GRANT_PENDING;
		break;
	}

	qdf_spin_unlock_bh(&hdd_ipa->rm_lock);

	ret = ipa_rm_inactivity_timer_request_resource(
			IPA_RM_RESOURCE_WLAN_PROD);

	qdf_spin_lock_bh(&hdd_ipa->rm_lock);
	if (ret == 0) {
		hdd_ipa->rm_state = HDD_IPA_RM_GRANTED;
		hdd_ipa->stats.num_rm_grant_imm++;
	}

	cancel_delayed_work(&hdd_ipa->wake_lock_work);
	if (hdd_ipa->wake_lock_released) {
		qdf_wake_lock_acquire(&hdd_ipa->wake_lock,
				      WIFI_POWER_EVENT_WAKELOCK_IPA);
		hdd_ipa->wake_lock_released = false;
	}
	qdf_spin_unlock_bh(&hdd_ipa->rm_lock);

	return ret;
}

/**
 * hdd_ipa_wdi_rm_try_release() - Attempt to release IPA resource
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: 0 if resources released, negative errno otherwise
 */
static int hdd_ipa_wdi_rm_try_release(struct hdd_ipa_priv *hdd_ipa)
{
	int ret = 0;

	if (!hdd_ipa_is_rm_enabled(hdd_ipa->hdd_ctx))
		return 0;

	if (atomic_read(&hdd_ipa->tx_ref_cnt))
		return -EAGAIN;

	qdf_spin_lock_bh(&hdd_ipa->pm_lock);

	if (!qdf_nbuf_is_queue_empty(&hdd_ipa->pm_queue_head)) {
		qdf_spin_unlock_bh(&hdd_ipa->pm_lock);
		return -EAGAIN;
	}
	qdf_spin_unlock_bh(&hdd_ipa->pm_lock);

	qdf_spin_lock_bh(&hdd_ipa->rm_lock);
	switch (hdd_ipa->rm_state) {
	case HDD_IPA_RM_GRANTED:
		break;
	case HDD_IPA_RM_GRANT_PENDING:
		qdf_spin_unlock_bh(&hdd_ipa->rm_lock);
		return -EINPROGRESS;
	case HDD_IPA_RM_RELEASED:
		qdf_spin_unlock_bh(&hdd_ipa->rm_lock);
		return 0;
	}

	/* IPA driver returns immediately so set the state here to avoid any
	 * race condition.
	 */
	hdd_ipa->rm_state = HDD_IPA_RM_RELEASED;
	hdd_ipa->stats.num_rm_release++;
	qdf_spin_unlock_bh(&hdd_ipa->rm_lock);

	ret = ipa_rm_inactivity_timer_release_resource(
				IPA_RM_RESOURCE_WLAN_PROD);

	qdf_spin_lock_bh(&hdd_ipa->rm_lock);
	if (unlikely(ret != 0)) {
		hdd_ipa->rm_state = HDD_IPA_RM_GRANTED;
		WARN_ON(1);
		HDD_IPA_LOG(QDF_TRACE_LEVEL_WARN,
			"ipa_rm_inactivity_timer_release_resource returnied fail");
	}

	/*
	 * If wake_lock is released immediately, kernel would try to suspend
	 * immediately as well, Just avoid ping-pong between suspend-resume
	 * while there is healthy amount of data transfer going on by
	 * releasing the wake_lock after some delay.
	 */
	schedule_delayed_work(&hdd_ipa->wake_lock_work,
			      msecs_to_jiffies
				      (HDD_IPA_RX_INACTIVITY_MSEC_DELAY));

	qdf_spin_unlock_bh(&hdd_ipa->rm_lock);

	return ret;
}

/**
 * hdd_ipa_rm_notify() - IPA resource manager notifier callback
 * @user_data: user data registered with IPA
 * @event: the IPA resource manager event that occurred
 * @data: the data associated with the event
 *
 * Return: None
 */
static void hdd_ipa_rm_notify(void *user_data, enum ipa_rm_event event,
			      unsigned long data)
{
	struct hdd_ipa_priv *hdd_ipa = user_data;

	if (unlikely(!hdd_ipa))
		return;

	if (!hdd_ipa_is_rm_enabled(hdd_ipa->hdd_ctx))
		return;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "Evt: %d", event);

	switch (event) {
	case IPA_RM_RESOURCE_GRANTED:
		if (hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
			/* RM Notification comes with ISR context
			 * it should be serialized into work queue to avoid
			 * ISR sleep problem
			 */
			hdd_ipa->uc_rm_work.event = event;
			schedule_work(&hdd_ipa->uc_rm_work.work);
			break;
		}
		qdf_spin_lock_bh(&hdd_ipa->rm_lock);
		hdd_ipa->rm_state = HDD_IPA_RM_GRANTED;
		qdf_spin_unlock_bh(&hdd_ipa->rm_lock);
		hdd_ipa->stats.num_rm_grant++;
		break;

	case IPA_RM_RESOURCE_RELEASED:
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "RM Release");
		hdd_ipa->resource_unloading = false;
		break;

	default:
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "Unknown RM Evt: %d", event);
		break;
	}
}

/**
 * hdd_ipa_rm_cons_release() - WLAN consumer resource release handler
 *
 * Callback function registered with IPA that is called when IPA wants
 * to release the WLAN consumer resource
 *
 * Return: 0 if the request is granted, negative errno otherwise
 */
static int hdd_ipa_rm_cons_release(void)
{
	return 0;
}

/**
 * hdd_ipa_rm_cons_request() - WLAN consumer resource request handler
 *
 * Callback function registered with IPA that is called when IPA wants
 * to access the WLAN consumer resource
 *
 * Return: 0 if the request is granted, negative errno otherwise
 */
static int hdd_ipa_rm_cons_request(void)
{
	int ret = 0;

	if (ghdd_ipa->resource_loading) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL,
			    "IPA resource loading in progress");
		ghdd_ipa->pending_cons_req = true;
		ret = -EINPROGRESS;
	} else if (ghdd_ipa->resource_unloading) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL,
				"IPA resource unloading in progress");
		ghdd_ipa->pending_cons_req = true;
		ret = -EPERM;
	}

	return ret;
}

/**
 * hdd_ipa_uc_rm_notify_handler() - IPA uC resource notification handler
 * @context: User context registered with TL (the IPA Global context is
 *	registered
 * @rxpkt: Packet containing the notification
 * @staid: ID of the station associated with the packet
 *
 * Return: None
 */
static void
hdd_ipa_uc_rm_notify_handler(void *context, enum ipa_rm_event event)
{
	struct hdd_ipa_priv *hdd_ipa = context;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	/*
	 * When SSR is going on or driver is unloading, just return.
	 */
	status = wlan_hdd_validate_context(hdd_ipa->hdd_ctx);
	if (status)
		return;

	if (!hdd_ipa_is_rm_enabled(hdd_ipa->hdd_ctx))
		return;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "event code %d",
		     event);

	switch (event) {
	case IPA_RM_RESOURCE_GRANTED:
		/* Differed RM Granted */
		qdf_mutex_acquire(&hdd_ipa->ipa_lock);
		if ((false == hdd_ipa->resource_unloading) &&
		    (!hdd_ipa->activated_fw_pipe)) {
			hdd_ipa_uc_enable_pipes(hdd_ipa);
			hdd_ipa->resource_loading = false;
		}
		qdf_mutex_release(&hdd_ipa->ipa_lock);
		break;

	case IPA_RM_RESOURCE_RELEASED:
		/* Differed RM Released */
		hdd_ipa->resource_unloading = false;
		break;

	default:
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "invalid event code %d",  event);
		break;
	}
}

/**
 * hdd_ipa_uc_rm_notify_defer() - Defer IPA uC notification
 * @hdd_ipa: Global HDD IPA context
 * @event: IPA resource manager event to be deferred
 *
 * This function is called when a resource manager event is received
 * from firmware in interrupt context.  This function will defer the
 * handling to the OL RX thread
 *
 * Return: None
 */
static void hdd_ipa_uc_rm_notify_defer(struct work_struct *work)
{
	enum ipa_rm_event event;
	struct uc_rm_work_struct *uc_rm_work = container_of(work,
			struct uc_rm_work_struct, work);
	struct hdd_ipa_priv *hdd_ipa = container_of(uc_rm_work,
			struct hdd_ipa_priv, uc_rm_work);

	cds_ssr_protect(__func__);
	event = uc_rm_work->event;
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
		"posted event %d",  event);

	hdd_ipa_uc_rm_notify_handler(hdd_ipa, event);
	cds_ssr_unprotect(__func__);
}

/**
 * hdd_ipa_wdi_setup_rm() - Setup IPA resource management
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: 0 on success, negative errno on error
 */
static int hdd_ipa_wdi_setup_rm(struct hdd_ipa_priv *hdd_ipa)
{
	struct ipa_rm_create_params create_params = { 0 };
	int ret;

	if (!hdd_ipa_is_rm_enabled(hdd_ipa->hdd_ctx))
		return 0;

	hdd_ipa_init_uc_rm_work(&hdd_ipa->uc_rm_work.work,
		hdd_ipa_uc_rm_notify_defer);
	memset(&create_params, 0, sizeof(create_params));
	create_params.name = IPA_RM_RESOURCE_WLAN_PROD;
	create_params.reg_params.user_data = hdd_ipa;
	create_params.reg_params.notify_cb = hdd_ipa_rm_notify;
	create_params.floor_voltage = IPA_VOLTAGE_SVS;

	ret = ipa_rm_create_resource(&create_params);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Create RM resource failed: %d", ret);
		goto setup_rm_fail;
	}

	memset(&create_params, 0, sizeof(create_params));
	create_params.name = IPA_RM_RESOURCE_WLAN_CONS;
	create_params.request_resource = hdd_ipa_rm_cons_request;
	create_params.release_resource = hdd_ipa_rm_cons_release;
	create_params.floor_voltage = IPA_VOLTAGE_SVS;

	ret = ipa_rm_create_resource(&create_params);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Create RM CONS resource failed: %d", ret);
		goto delete_prod;
	}

	ipa_rm_add_dependency(IPA_RM_RESOURCE_WLAN_PROD,
			      IPA_RM_RESOURCE_APPS_CONS);

	ret = ipa_rm_inactivity_timer_init(IPA_RM_RESOURCE_WLAN_PROD,
					   HDD_IPA_RX_INACTIVITY_MSEC_DELAY);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "Timer init failed: %d",
			    ret);
		goto timer_init_failed;
	}

	qdf_wake_lock_create(&hdd_ipa->wake_lock, "wlan_ipa");
	INIT_DELAYED_WORK(&hdd_ipa->wake_lock_work,
			  hdd_ipa_wake_lock_timer_func);
	qdf_spinlock_create(&hdd_ipa->rm_lock);
	hdd_ipa->rm_state = HDD_IPA_RM_RELEASED;
	hdd_ipa->wake_lock_released = true;
	atomic_set(&hdd_ipa->tx_ref_cnt, 0);

	return ret;

timer_init_failed:
	ipa_rm_delete_resource(IPA_RM_RESOURCE_WLAN_CONS);

delete_prod:
	ipa_rm_delete_resource(IPA_RM_RESOURCE_WLAN_PROD);

setup_rm_fail:
	return ret;
}

/**
 * hdd_ipa_wdi_destroy_rm() - Destroy IPA resources
 * @hdd_ipa: Global HDD IPA context
 *
 * Destroys all resources associated with the IPA resource manager
 *
 * Return: None
 */
static void hdd_ipa_wdi_destroy_rm(struct hdd_ipa_priv *hdd_ipa)
{
	int ret;

	if (!hdd_ipa_is_rm_enabled(hdd_ipa->hdd_ctx))
		return;

	cancel_delayed_work_sync(&hdd_ipa->wake_lock_work);
	qdf_wake_lock_destroy(&hdd_ipa->wake_lock);
	cancel_work_sync(&hdd_ipa->uc_rm_work.work);
	qdf_spinlock_destroy(&hdd_ipa->rm_lock);

	ipa_rm_inactivity_timer_destroy(IPA_RM_RESOURCE_WLAN_PROD);

	ret = ipa_rm_delete_resource(IPA_RM_RESOURCE_WLAN_PROD);
	if (ret)
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "RM PROD resource delete failed %d", ret);

	ret = ipa_rm_delete_resource(IPA_RM_RESOURCE_WLAN_CONS);
	if (ret)
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "RM CONS resource delete failed %d", ret);
}

static int hdd_ipa_wdi_rm_notify_completion(enum ipa_rm_event event,
		enum ipa_rm_resource_name resource_name)
{
	return ipa_rm_notify_completion(event, resource_name);
}

static bool hdd_ipa_is_rm_released(struct hdd_ipa_priv *hdd_ipa)
{
	qdf_spin_lock_bh(&hdd_ipa->rm_lock);

	if (hdd_ipa->rm_state != HDD_IPA_RM_RELEASED) {
		qdf_spin_unlock_bh(&hdd_ipa->rm_lock);
		return false;
	}

	qdf_spin_unlock_bh(&hdd_ipa->rm_lock);

	return true;
}

/**
 * hdd_ipa_pm_flush() - flush queued packets
 * @work: pointer to the scheduled work
 *
 * Called during PM resume to send packets to TL which were queued
 * while host was in the process of suspending.
 *
 * Return: None
 */
static void hdd_ipa_pm_flush(struct work_struct *work)
{
	struct hdd_ipa_priv *hdd_ipa = container_of(work,
						    struct hdd_ipa_priv,
						    pm_work);
	struct hdd_ipa_pm_tx_cb *pm_tx_cb = NULL;
	qdf_nbuf_t skb;
	uint32_t dequeued = 0;

	qdf_wake_lock_acquire(&hdd_ipa->wake_lock,
			      WIFI_POWER_EVENT_WAKELOCK_IPA);
	qdf_spin_lock_bh(&hdd_ipa->pm_lock);
	while (((skb = qdf_nbuf_queue_remove(&hdd_ipa->pm_queue_head))
								!= NULL)) {
		qdf_spin_unlock_bh(&hdd_ipa->pm_lock);

		pm_tx_cb = (struct hdd_ipa_pm_tx_cb *)skb->cb;
		dequeued++;
		if (pm_tx_cb->exception) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
				"Flush Exception");
			if (pm_tx_cb->adapter->dev)
				hdd_softap_hard_start_xmit(skb,
					  pm_tx_cb->adapter->dev);
			else
				dev_kfree_skb_any(skb);
		} else {
			hdd_ipa_send_pkt_to_tl(pm_tx_cb->iface_context,
				       pm_tx_cb->ipa_tx_desc);
		}
		qdf_spin_lock_bh(&hdd_ipa->pm_lock);
	}
	qdf_spin_unlock_bh(&hdd_ipa->pm_lock);
	qdf_wake_lock_release(&hdd_ipa->wake_lock,
			      WIFI_POWER_EVENT_WAKELOCK_IPA);

	hdd_ipa->stats.num_tx_dequeued += dequeued;
	if (dequeued > hdd_ipa->stats.num_max_pm_queue)
		hdd_ipa->stats.num_max_pm_queue = dequeued;
}

int hdd_ipa_uc_smmu_map(bool map, uint32_t num_buf, qdf_mem_info_t *buf_arr)
{
	if (!num_buf) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "No buffers to map/unmap");
		return 0;
	}

	if (map)
		return ipa_create_wdi_mapping(num_buf,
			   (struct ipa_wdi_buffer_info *)buf_arr);
	else
		return ipa_release_wdi_mapping(num_buf,
			   (struct ipa_wdi_buffer_info *)buf_arr);
}
#endif /* CONFIG_IPA_WDI_UNIFIED_API */

/**
 * hdd_ipa_init_perf_level() - Initialize IPA performance level
 * @hdd_cxt: HDD context
 *
 * If IPA clock scaling is disabled, initialize perf level to maximum.
 * Else set the lowest level to start with
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS hdd_ipa_init_perf_level(hdd_context_t *hdd_ctx)
{
	int ret;

	/* Set lowest bandwidth to start with if clk scaling enabled */
	if (hdd_ipa_is_clk_scaling_enabled(hdd_ctx)) {
		if (hdd_ipa_set_perf_level(hdd_ctx, 0, 0))
			return QDF_STATUS_E_FAILURE;
		else
			return QDF_STATUS_SUCCESS;
	}

	hdd_debug("IPA clock scaling is disabled. Set perf level to max %d",
		  HDD_IPA_MAX_BANDWIDTH);

	ret = hdd_ipa_wdi_rm_set_perf_profile(hdd_ctx->hdd_ipa,
			IPA_CLIENT_WLAN1_CONS, HDD_IPA_MAX_BANDWIDTH);
	if (ret) {
		hdd_err("CONS set perf profile failed: %d", ret);
		return QDF_STATUS_E_FAILURE;
	}

	ret = hdd_ipa_wdi_rm_set_perf_profile(hdd_ctx->hdd_ipa,
			IPA_CLIENT_WLAN1_PROD, HDD_IPA_MAX_BANDWIDTH);
	if (ret) {
		hdd_err("PROD set perf profile failed: %d", ret);
		return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}

/**
 * hdd_ipa_uc_rt_debug_host_fill - fill rt debug buffer
 * @ctext: pointer to hdd context.
 *
 * If rt debug enabled, periodically called, and fill debug buffer
 *
 * Return: none
 */
static void hdd_ipa_uc_rt_debug_host_fill(void *ctext)
{
	hdd_context_t *hdd_ctx = (hdd_context_t *)ctext;
	struct hdd_ipa_priv *hdd_ipa;
	struct uc_rt_debug_info *dump_info = NULL;

	if (wlan_hdd_validate_context(hdd_ctx))
		return;

	if (!hdd_ctx->hdd_ipa || !hdd_ipa_uc_is_enabled(hdd_ctx)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
			"IPA UC is not enabled");
		return;
	}

	hdd_ipa = (struct hdd_ipa_priv *)hdd_ctx->hdd_ipa;

	qdf_mutex_acquire(&hdd_ipa->rt_debug_lock);
	dump_info = &hdd_ipa->rt_bug_buffer[
		hdd_ipa->rt_buf_fill_index % HDD_IPA_UC_RT_DEBUG_BUF_COUNT];

	dump_info->time = (uint64_t)qdf_mc_timer_get_system_time();
	dump_info->ipa_excep_count = hdd_ipa->stats.num_rx_excep;
	dump_info->rx_drop_count = hdd_ipa->ipa_rx_internal_drop_count;
	dump_info->net_sent_count = hdd_ipa->ipa_rx_net_send_count;
	dump_info->tx_fwd_count = hdd_ipa->ipa_tx_forward;
	dump_info->tx_fwd_ok_count = hdd_ipa->stats.num_tx_fwd_ok;
	dump_info->rx_discard_count = hdd_ipa->ipa_rx_discard;
	dump_info->rx_destructor_call = hdd_ipa->ipa_rx_destructor_count;
	hdd_ipa->rt_buf_fill_index++;
	qdf_mutex_release(&hdd_ipa->rt_debug_lock);

	qdf_mc_timer_start(&hdd_ipa->rt_debug_fill_timer,
		HDD_IPA_UC_RT_DEBUG_FILL_INTERVAL);
}

/**
 * __hdd_ipa_uc_rt_debug_host_dump - dump rt debug buffer
 * @hdd_ctx: pointer to hdd context.
 *
 * If rt debug enabled, dump debug buffer contents based on requirement
 *
 * Return: none
 */
static void __hdd_ipa_uc_rt_debug_host_dump(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa;
	unsigned int dump_count;
	unsigned int dump_index;
	struct uc_rt_debug_info *dump_info = NULL;

	if (wlan_hdd_validate_context(hdd_ctx))
		return;

	hdd_ipa = hdd_ctx->hdd_ipa;
	if (!hdd_ipa || !hdd_ipa_uc_is_enabled(hdd_ctx)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
			"IPA UC is not enabled");
		return;
	}

	if (!hdd_ipa_is_rt_debugging_enabled(hdd_ctx)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
			"IPA RT debug is not enabled");
		return;
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
		"========= WLAN-IPA DEBUG BUF DUMP ==========\n");
	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
		"     TM     :   EXEP   :   DROP   :   NETS   :   FWOK   :   TXFD   :   DSTR   :   DSCD\n");

	qdf_mutex_acquire(&hdd_ipa->rt_debug_lock);
	for (dump_count = 0;
		dump_count < HDD_IPA_UC_RT_DEBUG_BUF_COUNT;
		dump_count++) {
		dump_index = (hdd_ipa->rt_buf_fill_index + dump_count) %
			HDD_IPA_UC_RT_DEBUG_BUF_COUNT;
		dump_info = &hdd_ipa->rt_bug_buffer[dump_index];
		HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
			"%12llu:%10llu:%10llu:%10llu:%10llu:%10llu:%10llu:%10llu\n",
			dump_info->time, dump_info->ipa_excep_count,
			dump_info->rx_drop_count, dump_info->net_sent_count,
			dump_info->tx_fwd_ok_count, dump_info->tx_fwd_count,
			dump_info->rx_destructor_call,
			dump_info->rx_discard_count);
	}
	qdf_mutex_release(&hdd_ipa->rt_debug_lock);
	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
		"======= WLAN-IPA DEBUG BUF DUMP END ========\n");
}

/**
 * hdd_ipa_uc_rt_debug_host_dump - SSR wrapper for
 * __hdd_ipa_uc_rt_debug_host_dump
 * @hdd_ctx: pointer to hdd context.
 *
 * If rt debug enabled, dump debug buffer contents based on requirement
 *
 * Return: none
 */
void hdd_ipa_uc_rt_debug_host_dump(hdd_context_t *hdd_ctx)
{
	cds_ssr_protect(__func__);
	__hdd_ipa_uc_rt_debug_host_dump(hdd_ctx);
	cds_ssr_unprotect(__func__);
}

/**
 * hdd_ipa_uc_rt_debug_handler - periodic memory health monitor handler
 * @ctext: pointer to hdd context.
 *
 * periodically called by timer expire
 * will try to alloc dummy memory and detect out of memory condition
 * if out of memory detected, dump wlan-ipa stats
 *
 * Return: none
 */
static void hdd_ipa_uc_rt_debug_handler(void *ctext)
{
	hdd_context_t *hdd_ctx = (hdd_context_t *)ctext;
	struct hdd_ipa_priv *hdd_ipa;
	void *dummy_ptr = NULL;

	if (wlan_hdd_validate_context(hdd_ctx))
		return;

	hdd_ipa = (struct hdd_ipa_priv *)hdd_ctx->hdd_ipa;

	if (!hdd_ipa_is_rt_debugging_enabled(hdd_ctx)) {
		hdd_debug("IPA RT debug is not enabled");
		return;
	}

	/* Allocate dummy buffer periodically and free immediately. this will
	 * proactively detect OOM and if allocation fails dump ipa stats
	 */
	dummy_ptr = kmalloc(HDD_IPA_UC_DEBUG_DUMMY_MEM_SIZE,
		GFP_KERNEL | GFP_ATOMIC);
	if (!dummy_ptr) {
		hdd_ipa_uc_rt_debug_host_dump(hdd_ctx);
		hdd_ipa_uc_stat_request(
			hdd_ctx,
			HDD_IPA_UC_STAT_REASON_DEBUG);
	} else {
		kfree(dummy_ptr);
	}

	qdf_mc_timer_start(&hdd_ipa->rt_debug_timer,
		HDD_IPA_UC_RT_DEBUG_PERIOD);
}

/**
 * hdd_ipa_uc_rt_debug_destructor() - called by data packet free
 * @skb: packet pinter
 *
 * when free data packet, will be invoked by wlan client and will increase
 * free counter
 *
 * Return: none
 */
static void hdd_ipa_uc_rt_debug_destructor(struct sk_buff *skb)
{
	if (!ghdd_ipa) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"invalid hdd context");
		return;
	}

	ghdd_ipa->ipa_rx_destructor_count++;
}

/**
 * hdd_ipa_uc_rt_debug_deinit() - remove resources to handle rt debugging
 * @hdd_ctx: hdd main context
 *
 * free all rt debugging resources
 *
 * Return: none
 */
static void hdd_ipa_uc_rt_debug_deinit(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa;

	if (wlan_hdd_validate_context(hdd_ctx))
		return;

	hdd_ipa = (struct hdd_ipa_priv *)hdd_ctx->hdd_ipa;

	qdf_mutex_destroy(&hdd_ipa->rt_debug_lock);

	if (!hdd_ipa_is_rt_debugging_enabled(hdd_ctx)) {
		hdd_debug("IPA RT debug is not enabled");
		return;
	}

	if (QDF_TIMER_STATE_STOPPED !=
		qdf_mc_timer_get_current_state(&hdd_ipa->rt_debug_fill_timer)) {
		qdf_mc_timer_stop(&hdd_ipa->rt_debug_fill_timer);
	}
	qdf_mc_timer_destroy(&hdd_ipa->rt_debug_fill_timer);

	if (QDF_TIMER_STATE_STOPPED !=
		qdf_mc_timer_get_current_state(&hdd_ipa->rt_debug_timer)) {
		qdf_mc_timer_stop(&hdd_ipa->rt_debug_timer);
	}
	qdf_mc_timer_destroy(&hdd_ipa->rt_debug_timer);
}

/**
 * hdd_ipa_uc_rt_debug_init() - intialize resources to handle rt debugging
 * @hdd_ctx: hdd main context
 *
 * alloc and initialize all rt debugging resources
 *
 * Return: none
 */
static void hdd_ipa_uc_rt_debug_init(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa;

	if (wlan_hdd_validate_context_in_loading(hdd_ctx))
		return;

	hdd_ipa = (struct hdd_ipa_priv *)hdd_ctx->hdd_ipa;

	qdf_mutex_create(&hdd_ipa->rt_debug_lock);
	hdd_ipa->rt_buf_fill_index = 0;
	qdf_mem_zero(hdd_ipa->rt_bug_buffer,
		sizeof(struct uc_rt_debug_info) *
		HDD_IPA_UC_RT_DEBUG_BUF_COUNT);
	hdd_ipa->ipa_tx_forward = 0;
	hdd_ipa->ipa_rx_discard = 0;
	hdd_ipa->ipa_rx_net_send_count = 0;
	hdd_ipa->ipa_rx_internal_drop_count = 0;
	hdd_ipa->ipa_rx_destructor_count = 0;

	/* Reatime debug enable on feature enable */
	if (!hdd_ipa_is_rt_debugging_enabled(hdd_ctx)) {
		hdd_debug("IPA RT debug is not enabled");
		return;
	}

	qdf_mc_timer_init(&hdd_ipa->rt_debug_fill_timer, QDF_TIMER_TYPE_SW,
		hdd_ipa_uc_rt_debug_host_fill, (void *)hdd_ctx);
	qdf_mc_timer_start(&hdd_ipa->rt_debug_fill_timer,
		HDD_IPA_UC_RT_DEBUG_FILL_INTERVAL);

	qdf_mc_timer_init(&hdd_ipa->rt_debug_timer, QDF_TIMER_TYPE_SW,
		hdd_ipa_uc_rt_debug_handler, (void *)hdd_ctx);
	qdf_mc_timer_start(&hdd_ipa->rt_debug_timer,
		HDD_IPA_UC_RT_DEBUG_PERIOD);

}

/**
 * hdd_ipa_dump_hdd_ipa() - dump entries in HDD IPA struct
 * @hdd_ipa: HDD IPA struct
 *
 * Dump entries in struct hdd_ipa
 *
 * Return: none
 */
static void hdd_ipa_dump_hdd_ipa(struct hdd_ipa_priv *hdd_ipa)
{
	int i;

	/* HDD IPA */
	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\n==== HDD IPA ====\n"
		"num_iface: %d\n"
		"rm_state: %d\n"
		"rm_lock: %pK\n"
		"uc_rm_work: %pK\n"
		"uc_op_work: %pK\n"
		"wake_lock: %pK\n"
		"wake_lock_work: %pK\n"
		"wake_lock_released: %d\n"
		"prod_client: %d\n"
		"tx_ref_cnt: %d\n"
		"pm_queue_head----\n"
		"\thead: %pK\n"
		"\ttail: %pK\n"
		"\tqlen: %d\n"
		"pm_work: %pK\n"
		"pm_lock: %pK\n"
		"suspended: %d\n",
		hdd_ipa->num_iface,
		hdd_ipa->rm_state,
		&hdd_ipa->rm_lock,
		&hdd_ipa->uc_rm_work,
		&hdd_ipa->uc_op_work,
		&hdd_ipa->wake_lock,
		&hdd_ipa->wake_lock_work,
		hdd_ipa->wake_lock_released,
		hdd_ipa->prod_client,
		hdd_ipa->tx_ref_cnt.counter,
		hdd_ipa->pm_queue_head.head,
		hdd_ipa->pm_queue_head.tail,
		hdd_ipa->pm_queue_head.qlen,
		&hdd_ipa->pm_work,
		&hdd_ipa->pm_lock,
		hdd_ipa->suspended);

	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\nq_lock: %pK\n"
		"pend_desc_head----\n"
		"\tnext: %pK\n"
		"\tprev: %pK\n"
		"hdd_ctx: %pK\n"
		"stats: %pK\n"
		"ipv4_notifier: %pK\n"
		"curr_prod_bw: %d\n"
		"curr_cons_bw: %d\n"
		"activated_fw_pipe: %d\n"
		"sap_num_connected_sta: %d\n"
		"sta_connected: %d\n",
		&hdd_ipa->q_lock,
		hdd_ipa->pend_desc_head.next,
		hdd_ipa->pend_desc_head.prev,
		hdd_ipa->hdd_ctx,
		&hdd_ipa->stats,
		&hdd_ipa->ipv4_notifier,
		hdd_ipa->curr_prod_bw,
		hdd_ipa->curr_cons_bw,
		hdd_ipa->activated_fw_pipe,
		hdd_ipa->sap_num_connected_sta,
		(unsigned int)hdd_ipa->sta_connected);

	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\ntx_pipe_handle: 0x%x\n"
		"rx_pipe_handle: 0x%x\n"
		"resource_loading: %d\n"
		"resource_unloading: %d\n"
		"pending_cons_req: %d\n"
		"pending_event----\n"
		"\tanchor.next: %pK\n"
		"\tanchor.prev: %pK\n"
		"\tcount: %d\n"
		"\tmax_size: %d\n"
		"event_lock: %pK\n"
		"ipa_tx_packets_diff: %d\n"
		"ipa_rx_packets_diff: %d\n"
		"ipa_p_tx_packets: %d\n"
		"ipa_p_rx_packets: %d\n"
		"stat_req_reason: %d\n",
		hdd_ipa->tx_pipe_handle,
		hdd_ipa->rx_pipe_handle,
		hdd_ipa->resource_loading,
		hdd_ipa->resource_unloading,
		hdd_ipa->pending_cons_req,
		hdd_ipa->pending_event.anchor.next,
		hdd_ipa->pending_event.anchor.prev,
		hdd_ipa->pending_event.count,
		hdd_ipa->pending_event.max_size,
		&hdd_ipa->event_lock,
		hdd_ipa->ipa_tx_packets_diff,
		hdd_ipa->ipa_rx_packets_diff,
		hdd_ipa->ipa_p_tx_packets,
		hdd_ipa->ipa_p_rx_packets,
		hdd_ipa->stat_req_reason);

	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\ncons_pipe_in----\n"
		"\tsys: %pK\n"
		"\tdl.comp_ring_base_pa: 0x%x\n"
		"\tdl.comp_ring_size: %d\n"
		"\tdl.ce_ring_base_pa: 0x%x\n"
		"\tdl.ce_door_bell_pa: 0x%x\n"
		"\tdl.ce_ring_size: %d\n"
		"\tdl.num_tx_buffers: %d\n"
		"prod_pipe_in----\n"
		"\tsys: %pK\n"
		"\tul.rdy_ring_base_pa: 0x%x\n"
		"\tul.rdy_ring_size: %d\n"
		"\tul.rdy_ring_rp_pa: 0x%x\n"
		"uc_loaded: %d\n"
		"wdi_enabled: %d\n"
		"rt_debug_fill_timer: %pK\n"
		"rt_debug_lock: %pK\n"
		"ipa_lock: %pK\n"
		"tx_comp_doorbell_dmaaddr: %pad\n"
		"rx_ready_doorbell_dmaaddr: %pad\n",
		&hdd_ipa->cons_pipe_in.sys,
		(unsigned int)hdd_ipa->cons_pipe_in.u.dl.comp_ring_base_pa,
		hdd_ipa->cons_pipe_in.u.dl.comp_ring_size,
		(unsigned int)hdd_ipa->cons_pipe_in.u.dl.ce_ring_base_pa,
		(unsigned int)hdd_ipa->cons_pipe_in.u.dl.ce_door_bell_pa,
		hdd_ipa->cons_pipe_in.u.dl.ce_ring_size,
		hdd_ipa->cons_pipe_in.u.dl.num_tx_buffers,
		&hdd_ipa->prod_pipe_in.sys,
		(unsigned int)hdd_ipa->prod_pipe_in.u.ul.rdy_ring_base_pa,
		hdd_ipa->prod_pipe_in.u.ul.rdy_ring_size,
		(unsigned int)hdd_ipa->prod_pipe_in.u.ul.rdy_ring_rp_pa,
		hdd_ipa->uc_loaded,
		hdd_ipa->wdi_enabled,
		&hdd_ipa->rt_debug_fill_timer,
		&hdd_ipa->rt_debug_lock,
		&hdd_ipa->ipa_lock,
		&hdd_ipa->tx_comp_doorbell_dmaaddr,
		&hdd_ipa->rx_ready_doorbell_dmaaddr);

	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\nvdev_to_iface----");
	for (i = 0; i < CSR_ROAM_SESSION_MAX; i++) {
		QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
			"\n\t[%d]=%d", i, hdd_ipa->vdev_to_iface[i]);
	}
	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\nvdev_offload_enabled----");
	for (i = 0; i < CSR_ROAM_SESSION_MAX; i++) {
		QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
			"\n\t[%d]=%d", i, hdd_ipa->vdev_offload_enabled[i]);
	}
	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\nassoc_stas_map ----");
	for (i = 0; i < WLAN_MAX_STA_COUNT; i++) {
		QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
			"\n\t[%d]: is_reserved=%d, sta_id=%d", i,
			hdd_ipa->assoc_stas_map[i].is_reserved,
			hdd_ipa->assoc_stas_map[i].sta_id);
	}
}

/**
 * hdd_ipa_dump_sys_pipe() - dump HDD IPA SYS Pipe struct
 * @hdd_ipa: HDD IPA struct
 *
 * Dump entire struct hdd_ipa_sys_pipe
 *
 * Return: none
 */
static void hdd_ipa_dump_sys_pipe(struct hdd_ipa_priv *hdd_ipa)
{
	int i;

	/* IPA SYS Pipes */
	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\n==== IPA SYS Pipes ====\n");

	for (i = 0; i < HDD_IPA_MAX_SYSBAM_PIPE; i++) {
		struct hdd_ipa_sys_pipe *sys_pipe;
		struct ipa_sys_connect_params *ipa_sys_params;

		sys_pipe = &hdd_ipa->sys_pipe[i];
		ipa_sys_params = &sys_pipe->ipa_sys_params;

		QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
			"\nsys_pipe[%d]----\n"
			"\tconn_hdl: 0x%x\n"
			"\tconn_hdl_valid: %d\n"
			"\tnat_en: %d\n"
			"\thdr_len %d\n"
			"\thdr_additional_const_len: %d\n"
			"\thdr_ofst_pkt_size_valid: %d\n"
			"\thdr_ofst_pkt_size: %d\n"
			"\thdr_little_endian: %d\n"
			"\tmode: %d\n"
			"\tclient: %d\n"
			"\tdesc_fifo_sz: %d\n"
			"\tpriv: %pK\n"
			"\tnotify: %pK\n"
			"\tskip_ep_cfg: %d\n"
			"\tkeep_ipa_awake: %d\n",
			i,
			sys_pipe->conn_hdl,
			sys_pipe->conn_hdl_valid,
			ipa_sys_params->ipa_ep_cfg.nat.nat_en,
			ipa_sys_params->ipa_ep_cfg.hdr.hdr_len,
			ipa_sys_params->ipa_ep_cfg.hdr.hdr_additional_const_len,
			ipa_sys_params->ipa_ep_cfg.hdr.hdr_ofst_pkt_size_valid,
			ipa_sys_params->ipa_ep_cfg.hdr.hdr_ofst_pkt_size,
			ipa_sys_params->ipa_ep_cfg.hdr_ext.hdr_little_endian,
			ipa_sys_params->ipa_ep_cfg.mode.mode,
			ipa_sys_params->client,
			ipa_sys_params->desc_fifo_sz,
			ipa_sys_params->priv,
			ipa_sys_params->notify,
			ipa_sys_params->skip_ep_cfg,
			ipa_sys_params->keep_ipa_awake);
	}
}

/**
 * hdd_ipa_dump_iface_context() - dump HDD IPA Interface Context struct
 * @hdd_ipa: HDD IPA struct
 *
 * Dump entire struct hdd_ipa_iface_context
 *
 * Return: none
 */
static void hdd_ipa_dump_iface_context(struct hdd_ipa_priv *hdd_ipa)
{
	int i;

	/* IPA Interface Contexts */
	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\n==== IPA Interface Contexts ====\n");

	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
		struct hdd_ipa_iface_context *iface_context;

		iface_context = &hdd_ipa->iface_context[i];

		QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
			"\niface_context[%d]----\n"
			"\thdd_ipa: %pK\n"
			"\tadapter: %pK\n"
			"\ttl_context: %pK\n"
			"\tcons_client: %d\n"
			"\tprod_client: %d\n"
			"\tiface_id: %d\n"
			"\tsta_id: %d\n"
			"\tinterface_lock: %pK\n"
			"\tifa_address: 0x%x\n",
			i,
			iface_context->hdd_ipa,
			iface_context->adapter,
			iface_context->tl_context,
			iface_context->cons_client,
			iface_context->prod_client,
			iface_context->iface_id,
			iface_context->sta_id,
			&iface_context->interface_lock,
			iface_context->ifa_address);
	}
}

/**
 * hdd_ipa_dump_info() - dump HDD IPA struct
 * @pHddCtx: hdd main context
 *
 * Dump entire struct hdd_ipa
 *
 * Return: none
 */
void hdd_ipa_dump_info(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa = (struct hdd_ipa_priv *)hdd_ctx->hdd_ipa;

	hdd_ipa_dump_hdd_ipa(hdd_ipa);
	hdd_ipa_dump_sys_pipe(hdd_ipa);
	hdd_ipa_dump_iface_context(hdd_ipa);
}

/**
 * __hdd_ipa_uc_stat_query() - Query the IPA stats
 * @hdd_ctx: Global HDD context
 * @ipa_tx_diff: tx packet count diff from previous tx packet count
 * @ipa_rx_diff: rx packet count diff from previous rx packet count
 *
 * Return: true if IPA is enabled, false otherwise
 */
static void __hdd_ipa_uc_stat_query(hdd_context_t *hdd_ctx,
	uint32_t *ipa_tx_diff, uint32_t *ipa_rx_diff)
{
	struct hdd_ipa_priv *hdd_ipa;

	*ipa_tx_diff = 0;
	*ipa_rx_diff = 0;

	if (wlan_hdd_validate_context(hdd_ctx))
		return;

	hdd_ipa = (struct hdd_ipa_priv *)hdd_ctx->hdd_ipa;

	if (!hdd_ipa_is_enabled(hdd_ctx) ||
	    !(hdd_ipa_uc_is_enabled(hdd_ctx))) {
		return;
	}

	qdf_mutex_acquire(&hdd_ipa->ipa_lock);
	if (hdd_ipa_is_fw_wdi_actived(hdd_ctx) &&
		(false == hdd_ipa->resource_loading)) {
		*ipa_tx_diff = hdd_ipa->ipa_tx_packets_diff;
		*ipa_rx_diff = hdd_ipa->ipa_rx_packets_diff;
		hdd_debug_ratelimited(HDD_IPA_UC_STAT_LOG_RATE,
				      "STAT Query TX DIFF %d, RX DIFF %d",
				      *ipa_tx_diff, *ipa_rx_diff);
	}
	qdf_mutex_release(&hdd_ipa->ipa_lock);
}

/**
 * hdd_ipa_uc_stat_query() - SSR wrapper for __hdd_ipa_uc_stat_query
 * @hdd_ctx: Global HDD context
 * @ipa_tx_diff: tx packet count diff from previous tx packet count
 * @ipa_rx_diff: rx packet count diff from previous rx packet count
 *
 * Return: true if IPA is enabled, false otherwise
 */
void hdd_ipa_uc_stat_query(hdd_context_t *hdd_ctx,
	uint32_t *ipa_tx_diff, uint32_t *ipa_rx_diff)
{
	cds_ssr_protect(__func__);
	__hdd_ipa_uc_stat_query(hdd_ctx, ipa_tx_diff, ipa_rx_diff);
	cds_ssr_unprotect(__func__);
}

/**
 * __hdd_ipa_uc_stat_request() - Get IPA stats from IPA.
 * @hdd_ctx: Global HDD context
 * @reason: STAT REQ Reason
 *
 * Return: None
 */
static void __hdd_ipa_uc_stat_request(hdd_context_t *hdd_ctx, uint8_t reason)
{
	struct hdd_ipa_priv *hdd_ipa;

	if (wlan_hdd_validate_context(hdd_ctx))
		return;

	hdd_ipa = (struct hdd_ipa_priv *)hdd_ctx->hdd_ipa;
	if (!hdd_ipa_is_enabled(hdd_ctx) ||
	    !(hdd_ipa_uc_is_enabled(hdd_ctx))) {
		return;
	}

	qdf_mutex_acquire(&hdd_ipa->ipa_lock);
	if (hdd_ipa_is_fw_wdi_actived(hdd_ctx) &&
		(false == hdd_ipa->resource_loading)) {
		hdd_ipa->stat_req_reason = reason;
		qdf_mutex_release(&hdd_ipa->ipa_lock);
		sme_ipa_uc_stat_request(hdd_ctx->hHal, 0,
			WMA_VDEV_TXRX_GET_IPA_UC_FW_STATS_CMDID,
			0, VDEV_CMD);
	} else {
		qdf_mutex_release(&hdd_ipa->ipa_lock);
	}
}

/**
 * hdd_ipa_uc_stat_request() - SSR wrapper for __hdd_ipa_uc_stat_request
 * @hdd_ctx: Global HDD context
 * @reason: STAT REQ Reason
 *
 * Return: None
 */
void hdd_ipa_uc_stat_request(hdd_context_t *hdd_ctx, uint8_t reason)
{
	cds_ssr_protect(__func__);
	__hdd_ipa_uc_stat_request(hdd_ctx, reason);
	cds_ssr_unprotect(__func__);
}

#ifdef FEATURE_METERING
/**
 * hdd_ipa_uc_sharing_stats_request() - Get IPA stats from IPA.
 * @adapter: network adapter
 * @reset_stats: reset stat countis after response
 *
 * Return: None
 */
void hdd_ipa_uc_sharing_stats_request(hdd_adapter_t *adapter,
				      uint8_t reset_stats)
{
	hdd_context_t *pHddCtx;
	struct hdd_ipa_priv *hdd_ipa;

	if (hdd_validate_adapter(adapter))
		return;

	pHddCtx = adapter->pHddCtx;
	hdd_ipa = pHddCtx->hdd_ipa;
	if (!hdd_ipa_is_enabled(pHddCtx) ||
		!(hdd_ipa_uc_is_enabled(pHddCtx))) {
		return;
	}

	qdf_mutex_acquire(&hdd_ipa->ipa_lock);
	if (false == hdd_ipa->resource_loading) {
		qdf_mutex_release(&hdd_ipa->ipa_lock);
		wma_cli_set_command(
			(int)adapter->sessionId,
			(int)WMA_VDEV_TXRX_GET_IPA_UC_SHARING_STATS_CMDID,
			reset_stats, VDEV_CMD);
	} else {
		qdf_mutex_release(&hdd_ipa->ipa_lock);
	}
}

/**
 * hdd_ipa_uc_set_quota() - Set quota limit bytes from IPA.
 * @adapter: network adapter
 * @set_quota: when 1, FW starts quota monitoring
 * @quota_bytes: quota limit in bytes
 *
 * Return: None
 */
void hdd_ipa_uc_set_quota(hdd_adapter_t *adapter, uint8_t set_quota,
			  uint64_t quota_bytes)
{
	hdd_context_t *pHddCtx;
	struct hdd_ipa_priv *hdd_ipa;

	if (hdd_validate_adapter(adapter))
		return;

	pHddCtx = adapter->pHddCtx;
	hdd_ipa = pHddCtx->hdd_ipa;
	if (!hdd_ipa_is_enabled(pHddCtx) ||
		!(hdd_ipa_uc_is_enabled(pHddCtx))) {
		return;
	}

	HDD_IPA_LOG(LOG1, "SET_QUOTA: set_quota=%d, quota_bytes=%llu",
		    set_quota, quota_bytes);

	qdf_mutex_acquire(&hdd_ipa->ipa_lock);
	if (false == hdd_ipa->resource_loading) {
		qdf_mutex_release(&hdd_ipa->ipa_lock);
		wma_cli_set2_command(
			(int)adapter->sessionId,
			(int)WMA_VDEV_TXRX_SET_IPA_UC_QUOTA_CMDID,
			(set_quota ? quota_bytes&0xffffffff : 0),
			(set_quota ? quota_bytes>>32 : 0),
			VDEV_CMD);
	} else {
		qdf_mutex_release(&hdd_ipa->ipa_lock);
	}
}
#endif

/**
 * hdd_ipa_uc_find_add_assoc_sta() - Find associated station
 * @hdd_ipa: Global HDD IPA context
 * @sta_add: Should station be added
 * @sta_id: ID of the station being queried
 *
 * Return: true if the station was found
 */
static bool hdd_ipa_uc_find_add_assoc_sta(struct hdd_ipa_priv *hdd_ipa,
					  bool sta_add, uint8_t sta_id)
{
	bool sta_found = false;
	uint8_t idx;

	for (idx = 0; idx < WLAN_MAX_STA_COUNT; idx++) {
		if ((hdd_ipa->assoc_stas_map[idx].is_reserved) &&
		    (hdd_ipa->assoc_stas_map[idx].sta_id == sta_id)) {
			sta_found = true;
			break;
		}
	}
	if (sta_add && sta_found) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "STA ID %d already exist, cannot add",
			     sta_id);
		return sta_found;
	}
	if (sta_add) {
		for (idx = 0; idx < WLAN_MAX_STA_COUNT; idx++) {
			if (!hdd_ipa->assoc_stas_map[idx].is_reserved) {
				hdd_ipa->assoc_stas_map[idx].is_reserved = true;
				hdd_ipa->assoc_stas_map[idx].sta_id = sta_id;
				return sta_found;
			}
		}
	}
	if (!sta_add && !sta_found) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "STA ID %d does not exist, cannot delete",
			     sta_id);
		return sta_found;
	}
	if (!sta_add) {
		for (idx = 0; idx < WLAN_MAX_STA_COUNT; idx++) {
			if ((hdd_ipa->assoc_stas_map[idx].is_reserved) &&
			    (hdd_ipa->assoc_stas_map[idx].sta_id == sta_id)) {
				hdd_ipa->assoc_stas_map[idx].is_reserved =
					false;
				hdd_ipa->assoc_stas_map[idx].sta_id = 0xFF;
				return sta_found;
			}
		}
	}
	return sta_found;
}

/**
 * hdd_ipa_uc_enable_pipes() - Enable IPA uC pipes
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: 0 on success, negative errno if error
 */
static int hdd_ipa_uc_enable_pipes(struct hdd_ipa_priv *hdd_ipa)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	int result = 0;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");
	if (qdf_unlikely(NULL == pdev)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "pdev is NULL");
		result = QDF_STATUS_E_FAILURE;
		goto end;
	}

	if (!hdd_ipa->ipa_pipes_down) {
		/*
		 * This shouldn't happen :
		 * IPA WDI Pipes are already activated
		 */
		WARN_ON(1);
		HDD_IPA_LOG(QDF_TRACE_LEVEL_WARN,
			"IPA WDI Pipes are already activated");
		goto end;
	}

	result = hdd_ipa_wdi_enable_pipes(hdd_ipa);
	if (result) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"Enable IPA WDI pipes failed ret=%d", result);
		goto end;
	}

	ol_txrx_ipa_uc_set_active(pdev, true, true);
	ol_txrx_ipa_uc_set_active(pdev, true, false);

	INIT_COMPLETION(hdd_ipa->ipa_resource_comp);
	hdd_ipa->ipa_pipes_down = false;

end:
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit: ipa_pipes_down=%d",
		    hdd_ipa->ipa_pipes_down);
	return result;
}

/**
 * hdd_ipa_uc_disable_pipes() - Disable IPA uC pipes
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: 0 on success, negative errno if error
 */
static int hdd_ipa_uc_disable_pipes(struct hdd_ipa_priv *hdd_ipa)
{
	int result = 0;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");

	if (hdd_ipa->ipa_pipes_down) {
		/*
		 * This shouldn't happen :
		 * IPA WDI Pipes are already deactivated
		 */
		WARN_ON(1);
		HDD_IPA_LOG(QDF_TRACE_LEVEL_WARN,
			"IPA WDI Pipes are already deactivated");
		goto end;
	}

	result = hdd_ipa_wdi_disable_pipes(hdd_ipa);
	if (result) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"Disable IPA WDI pipes failed ret=%d", result);
		goto end;
	}

	hdd_ipa->ipa_pipes_down = true;

end:
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit: ipa_pipes_down=%d",
		    hdd_ipa->ipa_pipes_down);
	return result;
}

/**
 * hdd_ipa_uc_handle_first_con() - Handle first uC IPA connection
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: 0 on success, negative errno if error
 */
static int hdd_ipa_uc_handle_first_con(struct hdd_ipa_priv *hdd_ipa)
{
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");

	hdd_ipa->activated_fw_pipe = 0;
	hdd_ipa->resource_loading = true;

	/* If RM feature enabled
	 * Request PROD Resource first
	 * PROD resource may return sync or async manners
	 */
	if (hdd_ipa_is_rm_enabled(hdd_ipa->hdd_ctx)) {
		if (!hdd_ipa_wdi_rm_request_resource(hdd_ipa,
					IPA_RM_RESOURCE_WLAN_PROD)) {
			/* RM PROD request sync return
			 * enable pipe immediately
			 */
			if (hdd_ipa_uc_enable_pipes(hdd_ipa)) {
				HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
					"IPA WDI Pipe activation failed");
				hdd_ipa->resource_loading = false;
				return -EBUSY;
			}
		} else {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
				    "IPA WDI Pipe activation deferred");
		}
	} else {
		/* RM Disabled
		 * Just enabled all the PIPEs
		 */
		if (hdd_ipa_uc_enable_pipes(hdd_ipa)) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "IPA WDI Pipe activation failed");
			hdd_ipa->resource_loading = false;
			return -EBUSY;
		}
		hdd_ipa->resource_loading = false;
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit: IPA WDI Pipes activated!");
	return 0;
}

/**
 * hdd_ipa_uc_handle_last_discon() - Handle last uC IPA disconnection
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: None
 */
static void hdd_ipa_uc_handle_last_discon(struct hdd_ipa_priv *hdd_ipa)
{
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");

	if (!pdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "txrx context is NULL");
		QDF_ASSERT(0);
		return;
	}

	hdd_ipa->resource_unloading = true;
	INIT_COMPLETION(hdd_ipa->ipa_resource_comp);
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "Disable FW RX PIPE");
	ol_txrx_ipa_uc_set_active(pdev, false, false);

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit: IPA WDI Pipes deactivated");
}


/**
 * hdd_ipa_uc_op_metering() - IPA uC operation for stats and quota limit
 * @hdd_ctx: Global HDD context
 * @op_msg: operation message received from firmware
 *
 * Return: QDF_STATUS enumeration
 */
#ifdef FEATURE_METERING
static QDF_STATUS hdd_ipa_uc_op_metering(hdd_context_t *hdd_ctx,
					 struct op_msg_type *op_msg)
{
	struct op_msg_type *msg = op_msg;
	struct ipa_uc_sharing_stats *uc_sharing_stats;
	struct ipa_uc_quota_rsp *uc_quota_rsp;
	struct ipa_uc_quota_ind *uc_quota_ind;
	struct hdd_ipa_priv *hdd_ipa;
	hdd_adapter_t *adapter;

	hdd_ipa = (struct hdd_ipa_priv *)hdd_ctx->hdd_ipa;

	if (HDD_IPA_UC_OPCODE_SHARING_STATS == msg->op_code) {
		/* fill-up ipa_uc_sharing_stats structure from FW */
		uc_sharing_stats = (struct ipa_uc_sharing_stats *)
			     ((uint8_t *)op_msg + sizeof(struct op_msg_type));

		memcpy(&(hdd_ipa->ipa_sharing_stats), uc_sharing_stats,
		       sizeof(struct ipa_uc_sharing_stats));

		complete(&hdd_ipa->ipa_uc_sharing_stats_comp);

		HDD_IPA_DP_LOG(QDF_TRACE_LEVEL_DEBUG,
			       "%s: %llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
			       "HDD_IPA_UC_OPCODE_SHARING_STATS",
			       hdd_ipa->ipa_sharing_stats.ipv4_rx_packets,
			       hdd_ipa->ipa_sharing_stats.ipv4_rx_bytes,
			       hdd_ipa->ipa_sharing_stats.ipv6_rx_packets,
			       hdd_ipa->ipa_sharing_stats.ipv6_rx_bytes,
			       hdd_ipa->ipa_sharing_stats.ipv4_tx_packets,
			       hdd_ipa->ipa_sharing_stats.ipv4_tx_bytes,
			       hdd_ipa->ipa_sharing_stats.ipv6_tx_packets,
			       hdd_ipa->ipa_sharing_stats.ipv6_tx_bytes);
	} else if (HDD_IPA_UC_OPCODE_QUOTA_RSP == msg->op_code) {
		/* received set quota response */
		uc_quota_rsp = (struct ipa_uc_quota_rsp *)
			     ((uint8_t *)op_msg + sizeof(struct op_msg_type));

		memcpy(&(hdd_ipa->ipa_quota_rsp), uc_quota_rsp,
			   sizeof(struct ipa_uc_quota_rsp));

		complete(&hdd_ipa->ipa_uc_set_quota_comp);
		HDD_IPA_DP_LOG(QDF_TRACE_LEVEL_DEBUG,
			      "%s: success=%d, quota_bytes=%llu",
			      "HDD_IPA_UC_OPCODE_QUOTA_RSP",
			      hdd_ipa->ipa_quota_rsp.success,
			      ((uint64_t)(hdd_ipa->ipa_quota_rsp.quota_hi)<<32)|
			      hdd_ipa->ipa_quota_rsp.quota_lo);
	} else if (HDD_IPA_UC_OPCODE_QUOTA_IND == msg->op_code) {
		/* hit quota limit */
		uc_quota_ind = (struct ipa_uc_quota_ind *)
			     ((uint8_t *)op_msg + sizeof(struct op_msg_type));

		hdd_ipa->ipa_quota_ind.quota_bytes =
					uc_quota_ind->quota_bytes;

		/* send quota exceeded indication to IPA */
		HDD_IPA_DP_LOG(QDF_TRACE_LEVEL_DEBUG,
			"OPCODE_QUOTA_IND: quota exceed! (quota_bytes=%llu)",
			hdd_ipa->ipa_quota_ind.quota_bytes);

		adapter = hdd_get_adapter(hdd_ipa->hdd_ctx, QDF_STA_MODE);
		if (adapter)
			ipa_broadcast_wdi_quota_reach_ind(
						adapter->dev->ifindex,
						uc_quota_ind->quota_bytes);
		else
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
					"Failed quota_reach_ind: NULL adapter");
	} else {
		return QDF_STATUS_E_INVAL;
	}

	return QDF_STATUS_SUCCESS;
}
#else
static QDF_STATUS hdd_ipa_uc_op_metering(hdd_context_t *hdd_ctx,
					 struct op_msg_type *op_msg)
{
	return QDF_STATUS_E_INVAL;
}
#endif

/**
 * hdd_ipa_uc_loaded_handler() - Process IPA uC loaded indication
 * @ipa_ctxt: hdd ipa local context
 *
 * Will handle IPA UC image loaded indication comes from IPA kernel
 *
 * Return: None
 */
static void hdd_ipa_uc_loaded_handler(struct hdd_ipa_priv *ipa_ctxt)
{
	struct ol_txrx_ipa_resources *ipa_res = &ipa_ctxt->ipa_resource;
	qdf_device_t osdev = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);
	struct ol_txrx_pdev_t *pdev;
	int ret;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO, "UC READY");
	if (true == ipa_ctxt->uc_loaded) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "UC already loaded");
		return;
	}

	ipa_ctxt->uc_loaded = true;

	if (!osdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL, "invalid qdf dev context");
		return;
	}

	pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	if (!pdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL, "invalid txrx context");
		return;
	}

	/* Setup IPA sys_pipe for MCC */
	if (hdd_ipa_uc_sta_is_enabled(ipa_ctxt->hdd_ctx)) {
		ret = hdd_ipa_setup_sys_pipe(ipa_ctxt);
		if (ret) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "ipa sys pipes setup failed ret=%d", ret);
			return;
		}

		INIT_WORK(&ipa_ctxt->mcc_work,
				hdd_ipa_mcc_work_handler);
	}

	/* Connect pipe */
	ret = hdd_ipa_wdi_conn_pipes(ipa_ctxt, ipa_res);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"ipa wdi conn pipes failed ret=%d", ret);
		return;
	}

	if (hdd_ipa_init_perf_level(ipa_ctxt->hdd_ctx) != QDF_STATUS_SUCCESS)
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"Failed to init perf level");

	/* If already any STA connected, enable IPA/FW PIPEs */
	if (ipa_ctxt->sap_num_connected_sta) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
			"Client already connected, enable IPA/FW PIPEs");
		hdd_ipa_uc_handle_first_con(ipa_ctxt);
	}
}

/**
 * hdd_ipa_wlan_event_to_str() - convert IPA WLAN event to string
 * @event: IPA WLAN event to be converted to a string
 *
 * Return: ASCII string representing the IPA WLAN event
 */
static inline char *hdd_ipa_wlan_event_to_str(enum ipa_wlan_event event)
{
	switch (event) {
	CASE_RETURN_STRING(WLAN_CLIENT_CONNECT);
	CASE_RETURN_STRING(WLAN_CLIENT_DISCONNECT);
	CASE_RETURN_STRING(WLAN_CLIENT_POWER_SAVE_MODE);
	CASE_RETURN_STRING(WLAN_CLIENT_NORMAL_MODE);
	CASE_RETURN_STRING(SW_ROUTING_ENABLE);
	CASE_RETURN_STRING(SW_ROUTING_DISABLE);
	CASE_RETURN_STRING(WLAN_AP_CONNECT);
	CASE_RETURN_STRING(WLAN_AP_DISCONNECT);
	CASE_RETURN_STRING(WLAN_STA_CONNECT);
	CASE_RETURN_STRING(WLAN_STA_DISCONNECT);
	CASE_RETURN_STRING(WLAN_CLIENT_CONNECT_EX);
	default:
		return "UNKNOWN";
	}
}

/**
 * hdd_ipa_print_resource_info - Print IPA resource info
 * @hdd_ipa: HDD IPA local context
 *
 * Return: None
 */
static void hdd_ipa_print_resource_info(struct hdd_ipa_priv *hdd_ipa)
{
	qdf_device_t osdev = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);
	struct ol_txrx_ipa_resources *res = &hdd_ipa->ipa_resource;

	if (!osdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL,
			    "qdf dev context is NULL");
		return;
	}

	if (IPA_RESOURCE_READY(res, osdev)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL,
			"IPA UC resource is not ready yet");
		return;
	}

	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\n==== IPA RESOURCE INFO ====\n"
		"CE RING SIZE: %d\n"
		"TX COMP RING SIZE: %d\n"
		"TX NUM ALLOC BUF: %d\n"
		"RX IND RING SIZE: %d\n"
#if defined(QCA_WIFI_3_0) && defined(CONFIG_IPA3)
		"RX2 IND RING SIZE: %d\n"
#endif
		"PROD CLIENT: %d\n"
		"TX PIPE HDL: 0x%x\n"
		"RX PIPE HDL: 0x%x\n",
		(int)res->ce_sr->mem_info.size,
		(int)res->tx_comp_ring->mem_info.size,
		res->tx_num_alloc_buffer,
		(int)res->rx_rdy_ring->mem_info.size,
#if defined(QCA_WIFI_3_0) && defined(CONFIG_IPA3)
		(int)res->rx2_rdy_ring->mem_info.size,
#endif
		hdd_ipa->prod_client,
		hdd_ipa->tx_pipe_handle,
		hdd_ipa->rx_pipe_handle);
}

/**
 * hdd_ipa_print_session_info - Print IPA session info
 * @hdd_ipa: HDD IPA local context
 *
 * Return: None
 */
static void hdd_ipa_print_session_info(struct hdd_ipa_priv *hdd_ipa)
{
	uint8_t session_id;
	int device_mode;
	struct ipa_uc_pending_event *event = NULL, *next = NULL;
	struct hdd_ipa_iface_context *iface_context = NULL;
	int i;

	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\n==== IPA SESSION INFO ====\n"
		"NUM IFACE: %d\n"
		"RM STATE: %d\n"
		"ACTIVATED FW PIPE: %d\n"
		"SAP NUM STAs: %d\n"
		"STA CONNECTED: %d\n"
		"CONCURRENT MODE: %s\n"
		"RSC LOADING: %d\n"
		"RSC UNLOADING: %d\n"
		"PENDING CONS REQ: %d\n"
		"IPA PIPES DOWN: %d\n"
		"IPA UC LOADED: %d\n"
		"IPA WDI ENABLED: %d\n"
		"NUM SEND MSG: %d\n"
		"NUM FREE MSG: %d\n",
		hdd_ipa->num_iface,
		hdd_ipa->rm_state,
		hdd_ipa->activated_fw_pipe,
		hdd_ipa->sap_num_connected_sta,
		hdd_ipa->sta_connected,
		(hdd_ipa->hdd_ctx->mcc_mode ? "MCC" : "SCC"),
		hdd_ipa->resource_loading,
		hdd_ipa->resource_unloading,
		hdd_ipa->pending_cons_req,
		hdd_ipa->ipa_pipes_down,
		hdd_ipa->uc_loaded,
		hdd_ipa->wdi_enabled,
		(unsigned int)hdd_ipa->stats.num_send_msg,
		(unsigned int)hdd_ipa->stats.num_free_msg);

	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
		iface_context = &hdd_ipa->iface_context[i];
		if (!iface_context || !iface_context->adapter)
			continue;

		session_id = iface_context->adapter->sessionId;
		if (session_id >= CSR_ROAM_SESSION_MAX)
			continue;

		device_mode = iface_context->adapter->device_mode;
		QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
			"\nIFACE[%d]: session:%d, sta_id:%d, mode:%s, offload:%d",
			i, session_id,
			iface_context->sta_id,
			hdd_device_mode_to_string(device_mode),
			hdd_ipa->vdev_offload_enabled[session_id]);
	}

	for (i = 0; i < IPA_WLAN_EVENT_MAX; i++)
		QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
			"\nEVENT[%d]=%d",
			i, hdd_ipa->stats.event[i]);

	i = 0;
	qdf_list_peek_front(&hdd_ipa->pending_event,
			    (qdf_list_node_t **)&event);
	while (event) {
		QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
			"\nPENDING EVENT[%d]: DEV:%s, EVT:%s, sta_id:%d, MAC:%pM",
			i, event->adapter->dev->name,
			hdd_ipa_wlan_event_to_str(event->type),
			event->sta_id, event->mac_addr);

		qdf_list_peek_next(&hdd_ipa->pending_event,
			(qdf_list_node_t *)event, (qdf_list_node_t **)&next);
		event = next;
		next = NULL;
		i++;
	}
}

/**
 * hdd_ipa_print_txrx_stats - Print HDD IPA TX/RX stats
 * @hdd_ipa: HDD IPA local context
 *
 * Return: None
 */
static void hdd_ipa_print_txrx_stats(struct hdd_ipa_priv *hdd_ipa)
{
	int i;
	struct hdd_ipa_iface_context *iface_context = NULL;

	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\n==== HDD IPA TX/RX STATS ====\n"
		"NUM RM GRANT: %llu\n"
		"NUM RM RELEASE: %llu\n"
		"NUM RM GRANT IMM: %llu\n"
		"NUM CONS PERF REQ: %llu\n"
		"NUM PROD PERF REQ: %llu\n"
		"NUM RX DROP: %llu\n"
		"NUM EXCP PKT: %llu\n"
		"NUM TX FWD OK: %llu\n"
		"NUM TX FWD ERR: %llu\n"
		"NUM TX DESC Q CNT: %llu\n"
		"NUM TX DESC ERROR: %llu\n"
		"NUM TX COMP CNT: %llu\n"
		"NUM TX QUEUED: %llu\n"
		"NUM TX DEQUEUED: %llu\n"
		"NUM MAX PM QUEUE: %llu\n"
		"TX REF CNT: %d\n"
		"SUSPENDED: %d\n"
		"PEND DESC HEAD: %pK\n"
		"TX DESC SIZE: %d\n"
		"TX DESC LIST: %pK\n"
		"FREE TX DESC HEAD: %pK\n",
		hdd_ipa->stats.num_rm_grant,
		hdd_ipa->stats.num_rm_release,
		hdd_ipa->stats.num_rm_grant_imm,
		hdd_ipa->stats.num_cons_perf_req,
		hdd_ipa->stats.num_prod_perf_req,
		hdd_ipa->stats.num_rx_drop,
		hdd_ipa->stats.num_rx_excep,
		hdd_ipa->stats.num_tx_fwd_ok,
		hdd_ipa->stats.num_tx_fwd_err,
		hdd_ipa->stats.num_tx_desc_q_cnt,
		hdd_ipa->stats.num_tx_desc_error,
		hdd_ipa->stats.num_tx_comp_cnt,
		hdd_ipa->stats.num_tx_queued,
		hdd_ipa->stats.num_tx_dequeued,
		hdd_ipa->stats.num_max_pm_queue,
		hdd_ipa->tx_ref_cnt.counter,
		hdd_ipa->suspended,
		&hdd_ipa->pend_desc_head,
		hdd_ipa->tx_desc_size,
		hdd_ipa->tx_desc_list,
		&hdd_ipa->free_tx_desc_head);

	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
		iface_context = &hdd_ipa->iface_context[i];
		if (!iface_context || !iface_context->adapter)
			continue;

		QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
			"IFACE[%d]: TX:%llu, TX DROP:%llu, TX ERR:%llu, TX CAC DROP:%llu, RX IPA EXCEP:%llu",
			i,
			iface_context->stats.num_tx,
			iface_context->stats.num_tx_drop,
			iface_context->stats.num_tx_err,
			iface_context->stats.num_tx_cac_drop,
			iface_context->stats.num_rx_ipa_excep);
	}
}

/**
 * hdd_ipa_print_fw_wdi_stats - Print WLAN FW WDI stats
 * @hdd_ipa: HDD IPA local context
 *
 * Return: None
 */
static void hdd_ipa_print_fw_wdi_stats(struct hdd_ipa_priv *hdd_ipa,
				       struct ipa_uc_fw_stats *uc_fw_stat)
{
	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\n==== WLAN FW WDI TX STATS ====\n"
		"COMP RING SIZE: %d\n"
		"COMP RING DBELL IND VAL : %d\n"
		"COMP RING DBELL CACHED VAL : %d\n"
		"PKTS ENQ : %d\n"
		"PKTS COMP : %d\n"
		"IS SUSPEND : %d\n",
		uc_fw_stat->tx_comp_ring_size,
		uc_fw_stat->tx_comp_ring_dbell_ind_val,
		uc_fw_stat->tx_comp_ring_dbell_cached_val,
		uc_fw_stat->tx_pkts_enqueued,
		uc_fw_stat->tx_pkts_completed,
		uc_fw_stat->tx_is_suspend);

	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\n==== WLAN FW WDI RX STATS ====\n"
		"IND RING SIZE: %d\n"
		"IND RING DBELL IND VAL : %d\n"
		"IND RING DBELL CACHED VAL : %d\n"
		"RDY IND CACHE VAL : %d\n"
		"RFIL IND : %d\n"
		"NUM PKT INDICAT : %d\n"
		"BUF REFIL : %d\n"
		"NUM DROP NO SPC : %d\n"
		"NUM DROP NO BUF : %d\n"
		"IS SUSPND : %d\n",
		uc_fw_stat->rx_ind_ring_size,
		uc_fw_stat->rx_ind_ring_dbell_ind_val,
		uc_fw_stat->rx_ind_ring_dbell_ind_cached_val,
		uc_fw_stat->rx_ind_ring_rd_idx_cached_val,
		uc_fw_stat->rx_refill_idx,
		uc_fw_stat->rx_num_pkts_indicated,
		uc_fw_stat->rx_buf_refilled,
		uc_fw_stat->rx_num_ind_drop_no_space,
		uc_fw_stat->rx_num_ind_drop_no_buf,
		uc_fw_stat->rx_is_suspend);
}

/**
 * hdd_ipa_print_ipa_wdi_stats - Print IPA WDI stats
 * @hdd_ipa: HDD IPA local context
 *
 * Return: None
 */
static void hdd_ipa_print_ipa_wdi_stats(struct hdd_ipa_priv *hdd_ipa)
{
	struct IpaHwStatsWDIInfoData_t ipa_stat;

	ipa_get_wdi_stats(&ipa_stat);

	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\n==== IPA WDI TX STATS ====\n"
		"NUM PROCD : %d\n"
		"CE DBELL : 0x%x\n"
		"NUM DBELL FIRED : %d\n"
		"COMP RNG FULL : %d\n"
		"COMP RNG EMPT : %d\n"
		"COMP RNG USE HGH : %d\n"
		"COMP RNG USE LOW : %d\n"
		"BAM FIFO FULL : %d\n"
		"BAM FIFO EMPT : %d\n"
		"BAM FIFO USE HGH : %d\n"
		"BAM FIFO USE LOW : %d\n"
		"NUM DBELL : %d\n"
		"NUM UNEXP DBELL : %d\n"
		"NUM BAM INT HDL : 0x%x\n"
		"NUM BAM INT NON-RUN : 0x%x\n"
		"NUM QMB INT HDL : 0x%x\n",
		ipa_stat.tx_ch_stats.num_pkts_processed,
		ipa_stat.tx_ch_stats.copy_engine_doorbell_value,
		ipa_stat.tx_ch_stats.num_db_fired,
		ipa_stat.tx_ch_stats.tx_comp_ring_stats.ringFull,
		ipa_stat.tx_ch_stats.tx_comp_ring_stats.ringEmpty,
		ipa_stat.tx_ch_stats.tx_comp_ring_stats.ringUsageHigh,
		ipa_stat.tx_ch_stats.tx_comp_ring_stats.ringUsageLow,
		ipa_stat.tx_ch_stats.bam_stats.bamFifoFull,
		ipa_stat.tx_ch_stats.bam_stats.bamFifoEmpty,
		ipa_stat.tx_ch_stats.bam_stats.bamFifoUsageHigh,
		ipa_stat.tx_ch_stats.bam_stats.bamFifoUsageLow,
		ipa_stat.tx_ch_stats.num_db,
		ipa_stat.tx_ch_stats.num_unexpected_db,
		ipa_stat.tx_ch_stats.num_bam_int_handled,
		ipa_stat.tx_ch_stats.
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0))
			num_bam_int_in_non_running_state,
#else
			num_bam_int_in_non_runnning_state,
#endif
		ipa_stat.tx_ch_stats.num_qmb_int_handled);

	QDF_TRACE(QDF_MODULE_ID_HDD, QDF_TRACE_LEVEL_INFO,
		"\n==== IPA WDI RX STATS ====\n"
		"MAX OST PKT : %d\n"
		"NUM PKT PRCSD : %d\n"
		"RNG RP : 0x%x\n"
		"IND RNG FULL : %d\n"
		"IND RNG EMPT : %d\n"
		"IND RNG USE HGH : %d\n"
		"IND RNG USE LOW : %d\n"
		"BAM FIFO FULL : %d\n"
		"BAM FIFO EMPT : %d\n"
		"BAM FIFO USE HGH : %d\n"
		"BAM FIFO USE LOW : %d\n"
		"NUM DB : %d\n"
		"NUM UNEXP DB : %d\n"
		"NUM BAM INT HNDL : 0x%x\n",
		ipa_stat.rx_ch_stats.max_outstanding_pkts,
		ipa_stat.rx_ch_stats.num_pkts_processed,
		ipa_stat.rx_ch_stats.rx_ring_rp_value,
		ipa_stat.rx_ch_stats.rx_ind_ring_stats.ringFull,
		ipa_stat.rx_ch_stats.rx_ind_ring_stats.ringEmpty,
		ipa_stat.rx_ch_stats.rx_ind_ring_stats.ringUsageHigh,
		ipa_stat.rx_ch_stats.rx_ind_ring_stats.ringUsageLow,
		ipa_stat.rx_ch_stats.bam_stats.bamFifoFull,
		ipa_stat.rx_ch_stats.bam_stats.bamFifoEmpty,
		ipa_stat.rx_ch_stats.bam_stats.bamFifoUsageHigh,
		ipa_stat.rx_ch_stats.bam_stats.bamFifoUsageLow,
		ipa_stat.rx_ch_stats.num_db,
		ipa_stat.rx_ch_stats.num_unexpected_db,
		ipa_stat.rx_ch_stats.num_bam_int_handled);
}

/**
 * hdd_ipa_uc_info() - Print IPA uC resource and session information
 * @adapter: network adapter
 *
 * Return: None
 */
void hdd_ipa_uc_info(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa;

	hdd_ipa = hdd_ctx->hdd_ipa;

	if (!hdd_ipa) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"HDD IPA context is NULL");
		return;
	}

	/* IPA resource info */
	hdd_ipa_print_resource_info(hdd_ipa);
	/* IPA session info */
	hdd_ipa_print_session_info(hdd_ipa);
}

/**
 * hdd_ipa_uc_stat() - Print IPA uC stats
 * @adapter: network adapter
 *
 * Return: None
 */
void hdd_ipa_uc_stat(hdd_adapter_t *adapter)
{
	hdd_context_t *hdd_ctx;
	struct hdd_ipa_priv *hdd_ipa;

	hdd_ctx = WLAN_HDD_GET_CTX(adapter);
	hdd_ipa = hdd_ctx->hdd_ipa;

	if (!hdd_ipa) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"HDD IPA context is NULL");
		return;
	}

	/* HDD IPA TX/RX stats */
	hdd_ipa_print_txrx_stats(hdd_ipa);
	/* IPA WDI stats */
	hdd_ipa_print_ipa_wdi_stats(hdd_ipa);
	/* WLAN FW WDI stats */
	hdd_ipa_uc_stat_request(hdd_ctx, HDD_IPA_UC_STAT_REASON_DEBUG);
}

/**
 * hdd_ipa_uc_op_cb() - IPA uC operation callback
 * @op_msg: operation message received from firmware
 * @usr_ctxt: user context registered with TL (we register the HDD Global
 *	context)
 *
 * Return: None
 */
static void hdd_ipa_uc_op_cb(struct op_msg_type *op_msg, void *usr_ctxt)
{
	struct op_msg_type *msg = op_msg;
	struct ipa_uc_fw_stats *uc_fw_stat;
	struct hdd_ipa_priv *hdd_ipa;
	hdd_context_t *hdd_ctx;
	struct ol_txrx_pdev_t *pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!op_msg) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "INVALID ARG");
		return;
	}

	if (!pdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL, "pdev is NULL");
		qdf_mem_free(op_msg);
		return;
	}

	if (HDD_IPA_UC_OPCODE_MAX <= msg->op_code) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "INVALID OPCODE %d",  msg->op_code);
		qdf_mem_free(op_msg);
		return;
	}

	hdd_ctx = (hdd_context_t *) usr_ctxt;

	/*
	 * When SSR is going on or driver is unloading, just return.
	 */
	status = wlan_hdd_validate_context(hdd_ctx);
	if (status) {
		qdf_mem_free(op_msg);
		return;
	}

	hdd_ipa = (struct hdd_ipa_priv *)hdd_ctx->hdd_ipa;

	HDD_IPA_DP_LOG(QDF_TRACE_LEVEL_DEBUG,
		       "OPCODE=%d", msg->op_code);

	if ((HDD_IPA_UC_OPCODE_TX_RESUME == msg->op_code) ||
	    (HDD_IPA_UC_OPCODE_RX_RESUME == msg->op_code)) {
		qdf_mutex_acquire(&hdd_ipa->ipa_lock);
		hdd_ipa->activated_fw_pipe++;
		if (hdd_ipa_is_fw_wdi_actived(hdd_ctx)) {
			hdd_ipa->resource_loading = false;
			complete(&hdd_ipa->ipa_resource_comp);
			if (hdd_ipa->wdi_enabled == false) {
				hdd_ipa->wdi_enabled = true;
				if (hdd_ipa_uc_send_wdi_control_msg(true) == 0)
					hdd_ipa_send_mcc_scc_msg(hdd_ctx,
							 hdd_ctx->mcc_mode);
			}
			hdd_ipa_uc_proc_pending_event(hdd_ipa, true);
			if (hdd_ipa->pending_cons_req)
				hdd_ipa_wdi_rm_notify_completion(
						IPA_RM_RESOURCE_GRANTED,
						IPA_RM_RESOURCE_WLAN_CONS);
			hdd_ipa->pending_cons_req = false;
		}
		qdf_mutex_release(&hdd_ipa->ipa_lock);
	} else if ((HDD_IPA_UC_OPCODE_TX_SUSPEND == msg->op_code) ||
		   (HDD_IPA_UC_OPCODE_RX_SUSPEND == msg->op_code)) {
		qdf_mutex_acquire(&hdd_ipa->ipa_lock);

		if (HDD_IPA_UC_OPCODE_RX_SUSPEND == msg->op_code) {
			hdd_ipa_uc_disable_pipes(hdd_ipa);
			HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
					"Disable FW TX PIPE");
			ol_txrx_ipa_uc_set_active(pdev, false, true);
		}

		hdd_ipa->activated_fw_pipe--;
		if (!hdd_ipa->activated_fw_pipe) {
			/*
			 * Async return success from FW
			 * Disable/suspend all the PIPEs
			 */
			hdd_ipa->resource_unloading = false;
			complete(&hdd_ipa->ipa_resource_comp);
			if (hdd_ipa_is_rm_enabled(hdd_ipa->hdd_ctx))
				hdd_ipa_wdi_rm_release_resource(hdd_ipa,
					IPA_RM_RESOURCE_WLAN_PROD);
			hdd_ipa_uc_proc_pending_event(hdd_ipa, false);
			hdd_ipa->pending_cons_req = false;
		}
		qdf_mutex_release(&hdd_ipa->ipa_lock);
	} else if ((HDD_IPA_UC_OPCODE_STATS == msg->op_code) &&
		(HDD_IPA_UC_STAT_REASON_DEBUG == hdd_ipa->stat_req_reason)) {
		uc_fw_stat = (struct ipa_uc_fw_stats *)
			((uint8_t *)op_msg + sizeof(struct op_msg_type));

		/* WLAN FW WDI stats */
		hdd_ipa_print_fw_wdi_stats(hdd_ipa, uc_fw_stat);
	} else if ((HDD_IPA_UC_OPCODE_STATS == msg->op_code) &&
		(HDD_IPA_UC_STAT_REASON_BW_CAL == hdd_ipa->stat_req_reason)) {
		/* STATs from FW */
		uc_fw_stat = (struct ipa_uc_fw_stats *)
			((uint8_t *)op_msg + sizeof(struct op_msg_type));
		qdf_mutex_acquire(&hdd_ipa->ipa_lock);
		hdd_ipa->ipa_tx_packets_diff = HDD_BW_GET_DIFF(
			uc_fw_stat->tx_pkts_completed,
			hdd_ipa->ipa_p_tx_packets);
		hdd_ipa->ipa_rx_packets_diff = HDD_BW_GET_DIFF(
			(uc_fw_stat->rx_num_ind_drop_no_space +
			uc_fw_stat->rx_num_ind_drop_no_buf +
			uc_fw_stat->rx_num_pkts_indicated),
			hdd_ipa->ipa_p_rx_packets);

		hdd_ipa->ipa_p_tx_packets = uc_fw_stat->tx_pkts_completed;
		hdd_ipa->ipa_p_rx_packets =
			(uc_fw_stat->rx_num_ind_drop_no_space +
			uc_fw_stat->rx_num_ind_drop_no_buf +
			uc_fw_stat->rx_num_pkts_indicated);
		qdf_mutex_release(&hdd_ipa->ipa_lock);
	} else if (msg->op_code == HDD_IPA_UC_OPCODE_UC_READY) {
		qdf_mutex_acquire(&hdd_ipa->ipa_lock);
		hdd_ipa_uc_loaded_handler(hdd_ipa);
		qdf_mutex_release(&hdd_ipa->ipa_lock);
	} else if (hdd_ipa_uc_op_metering(hdd_ctx, op_msg)) {
		HDD_IPA_LOG(LOGE, "Invalid message: op_code=%d, reason=%d",
			    msg->op_code, hdd_ipa->stat_req_reason);
	}

	qdf_mem_free(op_msg);
}


/**
 * hdd_ipa_uc_offload_enable_disable() - wdi enable/disable notify to fw
 * @adapter: device adapter instance
 * @offload_type: MCC or SCC
 * @enable: TX offload enable or disable
 *
 * Return: none
 */
static void hdd_ipa_uc_offload_enable_disable(hdd_adapter_t *adapter,
			uint32_t offload_type, bool enable)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	struct sir_ipa_offload_enable_disable ipa_offload_enable_disable;
	struct hdd_ipa_iface_context *iface_context = NULL;
	uint8_t session_id;

	if (hdd_validate_adapter(adapter) || !hdd_ipa)
		return;

	iface_context = adapter->ipa_context;
	session_id = adapter->sessionId;

	if (!iface_context) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Interface context is NULL");
		return;
	}
	if (session_id >= CSR_ROAM_SESSION_MAX) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "invalid session id: %d", session_id);
		return;
	}
	if (enable == hdd_ipa->vdev_offload_enabled[session_id]) {
		/*
		 * This shouldn't happen :
		 * IPA offload status is already set as desired
		 */
		WARN_ON(1);
		HDD_IPA_LOG(QDF_TRACE_LEVEL_WARN,
			"IPA offload status is already set (offload_type=%d, vdev_id=%d, enable=%d)",
			offload_type, session_id, enable);
		return;
	}

	if (wlan_hdd_validate_session_id(adapter->sessionId)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"invalid session id: %d, offload_type=%d, enable=%d",
			adapter->sessionId, offload_type, enable);
		return;
	}

	qdf_mem_zero(&ipa_offload_enable_disable,
		sizeof(ipa_offload_enable_disable));
	ipa_offload_enable_disable.offload_type = offload_type;
	ipa_offload_enable_disable.vdev_id = session_id;
	ipa_offload_enable_disable.enable = enable;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
		"offload_type=%d, vdev_id=%d, enable=%d",
		ipa_offload_enable_disable.offload_type,
		ipa_offload_enable_disable.vdev_id,
		ipa_offload_enable_disable.enable);

	if (QDF_STATUS_SUCCESS !=
		sme_ipa_offload_enable_disable(WLAN_HDD_GET_HAL_CTX(adapter),
			adapter->sessionId, &ipa_offload_enable_disable)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Failure to enable IPA offload (offload_type=%d, vdev_id=%d, enable=%d)",
			    ipa_offload_enable_disable.offload_type,
			    ipa_offload_enable_disable.vdev_id,
			    ipa_offload_enable_disable.enable);
	} else {
		/* Update the IPA offload status */
		hdd_ipa->vdev_offload_enabled[session_id] =
			ipa_offload_enable_disable.enable;
	}
}

/**
 * hdd_ipa_uc_fw_op_event_handler - IPA uC FW OPvent handler
 * @work: uC OP work
 *
 * Return: None
 */
static void hdd_ipa_uc_fw_op_event_handler(struct work_struct *work)
{
	struct op_msg_type *msg;
	struct uc_op_work_struct *uc_op_work = container_of(work,
			struct uc_op_work_struct, work);
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;

	cds_ssr_protect(__func__);

	msg = uc_op_work->msg;
	uc_op_work->msg = NULL;
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
			"posted msg %d",  msg->op_code);

	hdd_ipa_uc_op_cb(msg, hdd_ipa->hdd_ctx);

	cds_ssr_unprotect(__func__);
}

/**
 * hdd_ipa_uc_op_event_handler() - Adapter lookup
 * hdd_ipa_uc_fw_op_event_handler - IPA uC FW OPvent handler
 * @op_msg: operation message received from firmware
 * @hdd_ctx: Global HDD context
 *
 * Return: None
 */
static void hdd_ipa_uc_op_event_handler(uint8_t *op_msg, void *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa;
	struct op_msg_type *msg;
	struct uc_op_work_struct *uc_op_work;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	status = wlan_hdd_validate_context(hdd_ctx);
	if (status)
		goto end;

	msg = (struct op_msg_type *)op_msg;
	hdd_ipa = ((hdd_context_t *)hdd_ctx)->hdd_ipa;

	if (unlikely(!hdd_ipa))
		goto end;

	if (HDD_IPA_UC_OPCODE_MAX <= msg->op_code) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "Invalid OP Code (%d)",
				 msg->op_code);
		goto end;
	}

	uc_op_work = &hdd_ipa->uc_op_work[msg->op_code];
	if (uc_op_work->msg)
		/* When the same uC OPCODE is already pended, just return */
		goto end;

	uc_op_work->msg = msg;
	schedule_work(&uc_op_work->work);
	return;

end:
	qdf_mem_free(op_msg);
}

/**
 * hdd_ipa_init_uc_op_work - init ipa uc op work
 * @work: struct work_struct
 * @work_handler: work_handler
 *
 * Return: none
 */
static void hdd_ipa_init_uc_op_work(struct work_struct *work,
				    work_func_t work_handler)
{
	INIT_WORK(work, work_handler);
}

/**
 * hdd_ipa_uc_ol_init() - Initialize IPA uC offload
 * @hdd_ctx: Global HDD context
 *
 * This function is called to update IPA pipe configuration with resources
 * allocated by wlan driver (cds_pre_enable) before enabling it in FW
 * (cds_enable)
 *
 * Return: QDF_STATUS
 */
QDF_STATUS hdd_ipa_uc_ol_init(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *ipa_ctxt = (struct hdd_ipa_priv *)hdd_ctx->hdd_ipa;
	struct ol_txrx_ipa_resources *ipa_res = &ipa_ctxt->ipa_resource;
	struct ol_txrx_pdev_t *pdev = NULL;
	int i;
	QDF_STATUS stat = QDF_STATUS_SUCCESS;
	qdf_device_t osdev = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);
	uint32_t tx_comp_db_dmaaddr = 0, rx_rdy_db_dmaaddr = 0;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");

	if (!hdd_ipa_uc_is_enabled(hdd_ctx))
		return stat;

	if (!osdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL,
			    "qdf dev context is NULL");
		stat = QDF_STATUS_E_INVAL;
		goto fail_return;
	}

	/* Do only IPA Pipe specific configuration here. All one time
	 * initialization wrt IPA UC shall in hdd_ipa_init and those need
	 * to be reinit at SSR shall in be SSR deinit / reinit functions.
	 */
	pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	if (!pdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL, "pdev is NULL");
		stat = QDF_STATUS_E_FAILURE;
		goto fail_return;
	}

	for (i = 0; i < CSR_ROAM_SESSION_MAX; i++) {
		ipa_ctxt->vdev_to_iface[i] = CSR_ROAM_SESSION_MAX;
		ipa_ctxt->vdev_offload_enabled[i] = false;
	}

	ol_txrx_ipa_uc_get_resource(pdev, ipa_res);
	if (IPA_RESOURCE_READY(ipa_res, pdev->osdev)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL,
			"IPA UC resource alloc fail");
		stat = QDF_STATUS_E_FAILURE;
		goto fail_return;
	}

	if (ipa_ctxt->uc_loaded) {
		if (hdd_ipa_wdi_conn_pipes(ipa_ctxt, ipa_res)) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL,
					"IPA CONN PIPES failed");
			stat = QDF_STATUS_E_FAILURE;
			goto fail_return;
		}

		if (hdd_ipa_init_perf_level(hdd_ctx) != QDF_STATUS_SUCCESS)
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
					"Failed to init perf level");
	} else {
		hdd_ipa_uc_get_db_paddr(&ipa_ctxt->tx_comp_doorbell_dmaaddr,
				IPA_CLIENT_WLAN1_CONS);
		hdd_ipa_uc_get_db_paddr(&ipa_ctxt->rx_ready_doorbell_dmaaddr,
				IPA_CLIENT_WLAN1_PROD);
	}

	if (qdf_mem_smmu_s1_enabled(osdev)) {
		pld_smmu_map(osdev->dev,
				ipa_ctxt->tx_comp_doorbell_dmaaddr,
				&tx_comp_db_dmaaddr,
				sizeof(uint32_t));
		ipa_ctxt->tx_comp_doorbell_dmaaddr = tx_comp_db_dmaaddr;

		pld_smmu_map(osdev->dev,
				ipa_ctxt->rx_ready_doorbell_dmaaddr,
				&rx_rdy_db_dmaaddr,
				sizeof(uint32_t));
		ipa_ctxt->rx_ready_doorbell_dmaaddr = rx_rdy_db_dmaaddr;
	}

	ol_txrx_ipa_uc_set_doorbell_paddr(pdev,
			ipa_ctxt->tx_comp_doorbell_dmaaddr,
			ipa_ctxt->rx_ready_doorbell_dmaaddr);

	for (i = 0; i < HDD_IPA_UC_OPCODE_MAX; i++) {
		hdd_ipa_init_uc_op_work(&ipa_ctxt->uc_op_work[i].work,
				hdd_ipa_uc_fw_op_event_handler);
		ipa_ctxt->uc_op_work[i].msg = NULL;
	}

	ol_txrx_ipa_uc_register_op_cb(pdev,
				      hdd_ipa_uc_op_event_handler,
				      (void *)hdd_ctx);

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
	     "ipa_uc_op_cb=0x%pK, tx_comp_idx_paddr=0x%x, rx_rdy_idx_paddr=0x%x",
	     pdev->ipa_uc_op_cb,
	     (unsigned int)pdev->htt_pdev->ipa_uc_tx_rsc.tx_comp_idx_paddr,
	     (unsigned int)pdev->htt_pdev->ipa_uc_rx_rsc.rx_rdy_idx_paddr);

fail_return:
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit: stat=%d", stat);
	return stat;
}

/**
 * hdd_ipa_cleanup_pending_event() - Cleanup IPA pending event list
 * @hdd_ipa: pointer to HDD IPA struct
 *
 * Return: none
 */
static void hdd_ipa_cleanup_pending_event(struct hdd_ipa_priv *hdd_ipa)
{
	struct ipa_uc_pending_event *pending_event = NULL;

	while (qdf_list_remove_front(&hdd_ipa->pending_event,
		(qdf_list_node_t **)&pending_event) == QDF_STATUS_SUCCESS)
		qdf_mem_free(pending_event);
}

/**
 * hdd_ipa_uc_ol_deinit() - Disconnect IPA TX and RX pipes
 * @hdd_ctx: Global HDD context
 *
 * Return: 0 on success, negativer errno on error
 */
int hdd_ipa_uc_ol_deinit(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa = hdd_ctx->hdd_ipa;
	int i, ret = 0;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");

	if (!hdd_ipa_uc_is_enabled(hdd_ctx))
		return ret;

	if (!hdd_ipa->ipa_pipes_down)
		hdd_ipa_uc_disable_pipes(hdd_ipa);

	if (true == hdd_ipa->uc_loaded)
		ret = hdd_ipa_wdi_disconn_pipes(hdd_ipa);

	qdf_mutex_acquire(&hdd_ipa->ipa_lock);
	hdd_ipa_cleanup_pending_event(hdd_ipa);
	qdf_mutex_release(&hdd_ipa->ipa_lock);

	for (i = 0; i < HDD_IPA_UC_OPCODE_MAX; i++) {
		cancel_work_sync(&hdd_ipa->uc_op_work[i].work);
		qdf_mem_free(hdd_ipa->uc_op_work[i].msg);
		hdd_ipa->uc_op_work[i].msg = NULL;
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit: ret=%d", ret);
	return ret;
}

/**
 * __hdd_ipa_uc_force_pipe_shutdown() - Force shutdown IPA pipe
 * @hdd_ctx: hdd main context
 *
 * Force shutdown IPA pipe
 * Independent of FW pipe status, IPA pipe shutdonw progress
 * in case, any STA does not leave properly, IPA HW pipe should cleaned up
 * independent from FW pipe status
 *
 * Return: NONE
 */
static void __hdd_ipa_uc_force_pipe_shutdown(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");

	if (!hdd_ipa_is_enabled(hdd_ctx) || !hdd_ctx->hdd_ipa)
		return;

	hdd_ipa = (struct hdd_ipa_priv *)hdd_ctx->hdd_ipa;
	if (false == hdd_ipa->ipa_pipes_down) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
				"IPA pipes are not down yet, force shutdown");
		hdd_ipa_uc_disable_pipes(hdd_ipa);
	} else {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
				"IPA pipes are down, do nothing");
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit");
}

/**
 * hdd_ipa_uc_force_pipe_shutdown() - SSR wrapper for
 * __hdd_ipa_uc_force_pipe_shutdown
 * @hdd_ctx: hdd main context
 *
 * Force shutdown IPA pipe
 * Independent of FW pipe status, IPA pipe shutdonw progress
 * in case, any STA does not leave properly, IPA HW pipe should cleaned up
 * independent from FW pipe status
 *
 * Return: NONE
 */
void hdd_ipa_uc_force_pipe_shutdown(hdd_context_t *hdd_ctx)
{
	cds_ssr_protect(__func__);
	__hdd_ipa_uc_force_pipe_shutdown(hdd_ctx);
	cds_ssr_unprotect(__func__);
}

/**
 * hdd_ipa_msg_free_fn() - Free an IPA message
 * @buff: pointer to the IPA message
 * @len: length of the IPA message
 * @type: type of IPA message
 *
 * Return: None
 */
static void hdd_ipa_msg_free_fn(void *buff, uint32_t len, uint32_t type)
{
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "msg type:%d, len:%d", type, len);
	ghdd_ipa->stats.num_free_msg++;
	qdf_mem_free(buff);
}

/**
 * hdd_ipa_uc_send_evt() - send event to ipa
 * @hdd_ctx: pointer to hdd context
 * @type: event type
 * @mac_addr: pointer to mac address
 *
 * Send event to IPA driver
 *
 * Return: 0 - Success
 */
static int hdd_ipa_uc_send_evt(hdd_adapter_t *adapter,
	enum ipa_wlan_event type, uint8_t *mac_addr)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	struct ipa_msg_meta meta;
	struct ipa_wlan_msg *msg;
	int ret = 0;

	meta.msg_len = sizeof(struct ipa_wlan_msg);
	msg = qdf_mem_malloc(meta.msg_len);
	if (msg == NULL) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"msg allocation failed");
		return -ENOMEM;
	}

	meta.msg_type = type;
	strlcpy(msg->name, adapter->dev->name,
		IPA_RESOURCE_NAME_MAX);
	memcpy(msg->mac_addr, mac_addr, ETH_ALEN);
	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO, "%s: Evt: %d",
		msg->name, meta.msg_type);
	ret = ipa_send_msg(&meta, msg, hdd_ipa_msg_free_fn);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"%s: Evt: %d fail:%d",
			msg->name, meta.msg_type,  ret);
		qdf_mem_free(msg);
		return ret;
	}

	hdd_ipa->stats.num_send_msg++;

	return ret;
}

/**
 * hdd_ipa_uc_disconnect_client() - send client disconnect event
 * @hdd_ctx: pointer to hdd adapter
 *
 * Send disconnect client event to IPA driver during SSR
 *
 * Return: 0 - Success
 */
static int hdd_ipa_uc_disconnect_client(hdd_adapter_t *adapter)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	int ret = 0;
	int i;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");
	for (i = 0; i < WLAN_MAX_STA_COUNT; i++) {
		if (qdf_is_macaddr_broadcast(&adapter->aStaInfo[i].macAddrSTA))
			continue;
		if ((adapter->aStaInfo[i].isUsed) &&
		   (!adapter->aStaInfo[i].isDeauthInProgress) &&
		   hdd_ipa->sap_num_connected_sta) {
			hdd_ipa_uc_send_evt(adapter, WLAN_CLIENT_DISCONNECT,
				adapter->aStaInfo[i].macAddrSTA.bytes);
			hdd_ipa->sap_num_connected_sta--;
		}
	}
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit: sap_num_connected_sta=%d",
		    hdd_ipa->sap_num_connected_sta);

	return ret;
}

/**
 * hdd_ipa_uc_disconnect_ap() - send ap disconnect event
 * @hdd_ctx: pointer to hdd adapter
 *
 * Send disconnect ap event to IPA driver during SSR
 *
 * Return: 0 - Success
 */
int hdd_ipa_uc_disconnect_ap(hdd_adapter_t *adapter)
{
	int ret = 0;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");
	if (adapter->ipa_context) {
		hdd_ipa_uc_send_evt(adapter, WLAN_AP_DISCONNECT,
			adapter->dev->dev_addr);
	}
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit");

	return ret;
}

/**
 * hdd_ipa_uc_disconnect_sta() - send sta disconnect event
 * @hdd_ctx: pointer to hdd adapter
 *
 * Send disconnect sta event to IPA driver during SSR
 *
 * Return: 0 - Success
 */
static int hdd_ipa_uc_disconnect_sta(hdd_adapter_t *adapter)
{
	hdd_station_ctx_t *pHddStaCtx;
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	int ret = 0;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");
	if (hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx) &&
	    hdd_ipa->sta_connected) {
		pHddStaCtx = WLAN_HDD_GET_STATION_CTX_PTR(adapter);
		hdd_ipa_uc_send_evt(adapter, WLAN_STA_DISCONNECT,
				pHddStaCtx->conn_info.bssId.bytes);
	}
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit");

	return ret;
}

/**
 * hdd_ipa_uc_disconnect() - send disconnect ipa event
 * @hdd_ctx: pointer to hdd context
 *
 * Send disconnect event to IPA driver during SSR
 *
 * Return: 0 - Success
 */
static int hdd_ipa_uc_disconnect(hdd_context_t *hdd_ctx)
{
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;
	QDF_STATUS status;
	hdd_adapter_t *adapter;
	int ret = 0;

	status =  hdd_get_front_adapter(hdd_ctx, &adapter_node);
	while (NULL != adapter_node && QDF_STATUS_SUCCESS == status) {
		adapter = adapter_node->pAdapter;
		if (adapter->device_mode == QDF_SAP_MODE) {
			hdd_ipa_uc_disconnect_client(adapter);
			hdd_ipa_uc_disconnect_ap(adapter);
		} else if (adapter->device_mode == QDF_STA_MODE) {
			hdd_ipa_uc_disconnect_sta(adapter);
		}

		status = hdd_get_next_adapter(
				hdd_ctx, adapter_node, &next);
		adapter_node = next;
	}

	return ret;
}

/**
 * __hdd_ipa_uc_ssr_deinit() - handle ipa deinit for SSR
 *
 * Deinit basic IPA UC host side to be in sync reloaded FW during
 * SSR
 *
 * Return: 0 - Success
 */
static int __hdd_ipa_uc_ssr_deinit(void)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	int idx;
	struct hdd_ipa_iface_context *iface_context;
	hdd_context_t *hdd_ctx;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");

	if (!hdd_ipa)
		return 0;

	hdd_ctx = hdd_ipa->hdd_ctx;
	if (!hdd_ipa_uc_is_enabled(hdd_ctx))
		return 0;

	/* send disconnect to ipa driver */
	hdd_ipa_uc_disconnect(hdd_ctx);

	/* Clean up HDD IPA interfaces */
	for (idx = 0; (hdd_ipa->num_iface > 0) &&
		(idx < HDD_IPA_MAX_IFACE); idx++) {
		iface_context = &hdd_ipa->iface_context[idx];
		if (iface_context->adapter &&
		    hdd_is_adapter_valid(hdd_ctx, iface_context->adapter)) {
			hdd_ipa_cleanup_iface(iface_context);
		}
	}
	hdd_ipa->num_iface = 0;

	/* After SSR, wlan driver reloads FW again. But we need to protect
	 * IPA submodule during SSR transient state. So deinit basic IPA
	 * UC host side to be in sync with reloaded FW during SSR
	 */

	qdf_mutex_acquire(&hdd_ipa->ipa_lock);
	for (idx = 0; idx < WLAN_MAX_STA_COUNT; idx++) {
		hdd_ipa->assoc_stas_map[idx].is_reserved = false;
		hdd_ipa->assoc_stas_map[idx].sta_id = 0xFF;
	}
	qdf_mutex_release(&hdd_ipa->ipa_lock);

	if (hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx))
		hdd_ipa_uc_sta_reset_sta_connected(hdd_ipa);

	for (idx = 0; idx < HDD_IPA_UC_OPCODE_MAX; idx++) {
		cancel_work_sync(&hdd_ipa->uc_op_work[idx].work);
		qdf_mem_free(hdd_ipa->uc_op_work[idx].msg);
		hdd_ipa->uc_op_work[idx].msg = NULL;
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit");
	return 0;
}

/**
 * hdd_ipa_uc_ssr_deinit() - SSR wrapper for __hdd_ipa_uc_ssr_deinit
 *
 * Deinit basic IPA UC host side to be in sync reloaded FW during
 * SSR
 *
 * Return: 0 - Success
 */
int hdd_ipa_uc_ssr_deinit(void)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __hdd_ipa_uc_ssr_deinit();
	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * __hdd_ipa_uc_ssr_reinit() - handle ipa reinit after SSR
 *
 * Init basic IPA UC host side to be in sync with reloaded FW after
 * SSR to resume IPA UC operations
 *
 * Return: 0 - Success
 */
static int __hdd_ipa_uc_ssr_reinit(hdd_context_t *hdd_ctx)
{

	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	int i;
	struct hdd_ipa_iface_context *iface_context = NULL;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");

	if (!hdd_ipa || !hdd_ipa_uc_is_enabled(hdd_ctx))
		return 0;

	/* Create the interface context */
	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
		iface_context = &hdd_ipa->iface_context[i];
		iface_context->hdd_ipa = hdd_ipa;
		iface_context->cons_client =
			hdd_ipa_adapter_2_client[i].cons_client;
		iface_context->prod_client =
			hdd_ipa_adapter_2_client[i].prod_client;
		iface_context->iface_id = i;
		iface_context->adapter = NULL;
	}

	if (hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
		hdd_ipa->resource_loading = false;
		hdd_ipa->resource_unloading = false;
		hdd_ipa->sta_connected = 0;
		hdd_ipa->ipa_pipes_down = true;
		hdd_ipa->uc_loaded = true;
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit");
	return 0;
}

/**
 * hdd_ipa_uc_ssr_reinit() - SSR wrapper for __hdd_ipa_uc_ssr_reinit
 *
 * Init basic IPA UC host side to be in sync with reloaded FW after
 * SSR to resume IPA UC operations
 *
 * Return: 0 - Success
 */
int hdd_ipa_uc_ssr_reinit(hdd_context_t *hdd_ctx)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __hdd_ipa_uc_ssr_reinit(hdd_ctx);
	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * __hdd_ipa_tx_packet_ipa() - send packet to IPA
 * @hdd_ctx:    Global HDD context
 * @skb:        skb sent to IPA
 * @session_id: send packet instance session id
 *
 * Send TX packet which generated by system to IPA.
 * This routine only will be used for function verification
 *
 * Return: NULL packet sent to IPA properly
 *         NULL invalid packet drop
 *         skb packet not sent to IPA. legacy data path should handle
 */
static struct sk_buff *__hdd_ipa_tx_packet_ipa(hdd_context_t *hdd_ctx,
	struct sk_buff *skb, uint8_t session_id)
{
	struct ipa_header *ipa_header;
	struct frag_header *frag_header;
	struct hdd_ipa_priv *hdd_ipa;

	if (wlan_hdd_validate_context(hdd_ctx))
		return skb;

	hdd_ipa = hdd_ctx->hdd_ipa;

	if (!hdd_ipa_uc_is_enabled(hdd_ctx))
		return skb;

	if (!hdd_ipa)
		return skb;

	if (!hdd_ipa_is_fw_wdi_actived(hdd_ctx))
		return skb;

	if (skb_headroom(skb) <
		(sizeof(struct ipa_header) + sizeof(struct frag_header)))
		return skb;

	ipa_header = (struct ipa_header *) skb_push(skb,
		sizeof(struct ipa_header));
	if (!ipa_header) {
		/* No headroom, legacy */
		return skb;
	}
	memset(ipa_header, 0, sizeof(*ipa_header));
	ipa_header->vdev_id = 0;

	frag_header = (struct frag_header *) skb_push(skb,
		sizeof(struct frag_header));
	if (!frag_header) {
		/* No headroom, drop */
		kfree_skb(skb);
		return NULL;
	}
	memset(frag_header, 0, sizeof(*frag_header));
	frag_header->length = skb->len - sizeof(struct frag_header)
		- sizeof(struct ipa_header);

	ipa_tx_dp(IPA_CLIENT_WLAN1_CONS, skb, NULL);
	return NULL;
}

/**
 * hdd_ipa_tx_packet_ipa() - SSR wrapper for __hdd_ipa_tx_packet_ipa
 * @hdd_ctx:    Global HDD context
 * @skb:        skb sent to IPA
 * @session_id: send packet instance session id
 *
 * Send TX packet which generated by system to IPA.
 * This routine only will be used for function verification
 *
 * Return: NULL packet sent to IPA properly
 *         NULL invalid packet drop
 *         skb packet not sent to IPA. legacy data path should handle
 */
struct sk_buff *hdd_ipa_tx_packet_ipa(hdd_context_t *hdd_ctx,
	struct sk_buff *skb, uint8_t session_id)
{
	struct sk_buff *ret;

	cds_ssr_protect(__func__);
	ret = __hdd_ipa_tx_packet_ipa(hdd_ctx, skb, session_id);
	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * __hdd_ipa_set_perf_level() - Set IPA performance level
 * @hdd_ctx: Global HDD context
 * @tx_packets: Number of packets transmitted in the last sample period
 * @rx_packets: Number of packets received in the last sample period
 *
 * Return: 0 on success, negative errno on error
 */
static int __hdd_ipa_set_perf_level(hdd_context_t *hdd_ctx, uint64_t tx_packets,
			   uint64_t rx_packets)
{
	uint32_t next_cons_bw, next_prod_bw;
	struct hdd_ipa_priv *hdd_ipa;
	int ret;

	if (wlan_hdd_validate_context(hdd_ctx))
		return 0;

	hdd_ipa = hdd_ctx->hdd_ipa;

	if ((!hdd_ipa_is_enabled(hdd_ctx)) ||
		(!hdd_ipa_is_clk_scaling_enabled(hdd_ctx)))
		return 0;

	if (tx_packets > (hdd_ctx->config->busBandwidthHighThreshold / 2))
		next_cons_bw = hdd_ctx->config->IpaHighBandwidthMbps;
	else if (tx_packets >
		 (hdd_ctx->config->busBandwidthMediumThreshold / 2))
		next_cons_bw = hdd_ctx->config->IpaMediumBandwidthMbps;
	else
		next_cons_bw = hdd_ctx->config->IpaLowBandwidthMbps;

	if (rx_packets > (hdd_ctx->config->busBandwidthHighThreshold / 2))
		next_prod_bw = hdd_ctx->config->IpaHighBandwidthMbps;
	else if (rx_packets >
		 (hdd_ctx->config->busBandwidthMediumThreshold / 2))
		next_prod_bw = hdd_ctx->config->IpaMediumBandwidthMbps;
	else
		next_prod_bw = hdd_ctx->config->IpaLowBandwidthMbps;

	if (hdd_ipa->curr_cons_bw != next_cons_bw) {
		hdd_debug("Requesting CONS perf curr: %d, next: %d",
			    hdd_ipa->curr_cons_bw, next_cons_bw);
		ret = hdd_ipa_wdi_rm_set_perf_profile(hdd_ipa,
				IPA_CLIENT_WLAN1_CONS, next_cons_bw);
		if (ret) {
			hdd_err("RM CONS set perf profile failed: %d", ret);

			return ret;
		}
		hdd_ipa->curr_cons_bw = next_cons_bw;
		hdd_ipa->stats.num_cons_perf_req++;
	}

	if (hdd_ipa->curr_prod_bw != next_prod_bw) {
		hdd_debug("Requesting PROD perf curr: %d, next: %d",
			    hdd_ipa->curr_prod_bw, next_prod_bw);
		ret = hdd_ipa_wdi_rm_set_perf_profile(hdd_ipa,
				IPA_CLIENT_WLAN1_PROD, next_prod_bw);
		if (ret) {
			hdd_err("RM PROD set perf profile failed: %d", ret);
			return ret;
		}
		hdd_ipa->curr_prod_bw = next_prod_bw;
		hdd_ipa->stats.num_prod_perf_req++;
	}

	return 0;
}

/**
 * hdd_ipa_set_perf_level() - SSR wrapper for __hdd_ipa_set_perf_level
 * @hdd_ctx: Global HDD context
 * @tx_packets: Number of packets transmitted in the last sample period
 * @rx_packets: Number of packets received in the last sample period
 *
 * Return: 0 on success, negative errno on error
 */
int hdd_ipa_set_perf_level(hdd_context_t *hdd_ctx, uint64_t tx_packets,
			   uint64_t rx_packets)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __hdd_ipa_set_perf_level(hdd_ctx, tx_packets, rx_packets);
	cds_ssr_unprotect(__func__);

	return ret;
}

#ifdef QCA_CONFIG_SMP
/**
 * hdd_ipa_get_wake_up_idle() - Get PF_WAKE_UP_IDLE flag in the task structure
 *
 * Get PF_WAKE_UP_IDLE flag in the task structure
 *
 * Return: 1 if PF_WAKE_UP_IDLE flag is set, 0 otherwise
 */
static uint32_t hdd_ipa_get_wake_up_idle(void)
{
	return sched_get_wake_up_idle(current);
}

/**
 * hdd_ipa_set_wake_up_idle() - Set PF_WAKE_UP_IDLE flag in the task structure
 *
 * Set PF_WAKE_UP_IDLE flag in the task structure
 * This task and any task woken by this will be waken to idle CPU
 *
 * Return: None
 */
static void hdd_ipa_set_wake_up_idle(bool wake_up_idle)
{
	sched_set_wake_up_idle(current, wake_up_idle);

}

static int hdd_ipa_aggregated_rx_ind(qdf_nbuf_t skb)
{
	return netif_rx_ni(skb);
}
#else /* QCA_CONFIG_SMP */
static uint32_t hdd_ipa_get_wake_up_idle(void)
{
	return 0;
}

static void hdd_ipa_set_wake_up_idle(bool wake_up_idle)
{
}

static int hdd_ipa_aggregated_rx_ind(qdf_nbuf_t skb)
{
	struct iphdr *ip_h;
	static atomic_t softirq_mitigation_cntr =
		ATOMIC_INIT(IPA_WLAN_RX_SOFTIRQ_THRESH);
	int result;

	ip_h = (struct iphdr *)(skb->data);
	if ((skb->protocol == htons(ETH_P_IP)) &&
		(ip_h->protocol == IPPROTO_ICMP)) {
		result = netif_rx_ni(skb);
	} else {
		/* Call netif_rx_ni for every IPA_WLAN_RX_SOFTIRQ_THRESH packets
		 * to avoid excessive softirq's.
		 */
		if (atomic_dec_and_test(&softirq_mitigation_cntr)) {
			result = netif_rx_ni(skb);
			atomic_set(&softirq_mitigation_cntr,
					IPA_WLAN_RX_SOFTIRQ_THRESH);
		} else {
			result = netif_rx(skb);
		}
	}

	return result;
}
#endif /* QCA_CONFIG_SMP */

/**
 * hdd_ipa_send_skb_to_network() - Send skb to kernel
 * @skb: network buffer
 * @adapter: network adapter
 *
 * Called when a network buffer is received which should not be routed
 * to the IPA module.
 *
 * Return: None
 */
static void hdd_ipa_send_skb_to_network(qdf_nbuf_t skb,
	hdd_adapter_t *adapter)
{
	int result;
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	unsigned int cpu_index;
	uint32_t enabled;
	struct qdf_mac_addr src_mac;
	uint8_t staid;

	if (hdd_validate_adapter(adapter)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "Invalid adapter: 0x%pK",
			    adapter);
		hdd_ipa->ipa_rx_internal_drop_count++;
		kfree_skb(skb);
		return;
	}

	if (cds_is_driver_unloading()) {
		hdd_ipa->ipa_rx_internal_drop_count++;
		kfree_skb(skb);
		return;
	}

	/*
	 * Set PF_WAKE_UP_IDLE flag in the task structure
	 * This task and any task woken by this will be waken to idle CPU
	 */
	enabled = hdd_ipa_get_wake_up_idle();
	if (!enabled)
		hdd_ipa_set_wake_up_idle(true);

	if ((adapter->device_mode == QDF_SAP_MODE) &&
	     (qdf_nbuf_is_ipv4_dhcp_pkt(skb) == true)) {
		/* Send DHCP Indication to FW */
		qdf_mem_copy(&src_mac, skb->data + QDF_NBUF_SRC_MAC_OFFSET,
			     sizeof(src_mac));
		if (QDF_STATUS_SUCCESS ==
			hdd_softap_get_sta_id(adapter, &src_mac, &staid))
			hdd_dhcp_indication(adapter, staid, skb, QDF_RX);
	}

	skb->destructor = hdd_ipa_uc_rt_debug_destructor;
	skb->dev = adapter->dev;
	skb->protocol = eth_type_trans(skb, skb->dev);
	skb->ip_summed = CHECKSUM_NONE;

	cpu_index = wlan_hdd_get_cpu();

	++adapter->hdd_stats.hddTxRxStats.rxPackets[cpu_index];

	/*
	* Update STA RX exception packet stats.
	* For SAP as part of IPA HW stats are updated.
	*/
	if (adapter->device_mode == QDF_STA_MODE) {
		++adapter->stats.rx_packets;
		adapter->stats.rx_bytes += skb->len;
	}

	result = hdd_ipa_aggregated_rx_ind(skb);
	if (result == NET_RX_SUCCESS)
		++adapter->hdd_stats.hddTxRxStats.rxDelivered[cpu_index];
	else
		++adapter->hdd_stats.hddTxRxStats.rxRefused[cpu_index];

	hdd_ipa->ipa_rx_net_send_count++;

	/*
	 * Restore PF_WAKE_UP_IDLE flag in the task structure
	 */
	if (!enabled)
		hdd_ipa_set_wake_up_idle(false);
}

/**
 * hdd_ipa_forward() - handle packet forwarding to wlan tx
 * @hdd_ipa: pointer to hdd ipa context
 * @adapter: network adapter
 * @skb: data pointer
 *
 * if exception packet has set forward bit, copied new packet should be
 * forwarded to wlan tx. if wlan subsystem is in suspend state, packet should
 * put into pm queue and tx procedure will be differed
 *
 * Return: None
 */
static void hdd_ipa_forward(struct hdd_ipa_priv *hdd_ipa,
			    hdd_adapter_t *adapter, qdf_nbuf_t skb)
{
	struct hdd_ipa_pm_tx_cb *pm_tx_cb;

	qdf_spin_lock_bh(&hdd_ipa->pm_lock);

	/* Set IPA ownership for intra-BSS Tx packets to avoid skb_orphan */
	qdf_nbuf_ipa_owned_set(skb);

	/* WLAN subsystem is in suspend, put in queue */
	if (hdd_ipa->suspended) {
		qdf_spin_unlock_bh(&hdd_ipa->pm_lock);
		HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
			"Tx in suspend, put in queue");
		qdf_mem_set(skb->cb, sizeof(skb->cb), 0);
		pm_tx_cb = (struct hdd_ipa_pm_tx_cb *)skb->cb;
		pm_tx_cb->exception = true;
		pm_tx_cb->adapter = adapter;
		qdf_spin_lock_bh(&hdd_ipa->pm_lock);
		qdf_nbuf_queue_add(&hdd_ipa->pm_queue_head, skb);
		qdf_spin_unlock_bh(&hdd_ipa->pm_lock);
		hdd_ipa->stats.num_tx_queued++;
	} else {
		/* Resume, put packet into WLAN TX */
		qdf_spin_unlock_bh(&hdd_ipa->pm_lock);
		if (hdd_softap_hard_start_xmit(skb, adapter->dev)) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "packet Tx fail");
			hdd_ipa->stats.num_tx_fwd_err++;
		} else {
			hdd_ipa->stats.num_tx_fwd_ok++;
		}
	}
}

/**
 * hdd_ipa_intrabss_forward() - Forward intra bss packets.
 * @hdd_ipa: pointer to HDD IPA struct
 * @adapter: hdd adapter pointer
 * @desc: Firmware descriptor
 * @skb: Data buffer
 *
 * Return:
 *      HDD_IPA_FORWARD_PKT_NONE
 *      HDD_IPA_FORWARD_PKT_DISCARD
 *      HDD_IPA_FORWARD_PKT_LOCAL_STACK
 *
 */

static enum hdd_ipa_forward_type hdd_ipa_intrabss_forward(
		struct hdd_ipa_priv *hdd_ipa,
		hdd_adapter_t *adapter,
		uint8_t desc,
		qdf_nbuf_t skb)
{
	int ret = HDD_IPA_FORWARD_PKT_NONE;

	if ((desc & FW_RX_DESC_FORWARD_M)) {
		if (!ol_txrx_fwd_desc_thresh_check(
			ol_txrx_get_vdev_from_vdev_id(adapter->sessionId))) {
			/* Drop the packet*/
			hdd_ipa->stats.num_tx_fwd_err++;
			kfree_skb(skb);
			ret = HDD_IPA_FORWARD_PKT_DISCARD;
			return ret;
		}
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
				"Forward packet to Tx (fw_desc=%d)", desc);
		hdd_ipa->ipa_tx_forward++;

		if ((desc & FW_RX_DESC_DISCARD_M)) {
			hdd_ipa_forward(hdd_ipa, adapter, skb);
			hdd_ipa->ipa_rx_internal_drop_count++;
			hdd_ipa->ipa_rx_discard++;
			ret = HDD_IPA_FORWARD_PKT_DISCARD;
		} else {
			struct sk_buff *cloned_skb = skb_clone(skb, GFP_ATOMIC);

			if (cloned_skb)
				hdd_ipa_forward(hdd_ipa, adapter, cloned_skb);
			else
				HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
						"tx skb alloc failed");
			ret = HDD_IPA_FORWARD_PKT_LOCAL_STACK;
		}
	}

	return ret;
}

/**
 * wlan_ipa_eapol_intrabss_fwd_check() - Check if eapol pkt intrabss fwd is
 *  allowed or not
 * @nbuf: network buffer
 * @vdev_id: vdev id
 *
 * Return: true if intrabss fwd is allowed for eapol else false
 */
static bool
wlan_ipa_eapol_intrabss_fwd_check(qdf_nbuf_t nbuf, uint8_t vdev_id)
{
	ol_txrx_vdev_handle vdev;
	uint8_t *vdev_mac_addr;

	vdev = ol_txrx_get_vdev_from_vdev_id(vdev_id);

	if (!vdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "txrx vdev is NULL for vdev_id = %d", vdev_id);
		return false;
	}

	vdev_mac_addr = ol_txrx_get_vdev_mac_addr(vdev);

	if (!vdev_mac_addr)
		return false;

	if (qdf_mem_cmp(qdf_nbuf_data(nbuf) + QDF_NBUF_DEST_MAC_OFFSET,
			vdev_mac_addr, QDF_MAC_ADDR_SIZE))
		return false;

	return true;
}

/**
 * __hdd_ipa_w2i_cb() - WLAN to IPA callback handler
 * @priv: pointer to private data registered with IPA (we register a
 *	pointer to the global IPA context)
 * @evt: the IPA event which triggered the callback
 * @data: data associated with the event
 *
 * Return: None
 */
static void __hdd_ipa_w2i_cb(void *priv, enum ipa_dp_evt_type evt,
			   unsigned long data)
{
	struct hdd_ipa_priv *hdd_ipa = NULL;
	hdd_adapter_t *adapter = NULL;
	qdf_nbuf_t skb;
	uint8_t iface_id;
	uint8_t session_id;
	struct hdd_ipa_iface_context *iface_context;
	uint8_t fw_desc;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	bool is_eapol_wapi = false;
	struct qdf_mac_addr peer_mac_addr = QDF_MAC_ADDR_ZERO_INITIALIZER;
	uint8_t sta_idx;
	ol_txrx_peer_handle peer;
	ol_txrx_pdev_handle pdev;
	hdd_station_ctx_t *sta_ctx;

	hdd_ipa = (struct hdd_ipa_priv *)priv;

	if (!hdd_ipa || wlan_hdd_validate_context(hdd_ipa->hdd_ctx))
		return;

	switch (evt) {
	case IPA_RECEIVE:
		skb = (qdf_nbuf_t) data;

		/*
		 * When SSR is going on or driver is unloading,
		 * just drop the packets.
		 */
		status = wlan_hdd_validate_context(hdd_ipa->hdd_ctx);
		if (0 != status) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
					"Invalid context: drop packet");
			hdd_ipa->ipa_rx_internal_drop_count++;
			kfree_skb(skb);
			return;
		}

		pdev = cds_get_context(QDF_MODULE_ID_TXRX);
		if (NULL == pdev) {
			WMA_LOGE("%s: DP pdev is NULL", __func__);
			kfree_skb(skb);
			return;
		}

		if (hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
			session_id = (uint8_t)skb->cb[0];
			iface_id = hdd_ipa->vdev_to_iface[session_id];
			HDD_IPA_DP_LOG(QDF_TRACE_LEVEL_DEBUG,
				"IPA_RECEIVE: session_id=%u, iface_id=%u",
				session_id, iface_id);
		} else {
			iface_id = HDD_IPA_GET_IFACE_ID(skb->data);
		}

		if (iface_id >= HDD_IPA_MAX_IFACE) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "IPA_RECEIVE: Invalid iface_id: %u",
				    iface_id);
			HDD_IPA_DBG_DUMP(QDF_TRACE_LEVEL_DEBUG,
				"w2i -- skb",
				skb->data, HDD_IPA_DBG_DUMP_RX_LEN);
			hdd_ipa->ipa_rx_internal_drop_count++;
			kfree_skb(skb);
			return;
		}

		iface_context = &hdd_ipa->iface_context[iface_id];
		adapter = iface_context->adapter;
		if (hdd_validate_adapter(adapter)) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "IPA_RECEIVE: Invalid adapter");
			hdd_ipa->ipa_rx_internal_drop_count++;
			kfree_skb(skb);
			return;
		}

		HDD_IPA_DBG_DUMP(QDF_TRACE_LEVEL_DEBUG,
				"w2i -- skb",
				skb->data, HDD_IPA_DBG_DUMP_RX_LEN);
		if (hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
			hdd_ipa->stats.num_rx_excep++;
			skb_pull(skb, HDD_IPA_UC_WLAN_CLD_HDR_LEN);
		} else {
			skb_pull(skb, HDD_IPA_WLAN_CLD_HDR_LEN);
		}

		if (iface_context->adapter->device_mode == QDF_STA_MODE) {
			sta_ctx = WLAN_HDD_GET_STATION_CTX_PTR(
							iface_context->adapter);
			qdf_copy_macaddr(&peer_mac_addr,
					 &sta_ctx->conn_info.bssId);
		} else if (iface_context->adapter->device_mode
			   == QDF_SAP_MODE) {
			qdf_mem_copy(peer_mac_addr.bytes, qdf_nbuf_data(skb) +
				     QDF_NBUF_SRC_MAC_OFFSET,
				     QDF_MAC_ADDR_SIZE);
		}

		if (qdf_nbuf_is_ipv4_eapol_pkt(skb)) {
			is_eapol_wapi = true;
			if (iface_context->adapter->device_mode ==
			    QDF_SAP_MODE &&
			    !wlan_ipa_eapol_intrabss_fwd_check(skb,
					   iface_context->adapter->sessionId)) {
				HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
					    "EAPOL intrabss fwd drop DA: %pM",
					    qdf_nbuf_data(skb) +
					    QDF_NBUF_DEST_MAC_OFFSET);
				hdd_ipa->ipa_rx_internal_drop_count++;
				kfree_skb(skb);
				return;
			}
		} else if (qdf_nbuf_is_ipv4_wapi_pkt(skb)) {
			is_eapol_wapi = true;
		}

		peer = ol_txrx_find_peer_by_addr(pdev, peer_mac_addr.bytes,
						 &sta_idx);

		/*
		 * Check for peer auth state before allowing non-EAPOL/WAPI
		 * frames to be intrabss forwarded or submitted to stack.
		 */
		if (peer && ol_txrx_get_peer_state(peer) !=
		    OL_TXRX_PEER_STATE_AUTH && !is_eapol_wapi) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "non-EAPOL/WAPI frame received when peer is unauthorized");
			hdd_ipa->ipa_rx_internal_drop_count++;
			kfree_skb(skb);
			return;
		}

		iface_context->stats.num_rx_ipa_excep++;

		/* Disable to forward Intra-BSS Rx packets when
		 * ap_isolate=1 in hostapd.conf
		 */
		if (!adapter->sessionCtx.ap.apDisableIntraBssFwd) {
			/*
			 * When INTRA_BSS_FWD_OFFLOAD is enabled, FW will send
			 * all Rx packets to IPA uC, which need to be forwarded
			 * to other interface.
			 * And, IPA driver will send back to WLAN host driver
			 * through exception pipe with fw_desc field set by FW.
			 * Here we are checking fw_desc field for FORWARD bit
			 * set, and forward to Tx. Then copy to kernel stack
			 * only when DISCARD bit is not set.
			 */
			fw_desc = (uint8_t)skb->cb[1];
			if (HDD_IPA_FORWARD_PKT_DISCARD ==
			    hdd_ipa_intrabss_forward(hdd_ipa, adapter,
						     fw_desc, skb))
				break;
		} else {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
				"Intra-BSS FWD is disabled-skip forward to Tx");
		}

		hdd_ipa_send_skb_to_network(skb, adapter);
		break;

	default:
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "w2i cb wrong event: 0x%x", evt);
		return;
	}
}

/**
 * hdd_ipa_w2i_cb() - SSR wrapper for __hdd_ipa_w2i_cb
 * @priv: pointer to private data registered with IPA (we register a
 *	pointer to the global IPA context)
 * @evt: the IPA event which triggered the callback
 * @data: data associated with the event
 *
 * Return: None
 */
static void hdd_ipa_w2i_cb(void *priv, enum ipa_dp_evt_type evt,
			   unsigned long data)
{
	cds_ssr_protect(__func__);
	__hdd_ipa_w2i_cb(priv, evt, data);
	cds_ssr_unprotect(__func__);
}

/**
 * hdd_ipa_nbuf_cb() - IPA TX complete callback
 * @skb: packet buffer which was transmitted
 *
 * Return: None
 */
void hdd_ipa_nbuf_cb(qdf_nbuf_t skb)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	struct ipa_rx_data *ipa_tx_desc;
	struct hdd_ipa_tx_desc *tx_desc;
	uint16_t id;
	qdf_device_t osdev;

	if (!qdf_nbuf_ipa_owned_get(skb)) {
		dev_kfree_skb_any(skb);
		return;
	}

	osdev = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);
	if (osdev && qdf_mem_smmu_s1_enabled(osdev)) {
		if (hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx)) {
			qdf_dma_addr_t paddr = QDF_NBUF_CB_PADDR(skb);
			qdf_nbuf_mapped_paddr_set(skb,
						  paddr -
						  HDD_IPA_WLAN_FRAG_HEADER -
						  HDD_IPA_WLAN_IPA_HEADER);
		}

		qdf_nbuf_unmap(osdev, skb, QDF_DMA_TO_DEVICE);
	}

	/* Get Tx desc pointer from SKB CB */
	id = QDF_NBUF_CB_TX_IPA_PRIV(skb);
	tx_desc = hdd_ipa->tx_desc_list + id;
	ipa_tx_desc = tx_desc->ipa_tx_desc_ptr;

	/* Return Tx Desc to IPA */
	ipa_free_skb(ipa_tx_desc);

	/* Return to free tx desc list */
	qdf_spin_lock_bh(&hdd_ipa->q_lock);
	tx_desc->ipa_tx_desc_ptr = NULL;
	list_add_tail(&tx_desc->link, &hdd_ipa->free_tx_desc_head);
	hdd_ipa->stats.num_tx_desc_q_cnt--;
	qdf_spin_unlock_bh(&hdd_ipa->q_lock);

	hdd_ipa->stats.num_tx_comp_cnt++;

	atomic_dec(&hdd_ipa->tx_ref_cnt);

	hdd_ipa_wdi_rm_try_release(hdd_ipa);
}

/**
 * hdd_ipa_send_pkt_to_tl() - Send an IPA packet to TL
 * @iface_context: interface-specific IPA context
 * @ipa_tx_desc: packet data descriptor
 *
 * Return: None
 */
static void hdd_ipa_send_pkt_to_tl(
		struct hdd_ipa_iface_context *iface_context,
		struct ipa_rx_data *ipa_tx_desc)
{
	struct hdd_ipa_priv *hdd_ipa = iface_context->hdd_ipa;
	hdd_adapter_t *adapter = NULL;
	qdf_nbuf_t skb;
	struct hdd_ipa_tx_desc *tx_desc;
	qdf_device_t osdev;
	qdf_dma_addr_t paddr;
	QDF_STATUS status;

	qdf_spin_lock_bh(&iface_context->interface_lock);
	adapter = iface_context->adapter;
	if (hdd_validate_adapter(adapter)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_WARN, "Interface Down");
		ipa_free_skb(ipa_tx_desc);
		iface_context->stats.num_tx_drop++;
		qdf_spin_unlock_bh(&iface_context->interface_lock);
		hdd_ipa_wdi_rm_try_release(hdd_ipa);
		return;
	}

	/*
	 * During CAC period, data packets shouldn't be sent over the air so
	 * drop all the packets here
	 */
	if (QDF_SAP_MODE == adapter->device_mode ||
	    QDF_P2P_GO_MODE == adapter->device_mode) {
		if (WLAN_HDD_GET_AP_CTX_PTR(adapter)->dfs_cac_block_tx) {
			ipa_free_skb(ipa_tx_desc);
			qdf_spin_unlock_bh(&iface_context->interface_lock);
			iface_context->stats.num_tx_cac_drop++;
			hdd_ipa_wdi_rm_try_release(hdd_ipa);
			return;
		}
	}

	osdev = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);
	if (!osdev) {
		ipa_free_skb(ipa_tx_desc);
		iface_context->stats.num_tx_drop++;
		qdf_spin_unlock_bh(&iface_context->interface_lock);
		hdd_ipa_wdi_rm_try_release(hdd_ipa);
		return;
	}

	++adapter->stats.tx_packets;

	qdf_spin_unlock_bh(&iface_context->interface_lock);

	skb = ipa_tx_desc->skb;

	qdf_mem_set(skb->cb, sizeof(skb->cb), 0);

	/* Store IPA Tx buffer ownership into SKB CB */
	qdf_nbuf_ipa_owned_set(skb);

	if (qdf_mem_smmu_s1_enabled(osdev)) {
		status = qdf_nbuf_map(osdev, skb, QDF_DMA_TO_DEVICE);
		if (QDF_IS_STATUS_SUCCESS(status)) {
			paddr = qdf_nbuf_get_frag_paddr(skb, 0);
		} else {
			ipa_free_skb(ipa_tx_desc);
			qdf_spin_lock_bh(&iface_context->interface_lock);
			iface_context->stats.num_tx_drop++;
			qdf_spin_unlock_bh(&iface_context->interface_lock);
			hdd_ipa_wdi_rm_try_release(hdd_ipa);
			return;
		}
	} else {
		paddr = ipa_tx_desc->dma_addr;
	}

	if (hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx)) {
		qdf_nbuf_mapped_paddr_set(skb,
					  paddr +
					  HDD_IPA_WLAN_FRAG_HEADER +
					  HDD_IPA_WLAN_IPA_HEADER);

		ipa_tx_desc->skb->len -=
			HDD_IPA_WLAN_FRAG_HEADER + HDD_IPA_WLAN_IPA_HEADER;
	} else {
		qdf_nbuf_mapped_paddr_set(skb, paddr);
	}

	qdf_spin_lock_bh(&hdd_ipa->q_lock);
	/* get free Tx desc and assign ipa_tx_desc pointer */
	if (!list_empty(&hdd_ipa->free_tx_desc_head)) {
		tx_desc = list_first_entry(&hdd_ipa->free_tx_desc_head,
					   struct hdd_ipa_tx_desc, link);
		list_del(&tx_desc->link);
		tx_desc->ipa_tx_desc_ptr = ipa_tx_desc;
		hdd_ipa->stats.num_tx_desc_q_cnt++;
		qdf_spin_unlock_bh(&hdd_ipa->q_lock);
		/* Store Tx Desc index into SKB CB */
		QDF_NBUF_CB_TX_IPA_PRIV(skb) = tx_desc->id;
	} else {
		hdd_ipa->stats.num_tx_desc_error++;
		qdf_spin_unlock_bh(&hdd_ipa->q_lock);

		if (qdf_mem_smmu_s1_enabled(osdev)) {
			if (hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx))
				qdf_nbuf_mapped_paddr_set(skb, paddr);
			qdf_nbuf_unmap(osdev, skb, QDF_DMA_TO_DEVICE);
		}

		ipa_free_skb(ipa_tx_desc);
		hdd_ipa_wdi_rm_try_release(hdd_ipa);
		return;
	}

	adapter->stats.tx_bytes += ipa_tx_desc->skb->len;

	skb = ol_tx_send_ipa_data_frame(iface_context->tl_context,
					 ipa_tx_desc->skb);
	if (skb) {
		qdf_nbuf_free(skb);
		iface_context->stats.num_tx_err++;
		return;
	}

	atomic_inc(&hdd_ipa->tx_ref_cnt);

	iface_context->stats.num_tx++;
}

/**
 * hdd_ipa_is_present() - get IPA hw status
 *
 * ipa_uc_reg_rdyCB is not directly designed to check
 * ipa hw status. This is an undocumented function which
 * has confirmed with IPA team.
 *
 * Return: true - ipa hw present
 *         false - ipa hw not present
 */
bool hdd_ipa_is_present(void)
{
	/*
	 * Check if ipa hw is enabled
	 * TODO: Add support for WDI unified API
	 */
	if (ipa_uc_reg_rdyCB(NULL) != -EPERM)
		return true;
	else
		return false;
}

/**
 * __hdd_ipa_i2w_cb() - IPA to WLAN callback
 * @priv: pointer to private data registered with IPA (we register a
 *	pointer to the interface-specific IPA context)
 * @evt: the IPA event which triggered the callback
 * @data: data associated with the event
 *
 * Return: None
 */
static void __hdd_ipa_i2w_cb(void *priv, enum ipa_dp_evt_type evt,
			   unsigned long data)
{
	struct hdd_ipa_priv *hdd_ipa = NULL;
	struct ipa_rx_data *ipa_tx_desc;
	struct hdd_ipa_iface_context *iface_context;
	qdf_nbuf_t skb;
	struct hdd_ipa_pm_tx_cb *pm_tx_cb = NULL;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	iface_context = (struct hdd_ipa_iface_context *)priv;
	ipa_tx_desc = (struct ipa_rx_data *)data;
	hdd_ipa = iface_context->hdd_ipa;

	if (evt != IPA_RECEIVE) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "Event is not IPA_RECEIVE");
		ipa_free_skb(ipa_tx_desc);
		iface_context->stats.num_tx_drop++;
		return;
	}

	/*
	 * When SSR is going on or driver is unloading, just drop the packets.
	 * During SSR, there is no use in queueing the packets as STA has to
	 * connect back any way
	 */
	status = wlan_hdd_validate_context(hdd_ipa->hdd_ctx);
	if (status) {
		ipa_free_skb(ipa_tx_desc);
		iface_context->stats.num_tx_drop++;
		return;
	}

	skb = ipa_tx_desc->skb;

	HDD_IPA_DBG_DUMP(QDF_TRACE_LEVEL_DEBUG,
			 "i2w", skb->data, HDD_IPA_DBG_DUMP_TX_LEN);

	/*
	 * If PROD resource is not requested here then there may be cases where
	 * IPA hardware may be clocked down because of not having proper
	 * dependency graph between WLAN CONS and modem PROD pipes. Adding the
	 * workaround to request PROD resource while data is going over CONS
	 * pipe to prevent the IPA hardware clockdown.
	 */
	hdd_ipa_wdi_rm_request(hdd_ipa);

	qdf_spin_lock_bh(&hdd_ipa->pm_lock);
	/*
	 * If host is still suspended then queue the packets and these will be
	 * drained later when resume completes. When packet is arrived here and
	 * host is suspended, this means that there is already resume is in
	 * progress.
	 */
	if (hdd_ipa->suspended) {
		qdf_mem_set(skb->cb, sizeof(skb->cb), 0);
		pm_tx_cb = (struct hdd_ipa_pm_tx_cb *)skb->cb;
		pm_tx_cb->iface_context = iface_context;
		pm_tx_cb->ipa_tx_desc = ipa_tx_desc;
		qdf_nbuf_queue_add(&hdd_ipa->pm_queue_head, skb);
		hdd_ipa->stats.num_tx_queued++;

		qdf_spin_unlock_bh(&hdd_ipa->pm_lock);
		return;
	}

	qdf_spin_unlock_bh(&hdd_ipa->pm_lock);

	/*
	 * If we are here means, host is not suspended, wait for the work queue
	 * to finish.
	 */
	flush_work(&hdd_ipa->pm_work);

	return hdd_ipa_send_pkt_to_tl(iface_context, ipa_tx_desc);
}

/*
 * hdd_ipa_i2w_cb() - SSR wrapper for __hdd_ipa_i2w_cb
 * @priv: pointer to private data registered with IPA (we register a
 *	pointer to the interface-specific IPA context)
 * @evt: the IPA event which triggered the callback
 * @data: data associated with the event
 *
 * Return: None
 */
static void hdd_ipa_i2w_cb(void *priv, enum ipa_dp_evt_type evt,
			   unsigned long data)
{
	cds_ssr_protect(__func__);
	__hdd_ipa_i2w_cb(priv, evt, data);
	cds_ssr_unprotect(__func__);
}

/**
 * __hdd_ipa_suspend() - Suspend IPA
 * @hdd_ctx: Global HDD context
 *
 * Return: 0 on success, negativer errno on error
 */
static int __hdd_ipa_suspend(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa;

	if (wlan_hdd_validate_context(hdd_ctx))
		return 0;

	hdd_ipa = hdd_ctx->hdd_ipa;

	if (!hdd_ipa_is_enabled(hdd_ctx))
		return 0;

	/*
	 * Check if IPA is ready for suspend, If we are here means, there is
	 * high chance that suspend would go through but just to avoid any race
	 * condition after suspend started, these checks are conducted before
	 * allowing to suspend.
	 */
	if (atomic_read(&hdd_ipa->tx_ref_cnt))
		return -EAGAIN;

	if (!hdd_ipa_is_rm_released(hdd_ipa))
		return -EAGAIN;

	qdf_spin_lock_bh(&hdd_ipa->pm_lock);
	hdd_ipa->suspended = true;
	qdf_spin_unlock_bh(&hdd_ipa->pm_lock);

	return 0;
}

/**
 * hdd_ipa_suspend() - SSR wrapper for __hdd_ipa_suspend
 * @hdd_ctx: Global HDD context
 *
 * Return: 0 on success, negativer errno on error
 */
int hdd_ipa_suspend(hdd_context_t *hdd_ctx)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __hdd_ipa_suspend(hdd_ctx);
	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * __hdd_ipa_resume() - Resume IPA following suspend
 * hdd_ctx: Global HDD context
 *
 * Return: 0 on success, negative errno on error
 */
static int __hdd_ipa_resume(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa;

	if (wlan_hdd_validate_context(hdd_ctx))
		return 0;

	hdd_ipa = hdd_ctx->hdd_ipa;

	if (!hdd_ipa_is_enabled(hdd_ctx))
		return 0;

	schedule_work(&hdd_ipa->pm_work);

	qdf_spin_lock_bh(&hdd_ipa->pm_lock);
	hdd_ipa->suspended = false;
	qdf_spin_unlock_bh(&hdd_ipa->pm_lock);

	return 0;
}

/**
 * hdd_ipa_resume() - SSR wrapper for __hdd_ipa_resume
 * hdd_ctx: Global HDD context
 *
 * Return: 0 on success, negative errno on error
 */
int hdd_ipa_resume(hdd_context_t *hdd_ctx)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __hdd_ipa_resume(hdd_ctx);
	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * hdd_ipa_alloc_tx_desc_list() - Allocate IPA Tx desc list
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: 0 on success, negative errno on error
 */
static int hdd_ipa_alloc_tx_desc_list(struct hdd_ipa_priv *hdd_ipa)
{
	int i;
	struct hdd_ipa_tx_desc *tmp_desc;
	struct ol_txrx_pdev_t *pdev;

	pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	if (!pdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "pdev is NULL");
		return -ENODEV;
	}

	hdd_ipa->tx_desc_size = QDF_MIN(
			hdd_ipa->hdd_ctx->config->IpaMccTxDescSize,
			pdev->tx_desc.pool_size);

	INIT_LIST_HEAD(&hdd_ipa->free_tx_desc_head);

	tmp_desc = qdf_mem_malloc(sizeof(struct hdd_ipa_tx_desc) *
			hdd_ipa->tx_desc_size);

	if (!tmp_desc) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Free Tx descriptor allocation failed");
		return -ENOMEM;
	}

	hdd_ipa->tx_desc_list = tmp_desc;

	qdf_spin_lock_bh(&hdd_ipa->q_lock);
	for (i = 0; i < hdd_ipa->tx_desc_size; i++) {
		tmp_desc->id = i;
		tmp_desc->ipa_tx_desc_ptr = NULL;
		list_add_tail(&tmp_desc->link,
			      &hdd_ipa->free_tx_desc_head);
		tmp_desc++;
	}

	hdd_ipa->stats.num_tx_desc_q_cnt = 0;
	hdd_ipa->stats.num_tx_desc_error = 0;

	qdf_spin_unlock_bh(&hdd_ipa->q_lock);

	return 0;
}

/**
 * hdd_ipa_setup_sys_pipe() - Setup all IPA Sys pipes
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: 0 on success, negative errno on error
 */
static int hdd_ipa_setup_sys_pipe(struct hdd_ipa_priv *hdd_ipa)
{
	int i, ret = 0;
	struct ipa_sys_connect_params *ipa;
	uint32_t desc_fifo_sz;

	/* The maximum number of descriptors that can be provided to a BAM at
	 * once is one less than the total number of descriptors that the buffer
	 * can contain.
	 * If max_num_of_descriptors = (BAM_PIPE_DESCRIPTOR_FIFO_SIZE / sizeof
	 * (SPS_DESCRIPTOR)), then (max_num_of_descriptors - 1) descriptors can
	 * be provided at once.
	 * Because of above requirement, one extra descriptor will be added to
	 * make sure hardware always has one descriptor.
	 */
	desc_fifo_sz = hdd_ipa->hdd_ctx->config->IpaDescSize
		       + sizeof(struct sps_iovec);

	/*setup TX pipes */
	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
		ipa = &hdd_ipa->sys_pipe[i].ipa_sys_params;

		ipa->client = hdd_ipa_adapter_2_client[i].cons_client;
		ipa->desc_fifo_sz = desc_fifo_sz;
		ipa->priv = &hdd_ipa->iface_context[i];
		ipa->notify = hdd_ipa_i2w_cb;

		if (hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx)) {
			ipa->ipa_ep_cfg.hdr.hdr_len =
				HDD_IPA_UC_WLAN_TX_HDR_LEN;
			ipa->ipa_ep_cfg.nat.nat_en = IPA_BYPASS_NAT;
			ipa->ipa_ep_cfg.hdr.hdr_ofst_pkt_size_valid = 1;
			ipa->ipa_ep_cfg.hdr.hdr_ofst_pkt_size = 0;
			ipa->ipa_ep_cfg.hdr.hdr_additional_const_len =
				HDD_IPA_UC_WLAN_8023_HDR_SIZE;
			ipa->ipa_ep_cfg.hdr_ext.hdr_little_endian = true;
		} else {
			ipa->ipa_ep_cfg.hdr.hdr_len = HDD_IPA_WLAN_TX_HDR_LEN;
		}
		ipa->ipa_ep_cfg.mode.mode = IPA_BASIC;

		if (!hdd_ipa_is_rm_enabled(hdd_ipa->hdd_ctx))
			ipa->keep_ipa_awake = 1;

		ret = hdd_ipa_wdi_setup_sys_pipe(hdd_ipa, ipa,
				&(hdd_ipa->sys_pipe[i].conn_hdl));
		if (ret) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "Failed for pipe %d ret: %d", i, ret);
			goto setup_sys_pipe_fail;
		}
		if (!hdd_ipa->sys_pipe[i].conn_hdl)
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "Invalid conn handle sys_pipe: %d conn handle: %d",
				    i, hdd_ipa->sys_pipe[i].conn_hdl);
		hdd_ipa->sys_pipe[i].conn_hdl_valid = 1;
	}

	if (!hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx)) {
		/*
		 * Hard code it here, this can be extended if in case
		 * PROD pipe is also per interface.
		 * Right now there is no advantage of doing this.
		 */
		hdd_ipa->prod_client = IPA_CLIENT_WLAN1_PROD;

		ipa = &hdd_ipa->sys_pipe[HDD_IPA_RX_PIPE].ipa_sys_params;

		ipa->client = hdd_ipa->prod_client;

		ipa->desc_fifo_sz = desc_fifo_sz;
		ipa->priv = hdd_ipa;
		ipa->notify = hdd_ipa_w2i_cb;

		ipa->ipa_ep_cfg.nat.nat_en = IPA_BYPASS_NAT;
		ipa->ipa_ep_cfg.hdr.hdr_len = HDD_IPA_WLAN_RX_HDR_LEN;
		ipa->ipa_ep_cfg.hdr.hdr_ofst_metadata_valid = 1;
		ipa->ipa_ep_cfg.mode.mode = IPA_BASIC;

		if (!hdd_ipa_is_rm_enabled(hdd_ipa->hdd_ctx))
			ipa->keep_ipa_awake = 1;

		ret = hdd_ipa_wdi_setup_sys_pipe(hdd_ipa, ipa,
				&(hdd_ipa->sys_pipe[i].conn_hdl));
		if (ret) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
					"Failed for RX pipe: %d", ret);
			goto setup_sys_pipe_fail;
		}
		if (!hdd_ipa->sys_pipe[i].conn_hdl)
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "Invalid conn handle sys_pipe: %d conn handle: %d",
				    i, hdd_ipa->sys_pipe[i].conn_hdl);
		hdd_ipa->sys_pipe[HDD_IPA_RX_PIPE].conn_hdl_valid = 1;
	}

	/* Allocate free Tx desc list */
	ret = hdd_ipa_alloc_tx_desc_list(hdd_ipa);
	if (ret)
		goto setup_sys_pipe_fail;

	return ret;

setup_sys_pipe_fail:

	while (--i >= 0) {
		hdd_ipa_wdi_teardown_sys_pipe(hdd_ipa,
				hdd_ipa->sys_pipe[i].conn_hdl);
		qdf_mem_zero(&hdd_ipa->sys_pipe[i],
			     sizeof(struct hdd_ipa_sys_pipe));
	}

	return ret;
}

/**
 * hdd_ipa_teardown_sys_pipe() - Tear down all IPA Sys pipes
 * @hdd_ipa: Global HDD IPA context
 *
 * Return: None
 */
static void hdd_ipa_teardown_sys_pipe(struct hdd_ipa_priv *hdd_ipa)
{
	int ret = 0, i;
	struct hdd_ipa_tx_desc *tmp_desc;
	struct ipa_rx_data *ipa_tx_desc;

	for (i = 0; i < HDD_IPA_MAX_SYSBAM_PIPE; i++) {
		if (hdd_ipa->sys_pipe[i].conn_hdl_valid) {
			ret = hdd_ipa_wdi_teardown_sys_pipe(hdd_ipa,
					hdd_ipa->sys_pipe[i].conn_hdl);
			if (ret)
				HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "Failed: %d",
					    ret);

			hdd_ipa->sys_pipe[i].conn_hdl_valid = 0;
		}
	}

	if (hdd_ipa->tx_desc_list) {
		qdf_spin_lock_bh(&hdd_ipa->q_lock);
		for (i = 0; i < hdd_ipa->tx_desc_size; i++) {
			tmp_desc = hdd_ipa->tx_desc_list + i;
			ipa_tx_desc = tmp_desc->ipa_tx_desc_ptr;
			if (ipa_tx_desc)
				ipa_free_skb(ipa_tx_desc);
		}
		tmp_desc = hdd_ipa->tx_desc_list;
		hdd_ipa->tx_desc_list = NULL;
		hdd_ipa->stats.num_tx_desc_q_cnt = 0;
		hdd_ipa->stats.num_tx_desc_error = 0;
		qdf_spin_unlock_bh(&hdd_ipa->q_lock);
		qdf_mem_free(tmp_desc);
	}
}

/**
 * hdd_ipa_cleanup_iface() - Cleanup IPA on a given interface
 * @iface_context: interface-specific IPA context
 *
 * Return: None
 */
static void hdd_ipa_cleanup_iface(struct hdd_ipa_iface_context *iface_context)
{
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");

	if (iface_context == NULL)
		return;
	if (hdd_validate_adapter(iface_context->adapter)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "Invalid adapter: 0x%pK",
			    iface_context->adapter);
		return;
	}

	hdd_ipa_wdi_dereg_intf(iface_context->hdd_ipa,
			iface_context->adapter->dev->name);

	qdf_spin_lock_bh(&iface_context->interface_lock);
	/*
	 * Possible race condtion between supplicant and MC thread
	 * and check if the address has been already cleared by the
	 * other thread
	 */
	if (!iface_context->adapter) {
		qdf_spin_unlock_bh(&iface_context->interface_lock);
		HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO, "Already cleared");
		goto end;
	}
	iface_context->adapter->ipa_context = NULL;
	iface_context->adapter = NULL;
	iface_context->tl_context = NULL;
	iface_context->ifa_address = 0;
	qdf_spin_unlock_bh(&iface_context->interface_lock);
	if (!iface_context->hdd_ipa->num_iface) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			"NUM INTF 0, Invalid");
		QDF_ASSERT(0);
		goto end;
	}
	iface_context->hdd_ipa->num_iface--;

end:
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit: num_iface=%d",
		    iface_context->hdd_ipa->num_iface);
}

/**
 * hdd_ipa_setup_iface() - Setup IPA on a given interface
 * @hdd_ipa: HDD IPA global context
 * @adapter: Interface upon which IPA is being setup
 * @sta_id: Station ID of the API instance
 *
 * Return: 0 on success, negative errno value on error
 */
static int hdd_ipa_setup_iface(struct hdd_ipa_priv *hdd_ipa,
			       hdd_adapter_t *adapter, uint8_t sta_id)
{
	struct hdd_ipa_iface_context *iface_context = NULL;
	void *tl_context = NULL;
	int i, ret = 0;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");

	/* Lower layer may send multiple START_BSS_EVENT in DFS mode or during
	 * channel change indication. Since these indications are sent by lower
	 * layer as SAP updates and IPA doesn't have to do anything for these
	 * updates so ignoring!
	 */
	if (QDF_SAP_MODE == adapter->device_mode && adapter->ipa_context)
		return 0;

	if (HDD_IPA_MAX_IFACE == hdd_ipa->num_iface) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Max interface reached %d, Invalid",
			    HDD_IPA_MAX_IFACE);
		ret = -EINVAL;
		QDF_ASSERT(0);
		goto end;
	}

	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
		if (hdd_ipa->iface_context[i].adapter == NULL) {
			iface_context = &(hdd_ipa->iface_context[i]);
			break;
		}
	}

	if (iface_context == NULL) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "All the IPA interfaces are in use");
		ret = -ENOMEM;
		QDF_ASSERT(0);
		goto end;
	}

	adapter->ipa_context = iface_context;
	iface_context->adapter = adapter;
	iface_context->sta_id = sta_id;
	tl_context = ol_txrx_get_vdev_by_sta_id(sta_id);

	if (tl_context == NULL) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "Not able to get TL context sta_id: %d", sta_id);
		ret = -EINVAL;
		goto end;
	}

	iface_context->tl_context = tl_context;

	ret = hdd_ipa_wdi_reg_intf(hdd_ipa, iface_context);
	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"IPA WDI reg intf failed ret=%d", ret);
		goto end;
	}

	/* Register IPA Tx desc free callback */
	qdf_nbuf_reg_free_cb(hdd_ipa_nbuf_cb);

	hdd_ipa->num_iface++;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit: num_iface=%d",
		    hdd_ipa->num_iface);
	return ret;

end:
	if (iface_context)
		hdd_ipa_cleanup_iface(iface_context);
	return ret;
}

#ifndef QCA_LL_TX_FLOW_CONTROL_V2
/**
 * __hdd_ipa_send_mcc_scc_msg() - send IPA WLAN_SWITCH_TO_MCC/SCC message
 * @mcc_mode: 0=MCC/1=SCC
 *
 * Return: 0 on success, negative errno value on error
 */
static int __hdd_ipa_send_mcc_scc_msg(hdd_context_t *hdd_ctx, bool mcc_mode)
{
	hdd_adapter_list_node_t *adapter_node = NULL, *next = NULL;
	QDF_STATUS status;
	hdd_adapter_t *pAdapter;
	struct ipa_msg_meta meta;
	struct ipa_wlan_msg *msg;
	int ret;

	if (wlan_hdd_validate_context(hdd_ctx))
		return -EINVAL;

	if (!hdd_ipa_uc_sta_is_enabled(hdd_ctx))
		return -EINVAL;

	if (!hdd_ctx->mcc_mode) {
		/* Flush TxRx queue for each adapter before switch to SCC */
		status =  hdd_get_front_adapter(hdd_ctx, &adapter_node);
		while (NULL != adapter_node && QDF_STATUS_SUCCESS == status) {
			pAdapter = adapter_node->pAdapter;
			if (pAdapter->device_mode == QDF_STA_MODE ||
			    pAdapter->device_mode == QDF_SAP_MODE) {
				hdd_debug("MCC->SCC: Flush TxRx queue(d_mode=%d)",
					 pAdapter->device_mode);
				hdd_deinit_tx_rx(pAdapter);
			}
			status = hdd_get_next_adapter(
					hdd_ctx, adapter_node, &next);
			adapter_node = next;
		}
	}

	/* Send SCC/MCC Switching event to IPA */
	meta.msg_len = sizeof(*msg);
	msg = qdf_mem_malloc(meta.msg_len);
	if (msg == NULL) {
		hdd_err("msg allocation failed");
		return -ENOMEM;
	}

	meta.msg_type = mcc_mode ?
			WLAN_SWITCH_TO_MCC : WLAN_SWITCH_TO_SCC;
	hdd_debug("ipa_send_msg(Evt:%d)", meta.msg_type);

	ret = ipa_send_msg(&meta, msg, hdd_ipa_msg_free_fn);

	if (ret) {
		hdd_err("ipa_send_msg(Evt:%d) - fail=%d",
			meta.msg_type,  ret);
		qdf_mem_free(msg);
	}

	return ret;
}

/**
 * hdd_ipa_send_mcc_scc_msg() - SSR wrapper for __hdd_ipa_send_mcc_scc_msg
 * @mcc_mode: 0=MCC/1=SCC
 *
 * Return: 0 on success, negative errno value on error
 */
int hdd_ipa_send_mcc_scc_msg(hdd_context_t *hdd_ctx, bool mcc_mode)
{
	int ret;

	cds_ssr_protect(__func__);
	ret = __hdd_ipa_send_mcc_scc_msg(hdd_ctx, mcc_mode);
	cds_ssr_unprotect(__func__);

	return ret;
}

void hdd_ipa_set_mcc_mode(bool mcc_mode)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	hdd_context_t *hdd_ctx;

	if (!hdd_ipa) {
		hdd_err("hdd_ipa is NULL");
		return;
	}

	hdd_ctx = hdd_ipa->hdd_ctx;
	if (wlan_hdd_validate_context(hdd_ctx)) {
		hdd_err("invalid hdd_ctx");
		return;
	}

	if (!hdd_ipa_uc_sta_is_enabled(hdd_ctx)) {
		hdd_err("IPA UC STA not enabled");
		return;
	}

	if (mcc_mode == hdd_ctx->mcc_mode)
		return;

	hdd_ctx->mcc_mode = mcc_mode;
	schedule_work(&hdd_ipa->mcc_work);
}

static void hdd_ipa_mcc_work_handler(struct work_struct *work)
{
	struct hdd_ipa_priv *hdd_ipa;
	hdd_context_t *hdd_ctx;

	hdd_ipa = container_of(work, struct hdd_ipa_priv, mcc_work);
	hdd_ctx = hdd_ipa->hdd_ctx;

	if (wlan_hdd_validate_context(hdd_ctx)) {
		hdd_err("invalid hdd_ctx");
		return;
	}

	hdd_ipa_send_mcc_scc_msg(hdd_ctx, hdd_ctx->mcc_mode);
}
#endif

/**
 * hdd_to_ipa_wlan_event() - convert hdd_ipa_wlan_event to ipa_wlan_event
 * @hdd_ipa_event_type: HDD IPA WLAN event to be converted to an ipa_wlan_event
 *
 * Return: ipa_wlan_event representing the hdd_ipa_wlan_event
 */
static enum ipa_wlan_event
hdd_to_ipa_wlan_event(enum hdd_ipa_wlan_event hdd_ipa_event_type)
{
	enum ipa_wlan_event ipa_event;

	switch (hdd_ipa_event_type) {
	case HDD_IPA_CLIENT_CONNECT:
		ipa_event = WLAN_CLIENT_CONNECT;
		break;
	case HDD_IPA_CLIENT_DISCONNECT:
		ipa_event = WLAN_CLIENT_DISCONNECT;
		break;
	case HDD_IPA_AP_CONNECT:
		ipa_event = WLAN_AP_CONNECT;
		break;
	case HDD_IPA_AP_DISCONNECT:
		ipa_event = WLAN_AP_DISCONNECT;
		break;
	case HDD_IPA_STA_CONNECT:
		ipa_event = WLAN_STA_CONNECT;
		break;
	case HDD_IPA_STA_DISCONNECT:
		ipa_event = WLAN_STA_DISCONNECT;
		break;
	case HDD_IPA_CLIENT_CONNECT_EX:
		ipa_event = WLAN_CLIENT_CONNECT_EX;
		break;
	case HDD_IPA_WLAN_EVENT_MAX:
	default:
		ipa_event = IPA_WLAN_EVENT_MAX;
		break;
	}
	return ipa_event;

}

/**
 * __hdd_ipa_wlan_evt() - IPA event handler
 * @adapter: adapter upon which the event was received
 * @sta_id: station id for the event
 * @type: event enum of type ipa_wlan_event
 * @mac_address: MAC address associated with the event
 *
 * This function is meant to be called from within wlan_hdd_ipa.c
 *
 * Return: 0 on success, negative errno value on error
 */
static int __hdd_ipa_wlan_evt(hdd_adapter_t *adapter, uint8_t sta_id,
		     enum ipa_wlan_event type, uint8_t *mac_addr)
{
	struct hdd_ipa_priv *hdd_ipa = ghdd_ipa;
	struct ipa_msg_meta meta;
	struct ipa_wlan_msg *msg;
	struct ipa_wlan_msg_ex *msg_ex = NULL;
	int ret = 0;

	if (hdd_validate_adapter(adapter)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "Invalid adapter: 0x%pK",
			    adapter);
		return -EINVAL;
	}

	HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO, "%s: EVT: %s, MAC: %pM sta_id: %d",
		    adapter->dev->name, hdd_ipa_wlan_event_to_str(type),
		    mac_addr, sta_id);

	if (type >= IPA_WLAN_EVENT_MAX)
		return -EINVAL;

	if (WARN_ON(is_zero_ether_addr(mac_addr)))
		return -EINVAL;

	if (!hdd_ipa || !hdd_ipa_is_enabled(hdd_ipa->hdd_ctx)) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "IPA OFFLOAD NOT ENABLED");
		return -EINVAL;
	}

	if (hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx) &&
		!hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx) &&
		(QDF_SAP_MODE != adapter->device_mode)) {
		return 0;
	}

	/*
	 * During IPA UC resource loading/unloading new events can be issued.
	 */
	if (hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx) &&
	    (hdd_ipa->resource_loading || hdd_ipa->resource_unloading)) {
		unsigned int pending_event_count;
		struct ipa_uc_pending_event *pending_event = NULL;

		HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
			    "%s:IPA resource %s inprogress",
			    hdd_ipa_wlan_event_to_str(type),
			    hdd_ipa->resource_loading ?
			    "load" : "unload");

		/* Wait until completion of the long/unloading */
		ret = wait_for_completion_timeout(&hdd_ipa->ipa_resource_comp,
				msecs_to_jiffies(IPA_RESOURCE_COMP_WAIT_TIME));
		if (!ret) {
			/*
			 * If timed out, store the events separately and
			 * handle them later.
			 */
			HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
				    "IPA resource %s timed out",
				    hdd_ipa->resource_loading ?
				    "load" : "unload");

			qdf_mutex_acquire(&hdd_ipa->ipa_lock);

			pending_event_count =
				qdf_list_size(&hdd_ipa->pending_event);
			if (pending_event_count >=
					HDD_IPA_MAX_PENDING_EVENT_COUNT) {
				hdd_debug(
						"Reached max pending event count");
				qdf_list_remove_front(
						&hdd_ipa->pending_event,
						(qdf_list_node_t **)&pending_event);
			} else {
				pending_event =
					qdf_mem_malloc(
							sizeof(*pending_event));
			}

			if (!pending_event) {
				HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
					    "Pending event memory alloc fail");
				qdf_mutex_release(&hdd_ipa->ipa_lock);
				return -ENOMEM;
			}

			pending_event->adapter = adapter;
			pending_event->sta_id = sta_id;
			pending_event->type = type;
			pending_event->is_loading =
				hdd_ipa->resource_loading;
			qdf_mem_copy(pending_event->mac_addr,
					mac_addr, QDF_MAC_ADDR_SIZE);
			qdf_list_insert_back(&hdd_ipa->pending_event,
					&pending_event->node);

			qdf_mutex_release(&hdd_ipa->ipa_lock);

			/* Cleanup interface */
			if (type == WLAN_STA_DISCONNECT ||
			    type == WLAN_AP_DISCONNECT)
				hdd_ipa_cleanup_iface(adapter->ipa_context);

			return 0;
		}
		HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO,
			    "IPA resource %s completed",
			    hdd_ipa->resource_loading ?
			    "load" : "unload");
	}

	hdd_ipa->stats.event[type]++;

	meta.msg_type = type;
	switch (type) {
	case WLAN_STA_CONNECT:
		qdf_mutex_acquire(&hdd_ipa->event_lock);

		/* STA already connected and without disconnect, connect again
		 * This is Roaming scenario
		 */
		if (hdd_ipa->sta_connected)
			hdd_ipa_cleanup_iface(adapter->ipa_context);

		ret = hdd_ipa_setup_iface(hdd_ipa, adapter, sta_id);
		if (ret) {
			qdf_mutex_release(&hdd_ipa->event_lock);
			goto end;
		}

		if (hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx) &&
		    (hdd_ipa->sap_num_connected_sta > 0 ||
		     hdd_ipa_uc_sta_only_offload_is_enabled()) &&
		    !hdd_ipa->sta_connected) {
			qdf_mutex_release(&hdd_ipa->event_lock);
			hdd_ipa_uc_offload_enable_disable(adapter,
				SIR_STA_RX_DATA_OFFLOAD, true);
			qdf_mutex_acquire(&hdd_ipa->event_lock);
		}

		if (!hdd_ipa_uc_sta_only_offload_is_enabled()) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
					"IPA uC STA only offload not enabled");
		} else if (!hdd_ipa->sap_num_connected_sta &&
				!hdd_ipa->sta_connected) {
			ret = hdd_ipa_uc_handle_first_con(hdd_ipa);
			if (ret) {
				qdf_mutex_release(&hdd_ipa->event_lock);
				HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
						"handle 1st conn ret %d", ret);
				hdd_ipa_uc_offload_enable_disable(adapter,
						SIR_STA_RX_DATA_OFFLOAD, false);
				goto end;
			}
		}

		hdd_ipa->vdev_to_iface[adapter->sessionId] =
			((struct hdd_ipa_iface_context *)
			(adapter->ipa_context))->iface_id;

		hdd_ipa->sta_connected = 1;

		qdf_mutex_release(&hdd_ipa->event_lock);

		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "sta_connected=%d",
			    hdd_ipa->sta_connected);
		break;

	case WLAN_AP_CONNECT:
		qdf_mutex_acquire(&hdd_ipa->event_lock);

		/* For DFS channel we get two start_bss event (before and after
		 * CAC). Also when ACS range includes both DFS and non DFS
		 * channels, we could possibly change channel many times due to
		 * RADAR detection and chosen channel may not be a DFS channels.
		 * So dont return error here. Just discard the event.
		 */
		if (adapter->ipa_context) {
			qdf_mutex_release(&hdd_ipa->event_lock);
			return 0;
		}

		ret = hdd_ipa_setup_iface(hdd_ipa, adapter, sta_id);
		if (ret) {
			hdd_err("%s: Evt: %d, Interface setup failed",
				msg_ex->name, meta.msg_type);
			qdf_mutex_release(&hdd_ipa->event_lock);
			goto end;
		}

		if (hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
			qdf_mutex_release(&hdd_ipa->event_lock);
			hdd_ipa_uc_offload_enable_disable(adapter,
				SIR_AP_RX_DATA_OFFLOAD, true);
			qdf_mutex_acquire(&hdd_ipa->event_lock);
		}

		hdd_ipa->vdev_to_iface[adapter->sessionId] =
			((struct hdd_ipa_iface_context *)
			(adapter->ipa_context))->iface_id;

		qdf_mutex_release(&hdd_ipa->event_lock);
		break;

	case WLAN_STA_DISCONNECT:
		qdf_mutex_acquire(&hdd_ipa->event_lock);

		if (!hdd_ipa->sta_connected) {
			hdd_err("%s: Evt: %d, STA already disconnected",
				msg_ex->name, meta.msg_type);
			qdf_mutex_release(&hdd_ipa->event_lock);
			return -EINVAL;
		}

		hdd_ipa->sta_connected = 0;

		if (!hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
			hdd_debug("%s: IPA UC OFFLOAD NOT ENABLED",
				msg_ex->name);
		} else {
			/*
			 * Disable IPA UC TX PIPE when
			 * 1. STA is the last interface, Or
			 * 2. STA only offload enabled and no clients connected
			 * to SAP
			 */
			if (((1 == hdd_ipa->num_iface) ||
				(hdd_ipa_uc_sta_only_offload_is_enabled() &&
				 !hdd_ipa->sap_num_connected_sta)) &&
			    hdd_ipa_is_fw_wdi_actived(hdd_ipa->hdd_ctx) &&
			    !hdd_ipa->ipa_pipes_down)
				hdd_ipa_uc_handle_last_discon(hdd_ipa);
		}

		if (hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx) &&
		    (hdd_ipa->sap_num_connected_sta > 0 ||
		     hdd_ipa_uc_sta_only_offload_is_enabled())) {
			qdf_mutex_release(&hdd_ipa->event_lock);
			hdd_ipa_uc_offload_enable_disable(adapter,
				SIR_STA_RX_DATA_OFFLOAD, false);
			qdf_mutex_acquire(&hdd_ipa->event_lock);
			hdd_ipa->vdev_to_iface[adapter->sessionId] =
				CSR_ROAM_SESSION_MAX;
		}

		hdd_ipa_cleanup_iface(adapter->ipa_context);

		qdf_mutex_release(&hdd_ipa->event_lock);

		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "sta_connected=%d",
			    hdd_ipa->sta_connected);
		break;

	case WLAN_AP_DISCONNECT:
		qdf_mutex_acquire(&hdd_ipa->event_lock);

		if (!adapter->ipa_context) {
			hdd_err("%s: Evt: %d, SAP already disconnected",
				msg_ex->name, meta.msg_type);
			qdf_mutex_release(&hdd_ipa->event_lock);
			return -EINVAL;
		}

		if ((1 == hdd_ipa->num_iface) &&
		    hdd_ipa_is_fw_wdi_actived(hdd_ipa->hdd_ctx) &&
		    !hdd_ipa->ipa_pipes_down) {
			if (cds_is_driver_unloading()) {
				/*
				 * We disable WDI pipes directly here since
				 * IPA_OPCODE_TX/RX_SUSPEND message will not be
				 * processed when unloading WLAN driver is in
				 * progress
				 */
				hdd_ipa_uc_disable_pipes(hdd_ipa);
			} else {
				/*
				 * This shouldn't happen :
				 * No interface left but WDI pipes are still
				 * active - force close WDI pipes
				 */
				WARN_ON(1);
				HDD_IPA_LOG(QDF_TRACE_LEVEL_WARN,
					"No interface left but WDI pipes are still active - force close WDI pipes");

				hdd_ipa_uc_handle_last_discon(hdd_ipa);
			}
		}

		if (hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
			qdf_mutex_release(&hdd_ipa->event_lock);
			hdd_ipa_uc_offload_enable_disable(adapter,
				SIR_AP_RX_DATA_OFFLOAD, false);
			qdf_mutex_acquire(&hdd_ipa->event_lock);
			hdd_ipa->vdev_to_iface[adapter->sessionId] =
				CSR_ROAM_SESSION_MAX;
		}

		hdd_ipa_cleanup_iface(adapter->ipa_context);

		qdf_mutex_release(&hdd_ipa->event_lock);
		break;

	case WLAN_CLIENT_CONNECT_EX:
		if (!hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
				"%s: Evt: %d, IPA UC OFFLOAD NOT ENABLED",
				adapter->dev->name, type);
			return 0;
		}

		qdf_mutex_acquire(&hdd_ipa->event_lock);
		if (hdd_ipa_uc_find_add_assoc_sta(hdd_ipa,
				true, sta_id)) {
			qdf_mutex_release(&hdd_ipa->event_lock);
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				"%s: STA ID %d found, not valid",
				adapter->dev->name, sta_id);
			return 0;
		}

		/* Enable IPA UC Data PIPEs when first STA connected */
		if (hdd_ipa->sap_num_connected_sta == 0 &&
				hdd_ipa->uc_loaded == true) {
			if (hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx) &&
			    hdd_ipa->sta_connected &&
			    !hdd_ipa_uc_sta_only_offload_is_enabled()) {
				qdf_mutex_release(&hdd_ipa->event_lock);
				hdd_ipa_uc_offload_enable_disable(
					hdd_get_adapter(hdd_ipa->hdd_ctx,
							QDF_STA_MODE),
					SIR_STA_RX_DATA_OFFLOAD, true);
				qdf_mutex_acquire(&hdd_ipa->event_lock);
			}

			/*
			 * IPA pipes already enabled if STA only offload
			 * is enabled and STA is connected.
			 */
			if (hdd_ipa_uc_sta_only_offload_is_enabled() &&
					hdd_ipa->sta_connected) {
				HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
						"IPA pipes already enabled");
			} else if (hdd_ipa_uc_handle_first_con(hdd_ipa)) {
				HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
					    "%s: handle 1st con ret %d",
					    adapter->dev->name, ret);

				if (hdd_ipa_uc_sta_is_enabled(
					hdd_ipa->hdd_ctx) &&
				    hdd_ipa->sta_connected) {
					qdf_mutex_release(&hdd_ipa->event_lock);
					hdd_ipa_uc_offload_enable_disable(
						hdd_get_adapter(
							hdd_ipa->hdd_ctx,
							QDF_STA_MODE),
						SIR_STA_RX_DATA_OFFLOAD, false);
				} else {
					qdf_mutex_release(&hdd_ipa->event_lock);
				}

				return -EPERM;
			}
		}

		hdd_ipa->sap_num_connected_sta++;

		qdf_mutex_release(&hdd_ipa->event_lock);

		meta.msg_type = type;
		meta.msg_len = (sizeof(struct ipa_wlan_msg_ex) +
				sizeof(struct ipa_wlan_hdr_attrib_val));
		msg_ex = qdf_mem_malloc(meta.msg_len);

		if (msg_ex == NULL) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "msg_ex allocation failed");
			return -ENOMEM;
		}
		strlcpy(msg_ex->name, adapter->dev->name,
			IPA_RESOURCE_NAME_MAX);
		msg_ex->num_of_attribs = 1;
		msg_ex->attribs[0].attrib_type = WLAN_HDR_ATTRIB_MAC_ADDR;
		if (hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
			msg_ex->attribs[0].offset =
				HDD_IPA_UC_WLAN_HDR_DES_MAC_OFFSET;
		} else {
			msg_ex->attribs[0].offset =
				HDD_IPA_WLAN_HDR_DES_MAC_OFFSET;
		}
		memcpy(msg_ex->attribs[0].u.mac_addr, mac_addr,
		       IPA_MAC_ADDR_SIZE);

		ret = ipa_send_msg(&meta, msg_ex, hdd_ipa_msg_free_fn);

		if (ret) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "%s: Evt: %d : %d",
				    adapter->dev->name, type, ret);
			qdf_mem_free(msg_ex);
			return ret;
		}
		hdd_ipa->stats.num_send_msg++;

		HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO, "sap_num_connected_sta=%d",
			    hdd_ipa->sap_num_connected_sta);
		return ret;

	case WLAN_CLIENT_DISCONNECT:
		if (!hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
					"%s: IPA UC OFFLOAD NOT ENABLED",
					msg_ex->name);
			return 0;
		}
		qdf_mutex_acquire(&hdd_ipa->event_lock);
		if (!hdd_ipa->sap_num_connected_sta) {
			hdd_err("%s: Evt: %d, Client already disconnected",
				msg_ex->name, meta.msg_type);
			qdf_mutex_release(&hdd_ipa->event_lock);
			return 0;
		}
		if (!hdd_ipa_uc_find_add_assoc_sta(hdd_ipa, false, sta_id)) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
				    "%s: STA ID %d NOT found, not valid",
				    msg_ex->name, sta_id);
			qdf_mutex_release(&hdd_ipa->event_lock);
			return 0;
		}
		hdd_ipa->sap_num_connected_sta--;

		/*
		 * Disable IPA UC TX PIPE when
		 * 1. last client disconnected SAP and
		 * 2. STA is not connected
		 */
		if (!hdd_ipa->sap_num_connected_sta &&
				(hdd_ipa->uc_loaded == true) &&
				!(hdd_ipa_uc_sta_only_offload_is_enabled() &&
					hdd_ipa->sta_connected)) {
			if ((false == hdd_ipa->resource_unloading) &&
			    hdd_ipa_is_fw_wdi_actived(hdd_ipa->hdd_ctx) &&
			    !hdd_ipa->ipa_pipes_down) {
				hdd_ipa_uc_handle_last_discon(hdd_ipa);
			}

			if (hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx) &&
			    hdd_ipa->sta_connected) {
				qdf_mutex_release(&hdd_ipa->event_lock);
				hdd_ipa_uc_offload_enable_disable(
					hdd_get_adapter(hdd_ipa->hdd_ctx,
							QDF_STA_MODE),
					SIR_STA_RX_DATA_OFFLOAD, false);
			} else {
				qdf_mutex_release(&hdd_ipa->event_lock);
			}
		} else {
			qdf_mutex_release(&hdd_ipa->event_lock);
		}

		HDD_IPA_LOG(QDF_TRACE_LEVEL_INFO, "sap_num_connected_sta=%d",
			    hdd_ipa->sap_num_connected_sta);
		break;

	default:
		return 0;
	}

	meta.msg_len = sizeof(struct ipa_wlan_msg);
	msg = qdf_mem_malloc(meta.msg_len);
	if (msg == NULL) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR, "msg allocation failed");
		return -ENOMEM;
	}

	meta.msg_type = type;
	strlcpy(msg->name, adapter->dev->name, IPA_RESOURCE_NAME_MAX);
	memcpy(msg->mac_addr, mac_addr, ETH_ALEN);

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "%s: Evt: %d",
		    msg->name, meta.msg_type);

	ret = ipa_send_msg(&meta, msg, hdd_ipa_msg_free_fn);

	if (ret) {
		hdd_err("%s: Evt: %d fail:%d",
			msg->name, meta.msg_type, ret);
		qdf_mem_free(msg);
		return ret;
	}

	hdd_ipa->stats.num_send_msg++;

end:
	return ret;
}

/**
 * hdd_ipa_wlan_evt() - SSR wrapper for __hdd_ipa_wlan_evt
 * @adapter: adapter upon which the event was received
 * @sta_id: station id for the event
 * @hdd_event_type: event enum of type hdd_ipa_wlan_event
 * @mac_address: MAC address associated with the event
 *
 * This function is meant to be called from outside of wlan_hdd_ipa.c.
 *
 * Return: 0 on success, negative errno value on error
 */
int hdd_ipa_wlan_evt(hdd_adapter_t *adapter, uint8_t sta_id,
	enum hdd_ipa_wlan_event hdd_event_type, uint8_t *mac_addr)
{
	enum ipa_wlan_event type = hdd_to_ipa_wlan_event(hdd_event_type);
	int ret = 0;

	cds_ssr_protect(__func__);

	/* Data path offload only support for STA and SAP mode */
	if ((QDF_STA_MODE == adapter->device_mode) ||
	    (QDF_SAP_MODE == adapter->device_mode))
		ret = __hdd_ipa_wlan_evt(adapter, sta_id, type, mac_addr);

	cds_ssr_unprotect(__func__);

	return ret;
}

/**
 * hdd_ipa_uc_proc_pending_event() - Process IPA uC pending events
 * @hdd_ipa: Global HDD IPA context
 * @is_loading: Indicate if invoked during loading
 *
 * Return: None
 */
static void
hdd_ipa_uc_proc_pending_event(struct hdd_ipa_priv *hdd_ipa, bool is_loading)
{
	unsigned int pending_event_count;
	struct ipa_uc_pending_event *pending_event = NULL;

	pending_event_count = qdf_list_size(&hdd_ipa->pending_event);
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
		"Pending Event Count %d",  pending_event_count);
	if (!pending_event_count) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG,
			"No Pending Event");
		return;
	}

	qdf_list_remove_front(&hdd_ipa->pending_event,
			(qdf_list_node_t **)&pending_event);
	while (pending_event != NULL) {
		if (pending_event->is_loading == is_loading &&
		    hdd_is_adapter_valid(hdd_ipa->hdd_ctx,
					 pending_event->adapter) &&
		    !hdd_validate_adapter(pending_event->adapter)) {
			__hdd_ipa_wlan_evt(pending_event->adapter,
					pending_event->sta_id,
					pending_event->type,
					pending_event->mac_addr);
		}
		qdf_mem_free(pending_event);
		pending_event = NULL;
		qdf_list_remove_front(&hdd_ipa->pending_event,
			(qdf_list_node_t **)&pending_event);
	}
}

/**
 * hdd_ipa_rm_state_to_str() - Convert IPA RM state to string
 * @state: IPA RM state value
 *
 * Return: ASCII string representing the IPA RM state
 */
static inline char *hdd_ipa_rm_state_to_str(enum hdd_ipa_rm_state state)
{
	switch (state) {
	case HDD_IPA_RM_RELEASED:
		return "RELEASED";
	case HDD_IPA_RM_GRANT_PENDING:
		return "GRANT_PENDING";
	case HDD_IPA_RM_GRANTED:
		return "GRANTED";
	}

	return "UNKNOWN";
}

/**
 * __hdd_ipa_init() - IPA initialization function
 * @hdd_ctx: HDD global context
 *
 * Allocate hdd_ipa resources, ipa pipe resource and register
 * wlan interface with IPA module.
 *
 * Return: QDF_STATUS enumeration
 */
static QDF_STATUS __hdd_ipa_init(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa = NULL;
	int ret, i;
	struct hdd_ipa_iface_context *iface_context = NULL;
	struct ol_txrx_pdev_t *pdev = NULL;

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "enter");

	if (!hdd_ipa_is_enabled(hdd_ctx))
		return QDF_STATUS_SUCCESS;

	pdev = cds_get_context(QDF_MODULE_ID_TXRX);
	if (!pdev) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL, "pdev is NULL");
		goto fail_return;
	}

	hdd_ipa = qdf_mem_malloc(sizeof(*hdd_ipa));
	if (!hdd_ipa) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_FATAL, "hdd_ipa allocation failed");
		goto fail_return;
	}

	hdd_ctx->hdd_ipa = hdd_ipa;
	ghdd_ipa = hdd_ipa;
	hdd_ipa->hdd_ctx = hdd_ctx;
	hdd_ipa->num_iface = 0;

	hdd_ipa_wdi_get_wdi_version(hdd_ipa);

	/* Create the interface context */
	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
		iface_context = &hdd_ipa->iface_context[i];
		iface_context->hdd_ipa = hdd_ipa;
		iface_context->cons_client =
			hdd_ipa_adapter_2_client[i].cons_client;
		iface_context->prod_client =
			hdd_ipa_adapter_2_client[i].prod_client;
		iface_context->iface_id = i;
		iface_context->adapter = NULL;
		qdf_spinlock_create(&iface_context->interface_lock);
	}

	INIT_WORK(&hdd_ipa->pm_work, hdd_ipa_pm_flush);
	qdf_spinlock_create(&hdd_ipa->pm_lock);
	qdf_spinlock_create(&hdd_ipa->q_lock);
	qdf_nbuf_queue_init(&hdd_ipa->pm_queue_head);
	qdf_list_create(&hdd_ipa->pending_event, 1000);
	qdf_mutex_create(&hdd_ipa->event_lock);
	qdf_mutex_create(&hdd_ipa->ipa_lock);

	ret = hdd_ipa_wdi_setup_rm(hdd_ipa);
	if (ret)
		goto fail_setup_rm;

	if (hdd_ipa_uc_is_enabled(hdd_ipa->hdd_ctx)) {
		hdd_ipa_uc_rt_debug_init(hdd_ctx);
		qdf_mem_zero(&hdd_ipa->stats, sizeof(hdd_ipa->stats));
		hdd_ipa->sap_num_connected_sta = 0;
		hdd_ipa->ipa_tx_packets_diff = 0;
		hdd_ipa->ipa_rx_packets_diff = 0;
		hdd_ipa->ipa_p_tx_packets = 0;
		hdd_ipa->ipa_p_rx_packets = 0;
		hdd_ipa->resource_loading = false;
		hdd_ipa->resource_unloading = false;
		hdd_ipa->sta_connected = 0;
		hdd_ipa->ipa_pipes_down = true;
		hdd_ipa->wdi_enabled = false;

		ret = hdd_ipa_wdi_init(hdd_ipa);
		if (ret) {
			HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
					"ipa wdi init failed ret=%d", ret);
			if (ret == -EACCES) {
				if (hdd_ipa_uc_send_wdi_control_msg(false))
					goto ipa_wdi_destroy;
			} else {
				goto ipa_wdi_destroy;
			}
		} else {
			/* Setup IPA sys_pipe for MCC */
			if (hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx)) {
				ret = hdd_ipa_setup_sys_pipe(hdd_ipa);
				if (ret)
					goto ipa_wdi_destroy;

				INIT_WORK(&hdd_ipa->mcc_work,
					  hdd_ipa_mcc_work_handler);
			}
		}
	} else {
		ret = hdd_ipa_setup_sys_pipe(hdd_ipa);
		if (ret)
			goto ipa_wdi_destroy;
	}

	init_completion(&hdd_ipa->ipa_resource_comp);

	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit: success");
	return QDF_STATUS_SUCCESS;

ipa_wdi_destroy:
	hdd_ipa_wdi_destroy_rm(hdd_ipa);
fail_setup_rm:
	qdf_spinlock_destroy(&hdd_ipa->pm_lock);
	qdf_mem_free(hdd_ipa);
	hdd_ctx->hdd_ipa = NULL;
	ghdd_ipa = NULL;
fail_return:
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "exit: fail");
	return QDF_STATUS_E_FAILURE;
}

/**
 * hdd_ipa_init() - SSR wrapper for __hdd_ipa_init
 * @hdd_ctx: HDD global context
 *
 * Allocate hdd_ipa resources, ipa pipe resource and register
 * wlan interface with IPA module.
 *
 * Return: QDF_STATUS enumeration
 */
QDF_STATUS hdd_ipa_init(hdd_context_t *hdd_ctx)
{
	QDF_STATUS ret;

	cds_ssr_protect(__func__);
	ret = __hdd_ipa_init(hdd_ctx);
	cds_ssr_unprotect(__func__);

	return ret;
}


/**
 * __hdd_ipa_flush - flush IPA exception path SKB's
 * @hdd_ctx: HDD global context
 *
 * Return: none
 */
static void __hdd_ipa_flush(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa = hdd_ctx->hdd_ipa;
	qdf_nbuf_t skb;
	struct hdd_ipa_pm_tx_cb *pm_tx_cb = NULL;

	if (!hdd_ipa_is_enabled(hdd_ctx))
		return;

	cancel_work_sync(&hdd_ipa->pm_work);
	qdf_spin_lock_bh(&hdd_ipa->pm_lock);

	while (((skb = qdf_nbuf_queue_remove(&hdd_ipa->pm_queue_head))
								!= NULL)) {
		qdf_spin_unlock_bh(&hdd_ipa->pm_lock);

		pm_tx_cb = (struct hdd_ipa_pm_tx_cb *)skb->cb;
		if (pm_tx_cb->exception) {
			dev_kfree_skb_any(skb);
		} else {
			if (pm_tx_cb->ipa_tx_desc)
				ipa_free_skb(pm_tx_cb->ipa_tx_desc);
		}

		qdf_spin_lock_bh(&hdd_ipa->pm_lock);
	}
	qdf_spin_unlock_bh(&hdd_ipa->pm_lock);

}

/**
 * __hdd_ipa_cleanup - IPA cleanup function
 * @hdd_ctx: HDD global context
 *
 * Return: QDF_STATUS enumeration
 */
static QDF_STATUS __hdd_ipa_cleanup(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa = hdd_ctx->hdd_ipa;
	int i;
	struct hdd_ipa_iface_context *iface_context = NULL;

	if (!hdd_ipa_is_enabled(hdd_ctx))
		return QDF_STATUS_SUCCESS;

	if (!hdd_ipa_uc_is_enabled(hdd_ctx)) {
		unregister_inetaddr_notifier(&hdd_ipa->ipv4_notifier);
		hdd_ipa_teardown_sys_pipe(hdd_ipa);
	}

	/* Teardown IPA sys_pipe for MCC */
	if (hdd_ipa_uc_sta_is_enabled(hdd_ipa->hdd_ctx)) {
		hdd_ipa_teardown_sys_pipe(hdd_ipa);
		cancel_work_sync(&hdd_ipa->mcc_work);
	}

	hdd_ipa_wdi_destroy_rm(hdd_ipa);

	__hdd_ipa_flush(hdd_ctx);

	qdf_spinlock_destroy(&hdd_ipa->pm_lock);
	qdf_spinlock_destroy(&hdd_ipa->q_lock);

	/* destory the interface lock */
	for (i = 0; i < HDD_IPA_MAX_IFACE; i++) {
		iface_context = &hdd_ipa->iface_context[i];
		qdf_spinlock_destroy(&iface_context->interface_lock);
	}

	if (hdd_ipa_uc_is_enabled(hdd_ctx)) {
		hdd_ipa_wdi_cleanup();
		hdd_ipa_uc_rt_debug_deinit(hdd_ctx);
		qdf_mutex_destroy(&hdd_ipa->event_lock);
		qdf_mutex_destroy(&hdd_ipa->ipa_lock);
		qdf_list_destroy(&hdd_ipa->pending_event);

		for (i = 0; i < HDD_IPA_UC_OPCODE_MAX; i++) {
			cancel_work_sync(&hdd_ipa->uc_op_work[i].work);
			qdf_mem_free(hdd_ipa->uc_op_work[i].msg);
			hdd_ipa->uc_op_work[i].msg = NULL;
		}
	}

	qdf_mem_free(hdd_ipa);
	hdd_ctx->hdd_ipa = NULL;

	return QDF_STATUS_SUCCESS;
}

/**
 * hdd_ipa_cleanup - SSR wrapper for __hdd_ipa_flush
 * @hdd_ctx: HDD global context
 *
 * Return: None
 */
void hdd_ipa_flush(hdd_context_t *hdd_ctx)
{
	cds_ssr_protect(__func__);
	__hdd_ipa_flush(hdd_ctx);
	cds_ssr_unprotect(__func__);
}

/**
 * hdd_ipa_cleanup - SSR wrapper for __hdd_ipa_cleanup
 * @hdd_ctx: HDD global context
 *
 * Return: QDF_STATUS enumeration
 */
QDF_STATUS hdd_ipa_cleanup(hdd_context_t *hdd_ctx)
{
	QDF_STATUS ret;

	cds_ssr_protect(__func__);
	ret = __hdd_ipa_cleanup(hdd_ctx);
	cds_ssr_unprotect(__func__);

	return ret;
}

void hdd_ipa_clean_adapter_iface(hdd_adapter_t *adapter)
{
	struct hdd_ipa_iface_context *iface_ctx = adapter->ipa_context;

	if (iface_ctx)
		hdd_ipa_cleanup_iface(iface_ctx);
}

void hdd_ipa_fw_rejuvenate_send_msg(hdd_context_t *hdd_ctx)
{
	struct hdd_ipa_priv *hdd_ipa;
	struct ipa_msg_meta meta;
	struct ipa_wlan_msg *msg;
	int ret;

	hdd_ipa = hdd_ctx->hdd_ipa;
	meta.msg_len = sizeof(*msg);
	msg = qdf_mem_malloc(meta.msg_len);
	if (!msg) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "msg allocation failed");
		return;
	}
	meta.msg_type = WLAN_FWR_SSR_BEFORE_SHUTDOWN;
	HDD_IPA_LOG(QDF_TRACE_LEVEL_DEBUG, "ipa_send_msg(Evt:%d)",
		    meta.msg_type);
	ret = ipa_send_msg(&meta, msg, hdd_ipa_msg_free_fn);

	if (ret) {
		HDD_IPA_LOG(QDF_TRACE_LEVEL_ERROR,
			    "ipa_send_msg(Evt:%d)-fail=%d",
			    meta.msg_type, ret);
		qdf_mem_free(msg);
	}
	hdd_ipa->stats.num_send_msg++;
}

#endif /* IPA_OFFLOAD */
