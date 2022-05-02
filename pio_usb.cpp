/**
 * Copyright (c) 2021 sekigon-gonnoc
 */

#pragma GCC push_options
#pragma GCC optimize("-O3")

#include <Arduino.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h> // memcpy

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "pio_usb.h"
#include "usb_definitions.h"
#include "usb_crc.h"
#include "usb_tx.pio.h"
#include "usb_rx.pio.h"

#define UNUSED_PARAMETER(x) (void)x

#define IRQ_TX_EOP_MASK (1 << usb_tx_fs_IRQ_EOP)
#define IRQ_TX_COMP_MASK (1 << usb_tx_fs_IRQ_COMP)
#define IRQ_TX_ALL_MASK (IRQ_TX_EOP_MASK | IRQ_TX_COMP_MASK)
#define IRQ_RX_COMP_MASK (1 << IRQ_RX_EOP)
#define IRQ_RX_ALL_MASK ((1 << IRQ_RX_EOP) | (1 << IRQ_RX_BS_ERR) | (1 << IRQ_RX_START))

typedef struct {
  uint16_t div_int;
  uint8_t div_frac;
} pio_clk_div_t;

typedef struct {
  PIO pio_usb_tx;  // colud not set to volatile
  uint sm_tx;
  uint offset_tx;
  uint tx_ch;

  PIO pio_usb_rx;  // colud not set to volatile
  uint sm_rx;
  uint offset_rx;
  uint sm_eop;
  uint offset_eop;
  uint rx_reset_instr;
  uint device_rx_irq_num;

  pio_clk_div_t clk_div_fs_tx;
  pio_clk_div_t clk_div_fs_rx;
  pio_clk_div_t clk_div_ls_tx;
  pio_clk_div_t clk_div_ls_rx;

  bool need_pre;

  uint8_t usb_rx_buffer[128];
} pio_port_t;

static usb_device_t usb_device[PIO_USB_DEVICE_CNT];
static pio_port_t pio_port[1];
static root_port_t root_port[PIO_USB_ROOT_PORT_CNT];
static endpoint_t ep_pool[PIO_USB_EP_POOL_CNT];

static pio_usb_configuration_t current_config;

#define SM_SET_CLKDIV(pio, sm, div) pio_sm_set_clkdiv_int_frac(pio, sm, div.div_int, div.div_frac)

static void __no_inline_not_in_flash_func(send_pre)(const pio_port_t *pp) {
  uint8_t data[] = {USB_SYNC, USB_PID_PRE};

  // send PRE token in full-speed
  pio_sm_set_enabled(pp->pio_usb_tx, pp->sm_tx, false);
  for (uint i = 0; i < USB_TX_EOP_DISABLER_LEN; ++i) {
    uint16_t instr = usb_tx_fs_pre_program.instructions[i + USB_TX_EOP_OFFSET];
    pp->pio_usb_tx->instr_mem[pp->offset_tx + i + USB_TX_EOP_OFFSET] = instr;
  }

  SM_SET_CLKDIV(pp->pio_usb_tx, pp->sm_tx, pp->clk_div_fs_tx);

  dma_channel_transfer_from_buffer_now(pp->tx_ch, data, 2);

  pio_sm_set_enabled(pp->pio_usb_tx, pp->sm_tx, true);
  pp->pio_usb_tx->irq |= IRQ_TX_ALL_MASK;  // clear complete flag
  pp->pio_usb_tx->irq_force |= IRQ_TX_EOP_MASK;  // disable eop

  while ((pp->pio_usb_tx->irq & IRQ_TX_COMP_MASK) == 0) {
    continue;
  }

  // change bus speed to low-speed
  pio_sm_set_enabled(pp->pio_usb_tx, pp->sm_tx, false);
  for (uint i = 0; i < USB_TX_EOP_DISABLER_LEN; ++i) {
    uint16_t instr = usb_tx_fs_program.instructions[i + USB_TX_EOP_OFFSET];
    pp->pio_usb_tx->instr_mem[pp->offset_tx + i + USB_TX_EOP_OFFSET] = instr;
  }
  SM_SET_CLKDIV(pp->pio_usb_tx, pp->sm_tx, pp->clk_div_ls_tx);

  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
  SM_SET_CLKDIV(pp->pio_usb_rx, pp->sm_rx, pp->clk_div_ls_rx);

  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_eop, false);
  SM_SET_CLKDIV(pp->pio_usb_rx, pp->sm_eop, pp->clk_div_ls_rx);
  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_eop, true);
}

static void __no_inline_not_in_flash_func(restore_fs_bus)(const pio_port_t *pp) {
  // change bus speed to full-speed
  pio_sm_set_enabled(pp->pio_usb_tx, pp->sm_tx, false);
  SM_SET_CLKDIV(pp->pio_usb_tx, pp->sm_tx, pp->clk_div_fs_tx);

  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
  SM_SET_CLKDIV(pp->pio_usb_rx, pp->sm_rx, pp->clk_div_fs_rx);
  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, true);

  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_eop, false);
  SM_SET_CLKDIV(pp->pio_usb_rx, pp->sm_eop, pp->clk_div_fs_rx);
  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_eop, true);
}

static void __not_in_flash_func(usb_transfer)(const pio_port_t *pp,
                                              uint8_t *data, uint16_t len) {
  if (pp->need_pre) {
    send_pre(pp);
  }

  dma_channel_transfer_from_buffer_now(pp->tx_ch, data, len);

  pio_sm_set_enabled(pp->pio_usb_tx, pp->sm_tx, true);
  pp->pio_usb_tx->irq |= IRQ_TX_ALL_MASK;  // clear complete flag

  while ((pp->pio_usb_tx->irq & IRQ_TX_ALL_MASK) == 0) {
    continue;
  }
}

void __no_inline_not_in_flash_func(send_setup_packet)(const pio_port_t *pp,
                                                      uint8_t addr,
                                                      uint8_t ep_num) {
  uint8_t packet[] = {USB_SYNC, USB_PID_SETUP, 0, 0};
  uint16_t dat = ((uint16_t)(ep_num & 0xf) << 7) | (addr & 0x7f);
  uint8_t crc = calc_usb_crc5(dat);
  packet[2] = dat & 0xff;
  packet[3] = (crc << 3) | ((dat >> 8) & 0x1f);

  usb_transfer(pp, packet, sizeof(packet));
}

void __no_inline_not_in_flash_func(send_out_token)(const pio_port_t *pp,
                                                   uint8_t addr,
                                                   uint8_t ep_num) {
  uint8_t packet[] = {USB_SYNC, USB_PID_OUT, 0, 0};
  uint16_t dat = ((uint16_t)(ep_num & 0xf) << 7) | (addr & 0x7f);
  uint8_t crc = calc_usb_crc5(dat);
  packet[2] = dat & 0xff;
  packet[3] = (crc << 3) | ((dat >> 8) & 0x1f);

  usb_transfer(pp, packet, sizeof(packet));
}

static void __no_inline_not_in_flash_func(send_ack)(const pio_port_t *pp) {
  uint8_t data[] = {USB_SYNC, USB_PID_ACK};

  if (pp->need_pre) {
    send_pre(pp);
  }

  dma_channel_transfer_from_buffer_now(pp->tx_ch, data, 2);

  pio_sm_set_enabled(pp->pio_usb_tx, pp->sm_tx, true);
  pp->pio_usb_tx->irq |= IRQ_TX_ALL_MASK;  // clear complete flag

  while ((pp->pio_usb_tx->irq & IRQ_TX_COMP_MASK) == 0) {
    continue;
  }
}



void __no_inline_not_in_flash_func(send_nak)(const pio_port_t *pp) {
  uint8_t data[] = {USB_SYNC, USB_PID_NAK};
  usb_transfer(pp, data, sizeof(data));
}

static void __no_inline_not_in_flash_func(prepare_receive)(const pio_port_t *pp) {
  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
  pio_sm_clear_fifos(pp->pio_usb_rx, pp->sm_rx);
  pio_sm_restart(pp->pio_usb_rx, pp->sm_rx);
  pio_sm_exec(pp->pio_usb_rx, pp->sm_rx, pp->rx_reset_instr);
}

static void __no_inline_not_in_flash_func(start_receive)(const pio_port_t *pp) {
  pp->pio_usb_rx->ctrl |= (1 << pp->sm_rx);
  pp->pio_usb_rx->irq |= IRQ_RX_ALL_MASK;
}

void __no_inline_not_in_flash_func(data_transfer)(const pio_port_t *pp,
                                                  uint8_t *tx_data_address,
                                                  uint8_t tx_data_len) {
  prepare_receive(pp);
  usb_transfer(pp, tx_data_address, tx_data_len);
  start_receive(pp);
}

void __no_inline_not_in_flash_func(control_setup_transfer)(
    const pio_port_t *pp, uint8_t device_address, uint8_t *tx_data_address,
    uint8_t tx_data_len) {
  prepare_receive(pp);

  send_setup_packet(pp, device_address, 0);
  // ensure previous tx complete
  while ((pp->pio_usb_tx->irq & IRQ_TX_COMP_MASK) == 0) {
    continue;
  }
  usb_transfer(pp, tx_data_address, tx_data_len);

  start_receive(pp);
}

void __no_inline_not_in_flash_func(control_out_transfer)(
    const pio_port_t *pp, uint8_t device_address, uint8_t *tx_data_address,
    uint8_t tx_data_len) {
  prepare_receive(pp);

  send_out_token(pp, device_address, 0);
  // ensure previous tx complete
  while ((pp->pio_usb_tx->irq & IRQ_TX_COMP_MASK) == 0) {
    continue;
  }
  usb_transfer(pp, tx_data_address, tx_data_len);

  start_receive(pp);
}

