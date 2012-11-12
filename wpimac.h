/*
 * Copyright (c) 2012, Worcester Polytechnic Institute.
 * All rights reserved.
 *
 * This file is part of the Contiki operating system.
 */

/**
 * \file
 *         A MAC protocol implementation.
 * \author
 *         Chris Pinola <cpinola@wpi.edu>
 */

#ifndef __WPIMAC_H__
#define __WPIMAC_H__

#include <stdio.h>
#include "sys/rtimer.h"
#include "net/mac/rdc.h"
#include "dev/radio.h"

struct xmac_config {
  rtimer_clock_t on_time;
  rtimer_clock_t off_time;
  rtimer_clock_t strobe_time;
  rtimer_clock_t strobe_wait_time;
};

extern const struct rdc_driver wpimac_driver;

extern uint16_t node_id;

#endif /* __WPIMAC_H__ */
