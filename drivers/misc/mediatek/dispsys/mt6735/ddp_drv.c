#define LOG_TAG "ddp_drv"

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/module.h>
#include <generated/autoconf.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/kdev_t.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/param.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/timer.h>
#include <mach/mt_smi.h>
#include <linux/proc_fs.h>  //proc file use
#include <linux/miscdevice.h>
//ION
//#include <linux/ion.h>
//#include <linux/ion_drv.h>
//#include <mach/m4u.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <asm/io.h>
#include <mach/mt_smi.h>
#include <mach/irqs.h>
#include <mach/mt_reg_base.h>
#include <mach/mt_irq.h>
#include <mach/irqs.h>
#include <mach/mt_clkmgr.h> // ????
#include <mach/mt_irq.h>
#include <mach/sync_write.h>
#include <mach/m4u.h>

#include "disp_drv_platform.h"
#include "ddp_drv.h"
#include "ddp_reg.h"
#include "ddp_hal.h"
#include "ddp_log.h"
#include "ddp_irq.h"
#include "ddp_info.h"
#include "ddp_dpi_reg.h"

//for sysfs
#include <linux/kobject.h>
#include <linux/sysfs.h>


extern PDPI_REGS DPI_REG;
#define DISP_DEVNAME "DISPSYS"

typedef struct
{
    pid_t open_pid;
    pid_t open_tgid;
    struct list_head testList;
    spinlock_t node_lock;
} disp_node_struct;


static struct kobject kdispobj;
extern unsigned int ddp_dump_reg_to_buf(unsigned int start_module,unsigned long * addr);

/*****************************************************************************/
//sysfs for access information
//--------------------------------------//
static ssize_t disp_kobj_show(struct kobject *kobj, 
                               struct attribute *attr, 
                               char *buffer) 
{ 
    int size=0x0;
		
    if (0 == strcmp(attr->name, "dbg1")) 
    {    
        size = ddp_dump_reg_to_buf(2,(unsigned long *) buffer);//DISP_MODULE_RDMA
    }
    else if(0 == strcmp(attr->name, "dbg2"))
    {
        size = ddp_dump_reg_to_buf(1,(unsigned long *) buffer);//DISP_MODULE_OVL
    }
    else if(0 == strcmp(attr->name, "dbg3"))
    {
        size = ddp_dump_reg_to_buf(0,(unsigned long *) buffer);//DISP_MODULE_WDMA0
    }
	
    return size;
}
//--------------------------------------//

static struct kobj_type disp_kobj_ktype = { 
        .sysfs_ops = &(struct sysfs_ops){ 
                .show = disp_kobj_show, 
                .store = NULL 
        }, 
        .default_attrs = (struct attribute*[]){ 
                &(struct attribute){ 
                        .name = "dbg1",   //disp, dbg1
                        .mode = S_IRUGO 
                }, 
                &(struct attribute){ 
                        .name = "dbg2",   //disp, dbg2
                        .mode = S_IRUGO 
                }, 
                &(struct attribute){ 
                        .name = "dbg3",   //disp, dbg3
                        .mode = S_IRUGO 
                }, 
                NULL 
        } 
};


static unsigned int ddp_ms2jiffies(unsigned long ms)
{
    return ((ms*HZ + 512) >> 10);
}

#if defined(CONFIG_TRUSTONIC_TEE_SUPPORT) && defined(CONFIG_MTK_SEC_VIDEO_PATH_SUPPORT)

#include "mobicore_driver_api.h"
#include "tlcApitplay.h"

//------------------------------------------------------------------------------
// handle address for t-play
//------------------------------------------------------------------------------
static unsigned int *tplay_handle_virt_addr = NULL;
static dma_addr_t handle_pa = 0;

// allocate a fixed physical memory address for storing tplay handle
void init_tplay_handle(struct device *dev)
{
    void *va;
    va = dma_alloc_coherent(dev, sizeof(unsigned int), &handle_pa, GFP_KERNEL);
    if (NULL != va)
    {
        DDPDBG("[SVP] allocate handle_pa[%pa]\n", &va);
    }
    else
    {
        DDPERR("[SVP] failed to allocate handle_pa \n");
    }
    tplay_handle_virt_addr = (unsigned int *)va;
}

