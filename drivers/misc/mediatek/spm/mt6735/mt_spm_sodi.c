#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/delay.h>  

#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#endif

#include <mach/irqs.h>
#include <mach/mt_cirq.h>
#include <mach/mt_spm_idle.h>
#include <mach/mt_cpuidle.h>
#include <mach/mt_gpt.h>
#include <mach/mt_cpufreq.h>
#include <mach/mt_clkmgr.h>
#include <mach/mt_boot.h>
#include <mach/upmu_common.h>
#include <mach/mt_vcore_dvfs.h>
#include <mach/mt_secure_api.h>

#include "mt_spm_internal.h"


/**************************************
 * only for internal debug
 **************************************/

#define SODI_TAG     "[SODI] "
#define sodi_emerg(fmt, args...)	pr_emerg(SODI_TAG fmt, ##args)
#define sodi_alert(fmt, args...)	pr_alert(SODI_TAG fmt, ##args)
#define sodi_crit(fmt, args...)		pr_crit(SODI_TAG fmt, ##args)
#define sodi_err(fmt, args...)		pr_err(SODI_TAG fmt, ##args)
#define sodi_warn(fmt, args...)		pr_warn(SODI_TAG fmt, ##args)
#define sodi_notice(fmt, args...)	pr_notice(SODI_TAG fmt, ##args)
#define sodi_info(fmt, args...)		pr_info(SODI_TAG fmt, ##args)
#define sodi_debug(fmt, args...)	pr_info(SODI_TAG fmt, ##args)	/* pr_debug show nothing */

#define SPM_BYPASS_SYSPWREQ         0 //JTAG is used

#define SODI_DVT_APxGPT             0

#if SODI_DVT_APxGPT
#define SPM_DEBUG_FW                0
#define SODI_DVT_STEP_BY_STEP       0 
#define SODI_DVT_WAKEUP             0
#define SODI_DVT_PCM_TIMER_DISABLE  0
#define SPM_SODI_DUMP_REGS          0 
#define SODI_DVT_SPM_MEM_RW_TEST    0
#else
#define SPM_DEBUG_FW                0
#define SODI_DVT_STEP_BY_STEP       0 
#define SODI_DVT_WAKEUP             0
#define SODI_DVT_PCM_TIMER_DISABLE  0
#define SPM_SODI_DUMP_REGS          0 
#define SODI_DVT_SPM_MEM_RW_TEST    0
#endif

#define SPM_USE_TWAM_DEBUG	        0

#if SODI_DVT_SPM_MEM_RW_TEST
#define SODI_DVT_MAGIC_NUM 			0xa5a5a5a5 
static u32 magicArray[16]=
{	
	SODI_DVT_MAGIC_NUM,SODI_DVT_MAGIC_NUM,SODI_DVT_MAGIC_NUM,SODI_DVT_MAGIC_NUM,
	SODI_DVT_MAGIC_NUM,SODI_DVT_MAGIC_NUM,SODI_DVT_MAGIC_NUM,SODI_DVT_MAGIC_NUM,
	SODI_DVT_MAGIC_NUM,SODI_DVT_MAGIC_NUM,SODI_DVT_MAGIC_NUM,SODI_DVT_MAGIC_NUM,
	SODI_DVT_MAGIC_NUM,SODI_DVT_MAGIC_NUM,SODI_DVT_MAGIC_NUM,SODI_DVT_MAGIC_NUM,	
};
#endif

#if SODI_DVT_WAKEUP
#define WAKE_SRC_FOR_SODI WAKE_SRC_EINT //WAKE_SRC_GPT | WAKE_SRC_EINT
#else
#define WAKE_SRC_FOR_SODI \
    (WAKE_SRC_KP | WAKE_SRC_GPT | WAKE_SRC_EINT | WAKE_SRC_CONN_WDT | WAKE_SRC_CCIF0_MD | WAKE_SRC_CCIF1_MD | \
     WAKE_SRC_CONN2AP | WAKE_SRC_USB_CD | WAKE_SRC_USB_PDN | WAKE_SRC_SEJ | WAKE_SRC_AFE | \
     WAKE_SRC_CIRQ | WAKE_SRC_MD1_VRF18_WAKE | WAKE_SRC_SYSPWREQ | WAKE_SRC_MD_WDT | WAKE_SRC_C2K_WDT | WAKE_SRC_CLDMA_MD)
#endif 

#define reg_read(addr)         __raw_readl(IOMEM(addr))
#define reg_write(addr, val)   mt_reg_sync_writel((val), ((void *)addr))

#if defined (CONFIG_OF)
#define MCUCFG_NODE "mediatek,MCUCFG"
static unsigned long mcucfg_base;
static unsigned long mcucfg_phys_base;
#undef MCUCFG_BASE
#define MCUCFG_BASE             (mcucfg_base)         

#else //#if defined (CONFIG_OF)
#undef MCUCFG_BASE
#define MCUCFG_BASE             0xF0200000
#endif //#if defined (CONFIG_OF)

// MCUCFG registers
#define MP0_AXI_CONFIG       (MCUCFG_BASE + 0x2C)
#define MP0_AXI_CONFIG_PHYS  (mcucfg_phys_base + 0x2C) 
#define ACINACTM                (1<<4)

#if defined(CONFIG_ARM_PSCI) || defined(CONFIG_MTK_PSCI)
#define MCUSYS_SMC_WRITE(addr, val)  mcusys_smc_write_phy(addr##_PHYS, val)
#else
#define MCUSYS_SMC_WRITE(addr, val)  mcusys_smc_write(addr, val)
#endif


#ifdef CONFIG_MTK_RAM_CONSOLE
#define SPM_AEE_RR_REC 1
#else
#define SPM_AEE_RR_REC 0
#endif

#if SPM_AEE_RR_REC
enum spm_sodi_step
{
	SPM_SODI_ENTER=0,
	SPM_SODI_ENTER_SPM_FLOW,
	SPM_SODI_ENTER_WFI,
	SPM_SODI_LEAVE_WFI,
	SPM_SODI_LEAVE_SPM_FLOW,
	SPM_SODI_LEAVE,
	SPM_SODI_VCORE_HPM,
	SPM_SODI_VCORE_LPM
};
#endif

