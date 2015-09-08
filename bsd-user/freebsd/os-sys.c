/*
 *  FreeBSD sysctl() and sysarch() system call emulation
 *
 *  Copyright (c) 2013-15 Stacey D. Son
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/user.h>   /* For struct kinfo_* */

/* Undef all the macros from sys/user.h that conflict with qemu. */
#undef CPUID_ACPI
#undef CPUID_APIC
#undef CPUID_CMOV
#undef CPUID_CX8
#undef CPUID_DE
#undef CPUID_FXSR
#undef CPUID_IA64
#undef CPUID_PAE
#undef CPUID_PAT
#undef CPUID_PGE
#undef CPUID_PSE
#undef CPUID_MCA
#undef CPUID_MCE
#undef CPUID_MMX
#undef CPUID_MSR
#undef CPUID_MTRR
#undef CPUID_PBE
#undef CPUID_PSE36
#undef CPUID_SS
#undef CPUID_SEP
#undef CPUID_SSE
#undef CPUID_SSE2
#undef CPUID_TM
#undef CPUID_TSC
#undef CPUID_VME
#undef MCG_STATUS_EIPV
#undef MCG_STATUS_MCIP
#undef MCG_STATUS_RIPV
#undef MSR_IA32_FEATURE_CONTROL
#undef MSR_IA32_XSS
#undef MSR_MTRRcap

#include <string.h>

#include "qemu.h"
#include "qemu-bsd.h"

#include "target_arch_sysarch.h"
#include "target_os_vmparam.h"
#include "target_os_user.h"