static int write_tplay_handle(unsigned int handle_value)
{
    if (NULL != tplay_handle_virt_addr)
    {
        DDPDBG("[SVP] write_tplay_handle 0x%x \n", handle_value);
        *tplay_handle_virt_addr = handle_value;
		return 0;
    }
    return -EFAULT;
}

static const uint32_t mc_deviceId = MC_DEVICE_ID_DEFAULT;

static const struct mc_uuid_t MC_UUID_TPLAY = { {0x05, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };

static struct mc_session_handle tplaySessionHandle;
static tplay_tciMessage_t *pTplayTci  = NULL;

volatile static unsigned int opened_device = 0;
static enum mc_result late_open_mobicore_device(void)
{
    enum mc_result mcRet = MC_DRV_OK;

    if (0 == opened_device)
    {
        DDPDBG("=============== open mobicore device ===============\n");
        /* Open MobiCore device */
        mcRet = mc_open_device(mc_deviceId);
        if (MC_DRV_ERR_INVALID_OPERATION == mcRet)
        {
            // skip false alarm when the mc_open_device(mc_deviceId) is called more than once
            DDPDBG("mc_open_device already done\n");
        }
        else if (MC_DRV_OK != mcRet)
        {
            DDPERR("mc_open_device failed: %d @%s line %d\n", mcRet, __func__, __LINE__);
            return mcRet;
        }
        opened_device = 1;
    }
    return MC_DRV_OK;
}

static int open_tplay_driver_connection(void)
{
    enum mc_result mcRet = MC_DRV_OK;

    if (tplaySessionHandle.session_id != 0)
    {
        DDPMSG("tplay TDriver session already created\n");
        return 0;
    }

    DDPDBG("=============== late init tplay TDriver session ===============\n");
    do
    {
        late_open_mobicore_device();

        /* Allocating WSM for DCI */
        mcRet = mc_malloc_wsm(mc_deviceId, 0, sizeof(tplay_tciMessage_t), (uint8_t **) &pTplayTci, 0);
        if (MC_DRV_OK != mcRet)
        {
            DDPERR("mc_malloc_wsm failed: %d @%s line %d\n", mcRet, __func__, __LINE__);
            return -1;
        }

        /* Open session the TDriver */
        memset(&tplaySessionHandle, 0, sizeof(tplaySessionHandle));
        tplaySessionHandle.device_id = mc_deviceId;
        mcRet = mc_open_session(&tplaySessionHandle,
                                &MC_UUID_TPLAY,
                                (uint8_t *) pTplayTci,
                                (uint32_t) sizeof(tplay_tciMessage_t));
        if (MC_DRV_OK != mcRet)
        {
            DDPERR("mc_open_session failed: %d @%s line %d\n", mcRet, __func__, __LINE__);
            // if failed clear session handle
            memset(&tplaySessionHandle, 0, sizeof(tplaySessionHandle));
            return -1;
        }
    } while (0);

    return (MC_DRV_OK == mcRet) ? 0 : -1;
}

static int close_tplay_driver_connection(void)
{
    enum mc_result mcRet = MC_DRV_OK;
    DDPDBG("=============== close tplay TDriver session ===============\n");
        /* Close session*/
    if (tplaySessionHandle.session_id != 0) // we have an valid session
    {
        mcRet = mc_close_session(&tplaySessionHandle);
        if (MC_DRV_OK != mcRet)
        {
            DDPERR("mc_close_session failed: %d @%s line %d\n", mcRet, __func__, __LINE__);
            memset(&tplaySessionHandle, 0, sizeof(tplaySessionHandle)); // clear handle value anyway
            return -1;
        }
    }
    memset(&tplaySessionHandle, 0, sizeof(tplaySessionHandle));

    mcRet = mc_free_wsm(mc_deviceId, (uint8_t *)pTplayTci);
    if (MC_DRV_OK != mcRet)
    {
        DDPERR("mc_free_wsm failed: %d @%s line %d\n", mcRet, __func__, __LINE__);
        return -1;
    }

    return 0;
}

// return 0 for success and -1 for error
static int set_tplay_handle_addr_request(void)
{
    int ret = 0;
    enum mc_result mcRet = MC_DRV_OK;

    DDPDBG("[SVP] set_tplay_handle_addr_request \n");

    open_tplay_driver_connection();
    if (tplaySessionHandle.session_id == 0)
    {
        DDPERR("[SVP] invalid tplay session \n");
        return -1;
    }

    DDPDBG("[SVP] handle_pa=0x%pa \n", &handle_pa);
    /* set other TCI parameter */
    pTplayTci->tplay_handle_low_addr = (uint32_t)handle_pa;
    pTplayTci->tplay_handle_high_addr = (uint32_t)(handle_pa >> 32);
    /* set TCI command */
    pTplayTci->cmd.header.commandId = CMD_TPLAY_REQUEST;

    /* notify the trustlet */
    DDPDBG("[SVP] notify Tlsec trustlet CMD_TPLAY_REQUEST \n");
    mcRet = mc_notify(&tplaySessionHandle);
    if (MC_DRV_OK != mcRet)
    {
        DDPERR("[SVP] mc_notify failed: %d @%s line %d\n", mcRet, __func__, __LINE__);
        ret = -1;
        goto _notify_to_trustlet_fail;
    }

    /* wait for response from the trustlet */
	mcRet = mc_wait_notification(&tplaySessionHandle, MC_INFINITE_TIMEOUT);
    if (MC_DRV_OK != mcRet)
    {
        DDPERR("[SVP] mc_wait_notification failed: %d @%s line %d\n", mcRet, __func__, __LINE__);
        ret = -1;
        goto _notify_from_trustlet_fail;
    }

    DDPDBG("[SVP] CMD_TPLAY_REQUEST result=%d, return code=%d\n", pTplayTci->result, pTplayTci->rsp.header.returnCode);

_notify_from_trustlet_fail:
_notify_to_trustlet_fail:
    close_tplay_driver_connection();

    return ret;
}

#ifdef TPLAY_DUMP_PA_DEBUG
static int dump_tplay_physcial_addr(void)
{
    DDPDBG("[SVP] dump_tplay_physcial_addr \n");
    int ret = 0;   
    enum mc_result mcRet = MC_DRV_OK;
    open_tplay_driver_connection();
    if (tplaySessionHandle.session_id == 0)
    {
        DDPERR("[SVP] invalid tplay session \n");
        return -1;
    }

    /* set TCI command */
    pTplayTci->cmd.header.commandId = CMD_TPLAY_DUMP_PHY; 
   
    /* notify the trustlet */        
    DDPMSG("[SVP] notify Tlsec trustlet CMD_TPLAY_DUMP_PHY \n");
    mcRet = mc_notify(&tplaySessionHandle);        
    if (MC_DRV_OK != mcRet)        
    {            
        DDPERR("[SVP] mc_notify failed: %d @%s line %d\n", mcRet, __func__, __LINE__); 
        ret = -1;
        goto _notify_to_trustlet_fail;
    }

    /* wait for response from the trustlet */
    mcRet = mc_wait_notification(&tplaySessionHandle, MC_INFINITE_TIMEOUT);
    if (MC_DRV_OK != mcRet)
    {            
        DDPERR("[SVP] mc_wait_notification failed: %d @%s line %d\n", mcRet, __func__, __LINE__);
        ret = -1;
        goto _notify_from_trustlet_fail;
    }

    DDPDBG("[SVP] CMD_TPLAY_DUMP_PHY result=%d, return code=%d\n", pTplayTci->result, pTplayTci->rsp.header.returnCode);

_notify_from_trustlet_fail:
_notify_to_trustlet_fail:
    close_tplay_driver_connection();

    return ret;
}
#endif // TPLAY_DUMP_PA_DEBUG

static int disp_path_notify_tplay_handle(unsigned int handle_value)
{
    int ret;
    static int executed = 0; // this function can execute only once

    if (0 == executed)
    {
        if (set_tplay_handle_addr_request())
			return -EFAULT;
        executed = 1;
    }

    ret = write_tplay_handle(handle_value);

#ifdef TPLAY_DUMP_PA_DEBUG
    dump_tplay_physcial_addr();
#endif // TPLAY_DUMP_PA_DEBUG

    return ret;
}
#endif // defined(CONFIG_TRUSTONIC_TEE_SUPPORT) && defined(CONFIG_MTK_SEC_VIDEO_PATH_SUPPORT)

static long disp_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    disp_node_struct *pNode = (disp_node_struct *)file->private_data;

#if defined(CONFIG_TRUSTONIC_TEE_SUPPORT) && defined(CONFIG_MTK_SEC_VIDEO_PATH_SUPPORT)
    if (DISP_IOCTL_SET_TPLAY_HANDLE == cmd)
    {
        unsigned int value;
        if(copy_from_user(&value, (void*)arg , sizeof(unsigned int)))
        {
            DDPERR("DISP_IOCTL_SET_TPLAY_HANDLE, copy_from_user failed\n");
            return -EFAULT;
        }
        if (disp_path_notify_tplay_handle(value)) {
            DDPERR("DISP_IOCTL_SET_TPLAY_HANDLE, disp_path_notify_tplay_handle failed\n");
            return -EFAULT;
        }
    }
#endif

    return 0;        
}

