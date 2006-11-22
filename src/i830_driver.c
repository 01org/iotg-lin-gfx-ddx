/* $XFree86: xc/programs/Xserver/hw/xfree86/drivers/i810/i830_driver.c,v 1.50 2004/02/20 00:06:00 alanh Exp $ */
/**************************************************************************

Copyright 2001 VA Linux Systems Inc., Fremont, California.
Copyright © 2002 by David Dawes

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
on the rights to use, copy, modify, merge, publish, distribute, sub
license, and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
THE COPYRIGHT HOLDERS AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

/*
 * Reformatted with GNU indent (2.2.8), using the following options:
 *
 *    -bad -bap -c41 -cd0 -ncdb -ci6 -cli0 -cp0 -ncs -d0 -di3 -i3 -ip3 -l78
 *    -lp -npcs -psl -sob -ss -br -ce -sc -hnl
 *
 * This provides a good match with the original i810 code and preferred
 * XFree86 formatting conventions.
 *
 * When editing this driver, please follow the existing formatting, and edit
 * with <TAB> characters expanded at 8-column intervals.
 */

/*
 * Authors: Jeff Hartmann <jhartmann@valinux.com>
 *          Abraham van der Merwe <abraham@2d3d.co.za>
 *          David Dawes <dawes@xfree86.org>
 *          Alan Hourihane <alanh@tungstengraphics.com>
 */

/*
 * Mode handling is based on the VESA driver written by:
 * Paulo César Pereira de Andrade <pcpa@conectiva.com.br>
 */

/*
 * Changes:
 *
 *    23/08/2001 Abraham van der Merwe <abraham@2d3d.co.za>
 *        - Fixed display timing bug (mode information for some
 *          modes were not initialized correctly)
 *        - Added workarounds for GTT corruptions (I don't adjust
 *          the pitches for 1280x and 1600x modes so we don't
 *          need extra memory)
 *        - The code will now default to 60Hz if LFP is connected
 *        - Added different refresh rate setting code to work
 *          around 0x4f02 BIOS bug
 *        - BIOS workaround for some mode sets (I use legacy BIOS
 *          calls for setting those)
 *        - Removed 0x4f04, 0x01 (save state) BIOS call which causes
 *          LFP to malfunction (do some house keeping and restore
 *          modes ourselves instead - not perfect, but at least the
 *          LFP is working now)
 *        - Several other smaller bug fixes
 *
 *    06/09/2001 Abraham van der Merwe <abraham@2d3d.co.za>
 *        - Preliminary local memory support (works without agpgart)
 *        - DGA fixes (the code were still using i810 mode sets, etc.)
 *        - agpgart fixes
 *
 *    18/09/2001
 *        - Proper local memory support (should work correctly now
 *          with/without agpgart module)
 *        - more agpgart fixes
 *        - got rid of incorrect GTT adjustments
 *
 *    09/10/2001
 *        - Changed the DPRINTF() variadic macro to an ANSI C compatible
 *          version
 *
 *    10/10/2001
 *        - Fixed DPRINTF_stub(). I forgot the __...__ macros in there
 *          instead of the function arguments :P
 *        - Added a workaround for the 1600x1200 bug (Text mode corrupts
 *          when you exit from any 1600x1200 mode and 1280x1024@85Hz. I
 *          suspect this is a BIOS bug (hence the 1280x1024@85Hz case)).
 *          For now I'm switching to 800x600@60Hz then to 80x25 text mode
 *          and then restoring the registers - very ugly indeed.
 *
 *    15/10/2001
 *        - Improved 1600x1200 mode set workaround. The previous workaround
 *          was causing mode set problems later on.
 *
 *    18/10/2001
 *        - Fixed a bug in I830BIOSLeaveVT() which caused a bug when you
 *          switched VT's
 */
/*
 *    07/2002 David Dawes
 *        - Add Intel(R) 855GM/852GM support.
 */
/*
 *    07/2002 David Dawes
 *        - Cleanup code formatting.
 *        - Improve VESA mode selection, and fix refresh rate selection.
 *        - Don't duplicate functions provided in 4.2 vbe modules.
 *        - Don't duplicate functions provided in the vgahw module.
 *        - Rewrite memory allocation.
 *        - Rewrite initialisation and save/restore state handling.
 *        - Decouple the i810 support from i830 and later.
 *        - Remove various unnecessary hacks and workarounds.
 *        - Fix an 845G problem with the ring buffer not in pre-allocated
 *          memory.
 *        - Fix screen blanking.
 *        - Clear the screen at startup so you don't see the previous session.
 *        - Fix some HW cursor glitches, and turn HW cursor off at VT switch
 *          and exit.
 *
 *    08/2002 Keith Whitwell
 *        - Fix DRI initialisation.
 *
 *
 *    08/2002 Alan Hourihane and David Dawes
 *        - Add XVideo support.
 *
 *
 *    10/2002 David Dawes
 *        - Add Intel(R) 865G support.
 *
 *
 *    01/2004 Alan Hourihane
 *        - Add Intel(R) 915G support.
 *        - Add Dual Head and Clone capabilities.
 *        - Add lid status checking
 *        - Fix Xvideo with high-res LFP's
 *        - Add ARGB HW cursor support
 *
 *    05/2005 Alan Hourihane
 *        - Add Intel(R) 945G support.
 *
 *    09/2005 Alan Hourihane
 *        - Add Intel(R) 945GM support.
 *
 *    10/2005 Alan Hourihane, Keith Whitwell, Brian Paul
 *        - Added Rotation support
 *
 *    12/2005 Alan Hourihane, Keith Whitwell
 *        - Add Intel(R) 965G support.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef PRINT_MODE_INFO
#define PRINT_MODE_INFO 0
#endif

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86Resources.h"
#include "xf86RAC.h"
#include "xf86cmap.h"
#include "compiler.h"
#include "mibstore.h"
#include "vgaHW.h"
#include "mipointer.h"
#include "micmap.h"
#include "shadowfb.h"
#include <X11/extensions/randr.h>
#include "fb.h"
#include "miscstruct.h"
#include "dixstruct.h"
#include "xf86xv.h"
#include <X11/extensions/Xv.h>
#include "vbe.h"
#include "shadow.h"
#include "i830.h"
#include "i830_display.h"
#include "i830_debug.h"
#include "i830_bios.h"

#ifdef XF86DRI
#include "dri.h"
#include <sys/ioctl.h>
#include <errno.h>
#endif

#define BIT(x) (1 << (x))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define NB_OF(x) (sizeof (x) / sizeof (*x))

/* *INDENT-OFF* */
static SymTabRec I830Chipsets[] = {
   {PCI_CHIP_I830_M,		"i830"},
   {PCI_CHIP_845_G,		"845G"},
   {PCI_CHIP_I855_GM,		"852GM/855GM"},
   {PCI_CHIP_I865_G,		"865G"},
   {PCI_CHIP_I915_G,		"915G"},
   {PCI_CHIP_E7221_G,		"E7221 (i915)"},
   {PCI_CHIP_I915_GM,		"915GM"},
   {PCI_CHIP_I945_G,		"945G"},
   {PCI_CHIP_I945_GM,		"945GM"},
   {PCI_CHIP_I965_G,		"965G"},
   {PCI_CHIP_I965_G_1,		"965G"},
   {PCI_CHIP_I965_Q,		"965Q"},
   {PCI_CHIP_I946_GZ,		"946GZ"},
   {-1,				NULL}
};

static PciChipsets I830PciChipsets[] = {
   {PCI_CHIP_I830_M,		PCI_CHIP_I830_M,	RES_SHARED_VGA},
   {PCI_CHIP_845_G,		PCI_CHIP_845_G,		RES_SHARED_VGA},
   {PCI_CHIP_I855_GM,		PCI_CHIP_I855_GM,	RES_SHARED_VGA},
   {PCI_CHIP_I865_G,		PCI_CHIP_I865_G,	RES_SHARED_VGA},
   {PCI_CHIP_I915_G,		PCI_CHIP_I915_G,	RES_SHARED_VGA},
   {PCI_CHIP_E7221_G,		PCI_CHIP_E7221_G,	RES_SHARED_VGA},
   {PCI_CHIP_I915_GM,		PCI_CHIP_I915_GM,	RES_SHARED_VGA},
   {PCI_CHIP_I945_G,		PCI_CHIP_I945_G,	RES_SHARED_VGA},
   {PCI_CHIP_I945_GM,		PCI_CHIP_I945_GM,	RES_SHARED_VGA},
   {PCI_CHIP_I965_G,		PCI_CHIP_I965_G,	RES_SHARED_VGA},
   {PCI_CHIP_I965_G_1,		PCI_CHIP_I965_G_1,	RES_SHARED_VGA},
   {PCI_CHIP_I965_Q,		PCI_CHIP_I965_Q,	RES_SHARED_VGA},
   {PCI_CHIP_I946_GZ,		PCI_CHIP_I946_GZ,	RES_SHARED_VGA},
   {-1,				-1,			RES_UNDEFINED}
};

/*
 * Note: "ColorKey" is provided for compatibility with the i810 driver.
 * However, the correct option name is "VideoKey".  "ColorKey" usually
 * refers to the tranparency key for 8+24 overlays, not for video overlays.
 */

typedef enum {
   OPTION_NOACCEL,
   OPTION_SW_CURSOR,
   OPTION_CACHE_LINES,
   OPTION_DRI,
   OPTION_PAGEFLIP,
   OPTION_XVIDEO,
   OPTION_VIDEO_KEY,
   OPTION_COLOR_KEY,
   OPTION_VBE_RESTORE,
   OPTION_DISPLAY_INFO,
   OPTION_DEVICE_PRESENCE,
   OPTION_MONITOR_LAYOUT,
   OPTION_CLONE,
   OPTION_CLONE_REFRESH,
   OPTION_CHECKDEVICES,
   OPTION_FIXEDPIPE,
   OPTION_ROTATE,
   OPTION_LINEARALLOC,
   OPTION_INTELTEXPOOL,
   OPTION_INTELMMSIZE
} I830Opts;

static OptionInfoRec I830Options[] = {
   {OPTION_NOACCEL,	"NoAccel",	OPTV_BOOLEAN,	{0},	FALSE},
   {OPTION_SW_CURSOR,	"SWcursor",	OPTV_BOOLEAN,	{0},	FALSE},
   {OPTION_CACHE_LINES,	"CacheLines",	OPTV_INTEGER,	{0},	FALSE},
   {OPTION_DRI,		"DRI",		OPTV_BOOLEAN,	{0},	TRUE},
   {OPTION_PAGEFLIP,	"PageFlip",	OPTV_BOOLEAN,	{0},	FALSE},
   {OPTION_XVIDEO,	"XVideo",	OPTV_BOOLEAN,	{0},	TRUE},
   {OPTION_COLOR_KEY,	"ColorKey",	OPTV_INTEGER,	{0},	FALSE},
   {OPTION_VIDEO_KEY,	"VideoKey",	OPTV_INTEGER,	{0},	FALSE},
   {OPTION_MONITOR_LAYOUT, "MonitorLayout", OPTV_ANYSTR,{0},	FALSE},
   {OPTION_CLONE,	"Clone",	OPTV_BOOLEAN,	{0},	FALSE},
   {OPTION_CLONE_REFRESH,"CloneRefresh",OPTV_INTEGER,	{0},	FALSE},
   {OPTION_CHECKDEVICES, "CheckDevices",OPTV_BOOLEAN,	{0},	FALSE},
   {OPTION_FIXEDPIPE,   "FixedPipe",    OPTV_ANYSTR, 	{0},	FALSE},
   {OPTION_ROTATE,      "Rotate",       OPTV_ANYSTR,    {0},    FALSE},
   {OPTION_LINEARALLOC, "LinearAlloc",  OPTV_INTEGER,   {0},    FALSE},
   {OPTION_INTELTEXPOOL,"Legacy3D",     OPTV_BOOLEAN,	{0},	FALSE},
   {OPTION_INTELMMSIZE, "AperTexSize",  OPTV_INTEGER,	{0},	FALSE},
   {-1,			NULL,		OPTV_NONE,	{0},	FALSE}
};
/* *INDENT-ON* */

const char *i830_output_type_names[] = {
   "Unused",
   "Analog",
   "DVO",
   "SDVO",
   "LVDS",
   "TVOUT",
};

static void I830DisplayPowerManagementSet(ScrnInfoPtr pScrn,
					  int PowerManagementMode, int flags);
static void i830AdjustFrame(int scrnIndex, int x, int y, int flags);
static Bool I830CloseScreen(int scrnIndex, ScreenPtr pScreen);
static Bool I830SaveScreen(ScreenPtr pScreen, int unblack);
static Bool I830EnterVT(int scrnIndex, int flags);
static CARD32 I830CheckDevicesTimer(OsTimerPtr timer, CARD32 now, pointer arg);

extern int I830EntityIndex;

/* temporary */
extern void xf86SetCursor(ScreenPtr pScreen, CursorPtr pCurs, int x, int y);

#ifdef I830DEBUG
void
I830DPRINTF_stub(const char *filename, int line, const char *function,
		 const char *fmt, ...)
{
   va_list ap;

   ErrorF("\n##############################################\n"
	  "*** In function %s, on line %d, in file %s ***\n",
	  function, line, filename);
   va_start(ap, fmt);
   VErrorF(fmt, ap);
   va_end(ap);
   ErrorF("##############################################\n\n");
}
#else /* #ifdef I830DEBUG */
void
I830DPRINTF_stub(const char *filename, int line, const char *function,
		 const char *fmt, ...)
{
   /* do nothing */
}
#endif /* #ifdef I830DEBUG */

/* Export I830 options to i830 driver where necessary */
const OptionInfoRec *
I830AvailableOptions(int chipid, int busid)
{
   int i;

   for (i = 0; I830PciChipsets[i].PCIid > 0; i++) {
      if (chipid == I830PciChipsets[i].PCIid)
	 return I830Options;
   }
   return NULL;
}

static Bool
I830GetRec(ScrnInfoPtr pScrn)
{
   I830Ptr pI830;

   if (pScrn->driverPrivate)
      return TRUE;
   pI830 = pScrn->driverPrivate = xnfcalloc(sizeof(I830Rec), 1);
   return TRUE;
}

static void
I830FreeRec(ScrnInfoPtr pScrn)
{
   I830Ptr pI830;

   if (!pScrn)
      return;
   if (!pScrn->driverPrivate)
      return;

   pI830 = I830PTR(pScrn);

   xfree(pScrn->driverPrivate);
   pScrn->driverPrivate = NULL;
}

static void
I830ProbeDDC(ScrnInfoPtr pScrn, int index)
{
   vbeInfoPtr pVbe;

   /* The vbe module gets loaded in PreInit(), so no need to load it here. */

   pVbe = VBEInit(NULL, index);
   ConfiguredMonitor = vbeDoEDID(pVbe, NULL);
}

static int
I830DetectMemory(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);
   PCITAG bridge;
   CARD16 gmch_ctrl;
   int memsize = 0;
   int range;
#if 0
   VbeInfoBlock *vbeInfo;
#endif

   bridge = pciTag(0, 0, 0);		/* This is always the host bridge */
   gmch_ctrl = pciReadWord(bridge, I830_GMCH_CTRL);

   /* We need to reduce the stolen size, by the GTT and the popup.
    * The GTT varying according the the FbMapSize and the popup is 4KB */
   range = (pI830->FbMapSize / (1024*1024)) + 4;

   if (IS_I85X(pI830) || IS_I865G(pI830) || IS_I9XX(pI830)) {
      switch (gmch_ctrl & I830_GMCH_GMS_MASK) {
      case I855_GMCH_GMS_STOLEN_1M:
	 memsize = MB(1) - KB(range);
	 break;
      case I855_GMCH_GMS_STOLEN_4M:
	 memsize = MB(4) - KB(range);
	 break;
      case I855_GMCH_GMS_STOLEN_8M:
	 memsize = MB(8) - KB(range);
	 break;
      case I855_GMCH_GMS_STOLEN_16M:
	 memsize = MB(16) - KB(range);
	 break;
      case I855_GMCH_GMS_STOLEN_32M:
	 memsize = MB(32) - KB(range);
	 break;
      case I915G_GMCH_GMS_STOLEN_48M:
	 if (IS_I9XX(pI830))
	    memsize = MB(48) - KB(range);
	 break;
      case I915G_GMCH_GMS_STOLEN_64M:
	 if (IS_I9XX(pI830))
	    memsize = MB(64) - KB(range);
	 break;
      }
   } else {
      switch (gmch_ctrl & I830_GMCH_GMS_MASK) {
      case I830_GMCH_GMS_STOLEN_512:
	 memsize = KB(512) - KB(range);
	 break;
      case I830_GMCH_GMS_STOLEN_1024:
	 memsize = MB(1) - KB(range);
	 break;
      case I830_GMCH_GMS_STOLEN_8192:
	 memsize = MB(8) - KB(range);
	 break;
      case I830_GMCH_GMS_LOCAL:
	 memsize = 0;
	 xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		    "Local memory found, but won't be used.\n");
	 break;
      }
   }

#if 0
   /* And 64KB page aligned */
   memsize &= ~0xFFFF;
#endif

   if (memsize > 0) {
      xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		 "detected %d kB stolen memory.\n", memsize / 1024);
   } else {
      xf86DrvMsg(pScrn->scrnIndex, X_INFO, "no video memory detected.\n");
   }

   return memsize;
}

static Bool
I830MapMMIO(ScrnInfoPtr pScrn)
{
   int mmioFlags;
   I830Ptr pI830 = I830PTR(pScrn);

#if !defined(__alpha__)
   mmioFlags = VIDMEM_MMIO | VIDMEM_READSIDEEFFECT;
#else
   mmioFlags = VIDMEM_MMIO | VIDMEM_READSIDEEFFECT | VIDMEM_SPARSE;
#endif

   pI830->MMIOBase = xf86MapPciMem(pScrn->scrnIndex, mmioFlags,
				   pI830->PciTag,
				   pI830->MMIOAddr, I810_REG_SIZE);
   if (!pI830->MMIOBase)
      return FALSE;
   return TRUE;
}

