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
#include <stdlib.h>

#ifndef TOTAL_SLOTS
 #define TOTAL_SLOTS        12
 #endif

#ifndef CRANKSHAFT_PERIOD
 #define CRANKSHAFT_PERIOD 252
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
static volatile unsigned char needed_slot = 0;

static unsigned int REGULAR_SLOT = (RTIMER_SECOND / 1000) * (CRANKSHAFT_PERIOD / TOTAL_SLOTS);
static void advanceSlot(struct rtimer *t, void *ptr, int status);
// static void schedule_outgoing_packet(unsigned char, mac_callback_t, void *, void *, uint16_t);
static int real_send();
// static char check_buffers(unsigned char);

static struct rtimer taskSlot;

// typedef struct QueuedPacket{
//   mac_callback_t sent;
//   void *ptr;
//   void *packetheader;
//   uint16_t totlen;
//   struct QueuedPacket *next;
// } QueuedPacket;

// QueuedPacket *QPQueue[TOTAL_SLOTS];

/*---------------------------------------------------------------------------*/
// static char check_buffers(unsigned char for_slot) {
//   if(QPQueue[for_slot] == NULL){
//     return 0;
//   } else{
//     return 1;
//   }
// }
/*---------------------------------------------------------------------------*/
static void send_packet(mac_callback_t sent, void *ptr) {
  // schedule for proper slot
  // if broadcast, slot 0
  // if uni, slot of dest->u8[7]

  //packetbuf_addr(PACKETBUF_ADDR_RECEIVER)->u8
  rimeaddr_t *dest = (rimeaddr_t*)packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
  if(rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER), &rimeaddr_null)){
    // printf("WPI-MAC-send_packet(), node ID: %u, GOT BROADCAST?\n", node_id);
    // schedule for slot 0
    // void *newbuff = malloc(PACKETBUF_SIZE + PACKETBUF_HDR_SIZE);
    // schedule_outgoing_packet(BROADCAST_SLOT, sent, ptr, newbuff, packetbuf_copyto(newbuff));
    needed_slot = BROADCAST_SLOT;
  } else {
    // unsigned char dest_node = dest->u8[7];
    // printf("WPI-MAC-send_packet(), node ID: %u - GOT UNICAST? to %d\n", node_id, dest->u8[7]);
    // schedule for slot dest_node
    // void *newbuff = malloc(PACKETBUF_SIZE + PACKETBUF_HDR_SIZE);
    // schedule_outgoing_packet(dest_node, sent, ptr, newbuff, packetbuf_copyto(newbuff));
    needed_slot = dest->u8[7];
  }
  
  int ret = real_send();

  mac_call_sent_callback(sent, ptr, ret, 1);

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
// static void schedule_outgoing_packet(unsigned char slot, mac_callback_t sent, void *ptr, void *packetheader, uint16_t totlen){
//   if(QPQueue[slot] == NULL){
//     QPQueue[slot] = (QueuedPacket*) malloc(sizeof(QueuedPacket));
//     if(QPQueue[slot] == NULL){
//       printf("CRANKSHAFT RAN OUT OF MEMORY.\n");
//     } else{
//       QPQueue[slot]->sent = sent;
//       QPQueue[slot]->ptr = ptr;
//       QPQueue[slot]->packetheader = packetheader;
//       QPQueue[slot]->totlen = totlen;
//       QPQueue[slot]->next = NULL;
//     }
//   } else {
//     QueuedPacket *curr = QPQueue[slot];
//     while(curr->next != NULL){
//       curr = curr->next;
//     }
//     curr->next = (QueuedPacket*) malloc(sizeof(QueuedPacket));
//     curr = curr->next;
//     if(curr == NULL){
//       printf("CRANKSHAFT RAN OUT OF MEMORY.\n");
//     } else{
//       curr->sent = sent;
//       curr->ptr = ptr;
//       curr->packetheader = packetheader;
//       curr->totlen = totlen;
//       curr->next = NULL;
//     }
//   }
// }
/*---------------------------------------------------------------------------*/
static int real_send(){
  int ret;
  // struct queuebuf *packet;

  packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &rimeaddr_node_addr);
  if(NETSTACK_FRAMER.create() < 0) {
    /* Failed to allocate space for headers */
    printf("wpimac: send failed, too large header\n");
    ret = MAC_TX_ERR_FATAL;
  } else {
    while(radio_is_on == 0 && current_slot != needed_slot);
    switch(NETSTACK_RADIO.send(packetbuf_hdrptr(), packetbuf_totlen())) {
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

  // if(ret == MAC_TX_OK){
  //   QueuedPacket *head = QPQueue[current_slot];
  //   QPQueue[current_slot] = head->next;
  //   head->ptr = NULL;
  //   free(head->packetheader);
  //   head->packetheader = NULL;
  //   head->next = NULL;
  //   free(head);
  // }

  return ret;
}
/*---------------------------------------------------------------------------*/
static void send_list(mac_callback_t sent, void *ptr, struct rdc_buf_list *buf_list){
  printf("WPI-MAC-send_list(), node ID: %u\n", node_id);
  if(buf_list != NULL) {
    queuebuf_to_packetbuf(buf_list->buf);
    send_packet(sent, ptr);
  }
}
/*---------------------------------------------------------------------------*/
static void packet_input(void){
  printf("WPI-MAC-packet_input(), node ID: %u\n", node_id);
  if(NETSTACK_FRAMER.parse() < 0) {
    printf("nullrdc: failed to parse %u\n", packetbuf_datalen());
  } else {
    NETSTACK_MAC.input();
  }
}
/*---------------------------------------------------------------------------*/
static int on(void){
  //printf("WPI-MAC-on(), node ID: %u\n", node_id);
  radio_is_on = 1;
  return NETSTACK_RADIO.on();
}
/*---------------------------------------------------------------------------*/
static int off(int keep_radio_on){
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
  rtimer_set(t, RTIMER_TIME(t) + REGULAR_SLOT, REGULAR_SLOT, (void (*)(struct rtimer *, void *))advanceSlot, NULL);
  if(current_slot == TOTAL_SLOTS + 1){
    current_slot = BROADCAST_SLOT;
  } else{
    current_slot++;
  }
  if(current_slot > (TOTAL_SLOTS - 1)){
    current_slot = BROADCAST_SLOT;
  }
  //printf("Slot is now %u\n", current_slot);
  // unsigned char somethingToSend = check_buffers(current_slot);
  if(current_slot == BROADCAST_SLOT || current_slot == node_id || current_slot == needed_slot){
    // we need to be on
    if(!radio_is_on) on();

    // see if we gotta send
    // if((current_slot != node_id) && somethingToSend){
    //   // grab the necessary info from our queue
    //   QueuedPacket *curr = QPQueue[current_slot];
    //   real_send(curr->sent, curr->ptr, curr->packetheader, curr->totlen);
    // } // else we just need to be awake to listen

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
  rtimer_set(&taskSlot, RTIMER_NOW() + REGULAR_SLOT, REGULAR_SLOT, (void (*)(struct rtimer *, void *))advanceSlot, NULL);
  // int i = 0;
  // for(; i < TOTAL_SLOTS; i++){
  //   QPQueue[i] = NULL;
  // }

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
