/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2024 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file is part of the TinyUSB stack.
 */

#include "tusb_option.h"

#if CFG_TUH_ENABLED && defined(TUP_USBIP_DWC2)

// Debug level for DWC2
#define DWC2_DEBUG    2

#include "host/hcd.h"
#include "dwc2_common.h"

// DWC2 has limit number of channel, in order to support all endpoints we can store channel char/split to swap later on
#ifndef CFG_TUH_DWC2_CHANNEL_MAX
#define CFG_TUH_DWC2_CHANNEL_MAX (CFG_TUH_DEVICE_MAX*CFG_TUH_ENDPOINT_MAX + CFG_TUH_HUB)
#endif

enum {
  HPRT_W1C_MASK = HPRT_CONN_DETECT | HPRT_ENABLE | HPRT_ENABLE_CHANGE | HPRT_OVER_CURRENT_CHANGE
};

typedef struct {
  union {
    uint32_t hcchar;
    dwc2_channel_char_t hcchar_bm;
  };
  union {
    uint32_t hcsplt;
    dwc2_channel_split_t hcsplt_bm;
  };

  uint8_t next_data_toggle;
} hcd_pipe_t;

typedef struct {
  hcd_pipe_t pipe[CFG_TUH_DWC2_CHANNEL_MAX];
} dwc2_hcd_t;

dwc2_hcd_t _hcd_data;

//--------------------------------------------------------------------
//
//--------------------------------------------------------------------
TU_ATTR_ALWAYS_INLINE static inline tusb_speed_t convert_hprt_speed(uint32_t hprt_speed) {
  tusb_speed_t speed;
  switch(hprt_speed) {
    case HPRT_SPEED_HIGH: speed = TUSB_SPEED_HIGH; break;
    case HPRT_SPEED_FULL: speed = TUSB_SPEED_FULL; break;
    case HPRT_SPEED_LOW : speed = TUSB_SPEED_LOW ; break;
    default: TU_BREAKPOINT(); break;
  }
  return speed;
}

TU_ATTR_ALWAYS_INLINE static inline bool dma_host_enabled(const dwc2_regs_t* dwc2) {
  (void) dwc2;
  // Internal DMA only
  return CFG_TUH_DWC2_DMA && dwc2->ghwcfg2_bm.arch == GHWCFG2_ARCH_INTERNAL_DMA;
}

/* USB Data FIFO Layout

  The FIFO is split up into
  - EPInfo: for storing DMA metadata (check dcd_dwc2.c for more details)
  - 1 RX FIFO: for receiving data
  - 1 TX FIFO for non-periodic (NPTX)
  - 1 TX FIFO for periodic (PTX)

  We allocated TX FIFO from top to bottom (using top pointer), this to allow the RX FIFO to grow dynamically which is
  possible since the free space is located between the RX and TX FIFOs.

   ----------------- ep_fifo_size
  | EPInfo DMA   |
  |--------------|-- gdfifocfg.EPINFOBASE (max is ghwcfg3.dfifo_depth)
  | Non-Periodic |
  |   TX FIFO    |
  |--------------|--- GNPTXFSIZ.addr (fixed size)
  |   Periodic   |
  |   TX FIFO    |
  |--------------|--- HPTXFSIZ.addr (expandable downward)
  |    FREE      |
  |              |
  |--------------|-- GRXFSIZ (expandable upward)
  |  RX FIFO     |
  ---------------- 0
*/