static void
host_to_target_kinfo_proc(struct target_kinfo_proc *tki, struct kinfo_proc *hki)
{
    int i;

    __put_user(sizeof(struct target_kinfo_proc), &tki->ki_structsize);
    __put_user(hki->ki_layout, &tki->ki_layout);

    /* Some of these are used as flags (e.g. ki_fd == NULL in procstat). */
    tki->ki_args = tswapal((abi_ulong)(uintptr_t)hki->ki_args);
    tki->ki_paddr = tswapal((abi_ulong)(uintptr_t)hki->ki_paddr);
    tki->ki_addr = tswapal((abi_ulong)(uintptr_t)hki->ki_addr);
    tki->ki_tracep = tswapal((abi_ulong)(uintptr_t)hki->ki_tracep);
    tki->ki_textvp = tswapal((abi_ulong)(uintptr_t)hki->ki_textvp);
    tki->ki_fd = tswapal((abi_ulong)(uintptr_t)hki->ki_fd);
    tki->ki_vmspace = tswapal((abi_ulong)(uintptr_t)hki->ki_vmspace);
    tki->ki_wchan = tswapal((abi_ulong)(uintptr_t)hki->ki_wchan);

    __put_user(hki->ki_pid, &tki->ki_pid);
    __put_user(hki->ki_ppid, &tki->ki_ppid);
    __put_user(hki->ki_pgid, &tki->ki_pgid);
    __put_user(hki->ki_tpgid, &tki->ki_tpgid);
    __put_user(hki->ki_sid, &tki->ki_sid);
    __put_user(hki->ki_tsid, &tki->ki_tsid);
    __put_user(hki->ki_jobc, &tki->ki_jobc);
    __put_user(hki->ki_tdev, &tki->ki_tdev);

    host_to_target_sigset(&tki->ki_siglist, &hki->ki_siglist);
    host_to_target_sigset(&tki->ki_sigmask, &hki->ki_sigmask);
    host_to_target_sigset(&tki->ki_sigignore, &hki->ki_sigignore);
    host_to_target_sigset(&tki->ki_sigcatch, &hki->ki_sigcatch);

    __put_user(hki->ki_uid, &tki->ki_uid);
    __put_user(hki->ki_ruid, &tki->ki_ruid);
    __put_user(hki->ki_svuid, &tki->ki_svuid);
    __put_user(hki->ki_rgid, &tki->ki_rgid);
    __put_user(hki->ki_svgid, &tki->ki_svgid);
    __put_user(hki->ki_ngroups, &tki->ki_ngroups);

    for (i=0; i < TARGET_KI_NGROUPS; i++)
        __put_user(hki->ki_groups[i], &tki->ki_groups[i]);

    __put_user(hki->ki_size, &tki->ki_size);

    __put_user(hki->ki_rssize, &tki->ki_rssize);
    __put_user(hki->ki_swrss, &tki->ki_swrss);
    __put_user(hki->ki_tsize, &tki->ki_tsize);
    __put_user(hki->ki_dsize, &tki->ki_dsize);
    __put_user(hki->ki_ssize, &tki->ki_ssize);

    __put_user(hki->ki_xstat, &tki->ki_xstat);
    __put_user(hki->ki_acflag, &tki->ki_acflag);

    __put_user(hki->ki_pctcpu, &tki->ki_pctcpu);

    __put_user(hki->ki_estcpu, &tki->ki_estcpu);
    __put_user(hki->ki_slptime, &tki->ki_slptime);
    __put_user(hki->ki_swtime, &tki->ki_swtime);
    __put_user(hki->ki_cow, &tki->ki_cow);
    __put_user(hki->ki_runtime, &tki->ki_runtime);

    __put_user(hki->ki_start.tv_sec, &tki->ki_start.tv_sec);
    __put_user(hki->ki_start.tv_usec, &tki->ki_start.tv_usec);
    __put_user(hki->ki_childtime.tv_sec, &tki->ki_childtime.tv_sec);
    __put_user(hki->ki_childtime.tv_usec, &tki->ki_childtime.tv_usec);

    __put_user(hki->ki_flag, &tki->ki_flag);
    __put_user(hki->ki_kiflag, &tki->ki_kiflag);

    __put_user(hki->ki_traceflag, &tki->ki_traceflag);
    __put_user(hki->ki_stat, &tki->ki_stat);
    __put_user(hki->ki_nice, &tki->ki_nice);
    __put_user(hki->ki_lock, &tki->ki_lock);
    __put_user(hki->ki_rqindex, &tki->ki_rqindex);
#if defined(__FreeBSD_version) && __FreeBSD_version >= 1100000
    __put_user(hki->ki_oncpu_old, &tki->ki_oncpu_old);
    __put_user(hki->ki_lastcpu_old, &tki->ki_lastcpu_old);
#else
    __put_user(hki->ki_oncpu, &tki->ki_oncpu);
    __put_user(hki->ki_lastcpu, &tki->ki_lastcpu);
#endif /* ! __FreeBSD_version >= 1100000 */

#if defined(__FreeBSD_version) && __FreeBSD_version >= 900000
    strncpy(tki->ki_tdname, hki->ki_tdname, TARGET_TDNAMLEN+1);
#else
    strncpy(tki->ki_ocomm, hki->ki_ocomm, TARGET_TDNAMLEN+1);
#endif /* __FreeBSD_version >= 900000 */
    strncpy(tki->ki_wmesg, hki->ki_wmesg, TARGET_WMESGLEN+1);
    strncpy(tki->ki_login, hki->ki_login, TARGET_LOGNAMELEN+1);
    strncpy(tki->ki_lockname, hki->ki_lockname, TARGET_LOCKNAMELEN+1);
    strncpy(tki->ki_comm, hki->ki_comm, TARGET_COMMLEN+1);
    strncpy(tki->ki_emul, hki->ki_emul, TARGET_KI_EMULNAMELEN+1);
#if defined(__FreeBSD_version) && __FreeBSD_version >= 900000
    strncpy(tki->ki_loginclass, hki->ki_loginclass, TARGET_LOGINCLASSLEN+1);
#endif /* __FreeBSD_version >= 900000 */

#if defined(__FreeBSD_version) && __FreeBSD_version >= 1100000
    __put_user(hki->ki_oncpu, &tki->ki_oncpu);
    __put_user(hki->ki_lastcpu, &tki->ki_lastcpu);
    __put_user(hki->ki_tracer, &tki->ki_tracer);
#endif /* __FreeBSD_version >= 1100000 */
#if defined(__FreeBSD_version) && __FreeBSD_version >= 900000
    __put_user(hki->ki_flag2, &tki->ki_flag2);
    __put_user(hki->ki_fibnum, &tki->ki_fibnum);
#endif /* __FreeBSD_version >= 900000 */
    __put_user(hki->ki_cr_flags, &tki->ki_cr_flags);
    __put_user(hki->ki_jid, &tki->ki_jid);
    __put_user(hki->ki_numthreads, &tki->ki_numthreads);
    __put_user(hki->ki_tid, &tki->ki_tid);

    memcpy(&tki->ki_pri, &hki->ki_pri, sizeof(struct target_priority));

    h2t_rusage(&hki->ki_rusage, &tki->ki_rusage);
    h2t_rusage(&hki->ki_rusage_ch, &tki->ki_rusage_ch);

    __put_user(((uintptr_t)hki->ki_pcb), &tki->ki_pcb);
    __put_user(((uintptr_t)hki->ki_kstack), &tki->ki_kstack);
    __put_user(((uintptr_t)hki->ki_udata), &tki->ki_udata);
    __put_user(((uintptr_t)hki->ki_tdaddr), &tki->ki_tdaddr);

    __put_user(hki->ki_sflag, &tki->ki_sflag);
    __put_user(hki->ki_tdflags, &tki->ki_tdflags);
}

abi_long
do_sysctl_kern_getprocs(int op, int arg, size_t olen,
        struct target_kinfo_proc *tki, size_t *tlen)
{
    abi_long ret;
    struct kinfo_proc *kipp;
    int mib[4], num, i, miblen;
    size_t len;

    if (tlen == NULL)
        return -TARGET_EINVAL;

    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = op;
    mib[3] = arg;

    miblen = (op == KERN_PROC_ALL || op == KERN_PROC_PROC) ?  3 : 4;