static Bool
I830MapMem(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);
   long i;

   for (i = 2; i < pI830->FbMapSize; i <<= 1) ;
   pI830->FbMapSize = i;

   if (!I830MapMMIO(pScrn))
      return FALSE;

   pI830->FbBase = xf86MapPciMem(pScrn->scrnIndex, VIDMEM_FRAMEBUFFER,
				 pI830->PciTag,
				 pI830->LinearAddr, pI830->FbMapSize);
   if (!pI830->FbBase)
      return FALSE;

   if (I830IsPrimary(pScrn))
   pI830->LpRing->virtual_start = pI830->FbBase + pI830->LpRing->mem.Start;

   return TRUE;
}

static void
I830UnmapMMIO(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);

   xf86UnMapVidMem(pScrn->scrnIndex, (pointer) pI830->MMIOBase,
		   I810_REG_SIZE);
   pI830->MMIOBase = 0;
}

static Bool
I830UnmapMem(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);

   xf86UnMapVidMem(pScrn->scrnIndex, (pointer) pI830->FbBase,
		   pI830->FbMapSize);
   pI830->FbBase = 0;
   I830UnmapMMIO(pScrn);
   return TRUE;
}

static void
I830LoadPalette(ScrnInfoPtr pScrn, int numColors, int *indices,
		LOCO * colors, VisualPtr pVisual)
{
   I830Ptr pI830;
   int i,j, index;
   unsigned char r, g, b;
   CARD32 val, temp;
   int palreg;
   int dspreg, dspbase, dspsurf;
   int p;

   DPRINTF(PFX, "I830LoadPalette: numColors: %d\n", numColors);
   pI830 = I830PTR(pScrn);

   for(p=0; p < pI830->num_pipes; p++) {
      I830PipePtr pI830Pipe = &pI830->pipes[p];

      if (p == 0) {
         palreg = PALETTE_A;
         dspreg = DSPACNTR;
         dspbase = DSPABASE;
	 dspsurf = DSPASURF;
      } else {
         palreg = PALETTE_B;
         dspreg = DSPBCNTR;
         dspbase = DSPBBASE;
	 dspsurf = DSPBSURF;
      }

      if (pI830Pipe->enabled == 0)
	 continue;  

      pI830Pipe->gammaEnabled = 1;
      
      /* To ensure gamma is enabled we need to turn off and on the plane */
      temp = INREG(dspreg);
      OUTREG(dspreg, temp & ~(1<<31));
      OUTREG(dspbase, INREG(dspbase));
      OUTREG(dspreg, temp | DISPPLANE_GAMMA_ENABLE);
      OUTREG(dspbase, INREG(dspbase));
      if (IS_I965G(pI830))
	 OUTREG(dspsurf, INREG(dspsurf));

      /* It seems that an initial read is needed. */
      temp = INREG(palreg);

      switch(pScrn->depth) {
      case 15:
        for (i = 0; i < numColors; i++) {
         index = indices[i];
         r = colors[index].red;
         g = colors[index].green;
         b = colors[index].blue;
	 val = (r << 16) | (g << 8) | b;
         for (j = 0; j < 8; j++) {
	    OUTREG(palreg + index * 32 + (j * 4), val);
         }
        }
      break;
      case 16:
        for (i = 0; i < numColors; i++) {
         index = indices[i];
	 r   = colors[index / 2].red;
	 g   = colors[index].green;
	 b   = colors[index / 2].blue;

	 val = (r << 16) | (g << 8) | b;
	 OUTREG(palreg + index * 16, val);
	 OUTREG(palreg + index * 16 + 4, val);
	 OUTREG(palreg + index * 16 + 8, val);
	 OUTREG(palreg + index * 16 + 12, val);

   	 if (index <= 31) {
            r   = colors[index].red;
	    g   = colors[(index * 2) + 1].green;
	    b   = colors[index].blue;

	    val = (r << 16) | (g << 8) | b;
	    OUTREG(palreg + index * 32, val);
	    OUTREG(palreg + index * 32 + 4, val);
	    OUTREG(palreg + index * 32 + 8, val);
	    OUTREG(palreg + index * 32 + 12, val);
	 }
        }
        break;
      default:
        for(i = 0; i < numColors; i++) {
	 index = indices[i];
	 r = colors[index].red;
	 g = colors[index].green;
	 b = colors[index].blue;
	 val = (r << 16) | (g << 8) | b;
	 OUTREG(palreg + index * 4, val);
        }
        break;
     }
   }
   
   /* Enable gamma for Cursor if ARGB */
   if (pI830->CursorInfoRec && !pI830->SWCursor && pI830->cursorOn)
      pI830->CursorInfoRec->ShowCursor(pScrn);
}

/**
 * Set up the outputs according to what type of chip we are.
 *
 * Some outputs may not initialize, due to allocation failure or because a
 * controller chip isn't found.
 */
static void
I830SetupOutputs(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);

   /* everyone has at least a single analog output */
   i830_crt_init(pScrn);

   /* Set up integrated LVDS */
   if (IS_MOBILE(pI830) && !IS_I830(pI830))
      i830_lvds_init(pScrn);

   if (IS_I9XX(pI830)) {
      i830_sdvo_init(pScrn, SDVOB);
      i830_sdvo_init(pScrn, SDVOC);
   } else {
      i830_dvo_init(pScrn);
   }
   if (IS_I915GM(pI830) || IS_I945GM(pI830))
      i830_tv_init(pScrn);
}

static void 
I830PreInitDDC(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);

   if (!xf86LoadSubModule(pScrn, "ddc")) {
      pI830->ddc2 = FALSE;
   } else {
      xf86LoaderReqSymLists(I810ddcSymbols, NULL);
      pI830->ddc2 = TRUE;
   }

   /* DDC can use I2C bus */
   /* Load I2C if we have the code to use it */
   if (pI830->ddc2) {
      if (xf86LoadSubModule(pScrn, "i2c")) {
	 xf86LoaderReqSymLists(I810i2cSymbols, NULL);

	 I830SetupOutputs(pScrn);

	 pI830->ddc2 = TRUE;
      } else {
	 pI830->ddc2 = FALSE;
      }
   }
}

static void
PreInitCleanup(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);

   if (I830IsPrimary(pScrn)) {
      if (pI830->entityPrivate)
	 pI830->entityPrivate->pScrn_1 = NULL;
      if (pI830->LpRing)
         xfree(pI830->LpRing);
      pI830->LpRing = NULL;
      if (pI830->CursorMem)
         xfree(pI830->CursorMem);
      pI830->CursorMem = NULL;
      if (pI830->CursorMemARGB) 
         xfree(pI830->CursorMemARGB);
      pI830->CursorMemARGB = NULL;
      if (pI830->OverlayMem)
         xfree(pI830->OverlayMem);
      pI830->OverlayMem = NULL;
      if (pI830->overlayOn)
         xfree(pI830->overlayOn);
      pI830->overlayOn = NULL;
      if (pI830->used3D)
         xfree(pI830->used3D);
      pI830->used3D = NULL;
   } else {
      if (pI830->entityPrivate)
         pI830->entityPrivate->pScrn_2 = NULL;
   }
   if (pI830->swfSaved) {
      OUTREG(SWF0, pI830->saveSWF0);
      OUTREG(SWF4, pI830->saveSWF4);
   }
   if (pI830->MMIOBase)
      I830UnmapMMIO(pScrn);
   I830FreeRec(pScrn);
}

Bool
I830IsPrimary(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);

   if (xf86IsEntityShared(pScrn->entityList[0])) {
	if (pI830->init == 0) return TRUE;
	else return FALSE;
   }

   return TRUE;
}

#define HOTKEY_BIOS_SWITCH	0
#define HOTKEY_DRIVER_NOTIFY	1

/**
 * Controls the BIOS's behavior on hotkey switch.
 *
 * If the mode is HOTKEY_BIOS_SWITCH, the BIOS will be set to do a mode switch
 * on its own and update the state in the scratch register.
 * If the mode is HOTKEY_DRIVER_NOTIFY, the BIOS won't do a mode switch and
 * will just update the state to represent what it would have been switched to.
 */
static void
i830SetHotkeyControl(ScrnInfoPtr pScrn, int mode)
{
   I830Ptr pI830 = I830PTR(pScrn);
   CARD8 gr18;

   gr18 = pI830->readControl(pI830, GRX, 0x18);
   if (mode == HOTKEY_BIOS_SWITCH)
      gr18 &= ~HOTKEY_VBIOS_SWITCH_BLOCK;
   else
      gr18 |= HOTKEY_VBIOS_SWITCH_BLOCK;
   pI830->writeControl(pI830, GRX, 0x18, gr18);
}

#ifdef XF86DRI
static void 
I830ReduceMMSize(ScrnInfoPtr pScrn, unsigned long newSize,
		 const char *reason)
{
   I830Ptr pI830 = I830PTR(pScrn);

   newSize = ROUND_DOWN_TO(newSize, GTT_PAGE_SIZE);
   if (newSize / GTT_PAGE_SIZE > I830_MM_MINPAGES) {
      pI830->mmSize = newSize / 1024;
      xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		 "DRM memory manager aperture size is reduced to %d kiB\n"
		 "\t%s\n", pI830->mmSize, reason);
   } else {
      xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		 "DRM memory manager will be disabled\n\t%s\n", reason);
      pI830->mmSize = 0;
   }
}
#endif

static Bool
I830PreInit(ScrnInfoPtr pScrn, int flags)
{
   vgaHWPtr hwp;
   I830Ptr pI830;
   MessageType from = X_PROBED;
   rgb defaultWeight = { 0, 0, 0 };
   EntityInfoPtr pEnt;
   I830EntPtr pI830Ent = NULL;					
   int mem;
   int flags24;
   int i;
   char *s;
   pointer pVBEModule = NULL;
   Bool enable;
   const char *chipname;
   int mem_skip;
#ifdef XF86DRI
   unsigned long savedMMSize;
#endif

   if (pScrn->numEntities != 1)
      return FALSE;

   /* Load int10 module */
   if (!xf86LoadSubModule(pScrn, "int10"))
      return FALSE;
   xf86LoaderReqSymLists(I810int10Symbols, NULL);

   /* Load vbe module */
   if (!(pVBEModule = xf86LoadSubModule(pScrn, "vbe")))
      return FALSE;
   xf86LoaderReqSymLists(I810vbeSymbols, NULL);

   pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

   if (flags & PROBE_DETECT) {
      I830ProbeDDC(pScrn, pEnt->index);
      return TRUE;
   }

   /* The vgahw module should be loaded here when needed */
   if (!xf86LoadSubModule(pScrn, "vgahw"))
      return FALSE;
   xf86LoaderReqSymLists(I810vgahwSymbols, NULL);

   /* Allocate a vgaHWRec */
   if (!vgaHWGetHWRec(pScrn))
      return FALSE;

   /* Allocate driverPrivate */
   if (!I830GetRec(pScrn))
      return FALSE;

   pI830 = I830PTR(pScrn);
   pI830->SaveGeneration = -1;
   pI830->pEnt = pEnt;

   pI830->displayWidth = 640; /* default it */

   if (pI830->pEnt->location.type != BUS_PCI)
      return FALSE;

   pI830->PciInfo = xf86GetPciInfoForEntity(pI830->pEnt->index);
   pI830->PciTag = pciTag(pI830->PciInfo->bus, pI830->PciInfo->device,
			  pI830->PciInfo->func);

    /* Allocate an entity private if necessary */
    if (xf86IsEntityShared(pScrn->entityList[0])) {
	pI830Ent = xf86GetEntityPrivate(pScrn->entityList[0],
					I830EntityIndex)->ptr;
        pI830->entityPrivate = pI830Ent;
    } else 
        pI830->entityPrivate = NULL;

   if (xf86RegisterResources(pI830->pEnt->index, 0, ResNone)) {
      PreInitCleanup(pScrn);
      return FALSE;
   }

   if (xf86IsEntityShared(pScrn->entityList[0])) {
      if (xf86IsPrimInitDone(pScrn->entityList[0])) {
	 pI830->init = 1;

         if (!pI830Ent->pScrn_1) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
 		 "Failed to setup second head due to primary head failure.\n");
	    return FALSE;
         }
      } else {
         xf86SetPrimInitDone(pScrn->entityList[0]);
	 pI830->init = 0;
      }
   }

   if (xf86IsEntityShared(pScrn->entityList[0])) {
      if (!I830IsPrimary(pScrn)) {
         pI830Ent->pScrn_2 = pScrn;
      } else {
         pI830Ent->pScrn_1 = pScrn;
         pI830Ent->pScrn_2 = NULL;
      }
   }

   pScrn->racMemFlags = RAC_FB | RAC_COLORMAP;
   pScrn->monitor = pScrn->confScreen->monitor;
   pScrn->progClock = TRUE;
   pScrn->rgbBits = 8;

   flags24 = Support32bppFb | PreferConvert24to32 | SupportConvert24to32;

   if (!xf86SetDepthBpp(pScrn, 0, 0, 0, flags24))
      return FALSE;

   switch (pScrn->depth) {
   case 8:
   case 15:
   case 16:
   case 24:
      break;
   default:
      xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		 "Given depth (%d) is not supported by I830 driver\n",
		 pScrn->depth);
      return FALSE;
   }
   xf86PrintDepthBpp(pScrn);

   if (!xf86SetWeight(pScrn, defaultWeight, defaultWeight))
      return FALSE;
   if (!xf86SetDefaultVisual(pScrn, -1))
      return FALSE;

   hwp = VGAHWPTR(pScrn);
   pI830->cpp = pScrn->bitsPerPixel / 8;

   pI830->preinit = TRUE;

   /* Process the options */
   xf86CollectOptions(pScrn, NULL);
   if (!(pI830->Options = xalloc(sizeof(I830Options))))
      return FALSE;
   memcpy(pI830->Options, I830Options, sizeof(I830Options));
   xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, pI830->Options);

   /* We have to use PIO to probe, because we haven't mapped yet. */
   I830SetPIOAccess(pI830);

   switch (pI830->PciInfo->chipType) {
   case PCI_CHIP_I830_M:
      chipname = "830M";
      break;
   case PCI_CHIP_845_G:
      chipname = "845G";
      break;
   case PCI_CHIP_I855_GM:
      /* Check capid register to find the chipset variant */
      pI830->variant = (pciReadLong(pI830->PciTag, I85X_CAPID)
				>> I85X_VARIANT_SHIFT) & I85X_VARIANT_MASK;
      switch (pI830->variant) {
      case I855_GM:
	 chipname = "855GM";
	 break;
      case I855_GME:
	 chipname = "855GME";
	 break;
      case I852_GM:
	 chipname = "852GM";
	 break;
      case I852_GME:
	 chipname = "852GME";
	 break;
      default:
	 xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		    "Unknown 852GM/855GM variant: 0x%x)\n", pI830->variant);
	 chipname = "852GM/855GM (unknown variant)";
	 break;
      }
      break;
   case PCI_CHIP_I865_G:
      chipname = "865G";
      break;
   case PCI_CHIP_I915_G:
      chipname = "915G";
      break;
   case PCI_CHIP_E7221_G:
      chipname = "E7221 (i915)";
      break;
   case PCI_CHIP_I915_GM:
      chipname = "915GM";
      break;
   case PCI_CHIP_I945_G:
      chipname = "945G";
      break;
   case PCI_CHIP_I945_GM:
      chipname = "945GM";
      break;
   case PCI_CHIP_I965_G:
   case PCI_CHIP_I965_G_1:
      chipname = "965G";
      break;
   case PCI_CHIP_I965_Q:
      chipname = "965Q";
      break;
   case PCI_CHIP_I946_GZ:
      chipname = "946GZ";
      break;
   default:
      chipname = "unknown chipset";
      break;
   }
   xf86DrvMsg(pScrn->scrnIndex, X_INFO,
	      "Integrated Graphics Chipset: Intel(R) %s\n", chipname);

   /* Set the Chipset and ChipRev, allowing config file entries to override. */
   if (pI830->pEnt->device->chipset && *pI830->pEnt->device->chipset) {
      pScrn->chipset = pI830->pEnt->device->chipset;
      from = X_CONFIG;
   } else if (pI830->pEnt->device->chipID >= 0) {
      pScrn->chipset = (char *)xf86TokenToString(I830Chipsets,
						 pI830->pEnt->device->chipID);
      from = X_CONFIG;
      xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, "ChipID override: 0x%04X\n",
		 pI830->pEnt->device->chipID);
      pI830->PciInfo->chipType = pI830->pEnt->device->chipID;
   } else {
      from = X_PROBED;
      pScrn->chipset = (char *)xf86TokenToString(I830Chipsets,
						 pI830->PciInfo->chipType);
   }

   if (pI830->pEnt->device->chipRev >= 0) {
      xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, "ChipRev override: %d\n",
		 pI830->pEnt->device->chipRev);
   }

   xf86DrvMsg(pScrn->scrnIndex, from, "Chipset: \"%s\"\n",
	      (pScrn->chipset != NULL) ? pScrn->chipset : "Unknown i8xx");

   if (pI830->pEnt->device->MemBase != 0) {
      pI830->LinearAddr = pI830->pEnt->device->MemBase;
      from = X_CONFIG;
   } else {
      if (IS_I9XX(pI830)) {
	 pI830->LinearAddr = pI830->PciInfo->memBase[2] & 0xFF000000;
	 from = X_PROBED;
      } else if (pI830->PciInfo->memBase[1] != 0) {
	 /* XXX Check mask. */
	 pI830->LinearAddr = pI830->PciInfo->memBase[0] & 0xFF000000;
	 from = X_PROBED;
      } else {
	 xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		    "No valid FB address in PCI config space\n");
	 PreInitCleanup(pScrn);
	 return FALSE;
      }
   }

   xf86DrvMsg(pScrn->scrnIndex, from, "Linear framebuffer at 0x%lX\n",
	      (unsigned long)pI830->LinearAddr);

   if (pI830->pEnt->device->IOBase != 0) {
      pI830->MMIOAddr = pI830->pEnt->device->IOBase;
      from = X_CONFIG;
   } else {
      if (IS_I9XX(pI830)) {
	 pI830->MMIOAddr = pI830->PciInfo->memBase[0] & 0xFFF80000;
	 from = X_PROBED;
      } else if (pI830->PciInfo->memBase[1]) {
	 pI830->MMIOAddr = pI830->PciInfo->memBase[1] & 0xFFF80000;
	 from = X_PROBED;
      } else {
	 xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		    "No valid MMIO address in PCI config space\n");
	 PreInitCleanup(pScrn);
	 return FALSE;
      }
   }

   xf86DrvMsg(pScrn->scrnIndex, from, "IO registers at addr 0x%lX\n",
	      (unsigned long)pI830->MMIOAddr);

   /* Some of the probing needs MMIO access, so map it here. */
   I830MapMMIO(pScrn);