static long disp_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
#if defined(CONFIG_TRUSTONIC_TEE_SUPPORT) && defined(CONFIG_MTK_SEC_VIDEO_PATH_SUPPORT)
    if (DISP_IOCTL_SET_TPLAY_HANDLE == cmd)
    {
        return disp_unlocked_ioctl(file, cmd, arg);
    }
#endif

    return 0;		 
}

static int disp_open(struct inode *inode, struct file *file)
{
    disp_node_struct *pNode = NULL;

    DDPDBG("enter disp_open() process:%s\n",current->comm);

    //Allocate and initialize private data
    file->private_data = kmalloc(sizeof(disp_node_struct) , GFP_ATOMIC);
    if(NULL == file->private_data)
    {
        DDPMSG("Not enough entry for DDP open operation\n");
        return -ENOMEM;
    }

    pNode = (disp_node_struct *)file->private_data;
    pNode->open_pid = current->pid;
    pNode->open_tgid = current->tgid;
    INIT_LIST_HEAD(&(pNode->testList));
    spin_lock_init(&pNode->node_lock);

    return 0;

}

static ssize_t disp_read(struct file *file, char __user *data, size_t len, loff_t *ppos)
{
    return 0;
}

static int disp_release(struct inode *inode, struct file *file)
{
    disp_node_struct *pNode = NULL;
    unsigned int index = 0;
    DDPDBG("enter disp_release() process:%s\n",current->comm);

    pNode = (disp_node_struct *)file->private_data;

    spin_lock(&pNode->node_lock);

    spin_unlock(&pNode->node_lock);

    if(NULL != file->private_data)
    {
        kfree(file->private_data);
        file->private_data = NULL;
    }

    return 0;
}