/* Programming Guide 2.1.2 FIFO RAM allocation
 * RX
 * - Largest-EPsize/4 + 2 (status info). recommended x2 if high bandwidth or multiple ISO are used.
 * - 2 for transfer complete and channel halted status
 * - 1 for each Control/Bulk out endpoint to Handle NAK/NYET (i.e max is number of host channel)
 *
 * TX non-periodic (NPTX)
 * - At least largest-EPsize/4, recommended x2
 *
 * TX periodic (PTX)
 * - At least largest-EPsize*MulCount/4 (MulCount up to 3 for high-bandwidth ISO/interrupt)
*/
static void dfifo_host_init(uint8_t rhport) {
  const dwc2_controller_t* dwc2_controller = &_dwc2_controller[rhport];
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);

  // Scatter/Gather DMA mode is not yet supported. Buffer DMA only need 1 words per channel
  const bool is_dma = dma_host_enabled(dwc2);
  uint16_t dfifo_top = dwc2_controller->ep_fifo_size/4;
  if (is_dma) {
    dfifo_top -= dwc2->ghwcfg2_bm.num_host_ch;
  }

  // fixed allocation for now, improve later:
    // - ptx_largest is limited to 256 for FS since most FS core only has 1024 bytes total
  bool is_highspeed = dwc2_core_is_highspeed(dwc2, TUSB_ROLE_HOST);
  uint32_t nptx_largest = is_highspeed ? TUSB_EPSIZE_BULK_HS/4 : TUSB_EPSIZE_BULK_FS/4;
  uint32_t ptx_largest = is_highspeed ? TUSB_EPSIZE_ISO_HS_MAX/4 : 256/4;

  uint16_t nptxfsiz = 2 * nptx_largest;
  uint16_t rxfsiz = 2 * (ptx_largest + 2) + dwc2->ghwcfg2_bm.num_host_ch;
  TU_ASSERT(dfifo_top >= (nptxfsiz + rxfsiz),);
  uint16_t ptxfsiz = dfifo_top - (nptxfsiz + rxfsiz);

  dwc2->gdfifocfg = (dfifo_top << GDFIFOCFG_EPINFOBASE_SHIFT) | dfifo_top;

  dfifo_top -= rxfsiz;
  dwc2->grxfsiz = rxfsiz;

  dfifo_top -= nptxfsiz;
  dwc2->gnptxfsiz = tu_u32_from_u16(nptxfsiz, dfifo_top);

  dfifo_top -= ptxfsiz;
  dwc2->hptxfsiz = tu_u32_from_u16(ptxfsiz, dfifo_top);
}

//--------------------------------------------------------------------+
// Controller API
//--------------------------------------------------------------------+

// optional hcd configuration, called by tuh_configure()
bool hcd_configure(uint8_t rhport, uint32_t cfg_id, const void* cfg_param) {
  (void) rhport;
  (void) cfg_id;
  (void) cfg_param;

  return false;
}

// Initialize controller to host mode
bool hcd_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {
  (void) rh_init;
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);

  tu_memclr(&_hcd_data, sizeof(_hcd_data));

  // Core Initialization
  const bool is_highspeed = dwc2_core_is_highspeed(dwc2, TUSB_ROLE_HOST);
  const bool is_dma = dma_host_enabled(dwc2);
  TU_ASSERT(dwc2_core_init(rhport, is_highspeed, is_dma));

  //------------- 3.1 Host Initialization -------------//

  // FS/LS PHY Clock Select
  uint32_t hcfg = dwc2->hcfg;
  if (is_highspeed) {
    hcfg &= ~HCFG_FSLS_ONLY;
  } else {
    hcfg &= ~HCFG_FSLS_ONLY; // since we are using FS PHY
    hcfg &= ~HCFG_FSLS_PHYCLK_SEL;

    if (dwc2->ghwcfg2_bm.hs_phy_type == GHWCFG2_HSPHY_ULPI &&
        dwc2->ghwcfg2_bm.fs_phy_type == GHWCFG2_FSPHY_DEDICATED) {
      // dedicated FS PHY with 48 mhz
      hcfg |= HCFG_FSLS_PHYCLK_SEL_48MHZ;
    } else {
      // shared HS PHY running at full speed
      hcfg |= HCFG_FSLS_PHYCLK_SEL_30_60MHZ;
    }
  }
  dwc2->hcfg = hcfg;

  // Enable HFIR reload

  // force host mode and wait for mode switch
  dwc2->gusbcfg = (dwc2->gusbcfg & ~GUSBCFG_FDMOD) | GUSBCFG_FHMOD;
  while( (dwc2->gintsts & GINTSTS_CMOD) != GINTSTS_CMODE_HOST) {}

  // configure fixed-allocated fifo scheme
  dfifo_host_init(rhport);

  dwc2->hprt = HPRT_W1C_MASK; // clear all write-1-clear bits
  dwc2->hprt = HPRT_POWER; // turn on VBUS

  // Enable required interrupts
  dwc2->gintmsk |= GINTMSK_OTGINT | GINTSTS_CONIDSTSCHNG | GINTMSK_PRTIM; // | GINTMSK_WUIM;
  dwc2->gahbcfg |= GAHBCFG_GINT;   // Enable global interrupt

  return true;
}