void  __no_inline_not_in_flash_func(calc_in_token)(uint8_t * packet, uint8_t addr, uint8_t ep_num) {
  uint16_t dat = ((uint16_t)(ep_num & 0xf) << 7) | (addr & 0x7f);
  uint8_t crc = calc_usb_crc5(dat);
  packet[0] = USB_SYNC;
  packet[1] = USB_PID_IN;
  packet[2] = dat & 0xff;
  packet[3] = (crc << 3) | ((dat >> 8) & 0x1f);
}

static void __no_inline_not_in_flash_func(wait_handshake)(pio_port_t* pp) {
  int16_t t = 240;
  int16_t idx = 0;

  while (t--) {
    if (pio_sm_get_rx_fifo_level(pp->pio_usb_rx, pp->sm_rx)) {
      uint8_t data = pio_sm_get(pp->pio_usb_rx, pp->sm_rx) >> 24;
      pp->usb_rx_buffer[idx++] = data;
      if (idx == 2) {
        break;
      }
    }
  }

  if (t > 0) {
    while ((pp->pio_usb_rx->irq & IRQ_RX_COMP_MASK) == 0) {
      continue;
    }
  }

  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
}

static int __no_inline_not_in_flash_func(usb_out_transaction)(pio_port_t* pp, uint8_t addr, endpoint_t * ep) {
  int res = -1;

  prepare_receive(pp);
  send_out_token(pp, addr, ep->ep_num & 0xf);
  // ensure previous tx complete
  while ((pp->pio_usb_tx->irq & IRQ_TX_COMP_MASK) == 0) {
    continue;
  }
  usb_transfer(pp, (uint8_t*)ep->buffer, ep->packet_len);
  start_receive(pp);

  wait_handshake(pp);

  if (pp->usb_rx_buffer[1] == USB_PID_ACK) {
    res = 0;
    ep->data_id ^= 1;
    ep->new_data_flag = false;
  } else {
    res = -1;
  }

  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
  pp->usb_rx_buffer[0] = 0;
  pp->usb_rx_buffer[1] = 0;

  return res;
}

static int __no_inline_not_in_flash_func(receive_packet_and_ack)(pio_port_t* pp) {
  uint16_t crc = 0xffff;
  uint16_t crc_prev = 0xffff;
  uint16_t crc_prev2 = 0xffff;
  uint16_t crc_receive = 0xffff;
  bool  crc_match = false;
  int16_t t = 240;
  uint16_t idx = 0;

  while (t--) {
    if (pio_sm_get_rx_fifo_level(pp->pio_usb_rx, pp->sm_rx)) {
      uint8_t data = pio_sm_get(pp->pio_usb_rx, pp->sm_rx) >> 24;
      pp->usb_rx_buffer[idx++] = data;
      if (idx == 2) {
        break;
      }
    }
  }

  // timing critical start
  if (t > 0) {
    while ((pp->pio_usb_rx->irq & IRQ_RX_COMP_MASK) == 0) {
      if (pio_sm_get_rx_fifo_level(pp->pio_usb_rx, pp->sm_rx)) {
        uint8_t data = pio_sm_get(pp->pio_usb_rx, pp->sm_rx) >> 24;
        crc_prev2 = crc_prev;
        crc_prev = crc;
        crc = update_usb_crc16(crc, data);
        pp->usb_rx_buffer[idx++] = data;
        crc_receive = (crc_receive >> 8) | (data << 8);
        crc_match = ((crc_receive ^ 0xffff) == crc_prev2);
      }
    }
  }

  if (idx >= 4 && crc_match) {
    send_ack(pp);
    // timing critical end
    return idx - 4;
  }

  return -1;
}

static int __no_inline_not_in_flash_func(usb_in_transaction)(pio_port_t* pp, uint8_t addr, endpoint_t * ep) {
  int res = -1;

  uint8_t expect_token = ep->data_id == 0 ? USB_PID_DATA0 : USB_PID_DATA1;
  uint8_t packet[4];
  calc_in_token(packet, addr, ep->ep_num & 0x0f);
  data_transfer(pp, packet, sizeof(packet));

  int receive_len = receive_packet_and_ack(pp);
  if (receive_len >= 0) {
    if (pp->usb_rx_buffer[1] == expect_token) {
      memcpy((void *)(ep->buffer), &pp->usb_rx_buffer[2], receive_len);
      ep->packet_len = receive_len;
      ep->data_id ^= 1;
      ep->new_data_flag = true;
    }
    res = 0;
  } else {
    res = -1;
    if ((pp->pio_usb_rx->irq & IRQ_RX_COMP_MASK) == 0) {
      res = -2;
    }
  }

  pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
  pp->usb_rx_buffer[0] = 0;
  pp->usb_rx_buffer[1] = 0;

  return res;
}

static int __no_inline_not_in_flash_func(usb_control_out_transfer)(
    pio_port_t *pp, uint8_t addr, control_pipe_t *pipe) {
  int res = 0;

  if (pipe->stage == STAGE_SETUP) {
    control_setup_transfer(pp, addr, pipe->setup_packet.tx_address,
                           pipe->setup_packet.tx_length);
    wait_handshake(pp);

    if (pp->usb_rx_buffer[0] == USB_SYNC && pp->usb_rx_buffer[1] == USB_PID_ACK) {
      if (pipe->out_data_packet.tx_address != NULL) {
        pipe->stage = STAGE_OUT;
      } else {
        pipe->stage = STAGE_STATUS;
      }
      pp->usb_rx_buffer[1] = 0;  // reset buffer
    }
  } else if (pipe->stage == STAGE_OUT) {
    control_out_transfer(pp, addr, pipe->out_data_packet.tx_address,
                         pipe->out_data_packet.tx_length);
    wait_handshake(pp);
    if (pp->usb_rx_buffer[0] == USB_SYNC && pp->usb_rx_buffer[1] == USB_PID_ACK) {
        pipe->stage = STAGE_STATUS;
      }
      pp->usb_rx_buffer[1] = 0;
  } else if (pipe->stage == STAGE_STATUS) {
    uint8_t packet[4];
    calc_in_token(packet, addr, 0);
    data_transfer(pp, packet, sizeof(packet));

    volatile int16_t t = 240;
    volatile int16_t idx = 0;
    while (t--) {
      if (pio_sm_get_rx_fifo_level(pp->pio_usb_rx, pp->sm_rx)) {
        pp->usb_rx_buffer[idx++] = pio_sm_get(pp->pio_usb_rx, pp->sm_rx) >> 24;
        if (idx == 2) {
          break;
        }
      }
    }

    if (t > 0) {
      while ((pp->pio_usb_rx->irq & IRQ_RX_COMP_MASK) == 0) {
        if (pio_sm_get_rx_fifo_level(pp->pio_usb_rx, pp->sm_rx)) {
          pp->usb_rx_buffer[idx++] = pio_sm_get(pp->pio_usb_rx, pp->sm_rx) >> 24;
        }
      }

      if (pp->usb_rx_buffer[1] == USB_PID_DATA1) {
        send_ack(pp);
        pipe->stage = STAGE_COMPLETE;
      }
    }

    pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
    pp->usb_rx_buffer[1] = 0;
  }

  return res;
}

static uint8_t __no_inline_not_in_flash_func(usb_control_in_transfer)(pio_port_t* pp, uint8_t addr, control_pipe_t * pipe) {
  if (pipe->stage == STAGE_SETUP) {
    control_setup_transfer(pp, addr, pipe->setup_packet.tx_address,
                           pipe->setup_packet.tx_length);

    wait_handshake(pp);

    if (pp->usb_rx_buffer[0] == USB_SYNC && pp->usb_rx_buffer[1] == USB_PID_ACK) {
      pipe->stage = STAGE_IN;
      pp->usb_rx_buffer[1] = 0; // reset buffer
      pipe->data_in_num = 1;
      pipe->buffer_idx = 0;
    }
  } else if (pipe->stage == STAGE_IN) {
    uint8_t expect_token =
        pipe->data_in_num == 1 ? USB_PID_DATA1 : USB_PID_DATA0;
    uint8_t packet[4];
    calc_in_token(packet, addr, 0);
    data_transfer(pp, packet, sizeof(packet));

    int receive_len = receive_packet_and_ack(pp);

    if (receive_len >= 0) {
      if (pp->usb_rx_buffer[1] == expect_token) {
        memcpy((void *)(pipe->rx_buffer + pipe->buffer_idx), &pp->usb_rx_buffer[2],
               receive_len);
        pipe->buffer_idx += receive_len;

        if (pipe->request_length <= pipe->buffer_idx) {
          pipe->stage = STAGE_STATUS;
        }

        pipe->data_in_num ^= 1;  // togle data num
      }
    }

    pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
    pp->usb_rx_buffer[1] = 0;  // reset buffer
  }

  else if (pipe->stage == STAGE_STATUS) {
    uint8_t status_packet[] = {USB_SYNC, USB_PID_DATA1, 0, 0};
    control_out_transfer(pp, addr, status_packet,
                         sizeof(status_packet));

    wait_handshake(pp);

    if (pp->usb_rx_buffer[0] == USB_SYNC && pp->usb_rx_buffer[1] == USB_PID_ACK) {
      pipe->stage = STAGE_COMPLETE;
      pp->usb_rx_buffer[1] = 0; // reset buffer
    } else if (pp->usb_rx_buffer[0] == USB_SYNC && pp->usb_rx_buffer[1] == USB_PID_NAK) {
      pipe->stage = STAGE_STATUS;
      pp->usb_rx_buffer[1] = 0; // reset buffer
    } else {
      pipe->stage = STAGE_ERROR;
      pp->usb_rx_buffer[1] = 0; // reset buffer
    }
  }

  return pipe->stage;
}

