/*
 * Copyright © 2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <X11/Xlibint.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/shmproto.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/record.h>
#include <X11/Xcursor/Xcursor.h>
#include <pixman.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/timerfd.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <getopt.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>

#if 0
#define DBG(x) printf x
#else
#define DBG(x)
#endif

struct display {
	Display *dpy;
	struct clone *clone;

	int damage_event, damage_error;
	int xfixes_event, xfixes_error;
	int rr_event, rr_error;
	Window root;
	Visual *visual;
	Damage damage;

	int depth;

	XRenderPictFormat *root_format;
	XRenderPictFormat *rgb16_format;
	XRenderPictFormat *rgb24_format;

	int has_shm;
	int has_shm_pixmap;
	int shm_opcode;
	int shm_event;

	Cursor invisible_cursor;
	Cursor visible_cursor;

	int cursor_x;
	int cursor_y;
	int cursor_moved;
	int cursor_visible;
	int cursor;

	int flush;
};

struct output {
	struct display *display;
	Display *dpy;
	char *name;
	RROutput rr_output;
	RRCrtc rr_crtc;
	XShmSegmentInfo shm;
	Window window;
	Picture win_picture;
	Picture pix_picture;
	Pixmap pixmap;
	GC gc;

	int serial;
	int use_shm;
	int use_shm_pixmap;

	XRenderPictFormat *use_render;

	int x, y;
	XRRModeInfo mode;
	Rotation rotation;
};

struct clone {
	struct clone *next;

	struct output src, dst;

	XShmSegmentInfo shm;
	XImage image;

	int width, height, depth;
	struct { int x1, x2, y1, y2; } damaged;
	int rr_update;
};

struct context {
	struct display *display;
	struct clone *clones;
	struct pollfd *pfd;
#define timer pfd[0].fd
	Display *record;
	int nclone;
	int ndisplay;
	int nfd;

	Atom singleton;
	char command[1024];
	int command_continuation;
};

static int xlib_vendor_is_xorg(Display *dpy)
{
	const char *const vendor = ServerVendor(dpy);
	return strstr(vendor, "X.Org") || strstr(vendor, "Xorg");
}

#define XORG_VERSION_ENCODE(major,minor,patch,snap) \
    (((major) * 10000000) + ((minor) * 100000) + ((patch) * 1000) + snap)

static int _x_error_occurred;

static int
_check_error_handler(Display     *display,
		     XErrorEvent *event)
{
	_x_error_occurred = 1;
	return False; /* ignored */
}

static int
can_use_shm(Display *dpy,
	    Window window,
	    int *shm_event,
	    int *shm_opcode,
	    int *shm_pixmap)
{
	XShmSegmentInfo shm;
	Status success;
	XExtCodes *codes;
	int major, minor, has_shm, has_pixmap;

	if (!XShmQueryExtension(dpy))
		return 0;

	XShmQueryVersion(dpy, &major, &minor, &has_pixmap);

	shm.shmid = shmget(IPC_PRIVATE, 0x1000, IPC_CREAT | 0600);
	if (shm.shmid == -1)
		return 0;

	shm.readOnly = 0;
	shm.shmaddr = shmat (shm.shmid, NULL, 0);
	if (shm.shmaddr == (char *) -1) {
		shmctl(shm.shmid, IPC_RMID, NULL);
		return 0;
	}

	XSync(dpy, False);
	_x_error_occurred = 0;

	success = XShmAttach(dpy, &shm);

	XSync(dpy, False);
	has_shm = success && _x_error_occurred == 0;

	codes = XInitExtension(dpy, SHMNAME);
	if (codes == NULL)
		has_pixmap = 0;

	/* As libXext sets the SEND_EVENT bit in the ShmCompletionEvent,
	 * the Xserver may crash if it does not take care when processing
	 * the event type. For instance versions of Xorg prior to 1.11.1
	 * exhibited this bug, and was fixed by:
	 *
	 * commit 2d2dce558d24eeea0eb011ec9ebaa6c5c2273c39
	 * Author: Sam Spilsbury <sam.spilsbury@canonical.com>
	 * Date:   Wed Sep 14 09:58:34 2011 +0800
	 *
	 * Remove the SendEvent bit (0x80) before doing range checks on event type.
	 */
	if (has_pixmap &&
	    xlib_vendor_is_xorg(dpy) &&
	    VendorRelease(dpy) < XORG_VERSION_ENCODE(1,11,0,1))
		has_pixmap = 0;

	if (has_pixmap) {
		XShmCompletionEvent e;

		e.type = codes->first_event;
		e.send_event = 1;
		e.serial = 1;
		e.drawable = window;
		e.major_code = codes->major_opcode;
		e.minor_code = X_ShmPutImage;

		e.shmseg = shm.shmid;
		e.offset = 0;

		XSendEvent(dpy, e.drawable, False, 0, (XEvent *)&e);

		XSync(dpy, False);
		has_pixmap = _x_error_occurred == 0;
	}

	if (success)
		XShmDetach(dpy, &shm);

	shmctl(shm.shmid, IPC_RMID, NULL);
	shmdt(shm.shmaddr);

	if (has_pixmap) {
		*shm_opcode = codes->major_opcode;
		*shm_event = codes->first_event;
		*shm_pixmap = has_pixmap;
	}

	return has_shm;
}

static int mode_equal(const XRRModeInfo *a, const XRRModeInfo *b)
{
	return (a->width == b->width &&
		a->height == b->height &&
		a->dotClock == b->dotClock &&
		a->hSyncStart == b->hSyncStart &&
		a->hSyncEnd == b->hSyncEnd &&
		a->hTotal == b->hTotal &&
		a->hSkew == b->hSkew &&
		a->vSyncStart == b->vSyncStart &&
		a->vSyncEnd == b->vSyncEnd &&
		a->vTotal == b->vTotal &&
		a->modeFlags == b->modeFlags);
}


static XRRModeInfo *lookup_mode(XRRScreenResources *res, int id)
{
	int i;

	for (i = 0; i < res->nmode; i++) {
		if (res->modes[i].id == id)
			return &res->modes[i];
	}

	return NULL;
}

static int clone_update_modes(struct clone *clone)
{
	XRRScreenResources *from_res, *to_res;
	XRROutputInfo *from_info, *to_info;
	int i, j, ret = ENOENT;

	assert(clone->src.rr_output);
	assert(clone->dst.rr_output);

	from_res = XRRGetScreenResources(clone->dst.dpy, clone->dst.window);
	if (from_res == NULL)
		goto err;

	from_info = XRRGetOutputInfo(clone->dst.dpy, from_res, clone->dst.rr_output);
	if (from_info == NULL)
		goto err;

	to_res = XRRGetScreenResourcesCurrent(clone->src.dpy, clone->src.window);
	if (to_res == NULL)
		goto err;

	to_info = XRRGetOutputInfo(clone->src.dpy, to_res, clone->src.rr_output);
	if (to_info == NULL)
		goto err;

	clone->dst.rr_crtc = from_info->crtc;

	/* Clear all current UserModes on the output, including any active ones */
	if (to_info->crtc) {
		DBG(("%s(%s-%s): disabling active CRTC\n", __func__,
		     DisplayString(clone->src.dpy), clone->src.name));
		XRRSetCrtcConfig(clone->src.dpy, to_res, to_info->crtc, CurrentTime,
				0, 0, None, RR_Rotate_0, NULL, 0);
	}
	for (i = 0; i < to_info->nmode; i++) {
		DBG(("%s(%s-%s): deleting mode %ld\n", __func__,
		     DisplayString(clone->src.dpy), clone->src.name, (long)to_info->modes[i]));
		XRRDeleteOutputMode(clone->src.dpy, clone->src.rr_output, to_info->modes[i]);
	}

	clone->src.rr_crtc = 0;

	/* Create matching modes for the real output on the virtual */
	for (i = 0; i < from_info->nmode; i++) {
		XRRModeInfo *mode, *old;
		RRMode id;

		mode = lookup_mode(from_res, from_info->modes[i]);
		if (mode == NULL)
			continue;
		for (j = 0; j < i; j++) {
			old = lookup_mode(from_res, from_info->modes[j]);
			if (old && mode_equal(mode, old)) {
				mode = NULL;
				break;
			}
		}
		if (mode == NULL)
			continue;

		id = 0;
		for (j = 0; j < to_res->nmode; j++) {
			old = &to_res->modes[j];
			if (mode_equal(mode, old)) {
				id = old->id;
				break;
			}
		}
		if (id == 0) {
			XRRModeInfo m;
			char buf[256];

			/* XXX User names must be unique! */
			m = *mode;
			m.nameLength = snprintf(buf, sizeof(buf),
						"%s.%ld-%s", clone->src.name, (long)from_info->modes[i], mode->name);
			m.name = buf;

			id = XRRCreateMode(clone->src.dpy, clone->src.window, &m);
		}

		XRRAddOutputMode(clone->src.dpy, clone->src.rr_output, id);
	}
	ret = 0;

err:
	if (to_info)
		XRRFreeOutputInfo(to_info);
	if (to_res)
		XRRFreeScreenResources(to_res);
	if (from_info)
		XRRFreeOutputInfo(from_info);
	if (from_res)
		XRRFreeScreenResources(from_res);

	return ret;
}

