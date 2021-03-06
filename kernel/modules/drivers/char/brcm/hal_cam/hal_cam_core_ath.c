/*******************************************************************************************
 * Copyright 2010 Broadcom Corporation.  All rights reserved.
 *
 * Unless you and Broadcom execute a separate written software license agreement
 * governing use of this software, this software is licensed to you under the
 * terms of the GNU General Public License version 2, available at
 * http://www.gnu.org/copyleft/gpl.html (the "GPL").
 *
 * Notwithstanding the above, under no circumstances may you combine this software
 * in any way with any other Broadcom software provided under a license other than
 * the GPL, without Broadcom's express prior written consent.
 * *******************************************************************************************/

/*
 * *
 * *****************************************************************************
 * *
 * *  camera.c
 * *
 * *  PURPOSE:
 * *
 * *     This implements the driver for the stv0987 ISP  camera.
 * *
 * *  NOTES:
 * *
 * *****************************************************************************/

/* ---- Include Files ---------------------------------------------------- */

#include <linux/version.h>
//#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/poll.h>
#include <linux/sysctl.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#if 0
#include <mach/reg_camera.h>
#include <mach/reg_lcd.h>
#endif
#include <mach/reg_clkpwr.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/atomic.h>

#include <linux/semaphore.h>
//#include <linux/broadcom/types.h>
#include <linux/broadcom/bcm_major.h>
#include <linux/broadcom/hw_cfg.h>
#include <linux/broadcom/hal_camera.h>
#include <linux/broadcom/lcd.h>
#include <linux/broadcom/bcm_sysctl.h>
#include <plat/dma.h>
#include <linux/dma-mapping.h>
#include <linux/wakelock.h>
#include <linux/clk.h>
#include <mach/clkmgr.h>
#include <linux/regulator/consumer.h>
#include <plat/syscfg.h>
#if defined (CONFIG_CPU_FREQ_GOV_BCM21553)
#include <mach/bcm21553_cpufreq_gov.h>
#endif

/* For OSDAL MEM to MEM DMA */
#include <plat/types.h>
#include <plat/osdal_os_driver.h>
#include <plat/osdal_os_service.h>
#include <plat/dma_drv.h>


#include "hal_cam_drv_ath.h"
#include "camdrv_dev.h"
#include <plat/csl/csl_cam.h>

/* FIXME: Clear VSYNC interrupt explicitly until handler properly */
//#define CAM_BOOT_TIME_MEMORY_SIZE (640*480*4)
#define CAM_SEC_OFFSET (1024*1024*3/2)
#define CAM_NUM_VFVIDEO 4
#define XFR_SIZE_MAX (4095*4)
#define XFR_SIZE (4095)
#define MAX_QUEUE_SIZE 4
#define MAX_QUEUE_SIZE_MASK MAX_QUEUE_SIZE
#define SWAP(val)   cpu_to_le32(val)
#define SKIP_STILL_FRAMES 3
#define IF_NAME             "cami2c"
#define I2C_DRIVERID_CAM    0xf000

#define REFCAPTIME 80000000
#define SZ_BUFFER (1024 * 512 * 5)

struct clk *cam_clk;
/* Camera driver generic data. This contains camera variables that are common
 * across all instances */
enum camera_state {
	CAM_OFF,
	CAM_INIT,
	CAM_PAUSE,
	CAM_ON,
	CAM_STOPPING,
};

enum capture_mode {
	CAM_NONE,
	CAM_STILL,
	CAM_STREAM,
};

struct CAM_DATA{
	int vsyncIrqs;
	int dmaCam;
	int level;
	int blocked;

};

struct CAM_DATA gCamState;

static struct ctl_table gSysCtlCam[] = {
	{
	 .procname = "level",
	 .data = &gCamState.level,
	 .maxlen = sizeof(int),
	 .mode = 0644,
	 .proc_handler = &proc_dointvec},
	{
	 .procname = "vsyncIrqs",
	 .data = &gCamState.vsyncIrqs,
	 .maxlen = sizeof(int),
	 .mode = 0644,
	 .proc_handler = &proc_dointvec},
	{
	 .procname = "dmaCam",
	 .data = &gCamState.dmaCam,
	 .maxlen = sizeof(int),
	 .mode = 0644,
	 .proc_handler = &proc_dointvec},
	{
	 .procname = "blocked",
	 .data = &gCamState.blocked,
	 .maxlen = sizeof(int),
	 .mode = 0644,
	 .proc_handler = &proc_dointvec},
	{}
};

static struct ctl_table gSysCtl[] = {
	{
	 .procname = "camera",
	 .mode = 0555,
	 .child = gSysCtlCam},
	{}
};

struct cam_dma_buf_t {
	size_t sizeInBytes;
	void *virt;
	dma_addr_t phy;
};
struct cam_i2c_info {
	struct i2c_client *client;
};

struct buf_q {
	CAM_BufData data[MAX_QUEUE_SIZE];
	unsigned int Num;
	unsigned int ReadIndex;
	unsigned int WriteIndex;
	spinlock_t lock;
	struct semaphore Sem;
	bool isActive;
	bool isWaitQueue;
};

struct camera_sensor_t {
	int devbusy;
	CamIntfConfig_st_t *sensor_intf;
	struct sens_methods *sens_m;
	CAM_Parm_t main;
	/* Either Still or VF/Video but not both at the same time.*/
	CAM_Parm_t th;
	/* Represents thumbnail only valid with still cap*/
	enum capture_mode mode;
	enum camera_state state;
	struct cam_dma_buf_t camb[CAM_NUM_VFVIDEO];
	struct cam_dma_buf_t cam_ll[CAM_NUM_VFVIDEO];
	atomic_t captured;
	int sCaptureFrameCountdown;
	int dma_chan;
	int still_ready;
	short *framebuf;
	int gProcessFrameRunning;
	int sCaptureJpegSize;
	int sCaptureRawSize;
	spinlock_t c_lock;
	struct buf_q rd_Q;
	struct buf_q wr_Q;
	CAM_BufData gCurrData;
	struct cam_i2c_info *cam_i2c_datap;
	ktime_t prev;
	CSL_CAM_INTF_CFG_st_t cslCamIntfCfg;
	CSL_CAM_HANDLE hdl;
	int cam_irq;
	u8 *tnptr;
	int bufsw;
	int drop_fps;
	CamZoom_t zoom;
};

struct cam_generic_t {
	struct class *cam_class;
	struct regulator *cam_regulator;
	struct regulator *cam_regulator1;
	struct wake_lock camera_wake_lock;
	struct ctl_table_header *gSysCtlHeader;
	CamSensorSelect_t curr;
	struct cam_dma_buf_t cam_buf;
	struct cam_dma_buf_t cam_buf_main;
	struct semaphore cam_sem;
	struct camera_sensor_t sens[2];
#if defined (CONFIG_CPU_FREQ_GOV_BCM21553)
	struct cpufreq_client_desc *cam_dvfs;
#endif
};

static struct cam_generic_t *cam_g;
DECLARE_WAIT_QUEUE_HEAD(gDataReadyQ);
static char banner[] __initdata =
     "Camera Driver: 1.00 (built on " __DATE__ " " __TIME__ ")\n";
/* Module static functions prototype begin*/
static int camera_enable(CamSensorSelect_t sensor);
static int camera_disable(CamSensorSelect_t sensor);
static int process_frame(CamSensorSelect_t sensor);
static int cam_power_down(CamSensorSelect_t sensor);
static int cam_power_up(CamSensorSelect_t sensor);
static int cam_open(struct inode *inode, struct file *file);
static int cam_release(struct inode *inode, struct file *file);
static long cam_ioctl(struct file *file, unsigned int cmd,
		     unsigned long arg);

static void push_queue(struct buf_q *queue, CAM_BufData * buf);
static void deinit_queue(struct buf_q *queue);
static void init_queue(struct buf_q *queue);
static bool pull_queue(struct buf_q *queue, CAM_BufData * buf);
static void wakeup_push_queue(struct buf_q *queue, CAM_BufData * buf);
static bool wait_pull_queue(struct buf_q *queue, CAM_BufData * buf);
static void taskcallback(UInt32 intr_status, UInt32 rx_status, UInt32 image_addr, UInt32 image_size, UInt32 raw_intr_status, UInt32 raw_rx_status, void *userdata);
static int mem_mem_dma(dma_addr_t dst_addr, dma_addr_t src_addr, int dma_tx_size);
extern struct sens_methods *CAMDRV_primary_get(void);
ktime_t t;

static ktime_t t_fs[5];
static ktime_t t_fe[5];
static int fs_cnt = 0;
static int fe_cnt = 0;