static int disp_flush(struct file * file , fl_owner_t a_id)
{
    return 0;
}

static int disp_mmap(struct file * file, struct vm_area_struct * a_pstVMArea)
{
    return 0;
}

struct dispsys_device
{
	void __iomem *regs[DISP_REG_NUM];
	struct device *dev;
	int irq[DISP_REG_NUM];
};
static struct dispsys_device *dispsys_dev;
static int nr_dispsys_dev = 0;
unsigned int dispsys_irq[DISP_REG_NUM] = {0};
unsigned long dispsys_reg[DISP_REG_NUM] = {0};
unsigned long mipi_tx_reg=0;
unsigned long dsi_reg_va = 0;

extern irqreturn_t disp_irq_handler(int irq, void *dev_id);
extern int ddp_path_init();

static int disp_is_intr_enable(DISP_REG_ENUM module)
{
    switch(module)
    {
        case DISP_REG_OVL0   :
        case DISP_REG_OVL1   :
        case DISP_REG_RDMA0  :
        case DISP_REG_RDMA1  :
        case DISP_REG_MUTEX  :
        case DISP_REG_DSI0   :  
        case DISP_REG_AAL    :
        case DISP_REG_CONFIG :
            return 1;     
        case DISP_REG_WDMA1  :  //FIXME: WDMA1 intr is abonrmal FPGA so mark first, enable after EVB works
        case DISP_REG_COLOR  :
        case DISP_REG_CCORR  :
        case DISP_REG_GAMMA  :
        case DISP_REG_DITHER :
        case DISP_REG_UFOE   :
        case DISP_REG_OD   :
        case DISP_REG_PWM    :
        case DISP_REG_DPI0   :
        case DISP_REG_SMI_LARB0 :
        case DISP_REG_SMI_COMMON:
        case DISP_REG_CONFIG2:
        case DISP_REG_CONFIG3:
		case DISP_REG_IO_DRIVING1:
		case DISP_REG_IO_DRIVING2:
		case DISP_REG_IO_DRIVING3:
		case DISP_REG_TIMING1:
		case DISP_REG_TIMING2:
		case DISP_REG_TIMING3:
		case DISP_REG_MIPI:
            return 0;

        case DISP_REG_WDMA0  :
#if defined(CONFIG_TRUSTONIC_TEE_SUPPORT) && defined(CONFIG_MTK_SEC_VIDEO_PATH_SUPPORT)
			return 0;
#else
			return 1;
#endif

        default: return 0;
    }
}