static RROutput claim_virtual(struct display *display, const char *name)
{
	char buf[] = "ClaimVirtualHead";
	Display *dpy = display->dpy;
	XRRScreenResources *res;
	XRRModeInfo mode;
	RRMode id;
	RROutput rr_output;
	int i;

	DBG(("%s(%s)\n", __func__, name));

	res = XRRGetScreenResourcesCurrent(dpy, display->root);
	if (res == NULL)
		return 0;

	for (i = rr_output = 0; rr_output == 0 && i < res->noutput; i++) {
		XRROutputInfo *o = XRRGetOutputInfo(dpy, res, res->outputs[i]);
		if (strcmp(o->name, name) == 0)
			rr_output = res->outputs[i];
		XRRFreeOutputInfo(o);
	}
	for (i = id = 0; id == 0 && i < res->nmode; i++) {
		if (strcmp(res->modes[i].name, buf) == 0)
			id = res->modes[i].id;
	}
	XRRFreeScreenResources(res);

	DBG(("%s(%s): rr_output=%ld\n", __func__, name, (long)rr_output));
	if (rr_output == 0)
		return 0;

	/* Set any mode on the VirtualHead to make the Xserver allocate another */
	memset(&mode, 0, sizeof(mode));
	mode.width = 1024;
	mode.height = 768;
	mode.name = buf;
	mode.nameLength = sizeof(buf) - 1;

	if (id == 0)
		id = XRRCreateMode(dpy, display->root, &mode);
	XRRAddOutputMode(dpy, rr_output, id);

	/* Force a redetection for the ddx to spot the new outputs */
	res = XRRGetScreenResources(dpy, display->root);
	if (res == NULL)
		return 0;
	XRRFreeScreenResources(res);

	XRRDeleteOutputMode(dpy, rr_output, id);
	XRRDestroyMode(dpy, id);

	return rr_output;
}

static int stride_for_depth(int width, int depth)
{
	if (depth == 24)
		depth = 32;
	return ((width * depth + 7) / 8 + 3) & ~3;
}

static void init_image(struct clone *clone)
{
	XImage *image = &clone->image;
	int ret;

	image->width = clone->width;
	image->height = clone->height;
	image->format = ZPixmap;
	image->xoffset = 0;
	image->byte_order = LSBFirst;
	image->bitmap_unit = 32;
	image->bitmap_bit_order = LSBFirst;
	image->bitmap_pad = 32;
	image->data = clone->shm.shmaddr;
	image->bytes_per_line = stride_for_depth(clone->width, clone->depth);
	switch (clone->depth) {
	case 24:
		image->red_mask = 0xff << 16;
		image->green_mask = 0xff << 8;
		image->blue_mask = 0xff << 0;;
		image->depth = 24;
		image->bits_per_pixel = 32;
		break;
	case 16:
		image->red_mask = 0x1f << 11;
		image->green_mask = 0x3f << 5;
		image->blue_mask = 0x1f << 0;;
		image->depth = 16;
		image->bits_per_pixel = 16;
		break;
	}

	ret = XInitImage(image);
	assert(ret);
}

static void output_init_xfer(struct clone *clone, struct output *output)
{
	if (output->use_shm_pixmap) {
		DBG(("%s-%s: creating shm pixmap\n", DisplayString(output->dpy), output->name));
		if (output->pixmap)
			XFreePixmap(output->dpy, output->pixmap);
		output->pixmap = XShmCreatePixmap(output->dpy, output->window,
						  clone->shm.shmaddr, &clone->shm,
						  clone->width, clone->height, clone->depth);
		if (output->pix_picture) {
			XRenderFreePicture(output->dpy, output->pix_picture);
			output->pix_picture = None;
		}
	}
	if (output->use_render) {
		DBG(("%s-%s: creating picture\n", DisplayString(output->dpy), output->name));
		if (output->win_picture == None)
			output->win_picture = XRenderCreatePicture(output->dpy, output->window,
								   output->display->root_format, 0, NULL);
		if (output->pixmap == None)
			output->pixmap = XCreatePixmap(output->dpy, output->window,
						       clone->width, clone->height, clone->depth);
		if (output->pix_picture == None)
			output->pix_picture = XRenderCreatePicture(output->dpy, output->pixmap,
								   output->use_render, 0, NULL);
	}

	if (output->gc == None) {
		XGCValues gcv;

		gcv.graphics_exposures = False;
		gcv.subwindow_mode = IncludeInferiors;

		output->gc = XCreateGC(output->dpy, output->pixmap ?: output->window, GCGraphicsExposures | GCSubwindowMode, &gcv);
	}
}

static int clone_init_xfer(struct clone *clone)
{
	if (clone->src.mode.id == 0) {
		if (clone->width == 0 && clone->height == 0)
			return 0;

		clone->width = 0;
		clone->height = 0;

		if (clone->src.use_shm)
			XShmDetach(clone->src.dpy, &clone->shm);
		if (clone->dst.use_shm)
			XShmDetach(clone->dst.dpy, &clone->shm);

		if (clone->shm.shmaddr) {
			shmdt(clone->shm.shmaddr);
			clone->shm.shmaddr = 0;
		}

		return 0;
	}

	if (clone->src.mode.width == clone->width &&
	    clone->src.mode.height == clone->height)
		return 0;

	DBG(("%s-%s create xfer\n",
	     DisplayString(clone->dst.dpy), clone->dst.name));

	clone->width = clone->src.mode.width;
	clone->height = clone->src.mode.height;

	if (clone->shm.shmaddr)
		shmdt(clone->shm.shmaddr);

	clone->shm.shmid = shmget(IPC_PRIVATE,
				  clone->height * stride_for_depth(clone->width, clone->depth),
				  IPC_CREAT | 0666);
	if (clone->shm.shmid == -1)
		return errno;

	clone->shm.shmaddr = shmat(clone->shm.shmid, 0, 0);
	if (clone->shm.shmaddr == (char *) -1) {
		shmctl(clone->shm.shmid, IPC_RMID, NULL);
		return ENOMEM;
	}

	clone->shm.readOnly = 0;

	init_image(clone);

	if (clone->src.use_shm) {
		XShmAttach(clone->src.dpy, &clone->shm);
		XSync(clone->src.dpy, False);
	}
	if (clone->dst.use_shm) {
		XShmAttach(clone->dst.dpy, &clone->shm);
		XSync(clone->dst.dpy, False);
	}

	shmctl(clone->shm.shmid, IPC_RMID, NULL);

	output_init_xfer(clone, &clone->src);
	output_init_xfer(clone, &clone->dst);

	clone->damaged.x1 = clone->src.x;
	clone->damaged.x2 = clone->src.x + clone->width;
	clone->damaged.y1 = clone->src.y;
	clone->damaged.y2 = clone->src.y + clone->height;

	clone->dst.display->flush = 1;
	return 0;
}