#if 1
   pI830->saveSWF0 = INREG(SWF0);
   pI830->saveSWF4 = INREG(SWF4);
   pI830->swfSaved = TRUE;

   /* Set "extended desktop" */
   OUTREG(SWF0, pI830->saveSWF0 | (1 << 21));

   /* Set "driver loaded",  "OS unknown", "APM 1.2" */
   OUTREG(SWF4, (pI830->saveSWF4 & ~((3 << 19) | (7 << 16))) |
		(1 << 23) | (2 << 16));
#endif

   if (IS_I830(pI830) || IS_845G(pI830)) {
      PCITAG bridge;
      CARD16 gmch_ctrl;

      bridge = pciTag(0, 0, 0);		/* This is always the host bridge */
      gmch_ctrl = pciReadWord(bridge, I830_GMCH_CTRL);
      if ((gmch_ctrl & I830_GMCH_MEM_MASK) == I830_GMCH_MEM_128M) {
	 pI830->FbMapSize = 0x8000000;
      } else {
	 pI830->FbMapSize = 0x4000000; /* 64MB - has this been tested ?? */
      }
   } else {
      if (IS_I9XX(pI830)) {
	 if (pI830->PciInfo->memBase[2] & 0x08000000)
	    pI830->FbMapSize = 0x8000000;	/* 128MB aperture */
	 else
	    pI830->FbMapSize = 0x10000000;	/* 256MB aperture */

   	 if (pI830->PciInfo->chipType == PCI_CHIP_E7221_G)
	    pI830->FbMapSize = 0x8000000;	/* 128MB aperture */
      } else
	 /* 128MB aperture for later chips */
	 pI830->FbMapSize = 0x8000000;
   }

   if (pI830->PciInfo->chipType == PCI_CHIP_E7221_G)
      pI830->num_pipes = 1;
   else
   if (IS_MOBILE(pI830) || IS_I9XX(pI830))
      pI830->num_pipes = 2;
   else
      pI830->num_pipes = 1;
   xf86DrvMsg(pScrn->scrnIndex, X_INFO, "%d display pipe%s available.\n",
	      pI830->num_pipes, pI830->num_pipes > 1 ? "s" : "");

   /*
    * Get the pre-allocated (stolen) memory size.
    */
    
   mem_skip = 0;
   
   /* On 965, it looks like the GATT table is inside the aperture? */
   if (IS_I965G(pI830))
      mem_skip = pI830->FbMapSize >> 10;
    
   pI830->StolenMemory.Size = I830DetectMemory(pScrn) - mem_skip;
   pI830->StolenMemory.Start = mem_skip;
   pI830->StolenMemory.End = pI830->StolenMemory.Size;

   /* Find the maximum amount of agpgart memory available. */
   if (I830IsPrimary(pScrn)) {
      mem = I830CheckAvailableMemory(pScrn);
      pI830->StolenOnly = FALSE;
   } else {
      /* videoRam isn't used on the second head, but faked */
      mem = pI830->entityPrivate->pScrn_1->videoRam;
      pI830->StolenOnly = TRUE;
   }

   if (mem <= 0) {
      if (pI830->StolenMemory.Size <= 0) {
	 /* Shouldn't happen. */
	 xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		 "/dev/agpgart is either not available, or no memory "
		 "is available\nfor allocation, "
		 "and no pre-allocated memory is available.\n");
	 PreInitCleanup(pScrn);
	 return FALSE;
      }
      xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		 "/dev/agpgart is either not available, or no memory "
		 "is available\nfor allocation.  "
		 "Using pre-allocated memory only.\n");
      mem = 0;
      pI830->StolenOnly = TRUE;
   }

   if (xf86ReturnOptValBool(pI830->Options, OPTION_NOACCEL, FALSE)) {
      pI830->noAccel = TRUE;
   }
   if (xf86ReturnOptValBool(pI830->Options, OPTION_SW_CURSOR, FALSE)) {
      pI830->SWCursor = TRUE;
   }

   pI830->directRenderingDisabled =
	!xf86ReturnOptValBool(pI830->Options, OPTION_DRI, TRUE);

#ifdef XF86DRI
   if (!pI830->directRenderingDisabled) {
      if (pI830->noAccel || pI830->SWCursor) {
	 xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, "DRI is disabled because it "
		    "needs HW cursor and 2D acceleration.\n");
	 pI830->directRenderingDisabled = TRUE;
      } else if (pScrn->depth != 16 && pScrn->depth != 24) {
	 xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, "DRI is disabled because it "
		    "runs only at depths 16 and 24.\n");
	 pI830->directRenderingDisabled = TRUE;
      }

      pI830->mmModeFlags = 0;

      if (!pI830->directRenderingDisabled) {
	 Bool tmp = FALSE;

	 if (IS_I965G(pI830))
	    pI830->mmModeFlags |= I830_KERNEL_TEX;

	 from = X_PROBED;
	 if (xf86GetOptValBool(pI830->Options, 
			       OPTION_INTELTEXPOOL, &tmp)) {
	    from = X_CONFIG;
	    if (tmp) {
	       pI830->mmModeFlags |= I830_KERNEL_TEX;
	    } else {
	       pI830->mmModeFlags &= ~I830_KERNEL_TEX;
	    }	       
	 }
	 if (from == X_CONFIG || 
	     (pI830->mmModeFlags & I830_KERNEL_TEX)) { 
	    xf86DrvMsg(pScrn->scrnIndex, from, 
		       "Will %stry to allocate texture pool "
		       "for old Mesa 3D driver.\n",
		       (pI830->mmModeFlags & I830_KERNEL_TEX) ? 
		       "" : "not ");
	 }
	 pI830->mmSize = I830_MM_MAXSIZE;
	 from = X_INFO;
	 if (xf86GetOptValInteger(pI830->Options, OPTION_INTELMMSIZE,
				  &(pI830->mmSize))) {
	    from = X_CONFIG;
	 }
	 xf86DrvMsg(pScrn->scrnIndex, from, 
		    "Will try to reserve %d kiB of AGP aperture space\n"
		    "\tfor the DRM memory manager.\n",
		    pI830->mmSize);
      }
   } 
   
#endif

   pI830->LinearAlloc = 0;
   if (xf86GetOptValULong(pI830->Options, OPTION_LINEARALLOC,
			    &(pI830->LinearAlloc))) {
      if (pI830->LinearAlloc > 0)
         xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, "Allocating %luKbytes of memory\n",
		 pI830->LinearAlloc);
      else 
         pI830->LinearAlloc = 0;
   }

   I830PreInitDDC(pScrn);

#if 0
   /*
    * This moves to generic RandR-based configuration code
    */
   if ((s = xf86GetOptValString(pI830->Options, OPTION_MONITOR_LAYOUT)) &&
      I830IsPrimary(pScrn)) {
      char *Mon1;
      char *Mon2;
      char *sub;
        
      Mon1 = strtok(s, ",");
      Mon2 = strtok(NULL, ",");

      if (Mon1) {
         sub = strtok(Mon1, "+");
         do {
            if (strcmp(sub, "NONE") == 0)
               pI830->MonType1 |= PIPE_NONE;
            else if (strcmp(sub, "CRT") == 0)
               pI830->MonType1 |= PIPE_CRT;
            else if (strcmp(sub, "TV") == 0)
               pI830->MonType1 |= PIPE_TV;
            else if (strcmp(sub, "DFP") == 0)
               pI830->MonType1 |= PIPE_DFP;
            else if (strcmp(sub, "LFP") == 0)
               pI830->MonType1 |= PIPE_LFP;
            else if (strcmp(sub, "Second") == 0)
               pI830->MonType1 |= PIPE_CRT2;
            else if (strcmp(sub, "TV2") == 0)
               pI830->MonType1 |= PIPE_TV2;
            else if (strcmp(sub, "DFP2") == 0)
               pI830->MonType1 |= PIPE_DFP2;
            else if (strcmp(sub, "LFP2") == 0)
               pI830->MonType1 |= PIPE_LFP2;
            else 
               xf86DrvMsg(pScrn->scrnIndex, X_WARNING, 
			       "Invalid Monitor type specified for Pipe A\n"); 

            sub = strtok(NULL, "+");
         } while (sub);
      }

      if (Mon2) {
         sub = strtok(Mon2, "+");
         do {
            if (strcmp(sub, "NONE") == 0)
               pI830->MonType2 |= PIPE_NONE;
            else if (strcmp(sub, "CRT") == 0)
               pI830->MonType2 |= PIPE_CRT;
            else if (strcmp(sub, "TV") == 0)
               pI830->MonType2 |= PIPE_TV;
            else if (strcmp(sub, "DFP") == 0)
               pI830->MonType2 |= PIPE_DFP;
            else if (strcmp(sub, "LFP") == 0)
               pI830->MonType2 |= PIPE_LFP;
            else if (strcmp(sub, "Second") == 0)
               pI830->MonType2 |= PIPE_CRT2;
            else if (strcmp(sub, "TV2") == 0)
               pI830->MonType2 |= PIPE_TV2;
            else if (strcmp(sub, "DFP2") == 0)
               pI830->MonType2 |= PIPE_DFP2;
            else if (strcmp(sub, "LFP2") == 0)
               pI830->MonType2 |= PIPE_LFP2;
            else 
               xf86DrvMsg(pScrn->scrnIndex, X_WARNING, 
			       "Invalid Monitor type specified for Pipe B\n"); 

               sub = strtok(NULL, "+");
            } while (sub);
         }
    
         if (pI830->num_pipes == 1 && pI830->MonType2 != PIPE_NONE) {
	    xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		    "Monitor 2 cannot be specified on single pipe devices\n");
            return FALSE;
         }

         if (pI830->MonType1 == PIPE_NONE && pI830->MonType2 == PIPE_NONE) {
	    xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		    "Monitor 1 and 2 cannot be type NONE\n");
            return FALSE;
      }

      if (pI830->MonType1 != PIPE_NONE)
	 pI830->pipe = 0;
      else
	 pI830->pipe = 1;

   } else if (I830IsPrimary(pScrn)) {
      /* Choose a default set of outputs to use based on what we've detected.
       *
       * Assume that SDVO outputs are flat panels for now.  It's just a name
       * at the moment, since we don't treat different SDVO outputs
       * differently.
       */
      for (i = 0; i < pI830->num_outputs; i++) {
	 if (pI830->output[i].type == I830_OUTPUT_LVDS)
	    pI830->MonType2 = PIPE_LFP;

	 if (pI830->output[i].type == I830_OUTPUT_SDVO ||
	     pI830->output[i].type == I830_OUTPUT_ANALOG)
	 {
	    int pipetype;

	    if (pI830->output[i].detect(pScrn, &pI830->output[i]) ==
		OUTPUT_STATUS_DISCONNECTED)
	    {
	       continue;
	    }

	    if (pI830->output[i].type == I830_OUTPUT_SDVO)
	       pipetype = PIPE_DFP;
	    else
	       pipetype = PIPE_CRT;

	    if (pI830->MonType1 == PIPE_NONE)
	       pI830->MonType1 |= pipetype;
	    else if (pI830->MonType2 == PIPE_NONE)
	       pI830->MonType2 |= pipetype;
	 }
      }

      /* And, if we haven't found anything (including CRT through DDC), assume
       * that there's a CRT and that the user has set up some appropriate modes
       * or something.
       */
      if (pI830->MonType1 == PIPE_NONE && pI830->MonType2 == PIPE_NONE)
	 pI830->MonType1 |= PIPE_CRT;

      if (pI830->MonType1 != PIPE_NONE)
	 pI830->pipe = 0;
      else
	 pI830->pipe = 1;

      if (pI830->MonType1 != 0 && pI830->MonType2 != 0) {
         xf86DrvMsg(pScrn->scrnIndex, X_INFO, 
 		    "Enabling clone mode by default\n");
	 pI830->Clone = TRUE;
      }
   } else {
      I830Ptr pI8301 = I830PTR(pI830->entityPrivate->pScrn_1);
      pI830->pipe = !pI8301->pipe;
      pI830->MonType1 = pI8301->MonType1;
      pI830->MonType2 = pI8301->MonType2;
   }
#endif

   if (xf86ReturnOptValBool(pI830->Options, OPTION_CLONE, FALSE)) {
      if (pI830->num_pipes == 1) {
         xf86DrvMsg(pScrn->scrnIndex, X_ERROR, 
 		 "Can't enable Clone Mode because this is a single pipe device\n");
         PreInitCleanup(pScrn);
         return FALSE;
      }
      if (pI830->entityPrivate) {
         xf86DrvMsg(pScrn->scrnIndex, X_ERROR, 
 		 "Can't enable Clone Mode because second head is configured\n");
         PreInitCleanup(pScrn);
         return FALSE;
      }
      xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Enabling Clone Mode\n");
      pI830->Clone = TRUE;
   }


   /* Perform the pipe assignment of outputs. This is a kludge until
    * we have better configuration support in the generic RandR code
    */
   for (i = 0; i < pI830->num_outputs; i++) {
      pI830->output[i].enabled = FALSE;

      switch (pI830->output[i].type) {
      case I830_OUTPUT_LVDS:
	 /* LVDS must live on pipe B for two-pipe devices */
	 pI830->output[i].pipe = pI830->num_pipes - 1;
	 pI830->output[i].enabled = TRUE;
	 break;
      case I830_OUTPUT_ANALOG:
      case I830_OUTPUT_DVO:
      case I830_OUTPUT_SDVO:
	 if (pI830->output[i].detect(pScrn, &pI830->output[i]) !=
	     OUTPUT_STATUS_DISCONNECTED) {
	    if (!i830PipeInUse(pScrn, 0)) {
	       pI830->output[i].pipe = 0;
	       pI830->output[i].enabled = TRUE;
	    } else if (!i830PipeInUse(pScrn, 1)) {
	       pI830->output[i].pipe = 1;
	       pI830->output[i].enabled = TRUE;
	    }
	 }
	 break;
      case I830_OUTPUT_TVOUT:
         if (!i830PipeInUse(pScrn, 0)) {
	    pI830->output[i].pipe = 0;
	    pI830->output[i].enabled = TRUE;
	 }
	 break;
      default:
	 xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Unhandled output type\n");
	 break;
      }
   }

   for (i = 0; i < pI830->num_pipes; i++) {
      pI830->pipes[i].enabled = i830PipeInUse(pScrn, i);
   }

#if 0
   pI830->CloneRefresh = 60; /* default to 60Hz */
   if (xf86GetOptValInteger(pI830->Options, OPTION_CLONE_REFRESH,
			    &(pI830->CloneRefresh))) {
      xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, "Clone Monitor Refresh Rate %d\n",
		 pI830->CloneRefresh);
   }

   /* See above i830refreshes on why 120Hz is commented out */
   if (pI830->CloneRefresh < 60 || pI830->CloneRefresh > 85 /* 120 */) {
      xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Bad Clone Refresh Rate\n");
      PreInitCleanup(pScrn);
      return FALSE;
   }

   if ((pI830->entityPrivate && I830IsPrimary(pScrn)) || pI830->Clone) {
      if (pI830->MonType1 == PIPE_NONE || pI830->MonType2 == PIPE_NONE) {
         xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Monitor 1 or Monitor 2 "
	 		"cannot be type NONE in DualHead or Clone setup.\n");
         PreInitCleanup(pScrn);
         return FALSE;
      }
   }
#endif

   pI830->rotation = RR_Rotate_0;
   if ((s = xf86GetOptValString(pI830->Options, OPTION_ROTATE))) {
      pI830->InitialRotation = 0;
      if(!xf86NameCmp(s, "CW") || !xf86NameCmp(s, "270"))
         pI830->InitialRotation = 270;
      if(!xf86NameCmp(s, "CCW") || !xf86NameCmp(s, "90"))
         pI830->InitialRotation = 90;
      if(!xf86NameCmp(s, "180"))
         pI830->InitialRotation = 180;
   }

   /*
    * Let's setup the mobile systems to check the lid status
    */
   if (IS_MOBILE(pI830)) {
      pI830->checkDevices = TRUE;

      if (!xf86ReturnOptValBool(pI830->Options, OPTION_CHECKDEVICES, TRUE)) {
         pI830->checkDevices = FALSE;
         xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Monitoring connected displays disabled\n");
      } else
      if (pI830->entityPrivate && !I830IsPrimary(pScrn) &&
          !I830PTR(pI830->entityPrivate->pScrn_1)->checkDevices) {
         /* If checklid is off, on the primary head, then 
          * turn it off on the secondary*/
         xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Monitoring connected displays disabled\n");
         pI830->checkDevices = FALSE;
      } else
         xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Monitoring connected displays enabled\n");
   } else
      pI830->checkDevices = FALSE;

   /*
    * The "VideoRam" config file parameter specifies the total amount of
    * memory that will be used/allocated.  When agpgart support isn't
    * available (StolenOnly == TRUE), this is limited to the amount of
    * pre-allocated ("stolen") memory.
    */

   /*
    * Default to I830_DEFAULT_VIDEOMEM_2D (8192KB) for 2D-only,
    * or I830_DEFAULT_VIDEOMEM_3D (32768KB) for 3D.  If the stolen memory
    * amount is higher, default to it rounded up to the nearest MB.  This
    * guarantees that by default there will be at least some run-time
    * space for things that need a physical address.
    * But, we double the amounts when dual head is enabled, and therefore
    * for 2D-only we use 16384KB, and 3D we use 65536KB. The VideoRAM 
    * for the second head is never used, as the primary head does the 
    * allocation.
    */
   if (!pI830->pEnt->device->videoRam) {
      from = X_DEFAULT;
#ifdef XF86DRI
      if (!pI830->directRenderingDisabled)
	 pScrn->videoRam = I830_DEFAULT_VIDEOMEM_3D;
      else
#endif
	 pScrn->videoRam = I830_DEFAULT_VIDEOMEM_2D;

      if (xf86IsEntityShared(pScrn->entityList[0])) {
         if (I830IsPrimary(pScrn))
            pScrn->videoRam += I830_DEFAULT_VIDEOMEM_2D;
      else
            pScrn->videoRam = I830_MAXIMUM_VBIOS_MEM;
      } 

      if (pI830->StolenMemory.Size / 1024 > pScrn->videoRam)
	 pScrn->videoRam = ROUND_TO(pI830->StolenMemory.Size / 1024, 1024);
   } else {
      from = X_CONFIG;
      pScrn->videoRam = pI830->pEnt->device->videoRam;
   }

   /* Make sure it's on a page boundary */
   if (pScrn->videoRam & (GTT_PAGE_SIZE - 1)) {
      xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		    "VideoRAM reduced to %d kByte "
		    "(page aligned - was %d)\n", pScrn->videoRam & ~(GTT_PAGE_SIZE - 1), pScrn->videoRam);
      pScrn->videoRam &= ~(GTT_PAGE_SIZE - 1);
   }

   DPRINTF(PFX,
	   "Available memory: %dk\n"
	   "Requested memory: %dk\n", mem, pScrn->videoRam);


   if (mem + (pI830->StolenMemory.Size / 1024) < pScrn->videoRam) {
      pScrn->videoRam = mem + (pI830->StolenMemory.Size / 1024);
      from = X_PROBED;
      if (mem + (pI830->StolenMemory.Size / 1024) <
	  pI830->pEnt->device->videoRam) {
	 xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		    "VideoRAM reduced to %d kByte "
		    "(limited to available sysmem)\n", pScrn->videoRam);
      }
   }

   if (pScrn->videoRam > pI830->FbMapSize / 1024) {
      pScrn->videoRam = pI830->FbMapSize / 1024;
      if (pI830->FbMapSize / 1024 < pI830->pEnt->device->videoRam)
	 xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		    "VideoRam reduced to %d kByte (limited to aperture size)\n",
		    pScrn->videoRam);
   }

   xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
	      "Pre-allocated VideoRAM: %ld kByte\n",
	      pI830->StolenMemory.Size / 1024);
   xf86DrvMsg(pScrn->scrnIndex, from, "VideoRAM: %d kByte\n",
	      pScrn->videoRam);

   pI830->TotalVideoRam = KB(pScrn->videoRam);

   /*
    * If the requested videoRam amount is less than the stolen memory size,
    * reduce the stolen memory size accordingly.
    */
   if (pI830->StolenMemory.Size > pI830->TotalVideoRam) {
      pI830->StolenMemory.Size = pI830->TotalVideoRam;
      pI830->StolenMemory.End = pI830->TotalVideoRam;
   }

   if (xf86GetOptValInteger(pI830->Options, OPTION_CACHE_LINES,
			    &(pI830->CacheLines))) {
      xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, "Requested %d cache lines\n",
		 pI830->CacheLines);
   } else {
      pI830->CacheLines = -1;
   }

   pI830->XvDisabled =
	!xf86ReturnOptValBool(pI830->Options, OPTION_XVIDEO, TRUE);

