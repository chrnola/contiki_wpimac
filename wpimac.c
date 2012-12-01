/*
 * Copyright (c) 2012, Worcester Polytechnic Institute.
 * All rights reserved.
 *
 * This file is part of the Contiki operating system.
 */

/**
 * \file
 *         A RDC protocol implementation based on Crankshaft.
 * \author
 *         Chris Pinola <cpinola@wpi.edu>
 */

#include "sys/rtimer.h"
#include "sys/clock.h"
#include "net/mac/wpimac.h"
#include "net/packetbuf.h"
#include "net/queuebuf.h"
#include "net/netstack.h"
#include <string.h>

#ifndef TOTAL_SLOTS
 #define TOTAL_SLOTS        12
 #endif

#ifndef CRANKSHAFT_PERIOD
 #define CRANKSHAFT_PERIOD 1008
 #endif

#ifndef BROADCAST_SLOT
 #define BROADCAST_SLOT 0
 #endif

#ifndef TURN_OFF
 #define TURN_OFF 0
 #endif

static volatile unsigned char radio_is_on = 0;
static volatile unsigned char current_slot = 0;
static volatile unsigned char crankshaft_is_running = 0;

static unsigned int REGULAR_SLOT = (RTIMER_SECOND / 1000) * (CRANKSHAFT_PERIOD / TOTAL_SLOTS);
static void advanceSlot(struct rtimer *t, void *ptr, int status);
static void schedule_outgoing_packet(unsigned char, mac_callback_t, void *, void *, uint16_t);
static void real_send(mac_callback_t, void *, void *, uint16_t);
static char check_buffers(unsigned char);

static struct rtimer taskSlot;