// Enable USB interrupt
void hcd_int_enable (uint8_t rhport) {
  dwc2_int_set(rhport, TUSB_ROLE_HOST, true);
}

// Disable USB interrupt
void hcd_int_disable(uint8_t rhport) {
  dwc2_int_set(rhport, TUSB_ROLE_HOST, false);
}

// Get frame number (1ms)
uint32_t hcd_frame_number(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  return dwc2->hfnum & HFNUM_FRNUM_Msk;
}

//--------------------------------------------------------------------+
// Port API
//--------------------------------------------------------------------+

// Get the current connect status of roothub port
bool hcd_port_connect_status(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  return dwc2->hprt & HPRT_CONN_STATUS;
}

// Reset USB bus on the port. Return immediately, bus reset sequence may not be complete.
// Some port would require hcd_port_reset_end() to be invoked after 10ms to complete the reset sequence.
void hcd_port_reset(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  uint32_t hprt = dwc2->hprt & ~HPRT_W1C_MASK;
  hprt |= HPRT_RESET;
  dwc2->hprt = hprt;
}

// Complete bus reset sequence, may be required by some controllers
void hcd_port_reset_end(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  uint32_t hprt = dwc2->hprt & ~HPRT_W1C_MASK; // skip w1c bits
  hprt &= ~HPRT_RESET;
  dwc2->hprt = hprt;
}

// Get port link speed
tusb_speed_t hcd_port_speed_get(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  const tusb_speed_t speed = convert_hprt_speed(dwc2->hprt_bm.speed);
  return speed;
}

// HCD closes all opened endpoints belong to this device
void hcd_device_close(uint8_t rhport, uint8_t dev_addr) {
  (void) rhport;
  (void) dev_addr;
}

//--------------------------------------------------------------------+
// Endpoints API
//--------------------------------------------------------------------+

// Open an endpoint
// channel0 is reserved for dev0 control endpoint
bool hcd_edpt_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_endpoint_t const * desc_ep) {
  (void) rhport;
  //dwc2_regs_t* dwc2 = DWC2_REG(rhport);

  hcd_devtree_info_t devtree_info;
  hcd_devtree_get_info(dev_addr, &devtree_info);

  // find a free pipe
  for (uint32_t i = 0; i < CFG_TUH_DWC2_CHANNEL_MAX; i++) {
    hcd_pipe_t* pipe = &_hcd_data.pipe[i];
    dwc2_channel_char_t* hcchar_bm = &pipe->hcchar_bm;
    dwc2_channel_split_t* hcsplt_bm = &pipe->hcsplt_bm;

    if (hcchar_bm->enable == 0) {
      hcchar_bm->ep_size = tu_edpt_packet_size(desc_ep);
      hcchar_bm->ep_num = tu_edpt_number(desc_ep->bEndpointAddress);
      hcchar_bm->ep_dir = tu_edpt_dir(desc_ep->bEndpointAddress);
      hcchar_bm->low_speed_dev = (devtree_info.speed == TUSB_SPEED_LOW) ? 1 : 0;
      hcchar_bm->ep_type = desc_ep->bmAttributes.xfer;
      hcchar_bm->err_multi_count = 0;
      hcchar_bm->dev_addr = dev_addr;
      hcchar_bm->odd_frame = 0;
      hcchar_bm->disable = 0;
      hcchar_bm->enable = 1;

      hcsplt_bm->hub_port = devtree_info.hub_port;
      hcsplt_bm->hub_addr = devtree_info.hub_addr;
      // TODO not support split transaction yet
      hcsplt_bm->xact_pos = 0;
      hcsplt_bm->split_compl = 0;
      hcsplt_bm->split_en = 0;

      pipe->next_data_toggle = HCTSIZ_PID_DATA0;

      return true;
    }
  }

  return false;
}