#ifdef I830_XV
   if (xf86GetOptValInteger(pI830->Options, OPTION_VIDEO_KEY,
			    &(pI830->colorKey))) {
      from = X_CONFIG;
   } else if (xf86GetOptValInteger(pI830->Options, OPTION_COLOR_KEY,
			    &(pI830->colorKey))) {
      from = X_CONFIG;
   } else {
      pI830->colorKey = (1 << pScrn->offset.red) |
			(1 << pScrn->offset.green) |
			(((pScrn->mask.blue >> pScrn->offset.blue) - 1) <<
			 pScrn->offset.blue);
      from = X_DEFAULT;
   }
   xf86DrvMsg(pScrn->scrnIndex, from, "video overlay key set to 0x%x\n",
	      pI830->colorKey);
#endif

   pI830->allowPageFlip = FALSE;
   enable = xf86ReturnOptValBool(pI830->Options, OPTION_PAGEFLIP, FALSE);
#ifdef XF86DRI
   if (!pI830->directRenderingDisabled) {
      pI830->allowPageFlip = enable;
      xf86DrvMsg(pScrn->scrnIndex, X_CONFIG, "page flipping %s\n",
		 enable ? "enabled" : "disabled");
   }
#endif

   /*
    * If the driver can do gamma correction, it should call xf86SetGamma() here.
    */

   {
      Gamma zeros = { 0.0, 0.0, 0.0 };

      if (!xf86SetGamma(pScrn, zeros)) {
         PreInitCleanup(pScrn);
	 return FALSE;
      }
   }

#if 0
   if (xf86IsEntityShared(pScrn->entityList[0])) {
      if (!I830IsPrimary(pScrn)) {
	 /* This could be made to work with a little more fiddling */
	 pI830->directRenderingDisabled = TRUE;

         xf86DrvMsg(pScrn->scrnIndex, from, "Secondary head is using Pipe %s\n",
		pI830->pipe ? "B" : "A");
      } else {
         xf86DrvMsg(pScrn->scrnIndex, from, "Primary head is using Pipe %s\n",
		pI830->pipe ? "B" : "A");
      }
   } else {
      xf86DrvMsg(pScrn->scrnIndex, from, "Display is using Pipe %s\n",
		pI830->pipe ? "B" : "A");
   }
#endif

   /* Alloc our pointers for the primary head */
   if (I830IsPrimary(pScrn)) {
      pI830->LpRing = xalloc(sizeof(I830RingBuffer));
      pI830->CursorMem = xalloc(sizeof(I830MemRange));
      pI830->CursorMemARGB = xalloc(sizeof(I830MemRange));
      pI830->OverlayMem = xalloc(sizeof(I830MemRange));
      pI830->overlayOn = xalloc(sizeof(Bool));
      pI830->used3D = xalloc(sizeof(int));
      if (!pI830->LpRing || !pI830->CursorMem || !pI830->CursorMemARGB ||
          !pI830->OverlayMem || !pI830->overlayOn || !pI830->used3D) {
         xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		 "Could not allocate primary data structures.\n");
         PreInitCleanup(pScrn);
         return FALSE;
      }
      *pI830->overlayOn = FALSE;
      if (pI830->entityPrivate)
         pI830->entityPrivate->XvInUse = -1;
   }

   /* Check if the HW cursor needs physical address. */
   if (IS_MOBILE(pI830) || IS_I9XX(pI830))
      pI830->CursorNeedsPhysical = TRUE;
   else
      pI830->CursorNeedsPhysical = FALSE;

   if (IS_I965G(pI830))
      pI830->CursorNeedsPhysical = FALSE;

   /* Force ring buffer to be in low memory for all chipsets */
   pI830->NeedRingBufferLow = TRUE;

   /*
    * XXX If we knew the pre-initialised GTT format for certain, we could
    * probably figure out the physical address even in the StolenOnly case.
    */
   if (!I830IsPrimary(pScrn)) {
        I830Ptr pI8301 = I830PTR(pI830->entityPrivate->pScrn_1);
	if (!pI8301->SWCursor) {
          xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
		 "Using HW Cursor because it's enabled on primary head.\n");
          pI830->SWCursor = FALSE;
        }
   } else 
   if (pI830->StolenOnly && pI830->CursorNeedsPhysical && !pI830->SWCursor) {
      xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
		 "HW Cursor disabled because it needs agpgart memory.\n");
      pI830->SWCursor = TRUE;
   }

   /*
    * Reduce the maximum videoram available for video modes by the ring buffer,
    * minimum scratch space and HW cursor amounts.
    */
   if (!pI830->SWCursor) {
      pScrn->videoRam -= (HWCURSOR_SIZE / 1024);
      pScrn->videoRam -= (HWCURSOR_SIZE_ARGB / 1024);
   }
   if (!pI830->XvDisabled)
      pScrn->videoRam -= (OVERLAY_SIZE / 1024);
   if (!pI830->noAccel) {
      pScrn->videoRam -= (PRIMARY_RINGBUFFER_SIZE / 1024);
      pScrn->videoRam -= (MIN_SCRATCH_BUFFER_SIZE / 1024);
   }

   xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
	      "Maximum frambuffer space: %d kByte\n", pScrn->videoRam);

   if (!I830RandRPreInit (pScrn))
   {
      xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No valid modes.\n");
      PreInitCleanup(pScrn);
      return FALSE;
   }	

   if (pScrn->modes == NULL) {
      xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No modes.\n");
      PreInitCleanup(pScrn);
      return FALSE;
   }

   /*
    * Fix up modes to make hblank start at hsync start.
    * I don't know why the xf86 code mangles this...
    */
    {
	DisplayModePtr	p;

	for (p = pScrn->modes; p;) {
	    xf86DrvMsg (pScrn->scrnIndex,
			X_INFO, "move blank start from %d to %d\n",
			p->CrtcHBlankStart, p->CrtcHDisplay);
	    p->CrtcHBlankStart = p->CrtcHDisplay;
	    p = p->next;
	    if (p == pScrn->modes)
		break;
	}
    }
   
   pScrn->currentMode = pScrn->modes;

#ifndef USE_PITCHES
#define USE_PITCHES 1
#endif
   pI830->disableTiling = FALSE;

   /*
    * If DRI is potentially usable, check if there is enough memory available
    * for it, and if there's also enough to allow tiling to be enabled.
    */

#if defined(XF86DRI)
   if (!I830CheckDRIAvailable(pScrn)) {
      pI830->directRenderingDisabled = TRUE;
      pI830->mmSize = 0;
   } else if (pScrn->videoRam > pI830->FbMapSize / 1024 - pI830->mmSize) {
      I830ReduceMMSize(pScrn, pI830->FbMapSize - KB(pScrn->videoRam), 
		       "to make room for video memory");
   }

   if (I830IsPrimary(pScrn) && !pI830->directRenderingDisabled) {
      int savedDisplayWidth = pScrn->displayWidth;
      int memNeeded = 0;
      /* Good pitches to allow tiling.  Don't care about pitches < 1024. */
      static const int pitches[] = {
/*
	 128 * 2,
	 128 * 4,
*/
	 128 * 8,
	 128 * 16,
	 128 * 32,
	 128 * 64,
	 0
      };

#ifdef I830_XV
      /*
       * Set this so that the overlay allocation is factored in when
       * appropriate.
       */
      pI830->XvEnabled = !pI830->XvDisabled;
#endif

      for (i = 0; pitches[i] != 0; i++) {
#if USE_PITCHES
	 if (pitches[i] >= pScrn->displayWidth) {
	    pScrn->displayWidth = pitches[i];
	    break;
	 }
#else
	 if (pitches[i] == pScrn->displayWidth)
	    break;
#endif
      }

      /*
       * If the displayWidth is a tilable pitch, test if there's enough
       * memory available to enable tiling.
       */
      savedMMSize = pI830->mmSize;
      if (pScrn->displayWidth == pitches[i]) {
      retry_dryrun:
	 I830ResetAllocations(pScrn, 0);
	 if (I830Allocate2DMemory(pScrn, ALLOCATE_DRY_RUN | ALLOC_INITIAL) &&
	     I830Allocate3DMemory(pScrn, ALLOCATE_DRY_RUN)) {
	    memNeeded = I830GetExcessMemoryAllocations(pScrn);
	    if (memNeeded > 0 || pI830->MemoryAperture.Size < 0) {
	       if (memNeeded > 0) {
		  xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			     "%d kBytes additional video memory is "
			     "required to\n\tenable tiling mode for DRI.\n",
			     (memNeeded + 1023) / 1024);
	       }
	       if (pI830->MemoryAperture.Size < 0) {		  
		  if (KB(pI830->mmSize) > I830_MM_MINPAGES * GTT_PAGE_SIZE) {
		     I830ReduceMMSize(pScrn, I830_MM_MINPAGES * GTT_PAGE_SIZE,
				      "to make room in AGP aperture for tiling.");
		     goto retry_dryrun;
		  }

		  xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			     "Allocation with DRI tiling enabled would "
			     "exceed the\n"
			     "\tmemory aperture size (%ld kB) by %ld kB.\n"
			     "\tReduce VideoRam amount to avoid this!\n",
			     pI830->FbMapSize / 1024,
			     -pI830->MemoryAperture.Size / 1024);
	       }
	       pScrn->displayWidth = savedDisplayWidth;
	       pI830->allowPageFlip = FALSE;
	    } else if (pScrn->displayWidth != savedDisplayWidth) {
	       xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			  "Increasing the scanline pitch to allow tiling mode "
			  "(%d -> %d).\n",
			  savedDisplayWidth, pScrn->displayWidth);
	    }
	 } else {
	    memNeeded = 0;
	    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		       "Unexpected dry run allocation failure (1).\n");
	 }
      }
      if (memNeeded > 0 || pI830->MemoryAperture.Size < 0) {
	 /*
	  * Tiling can't be enabled.  Check if there's enough memory for DRI
	  * without tiling.
	  */
	 pI830->mmSize = savedMMSize;
	 pI830->disableTiling = TRUE;
      retry_dryrun2:
	 I830ResetAllocations(pScrn, 0);
	 if (I830Allocate2DMemory(pScrn, ALLOCATE_DRY_RUN | ALLOC_INITIAL) &&
	     I830Allocate3DMemory(pScrn, ALLOCATE_DRY_RUN | ALLOC_NO_TILING)) {
	    memNeeded = I830GetExcessMemoryAllocations(pScrn);
	    if (memNeeded > 0 || pI830->MemoryAperture.Size < 0) {
	       if (memNeeded > 0) {
		  xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			     "%d kBytes additional video memory is required "
			     "to enable DRI.\n",
			     (memNeeded + 1023) / 1024);
	       }
	       if (pI830->MemoryAperture.Size < 0) {
		  if (KB(pI830->mmSize) > I830_MM_MINPAGES * GTT_PAGE_SIZE) {
		     I830ReduceMMSize(pScrn, I830_MM_MINPAGES * GTT_PAGE_SIZE,
				      "to save AGP aperture space for video memory.");
		     goto retry_dryrun2;
		  }
		  xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			     "Allocation with DRI enabled would "
			     "exceed the\n"
			     "\tmemory aperture size (%ld kB) by %ld kB.\n"
			     "\tReduce VideoRam amount to avoid this!\n",
			     pI830->FbMapSize / 1024,
			     -pI830->MemoryAperture.Size / 1024);
	       }
	       pI830->mmSize = 0;
	       pI830->directRenderingDisabled = TRUE;
	       xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Disabling DRI.\n");
	    }
	 } else {
	    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		       "Unexpected dry run allocation failure (2).\n");
	 }
      }
   } else
#endif
      pI830->disableTiling = TRUE; /* no DRI - so disableTiling */

   if (pScrn->displayWidth * pI830->cpp > 8192) {
      xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Cannot support frame buffer stride > 8K >  DRI.\n");
      pI830->disableTiling = TRUE;
   }

   if (pScrn->virtualY > 2048) {
      xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Cannot support > 2048 vertical lines. disabling acceleration.\n");
      pI830->noAccel = TRUE;
   }

   pI830->displayWidth = pScrn->displayWidth;

   I830PrintModes(pScrn);

   /* Don't need MMIO access anymore. */
   if (pI830->swfSaved) {
      OUTREG(SWF0, pI830->saveSWF0);
      OUTREG(SWF4, pI830->saveSWF4);
   }

   /* Set display resolution */
   xf86SetDpi(pScrn, 0, 0);

   /* Load the required sub modules */
   if (!xf86LoadSubModule(pScrn, "fb")) {
      PreInitCleanup(pScrn);
      return FALSE;
   }

   xf86LoaderReqSymLists(I810fbSymbols, NULL);

   if (!pI830->noAccel) {
      if (!xf86LoadSubModule(pScrn, "xaa")) {
	 PreInitCleanup(pScrn);
	 return FALSE;
      }
      xf86LoaderReqSymLists(I810xaaSymbols, NULL);
   }

   if (!pI830->SWCursor) {
      if (!xf86LoadSubModule(pScrn, "ramdac")) {
	 PreInitCleanup(pScrn);
	 return FALSE;
      }
      xf86LoaderReqSymLists(I810ramdacSymbols, NULL);
   }

   I830UnmapMMIO(pScrn);

   /*  We won't be using the VGA access after the probe. */
   I830SetMMIOAccess(pI830);
   xf86SetOperatingState(resVgaIo, pI830->pEnt->index, ResUnusedOpr);
   xf86SetOperatingState(resVgaMem, pI830->pEnt->index, ResDisableOpr);

#if 0
   if (I830IsPrimary(pScrn)) {
      vbeFree(pI830->pVbe);
   }
   pI830->pVbe = NULL;
#endif

#if defined(XF86DRI)
   /* Load the dri module if requested. */
   if (xf86ReturnOptValBool(pI830->Options, OPTION_DRI, FALSE) &&
       !pI830->directRenderingDisabled) {
      if (xf86LoadSubModule(pScrn, "dri")) {
	 xf86LoaderReqSymLists(I810driSymbols, I810drmSymbols, NULL);
      }
   }
#endif

   /* rotation requires the newer libshadow */
   if (I830IsPrimary(pScrn)) {
      int errmaj, errmin;
      pI830->shadowReq.majorversion = 1;
      pI830->shadowReq.minorversion = 1;

      if (!LoadSubModule(pScrn->module, "shadow", NULL, NULL, NULL,
			       &pI830->shadowReq, &errmaj, &errmin)) {
         pI830->shadowReq.minorversion = 0;
         if (!LoadSubModule(pScrn->module, "shadow", NULL, NULL, NULL,
			       &pI830->shadowReq, &errmaj, &errmin)) {
            LoaderErrorMsg(NULL, "shadow", errmaj, errmin);
	    return FALSE;
         }
      }
   } else {
      I830Ptr pI8301 = I830PTR(pI830->entityPrivate->pScrn_1);
      pI830->shadowReq.majorversion = pI8301->shadowReq.majorversion;
      pI830->shadowReq.minorversion = pI8301->shadowReq.minorversion;
      pI830->shadowReq.patchlevel = pI8301->shadowReq.patchlevel;
   }
   xf86LoaderReqSymLists(I810shadowSymbols, NULL);

   pI830->preinit = FALSE;

   return TRUE;
}

/*
 * As the name says.  Check that the initial state is reasonable.
 * If any unrecoverable problems are found, bail out here.
 */
