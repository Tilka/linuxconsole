/*
 *  linux/drivers/video/vfb.c -- Virtual frame buffer device
 *
 *      Copyright (C) 2000 James Simmons
 *
 *	Copyright (C) 1997 Geert Uytterhoeven
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/malloc.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <asm/uaccess.h>
#include <linux/fb.h>
#include <linux/init.h>

#include "fbcon.h"

    /*
     *  RAM we reserve for the frame buffer. This defines the maximum screen
     *  size
     *
     *  The default can be overridden if the driver is compiled as a module
     */

#define VIDEOMEMSIZE	(1*1024*1024)	/* 1 MB */

static void* videomemory;
static u_long videomemorysize = VIDEOMEMSIZE;
MODULE_PARM(videomemorysize, "l");
static struct fb_info fb_info;
static u32 vfb_pseudo_palette[17];

static struct fb_var_screeninfo vfb_default __initdata = {
    /* 640x480, 8 bpp */
    640, 480, 640, 480, 0, 0, 8, 0,
    {0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0},
    0, 0, -1, -1, 0, 20000, 64, 64, 32, 32, 64, 2,
    0, FB_VMODE_NONINTERLACED
};

#ifndef MODULE
/* default modedb mode */
static struct fb_videomode default_mode __initdata = {
    /* 640x480, 60 Hz, Non-Interlaced (25.172 MHz dotclock) */
    NULL, 60, 640, 480, 39722, 48, 16, 33, 10, 96, 2,
    0, FB_VMODE_NONINTERLACED
};
#endif

static struct fb_fix_screeninfo vfb_fix __initdata = {
    "Virtual FB", (unsigned long) NULL, 0, FB_TYPE_PACKED_PIXELS, 0,
    FB_VISUAL_PSEUDOCOLOR, 1, 1, 1, 0, (unsigned long) NULL, 0, FB_ACCEL_NONE
};

static int vfb_enable __initdata = 0;	/* disabled by default */
MODULE_PARM(vfb_enable, "i");

    /*
     *  Interface used by the world
     */
int vfb_init(void);
int vfb_setup(char*);

static int vfb_open(struct fb_info *info, int user);
static int vfb_release(struct fb_info *info, int user);
static int vfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info); 
static int vfb_set_par(struct fb_info *info);
static int vfb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
                         u_int transp, struct fb_info *info);
static int vfb_pan_display(struct fb_var_screeninfo *var, int con, 
			   struct fb_info *info); 
static int vfb_mmap(struct fb_info *info, struct file *file,
                    struct vm_area_struct *vma);

static struct fb_ops vfb_ops = {
    fb_open:		vfb_open, 
    fb_release:		vfb_release, 
    fb_get_fix:		fbgen_get_fix,
    fb_get_var:		fbgen_get_var,
    fb_check_var:	vfb_check_var, 
    fb_set_par:		vfb_set_par, 
    fb_setcolreg:	vfb_setcolreg,
    fb_pan_display:	vfb_pan_display, 
    fb_mmap:		vfb_mmap
};

    /*
     *  Internal routines
     */

static u_long get_line_length(int xres_virtual, int bpp)
{
    u_long length;

    length = xres_virtual * bpp;
    length = (length+31)&~31;
    length >>= 3;
    return(length);
}

    /*
     *  Open/Release the frame buffer device
     */

static int vfb_open(struct fb_info *info, int user)
{
    /*                                                                     
     *  Nothing, only a usage count for the moment                          
     */                                                                    

    MOD_INC_USE_COUNT;
    return(0);                              
}
        
static int vfb_release(struct fb_info *info, int user)
{
    MOD_DEC_USE_COUNT;
    return(0);                                                    
}

    /*
     *  This function must not write anything to hardware, it should only
     *  verify and adjust var. To verify that this var is valid we use 
     *  the dirver's native struct par to test but the current par must NOT
     *  be altered. See skeletonfb.c for better details. This driver lacks
     *  a par so it does only basic test.
     */

