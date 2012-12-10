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
#include "lib/random.h"
#include <string.h>
#include <stdlib.h>

#ifndef TOTAL_SLOTS
 #define TOTAL_SLOTS        12
 #endif

// 252
 // 1008
#ifndef CRANKSHAFT_PERIOD
 #define CRANKSHAFT_PERIOD 180
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

#ifndef CCA_CONTENTION_SIZE
 #define CCA_CONTENTION_SIZE 22
 #endif

#ifndef CONTENTION_SIZE
 #define CONTENTION_SIZE 24
 #endif

#ifndef CONTENTION_TICKS
 #define CONTENTION_TICKS 40
 #endif

#ifndef MAX_STROBE_SIZE
 #define MAX_STROBE_SIZE 200
 #endif

#ifndef CONTENTION_PREPARE
 #define CONTENTION_PREPARE 5
 #endif

static unsigned int REGULAR_SLOT = (RTIMER_SECOND / 1000) * (CRANKSHAFT_PERIOD / TOTAL_SLOTS);
static void advanceSlot(struct rtimer *t, void *ptr, int status);
static void async_on(struct rtimer *t, void *ptr, int status);
static void schedule_outgoing_packet(unsigned char, mac_callback_t, void *, struct queuebuf *);
static void real_send(mac_callback_t, void *, struct queuebuf *);
static char check_buffers(unsigned char);
static unsigned short map_rand(unsigned short);

typedef struct QueuedPacket{
  mac_callback_t sent;
  void *ptr;
  struct queuebuf *packet;
  struct QueuedPacket *next;
} QueuedPacket;

QueuedPacket *QPQueue[TOTAL_SLOTS];
static struct rtimer taskSlot;
rtimer_clock_t last;
static volatile unsigned char radio_is_on = 0;
static volatile unsigned char current_slot = 0;