static void clone_update(struct clone *clone)
{
	if (!clone->rr_update)
		return;

	DBG(("%s-%s cloning modes\n",
	     DisplayString(clone->dst.dpy), clone->dst.name));

	clone_update_modes(clone);
	clone->rr_update = 0;
}

static void context_update(struct context *ctx)
{
	Display *dpy = ctx->display->dpy;
	XRRScreenResources *res;
	int context_changed = 0;
	int i, n;

	res = XRRGetScreenResourcesCurrent(dpy, ctx->display->root);
	if (res == NULL)
		return;

	for (n = 0; n < ctx->nclone; n++) {
		struct output *output = &ctx->clones[n].src;
		XRROutputInfo *o;
		XRRCrtcInfo *c;
		RRMode mode = 0;
		int changed = 0l;

		o = XRRGetOutputInfo(dpy, res, output->rr_output);
		if (o == NULL)
			continue;

		c = NULL;
		if (o->crtc)
			c = XRRGetCrtcInfo(dpy, res, o->crtc);
		if (c) {
			changed |= output->rotation |= c->rotation;
			output->rotation = c->rotation;

			changed |= output->x != c->x;
			output->x = c->x;

			changed |= output->y != c->y;
			output->y = c->y;

			changed |= output->mode.id != mode;
			mode = c->mode;
			XRRFreeCrtcInfo(c);
		}
		output->rr_crtc = o->crtc;
		XRRFreeOutputInfo(o);

		if (mode) {
			if (output->mode.id != mode) {
				for (i = 0; i < res->nmode; i++) {
					if (res->modes[i].id == mode) {
						output->mode = res->modes[i];
						break;
					}
				}
			}
		} else {
			changed = output->mode.id != 0;
			output->mode.id = 0;
		}

		if (changed)
			clone_init_xfer(&ctx->clones[n]);
		context_changed |= changed;
	}
	XRRFreeScreenResources(res);

	if (!context_changed)
		return;

	for (n = 1; n < ctx->ndisplay; n++) {
		struct display *display = &ctx->display[n];
		struct clone *clone;
		int x1, x2, y1, y2;

		x1 = y1 = INT_MAX;
		x2 = y2 = INT_MIN;

		for (clone = display->clone; clone; clone = clone->next) {
			struct output *output = &clone->src;
			int v;

			assert(clone->dst.display == display);

			if (output->mode.id == 0)
				continue;

			DBG(("%s: source %s enabled (%d, %d)x(%d, %d)\n",
			     DisplayString(clone->dst.dpy), output->name,
			     output->x, output->y,
			     output->mode.width, output->mode.height));

			if (output->x < x1)
				x1 = output->x;
			if (output->y < y1)
				y1 = output->y;

			v = (int)output->x + output->mode.width;
			if (v > x2)
				x2 = v;
			v = (int)output->y + output->mode.height;
			if (v > y2)
				y2 = v;
		}

		DBG(("%s fb bounds (%d, %d)x(%d, %d)\n", DisplayString(display->dpy),
		     x1, y1, x2-x1, y2-y1));

		res = XRRGetScreenResourcesCurrent(display->dpy, display->root);
		if (res == NULL)
			continue;

		for (clone = display->clone; clone; clone = clone->next) {
			struct output *src = &clone->src;
			struct output *dst = &clone->dst;
			XRROutputInfo *o;
			struct clone *set;
			RRCrtc rr_crtc;

			DBG(("%s: copying configuration from %s (mode=%ld) to %s\n",
			     DisplayString(dst->dpy), src->name, (long)src->mode.id, dst->name));

			if (src->mode.id == 0) {
err:
				if (dst->rr_crtc) {
					DBG(("%s: disabling unused output '%s'\n",
					     DisplayString(dst->dpy), dst->name));
					XRRSetCrtcConfig(dst->dpy, res, dst->rr_crtc, CurrentTime,
							 0, 0, None, RR_Rotate_0, NULL, 0);
					dst->rr_crtc = 0;
					dst->mode.id = 0;
				}
				continue;
			}

			dst->x = src->x - x1;
			dst->y = src->y - y1;
			dst->rotation = src->rotation;
			dst->mode = src->mode;

			dst->mode.id = 0;
			for (i = 0; i < res->nmode; i++) {
				if (mode_equal(&src->mode, &res->modes[i])) {
					dst->mode.id = res->modes[i].id;
					break;
				}
			}
			if (dst->mode.id == 0) {
				DBG(("%s: failed to find suitable mode for %s\n",
				     DisplayString(dst->dpy), dst->name));
				goto err;
			}

			rr_crtc = dst->rr_crtc;
			if (rr_crtc) {
				for (set = display->clone; set != clone; set = set->next) {
					if (set->dst.rr_crtc == rr_crtc) {
						DBG(("%s: CRTC reassigned from %s\n",
						     DisplayString(dst->dpy), dst->name));
						rr_crtc = 0;
						break;
					}
				}
			}
			if (rr_crtc == 0) {
				o = XRRGetOutputInfo(dst->dpy, res, dst->rr_output);
				for (i = 0; i < o->ncrtc; i++) {
					DBG(("%s: checking whether CRTC:%ld is available\n",
					     DisplayString(dst->dpy), (long)o->crtcs[i]));
					for (set = display->clone; set != clone; set = set->next) {
						if (set->dst.rr_crtc == o->crtcs[i]) {
							DBG(("%s: CRTC:%ld already assigned to %s\n",
							     DisplayString(dst->dpy), (long)o->crtcs[i], set->dst.name));
							break;
						}
					}
					if (set == clone) {
						rr_crtc = o->crtcs[i];
						break;
					}
				}
				XRRFreeOutputInfo(o);
			}
			if (rr_crtc == 0) {
				DBG(("%s: failed to find availble CRTC for %s\n",
				     DisplayString(dst->dpy), dst->name));
				goto err;
			}

			DBG(("%s: enabling output '%s' (%d,%d)x(%d,%d) on CRTC:%ld\n",
			     DisplayString(dst->dpy), dst->name,
			     dst->x, dst->y, dst->mode.width, dst->mode.height, (long)rr_crtc));
			XRRSetCrtcConfig(dst->dpy, res, rr_crtc, CurrentTime,
					 dst->x, dst->y, dst->mode.id, dst->rotation,
					 &dst->rr_output, 1);
			dst->rr_crtc = rr_crtc;
		}

		XRRFreeScreenResources(res);
	}
}

static Cursor display_load_invisible_cursor(struct display *display)
{
	char zero[8] = {};
	XColor black = {};
	Pixmap bitmap = XCreateBitmapFromData(display->dpy, display->root, zero, 8, 8);
	return XCreatePixmapCursor(display->dpy, bitmap, bitmap, &black, &black, 0, 0);
}

static void display_load_visible_cursor(struct display *display, XFixesCursorImage *cur)
{
	XcursorImage image;

	memset(&image, 0, sizeof(image));
	image.width = cur->width;
	image.height = cur->height;
	image.size = image.width;
	if (image.height > image.size)
		image.size = image.height;
	image.xhot = cur->xhot;
	image.yhot = cur->yhot;
	image.pixels = (void *)cur->pixels;

	if (display->visible_cursor)
		XFreeCursor(display->dpy, display->visible_cursor);

	DBG(("%s updating cursor\n", DisplayString(display->dpy)));
	display->visible_cursor = XcursorImageLoadCursor(display->dpy, &image);

	display->cursor_moved++;
	display->cursor_visible += display->cursor != display->invisible_cursor;
}

static void display_cursor_move(struct display *display, int x, int y, int visible)
{
	display->cursor_moved++;
	display->cursor_visible += visible;
	if (visible) {
		display->cursor_x = x;
		display->cursor_y = y;
	}
}

