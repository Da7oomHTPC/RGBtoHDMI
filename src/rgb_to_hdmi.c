#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include "cache.h"
#include "defs.h"
#include "cpld.h"
#include "info.h"
#include "logging.h"
#include "rpi-aux.h"
#include "rpi-gpio.h"
#include "rpi-interrupts.h"
#include "rpi-mailbox-interface.h"
#include "startup.h"
#include "rpi-mailbox.h"
#include "osd.h"
#include "cpld.h"
#include "cpld_normal.h"
#include "cpld_atom.h"
#include "geometry.h"
#include "rgb_to_fb.h"

// #define INSTRUMENT_CAL
#define NUM_CAL_PASSES 1

typedef void (*func_ptr)();

#define GZ_CLK_BUSY    (1 << 7)
#define GP_CLK1_CTL (volatile uint32_t *)(PERIPHERAL_BASE + 0x101078)
#define GP_CLK1_DIV (volatile uint32_t *)(PERIPHERAL_BASE + 0x10107C)


#define PIXELVALVE2_BASE  (volatile uint32_t *)(PERIPHERAL_BASE + 0x807000)
#define PIXELVALVE2_HORZA (volatile uint32_t *)(PERIPHERAL_BASE + 0x80700c)
#define PIXELVALVE2_HORZB (volatile uint32_t *)(PERIPHERAL_BASE + 0x807010)
#define PIXELVALVE2_VERTA (volatile uint32_t *)(PERIPHERAL_BASE + 0x807014)
#define PIXELVALVE2_VERTB (volatile uint32_t *)(PERIPHERAL_BASE + 0x807018)

// =============================================================
// Forward declarations
// =============================================================

static void cpld_init();

// =============================================================
// Global variables
// =============================================================

cpld_t *cpld = NULL;
int clock_error_ppm = 0;
int vsync_time_ns = 0;
capture_info_t *capinfo;
clk_info_t clkinfo;

// =============================================================
// Local variables
// =============================================================

static capture_info_t default_capinfo  __attribute__((aligned(32)));
static capture_info_t mode7_capinfo    __attribute__((aligned(32)));
static uint32_t cpld_version_id;
static int mode7;
static int interlaced;
static int clear;
static volatile int delay;
static double pllh_clock = 0;
static int genlocked = 0;
static int resync_count = 0;
static int target_difference = 0;

// =============================================================
// OSD parameters
// =============================================================

static int elk         = 0;
static int debug       = 0;
static int m7disable   = 0;
static int scanlines   = 0;
static int deinterlace = 0;
static int vsync       = 0;
static int vlockmode   = 0;
static int vlockline   = 5;
#ifdef MULTI_BUFFER
static int nbuffers    = 2;
#endif

static int current_vlockmode = -1;

// Calculated so that the constants from librpitx work
static volatile uint32_t *gpioreg = (volatile uint32_t *)(PERIPHERAL_BASE + 0x101000UL);

// Temporary buffer that must be at least as large as a frame buffer
static unsigned char last[2048 * 1024] __attribute__((aligned(32)));

#ifndef USE_PROPERTY_INTERFACE_FOR_FB
typedef struct {
   uint32_t width;
   uint32_t height;
   uint32_t virtual_width;
   uint32_t virtual_height;
   volatile uint32_t pitch;
   volatile uint32_t depth;
   uint32_t x_offset;
   uint32_t y_offset;
   volatile uint32_t pointer;
   volatile uint32_t size;
} framebuf;
// The + 0x10000 is to miss the property buffer
static framebuf *fbp = (framebuf *) (UNCACHED_MEM_BASE + 0x10000);
#endif

// =============================================================
// Private methods
// =============================================================

// 0     0 Hz     Ground
// 1     19.2 MHz oscillator
// 2     0 Hz     testdebug0
// 3     0 Hz     testdebug1
// 4     0 Hz     PLLA
// 5     1000 MHz PLLC (changes with overclock settings)
// 6     500 MHz  PLLD
// 7     216 MHz  HDMI auxiliary
// 8-15  0 Hz     Ground


// Source 1 = OSC = 19.2MHz
// Source 4 = PLLA = 0MHz
// Source 5 = PLLC = core_freq * 3
// Source 6 = PLLD = 500MHz