typedef enum{
  PORT_PIN_SE0,
  PORT_PIN_FS_IDLE,
  PORT_PIN_LS_IDLE,
  PORT_PIN_SE1,
} port_pin_status_t;

static port_pin_status_t __no_inline_not_in_flash_func(get_port_pin_status)(
    root_port_t *port) {
  bool dp = gpio_get(port->pin_dp);
  bool dm = gpio_get(port->pin_dm);

  if (dp == false && dm == false) {
    return PORT_PIN_SE0;
  } else if (dp == true && dm == false) {
    return PORT_PIN_FS_IDLE;
  } else if (dp == false && dm == true) {
    return PORT_PIN_LS_IDLE;
  }

  return PORT_PIN_SE1;
}

static bool __no_inline_not_in_flash_func(connection_check)(root_port_t *port) {
  if (get_port_pin_status(port) == PORT_PIN_SE0) {
    busy_wait_us_32(1);

    if (get_port_pin_status(port) == PORT_PIN_SE0) {
      busy_wait_us_32(1);
      // device disconnect
      port->event = EVENT_DISCONNECT;
      return false;
    }
  }

  return true;
}

static void __no_inline_not_in_flash_func(override_pio_program)(PIO pio, const pio_program_t* program, uint offset) {
    for (uint i = 0; i < program->length; ++i) {
      uint16_t instr = program->instructions[i];
      pio->instr_mem[offset + i] =
          pio_instr_bits_jmp != _pio_major_instr_bits(instr) ? instr
                                                             : instr + offset;
    }
}

static __always_inline void override_pio_rx_program(PIO pio,
                                             const pio_program_t *program,
                                             const pio_program_t *debug_program,
                                             uint offset, int debug_pin) {
  if (debug_pin < 0) {
    override_pio_program(pio, program, offset);
  } else {
    override_pio_program(pio, debug_program, offset);
  }
}

static __always_inline void add_pio_host_rx_program(PIO pio,
                                             const pio_program_t *program,
                                             const pio_program_t *debug_program,
                                             uint *offset, int debug_pin) {
  if (debug_pin < 0) {
    *offset = pio_add_program(pio, program);
  } else {
    *offset = pio_add_program(pio, debug_program);
  }
}

static void __no_inline_not_in_flash_func(initialize_host_programs)(
    pio_port_t *pp, const pio_usb_configuration_t *c, root_port_t *port) {
  pp->offset_tx = pio_add_program(pp->pio_usb_tx, &usb_tx_fs_program);
  usb_tx_fs_program_init(pp->pio_usb_tx, pp->sm_tx, pp->offset_tx,
                         port->pin_dp);

  add_pio_host_rx_program(pp->pio_usb_rx, &usb_rx_fs_program,
                          &usb_rx_fs_debug_program, &pp->offset_rx,
                          c->debug_pin_rx);
  usb_rx_fs_program_init(pp->pio_usb_rx, pp->sm_rx, pp->offset_rx, port->pin_dp,
                         c->debug_pin_rx);
  pp->rx_reset_instr = pio_encode_jmp(pp->offset_rx);

  add_pio_host_rx_program(pp->pio_usb_rx, &eop_detect_fs_program,
                          &eop_detect_fs_debug_program, &pp->offset_eop,
                          c->debug_pin_eop);
  eop_detect_fs_program_init(pp->pio_usb_rx, c->sm_eop, pp->offset_eop,
                             port->pin_dp, true, c->debug_pin_eop);

  usb_tx_configure_pins(pp->pio_usb_tx, pp->sm_tx, port->pin_dp);
  usb_rx_configure_pins(pp->pio_usb_rx, pp->sm_eop, port->pin_dp);
  usb_rx_configure_pins(pp->pio_usb_rx, pp->sm_rx, port->pin_dp);
}

static void __no_inline_not_in_flash_func(configure_fullspeed_host)(
    pio_port_t *pp, const pio_usb_configuration_t *c, root_port_t *port) {
  override_pio_program(pp->pio_usb_tx, &usb_tx_fs_program, pp->offset_tx);
  SM_SET_CLKDIV(pp->pio_usb_tx, pp->sm_tx, pp->clk_div_fs_tx);

  override_pio_rx_program(pp->pio_usb_rx, &usb_rx_fs_program,
                          &usb_rx_fs_debug_program, pp->offset_rx,
                          c->debug_pin_rx);
  SM_SET_CLKDIV(pp->pio_usb_rx, pp->sm_rx, pp->clk_div_fs_rx);

  override_pio_rx_program(pp->pio_usb_rx, &eop_detect_fs_program,
                          &eop_detect_fs_debug_program, pp->offset_eop,
                          c->debug_pin_eop);
  SM_SET_CLKDIV(pp->pio_usb_rx, pp->sm_eop, pp->clk_div_fs_rx);

  usb_tx_configure_pins(pp->pio_usb_tx, pp->sm_tx, port->pin_dp);
  usb_rx_configure_pins(pp->pio_usb_rx, pp->sm_eop, port->pin_dp);
  usb_rx_configure_pins(pp->pio_usb_rx, pp->sm_rx, port->pin_dp);
}

static void __no_inline_not_in_flash_func(configure_lowspeed_host)(
    pio_port_t *pp, const pio_usb_configuration_t *c, root_port_t *port) {
  override_pio_program(pp->pio_usb_tx, &usb_tx_ls_program, pp->offset_tx);
  SM_SET_CLKDIV(pp->pio_usb_tx, pp->sm_tx, pp->clk_div_ls_tx);

  override_pio_rx_program(pp->pio_usb_rx, &usb_rx_ls_program,
                          &usb_rx_ls_debug_program, pp->offset_rx,
                          c->debug_pin_rx);
  SM_SET_CLKDIV(pp->pio_usb_rx, pp->sm_rx, pp->clk_div_ls_rx);

  override_pio_rx_program(pp->pio_usb_rx, &eop_detect_ls_program,
                          &eop_detect_ls_debug_program, pp->offset_eop,
                          c->debug_pin_eop);
  SM_SET_CLKDIV(pp->pio_usb_rx, pp->sm_eop, pp->clk_div_ls_rx);

  usb_tx_configure_pins(pp->pio_usb_tx, pp->sm_tx, port->pin_dp);
  usb_rx_configure_pins(pp->pio_usb_rx, pp->sm_eop, port->pin_dp);
  usb_rx_configure_pins(pp->pio_usb_rx, pp->sm_rx, port->pin_dm);
}

static void __no_inline_not_in_flash_func(configure_root_port)(
    pio_port_t *pp, root_port_t *root) {
  usb_device_t *root_device = root->root_device;
  bool is_fs = root_device->is_fullspeed || (!root_device->is_root);
  if (is_fs) {
    configure_fullspeed_host(pp, &current_config, root);
  } else {
    configure_lowspeed_host(pp, &current_config, root);
  }
}

static void __no_inline_not_in_flash_func(process_control_transfer)(
    pio_port_t *pp, root_port_t *root, usb_device_t *device) {
  uint8_t addr = device->address;
  control_pipe_t *pipe = &device->control_pipe;

  if (!device->connected || (device->root != root)) {
    return;
  }

  if ((!device->is_root) && (!device->is_fullspeed)) {
    pp->need_pre = true;
  }

  switch (pipe->operation) {
    case CONTROL_NONE:
      break;
    case CONTROL_IN:
      usb_control_in_transfer(pp, addr, pipe);
      if (pipe->stage == STAGE_COMPLETE) {
        pipe->stage = STAGE_SETUP;
        pipe->operation = CONTROL_COMPLETE;
      } else if (pipe->stage == STAGE_ERROR) {
        pipe->stage = STAGE_SETUP;
        pipe->operation = CONTROL_ERROR;
      }
      break;
    case CONTROL_OUT:
      usb_control_out_transfer(pp, addr, pipe);
      if (pipe->stage == STAGE_COMPLETE) {
        pipe->stage = STAGE_SETUP;
        pipe->operation = CONTROL_COMPLETE;
      } else if (pipe->stage == STAGE_ERROR) {
        pipe->stage = STAGE_SETUP;
        pipe->operation = CONTROL_ERROR;
      }
      break;
    default:
      break;
  }

  if ((!device->is_root) && (!device->is_fullspeed)) {
    pp->need_pre = false;
    restore_fs_bus(pp);
  }
}

