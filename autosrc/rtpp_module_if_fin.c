/* Auto-generated by genfincode.sh - DO NOT EDIT! */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "rtpp_types.h"
#include "rtpp_debug.h"
#define rtpp_module_if_h_fin 1
#include "rtpp_module_if.h"
static void rtpp_module_if_config_fin(void *pub) {
    fprintf(stderr, "Method rtpp_module_if_config is called after destruction\x0a");
    abort();
}
static void rtpp_module_if_do_acct_fin(void *pub) {
    fprintf(stderr, "Method rtpp_module_if_do_acct is called after destruction\x0a");
    abort();
}
static void rtpp_module_if_do_acct_rtcp_fin(void *pub) {
    fprintf(stderr, "Method rtpp_module_if_do_acct_rtcp is called after destruction\x0a");
    abort();
}
static void rtpp_module_if_get_mconf_fin(void *pub) {
    fprintf(stderr, "Method rtpp_module_if_get_mconf is called after destruction\x0a");
    abort();
}
static void rtpp_module_if_load_fin(void *pub) {
    fprintf(stderr, "Method rtpp_module_if_load is called after destruction\x0a");
    abort();
}
static void rtpp_module_if_start_fin(void *pub) {
    fprintf(stderr, "Method rtpp_module_if_start is called after destruction\x0a");
    abort();
}
void rtpp_module_if_fin(struct rtpp_module_if *pub) {
    RTPP_DBG_ASSERT(pub-> load != (rtpp_module_if_load_t)&rtpp_module_if_load_fin);
    pub-> load = (rtpp_module_if_load_t)&rtpp_module_if_load_fin;
    RTPP_DBG_ASSERT(pub-> config != (rtpp_module_if_config_t)&rtpp_module_if_config_fin);
    pub-> config = (rtpp_module_if_config_t)&rtpp_module_if_config_fin;
    RTPP_DBG_ASSERT(pub-> start != (rtpp_module_if_start_t)&rtpp_module_if_start_fin);
    pub-> start = (rtpp_module_if_start_t)&rtpp_module_if_start_fin;
    RTPP_DBG_ASSERT(pub-> do_acct != (rtpp_module_if_do_acct_t)&rtpp_module_if_do_acct_fin);
    pub-> do_acct = (rtpp_module_if_do_acct_t)&rtpp_module_if_do_acct_fin;
    RTPP_DBG_ASSERT(pub-> do_acct_rtcp != (rtpp_module_if_do_acct_rtcp_t)&rtpp_module_if_do_acct_rtcp_fin);
    pub-> do_acct_rtcp = (rtpp_module_if_do_acct_rtcp_t)&rtpp_module_if_do_acct_rtcp_fin;
    RTPP_DBG_ASSERT(pub-> get_mconf != (rtpp_module_if_get_mconf_t)&rtpp_module_if_get_mconf_fin);
    pub-> get_mconf = (rtpp_module_if_get_mconf_t)&rtpp_module_if_get_mconf_fin;
}