    len = 0;
    ret = get_errno(sysctl(mib, miblen, NULL, &len, NULL, 0));
    if (is_error(ret))
        return ret;

    num = len / sizeof(*kipp);
    *tlen = num * sizeof(struct target_kinfo_proc);
    if (tki == NULL)
        return ret;

    if (olen < *tlen)
        return -TARGET_EINVAL;

    kipp = g_malloc(len);
    if (kipp == NULL)
        return -TARGET_ENOMEM;
    ret = get_errno(sysctl(mib, miblen, kipp, &len, NULL, 0));
    num = len / sizeof(*kipp);
    *tlen = num * sizeof(struct target_kinfo_proc);
    if (len % sizeof(*kipp) != 0 || kipp->ki_structsize != sizeof(*kipp)) {
        ret = -TARGET_EINVAL; /* XXX */
    } else if (!is_error(ret)) {
        for(i=0; i < num; i++)
            host_to_target_kinfo_proc(&tki[i], &kipp[i]);
    }

    g_free(kipp);
    return ret;
}


static void
host_to_target_kinfo_file(struct target_kinfo_file *tkif,
        struct kinfo_file *hkif)
{
#if defined(__FreeBSD_version) && __FreeBSD_version >= 900000
    int type = hkif->kf_type;
#endif /* __FreeBSD_version >= 900000 */

    __put_user(hkif->kf_structsize, &tkif->kf_structsize);
    __put_user(hkif->kf_type, &tkif->kf_type);
    __put_user(hkif->kf_fd, &tkif->kf_fd);
    __put_user(hkif->kf_ref_count, &tkif->kf_ref_count);
    __put_user(hkif->kf_flags, &tkif->kf_flags);
    __put_user(hkif->kf_offset, &tkif->kf_offset);
    __put_user(hkif->kf_vnode_type, &tkif->kf_vnode_type);
    __put_user(hkif->kf_sock_domain, &tkif->kf_sock_domain);
    __put_user(hkif->kf_sock_type, &tkif->kf_sock_type);
    __put_user(hkif->kf_sock_protocol, &tkif->kf_sock_protocol);

#if defined(__FreeBSD_version) && __FreeBSD_version >= 900000
    switch(type) {
    case KF_TYPE_FIFO:
    case KF_TYPE_SHM:
    case KF_TYPE_VNODE:
        __put_user(hkif->kf_un.kf_file.kf_file_fileid,
                &tkif->kf_un.kf_file.kf_file_fileid);
        __put_user(hkif->kf_un.kf_file.kf_file_size,
                &tkif->kf_un.kf_file.kf_file_size);
        __put_user(hkif->kf_un.kf_file.kf_file_fsid,
                &tkif->kf_un.kf_file.kf_file_fsid);
        __put_user(hkif->kf_un.kf_file.kf_file_rdev,
                &tkif->kf_un.kf_file.kf_file_rdev);
        __put_user(hkif->kf_un.kf_file.kf_file_mode,
                &tkif->kf_un.kf_file.kf_file_mode);
        break;

    case KF_TYPE_SOCKET:
        __put_user(hkif->kf_un.kf_sock.kf_sock_pcb,
                &tkif->kf_un.kf_sock.kf_sock_pcb);
        __put_user(hkif->kf_un.kf_sock.kf_sock_inpcb,
                &tkif->kf_un.kf_sock.kf_sock_inpcb);
        __put_user(hkif->kf_un.kf_sock.kf_sock_unpconn,
                &tkif->kf_un.kf_sock.kf_sock_unpconn);
        __put_user(hkif->kf_un.kf_sock.kf_sock_snd_sb_state,
                &tkif->kf_un.kf_sock.kf_sock_snd_sb_state);
        __put_user(hkif->kf_un.kf_sock.kf_sock_rcv_sb_state,
                &tkif->kf_un.kf_sock.kf_sock_rcv_sb_state);
        break;

    case KF_TYPE_PIPE:
        __put_user(hkif->kf_un.kf_pipe.kf_pipe_addr,
                &tkif->kf_un.kf_pipe.kf_pipe_addr);
        __put_user(hkif->kf_un.kf_pipe.kf_pipe_peer,
                &tkif->kf_un.kf_pipe.kf_pipe_peer);
        __put_user(hkif->kf_un.kf_pipe.kf_pipe_buffer_cnt,
                &tkif->kf_un.kf_pipe.kf_pipe_buffer_cnt);
        break;

    case KF_TYPE_SEM:
        __put_user(hkif->kf_un.kf_sem.kf_sem_value,
                &tkif->kf_un.kf_sem.kf_sem_value);
        __put_user(hkif->kf_un.kf_sem.kf_sem_mode,
                &tkif->kf_un.kf_sem.kf_sem_mode);
        break;

    case KF_TYPE_PTS:
        __put_user(hkif->kf_un.kf_pts.kf_pts_dev,
                &tkif->kf_un.kf_pts.kf_pts_dev);
        break;

    case KF_TYPE_PROCDESC:
        __put_user(hkif->kf_un.kf_proc.kf_pid,
                &tkif->kf_un.kf_proc.kf_pid);
        break;


    case KF_TYPE_CRYPTO:
    case KF_TYPE_KQUEUE:
    case KF_TYPE_MQUEUE:
    case KF_TYPE_NONE:
    case KF_TYPE_UNKNOWN:
    default:
        /* Do nothing. */
        break;
    }
    __put_user(hkif->kf_status, &tkif->kf_status);
#if defined(__FreeBSD_version) && __FreeBSD_version >= 1000000
    for (int i = 0; i < (CAP_RIGHTS_VERSION + 2); i++)
        __put_user(hkif->kf_cap_rights.cr_rights[i],
                &tkif->kf_cap_rights.cr_rights[i]);
#else /* !  __FreeBSD_version >= 1000000 */
    __put_user(hkif->kf_cap_rights, &tkif->kf_cap_rights);
#endif /* *  __FreeBSD_version >= 1000000 */
#endif /* ! __FreeBSD_version >= 900000 */
    strncpy(tkif->kf_path, hkif->kf_path, PATH_MAX);
}