static void display_flush_cursor(struct display *display)
{
	Cursor cursor;
	int x, y;

	if (!display->cursor_moved)
		return;

	if (display->cursor_visible) {
		x = display->cursor_x;
		y = display->cursor_y;
	} else {
		x = display->cursor_x++ & 31;
		y = display->cursor_y++ & 31;
	}

	XWarpPointer(display->dpy, None, display->root, 0, 0, 0, 0, x, y);
	display->flush = 1;

	cursor = None;
	if (display->cursor_visible)
		cursor = display->visible_cursor;
	if (cursor == None)
		cursor = display->invisible_cursor;
	if (cursor != display->cursor) {
		XDefineCursor(display->dpy, display->root, cursor);
		display->cursor = cursor;
	}

	display->cursor_moved = 0;
	display->cursor_visible = 0;
}

static void clone_move_cursor(struct clone *c, int x, int y)
{
	int visible;

	DBG(("%s-%s moving cursor (%d, %d) [(%d, %d), (%d, %d)]\n",
	     DisplayString(c->dst.dpy), c->dst.name,
	     x, y,
	     c->src.x, c->src.y,
	     c->src.x + c->width, c->src.y + c->height));

	visible = (x >= c->src.x && x < c->src.x + c->width &&
		   y >= c->src.y && y < c->src.y + c->height);

	x += c->dst.x - c->src.x;
	y += c->dst.y - c->src.y;

	display_cursor_move(c->dst.display, x, y, visible);
}

static int clone_output_init(struct clone *clone, struct output *output,
			     struct display *display, const char *name,
			     RROutput rr_output)
{
	Display *dpy = display->dpy;
	int depth;

	if (rr_output == 0)
		return -ENOENT;

	DBG(("%s(%s, %s)\n", __func__, DisplayString(dpy), name));

	output->name = strdup(name);
	if (output->name == NULL)
		return -ENOMEM;

	output->display = display;
	output->dpy = dpy;

	output->rr_output = rr_output;

	output->window = display->root;
	output->use_shm = display->has_shm;
	output->use_shm_pixmap = display->has_shm_pixmap;

	depth = output->use_shm ? display->depth : 16;
	if (depth < clone->depth)
		clone->depth = depth;

	return 0;
}

static void send_shm(struct output *o, int serial)
{
	XShmCompletionEvent e;

	if (o->display->shm_event == 0) {
		XSync(o->dpy, False);
		return;
	}

	e.type = o->display->shm_event;
	e.send_event = 1;
	e.serial = serial;
	e.drawable = o->pixmap;
	e.major_code = o->display->shm_opcode;
	e.minor_code = X_ShmPutImage;
	e.shmseg = 0;
	e.offset = 0;

	XSendEvent(o->dpy, o->window, False, 0, (XEvent *)&e);
	o->serial = serial;
}

static void get_src(struct clone *c, const XRectangle *clip)
{
	DBG(("%s-%s get_src(%d,%d)x(%d,%d)\n", DisplayString(c->dst.dpy), c->dst.name,
	     clip->x, clip->y, clip->width, clip->height));
	if (c->src.use_render) {
		XRenderComposite(c->src.dpy, PictOpSrc,
				 c->src.win_picture, 0, c->src.pix_picture,
				 clip->x, clip->y,
				 0, 0,
				 0, 0,
				 clip->width, clip->height);
		if (c->src.use_shm_pixmap) {
			XSync(c->src.dpy, False);
		} else if (c->src.use_shm) {
			c->image.width = clip->width;
			c->image.height = clip->height;
			c->image.obdata = (char *)&c->shm;
			XShmGetImage(c->src.dpy, c->src.pixmap, &c->image,
				     clip->x, clip->y, AllPlanes);
		} else {
			c->image.width = c->width;
			c->image.height = c->height;
			c->image.obdata = 0;
			XGetSubImage(c->src.dpy, c->src.pixmap,
				     clip->x, clip->y, clip->width, clip->height,
				     AllPlanes, ZPixmap,
				     &c->image, 0, 0);
		}
	} else if (c->src.pixmap) {
		XCopyArea(c->src.dpy, c->src.window, c->src.pixmap, c->src.gc,
			  clip->x, clip->y,
			  clip->width, clip->height,
			  0, 0);
		XSync(c->src.dpy, False);
	} else if (c->src.use_shm) {
		c->image.width = clip->width;
		c->image.height = clip->height;
		c->image.obdata = (char *)&c->shm;
		XShmGetImage(c->src.dpy, c->src.window, &c->image,
			     clip->x, clip->y, AllPlanes);
	} else {
		c->image.width = c->width;
		c->image.height = c->height;
		c->image.obdata = 0;
		XGetSubImage(c->src.dpy, c->src.window,
			     clip->x, clip->y, clip->width, clip->height,
			     AllPlanes, ZPixmap,
			     &c->image, 0, 0);
	}
}

static void put_dst(struct clone *c, const XRectangle *clip)
{
	DBG(("%s-%s put_dst(%d,%d)x(%d,%d)\n", DisplayString(c->dst.dpy), c->dst.name,
	     clip->x, clip->y, clip->width, clip->height));
	if (c->dst.use_render) {
		int serial;
		if (c->dst.use_shm_pixmap) {
		} else if (c->dst.use_shm) {
			c->image.width = clip->width;
			c->image.height = clip->height;
			c->image.obdata = (char *)&c->shm;
			XShmPutImage(c->dst.dpy, c->dst.pixmap, c->dst.gc, &c->image,
				     0, 0,
				     0, 0,
				     clip->width, clip->height,
				     False);
		} else {
			c->image.width = c->width;
			c->image.height = c->height;
			c->image.obdata = 0;
			XPutImage(c->dst.dpy, c->dst.pixmap, c->dst.gc, &c->image,
				  0, 0,
				  0, 0,
				  clip->width, clip->height);
		}
		serial = NextRequest(c->dst.dpy);
		XRenderComposite(c->dst.dpy, PictOpSrc,
				 c->dst.pix_picture, 0, c->dst.win_picture,
				 0, 0,
				 0, 0,
				 clip->x, clip->y,
				 clip->width, clip->height);
		if (c->dst.use_shm)
			send_shm(&c->dst, serial);
	} else if (c->dst.pixmap) {
		int serial = NextRequest(c->dst.dpy);
		XCopyArea(c->dst.dpy, c->dst.pixmap, c->dst.window, c->dst.gc,
			  0, 0,
			  clip->width, clip->height,
			  clip->x, clip->y);
		send_shm(&c->dst, serial);
	} else if (c->dst.use_shm) {
		c->image.width = clip->width;
		c->image.height = clip->height;
		c->image.obdata = (char *)&c->shm;
		c->dst.serial = NextRequest(c->dst.dpy);
		XShmPutImage(c->dst.dpy, c->dst.window, c->dst.gc, &c->image,
			     0, 0,
			     clip->x, clip->y,
			     clip->width, clip->height,
			     True);
	} else {
		c->image.width = c->width;
		c->image.height = c->height;
		c->image.obdata = 0;
		XPutImage(c->dst.dpy, c->dst.window, c->dst.gc, &c->image,
			  0, 0,
			  clip->x, clip->y,
			  clip->width, clip->height);
		c->dst.serial = 0;
	}

	c->dst.display->flush = 1;
}

static int clone_paint(struct clone *c)
{
	XRectangle clip;

	DBG(("%s-%s paint clone\n",
	     DisplayString(c->dst.dpy), c->dst.name));

	if (c->damaged.x1 < c->src.x)
		c->damaged.x1 = c->src.x;
	if (c->damaged.x2 > c->src.x + c->width)
		c->damaged.x2 = c->src.x + c->width;
	if (c->damaged.x2 <= c->damaged.x1)
		goto done;

	if (c->damaged.y1 < c->src.y)
		c->damaged.y1 = c->src.y;
	if (c->damaged.y2 > c->src.y + c->height)
		c->damaged.y2 = c->src.y + c->height;
	if (c->damaged.y2 <= c->damaged.y1)
		goto done;

	if (c->dst.serial > LastKnownRequestProcessed(c->dst.dpy))
		return EAGAIN;

	clip.x = c->damaged.x1;
	clip.y = c->damaged.y1;
	clip.width  = c->damaged.x2 - c->damaged.x1;
	clip.height = c->damaged.y2 - c->damaged.y1;
	get_src(c, &clip);

	clip.x += c->dst.x - c->src.x;
	clip.y += c->dst.y - c->src.y;
	put_dst(c, &clip);

done:
	c->damaged.x2 = c->damaged.y2 = INT_MIN;
	c->damaged.x1 = c->damaged.y1 = INT_MAX;
	return 0;
}

