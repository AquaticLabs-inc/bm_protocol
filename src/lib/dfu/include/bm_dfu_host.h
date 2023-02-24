#pragma once

#include "bm_dfu.h"

// Includes for FreeRTOS
#include "FreeRTOS.h"
#include "queue.h"
#include "timers.h"
#include "semphr.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define BM_DFU_MAX_ACK_RETRIES              2
#define BM_DFU_HOST_ACK_TIMEOUT_MS          10000UL
#define BM_DFU_HOST_HEARTBEAT_TIMEOUT_MS    10000UL

typedef int (*bm_dfu_chunk_req_cb)(uint16_t chunk_num, uint16_t *chunk_len, uint8_t *buf, uint16_t buf_len);

void s_host_run(void);
void s_host_req_update_entry(void);
void s_host_req_update_run(void);
void s_host_update_entry(void);
void s_host_update_run(void);

void bm_dfu_host_init(ip6_addr_t _self_addr, struct udp_pcb* _pcb, uint16_t _port, struct netif* _netif);

#ifdef __cplusplus
}
#endif