static int vfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
    u_long line_length;

    /*
     *  FB_VMODE_CONUPDATE and FB_VMODE_SMOOTH_XPAN are equal!
     *  as FB_VMODE_SMOOTH_XPAN is only used internally
     */

    if (var->vmode & FB_VMODE_CONUPDATE) {
	var->vmode |= FB_VMODE_YWRAP;
	var->xoffset = info->var.xoffset;
	var->yoffset = info->var.yoffset;
    }

    /*
     *  Some very basic checks
     */
    if (!var->xres)
	var->xres = 1;
    if (!var->yres)
	var->yres = 1;
    if (var->xres > var->xres_virtual)
	var->xres_virtual = var->xres;
    if (var->yres > var->yres_virtual)
	var->yres_virtual = var->yres;
    if (var->bits_per_pixel <= 1)
	var->bits_per_pixel = 1;
    else if (var->bits_per_pixel <= 8)
	var->bits_per_pixel = 8;
    else if (var->bits_per_pixel <= 16)
	var->bits_per_pixel = 16;
    else if (var->bits_per_pixel <= 24)
	var->bits_per_pixel = 24;
    else if (var->bits_per_pixel <= 32)
	var->bits_per_pixel = 32;
    else
	return -EINVAL;

    if (var->xres_virtual < var->xoffset + var->xres)
        var->xres_virtual = var->xoffset + var->xres;
    if (var->yres_virtual < var->yoffset + var->yres)
       var->yres_virtual = var->yoffset + var->yres;

    /*
     *  Memory limit
     */
    line_length = get_line_length(var->xres_virtual, var->bits_per_pixel);
    if (line_length*var->yres_virtual > videomemorysize)
	return -ENOMEM;

    /*
     * Now that we checked it we alter var. The reason being is that the video
     * mode passed in might not work but slight changes to it might make it 
     * work. This way we let the user know what is acceptable.
     */
    switch (var->bits_per_pixel) {
       case 1:
       case 8:
           var->red.offset = 0;
           var->red.length = 8;
           var->green.offset = 0;
           var->green.length = 8;
           var->blue.offset = 0;
           var->blue.length = 8;
           var->transp.offset = 0;
           var->transp.length = 0;
           break;
       case 16:        /* RGBA 5551 */
           if (var->transp.length) {
               var->red.offset = 0;
               var->red.length = 5;
               var->green.offset = 5;
               var->green.length = 5;
               var->blue.offset = 10;
               var->blue.length = 5;
               var->transp.offset = 15;
               var->transp.length = 1;
           } else {    /* RGB 565 */
               var->red.offset = 0;
               var->red.length = 5;
               var->green.offset = 5;
               var->green.length = 6;
               var->blue.offset = 11;
               var->blue.length = 5;
               var->transp.offset = 0;
               var->transp.length = 0;
           }
           break;
       case 24:        /* RGB 888 */
           var->red.offset = 0;
           var->red.length = 8;
           var->green.offset = 8;
           var->green.length = 8;
           var->blue.offset = 16;
           var->blue.length = 8;
           var->transp.offset = 0;
           var->transp.length = 0;
           break;
       case 32:        /* RGBA 8888 */
           var->red.offset = 0;
           var->red.length = 8;
           var->green.offset = 8;
           var->green.length = 8;
           var->blue.offset = 16;
           var->blue.length = 8;
           var->transp.offset = 24;
           var->transp.length = 8;
           break;
    }
    var->red.msb_right = 0;
    var->green.msb_right = 0;
    var->blue.msb_right = 0;
    var->transp.msb_right = 0;

    return 0;
}

      /* 
       * This function actually sets the video mode. In fb_set_var the
       * new video mode we want to set the hardware to was already validate
       * and copied to fb_info in a struct fb_var_screeninfo format. If this 
       * was not the case we wouldn't be here. We then use the new var in 
       * fb_info to alter par in fb_info. Then using this par we can set a new
       * video mode. In the case of this driver we don't have a par struct. 	
       */

static int vfb_set_par(struct fb_info *info)
{
	info->fix.line_length = get_line_length(info->var.xres_virtual,
                                                info->var.bits_per_pixel);
	return 0;
}
	
    /*
     *  Set a single color register. The values supplied are already
     *  rounded down to the hardware's capabilities (according to the
     *  entries in the var structure). Return != 0 for invalid regno.
     */

static int vfb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
                         u_int transp, struct fb_info *info)
{
    if (regno >= 256)  /* no. of hw registers */
       return 1;
    /*
     * Program hardware... do anything you want with transp
     */

    /* grayscale works only partially under directcolor */    
    if (info->var.grayscale) { 
       /* grayscale = 0.30*R + 0.59*G + 0.11*B */
       red = green = blue = (red * 77 + green * 151 + blue * 28) >> 8;
    }	
    
    /* Directcolor:
     *   var->{color}.offset contains start of bitfield
     *   var->{color}.length contains length of bitfield
     *   {hardwarespecific} contains width of DAC
     *   cmap[X] is programmed to (X << red.offset) | (X << green.offset) | (X << blue.offset)
     *   DAC[X] is programmed to (red, green, blue)
     * 
     * Pseudocolor:
     *    uses offset = 0 && length = DAC register width.
     *    var->{color}.offset is 0
     *    var->{color}.length contains widht of DAC
     *    cmap is not used
     *    DAC[X] is programmed to (red, green, blue)
     * Truecolor:
     *    does not use DAC.
     *    var->{color}.offset contains start of bitfield
     *    var->{color}.length contains length of bitfield
     *    cmap is programmed to (red << red.offset) | (green << green.offset) |
     *                      (blue << blue.offset) | (transp << transp.offset)
     *    DAC does not exist
     */
#define CNVT_TOHW(val,width) ((((val)<<(width))+0x7FFF-(val))>>16)
    switch (info->fix.visual) {
       case FB_VISUAL_TRUECOLOR:
       case FB_VISUAL_PSEUDOCOLOR:
               red = CNVT_TOHW(red, info->var.red.length);
               green = CNVT_TOHW(green, info->var.green.length);
               blue = CNVT_TOHW(blue, info->var.blue.length);
               transp = CNVT_TOHW(transp, info->var.transp.length);
               break;
       case FB_VISUAL_DIRECTCOLOR:
               red = CNVT_TOHW(red, 8);        /* expect 8 bit DAC */
               green = CNVT_TOHW(green, 8);
               blue = CNVT_TOHW(blue, 8);
	       /* hey, there is bug in transp handling... */
               transp = CNVT_TOHW(transp, 8);
               break;
    }
#undef CNVT_TOHW
    /* Truecolor has hardware independent palette */
    if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
       u32 v;

       if (regno >= 16)
           return 1;

       v = (red << info->var.red.offset) |
           (green << info->var.green.offset) |
           (blue << info->var.blue.offset) |
           (transp << info->var.transp.offset);
       if (info->var.bits_per_pixel == 16)
           ((u16*)(info->pseudo_palette))[regno] = v;
       else
           ((u32*)(info->pseudo_palette))[regno] = v;
       return 0;
    }
    return 0;
}

    /*
     *  Pan or Wrap the Display
     *
     *  This call looks only at xoffset, yoffset and the FB_VMODE_YWRAP flag
     */