static Bool
CheckInheritedState(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);
   int errors = 0, fatal = 0;
   unsigned long temp, head, tail;

   if (!I830IsPrimary(pScrn)) return TRUE;

   /* Check first for page table errors */
   temp = INREG(PGE_ERR);
   if (temp != 0) {
      xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "PGTBL_ER is 0x%08lx\n", temp);
      errors++;
   }
   temp = INREG(PGETBL_CTL);
   if (!(temp & 1)) {
      xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		 "PGTBL_CTL (0x%08lx) indicates GTT is disabled\n", temp);
      errors++;
   }
   temp = INREG(LP_RING + RING_LEN);
   if (temp & 1) {
      xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		 "PRB0_CTL (0x%08lx) indicates ring buffer enabled\n", temp);
      errors++;
   }
   head = INREG(LP_RING + RING_HEAD);
   tail = INREG(LP_RING + RING_TAIL);
   if ((tail & I830_TAIL_MASK) != (head & I830_HEAD_MASK)) {
      xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		 "PRB0_HEAD (0x%08lx) and PRB0_TAIL (0x%08lx) indicate "
		 "ring buffer not flushed\n", head, tail);
      errors++;
   }

#if 0
   if (errors) {
      if (IS_I965G(pI830))
         I965PrintErrorState(pScrn);
      else
         I830PrintErrorState(pScrn);
   }
#endif

   if (fatal)
      FatalError("CheckInheritedState: can't recover from the above\n");

   return (errors != 0);
}

/*
 * Reset registers that it doesn't make sense to save/restore to a sane state.
 * This is basically the ring buffer and fence registers.  Restoring these
 * doesn't make sense without restoring GTT mappings.  This is something that
 * whoever gets control next should do.
 */
static void
ResetState(ScrnInfoPtr pScrn, Bool flush)
{
   I830Ptr pI830 = I830PTR(pScrn);
   int i;
   unsigned long temp;

   DPRINTF(PFX, "ResetState: flush is %s\n", BOOLTOSTRING(flush));

   if (!I830IsPrimary(pScrn)) return;

   if (pI830->entityPrivate)
      pI830->entityPrivate->RingRunning = 0;

   /* Reset the fence registers to 0 */
   if (IS_I965G(pI830)) {
      for (i = 0; i < FENCE_NEW_NR; i++) {
	 OUTREG(FENCE_NEW + i * 8, 0);
	 OUTREG(FENCE_NEW + 4 + i * 8, 0);
      }
   } else {
      for (i = 0; i < FENCE_NR; i++)
         OUTREG(FENCE + i * 4, 0);
   }

   /* Flush the ring buffer (if enabled), then disable it. */
   if (pI830->AccelInfoRec != NULL && flush) {
      temp = INREG(LP_RING + RING_LEN);
      if (temp & 1) {
	 I830RefreshRing(pScrn);
	 I830Sync(pScrn);
	 DO_RING_IDLE();
      }
   }
   OUTREG(LP_RING + RING_LEN, 0);
   OUTREG(LP_RING + RING_HEAD, 0);
   OUTREG(LP_RING + RING_TAIL, 0);
   OUTREG(LP_RING + RING_START, 0);
  
   if (pI830->CursorInfoRec && pI830->CursorInfoRec->HideCursor)
      pI830->CursorInfoRec->HideCursor(pScrn);
}

static void
SetFenceRegs(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);
   int i;

   DPRINTF(PFX, "SetFenceRegs\n");

   if (!I830IsPrimary(pScrn)) return;

   if (IS_I965G(pI830)) {
      for (i = 0; i < FENCE_NEW_NR; i++) {
         OUTREG(FENCE_NEW + i * 8, pI830->ModeReg.Fence[i]);
         OUTREG(FENCE_NEW + 4 + i * 8, pI830->ModeReg.Fence[i+FENCE_NEW_NR]);
         if (I810_DEBUG & DEBUG_VERBOSE_VGA) {
	    ErrorF("Fence Start Register : %x\n", pI830->ModeReg.Fence[i]);
	    ErrorF("Fence End Register : %x\n", pI830->ModeReg.Fence[i+FENCE_NEW_NR]);
         }
      }
   } else {
      for (i = 0; i < FENCE_NR; i++) {
         OUTREG(FENCE + i * 4, pI830->ModeReg.Fence[i]);
         if (I810_DEBUG & DEBUG_VERBOSE_VGA)
	    ErrorF("Fence Register : %x\n", pI830->ModeReg.Fence[i]);
      }
   }
}

static void
SetRingRegs(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);
   unsigned int itemp;

   DPRINTF(PFX, "SetRingRegs\n");

   if (pI830->noAccel)
      return;

   if (!I830IsPrimary(pScrn)) return;

   if (pI830->entityPrivate)
      pI830->entityPrivate->RingRunning = 1;

   OUTREG(LP_RING + RING_LEN, 0);
   OUTREG(LP_RING + RING_TAIL, 0);
   OUTREG(LP_RING + RING_HEAD, 0);

   if ((long)(pI830->LpRing->mem.Start & I830_RING_START_MASK) !=
       pI830->LpRing->mem.Start) {
      xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		 "I830SetRingRegs: Ring buffer start (%lx) violates its "
		 "mask (%x)\n", pI830->LpRing->mem.Start, I830_RING_START_MASK);
   }
   /* Don't care about the old value.  Reserved bits must be zero anyway. */
   itemp = pI830->LpRing->mem.Start & I830_RING_START_MASK;
   OUTREG(LP_RING + RING_START, itemp);

   if (((pI830->LpRing->mem.Size - 4096) & I830_RING_NR_PAGES) !=
       pI830->LpRing->mem.Size - 4096) {
      xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		 "I830SetRingRegs: Ring buffer size - 4096 (%lx) violates its "
		 "mask (%x)\n", pI830->LpRing->mem.Size - 4096,
		 I830_RING_NR_PAGES);
   }
   /* Don't care about the old value.  Reserved bits must be zero anyway. */
   itemp = (pI830->LpRing->mem.Size - 4096) & I830_RING_NR_PAGES;
   itemp |= (RING_NO_REPORT | RING_VALID);
   OUTREG(LP_RING + RING_LEN, itemp);
   I830RefreshRing(pScrn);
}

/*
 * This should be called everytime the X server gains control of the screen,
 * before any video modes are programmed (ScreenInit, EnterVT).
 */
static void
SetHWOperatingState(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);

   DPRINTF(PFX, "SetHWOperatingState\n");

   if (!pI830->noAccel)
      SetRingRegs(pScrn);
   SetFenceRegs(pScrn);
   if (!pI830->SWCursor)
      I830InitHWCursor(pScrn);
}

static Bool
SaveHWState(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);
   vgaHWPtr hwp = VGAHWPTR(pScrn);
   vgaRegPtr vgaReg = &hwp->SavedReg;
   CARD32 temp;
   int i;

   /*
    * Print out the PIPEACONF and PIPEBCONF registers.
    */
   temp = INREG(PIPEACONF);
   xf86DrvMsg(pScrn->scrnIndex, X_INFO, "PIPEACONF is 0x%08lx\n", 
	      (unsigned long) temp);
   if (pI830->num_pipes == 2) {
      temp = INREG(PIPEBCONF);
      xf86DrvMsg(pScrn->scrnIndex, X_INFO, "PIPEBCONF is 0x%08lx\n", 
		 (unsigned long) temp);
   }

   i830TakeRegSnapshot(pScrn);

   /* Save video mode information for native mode-setting. */
   pI830->saveDSPACNTR = INREG(DSPACNTR);
   pI830->savePIPEACONF = INREG(PIPEACONF);
   pI830->savePIPEASRC = INREG(PIPEASRC);
   pI830->saveFPA0 = INREG(FPA0);
   pI830->saveFPA1 = INREG(FPA1);
   pI830->saveDPLL_A = INREG(DPLL_A);
   if (IS_I965G(pI830))
      pI830->saveDPLL_A_MD = INREG(DPLL_A_MD);
   pI830->saveHTOTAL_A = INREG(HTOTAL_A);
   pI830->saveHBLANK_A = INREG(HBLANK_A);
   pI830->saveHSYNC_A = INREG(HSYNC_A);
   pI830->saveVTOTAL_A = INREG(VTOTAL_A);
   pI830->saveVBLANK_A = INREG(VBLANK_A);
   pI830->saveVSYNC_A = INREG(VSYNC_A);
   pI830->saveDSPASTRIDE = INREG(DSPASTRIDE);
   pI830->saveDSPASIZE = INREG(DSPASIZE);
   pI830->saveDSPAPOS = INREG(DSPAPOS);
   pI830->saveDSPABASE = INREG(DSPABASE);

   for(i= 0; i < 256; i++) {
      pI830->savePaletteA[i] = INREG(PALETTE_A + (i << 2));
   }

   if(pI830->num_pipes == 2) {
      pI830->savePIPEBCONF = INREG(PIPEBCONF);
      pI830->savePIPEBSRC = INREG(PIPEBSRC);
      pI830->saveDSPBCNTR = INREG(DSPBCNTR);
      pI830->saveFPB0 = INREG(FPB0);
      pI830->saveFPB1 = INREG(FPB1);
      pI830->saveDPLL_B = INREG(DPLL_B);
      if (IS_I965G(pI830))
	 pI830->saveDPLL_B_MD = INREG(DPLL_B_MD);
      pI830->saveHTOTAL_B = INREG(HTOTAL_B);
      pI830->saveHBLANK_B = INREG(HBLANK_B);
      pI830->saveHSYNC_B = INREG(HSYNC_B);
      pI830->saveVTOTAL_B = INREG(VTOTAL_B);
      pI830->saveVBLANK_B = INREG(VBLANK_B);
      pI830->saveVSYNC_B = INREG(VSYNC_B);
      pI830->saveDSPBSTRIDE = INREG(DSPBSTRIDE);
      pI830->saveDSPBSIZE = INREG(DSPBSIZE);
      pI830->saveDSPBPOS = INREG(DSPBPOS);
      pI830->saveDSPBBASE = INREG(DSPBBASE);
      for(i= 0; i < 256; i++) {
         pI830->savePaletteB[i] = INREG(PALETTE_B + (i << 2));
      }
   }

   if (IS_I965G(pI830)) {
      pI830->saveDSPASURF = INREG(DSPASURF);
      pI830->saveDSPBSURF = INREG(DSPBSURF);
   }

   pI830->saveVCLK_DIVISOR_VGA0 = INREG(VCLK_DIVISOR_VGA0);
   pI830->saveVCLK_DIVISOR_VGA1 = INREG(VCLK_DIVISOR_VGA1);
   pI830->saveVCLK_POST_DIV = INREG(VCLK_POST_DIV);
   pI830->saveVGACNTRL = INREG(VGACNTRL);

   for(i = 0; i < 7; i++) {
      pI830->saveSWF[i] = INREG(SWF0 + (i << 2));
      pI830->saveSWF[i+7] = INREG(SWF00 + (i << 2));
   }
   pI830->saveSWF[14] = INREG(SWF30);
   pI830->saveSWF[15] = INREG(SWF31);
   pI830->saveSWF[16] = INREG(SWF32);

   pI830->savePFIT_CONTROL = INREG(PFIT_CONTROL);

   for (i = 0; i < pI830->num_outputs; i++) {
      if (pI830->output[i].save != NULL)
	 pI830->output[i].save(pScrn, &pI830->output[i]);
   }

   vgaHWUnlock(hwp);
   vgaHWSave(pScrn, vgaReg, VGA_SR_FONTS);

   return TRUE;
}

static Bool
RestoreHWState(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);
   vgaHWPtr hwp = VGAHWPTR(pScrn);
   vgaRegPtr vgaReg = &hwp->SavedReg;
   CARD32 temp;
   int i;

   DPRINTF(PFX, "RestoreHWState\n");

#ifdef XF86DRI
   I830DRISetVBlankInterrupt (pScrn, FALSE);
#endif
   vgaHWRestore(pScrn, vgaReg, VGA_SR_FONTS);
   vgaHWLock(hwp);

   /* First, disable display planes */
   temp = INREG(DSPACNTR);
   OUTREG(DSPACNTR, temp & ~DISPLAY_PLANE_ENABLE);
   temp = INREG(DSPBCNTR);
   OUTREG(DSPBCNTR, temp & ~DISPLAY_PLANE_ENABLE);

   /* Next, disable display pipes */
   temp = INREG(PIPEACONF);
   OUTREG(PIPEACONF, temp & ~PIPEACONF_ENABLE);
   temp = INREG(PIPEBCONF);
   OUTREG(PIPEBCONF, temp & ~PIPEBCONF_ENABLE);

   /* Disable outputs if necessary */
   for (i = 0; i < pI830->num_outputs; i++) {
      pI830->output[i].pre_set_mode(pScrn, &pI830->output[i], NULL);
   }

   i830WaitForVblank(pScrn);

   OUTREG(FPA0, pI830->saveFPA0);
   OUTREG(FPA1, pI830->saveFPA1);
   OUTREG(DPLL_A, pI830->saveDPLL_A);
   if (IS_I965G(pI830))
      OUTREG(DPLL_A_MD, pI830->saveDPLL_A_MD);
   OUTREG(HTOTAL_A, pI830->saveHTOTAL_A);
   OUTREG(HBLANK_A, pI830->saveHBLANK_A);
   OUTREG(HSYNC_A, pI830->saveHSYNC_A);
   OUTREG(VTOTAL_A, pI830->saveVTOTAL_A);
   OUTREG(VBLANK_A, pI830->saveVBLANK_A);
   OUTREG(VSYNC_A, pI830->saveVSYNC_A);
   OUTREG(DSPASTRIDE, pI830->saveDSPASTRIDE);
   OUTREG(DSPASIZE, pI830->saveDSPASIZE);
   OUTREG(DSPAPOS, pI830->saveDSPAPOS);
   OUTREG(DSPABASE, pI830->saveDSPABASE);
   OUTREG(PIPEASRC, pI830->savePIPEASRC);
   for(i = 0; i < 256; i++) {
         OUTREG(PALETTE_A + (i << 2), pI830->savePaletteA[i]);
   }

   if(pI830->num_pipes == 2) {
      OUTREG(FPB0, pI830->saveFPB0);
      OUTREG(FPB1, pI830->saveFPB1);
      OUTREG(DPLL_B, pI830->saveDPLL_B);
      if (IS_I965G(pI830))
	 OUTREG(DPLL_B_MD, pI830->saveDPLL_B_MD);
      OUTREG(HTOTAL_B, pI830->saveHTOTAL_B);
      OUTREG(HBLANK_B, pI830->saveHBLANK_B);
      OUTREG(HSYNC_B, pI830->saveHSYNC_B);
      OUTREG(VTOTAL_B, pI830->saveVTOTAL_B);
      OUTREG(VBLANK_B, pI830->saveVBLANK_B);
      OUTREG(VSYNC_B, pI830->saveVSYNC_B);
      OUTREG(DSPBSTRIDE, pI830->saveDSPBSTRIDE);
      OUTREG(DSPBSIZE, pI830->saveDSPBSIZE);
      OUTREG(DSPBPOS, pI830->saveDSPBPOS);
      OUTREG(DSPBBASE, pI830->saveDSPBBASE);
      OUTREG(PIPEBSRC, pI830->savePIPEBSRC);
      for(i= 0; i < 256; i++) {
         OUTREG(PALETTE_B + (i << 2), pI830->savePaletteB[i]);
      }
   }

   OUTREG(PFIT_CONTROL, pI830->savePFIT_CONTROL);

   for (i = 0; i < pI830->num_outputs; i++) {
      pI830->output[i].restore(pScrn, &pI830->output[i]);
   }

   if (IS_I965G(pI830)) {
      OUTREG(DSPASURF, pI830->saveDSPASURF);
      OUTREG(DSPBSURF, pI830->saveDSPBSURF);
   }

   OUTREG(VCLK_DIVISOR_VGA0, pI830->saveVCLK_DIVISOR_VGA0);
   OUTREG(VCLK_DIVISOR_VGA1, pI830->saveVCLK_DIVISOR_VGA1);
   OUTREG(VCLK_POST_DIV, pI830->saveVCLK_POST_DIV);

   OUTREG(PIPEACONF, pI830->savePIPEACONF);
   OUTREG(PIPEBCONF, pI830->savePIPEBCONF);

   OUTREG(VGACNTRL, pI830->saveVGACNTRL);
   OUTREG(DSPACNTR, pI830->saveDSPACNTR);
   OUTREG(DSPBCNTR, pI830->saveDSPBCNTR);

   for(i = 0; i < 7; i++) {
	   OUTREG(SWF0 + (i << 2), pI830->saveSWF[i]);
	   OUTREG(SWF00 + (i << 2), pI830->saveSWF[i+7]);
   }

   OUTREG(SWF30, pI830->saveSWF[14]);
   OUTREG(SWF31, pI830->saveSWF[15]);
   OUTREG(SWF32, pI830->saveSWF[16]);

   i830CompareRegsToSnapshot(pScrn);

   return TRUE;
}

static void
InitRegisterRec(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);
   I830RegPtr i830Reg = &pI830->ModeReg;
   int i;

   if (!I830IsPrimary(pScrn)) return;

   for (i = 0; i < 8; i++)
      i830Reg->Fence[i] = 0;
}

/* Famous last words
 */
void
I830PrintErrorState(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);

   ErrorF("pgetbl_ctl: 0x%lx pgetbl_err: 0x%lx\n",
	  (unsigned long)INREG(PGETBL_CTL), (unsigned long)INREG(PGE_ERR));

   ErrorF("ipeir: %lx iphdr: %lx\n", (unsigned long)INREG(IPEIR), 
	  (unsigned long)INREG(IPEHR));

   ErrorF("LP ring tail: %lx head: %lx len: %lx start %lx\n",
	  (unsigned long)INREG(LP_RING + RING_TAIL),
	  (unsigned long)INREG(LP_RING + RING_HEAD) & HEAD_ADDR,
	  (unsigned long)INREG(LP_RING + RING_LEN), 
	  (unsigned long)INREG(LP_RING + RING_START));

   ErrorF("eir: %x esr: %x emr: %x\n",
	  INREG16(EIR), INREG16(ESR), INREG16(EMR));

   ErrorF("instdone: %x instpm: %x\n", INREG16(INST_DONE), INREG8(INST_PM));

   ErrorF("memmode: %lx instps: %lx\n", (unsigned long)INREG(MEMMODE), 
	  (unsigned long)INREG(INST_PS));

   ErrorF("hwstam: %x ier: %x imr: %x iir: %x\n",
	  INREG16(HWSTAM), INREG16(IER), INREG16(IMR), INREG16(IIR));
}

