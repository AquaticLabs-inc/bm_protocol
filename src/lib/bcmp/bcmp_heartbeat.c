#include "bcmp.h"

err_t bcmp_send_heartbeat(uint32_t count) {
  bcmpHeartbeat_t heartbeat = {.count=count};
  printf("heartbeat count %"PRIu32"\n", heartbeat.count);
  return bcmp_tx(&multicast_ll_addr, BCMP_HEARTBEAT, (uint8_t *)&heartbeat, sizeof(heartbeat));
}

err_t bcmp_process_heartbeat(void *payload, const ip_addr_t *src, uint8_t dst_port) {
  bcmpHeartbeat_t *heartbeat = (bcmpHeartbeat_t *)payload;
  printf("❤️  from %s (%"PRIu32") [%02X]\n", ipaddr_ntoa(src), heartbeat->count, dst_port);

  // TODO - add to neighbor table

  return ERR_OK;
}