static void taskcallback(UInt32 intr_status, UInt32 rx_status, UInt32 image_addr, UInt32 image_size, UInt32 raw_intr_status, UInt32 raw_rx_status, void *userdata)
{
	struct camera_sensor_t *c = &cam_g->sens[cam_g->curr];
	int fs = 0, fe = 0;
	fs = intr_status & CSL_CAM_INT_FRAME_START;
	fe = intr_status & CSL_CAM_INT_FRAME_END;
	t = ktime_get();
	if((intr_status & CSL_CAM_INT_FRAME_START) && (intr_status & CSL_CAM_INT_FRAME_END)){
                printk("FS and FE **\n");
	}

	if((intr_status & CSL_CAM_INT_FRAME_START) && (c->mode == CAM_STREAM)) {
#ifdef CAM_CORRUPTION_CONCURRENCY
		writel(0x0300200f , io_p2v(BCM21553_MLARB_BASE + 0x100)); //BMARBL_MACONF0 
		writel(0x44444 , io_p2v(BCM21553_MLARB_BASE + 0x108)); //BMARBL_MACONF2
		udelay(5);
#endif
	}
	else if ((intr_status & CSL_CAM_INT_FRAME_START) && (c->mode == CAM_STILL)) {
		/*u32 rdr3_cnt = readl(io_p2v(0x08440094));
		pr_debug("################### Value of RDR3 in FS is 0x%x ###################\n", rdr3_cnt);*/
		t_fs[fs_cnt ++] = ktime_get();
	}
	if(intr_status & CSL_CAM_INT_FRAME_END) {

#ifdef CAM_CORRUPTION_CONCURRENCY
		/* check if the L-matrix register is changed by someone else */
		if(readl(io_p2v(BCM21553_MLARB_BASE + 0x100)) != 0x0300200f)
				pr_debug("****** L-Matrix register changed to 0x%x *******\n", readl(io_p2v(BCM21553_MLARB_BASE + 0x100)));
#endif
		if(c->mode == CAM_STREAM) {
			//wakeup_push_queue(&c->rd_Q, &c->gCurrData);
			//pull_queue(&c->wr_Q, &c->gCurrData);
#ifdef CAM_CORRUPTION_CONCURRENCY
			writel(0x3f0f, io_p2v(BCM21553_MLARB_BASE + 0x100)); //BMARBL_MACONF0 
#endif
			if((c->main.fps == 15) && (c->cslCamIntfCfg.intf == CSL_CAM_INTF_CSI)) {
				c->drop_fps ++;
				c->drop_fps %= 2;
				if (c->drop_fps)
					mem_mem_dma(c->gCurrData.busAddress,(dma_addr_t)image_addr,(c->main.size_window.end_pixel * c->main.size_window.end_line * 2));
			}
			else
				mem_mem_dma(c->gCurrData.busAddress,(dma_addr_t)image_addr,(c->main.size_window.end_pixel * c->main.size_window.end_line * 2));
			/* And then do what ?? */
		} else if(c->mode == CAM_STILL) {
			//pr_debug("FE\n");
			t_fe[fe_cnt ++] = ktime_get();
			if(c->state != CAM_PAUSE) {
				c->sCaptureFrameCountdown--;
			}
			//pr_debug("%s mode stream %d sec %d nsec %d\n",__FUNCTION__,c->sCaptureFrameCountdown,t.tv.sec,t.tv.nsec);
			if(c->sCaptureFrameCountdown <= 0) {
				if( (fs != 0) && (fe != 0)) {
					printk("Rejecting possible corruption :) \n");
				} else {
					if((image_addr == cam_g->cam_buf.phy) && ((intr_status & CSL_CAM_INT_FRAME_ERROR) == 0)) {
						u32 rdr3_cnt = readl(io_p2v(0x08440094));
						/* Got our JPEG */
						c->state = CAM_PAUSE;
						csl_cam_rx_stop(c->hdl);
						c->still_ready = 1;
						c->sCaptureJpegSize = image_size;
						printk("### RDR3 is 0x%x... RS0 is 0x%x ###\n", rdr3_cnt, raw_intr_status);
#ifdef CAM_CORRUPTION_CONCURRENCY
						writel(0x3f0f, io_p2v(BCM21553_MLARB_BASE + 0x100)); //BMARBL_MACONF0
#endif
						/* allow process context to process next frame */
						wake_up_interruptible(&gDataReadyQ);
					}
				}
			}
		}
	}
}

static void mem_mem_dma_isr(DMADRV_CALLBACK_STATUS_t status)
{
	struct camera_sensor_t *c = &cam_g->sens[0];
	if(status == DMADRV_CALLBACK_OK)
	{
		//pr_debug("Sending: 0x%x\n", c->gCurrData.busAddress);
		wakeup_push_queue(&c->rd_Q, &c->gCurrData);
		pull_queue(&c->wr_Q, &c->gCurrData);
	}
}


static int mem_mem_dma(dma_addr_t dst_addr, dma_addr_t src_addr, int dma_tx_size)
{
	DMA_CHANNEL gDmaChannel;
	OSDAL_Dma_Chan_Info dmaChInfoMem;
	OSDAL_Dma_Buffer_List dmaBuffListMem;
	OSDAL_Dma_Data dmaDataMem;
	struct camera_sensor_t *c = &cam_g->sens[0];

	if((dst_addr == (dma_addr_t)NULL) || (src_addr == (dma_addr_t)NULL))
		{
			pr_debug("Dst addr is zero, but no guy to receive\n");
			pull_queue(&c->wr_Q, &c->gCurrData);
			return 0;
		}
	/*Request the channel */
	if (OSDAL_ERR_OK != OSDAL_DMA_Obtain_Channel(OSDAL_DMA_CLIENT_MEMORY, OSDAL_DMA_CLIENT_MEMORY,(UInt32 *)&gDmaChannel))
	{
		pr_info("---->OSDAL_DMA_Obtain_Channel failed for channel %d \n",gDmaChannel);
		return -1;
	}

	/* Configuring the DMA channel info */
	dmaChInfoMem.type = OSDAL_DMA_FCTRL_MEM_TO_MEM;
	dmaChInfoMem.srcBstSize = OSDAL_DMA_BURST_SIZE_16;
	dmaChInfoMem.dstBstSize = OSDAL_DMA_BURST_SIZE_16;
	dmaChInfoMem.srcDataWidth = OSDAL_DMA_DATA_SIZE_32BIT;
	dmaChInfoMem.dstDataWidth = OSDAL_DMA_DATA_SIZE_32BIT;
	dmaChInfoMem.dstMaster = 1;
	dmaChInfoMem.srcMaster = 1;
	dmaChInfoMem.incMode = OSDAL_DMA_INC_MODE_BOTH;
	dmaChInfoMem.freeChan = TRUE;
	dmaChInfoMem.priority = 2;
	dmaChInfoMem.bCircular = FALSE;
	dmaChInfoMem.alignment = OSDAL_DMA_ALIGNMENT_32;
	dmaChInfoMem.xferCompleteCb = (OSDAL_DMA_CALLBACK)mem_mem_dma_isr;

	if (OSDAL_DMA_Config_Channel(gDmaChannel, &dmaChInfoMem)!= OSDAL_ERR_OK)
	{
		pr_info("---->DMADRV_Config_Channel - Failed for channel %d \n",gDmaChannel);
		return -1;
	}
	/* end of DMA channel configuration. */

	//Bind the channel
	dmaBuffListMem.buffers[0].srcAddr = src_addr;
	dmaBuffListMem.buffers[0].destAddr = dst_addr;
	dmaBuffListMem.buffers[0].length = dma_tx_size;
	dmaBuffListMem.buffers[0].bRepeat = 0;
	dmaBuffListMem.buffers[0].interrupt = 1;
	dmaDataMem.numBuffer = 1;
	dmaDataMem.pBufList = (OSDAL_Dma_Buffer_List *)&dmaBuffListMem;

	if (OSDAL_DMA_Bind_Data(gDmaChannel, &dmaDataMem) != OSDAL_ERR_OK)
	{
		pr_info("DMADRV_Bind_Data - Failed\n");
		return -EINVAL;
	}

	// Start transfer
	if (OSDAL_DMA_Start_Transfer(gDmaChannel) != OSDAL_ERR_OK)
	{
		pr_info("DMADRV_Start_Transfer - Failed for channel %d\n",gDmaChannel);
		return -EINVAL;
	}
	return 0;
}