void
I965PrintErrorState(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);

   ErrorF("pgetbl_ctl: 0x%lx pgetbl_err: 0x%lx\n",
	  INREG(PGETBL_CTL), INREG(PGE_ERR));

   ErrorF("ipeir: %lx iphdr: %lx\n", INREG(IPEIR_I965), INREG(IPEHR_I965));

   ErrorF("LP ring tail: %lx head: %lx len: %lx start %lx\n",
	  INREG(LP_RING + RING_TAIL),
	  INREG(LP_RING + RING_HEAD) & HEAD_ADDR,
	  INREG(LP_RING + RING_LEN), INREG(LP_RING + RING_START));

   ErrorF("Err ID (eir): %x Err Status (esr): %x Err Mask (emr): %x\n",
	  (int)INREG(EIR), (int)INREG(ESR), (int)INREG(EMR));

   ErrorF("instdone: %x instdone_1: %x\n", (int)INREG(INST_DONE_I965),
	  (int)INREG(INST_DONE_1));
   ErrorF("instpm: %x\n", (int)INREG(INST_PM));

   ErrorF("memmode: %lx instps: %lx\n", INREG(MEMMODE), INREG(INST_PS_I965));

   ErrorF("HW Status mask (hwstam): %x\nIRQ enable (ier): %x imr: %x iir: %x\n",
	  (int)INREG(HWSTAM), (int)INREG(IER), (int)INREG(IMR),
	  (int)INREG(IIR));

   ErrorF("acthd: %lx dma_fadd_p: %lx\n", INREG(ACTHD), INREG(DMA_FADD_P));
   ErrorF("ecoskpd: %lx excc: %lx\n", INREG(ECOSKPD), INREG(EXCC));

   ErrorF("cache_mode: %x/%x\n", (int)INREG(CACHE_MODE_0),
	  (int)INREG(CACHE_MODE_1));
   ErrorF("mi_arb_state: %x\n", (int)INREG(MI_ARB_STATE));

   ErrorF("IA_VERTICES_COUNT_QW %x/%x\n", (int)INREG(IA_VERTICES_COUNT_QW),
	  (int)INREG(IA_VERTICES_COUNT_QW+4));
   ErrorF("IA_PRIMITIVES_COUNT_QW %x/%x\n", (int)INREG(IA_PRIMITIVES_COUNT_QW),
	  (int)INREG(IA_PRIMITIVES_COUNT_QW+4));

   ErrorF("VS_INVOCATION_COUNT_QW %x/%x\n", (int)INREG(VS_INVOCATION_COUNT_QW),
	  (int)INREG(VS_INVOCATION_COUNT_QW+4));

   ErrorF("GS_INVOCATION_COUNT_QW %x/%x\n", (int)INREG(GS_INVOCATION_COUNT_QW),
	  (int)INREG(GS_INVOCATION_COUNT_QW+4));
   ErrorF("GS_PRIMITIVES_COUNT_QW %x/%x\n", (int)INREG(GS_PRIMITIVES_COUNT_QW),
	  (int)INREG(GS_PRIMITIVES_COUNT_QW+4));

   ErrorF("CL_INVOCATION_COUNT_QW %x/%x\n", (int)INREG(CL_INVOCATION_COUNT_QW),
	  (int)INREG(CL_INVOCATION_COUNT_QW+4));
   ErrorF("CL_PRIMITIVES_COUNT_QW %x/%x\n", (int)INREG(CL_PRIMITIVES_COUNT_QW),
	  (int)INREG(CL_PRIMITIVES_COUNT_QW+4));

   ErrorF("PS_INVOCATION_COUNT_QW %x/%x\n", (int)INREG(PS_INVOCATION_COUNT_QW),
	  (int)INREG(PS_INVOCATION_COUNT_QW+4));
   ErrorF("PS_DEPTH_COUNT_QW %x/%x\n", (int)INREG(PS_DEPTH_COUNT_QW),
	  (int)INREG(PS_DEPTH_COUNT_QW+4));

   ErrorF("WIZ_CTL %x\n", (int)INREG(WIZ_CTL));
   ErrorF("TS_CTL %x  TS_DEBUG_DATA %x\n", (int)INREG(TS_CTL),
	  (int)INREG(TS_DEBUG_DATA));
   ErrorF("TD_CTL %x / %x\n", (int)INREG(TD_CTL), (int)INREG(TD_CTL2));

   
}

#ifdef I830DEBUG
static void
dump_DSPACNTR(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);
   unsigned int tmp;

   /* Display A Control */
   tmp = INREG(0x70180);
   ErrorF("Display A Plane Control Register (0x%.8x)\n", tmp);

   if (tmp & BIT(31))
      ErrorF("   Display Plane A (Primary) Enable\n");
   else
      ErrorF("   Display Plane A (Primary) Disabled\n");

   if (tmp & BIT(30))
      ErrorF("   Display A pixel data is gamma corrected\n");
   else
      ErrorF("   Display A pixel data bypasses gamma correction logic (default)\n");

   switch ((tmp & 0x3c000000) >> 26) {	/* bit 29:26 */
   case 0x00:
   case 0x01:
   case 0x03:
      ErrorF("   Reserved\n");
      break;
   case 0x02:
      ErrorF("   8-bpp Indexed\n");
      break;
   case 0x04:
      ErrorF("   15-bit (5-5-5) pixel format (Targa compatible)\n");
      break;
   case 0x05:
      ErrorF("   16-bit (5-6-5) pixel format (XGA compatible)\n");
      break;
   case 0x06:
      ErrorF("   32-bit format (X:8:8:8)\n");
      break;
   case 0x07:
      ErrorF("   32-bit format (8:8:8:8)\n");
      break;
   default:
      ErrorF("   Unknown - Invalid register value maybe?\n");
   }

   if (tmp & BIT(25))
      ErrorF("   Stereo Enable\n");
   else
      ErrorF("   Stereo Disable\n");

   if (tmp & BIT(24))
      ErrorF("   Display A, Pipe B Select\n");
   else
      ErrorF("   Display A, Pipe A Select\n");

   if (tmp & BIT(22))
      ErrorF("   Source key is enabled\n");
   else
      ErrorF("   Source key is disabled\n");

   switch ((tmp & 0x00300000) >> 20) {	/* bit 21:20 */
   case 0x00:
      ErrorF("   No line duplication\n");
      break;
   case 0x01:
      ErrorF("   Line/pixel Doubling\n");
      break;
   case 0x02:
   case 0x03:
      ErrorF("   Reserved\n");
      break;
   }

   if (tmp & BIT(18))
      ErrorF("   Stereo output is high during second image\n");
   else
      ErrorF("   Stereo output is high during first image\n");
}

static void
dump_DSPBCNTR(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);
   unsigned int tmp;

   /* Display B/Sprite Control */
   tmp = INREG(0x71180);
   ErrorF("Display B/Sprite Plane Control Register (0x%.8x)\n", tmp);

   if (tmp & BIT(31))
      ErrorF("   Display B/Sprite Enable\n");
   else
      ErrorF("   Display B/Sprite Disable\n");

   if (tmp & BIT(30))
      ErrorF("   Display B pixel data is gamma corrected\n");
   else
      ErrorF("   Display B pixel data bypasses gamma correction logic (default)\n");

   switch ((tmp & 0x3c000000) >> 26) {	/* bit 29:26 */
   case 0x00:
   case 0x01:
   case 0x03:
      ErrorF("   Reserved\n");
      break;
   case 0x02:
      ErrorF("   8-bpp Indexed\n");
      break;
   case 0x04:
      ErrorF("   15-bit (5-5-5) pixel format (Targa compatible)\n");
      break;
   case 0x05:
      ErrorF("   16-bit (5-6-5) pixel format (XGA compatible)\n");
      break;
   case 0x06:
      ErrorF("   32-bit format (X:8:8:8)\n");
      break;
   case 0x07:
      ErrorF("   32-bit format (8:8:8:8)\n");
      break;
   default:
      ErrorF("   Unknown - Invalid register value maybe?\n");
   }

   if (tmp & BIT(25))
      ErrorF("   Stereo is enabled and both start addresses are used in a two frame sequence\n");
   else
      ErrorF("   Stereo disable and only a single start address is used\n");

   if (tmp & BIT(24))
      ErrorF("   Display B/Sprite, Pipe B Select\n");
   else
      ErrorF("   Display B/Sprite, Pipe A Select\n");

   if (tmp & BIT(22))
      ErrorF("   Sprite source key is enabled\n");
   else
      ErrorF("   Sprite source key is disabled (default)\n");

   switch ((tmp & 0x00300000) >> 20) {	/* bit 21:20 */
   case 0x00:
      ErrorF("   No line duplication\n");
      break;
   case 0x01:
      ErrorF("   Line/pixel Doubling\n");
      break;
   case 0x02:
   case 0x03:
      ErrorF("   Reserved\n");
      break;
   }

   if (tmp & BIT(18))
      ErrorF("   Stereo output is high during second image\n");
   else
      ErrorF("   Stereo output is high during first image\n");

   if (tmp & BIT(15))
      ErrorF("   Alpha transfer mode enabled\n");
   else
      ErrorF("   Alpha transfer mode disabled\n");

   if (tmp & BIT(0))
      ErrorF("   Sprite is above overlay\n");
   else
      ErrorF("   Sprite is above display A (default)\n");
}

void
I830_dump_registers(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);
   unsigned int i;

   ErrorF("%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n");

   dump_DSPACNTR(pScrn);
   dump_DSPBCNTR(pScrn);

   ErrorF("0x71400 == 0x%.8x\n", INREG(0x71400));
   ErrorF("0x70008 == 0x%.8x\n", INREG(0x70008));
   for (i = 0x71410; i <= 0x71428; i += 4)
      ErrorF("0x%x == 0x%.8x\n", i, INREG(i));

   ErrorF("%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n");
}
#endif

static void
I830PointerMoved(int index, int x, int y)
{
   ScrnInfoPtr pScrn = xf86Screens[index];
   I830Ptr pI830 = I830PTR(pScrn);
   int newX = x, newY = y;

   switch (pI830->rotation) {
      case RR_Rotate_0:
         break;
      case RR_Rotate_90:
         newX = y;
         newY = pScrn->pScreen->width - x - 1;
         break;
      case RR_Rotate_180:
         newX = pScrn->pScreen->width - x - 1;
         newY = pScrn->pScreen->height - y - 1;
         break;
      case RR_Rotate_270:
         newX = pScrn->pScreen->height - y - 1;
         newY = x;
         break;
   }

   (*pI830->PointerMoved)(index, newX, newY);
}

static Bool
I830CreateScreenResources (ScreenPtr pScreen)
{
   ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
   I830Ptr pI830 = I830PTR(pScrn);

   pScreen->CreateScreenResources = pI830->CreateScreenResources;
   if (!(*pScreen->CreateScreenResources)(pScreen))
      return FALSE;

   if (!I830RandRCreateScreenResources (pScreen))
      return FALSE;

   return TRUE;
}

static Bool
I830InitFBManager(
    ScreenPtr pScreen,  
    BoxPtr FullBox
){
   ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
   RegionRec ScreenRegion;
   RegionRec FullRegion;
   BoxRec ScreenBox;
   Bool ret;

   ScreenBox.x1 = 0;
   ScreenBox.y1 = 0;
   ScreenBox.x2 = pScrn->displayWidth;
   if (pScrn->virtualX > pScrn->virtualY)
      ScreenBox.y2 = pScrn->virtualX;
   else
      ScreenBox.y2 = pScrn->virtualY;

   if((FullBox->x1 >  ScreenBox.x1) || (FullBox->y1 >  ScreenBox.y1) ||
      (FullBox->x2 <  ScreenBox.x2) || (FullBox->y2 <  ScreenBox.y2)) {
	return FALSE;   
   }

   if (FullBox->y2 < FullBox->y1) return FALSE;
   if (FullBox->x2 < FullBox->x2) return FALSE;

   REGION_INIT(pScreen, &ScreenRegion, &ScreenBox, 1); 
   REGION_INIT(pScreen, &FullRegion, FullBox, 1); 

   REGION_SUBTRACT(pScreen, &FullRegion, &FullRegion, &ScreenRegion);

   ret = xf86InitFBManagerRegion(pScreen, &FullRegion);

   REGION_UNINIT(pScreen, &ScreenRegion);
   REGION_UNINIT(pScreen, &FullRegion);
    
   return ret;
}

/* Initialize the first context */
void
IntelEmitInvarientState(ScrnInfoPtr pScrn)
{
   I830Ptr pI830 = I830PTR(pScrn);
   CARD32 ctx_addr;

   if (pI830->noAccel)
      return;

   ctx_addr = pI830->ContextMem.Start;
   /* Align to a 2k boundry */
   ctx_addr = ((ctx_addr + 2048 - 1) / 2048) * 2048;

   {
      BEGIN_LP_RING(2);
      OUT_RING(MI_SET_CONTEXT);
      OUT_RING(ctx_addr |
	       CTXT_NO_RESTORE |
	       CTXT_PALETTE_SAVE_DISABLE | CTXT_PALETTE_RESTORE_DISABLE);
      ADVANCE_LP_RING();
   }

   if (!IS_I965G(pI830))
   {
      if (IS_I9XX(pI830))
         I915EmitInvarientState(pScrn);
      else
         I830EmitInvarientState(pScrn);
   }
}

#ifdef XF86DRI
#ifndef DRM_BO_MEM_TT
#error "Wrong drm.h file included. You need to compile and install a recent libdrm."
#endif

#ifndef XSERVER_LIBDRM_MM

static int
I830DrmMMInit(int drmFD, unsigned long pageOffs, unsigned long pageSize,
	      unsigned memType)
{

   drm_mm_init_arg_t arg;
   int ret;
   
   memset(&arg, 0, sizeof(arg));
   arg.req.op = mm_init;
   arg.req.p_offset = pageOffs;
   arg.req.p_size = pageSize;
   arg.req.mem_type = memType;

   ret = ioctl(drmFD, DRM_IOCTL_MM_INIT, &arg);
   
   if (ret)
      return -errno;
   
   return 0;
   
}

static int
I830DrmMMTakedown(int drmFD, unsigned memType)
{
   drm_mm_init_arg_t arg;
   int ret = 0;
   
   memset(&arg, 0, sizeof(arg));
   arg.req.op = mm_takedown;
   arg.req.mem_type = memType;
   if (ioctl(drmFD, DRM_IOCTL_MM_INIT, &arg)) {
      ret = -errno;
   }
   
   return ret;
}

static int I830DrmMMLock(int fd, unsigned memType)
{
    drm_mm_init_arg_t arg;
    int ret;

    memset(&arg, 0, sizeof(arg));
    arg.req.op = mm_lock;
    arg.req.mem_type = memType;

    do{
	ret = ioctl(fd, DRM_IOCTL_MM_INIT, &arg);
    } while (ret && errno == EAGAIN);
    
    return ret;	
}

static int I830DrmMMUnlock(int fd, unsigned memType)
{
    drm_mm_init_arg_t arg;
    int ret;

    memset(&arg, 0, sizeof(arg));
    arg.req.op = mm_unlock;
    arg.req.mem_type = memType;

    do{
	ret = ioctl(fd, DRM_IOCTL_MM_INIT, &arg);
    } while (ret && errno == EAGAIN);
    
    return ret;	
}

#endif
#endif

static Bool
I830ScreenInit(int scrnIndex, ScreenPtr pScreen, int argc, char **argv)
{
   ScrnInfoPtr pScrn;
   vgaHWPtr hwp;
   I830Ptr pI830;
   VisualPtr visual;
   I830Ptr pI8301 = NULL;
#ifdef XF86DRI
   Bool driDisabled;
#endif

   pScrn = xf86Screens[pScreen->myNum];
   pI830 = I830PTR(pScrn);
   hwp = VGAHWPTR(pScrn);

   pScrn->displayWidth = pI830->displayWidth;

   if (I830IsPrimary(pScrn)) {
      /* Rotated Buffer */
      memset(&(pI830->RotatedMem), 0, sizeof(pI830->RotatedMem));
      pI830->RotatedMem.Key = -1;
      /* Rotated2 Buffer */
      memset(&(pI830->RotatedMem2), 0, sizeof(pI830->RotatedMem2));
      pI830->RotatedMem2.Key = -1;
   }

#ifdef HAS_MTRR_SUPPORT
   {
      int fd;
      struct mtrr_gentry gentry;
      struct mtrr_sentry sentry;

      if ( ( fd = open ("/proc/mtrr", O_RDONLY, 0) ) != -1 ) {
         for (gentry.regnum = 0; ioctl (fd, MTRRIOC_GET_ENTRY, &gentry) == 0;
	      ++gentry.regnum) {

	    if (gentry.size < 1) {
	       /* DISABLED */
	       continue;
	    }

            /* Check the MTRR range is one we like and if not - remove it.
             * The Xserver common layer will then setup the right range
             * for us.
             */
	    if (gentry.base == pI830->LinearAddr && 
	        gentry.size < pI830->FbMapSize) {

               xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		  "Removing bad MTRR range (base 0x%lx, size 0x%x)\n",
		  gentry.base, gentry.size);

    	       sentry.base = gentry.base;
               sentry.size = gentry.size;
               sentry.type = gentry.type;

               if (ioctl (fd, MTRRIOC_DEL_ENTRY, &sentry) == -1) {
                  xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		     "Failed to remove bad MTRR range\n");
               }
	    }
         }
         close(fd);
      }
   }
#endif

   pI830->starting = TRUE;

   /* Alloc our pointers for the primary head */
   if (I830IsPrimary(pScrn)) {
      if (!pI830->LpRing)
         pI830->LpRing = xalloc(sizeof(I830RingBuffer));
      if (!pI830->CursorMem)
         pI830->CursorMem = xalloc(sizeof(I830MemRange));
      if (!pI830->CursorMemARGB)
         pI830->CursorMemARGB = xalloc(sizeof(I830MemRange));
      if (!pI830->OverlayMem)
         pI830->OverlayMem = xalloc(sizeof(I830MemRange));
      if (!pI830->overlayOn)
         pI830->overlayOn = xalloc(sizeof(Bool));
      if (!pI830->used3D)
         pI830->used3D = xalloc(sizeof(int));
      if (!pI830->LpRing || !pI830->CursorMem || !pI830->CursorMemARGB ||
          !pI830->OverlayMem || !pI830->overlayOn || !pI830->used3D) {
         xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		 "Could not allocate primary data structures.\n");
         return FALSE;
      }
      *pI830->overlayOn = FALSE;
      if (pI830->entityPrivate)
         pI830->entityPrivate->XvInUse = -1;
   }

   /* Make our second head point to the first heads structures */
   if (!I830IsPrimary(pScrn)) {
      pI8301 = I830PTR(pI830->entityPrivate->pScrn_1);
      pI830->LpRing = pI8301->LpRing;
      pI830->CursorMem = pI8301->CursorMem;
      pI830->CursorMemARGB = pI8301->CursorMemARGB;
      pI830->OverlayMem = pI8301->OverlayMem;
      pI830->overlayOn = pI8301->overlayOn;
      pI830->used3D = pI8301->used3D;
   }

   miClearVisualTypes();
   if (!miSetVisualTypes(pScrn->depth,
			    miGetDefaultVisualMask(pScrn->depth),
			    pScrn->rgbBits, pScrn->defaultVisual))
	 return FALSE;
   if (!miSetPixmapDepths())
      return FALSE;