static void __no_inline_not_in_flash_func(process_interrupt_transfer)(
    pio_port_t *pp, root_port_t *root, usb_device_t *device) {

  if (!device->connected || device->root != root) {
    return;
  }

  if ((!device->is_root) && (!device->is_fullspeed)) {
    pp->need_pre = true;
  }

  for (int ep_idx = 0; ep_idx < PIO_USB_DEV_EP_CNT; ep_idx++) {
    endpoint_t *ep = pio_usb_get_endpoint(device, ep_idx);
    if (ep == NULL) {
      break;
    }

    if (!ep->ep_num || ep->attr != EP_ATTR_INTERRUPT) {
      continue;
    }

    if (ep->interval_counter > 0) {
      ep->interval_counter--;
      continue;
    }

    if (ep->ep_num & EP_IN) {
      int res = usb_in_transaction(pp, device->address, ep);
      ep->interval_counter = ep->interval - 1;
      if (res == 0 && device->device_class == CLASS_HUB) {
        device->event = EVENT_HUB_PORT_CHANGE;
      } else if (res <= -2) {
        // fatal
        break;
      }
    } else {
      // EP_OUT
      if (ep->new_data_flag) {
        int res = usb_out_transaction(pp, device->address, ep);
        ep->interval_counter = ep->interval - 1;
        if (res == 0) {
          ep->interval_counter = ep->interval - 1;
        }
      }
    }
  }

  if ((!device->is_root) && (!device->is_fullspeed)) {
    pp->need_pre = false;
    restore_fs_bus(pp);
  }
}

static bool __no_inline_not_in_flash_func(sof_timer)(repeating_timer_t *_rt) {
  static uint8_t sof_packet[4] = {USB_SYNC, USB_PID_SOF, 0x00, 0x10};
  static uint8_t sof_count = 0;
  UNUSED_PARAMETER(_rt);

  pio_port_t *pp = &pio_port[0];

  for (int root_idx = 0; root_idx < PIO_USB_ROOT_PORT_CNT; root_idx++) {
    root_port_t *active_root = &root_port[root_idx];
    usb_device_t *root_device = active_root->root_device;
    if (!(active_root->initialized && root_device != NULL &&
        root_device->connected && connection_check(active_root))) {
      continue;
    }
    configure_root_port(pp, active_root);
    usb_transfer(pp, sof_packet, 4);
  }

  for (int root_idx = 0; root_idx < PIO_USB_ROOT_PORT_CNT; root_idx++) {
    root_port_t *active_root = &root_port[root_idx];
    usb_device_t *root_device = active_root->root_device;
    if (!(active_root->initialized && root_device != NULL &&
        root_device->connected)) {
      continue;
    }

    configure_root_port(pp, active_root);

    for (int idx = 0; idx < PIO_USB_DEVICE_CNT; idx++) {
      usb_device_t *device = &usb_device[idx];
      process_control_transfer(pp, active_root, device);
    }

    for (int idx = 0; idx < PIO_USB_DEVICE_CNT; idx++) {
      usb_device_t *device = &usb_device[idx];
      process_interrupt_transfer(pp, active_root, device);
    }
  }

  for (int root_idx = 0; root_idx < PIO_USB_ROOT_PORT_CNT; root_idx++) {
    root_port_t *active_root = &root_port[root_idx];
    if ((!active_root->initialized) || (active_root->root_device != NULL)) {
      continue;
    }

    if (active_root->event == EVENT_NONE &&
        (get_port_pin_status(active_root) == PORT_PIN_FS_IDLE ||
         (get_port_pin_status(active_root) == PORT_PIN_LS_IDLE))) {
      active_root->event = EVENT_CONNECT;
    }
  }

  sof_count = (sof_count + 1) & 0x1f;
  sof_packet[2] = sof_count & 0xff;
  sof_packet[3] = (calc_usb_crc5(sof_count) << 3) | (sof_count >> 8);

  return true;
}

static void on_device_connect(pio_port_t *pp, root_port_t *root,
                              int device_idx) {
  bool fullspeed_flag = false;

  if (get_port_pin_status(root) == PORT_PIN_FS_IDLE) {
    fullspeed_flag = true;
  } else if (get_port_pin_status(root) == PORT_PIN_LS_IDLE) {
    fullspeed_flag = false;
  }

  pio_sm_set_pins_with_mask(pp->pio_usb_tx, pp->sm_tx, (0b00 << root->pin_dp),
                            (0b11u << root->pin_dp));
  pio_sm_set_pindirs_with_mask(pp->pio_usb_tx, pp->sm_tx, (0b11u << root->pin_dp),
                               (0b11u << root->pin_dp));

  busy_wait_ms(100);

  pio_sm_set_pindirs_with_mask(pp->pio_usb_tx, pp->sm_tx, (0b00u << root->pin_dp),
                               (0b11u << root->pin_dp));

  busy_wait_us(100);

  if (fullspeed_flag && get_port_pin_status(root) == PORT_PIN_FS_IDLE) {
    root->root_device = &usb_device[device_idx];
    if (!root->root_device->connected) {
      configure_fullspeed_host(pp, &current_config, root);
      root->root_device->is_fullspeed = true;
      root->root_device->is_root = true;
      root->root_device->connected = true;
      root->root_device->root = root;
      root->root_device->event = EVENT_CONNECT;
    }
  } else if (!fullspeed_flag && get_port_pin_status(root) == PORT_PIN_LS_IDLE) {
    root->root_device = &usb_device[device_idx];
    if (!root->root_device->connected) {
      configure_lowspeed_host(pp, &current_config, root);
      root->root_device->is_fullspeed = false;
      root->root_device->is_root = true;
      root->root_device->connected = true;
      root->root_device->root = root;
      root->root_device->event = EVENT_CONNECT;
    }
  }
}

static void update_packet_crc16(usb_setup_packet_t * packet) {
  uint16_t crc16 = calc_usb_crc16(&packet->request_type,
                                  sizeof(*packet) - 4);
  packet->crc16[0] = crc16 & 0xff;
  packet->crc16[1] = crc16 >> 8;
}

static int __no_inline_not_in_flash_func(control_out_protocol)(
    usb_device_t *device, uint8_t *setup_data, uint16_t setup_length,
    uint8_t *out_data, uint16_t out_length) {
  int res = 0;

  control_pipe_t *pipe = &device->control_pipe;

  if (pipe->operation == CONTROL_NONE) {
    pipe->setup_packet.tx_address = setup_data;
    pipe->setup_packet.tx_length = setup_length;
    pipe->out_data_packet.tx_address = out_data;
    pipe->out_data_packet.tx_length = out_length;
    pipe->operation = CONTROL_OUT;
  } else {
    return -1;
  }

  const uint64_t timeout = 5000 * 1000;  // 5s
  uint64_t start_time = time_us_64();
  while (pipe->operation == CONTROL_OUT &&
         time_us_64() - start_time < timeout) {
    if (!device->connected) {
      pipe->operation = CONTROL_NONE;
      return -1;
    }
  }

  if (time_us_64() - start_time >= timeout) {
    Serial.printf("control out[timeout]\n");
    res = -2;
  } else if (pipe->operation == CONTROL_ERROR) {
    Serial.printf("control out[error]\n");
    res = -1;
  } else if (pipe->operation == CONTROL_COMPLETE) {
    Serial.printf("control out[complete]\n");
    res = 0;
  }
    pipe->operation = CONTROL_NONE;

  return res;
}

static int __no_inline_not_in_flash_func(control_in_protocol)(
    usb_device_t *device, uint8_t *tx_data, uint16_t tx_length,
    uint8_t *rx_buffer, uint16_t request_length) {
  int res = 0;

  control_pipe_t *pipe = &device->control_pipe;

  if (pipe->operation == CONTROL_NONE) {
    pipe->setup_packet.tx_address = tx_data;
    pipe->setup_packet.tx_length = tx_length;
    pipe->rx_buffer = rx_buffer;
    pipe->request_length = request_length;
    pipe->operation = CONTROL_IN;
  } else {
    return -1;
  }

  const uint64_t timeout = 5000 * 1000;  // 5s
  uint64_t start_time = time_us_64();
  while (pipe->operation == CONTROL_IN &&
         time_us_64() - start_time < timeout) {
    if (!device->connected) {
      pipe->operation = CONTROL_NONE;
      return -1;
    }
  }

  if (time_us_64() - start_time >= timeout) {
    Serial.printf("control in[timeout]\n");
    res = -2;
  } else if (pipe->operation == CONTROL_ERROR) {
    Serial.printf("control in[error]\n");
    res = -1;
  } else if (pipe->operation == CONTROL_COMPLETE) {
    Serial.printf("control in[complete]\n");
    res = 0;
  }
  pipe->operation = CONTROL_NONE;

  return res;
}

int __no_inline_not_in_flash_func(pio_usb_get_in_data)(endpoint_t *ep,
                                                       uint8_t *buffer,
                                                       uint8_t len) {
  if ((ep->ep_num & EP_IN) && ep->new_data_flag) {
    len = len < ep->packet_len ? len : ep->packet_len;
    memcpy(buffer, (void *)ep->buffer, len);

    ep->new_data_flag = false;

    return len;
  }

  return -1;
}

int __no_inline_not_in_flash_func(pio_usb_set_out_data)(endpoint_t *ep,
                                                          const uint8_t *buffer,
                                                          uint8_t len) {
  if (ep->new_data_flag || (ep->ep_num & EP_IN)) {
    return -1;
  }

  ep->buffer[0] = USB_SYNC;
  ep->buffer[1] = ep->data_id == 0 ? USB_PID_DATA0 : USB_PID_DATA1;
  memcpy((uint8_t *)&ep->buffer[2], buffer, len);
  uint16_t crc = calc_usb_crc16(buffer, len);
  ep->buffer[2 + len] = crc & 0xff;
  ep->buffer[2 + len + 1] = (crc >> 8) & 0xff;
  ep->packet_len = len + 4;

  ep->new_data_flag = true;
  return 0;
}