/* Module static functions prototype end*/
static long cam_ioctl(struct file *file, unsigned int cmd,
		     unsigned long arg)
{
	int sensor = 0;
	int ret;
	int rc = 0;
	switch (cmd) {
	case CAM_IOCTL_ENABLE:
		{
			if (arg)
				camera_enable(sensor);
			else
				camera_disable(sensor);
			break;
		}
	case CAM_IOCTL_SET_PARAMS:
		{
			CAM_Parm_t parm;
			ret =
			    copy_from_user(&parm, (CAM_Parm_t *) arg,
					   sizeof(parm));
			if (ret != 0)
				return ret;
			printk("IOCTL set params called %d %d\n",parm.size_window.end_pixel,parm.size_window.end_line);
			cam_g->sens[sensor].main = parm;
			/* Old code calls CAM_Enable over here .... Not needed for this */
			break;
		}
	case CAM_IOCTL_SET_FPS:
		cam_g->sens[sensor].main.fps = (CamRates_t) arg;
		pr_debug("***** Requested FPS: %d\n", cam_g->sens[sensor].main.fps);
		/* Set it with the next enable */
		break;
	case CAM_IOCTL_SET_THUMBNAIL_PARAMS:
		if(copy_from_user(&cam_g->sens[sensor].th, (void *)arg,
			       sizeof(CAM_Parm_t)))
			return -EFAULT;
		break;
	case CAM_IOCTL_MEM_REGISTER:
		{
			CAM_BufData p;
			if(copy_from_user(&p, (CAM_BufData *) arg,
				       sizeof(CAM_BufData)))
			    return -EFAULT;
			cam_g->cam_buf.phy = (dma_addr_t) p.busAddress;
			cam_g->cam_buf_main.phy = (dma_addr_t) p.busAddress;
			cam_g->cam_buf_main.virt = __va(cam_g->cam_buf_main.phy);
			cam_g->cam_buf.virt = __va(cam_g->cam_buf_main.phy);
			break;
		}
	case CAM_IOCTL_MEM_BUFFERS:
		{
			CAM_BufData buf;
			if(copy_from_user(&buf, (CAM_BufData *) arg,
				       sizeof(CAM_BufData)))
			    return -EFAULT;
			if(buf.id >= CAM_NUM_VFVIDEO)
			    return -EFAULT;
			if (buf.busAddress ==
			    (void *)cam_g->sens[sensor].camb[buf.id].phy) {
				push_queue(&cam_g->sens[sensor].wr_Q, &buf);
			}
			else
				ret = -EFAULT;
			break;
		}
	case CAM_IOCTL_GET_FRAME:
		{
			CAM_BufData buf;
			/* For VF and Video */
			wait_pull_queue(&cam_g->sens[sensor].rd_Q, &buf);
			if(copy_to_user((void *)arg, &buf, sizeof(buf)))
			    return -EFAULT;
			break;
		}
	case CAM_IOCTL_GET_FRAME1:
		{
			/* Block for still capture */
			CAM_Frame1_t frame;
			int copy = 0;
			pr_debug("Get frame1 called arg 0x%x frame 0x%x\n",arg,&frame);
			if (copy_from_user
			    (&frame, (CAM_Frame1_t *) arg,
			     sizeof(CAM_Frame1_t)) != 0)
				ret = -EFAULT;
			else {
				if (frame.len)
					copy = 1;
				process_frame(sensor);
				if (cam_g->sens[sensor].sCaptureJpegSize)
					frame.len = (cam_g->sens[sensor].sCaptureJpegSize + 1) >> 1;
				if (copy) {
					/*copy_to_user();*/
				}
				if(copy_to_user((CAM_Frame1_t *) arg, &frame,
					     sizeof(frame)))
				    return -EFAULT;
			}
			break;
		}
	case CAM_IOCTL_GET_JPEG:
		{
			CAM_Frame1_t frame;
			int length;
			short *fbuf;
			pr_debug("IOCTL Get JPEG called\n");
			if (0 !=
			    (copy_from_user
			     (&frame, (void *)arg, sizeof(CAM_Frame1_t)))) {
				return -EINVAL;
			}
			length = (cam_g->sens[sensor].sCaptureJpegSize + 1) >> 1;
			if (!frame.buffer || (frame.len < length)) {
				printk(KERN_ERR "Error 0x%x length %d\n",
				       (u32) frame.buffer, frame.len);
				return -EINVAL;
			}
			frame.len = length;
			printk("Copying to user %d bytes from 0x%x\n",frame.len,(u32)cam_g->cam_buf_main.virt);
			fbuf = (short *)cam_g->cam_buf_main.virt;
			if (copy_to_user
			    (frame.buffer, fbuf,
			     frame.len * sizeof(unsigned short)) != 0)
				frame.len = 0;
			if (copy_to_user
			    ((CAM_Frame1_t *) arg, &frame, sizeof(frame)) != 0)
				return -EFAULT;
			break;
		}
	case CAM_IOCTL_GET_THUMBNAIL:
		{
			CAM_Frame1_t frame;
			char *thumbnail;
			int length;
			struct camera_sensor_t *c = &cam_g->sens[sensor];
			if (copy_from_user
			    (&frame, (CAM_Frame1_t *) arg,
			     sizeof(frame)) != 0) {
				return -EFAULT;
			}
			//length = c->th.size_window.end_pixel * c->th.size_window.end_line;
			/* Supporting only a QVGA Thumbnail for now */
			length = 320*240; 
			if (!frame.buffer || (frame.len < length)) {
				pr_debug("No thumbnail to return\n");
				return -EFAULT;
			}
			frame.len = length;
			thumbnail = (char *)c->tnptr;
			if (copy_to_user
			    (frame.buffer, thumbnail,
			     frame.len * sizeof(unsigned short)) != 0)
				frame.len = 0;

			if (copy_to_user
			    ((CAM_Frame1_t *) arg, &frame, sizeof(frame)) != 0)
				return -EFAULT;
			break;
		}
	case CAM_IOCTL_SET_DIGITAL_EFFECT:
		{
			CAM_Parm_t parm;
			if (copy_from_user
			    (&parm, (CAM_Parm_t *) arg, sizeof(parm)) != 0) {
				rc = -EFAULT;
				break;
			}
			if (cam_g->sens[sensor].sens_m->
			    DRV_SetDigitalEffect(parm.coloreffects,
						 sensor) != HAL_CAM_SUCCESS)
				rc = -EFAULT;
			break;
		}
	case CAM_IOCTL_SET_SCENE_MODE:
		{
			CAM_Parm_t parm;
			if (copy_from_user
			    (&parm, (CAM_Parm_t *) arg, sizeof(parm)) != 0) {
				rc = -EFAULT;
				break;
			}
			if (cam_g->sens[sensor].sens_m->
			    DRV_SetSceneMode(parm.scenemode,
					     sensor) != HAL_CAM_SUCCESS) {
				rc = -EFAULT;
				pr_debug("CAM_IOCTL_SET_SCENE_MODE: CAMDRV_SetSceneMode(): ERROR: \r\n");
			}
			break;
		}
	case CAM_IOCTL_SET_WB:
		{
			CAM_Parm_t parm;
			if (copy_from_user
			    (&parm, (CAM_Parm_t *) arg, sizeof(parm)) != 0) {
				rc = -EFAULT;
				break;
			}
			if (cam_g->sens[sensor].sens_m->
			    DRV_SetWBMode(parm.wbmode,
					  sensor) != HAL_CAM_SUCCESS) {
				rc = -EFAULT;
				pr_debug("CAM_IOCTL_SET_WB: CAMDRV_SetWBMode(): ERROR: \r\n");
			}
			break;
		}
	case CAM_IOCTL_SET_ANTI_BANDING:
		{
			CAM_Parm_t parm;
			if (copy_from_user
			    (&parm, (CAM_Parm_t *) arg, sizeof(parm)) != 0) {
				rc = -EFAULT;
				break;
			}
			if (cam_g->sens[sensor].sens_m->
			    DRV_SetAntiBanding(parm.antibanding,
					       sensor) != HAL_CAM_SUCCESS) {
				rc = -EFAULT;
				pr_debug("CAM_IOCTL_SET_ANTI_BANDING: CAMDRV_SetAntiBanding(): ERROR: \r\n");
			}
			break;
		}
	case CAM_IOCTL_SET_FLASH_MODE:
		{
			CAM_Parm_t parm;
			if (copy_from_user
			    (&parm, (CAM_Parm_t *) arg, sizeof(parm)) != 0) {
				rc = -EFAULT;
				break;
			}
			if (cam_g->sens[sensor].sens_m->
			    DRV_SetFlashMode(parm.flash,
					     sensor) != HAL_CAM_SUCCESS) {
				rc = -EFAULT;
				pr_debug("CAM_IOCTL_SET_FLASH_MODE: CAMDRV_SetFlashMode(): ERROR: \r\n");
			}
			break;
		}
	case CAM_IOCTL_SET_FOCUS_MODE:
		{
			CAM_Parm_t parm;
			if (copy_from_user
			    (&parm, (CAM_Parm_t *) arg, sizeof(parm)) != 0) {
				rc = -EFAULT;
				break;
			}
			if (cam_g->sens[sensor].sens_m->
			    DRV_SetFocusMode(parm.focus,
					     sensor) != HAL_CAM_SUCCESS) {
				rc = -EFAULT;
				pr_debug("CAM_IOCTL_SET_FOCUS_MODE: CAMDRV_SetFocusMode(): ERROR: \r\n");
			}
			break;
		}
	case CAM_IOCTL_ENABLE_AUTOFOCUS:
		{
			if (cam_g->sens[sensor].sens_m->
			    DRV_TurnOnAF(sensor) != HAL_CAM_SUCCESS) {
				rc = -EFAULT;
				pr_debug("CAM_IOCTL_ENABLE_AUTOFOCUS: CAMDRV_TurnOnAF(): ERROR: \r\n");
			}
			break;
		}
	case CAM_IOCTL_DISABLE_AUTOFOCUS:
		{
			if (cam_g->sens[sensor].sens_m->
			    DRV_TurnOffAF(sensor) != HAL_CAM_SUCCESS) {
				rc = -EFAULT;
				pr_debug("CAM_IOCTL_DISABLE_AUTOFOCUS: CAMDRV_TurnOffAF(): ERROR: \r\n");
			}
			break;
		}
	case CAM_IOCTL_SET_JPEG_QUALITY:
		{
			CAM_Parm_t parm;
			if (copy_from_user
			    (&parm, (CAM_Parm_t *) arg, sizeof(parm)) != 0) {
				rc = -EFAULT;
				break;
			}
			if (cam_g->sens[sensor].sens_m->
			    DRV_SetJpegQuality(parm.quality,
					       sensor) != HAL_CAM_SUCCESS) {
				rc = -EFAULT;
				pr_debug("CAM_IOCTL_SET_JPEG_QUALITY: CAMDRV_SetJpegQuality(): ERROR: \r\n");
			}
			break;
		}
	case CAM_IOCTL_GET_STILL_YCbCr:
		{
			CAM_Frame1_t frame;
			int length;
			short *fbuf;
			pr_debug("IOCTL Get STILL YUV called\n");
			if (0 != (copy_from_user(&frame, (void *)arg, sizeof(CAM_Frame1_t)))) {
				return -EINVAL;
			}
			process_frame(sensor);
			length = (cam_g->sens[sensor].sCaptureRawSize + 1) >> 1;
			frame.len = length;
			//pr_debug("\n Copying to user %d bytes from 0x%x\n",frame.len,(u32)cam_g->cam_buf_main.virt);
			if (copy_to_user((CAM_Frame1_t *) arg, &frame, sizeof(frame)) != 0)
				return -EFAULT;
			break;
		}

	case CAM_IOCTL_SET_ZOOM:
		{
			CAM_Parm_t parm;
			struct camera_sensor_t *c = &cam_g->sens[sensor];
			if (copy_from_user(&parm, (CAM_Parm_t *) arg, sizeof(parm)) != 0) {
				rc = -EFAULT;
				break;
			}
			if(parm.zoom != CamZoom_1_0)
				c->bufsw = 0;
			else
				c->bufsw = 0;
			c->zoom = parm.zoom;
			break;
		}
	
	case CAM_IOCTL_PHY_ZOOM_ADDR:
		{
			if(copy_to_user((void *)arg, &cam_g->cam_buf_main.phy,
			    sizeof(dma_addr_t)))
			    return -EFAULT;
		}
		break;
		
	case CAM_IOCTL_GET_DATA:
		{
			if(copy_to_user((void *)arg, cam_g->cam_buf_main.virt,
			    (640*480*2)))
			    return -EFAULT;
		}
		break;

	case CAM_IOCTL_GET_CAPTURE_TYPE:
		{
			CamCapture_t *cap_fmt;
#if defined (CONFIG_CAM_CSI2)
			pr_debug("Type JPEG from sensor\n");
			cap_fmt = Cam_Capture_JPEG;
#else
			pr_debug("Type YUV from sensor\n");
			cap_fmt = Cam_Capture_YUV;
#endif
			if(copy_to_user((void *)arg, &cap_fmt,
			    sizeof(CamCapture_t)))
			    return -EFAULT;
		}
		break;		
	default:
		pr_debug("Default cam IOCTL *************\n");
		break;
	}
	return rc;
}