abi_long
do_sysctl_kern_proc_filedesc(int pid, size_t olen,
        struct target_kinfo_file *tkif, size_t *tlen)
{
    abi_long ret;
    int mib[4], cnt, sz;
    size_t len;
    char *buf, *bp, *eb, *tp;
    struct kinfo_file *kf, kif;
    struct target_kinfo_file target_kif;

    if (tlen == NULL)
        return -TARGET_EINVAL;

    len = 0;
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_FILEDESC;
    mib[3] = pid;

    ret = get_errno(sysctl(mib, 4, NULL, &len, NULL, 0));
    if (is_error(ret))
        return ret;
    if (tkif == NULL) {
        *tlen = len;
        return ret;
    }
    len = len * 4 / 3;
    buf = g_malloc(len);
    if (buf == NULL)
        return -TARGET_ENOMEM;

    /*
     * Count the number of records.
     *
     * Given that the kinfo_file information returned by
     * the kernel may be different sizes per record we have
     * to read it in and count the variable length records
     * by walking them.
     */
    ret = get_errno(sysctl(mib, 4, buf, &len, NULL, 0));
    if (is_error(ret)) {
        g_free(buf);
        return ret;
    }
    *tlen = len;
    cnt = 0;
    bp = buf;
    eb = buf + len;
    while (bp < eb) {
        kf = (struct kinfo_file *)(uintptr_t)bp;
        bp += kf->kf_structsize;
        cnt++;
    }
    if (olen < *tlen) {
        g_free(buf);
        return -TARGET_EINVAL;
    }

    /*
     * Unpack the records from the kernel into full length records
     * and byte swap, if needed.
     */
    bp = buf;
    eb = buf + len;
    tp = (char *)tkif;
    while (bp < eb) {
        kf = (struct kinfo_file *)(uintptr_t)bp;
        sz = kf->kf_structsize;
        /* Copy/expand into a zeroed buffer */
        memset(&kif, 0, sizeof (kif));
        memcpy(&kif, kf, sz);
        /* Byte swap and copy into a target buffer. */
        host_to_target_kinfo_file(&target_kif, &kif);
        /* Copy target buffer to user buffer and pack */
        memcpy(tp, &target_kif, sz);
        /* Advance to next packed record. */
        bp += sz;
        /* Advance to next packed, target record. */
        tp += sz;
    }

    g_free(buf);
    return ret;
}

static void
host_to_target_kinfo_vmentry(struct target_kinfo_vmentry *tkve,
        struct kinfo_vmentry *hkve)
{

    __put_user(hkve->kve_structsize, &tkve->kve_structsize);
    __put_user(hkve->kve_type, &tkve->kve_type);
    __put_user(hkve->kve_start, &tkve->kve_start);
    __put_user(hkve->kve_end, &tkve->kve_end);
    __put_user(hkve->kve_offset, &tkve->kve_offset);
#if defined(__FreeBSD_version) && __FreeBSD_version >= 900000
    __put_user(hkve->kve_vn_fileid, &tkve->kve_vn_fileid);
    __put_user(hkve->kve_vn_fsid, &tkve->kve_vn_fsid);
#else /* ! __FreeBSD_version >= 900000 */
    __put_user(hkve->kve_fileid, &tkve->kve_fileid);
    __put_user(hkve->kve_fsid, &tkve->kve_fsid);
#endif /* ! __FreeBSD_version >= 900000 */
    __put_user(hkve->kve_flags, &tkve->kve_flags);
    __put_user(hkve->kve_resident, &tkve->kve_resident);
    __put_user(hkve->kve_private_resident, &tkve->kve_private_resident);
    __put_user(hkve->kve_protection, &tkve->kve_protection);
    __put_user(hkve->kve_ref_count, &tkve->kve_ref_count);
    __put_user(hkve->kve_shadow_count, &tkve->kve_shadow_count);
#if defined(__FreeBSD_version) && __FreeBSD_version >= 900000
    __put_user(hkve->kve_vn_type, &tkve->kve_vn_type);
    __put_user(hkve->kve_vn_size, &tkve->kve_vn_size);
    __put_user(hkve->kve_vn_rdev, &tkve->kve_vn_rdev);
    __put_user(hkve->kve_vn_mode, &tkve->kve_vn_mode);
    __put_user(hkve->kve_status, &tkve->kve_status);
#endif /* __FreeBSD_version >= 900000 */
    strncpy(tkve->kve_path, hkve->kve_path, PATH_MAX);
}

