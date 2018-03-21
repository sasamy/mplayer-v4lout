#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <linux/videodev2.h>

#include "config.h"
#include "mp_msg.h"
#include "help_mp.h"
#include "subopt-helper.h"
#include "video_out.h"
#include "video_out_internal.h"
#include "aspect.h"
#include "sub/sub.h"

#define BUF_NUM 4
#define CLEAR(x) memset (&(x), 0, sizeof (x))

#define DEQUEUE_TIMES_IN_SHOW	16
#define WAIT_ON_DQUEUE_FAIL_IN_US	30000

static vo_info_t info =
{
	"v4l2 output",
	"v4lout",
	"Starterkit <info@starterkit.ru>",
	""
};

LIBVO_EXTERN(v4lout)

static uint32_t  l = 0;
static uint32_t  t = 0;
static uint32_t  w = 0;
static uint32_t  h = 0;

static char *fbdev = NULL;
static char *vdev = NULL;
static int fd_v4l = -1;

static struct v4l2_buffer v4lbuf = { 0 };
static uint8_t* frame[BUF_NUM] = { NULL };

static int image_width = 0;
static int image_height = 0;
static int stride_uv = 0;
static uint8_t *image_y = NULL;
static uint8_t *image_u = NULL;
static uint8_t *image_v = NULL;
static int chroma_x_shift = 0;
static int chroma_y_shift = 0;
static int v4l2_cap = 0;

static inline void setup_yuv_ptr(void)
{
	image_y = frame[v4lbuf.index];
	image_u = image_y + image_width * image_height;
	image_v = image_u + (image_width >> chroma_x_shift) * (image_height >> chroma_y_shift);
}

static int get_screeninfo(struct fb_var_screeninfo *fb_vinfo)
{
	int fd_fb = -1;
	int ret = 0;

	if (!fbdev)
		fbdev = strdup("/dev/fb0");

	fd_fb = open(fbdev, O_RDWR);
	if (fd_fb == -1) {
		mp_msg(MSGT_VO, MSGL_FATAL,
			"can't open %s: %s\n", fbdev, strerror(errno));
		ret = -1;
		goto out;
	}

	if (ioctl(fd_fb, FBIOGET_VSCREENINFO, fb_vinfo)) {
		mp_msg(MSGT_VO, MSGL_FATAL,
			"problem with FBITGET_VSCREENINFO ioctl: %s\n",
			strerror(errno));
		ret = -1;
	}

	close(fd_fb);

out:
	return ret;
}

static int get_v4ldev_cap(void)
{
	struct v4l2_capability cap;
	int ret = 0;

	if (!vdev)
		vdev = strdup("/dev/video0");

	fd_v4l = open(vdev, O_RDWR | O_NONBLOCK, 0);
	if (fd_v4l == -1) {
		mp_msg(MSGT_VO, MSGL_FATAL, "can't open %s: %s\n",
			vdev, strerror(errno));
		ret = -1;
		goto out;
	}

	if (ioctl(fd_v4l, VIDIOC_QUERYCAP, &cap) < 0) {
		mp_msg(MSGT_VO, MSGL_FATAL, "query cap failed\n");
		ret = -1;
		goto out;
	}

	v4l2_cap = cap.capabilities &
		(V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_VIDEO_OUTPUT_OVERLAY);

	if (!v4l2_cap) {
		mp_msg(MSGT_VO, MSGL_FATAL,
			"video output overlay not detected\n");
		ret = -1;
	}

out:
	return ret;
}

static int crop_init(struct v4l2_crop *crop)
{
	struct v4l2_output output;
	struct v4l2_framebuffer fb;
	int out_idx = 1;
	int ret = 0;

	if (v4l2_cap & V4L2_CAP_VIDEO_OUTPUT_OVERLAY) {
		if (ioctl(fd_v4l, VIDIOC_S_OUTPUT, &out_idx) < 0) {
			mp_msg(MSGT_VO, MSGL_FATAL, "failed to set output\n");
			ret = -1;
			goto out;
		}

		output.index = out_idx;
		if (ioctl(fd_v4l, VIDIOC_ENUMOUTPUT, &output) < 0) {
			mp_msg(MSGT_VO, MSGL_FATAL, "failed to VIDIOC_ENUMOUTPUT\n");
			ret = -1;
			goto out;
		}

		fb.flags = V4L2_FBUF_FLAG_OVERLAY;
		fb.flags |= V4L2_FBUF_FLAG_GLOBAL_ALPHA;
		if (ioctl(fd_v4l, VIDIOC_S_FBUF, &fb) < 0) {
			mp_msg(MSGT_VO, MSGL_FATAL, "set fbuf failed\n");
			ret = -1;
			goto out;
		}
	}

	if (ioctl(fd_v4l, VIDIOC_S_CROP, crop) < 0) {
		mp_msg(MSGT_VO, MSGL_FATAL, "set crop failed\n");
		ret = -1;
	}

out:
	return ret;
}

