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

#ifndef CONTENTION_SLOTS
 #define CONTENTION_SLOTS 4
 #endif

#ifndef CONTENTION_TICKS
 #define CONTENTION_TICKS 5
 #endif

#ifndef MAX_STROBE_SIZE
 #define MAX_STROBE_SIZE 1000
 #endif

#ifndef CCA_TICKS
 #define CCA_TICKS 4
 #endif

#ifndef CCA_TICKS
 #define CCA_TICKS 42
 #endif

static volatile unsigned char radio_is_on = 0;
static volatile unsigned char current_slot = 0;
static volatile unsigned char waiting_to_send = 0;
static volatile unsigned char crankshaft_is_running = 0;
static volatile unsigned char needed_slot = BROADCAST_SLOT;

static unsigned int REGULAR_SLOT = (RTIMER_SECOND / 1000) * (CRANKSHAFT_PERIOD / TOTAL_SLOTS);
static void advanceSlot(struct rtimer *t, void *ptr, int status);
static void schedule_outgoing_packet(unsigned char, mac_callback_t, void *, struct queuebuf *);
static void real_send(mac_callback_t, void *, struct queuebuf *);
static char check_buffers(unsigned char);

static struct rtimer taskSlot;

typedef struct QueuedPacket{
  mac_callback_t sent;
  void *ptr;
  struct queuebuf *packet;
  struct QueuedPacket *next;
} QueuedPacket;

QueuedPacket *QPQueue[TOTAL_SLOTS];