static const u32 sodi_binary[] = {
	0x814e0001, 0xd8200405, 0x17c07c1f, 0x18c0001f, 0x102135cc, 0x1910001f,
	0x102135cc, 0x813f8404, 0xe0c00004, 0x1910001f, 0x102135cc, 0x81411801,
	0xd8000285, 0x17c07c1f, 0x18c0001f, 0x10006240, 0xe0e00016, 0xe0e0001e,
	0xe0e0000e, 0xe0e0000f, 0x803e0400, 0x1b80001f, 0x20000222, 0x80380400,
	0x1b80001f, 0x20000280, 0x803b0400, 0x1b80001f, 0x2000001a, 0x803d0400,
	0x1b80001f, 0x20000208, 0x80340400, 0x80310400, 0x1950001f, 0x10006b04,
	0x81439401, 0xd80009c5, 0x17c07c1f, 0x81431801, 0xd80009c5, 0x17c07c1f,
	0x1b80001f, 0x2000000a, 0x18c0001f, 0x10006240, 0xe0e0000d, 0x81411801,
	0xd80009c5, 0x17c07c1f, 0x1b80001f, 0x20000020, 0x18c0001f, 0x102130f0,
	0x1910001f, 0x102130f0, 0xa9000004, 0x10000000, 0xe0c00004, 0x1b80001f,
	0x2000000a, 0x89000004, 0xefffffff, 0xe0c00004, 0x18c0001f, 0x102140f4,
	0x1910001f, 0x102140f4, 0xa9000004, 0x02000000, 0xe0c00004, 0x1b80001f,
	0x2000000a, 0x89000004, 0xfdffffff, 0xe0c00004, 0x1910001f, 0x102140f4,
	0x81fa0407, 0x81f08407, 0xe8208000, 0x10006354, 0x001efaa1, 0xa1d80407,
	0xa1de8407, 0xa1df0407, 0xc2003a80, 0x1211041f, 0x1b00001f, 0xaf7cefff,
	0xf0000000, 0x17c07c1f, 0x1a50001f, 0x10006608, 0x80c9a401, 0x810ba401,
	0x10920c1f, 0xa0979002, 0x80ca2401, 0xa0938c02, 0xa0958402, 0x8080080d,
	0xd8200ec2, 0x17c07c1f, 0x81f08407, 0xa1d80407, 0xa1de8407, 0xa1df0407,
	0x1b00001f, 0x2f7ce7ff, 0x1b80001f, 0x20000004, 0xd800158c, 0x17c07c1f,
	0x1b00001f, 0xaf7cefff, 0xd0001580, 0x17c07c1f, 0x81f80407, 0x81fe8407,
	0x81ff0407, 0x1880001f, 0x10006320, 0xc0c02560, 0xe080000f, 0xd8000d03,
	0x17c07c1f, 0x1880001f, 0x10006320, 0xe080001f, 0xa0c01403, 0xd8000d03,
	0x17c07c1f, 0xa1da0407, 0xa0110400, 0xa0140400, 0x1950001f, 0x10006b04,
	0x81439401, 0xd80014e5, 0x17c07c1f, 0x81431801, 0xd80014e5, 0x17c07c1f,
	0xa0180400, 0xa01b0400, 0xa01d0400, 0x1b80001f, 0x20000068, 0xa01e0400,
	0x1b80001f, 0x20000104, 0x81411801, 0xd80013e5, 0x17c07c1f, 0x18c0001f,
	0x10006240, 0xc0c024c0, 0x17c07c1f, 0x18c0001f, 0x102135cc, 0x1910001f,
	0x102135cc, 0xa11f8404, 0xe0c00004, 0x1910001f, 0x102135cc, 0x0280040a,
	0xc2003a80, 0x1211841f, 0x1b00001f, 0x6f7ce7ff, 0xf0000000, 0x17c07c1f,
	0x81459801, 0xd8001845, 0x17c07c1f, 0x1a00001f, 0x10006604, 0xe2200004,
	0xc0c032e0, 0x17c07c1f, 0xc0c033c0, 0x1093041f, 0xe2200003, 0xc0c032e0,
	0x17c07c1f, 0xc0c033c0, 0x1093041f, 0xe2200002, 0xc0c032e0, 0x17c07c1f,
	0xc0c033c0, 0x1093041f, 0x80378400, 0xc2003a80, 0x1210041f, 0x1b00001f,
	0x2f7cf7ff, 0xf0000000, 0x17c07c1f, 0x1b00001f, 0x2f7ce7ff, 0x1b80001f,
	0x20000004, 0xd8001d0c, 0x17c07c1f, 0x81459801, 0xd8001c65, 0x17c07c1f,
	0x1a00001f, 0x10006604, 0xe2200003, 0xc0c032e0, 0x17c07c1f, 0xc0c033c0,
	0x1093041f, 0xe2200004, 0xc0c032e0, 0x17c07c1f, 0xc0c033c0, 0x1093041f,
	0xe2200005, 0xc0c032e0, 0x17c07c1f, 0xc0c033c0, 0x1093041f, 0xc2003a80,
	0x1210841f, 0xa0178400, 0x1b00001f, 0xaf7cefff, 0xf0000000, 0x17c07c1f,
	0x81441801, 0xd8201fc5, 0x17c07c1f, 0x1a00001f, 0x10006604, 0xe2200004,
	0xc0c032e0, 0x17c07c1f, 0xc0c033c0, 0x1093041f, 0xe2200003, 0xc0c032e0,
	0x17c07c1f, 0xc0c033c0, 0x1093041f, 0xe2200002, 0xc0c032e0, 0x17c07c1f,
	0xc0c033c0, 0x1093041f, 0x1a00001f, 0x100062c4, 0x1890001f, 0x100062c4,
	0xa0908402, 0xe2000002, 0x1b00001f, 0x00001001, 0xf0000000, 0x17c07c1f,
	0x1a00001f, 0x100062c4, 0x1890001f, 0x100062c4, 0x80b08402, 0xe2000002,
	0x81441801, 0xd8202445, 0x17c07c1f, 0x1a00001f, 0x10006604, 0xe2200003,
	0xc0c032e0, 0x17c07c1f, 0xc0c033c0, 0x1093041f, 0xe2200004, 0xc0c032e0,
	0x17c07c1f, 0xc0c033c0, 0x1093041f, 0xe2200005, 0xc0c032e0, 0x17c07c1f,
	0xc0c033c0, 0x1093041f, 0x1b00001f, 0x00000801, 0xf0000000, 0x17c07c1f,
	0xe0e0000f, 0xe0e0001e, 0xe0e00012, 0xf0000000, 0x17c07c1f, 0x1112841f,
	0xa1d08407, 0x1a50001f, 0x10006608, 0x8209a401, 0x814ba401, 0x1092201f,
	0xa0979402, 0x820a2401, 0xa093a002, 0xa0958402, 0xd82027a4, 0x8140080d,
	0xd8002805, 0x10c07c1f, 0x80eab401, 0xd80026c3, 0x01200404, 0x1a00001f,
	0x10006814, 0xe2000003, 0xf0000000, 0x17c07c1f, 0xd800294a, 0x17c07c1f,
	0xe2e00036, 0x17c07c1f, 0x17c07c1f, 0xe2e0003e, 0x1380201f, 0xe2e0003c,
	0xd8202a8a, 0x17c07c1f, 0x1b80001f, 0x20000018, 0xe2e0007c, 0x1b80001f,
	0x20000003, 0xe2e0005c, 0xe2e0004c, 0xe2e0004d, 0xf0000000, 0x17c07c1f,
	0xa1d10407, 0x1b80001f, 0x20000020, 0xf0000000, 0x17c07c1f, 0xd8002c0a,
	0x17c07c1f, 0xe2e0004f, 0xe2e0006f, 0xe2e0002f, 0xd8202caa, 0x17c07c1f,
	0xe2e0002e, 0xe2e0003e, 0xe2e00032, 0xf0000000, 0x17c07c1f, 0x81429801,
	0xd8002ee5, 0x17c07c1f, 0x18c0001f, 0x10001138, 0x1910001f, 0x10001138,
	0xa1108404, 0xe0c00004, 0x1910001f, 0x10001138, 0x1a00001f, 0x10006604,
	0xe2200007, 0xc0c032e0, 0x17c07c1f, 0x1a00001f, 0x100062c4, 0x1890001f,
	0x100062c4, 0xa0940402, 0xe2000002, 0xf0000000, 0x17c07c1f, 0x81429801,
	0xd80031e5, 0x17c07c1f, 0x18c0001f, 0x10001138, 0x1910001f, 0x10001138,
	0x81308404, 0xe0c00004, 0x1910001f, 0x10001138, 0x1a00001f, 0x10006604,
	0xe2200007, 0xc0c032e0, 0x17c07c1f, 0x1a00001f, 0x100062c4, 0x1890001f,
	0x100062c4, 0x80b40402, 0xe2000002, 0xf0000000, 0x17c07c1f, 0x18d0001f,
	0x10006604, 0x10cf8c1f, 0xd82032e3, 0x17c07c1f, 0xf0000000, 0x17c07c1f,
	0x81499801, 0xd8203525, 0x17c07c1f, 0xd8203a42, 0x17c07c1f, 0x18d0001f,
	0x40000000, 0x18d0001f, 0x70000000, 0xd8003422, 0x00a00402, 0x814a1801,
	0xd8203685, 0x17c07c1f, 0xd8203a42, 0x17c07c1f, 0x18d0001f, 0x40000000,
	0x18d0001f, 0x80000000, 0xd8003582, 0x00a00402, 0x814a9801, 0xd82037e5,
	0x17c07c1f, 0xd8203a42, 0x17c07c1f, 0x18d0001f, 0x40000000, 0x18d0001f,
	0xc0000000, 0xd80036e2, 0x00a00402, 0x814c1801, 0xd8203945, 0x17c07c1f,
	0xd8203a42, 0x17c07c1f, 0x18d0001f, 0x40000000, 0x18d0001f, 0xa0000000,
	0xd8003842, 0x00a00402, 0xd8203a42, 0x17c07c1f, 0x18d0001f, 0x40000000,
	0x18d0001f, 0x40000000, 0xd8003942, 0x00a00402, 0xf0000000, 0x17c07c1f,
	0x18c0001f, 0x10006b18, 0x1910001f, 0x10006b18, 0xa1002004, 0xe0c00004,
	0xf0000000, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x1840001f, 0x00000001, 0xa1d48407, 0x1990001f,
	0x10006b08, 0xe8208000, 0x10006b18, 0x00000000, 0xe8208000, 0x10006b6c,
	0x00000000, 0x1910001f, 0x100062c4, 0x81491801, 0xd8204285, 0x80841001,
	0xc8e02ce2, 0x17c07c1f, 0xd0004400, 0x17c07c1f, 0x18d0001f, 0x10006284,
	0x81410c01, 0x91601402, 0xd8204405, 0x17c07c1f, 0xc8e02ce2, 0x1240081f,
	0xd8204409, 0x17c07c1f, 0xc0c02fe0, 0x17c07c1f, 0x81441801, 0xd8204705,
	0x17c07c1f, 0x1910001f, 0x100062c4, 0x80809001, 0x18c0001f, 0x2f7ce7ff,
	0x81600801, 0xa0d59403, 0xa0d60803, 0x13000c1f, 0x80849001, 0x1a00001f,
	0x10006b0c, 0x1950001f, 0x10006b0c, 0xa0908805, 0xe2000002, 0xe8208000,
	0x10006310, 0x0b160308, 0x1b80001f, 0xd00f0000, 0x81469801, 0xd8204865,
	0x17c07c1f, 0x1b80001f, 0xd00f0000, 0x8880000c, 0x2f7ce7ff, 0xd8006282,
	0x17c07c1f, 0xd00048a0, 0x17c07c1f, 0x1b80001f, 0x500f0000, 0xe8208000,
	0x10006354, 0x001efaa1, 0xc0c02ac0, 0x81401801, 0xd8004dc5, 0x17c07c1f,
	0x81f60407, 0x18c0001f, 0x10006200, 0xc0c02b60, 0x12807c1f, 0xe8208000,
	0x1000625c, 0x00000001, 0x1b80001f, 0x20000080, 0xc0c02b60, 0x1280041f,
	0x18c0001f, 0x10006208, 0xc0c02b60, 0x12807c1f, 0xe8208000, 0x10006248,
	0x00000000, 0x1b80001f, 0x20000080, 0xc0c02b60, 0x1280041f, 0x18c0001f,
	0x10006290, 0xc0c02b60, 0x12807c1f, 0xc0c02b60, 0x1280041f, 0x18c0001f,
	0x100062dc, 0xe0c00001, 0xc2003a80, 0x1212041f, 0x81469801, 0xd8004ea5,
	0x17c07c1f, 0x8880000c, 0x2f7ce7ff, 0xd8005de2, 0x17c07c1f, 0x18c0001f,
	0x10006294, 0xe0f07fff, 0xe0e00fff, 0xe0e000ff, 0x81449801, 0xd8005185,
	0x17c07c1f, 0x1a00001f, 0x10006604, 0xe2200006, 0xc0c032e0, 0x17c07c1f,
	0x1a00001f, 0x10006604, 0xe2200001, 0xc0c032e0, 0x17c07c1f, 0x18c0001f,
	0x10001130, 0x1910001f, 0x10006b14, 0xe0c00004, 0xa1d38407, 0xa0108400,
	0xa0148400, 0xa01b8400, 0xa0188400, 0x81441801, 0xd8205505, 0x17c07c1f,
	0x1910001f, 0x10006b0c, 0x1a00001f, 0x100062c4, 0x1950001f, 0x100062c4,
	0x80809001, 0x81748405, 0xa1548805, 0xe2000005, 0x1a00001f, 0x10006b08,
	0x1950001f, 0x10006b08, 0xa1558805, 0xe2000005, 0x1a00001f, 0x10006b0c,
	0x81308404, 0xe2000004, 0xe8208000, 0x10006310, 0x0b1600f8, 0x1b00001f,
	0x2f7cf7ff, 0x81439801, 0xd82056a5, 0x17c07c1f, 0xe8208000, 0x10006310,
	0x0b160008, 0x1b00001f, 0x2f7ce7ff, 0x12807c1f, 0x1b80001f, 0x90100000,
	0x1ac0001f, 0x10006b6c, 0xe2c0000a, 0xe8208000, 0x10006310, 0x0b160008,
	0x80c10001, 0xc8c00003, 0x17c07c1f, 0x814f9801, 0xd80058e5, 0x17c07c1f,
	0x80c78001, 0xc8c015c3, 0x17c07c1f, 0x1a00001f, 0x100062c4, 0x1890001f,
	0x100062c4, 0xa0908402, 0xe2000002, 0x1b00001f, 0x2f7ce7ff, 0x18c0001f,
	0x10006294, 0xe0e001fe, 0xe0e003fc, 0xe0e007f8, 0xe0e00ff0, 0x1b80001f,
	0x20000020, 0xe0f07ff0, 0xe0f07f00, 0x81449801, 0xd8005c85, 0x17c07c1f,
	0x1a00001f, 0x10006604, 0xe2200006, 0xc0c032e0, 0x17c07c1f, 0xe2200000,
	0xc0c032e0, 0x17c07c1f, 0x80388400, 0x1b80001f, 0x20000300, 0x803b8400,
	0x1b80001f, 0x20000300, 0x80348400, 0x1b80001f, 0x20000104, 0x10007c1f,
	0x81f38407, 0x81401801, 0xd8006285, 0x17c07c1f, 0x18c0001f, 0x100062dc,
	0xe0c0001f, 0x18c0001f, 0x10006290, 0x1212841f, 0xc0c02840, 0x12807c1f,
	0xc0c02840, 0x1280041f, 0x18c0001f, 0x10006208, 0x1212841f, 0xc0c02840,
	0x12807c1f, 0xe8208000, 0x10006248, 0x00000001, 0x1b80001f, 0x20000080,
	0xc0c02840, 0x1280041f, 0x18c0001f, 0x10006200, 0x1212841f, 0xc0c02840,
	0x12807c1f, 0xe8208000, 0x1000625c, 0x00000000, 0x1b80001f, 0x20000080,
	0xc0c02840, 0x1280041f, 0x19c0001f, 0x60415820, 0x1ac0001f, 0x44bb44bb,
	0xc2003a80, 0x1213841f, 0x18c0001f, 0x10006b14, 0xe0c0000c, 0x18c0001f,
	0x10006b68, 0x1950001f, 0x100063c0, 0xe0c00005, 0x81441801, 0xd82066c5,
	0x10c0041f, 0x1910001f, 0x100062c4, 0x80849001, 0x1a00001f, 0x10006b0c,
	0x1910001f, 0x10006b0c, 0xa0908804, 0xe2000002, 0x81441801, 0xd82066c5,
	0x10c0041f, 0x18c0001f, 0x00001801, 0xe8208000, 0x10006310, 0x0b160308,
	0x13000c1f, 0x1b80001f, 0x900a0000, 0x88900001, 0x10006814, 0xd82065c2,
	0x17c07c1f, 0x18d0001f, 0x10006b6c, 0x78a00003, 0x0000beef, 0xd8006ce2,
	0x17c07c1f, 0x1900001f, 0x10006014, 0x1950001f, 0x10006014, 0xa1508405,
	0xe1000005, 0x1900001f, 0x10006814, 0xe100001f, 0x812ab401, 0xd8006984,
	0x17c07c1f, 0x1880001f, 0x10006284, 0x18d0001f, 0x10006284, 0x80f20403,
	0xe0800003, 0x80f08403, 0xe0800003, 0x1900001f, 0x10006014, 0x1950001f,
	0x10006014, 0x81708405, 0xe1000005, 0x1900001f, 0x10006b6c, 0xe100001f,
	0x1910001f, 0x100062c4, 0x80841001, 0xc8e02ce2, 0x17c07c1f, 0xd00065c0,
	0x17c07c1f, 0x1910001f, 0x10006b0c, 0x1a00001f, 0x100062c4, 0x1950001f,
	0x100062c4, 0x80809001, 0x81748405, 0xa1548805, 0xe2000005, 0x1ac0001f,
	0x55aa55aa, 0x10007c1f, 0xf0000000
};
static struct pcm_desc sodi_pcm = {
	.version	= "pcm_sodi_v9.17.4.8_20150901-slow_dvs",
	.base		= sodi_binary,
	.size		= 885,
	.sess		= 2,
	.replace	= 0,
	.vec0		= EVENT_VEC(30, 1, 0, 0),	/* FUNC_APSRC_WAKEUP */
	.vec1		= EVENT_VEC(31, 1, 0, 92),	/* FUNC_APSRC_SLEEP */
	.vec2		= EVENT_VEC(11, 1, 0, 174),	/* FUNC_VCORE_HIGH */
	.vec3		= EVENT_VEC(12, 1, 0, 201),	/* FUNC_VCORE_LOW */
	.vec4		= EVENT_VEC(11, 1, 0, 234),	/* FUNC_VCORE_HIGH2 */
	.vec5		= EVENT_VEC(12, 1, 0, 264),	/* FUNC_VCORE_LOW2 */
};