static int process_frame(CamSensorSelect_t sensor)
{
	struct camera_sensor_t *c = &cam_g->sens[sensor];
	int tp;
	u8 *src;
	u8 *dst;
	int num_pack = 0;
	int i = 0;
	int ret;

	/*  processFrame is not a kernel thread anymore
	 *  Just a regular blocking function invoked from process context
	 *  Since process context can sleep, we don't see any need for a separate
	 *  kernel thread to carry-out JPEG processing */

	/* While(1) and break is required as the STV sensor operates that way */
	while (1) {
				pr_debug("JPEG size before is %d\n",c->sCaptureJpegSize);
	/* Block waiting to be signalled by either VSYNC ISR or camera disable */
		ret =  wait_event_interruptible_timeout(gDataReadyQ, c->still_ready, msecs_to_jiffies(500));
		if (ret > 0) {
			c->still_ready = 0;
/* A Valid wake-up by VSYNC ISR has gProcessFrameRunning = 1.
 * The disable method wakes up with gProcessFrameRunning = 0 which means exit */
			if (!c->gProcessFrameRunning)
				break;
			atomic_set(&c->captured, 1);
			tp = c->sens_m->DRV_GetJpegSize(sensor,NULL);
			if(c->sCaptureJpegSize == 0){
				pr_debug("Major problem with JPEG capture .. size 0 returned but tp is %d\n",tp);
			} else {
				pr_debug("JPEG size = %d Sensor size %d\n",c->sCaptureJpegSize,tp);
			}
			c->sCaptureJpegSize = tp;
			
			if(c->main.format != CamDataFmtJPEG){
				/* No Thumbnail */
				c->sCaptureRawSize = (c->main.size_window.end_pixel*c->main.size_window.end_line*2);
			} 
			else {

				num_pack = tp/240;
				if(tp % 240)
					num_pack++;
				c->framebuf = cam_g->cam_buf_main.virt;
				src = (u8 *)cam_g->cam_buf_main.virt;
				dst = (u8 *)cam_g->cam_buf_main.virt;
				src = src + 640;
				dst = dst + 240;
				for(i = 1; i < num_pack; i++){
					memcpy(dst,src,240);
					src = src + 640;
					dst = dst + 240;
				}
				c->tnptr = (u8 *)cam_g->cam_buf_main.virt;
				c->tnptr += (num_pack*640);

			}

			/* This is where the JPEG would be stored */
			atomic_set(&c->captured, 0);
			break;
		} else {
			printk(KERN_ERR"Timeout trying to capture JPEG .. returning 0 length\n");
			c->sCaptureJpegSize = 0;
			c->sCaptureRawSize = 0;
			break;
		}
	}
	pr_debug("Exit process frame\n");
	return 0;
}

static struct file_operations cam_fops = {
owner:  THIS_MODULE,
open :	cam_open,
release : cam_release,
unlocked_ioctl : cam_ioctl,
};

/* Queue handling functions begin */
static int64_t systemTime(void)
{
	struct timespec t;
	t.tv_sec = t.tv_nsec = 0;
	ktime_get_ts(&t);
	return (int64_t) (t.tv_sec) * 1000000000LL + t.tv_nsec;
}

static void init_queue(struct buf_q *queue)
{
	if (queue) {
		if (queue->isActive)
			return;
		queue->ReadIndex = 0;
		queue->WriteIndex = 0;
		sema_init(&queue->Sem, 0);
		spin_lock_init(&queue->lock);
		queue->isActive = true;
		queue->isWaitQueue = false;
		queue->Num = 0;
		pr_debug("Queue RD and WR %d and %d\n",queue->ReadIndex,queue->WriteIndex);
	} else {
	}
	/*
	   g_CurrData.id =-1;
	   g_CurrData.busAddress =NULL;
	   g_CurrData.timestamp=0;
	 */
}

static void deinit_queue(struct buf_q *queue)
{
	unsigned long stat;
	if (queue) {
		if (!queue->isActive)
			return;

		spin_lock_irqsave(&queue->lock, stat);
		queue->ReadIndex = 0;
		queue->WriteIndex = 0;
		sema_init(&queue->Sem, 1);
		spin_unlock_irqrestore(&queue->lock, stat);
		queue->isActive = false;
		queue->isWaitQueue = false;
		queue->Num = 0;
	}
}

static void push_queue(struct buf_q *queue, CAM_BufData * buf)
{
	unsigned long stat;
	if (!queue)
		return;
	if (!queue->isActive)
		return;
	if (queue->isWaitQueue)
		return;

	spin_lock_irqsave(&queue->lock, stat);

	
	if (queue->Num == MAX_QUEUE_SIZE) {
		pr_debug("Push datat fail!!: Camera  Data Queue Full!\n");
	} else {
		queue->data[queue->WriteIndex].busAddress = buf->busAddress;
		queue->data[queue->WriteIndex].id = buf->id;
		queue->data[queue->WriteIndex].timestamp = buf->timestamp;
		queue->WriteIndex = (queue->WriteIndex + 1) % MAX_QUEUE_SIZE;
		queue->Num++;
	}

	spin_unlock_irqrestore(&queue->lock, stat);
}

static bool pull_queue(struct buf_q *queue, CAM_BufData * buf)
{
	unsigned long stat;
	if (!queue)
		return false;
	if (!queue->isActive)
		return false;
	if (queue->isWaitQueue)
		return false;
	spin_lock_irqsave(&queue->lock, stat);
	
	if (queue->Num == 0) {
		buf->busAddress = NULL;
		buf->id = -1;
	} else {
		buf->busAddress = queue->data[queue->ReadIndex].busAddress;
		buf->id = queue->data[queue->ReadIndex].id;
		buf->timestamp = queue->data[queue->ReadIndex].timestamp;
		queue->ReadIndex = (queue->ReadIndex + 1) % MAX_QUEUE_SIZE;//& MAX_QUEUE_SIZE_MASK;
		queue->Num--;
	//	pr_debug("%s ID is %d\n",__FUNCTION__,buf->id);
	}
	
	spin_unlock_irqrestore(&queue->lock, stat);
	return true;
}
ktime_t new;
static void wakeup_push_queue(struct buf_q *queue, CAM_BufData * buf) //wr_Q
{
	unsigned int NextIndex;
	unsigned long stat;
	if (!queue)
		return;
	if (!queue->isActive)
		return;
	if (!buf)
		return;
	if (!buf->busAddress)
		return;

	spin_lock_irqsave(&queue->lock, stat);
	
	NextIndex = (queue->WriteIndex + 1) % MAX_QUEUE_SIZE_MASK; //& MAX_QUEUE_SIZE_MASK;

	if (queue->Num == MAX_QUEUE_SIZE) {
		 pr_debug( "Wake Push datat failed!!l Camera  Data Queue Full!\n"); 
	} else {
		queue->data[queue->WriteIndex].busAddress = buf->busAddress;
		queue->data[queue->WriteIndex].id = buf->id;
		queue->data[queue->WriteIndex].timestamp = systemTime();
		queue->WriteIndex = (queue->WriteIndex + 1) % MAX_QUEUE_SIZE;
                queue->Num++;
		new = ktime_get();	
		//pr_debug("Sec %d nsec %d id %d\n",new.tv.sec,new.tv.nsec,buf->id);
	}
	
	queue->isWaitQueue = true;
	spin_unlock_irqrestore(&queue->lock, stat);
	//pr_debug("%s id is %d\n",__FUNCTION__,buf->id);
	up(&queue->Sem);

}

static bool wait_pull_queue(struct buf_q *queue, CAM_BufData * buf) //rd-Q
{
	unsigned long stat;
	int ret = 0;
	if (!queue)
		return false;
	if (!queue->isActive)
		return false;

	ret = down_timeout(&queue->Sem,msecs_to_jiffies(500));
        if(ret == -ETIME){
                printk("**** VF frames timing out\n");
		buf->busAddress = NULL;
		buf->id = -1;
#ifdef CAM_CORRUPTION_CONCURRENCY
                /* Freeup SD/USB just in case it's been blocked */
                writel(0x3f0f, io_p2v(BCM21553_MLARB_BASE + 0x100)); //BMARBL_MACONF0
#endif
                return false;
        }

	spin_lock_irqsave(&queue->lock, stat);
	if (queue->Num == 0) {
		buf->busAddress = NULL;
		buf->id = -1;
		pr_debug("Wait WaitPullQueue failed !! Camera  Data Queue Empty!\n");
	} else {
		buf->busAddress = queue->data[queue->ReadIndex].busAddress;
		buf->id = queue->data[queue->ReadIndex].id;
		buf->timestamp = queue->data[queue->ReadIndex].timestamp;
		queue->ReadIndex = (queue->ReadIndex + 1) % MAX_QUEUE_SIZE;//& MAX_QUEUE_SIZE_MASK;
		queue->Num--;
	}
	
	queue->isWaitQueue = true;
	spin_unlock_irqrestore(&queue->lock, stat);
	return true;
}

/* Queue handling functions end*/

static int cam_i2c_probe(struct i2c_client *client,
			 const struct i2c_device_id *dev_id)
{
	int s = 0; /* For primary sensor */
	pr_debug("In I2C probe **** \n");
	cam_g->sens[s].cam_i2c_datap =
	    kmalloc(sizeof(struct cam_i2c_info), GFP_KERNEL);
	memset(cam_g->sens[s].cam_i2c_datap, 0, sizeof(struct cam_i2c_info));
	i2c_set_clientdata(client, cam_g->sens[s].cam_i2c_datap);
	cam_g->sens[s].cam_i2c_datap->client = client;
	cam_g->sens[0].cam_irq = client->irq;
	return 0;
}

static int cam_i2c_remove(struct i2c_client *client)
{
	int s = 0;
	kfree(cam_g->sens[s].cam_i2c_datap);
	cam_g->sens[s].cam_i2c_datap = NULL;
	return 0;
}

static int cam_i2c_command(struct i2c_client *device, unsigned int cmd,
			   void *arg)
{
	return 0;
}

