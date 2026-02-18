#ifndef _NET_FUNCTIONS_H_
#define _NET_FUNCTIONS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "mdns.h"

#define SNTP_TIMEZONE CONFIG_SNTP_TIMEZONE

void net_mdns_register(const char* clientname);
void mdns_print_results(const mdns_result_t* results);

// Return port number
uint32_t find_mdns_service(const char* service_name, const char* proto);

void set_time_from_sntp(void);

#ifdef __cplusplus
}
#endif

#endif /* _NET_FUNCTIONS_H_ */