#ifdef I830_XV
   pI830->XvEnabled = !pI830->XvDisabled;
   if (pI830->XvEnabled) {
      if (!I830IsPrimary(pScrn)) {
         if (!pI8301->XvEnabled || pI830->noAccel) {
            pI830->XvEnabled = FALSE;
	    xf86DrvMsg(pScrn->scrnIndex, X_PROBED, "Xv is disabled.\n");
         }
      } else
      if (pI830->noAccel || pI830->StolenOnly) {
	 xf86DrvMsg(pScrn->scrnIndex, X_PROBED, "Xv is disabled because it "
		    "needs 2D accel and AGPGART.\n");
	 pI830->XvEnabled = FALSE;
      }
   }
#else
   pI830->XvEnabled = FALSE;
#endif

   if (I830IsPrimary(pScrn)) {
      I830ResetAllocations(pScrn, 0);

      if (!I830Allocate2DMemory(pScrn, ALLOC_INITIAL))
	return FALSE;
   }

   if (!pI830->noAccel) {
      if (pI830->LpRing->mem.Size == 0) {
	  xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		     "Disabling acceleration because the ring buffer "
		      "allocation failed.\n");
	   pI830->noAccel = TRUE;
      }
   }

   if (!pI830->SWCursor) {
      if (pI830->CursorMem->Size == 0) {
	  xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		     "Disabling HW cursor because the cursor memory "
		      "allocation failed.\n");
	   pI830->SWCursor = TRUE;
      }
   }

#ifdef I830_XV
   if (pI830->XvEnabled) {
      if (pI830->noAccel) {
	 xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "Disabling Xv because it "
		    "needs 2D acceleration.\n");
	 pI830->XvEnabled = FALSE;
      }
      if (pI830->OverlayMem->Physical == 0) {
	  xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
		     "Disabling Xv because the overlay register buffer "
		      "allocation failed.\n");
	 pI830->XvEnabled = FALSE;
      }
   }
#endif

   InitRegisterRec(pScrn);

#ifdef XF86DRI
   /*
    * pI830->directRenderingDisabled is set once in PreInit.  Reinitialise
    * pI830->directRenderingEnabled based on it each generation.
    */
   pI830->directRenderingEnabled = !pI830->directRenderingDisabled;
   /*
    * Setup DRI after visuals have been established, but before fbScreenInit
    * is called.   fbScreenInit will eventually call into the drivers
    * InitGLXVisuals call back.
    */

   if (pI830->directRenderingEnabled) {
      if (pI830->noAccel || pI830->SWCursor || (pI830->StolenOnly && I830IsPrimary(pScrn))) {
	 xf86DrvMsg(pScrn->scrnIndex, X_PROBED, "DRI is disabled because it "
		    "needs HW cursor, 2D accel and AGPGART.\n");
	 pI830->directRenderingEnabled = FALSE;
      }
   }

   driDisabled = !pI830->directRenderingEnabled;

   if (pI830->directRenderingEnabled)
      pI830->directRenderingEnabled = I830DRIScreenInit(pScreen);

   if (pI830->directRenderingEnabled) {
      pI830->directRenderingEnabled =
	 I830Allocate3DMemory(pScrn,
			      pI830->disableTiling ? ALLOC_NO_TILING : 0);
      if (!pI830->directRenderingEnabled)
	  I830DRICloseScreen(pScreen);
   }

#else
   pI830->directRenderingEnabled = FALSE;
#endif

   /*
    * After the 3D allocations have been done, see if there's any free space
    * that can be added to the framebuffer allocation.
    */
   if (I830IsPrimary(pScrn)) {
      I830Allocate2DMemory(pScrn, 0);

      DPRINTF(PFX, "assert(if(!I830DoPoolAllocation(pScrn, pI830->StolenPool)))\n");
      if (!I830DoPoolAllocation(pScrn, &(pI830->StolenPool)))
         return FALSE;

      DPRINTF(PFX, "assert( if(!I830FixupOffsets(pScrn)) )\n");
      if (!I830FixupOffsets(pScrn))
         return FALSE;
   }

#ifdef XF86DRI
   if (pI830->directRenderingEnabled) {
      I830SetupMemoryTiling(pScrn);
      pI830->directRenderingEnabled = I830DRIDoMappings(pScreen);
   }
#endif

   DPRINTF(PFX, "assert( if(!I830MapMem(pScrn)) )\n");
   if (!I830MapMem(pScrn))
      return FALSE;

   pScrn->memPhysBase = (unsigned long)pI830->FbBase;

   if (I830IsPrimary(pScrn)) {
      pScrn->fbOffset = pI830->FrontBuffer.Start;
   } else {
      pScrn->fbOffset = pI8301->FrontBuffer2.Start;
   }

   pI830->xoffset = (pScrn->fbOffset / pI830->cpp) % pScrn->displayWidth;
   pI830->yoffset = (pScrn->fbOffset / pI830->cpp) / pScrn->displayWidth;

   vgaHWSetMmioFuncs(hwp, pI830->MMIOBase, 0);
   vgaHWGetIOBase(hwp);
   DPRINTF(PFX, "assert( if(!vgaHWMapMem(pScrn)) )\n");
   if (!vgaHWMapMem(pScrn))
      return FALSE;

   DPRINTF(PFX, "assert( if(!I830EnterVT(scrnIndex, 0)) )\n");

   if (!I830EnterVT(scrnIndex, 0))
      return FALSE;

    if (pScrn->virtualX > pScrn->displayWidth)
	pScrn->displayWidth = pScrn->virtualX;

   DPRINTF(PFX, "assert( if(!fbScreenInit(pScreen, ...) )\n");
   if (!fbScreenInit(pScreen, pI830->FbBase + pScrn->fbOffset, 
                     pScrn->virtualX, pScrn->virtualY,
		     pScrn->xDpi, pScrn->yDpi,
		     pScrn->displayWidth, pScrn->bitsPerPixel))
      return FALSE;

   if (pScrn->bitsPerPixel > 8) {
      /* Fixup RGB ordering */
      visual = pScreen->visuals + pScreen->numVisuals;
      while (--visual >= pScreen->visuals) {
	 if ((visual->class | DynamicClass) == DirectColor) {
	    visual->offsetRed = pScrn->offset.red;
	    visual->offsetGreen = pScrn->offset.green;
	    visual->offsetBlue = pScrn->offset.blue;
	    visual->redMask = pScrn->mask.red;
	    visual->greenMask = pScrn->mask.green;
	    visual->blueMask = pScrn->mask.blue;
	 }
      }
   }

   fbPictureInit(pScreen, 0, 0);

   xf86SetBlackWhitePixels(pScreen);

   I830DGAInit(pScreen);

   DPRINTF(PFX,
	   "assert( if(!I830InitFBManager(pScreen, &(pI830->FbMemBox))) )\n");
   if (I830IsPrimary(pScrn)) {
      if (!I830InitFBManager(pScreen, &(pI830->FbMemBox))) {
         xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		 "Failed to init memory manager\n");
      }

      if (pI830->LinearAlloc && xf86InitFBManagerLinear(pScreen, pI830->LinearMem.Offset / pI830->cpp, pI830->LinearMem.Size / pI830->cpp))
            xf86DrvMsg(scrnIndex, X_INFO, 
			"Using %ld bytes of offscreen memory for linear (offset=0x%lx)\n", pI830->LinearMem.Size, pI830->LinearMem.Offset);

   } else {
      if (!I830InitFBManager(pScreen, &(pI8301->FbMemBox2))) {
         xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		 "Failed to init memory manager\n");
      }
   }

   if (!pI830->noAccel) {
      if (!I830AccelInit(pScreen)) {
	 xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		    "Hardware acceleration initialization failed\n");
      }
   }

   miInitializeBackingStore(pScreen);
   xf86SetBackingStore(pScreen);
   xf86SetSilkenMouse(pScreen);
   miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

   if (!pI830->SWCursor) {
      xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Initializing HW Cursor\n");
      if (!I830CursorInit(pScreen))
	 xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		    "Hardware cursor initialization failed\n");
   } else
      xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Initializing SW Cursor!\n");

   DPRINTF(PFX, "assert( if(!miCreateDefColormap(pScreen)) )\n");
   if (!miCreateDefColormap(pScreen))
      return FALSE;

   DPRINTF(PFX, "assert( if(!xf86HandleColormaps(pScreen, ...)) )\n");
   if (!xf86HandleColormaps(pScreen, 256, 8, I830LoadPalette, 0,
			    CMAP_RELOAD_ON_MODE_SWITCH |
			    CMAP_PALETTED_TRUECOLOR)) {
      return FALSE;
   }

   xf86DPMSInit(pScreen, I830DisplayPowerManagementSet, 0);

#ifdef I830_XV
   /* Init video */
   if (pI830->XvEnabled)
      I830InitVideo(pScreen);
#endif

#ifdef XF86DRI
   if (pI830->directRenderingEnabled) {
      pI830->directRenderingEnabled = I830DRIFinishScreenInit(pScreen);
   }
#endif

   /* Setup 3D engine, needed for rotation too */
   IntelEmitInvarientState(pScrn);

#ifdef XF86DRI
   if (pI830->directRenderingEnabled) {
      pI830->directRenderingOpen = TRUE;
      xf86DrvMsg(pScrn->scrnIndex, X_INFO, "direct rendering: Enabled\n");
   } else {
      if (driDisabled)
	 xf86DrvMsg(pScrn->scrnIndex, X_INFO, "direct rendering: Disabled\n");
      else
	 xf86DrvMsg(pScrn->scrnIndex, X_INFO, "direct rendering: Failed\n");
   }
#else
   xf86DrvMsg(pScrn->scrnIndex, X_INFO, "direct rendering: Not available\n");
#endif

   pScreen->SaveScreen = I830SaveScreen;
   pI830->CloseScreen = pScreen->CloseScreen;
   pScreen->CloseScreen = I830CloseScreen;

   if (pI830->shadowReq.minorversion >= 1) {
      /* Rotation */
      xf86DrvMsg(pScrn->scrnIndex, X_INFO, "RandR enabled, ignore the following RandR disabled message.\n");
      xf86DisableRandR(); /* Disable built-in RandR extension */
      shadowSetup(pScreen);
      /* support all rotations */
      if (IS_I965G(pI830)) {
	 I830RandRInit(pScreen, RR_Rotate_0); /* only 0 degrees for I965G */
      } else {
	 I830RandRInit(pScreen, RR_Rotate_0 | RR_Rotate_90 | RR_Rotate_180 | RR_Rotate_270);
      }
      pI830->PointerMoved = pScrn->PointerMoved;
      pScrn->PointerMoved = I830PointerMoved;
      pI830->CreateScreenResources = pScreen->CreateScreenResources;
      pScreen->CreateScreenResources = I830CreateScreenResources;
   } else {
      /* Rotation */
      xf86DrvMsg(pScrn->scrnIndex, X_INFO, "libshadow is version %d.%d.%d, required 1.1.0 or greater for rotation.\n",pI830->shadowReq.majorversion,pI830->shadowReq.minorversion,pI830->shadowReq.patchlevel);
   }

   if (serverGeneration == 1)
      xf86ShowUnusedOptions(pScrn->scrnIndex, pScrn->options);

#ifdef I830DEBUG
   I830_dump_registers(pScrn);
#endif

   if (IS_I965G(pI830)) {
      /* turn off clock gating */
#if 0
      OUTREG(0x6204, 0x70804000);
      OUTREG(0x6208, 0x00000001);
#else
      OUTREG(0x6204, 0x70000000);
#endif
      /* Enable DAP stateless accesses.  
       * Required for all i965 steppings.
       */
      OUTREG(SVG_WORK_CTL, 0x00000010);
   }

   pI830->starting = FALSE;
   pI830->closing = FALSE;
   pI830->suspended = FALSE;

   switch (pI830->InitialRotation) {
      case 0:
         xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Rotating to 0 degrees\n");
         pI830->rotation = RR_Rotate_0;
         break;
      case 90:
         xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Rotating to 90 degrees\n");
         pI830->rotation = RR_Rotate_90;
         break;
      case 180:
         xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Rotating to 180 degrees\n");
         pI830->rotation = RR_Rotate_180;
         break;
      case 270:
         xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Rotating to 270 degrees\n");
         pI830->rotation = RR_Rotate_270;
         break;
      default:
         xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Bad rotation setting - defaulting to 0 degrees\n");
         pI830->rotation = RR_Rotate_0;
         break;
   }


#ifdef XF86DRI
   if (pI830->directRenderingEnabled && (pI830->mmModeFlags & I830_KERNEL_MM)) {
      unsigned long aperEnd = ROUND_DOWN_TO(pI830->FbMapSize, GTT_PAGE_SIZE) 
	 / GTT_PAGE_SIZE;
      unsigned long aperStart = ROUND_TO(pI830->FbMapSize - KB(pI830->mmSize), GTT_PAGE_SIZE) 
	 / GTT_PAGE_SIZE;

      if (aperEnd < aperStart || aperEnd - aperStart < I830_MM_MINPAGES) {
	 xf86DrvMsg(pScrn->scrnIndex, X_ERROR, 
		    "Too little AGP aperture space for DRM memory manager.\n"
		    "\tPlease increase AGP aperture size from BIOS configuration screen\n"
		    "\tor decrease the amount of video RAM using option \"VideoRam\".\n"
		    "\tDisabling DRI.\n");
	 pI830->directRenderingOpen = FALSE;
	 I830DRICloseScreen(pScreen);
	 pI830->directRenderingEnabled = FALSE;
      } else {
#ifndef XSERVER_LIBDRM_MM
	 if (I830DrmMMInit(pI830->drmSubFD, aperStart, aperEnd - aperStart,
			   DRM_BO_MEM_TT)) {
#else
	 if (drmMMInit(pI830->drmSubFD, aperStart, aperEnd - aperStart,
		       DRM_BO_MEM_TT)) {
#endif	   
	    xf86DrvMsg(pScrn->scrnIndex, X_ERROR, 
		       "Could not initialize the DRM memory manager.\n");
	    
	    pI830->directRenderingOpen = FALSE;
	    I830DRICloseScreen(pScreen);
	    pI830->directRenderingEnabled = FALSE;
	 } else {
	    xf86DrvMsg(pScrn->scrnIndex, X_INFO, 
		       "Initialized DRM memory manager, %ld AGP pages\n"
		       "\tat AGP offset 0x%lx\n", 
		       aperEnd - aperStart,
		       aperStart);
	 }
      }
   }
#endif

   return TRUE;
}

static void
i830AdjustFrame(int scrnIndex, int x, int y, int flags)
{
   ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
   I830Ptr pI830 = I830PTR(pScrn);
   int i;

   DPRINTF(PFX, "i830AdjustFrame: y = %d (+ %d), x = %d (+ %d)\n",
	   x, pI830->xoffset, y, pI830->yoffset);

   /* Sync the engine before adjust frame */
   if (pI830->AccelInfoRec && pI830->AccelInfoRec->NeedToSync) {
      (*pI830->AccelInfoRec->Sync)(pScrn);
      pI830->AccelInfoRec->NeedToSync = FALSE;
   }

   for (i = 0; i < pI830->num_pipes; i++)
      if (pI830->pipes[i].enabled)
	 i830PipeSetBase(pScrn, i, x, y);
}

static void
I830FreeScreen(int scrnIndex, int flags)
{
   I830FreeRec(xf86Screens[scrnIndex]);
   if (xf86LoaderCheckSymbol("vgaHWFreeHWRec"))
      vgaHWFreeHWRec(xf86Screens[scrnIndex]);
}

static void
I830LeaveVT(int scrnIndex, int flags)
{
   ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
   I830Ptr pI830 = I830PTR(pScrn);

   DPRINTF(PFX, "Leave VT\n");

   pI830->leaving = TRUE;

   if (pI830->devicesTimer)
      TimerCancel(pI830->devicesTimer);
   pI830->devicesTimer = NULL;

   i830SetHotkeyControl(pScrn, HOTKEY_BIOS_SWITCH);

#ifdef I830_XV
   /* Give the video overlay code a chance to shutdown. */
   I830VideoSwitchModeBefore(pScrn, NULL);
#endif

   if (pI830->Clone) {
      /* Ensure we don't try and setup modes on a clone head */
      pI830->CloneHDisplay = 0;
      pI830->CloneVDisplay = 0;
   }

   if (!I830IsPrimary(pScrn)) {
   	I830Ptr pI8301 = I830PTR(pI830->entityPrivate->pScrn_1);
	if (!pI8301->GttBound) {
		return;
	}
   }

#ifdef XF86DRI
   if (pI830->directRenderingOpen) {
      DRILock(screenInfo.screens[pScrn->scrnIndex], 0);
      if (pI830->mmModeFlags & I830_KERNEL_MM) {
#ifndef XSERVER_LIBDRM_MM
	 I830DrmMMLock(pI830->drmSubFD, DRM_BO_MEM_TT);
#else
	 drmMMLock(pI830->drmSubFD, DRM_BO_MEM_TT);
#endif
      }
      I830DRISetVBlankInterrupt (pScrn, FALSE);
      
      drmCtlUninstHandler(pI830->drmSubFD);
   }
#endif

   if (pI830->CursorInfoRec && pI830->CursorInfoRec->HideCursor)
      pI830->CursorInfoRec->HideCursor(pScrn);

   ResetState(pScrn, TRUE);

   RestoreHWState(pScrn);
   if (I830IsPrimary(pScrn))
      I830UnbindAGPMemory(pScrn);
   if (pI830->AccelInfoRec)
      pI830->AccelInfoRec->NeedToSync = FALSE;
}

/*
 * This gets called when gaining control of the VT, and from ScreenInit().
 */