TU_ATTR_ALWAYS_INLINE static inline uint8_t find_free_channel(dwc2_regs_t* dwc2) {
  const uint8_t max_channel = tu_min8(dwc2->ghwcfg2_bm.num_host_ch, 16);
  for (uint8_t i=0; i<max_channel; i++) {
    // haintmsk bit enabled means channel is currently in use
    if (!tu_bit_test(dwc2->haintmsk, i)) {
      return i;
    }
  }
  return TUSB_INDEX_INVALID_8;
}

TU_ATTR_ALWAYS_INLINE static inline uint8_t find_opened_pipe(uint8_t dev_addr, uint8_t ep_addr) {
  for (uint32_t i = 0; i < CFG_TUH_DWC2_CHANNEL_MAX; i++) {
    const dwc2_channel_char_t* hcchar_bm = &_hcd_data.pipe[i].hcchar_bm;
    if (hcchar_bm->enable && hcchar_bm->dev_addr == dev_addr &&
        ep_addr == tu_edpt_addr(hcchar_bm->ep_num, hcchar_bm->ep_dir)) {
      return i;
    }
  }
  return TUSB_INDEX_INVALID_8;
}

// Submit a transfer, when complete hcd_event_xfer_complete() must be invoked
bool hcd_edpt_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr, uint8_t * buffer, uint16_t buflen) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);

  uint8_t pipe_id = find_opened_pipe(dev_addr, ep_addr);
  TU_ASSERT(pipe_id < CFG_TUH_DWC2_CHANNEL_MAX); // no opened pipe
  hcd_pipe_t* pipe = &_hcd_data.pipe[pipe_id];
  const dwc2_channel_char_t* hcchar_bm = &pipe->hcchar_bm;

  uint8_t ch_id = find_free_channel(dwc2);
  TU_ASSERT(ch_id < 16); // all channel are in use
  dwc2->haintmsk |= TU_BIT(ch_id);

  dwc2_channel_t* channel = &dwc2->channel[ch_id];
  channel->hcintmsk = HCINT_XFER_COMPLETE | HCINT_CHANNEL_HALTED | HCINT_STALL |
    HCINT_AHB_ERR | HCINT_XACT_ERR | HCINT_BABBLE_ERR | HCINT_DATATOGGLE_ERR;

  const uint16_t packet_count = tu_div_ceil(buflen, hcchar_bm->ep_size);
  channel->hctsiz = (pipe->next_data_toggle << HCTSIZ_PID_Pos) | (packet_count << HCTSIZ_PKTCNT_Pos) | buflen;

  // Control transfer always start with DATA1 for data and status stage. May has issue with ZLP
  if (pipe->next_data_toggle == HCTSIZ_PID_DATA0 || tu_edpt_number(ep_addr) == 0) {
    pipe->next_data_toggle = HCTSIZ_PID_DATA1;
  } else {
    pipe->next_data_toggle = HCTSIZ_PID_DATA0;
  }

  if (dma_host_enabled(dwc2)) {
    channel->hcdma = (uint32_t) buffer;
  } else {
    TU_ASSERT(false); // not yet support
  }

  // TODO support split transaction
  channel->hcsplt = pipe->hcsplt;
  channel->hcchar = pipe->hcchar; // kick-off transfer

  return true;
}

// Abort a queued transfer. Note: it can only abort transfer that has not been started
// Return true if a queued transfer is aborted, false if there is no transfer to abort
bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void) rhport;
  (void) dev_addr;
  (void) ep_addr;

  return false;
}