static int set_hub_feature(usb_device_t *device, uint8_t port, uint8_t value) {
  usb_setup_packet_t req = SET_HUB_FEATURE_REQUEST;
  req.index_lsb = port + 1;
  req.value_lsb = value;
  update_packet_crc16(&req);
  return control_out_protocol(device, (uint8_t *)&req, sizeof(req), NULL, 0);
}

static int clear_hub_feature(usb_device_t *device, uint8_t port,
                             uint8_t value) {
  usb_setup_packet_t req = CLEAR_HUB_FEATURE_REQUEST;
  req.index_lsb = port + 1;
  req.value_lsb = value;
  update_packet_crc16(&req);
  return control_out_protocol(device, (uint8_t *)&req, sizeof(req), NULL, 0);
}

static int get_hub_port_status(usb_device_t *device, uint8_t port,
                               hub_port_status_t *status) {
  usb_setup_packet_t req = GET_HUB_PORT_STATUS_REQUEST;
  req.index_lsb = port + 1;
  update_packet_crc16(&req);
  return control_in_protocol(device, (uint8_t *)&req, sizeof(req), (uint8_t*)status,
                             sizeof(*status));
}

static int initialize_hub(usb_device_t *device) {
  uint8_t rx_buffer[16];
  int res = 0;
  Serial.printf("USB Hub detected\n");
  usb_setup_packet_t get_hub_desc_request = GET_HUB_DESCRPTOR_REQUEST;
  update_packet_crc16(&get_hub_desc_request);
  control_in_protocol(device, (uint8_t *)&get_hub_desc_request,
                      sizeof(get_hub_desc_request), rx_buffer, 8);
  const hub_descriptor_t *desc = (hub_descriptor_t *)rx_buffer;
  uint8_t port_num = desc->port_num;

  Serial.printf("\tTurn on port powers\n");
  for (int idx = 0; idx < port_num; idx++) {
    res = set_hub_feature(device, idx, HUB_SET_PORT_POWER);
    if (res != 0) {
      Serial.printf("\tFailed to turn on ports\n");
      break;
    }
  }

  busy_wait_ms(500);

  return res;
}

static int get_string_descriptor(usb_device_t *device, uint8_t idx,
                                 uint8_t *rx_buffer, uint8_t *str_buffer) {
  int res = -1;
  usb_setup_packet_t req = GET_DEVICE_DESCRIPTOR_REQ_DEFAULT;
  req.value_msb = DESC_TYPE_STRING;
  req.value_lsb = idx;
  req.length_lsb = 1;
  req.length_msb = 0;
  update_packet_crc16(&req);
  res = control_in_protocol(device, (uint8_t *)&req, sizeof(req), rx_buffer, 1);
  if (res != 0) {
    return res;
  }

  uint8_t len = rx_buffer[0];
  req.length_lsb = len;
  req.length_msb = 0;
  update_packet_crc16(&req);
  res =
      control_in_protocol(device, (uint8_t *)&req, sizeof(req), rx_buffer, len);
  if (res != 0) {
    return res;
  }

  uint16_t *wchar_buffer = (uint16_t *)rx_buffer;
  for (int i = 0; i < (len - 2) / 2; i++) {
    str_buffer[i] = wchar_buffer[i + 1];
  }
  str_buffer[(len - 2) / 2] = '\0';

  return res;
}

static int enumerate_device(usb_device_t *device, uint8_t address) {
  int res = 0;
  uint8_t rx_buffer[512];

  usb_setup_packet_t get_device_descriptor_request =
      GET_DEVICE_DESCRIPTOR_REQ_DEFAULT;
  update_packet_crc16(&get_device_descriptor_request);
  res = control_in_protocol(device, (uint8_t *)&get_device_descriptor_request,
                            sizeof(get_device_descriptor_request), rx_buffer, 18);
  if (res != 0) {
    return res;
  }

  const device_descriptor_t *desc =
      (device_descriptor_t *)device->control_pipe.rx_buffer;
  device->vid = desc->vid[0] | (desc->vid[1] << 8);
  device->pid = desc->pid[0] | (desc->pid[1] << 8);
  device->device_class = desc->device_class;
  uint8_t idx_manufacture = desc->manufacture;
  uint8_t idx_product = desc->product;
  uint8_t idx_serial = desc->serial;

  Serial.printf("Enumerating %04x:%04x, class:%d, address:%d\n", device->vid,
         device->pid, device->device_class, address);

  usb_setup_packet_t set_address_request = SET_ADDRESS_REQ_DEFAULT;
  set_address_request.value_lsb = address;
  set_address_request.value_msb = 0;
  update_packet_crc16(&set_address_request);
  res = control_out_protocol(device, (uint8_t *)&set_address_request,
                             sizeof(set_address_request), NULL, 0);
  if (res != 0) {
    return res;
  }
  device->address = address;

  uint8_t str[64];
  if (idx_manufacture != 0) {
    res = get_string_descriptor(device, idx_manufacture, rx_buffer, str);
    if (res == 0) {
      Serial.printf("Manufacture:%s\n", str);
    } else {
      Serial.printf("Failed to get string descriptor (Manufacture)\n");
    }
  }

  if (idx_product != 0) {
    res = get_string_descriptor(device, idx_product, rx_buffer, str);
    if (res == 0) {
      Serial.printf("Product:%s\n", str);
    } else {
      Serial.printf("Failed to get string descriptor (Product)\n");
    }
  }

  if (idx_serial != 0) {
    res = get_string_descriptor(device, idx_serial, rx_buffer, str);
    if (res == 0) {
      Serial.printf("Serial:%s\n", str);
    } else {
      Serial.printf("Failed to get string descriptor (Serial)\n");
    }
  }

  usb_setup_packet_t get_configuration_descriptor_request =
      GET_CONFIGURATION_DESCRIPTOR_REQ_DEFAULT;
  get_configuration_descriptor_request.length_lsb = 9;
  get_configuration_descriptor_request.length_msb = 0;
  update_packet_crc16(&get_configuration_descriptor_request);
  res = control_in_protocol(
      device, (uint8_t *)&get_configuration_descriptor_request,
      sizeof(get_configuration_descriptor_request), rx_buffer, 9);
  if (res != 0) {
    return res;
  }

  get_configuration_descriptor_request.length_lsb =
      ((configuration_descriptor_t *)(device->control_pipe.rx_buffer))
          ->total_length_lsb;
  get_configuration_descriptor_request.length_msb =
      ((configuration_descriptor_t *)(device->control_pipe.rx_buffer))
          ->total_length_msb;
  uint16_t request_length =
      get_configuration_descriptor_request.length_lsb |
      (get_configuration_descriptor_request.index_msb << 8);
  update_packet_crc16(&get_configuration_descriptor_request);
  res = control_in_protocol(
      device, (uint8_t *)&get_configuration_descriptor_request,
      sizeof(get_configuration_descriptor_request), rx_buffer, request_length);

  if (res != 0) {
    return res;
  }
  uint8_t configuration_descrptor_data[512];
  int16_t configuration_descrptor_length =
      request_length > 512 ? 512 : request_length;
  memcpy(configuration_descrptor_data,
         (const void *)device->control_pipe.rx_buffer,
         configuration_descrptor_length);

  usb_setup_packet_t set_usb_configuration_request =
      SET_CONFIGURATION_REQ_DEFAULT;
  set_usb_configuration_request.value_lsb =
      ((configuration_descriptor_t *)(device->control_pipe.rx_buffer))
          ->configuration_value;
  update_packet_crc16(&set_usb_configuration_request);
  res = control_out_protocol(device, (uint8_t *)&set_usb_configuration_request,
                             sizeof(set_usb_configuration_request), NULL, 0);

  if (res != 0) {
    return res;
  }
  volatile uint8_t ep_id_idx = 0;
  volatile uint8_t interface = 0;
  volatile uint8_t class_ = 0;
  uint8_t *descriptor = configuration_descrptor_data;
  while (configuration_descrptor_length > 0) {
    switch (descriptor[1]) {
      case DESC_TYPE_INTERFACE: {
        const interface_descriptor_t *d =
            (const interface_descriptor_t *)descriptor;
        Serial.printf(
            "inum:%d, altsetting:%d, numep:%d, iclass:%d, isubclass:%d, "
            "iprotcol:%d, iface:%d\n",
            d->inum, d->altsetting, d->numep, d->iclass, d->isubclass,
            d->iprotocol, d->iface);
        interface = d->inum;
        class_ = d->iclass;
      } break;
      case DESC_TYPE_ENDPOINT: {
        const endpoint_descriptor_t *d =
            (const endpoint_descriptor_t *)descriptor;
        Serial.printf("\t\t\tepaddr:0x%02x, attr:%d, size:%d, interval:%d\n",
               d->epaddr, d->attr, d->max_size[0] | (d->max_size[1] << 8),
               d->interval);

        if ((class_ == CLASS_HID || class_ == CLASS_HUB) &&
            d->attr == EP_ATTR_INTERRUPT) {
          volatile endpoint_t *ep = NULL;
          for (int ep_pool_idx = 0; ep_pool_idx < PIO_USB_EP_POOL_CNT;
               ep_pool_idx++) {
            if (ep_pool[ep_pool_idx].ep_num == 0) {
              ep = &ep_pool[ep_pool_idx];
              device->endpoint_id[ep_id_idx] = ep_pool_idx + 1;
              ep_id_idx++;
              break;
            }
          }

          if (ep != NULL) {
            for (int bit_idx = 0; bit_idx < 6; bit_idx++) {
              if ((1 << bit_idx) <= d->interval) {
                ep->interval = (1 << bit_idx);
              }
            }
            ep->interval_counter = 0;
            ep->size = d->max_size[0] | (d->max_size[1] << 8);
            ep->attr = d->attr | EP_ATTR_ENUMERATING;
            ep->ep_num = d->epaddr;
          } else {
            Serial.printf("No empty EP\n");
          }
        }
      } break;
      case DESC_TYPE_HID: {
        const hid_descriptor_t *d = (const hid_descriptor_t *)descriptor;
        Serial.printf(
            "\tbcdHID:%x.%x, country:%d, desc num:%d, desc_type:%d, "
            "desc_size:%d\n",
            d->bcd_hid[1], d->bcd_hid[0], d->contry_code, d->num_desc,
            d->desc_type, d->desc_len[0] | (d->desc_len[1] << 8));

        usb_setup_packet_t set_hid_idle_request = SET_HID_IDLE_REQ_DEFAULT;
        set_hid_idle_request.value_lsb = interface;
        set_hid_idle_request.value_msb = 0;
        update_packet_crc16(&set_hid_idle_request);
        control_out_protocol(device, (uint8_t *)&set_hid_idle_request,
                             sizeof(set_hid_idle_request), NULL, 0);

        usb_setup_packet_t get_hid_report_descrpitor_request =
            GET_HID_REPORT_DESCRIPTOR_DEFAULT;
        get_hid_report_descrpitor_request.index_lsb = interface;
        get_hid_report_descrpitor_request.index_msb = 0;
        get_hid_report_descrpitor_request.length_lsb = d->desc_len[0];
        get_hid_report_descrpitor_request.length_msb = d->desc_len[1];
        uint16_t desc_len = d->desc_len[0] | (d->desc_len[1] << 8);
        update_packet_crc16(&get_hid_report_descrpitor_request);
        control_in_protocol(
            device, (uint8_t *)&get_hid_report_descrpitor_request,
            sizeof(get_hid_report_descrpitor_request), rx_buffer, desc_len);
        Serial.printf("\t\tReport descriptor:");
        for (int i = 0; i < desc_len; i++) {
          Serial.printf("%02x ", device->control_pipe.rx_buffer[i]);
        }
        Serial.printf("\n");
        Serial.flush();
      } break;
      default:
        break;
    }

    configuration_descrptor_length -= descriptor[0];
    descriptor += descriptor[0];
  }

  for (int epidx = 0; epidx < PIO_USB_DEV_EP_CNT; epidx++) {
    endpoint_t *ep = pio_usb_get_endpoint(device, epidx);
    if (ep == NULL) {
      break;
    }
    ep->attr &= ~EP_ATTR_ENUMERATING;
  }

  return res;
}

