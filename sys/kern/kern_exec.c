/*-
 * Execve system call implementation ported from FreeBSD 1.0.
 *
 * Copyright (c) 1989, 1990, 1991, 1992 William F. Jolitz, TeleMuse
 * All rights reserved.
 * Copyright (C) 2014 Serge Vakulenko, <serge@vak.ru>
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/exec.h>
#include <sys/mount.h>
#include <sys/resourcevar.h>
#include <sys/mman.h>
#include <sys/acct.h>
#include <sys/wait.h>
#include <sys/filedesc.h>
#include <vm/vm.h>
#include <machine/reg.h>

/*
 * exec system call
 */
struct execve_args {
	char	*fname;
	char	**argp;
	char	**envp;
};

/*
 * Close any files on exec?
 */
static void
fdcloseexec(p)
        struct proc *p;
{
        struct filedesc *fdp = p->p_fd;
        struct file **fpp;
        char *fdfp;
        register int i;

        fpp = fdp->fd_ofiles;
        fdfp = fdp->fd_ofileflags;
        for (i = 0; i <= fdp->fd_lastfile; i++, fpp++, fdfp++)
                if (*fpp != NULL && (*fdfp & UF_EXCLOSE)) {
                        if (*fdfp & UF_MAPPED)
                                (void) munmapfd(p, i);
                        (void) closef(*fpp, p);
                        *fpp = NULL;
                        *fdfp = 0;
                        if (i < fdp->fd_freefile)
                                fdp->fd_freefile = i;
                }
        while (fdp->fd_lastfile > 0 && fdp->fd_ofiles[fdp->fd_lastfile] == NULL)
                fdp->fd_lastfile--;
}

/*
 * Copy string to user space and count the length.
 */
static int
countoutstr(str, buf, argc, arglen)
        char *str, *buf;
        int *argc, *arglen;
{
        u_int stringlen;
        int rv;

        if (*arglen >= ARG_MAX)
                return E2BIG;

        rv = copyoutstr(str, buf, NBPG, &stringlen);
        if (rv)
                return (rv == ENAMETOOLONG) ? E2BIG : rv;
        *argc += 1;
        *arglen += stringlen;
        return 0;
}

/*
 * Copy string from user space and count the length.
 */
static int
countinstr(str, buf, argc, arglen)
        char *str, *buf;
        int *argc, *arglen;
{
        u_int stringlen;
        int rv;

        if (*arglen >= ARG_MAX)
                return E2BIG;

        rv = copyinstr(str, buf, NBPG, &stringlen);
        if (rv)
                return (rv == ENAMETOOLONG) ? E2BIG : rv;
        *argc += 1;
        *arglen += stringlen;
        return 0;
}

/*
 * Copy arguments and environment.
 */
static int
copyargs (uap, newframe, framep, indir, shellname, shellargs)
	register struct execve_args *uap;
	vm_offset_t newframe, *framep;
	int indir;
	char *shellname, *shellargs;
{
	char *stackp, **vectp, *stringp;
	int argc, envc, rv, *argbuf, *argp;

        /* Use the first stack page as buffer. */
	stackp = (char*) (newframe + MAXSSIZ - NBPG);

        /*
         * Pass 1: compute arg count and size.
         */
        u_int arglen;

        argc = 0;
        arglen = 0;
	vectp = uap->argp;
	if (indir) {
	    /* Count shell parameters. */
            rv = countoutstr(shellname, stackp, &argc, &arglen);
            if (rv)
                return rv;

            if (shellargs) {
                rv = countoutstr(shellargs, stackp, &argc, &arglen);
                if (rv)
                    return rv;
            }

            rv = countoutstr(uap->fname, stackp, &argc, &arglen);
            if (rv)
                return rv;

            if (vectp)
                vectp++;
        }

	if (vectp) {
	    /* Count arguments. */
            for (;;) {
		char *ptr = (char*) fuword(vectp++);
		if (ptr == 0) {
                    argc++;
                    break;
                }
                rv = countinstr(ptr, stackp, &argc, &arglen);
                if (rv)
                    return rv;
            }
        }

        vectp = uap->envp;
        envc = 0;
	if (vectp) {
	    /* Count environment strings. */
            for (;;) {
		char *ptr = (char*) fuword(vectp++);

		if (ptr == 0) {
                    break;
                }
                rv = countinstr(ptr, stackp, &envc, &arglen);
                if (rv)
                    return rv;
            }
        }
        envc++;
	arglen += (argc + envc + 1) * sizeof(int);

        /*
         * Pass 2: copy arguments to user stack.
         */
#define QALIGN(ptr) ((vm_offset_t) (ptr) & ~(sizeof(quad_t) - 1))

	/* allocate string buffer and arg buffer */
	argbuf = (int*) QALIGN(newframe + MAXSSIZ - arglen);
	stringp = (char*) (argbuf + argc + envc + 1);
        argp = argbuf + 1;

        argc = 0;
        arglen = 0;
	vectp = uap->argp;
	if (indir) {
	    /* Copy shell parameters. */
            suword(argp++, stringp);
            rv = countoutstr(shellname, stringp, &argc, &arglen);
            if (rv)
                return rv;

            if (shellargs) {
                suword(argp++, stringp+arglen);
                rv = countoutstr(shellargs, stringp+arglen, &argc, &arglen);
                if (rv)
                    return rv;
            }

            suword(argp++, stringp+arglen);
            rv = countoutstr(uap->fname, stringp+arglen, &argc, &arglen);
            if (rv)
                return rv;

            if (vectp)
                vectp++;
        }

	if (vectp) {
	    /* Count arguments. */
            for (;;) {
		char *ptr = (char*) fuword(vectp++);
		if (ptr == 0) {
                    suword(argp++, 0);
                    break;
                }
                suword(argp++, stringp+arglen);
                rv = countinstr(ptr, stringp+arglen, &argc, &arglen);
                if (rv)
                    return rv;
            }
        }

        vectp = uap->envp;
        envc = 0;
	if (vectp) {
	    /* Count environment strings. */
            for (;;) {
		char *ptr = (char*) fuword(vectp++);
		if (ptr == 0) {
                    suword(argp++, 0);
                    break;
                }
                suword(argp++, stringp+arglen);
                rv = countinstr(ptr, stringp+arglen, &envc, &arglen);
                if (rv)
                    return rv;
            }
        }
        argbuf[0] = argc;
        *framep = (vm_offset_t) argbuf;
        return 0;
}