static void send_packet(mac_callback_t sent, void *ptr) {
  struct queuebuf *packet;

  packet = queuebuf_new_from_packetbuf();
  if(packet == NULL) {
      /* No buffer available */
      printf("WPI-MAC: send failed, no queue buffer available (of %u)\n", QUEUEBUF_CONF_NUM);
      mac_call_sent_callback(sent, ptr, MAC_TX_ERR, 1);
  } else {
    // schedule for proper slot
    // if broadcast, slot 0
    // if uni, slot of dest->u8[7]

    rimeaddr_t *dest = (rimeaddr_t*)packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
    if(rimeaddr_cmp(packetbuf_addr(PACKETBUF_ADDR_RECEIVER), &rimeaddr_null)){
      // schedule for slot 0
      // printf("scheduling bcast\n");
      schedule_outgoing_packet(BROADCAST_SLOT, sent, ptr, packet);
    } else {
      // schedule for slot dest_node
      schedule_outgoing_packet((unsigned short)dest->u8[7], sent, ptr, packet);
    }

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
    printf("WPI-MAC: failed to parse %u\n", packetbuf_datalen());
  } else {
    NETSTACK_MAC.input();
    // printf("%s\n", "PASSED UP\n");
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
  return (1ul * CLOCK_SECOND * CRANKSHAFT_PERIOD) / RTIMER_ARCH_SECOND;
}
/*---------------------------------------------------------------------------*/
static void advanceSlot(struct rtimer *t, void *ptr, int status){
  off(TURN_OFF);
  last = RTIMER_TIME(t);
  if(!(rtimer_set(t, last + REGULAR_SLOT, 1, (void (*)(struct rtimer *, void *))advanceSlot, NULL) == RTIMER_OK)){
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

  if(somethingToSend){
    // grab the necessary info from our queue
    QueuedPacket *curr = QPQueue[current_slot];
    real_send(curr->sent, curr->ptr, curr->packet);
  } else if(current_slot == BROADCAST_SLOT || current_slot == node_id){ // just need to be awake to listen
    // if(!(rtimer_set(t, last + CONTENTION_PREPARE + (CONTENTION_TICKS * (CONTENTION_SLOTS)), 1, (void (*)(struct rtimer *, void *))async_on, NULL) == RTIMER_OK)){
    //   printf("%s\n", "Could not schedule task!!!!!");
    // }
    rtimer_clock_t stall = last + CONTENTION_PREPARE + (CONTENTION_TICKS * (CONTENTION_SLOTS));
    // printf("STALLLLL: %u %u %u %u\n", RTIMER_NOW(), stall, REGULAR_SLOT, last);
    while(RTIMER_CLOCK_LT(RTIMER_NOW(), stall));
    if(!radio_is_on) on();
  } else {
    // we can snooze
    if(radio_is_on) off(TURN_OFF);

  }

}
/*---------------------------------------------------------------------------*/
static void async_on(struct rtimer *t, void *ptr, int status){
  if(!radio_is_on) on();
  if(!(rtimer_set(t, last + REGULAR_SLOT, 1, (void (*)(struct rtimer *, void *))advanceSlot, NULL) == RTIMER_OK)){
    printf("%s\n", "Could not schedule task!!!!!");
  }
}
/*---------------------------------------------------------------------------*/
static void init(void){
  current_slot = TOTAL_SLOTS + 1;

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
static unsigned short map_rand(unsigned short r){
  unsigned short size_of_bracket = RANDOM_RAND_MAX / CONTENTION_SLOTS;
  int i;
  for (i = 0; i < CONTENTION_SLOTS; i++){
    if(r <= (size_of_bracket * (i + 1))){
      return i;
    }
  }
  return CONTENTION_SLOTS - 1;
}
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
      random_init(RTIMER_NOW() * node_id);
      unsigned short rand_slot = map_rand(random_rand());
      // make filler packet
      memcpy(contention_strobe, packetbuf_hdrptr(), len);
      // strobe needs to cover at least one CCA_CONTENTION slot
      // plus any additional slots we need to cover
      unsigned short j;
      unsigned short fill_amount = (CCA_CONTENTION_SIZE - len) + (CONTENTION_SIZE * ((CONTENTION_SLOTS - 1) - rand_slot));
      for(j = 0; j < fill_amount; j++){
        contention_strobe[len] = 7;
        len++;
      }

      rtimer_clock_t start_of_cont;

      // wait for prep period to end
      while(RTIMER_CLOCK_LT(RTIMER_NOW(), last + CONTENTION_PREPARE));
      start_of_cont = RTIMER_NOW();

      // wait for our random slot
      unsigned short cont_slot = 0;
      while(!(cont_slot == rand_slot)){
        while(RTIMER_CLOCK_LT(RTIMER_NOW(), start_of_cont + CONTENTION_TICKS));
        start_of_cont = RTIMER_NOW();
        cont_slot++;
      }

      // NOW its our turn
      if(!radio_is_on) on();
      queuebuf_to_packetbuf(pkt);
      // first, test the air
      if(NETSTACK_RADIO.channel_clear()){
        // send filler, if no err, we won contention
        int c_ret = NETSTACK_RADIO.send(contention_strobe, len);
        if(c_ret == RADIO_TX_OK){
          won_contention = 1;
        } else{
          won_contention = 0;
        }
      } else { // another packet in the air, we lost contention
        won_contention = 0;
      }

      if(won_contention){
        packetbuf_set_addr(PACKETBUF_ADDR_SENDER, &rimeaddr_node_addr);
        if(NETSTACK_FRAMER.create() < 0) {
          /* Failed to allocate space for headers */
          printf("WPI-MAC: send failed, too large header\n");
          ret = MAC_TX_ERR_FATAL;
        } else {
          // printf("%s - %u - %u\n", "Waiting to send...", current_slot, needed_slot);
          // while(radio_is_on == 0 && !(current_slot == needed_slot)); //wish i knew why this didn't work
          // rtimer_clock_t recent = RTIMER_TIME(&taskSlot);
          // rtimer_clock_t until = calcNext(current_slot, needed_slot);
          // while(RTIMER_CLOCK_LT(RTIMER_NOW(), recent + until));
          // printf("%s - %u - %u\n", "sending!", current_slot, needed_slot);
          // printf("%u\n", packetbuf_totlen());
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

        
        mac_call_sent_callback(sent, ptr, ret, 1);

      } else { // lost contention
        mac_call_sent_callback(sent, ptr, MAC_TX_COLLISION, 1);
      }
      
    }

    QueuedPacket *head = QPQueue[current_slot];
    QPQueue[current_slot] = head->next;
    head->ptr = NULL;
    queuebuf_free(head->packet);
    head->packet = NULL;
    head->next = NULL;
    free(head);
}
/*---------------------------------------------------------------------------*/
static void schedule_outgoing_packet(unsigned char slot, mac_callback_t sent, void *ptr, struct queuebuf *pkt){
   if(slot != node_id) {
    if(QPQueue[slot] == NULL){
      QPQueue[slot] = (QueuedPacket*) malloc(sizeof(QueuedPacket));
      if(QPQueue[slot] == NULL){
        printf("WPI-MAC RAN OUT OF MEMORY.\n");
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
        printf("WPI-MAC RAN OUT OF MEMORY.\n");
      } else{
        curr->sent = sent;
        curr->ptr = ptr;
        curr->packet = pkt;
        curr->next = NULL;
      }
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