static struct pwr_ctrl sodi_ctrl = {
	.wake_src		= WAKE_SRC_FOR_SODI,
	
	.r0_ctrl_en		= 1,
	.r7_ctrl_en		= 1,

	.ca7_wfi0_en	= 1,
	.ca7_wfi1_en	= 1,
	.ca7_wfi2_en	= 1,
	.ca7_wfi3_en	= 1,
	.ca15_wfi0_en	= 1,
	.ca15_wfi1_en	= 1,
	.ca15_wfi2_en	= 1,
	.ca15_wfi3_en	= 1,

	/* SPM_AP_STANBY_CON */
	.wfi_op			= WFI_OP_AND,
	.mfg_req_mask		= 1,
	.lte_mask			= 1,
#if 0//(SODI_DVT_APxGPT)
	//.ca7top_idle_mask   = 1,
	//.ca15top_idle_mask  = 1,
	//.mcusys_idle_mask   = 1,
	//.disp_req_mask		= 1,
	.md1_req_mask		= 1,
	//.md2_req_mask		= 1,
	.conn_mask			= 1,
#endif	

    /* SPM_PCM_SRC_REQ */
#if 0 
    .pcm_apsrc_req      = 0,
    .pcm_f26m_req       = 0,
	.ccif0_to_ap_mask   = 1,
	.ccif0_to_md_mask   = 1,
	.ccif1_to_ap_mask   = 1,
	.ccif1_to_md_mask   = 1,
	.ccifmd_md1_event_mask = 1,
	.ccifmd_md2_event_mask = 1,
#endif

#if SPM_BYPASS_SYSPWREQ
	.syspwreq_mask		= 1,
#endif

#if SODI_DVT_STEP_BY_STEP
	.pcm_reserve		= 0x000001ff, //SPM DVT test step by step (will be defined by Hank)
#endif
};

