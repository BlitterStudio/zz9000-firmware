/* SPDX-License-Identifier: MIT
 * Actual MAC-update caller; keep old ring/accounting intact until guarded restart.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ETH_TASK_READY 2
#define XST_SUCCESS 0
#define XST_FAILURE 1
typedef struct { unsigned unused; } XEmacPs;
static XEmacPs EmacPsInstance;
static uint8_t EmacPsMAC[6];
static int ethernet_task_state, mac_status;
static unsigned stopped, programmed, restarted;
static void ethernet_log_status(const char *why) { assert(why != NULL); }
static void XEmacPs_Stop(XEmacPs *p) { assert(p == &EmacPsInstance); stopped++; }
static void ethernet_clear_host_state(void)
{ assert(0 && "unguarded accounting clear before restart"); }
static int XEmacPs_SetMacAddress(XEmacPs *p, uint8_t *mac, unsigned slot)
{ assert(p == &EmacPsInstance && mac == EmacPsMAC && slot == 1 && stopped == 1); programmed++; return mac_status; }
static int ethernet_restart_dma(const char *why)
{ assert(!strcmp(why, "mac-update-restart") && programmed == 1); restarted++; return XST_SUCCESS; }
/* MAC_FUNCTION */
int main(int argc, char **argv)
{
    assert(argc == 2);
    (void)ethernet_clear_host_state;
    ethernet_task_state = !strcmp(argv[1], "inactive") ? 0 : ETH_TASK_READY;
    mac_status = !strcmp(argv[1], "program-failure") ? XST_FAILURE : XST_SUCCESS;
    ethernet_update_mac_address();
    unsigned expected = ethernet_task_state == ETH_TASK_READY ? 1 : 0;
    assert(stopped == expected && programmed == expected && restarted == expected);
    printf("PASS MAC restart %s\n", argv[1]);
    return 0;
}