static int set_color(void)
{
	struct v4l2_control ctrl;
	int ret = 0;

	/* Set background color */
	ctrl.id = V4L2_CID_PRIVATE_BASE + 1;
	ctrl.value = 0xFFFFEE;
	if (ioctl(fd_v4l, VIDIOC_S_CTRL, &ctrl) < 0) {
		mp_msg(MSGT_VO, MSGL_FATAL, "set ctrl %d failed\n",ctrl.id);
		ret = -1;
		goto out;
	}

	/* Set s0 color */
	ctrl.id = V4L2_CID_PRIVATE_BASE + 2;
	ctrl.value = 0xFFFFEE;
	if (ioctl(fd_v4l, VIDIOC_S_CTRL, &ctrl) < 0) {
		mp_msg(MSGT_VO, MSGL_FATAL, "set ctrl %d failed\n",ctrl.id);
		ret = -1;
	}

out:
	return ret;
}

static int set_v4lfmt(uint32_t outformat)
{
	struct v4l2_format fmt;
	int ret = 0;

	switch (outformat) {
	case IMGFMT_I420:
	case IMGFMT_IYUV:
	case IMGFMT_YV12:
		outformat = V4L2_PIX_FMT_YUV420;
		break;
	case IMGFMT_422P:
		outformat = V4L2_PIX_FMT_YUV422P;
		break;
	default:
		ret = -1;
		goto out;
	}

	CLEAR (fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = image_width;
	fmt.fmt.pix.height = image_height;
	fmt.fmt.pix.pixelformat = outformat;

	if (ioctl(fd_v4l, VIDIOC_S_FMT, &fmt) < 0) {
		mp_msg(MSGT_VO, MSGL_WARN, "set format failed \n");
		ret = -1;
		goto out;
	}

	if (ioctl(fd_v4l, VIDIOC_G_FMT, &fmt) < 0) {
		mp_msg(MSGT_VO, MSGL_ERR, "get format failed\n");
		ret = -1;
		goto out;
	}

	if (v4l2_cap & V4L2_CAP_VIDEO_OUTPUT_OVERLAY) {
		fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY;
		fmt.fmt.win.w.left = 0;
		fmt.fmt.win.w.top = 0;
		fmt.fmt.win.w.width = image_width;
		fmt.fmt.win.w.height = image_height;
		fmt.fmt.win.global_alpha = 1;
		fmt.fmt.win.chromakey = 1;

		if (ioctl(fd_v4l, VIDIOC_S_FMT, &fmt) < 0) {
			mp_msg(MSGT_VO, MSGL_ERR, "set format output overlay failed\n");
			ret = -1;
		}
	}

out:
	return ret;
}

static int buffers_init(void)
{
	struct v4l2_requestbuffers buf_req;
	struct timeval queuetime;
	int i;
	int ret = 0;

	CLEAR (buf_req);
	buf_req.count = BUF_NUM;
	buf_req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf_req.memory = V4L2_MEMORY_MMAP;

	if (ioctl(fd_v4l, VIDIOC_REQBUFS, &buf_req) < 0) {
		mp_msg(MSGT_VO, MSGL_FATAL,
				"%d hwbuffers request failed\n", BUF_NUM);
		ret = -1;
		goto out;
	}

	if (buf_req.count < 3) {
		mp_msg(MSGT_VO, MSGL_FATAL,
			"Insufficient buffer memory on %s\n", vdev);
		ret = -1;
		goto out;
	}

	for (i = 0; i < buf_req.count; i++) {
		CLEAR (v4lbuf);
		v4lbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		v4lbuf.memory = V4L2_MEMORY_MMAP;
		v4lbuf.index = i;

		if (ioctl(fd_v4l, VIDIOC_QUERYBUF, &v4lbuf) < 0) {
			mp_msg(MSGT_VO, MSGL_FATAL,
				"VIDIOC_QUERYBUF failed %d, device id: %d, err: %s\n",
			i, fd_v4l, strerror(errno));
			ret = -1;
			break;
		}

		frame[i] = mmap(NULL,
						v4lbuf.length,
						PROT_READ | PROT_WRITE,
						MAP_SHARED,
						fd_v4l,
						v4lbuf.m.offset);

		if (frame[i] == MAP_FAILED) {
			mp_msg(MSGT_VO, MSGL_FATAL, "mmap failed\n");
			ret = -1;
			break;
		}

		setup_yuv_ptr();

		if (i < (buf_req.count - 1)) {
			gettimeofday(&queuetime, NULL);
			v4lbuf.timestamp = queuetime;

			if (ioctl(fd_v4l, VIDIOC_QBUF, &v4lbuf) < 0) {
				mp_msg(MSGT_VO, MSGL_FATAL, "VIDIOC_QBUF failed\n");
				ret = -1;
				break;
			}
		}
	}

out:
	return ret;
}


static opt_t subopts[] = {
	{"vdev", OPT_ARG_MSTRZ, &vdev, NULL},
	{"fbdev", OPT_ARG_MSTRZ, &fbdev, NULL},
	{"l", OPT_ARG_INT, &l, (opt_test_f)int_non_neg},
	{"t", OPT_ARG_INT, &t, (opt_test_f)int_non_neg},
	{"w", OPT_ARG_INT, &w, (opt_test_f)int_non_neg},
	{"h", OPT_ARG_INT, &h, (opt_test_f)int_non_neg},
	{NULL}
};

static int preinit(const char *arg)
{
	struct fb_var_screeninfo fb_vinfo;
	static struct v4l2_crop crop;

	if (subopt_parse(arg, subopts) != 0) {
	  mp_msg(MSGT_VO, MSGL_HINT,
			  "\n-vo v4lout command line help:\n"
			  "Example: mplayer -vo v4lout:vdev=/dev/video1:fbdev=/dev/fb3:l=8:t=16:w=128:h=64\n"
			  "\nOptions:\n"
			  "  vdev - video out device(default: /dev/video0)\n"
			  "  fbdev - framebuffer device (screeninfo, default: /dev/fb0)\n"
			  "  t,l - top and left of the cropped window (default: 0,0)\n"
			  "  w,h - width and height of the cropped window (default: full screen)\n"
			  "\n" );
	}

	if (get_screeninfo(&fb_vinfo) < 0)
		goto out;

	if (get_v4ldev_cap() < 0)
		goto out;

	CLEAR (crop);
	crop.type = (v4l2_cap & V4L2_CAP_VIDEO_OUTPUT_OVERLAY) ?
		V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY : V4L2_BUF_TYPE_VIDEO_OUTPUT;
	crop.c.width = w ? w : fb_vinfo.xres;
	crop.c.height = h ? h : fb_vinfo.yres;
	crop.c.top = t;
	crop.c.left = l;

	if (crop_init(&crop) < 0)
		goto out;

	if ((v4l2_cap & V4L2_CAP_VIDEO_OUTPUT_OVERLAY) && (set_color() < 0))
		goto out;

	return 0;

out:
	if (!(fd_v4l < 0)) {
		close(fd_v4l);
		fd_v4l = -1;
	}

	return -1;
}

static void uninit(void)
{
	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	if (ioctl(fd_v4l, VIDIOC_STREAMOFF, &type) < 0)
		mp_msg(MSGT_VO, MSGL_ERR, "Could not stream off\n");

	if (fbdev)
		free(fbdev);

	if (vdev)
		free(vdev);

	if (!(fd_v4l < 0)) {
		close(fd_v4l);
		fd_v4l = -1;
	}
}

static void check_events(void)
{
  /* do nothing */
}

/* Attempt to start doing DR */
static uint32_t get_image (mp_image_t *mpi)
{
	if (!(mpi->flags & MP_IMGFLAG_PLANAR))
		return VO_FALSE;

	mpi->planes[0] = image_y;
	mpi->planes[1] = image_u;
	mpi->planes[2] = image_v;
	mpi->stride[0] = image_width;
	mpi->stride[1] = mpi->stride[2] = stride_uv;
	mpi->flags |= MP_IMGFLAG_DIRECT;

	return VO_TRUE;
}

static uint32_t draw_image(mp_image_t *mpi) {
	/* if -dr or -slices then do nothing: */
	if (!(mpi->flags & (MP_IMGFLAG_DIRECT | MP_IMGFLAG_DRAW_CALLBACK))) {
		draw_slice(mpi->planes, mpi->stride, mpi->w, mpi->h, 0, 0);
	}

	return VO_TRUE;
}

static int query_format(uint32_t format)
{
	int ret = VO_FALSE;

	if (!(set_v4lfmt(format) < 0))
		ret = VFCAP_CSP_SUPPORTED |
			VFCAP_CSP_SUPPORTED_BY_HW |
			VFCAP_HWSCALE_UP |
			VFCAP_HWSCALE_DOWN |
			VFCAP_ACCEPT_STRIDE;

	return ret;
}

static int control(uint32_t request, void *data)
{
	switch (request) {
	case VOCTRL_GET_IMAGE:
		return get_image(data);
	case VOCTRL_DRAW_IMAGE:
		return draw_image(data);
	case VOCTRL_QUERY_FORMAT:
		return query_format(*((uint32_t*)data));
	}

	return VO_NOTIMPL;
}

static int config(uint32_t width,
				  uint32_t height,
				  uint32_t d_width,
				  uint32_t d_height,
				  uint32_t flags,
				  char *title,
				  uint32_t format)
{
	int ret = 0;

	image_width = width;
	image_height = height;

	if (set_v4lfmt(format) < 0) {
		mp_msg(MSGT_VO, MSGL_ERR, "Format not supported\n");
		ret = -1;
		goto out;
	}

	switch (format) {
	case IMGFMT_I420:
	case IMGFMT_IYUV:
	case IMGFMT_YV12:
		chroma_x_shift = 1;
		chroma_y_shift = 1;
		break;
	case IMGFMT_422P:
		chroma_x_shift = 1;
		chroma_y_shift = 0;
		break;
	default:
		mp_msg(MSGT_VO, MSGL_ERR, "Format not supported\n");
		ret = -1;
		goto out;
	}

	stride_uv = width >> chroma_x_shift;

	if (!buffers_init()) {
		int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

		if (ioctl(fd_v4l, VIDIOC_STREAMON, &type) < 0) {
			mp_msg(MSGT_VO, MSGL_FATAL, "Could not stream on\n");
			ret = -1;
			goto out;
		}
		mp_msg(MSGT_VO, MSGL_STATUS, "Set to Stream ON successfully\n");
	}

out:
	return ret;
}

static int draw_slice(uint8_t *srcimg[], int stride[], int w, int h, int x, int y)
{
	int i;
	uint8_t *srcy = srcimg[0];
	uint8_t *srcu = srcimg[1];
	uint8_t *srcv = srcimg[2];
	uint8_t *dsty = image_y + image_width * y + x;
	uint8_t *dstu = image_u + stride_uv * (y >> chroma_y_shift) + (x >> chroma_x_shift);
	uint8_t *dstv = image_v + stride_uv * (y >> chroma_y_shift) + (x >> chroma_x_shift);

	for (i = 0; i < h; i++) {
		memcpy(dsty, srcy, w);
		srcy += stride[0];
		dsty += image_width;
	}

	for (i = 0; i < (h >> chroma_y_shift); i++) {
		memcpy(dstu, srcu, w >> chroma_x_shift);
		srcu += stride[1];
		dstu += stride_uv;

		memcpy(dstv, srcv, w >> chroma_x_shift);
		srcv += stride[2];
		dstv += stride_uv;
	}

	return 0;
}

static int draw_frame(uint8_t *src[])
{
	return -1;
}

static void draw_osd(void)
{
  /* do nothing */
}

/* Render onto the screen */
static void flip_page(void)
{
	struct timeval queuetime;
	int cnt = DEQUEUE_TIMES_IN_SHOW;

	gettimeofday(&queuetime, NULL);
	v4lbuf.timestamp = queuetime;

	if (ioctl(fd_v4l, VIDIOC_QBUF, &v4lbuf) < 0)
		mp_msg(MSGT_VO, MSGL_ERR, "VIDIOC_QBUF failed\n");

	CLEAR (v4lbuf);

	v4lbuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	v4lbuf.memory = V4L2_MEMORY_MMAP;

	while ((ioctl(fd_v4l, VIDIOC_DQBUF, &v4lbuf) < 0) && (--cnt))
		usleep(WAIT_ON_DQUEUE_FAIL_IN_US);

	setup_yuv_ptr();
}