struct spm_lp_scen __spm_sodi = {
	.pcmdesc	= &sodi_pcm,
	.pwrctrl	= &sodi_ctrl,
};

//0:power-down mode, 1:CG mode
static bool gSpm_SODI_mempll_pwr_mode = 1;

static bool gSpm_sodi_en=0;

extern int mt_irq_mask_all(struct mtk_irq_mask *mask);
extern int mt_irq_mask_restore(struct mtk_irq_mask *mask);
extern void mt_irq_unmask_for_sleep(unsigned int irq);

extern void soidle_before_wfi(int cpu);
extern void soidle_after_wfi(int cpu);
extern void spm_i2c_control(u32 channel, bool onoff);

#if SPM_AEE_RR_REC
extern void aee_rr_rec_sodi_val(u32 val);
extern u32 aee_rr_curr_sodi_val(void);
#endif


#if SPM_SODI_DUMP_REGS
static void spm_sodi_dump_regs(void)
{
    /* SPM register */
    printk("SPM_CA7_CPU0_IRQ_MASK   0x%p = 0x%x\n", SPM_CA7_CPU0_IRQ_MASK, spm_read(SPM_CA7_CPU0_IRQ_MASK));
    printk("SPM_CA7_CPU1_IRQ_MASK   0x%p = 0x%x\n", SPM_CA7_CPU1_IRQ_MASK, spm_read(SPM_CA7_CPU1_IRQ_MASK));    
    printk("SPM_CA7_CPU2_IRQ_MASK   0x%p = 0x%x\n", SPM_CA7_CPU2_IRQ_MASK, spm_read(SPM_CA7_CPU2_IRQ_MASK));
    printk("SPM_CA7_CPU3_IRQ_MASK   0x%p = 0x%x\n", SPM_CA7_CPU3_IRQ_MASK, spm_read(SPM_CA7_CPU3_IRQ_MASK));

#if 0    
    printk("SPM_MP1_CPU0_IRQ_MASK   0x%p = 0x%x\n", SPM_MP1_CPU0_IRQ_MASK, spm_read(SPM_MP1_CPU0_IRQ_MASK));
    printk("SPM_MP1_CPU1_IRQ_MASK   0x%p = 0x%x\n", SPM_MP1_CPU1_IRQ_MASK, spm_read(SPM_MP1_CPU1_IRQ_MASK));
    printk("SPM_MP1_CPU2_IRQ_MASK   0x%p = 0x%x\n", SPM_MP1_CPU2_IRQ_MASK, spm_read(SPM_MP1_CPU2_IRQ_MASK));
    printk("SPM_MP1_CPU3_IRQ_MASK   0x%p = 0x%x\n", SPM_MP1_CPU3_IRQ_MASK, spm_read(SPM_MP1_CPU3_IRQ_MASK));    
  
    printk("POWER_ON_VAL0           0x%p = 0x%x\n", SPM_POWER_ON_VAL0          , spm_read(SPM_POWER_ON_VAL0));
    printk("POWER_ON_VAL1           0x%p = 0x%x\n", SPM_POWER_ON_VAL1          , spm_read(SPM_POWER_ON_VAL1));
    printk("PCM_PWR_IO_EN           0x%p = 0x%x\n", SPM_PCM_PWR_IO_EN          , spm_read(SPM_PCM_PWR_IO_EN));
    printk("CLK_CON                 0x%p = 0x%x\n", SPM_CLK_CON                , spm_read(SPM_CLK_CON));
    printk("AP_DVFS_CON             0x%p = 0x%x\n", SPM_AP_DVFS_CON_SET        , spm_read(SPM_AP_DVFS_CON_SET));
    printk("PWR_STATUS              0x%p = 0x%x\n", SPM_PWR_STATUS             , spm_read(SPM_PWR_STATUS));
    printk("PWR_STATUS_S            0x%p = 0x%x\n", SPM_PWR_STATUS_S           , spm_read(SPM_PWR_STATUS_S));
    printk("SLEEP_TIMER_STA         0x%p = 0x%x\n", SPM_SLEEP_TIMER_STA        , spm_read(SPM_SLEEP_TIMER_STA));
    printk("WAKE_EVENT_MASK         0x%p = 0x%x\n", SPM_SLEEP_WAKEUP_EVENT_MASK, spm_read(SPM_SLEEP_WAKEUP_EVENT_MASK));
    printk("SPM_SLEEP_CPU_WAKEUP_EVENT 0x%p = 0x%x\n", SPM_SLEEP_CPU_WAKEUP_EVENT, spm_read(SPM_SLEEP_CPU_WAKEUP_EVENT));
    printk("SPM_PCM_RESERVE         0x%p = 0x%x\n", SPM_PCM_RESERVE          , spm_read(SPM_PCM_RESERVE));  
    printk("SPM_AP_STANBY_CON       0x%p = 0x%x\n", SPM_AP_STANBY_CON          , spm_read(SPM_AP_STANBY_CON));  
    printk("SPM_PCM_TIMER_OUT       0x%p = 0x%x\n", SPM_PCM_TIMER_OUT          , spm_read(SPM_PCM_TIMER_OUT));
    printk("SPM_PCM_CON1            0x%p = 0x%x\n", SPM_PCM_CON1          , spm_read(SPM_PCM_CON1));
#endif    
    
    // PCM register
    printk("PCM_REG0_DATA       0x%p = 0x%x\n", SPM_PCM_REG0_DATA          , spm_read(SPM_PCM_REG0_DATA));
    printk("PCM_REG1_DATA       0x%p = 0x%x\n", SPM_PCM_REG1_DATA          , spm_read(SPM_PCM_REG1_DATA));
    printk("PCM_REG2_DATA       0x%p = 0x%x\n", SPM_PCM_REG2_DATA          , spm_read(SPM_PCM_REG2_DATA));
    printk("PCM_REG3_DATA       0x%p = 0x%x\n", SPM_PCM_REG3_DATA          , spm_read(SPM_PCM_REG3_DATA));
    printk("PCM_REG4_DATA       0x%p = 0x%x\n", SPM_PCM_REG4_DATA          , spm_read(SPM_PCM_REG4_DATA));
    printk("PCM_REG5_DATA       0x%p = 0x%x\n", SPM_PCM_REG5_DATA          , spm_read(SPM_PCM_REG5_DATA));
    printk("PCM_REG6_DATA       0x%p = 0x%x\n", SPM_PCM_REG6_DATA          , spm_read(SPM_PCM_REG6_DATA));
    printk("PCM_REG7_DATA       0x%p = 0x%x\n", SPM_PCM_REG7_DATA          , spm_read(SPM_PCM_REG7_DATA));
    printk("PCM_REG8_DATA       0x%p = 0x%x\n", SPM_PCM_REG8_DATA          , spm_read(SPM_PCM_REG8_DATA));
    printk("PCM_REG9_DATA       0x%p = 0x%x\n", SPM_PCM_REG9_DATA          , spm_read(SPM_PCM_REG9_DATA));
    printk("PCM_REG10_DATA      0x%p = 0x%x\n", SPM_PCM_REG10_DATA          , spm_read(SPM_PCM_REG10_DATA));
    printk("PCM_REG11_DATA      0x%p = 0x%x\n", SPM_PCM_REG11_DATA          , spm_read(SPM_PCM_REG11_DATA));
    printk("PCM_REG12_DATA      0x%p = 0x%x\n", SPM_PCM_REG12_DATA          , spm_read(SPM_PCM_REG12_DATA));
    printk("PCM_REG13_DATA      0x%p = 0x%x\n", SPM_PCM_REG13_DATA          , spm_read(SPM_PCM_REG13_DATA));
    printk("PCM_REG14_DATA      0x%p = 0x%x\n", SPM_PCM_REG14_DATA          , spm_read(SPM_PCM_REG14_DATA));
    printk("PCM_REG15_DATA      0x%p = 0x%x\n", SPM_PCM_REG15_DATA          , spm_read(SPM_PCM_REG15_DATA));  

#if 0
    printk("SPM_MP0_FC0_PWR_CON 0x%p = 0x%x\n", SPM_MP0_FC0_PWR_CON, spm_read(SPM_MP0_FC0_PWR_CON));    
    printk("SPM_MP0_FC1_PWR_CON 0x%p = 0x%x\n", SPM_MP0_FC1_PWR_CON, spm_read(SPM_MP0_FC1_PWR_CON));    
    printk("SPM_MP0_FC2_PWR_CON 0x%p = 0x%x\n", SPM_MP0_FC2_PWR_CON, spm_read(SPM_MP0_FC2_PWR_CON));    
    printk("SPM_MP0_FC3_PWR_CON 0x%p = 0x%x\n", SPM_MP0_FC3_PWR_CON, spm_read(SPM_MP0_FC3_PWR_CON));    
    printk("SPM_MP1_FC0_PWR_CON 0x%p = 0x%x\n", SPM_MP1_FC0_PWR_CON, spm_read(SPM_MP1_FC0_PWR_CON));    
    printk("SPM_MP1_FC1_PWR_CON 0x%p = 0x%x\n", SPM_MP1_FC1_PWR_CON, spm_read(SPM_MP1_FC1_PWR_CON));    
    printk("SPM_MP1_FC2_PWR_CON 0x%p = 0x%x\n", SPM_MP1_FC2_PWR_CON, spm_read(SPM_MP1_FC2_PWR_CON));    
    printk("SPM_MP1_FC3_PWR_CON 0x%p = 0x%x\n", SPM_MP1_FC3_PWR_CON, spm_read(SPM_MP1_FC3_PWR_CON));    
#endif

    printk("CLK_CON                 0x%p = 0x%x\n", SPM_CLK_CON           , spm_read(SPM_CLK_CON));
    printk("SPM_PCM_CON0            0x%p = 0x%x\n", SPM_PCM_CON0          , spm_read(SPM_PCM_CON0));
    printk("SPM_PCM_CON1            0x%p = 0x%x\n", SPM_PCM_CON1          , spm_read(SPM_PCM_CON1));
    
    printk("SPM_PCM_EVENT_VECTOR2   0x%p = 0x%x\n", SPM_PCM_EVENT_VECTOR2  , spm_read(SPM_PCM_EVENT_VECTOR2));
    printk("SPM_PCM_EVENT_VECTOR3   0x%p = 0x%x\n", SPM_PCM_EVENT_VECTOR3  , spm_read(SPM_PCM_EVENT_VECTOR3));
    printk("SPM_PCM_EVENT_VECTOR4   0x%p = 0x%x\n", SPM_PCM_EVENT_VECTOR4  , spm_read(SPM_PCM_EVENT_VECTOR4));
    printk("SPM_PCM_EVENT_VECTOR5   0x%p = 0x%x\n", SPM_PCM_EVENT_VECTOR5  , spm_read(SPM_PCM_EVENT_VECTOR5));
    printk("SPM_PCM_EVENT_VECTOR6   0x%p = 0x%x\n", SPM_PCM_EVENT_VECTOR6  , spm_read(SPM_PCM_EVENT_VECTOR6));
    printk("SPM_PCM_EVENT_VECTOR7   0x%p = 0x%x\n", SPM_PCM_EVENT_VECTOR7  , spm_read(SPM_PCM_EVENT_VECTOR7));
    
    printk("SPM_PCM_RESERVE         0x%p = 0x%x\n", SPM_PCM_RESERVE  , spm_read(SPM_PCM_RESERVE));
    printk("SPM_PCM_WDT_TIMER_VAL   0x%p = 0x%x\n", SPM_PCM_WDT_TIMER_VAL  , spm_read(SPM_PCM_WDT_TIMER_VAL));

    printk("SPM_SLEEP_CA7_WFI0_EN   0x%p = 0x%x\n", SPM_SLEEP_CA7_WFI0_EN  , spm_read(SPM_SLEEP_CA7_WFI0_EN));
    printk("SPM_SLEEP_CA7_WFI1_EN   0x%p = 0x%x\n", SPM_SLEEP_CA7_WFI1_EN  , spm_read(SPM_SLEEP_CA7_WFI1_EN));
    printk("SPM_SLEEP_CA7_WFI2_EN   0x%p = 0x%x\n", SPM_SLEEP_CA7_WFI2_EN  , spm_read(SPM_SLEEP_CA7_WFI2_EN));
    printk("SPM_SLEEP_CA7_WFI3_EN   0x%p = 0x%x\n", SPM_SLEEP_CA7_WFI3_EN  , spm_read(SPM_SLEEP_CA7_WFI3_EN));
    printk("SPM_MP1_CORE0_WFI_SEL   0x%p = 0x%x\n", SPM_SLEEP_CA15_WFI0_EN  , spm_read(SPM_SLEEP_CA15_WFI0_EN));
    printk("SPM_MP1_CORE1_WFI_SEL   0x%p = 0x%x\n", SPM_SLEEP_CA15_WFI1_EN  , spm_read(SPM_SLEEP_CA15_WFI1_EN));
    printk("SPM_MP1_CORE2_WFI_SEL   0x%p = 0x%x\n", SPM_SLEEP_CA15_WFI2_EN  , spm_read(SPM_SLEEP_CA15_WFI2_EN));
    printk("SPM_MP1_CORE3_WFI_SEL   0x%p = 0x%x\n", SPM_SLEEP_CA15_WFI3_EN  , spm_read(SPM_SLEEP_CA15_WFI3_EN));

    printk("SPM_SLEEP_TIMER_STA     0x%p = 0x%x\n", SPM_SLEEP_TIMER_STA  , spm_read(SPM_SLEEP_TIMER_STA));
    printk("SPM_PWR_STATUS          0x%p = 0x%x\n", SPM_PWR_STATUS  , spm_read(SPM_PWR_STATUS));

    printk("SPM_PCM_FLAGS           0x%p = 0x%x\n", SPM_PCM_FLAGS  , spm_read(SPM_PCM_FLAGS));
    printk("SPM_PCM_RESERVE         0x%p = 0x%x\n", SPM_PCM_RESERVE  , spm_read(SPM_PCM_RESERVE));
    printk("SPM_PCM_RESERVE4        0x%p = 0x%x\n", SPM_PCM_RESERVE4  , spm_read(SPM_PCM_RESERVE4));
    printk("SPM_AP_STANBY_CON       0x%p = 0x%x\n", SPM_AP_STANBY_CON  , spm_read(SPM_AP_STANBY_CON));
    printk("SPM_PCM_SRC_REQ         0x%p = 0x%x\n", SPM_PCM_SRC_REQ  , spm_read(SPM_PCM_SRC_REQ));
}
#endif