extern int m4u_enable_tf(int port, bool fgenable);
m4u_callback_ret_t disp_m4u_callback(int port, unsigned long mva, void* data)
{
    DISP_MODULE_ENUM module = DISP_MODULE_OVL0;
    DDPERR("fault call port=%d, mva=0x%lx, data=0x%p\n", port, mva, data);
    switch(port)
    {
		case M4U_PORT_DISP_OVL0 : module = DISP_MODULE_OVL0; break;
		case M4U_PORT_DISP_RDMA0: module = DISP_MODULE_RDMA0; break;
		case M4U_PORT_DISP_WDMA0: module = DISP_MODULE_WDMA0; break;
#if defined(MTK_FB_OVL1_SUPPORT)
		case M4U_PORT_DISP_OVL1 : module = DISP_MODULE_OVL1; break;
#endif
#if defined(MTK_FB_RDMA1_SUPPORT)
		case M4U_PORT_DISP_RDMA1: module = DISP_MODULE_RDMA1; break;
#endif
		default: DDPERR("unknown port=%d\n", port);
    }
    ddp_dump_analysis(module);
    ddp_dump_reg(module);

#if defined(OVL_CASCADE_SUPPORT)
    // always dump 2 OVL
    if(module==DISP_MODULE_OVL0)
    {
        ddp_dump_analysis(DISP_MODULE_OVL1);
    }
    else if(module==DISP_MODULE_OVL1)
    {
        ddp_dump_analysis(DISP_MODULE_OVL0);
    }
#endif
    m4u_enable_tf(port, 0);  // disable translation fault after it happens to avoid prinkt too much issues(log is override)
}

void disp_m4u_tf_disable(void)
{
    // m4u_enable_tf(M4U_PORT_DISP_OVL0, 0);
#if defined(OVL_CASCADE_SUPPORT)
    m4u_enable_tf(M4U_PORT_DISP_OVL1, 0);
#endif
}

#if defined(CONFIG_TRUSTONIC_TEE_SUPPORT) && (CONFIG_MTK_SEC_VIDEO_PATH_SUPPORT)
static struct miscdevice disp_misc_dev;
#endif
/* Kernel interface */
static struct file_operations disp_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl = disp_unlocked_ioctl,
	.compat_ioctl   = disp_compat_ioctl,
	.open		= disp_open,
	.release	= disp_release,
	.flush		= disp_flush,
	.read       = disp_read,
#if 1
	.mmap       = disp_mmap
#endif
};