// Submit a special transfer to send 8-byte Setup Packet, when complete hcd_event_xfer_complete() must be invoked
bool hcd_setup_send(uint8_t rhport, uint8_t dev_addr, const uint8_t setup_packet[8]) {
  uint8_t pipe_id = find_opened_pipe(dev_addr, 0);
  TU_ASSERT(pipe_id < CFG_TUH_DWC2_CHANNEL_MAX); // no opened pipe
  hcd_pipe_t* pipe = &_hcd_data.pipe[pipe_id];
  pipe->next_data_toggle = HCTSIZ_PID_SETUP;

  return hcd_edpt_xfer(rhport, dev_addr, 0, (uint8_t*)(uintptr_t) setup_packet, 8);
}

// clear stall, data toggle is also reset to DATA0
bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
  (void) rhport;
  (void) dev_addr;
  (void) ep_addr;

  return false;
}

//--------------------------------------------------------------------
// HCD Event Handler
//--------------------------------------------------------------------

#if 1
static void handle_rxflvl_irq(uint8_t rhport) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  // volatile uint32_t const* rx_fifo = dwc2->fifo[0];

  // Pop control word off FIFO
  uint32_t const grxstsp = dwc2->grxstsp;
  (void) grxstsp;
  // uint8_t const pktsts = (grxstsp & GRXSTSP_PKTSTS_Msk) >> GRXSTSP_PKTSTS_Pos;
  // uint8_t const epnum = (grxstsp & GRXSTSP_EPNUM_Msk) >> GRXSTSP_EPNUM_Pos;
  // uint16_t const bcnt = (grxstsp & GRXSTSP_BCNT_Msk) >> GRXSTSP_BCNT_Pos;
  // dwc2_epout_t* epout = &dwc2->epout[epnum];
}
#endif

/* Handle Host Port interrupt, possible source are:
   - Connection Detection
   - Enable Change
   - Over Current Change
*/
TU_ATTR_ALWAYS_INLINE static inline void handle_hprt_irq(uint8_t rhport, bool in_isr) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  uint32_t hprt = dwc2->hprt & ~HPRT_W1C_MASK;
  const dwc2_hprt_t hprt_bm = dwc2->hprt_bm;

  if (dwc2->hprt & HPRT_CONN_DETECT) {
    // Port Connect Detect
    hprt |= HPRT_CONN_DETECT;

    if (hprt_bm.conn_status) {
      hcd_event_device_attach(rhport, in_isr);
    } else {
      hcd_event_device_remove(rhport, in_isr);
    }
  }

  if (dwc2->hprt & HPRT_ENABLE_CHANGE) {
    // Port enable change
    hprt |= HPRT_ENABLE_CHANGE;

    if (hprt_bm.enable) {
      // Port enable
      // Config HCFG FS/LS clock and HFIR for SOF interval according to link speed (value is in PHY clock unit)
      const tusb_speed_t speed = convert_hprt_speed(hprt_bm.speed);
      uint32_t hcfg = dwc2->hcfg & ~HCFG_FSLS_PHYCLK_SEL;

      const dwc2_gusbcfg_t gusbcfg_bm = dwc2->gusbcfg_bm;
      uint32_t clock = 60;
      if (gusbcfg_bm.phy_sel) {
        // dedicated FS is 48Mhz
        clock = 48;
        hcfg |= HCFG_FSLS_PHYCLK_SEL_48MHZ;
      } else {
        // UTMI+ or ULPI
        if (gusbcfg_bm.ulpi_utmi_sel) {
          clock = 60; // ULPI 8-bit is  60Mhz
        } else if (gusbcfg_bm.phy_if16) {
          clock = 30; // UTMI+ 16-bit is 30Mhz
        } else {
          clock = 60; // UTMI+ 8-bit is 60Mhz
        }
        hcfg |= HCFG_FSLS_PHYCLK_SEL_30_60MHZ;
      }

      dwc2->hcfg = hcfg;

      uint32_t hfir = dwc2->hfir & ~HFIR_FRIVL_Msk;
      if (speed == TUSB_SPEED_HIGH) {
        hfir |= 125*clock;
      } else {
        hfir |= 1000*clock;
      }

      dwc2->hfir = hfir;
    }
  }

  dwc2->hprt = hprt; // clear interrupt
}