abi_long
do_sysctl_kern_proc_vmmap(int pid, size_t olen,
        struct target_kinfo_vmentry *tkve, size_t *tlen)
{
    abi_long ret;
    int mib[4], cnt, sz;
    size_t len;
    char *buf, *bp, *eb, *tp;
    struct kinfo_vmentry *kve, kvme;
    struct target_kinfo_vmentry target_kvme;

    if (tlen == NULL)
        return -TARGET_EINVAL;

    len = 0;
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_VMMAP;
    mib[3] = pid;

    ret = get_errno(sysctl(mib, 4, NULL, &len, NULL, 0));
    if (is_error(ret))
        return ret;
    if (tkve == NULL) {
        *tlen = len;
        return ret;
    }
    len = len * 4 / 3;
    buf = g_malloc(len);
    if (buf == NULL)
        return -TARGET_ENOMEM;

    /*
     * Count the number of records.
     *
     * Given that the kinfo_file information returned by
     * the kernel may be differents sizes per record we have
     * to read it in and count the variable length records
     * by walking them.
     */
    ret = get_errno(sysctl(mib, 4, buf, &len, NULL, 0));
    if (is_error(ret)) {
        g_free(buf);
        return ret;
    }
    *tlen = len;
    cnt = 0;
    bp = buf;
    eb = buf + len;
    while (bp < eb) {
        kve = (struct kinfo_vmentry *)(uintptr_t)bp;
        bp += kve->kve_structsize;
        cnt++;
    }
    if (olen < *tlen) {
        g_free(buf);
        return -TARGET_EINVAL;
    }

    /*
     * Unpack the records from the kernel into full length records
     * and byte swap, if needed.
     */
    bp = buf;
    eb = buf + len;
    tp = (char *)tkve;
    while (bp < eb) {
        kve = (struct kinfo_vmentry *)(uintptr_t)bp;
        sz = kve->kve_structsize;
        /* Copy/expand into a zeroed buffer */
        memset(&kvme, 0, sizeof (kvme));
        memcpy(&kvme, kve, sz);
        /* Byte swap and copy into a target aligned buffer. */
        host_to_target_kinfo_vmentry(&target_kvme, &kvme);
        /* Copy target buffer to user buffer, packed. */
        memcpy(tp, &target_kvme, sz);
        /* Advance to next packed record. */
        bp += sz;
        /* Advance to next packed, target record. */
        tp += sz;
    }

    g_free(buf);
    return ret;
}

/*
 * XXX The following should maybe go some place else.  Also, see the note
 * about using "thunk" for sysctl's that pass data using structures.
 */
/* From sys/mount.h: */
#define TARGET_MFSNAMELEN	16	/* length of type name including null */
struct target_xvfsconf {
	abi_ulong vfc_vfsops;		/* filesystem op vector - not used */
	char vfc_name[TARGET_MFSNAMELEN];	/* filesystem type name */
	int32_t  vfc_typenum;		/* historic fs type number */
	int32_t  vfc_refcount;		/* number mounted of this type */
	int32_t  vfc_flags;		/* permanent flags */
	abi_ulong vfc_next;		/* next int list - not used */
};

/* vfc_flag definitions */
#define TARGET_VFCF_STATIC     0x00010000  /* statically compiled into kernel */
#define TARGET_VFCF_NETWORK    0x00020000  /* may get data over the network */
#define TARGET_VFCF_READONLY   0x00040000  /* writes are not implemented */
#define TARGET_VFCF_SYNTHETIC  0x00080000  /* doesn't represent real files */
#define TARGET_VFCF_LOOPBACK   0x00100000  /* aliases some other mounted FS */
#define TARGET_VFCF_UNICODE    0x00200000  /* stores file names as Unicode */
#define TARGET_VFCF_JAIL       0x00400000  /* can be mounted within a jail */
#define TARGET_VFCF_DELEGADMIN 0x00800000  /* supports delegated admin */
#define TARGET_VFCF_SBDRY      0x01000000  /* defer stop requests */