static int disp_probe(struct platform_device *pdev)
{
    struct class_device;
	int ret;
	int i;
    int new_count;
    static unsigned int disp_probe_cnt = 0;

    if(disp_probe_cnt!=0)
    {
        return 0;
    }

#if defined(CONFIG_TRUSTONIC_TEE_SUPPORT) && (CONFIG_MTK_SEC_VIDEO_PATH_SUPPORT)
	disp_misc_dev.minor = MISC_DYNAMIC_MINOR;
	disp_misc_dev.name = "mtk_disp";
	disp_misc_dev.fops = &disp_fops;
	disp_misc_dev.parent = NULL;
	ret = misc_register(&disp_misc_dev);
	if (ret) {
		pr_err("disp: fail to create mtk_disp node\n");
		return ERR_PTR(ret);
	}

	// secure video path implementation: a physical address is allocated to place a handle for decryption buffer.
	init_tplay_handle(&(pdev->dev)); // non-zero value for valid VA
#endif
	
    new_count = nr_dispsys_dev + 1;
    dispsys_dev = krealloc(dispsys_dev, 
        sizeof(struct dispsys_device) * new_count, GFP_KERNEL);
    if (!dispsys_dev) {
        DDPERR("Unable to allocate dispsys_dev\n");
        return -ENOMEM;
    }
	
    dispsys_dev = &(dispsys_dev[nr_dispsys_dev]);
    dispsys_dev->dev = &pdev->dev;

    /* iomap registers and irq*/
    for(i=0;i<DISP_REG_NUM;i++)
    {
        dispsys_dev->regs[i] = of_iomap(pdev->dev.of_node, i);
        if (!dispsys_dev->regs[i]) {
            continue;  // skip
        }
		dispsys_reg[i] = dispsys_dev->regs[i];
        /* get IRQ ID and request IRQ */
        dispsys_dev->irq[i] = irq_of_parse_and_map(pdev->dev.of_node, i);
        dispsys_irq[i] = dispsys_dev->irq[i];
        if (!dispsys_dev->irq[i]) {
            continue;  // skip
        }
        
        if(disp_is_intr_enable(i)==1)
        {
            ret = request_irq(dispsys_dev->irq[i], (irq_handler_t)disp_irq_handler, 
                        IRQF_TRIGGER_NONE, DISP_DEVNAME,  NULL);  // IRQF_TRIGGER_NONE dose not take effect here, real trigger mode set in dts file
            if (ret) {
                DDPERR("Unable to request IRQ, request_irq fail, i=%d, irq=%d \n", i, dispsys_dev->irq[i]);
                return ret;
            }
        }            

        DDPMSG("DT, i=%d, module=%s, map_addr=%p, map_irq=%d, reg_pa=0x%x, irq=%d \n", 
            i, ddp_get_reg_module_name(i), dispsys_dev->regs[i], dispsys_dev->irq[i], ddp_reg_pa_base[i], ddp_irq_num[i]);
    }    
    nr_dispsys_dev = new_count;
    //mipi tx reg map here
	dsi_reg_va = dispsys_reg[DISP_REG_DSI0];
    mipi_tx_reg = dispsys_reg[DISP_REG_MIPI];
	DPI_REG = (PDPI_REGS)dispsys_reg[DISP_REG_DPI0];

	////// power on MMSYS for early porting
#ifdef CONFIG_FPGA_EARLY_PORTING
	pr_debug("[DISP Probe] power MMSYS:0x%lx,0x%lx\n",DISP_REG_CONFIG_MMSYS_CG_CLR0,DISP_REG_CONFIG_MMSYS_CG_CLR1);
	DISP_REG_SET(0,DISP_REG_CONFIG_MMSYS_CG_CLR0,0xFFFFFFFF);
	DISP_REG_SET(0,DISP_REG_CONFIG_MMSYS_CG_CLR1,0xFFFFFFFF);
	DISP_REG_SET(0,DISPSYS_CONFIG_BASE + 0xC00,0x0);//fpga should set this register
#endif
	//////
#ifdef MTKFB_NO_M4U
	DISP_REG_SET(0,DISP_REG_SMI_LARB_MMU_EN,0x0);//m4u disable
#endif

	// init arrays
	ddp_path_init();

    // init M4U callback
    DDPMSG("register m4u callback\n");
    m4u_register_fault_callback(M4U_PORT_DISP_OVL0, disp_m4u_callback, 0);
    m4u_register_fault_callback(M4U_PORT_DISP_RDMA0, disp_m4u_callback, 0);
    m4u_register_fault_callback(M4U_PORT_DISP_WDMA0, disp_m4u_callback, 0);
#if defined(MTK_FB_OVL1_SUPPORT)
    m4u_register_fault_callback(M4U_PORT_DISP_OVL1, disp_m4u_callback, 0);
#endif
#if defined(MTK_FB_RDMA1_SUPPORT)
    m4u_register_fault_callback(M4U_PORT_DISP_RDMA1, disp_m4u_callback, 0);
#endif


	DDPMSG("dispsys probe done.\n");	
	//NOT_REFERENCED(class_dev);

	// bus hang issue error intr enable
	// when MMSYS clock off but GPU/MJC/PWM clock on, avoid display hang and trigger error intr
	{
        DISP_REG_SET_FIELD(0, MMSYS_TO_MFG_TX_ERROR, DISP_REG_CONFIG_MMSYS_INTEN, 1);
        DISP_REG_SET_FIELD(0, MMSYS_TO_MJC_TX_ERROR, DISP_REG_CONFIG_MMSYS_INTEN, 1);
        DISP_REG_SET_FIELD(0, PWM0_APB_TX_ERROR, DISP_REG_CONFIG_MMSYS_INTEN, 1);
	}
	
    /* sysfs */
     DDPMSG("sysfs disp +");
    //add kobject
    if(kobject_init_and_add(&kdispobj, &disp_kobj_ktype, NULL, "disp") <0){
		DDPERR("fail to add disp\n");
		return -ENOMEM;
    }	
	
	return 0;
}