void handle_channel_irq(uint8_t rhport, bool in_isr) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  for(uint8_t ch_id=0; ch_id<32; ch_id++) {
    if (tu_bit_test(dwc2->haint, ch_id)) {
      dwc2_channel_t* channel = &dwc2->channel[ch_id];
      uint32_t hcint = channel->hcint;
      hcint &= channel->hcintmsk;

      xfer_result_t result = XFER_RESULT_FAILED;
      if (hcint & HCINT_XFER_COMPLETE) {
        result = XFER_RESULT_SUCCESS;
      }
      if (hcint & HCINT_STALL) {
        result = XFER_RESULT_STALLED;
      }
      if (hcint & (HCINT_CHANNEL_HALTED | HCINT_AHB_ERR | HCINT_XACT_ERR | HCINT_BABBLE_ERR | HCINT_DATATOGGLE_ERR |
                   HCINT_BUFFER_NAK | HCINT_XCS_XACT_ERR | HCINT_DESC_ROLLOVER)) {
        result = XFER_RESULT_FAILED;
      }

      const uint8_t ep_addr = tu_edpt_addr(channel->hcchar_bm.ep_num, channel->hcchar_bm.ep_dir);
      hcd_event_xfer_complete(channel->hcchar_bm.dev_addr, ep_addr, 0, result, in_isr);

      channel->hcint = hcint;  // clear all interrupt flags

      // de-allocate channel by clearing haintmsk
      dwc2->haintmsk &= ~TU_BIT(ch_id);
    }
  }
}

/* Interrupt Hierarchy
   HCINTn.XferCompl     HCINTMSKn.XferComplMsk     HPRT ConnDetect    PrtEnChng    OverCurChng
         |                       |                          |             |             |
         +---------- AND --------+                          +------------ OR -----------+
                      |                                                   |
      HAINT.CHn            HAINTMSK.CHn                                   |
         |                       |                                        |
         +---------- AND --------+                                        |
                      |                                                   |
    GINTSTS.HCInt           GINTMSK.HCInt             GINTSTS.PrtInt            GINTMSK.PrtInt
         |                        |                          |                         |
         +---------- AND ---------+                          +---------- AND ---------+
                      |                                                   |
                      +-------------------- OR ---------------------------+
                                            |
                                   GAHBCFG.GblIntrMsk
                                            |
                                          IRQn
 */
void hcd_int_handler(uint8_t rhport, bool in_isr) {
  dwc2_regs_t* dwc2 = DWC2_REG(rhport);
  const uint32_t int_mask = dwc2->gintmsk;
  const uint32_t int_status = dwc2->gintsts & int_mask;

  TU_LOG1_HEX(int_status);

  if (int_status & GINTSTS_CONIDSTSCHNG) {
    // Connector ID status change
    dwc2->gintsts = GINTSTS_CONIDSTSCHNG;

    //if (dwc2->gotgctl)
    // dwc2->hprt = HPRT_POWER; // power on port to turn on VBUS
    //dwc2->gintmsk |= GINTMSK_PRTIM;
    // TODO wait for SRP if OTG
  }

  if (int_status & GINTSTS_HPRTINT) {
    // Host port interrupt: source is cleared in HPRT register
    TU_LOG1_HEX(dwc2->hprt);
    handle_hprt_irq(rhport, in_isr);
  }

  if (int_status & GINTSTS_HCINT) {
    // Host Channel interrupt: source is cleared in HCINT register
    TU_LOG1_HEX(dwc2->hprt);
    handle_channel_irq(rhport, in_isr);
  }

  // RxFIFO non-empty interrupt handling.
  if (int_status & GINTSTS_RXFLVL) {
    // RXFLVL bit is read-only
    dwc2->gintmsk &= ~GINTMSK_RXFLVLM; // disable RXFLVL interrupt while reading

    do {
      handle_rxflvl_irq(rhport); // read all packets
    } while(dwc2->gintsts & GINTSTS_RXFLVL);

    dwc2->gintmsk |= GINTMSK_RXFLVLM;
  }

}

#endif