static void clone_damage(struct clone *c, const XRectangle *rec)
{
	if (rec->x < c->damaged.x1)
		c->damaged.x1 = rec->x;
	if (rec->x + rec->width > c->damaged.x2)
		c->damaged.x2 = rec->x + rec->width;
	if (rec->y < c->damaged.y1)
		c->damaged.y1 = rec->y;
	if (rec->y + rec->height > c->damaged.y2)
		c->damaged.y2 = rec->y + rec->height;
}

static void usage(const char *arg0)
{
	printf("usage: %s [-d <source display>] [-b] [<target display>]...\n", arg0);
}

static void record_callback(XPointer closure, XRecordInterceptData *data)
{
	struct context *ctx = (struct context *)closure;
	int n;

	if (data->category == XRecordFromServer) {
		const xEvent *e = (const xEvent *)data->data;

		if (e->u.u.type == MotionNotify) {
			for (n = 0; n < ctx->nclone; n++)
				clone_move_cursor(&ctx->clones[n],
						  e->u.keyButtonPointer.rootX,
						  e->u.keyButtonPointer.rootY);
		}
	}

	XRecordFreeData(data);
}

static int record_mouse(struct context *ctx)
{
	Display *dpy;
	XRecordRange *rr;
	XRecordClientSpec rcs;
	XRecordContext rc;

	DBG(("%s(%s)\n", __func__, DisplayString(ctx->display[0].dpy)));

	dpy = XOpenDisplay(DisplayString(ctx->display[0].dpy));
	if (dpy == NULL)
		return -ECONNREFUSED;

	rr = XRecordAllocRange();
	if (rr == NULL)
		return -ENOMEM;

	rr->device_events.first = rr->device_events.last = MotionNotify;

	rcs = XRecordAllClients;
	rc = XRecordCreateContext(dpy, 0, &rcs, 1, &rr, 1);

	XSync(dpy, False);

	if (!XRecordEnableContextAsync(dpy, rc, record_callback, (XPointer)ctx))
		return -EINVAL;

	ctx->record = dpy;
	return ConnectionNumber(dpy);
}

static int bad_visual(Visual *visual, int depth)
{
	switch (depth) {
	case 16: return (visual->bits_per_rgb != 6 ||
			 visual->red_mask != 0x1f << 11 ||
			 visual->green_mask != 0x3f << 5 ||
			 visual->blue_mask != 0x1f << 0);
	case 24: return (visual->bits_per_rgb != 8 ||
			 visual->red_mask != 0xff << 16 ||
			 visual->green_mask != 0xff << 8 ||
			 visual->blue_mask != 0xff << 0);
	default: return 0;
	}
}

static XRenderPictFormat *
find_xrender_format(Display *dpy, pixman_format_code_t format)
{
    XRenderPictFormat tmpl;
    int mask;

#define MASK(x) ((1<<(x))-1)

    memset(&tmpl, 0, sizeof(tmpl));

    tmpl.depth = PIXMAN_FORMAT_DEPTH(format);
    mask = PictFormatType | PictFormatDepth;

    DBG(("%s(0x%08lx)\n", __func__, (long)format));

    switch (PIXMAN_FORMAT_TYPE(format)) {
    case PIXMAN_TYPE_ARGB:
	tmpl.type = PictTypeDirect;

	if (PIXMAN_FORMAT_A(format)) {
		tmpl.direct.alphaMask = MASK(PIXMAN_FORMAT_A(format));
		tmpl.direct.alpha = (PIXMAN_FORMAT_R(format) +
				     PIXMAN_FORMAT_G(format) +
				     PIXMAN_FORMAT_B(format));
	}

	tmpl.direct.redMask = MASK(PIXMAN_FORMAT_R(format));
	tmpl.direct.red = (PIXMAN_FORMAT_G(format) +
			   PIXMAN_FORMAT_B(format));

	tmpl.direct.greenMask = MASK(PIXMAN_FORMAT_G(format));
	tmpl.direct.green = PIXMAN_FORMAT_B(format);

	tmpl.direct.blueMask = MASK(PIXMAN_FORMAT_B(format));
	tmpl.direct.blue = 0;

	mask |= PictFormatRed | PictFormatRedMask;
	mask |= PictFormatGreen | PictFormatGreenMask;
	mask |= PictFormatBlue | PictFormatBlueMask;
	mask |= PictFormatAlpha | PictFormatAlphaMask;
	break;

    case PIXMAN_TYPE_ABGR:
	tmpl.type = PictTypeDirect;

	if (tmpl.direct.alphaMask) {
		tmpl.direct.alphaMask = MASK(PIXMAN_FORMAT_A(format));
		tmpl.direct.alpha = (PIXMAN_FORMAT_B(format) +
				     PIXMAN_FORMAT_G(format) +
				     PIXMAN_FORMAT_R(format));
	}

	tmpl.direct.blueMask = MASK(PIXMAN_FORMAT_B(format));
	tmpl.direct.blue = (PIXMAN_FORMAT_G(format) +
			    PIXMAN_FORMAT_R(format));

	tmpl.direct.greenMask = MASK(PIXMAN_FORMAT_G(format));
	tmpl.direct.green = PIXMAN_FORMAT_R(format);

	tmpl.direct.redMask = MASK(PIXMAN_FORMAT_R(format));
	tmpl.direct.red = 0;

	mask |= PictFormatRed | PictFormatRedMask;
	mask |= PictFormatGreen | PictFormatGreenMask;
	mask |= PictFormatBlue | PictFormatBlueMask;
	mask |= PictFormatAlpha | PictFormatAlphaMask;
	break;

    case PIXMAN_TYPE_BGRA:
	tmpl.type = PictTypeDirect;

	tmpl.direct.blueMask = MASK(PIXMAN_FORMAT_B(format));
	tmpl.direct.blue = (PIXMAN_FORMAT_BPP(format) - PIXMAN_FORMAT_B(format));

	tmpl.direct.greenMask = MASK(PIXMAN_FORMAT_G(format));
	tmpl.direct.green = (PIXMAN_FORMAT_BPP(format) - PIXMAN_FORMAT_B(format) -
			     PIXMAN_FORMAT_G(format));

	tmpl.direct.redMask = MASK(PIXMAN_FORMAT_R(format));
	tmpl.direct.red = (PIXMAN_FORMAT_BPP(format) - PIXMAN_FORMAT_B(format) -
			   PIXMAN_FORMAT_G(format) - PIXMAN_FORMAT_R(format));

	if (tmpl.direct.alphaMask) {
		tmpl.direct.alphaMask = MASK(PIXMAN_FORMAT_A(format));
		tmpl.direct.alpha = 0;
	}

	mask |= PictFormatRed | PictFormatRedMask;
	mask |= PictFormatGreen | PictFormatGreenMask;
	mask |= PictFormatBlue | PictFormatBlueMask;
	mask |= PictFormatAlpha | PictFormatAlphaMask;
	break;

    case PIXMAN_TYPE_A:
	tmpl.type = PictTypeDirect;

	tmpl.direct.alpha = 0;
	tmpl.direct.alphaMask = MASK(PIXMAN_FORMAT_A(format));

	mask |= PictFormatAlpha | PictFormatAlphaMask;
	break;

    case PIXMAN_TYPE_COLOR:
    case PIXMAN_TYPE_GRAY:
	/* XXX Find matching visual/colormap */
	tmpl.type = PictTypeIndexed;
	//tmpl.colormap = screen->visuals[PIXMAN_FORMAT_VIS(format)].vid;
	//mask |= PictFormatColormap;
	return NULL;
    }
#undef MASK

    return XRenderFindFormat(dpy, mask, &tmpl, 0);
}