static Bool
I830EnterVT(int scrnIndex, int flags)
{
   ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
   I830Ptr  pI830 = I830PTR(pScrn);
   int	    i;

   DPRINTF(PFX, "Enter VT\n");

   /*
    * Only save state once per server generation since that's what most
    * drivers do.  Could change this to save state at each VT enter.
    */
   if (pI830->SaveGeneration != serverGeneration) {
      pI830->SaveGeneration = serverGeneration;
      SaveHWState(pScrn);
   }

   pI830->leaving = FALSE;

#if 1
   /* Clear the framebuffer */
   memset(pI830->FbBase + pScrn->fbOffset, 0,
	  pScrn->virtualY * pScrn->displayWidth * pI830->cpp);
#endif

   if (I830IsPrimary(pScrn))
      if (!I830BindAGPMemory(pScrn))
         return FALSE;

   CheckInheritedState(pScrn);

   ResetState(pScrn, FALSE);
   SetHWOperatingState(pScrn);

   for (i = 0; i < pI830->num_pipes; i++)
   {
      I830PipePtr pipe = &pI830->pipes[i];
      /* Mark that we'll need to re-set the mode for sure */
      memset(&pipe->curMode, 0, sizeof(pipe->curMode));
      if (!pipe->desiredMode.CrtcHDisplay)
      {
	 pipe->desiredMode = *i830PipeFindClosestMode (pScrn, i,
						       pScrn->currentMode);
      }
      if (!i830PipeSetMode (pScrn, &pipe->desiredMode, i, TRUE))
	 return FALSE;
      i830PipeSetBase(pScrn, i, pipe->x, pipe->y);
   }

   i830DisableUnusedFunctions(pScrn);

   i830DumpRegs (pScrn);
   i830DescribeOutputConfiguration(pScrn);

#ifdef XF86DRI
   I830DRISetVBlankInterrupt (pScrn, TRUE);
#endif
   
#if 0
   if (!i830SetMode(pScrn, pScrn->currentMode))
      return FALSE;
#endif
   
#ifdef I830_XV
   I830VideoSwitchModeAfter(pScrn, pScrn->currentMode);
#endif

   ResetState(pScrn, TRUE);
   SetHWOperatingState(pScrn);

   pScrn->AdjustFrame(scrnIndex, pScrn->frameX0, pScrn->frameY0, 0);

#ifdef XF86DRI
   if (pI830->directRenderingEnabled) {

      I830DRISetVBlankInterrupt (pScrn, TRUE);

      if (!pI830->starting) {
         ScreenPtr pScreen = pScrn->pScreen;
         drmI830Sarea *sarea = (drmI830Sarea *) DRIGetSAREAPrivate(pScreen);
         int i;

	 I830DRIResume(screenInfo.screens[scrnIndex]);
      
	 I830RefreshRing(pScrn);
	 I830Sync(pScrn);
	 DO_RING_IDLE();

	 sarea->texAge++;
	 for(i = 0; i < I830_NR_TEX_REGIONS+1 ; i++)
	    sarea->texList[i].age = sarea->texAge;

	 if (pI830->mmModeFlags & I830_KERNEL_MM) {
#ifndef XSERVER_LIBDRM_MM
	    I830DrmMMUnlock(pI830->drmSubFD, DRM_BO_MEM_TT);
#else
	    drmMMUnlock(pI830->drmSubFD, DRM_BO_MEM_TT);
#endif
	 }

	 DPRINTF(PFX, "calling dri unlock\n");
	 DRIUnlock(screenInfo.screens[pScrn->scrnIndex]);
      }
      pI830->LockHeld = 0;
   }
#endif

   /* Set the hotkey to just notify us.  We can check its results periodically
    * in the CheckDevicesTimer.  Eventually we want the kernel to just hand us
    * an input event when someone presses the button, but for now we just have
    * to poll.
    */
   i830SetHotkeyControl(pScrn, HOTKEY_DRIVER_NOTIFY);

   /* Needed for rotation */
   IntelEmitInvarientState(pScrn);

   if (pI830->checkDevices)
      pI830->devicesTimer = TimerSet(NULL, 0, 1000, I830CheckDevicesTimer, pScrn);

   pI830->currentMode = pScrn->currentMode;

   /* Force invarient state when rotated to be emitted */
   *pI830->used3D = 1<<31;

   return TRUE;
}

static Bool
I830SwitchMode(int scrnIndex, DisplayModePtr mode, int flags)
{

   ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
   I830Ptr pI830 = I830PTR(pScrn);
   Bool ret = TRUE;
   PixmapPtr pspix = (*pScrn->pScreen->GetScreenPixmap) (pScrn->pScreen);

   DPRINTF(PFX, "I830SwitchMode: mode == %p\n", mode);

#ifdef I830_XV
   /* Give the video overlay code a chance to see the new mode. */
   I830VideoSwitchModeBefore(pScrn, mode);
#endif

   /* Sync the engine before mode switch */
   if (pI830->AccelInfoRec && pI830->AccelInfoRec->NeedToSync) {
      (*pI830->AccelInfoRec->Sync)(pScrn);
      pI830->AccelInfoRec->NeedToSync = FALSE;
   }

   /* Check if our currentmode is about to change. We do this so if we
    * are rotating, we don't need to call the mode setup again.
    */
   if (pI830->currentMode != mode) {
      if (!i830SetMode(pScrn, mode))
         ret = FALSE;
   }

   /* Kludge to detect Rotate or Vidmode switch. Not very elegant, but
    * workable given the implementation currently. We only need to call
    * the rotation function when we know that the framebuffer has been
    * disabled by the EnableDisableFBAccess() function.
    *
    * The extra WindowTable check detects a rotation at startup.
    */
   if ( (!WindowTable[pScrn->scrnIndex] || pspix->devPrivate.ptr == NULL) &&
         !pI830->DGAactive && (pScrn->PointerMoved == I830PointerMoved) &&
	 !IS_I965G(pI830)) {
      if (!I830Rotate(pScrn, mode))
         ret = FALSE;
   }

   /* Either the original setmode or rotation failed, so restore the previous
    * video mode here, as we'll have already re-instated the original rotation.
    */
   if (!ret) {
      if (!i830SetMode(pScrn, pI830->currentMode)) {
	 xf86DrvMsg(scrnIndex, X_INFO,
		    "Failed to restore previous mode (SwitchMode)\n");
      }

#ifdef I830_XV
      /* Give the video overlay code a chance to see the new mode. */
      I830VideoSwitchModeAfter(pScrn, pI830->currentMode);
#endif
   } else {
      pI830->currentMode = mode;

#ifdef I830_XV
      /* Give the video overlay code a chance to see the new mode. */
      I830VideoSwitchModeAfter(pScrn, mode);
#endif
   }

   return ret;
}

static Bool
I830SaveScreen(ScreenPtr pScreen, int mode)
{
   ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
   I830Ptr pI830 = I830PTR(pScrn);
   Bool on = xf86IsUnblank(mode);
   CARD32 temp, ctrl, base, surf;
   int i;

   DPRINTF(PFX, "I830SaveScreen: %d, on is %s\n", mode, BOOLTOSTRING(on));

   if (pScrn->vtSema) {
      for (i = 0; i < pI830->num_pipes; i++) {
        if (i == 0) {
	    ctrl = DSPACNTR;
	    base = DSPABASE;
	    surf = DSPASURF;
        } else {
	    ctrl = DSPBCNTR;
	    base = DSPBADDR;
	    surf = DSPBSURF;
        }
        if (pI830->pipes[i].enabled) {
	   temp = INREG(ctrl);
	   if (on)
	      temp |= DISPLAY_PLANE_ENABLE;
	   else
	      temp &= ~DISPLAY_PLANE_ENABLE;
	   OUTREG(ctrl, temp);
	   /* Flush changes */
	   temp = INREG(base);
	   OUTREG(base, temp);
	   if (IS_I965G(pI830)) {
	      temp = INREG(surf);
	      OUTREG(surf, temp);
	   }
        }
      }

      if (pI830->CursorInfoRec && !pI830->SWCursor && pI830->cursorOn) {
	 if (on)
	    pI830->CursorInfoRec->ShowCursor(pScrn);
	 else
	    pI830->CursorInfoRec->HideCursor(pScrn);
	 pI830->cursorOn = TRUE;
      }
   }
   return TRUE;
}

/* Use the VBE version when available. */
static void
I830DisplayPowerManagementSet(ScrnInfoPtr pScrn, int PowerManagementMode,
			      int flags)
{
   I830Ptr pI830 = I830PTR(pScrn);
   int i;
   CARD32 temp, ctrl, base;

   for (i = 0; i < pI830->num_outputs; i++) {
      pI830->output[i].dpms(pScrn, &pI830->output[i], PowerManagementMode);
   }

   for (i = 0; i < pI830->num_pipes; i++) {
      if (i == 0) {
         ctrl = DSPACNTR;
         base = DSPABASE;
      } else {
         ctrl = DSPBCNTR;
         base = DSPBADDR;
      }
      if (pI830->pipes[i].enabled) {
	   temp = INREG(ctrl);
	   if (PowerManagementMode == DPMSModeOn)
	      temp |= DISPLAY_PLANE_ENABLE;
	   else
	      temp &= ~DISPLAY_PLANE_ENABLE;
	   OUTREG(ctrl, temp);
	   /* Flush changes */
	   temp = INREG(base);
	   OUTREG(base, temp);
      }
   }

   if (pI830->CursorInfoRec && !pI830->SWCursor && pI830->cursorOn) {
      if (PowerManagementMode == DPMSModeOn)
         pI830->CursorInfoRec->ShowCursor(pScrn);
      else
         pI830->CursorInfoRec->HideCursor(pScrn);
      pI830->cursorOn = TRUE;
   }
}

static Bool
I830CloseScreen(int scrnIndex, ScreenPtr pScreen)
{
   ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
   I830Ptr pI830 = I830PTR(pScrn);
   XAAInfoRecPtr infoPtr = pI830->AccelInfoRec;

   pI830->closing = TRUE;
#ifdef XF86DRI
   if (pI830->directRenderingOpen) {
      if (pI830->mmModeFlags & I830_KERNEL_MM) {
#ifndef XSERVER_LIBDRM_MM
	 I830DrmMMTakedown(pI830->drmSubFD, DRM_BO_MEM_TT);
#else
	 drmMMTakedown(pI830->drmSubFD, DRM_BO_MEM_TT);	 
#endif
      }
      pI830->directRenderingOpen = FALSE;
      I830DRICloseScreen(pScreen);
   }
#endif

   if (pScrn->vtSema == TRUE) {
      I830LeaveVT(scrnIndex, 0);
   }

   if (pI830->devicesTimer)
      TimerCancel(pI830->devicesTimer);
   pI830->devicesTimer = NULL;

   DPRINTF(PFX, "\nUnmapping memory\n");
   I830UnmapMem(pScrn);
   vgaHWUnmapMem(pScrn);

   if (pI830->ScanlineColorExpandBuffers) {
      xfree(pI830->ScanlineColorExpandBuffers);
      pI830->ScanlineColorExpandBuffers = 0;
   }

   if (infoPtr) {
      if (infoPtr->ScanlineColorExpandBuffers)
	 xfree(infoPtr->ScanlineColorExpandBuffers);
      XAADestroyInfoRec(infoPtr);
      pI830->AccelInfoRec = NULL;
   }

   if (pI830->CursorInfoRec) {
      xf86DestroyCursorInfoRec(pI830->CursorInfoRec);
      pI830->CursorInfoRec = 0;
   }

   if (I830IsPrimary(pScrn)) {
      xf86GARTCloseScreen(scrnIndex);

      xfree(pI830->LpRing);
      pI830->LpRing = NULL;
      xfree(pI830->CursorMem);
      pI830->CursorMem = NULL;
      xfree(pI830->CursorMemARGB);
      pI830->CursorMemARGB = NULL;
      xfree(pI830->OverlayMem);
      pI830->OverlayMem = NULL;
      xfree(pI830->overlayOn);
      pI830->overlayOn = NULL;
      xfree(pI830->used3D);
      pI830->used3D = NULL;
   }

   pScrn->PointerMoved = pI830->PointerMoved;
   pScrn->vtSema = FALSE;
   pI830->closing = FALSE;
   pScreen->CloseScreen = pI830->CloseScreen;
   return (*pScreen->CloseScreen) (scrnIndex, pScreen);
}

static ModeStatus
I830ValidMode(int scrnIndex, DisplayModePtr mode, Bool verbose, int flags)
{
   if (mode->Flags & V_INTERLACE) {
      if (verbose) {
	 xf86DrvMsg(scrnIndex, X_PROBED,
		    "Removing interlaced mode \"%s\"\n", mode->name);
      }
      return MODE_BAD;
   }
   return MODE_OK;
}

#ifndef SUSPEND_SLEEP
#define SUSPEND_SLEEP 0
#endif
#ifndef RESUME_SLEEP
#define RESUME_SLEEP 0
#endif

/*
 * This function is only required if we need to do anything differently from
 * DoApmEvent() in common/xf86PM.c, including if we want to see events other
 * than suspend/resume.
 */
static Bool
I830PMEvent(int scrnIndex, pmEvent event, Bool undo)
{
   ScrnInfoPtr pScrn = xf86Screens[scrnIndex];
   I830Ptr pI830 = I830PTR(pScrn);

   DPRINTF(PFX, "Enter VT, event %d, undo: %s\n", event, BOOLTOSTRING(undo));
 
   switch(event) {
   case XF86_APM_SYS_SUSPEND:
   case XF86_APM_CRITICAL_SUSPEND: /*do we want to delay a critical suspend?*/
   case XF86_APM_USER_SUSPEND:
   case XF86_APM_SYS_STANDBY:
   case XF86_APM_USER_STANDBY:
      if (!undo && !pI830->suspended) {
	 pScrn->LeaveVT(scrnIndex, 0);
	 pI830->suspended = TRUE;
	 sleep(SUSPEND_SLEEP);
      } else if (undo && pI830->suspended) {
	 sleep(RESUME_SLEEP);
	 pScrn->EnterVT(scrnIndex, 0);
	 pI830->suspended = FALSE;
      }
      break;
   case XF86_APM_STANDBY_RESUME:
   case XF86_APM_NORMAL_RESUME:
   case XF86_APM_CRITICAL_RESUME:
      if (pI830->suspended) {
	 sleep(RESUME_SLEEP);
	 pScrn->EnterVT(scrnIndex, 0);
	 pI830->suspended = FALSE;
	 /*
	  * Turn the screen saver off when resuming.  This seems to be
	  * needed to stop xscreensaver kicking in (when used).
	  *
	  * XXX DoApmEvent() should probably call this just like
	  * xf86VTSwitch() does.  Maybe do it here only in 4.2
	  * compatibility mode.
	  */
	 SaveScreens(SCREEN_SAVER_FORCER, ScreenSaverReset);
      }
      break;
   /* This is currently used for ACPI */
   case XF86_APM_CAPABILITY_CHANGED:
#if 0
      /* If we had status checking turned on, turn it off now */
      if (pI830->checkDevices) {
         if (pI830->devicesTimer)
            TimerCancel(pI830->devicesTimer);
         pI830->devicesTimer = NULL;
         pI830->checkDevices = FALSE; 
      }
#endif
      if (!I830IsPrimary(pScrn))
         return TRUE;

      ErrorF("I830PMEvent: Capability change\n");

      I830CheckDevicesTimer(NULL, 0, pScrn);
      SaveScreens(SCREEN_SAVER_FORCER, ScreenSaverReset);
      break;
   default:
      ErrorF("I830PMEvent: received APM event %d\n", event);
   }
   return TRUE;
}

#if 0
/**
 * This function is used for testing of the screen detect functions from the
 * periodic timer.
 */
static void
i830MonitorDetectDebugger(ScrnInfoPtr pScrn)
{
   Bool found_crt;
   I830Ptr pI830 = I830PTR(pScrn);
   int start, finish, i;

   if (!pScrn->vtSema)
      return 1000;

   for (i = 0; i < pI830->num_outputs; i++) {
      enum output_status ret;
      char *result;

      start = GetTimeInMillis();
      ret = pI830->output[i].detect(pScrn, &pI830->output[i]);
      finish = GetTimeInMillis();

      if (ret == OUTPUT_STATUS_CONNECTED)
	 result = "connected";
      else if (ret == OUTPUT_STATUS_DISCONNECTED)
	 result = "disconnected";
      else
	 result = "unknown";

      xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Detected SDVO as %s in %dms\n",
		 result, finish - start);
   }
}
#endif

static CARD32
I830CheckDevicesTimer(OsTimerPtr timer, CARD32 now, pointer arg)
{
   ScrnInfoPtr pScrn = (ScrnInfoPtr) arg;
   I830Ptr pI830 = I830PTR(pScrn);
   CARD8 gr18;

   if (!pScrn->vtSema)
      return 1000;

#if 0
   i830MonitorDetectDebugger(pScrn);
#endif

   /* Check for a hotkey press report from the BIOS. */
   gr18 = pI830->readControl(pI830, GRX, 0x18);
   if ((gr18 & (HOTKEY_TOGGLE | HOTKEY_SWITCH)) != 0) {
      /* The user has pressed the hotkey requesting a toggle or switch.
       * Re-probe our connected displays and turn on whatever we find.
       *
       * In the future, we want the hotkey to dump down to a user app which
       * implements a sensible policy using RandR-1.2.  For now, all we get
       * is this.
       */
      I830ValidateXF86ModeList(pScrn, FALSE);
      xf86SwitchMode(pScrn->pScreen, pScrn->currentMode);

      /* Clear the BIOS's hotkey press flags */
      gr18 &= ~(HOTKEY_TOGGLE | HOTKEY_SWITCH);
      pI830->writeControl(pI830, GRX, 0x18, gr18);
   }

   return 1000;
}

void
I830InitpScrn(ScrnInfoPtr pScrn)
{
   pScrn->PreInit = I830PreInit;
   pScrn->ScreenInit = I830ScreenInit;
   pScrn->SwitchMode = I830SwitchMode;
   pScrn->AdjustFrame = i830AdjustFrame;
   pScrn->EnterVT = I830EnterVT;
   pScrn->LeaveVT = I830LeaveVT;
   pScrn->FreeScreen = I830FreeScreen;
   pScrn->ValidMode = I830ValidMode;
   pScrn->PMEvent = I830PMEvent;
}
