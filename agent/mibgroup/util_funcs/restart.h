#ifndef _MIBGROUP_UTIL_FUNCS_RESTART_H
#define _MIBGROUP_UTIL_FUNCS_RESTART_H

#ifdef __cplusplus
extern "C" {
#endif

extern char **argvrestartp, *argvrestartname, *argvrestart;

WriteMethod     restart_hook;

#ifdef __cplusplus
}
#endif

#endif /* _MIBGROUP_UTIL_FUNCS_RESTART_H */
