#include <config.h>

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <ctype.h>
#include <signal.h>
#if HAVE_MACHINE_PARAM_H
#include <machine/param.h>
#endif
#if HAVE_SYS_VMMETER_H
#if !defined(bsdi2) && !defined(netbsd1)
#include <sys/vmmeter.h>
#endif
#endif
#if HAVE_SYS_CONF_H
#include <sys/conf.h>
#endif
#include <sys/param.h>
#if HAVE_SYS_FS_H
#include <sys/fs.h>
#else
#if HAVE_UFS_FS_H
#include <ufs/fs.h>
#else
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_VNODE_H
#include <sys/vnode.h>
#endif
#ifdef HAVE_UFS_UFS_QUOTA_H
#include <ufs/ufs/quota.h>
#endif
#ifdef HAVE_UFS_UFS_INODE_H
#include <ufs/ufs/inode.h>
#endif
#if HAVE_UFS_FFS_FS_H
#include <ufs/ffs/fs.h>
#endif
#endif
#endif
#if HAVE_MTAB_H
#include <mtab.h>
#endif
#include <sys/stat.h>
#include <errno.h>
#if HAVE_FSTAB_H
#include <fstab.h>
#endif
#if HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif
#if HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif
#if (!defined(HAVE_STATVFS)) && defined(HAVE_STATFS)
#if HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#if HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif
#if HAVE_SYS_SYSCTL_H
#include <sys/sysctl.h>
#endif
#define statvfs statfs
#endif
#if HAVE_VM_SWAP_PAGER_H
#include <vm/swap_pager.h>
#endif
#if HAVE_SYS_FIXPOINT_H
#include <sys/fixpoint.h>
#endif
#if HAVE_MALLOC_H
#include <malloc.h>
#endif
#if STDC_HEADERS
#include <string.h>
#endif

#include "mibincl.h"
#include "mibdefs.h"
#include "struct.h"
#include "util_funcs.h"
#include "vmstat.h"
#include "auto_nlist.h"


#define VMSTAT_FILE "/proc/stat"
#define BUFFSIZE 1024
static char buff[BUFFSIZE];

void getstat(unsigned *, unsigned *, unsigned *, unsigned long *,
             unsigned *, unsigned *, unsigned *, unsigned *,
             unsigned *, unsigned *, unsigned *);

void getstat(unsigned *cuse, unsigned *cice, unsigned *csys,unsigned long *cide,
             unsigned *pin, unsigned *pout, unsigned *sin, unsigned *sout,
             unsigned *itot, unsigned *i1, unsigned *ct) {
  static int stat;

  if ((stat=open(VMSTAT_FILE, O_RDONLY, 0)) != -1) {
    char* b;
    buff[BUFFSIZE-1] = 0;  /* ensure null termination in buffer */
    read(stat,buff,BUFFSIZE-1);
    close(stat);
    *itot = 0; 
    *i1 = 1;   /* ensure assert below will fail if the sscanf bombs */
    b = strstr(buff, "cpu ");
    sscanf(b, "cpu  %u %u %u %lu", cuse, cice, csys, cide);
    b = strstr(buff, "page ");
    sscanf(b, "page %u %u", pin, pout);
    b = strstr(buff, "swap ");
    sscanf(b, "swap %u %u", sin, sout);
    b = strstr(buff, "intr ");
    sscanf(b, "intr %u %u", itot, i1);
    b = strstr(buff, "ctxt ");
    sscanf(b, "ctxt %u", ct);
  }
  else {
    perror("/proc/stat");
  }
}

enum vmstat_index { swapin = 0,    swapout, 
		    iosent,        ioreceive, 
		    sysinterrupts, syscontext,
		    cpuuser,       cpusystem, cpuidle };