static void init_gpclk(int source, int divisor) {
   log_debug("A GP_CLK1_DIV = %08"PRIx32, *GP_CLK1_DIV);

   log_debug("B GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);

   // Stop the clock generator
   *GP_CLK1_CTL = 0x5a000000 | source;

   // Wait for BUSY low
   log_debug("C GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);
   while ((*GP_CLK1_CTL) & GZ_CLK_BUSY) {}
   log_debug("D GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);

   // Configure the clock generator
   *GP_CLK1_DIV = 0x5A000000 | (divisor << 12);
   *GP_CLK1_CTL = 0x5A000000 | source;

   log_debug("E GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);

   // Start the clock generator
   *GP_CLK1_CTL = 0x5A000010 | source;

   log_debug("F GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);
   while (!((*GP_CLK1_CTL) & GZ_CLK_BUSY)) {}    // Wait for BUSY high
   log_debug("G GP_CLK1_CTL = %08"PRIx32, *GP_CLK1_CTL);

   log_debug("H GP_CLK1_DIV = %08"PRIx32, *GP_CLK1_DIV);
}

#ifdef USE_PROPERTY_INTERFACE_FOR_FB

static void init_framebuffer(capture_info_t *capinfo) {

   rpi_mailbox_property_t *mp;

   /* Initialise a framebuffer... */
   RPI_PropertyInit();
   RPI_PropertyAddTag(TAG_ALLOCATE_BUFFER);
   RPI_PropertyAddTag(TAG_SET_PHYSICAL_SIZE, capinfo->width, capinfo->height);
#ifdef MULTI_BUFFER
   RPI_PropertyAddTag(TAG_SET_VIRTUAL_SIZE, capinfo->width, capinfo->height * NBUFFERS);
#else
   RPI_PropertyAddTag(TAG_SET_VIRTUAL_SIZE, capinfo->width, capinfo->height);
#endif
   RPI_PropertyAddTag(TAG_SET_DEPTH, capinfo->bpp);
   RPI_PropertyAddTag(TAG_GET_PITCH);
   RPI_PropertyAddTag(TAG_GET_PHYSICAL_SIZE);
   RPI_PropertyAddTag(TAG_GET_DEPTH);

   RPI_PropertyProcess();

   // FIXME: A small delay (like the log) is neccessary here
   // or the RPI_PropertyGet seems to return garbage
   log_info("Initialised Framebuffer");

   if ((mp = RPI_PropertyGet(TAG_GET_PHYSICAL_SIZE))) {
      int width = mp->data.buffer_32[0];
      int height = mp->data.buffer_32[1];
      log_info("Size: %dx%d ", width, height);
   }

   if ((mp = RPI_PropertyGet(TAG_GET_PITCH))) {
      capinfo->pitch = mp->data.buffer_32[0];
      log_info("Pitch: %d bytes", capinfo->pitch);
   }

   if ((mp = RPI_PropertyGet(TAG_ALLOCATE_BUFFER))) {
      capinfo->fb = (unsigned char*)mp->data.buffer_32[0];
      log_info("Framebuffer address: %8.8X", (unsigned int)capinfo->fb);
   }
   // On the Pi 2/3 the mailbox returns the address with bits 31..30 set, which is wrong
   capinfo->fb = (unsigned char *)(((unsigned int) capinfo->fb) & 0x3fffffff);

   // Initialize the palette
   osd_update_palette();
}

#else

// An alternative way to initialize the framebuffer using mailbox channel 1
//
// I was hoping it would then be possible to page flip just by modifying the structure
// in-place. Unfortunately that didn't work, but the code might be useful in the future.

static void init_framebuffer(capture_info_t *capinfo) {
   log_info("Framebuf struct address: %p", fbp);

   // Fill in the frame buffer structure
   fbp->width          = capinfo->width;
   fbp->height         = capinfo->height;
   fbp->virtual_width  = capinfo->width;
#ifdef MULTI_BUFFER
   fbp->virtual_height = capinfo->height * NBUFFERS;
#else
   fbp->virtual_height = capinfo->height;
#endif
   fbp->pitch          = 0;
   fbp->depth          = capinfo->bpp;
   fbp->x_offset       = 0;
   fbp->y_offset       = 0;
   fbp->pointer        = 0;
   fbp->size           = 0;

   // Send framebuffer struct to the mailbox
   //
   // The +0x40000000 ensures the GPU bypasses it's cache when accessing
   // the framebuffer struct. If this is not done, the screen still initializes
   // but the ARM doesn't see the updated value for a very long time
   // i.e. the commented out section of code below is needed, and this eventually
   // exits with i=4603039
   //
   // 0xC0000000 should be added if disable_l2cache=1
   RPI_Mailbox0Write(MB0_FRAMEBUFFER, ((unsigned int)fbp) + 0xC0000000);

   // Wait for the response (0)
   RPI_Mailbox0Read(MB0_FRAMEBUFFER);

   capinfo->pitch = fbp->pitch;
   capinfo->fb = (unsigned char*)(fbp->pointer);
   int width = fbp->width;
   int height = fbp->height;

   // See comment above
   // int i  = 0;
   // while (!pitch || !fb) {
   //    pitch = fbp->pitch;
   //    fb = (unsigned char*)(fbp->pointer);
   //    i++;
   // }
   // log_info("i=%d", i);

   log_info("Initialised Framebuffer: %dx%d ", width, height);
   log_info("Pitch: %d bytes", capinfo->pitch);
   log_info("Framebuffer address: %8.8X", (unsigned int)capinfo->fb);

   // On the Pi 2/3 the mailbox returns the address with bits 31..30 set, which is wrong
   capinfo->fb = (unsigned char *)(((unsigned int) capinfo->fb) & 0x3fffffff);

   // Initialize the palette
   osd_update_palette();
}

#endif

static int calibrate_sampling_clock() {
   int a = 13;
   static int old_clock = 0;

   // Default values for the Beeb
   clkinfo.clock      = 96000000;
   clkinfo.line_len   = 64000;

   // Update from configuration
   geometry_get_clk_params(&clkinfo);

   log_info("     clkinfo.clock = %d Hz",  clkinfo.clock);
   log_info("  clkinfo.line_len = %d",     clkinfo.line_len);
   log_info(" clkinfo.clock_ppm = %d ppm", clkinfo.clock_ppm);

   // Pick the best value for core_freq and gpclk_divisor given the following constraints
   // 1. Core Freq should be as high as possible, but <= 400MHz
   // 2. Core Freq * 3 / gp_clk_divisor = clkinfo.clock
   // 3. gp_clk_divisor is an integer
   //
   // i.e. gp_clk_divisor = floor(Core Freq * 3 / clkinfo.clock)
   // and  core_freq = clkinfo.clock * gp_clk_divisor / 3

   int  gpclk_divisor = 400000000 * 3 / clkinfo.clock;
   int      core_freq = clkinfo.clock * gpclk_divisor / 3;
   int         nlines = 100; // Measure over N=100 lines
   int  nlines_ref_ns = nlines * (int) (1e9 * ((double) clkinfo.line_len) / ((double) clkinfo.clock));
   int nlines_time_ns = measure_n_lines(nlines);

   log_info("     GPCLK Divisor = %d", gpclk_divisor);
   log_info("Nominal core clock = %d Hz", core_freq);
   log_info(" Nominal %3d lines = %d ns", nlines, nlines_ref_ns);
   log_info("  Actual %3d lines = %d ns", nlines, nlines_time_ns);

   double error = (double) nlines_time_ns / (double) nlines_ref_ns;
   clock_error_ppm = ((error - 1.0) * 1e6);
   log_info("       Clock error = %d PPM", clock_error_ppm);

   int new_clock;
   if (clkinfo.clock_ppm > 0 && abs(clock_error_ppm) > clkinfo.clock_ppm) {
      if (old_clock > 0) {
         log_warn("PPM error too large, using previous clock");
         new_clock = old_clock;
      } else {
         log_warn("PPM error too large, using nominal clock");
         new_clock = core_freq;
      }
   } else {
      new_clock = (int) (((double) core_freq) / error);
   }

   log_info(" Target core clock = %d Hz", new_clock);

   // Sanity check clock
   if (new_clock < 300000000 || new_clock > 400000000) {
      log_warn("Clock out of range 300MHz-400MHz, defaulting to 384MHz");
      new_clock = DEFAULT_CORE_CLOCK;
   }

   // If the clock has changed from it's previous value, then actually change it
   if (new_clock != old_clock) {

      // Wait a while to allow UART time to empty
      for (delay = 0; delay < 100000; delay++) {
         a = a * 13;
      }

      // Switch to new core clock speed
      RPI_Mailbox0Flush(MB0_TAGS_ARM_TO_VC);
      RPI_PropertyInit();
      RPI_PropertyAddTag(TAG_SET_CLOCK_RATE, CORE_CLK_ID, new_clock, 1);
      RPI_PropertyProcess();

      // Re-initialize UART, as system clock rate changed
      RPI_AuxMiniUartInit(115200, 8);

      // And remember for next time
      old_clock = new_clock;
   }

   // Check the new clock
   int actual_clock = get_clock_rate(CORE_CLK_ID);
   log_info("  Final core clock = %d Hz", actual_clock);

   // Finally, set the new divisor
   log_debug("Setting up divisor");
   init_gpclk(GPCLK_SOURCE, gpclk_divisor);
   log_debug("Done setting up divisor");

   // Remeasure the vsync time
   vsync_time_ns = measure_vsync();

   // Remeasure the hsync time
   nlines_time_ns = measure_n_lines(nlines);

   // Ignore the interlaced flag, as this can be unreliable (e.g. Monsters)
   vsync_time_ns &= ~INTERLACED_FLAG;

   // Instead, calculate the number of lines per frame
   double lines_per_frame = ((double) vsync_time_ns) / (((double) nlines_time_ns) / ((double) nlines));

   // If number of lines is odd, then we must be interlaced
   interlaced = ((int)(lines_per_frame + 0.5)) % 2;

   // Log it
   log_info("   Lines per frame = %g", lines_per_frame);
   if (interlaced) {
      log_info(" Actual frame time = %d ns (interlaced)", vsync_time_ns);
   } else {
      log_info(" Actual frame time = %d ns (non-interlaced)", vsync_time_ns);
   }

   // Invalidate the current vlock mode to force an updated, as vsync_time_ns will have changed
   current_vlockmode = -1;

   return a;
}

static void recalculate_hdmi_clock(int vlockmode) {  // use local vsyncmode, not global
   // The very first time we get called, vsync_time_ns has not been set
   // so exit gracefully
   if (vsync_time_ns == 0) {
      return;
   }

   // Dump the PLLH registers
   log_debug("PLLH: PDIV=%d NDIV=%d FRAC=%d AUX=%d RCAL=%d PIX=%d STS=%d",
             (gpioreg[PLLH_CTRL] >> 12) & 0x7,
             gpioreg[PLLH_CTRL] & 0x3ff,
             gpioreg[PLLH_FRAC],
             gpioreg[PLLH_AUX],
             gpioreg[PLLH_RCAL],
             gpioreg[PLLH_PIX],
             gpioreg[PLLH_STS]);

   // Grab the original PLLH frequency once, at it's original value
   if (pllh_clock == 0) {
      pllh_clock = 19.2 * ((double)(gpioreg[PLLH_CTRL] & 0x3ff) + ((double)gpioreg[PLLH_FRAC]) / ((double)(1 << 20)));
   }

   //for (int i = 0; i < 32; i++) {
   //   log_debug("   PIXELVALVE2[%2d]: %08x", i, *(PIXELVALVE2_BASE + i));
   //}

   // Dump the PIXELVALVE2 registers
   log_debug(" PIXELVALVE2_HORZA: %08x", *PIXELVALVE2_HORZA);
   log_debug(" PIXELVALVE2_HORZB: %08x", *PIXELVALVE2_HORZB);
   log_debug(" PIXELVALVE2_VERTA: %08x", *PIXELVALVE2_VERTA);
   log_debug(" PIXELVALVE2_VERTB: %08x", *PIXELVALVE2_VERTB);

   // Work out the htotal and vtotal by summing the four  16-bit values:
   // A[31:16] - back porch width in pixels
   // A[15: 0] - synch width in pixels
   // B[31:16] - front porch width in pixels
   // B[15: 0] - active line width in pixels
   uint32_t htotal = (*PIXELVALVE2_HORZA) + (*PIXELVALVE2_HORZB);
   htotal = (htotal + (htotal >> 16)) & 0xFFFF;
   uint32_t vtotal = (*PIXELVALVE2_VERTA) + (*PIXELVALVE2_VERTB);
   vtotal = (vtotal + (vtotal >> 16)) & 0xFFFF;
   log_debug("           H-Total: %d pixels", htotal);
   log_debug("           V-Total: %d pixels", vtotal);

   // PLLH seems to use a fixed divider to generate the pixel clock
   int fixed_divider = 10;
   log_debug("     Fixed divider: %d", fixed_divider);

   // 720x576@50    PLLH: PDIV=1 NDIV=56 FRAC=262144 AUX=256 RCAL=256 PIX=4 STS=526655
   // 1920x1080@50  PLLH: PDIV=1 NDIV=77 FRAC=360448 AUX=256 RCAL=256 PIX=1 STS=526655
   //     An additional divider is used to get very low pixel clock rates ^
   int additional_divider = gpioreg[PLLH_PIX];
   log_debug("Additional divider: %d", additional_divider);

   // Calculate the pixel clock
   double pixel_clock = pllh_clock / ((double) fixed_divider) / ((double) additional_divider);
   log_debug("       Pixel Clock: %lf MHz", pixel_clock);

   // Calculate the error between the HDMI VSync and the Source VSync
   double source_vsync_freq = 2e9 / ((double) vsync_time_ns);
   double display_vsync_freq = 1e6 * pixel_clock / ((double) htotal) / ((double) vtotal);
   double error = display_vsync_freq / source_vsync_freq;
   double error_ppm = 1e6 * (error - 1.0);
   double f2 = pllh_clock;
   if (vlockmode > 0) {
      f2 /= error;
      f2 /= 1.0 + ((double) (HDMI_EXACT - vlockmode)) / 1000.0;
   }

   // Sanity check HDMI pixel clock
   pixel_clock = f2 / ((double) fixed_divider) / ((double) additional_divider);
   if (pixel_clock < MIN_PIXEL_CLOCK) {
      log_warn("Pixel clock of %.2lf MHz is too low; leaving unchanged", pixel_clock);
      f2 = pllh_clock;
   } else if (pixel_clock > MAX_PIXEL_CLOCK) {
      log_warn("Pixel clock of %.2lf MHz is too high; leaving unchanged", pixel_clock);
      f2 = pllh_clock;
   }

   log_debug(" Source vsync freq: %lf Hz (measured)",  source_vsync_freq);
   log_debug("Display vsync freq: %lf Hz",  display_vsync_freq);
   log_debug("       Vsync error: %lf ppm", error_ppm);
   log_debug("     Original PLLH: %lf MHz", pllh_clock);
   log_debug("       Target PLLH: %lf MHz", f2);

   // Calculate the new dividers
   int div = (int) (f2 / 19.2);
   int fract = (int) ((double)(1<<20) * (f2 / 19.2 - (double) div));
   // Sanity check the range of the fractional divider (it should actually always be in range)
   if (fract < 0) {
      log_warn("PLLH fraction < 0");
      fract = 0;
   }
   if (fract > (1<<20) - 1) {
      log_warn("PLLH fraction > 1");
      fract = (1<<20) - 1;
   }
   // Update the integer divider
   int old_ctrl = gpioreg[PLLH_CTRL];
   int old_div = old_ctrl & 0x3ff;
   if (div != old_div) {
      gpioreg[PLLH_CTRL] = 0x5A000000 | (old_ctrl & 0x00FFFC00) | div;
      int new_ctrl = gpioreg[PLLH_CTRL];
      int new_div = new_ctrl & 0x3ff;
      if (new_div == div) {
         log_debug("   New int divider: %d", new_div);
      } else {
         log_warn("Failed to write int divider: wrote %d, read back %d", div, new_div);
      }
   }

   // Update the Fractional Divider
   int old_fract = gpioreg[PLLH_FRAC];
   if (fract != old_fract) {
      gpioreg[PLLH_FRAC] = 0x5A000000 | fract;
      int new_fract = gpioreg[PLLH_FRAC];
      if (new_fract == fract) {
         log_debug(" New fract divider: %d", new_fract);
      } else {
         log_warn("Failed to write fract divider: wrote %d, read back %d", fract, new_fract);
      }
   }

   // Dump the the actual PLL frequency
   double f3 = 19.2 * ((double)(gpioreg[PLLH_CTRL] & 0x3ff) + ((double)gpioreg[PLLH_FRAC]) / ((double)(1 << 20)));
   log_debug("        Final PLLH: %lf MHz", f3);

   log_debug("PLLH: PDIV=%d NDIV=%d FRAC=%d AUX=%d RCAL=%d PIX=%d STS=%d",
             (gpioreg[PLLH_CTRL] >> 12) & 0x7,
             gpioreg[PLLH_CTRL] & 0x3ff,
             gpioreg[PLLH_FRAC],
             gpioreg[PLLH_AUX],
             gpioreg[PLLH_RCAL],
             gpioreg[PLLH_PIX],
             gpioreg[PLLH_STS]);
}

static void recalculate_hdmi_clock_once(int vlockmode) {
   if (current_vlockmode != vlockmode){
      current_vlockmode = vlockmode;
      recalculate_hdmi_clock(vlockmode);
   }
}

int recalculate_hdmi_clock_line_locked_update() {
   lock_fail = 0;
   if (vlockmode != HDMI_EXACT) {
      genlocked = 0;
      target_difference = 0;
      resync_count = 0;
      recalculate_hdmi_clock_once(vlockmode);
   } else {
      signed int difference = vsync_line - vlockline;
      if (abs(difference) > capinfo->height/4) {
         difference = -difference;
      }
      if (genlocked == 1 && abs(difference) > 2) {
         genlocked = 0;
         if (difference >= 0) {
            target_difference = -1;
         } else {
            target_difference = 1;
         }
         if (abs(difference) > 3) {
            log_info("Lock lost probably due to mode change - resetting ReSync counter");
            resync_count = 0;
            target_difference = 0;
            lock_fail = 1;
         } else {
            log_info("ReSync: %d %d", ++resync_count, target_difference);
         }
      }
      if (genlocked == 0) {
         if (difference == target_difference) {
            genlocked = 1;
            target_difference = 0;
            recalculate_hdmi_clock_once(HDMI_EXACT);
            log_info("Locked");
         } else {
            if (difference >= target_difference) {
               if (difference < (20 + target_difference)) {
                  recalculate_hdmi_clock_once(HDMI_SLOW_1000PPM);
               } else {
                  recalculate_hdmi_clock_once(HDMI_SLOW_2000PPM);
               }
            } else {
               if (difference > (-20 + target_difference)) {
                  recalculate_hdmi_clock_once(HDMI_FAST_1000PPM);
               } else {
                  recalculate_hdmi_clock_once(HDMI_FAST_2000PPM);
               }
            }
         }
      }
   }
   if (vlockmode != HDMI_EXACT) {
      // Return 0 if genlock disabled
      return 0;
   } else {
      // Return 1 if genlock enabled but not yet locked
      // Return 2 if genlock enabled and locked
      return 1 + genlocked;
   }
}

static void init_hardware() {
   int i;
   for (i = 0; i < 12; i++) {
      RPI_SetGpioPinFunction(PIXEL_BASE + i, FS_INPUT);
   }
   RPI_SetGpioPinFunction(PSYNC_PIN,    FS_INPUT);
   RPI_SetGpioPinFunction(CSYNC_PIN,    FS_INPUT);
   RPI_SetGpioPinFunction(SW1_PIN,      FS_INPUT);
   RPI_SetGpioPinFunction(SW2_PIN,      FS_INPUT);
   RPI_SetGpioPinFunction(SW3_PIN,      FS_INPUT);
   RPI_SetGpioPinFunction(SPARE_PIN,    FS_INPUT);

   RPI_SetGpioPinFunction(VERSION_PIN,  FS_OUTPUT);
   RPI_SetGpioPinFunction(MODE7_PIN,    FS_OUTPUT);
   RPI_SetGpioPinFunction(MUX_PIN,      FS_OUTPUT);
   RPI_SetGpioPinFunction(SP_CLK_PIN,   FS_OUTPUT);
   RPI_SetGpioPinFunction(SP_DATA_PIN,  FS_OUTPUT);
   RPI_SetGpioPinFunction(SP_CLKEN_PIN, FS_OUTPUT);
   RPI_SetGpioPinFunction(LED1_PIN,     FS_OUTPUT);

   RPI_SetGpioValue(VERSION_PIN,        1);
   RPI_SetGpioValue(MODE7_PIN,          1);
   RPI_SetGpioValue(MUX_PIN,            0);
   RPI_SetGpioValue(SP_CLK_PIN,         1);
   RPI_SetGpioValue(SP_DATA_PIN,        0);
   RPI_SetGpioValue(SP_CLKEN_PIN,       0);
   RPI_SetGpioValue(LED1_PIN,           0); // active high

   // This line enables IRQ interrupts
   // Enable smi_int which is IRQ 48
   // https://github.com/raspberrypi/firmware/issues/67
   RPI_GetIrqController()->Enable_IRQs_2 = (1 << VSYNCINT);

   // Initialize hardware cycle counter
   _init_cycle_counter();

   // Configure the GPCLK pin as a GPCLK
   RPI_SetGpioPinFunction(GPCLK_PIN, FS_ALT5);

   // The divisor us now the same for both modes
   log_debug("Setting up divisor");
   init_gpclk(GPCLK_SOURCE, DEFAULT_GPCLK_DIVISOR);
   log_debug("Done setting up divisor");

   // Initialize the cpld after the gpclk generator has been started
   cpld_init();

   // Initialize the On-Screen Display
   osd_init();

   // Initialise the info system with cached values (as we break the GPU property interface)
   init_info();

#ifdef DEBUG
   dump_useful_info();
#endif
}

static void cpld_init() {
   // Assert the active low version pin
   RPI_SetGpioValue(VERSION_PIN, 0);
   // The CPLD now outputs a identifier and version number on the 12-bit pixel quad bus
   cpld_version_id = 0;
   for (int i = PIXEL_BASE + 11; i >= PIXEL_BASE; i--) {
      cpld_version_id <<= 1;
      cpld_version_id |= RPI_GetGpioValue(i) & 1;
   }
   // Release the active low version pin
   RPI_SetGpioValue(VERSION_PIN, 1);

   // Set the appropriate cpld "driver" based on the version
   if ((cpld_version_id >> VERSION_DESIGN_BIT) == DESIGN_NORMAL) {
      cpld = &cpld_normal;
   } else if ((cpld_version_id >> VERSION_DESIGN_BIT) == DESIGN_ATOM) {
      cpld = &cpld_atom;
   } else {
      log_info("Unknown CPLD: identifier = %03x", cpld_version_id);
      log_info("Halting\n");
      while (1);
   }
   log_info("CPLD  Design: %s", cpld->name);
   log_info("CPLD Version: %x.%x", (cpld_version_id >> VERSION_MAJOR_BIT) & 0x0f, (cpld_version_id >> VERSION_MINOR_BIT) & 0x0f);

   // Initialize the CPLD's default sampling points
   cpld->init(cpld_version_id);
   // Initialize the geometry
   geometry_init(cpld_version_id);
}

static int test_for_elk(capture_info_t *capinfo, int elk, int mode7) {

   // If mode 7, then assume the Beeb
   if (mode7) {
      // Leave the setting unchanged
      return elk;
   }

   unsigned int ret;
   unsigned int flags = BIT_CALIBRATE | (2 << OFFSET_NBUFFERS);

   // Set to capture exactly one field
   capinfo->ncapture = 1;

   // Grab one field
   ret = rgb_to_fb(capinfo, flags);
   unsigned char *fb1 = capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch;

   // Grab second field
   ret = rgb_to_fb(capinfo, flags);
   unsigned char *fb2 = capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch;

   if (fb1 == fb2) {
      log_warn("test_for_elk() failed, both buffers the same!");
      // Leave the setting unchanged
      return elk;
   }

   unsigned int min_diff = INT_MAX;
   unsigned int min_offset = 0;

   for (int offset = -2; offset <= 2; offset += 2) {

      uint32_t *p1 = (uint32_t *)(fb1 + 2 * capinfo->pitch);
      uint32_t *p2 = (uint32_t *)(fb2 + 2 * capinfo->pitch + offset * capinfo->pitch);
      unsigned int diff = 0;
      for (int i = 0; i < (capinfo->height - 4) * capinfo->pitch; i += 4) {
         uint32_t d = (*p1++) ^ (*p2++);
         while (d) {
            if (d & 0x0F) {
               diff++;
            }
            d >>= 4;
         }
      }
      if (diff < min_diff) {
         min_diff = diff;
         min_offset = offset;
      }
      log_debug("offset = %d, diff = %u", offset, diff);

   }
   log_debug("min offset = %d", min_offset);
   return min_offset != 0;
}

#ifdef HAS_MULTICORE
static void start_core(int core, func_ptr func) {
   printf("starting core %d\r\n", core);
   *(unsigned int *)(0x4000008C + 0x10 * core) = (unsigned int) func;
}
#endif

// =============================================================
// Public methods
// =============================================================

int diff_N_frames(capture_info_t *capinfo, int n, int mode7, int elk) {
   int result = 0;

   // Calculate frame differences, broken out by channel and by sample point (A..F)
   int *by_offset = diff_N_frames_by_sample(capinfo, n, mode7, elk);

   // Collapse the offset dimension
   for (int i = 0; i < NUM_OFFSETS; i++) {
      result += by_offset[i];
   }
   return result;
}

int *diff_N_frames_by_sample(capture_info_t *capinfo, int n, int mode7, int elk) {

   unsigned int ret;

   // NUM_OFFSETS is 6 (Sample Offset A..Sample Offset F)
   static int  sum[NUM_OFFSETS];
   static int  min[NUM_OFFSETS];
   static int  max[NUM_OFFSETS];
   static int diff[NUM_OFFSETS];

   for (int i = 0; i < NUM_OFFSETS; i++) {
      sum[i] = 0;
      min[i] = INT_MAX;
      max[i] = INT_MIN;
   }

#ifdef INSTRUMENT_CAL
   unsigned int t;
   unsigned int t_capture = 0;
   unsigned int t_memcpy = 0;
   unsigned int t_compare = 0;
#endif

   unsigned int flags = mode7 | BIT_CALIBRATE | BIT_OSD | ((elk & (!mode7)) ? BIT_ELK : 0) | (2 << OFFSET_NBUFFERS);

   uint32_t bpp      = capinfo->bpp;
   uint32_t pix_mask = (bpp == 8) ? 0x0000007F : 0x00000007;
   uint32_t osd_mask = (bpp == 8) ? 0x7F7F7F7F : 0x77777777;

   // In mode 0..6, capture one field
   // In mode 7,    capture two fields
   capinfo->ncapture = mode7 ? 2 : 1;

#ifdef INSTRUMENT_CAL
   t = _get_cycle_counter();
#endif
   // Grab an initial frame
   ret = rgb_to_fb(capinfo, flags);
#ifdef INSTRUMENT_CAL
   t_capture += _get_cycle_counter() - t;
#endif

   for (int i = 0; i < n; i++) {

      for (int j = 0; j < NUM_OFFSETS; j++) {
         diff[j] = 0;
      }

#ifdef INSTRUMENT_CAL
      t = _get_cycle_counter();
#endif
      // Save the last frame
      memcpy((void *)last, (void *)(capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch), capinfo->height * capinfo->pitch);
#ifdef INSTRUMENT_CAL
      t_memcpy += _get_cycle_counter() - t;
      t = _get_cycle_counter();
#endif
      // Grab the next frame
      ret = rgb_to_fb(capinfo, flags);
#ifdef INSTRUMENT_CAL
      t_capture += _get_cycle_counter() - t;
      t = _get_cycle_counter();
#endif
      // Compare the frames
      uint32_t *fbp = (uint32_t *)(capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch);
      uint32_t *lastp = (uint32_t *)last;
      for (int y = 0; y < capinfo->height; y++) {
         int skip = 0;
         // As v_offset increases, e.g. by one, the screen image moves up one scan line, which is two frame buffer lines
         // So line N in the framebuffer corresponds to line N + 2 in the image
         int line = y + (capinfo->v_offset - 21) * 2;
         // Skip lines that might contain flashing cursor
         // (the cursor rows were determined empirically)
         if (line >= 0) {
            if (elk) {
               // Eliminate cursor lines in 32 row modes (0,1,2,4,5)
               if (!mode7 && ((line >> 1) % 8) == 5) {
                  skip = 1;
               }
               // Eliminate cursor lines in 25 row modes (3, 6)
               if (!mode7 && ((line >> 1) % 10) == 3) {
                  skip = 1;
               }
               // Eliminate cursor lines in mode 7
               // (this case is untested as I don't have a Jafa board)
               if (mode7 && ((line % 20) == 14 || (line % 20) == 15)) {
                  skip = 1;
               }
            } else {
               // Eliminate cursor lines in 32 row modes (0,1,2,4,5)
               if (!mode7 && ((line >> 1) % 8) == 7) {
                  skip = 1;
               }
               // Eliminate cursor lines in 25 row modes (3, 6)
               if (!mode7 && ((line >> 1) % 10) >= 5 && ((line >> 1) % 10) <= 7) {
                  skip = 1;
               }
               // Eliminate cursor lines in mode 7
               if (mode7 && ((line % 20) == 14 || (line % 20) == 15)) {
                  skip = 1;
               }
            }
         }
         if (skip) {
            // For debugging it's useful to see if the lines being eliminated align with the cursor
            // for (int x = 0; x < capinfo->pitch; x += 4) {
            //    *fbp++ = 0x11111111;
            // }
            fbp   += capinfo->pitch >> 2;
            lastp += capinfo->pitch >> 2;
         } else {
            for (int x = 0; x < capinfo->pitch; x += 4) {
               uint32_t d = (*fbp++) ^ (*lastp++);
               // Mask out OSD
               d &= osd_mask;
               // Work out the starting index
               int index = (x << 1) % 6;
               while (d) {
                  if (d & pix_mask) {
                     diff[index]++;
                  }
                  d >>= bpp;
                  index = (index + 1) % NUM_OFFSETS;
               }
            }
         }
      }
#ifdef INSTRUMENT_CAL
      t_compare += _get_cycle_counter() - t;
#endif
      // At this point the diffs correspond to the sample points in
      // an unusual order: A F C B E D
      //
      // This happens for three reasons:
      // - the CPLD starts with sample point B, so you get B C D E F A
      // - the firmware skips the first quad, so you get F A B C D E
      // - the frame buffer swaps odd and even pixels, so you get A F C B E D
      //
      // Mutate the result to correctly order the sample points:
      // A F C B E D => A B C D E F
      //
      // Then the downstream algorithms don't have to worry
      int f = diff[1];
      int b = diff[3];
      int d = diff[5];
      diff[1] = b;
      diff[3] = d;
      diff[5] = f;

      // Accumulate the result
      for (int j = 0; j < NUM_OFFSETS; j++) {
         sum[j] += diff[j];
         if (diff[j] < min[j]) {
            min[j] = diff[j];
         }
         if (diff[j] > max[j]) {
            max[j] = diff[j];
         }
      }
   }

#if 0
   for (int i = 0; i < NUM_OFFSETS; i++) {
      log_debug("offset %d diff:  sum = %d min = %d, max = %d", i, sum[i], min[i], max[i]);
   }
#endif

#ifdef INSTRUMENT_CAL
   log_debug("t_capture total = %d, mean = %d ", t_capture, t_capture / (n + 1));
   log_debug("t_compare total = %d, mean = %d ", t_compare, t_compare / n);
   log_debug("t_memcpy  total = %d, mean = %d ", t_memcpy,  t_memcpy / n);
   log_debug("total = %d", t_capture + t_compare + t_memcpy);
#endif
   return sum;
}


#define MODE7_CHAR_WIDTH 12

int analyze_mode7_alignment(capture_info_t *capinfo) {
   // mode 7 character is 12 pixels wide
   int counts[MODE7_CHAR_WIDTH];
   // bit offset pixels 0..7
   int px_offset_map[] = {4, 0, 12, 8, 20, 16, 28, 24};

   unsigned int flags = BIT_MODE7 | BIT_CALIBRATE | BIT_OSD | (2 << OFFSET_NBUFFERS);

   // Capture two fields
   capinfo->ncapture = 2;

   // Grab a frame
   int ret = rgb_to_fb(capinfo, flags);

   // Work out the base address of the frame buffer that was used
   uint32_t *fbp = (uint32_t *)(capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch);

   // Initialize the counters
   for (int i = 0; i < MODE7_CHAR_WIDTH; i++) {
      counts[i] = 0;
   }

   // Count the pixels
   for (int line = 0; line < capinfo->height; line++) {
      int index = 0;
      for (int byte = 0; byte < capinfo->pitch; byte += 4) {
         uint32_t word = *fbp++;
         int *offset = px_offset_map;
         for (int i = 0; i < 8; i++) {
            int px = (word >> (*offset++)) & 7;
            if (px) {
               counts[index]++;
            }
            index = (index + 1) % MODE7_CHAR_WIDTH;
         }
      }
   }

   // Log the raw counters
   for (int i = 0; i < MODE7_CHAR_WIDTH; i++) {
      log_info("counter %2d = %d", i, counts[i]);
   }

   // A typical distribution looks like
   // INFO: counter  0 = 647
   // INFO: counter  1 = 573
   // INFO: counter  2 = 871
   // INFO: counter  3 = 878
   // INFO: counter  4 = 572
   // INFO: counter  5 = 653
   // INFO: counter  6 = 869
   // INFO: counter  7 = 742
   // INFO: counter  8 = 2
   // INFO: counter  9 = 2
   // INFO: counter 10 = 906
   // INFO: counter 11 = 1019

   // There should be a two pixel minim, so look for
   int min_count = INT_MAX;
   int min_i = -1;
   for (int i = 0; i < MODE7_CHAR_WIDTH; i++) {
      int c = counts[i] + counts[(i + 1) % MODE7_CHAR_WIDTH];
      if (c < min_count) {
         min_count = c;
         min_i = i;
      }
   }
   log_info("minima at index: %d", min_i);

   // That minima should occur in pixels 0 and 1, so compute a delay to make this so
   return MODE7_CHAR_WIDTH - min_i;
}

#if 0
int total_N_frames(capture_info_t *capinfo, int n, int mode7, int elk) {

   int sum = 0;
   int min = INT_MAX;
   int max = INT_MIN;

#ifdef INSTRUMENT_CAL
   unsigned int t;
   unsigned int t_capture = 0;
   unsigned int t_compare = 0;
#endif

   unsigned int flags = mode7 | BIT_CALIBRATE | BIT_OSD | ((elk & !mode7) ? BIT_ELK : 0) | (2 << OFFSET_NBUFFERS);

   // In mode 0..6, capture one field
   // In mode 7,    capture two fields
   capinfo->ncapture = mode7 ? 2 : 1;

   for (int i = 0; i < n; i++) {
      int total = 0;

      // Grab the next frame
      ret = rgb_to_fb(capinfo, flags);
#ifdef INSTRUMENT_CAL
      t_capture += _get_cycle_counter() - t;
      t = _get_cycle_counter();
#endif
      // Compare the frames
      uint32_t *fbp = (uint32_t *)(capinfo->fb + ((ret >> OFFSET_LAST_BUFFER) & 3) * capinfo->height * capinfo->pitch);
      for (int j = 0; j < capinfo->height * capinfo->pitch; j += 4) {
         uint32_t f = *fbp++;
         // Mask out OSD
         f &= 0x77777777;
         while (f) {
            if (f & 0x0F) {
               total++;
            }
            f >>= 4;
         }
      }
#ifdef INSTRUMENT_CAL
      t_compare += _get_cycle_counter() - t;
#endif

      // Accumulate the result
      sum += total;
      if (total < min) {
         min = total;
      }
      if (total > max) {
         max = total;
      }
   }

   int mean = sum / n;
   log_debug("total: sum = %d mean = %d, min = %d, max = %d", sum, mean, min, max);
#ifdef INSTRUMENT_CAL
   log_debug("t_capture total = %d, mean = %d ", t_capture, t_capture / (n + 1));
   log_debug("t_compare total = %d, mean = %d ", t_compare, t_compare / n);
   log_debug("total = %d", t_capture + t_compare + t_memcpy);
#endif
   return sum;
}
#endif

#ifdef MULTI_BUFFER
void swapBuffer(int buffer) {
   // Flush the previous response from the GPU->ARM mailbox
   // Doing it like this avoids stalling for the response
   RPI_Mailbox0Flush(MB0_TAGS_ARM_TO_VC);
   RPI_PropertyInit();
   RPI_PropertyAddTag(TAG_SET_VIRTUAL_OFFSET, 0, capinfo->height * buffer);
   // Use version that doesn't wait for the response
   RPI_PropertyProcessNoCheck();
}
#endif

void set_deinterlace(int mode) {
   deinterlace = mode;
}

int get_deinterlace() {
   return deinterlace;
}

void set_scanlines(int on) {
   scanlines = on;
   clear = BIT_CLEAR;
}

int get_scanlines() {
   return scanlines;
}

void set_elk(int on) {
   elk = on;
   clear = BIT_CLEAR;
}

int get_elk() {
   return elk;
}

void set_vsync(int on) {
   vsync = on;
}

int get_vsync() {
   return vsync;
}

void set_vlockmode(int val) {
   genlocked = 0;
   vlockmode = val;
   recalculate_hdmi_clock_line_locked_update();
}

int get_vlockmode() {
   return vlockmode;
}

void set_vlockline(int val) {
   genlocked = 0;
   vlockline = val;
   if (vlockline > capinfo->height/4) {
      default_vsync_line = 1;
   } else {
      default_vsync_line = capinfo->height/2;
   }
}

int get_vlockline() {
   return vlockline;
}

#ifdef MULTI_BUFFER
int get_nbuffers() {
   return nbuffers;
}

void set_nbuffers(int val) {
   nbuffers=val;
}
#endif

void set_debug(int on) {
   debug = on;
}

int get_debug() {
   return debug;
}

void set_m7disable(int on) {
   m7disable = on;
}

int get_m7disable() {
   return m7disable;
}

void action_calibrate_clocks() {
   // re-measure vsync and set the core/sampling clocks
   calibrate_sampling_clock();
   // set the hdmi clock property to match exactly
   set_vlockmode(HDMI_EXACT);
}

void action_calibrate_auto() {
   // re-measure vsync and set the core/sampling clocks
   calibrate_sampling_clock();
   // During calibration we do our best to auto-delect an Electron
   elk = test_for_elk(capinfo, elk, mode7);
   log_debug("Elk mode = %d", elk);
   for (int c = 0; c < NUM_CAL_PASSES; c++) {
      cpld->calibrate(capinfo, elk);
   }
}

int is_genlocked() {
   return genlocked;
}

void rgb_to_hdmi_main() {

   int result;
   int last_mode7;
   int mode_changed;
   int fb_size_changed;
   int active_size_decreased;
   int clk_changed;
   int ncapture;

   capture_info_t last_capinfo;
   clk_info_t last_clkinfo;

   // Setup defaults (these may be overridden by the CPLD)
   default_capinfo.capture_line = capture_line_default_4bpp;
   mode7_capinfo.capture_line   = capture_line_mode7_4bpp;

   capinfo = &default_capinfo;

   // Determine initial mode
   mode7 = rgb_to_fb(capinfo, BIT_PROBE) & BIT_MODE7 & (!m7disable);

   // Default to capturing indefinitely
   ncapture = -1;

   while (1) {

      // Switch the the approriate capinfo structure instance
      capinfo = mode7 ? &mode7_capinfo : &default_capinfo;

      log_debug("Setting mode7 = %d", mode7);

      geometry_set_mode(mode7);
      geometry_get_fb_params(capinfo);

      log_debug("Loading sample points");
      cpld->set_mode(capinfo, mode7);
      log_debug("Done loading sample points");

      log_debug("Setting up frame buffer");
      init_framebuffer(capinfo);
      log_debug("Done setting up frame buffer");

      // Measure the frame time and set the sampling clock
      calibrate_sampling_clock();

      // Recalculate the HDMI clock (if the vlockmode property requires this)
      recalculate_hdmi_clock_line_locked_update();

      clear = BIT_CLEAR;

      osd_refresh();

      do {

         int flags = mode7 | clear;
         if (!m7disable) {
            flags |= BIT_MODE_DETECT;
         }
         if (interlaced) {
            flags |= BIT_INTERLACED;
         }
         if (vsync) {
            flags |= BIT_VSYNC;
         }
         if (elk & (!mode7)) {
            flags |= BIT_ELK;
         }
         if (debug) {
            flags |= BIT_DEBUG;
         }
         if (scanlines) {
            flags |= BIT_SCANLINES;
         }
         if (osd_active()) {
            flags |= BIT_OSD;
         }
         flags |= deinterlace << OFFSET_INTERLACE;
#ifdef MULTI_BUFFER
         flags |= nbuffers << OFFSET_NBUFFERS;
#endif
         capinfo->ncapture = ncapture;
         log_debug("Entering rgb_to_fb, flags=%08x", flags);
         result = rgb_to_fb(capinfo, flags);
         log_debug("Leaving rgb_to_fb, result=%04x", result);
         clear = 0;

         if (result & RET_EXPIRED) {
            ncapture = osd_key(OSD_EXPIRED);
         } else if (result & RET_SW1) {
            ncapture = osd_key(OSD_SW1);
         } else if (result & RET_SW2) {
            ncapture = osd_key(OSD_SW2);
         } else if (result & RET_SW3) {
            ncapture = osd_key(OSD_SW3);
         }

         // Possibly the size or offset has been adjusted, so update current capinfo
         memcpy(&last_capinfo, capinfo, sizeof last_capinfo);
         memcpy(&last_clkinfo, &clkinfo, sizeof last_clkinfo);

         geometry_get_fb_params(capinfo);

         fb_size_changed = (capinfo->width != last_capinfo.width) || (capinfo->height != last_capinfo.height) || (capinfo->bpp != last_capinfo.bpp);
         active_size_decreased = (capinfo->chars_per_line < last_capinfo.chars_per_line) || (capinfo->nlines < last_capinfo.nlines);

         geometry_get_clk_params(&clkinfo);
         clk_changed = (clkinfo.clock != last_clkinfo.clock) || (clkinfo.line_len != last_clkinfo.line_len);

         last_mode7 = mode7;
         mode7 = result & BIT_MODE7 & (!m7disable);
         mode_changed = (mode7 != last_mode7) || (capinfo->px_sampling != last_capinfo.px_sampling);

         if (active_size_decreased) {
            clear = BIT_CLEAR;
         }

         if (clk_changed || (result & RET_INTERLACE_CHANGED) || lock_fail != 0) {
            target_difference = 0;
            resync_count = 0;
            // Measure the frame time and set the sampling clock
            calibrate_sampling_clock();
            // Recalculate the HDMI clock (if the vlockmode property requires this)
            recalculate_hdmi_clock_line_locked_update();
         }

      } while (!mode_changed && !fb_size_changed);

      osd_clear();
   }
}

void kernel_main(unsigned int r0, unsigned int r1, unsigned int atags)
{
   RPI_AuxMiniUartInit(115200, 8);

   log_info("RGB to HDMI booted");

   enable_MMU_and_IDCaches();
   _enable_unaligned_access();

   init_hardware();

#ifdef HAS_MULTICORE
   int i;

   printf("main running on core %u\r\n", _get_core());

   for (i = 0; i < 10000000; i++);
   start_core(1, _spin_core);
   for (i = 0; i < 10000000; i++);
   start_core(2, _spin_core);
   for (i = 0; i < 10000000; i++);
   start_core(3, _spin_core);
   for (i = 0; i < 10000000; i++);
#endif

   rgb_to_hdmi_main();
}
