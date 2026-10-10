/* Actual offset setter+DMA restart, explicit GEM/GIC/init boundary stubs.
 * No hardware IRQ, DMA, cache or AXI model. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef uint32_t u32;
typedef struct { struct { u32 BaseAddress; } Config; } XEmacPs;
#define XST_SUCCESS 0
#define XST_FAILURE 1
#define ETH_TASK_SETUP 0
#define ETH_TASK_READY 3
#define XEMACPS_TXSR_OFFSET 1
#define XEMACPS_RXSR_OFFSET 2
#define XEMACPS_ISR_OFFSET 3
static XEmacPs EmacPsInstance = {{0x1000}};
static int ethernet_task_state=ETH_TASK_READY,ethernet_hw_ready=1;
static u32 rx_offset_req,rx_offset_ring;
static int init_status,irq_enabled=1;
static unsigned pauses,stops,clears,inits,resumes;
static void ethernet_log_status(const char *s){(void)s;}
static int ethernet_pause_rx_irq(void){assert(irq_enabled);irq_enabled=0;pauses++;return 1;}
static void ethernet_resume_rx_irq(int paused){assert(paused && !irq_enabled);irq_enabled=1;resumes++;}
static void XEmacPs_Stop(XEmacPs *p){assert(p==&EmacPsInstance && !irq_enabled && !ethernet_hw_ready);stops++;}
static u32 XEmacPs_ReadReg(u32 base,unsigned off){assert(base && !irq_enabled);return off;}
static void XEmacPs_WriteReg(u32 base,unsigned off,u32 value){assert(base && !irq_enabled && off==value);}
static void ethernet_clear_host_state(void){assert(!irq_enabled && !ethernet_hw_ready);clears++;}
static int init_ethernet_buffers(void){assert(!irq_enabled && !ethernet_hw_ready);inits++;if(!init_status)rx_offset_ring=rx_offset_req;return init_status;}
/* ACTUAL_FUNCTIONS */
int main(int argc,char **argv)
{
 assert(argc==2);
 if(!strcmp(argv[1],"healthy-idempotent")) {
  rx_offset_req=rx_offset_ring=2;ethernet_set_rx_offset2(1);
  assert(!inits && ethernet_hw_ready && rx_offset_ring==2);
 } else if(!strcmp(argv[1],"pre-init")) {
  ethernet_task_state=ETH_TASK_SETUP;ethernet_hw_ready=0;
  ethernet_set_rx_offset2(1);ethernet_set_rx_offset2(1);
  assert(!inits && rx_offset_req==2 && rx_offset_ring==0);
 } else if(!strcmp(argv[1],"normal-switch")) {
  ethernet_set_rx_offset2(1);
  assert(inits==1 && ethernet_hw_ready && rx_offset_ring==2);
 } else {
  int off=!strcmp(argv[1],"disable-retry");
  int permanent=!strcmp(argv[1],"permanent-failure");
  int startup=!strcmp(argv[1],"failed-startup-default");
  rx_offset_req=rx_offset_ring=off?2:0;
  if(startup)ethernet_hw_ready=0;
  else {init_status=XST_FAILURE;ethernet_set_rx_offset2(!off);assert(inits==1 && !ethernet_hw_ready);}
  init_status=permanent?XST_FAILURE:XST_SUCCESS;
  unsigned before=inits;
  ethernet_set_rx_offset2(startup?0:!off);
  int ok=inits==before+1 && (permanent?!ethernet_hw_ready:(ethernet_hw_ready && rx_offset_ring==(off||startup?0:2)));
  printf("case=%s retry_calls=%u hw_ready=%d requested=%u ring=%u %s\n",argv[1],inits-before,ethernet_hw_ready,rx_offset_req,rx_offset_ring,ok?"PASS":"FAIL");
  if(!ok)return 1;
 }
 assert(irq_enabled && pauses==stops && stops==clears && clears==inits && inits==resumes);
 printf("PASS %s irq-exclusion/restart accounting\n",argv[1]);
 return 0;
}