/*---------------------------------------------------------------------------*/
static char check_buffers(unsigned char for_slot){
  return 0;
}
/*---------------------------------------------------------------------------*/
static void send_packet(mac_callback_t sent, void *ptr)
{
  // schedule for proper slot
  // if broadcast, slot 0
  // if uni, slot of dest->u8[7]

  //packetbuf_addr(PACKETBUF_ADDR_RECEIVER)->u8
  rimeaddr_t *dest = packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
  if(rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER), &rimeaddr_null)){
    printf("WPI-MAC-send_packet(), node ID: %u, GOT BROADCAST?\n", node_id);
    // schedule for slot 0
    schedule_outgoing_packet(BROADCAST_SLOT, sent, ptr, packetbuf_hdrptr(), packetbuf_totlen());
  } else {
    unsigned char dest_node = dest->u8[7];
    printf("WPI-MAC-send_packet(), node ID: %u - GOT UNICAST? to %d\n", node_id, dest->u8[7]);
    // schedule for slot dest_node
    schedule_outgoing_packet(dest_node, sent, ptr, packetbuf_hdrptr(), packetbuf_totlen());
  }
  

  // orig send
  // if(NETSTACK_FRAMER.create() < 0) {
  //   /* Failed to allocate space for headers */
  //   printf("nullrdc: send failed, too large header\n");
  //   ret = MAC_TX_ERR_FATAL;
  // } else {
  //   switch(NETSTACK_RADIO.send(packetbuf_hdrptr(), packetbuf_totlen())) {
  //   case RADIO_TX_OK:
  //     ret = MAC_TX_OK;
  //     break;
  //   case RADIO_TX_COLLISION:
  //     ret = MAC_TX_COLLISION;
  //     break;
  //   case RADIO_TX_NOACK:
  //     ret = MAC_TX_NOACK;
  //     break;
  //   default:
  //     ret = MAC_TX_ERR;
  //     break;
  //   }
}
/*---------------------------------------------------------------------------*/
static void schedule_outgoing_packet(unsigned char slot, mac_callback_t sent, void *ptr, void *packetheader, uint16_t totlen){
  real_send(sent, ptr, packetheader, totlen);
}
/*---------------------------------------------------------------------------*/
static void real_send(mac_callback_t sent, void *ptr, void *packetheader, uint16_t totlen){
  int ret;
  packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &rimeaddr_node_addr);
  if(NETSTACK_FRAMER.create() < 0) {
    /* Failed to allocate space for headers */
    printf("nullrdc: send failed, too large header\n");
    ret = MAC_TX_ERR_FATAL;
  } else {
    switch(NETSTACK_RADIO.send(packetheader, totlen)) {
    case RADIO_TX_OK:
      ret = MAC_TX_OK;
      break;
    case RADIO_TX_COLLISION:
      ret = MAC_TX_COLLISION;
      break;
    case RADIO_TX_NOACK:
      ret = MAC_TX_NOACK;
      break;
    default:
      ret = MAC_TX_ERR;
      break;
    }

  }
  mac_call_sent_callback(sent, ptr, ret, 1);
}
/*---------------------------------------------------------------------------*/
static void send_list(mac_callback_t sent, void *ptr, struct rdc_buf_list *buf_list)
{
  printf("WPI-MAC-send_list(), node ID: %u\n", node_id);
  if(buf_list != NULL) {
    queuebuf_to_packetbuf(buf_list->buf);
    send_packet(sent, ptr);
  }
}
/*---------------------------------------------------------------------------*/
static void packet_input(void)
{
  printf("WPI-MAC-packet_input(), node ID: %u\n", node_id);
  if(NETSTACK_FRAMER.parse() < 0) {
    printf("nullrdc: failed to parse %u\n", packetbuf_datalen());
  } else {
    NETSTACK_MAC.input();
  }
}
/*---------------------------------------------------------------------------*/
static int on(void)
{
  //printf("WPI-MAC-on(), node ID: %u\n", node_id);
  radio_is_on = 1;
  return NETSTACK_RADIO.on();
}
/*---------------------------------------------------------------------------*/
static int off(int keep_radio_on)
{
  //printf("WPI-MAC-off(), node ID: %u\n", node_id);
  if(keep_radio_on) {
    return NETSTACK_RADIO.on();
  } else {
    radio_is_on = 0;
    return NETSTACK_RADIO.off();
  }
}
/*---------------------------------------------------------------------------*/
static unsigned short channel_check_interval(void){
  return 0;
}
/*---------------------------------------------------------------------------*/
static void advanceSlot(struct rtimer *t, void *ptr, int status){
  rtimer_set(t, RTIMER_TIME(t) + REGULAR_SLOT, 5, (void (*)(struct rtimer *, void *))advanceSlot, NULL);
  if(current_slot == TOTAL_SLOTS + 1){
    current_slot = BROADCAST_SLOT;
  } else{
    current_slot++;
  }
  if(current_slot > (TOTAL_SLOTS - 1)){
    current_slot = BROADCAST_SLOT;
  }
  //printf("Slot is now %u\n", current_slot);
  if(current_slot == BROADCAST_SLOT || current_slot == node_id || check_buffers(current_slot)){
    // we need to be on
    if(!radio_is_on) on();
  } else{
    // we can snooze
    if(radio_is_on) off(TURN_OFF);
  }

}
/*---------------------------------------------------------------------------*/
static void init(void){
  on();
  current_slot = TOTAL_SLOTS + 1;
  crankshaft_is_running = 1;
  //must schedule recurring wakeups for broadcast and uni-recv
  //also schedule slot advance
  rtimer_set(&taskSlot, RTIMER_NOW() + REGULAR_SLOT, 5, (void (*)(struct rtimer *, void *))advanceSlot, NULL);

  // if(node_id == 1){
  //   sendSyncMessage();
  // } else{
  //   waiting_for_sync = 1;
  //   // printf("Waiting until we get the sync packet.\n");
  //   // while(waiting_for_sync){
  //   //   checkForSyncPacket();
  //   // }
  //   // printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! DONE WAITING!!!!!!\n");
  // }
}
/*---------------------------------------------------------------------------*/
const struct rdc_driver wpimac_driver = {
  "WPI-MAC",
  init,
  send_packet,
  send_list,
  packet_input,
  on,
  off,
  channel_check_interval,
};
/*---------------------------------------------------------------------------*/
