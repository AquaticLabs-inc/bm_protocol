#include "bm_l2.h"
#include "bm_config.h"
#include "eth_adin2111.h"
#include "lwip/ethip6.h"
#include "lwip/snmp.h"
#include "semphr.h"

#define IFNAME0                     'b'
#define IFNAME1                     'm'
#define NETIF_LINK_SPEED_IN_BPS     10000000
#define ETHERNET_MTU                1500

struct bm_netdev_ctx_t {
    uint8_t num_ports;
    void* device_handle;
    uint8_t start_port_idx;
    bm_netdev_type_t type;
};

typedef struct l2_queue_element_t {
    void* device_handle;
    uint8_t port_mask;
    uint16_t payload_len;
    uint8_t payload[ETHERNET_MTU];
} __attribute__((packed)) l2_queue_element_t;

static struct netif* net_if;
static struct bm_netdev_ctx_t devices[BM_NETDEV_COUNT];

/* TODO: We are only supporting the ADIN2111 net device. Currently assuming we have up to BM_NETDEV_TYPE_MAX ADIN2111s.
   We want to eventually support new net devices and pre-allocate here before giving to init function */
static adin2111_DeviceStruct_t adin_devices[BM_NETDEV_TYPE_MAX];
static uint8_t adin_device_counter = 0;

static uint8_t available_ports_mask = 0;
static uint8_t available_port_mask_idx = 0;
static TaskHandle_t rx_thread = NULL;
static TaskHandle_t tx_thread = NULL;
static QueueHandle_t  rx_queue, tx_queue;
static SemaphoreHandle_t tx_sem;

/* TODO: ADIN2111-specifc, let's move to ADIN driver.
   Rx Callback can only get the MAC Handle, not the Device handle itself */
static int bm_l2_get_device_index(void* device_handle) {
    int idx;

    for (idx=0; idx<BM_NETDEV_TYPE_MAX; idx++) {
        if (device_handle == ((adin2111_DeviceHandle_t) devices[idx].device_handle)){
            return idx;
        }
    }

    return -1;
}

/* L2 RX Thread */
static void bm_l2_rx_thread(void *parameters) {
    (void) parameters;
    static l2_queue_element_t rx_data;
    uint8_t new_port_mask = 0;
    uint8_t rx_port_mask = 0;
    uint8_t device_idx = 0;
    bool is_global_multicast = true;

    while (1) {
        if(xQueueReceive(rx_queue, &rx_data, portMAX_DELAY) == pdPASS) {

            struct pbuf* p = pbuf_alloc(PBUF_RAW, rx_data.payload_len, PBUF_RAM);
            if (p == NULL) {
                printf("No mem for pbuf in RX pathway\n");
                continue;
            }

            /* Copy over RX packet contents to a pBuf to submit to lwip */
            memcpy(((uint8_t*) p->payload) , rx_data.payload, rx_data.payload_len);

            /* TODO: Check if global multicast message. Re-TX on every other port */
            if (is_global_multicast)
            {
                device_idx = bm_l2_get_device_index(rx_data.device_handle);
                switch (devices[device_idx].type) {
                    case BM_NETDEV_TYPE_ADIN2111:
                        rx_port_mask = ((rx_data.port_mask & ADIN2111_PORT_MASK) << devices[device_idx].start_port_idx);
                        break;
                    case BM_NETDEV_TYPE_NONE:
                    default:
                        /* No device or not supported. How did we get here? */
                        configASSERT(0);
                        rx_port_mask = 0;
                        break;
                }
                new_port_mask = available_ports_mask & ~(rx_port_mask);
                bm_l2_tx(p, new_port_mask);
            }

            /* TODO: This is the place where routing and filtering functions would happen, to prevent passing the
               packet to net_if->input() if unnecessary, as well as forwarding routed multicast data to interested
               neighbors and user devices. */

            /* Submit packet to lwip. User RX Callback is responsible for freeing the packet*/
            if (net_if->input(p, net_if) != ERR_OK) {
                // LWIP_DEBUGF(NETIF_DEBUG, ("IP input error\r\n"));
                pbuf_free(p);
                p = NULL;
            }
        }
    }
}