static int disp_remove(struct platform_device *pdev)
{
    /* sysfs */
    kobject_put(&kdispobj);

#if defined(CONFIG_TRUSTONIC_TEE_SUPPORT) && (CONFIG_MTK_SEC_VIDEO_PATH_SUPPORT)
	misc_deregister(&disp_misc_dev);
#endif
    return 0;
}

static void disp_shutdown(struct platform_device *pdev)
{
	/* Nothing yet */
}


/* PM suspend */
static int disp_suspend(struct platform_device *pdev, pm_message_t mesg)
{
    pr_debug("\n\n==== DISP suspend is called ====\n");


    return 0;
}

/* PM resume */
static int disp_resume(struct platform_device *pdev)
{
    pr_debug("\n\n==== DISP resume is called ====\n");


    return 0;
}


static const struct of_device_id dispsys_of_ids[] = {
	{ .compatible = "mediatek,DISPSYS", },
	{}
};

static struct platform_driver dispsys_of_driver = {
	.driver = {
		.name = DISP_DEVNAME,
		.owner = THIS_MODULE,
		.of_match_table = dispsys_of_ids,
	},
	.probe    = disp_probe,
	.remove   = disp_remove,
    .shutdown = disp_shutdown,
    .suspend  = disp_suspend,
    .resume   = disp_resume,
};

static int __init disp_init(void)
{
    int ret = 0;

    DDPMSG("Register the disp driver\n");
    if(platform_driver_register(&dispsys_of_driver))
    {
        DDPERR("failed to register disp driver\n");
        //platform_device_unregister(&disp_device);
        ret = -ENODEV;
        return ret;
    }

    return 0;
}

static void __exit disp_exit(void)
{
    platform_driver_unregister(&dispsys_of_driver);
}

#ifndef MTK_FB_DO_NOTHING
module_init(disp_init);
module_exit(disp_exit);
#endif
MODULE_AUTHOR("Tzu-Meng, Chung <Tzu-Meng.Chung@mediatek.com>");
MODULE_DESCRIPTION("Display subsystem Driver");
MODULE_LICENSE("GPL");