static int display_init_render(struct display *display, int depth, XRenderPictFormat **use_render)
{
	Display *dpy = display->dpy;
	int major, minor;

	DBG(("%s is depth %d, want %d\n", DisplayString(dpy), display->depth, depth));

	*use_render = 0;
	if (depth == display->depth && !bad_visual(display->visual, depth))
		return 0;

	if (display->root_format == 0) {
		if (!XRenderQueryVersion(dpy, &major, &minor)) {
			fprintf(stderr, "Render extension not supported by %s\n", DisplayString(dpy));
			return -EINVAL;
		}

		display->root_format = XRenderFindVisualFormat(dpy, display->visual);
		display->rgb16_format = find_xrender_format(dpy, PIXMAN_r5g6b5);
		display->rgb24_format = XRenderFindStandardFormat(dpy, PictStandardRGB24);

		DBG(("%s: root format=%lx, rgb16 format=%lx, rgb24 format=%lx\n",
		     DisplayString(dpy),
		     (long)display->root_format,
		     (long)display->rgb16_format,
		     (long)display->rgb24_format));
	}

	switch (depth) {
	case 16: *use_render = display->rgb16_format; break;
	case 24: *use_render = display->rgb24_format; break;
	}
	if (*use_render == 0)
		return -ENOENT;

	return 0;
}

static int clone_init_depth(struct clone *clone)
{
	int ret, depth;

	DBG(("%s-%s wants depth %d\n",
	     DisplayString(clone->dst.dpy), clone->dst.name, clone->depth));

	for (depth = clone->depth; depth <= 24; depth += 8) {
		ret = display_init_render(clone->src.display, depth, &clone->src.use_render);
		if (ret)
			continue;

		ret = display_init_render(clone->dst.display, depth, &clone->dst.use_render);
		if (ret)
			continue;

		break;
	}
	if (ret)
		return ret;

	DBG(("%s-%s using depth %d, requires xrender for src? %d, for dst? %d\n",
	     DisplayString(clone->dst.dpy), clone->dst.name,
	     clone->depth,
	     clone->src.use_render != NULL,
	     clone->dst.use_render != NULL));

	return 0;
}

static inline int is_power_of_2(unsigned long n)
{
	return n && ((n & (n - 1)) == 0);
}

static int add_display(struct context *ctx, Display *dpy)
{
	struct display *display;

	if (is_power_of_2(ctx->ndisplay)) {
		ctx->display = realloc(ctx->display, 2*ctx->ndisplay*sizeof(struct display));
		if (ctx->display == NULL)
			return -ENOMEM;
	}

	display = memset(&ctx->display[ctx->ndisplay++], 0, sizeof(struct display));

	display->dpy = dpy;

	display->root = DefaultRootWindow(dpy);
	display->depth = DefaultDepth(dpy, DefaultScreen(dpy));
	display->visual = DefaultVisual(dpy, DefaultScreen(dpy));

	display->has_shm = can_use_shm(dpy, display->root,
				       &display->shm_event,
				       &display->shm_opcode,
				       &display->has_shm_pixmap);

	if (!XRRQueryExtension(dpy, &display->rr_event, &display->rr_error)) {
		fprintf(stderr, "RandR extension not supported by %s\n", DisplayString(dpy));
		return -EINVAL;
	}

	display->invisible_cursor = display_load_invisible_cursor(display);
	display->cursor = None;

	return ConnectionNumber(dpy);
}

static int display_open(struct context *ctx, const char *name)
{
	Display *dpy;
	int n;

	DBG(("%s(%s)\n", __func__, name));

	dpy = XOpenDisplay(name);
	if (dpy == NULL) {
		fprintf(stderr, "Unable to connect to %s\n", name);
		return -ECONNREFUSED;
	}

	/* Prevent cloning the same display twice */
	for (n = 0; n < ctx->ndisplay; n++) {
		if (strcmp(DisplayString(dpy), DisplayString(ctx->display[n].dpy)) == 0) {
			XCloseDisplay(dpy);
			return -EBUSY;
		}
	}

	return add_display(ctx, dpy);
}

static int bumblebee_open(struct context *ctx)
{
	char buf[256];
	struct sockaddr_un addr;
	int fd, len;

	fd = socket(PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		goto err;

	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, optarg && *optarg ? optarg : "/var/run/bumblebee.socket");
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		goto err;

	/* Ask bumblebee to start the second server */
	buf[0] = 'C';
	if (send(fd, &buf, 1, 0) != 1 || (len = recv(fd, &buf, 255, 0)) <= 0) {
		close(fd);
		goto err;
	}
	buf[len] = '\0';

	/* Query the display name */
	strcpy(buf, "Q VirtualDisplay");
	if (send(fd, buf, 17, 0) != 17 || (len = recv(fd, buf, 255, 0)) <= 0)
		goto err;
	buf[len] = '\0';
	close(fd);

	if (strncmp(buf, "Value: ", 7))
		goto err;

	while (isspace(buf[--len]))
		buf[len] = '\0';

	return display_open(ctx, buf+7);

err:
	fprintf(stderr, "Unable to connect to bumblebee\n");
	return -ECONNREFUSED;
}

static int display_init_damage(struct display *display)
{
	DBG(("%s(%s)\n", __func__, DisplayString(display->dpy)));

	if (!XDamageQueryExtension(display->dpy, &display->damage_event, &display->damage_error) ||
	    !XFixesQueryExtension(display->dpy, &display->xfixes_event, &display->xfixes_error)) {
		fprintf(stderr, "Damage/Fixes extension not supported by %s\n", DisplayString(display->dpy));
		return EINVAL;
	}

	display->damage = XDamageCreate(display->dpy, display->root, XDamageReportRawRectangles);
	if (display->damage == 0)
		return EACCES;

	return 0;
}

static int timerfd(int hz)
{
	struct itimerspec it;
	int fd;

	fd = timerfd_create(CLOCK_MONOTONIC_COARSE, TFD_NONBLOCK);
	if (fd < 0)
		fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (fd < 0)
		return -ETIME;

	it.it_interval.tv_sec = 0;
	it.it_interval.tv_nsec = 1000000000 / hz;
	it.it_value = it.it_interval;
	if (timerfd_settime(fd, 0, &it, NULL) < 0) {
		close(fd);
		return -ETIME;
	}

	return fd;
}

static int context_init(struct context *ctx)
{
	memset(ctx, 0, sizeof(*ctx));

	ctx->pfd = malloc(2*sizeof(struct pollfd));
	if (ctx->pfd == NULL)
		return -ENOMEM;

	ctx->clones = malloc(sizeof(struct clone));
	if (ctx->clones == NULL)
		return -ENOMEM;

	ctx->display = malloc(sizeof(struct display));
	if (ctx->display == NULL)
		return -ENOMEM;

	ctx->pfd[0].fd = timerfd(60);
	if (ctx->pfd[0].fd < 0)
		return ctx->pfd[0].fd;

	ctx->pfd[0].events = 0;
	ctx->pfd[0].revents = 0;
	ctx->nfd++;

	return 0;
}

static void context_build_lists(struct context *ctx)
{
	int n, m;

	for (n = 1; n < ctx->ndisplay; n++) {
		struct display *d = &ctx->display[n];

		d->clone = NULL;
		for (m = 0; m < ctx->nclone; m++) {
			struct clone *c = &ctx->clones[m];

			if (c->dst.display != d)
				continue;

			c->next = d->clone;
			d->clone = c;
		}
	}
}

static int add_fd(struct context *ctx, int fd)
{
	if (fd < 0)
		return fd;

	if (is_power_of_2(ctx->nfd)) {
		ctx->pfd = realloc(ctx->pfd, 2*ctx->nfd*sizeof(struct pollfd));
		if (ctx->pfd == NULL)
			return -ENOMEM;
	}

	ctx->pfd[ctx->nfd].fd = fd;
	ctx->pfd[ctx->nfd].events = POLLIN;
	ctx->pfd[ctx->nfd].revents = 0;
	ctx->nfd++;
	return 0;
}