struct i2c_device_id cam_i2c_id_table[] = {
	{"cami2c", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, cam_i2c_id_table);

static HAL_CAM_Result_en_t cam_sensor_cntrl_seq(CamSensorIntfCntrl_st_t *seq,
						UInt32 length)
{
	HAL_CAM_Result_en_t result = HAL_CAM_SUCCESS;
	UInt32 i;
	CSL_CAM_CLOCK_OUT_st_t cslCamClock;
	pr_debug("%s(): \r\n", __FUNCTION__);
	pr_debug("Sequence Length=%d: \r\n",
		 (int)length / sizeof(CamSensorIntfCntrl_st_t));

	for (i = 0; i < length / sizeof(CamSensorIntfCntrl_st_t); i++) {
		switch (seq[i].cntrl_sel) {
		case GPIO_CNTRL:
			gpio_request(seq[i].value, "hal_cam_gpio_cntrl");
			if (seq[i].cmd == GPIO_SetHigh) {
				pr_debug("GPIO_CNTRL(%d), GPIO_SetHigh\n",
					 (int)seq[i].value);
				if (gpio_is_valid(seq[i].value))
					gpio_direction_output(seq[i].value, 1);
			} else {
				pr_debug("GPIO_CNTRL(%d), GPIO_SetLow\n",
					 (int)seq[i].value);
				if (gpio_is_valid(seq[i].value))
					gpio_direction_output(seq[i].value, 0);
			}
			gpio_free(seq[i].value);
			break;

		case MCLK_CNTRL:
			if (seq[i].cmd == CLK_TurnOn){
				switch(seq[i].value)
				{
					case CamDrv_12MHz :
		                        case CamDrv_24MHz :
                		        case CamDrv_48MHz :
					// PWRMGMT_48MPLL_Enable("CAM"); FIXME
					cslCamClock.clock_select = 0x00;          
                            		cslCamClock.enable = TRUE;          
	                                cslCamClock.speed = (CSL_CAM_CLK_SPEED_T)seq[i].value;          
					//pr_debug("Trying to set 12/24/48\n");
            		                if ( csl_cam_set_clk ( &cslCamClock) )
                   			{
						pr_debug("Failed to set MCLK CTRL 12/24/48\n");
                	                }
					break;
					case CamDrv_13MHz :
		                        case CamDrv_26MHz :
					pr_debug("Trying to set clock 13Mhz %d %d\n",(int)CamDrv_13MHz,(int)seq[i].value);
                		        cslCamClock.clock_select = 0x00;          
                           		cslCamClock.enable = TRUE;          
	                                cslCamClock.speed = (CSL_CAM_CLK_SPEED_T)seq[i].value;          
            		                if ( csl_cam_set_clk ( &cslCamClock) )
                        		{
						 pr_debug("Failed to set MCLK CTRL 13/26\n");
                            		}
					break;
					case CamDrv_NO_CLK:
                            		cslCamClock.clock_select = 0x00;          
		                            cslCamClock.enable = FALSE;          
                		            cslCamClock.speed = (CSL_CAM_CLK_SPEED_T)seq[i].value;          
		                            if ( csl_cam_set_clk ( &cslCamClock) )
                		            {
						 pr_debug("Failed to set MCLK CTRL NO CLOCK\n");
                		            }
					break;
					default:
						 pr_debug("Default MCLK CTRL NO CLOCK\n");
						break;
				}
			} else {
				cslCamClock.clock_select = 0x00;          
		                    cslCamClock.enable = FALSE;          
                		    cslCamClock.speed = (CSL_CAM_CLK_SPEED_T)seq[i].value;          
	                    if ( csl_cam_set_clk ( &cslCamClock) )
        	            {
				pr_debug("Failed to disable clock\n");
                    		}
			}
			break;

		case PAUSE:
			if (seq[i].value != 0) {
				pr_debug("PAUSE - %d ms\n", (int)seq[i].value);
				msleep(seq[i].value);
			}
			break;

		default:
			pr_debug("CNTRL - Not Supported\n");
			result = HAL_CAM_ERROR_ACTION_NOT_SUPPORTED;
			break;
		}
	}
	return result;
}

int cam_get_bpp_from_fmt(CamDataFmt_t fmt)
{
	/* For now */
	return 2;
}

static HAL_CAM_Result_en_t cam_sensor_intf_seqsel(CamSensorSelect_t nSensor,
						  CamSensorSeqSel_t nSeqSel)
{
	HAL_CAM_Result_en_t result = HAL_CAM_SUCCESS;
	UInt32 len = 0;
	CamSensorIntfCntrl_st_t *power_seq = NULL;
	struct camera_sensor_t *c = &cam_g->sens[nSensor];
	power_seq = c->sens_m->DRV_GetIntfSeqSel(nSensor, nSeqSel, &len);
		/* get power-up/power-down sequence */
	if ((len != 0) && (power_seq != NULL)) {
		result = cam_sensor_cntrl_seq(power_seq, len);
	} else {
		pr_debug("%s(): No Sequence\r\n", __FUNCTION__);
	}
	return result;
}

static int cam_power_up(CamSensorSelect_t sensor)
{
	int rc = -1;
	struct camera_sensor_t *c;
#if defined(CONFIG_BOARD_THUNDERBIRD_EDN31) || defined(CONFIG_BOARD_THUNDERBIRD_EDN5x)
	if (cam_g->cam_regulator)
		rc = regulator_enable(cam_g->cam_regulator);
	if (cam_g->cam_regulator1)
		rc = regulator_enable(cam_g->cam_regulator1);
#endif
	
	c = &cam_g->sens[sensor];
	c->sensor_intf = c->sens_m->DRV_GetIntfConfig(sensor);
	if (!c->sensor_intf) {
		pr_debug("Unable to get sensor interface config \n");
		rc = -EFAULT;
	}
	/* Config CMI controller over here */
	if (cam_sensor_intf_seqsel(sensor, SensorPwrUp) != 0) {
		pr_debug("Unable to Set power seq at Open\n");
		rc = -EFAULT;
	}
	return 0;
}

static int cam_power_down(CamSensorSelect_t sensor)
{
	int rc = -1;
	struct camera_sensor_t *c;
	c = &cam_g->sens[sensor];
	c->sensor_intf = c->sens_m->DRV_GetIntfConfig(sensor);
	if (c->sensor_intf == NULL) {
		printk(KERN_ERR "Cam power down unable to get intf config\n");
		return -1;
	}
	if (c->sens_m->DRV_SetCamSleep(sensor) != HAL_CAM_SUCCESS) {
		printk(KERN_ERR "Cam power down unable to get intf config\n");
		return -1;
	}
	if (cam_sensor_intf_seqsel(sensor, SensorPwrDn) != 0) {
		printk(KERN_ERR "Unable to Set power seq at power down\n");
		rc = -EFAULT;
	}
#if defined(CONFIG_BOARD_THUNDERBIRD_EDN31) || defined(CONFIG_BOARD_THUNDERBIRD_EDN5x)
	if (cam_g->cam_regulator)
		regulator_disable(cam_g->cam_regulator);
        if (cam_g->cam_regulator1)
                regulator_disable(cam_g->cam_regulator1);
#endif
	
	return 0;

}
ktime_t t1;
static irqreturn_t vsync_isr(int irq, void *arg)
{
	cslCamFrameLisr();
	return IRQ_HANDLED;
}

/* Invoke these functions directly from IOCTL based on arg */
int camera_enable(CamSensorSelect_t sensor)
{
	struct camera_sensor_t *c = &cam_g->sens[sensor];
	u32 size = 0;
	int i;
	int ydelta = 0;
	CSL_CAM_PIPELINE_st_t   cslCamPipeline;
	CSL_CAM_FRAME_st_t      cslCamFrame;
	/* SetParm IOCTL would have populated the CAM_PARM_t structure */
	HAL_CAM_ResolutionSize_st_t sensor_size;
	CSL_CAM_INPUT_st_t      cslCamInput;
	CSL_CAM_BUFFER_st_t     cslCamBuffer1, cslCamBuffer2;
	CSL_CAM_IMAGE_ID_st_t   cslCamImageCtrl;
	CSL_CAM_WINDOW_st_t     cslCamWindow;
	
	sensor_size.resX = c->main.size_window.end_pixel;
	sensor_size.resY = c->main.size_window.end_line;
	if (c->main.format == CamDataFmtJPEG) {
		sensor_size.resX =
		    c->sensor_intf->sensor_config_jpeg->
		    jpeg_packet_size_bytes;// >> 1;// why >> 1 ??
		sensor_size.resY =
		    c->sensor_intf->sensor_config_jpeg->jpeg_max_packets;
	} else if (c->main.format == CamDataFmtYUV) {
		sensor_size.resX = (sensor_size.resX * 3) >> 1;
		/* YUV420 1.5 bytes per pixel case */
	}
	/* Config DMA with dma_x and dma_y */
	/* Config DMA over here
	Sensor driver settings */
	if ((c->main.mode == CamStillnThumb) || (c->main.mode == CamStill)) {
		
		/* End pixel and end line calculations above */
/*
		cam_power_down(cam_g->curr);	
		msleep(100);
		cam_power_up(cam_g->curr);	
		pr_debug("Trying wakeup in enable\n");
		 if (c->sens_m->DRV_Wakeup(cam_g->curr)) {
         	       printk(KERN_ERR "Failed to init the sensor\n");
        	} else {
                	pr_debug("Sensor communication over I2C success\n");
        	} */
		fs_cnt = fe_cnt = 0;
		c->still_ready = 0;			
		cslCamInput.input_mode = c->cslCamIntfCfg.input_mode; 
		cslCamInput.frame_time_out = c->cslCamIntfCfg.frame_time_out;
		cslCamInput.p_cpi_intf_st = NULL;
		if ( csl_cam_set_input_mode( c->hdl, &cslCamInput) ) {
			printk(KERN_ERR"Unable to set CSI input mode\n");
		}
		if((c->zoom == CamZoom_1_0) || (c->main.format == CamDataFmtJPEG)){
			cslCamBuffer1.start_addr = cam_g->cam_buf.phy;
			cslCamBuffer2.start_addr = cam_g->cam_buf.phy + SZ_BUFFER; // offset 
			size = sensor_size.resX * sensor_size.resY;
		}
		else {
			if((sensor_size.resX == 640) && (sensor_size.resY == 480)){
				cslCamBuffer1.start_addr = cam_g->cam_buf.phy + (2*1024*1024);
				size = sensor_size.resX * sensor_size.resY;
			}
			else {
				ydelta = sensor_size.resY - ((sensor_size.resY*c->zoom)/CamZoom_1_0);
				ydelta /= 2;
				/* The top and the bottom lines which are not required while cropping/zooming are represented by ydelta */
				cslCamBuffer1.start_addr = cam_g->cam_buf.phy + (11 * 512 * 1024) - (c->main.size_window.end_pixel * c->main.size_window.end_line * 2);
				cslCamBuffer1.start_addr &= 0xFFFF0000;
				cslCamBuffer1.start_addr =  cslCamBuffer1.start_addr + ((ydelta - 20) * sensor_size.resX * 2);
				size = sensor_size.resX * (sensor_size.resY- ydelta + 20);
				/* We capture 20 extra lines to compensate for approximations while zooming */
			}
		}
		
		if(c->main.format == CamDataFmtJPEG) {
			cslCamBuffer1.line_stride = sensor_size.resX;
			cslCamBuffer2.line_stride = sensor_size.resX; // same as csl buffer 1
			cslCamBuffer1.size = size;
			cslCamBuffer2.size = size;
		}
		else {
			cslCamBuffer1.line_stride = sensor_size.resX * 2;
			cslCamBuffer1.size = size * 2;
		}
		pr_debug("!!!! ydelta %d addr 0x%x size %d original addr 0x%x\n\n",ydelta,cslCamBuffer1.start_addr, cslCamBuffer1.size,cam_g->cam_buf.phy);
		cslCamBuffer1.buffer_wrap_en = FALSE;	
		cslCamBuffer2.buffer_wrap_en = FALSE;
#ifdef CAM_CORRUPTION_CONCURRENCY
		if (csl_cam_set_input_addr(c->hdl, &cslCamBuffer1, /*NULL, */&cslCamBuffer2, NULL)){
#else
		if (csl_cam_set_input_addr(c->hdl, &cslCamBuffer1, NULL, NULL)){
#endif
			pr_err("Unable to set CSI buffer control\n");
		}
		if(c->cslCamIntfCfg.intf == CSL_CAM_INTF_CSI){
			cslCamImageCtrl.image_data_id0 = 0x30;//pCdiState_st->csi2ImageDataType; //only id0 is effective in Athena.
			cslCamImageCtrl.image_data_id1 = 0x00;
			cslCamImageCtrl.image_data_id2 = 0x00;
			cslCamImageCtrl.image_data_id3 = 0x00;
			if (csl_cam_set_image_type_control(c->hdl, &cslCamImageCtrl ) ){
				printk(KERN_ERR"Unable to set image type control\n");
			}
		}
		/* PIPELINE CONTROL */
		cslCamPipeline.decode = CSL_CAM_DEC_NONE;
		cslCamPipeline.unpack = CSL_CAM_PIXEL_8BIT;
	    cslCamPipeline.dec_adv_predictor = FALSE;
		cslCamPipeline.encode               = CSL_CAM_ENC_NONE;
		cslCamPipeline.pack                 = CSL_CAM_PIXEL_8BIT;
	    cslCamPipeline.enc_adv_predictor    = FALSE;
	    cslCamPipeline.encode_blk_size      = 0x0000;
		if ( csl_cam_set_pipeline_control(c->hdl, &cslCamPipeline) ) {
			pr_err("Unable to set CSI pipeline control\n");
    	}

		cslCamWindow.enable = FALSE;
        cslCamWindow.horizontal_start_byte = 0;
		cslCamWindow.horizontal_size_bytes = sensor_size.resX * 2;//(pcam_window_config->win_end_X - pcam_window_config->win_start_X);
		cslCamWindow.vertical_start_line = 0;//pcam_window_config->win_start_Y;
		cslCamWindow.vertical_size_lines = sensor_size.resY;//pcam_window_config->win_start_Y;

		if ( csl_cam_set_image_window(c->hdl, &cslCamWindow ) ) {
			pr_err("Cam_CfgWindowScale(): csl_cam_set_image_window() ERROR: \n");
		}
		
	    cslCamFrame.int_enable      = c->sensor_intf->sensor_config_ccp_csi->pkt_intr_enable | CSL_CAM_INT_FRAME_ERROR | CSL_CAM_INT_FRAME_START;
	    cslCamFrame.capture_mode    = CSL_CAM_CAPTURE_MODE_NORMAL;
	    cslCamFrame.int_line_count  = sensor_size.resY;
	    cslCamFrame.capture_size    = size;
		
		if (csl_cam_set_frame_control( c->hdl, &cslCamFrame) ) {
			pr_err("Unable to set CSI frame control\n");
		}

#ifdef CAM_CORRUPTION_CONCURRENCY
		writel(0x0300200f , io_p2v(BCM21553_MLARB_BASE + 0x100)); //BMARBL_MACONF0
		writel(0x44444 , io_p2v(BCM21553_MLARB_BASE + 0x108)); //BMARBL_MACONF2
		udelay(5);
#endif

		{
			/* writing to the section of DDR some known pattern. This section is used by Camera to dump the JPEG. */
			memset(cam_g->cam_buf.virt, 0xAB, SZ_BUFFER);
		}
		c->sens_m->DRV_CfgStillnThumbCapture(c->main.size_window.size,
                            c->main.format, c->th.size_window.size, c->th.format, sensor);
		
		if(csl_cam_rx_start(c->hdl))
			pr_debug("Unable to start RX\n");
		
		c->mode = CAM_STILL;
		c->prev.tv.sec = 0;
		c->prev.tv.nsec = 0;
		/*
		cam_config_dma_buffers(sensor_size.resX, sensor_size.resY,
				       c->main.format, sensor); */
		c->gProcessFrameRunning = 1;
		c->state = CAM_INIT;
		c->sCaptureJpegSize = 0;
		enable_irq(c->cam_irq);
		printk("Enabled camera STILLS\n");
	} else {
		/* Video and/or VF mode */
		/*
		cam_config_dma_buffers(sensor_size.resX, sensor_size.resY,
				       c->main.format, sensor); */
		//c->main.format = CAMDRV_IMAGE_YUV422;	
		for(i = 0; i < CAM_NUM_VFVIDEO; i++) {
			cam_g->sens[sensor].camb[i].phy = cam_g->cam_buf.phy + (i * sensor_size.resX * sensor_size.resY * 2);
		}
		cslCamInput.input_mode = c->cslCamIntfCfg.input_mode; 
		cslCamInput.frame_time_out = c->cslCamIntfCfg.frame_time_out;
		cslCamInput.p_cpi_intf_st = NULL;

		if(c->cslCamIntfCfg.intf == CSL_CAM_INTF_CPI)
			cslCamInput.p_cpi_intf_st = c->cslCamIntfCfg.p_cpi_intf_st;
		
		if ( csl_cam_set_input_mode( c->hdl, &cslCamInput) ) {
			pr_err("Unable to set CSI input mode\n");
    		}
		cslCamBuffer1.start_addr = cam_g->cam_buf.phy + (2 * sensor_size.resX * sensor_size.resY * 4);
		//cam_g->sens[sensor].camb[0].phy = cslCamBuffer1.start_addr;
		/* 2 bpp */
		size = (sensor_size.resX * sensor_size.resY * 2);
		/* Align all addresses to 10 bit address boundary*/
		size = (size + 1023)/1024;
		size *= 1024;
		pr_debug("Buf 1 addr 0x%x and fps %d\n",(u32)cslCamBuffer1.start_addr,(int)c->main.fps);
		cslCamBuffer1.size = size;
		cslCamBuffer1.line_stride = sensor_size.resX * 2;
		cslCamBuffer1.buffer_wrap_en = FALSE;	
		cslCamBuffer2.start_addr = (cslCamBuffer1.start_addr + size);
		//cam_g->sens[sensor].camb[1].phy = cslCamBuffer2.start_addr;
		/* 2 bpp */
		cslCamBuffer2.size = size;
		cslCamBuffer2.line_stride = sensor_size.resX * 2;
		cslCamBuffer2.buffer_wrap_en = FALSE;	
		pr_debug("Buf 2 addr 0x%x\n",(u32)cslCamBuffer2.start_addr);
		if (csl_cam_set_input_addr(c->hdl, &cslCamBuffer1,&cslCamBuffer2, NULL)){
			pr_err("Unable to set CSI buffer control\n");
    		}
		/* PIPELINE CONTROL */
		cslCamPipeline.decode = CSL_CAM_DEC_NONE;
		cslCamPipeline.unpack = CSL_CAM_PIXEL_8BIT;
	    	cslCamPipeline.dec_adv_predictor = FALSE;
		cslCamPipeline.encode               = CSL_CAM_ENC_NONE;
		cslCamPipeline.pack                 = CSL_CAM_PIXEL_8BIT;
	    	cslCamPipeline.enc_adv_predictor    = FALSE;
	    	cslCamPipeline.encode_blk_size      = 0x0000;
		if ( csl_cam_set_pipeline_control(c->hdl, &cslCamPipeline) ) {
			pr_err("Unable to set CSI pipeline control\n");
    		}
		cslCamWindow.enable = FALSE;
		cslCamWindow.horizontal_start_byte = 0;
		cslCamWindow.horizontal_size_bytes = sensor_size.resX*2;//(pcam_window_config->win_end_X - pcam_window_config->win_start_X);
		cslCamWindow.vertical_start_line = 0;//pcam_window_config->win_start_Y;
		cslCamWindow.vertical_size_lines = sensor_size.resY;//pcam_window_config->win_start_Y;
		if ( csl_cam_set_image_window(c->hdl, &cslCamWindow ) ) {
			pr_err("Cam_CfgWindowScale(): csl_cam_set_image_window() ERROR: \n");
		}

		cslCamFrame.int_enable      = c->sensor_intf->sensor_config_ccp_csi->data_intr_enable | CSL_CAM_INT_FRAME_START; // | CSL_CAM_INT_FRAME_ERROR;
	    	cslCamFrame.capture_mode    = CSL_CAM_CAPTURE_MODE_NORMAL;
	  	cslCamFrame.int_line_count  = sensor_size.resY;
	    	cslCamFrame.capture_size    = size;
		if (csl_cam_set_frame_control( c->hdl, &cslCamFrame) ) {
			pr_err("Unable to set CSI frame control\n");
		}
		if(csl_cam_rx_start(c->hdl))
			pr_debug("Unable to start RX\n");
		enable_irq(c->cam_irq);
		c->sens_m->
			  DRV_SetVideoCaptureMode(c->main.size_window.size,
						  c->main.format, sensor);
		c->sens_m->DRV_SetFrameRate(c->main.fps, sensor);
		c->sens_m->DRV_EnableVideoCapture(sensor);
		/* Sensor driver methods to enable video modes */
		c->mode = CAM_STREAM;
		c->state = CAM_INIT;
		printk("Enabled camera VF/Video\n");
	}
	/* Sequence from app is MEM_REGISTER -- ENABLE -- MEM_BUFFERS */
#ifdef CAM_CORRUPTION_CONCURRENCY
	c->sCaptureFrameCountdown = 3;
#else
	c->sCaptureFrameCountdown = 2;
#endif

	wake_lock(&cam_g->camera_wake_lock);
	c->rd_Q.isActive = 0;
	c->wr_Q.isActive = 0;
	init_queue(&c->wr_Q);
	init_queue(&c->rd_Q);
	c->drop_fps = 0;
	c->state = CAM_ON;
	return 0;
}

int camera_disable(CamSensorSelect_t sensor)
{
	struct camera_sensor_t *c = &cam_g->sens[sensor];
/* SetParm IOCTL would have populated the CAM_PARM_t structure */
	unsigned long stat;
	int rc = 0;
	spin_lock_irqsave(&c->c_lock, stat);
	c->mode = CAM_NONE;
	c->state = CAM_STOPPING;
	spin_unlock_irqrestore(&c->c_lock, stat);
	c->gProcessFrameRunning = 0;
	wake_up_interruptible(&gDataReadyQ);
	if ((c->main.mode == CamStillnThumb) || (c->main.mode == CamStill)) {
		pr_debug( "Disabling capture\n");
		/* taskcallback would have already disabled csl_cam_rx*/
		c->sens_m->DRV_DisableCapture(sensor);
	} else {
		pr_debug( "Disabling stream\n");
		/* The ISP (STV) has an issue that if the ISP is disabled
		* within a frame time, the ISP goes into an unknown state.
		* Since the frame time is a function of FPS, we are adding
		* a max of 100msec delay to work for 10 to 30 fps */
		msleep(100);
		csl_cam_rx_stop(c->hdl);
		c->sens_m->DRV_DisablePreview(sensor);
	}

#ifdef CAM_CORRUPTION_CONCURRENCY
	writel(0x3f0f, io_p2v(BCM21553_MLARB_BASE + 0x100)); //BMARBL_MACONF0 
#endif

	csl_cam_reset(c->hdl, (CSL_CAM_RESET_t)(CSL_CAM_RESET_SWR | CSL_CAM_RESET_ARST ));
	disable_irq(c->cam_irq);
	wake_unlock(&cam_g->camera_wake_lock);
	deinit_queue(&c->wr_Q);
	deinit_queue(&c->rd_Q);
	c->gCurrData.id = -1;
	c->gCurrData.busAddress = NULL;
	c->gCurrData.timestamp = 0;
	c->state = CAM_OFF;
		{
			int i = 0;
			for(i = 0; i < fs_cnt; i ++)
				pr_debug("FS of %d at %d sec %ld nsec\n", i, t_fs[i].tv.sec, t_fs[i].tv.nsec);
			for(i = 0; i < fe_cnt; i ++)
				pr_debug("FE of %d at %d sec %ld nsec\n", i, t_fe[i].tv.sec, t_fe[i].tv.nsec);
			fs_cnt = 0;
			fe_cnt = 0;
		}
	printk("Disabled camera\n");
	return rc;
}

static int cam_open(struct inode *inode, struct file *file)
{
	int rc = 0;
	struct camera_sensor_t *c;
	/* Choose between primary and sec camera */
	pr_debug("%s called \n",__FUNCTION__);
#if defined (CONFIG_CPU_FREQ_GOV_BCM21553)
	cpufreq_bcm_dvfs_disable(cam_g->cam_dvfs);
#endif
	down(&cam_g->cam_sem);
	if (1) {
		cam_g->curr = CamSensorPrimary;
		if(cam_g->sens[cam_g->curr].devbusy){
			up(&cam_g->cam_sem);
			return -EBUSY;
		}
		cam_g->sens[cam_g->curr].devbusy= 1;
		cam_g->sens[CamSensorPrimary].sens_m = CAMDRV_primary_get();
	} else {
		cam_g->curr = CamSensorSecondary;
	}
	board_sysconfig(SYSCFG_CAMERA,SYSCFG_INIT);
	board_sysconfig(SYSCFG_CAMERA,SYSCFG_ENABLE);
	c = &cam_g->sens[cam_g->curr];
	if(csl_cam_init()) {
		pr_err("Unable to init the CAMINTF \n");
		rc = -EBUSY;
		goto oerr;
	}
	/* Need to init I2C but our probe would have already done that */
	cam_power_up(cam_g->curr);	
	/* Camera CSL open */	
	c->sensor_intf = c->sens_m->DRV_GetIntfConfig(cam_g->curr);
	
	c->cslCamIntfCfg.intf = (CSL_CAM_INTF_T)c->sensor_intf->sensor_config_caps->intf_mode;
    	c->cslCamIntfCfg.afe_port = (CSL_CAM_PORT_AFE_T)c->sensor_intf->sensor_config_caps->intf_port;
    	c->cslCamIntfCfg.frame_time_out = 100; /* 100ms or 10 fps*/
    	//c->cslCamIntfCfg.capture_mode = CSL_CAM_CAPTURE_MODE_NORMAL;
	pr_debug("Out interface %d and CSL_ %d\n",c->cslCamIntfCfg.intf,CSL_CAM_INTF_CSI);
	if(c->cslCamIntfCfg.intf == CSL_CAM_INTF_CPI){
		pr_debug("CPI being opened\n");
		c->cslCamIntfCfg.p_cpi_intf_st  = kmalloc(sizeof(CSL_CAM_CPI_INTF_st_t),GFP_KERNEL);
		c->cslCamIntfCfg.p_cpi_intf_st->sync_mode = CSL_CAM_NO_MODE_SELECT;
		c->cslCamIntfCfg.p_cpi_intf_st->sync_mode = c->sensor_intf->sensor_config_ccir656->sync_mode;
		c->cslCamIntfCfg.p_cpi_intf_st->hsync_signal = c->sensor_intf->sensor_config_ccir656->hsync_control;
		c->cslCamIntfCfg.p_cpi_intf_st->hsync_trigger = c->sensor_intf->sensor_config_ccir656->hsync_polarity;
		c->cslCamIntfCfg.p_cpi_intf_st->vsync_signal = c->sensor_intf->sensor_config_ccir656->vsync_control;
		c->cslCamIntfCfg.p_cpi_intf_st->vsync_trigger = c->sensor_intf->sensor_config_ccir656->vsync_polarity;
		c->cslCamIntfCfg.p_cpi_intf_st->clock_edge = c->sensor_intf->sensor_config_ccir656->data_clock_sample;
		c->cslCamIntfCfg.p_cpi_intf_st->bit_width = c->sensor_intf->sensor_config_ccir656->bus_width;
		c->cslCamIntfCfg.p_cpi_intf_st->data_shift = c->sensor_intf->sensor_config_ccir656->data_shift;
		c->cslCamIntfCfg.p_cpi_intf_st->fmode = c->sensor_intf->sensor_config_ccir656->field_mode;
		c->cslCamIntfCfg.p_cpi_intf_st->smode = CSL_CAM_SCOPE_DISABLED;
	} else if(c->cslCamIntfCfg.intf == CSL_CAM_INTF_CSI){
		pr_debug("CSI2 being opened\n");
		c->cslCamIntfCfg.input_mode = (CSL_CAM_INPUT_MODE_t)c->sensor_intf->sensor_config_ccp_csi->input_mode;
        c->cslCamIntfCfg.p_cpi_intf_st  = NULL;
	}
	if(0 != request_irq(c->cam_irq,vsync_isr,IRQF_DISABLED,"vsync",&cam_g->curr)){
			printk(KERN_ERR"Major problem .. no IRQ available for CSI2****\n");
			goto oerr;
	}
	disable_irq(c->cam_irq); /* Is it needed ??*/
	if ( csl_cam_open( &c->cslCamIntfCfg, &c->hdl ) ){
		printk(KERN_ERR"Failed to open CSL interface\n");
		rc = -EBUSY;
		goto oerr;
    }
	pr_debug("Sensor communication over I2C start\n");
	if (c->sens_m->DRV_Wakeup(cam_g->curr)) {
    	printk(KERN_ERR "Failed to init the sensor\n");
		rc = -EFAULT;
		goto oerr;
    	} else {
		printk(" %s Sensor communication over I2C success\n",__FUNCTION__);
	}
	pr_debug("Done DRV_Wakeup \n");
	/*TODO interrupt processing request callback */
	/* Avoid LISR callback as it's only status storage
 	* Register task callback instead */	
	if( csl_cam_register_event_callback( c->hdl, CSL_CAM_TASK, (cslCamCB_t)taskcallback, NULL ) ) {
		/* This is to be seen :)*/
		printk(KERN_ERR"Failed to register task callback\n");
		rc = -EBUSY;
		goto oerr;
	}
#ifdef REG_DISP
	csl_cam_register_display(c->hdl );
#endif
	// CAMDRV_InitSensor
	/* c->sensor_intf would be valid after successful return of power up */
	/* Init the working buffer queue */
	c->gCurrData.id = -1;
	c->gCurrData.busAddress = NULL;
	c->gCurrData.timestamp = 0;
	cam_g->sens[cam_g->curr].state = CAM_OFF;
	c->mode = CAM_NONE;
	memset(&c->th,0,sizeof(c->th));
	c->bufsw = 0;
	c->zoom = CamZoom_1_0;
	up(&cam_g->cam_sem);
	// disable_irq(IRQ_VSYNC);
	return 0;
oerr:
	up(&cam_g->cam_sem);
	return rc;
}
/*  Very important need to see if reset is enough in camera_disable 
 *  We release the csl_interface only on powerdown VVVV */
static int cam_release(struct inode *inode, struct file *file)
{
	struct camera_sensor_t *c;
	down(&cam_g->cam_sem);
	c = &cam_g->sens[cam_g->curr];
	if (c->state != CAM_OFF) {
		camera_disable(cam_g->curr);
	}
	cam_power_down(cam_g->curr);
	csl_cam_reset(c->hdl, (CSL_CAM_RESET_t)(CSL_CAM_RESET_SWR | CSL_CAM_RESET_ARST ));
	csl_cam_close(c->hdl);
	csl_cam_exit();
	if(c->cslCamIntfCfg.intf == CSL_CAM_INTF_CPI)
		kfree(c->cslCamIntfCfg.p_cpi_intf_st);
	free_irq(c->cam_irq,&cam_g->curr);
	board_sysconfig(SYSCFG_CAMERA,SYSCFG_DISABLE);
	wake_unlock(&cam_g->camera_wake_lock);
#if defined (CONFIG_CPU_FREQ_GOV_BCM21553)
	cpufreq_bcm_dvfs_enable(cam_g->cam_dvfs);
#endif
	cam_g->sens[cam_g->curr].devbusy= 0;
	printk( "IRQ VSYNC freed\n");
	up(&cam_g->cam_sem);
	return 0;
}

/* CAM I2C functions begin */
unsigned char msgbuf[MAX_I2C_DATA_COUNT];
HAL_CAM_Result_en_t CAM_WriteI2c(UInt16 camRegID, UInt16 DataCnt, UInt8 *Data)
{
	HAL_CAM_Result_en_t result = HAL_CAM_SUCCESS;
	struct i2c_adapter *adap;
	int s = cam_g->curr;

	if (!DataCnt || ((DataCnt + sizeof(camRegID)) > MAX_I2C_DATA_COUNT)) {
		result = HAL_CAM_ERROR_INTERNAL_ERROR;
		pr_debug("%s() - DataCnt = %d out of range\n", __FUNCTION__,
			 DataCnt);
		goto done;
	}

	if ((cam_g->sens[s].cam_i2c_datap != NULL)
	    && (adap = cam_g->sens[s].cam_i2c_datap->client->adapter)) {
		int ret;
		struct i2c_msg msg = {
			cam_g->sens[s].cam_i2c_datap->client->addr,
			cam_g->sens[s].cam_i2c_datap->client->flags,
			sizeof(camRegID) + DataCnt, msgbuf };
		/* Check the swapping of sensor ID */
		msgbuf[0] = (u8) ((camRegID & 0xFF00) >> 8);
		msgbuf[1] = (u8) (camRegID & 0x00FF);
		memcpy(&msgbuf[sizeof(camRegID)], Data, DataCnt);

		ret = i2c_transfer(adap, &msg, 1);
		if (ret != 1)
			result = HAL_CAM_ERROR_INTERNAL_ERROR;

	} else {
		pr_debug("%s() - Camera I2C adapter null\n", __FUNCTION__);
		result = HAL_CAM_ERROR_INTERNAL_ERROR;
	}
done:
	return result;
}

HAL_CAM_Result_en_t CAM_ReadI2c(UInt16 camRegID, UInt16 DataCnt, UInt8 *Data)
{
	HAL_CAM_Result_en_t result = HAL_CAM_SUCCESS;
	struct i2c_adapter *adap;
	int s = cam_g->curr;

	if (!DataCnt) {
		result = HAL_CAM_ERROR_INTERNAL_ERROR;
		pr_debug("%s() - DataCnt = %d out of range\n", __FUNCTION__,
			 DataCnt);
		goto done;
	}

	if ((cam_g->sens[s].cam_i2c_datap)
	    && (adap = cam_g->sens[s].cam_i2c_datap->client->adapter)) {
		int ret;
		unsigned char msgbuf0[sizeof(camRegID)];
		struct i2c_msg msg[2] = {
			{cam_g->sens[s].cam_i2c_datap->client->addr,
			 cam_g->sens[s].cam_i2c_datap->client->flags,
			 sizeof(camRegID), msgbuf0}
			,
			{cam_g->sens[s].cam_i2c_datap->client->addr,
			 cam_g->sens[s].cam_i2c_datap->client->flags | I2C_M_RD,
			 DataCnt, Data}
		};

		msgbuf0[0] = (camRegID & 0xFF00) >> 8;
		msgbuf0[1] = (camRegID & 0x00FF);
		ret = i2c_transfer(adap, msg, 2);
		if (ret != 2)
			result |= HAL_CAM_ERROR_INTERNAL_ERROR;

	} else {
		pr_debug("%s() - Camera I2C adapter null\n", __FUNCTION__);
		result = HAL_CAM_ERROR_INTERNAL_ERROR;
	}
done:
	return result;
}

/* CAM I2C functions end */
struct i2c_driver i2c_driver_cam = {
	.driver = {
		   .name = IF_NAME,
		   },
	.id_table = cam_i2c_id_table,
	.probe = cam_i2c_probe,
	.remove = cam_i2c_remove,
	.command = cam_i2c_command,
};

static int __init cam_init(void)
{
	int rc;
	struct camera_sensor_t *c;
	ktime_t in = ktime_get();
	pr_debug( "%s sec %d nsec %d\n", banner,in.tv.sec,in.tv.nsec);
	cam_g =
	(struct cam_generic_t *)kmalloc(sizeof(struct cam_generic_t), GFP_KERNEL);
	if (!cam_g) {
		printk(KERN_ERR "No memory for camera driver\n");
		return -ENOMEM;
	}
	rc = register_chrdev(BCM_CAM_MAJOR, "camera", &cam_fops);
	if (rc < 0) {
		printk(KERN_ERR "Camera: register_chrdev failed for major %d\n",
		       BCM_CAM_MAJOR);
		return rc;
	}
	cam_g->cam_class = class_create(THIS_MODULE, "camera");
	if (IS_ERR(cam_g->cam_class)) {
		unregister_chrdev(BCM_CAM_MAJOR, "camera");
		rc = PTR_ERR(cam_g->cam_class);
		goto err;
	}
	device_create(cam_g->cam_class, NULL, MKDEV(BCM_CAM_MAJOR, 0), NULL,
		      "camera");
#if defined(CONFIG_BOARD_THUNDERBIRD_EDN31) || defined(CONFIG_BOARD_THUNDERBIRD_EDN5x)
	cam_g->cam_regulator = regulator_get(NULL, "cam_vdd");
	if (!cam_g->cam_regulator || IS_ERR(cam_g->cam_regulator)) {
		printk(KERN_ERR "No Regulator available\n");
		rc = -EFAULT;
		goto err;
	}
	regulator_set_voltage(cam_g->cam_regulator,1800000,1800000);
	cam_g->cam_regulator1 = regulator_get(NULL, "cam_2v8");
	if (!cam_g->cam_regulator1 || IS_ERR(cam_g->cam_regulator1)) {
		printk(KERN_ERR "No Regulator available for ALDO8\n");
		rc = -EFAULT;
		goto err;
	}
#endif
#if defined(CONFIG_BOARD_THUNDERBIRD_EDN31) || defined(CONFIG_BOARD_THUNDERBIRD_EDN5x)
        if (cam_g->cam_regulator)
                regulator_enable(cam_g->cam_regulator);
        if (cam_g->cam_regulator1)
                regulator_enable(cam_g->cam_regulator1);
#endif
#if defined (CONFIG_CPU_FREQ_GOV_BCM21553)
	cam_g->cam_dvfs = cpufreq_bcm_client_get("cam_dvfs");
	if(!cam_g->cam_dvfs)
		printk(KERN_ERR"Unable to get a DVFS client for camera driver !! \n");
	else
		cpufreq_bcm_dvfs_enable(cam_g->cam_dvfs);
#endif
	
	wake_lock_init(&cam_g->camera_wake_lock, WAKE_LOCK_SUSPEND, "camera");
	cam_g->gSysCtlHeader = register_sysctl_table(gSysCtl);
	cam_g->sens[0].sens_m = CAMDRV_primary_get();
	cam_g->curr = 0;
	//	cam_power_up(cam_g->curr);
	/* Alloc separately for primary and secondary cameras. For now only primary
	 cam_buf is the operating buffer
	 cam_buf_main always holds the addresses of the allocated boot time buffer
	 Stub 1 */
	c = &cam_g->sens[0];
	c->devbusy = 0;
	c->sensor_intf = c->sens_m->DRV_GetIntfConfig(0);
	if (!c->sensor_intf) {
		printk(KERN_ERR "Unable to get sensor interface config \n");
		rc = -EFAULT;
	}
	cam_g->sens[0].cam_i2c_datap = NULL;
	/* For now only for primary camera */
	pr_debug("Adding I2C driver **** \n");
	rc = i2c_add_driver(&i2c_driver_cam);
	sema_init(&cam_g->cam_sem, 1);
	mdelay(2);
	/*
	if (cam_sensor_intf_seqsel(sensor, SensorPwrDn) != 0) {
		printk(KERN_ERR "Unable to Set power seq at Open\n");
		rc = -EFAULT;
	}*/

	cam_power_down(cam_g->curr);
	in = ktime_get();
	pr_debug("Cam_init end sec %d nsec %d\n",in.tv.sec,in.tv.nsec);
	return rc;
err:	unregister_chrdev(BCM_CAM_MAJOR, "camera");
	return rc;
}

static void __exit cam_exit(void)
{
	if (cam_g->gSysCtlHeader != NULL) {
		unregister_sysctl_table(cam_g->gSysCtlHeader);
	}
	/*
	if (cam_g->cam_regulator)
		regulator_put(cam_g->cam_regulator);
	*/
	wake_lock_destroy(&cam_g->camera_wake_lock);
	device_destroy(cam_g->cam_class, MKDEV(BCM_CAM_MAJOR, 0));
	class_destroy(cam_g->cam_class);
	unregister_chrdev(BCM_CAM_MAJOR, "camera");
#if defined (CONFIG_CPU_FREQ_GOV_BCM21553)
	cpufreq_bcm_client_put(cam_g->cam_dvfs);
#endif
	kfree(cam_g);
}

module_init(cam_init);
module_exit(cam_exit);
MODULE_AUTHOR("Broadcom");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Camera Driver");