/* ARGSUSED */
execve(p, uap, retval)
	struct proc *p;
	register struct execve_args *uap;
	int *retval;
{
	register struct nameidata *ndp;
	struct nameidata nd;
        int rv, amt, file_offset, virtual_offset, tsize, dsize, bsize, len;
	struct vattr attr;
	struct vmspace *vs;
	vm_offset_t addr, newframe, framep;
	char shellname[MAXINTERP];			/* 05 Aug 92*/
 	char *shellargs;
	union {
		char	ex_shell[MAXINTERP];	/* #! and interpreter name */
		struct	exec ex_hdr;
	} exdata;
	int indir = 0;

	/*
	 * Step 1. Lookup filename to see if we have something to execute.
	 */
	ndp = &nd;
again:
	NDINIT(ndp, LOOKUP, LOCKLEAF | FOLLOW | SAVENAME, UIO_USERSPACE,
	    uap->fname, p);

	/* is it there? */
	if (rv = namei(ndp))
		return rv;

	if (ndp->ni_vp->v_writecount) { /* don't exec if file is busy */
		rv = EBUSY;
		goto exec_fail;
	}
	/* does it have any attributes? */
	rv = VOP_GETATTR(ndp->ni_vp, &attr, p->p_ucred, p);
	if (rv)
		goto exec_fail;

	if (ndp->ni_vp->v_mount->mnt_flag & MNT_NOEXEC) { /* no exec on fs ?*/
		rv = EACCES;
		goto exec_fail;
	}

	/* is it executable, and a regular file? */
	if ((ndp->ni_vp->v_mount->mnt_flag & MNT_NOEXEC) ||	/* 29 Jul 92*/
		(VOP_ACCESS(ndp->ni_vp, VEXEC, p->p_ucred, p)) ||
		((attr.va_mode & 0111) == 0) ||
		(attr.va_type != VREG)) {
		rv = EACCES;
		goto exec_fail;
	}

	/*
	 * Step 2. Does the file contain a format we can
	 * understand and execute
	 *
	 * XXX 05 Aug 92
	 * Read in first few bytes of file for segment sizes, magic number:
	 *      ZMAGIC = demand paged RO text
	 * Also an ASCII line beginning with #! is
	 * the file name of a ``shell'' and arguments may be prepended
	 * to the argument list if given here.
	 */
	exdata.ex_shell[0] = '\0';	/* for zero length files */

	rv = vn_rdwr(UIO_READ, ndp->ni_vp, (caddr_t)&exdata, sizeof(exdata),
		0, UIO_SYSSPACE, IO_NODELOCKED, p->p_ucred, &amt, p);

	/* big enough to hold a header? */
	if (rv)
		goto exec_fail;

        if (exdata.ex_hdr.a_text != 0 && (ndp->ni_vp->v_flag & VTEXT) == 0 &&
	    ndp->ni_vp->v_writecount != 0) {
		rv = ETXTBSY;
		goto exec_fail;
	}

#define SHELLMAGIC	0x2123 /* #! */

	switch (exdata.ex_hdr.a_magic) {
	case ZMAGIC:
		virtual_offset = 0x400000;
		file_offset = NBPG;
		break;
	default:
		if ((exdata.ex_hdr.a_magic & 0xffff) != SHELLMAGIC) {
			rv = ENOEXEC;
			goto exec_fail;
		}
                if (indir) {
                        rv = ENOEXEC;
                        goto exec_fail;
                }

                char *cp, *sp;
                for (cp = &exdata.ex_shell[2];; ++cp) {
                        if (cp >= &exdata.ex_shell[MAXINTERP]) {
                                rv = ENOEXEC;
                                goto exec_fail;
                        }
                        if (*cp == '\n') {
                                *cp = '\0';
                                break;
                        }
                        if (*cp == '\t')
                                *cp = ' ';
                }
                cp = &exdata.ex_shell[2]; /* get shell interpreter name */
                while (*cp == ' ')
                        cp++;

                sp = shellname;
                while (*cp && *cp != ' ')
                        *sp++ = *cp++;
                *sp = '\0';

                /* copy the args in the #! line */
                while (*cp == ' ')
                        cp++;
                if (*cp) {
                        sp++;
                        shellargs = sp;
                        while (*cp)
                            *sp++ = *cp++;
                        *sp = '\0';
                } else {
                        shellargs = 0;
                }

                indir = 1;              /* indicate this is a script file */
                vput(ndp->ni_vp);

                ndp->ni_dirp = shellname;       /* find shell interpreter */
                ndp->ni_segflg = UIO_SYSSPACE;
                goto again;
	}
//printf("%s: text=%u, data=%u, bss=%u, entry=%08x\n", __func__, exdata.ex_hdr.a_text, exdata.ex_hdr.a_data, exdata.ex_hdr.a_bss, exdata.ex_hdr.a_entry);

	/* sanity check  "ain't not such thing as a sanity clause" -groucho */
	rv = ENOMEM;
	if (exdata.ex_hdr.a_text == 0 || exdata.ex_hdr.a_text > MAXTSIZ ||
	    exdata.ex_hdr.a_text % NBPG || exdata.ex_hdr.a_text > attr.va_size)
		goto exec_fail;

	if (exdata.ex_hdr.a_data == 0 || exdata.ex_hdr.a_data > DFLDSIZ
		|| exdata.ex_hdr.a_data > attr.va_size
		|| exdata.ex_hdr.a_data + exdata.ex_hdr.a_text > attr.va_size)
		goto exec_fail;

	if (exdata.ex_hdr.a_bss > MAXDSIZ)
		goto exec_fail;

	if (exdata.ex_hdr.a_text + exdata.ex_hdr.a_data + exdata.ex_hdr.a_bss > MAXTSIZ + MAXDSIZ)
		goto exec_fail;

	if (exdata.ex_hdr.a_data + exdata.ex_hdr.a_bss > p->p_rlimit[RLIMIT_DATA].rlim_cur)
		goto exec_fail;

	if (exdata.ex_hdr.a_entry > virtual_offset + exdata.ex_hdr.a_text + exdata.ex_hdr.a_data)
		goto exec_fail;

	/*
	 * Step 3.  File and header are valid. Now, dig out the strings
	 * out of the old process image.
	 */

	/*
	 * We implement a single-pass algorithm that builds a new stack
	 * frame within the address space of the "old" process image,
	 * avoiding the second pass entirely. Thus, the new frame is
	 * in position to be run. This consumes much virtual address space,
	 * and two pages more of 'real' memory, such are the costs.
	 * [Also, note the cache wipe that's avoided!]
	 */

	/* create anonymous memory region for new stack */
	vs = p->p_vmspace;
        if ((unsigned)vs->vm_maxsaddr + MAXSSIZ < USRSTACK)
               newframe = (vm_offset_t) USRSTACK - MAXSSIZ;
        else {
               newframe = (vm_offset_t) USRSTACK - 2*MAXSSIZ;
               vs->vm_maxsaddr = (caddr_t) newframe;
        }
	rv = vm_allocate(&vs->vm_map, &newframe, MAXSSIZ, FALSE);
	if (rv)
                goto exec_fail;

        rv = copyargs (uap, newframe, &framep, indir, shellname, shellargs);
	if (rv) {
                goto exec_dealloc;
        }

	/*
	 * Step 4. Build the new processes image.
	 *
	 * At this point, we are committed -- destroy old executable!
	 */

	/* blow away all address space, except the stack */
	rv = vm_deallocate(&vs->vm_map, 0, USRSTACK - 2*MAXSSIZ);
	if (rv)
		goto exec_abort;

	/* destroy "old" stack */
	if ((unsigned)newframe < USRSTACK - MAXSSIZ) {
		rv = vm_deallocate(&vs->vm_map, USRSTACK - MAXSSIZ, MAXSSIZ);
		if (rv)
			goto exec_abort;
	} else {
		rv = vm_deallocate(&vs->vm_map, USRSTACK - 2*MAXSSIZ, MAXSSIZ);
		if (rv)
			goto exec_abort;
	}

	/* build a new address space */

	/* treat text, data, and bss in terms of integral page size */
	tsize = roundup(exdata.ex_hdr.a_text, NBPG);
	dsize = roundup(exdata.ex_hdr.a_data, NBPG);
	bsize = roundup(exdata.ex_hdr.a_bss, NBPG);

	addr = virtual_offset;

	/* map text as being read/execute only and demand paged */
	rv = vm_mmap(&vs->vm_map, &addr, tsize, VM_PROT_READ|VM_PROT_EXECUTE,
		VM_PROT_DEFAULT, MAP_FILE|MAP_PRIVATE|MAP_FIXED,
		(caddr_t)ndp->ni_vp, file_offset);
	if (rv)
		goto exec_abort;

	addr = virtual_offset + tsize;

	/* map data as being read/write and demand paged */
	rv = vm_mmap(&vs->vm_map, &addr, dsize,
		VM_PROT_READ | VM_PROT_WRITE | (tsize ? 0 : VM_PROT_EXECUTE),
		VM_PROT_DEFAULT, MAP_FILE|MAP_PRIVATE|MAP_FIXED,
		(caddr_t)ndp->ni_vp, file_offset + tsize);
	if (rv)
		goto exec_abort;

	/* create anonymous memory region for bss */
	addr = virtual_offset + tsize + dsize;
	rv = vm_allocate(&vs->vm_map, &addr, bsize, FALSE);
	if (rv)
		goto exec_abort;

	/*
	 * Step 5. Prepare process for execution.
	 */

	/* touchup process information -- vm system is unfinished! */
	vs->vm_tsize = tsize/NBPG;		/* text size (pages) XXX */
	vs->vm_dsize = (dsize+bsize)/NBPG;	/* data size (pages) XXX */
	vs->vm_taddr = (caddr_t) virtual_offset; /* virtual address of text */
	vs->vm_daddr = (caddr_t) virtual_offset + tsize; /* virtual address of data */
	vs->vm_maxsaddr = (caddr_t) newframe;	/* user VA at max stack growth XXX */
	vs->vm_ssize = (newframe + MAXSSIZ - framep - 1) / NBPG + 1;	/* stack size (pages) */

	/* close files on exec, fixup signals */
	fdcloseexec(p);
	execsigs(p);

	/* name this process - nameiexec(p, ndp) */
	len = ndp->ni_cnd.cn_namelen;
        if (len > MAXCOMLEN)
                len = MAXCOMLEN;
	bcopy(ndp->ni_cnd.cn_nameptr, p->p_comm, len);
	p->p_comm[len] = 0;

	/* mark as executable, wakeup any process that was vforked and tell
	 * it that it now has it's own resources back */
	p->p_flag |= P_EXEC;
	if (p->p_pptr && (p->p_flag & P_PPWAIT)) {
	    p->p_flag &= ~P_PPWAIT;
	    wakeup(p->p_pptr);
	}

	/* implement set userid/groupid */
	if ((attr.va_mode&VSUID) && (p->p_flag & P_TRACED) == 0) {
	    p->p_ucred = crcopy(p->p_ucred);
	    p->p_cred->p_svuid = p->p_ucred->cr_uid = attr.va_uid;
	}
	if ((attr.va_mode&VSGID) && (p->p_flag & P_TRACED) == 0) {
	    p->p_ucred = crcopy(p->p_ucred);
	    p->p_cred->p_svgid = p->p_ucred->cr_groups[0] = attr.va_gid;
	}

	/* setup initial register state */
	p->p_md.md_regs[SP] = framep;
	setregs(p, exdata.ex_hdr.a_entry);

 	ndp->ni_vp->v_flag |= VTEXT;		/* mark vnode pure text */
	vput(ndp->ni_vp);

	/* if tracing process, pass control back to debugger so breakpoints
	   can be set before the program "runs" */
	if (p->p_flag & P_TRACED)
		psignal(p, SIGTRAP);
	p->p_acflag &= ~AFORK;		/* remove fork, but no exec flag */
//printf("%s: succeeded\n", __func__);
	return 0;

exec_dealloc:
	/* remove interim "new" stack frame we were building */
	vm_deallocate(&vs->vm_map, newframe, MAXSSIZ);

exec_fail:
	vput(ndp->ni_vp);
	return rv;

exec_abort:
	/* sorry, no more process anymore. exit gracefully */
	vm_deallocate(&vs->vm_map, newframe, MAXSSIZ);
	vput(ndp->ni_vp);
	exit1(p, W_EXITCODE(0, SIGABRT));

	/* NOTREACHED */
	return 0;
}