static void spm_trigger_wfi_for_sodi(struct pwr_ctrl *pwrctrl)
{
    if (is_cpu_pdn(pwrctrl->pcm_flags)) {    
        //sodi_debug("enter mt_cpu_dormant(CPU_SODI_MODE)\n");
        mt_cpu_dormant(CPU_SODI_MODE);
        //sodi_debug("exit mt_cpu_dormant(CPU_SODI_MODE)\n");
    } else {
        u32 val = 0;
        
        //backup MP0_AXI_CONFIG
    	val = reg_read(MP0_AXI_CONFIG);
    	
    	//disable snoop function 
        MCUSYS_SMC_WRITE(MP0_AXI_CONFIG, val | ACINACTM);
        
        sodi_debug("enter legacy WIFI, MP0_AXI_CONFIG=0x%x\n", reg_read(MP0_AXI_CONFIG));
        
        //enter WFI
        wfi_with_sync();
        
        //restore MP0_AXI_CONFIG
        MCUSYS_SMC_WRITE(MP0_AXI_CONFIG, val);
        
        sodi_debug("exit legacy WIFI, MP0_AXI_CONFIG=0x%x\n", reg_read(MP0_AXI_CONFIG));
    }
}

static u32 vsram_vosel_on_lb;
static void spm_sodi_pre_process(void)
{
    /* set PMIC WRAP table for deepidle power control */
    mt_cpufreq_set_pmic_phase(PMIC_WRAP_PHASE_DEEPIDLE);	

    vsram_vosel_on_lb = __spm_dpidle_sodi_set_pmic_setting();
}