static void device_disconnect(usb_device_t *device) {
  Serial.printf("Disconnect device %d\n", device->address);
  for (int port = 0; port < PIO_USB_HUB_PORT_CNT; port++) {
    if (device->child_devices[port] != 0) {
      device_disconnect(&usb_device[device->child_devices[port]]);
    }
  }

  for (int ep_idx = 0; ep_idx < PIO_USB_DEV_EP_CNT; ep_idx++) {
    endpoint_t *ep = pio_usb_get_endpoint(device, ep_idx);
    if (ep == NULL) {
      break;
    }
    memset(ep, 0, sizeof(*ep));
  }

  if (device->address == 0 && device->root != NULL) {
    device->root->addr0_exists = false;
  }

  memset(device, 0, sizeof(*device));
}

static int device_pool_vacant(void) {
  for (int idx = 0; idx < PIO_USB_DEVICE_CNT; idx++) {
    if (!usb_device[idx].connected) {
      return idx;
    }
  }

  return -1;
}

static int assign_new_device_to_port(usb_device_t *hub_device, uint8_t port, bool is_ls) {
  int idx = device_pool_vacant();
  if (idx >= 0) {
    hub_device->child_devices[port] = idx;
    usb_device[idx].parent_device = hub_device;
    usb_device[idx].parent_port = port;
    usb_device[idx].root = hub_device->root;
    usb_device[idx].connected = true;
    usb_device[idx].is_fullspeed = !is_ls;
    usb_device[idx].event = EVENT_CONNECT;
    Serial.printf("Assign device %d to %d-%d\n", idx, hub_device->address, port);
    return 0;
  }

  Serial.printf("Failed to assign device\n");

  return -1;
}

static void configure_tx_channel(uint8_t ch, PIO pio, uint sm) {
  dma_channel_config conf = dma_channel_get_default_config(ch);

  channel_config_set_read_increment(&conf, true);
  channel_config_set_write_increment(&conf, false);
  channel_config_set_transfer_data_size(&conf, DMA_SIZE_8);
  channel_config_set_dreq(&conf, pio_get_dreq(pio, sm, true));

  dma_channel_set_config(ch, &conf, false);
  dma_channel_set_write_addr(ch, &pio->txf[sm], false);
}

static void apply_config(pio_port_t *pp, const pio_usb_configuration_t *c,
                         root_port_t *port) {
  pp->pio_usb_tx = c->pio_tx_num == 0 ? pio0 : pio1;
  pp->sm_tx = c->sm_tx;
  pp->tx_ch = c->tx_ch;
  pp->pio_usb_rx = c->pio_rx_num == 0 ? pio0 : pio1;
  pp->sm_rx = c->sm_rx;
  pp->sm_eop = c->sm_eop;
  port->pin_dp = c->pin_dp;
  port->pin_dm = c->pin_dp + 1;
}

static repeating_timer_t sof_rt;
static bool timer_active;

static void start_timer(alarm_pool_t *alarm_pool) {
  if (timer_active) {
    return;
  }

  if (alarm_pool != NULL) {
    alarm_pool_add_repeating_timer_us(alarm_pool, -1000, sof_timer, NULL,
                                      &sof_rt);
  } else {
    add_repeating_timer_us(-1000, sof_timer, NULL, &sof_rt);
  }

  timer_active = true;
}

static void stop_timer(void) {
  cancel_repeating_timer(&sof_rt);
  timer_active = false;
}

static void port_pin_drive_setting(const root_port_t *port) {
  gpio_set_slew_rate(port->pin_dp, GPIO_SLEW_RATE_FAST);
  gpio_set_slew_rate(port->pin_dm, GPIO_SLEW_RATE_FAST);
  gpio_set_drive_strength(port->pin_dp, GPIO_DRIVE_STRENGTH_12MA);
  gpio_set_drive_strength(port->pin_dm, GPIO_DRIVE_STRENGTH_12MA);
}

endpoint_t *pio_usb_get_endpoint(usb_device_t *device, uint8_t idx) {
  uint8_t ep_id = device->endpoint_id[idx];
  if (ep_id == 0) {
    return NULL;
  } else if (ep_id >= 1) {
    return &ep_pool[ep_id - 1];
  }
  return NULL;
}

usb_device_t *pio_usb_host_init(const pio_usb_configuration_t *c) {
  pio_port_t *pp = &pio_port[0];
  pp->pio_usb_tx = c->pio_tx_num == 0 ? pio0 : pio1;
  configure_tx_channel(c->tx_ch, pp->pio_usb_tx, c->sm_tx);

  apply_config(pp, c, &root_port[0]);
  initialize_host_programs(pp, c, &root_port[0]);
  port_pin_drive_setting(&root_port[0]);
  root_port[0].initialized = true;

  pio_calculate_clkdiv_from_float((float)clock_get_hz(clk_sys) / 48000000,
                                  &pp->clk_div_fs_tx.div_int,
                                  &pp->clk_div_fs_tx.div_frac);
  pio_calculate_clkdiv_from_float((float)clock_get_hz(clk_sys) / 6000000,
                                  &pp->clk_div_ls_tx.div_int,
                                  &pp->clk_div_ls_tx.div_frac);

  pio_calculate_clkdiv_from_float((float)clock_get_hz(clk_sys) / 96000000,
                                  &pp->clk_div_fs_rx.div_int,
                                  &pp->clk_div_fs_rx.div_frac);
  pio_calculate_clkdiv_from_float((float)clock_get_hz(clk_sys) / 12000000,
                                  &pp->clk_div_ls_rx.div_int,
                                  &pp->clk_div_ls_rx.div_frac);

  start_timer(reinterpret_cast<alarm_pool_t*>(c->alarm_pool));

  current_config = *c;

  return &usb_device[0];
}

int pio_usb_host_add_port(uint8_t pin_dp) {
  for (int idx = 0; idx < PIO_USB_ROOT_PORT_CNT; idx++) {
    if (!root_port[idx].initialized) {
      root_port[idx].pin_dp = pin_dp;
      root_port[idx].pin_dm = pin_dp + 1;
      gpio_pull_down(pin_dp);
      gpio_pull_down(pin_dp + 1);
      pio_gpio_init(pio_port[0].pio_usb_tx, pin_dp);
      pio_gpio_init(pio_port[0].pio_usb_tx, pin_dp + 1);
      pio_sm_set_pindirs_with_mask(pio_port[0].pio_usb_tx, pio_port[0].sm_tx, 0,
                                   (0b11 << pin_dp));
      port_pin_drive_setting(&root_port[idx]);
      root_port[idx].initialized = true;

      return 0;
    }
  }

  return -1;
}

static volatile bool cancel_timer_flag;
static volatile bool start_timer_flag;
static uint32_t int_stat;

void pio_usb_host_stop(void) {
  cancel_timer_flag = true;
  while (cancel_timer_flag) {
    continue;
  }
}

