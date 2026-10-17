/*
 * QwzWifi compat: OpenBSD <net/route.h>. net80211 only sends routing
 * socket messages about link changes; there is no routing socket here.
 */
#ifndef _QWZMAC_NET_ROUTE_H_
#define _QWZMAC_NET_ROUTE_H_
#include <sys/types.h>
struct ifnet;
struct if_ieee80211_data;
void	rtm_80211info(struct ifnet *, struct if_ieee80211_data *);
#endif
