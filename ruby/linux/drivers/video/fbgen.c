/*
 * linux/drivers/video/fbgen.c -- Generic routines for frame buffer devices
 *
 *  Modified to new API May 2000 by James Simmons
 *
 *  Created 3 Jan 1998 by Geert Uytterhoeven
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/fb.h>
#include <linux/slab.h>

#include <asm/uaccess.h>
#include <asm/io.h>

#include "fbcon.h"

/* ---- `Generic' versions of the frame buffer device operations ----------- */

    /*
     *  Get the Fixed Part of the Display
     */

int fbgen_get_fix(struct fb_fix_screeninfo *fix, struct fb_info *info)
{
    *fix = info->fix;	    
    return 0;
}

    /*
     *  Get the User Defined Part of the Display
     */

int fbgen_get_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
    *var = info->var;
    return 0;
}

    /*
     *  Set the User Defined Part of the Display
     */

int fbgen_set_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
    int err, oldbpp;
	
    if (memcmp(info->var, var, sizeof(var)) {	
	if ((err = info->fbops->fb_check_var(var, info)))
		return err;

    	if ((var->activate & FB_ACTIVATE_MASK) == FB_ACTIVATE_NOW) {
		oldbpp = info->var.bits_per_pixel;
	    	info->fbops->fb_set_par(info); 
 
	    	if (oldbpp != var->bits_per_pixel) {
			if ((err = fb_set_cmap(&info->cmap, 1, info)))
            			return err;
      	    	}
	}
	var->activate = 0;
    }
    return 0;
}

    /*
     *  Blank the screen
     */

int fbgen_blank(int blank, struct fb_info *info)
{
    struct fb_cmap cmap;
    u16 black[16];

    if (info->fbops->fb_blank && !info->fbops->fb_blank(blank, info))
        return 0;
    if (blank) {
        memset(black, 0, 16*sizeof(u16));
        cmap.red = black;
        cmap.green = black;
        cmap.blue = black;
        cmap.transp = NULL;
        cmap.start = 0;
        cmap.len = 16;
        fb_set_cmap(&cmap, 1, info);
        return 0;
    }
    return 0;
}

    /*
     *  Pan or Wrap the Display
     *
     *  This call looks only at xoffset, yoffset and the FB_VMODE_YWRAP flag
     */

int fbgen_pan_display(struct fb_var_screeninfo *var, struct fb_info *info) 
{
    int err;

    if (var->xoffset + info->var.xres > info->var.xres_virtual ||
	var->yoffset + info->var.yres > info->var.yres_virtual || 
	var->xoffset < 0 || var->yoffset < 0)
	return -EINVAL;
    if (info->fbops->fb_pan_display) {
    	if ((err = info->fbops->fb_pan_display(var, info)))
		return err;
	} else
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

void fbgen_set_disp(int con, struct fb_info *info)
{
    struct display *display;

    if (con >= 0)
	display = &fb_display[con];
    else
	display = info->disp;	/* used during initialization */

    display->screen_base = info->screen_base;	
    display->visual = info->fix.visual;
    display->type = info->fix.type;
    display->type_aux = info->fix.type_aux;
    display->ypanstep = info->fix.ypanstep;
    display->ywrapstep = info->fix.ywrapstep;
    display->line_length = info->fix.line_length;
    if (info->fbops->fb_blank || info->fix.visual == FB_VISUAL_PSEUDOCOLOR ||
	info->fix.visual == FB_VISUAL_DIRECTCOLOR)
	display->can_soft_blank = 1;
    else
	display->can_soft_blank = 0;

    switch (info->var.bits_per_pixel) {
#ifdef FBCON_HAS_MFB
	case 1:
            display->dispsw = &fbcon_mfb;
            break;
#endif
#ifdef FBCON_HAS_CFB2
        case 2:
            display->dispsw = &fbcon_cfb2;
            break;
#endif
#ifdef FBCON_HAS_CFB4
        case 4:
            display->dispsw = &fbcon_cfb4;
            break;
#endif
#ifdef FBCON_HAS_CFB8
        case 8:
            display->dispsw = &fbcon_cfb8;
            break;
#endif
#ifdef FBCON_HAS_CFB16
        case 16:
            display->dispsw = &fbcon_cfb16;
            display->dispsw_data = info->pseudo_palette;
            break;
#endif
#ifdef FBCON_HAS_CFB24
        case 24:
            display->dispsw = &fbcon_cfb24;
            display->dispsw_data = info->pseudo_palette;
            break;
#endif
#ifdef FBCON_HAS_CFB32
        case 32:
            display->dispsw = &fbcon_cfb32;
            display->dispsw_data = info->pseudo_palette;
            break;
#endif
        default:
            display->dispsw = &fbcon_dummy;
            break;
        }
}

    /*
     *  Update the `var' structure (called by fbcon.c)
     */

int fbgen_update_var(struct fb_info *info)
{
    int err;

    if (info->fbops->fb_pan_display) {
        if ((err = info->fbops->fb_pan_display(&info->var, info)))
            return err;
    }
    return 0;
}

    /*
     *  Switch to a different virtual console
     */

int fbgen_switch(int con, struct fb_info *info)
{
    struct display *prev = &fb_display[last_console];
    struct display *new = &fb_display[con];

    /* We might of VT switch from a non fbdev device */	
    if (prev->conp && prev->fb_info) {	
    	/* Save current video mode and colormap */
    	memcpy(&prev->var,&prev->fb_info->var,sizeof(struct fb_var_screeninfo));
    	fb_copy_cmap(&prev->fb_info->cmap, &prev->cmap, 0);
    }	     
    /*
     * Install new video mode and colormap.
     */
    new->fb_info->display_fg = vc_cons[con].d;
    new->fb_info->display_fg->vc_num = con;
    fb_copy_cmap(&new->cmap, &new->fb_info->cmap, 0);
    new->fb_info->fbops->fb_set_var(&new->var, con, new->fb_info);
    return 0;
}
