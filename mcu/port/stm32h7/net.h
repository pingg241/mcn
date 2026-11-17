/* 以太网 + LwIP（NO_SYS）：RMII PHY、netif、UDP 5000/5001/6000 */
#ifndef NET_H
#define NET_H

void net_init(void);
void net_poll(void);

#endif