static int
host_to_target_vfc_flags(int flags)
{
    int ret = 0;

    if (flags & VFCF_STATIC)
	ret |= TARGET_VFCF_STATIC;
    if (flags & VFCF_NETWORK)
	ret |= TARGET_VFCF_NETWORK;
    if (flags & VFCF_READONLY)
	ret |= TARGET_VFCF_READONLY;
    if (flags & VFCF_SYNTHETIC)
	ret |= TARGET_VFCF_SYNTHETIC;
    if (flags & VFCF_LOOPBACK)
	ret |= TARGET_VFCF_LOOPBACK;
    if (flags & VFCF_UNICODE)
	ret |= TARGET_VFCF_UNICODE;
    if (flags & VFCF_JAIL)
	ret |= TARGET_VFCF_JAIL;
    if (flags & VFCF_DELEGADMIN)
	ret |= TARGET_VFCF_DELEGADMIN;
#ifdef VFCF_SBRDY
    if (flags & VFCF_SBDRY)
	ret |= TARGET_VFCF_SBDRY;
#endif

    return ret;
}

/*
 * XXX this uses the undocumented oidfmt interface to find the kind of
 * a requested sysctl, see /sys/kern/kern_sysctl.c:sysctl_sysctl_oidfmt()
 * (compare to src/sbin/sysctl/sysctl.c)
 */
static int
oidfmt(int *oid, int len, char *fmt, uint32_t *kind)
{
    int qoid[CTL_MAXNAME+2];
    uint8_t buf[BUFSIZ];
    int i;
    size_t j;

    qoid[0] = 0;
    qoid[1] = 4;
    memcpy(qoid + 2, oid, len * sizeof(int));

    j = sizeof(buf);
    i = sysctl(qoid, len + 2, buf, &j, 0, 0);
    if (i) {
        return i;
    }

    if (kind) {
        *kind = *(uint32_t *)buf;
    }

    if (fmt) {
        strcpy(fmt, (char *)(buf + sizeof(uint32_t)));
    }
    return 0;
}

/*
 * try and convert sysctl return data for the target.
 * XXX doesn't handle CTLTYPE_OPAQUE and CTLTYPE_STRUCT.
 */
static int sysctl_oldcvt(void *holdp, size_t holdlen, uint32_t kind)
{
    switch (kind & CTLTYPE) {
    case CTLTYPE_INT:
    case CTLTYPE_UINT:
        *(uint32_t *)holdp = tswap32(*(uint32_t *)holdp);
        break;

#ifdef TARGET_ABI32
    case CTLTYPE_LONG:
    case CTLTYPE_ULONG:
        *(uint32_t *)holdp = tswap32(*(long *)holdp);
        break;
#else
    case CTLTYPE_LONG:
        *(uint64_t *)holdp = tswap64(*(long *)holdp);
    case CTLTYPE_ULONG:
        *(uint64_t *)holdp = tswap64(*(unsigned long *)holdp);
        break;
#endif
#if !defined(__FreeBSD_version) || __FreeBSD_version < 900031
    case CTLTYPE_QUAD:
#else
    case CTLTYPE_U64:
    case CTLTYPE_S64:
#endif
        *(uint64_t *)holdp = tswap64(*(uint64_t *)holdp);
        break;

    case CTLTYPE_STRING:
        break;

    default:
        /* XXX unhandled */
        return -1;
    }
    return 0;
}

/*
 * Convert the undocmented name2oid sysctl data for the target.
 */
static inline void sysctl_name2oid(uint32_t *holdp, size_t holdlen)
{
    size_t i, num = holdlen / sizeof(uint32_t);

    for (i = 0; i < num; i++) {
        holdp[i] = tswap32(holdp[i]);
    }
}

static inline void sysctl_oidfmt(uint32_t *holdp)
{
    /* byte swap the kind */
    holdp[0] = tswap32(holdp[0]);
}