static void display_init_randr_hpd(struct display *display)
{
	int major, minor;

	DBG(("%s(%s)\n", __func__, DisplayString(display->dpy)));

	if (!XRRQueryVersion(display->dpy, &major, &minor))
		return;

	if (major > 1 || (major == 1 && minor >= 2))
		XRRSelectInput(display->dpy, display->root, RROutputChangeNotifyMask);
}

static struct clone *add_clone(struct context *ctx)
{
	if (is_power_of_2(ctx->nclone)) {
		ctx->clones = realloc(ctx->clones, 2*ctx->nclone*sizeof(struct clone));
		if (ctx->clones == NULL)
			return NULL;
	}

	return memset(&ctx->clones[ctx->nclone++], 0, sizeof(struct clone));
}

static struct display *last_display(struct context *ctx)
{
	return &ctx->display[ctx->ndisplay-1];
}

static int last_display_add_clones(struct context *ctx)
{
	struct display *display = last_display(ctx);
	XRRScreenResources *res;
	char buf[80];
	int i, ret;

	display_init_randr_hpd(display);

	res = XRRGetScreenResourcesCurrent(display->dpy, display->root);
	if (res == NULL)
		return -ENOMEM;

	for (i = 0; i < res->noutput; i++) {
		XRROutputInfo *o = XRRGetOutputInfo(display->dpy, res, res->outputs[i]);
		struct clone *clone = add_clone(ctx);

		clone->depth = 24;

		sprintf(buf, "VIRTUAL%d", ctx->nclone);
		ret = clone_output_init(clone, &clone->src, ctx->display, buf, claim_virtual(ctx->display, buf));
		if (ret) {
			fprintf(stderr, "Failed to find available VirtualHead \"%s\" for \"%s\" on display \"%s\"\n",
				buf, o->name, DisplayString(display->dpy));
			return ret;
		}

		ret = clone_output_init(clone, &clone->dst, display, o->name, res->outputs[i]);
		if (ret) {
			fprintf(stderr, "Failed to add output \"%s\" on display \"%s\"\n",
				o->name, DisplayString(display->dpy));
			return ret;
		}

		ret = clone_init_depth(clone);
		if (ret) {
			fprintf(stderr, "Failed to negotiate image format for display \"%s\"\n",
				DisplayString(display->dpy));
			return ret;
		}

		ret = clone_update_modes(clone);
		if (ret) {
			fprintf(stderr, "Failed to clone output \"%s\" from display \"%s\"\n",
				o->name, DisplayString(display->dpy));
			return ret;
		}

		XRRFreeOutputInfo(o);
	}
	XRRFreeScreenResources(res);
	return 0;
}

static int last_display_clone(struct context *ctx, int fd)
{
	if (fd < 0)
		goto err;

	fd = add_fd(ctx, fd);
	if (fd < 0)
		goto err;

	fd = last_display_add_clones(ctx);
	if (fd)
		goto err;

err:
	context_build_lists(ctx);
	return fd;
}

static int first_display_has_singleton(struct context *ctx)
{
	struct display *display = ctx->display;
	unsigned long nitems, bytes;
	unsigned char *prop;
	int format;
	Atom type;

	ctx->singleton = XInternAtom(display->dpy, "intel-virtual-output-singleton", False);

	XGetWindowProperty(display->dpy, display->root, ctx->singleton,
			   0, 0, 0, AnyPropertyType, &type, &format, &nitems, &bytes, &prop);
	DBG(("%s: singleton registered? %d\n", DisplayString(display->dpy), type != None));
	return type != None;
}

static int first_display_wait_for_ack(struct context *ctx, int timeout, int id)
{
	struct display *display = ctx->display;
	struct pollfd pfd;
	char expect[5];

	sprintf(expect, "%04xR", id);
	DBG(("%s: wait for act '%c%c%c%c%c'\n",
	     DisplayString(display->dpy),
	     expect[0], expect[1], expect[2], expect[3], expect[4]));

	XFlush(display->dpy);

	pfd.fd = ConnectionNumber(display->dpy);
	pfd.events = POLLIN;
	do {
		if (poll(&pfd, 1, timeout) <= 0)
			return -ETIME;

		while (XPending(display->dpy)) {
			XEvent e;
			XClientMessageEvent *cme;

			XNextEvent(display->dpy, &e);
			DBG(("%s: reading event type %d\n", DisplayString(display->dpy), e.type));

			if (e.type != ClientMessage)
				continue;

			cme = (XClientMessageEvent *)&e;
			if (cme->message_type != ctx->singleton)
				continue;
			if (cme->format != 8)
				continue;

			DBG(("%s: client message '%c%c%c%c%c'\n",
			     DisplayString(display->dpy),
			     cme->data.b[0],
			     cme->data.b[1],
			     cme->data.b[2],
			     cme->data.b[3],
			     cme->data.b[4]));
			if (memcmp(cme->data.b, expect, 5))
				continue;

			return -atoi(cme->data.b + 5);
		}
	} while(1);
}

#if defined(__GNUC__) && (__GNUC__ > 3)
__attribute__((format(gnu_printf, 3, 4)))
#endif
static int first_display_send_command(struct context *ctx, int timeout,
				      const char *format,
				      ...)
{
	struct display *display = ctx->display;
	char buf[1024], *b;
	int len, id;
	va_list va;

	id = rand() & 0xffff;
	sprintf(buf, "%04x", id);
	va_start(va, format);
	len = vsnprintf(buf+4, sizeof(buf)-4, format, va)+5;
	va_end(va);
	assert(len < sizeof(buf));

	DBG(("%s: send command '%s'\n", DisplayString(display->dpy), buf));

	b = buf;
	while (len) {
		XClientMessageEvent msg;
		int n = len;
		if (n > sizeof(msg.data.b))
			n = sizeof(msg.data.b);
		len -= n;

		msg.type = ClientMessage;
		msg.serial = 0;
		msg.message_type = ctx->singleton;
		msg.format = 8;
		memcpy(msg.data.b, b, n);
		b += n;

		XSendEvent(display->dpy, display->root, False, PropertyChangeMask, (XEvent *)&msg);
	}

	return first_display_wait_for_ack(ctx, timeout, id);
}

static void first_display_reply(struct context *ctx, int result)
{
	struct display *display = ctx->display;
	XClientMessageEvent msg;

	sprintf(msg.data.b, "%c%c%c%cR%d",
	     ctx->command[0],
	     ctx->command[1],
	     ctx->command[2],
	     ctx->command[3],
	     -result);

	DBG(("%s: send reply '%s'\n", DisplayString(display->dpy), msg.data.b));

	msg.type = ClientMessage;
	msg.serial = 0;
	msg.message_type = ctx->singleton;
	msg.format = 8;

	XSendEvent(display->dpy, display->root, False, PropertyChangeMask, (XEvent *)&msg);
	XFlush(display->dpy);
}

static void first_display_handle_command(struct context *ctx,
					 const char *msg)
{
	int len;

	DBG(("client message!\n"));

	for (len = 0; len < 20 && msg[len]; len++)
		;

	if (ctx->command_continuation + len > sizeof(ctx->command)) {
		ctx->command_continuation = 0;
		return;
	}

	memcpy(ctx->command + ctx->command_continuation, msg, len);
	ctx->command_continuation += len;

	if (len < 20) {
		ctx->command[ctx->command_continuation] = 0;
		DBG(("client command complete! '%s'\n", ctx->command));
		switch (ctx->command[4]) {
		case 'B':
			first_display_reply(ctx, last_display_clone(ctx, bumblebee_open(ctx)));
			break;
		case 'C':
			first_display_reply(ctx, last_display_clone(ctx, display_open(ctx, ctx->command + 5)));
			break;
		case 'P':
			first_display_reply(ctx, 0);
			break;
		case 'R':
			break;
		}
		ctx->command_continuation = 0;
		return;
	}
}