void pio_usb_host_restart(void) {
  start_timer_flag = true;
  while (start_timer_flag) {
    continue;
  }
}

static void __no_inline_not_in_flash_func(process_hub_event)(
    usb_device_t *device) {
  volatile endpoint_t *ep = pio_usb_get_endpoint(device, 0);
  uint8_t bitmap = ep->buffer[0];
  for (int bit = 1; bit < 8; bit++) {
    if (!(bitmap & (1 << bit))) {
      continue;
    }
    uint8_t port = bit - 1;
    hub_port_status_t status;
    int res = get_hub_port_status(device, port, &status);
    if (res != 0) {
      Serial.printf("Failed to get port%d-%d status\n", device->address, port);
      continue;
    }
    Serial.printf("port%d-%d status:%d %d\n", device->address, port,
           status.port_change, status.port_status);

    if (status.port_change & HUB_CHANGE_PORT_CONNECTION) {
      if (status.port_status & HUB_STAT_PORT_CONNECTION) {
        Serial.printf("new device on port %d, reset port\n", port);
        if (device->child_devices[port] != 0) {
          Serial.printf("device is already assigned. disconnect previous\n");
          device_disconnect(&usb_device[device->child_devices[port]]);
        }

        if (device->root->addr0_exists) {
          Serial.printf("Address 0 already exists\n");
          continue;
        }

        if (device_pool_vacant() >= 0) {
          set_hub_feature(device, port, HUB_SET_PORT_RESET);
          device->root->addr0_exists = true;
        } else {
          Serial.printf("No vacant in device pool\n");
        }
      } else {
        Serial.printf("device removed from port %d\n", port);
        if (device->child_devices[port] != 0) {
          device_disconnect(&usb_device[device->child_devices[port]]);
        }
      }
      clear_hub_feature(device, port, HUB_CLR_PORT_CONNECTION);
    } else if (status.port_change & HUB_CHANGE_PORT_RESET) {
      Serial.printf("reset port %d complete\n", port);
      int res = clear_hub_feature(device, port, HUB_CLR_PORT_RESET);
      if (res == 0) {
        assign_new_device_to_port(device, port,
                                  status.port_status & HUB_STAT_PORT_LOWSPEED);
      }
    } else if (status.port_change & HUB_CHANGE_PORT_ENABLE) {
      clear_hub_feature(device, port, HUB_CLR_PORT_ENABLE);
    }
  }

  device->event = EVENT_NONE;
}

void __no_inline_not_in_flash_func(pio_usb_host_task)(void) {
  for (int root_idx = 0; root_idx < PIO_USB_ROOT_PORT_CNT; root_idx++) {
    if (root_port[root_idx].event == EVENT_CONNECT) {
      Serial.printf("Root %d connected\n", root_idx);
      int dev_idx = device_pool_vacant();
      if (dev_idx >= 0) {
        on_device_connect(&pio_port[0], &root_port[root_idx], dev_idx);
        root_port[root_idx].addr0_exists = true;
      }
      root_port[root_idx].event = EVENT_NONE;
    } else if (root_port[root_idx].event == EVENT_DISCONNECT) {
      Serial.printf("Root %d disconnected\n", root_idx);
      root_port[root_idx].root_device->connected = false;
      root_port[root_idx].root_device->event = EVENT_DISCONNECT;
      root_port[root_idx].root_device = NULL;
      root_port[root_idx].event = EVENT_NONE;
    }
  }

  for (int idx = 0; idx < PIO_USB_DEVICE_CNT; idx++) {
    usb_device_t *device = &usb_device[idx];

    if (device->event == EVENT_CONNECT) {
      device->event = EVENT_NONE;
      Serial.printf("Device %d Connected\n", idx);
      int res = enumerate_device(device, idx + 1);
      if (res == 0) {
        device->enumerated = true;
        device->root->addr0_exists = false;
        if (device->device_class == CLASS_HUB) {
          res = initialize_hub(device);
        }
      }

      if (res != 0) {
        Serial.printf("Enumeration failed(%d)\n", res);
        // retry
        if (device->is_root) {
          device->root->event = EVENT_DISCONNECT;
        } else {
          set_hub_feature(device->parent_device, device->parent_port,
                          HUB_SET_PORT_RESET);
          device_disconnect(device);
        }
      }
    } else if (device->event == EVENT_DISCONNECT) {
      device->event = EVENT_NONE;
      Serial.printf("Disconnect\n");
      device_disconnect(device);
    } else if (device->event == EVENT_HUB_PORT_CHANGE) {
      process_hub_event(device);
    }
  }

  if (cancel_timer_flag) {
    int_stat = save_and_disable_interrupts();
    stop_timer();
    if (root_port->root_device != NULL) {
      device_disconnect(root_port->root_device);
    }
    cancel_timer_flag = false;
  }

  if (start_timer_flag) {
    start_timer(reinterpret_cast<alarm_pool_t*>(current_config.alarm_pool));
    restore_interrupts(int_stat);
    start_timer_flag = false;
  }
}

//
// Device implementation
//

static endpoint_t* active_ep = NULL;
static usb_descriptor_buffers_t descriptor_buffers;
static int8_t new_devaddr = -1;
static uint8_t ep0_crc5_lut[16];

static int8_t ep0_desc_request_type = -1;
static uint16_t ep0_desc_request_len;
static uint8_t ep0_desc_request_idx;

static void __no_inline_not_in_flash_func(update_ep0_crc5_lut)(uint8_t addr) {
  uint16_t dat;
  uint8_t crc;
  for (int epnum = 0; epnum < 16; epnum++) {
    dat = (addr) | (epnum << 7);
    crc = calc_usb_crc5(dat);
    ep0_crc5_lut[epnum] = (crc << 3) | ((epnum >> 1) & 0x07);
  }
}

static __always_inline void restart_usb_reveiver(pio_port_t *pp) {
  pio_sm_exec(pp->pio_usb_rx, pp->sm_rx, pp->rx_reset_instr);
  pio_sm_restart(pp->pio_usb_rx, pp->sm_rx);
  pio_sm_restart(pp->pio_usb_rx, pp->sm_eop);
  pp->pio_usb_rx->irq = IRQ_RX_ALL_MASK;
}

static __always_inline endpoint_t *device_receive_token(uint8_t *buffer,
                                                        uint8_t dev_addr) {
  pio_port_t *pp = &pio_port[0];
  uint8_t idx = 0;
  int8_t addr = -1;
  int8_t ep = -1;
  bool match = false;

  static uint8_t eplut[2][8] = {{0, 2, 4, 6, 8, 10, 12, 14},
                                {1, 3, 5, 7, 9, 11, 13, 15}};
  uint8_t *current_lut;

  if ((pp->pio_usb_rx->irq & IRQ_RX_COMP_MASK) == 0) {
    while ((pp->pio_usb_rx->irq & IRQ_RX_COMP_MASK) == 0) {
      if (pio_sm_get_rx_fifo_level(pp->pio_usb_rx, pp->sm_rx)) {
        buffer[idx++] = pio_sm_get(pp->pio_usb_rx, pp->sm_rx) >> 24;
        if (idx == 3) {
          addr = buffer[2] & 0x7f;
          current_lut = &eplut[buffer[2] >> 7][0];
          match = dev_addr == addr ? true : false;
        }
      }
    }
  } else {
    // host is probaly timeout. Ignore this packets.
    pio_sm_clear_fifos(pp->pio_usb_rx, pp->sm_rx);
  }

  restart_usb_reveiver(pp);

  if (match) {
    ep = current_lut[buffer[3] & 0x07];
    if (ep0_crc5_lut[ep] == buffer[3]) {
      return &ep_pool[ep];
    } else {
      return NULL;
    }
  }

  return NULL;
}

static void __no_inline_not_in_flash_func(prepare_ep0_data)(uint8_t *data,
                                                              uint8_t len) {
  usb_device_t *dev = &usb_device[0];
  control_pipe_t *p = &dev->control_pipe;

  p->request_length = len;
  p->rx_buffer = data;
  p->buffer_idx = 0;

  ep_pool[0].data_id = 1;
  ep_pool[0].new_data_flag = false;
  uint8_t packet_len =
      p->request_length > PIO_USB_EP_SIZE ? PIO_USB_EP_SIZE : p->request_length;
  pio_usb_set_out_data(&ep_pool[0], (uint8_t *)&p->rx_buffer[p->buffer_idx],
                       packet_len);
}

void pio_usb_device_task(void) {
  switch (ep0_desc_request_type) {
    case DESC_TYPE_CONFIG: {
      uint16_t req_len = ep0_desc_request_len;
      uint16_t desc_len =
          descriptor_buffers.config[2] | (descriptor_buffers.config[3] << 8);
      req_len = req_len > desc_len ? desc_len : req_len;
      prepare_ep0_data((uint8_t *)descriptor_buffers.config, req_len);
      ep0_desc_request_type = -1;
    } break;
    case DESC_TYPE_STRING: {
      const uint16_t *str =
          (uint16_t *)&descriptor_buffers.string[ep0_desc_request_idx];
      prepare_ep0_data((uint8_t *)str, str[0] & 0xff);
      ep0_desc_request_type = -1;
    } break;
    case DESC_TYPE_HID_REPORT:{
      prepare_ep0_data(
          (uint8_t *)descriptor_buffers.hid_report[ep0_desc_request_idx],
          ep0_desc_request_len);
      ep0_desc_request_type = -1;
    }
    default:
      break;
  }
}