static int vfb_pan_display(struct fb_var_screeninfo *var, int con, 
			   struct fb_info *info) 
{
    if (var->vmode & FB_VMODE_YWRAP) {
	if (var->yoffset < 0 || var->yoffset >= info->var.yres_virtual ||
	    var->xoffset)
	    return -EINVAL;
    } else {
	if (var->xoffset + var->xres > info->var.xres_virtual ||
            var->yoffset + var->yres > info->var.yres_virtual)
            return -EINVAL;
    }
    info->var.xoffset = var->xoffset;
    info->var.yoffset = var->yoffset;
    if (var->vmode & FB_VMODE_YWRAP)
	info->var.vmode |= FB_VMODE_YWRAP;
    else
	info->var.vmode &= ~FB_VMODE_YWRAP;
    return 0;
}

    /*
     *  Most drivers don't need their own mmap function 
     */

static int vfb_mmap(struct fb_info *info, struct file *file,
                    struct vm_area_struct *vma)
{
    return -EINVAL;	
}

int __init vfb_setup(char *options)
{
    char *this_opt;

    vfb_enable = 1;

    if (!options || !*options)
	return 1;

    for (this_opt = strtok(options, ","); this_opt;
	 this_opt = strtok(NULL, ",")) {
	if (!strncmp(this_opt, "disable", 7))
	    vfb_enable = 0;	    
    }
    return 1;
}

    /*
     *  Initialisation
     */

int __init vfb_init(void)
{
    if (!vfb_enable)
	return -ENXIO;

    /*
     * For real video cards we use ioremap.
     */
    if (!(videomemory = vmalloc(videomemorysize)))
	return -ENOMEM;

    /*
     * VFB must clear memory to prevent kernel info
     * leakage into userspace
     * VGA-based drivers MUST NOT clear memory if
     * they want to be able to take over vgacon
     */
    memset(videomemory, 0, videomemorysize);
 
    fb_info.screen_base = videomemory;
    strcpy(fb_info.modename, vfb_fix.id);
    fb_info.node = -1;
    fb_info.fbops = &vfb_ops;

#ifndef MODULE
    if (!fb_find_mode(&fb_info.var, &fb_info, NULL,
            NULL, 0, &default_mode, 8)) {
    	fb_info.var = vfb_default;
    }
#else
    fb_info.var = vfb_default;
#endif
    fb_info.fix = vfb_fix;
    fb_info.switch_con = &fbgen_switch;
    fb_info.updatevar = &fbgen_update_var;
    fb_info.pseudo_palette = &vfb_pseudo_palette;	
    fb_info.flags = FBINFO_FLAG_DEFAULT;
  
    /* Alloc but do not set the default color map.
       vfb_set_var will do this for us */
    fb_info.cmap.len = 1<<fb_info.var.bits_per_pixel;
    fb_alloc_cmap(&fb_info.cmap, fb_info.cmap.len, 0);	 

    if (register_framebuffer(&fb_info) < 0) {
	vfree(videomemory);
	return -EINVAL;
    }

    printk(KERN_INFO "fb%d: Virtual frame buffer device, using %ldK of video memory\n", GET_FB_IDX(fb_info.node), videomemorysize>>10);
    return 0;
}

#ifdef MODULE
int init_module(void)
{
    return vfb_init();
}

void cleanup_module(void)
{
    unregister_framebuffer(&fb_info);
    vfree(videomemory);
}

#endif /* MODULE */