static int first_display_register_as_singleton(struct context *ctx)
{
	struct display *display = ctx->display;
	struct pollfd pfd;

	XChangeProperty(display->dpy, display->root, ctx->singleton,
			XA_STRING, 8, PropModeReplace, (unsigned char *)".", 1);
	XFlush(display->dpy);

	/* And eat the notify (presuming that it is ours!) */

	pfd.fd = ConnectionNumber(display->dpy);
	pfd.events = POLLIN;
	do {
		if (poll(&pfd, 1, 1000) <= 0) {
			fprintf(stderr, "Failed to register as singleton\n");
			return EBUSY;
		}

		while (XPending(display->dpy)) {
			XPropertyEvent pe;

			XNextEvent(display->dpy, (XEvent *)&pe);
			DBG(("%s: reading event type %d\n", DisplayString(display->dpy), pe.type));

			if (pe.type == PropertyNotify &&
			    pe.atom == ctx->singleton)
				return 0;
		}
	} while(1);
}

static void display_flush(struct display *display)
{
	display_flush_cursor(display);

	if (!display->flush)
		return;

	DBG(("%s(%s)\n", __func__, DisplayString(display->dpy)));

	XFlush(display->dpy);
	display->flush = 0;
}

int main(int argc, char **argv)
{
	struct context ctx;
	const char *src_name = NULL;
	uint64_t count;
	int enable_timer = 0;
	int i, ret, daemonize = 1, bumblebee = 0, singleton = 1;

	while ((i = getopt(argc, argv, "bd:fhS")) != -1) {
		switch (i) {
		case 'd':
			src_name = optarg;
			break;
		case 'f':
			daemonize = 0;
			break;
		case 'b':
			bumblebee = 1;
			break;
		case 'S':
			singleton = 0;
			break;
		case 'h':
		default:
			usage(argv[0]);
			exit(0);
		}
	}

	ret = context_init(&ctx);
	if (ret)
		return -ret;

	XSetErrorHandler(_check_error_handler);

	ret = add_fd(&ctx, display_open(&ctx, src_name));
	if (ret)
		return -ret;

	if (singleton) {
		XSelectInput(ctx.display->dpy, ctx.display->root, PropertyChangeMask);
		if (first_display_has_singleton(&ctx)) {
			DBG(("%s: pinging singleton\n", DisplayString(ctx.display->dpy)));
			ret = first_display_send_command(&ctx, 2000, "P");
			if (ret) {
				if (ret != -ETIME)
					return -ret;
				DBG(("No reply from singleton; assuming control\n"));
			} else {
				DBG(("%s: singleton active, sending open commands\n", DisplayString(ctx.display->dpy)));
				if (optind == argc || bumblebee) {
					ret = first_display_send_command(&ctx, 5000, "B");
					if (ret && ret != -EBUSY)
						return -ret;
				}
				for (i = optind; i < argc; i++) {
					ret = first_display_send_command(&ctx, 5000, "C%s", argv[i]);
					if (ret && ret != -EBUSY)
						return -ret;
				}

				return 0;
			}
		}
		ret = first_display_register_as_singleton(&ctx);
		if (ret)
			return ret;
	}

	ret = display_init_damage(ctx.display);
	if (ret)
		return ret;

	XRRSelectInput(ctx.display->dpy, ctx.display->root, RRScreenChangeNotifyMask);
	XFixesSelectCursorInput(ctx.display->dpy, ctx.display->root, XFixesDisplayCursorNotifyMask);

	if (optind == argc || bumblebee) {
		ret = last_display_clone(&ctx, bumblebee_open(&ctx));
		if (ret) {
			if (!bumblebee) {
				usage(argv[0]);
				return 0;
			}
			return -ret;
		}
	}

	for (i = optind; i < argc; i++) {
		ret = last_display_clone(&ctx, display_open(&ctx, argv[i]));
		if (ret) {
			if (ret == -EBUSY)
				continue;
			return -ret;
		}
	}

	ret = add_fd(&ctx, record_mouse(&ctx));
	if (ret) {
		fprintf(stderr, "XTEST extension not supported by display \"%s\"\n", DisplayString(ctx.display[0].dpy));
		return -ret;
	}

	if (daemonize && daemon(0, 0))
		return EINVAL;

	while (1) {
		XEvent e;
		int reconfigure = 0;

		ret = poll(ctx.pfd + !enable_timer, ctx.nfd - !enable_timer, -1);
		if (ret <= 0)
			break;

		if (ctx.pfd[1].revents) {
			int damaged = 0;

			do {
				XNextEvent(ctx.display->dpy, &e);

				if (e.type == ctx.display->damage_event + XDamageNotify ) {
					const XDamageNotifyEvent *de = (const XDamageNotifyEvent *)&e;
					for (i = 0; i < ctx.nclone; i++)
						clone_damage(&ctx.clones[i], &de->area);
					if (!enable_timer)
						enable_timer = read(ctx.timer, &count, sizeof(count)) > 0;
					damaged++;
				} else if (e.type == ctx.display->xfixes_event + XFixesCursorNotify) {
					XFixesCursorImage *cur;

					cur = XFixesGetCursorImage(ctx.display->dpy);
					if (cur == NULL)
						continue;

					for (i = 1; i < ctx.ndisplay; i++)
						display_load_visible_cursor(&ctx.display[i], cur);

					XFree(cur);
				} else if (e.type == ctx.display->rr_event + RRScreenChangeNotify) {
					reconfigure = 1;
					if (!enable_timer)
						enable_timer = read(ctx.timer, &count, sizeof(count)) > 0;
				} else if (e.type == PropertyNotify) {
					XPropertyEvent *pe = (XPropertyEvent *)&e;
					if (pe->atom == ctx.singleton) {
						DBG(("lost control of singleton\n"));
						return 0;
					}
				} else if (e.type == ClientMessage) {
					XClientMessageEvent *cme;

					cme = (XClientMessageEvent *)&e;
					if (cme->message_type != ctx.singleton)
						continue;
					if (cme->format != 8)
						continue;

					first_display_handle_command(&ctx, cme->data.b);
				} else {
					DBG(("unknown event %d\n", e.type));
				}
			} while (XPending(ctx.display->dpy) || poll(&ctx.pfd[1], 1, 0) > 0);

			if (damaged)
				XDamageSubtract(ctx.display->dpy, ctx.display->damage, None, None);
			ret--;
		}

		for (i = 1; ret && i < ctx.ndisplay; i++) {
			if (ctx.pfd[i+1].revents == 0)
				continue;

			do {
				XNextEvent(ctx.display[i].dpy, &e);

				if (e.type == ctx.display[i].rr_event + RRNotify) {
					XRRNotifyEvent *re = (XRRNotifyEvent *)&e;
					if (re->subtype == RRNotify_OutputChange) {
						XRROutputPropertyNotifyEvent *ro = (XRROutputPropertyNotifyEvent *)re;
						int j;

						for (j = 0; j < ctx.nclone; j++) {
							if (ctx.clones[j].dst.display == &ctx.display[i] &&
							    ctx.clones[j].dst.rr_output == ro->output)
								ctx.clones[j].rr_update = 1;
						}
					}
				}
			} while (XPending(ctx.display[i].dpy) || poll(&ctx.pfd[i+1], 1, 0) > 0);

			ret--;
		}

		if (reconfigure)
			context_update(&ctx);

		for (i = 0; i < ctx.nclone; i++)
			clone_update(&ctx.clones[i]);

		if (enable_timer && read(ctx.timer, &count, sizeof(count)) > 0 && count > 0) {
			ret = 0;
			for (i = 0; i < ctx.nclone; i++)
				ret |= clone_paint(&ctx.clones[i]);
			enable_timer = ret != 0;
		}

		XPending(ctx.record);

		for (i = 0; i < ctx.ndisplay; i++)
			display_flush(&ctx.display[i]);
	}

	return EINVAL;
}