static int __no_inline_not_in_flash_func(process_device_setup_stage)(
    uint8_t *buffer) {
  int res = -1;
  const usb_setup_packet_t *packet = (usb_setup_packet_t *)buffer;

  if (packet->request_type == USB_REQ_DIR_IN) {
    if (packet->request == 0x06) {
      if (packet->value_msb == DESC_TYPE_DEVICE) {
        prepare_ep0_data((uint8_t *)descriptor_buffers.device, 18);
        res = 0;
      } else if (packet->value_msb == DESC_TYPE_CONFIG) {
        ep0_desc_request_len = (packet->length_lsb | (packet->length_msb << 8));
        ep0_desc_request_type = DESC_TYPE_CONFIG;
        res = 0;
      } else if (packet->value_msb == DESC_TYPE_STRING) {
        if (descriptor_buffers.string != NULL) {
          ep0_desc_request_idx = packet->value_lsb;
          ep0_desc_request_type = DESC_TYPE_STRING;
          res = 0;
        }
      }
    }
  } else if (packet->request_type == USB_REQ_DIR_OUT) {
    if (packet->request == 0x05) {
      // set address
      new_devaddr = packet->value_lsb;
      prepare_ep0_data(NULL, 0);
      res = 0;
    } else if (packet->request == 0x09) {
      // set configuration
      prepare_ep0_data(NULL, 0);
      res = 0;
    }
  } else if (packet->request_type == (USB_REQ_DIR_IN | USB_REQ_REC_IFACE)) {
    if (packet->request == 0x06 && packet->value_msb == DESC_TYPE_HID_REPORT) {
      // get hid report desc
      ep0_desc_request_len = (packet->length_lsb | (packet->length_msb << 8));
      ep0_desc_request_idx = packet->index_lsb;
      ep0_desc_request_type = DESC_TYPE_HID_REPORT;
      res = 0;
    }
  } else if (packet->request_type == (USB_REQ_TYP_CLASS | USB_REQ_REC_IFACE)) {
    if (packet->request == 0x09) {
      // set hid report
      prepare_ep0_data(NULL, 0);
      res = 0;
    } else if (packet->request == 0x0A) {
      // set hid idle request
      prepare_ep0_data(NULL, 0);
      res = 0;
    } else if (packet->request == 0x0B) {
      // set hid protocol request
      prepare_ep0_data(NULL, 0);
      res = 0;
    }
  }

  return res;
}

static void __no_inline_not_in_flash_func(usb_device_packet_handler)(void) {
  static uint8_t token_buf[64];
  static bool wait_ack = false;
  static uint8_t hand_shake_token[] = {USB_SYNC, USB_PID_NAK};
  usb_device_t *dev = &usb_device[0];
  pio_port_t *pp = &pio_port[0];

  //
  // time critical start
  //
  endpoint_t *ep = device_receive_token(token_buf, dev->address);

  if (token_buf[1] == USB_PID_IN) {
    if (ep == NULL) {
      return;
    }

    pio_sm_set_enabled(pp->pio_usb_rx, pp->sm_rx, false);
    if (ep->new_data_flag) {
      dma_channel_transfer_from_buffer_now(pp->tx_ch, ep->buffer,
                                           ep->packet_len);
    } else {
      dma_channel_transfer_from_buffer_now(pp->tx_ch, hand_shake_token, 2);
    }
    pp->pio_usb_tx->irq |= IRQ_TX_ALL_MASK;  // clear complete flag
    active_ep = ep;
    while ((pp->pio_usb_tx->irq & IRQ_TX_COMP_MASK) == 0) {
      continue;
    }
    pp->pio_usb_rx->irq = IRQ_RX_ALL_MASK;
    irq_clear(pp->device_rx_irq_num);

    start_receive(pp);

    //
    // time critical end
    //

    // if control pipe
    if (active_ep == &ep_pool[0]) {
      if (dev->control_pipe.stage == STAGE_STATUS) {
        dev->control_pipe.stage = STAGE_COMPLETE;
      }

      if (new_devaddr > 0) {
        dev->address = new_devaddr;
        new_devaddr = -1;
        update_ep0_crc5_lut(dev->address);
      }
    }

    wait_ack = true;

  } else if (token_buf[1] == USB_PID_OUT) {
    wait_ack = false;
    if (ep == NULL) {
      return;
    }
    active_ep = ep;
    int res = receive_packet_and_ack(pp);
    pio_sm_clear_fifos(pp->pio_usb_rx, pp->sm_rx);
    restart_usb_reveiver(pp);
    irq_clear(pp->device_rx_irq_num);

    // if control pipe
    if (active_ep == &ep_pool[0]) {
      if (dev->control_pipe.stage == STAGE_STATUS) {
        dev->control_pipe.stage = STAGE_COMPLETE;
      } else if (dev->control_pipe.stage == STAGE_DATA) {
        dev->control_pipe.stage = STAGE_STATUS;
        prepare_ep0_data(NULL, 0);
      }
    } else if (res >= 0) {
      memcpy((void*)ep->buffer, pp->usb_rx_buffer, ep->size);
      ep->new_data_flag = true;
    }

  } else if (token_buf[1] == USB_PID_SETUP) {
    wait_ack = false;
    if (ep == NULL) {
      return;
    }
    active_ep = ep;
    int res = receive_packet_and_ack(pp);
    pio_sm_clear_fifos(pp->pio_usb_rx, pp->sm_rx);
    restart_usb_reveiver(pp);
    irq_clear(pp->device_rx_irq_num);

    if (res >= 0) {
      res = process_device_setup_stage(pp->usb_rx_buffer);
      dev->control_pipe.stage = STAGE_DATA;
    }

    if (res == 0) {
      hand_shake_token[1] = USB_PID_NAK;
    } else {
      hand_shake_token[1] = USB_PID_STALL;
    }

  } else if (token_buf[1] == USB_PID_ACK && wait_ack) {
    wait_ack = false;
    active_ep->new_data_flag = false;
    active_ep->data_id ^= 1;

    // if control pipe
    if (active_ep == &ep_pool[0] && dev->control_pipe.stage == STAGE_DATA) {
      control_pipe_t *p = &dev->control_pipe;
      p->buffer_idx += ep_pool[0].packet_len - 4;
      p->request_length -= ep_pool[0].packet_len - 4;
      if (p->request_length > 0) {
        uint8_t len = p->request_length > 64 ? 64 : p->request_length;
        pio_usb_set_out_data(&ep_pool[0],
                             (uint8_t *)&p->rx_buffer[p->buffer_idx], len);
      } else {
        dev->control_pipe.stage = STAGE_STATUS;
      }
    }
  } else {
    wait_ack = false;
  }

  if (token_buf[0] == 0 && get_port_pin_status(&root_port[0]) == PORT_PIN_SE0) {
    // Reset detected
    memset(dev, 0, sizeof(*dev));
    memset(ep_pool, 0, sizeof(ep_pool));
    for (int i = 0; i < PIO_USB_DEV_EP_CNT; i++) {
      usb_device[0].endpoint_id[i] = i + 1;
    }
    update_ep0_crc5_lut(dev->address);
  }

  token_buf[0] = 0; // clear received token
  token_buf[1] = 0;
}

usb_device_t *pio_usb_device_init(const pio_usb_configuration_t *c,
                                  const usb_descriptor_buffers_t *buffers) {
  pio_port_t *pp = &pio_port[0];
  usb_device_t *dev = &usb_device[0];
  pp->pio_usb_tx = c->pio_tx_num == 0 ? pio0 : pio1;
  configure_tx_channel(c->tx_ch, pp->pio_usb_tx, c->sm_tx);

  apply_config(pp, c, &root_port[0]);
  initialize_host_programs(pp, c, &root_port[0]);
  port_pin_drive_setting(&root_port[0]);
  root_port[0].initialized = true;
  memset(dev, 0, sizeof(*dev));
  for (int i = 0; i < PIO_USB_DEV_EP_CNT; i++) {
    dev->endpoint_id[i] = i + 1;
  }
  update_ep0_crc5_lut(dev->address);

  pio_calculate_clkdiv_from_float((float)clock_get_hz(clk_sys) / 48000000,
                                  &pp->clk_div_fs_tx.div_int,
                                  &pp->clk_div_fs_tx.div_frac);
  pio_calculate_clkdiv_from_float((float)clock_get_hz(clk_sys) / 96000000,
                                  &pp->clk_div_fs_rx.div_int,
                                  &pp->clk_div_fs_rx.div_frac);

  current_config = *c;
  descriptor_buffers = *buffers;

  pio_sm_set_enabled(pp->pio_usb_tx, pp->sm_tx, true);
  prepare_receive(pp);
  pp->pio_usb_rx->ctrl |= (1 << pp->sm_rx);
  pp->pio_usb_rx->irq |= IRQ_RX_ALL_MASK;

  // configure PIOx_IRQ_0 to detect packet receive start
  pio_set_irqn_source_enabled(pp->pio_usb_rx, 0, static_cast<pio_interrupt_source>(pis_interrupt0 + IRQ_RX_START),
                              true);
  pp->device_rx_irq_num = (pp->pio_usb_rx == pio0) ? PIO0_IRQ_0 : PIO1_IRQ_0;
  irq_set_exclusive_handler(pp->device_rx_irq_num, usb_device_packet_handler);
  irq_set_enabled(pp->device_rx_irq_num, true);

  return dev;
}

#pragma GCC pop_options