static void spm_sodi_post_process(void)
{
    __spm_dpidle_sodi_restore_pmic_setting(vsram_vosel_on_lb);  
    
    /* set PMIC WRAP table for normal power control */
    mt_cpufreq_set_pmic_phase(PMIC_WRAP_PHASE_NORMAL);
}

void spm_go_to_sodi(u32 spm_flags, u32 spm_data)
{
    struct wake_status wakesta;
    unsigned long flags;
    struct mtk_irq_mask mask;
    wake_reason_t wr = WR_NONE;
    struct pcm_desc *pcmdesc = __spm_sodi.pcmdesc;
    struct pwr_ctrl *pwrctrl = __spm_sodi.pwrctrl;

#if SPM_AEE_RR_REC
    aee_rr_rec_sodi_val(1<<SPM_SODI_ENTER);
#endif 
    
#if defined (CONFIG_ARM_PSCI)||defined(CONFIG_MTK_PSCI)
    spm_flags &= ~SPM_DISABLE_ATF_ABORT;
#else
    spm_flags |= SPM_DISABLE_ATF_ABORT;
#endif

	if(gSpm_SODI_mempll_pwr_mode == 1)
	{
		spm_flags |= SPM_MEMPLL_CG_EN; //MEMPLL CG mode
	}
	else
	{
		spm_flags &= ~SPM_MEMPLL_CG_EN; //DDRPHY power down mode
	}

    set_pwrctrl_pcm_flags(pwrctrl, spm_flags);
    
    //If Vcore DVFS is disable, force to disable SODI internal Vcore DVS
    if (pwrctrl->pcm_flags_cust == 0)
    {
        if ((pwrctrl->pcm_flags & SPM_VCORE_DVFS_EN) == 0) 
        {
            pwrctrl->pcm_flags |= SPM_VCORE_DVS_EVENT_DIS;
        }
    }
    
    //SODI will not decrease Vcore voltage in HPM mode.
    if ((pwrctrl->pcm_flags & SPM_VCORE_DVS_EVENT_DIS) == 0)
    {
        if (get_ddr_khz() != FDDR_S1_KHZ)
        {
#if SPM_AEE_RR_REC
            aee_rr_rec_sodi_val(aee_rr_curr_sodi_val()|(1<<SPM_SODI_VCORE_HPM));
#endif 
            //printk("SODI: get_ddr_khz() = %d\n", get_ddr_khz());
        }
        else
        {
#if SPM_AEE_RR_REC
            aee_rr_rec_sodi_val(aee_rr_curr_sodi_val()|(1<<SPM_SODI_VCORE_LPM));
#endif            
        }
    }
    
    //enable APxGPT timer
	soidle_before_wfi(0);
	
	lockdep_off();
    spin_lock_irqsave(&__spm_lock, flags);

    mt_irq_mask_all(&mask);
    mt_irq_unmask_for_sleep(SPM_IRQ0_ID);
    mt_cirq_clone_gic();
    mt_cirq_enable();

#if SPM_AEE_RR_REC
    aee_rr_rec_sodi_val(aee_rr_curr_sodi_val()|(1<<SPM_SODI_ENTER_SPM_FLOW));
#endif   

    __spm_reset_and_init_pcm(pcmdesc);

	/*
	 * When commond-queue is in shut-down mode, SPM will hang if it tries to access commond-queue status.  
     * Follwoing patch is to let SODI driver to notify SPM that commond-queue is in shut-down mode or not to avoid above SPM hang issue. 
     * But, now display can automatically notify SPM that command-queue is shut-down or not, so following code is not needed anymore.
	 */
	#if 0 
    //check GCE
	if(clock_is_on(MT_CG_INFRA_GCE))
	{
		pwrctrl->pcm_flags &= ~SPM_DDR_HIGH_SPEED; 
	}
	else
	{
		pwrctrl->pcm_flags |= SPM_DDR_HIGH_SPEED; 
	}
	#endif

    __spm_kick_im_to_fetch(pcmdesc);
    
    __spm_init_pcm_register();
    
    __spm_init_event_vector(pcmdesc);
    
    //Display set SPM_PCM_SRC_REQ[0]=1'b1 to force DRAM not enter self-refresh mode
   	if((spm_read(SPM_PCM_SRC_REQ)&0x00000001))
    {
    	pwrctrl->pcm_apsrc_req = 1;
    }
    else
    {
    	pwrctrl->pcm_apsrc_req = 0;
    }

    __spm_set_power_control(pwrctrl);
    
    __spm_set_wakeup_event(pwrctrl);
    
#if SODI_DVT_PCM_TIMER_DISABLE
	//PCM_Timer is enable in above '__spm_set_wakeup_event(pwrctrl);', disable PCM Timer here
	spm_write(SPM_PCM_CON1 ,spm_read(SPM_PCM_CON1)&(~CON1_PCM_TIMER_EN));
#endif

    spm_sodi_pre_process();
    
    __spm_kick_pcm_to_run(pwrctrl);

#if SPM_SODI_DUMP_REGS
    printk("============SODI Before============\n");
    spm_sodi_dump_regs(); //dump debug info
#endif
      
#if SPM_AEE_RR_REC
    aee_rr_rec_sodi_val(aee_rr_curr_sodi_val()|(1<<SPM_SODI_ENTER_WFI));
#endif

    spm_trigger_wfi_for_sodi(pwrctrl);

#if SPM_AEE_RR_REC
    aee_rr_rec_sodi_val(aee_rr_curr_sodi_val()|(1<<SPM_SODI_LEAVE_WFI));
#endif  

#if SPM_SODI_DUMP_REGS
    printk("============SODI After=============\n");
    spm_sodi_dump_regs();//dump debug info
#endif

    spm_sodi_post_process();
    
    __spm_get_wakeup_status(&wakesta);
    
    sodi_debug("emi-selfrefrsh cnt = %d, pcm_flag = 0x%x, SPM_PCM_RESERVE2 = 0x%x, %s\n", spm_read(SPM_PCM_PASR_DPD_3), spm_read(SPM_PCM_FLAGS), spm_read(SPM_PCM_RESERVE2), pcmdesc->version);
    
    __spm_clean_after_wakeup();
    
    wr = __spm_output_wake_reason(&wakesta, pcmdesc, false);
    if (wr == WR_PCM_ASSERT)
    {
        sodi_err("PCM ASSERT AT %u (%s), r13 = 0x%x, debug_flag = 0x%x\n", wakesta.assert_pc, pcmdesc->version, wakesta.r13, wakesta.debug_flag);   
    }
    
#if SPM_AEE_RR_REC
    aee_rr_rec_sodi_val(aee_rr_curr_sodi_val()|(1<<SPM_SODI_LEAVE_SPM_FLOW));
#endif  

    mt_cirq_flush();
    mt_cirq_disable();
    mt_irq_mask_restore(&mask);
	
    spin_unlock_irqrestore(&__spm_lock, flags);
    lockdep_on();

    //stop APxGPT timer and enable caore0 local timer
    soidle_after_wfi(0);  

#if SODI_DVT_SPM_MEM_RW_TEST
    {	
        static u32 magic_init = 0;
        int i =0;

        if(magic_init == 0){
		    magic_init++;
		    printk("magicNumArray:0x%p",magicArray);
	    }

    	for(i=0;i<16;i++)
    	{
    		if(magicArray[i]!=SODI_DVT_MAGIC_NUM)
    		{
    			printk("Error: sodi magic number no match!!!");
    			ASSERT(0);
    		}
    	}
    	
    	if (i>=16)
    	    printk("SODI_DVT_SPM_MEM_RW_TEST pass (count = %d)\n", magic_init);
    }
#endif

#if SPM_AEE_RR_REC
    aee_rr_rec_sodi_val(0);
#endif 
}