static void send_packet(mac_callback_t sent, void *ptr) {
  struct queuebuf *packet;

  packet = queuebuf_new_from_packetbuf();
  if(packet == NULL) {
      /* No buffer available */
      printf("xmac: send failed, no queue buffer available (of %u)\n", QUEUEBUF_CONF_NUM);
      mac_call_sent_callback(sent, ptr, MAC_TX_ERR, 1);
  } else {
    // schedule for proper slot
    // if broadcast, slot 0
    // if uni, slot of dest->u8[7]

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

    schedule_outgoing_packet(needed_slot, sent, ptr, packet);

  }
}
/*---------------------------------------------------------------------------*/
static void send_list(mac_callback_t sent, void *ptr, struct rdc_buf_list *buf_list){
  // printf("WPI-MAC-send_list(), node ID: %u\n", node_id);
  if(buf_list != NULL) {
    queuebuf_to_packetbuf(buf_list->buf);
    send_packet(sent, ptr);
  }
}
/*---------------------------------------------------------------------------*/
static void packet_input(void){
  // printf("WPI-MAC-packet_input(), node ID: %u\n", node_id);
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
  if(!(rtimer_set(t, RTIMER_TIME(t) + REGULAR_SLOT, REGULAR_SLOT, (void (*)(struct rtimer *, void *))advanceSlot, NULL) == RTIMER_OK)){
    printf("%s\n", "Could not schedule task!!!!!");
  }
  if(current_slot == TOTAL_SLOTS + 1){
    current_slot = BROADCAST_SLOT;
  } else{
    current_slot++;
  }
  if(current_slot > (TOTAL_SLOTS - 1)){
    current_slot = BROADCAST_SLOT;
  }
  //printf("Slot is now %u\n", current_slot);
  unsigned char somethingToSend = check_buffers(current_slot);
  if(current_slot == BROADCAST_SLOT || current_slot == node_id || somethingToSend){
    // we need to be on

    // see if we gotta send
    if((current_slot != node_id) && somethingToSend){
    // grab the necessary info from our queue
      QueuedPacket *curr = QPQueue[current_slot];
      if(!radio_is_on) on();
      real_send(curr->sent, curr->ptr, curr->packet);
    } else { // else we just need to be awake to listen
      // WAIT FOR CONTENTION
      // while(RTIMER_CLOCK_LT(RTIMER_NOW(), RTIMER_TIME(t) + CONTENTION_TICKS));
      if(!radio_is_on) on();
    } 

  } else{
    // we can snooze
    if(radio_is_on) off(TURN_OFF);
  }

}
/*---------------------------------------------------------------------------*/
static void init(void){
  // on();
  current_slot = TOTAL_SLOTS + 1;
  crankshaft_is_running = 1;
  //must schedule recurring wakeups for broadcast and uni-recv
  //also schedule slot advance
  int t;
  if(node_id < 10) { 
    t = REGULAR_SLOT + 300;
  } else {
    t = REGULAR_SLOT + 240;
  }
  if(!(rtimer_set(&taskSlot, RTIMER_NOW() + t, 1, (void (*)(struct rtimer *, void *))advanceSlot, NULL) == RTIMER_OK)){
    printf("%s\n", "Could not schedule initial task!!!!!");
  }

  int i = 0;
  for(; i < TOTAL_SLOTS; i++){
    QPQueue[i] = NULL;
  }

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
static void real_send(mac_callback_t sent, void *ptr, struct queuebuf *pkt){
    int ret;
    uint8_t contention_strobe[MAX_STROBE_SIZE];
    unsigned char won_contention = 0;

    int len = NETSTACK_FRAMER.create();
    if(len < 0) {
      // off(TURN_OFF);
      /* Failed to send */
      printf("WPI-MAC: send failed, too large header\n");
      mac_call_sent_callback(sent, ptr, MAC_TX_ERR_FATAL, 1);
    } else {
      // randomly pick slot
      // make filler packet
      // wait for our random slot
      if(NETSTACK_RADIO.channel_clear()){
        // send filler, if no err, we won contention
        won_contention = 1;
      } else {
        won_contention = 0;
      }

      if(won_contention){
        memcpy(contention_strobe, packetbuf_hdrptr(), len);
        int j;
        for(j = 0; j < 45; j++){
          contention_strobe[len] = 7;
          len++;
        }

        rtimer_clock_t t0 = RTIMER_NOW();
        NETSTACK_RADIO.channel_clear();
        rtimer_clock_t t1 = RTIMER_NOW();
        NETSTACK_RADIO.send(contention_strobe, len);
        rtimer_clock_t t2 = RTIMER_NOW();
        printf("TIMES %u - %u - %u - %u\n", t0, t1, t2, len);


        // if(NETSTACK_RADIO.clear_channel()){

        // }

        queuebuf_to_packetbuf(pkt);
        packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &rimeaddr_node_addr);
        if(NETSTACK_FRAMER.create() < 0) {
          /* Failed to allocate space for headers */
          printf("wpimac: send failed, too large header\n");
          ret = MAC_TX_ERR_FATAL;
        } else {
          // printf("%s - %u - %u\n", "Waiting to send...", current_slot, needed_slot);
          // while(radio_is_on == 0 && !(current_slot == needed_slot)); //wish i knew why this didn't work
          // rtimer_clock_t recent = RTIMER_TIME(&taskSlot);
          // rtimer_clock_t until = calcNext(current_slot, needed_slot);
          // while(RTIMER_CLOCK_LT(RTIMER_NOW(), recent + until));
          // printf("%s - %u - %u\n", "sending!", current_slot, needed_slot);
          printf("%u\n", packetbuf_totlen());
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

        // off(TURN_OFF);
        mac_call_sent_callback(sent, ptr, ret, 1);

        // remove packet from our queue
        if(ret == MAC_TX_OK){
          QueuedPacket *head = QPQueue[current_slot];
          QPQueue[current_slot] = head->next;
          head->ptr = NULL;
          queuebuf_free(head->packet);
          head->packet = NULL;
          head->next = NULL;
          free(head);
        }
      } else { // lost contention
        mac_call_sent_callback(sent, ptr, MAC_TX_COLLISION, 1);
      }
      
    }
}
/*---------------------------------------------------------------------------*/
static void schedule_outgoing_packet(unsigned char slot, mac_callback_t sent, void *ptr, struct queuebuf *pkt){
  if(QPQueue[slot] == NULL){
    QPQueue[slot] = (QueuedPacket*) malloc(sizeof(QueuedPacket));
    if(QPQueue[slot] == NULL){
      printf("CRANKSHAFT RAN OUT OF MEMORY.\n");
    } else{
      QPQueue[slot]->sent = sent;
      QPQueue[slot]->ptr = ptr;
      QPQueue[slot]->packet = pkt;
      QPQueue[slot]->next = NULL;
    }
  } else {
    QueuedPacket *curr = QPQueue[slot];
    while(curr->next != NULL){
      curr = curr->next;
    }
    curr->next = (QueuedPacket*) malloc(sizeof(QueuedPacket));
    curr = curr->next;
    if(curr == NULL){
      printf("CRANKSHAFT RAN OUT OF MEMORY.\n");
    } else{
      curr->sent = sent;
      curr->ptr = ptr;
      curr->packet = pkt;
      curr->next = NULL;
    }
  }
}
/*---------------------------------------------------------------------------*/
static char check_buffers(unsigned char for_slot) {
  if(QPQueue[for_slot] == NULL){
    return 0;
  } else{
    return 1;
  }
}
/*---------------------------------------------------------------------------*/