abi_long do_freebsd_sysctl(CPUArchState *env, abi_ulong namep, int32_t namelen,
        abi_ulong oldp, abi_ulong oldlenp, abi_ulong newp, abi_ulong newlen)
{
    abi_long ret;
    void *hnamep, *holdp = NULL, *hnewp = NULL;
    size_t holdlen;
    abi_ulong oldlen = 0;
    int32_t *snamep = g_malloc(sizeof(int32_t) * namelen), *p, *q, i;
    uint32_t kind = 0;
    CPUState *cpu = ENV_GET_CPU(env);
    TaskState *ts = (TaskState *)cpu->opaque;

    if (oldlenp) {
        if (get_user_ual(oldlen, oldlenp)) {
            return -TARGET_EFAULT;
        }
    }
    hnamep = lock_user(VERIFY_READ, namep, namelen, 1);
    if (hnamep == NULL) {
        return -TARGET_EFAULT;
    }
    if (newp) {
        hnewp = lock_user(VERIFY_READ, newp, newlen, 1);
        if (hnewp == NULL) {
            return -TARGET_EFAULT;
        }
    }
    if (oldp) {
        holdp = lock_user(VERIFY_WRITE, oldp, oldlen, 0);
        if (holdp == NULL) {
            return -TARGET_EFAULT;
        }
    }
    holdlen = oldlen;
    for (p = hnamep, q = snamep, i = 0; i < namelen; p++, i++) {
        *q++ = tswap32(*p);
    }
    oidfmt(snamep, namelen, NULL, &kind);

    /* Handle some arch/emulator dependent sysctl()'s here. */
    switch (snamep[0]) {
#if defined(TARGET_PPC)
	case CTL_MACHDEP:
		switch (snamep[1]) {
		case 1:	/* This should be documented elsewhere. */
			holdlen = sizeof(abi_ulong);
			(*(abi_ulong *)holdp) = env->dcache_line_size;
			ret = 0;
			goto out;
		}
#endif
    case CTL_KERN:
        switch (snamep[1]) {
        case KERN_USRSTACK:
#if TARGET_USRSTACK != 0
            if (oldlen) {
                (*(abi_ulong *)holdp) = tswapal(TARGET_USRSTACK);
            }
            holdlen = sizeof(abi_ulong);
            ret = 0;
#else
            ret = -TARGET_ENOENT;
#endif
            goto out;

        case KERN_PS_STRINGS:
#if defined(TARGET_PS_STRINGS)
            if (oldlen) {
                (*(abi_ulong *)holdp) = tswapal(TARGET_PS_STRINGS);
            }
            holdlen = sizeof(abi_ulong);
            ret = 0;
#else
            ret = -TARGET_ENOENT;
#endif
            goto out;

        case KERN_PROC:
            switch (snamep[2]) {
            case KERN_PROC_PATHNAME:
                holdlen = strlen(ts->bprm->fullpath) + 1;
                if (holdp) {
                    if (oldlen < holdlen) {
                        ret = -TARGET_EINVAL;
                        goto out;
                    }
                    strlcpy(holdp, ts->bprm->fullpath, oldlen);
                }
                ret = 0;
                goto out;

            case KERN_PROC_ALL:
            case KERN_PROC_PROC:
            case KERN_PROC_PID:
            case KERN_PROC_PID | KERN_PROC_INC_THREAD:
            case KERN_PROC_PGRP:
            case KERN_PROC_PGRP | KERN_PROC_INC_THREAD:
            case KERN_PROC_SESSION:
            case KERN_PROC_SESSION | KERN_PROC_INC_THREAD:
            case KERN_PROC_TTY:
            case KERN_PROC_TTY | KERN_PROC_INC_THREAD:
            case KERN_PROC_UID:
            case KERN_PROC_UID | KERN_PROC_INC_THREAD:
            case KERN_PROC_RUID:
            case KERN_PROC_RUID | KERN_PROC_INC_THREAD:
                ret = do_sysctl_kern_getprocs(snamep[2], snamep[3], oldlen,
                        holdp, &holdlen);
                goto out;

            case KERN_PROC_FILEDESC:
                ret = do_sysctl_kern_proc_filedesc(snamep[3], oldlen, holdp,
                        &holdlen);
                goto out;

            case KERN_PROC_VMMAP:
                ret = do_sysctl_kern_proc_vmmap(snamep[3], oldlen, holdp,
                        &holdlen);
                goto out;

            default:
                break;
            }
            break;

        default:
            break;
        }
        break;

    case CTL_VFS:
	{
	    static int oid_vfs_conflist;

	    if (!oid_vfs_conflist) {
	        int real_oid[CTL_MAXNAME+2];
		size_t len = sizeof(real_oid) / sizeof(int);

		if (sysctlnametomib("vfs.conflist", real_oid, &len) >= 0)
		    oid_vfs_conflist = real_oid[1];
	    }

	    if (oid_vfs_conflist && snamep[1] == oid_vfs_conflist) {
		struct xvfsconf *xvfsp;
		struct target_xvfsconf *txp;
		int cnt, i;

		if (sysctlbyname("vfs.conflist", NULL, &holdlen, NULL, 0) < 0) {
		    ret = -1;
		    goto out;
		}
		if (!holdp) {
		    ret = 0;
		    goto out;
		}
		xvfsp = (struct xvfsconf *)g_malloc(holdlen);
		if (xvfsp == NULL) {
		    ret = -TARGET_ENOMEM;
		    goto out;
		}
		if (sysctlbyname("vfs.conflist", xvfsp, &holdlen, NULL, 0) < 0){
		    g_free(xvfsp);
		    ret = -1;
		    goto out;
		}
		cnt = holdlen / sizeof(struct xvfsconf);
		holdlen = cnt * sizeof(struct target_xvfsconf);
		txp = (struct target_xvfsconf *)holdp;
		for (i = 0; i < cnt; i++) {
		    txp[i].vfc_vfsops = 0;
		    strlcpy(txp[i].vfc_name, xvfsp[i].vfc_name,
			TARGET_MFSNAMELEN);
		    txp[i].vfc_typenum = tswap32(xvfsp[i].vfc_typenum);
		    txp[i].vfc_refcount = tswap32(xvfsp[i].vfc_refcount);
		    txp[i].vfc_flags = tswap32(
			host_to_target_vfc_flags(xvfsp[i].vfc_flags));
		    txp[i].vfc_next = 0;
	        }
		g_free(xvfsp);
		ret = 0;
		goto out;
	    }
	}
	break;

    case CTL_HW:
        switch (snamep[1]) {
        case HW_MACHINE:
	    holdlen = sizeof(TARGET_HW_MACHINE);
	    if (holdp)
		strlcpy(holdp, TARGET_HW_MACHINE, oldlen);
            ret = 0;
            goto out;

        case HW_MACHINE_ARCH:
	    holdlen = sizeof(TARGET_HW_MACHINE_ARCH);
	    if (holdp)
		strlcpy(holdp, TARGET_HW_MACHINE_ARCH, oldlen);
            ret = 0;
            goto out;

#if defined(TARGET_AARCH64)
        case HW_NCPU:           /* XXX AARCH64 is not SMP ready */
            if (oldlen) {
                (*(int32_t *)holdp) = 1;
            }
            holdlen = sizeof(int32_t);
            ret = 0;
            goto out;
#endif

#if TARGET_ABI_BITS != HOST_LONG_BITS
	case HW_PHYSMEM:
	case HW_USERMEM:
	case HW_REALMEM:
	    holdlen = sizeof(abi_ulong);
	    ret = 0;

	    if (oldlen) {
	    	int mib[2] = {snamep[0], snamep[1]};
		unsigned long lvalue;
		size_t len = sizeof(lvalue);

		if (sysctl(mib, 2, &lvalue, &len, NULL, 0) == -1 ) {
			ret = -1;
		} else {
			abi_ulong maxmem = -0x100c000;
			if (((unsigned long)maxmem) < lvalue)
			    lvalue = maxmem;
			    (*(abi_ulong *)holdp) = lvalue;
		}
	    }
	    goto out;
#endif

        default:
            {
                static int oid_hw_availpages;
                static int oid_hw_pagesizes;

                if (!oid_hw_availpages) {
                    int real_oid[CTL_MAXNAME+2];
                    size_t len = sizeof(real_oid) / sizeof(int);

                    if (sysctlnametomib("hw.availpages", real_oid, &len) >= 0)
                        oid_hw_availpages = real_oid[1];
                }
                if (!oid_hw_pagesizes) {
                    int real_oid[CTL_MAXNAME+2];
                    size_t len = sizeof(real_oid) / sizeof(int);

                    if (sysctlnametomib("hw.pagesizes", real_oid, &len) >= 0)
                        oid_hw_pagesizes = real_oid[1];
                }

                if (oid_hw_availpages && snamep[1] == oid_hw_availpages) {
                    long lvalue;
                    size_t len = sizeof(lvalue);

                    if (sysctlbyname("hw.availpages", &lvalue, &len, NULL, 0)
                            == -1) {
                        ret = -1;
                    } else {
                        if (oldlen) {
                            (*(abi_ulong *)holdp) = tswapal((abi_ulong)lvalue);
                        }
                        holdlen = sizeof(abi_ulong);
                        ret = 0;
                    }
                    goto out;
                }

                if (oid_hw_pagesizes && snamep[1] == oid_hw_pagesizes) {
                    // XXX some targets do superpages now too... */
                    if (oldlen) {
                        (*(abi_ulong *)holdp) = tswapal((abi_ulong)getpagesize());
                        ((abi_ulong *)holdp)[1] = 0;
                    }
                    holdlen = sizeof(abi_ulong) * 2;
                    ret = 0;
                    goto out;
                }
                break;
            }
        }
    default:
        break;
    }

    ret = get_errno(sysctl(snamep, namelen, holdp, &holdlen, hnewp, newlen));
    if (!ret && (holdp != 0 && holdlen != 0)) {
        if (0 == snamep[0] && (2 == snamep[1] || 3 == snamep[1] ||
                    4 == snamep[1])) {
            switch (snamep[1]) {
            case 2:
            case 3:
                /* Handle the undocumented name2oid special case. */
                sysctl_name2oid(holdp, holdlen);
                break;

            case 4:
            default:
                /* Handle oidfmt */
                sysctl_oidfmt(holdp);
                break;
            }
        } else {
            sysctl_oldcvt(holdp, holdlen, kind);
        }
    }
#ifdef DEBUG
    else {
        printf("sysctl(mib[0]=%d, mib[1]=%d, mib[3]=%d...) returned %d\n",
        snamep[0], snamep[1], snamep[2], (int)ret);
    }
#endif

out:
    if (oldlenp) {
        put_user_ual(holdlen, oldlenp);
    }
    unlock_user(hnamep, namep, 0);
    unlock_user(holdp, oldp, holdlen);
    if (hnewp) {
        unlock_user(hnewp, newp, 0);
    }
    g_free(snamep);
    return ret;
}

/* sysarch() is architecture dependent. */
abi_long do_freebsd_sysarch(void *cpu_env, abi_long arg1, abi_long arg2)
{

    return do_freebsd_arch_sysarch(cpu_env, arg1, arg2);
}