void spm_sodi_mempll_pwr_mode(bool pwr_mode)
{
	//printk("[SODI]set pwr_mode = %d\n",pwr_mode);
    gSpm_SODI_mempll_pwr_mode = pwr_mode;
}

void spm_enable_sodi(bool en)
{
    gSpm_sodi_en=en;
}

bool spm_get_sodi_en(void)
{
    return gSpm_sodi_en;
}

#if SPM_AEE_RR_REC
static void spm_sodi_aee_init(void)
{
    aee_rr_rec_sodi_val(0);
}
#endif

#if SPM_USE_TWAM_DEBUG
#define SPM_TWAM_MONITOR_TICK 333333
static void twam_handler(struct twam_sig *twamsig)
{
	spm_crit("sig_high = %u%%  %u%%  %u%%  %u%%, r13 = 0x%x\n",
		 get_percent(twamsig->sig0,SPM_TWAM_MONITOR_TICK),
		 get_percent(twamsig->sig1,SPM_TWAM_MONITOR_TICK),
		 get_percent(twamsig->sig2,SPM_TWAM_MONITOR_TICK),
		 get_percent(twamsig->sig3,SPM_TWAM_MONITOR_TICK),
		 spm_read(SPM_PCM_REG13_DATA));
}
#endif

void spm_sodi_init(void)
{
#if SPM_USE_TWAM_DEBUG
	unsigned long flags;
	struct twam_sig twamsig = {
		.sig0 = 10,	/* disp_req */
		.sig1 = 23,	/* self-refresh */
		.sig2 = 25,	/* md2_srcclkena */
		.sig3 = 21,	/* md2_apsrc_req_mux */		
	};
#endif
	
#if defined (CONFIG_OF)
    struct device_node *node;
    struct resource r;

    /* mcucfg */
    node = of_find_compatible_node(NULL, NULL, MCUCFG_NODE);
    if (!node) 
    {
        spm_err("error: cannot find node " MCUCFG_NODE); 
        BUG();
    }
    if (of_address_to_resource(node, 0, &r)) {
        spm_err("error: cannot get phys addr" MCUCFG_NODE);
        BUG();
    }
    mcucfg_phys_base = r.start;

    mcucfg_base = (unsigned long)of_iomap(node, 0);
    if(!mcucfg_base) {
        spm_err("error: cannot iomap " MCUCFG_NODE);
        BUG();
    }
    
    printk("mcucfg_base = 0x%u\n", (unsigned int)mcucfg_base); 
#endif

#if SPM_AEE_RR_REC
    spm_sodi_aee_init();
#endif

#if SPM_USE_TWAM_DEBUG	
    #if 0
	spin_lock_irqsave(&__spm_lock, flags);
	spm_write(SPM_AP_STANBY_CON, spm_read(SPM_AP_STANBY_CON) | ASC_MD_DDR_EN_SEL);
	spin_unlock_irqrestore(&__spm_lock, flags);
    #endif
    
	spm_twam_register_handler(twam_handler);
	spm_twam_enable_monitor(&twamsig, false,SPM_TWAM_MONITOR_TICK);
#endif
}

MODULE_DESCRIPTION("SPM-SODI Driver v0.1");