/* L2 TX Thread */
static void bm_l2_tx_thread(void *parameters) {
    (void) parameters;
    static l2_queue_element_t tx_data;

    int idx;
    uint8_t mask_idx;
    err_t retv;

    while (1) {
        if(xQueueReceive(tx_queue, &tx_data, portMAX_DELAY) == pdPASS) {
            mask_idx = 0;
            for (idx=0; idx<BM_NETDEV_TYPE_MAX; idx++) {
                switch (devices[idx].type) {
                    case BM_NETDEV_TYPE_ADIN2111:
                        retv = adin2111_tx((adin2111_DeviceHandle_t) devices[idx].device_handle, tx_data.payload, tx_data.payload_len,
                                           (tx_data.port_mask >> mask_idx) & ADIN2111_PORT_MASK);
                        mask_idx += devices[idx].num_ports;
                        if (retv != ERR_OK) {
                            //printf("Failed to submit TX buffer to ADIN");
                        }
                        break;
                    case BM_NETDEV_TYPE_NONE:
                    default:
                        /* No device or not supported */
                        break;
                }
            }
        }
    }
}

/* L2 SEND */
err_t bm_l2_tx(struct pbuf *p, uint8_t port_mask) {
    err_t retv = ERR_OK;
    static l2_queue_element_t tx_data;

    if(xSemaphoreTake(tx_sem, portMAX_DELAY) == pdTRUE) {
        tx_data.port_mask = port_mask;
        tx_data.payload_len = p->len;
        memcpy(tx_data.payload, p->payload, p->len);

        if(xQueueSend(tx_queue, (void *) &tx_data, 0) != pdTRUE) {
            retv = ERR_MEM;
        }
        xSemaphoreGive(tx_sem);
    } else {
        printf("Unable to take BM L2 TX mutex.\n");
        retv = ERR_TIMEOUT;
    }

    return retv;
}

/* L2 RECV */
err_t bm_l2_rx(void* device_handle, uint8_t* payload, uint16_t payload_len, uint8_t port_mask) {
    err_t retv = ERR_OK;
   static l2_queue_element_t rx_data;

    rx_data.device_handle = device_handle;
    rx_data.port_mask = port_mask;
    rx_data.payload_len = payload_len;
    memcpy(rx_data.payload, payload, payload_len);

    if(xQueueSend(rx_queue, (void *) &rx_data, 0) != pdTRUE) {
        retv = ERR_MEM;
    }
    return retv;
}

/* LWIP OUTPUT (acts as a wrapper for bm_l2_tx) */
err_t bm_l2_link_output(struct netif *netif, struct pbuf *p) {
    (void) netif;

    /* Send on all available ports (Multicast) */
    return bm_l2_tx(p, available_ports_mask);
}

/* L2 Initialization */
err_t bm_l2_init(struct netif *netif) {
    int idx;
    BaseType_t rval;

    for (idx=0; idx < BM_NETDEV_COUNT; idx++) {
        devices[idx].type = bm_netdev_config[idx].type;
        switch (devices[idx].type) {
            case BM_NETDEV_TYPE_ADIN2111:
                if (adin2111_hw_init(&adin_devices[adin_device_counter]) == ADI_ETH_SUCCESS) {
                    devices[idx].device_handle = &adin_devices[adin_device_counter++];
                    devices[idx].num_ports = ADIN2111_PORT_NUM;
                    devices[idx].start_port_idx = available_port_mask_idx;
                    available_ports_mask |= (ADIN2111_PORT_MASK << available_port_mask_idx);
                    available_port_mask_idx += ADIN2111_PORT_NUM;
                } else {
                    printf("Failed to init ADIN2111");
                }
                break;
            case BM_NETDEV_TYPE_NONE:
            default:
                /* No device or not supported */
                devices[idx].device_handle = NULL;
                devices[idx].num_ports = 0;
                break;
        }
    }

    MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, NETIF_LINK_SPEED_IN_BPS);

    netif->name[0] = IFNAME0;
    netif->name[1] = IFNAME1;
    netif->output_ip6 = ethip6_output;
    netif->linkoutput = bm_l2_link_output;

    /* Store netif in local pointer */
    net_if = netif;

    netif->mtu = ETHERNET_MTU;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

    /* Create the queues and semaphores that will be contained in the set. */
    tx_queue = xQueueCreate( TX_QUEUE_NUM_ENTRIES, sizeof(l2_queue_element_t));
    rx_queue = xQueueCreate( RX_QUEUE_NUM_ENTRIES, sizeof(l2_queue_element_t));

    tx_sem = xSemaphoreCreateMutex();
    configASSERT(tx_sem != NULL);

    rval = xTaskCreate(bm_l2_tx_thread,
                       "L2 TX Thread",
                       8192,
                       NULL,
                       15,
                       &tx_thread);
    configASSERT(rval == pdPASS);

    rval = xTaskCreate(bm_l2_rx_thread,
                       "L2 RX Thread",
                       8192, 
                       NULL,
                       15,
                       &rx_thread);
    configASSERT(rval == pdPASS);

    return ERR_OK;
}