unsigned vmstat (int index) {
  unsigned int cpu_use[2], cpu_nic[2], cpu_sys[2];
  unsigned int duse,dsys,didl,div,divo2;
  unsigned long cpu_idl[2];
  unsigned int pgpgin[2], pgpgout[2], pswpin[2], pswpout[2];
  unsigned int inter[2],ticks[2],ctxt[2];
  unsigned int hz;

  getstat(cpu_use,cpu_nic,cpu_sys,cpu_idl,
	  pgpgin,pgpgout,pswpin,pswpout,
          inter,ticks,ctxt);
  duse= *(cpu_use)+ *(cpu_nic);
  dsys= *(cpu_sys);
  didl= (*(cpu_idl))%UINT_MAX;
  div= (duse+dsys+didl);
  hz=sysconf(_SC_CLK_TCK); /* get ticks/s from system */
  divo2= div/2;

  if (index == swapin) {
    return (*(pswpin)*4*hz+divo2)/div;
  } else if (index == swapout) {
    return (*(pswpout)*4*hz+divo2)/div;
  } else if (index == iosent) {
    return (*(pgpgin)*hz+divo2)/div;
  } else if (index == ioreceive) {
    return (*(pgpgout)*hz+divo2)/div;
  } else if (index == sysinterrupts) {
    return (*(inter)*hz+divo2)/div;
  } else if (index == syscontext) {
    return (*(ctxt)*hz+divo2)/div;
  } else if (index == cpuuser) {
    return (100*duse+divo2)/div;
  } else if (index == cpusystem) {
    return (100*dsys+divo2)/div;
  } else if (index == cpuidle) {
    return (100*didl+divo2)/div;
  } else {
    return -1;
  }
}

unsigned char *var_extensible_vmstat(vp, name, length, exact, var_len, write_method)
    register struct variable *vp;
/* IN - pointer to variable entry that points here */
    register oid	*name;
/* IN/OUT - input name requested, output name found */
    register int	*length;
/* IN/OUT - length of input and output oid's */
    int			exact;
/* IN - TRUE if an exact match was requested. */
    int			*var_len;
/* OUT - length of variable or 0 if function returned. */
    int			(**write_method)__P((int, u_char *, u_char, int, u_char *, oid *, int));
/* OUT - pointer to function to set variable, otherwise 0 */
{

  oid newname[30];
  int result;
  static long long_ret;
  static char errmsg[300];
#ifndef linux
  struct vmtotal total;
#endif

  long_ret = 0;  /* set to 0 as default */

  if (!checkmib(vp,name,length,exact,var_len,write_method,newname,1))
    return(NULL);
  switch (vp->magic) {
    case MIBINDEX:
      long_ret = 1;
      return((u_char *) (&long_ret));
    case ERRORNAME:    /* dummy name */
      sprintf(errmsg,"vmstat");
      *var_len = strlen(errmsg);
      return((u_char *) (errmsg));
    case SWAPIN:
#ifdef linux
      long_ret = vmstat(swapin);
#endif
      return((u_char *) (&long_ret));
    case SWAPOUT:
#ifdef linux
      long_ret = vmstat(swapout);
#endif
      return((u_char *) (&long_ret));
    case IOSENT:
#ifdef linux
      long_ret = vmstat(iosent);;
#endif
      return((u_char *) (&long_ret));
    case IORECEIVE:
#ifdef linux
	long_ret = vmstat(ioreceive);
#endif
      return((u_char *) (&long_ret));
    case SYSINTERRUPTS:
#ifdef linux
	long_ret = vmstat(sysinterrupts);
#endif
      return((u_char *) (&long_ret));
    case SYSCONTEXT:
#ifdef linux
      long_ret = vmstat(syscontext);
#endif
      return((u_char *) (&long_ret));
    case CPUUSER:
#ifdef linux
      long_ret = vmstat(cpuuser);
#endif
      return((u_char *) (&long_ret));
    case CPUSYSTEM:
#ifdef linux
      long_ret = vmstat(cpusystem);
#endif
      return((u_char *) (&long_ret));
    case CPUIDLE:
#ifdef linux
      long_ret = vmstat(cpuidle);
#endif
      return((u_char *) (&long_ret));
    case ERRORFLAG:
      return((u_char *) (&long_ret));
    case ERRORMSG:
      return((u_char *) (&long_ret));
  }
  return NULL;
}

