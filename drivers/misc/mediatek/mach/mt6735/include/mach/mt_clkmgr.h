#ifndef _MT_CLKMGR_H
#define _MT_CLKMGR_H

#if defined(CONFIG_ARCH_MT6735M)
#include "mach/mt_clkmgr2.h"
#elif defined(CONFIG_ARCH_MT6753)
#include "mach/mt_clkmgr3.h"
#else
#include "mach/mt_clkmgr1.h"
#endif

#endif
