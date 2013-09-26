/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011 Pawel Jakub Dawidek <pawel@dawidek.net>.
 * All rights reserved.
 * Copyright (c) 2013 by Delphix. All rights reserved.
 */

/* Portions Copyright 2010 Robert Milkowski */
/* Portions Copyright 2013 Jorgen Lundman */

#include <sys/types.h>
#include <sys/syslimits.h>
#include <sys/param.h>

#include <sys/systm.h>

#ifndef __APPLE__
#include <sys/sysmacros.h>
#include <sys/kmem.h>
#include <sys/pathname.h>
#include <sys/vfs.h>
#include <sys/vfs_opreg.h>
#endif /* !__APPLE__ */

#include <sys/mount.h>
#include <sys/vnode.h>

#ifdef __APPLE__
#include <mntent.h>
#include <sys/zfs_context.h>
#include <sys/zfs_vfsops.h>
#include <sys/sysctl.h>
#endif /* __APPLE__ */

#ifndef __APPLE__
#include <sys/cmn_err.h>
#include "fs/fs_subr.h"
#endif /* !__APPLE__ */

#include <sys/zfs_znode.h>
#include <sys/zfs_dir.h>

#include <sys/zfs_ctldir.h>
#include <sys/refcount.h>
#include <sys/zil.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dsl_prop.h>

#include <sys/dmu_objset.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_deleg.h>
#include <sys/spa.h>
#include <sys/zap.h>

#include <zfs_comutil.h>

#ifndef __APPLE__
#include <sys/sa.h>
#include <sys/sa_impl.h>
#include <sys/varargs.h>
#include <sys/policy.h>
#include <sys/atomic.h>
#include <sys/mkdev.h>
#include <sys/modctl.h>
#include <sys/refstr.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_ctldir.h>
#include <sys/bootconf.h>
#include <sys/sunddi.h>
#include <sys/dnlc.h>
#endif /* !__APPLE__ */
#include <sys/dmu_objset.h>
#include <sys/spa_boot.h>
#include <sys/zpl.h>
#include "zfs_comutil.h"

//#define dprintf printf

//illumos defines their vfssw[] in /usr/src/uts/common/os/vfs_conf.c
struct vfssw vfssw[] = {
	{ "BADVFS" },				/* invalid */
	{ "specfs" },				/* SPECFS */
	{ "ufs" },				/* UFS */
	{ "fifofs" },				/* FIFOFS */
	{ "namefs" },				/* NAMEFS */
	{ "proc" },				/* PROCFS */
	{ "samfs" },				/* QFS */
	{ "nfs" },				/* NFS Version 2 */
	{ "zfs" },				/* ZFS */
	{ "hsfs" },				/* HSFS */
	{ "lofs" },				/* LOFS */
	{ "tmpfs" },				/* TMPFS */
	{ "fd" },				/* FDFS */
	{ "pcfs" },				/* PCFS */
	{ "swapfs" /*, swapinit*/ },		/* SWAPFS */
	{ "mntfs" },				/* MNTFS */
	{ "devfs" },				/* DEVFS */
	{ "dev" },				/* DEV */
	{ "ctfs" },				/* CONTRACTFS */
	{ "objfs" },				/* OBJFS */
	{ "sharefs" },				/* SHAREFS */
	{ "dcfs" },				/* DCFS */
	{ "smbfs" },				/* SMBFS */
	{ "" },					/* reserved for loadable fs */
	{ "" },
	{ "" },
	{ "" },
	{ "" },
	{ "" },
	{ "" },
	{ "" },
	{ "" },
	{ "" },
	{ "" },
	{ "" },
	{ "" },
};

const int nfstype = (sizeof (vfssw) / sizeof (vfssw[0]));


#define	FSTYPSZ		16	/* max size of fs identifier */

#define	MAX_MNTOPT_STR	1024	/* max length of mount options string */

#define MNTOPT_OWNERS "owners"
#define MNTOPT_NOOWNERS "noowners"

static char ** vfs_mergecancelopts(const mntopt_t *, const mntopt_t *);
static void vfs_clearmntopt_nolock(mntopts_t *, const char *, int);
static void vfs_setmntopt_nolock(mntopts_t *, const char *,
    const char *, int, int);
static int  vfs_optionisset_nolock(const mntopts_t *, const char *, char **);
static void vfs_freeopt(mntopt_t *);
static void vfs_swapopttbl_nolock(mntopts_t *, mntopts_t *);
static void vfs_swapopttbl(mntopts_t *, mntopts_t *);
static void vfs_copyopt(const mntopt_t *s, mntopt_t *d);
static void vfs_copyopttbl_extend(const mntopts_t *, mntopts_t *, int);
static void vfs_createopttbl_extend(mntopts_t *, const char *,
    const mntopts_t *);
static char **vfs_copycancelopt_extend(char **const, int);
static void vfs_freecancelopt(char **);
static int zfs_vfsinit(int, char *);

static char **
vfs_mergecancelopts(const mntopt_t *mop1, const mntopt_t *mop2)
{
	dprintf("+vfs_mergecancelopts\n");
	int c1 = 0;
	int c2 = 0;
	char **result;
	char **sp1, **sp2, **dp;

	/*
	 * First we count both lists of cancel options.
	 * If either is NULL or has no elements, we return a copy of
	 * the other.
	 */
	if (mop1->mo_cancel != NULL) {
		for (; mop1->mo_cancel[c1] != NULL; c1++)
			/* count cancel options in mop1 */;
	}

	if (c1 == 0)
		return (vfs_copycancelopt_extend(mop2->mo_cancel, 0));

	if (mop2->mo_cancel != NULL) {
		for (; mop2->mo_cancel[c2] != NULL; c2++)
			/* count cancel options in mop2 */;
	}

	result = vfs_copycancelopt_extend(mop1->mo_cancel, c2);

	if (c2 == 0)
		return (result);

	/*
	 * When we get here, we've got two sets of cancel options;
	 * we need to merge the two sets.  We know that the result
	 * array has "c1+c2+1" entries and in the end we might shrink
	 * it.
	 * Result now has a copy of the c1 entries from mop1; we'll
	 * now lookup all the entries of mop2 in mop1 and copy it if
	 * it is unique.
	 * This operation is O(n^2) but it's only called once per
	 * filesystem per duplicate option.  This is a situation
	 * which doesn't arise with the filesystems in ON and
	 * n is generally 1.
	 */

	dp = &result[c1];
	for (sp2 = mop2->mo_cancel; *sp2 != NULL; sp2++) {
		for (sp1 = mop1->mo_cancel; *sp1 != NULL; sp1++) {
			if (strcmp(*sp1, *sp2) == 0)
				break;
		}
		if (*sp1 == NULL) {
			/*
			 * Option *sp2 not found in mop1, so copy it.
			 * The calls to vfs_copycancelopt_extend()
			 * guarantee that there's enough room.
			 */
			*dp = kmem_alloc(strlen(*sp2) + 1, KM_SLEEP);
			(void) strcpy(*dp++, *sp2);
		}
	}
	if (dp != &result[c1+c2]) {
		size_t bytes = (dp - result + 1) * sizeof (char *);
		char **nres = kmem_alloc(bytes, KM_SLEEP);

		bcopy(result, nres, bytes);
		kmem_free(result, (c1 + c2 + 1) * sizeof (char *));
		result = nres;
	}
	dprintf("-vfs_mergecancelopts\n");
	return (result);
}

/*
 * Merge two mount option tables (outer and inner) into one.  This is very
 * similar to "merging" global variables and automatic variables in C.
 *
 * This isn't (and doesn't have to be) fast.
 *
 * This function is *not* for general use by filesystems.
 *
 * Note: caller is responsible for locking the vfs list, if needed,
 *       to protect omo, imo & dmo.
 */
void
vfs_mergeopttbl(const mntopts_t *omo, const mntopts_t *imo, mntopts_t *dmo)
{
	dprintf("+vfs_mergeopttbl\n");

	if(omo != NULL && imo != NULL) {
		dprintf("omo count %d : imo count %d\n", omo->mo_count, imo->mo_count);
	}
	
	uint_t i, count;
	mntopt_t *mop, *motbl;
	uint_t freeidx;

	/*
	 * First determine how much space we need to allocate.
	 */
	count = omo->mo_count;
	for (i = 0; i < imo->mo_count; i++) {
		if (imo->mo_list[i].mo_flags & MO_EMPTY)
			continue;
		if (vfs_hasopt(omo, imo->mo_list[i].mo_name) == NULL)
			count++;
	}
	ASSERT(count >= omo->mo_count &&
	    count <= omo->mo_count + imo->mo_count);
	motbl = kmem_alloc(count * sizeof (mntopt_t), KM_SLEEP);
	for (i = 0; i < omo->mo_count; i++)
		vfs_copyopt(&omo->mo_list[i], &motbl[i]);
	freeidx = omo->mo_count;
	for (i = 0; i < imo->mo_count; i++) {
		if (imo->mo_list[i].mo_flags & MO_EMPTY)
			continue;
		if ((mop = vfs_hasopt(omo, imo->mo_list[i].mo_name)) != NULL) {
			char **newcanp;
			uint_t index = mop - omo->mo_list;

			newcanp = vfs_mergecancelopts(mop, &motbl[index]);

			vfs_freeopt(&motbl[index]);
			vfs_copyopt(&imo->mo_list[i], &motbl[index]);

			vfs_freecancelopt(motbl[index].mo_cancel);
			motbl[index].mo_cancel = newcanp;
		} else {
			/*
			 * If it's a new option, just copy it over to the first
			 * free location.
			 */
			vfs_copyopt(&imo->mo_list[i], &motbl[freeidx++]);
		}
	}
	dmo->mo_count = count;
	dmo->mo_list = motbl;
	dprintf("-vfs_mergeopttbl\n");
}



/*
 * Functions to set and clear mount options in a mount options table.
 */

/*
 * Clear a mount option, if it exists.
 *
 * The update_mnttab arg indicates whether mops is part of a vfs that is on
 * the vfs list.
 */
static void
vfs_clearmntopt_nolock(mntopts_t *mops, const char *opt, int update_mnttab)
{
	dprintf("+vfs_clearmntopt_nolock\n");
	struct mntopt *mop;
	uint_t i, count;

	//ASSERT(!update_mnttab || RW_WRITE_HELD(&vfslist));
	ASSERT(!update_mnttab);

	count = mops->mo_count;
	dprintf("mops mo_count is %d\n", count);
	for (i = 0; i < count; i++) {
		mop = &mops->mo_list[i];

		if (mop->mo_flags & MO_EMPTY)
			continue;
		if (strcmp(opt, mop->mo_name))
			continue;
		mop->mo_flags &= ~MO_SET;
		if (mop->mo_arg != NULL) {
			kmem_free(mop->mo_arg, strlen(mop->mo_arg) + 1);
		}
		mop->mo_arg = NULL;
		//if (update_mnttab)
		//	vfs_mnttab_modtimeupd();
		break;
	}
	dprintf("-vfs_clearmntopt_nolock\n");
}

void
vfs_clearmntopt(zfsvfs_t *zfsvfs, const char *opt)
{
	dprintf("+vfs_clearmntopt\n");

        int gotlock = 0;

        //if (VFS_ON_LIST(vfsp)) {
        //        gotlock = 1;
        //        vfs_list_lock();
        //}
	//zfsvfs_t *zfsvfs = vfsp->vfs_data;
	//zfsvfs_t *zfsvfs = vfs_fsprivate(vfsp);
	//mntopts_t *mops = vfsp.vfs_mntopts;

        vfs_clearmntopt_nolock(&zfsvfs->vfs_mntopts, opt, gotlock);
        //if (gotlock)
                //vfs_list_unlock();
	dprintf("-vfs_clearmntopt\n");
}


/*
 * Set a mount option on.  If it's not found in the table, it's silently
 * ignored.  If the option has MO_IGNORE set, it is still set unless the
 * VFS_NOFORCEOPT bit is set in the flags.  Also, VFS_DISPLAY/VFS_NODISPLAY flag
 * bits can be used to toggle the MO_NODISPLAY bit for the option.
 * If the VFS_CREATEOPT flag bit is set then the first option slot with
 * MO_EMPTY set is created as the option passed in.
 *
 * The update_mnttab arg indicates whether mops is part of a vfs that is on
 * the vfs list.
 */
static void
vfs_setmntopt_nolock(mntopts_t *mops, const char *opt,
    const char *arg, int flags, int update_mnttab)
{
	dprintf("+vfs_setmntopt_nolock\n");
        mntopt_t *mop;
        uint_t i, count;
        char *sp;

        //ASSERT(!update_mnttab || RW_WRITE_HELD(&vfslist));
        ASSERT(!update_mnttab);

        if (flags & VFS_CREATEOPT) {
                if (vfs_hasopt(mops, opt) != NULL) {
                        flags &= ~VFS_CREATEOPT;
                }
        }
        count = mops->mo_count;
	dprintf("mops mo_count is %d\n", count);
        for (i = 0; i < count; i++) {
                mop = &mops->mo_list[i];

                if (mop->mo_flags & MO_EMPTY) {
                        if ((flags & VFS_CREATEOPT) == 0)
                                continue;
                        sp = kmem_alloc(strlen(opt) + 1, KM_SLEEP);
                        (void) strcpy(sp, opt);
                        mop->mo_name = sp;
                        if (arg != NULL)
                                mop->mo_flags = MO_HASVALUE;
                        else
                                mop->mo_flags = 0;
                } else if (strcmp(opt, mop->mo_name)) {
                        continue;
                }
                if ((mop->mo_flags & MO_IGNORE) && (flags & VFS_NOFORCEOPT))
                        break;
                if (arg != NULL && (mop->mo_flags & MO_HASVALUE) != 0) {
                        sp = kmem_alloc(strlen(arg) + 1, KM_SLEEP);
                        (void) strcpy(sp, arg);
                } else {
                        sp = NULL;
                }
                if (mop->mo_arg != NULL)
                        kmem_free(mop->mo_arg, strlen(mop->mo_arg) + 1);
                mop->mo_arg = sp;
                if (flags & VFS_DISPLAY)
                        mop->mo_flags &= ~MO_NODISPLAY;
                if (flags & VFS_NODISPLAY)
                        mop->mo_flags |= MO_NODISPLAY;
                mop->mo_flags |= MO_SET;
                if (mop->mo_cancel != NULL) {
                        char **cp;

                        for (cp = mop->mo_cancel; *cp != NULL; cp++)
                                vfs_clearmntopt_nolock(mops, *cp, 0);
                }
                //if (update_mnttab)
                //        vfs_mnttab_modtimeupd();
                break;
        }
	dprintf("-vfs_setmntopt_nolock\n");
}

void
vfs_setmntopt(zfsvfs_t *zfsvfs, const char *opt, const char *arg, int flags)
{
	dprintf("+vfs_setmntopt\n");

        int gotlock = 0;

        //if (VFS_ON_LIST(vfsp)) {
        //        gotlock = 1;
        //        vfs_list_lock();
        //}
        vfs_setmntopt_nolock(&zfsvfs->vfs_mntopts, opt, arg, flags, gotlock);
        //if (gotlock)
        //        vfs_list_unlock();
	dprintf("-vfs_setmntopt\n");

}

void
vfs_parsemntopts(mntopts_t *mops, char *osp, int create)
{
	dprintf("+vfs_parsemntopts\n");
	char *s = osp, *p, *nextop, *valp, *cp, *ep;
	int setflg = VFS_NOFORCEOPT;

	if (osp == NULL)
		return;
	while (*s != '\0') {
		p = strchr(s, ',');	/* find next option */
		if (p == NULL) {
			cp = NULL;
			p = s + strlen(s);
		} else {
			cp = p;		/* save location of comma */
			*p++ = '\0';	/* mark end and point to next option */
		}
		nextop = p;
		p = strchr(s, '=');	/* look for value */
		if (p == NULL) {
			valp = NULL;	/* no value supplied */
		} else {
			ep = p;		/* save location of equals */
			*p++ = '\0';	/* end option and point to value */
			valp = p;
		}
		/*
		 * set option into options table
		 */
		if (create)
			setflg |= VFS_CREATEOPT;
		vfs_setmntopt_nolock(mops, s, valp, setflg, 0);
		if (cp != NULL)
			*cp = ',';	/* restore the comma */
		if (valp != NULL)
			*ep = '=';	/* restore the equals */
		s = nextop;
	}
	dprintf("-vfs_parsemntopts\n");
}

/*
 * Function to inquire if an option exists in a mount options table.
 * Returns a pointer to the option if it exists, else NULL.
 *
 * This function is *not* for general use by filesystems.
 *
 * Note: caller is responsible for locking the vfs list, if needed,
 *       to protect mops.
 */
struct mntopt *
vfs_hasopt(const mntopts_t *mops, const char *opt)
{
        dprintf("+vfs_hasopt\n");
        struct mntopt *mop;
        uint_t i, count;

        count = mops->mo_count;
        for (i = 0; i < count; i++) {
                mop = &mops->mo_list[i];

                if (mop->mo_flags & MO_EMPTY)
                        continue;
                if (strcmp(opt, mop->mo_name) == 0)
                {
                        dprintf("-vfs_hasopt 1\n");
                        return (mop);
                }
        }
        dprintf("-vfs_hasopt 2\n");
        return (NULL);
}

/*
 * Function to inquire if an option is set in a mount options table.
 * Returns non-zero if set and fills in the arg pointer with a pointer to
 * the argument string or NULL if there is no argument string.
 */
static int
vfs_optionisset_nolock(const mntopts_t *mops, const char *opt, char **argp)
{
	dprintf("+vfs_optionisset_nolock\n");
	struct mntopt *mop;
	uint_t i, count;

	count = mops->mo_count;
	for (i = 0; i < count; i++) {
		mop = &mops->mo_list[i];

		if (mop->mo_flags & MO_EMPTY)
			continue;
		if (strcmp(opt, mop->mo_name))
			continue;
		if ((mop->mo_flags & MO_SET) == 0) {
			dprintf("-vfs_optionisset_nolock ret 0 1st\n");
			return (0);
		}
		if (argp != NULL && (mop->mo_flags & MO_HASVALUE) != 0)
			*argp = mop->mo_arg;
		dprintf("-vfs_optionisset_nolock ret 1\n");
		return (1);
	}
	dprintf("-vfs_optionisset_nolock ret 0 2nd\n");
	return (0);
}


int
vfs_optionisset(const zfsvfs_t *zfsvfs, const char *opt, char **argp)
{
	dprintf("+vfs_optionisset\n");
	int ret;

	//vfs_list_read_lock();
	ret = vfs_optionisset_nolock(&zfsvfs->vfs_mntopts, opt, argp);
	//vfs_list_unlock();
	dprintf("-vfs_optionisset\n");
	return (ret);
}

/*
 * Construct a comma separated string of the options set in the given
 * mount table, return the string in the given buffer.  Return non-zero if
 * the buffer would overflow.
 *
 * This function is *not* for general use by filesystems.
 *
 * Note: caller is responsible for locking the vfs list, if needed,
 *       to protect mp.
 */
int
vfs_buildoptionstr(const mntopts_t *mp, char *buf, int len)
{
	char *cp;
	uint_t i;

	buf[0] = '\0';
	cp = buf;
	for (i = 0; i < mp->mo_count; i++) {
		struct mntopt *mop;

		mop = &mp->mo_list[i];
		if (mop->mo_flags & MO_SET) {
			int optlen, comma = 0;

			if (buf[0] != '\0')
				comma = 1;
			optlen = strlen(mop->mo_name);
			if (strlen(buf) + comma + optlen + 1 > len)
				goto err;
			if (comma)
				*cp++ = ',';
			(void) strcpy(cp, mop->mo_name);
			cp += optlen;
			/*
			 * Append option value if there is one
			 */
			if (mop->mo_arg != NULL) {
				int arglen;

				arglen = strlen(mop->mo_arg);
				if (strlen(buf) + arglen + 2 > len)
					goto err;
				*cp++ = '=';
				(void) strcpy(cp, mop->mo_arg);
				cp += arglen;
			}
		}
	}
	return (0);
err:
	return (EOVERFLOW);
}

/*
 * Create an empty options table with enough empty slots to hold all
 * The options in the options string passed as an argument.
 * Potentially prepend another options table.
 *
 * Note: caller is responsible for locking the vfs list, if needed,
 *       to protect mops.
 */
static void
vfs_createopttbl_extend(mntopts_t *mops, const char *opts,
    const mntopts_t *mtmpl)
{
	dprintf("+vfs_createopttbl_extend\n");
	const char *s = opts;
	uint_t count;

	if (opts == NULL || *opts == '\0') {
		count = 0;
	} else {
		count = 1;

		/*
		 * Count number of options in the string
		 */
		for (s = strchr(s, ','); s != NULL; s = strchr(s, ',')) {
			count++;
			s++;
		}
	}
	vfs_copyopttbl_extend(mtmpl, mops, count);
	dprintf("-vfs_createopttbl_extend\n");
}

/*
 * Swap two mount options tables
 */
static void
vfs_swapopttbl_nolock(mntopts_t *optbl1, mntopts_t *optbl2)
{
	uint_t tmpcnt;
	mntopt_t *tmplist;

	tmpcnt = optbl2->mo_count;
	tmplist = optbl2->mo_list;
	optbl2->mo_count = optbl1->mo_count;
	optbl2->mo_list = optbl1->mo_list;
	optbl1->mo_count = tmpcnt;
	optbl1->mo_list = tmplist;
}

static void
vfs_swapopttbl(mntopts_t *optbl1, mntopts_t *optbl2)
{
	//vfs_list_lock();
	vfs_swapopttbl_nolock(optbl1, optbl2);
	//vfs_mnttab_modtimeupd();
	//vfs_list_unlock();
}

static char **
vfs_copycancelopt_extend(char **const moc, int extend)
{
	int i = 0;
	int j;
	char **result;

	if (moc != NULL) {
		for (; moc[i] != NULL; i++)
			/* count number of options to cancel */;
	}

	if (i + extend == 0)
		return (NULL);

	result = kmem_alloc((i + extend + 1) * sizeof (char *), KM_SLEEP);

	for (j = 0; j < i; j++) {
		result[j] = kmem_alloc(strlen(moc[j]) + 1, KM_SLEEP);
		(void) strcpy(result[j], moc[j]);
	}
	for (; j <= i + extend; j++)
		result[j] = NULL;

	return (result);
}


static void
vfs_copyopt(const mntopt_t *s, mntopt_t *d)
{
	char *sp, *dp;

	d->mo_flags = s->mo_flags;
	d->mo_data = s->mo_data;
	sp = s->mo_name;
	if (sp != NULL) {
		dp = kmem_alloc(strlen(sp) + 1, KM_SLEEP);
		(void) strcpy(dp, sp);
		d->mo_name = dp;
	} else {
		d->mo_name = NULL; /* should never happen */
	}

	d->mo_cancel = vfs_copycancelopt_extend(s->mo_cancel, 0);

	sp = s->mo_arg;
	if (sp != NULL) {
		dp = kmem_alloc(strlen(sp) + 1, KM_SLEEP);
		(void) strcpy(dp, sp);
		d->mo_arg = dp;
	} else {
		d->mo_arg = NULL;
	}
}

/*
 * Copy a mount options table, possibly allocating some spare
 * slots at the end.  It is permissible to copy_extend the NULL table.
 */
static void
vfs_copyopttbl_extend(const mntopts_t *smo, mntopts_t *dmo, int extra)
{
	dprintf("+vfs_copyopttbl_extend\n");
	uint_t i, count;
	mntopt_t *motbl;

	/*
	 * Clear out any existing stuff in the options table being initialized
	 */
	vfs_freeopttbl(dmo);
	count = (smo == NULL) ? 0 : smo->mo_count;
	if ((count + extra) == 0)	/* nothing to do */
		return;
	dmo->mo_count = count + extra;
	motbl = kmem_zalloc((count + extra) * sizeof (mntopt_t), KM_SLEEP);
	dmo->mo_list = motbl;
	for (i = 0; i < count; i++) {
		vfs_copyopt(&smo->mo_list[i], &motbl[i]);
	}
	for (i = count; i < count + extra; i++) {
		motbl[i].mo_flags = MO_EMPTY;
	}
	dprintf("-vfs_copyopttbl_extend\n");
}



/*
 * Copy a mount options table.
 *
 * This function is *not* for general use by filesystems.
 *
 * Note: caller is responsible for locking the vfs list, if needed,
 *       to protect smo and dmo.
 */
void
vfs_copyopttbl(const mntopts_t *smo, mntopts_t *dmo)
{
	vfs_copyopttbl_extend(smo, dmo, 0);
}


static void
vfs_freecancelopt(char **moc)
{
	dprintf("+vfs_freecancelopt\n");
	if (moc != NULL) {
		int ccnt = 0;
		char **cp;

		for (cp = moc; *cp != NULL; cp++) {
			kmem_free(*cp, strlen(*cp) + 1);
			ccnt++;
		}
		kmem_free(moc, (ccnt + 1) * sizeof (char *));
	}
	dprintf("-vfs_freecancelopt\n");
}

static void
vfs_freeopt(mntopt_t *mop)
{
	dprintf("+vfs_freeopt\n");
	if (mop->mo_name != NULL)
		kmem_free(mop->mo_name, strlen(mop->mo_name) + 1);

	vfs_freecancelopt(mop->mo_cancel);

	if (mop->mo_arg != NULL)
		kmem_free(mop->mo_arg, strlen(mop->mo_arg) + 1);
	dprintf("-vfs_freeopt\n");
}

/*
 * Free a mount options table
 *
 * This function is *not* for general use by filesystems.
 *
 * Note: caller is responsible for locking the vfs list, if needed,
 *       to protect mp.
 */
void
vfs_freeopttbl(mntopts_t *mp)
{
	dprintf("+vfs_freeopttbl\n");
	uint_t i, count;

	count = mp->mo_count;
	for (i = 0; i < count; i++) {
		vfs_freeopt(&mp->mo_list[i]);
	}
	if (count) {
		kmem_free(mp->mo_list, sizeof (mntopt_t) * count);
		mp->mo_count = 0;
		mp->mo_list = NULL;
	}
	dprintf("-vfs_freeopttbl\n");
}

/*
 * Find a vfssw entry given a file system type name.
 * Try to autoload the filesystem if it's not found.
 * If it's installed, return the vfssw locked to prevent unloading.
 */
struct vfssw *
vfs_getvfssw(const char *type)
{
	struct vfssw *vswp;
	const char *modname;

	//RLOCK_VFSSW();
	vswp = vfs_getvfsswbyname(type);
#if 0
	modname = vfs_to_modname(type);

	if (rootdir == NULL) {
		/*
		 * If we haven't yet loaded the root file system, then our
		 * _init won't be called until later. Allocate vfssw entry,
		 * because mod_installfs won't be called.
		 */
		if (vswp == NULL) {
			RUNLOCK_VFSSW();
			WLOCK_VFSSW();
			if ((vswp = vfs_getvfsswbyname(type)) == NULL) {
				if ((vswp = allocate_vfssw(type)) == NULL) {
					WUNLOCK_VFSSW();
					return (NULL);
				}
			}
			WUNLOCK_VFSSW();
			RLOCK_VFSSW();
		}
		if (!VFS_INSTALLED(vswp)) {
			RUNLOCK_VFSSW();
			(void) modloadonly("fs", modname);
		} else
			RUNLOCK_VFSSW();
		return (vswp);
	}

	/*
	 * Try to load the filesystem.  Before calling modload(), we drop
	 * our lock on the VFS switch table, and pick it up after the
	 * module is loaded.  However, there is a potential race:  the
	 * module could be unloaded after the call to modload() completes
	 * but before we pick up the lock and drive on.  Therefore,
	 * we keep reloading the module until we've loaded the module
	 * _and_ we have the lock on the VFS switch table.
	 */
	while (vswp == NULL || !VFS_INSTALLED(vswp)) {
		RUNLOCK_VFSSW();
		if (modload("fs", modname) == -1)
			return (NULL);
		RLOCK_VFSSW();
		if (vswp == NULL)
			if ((vswp = vfs_getvfsswbyname(type)) == NULL)
				break;
	}
	RUNLOCK_VFSSW();
#endif
	return (vswp);
}

/*
 * Find a vfssw entry given a file system type name.
 */
struct vfssw *
vfs_getvfsswbyname(const char *type)
{
	struct vfssw *vswp;

//	ASSERT(VFSSW_LOCKED());
	if (type == NULL || *type == '\0')
		return (NULL);

	for (vswp = &vfssw[1]; vswp < &vfssw[nfstype]; vswp++) {
		if (strcmp(type, vswp->vsw_name) == 0) {
			vfs_refvfssw(vswp);
			return (vswp);
		}
	}

	return (NULL);
}

/*
 * Reference a vfssw entry.
 */
void
vfs_refvfssw(struct vfssw *vswp)
{
	dprintf("vfs_refvfssw\n");
	//mutex_enter(&vswp->vsw_lock);
	//vswp->vsw_count++;
	//mutex_exit(&vswp->vsw_lock);
}

#ifdef __APPLE__

int  zfs_module_start(kmod_info_t *ki, void *data);
int  zfs_module_stop(kmod_info_t *ki, void *data);


// move these structs to _osx once wrappers are updated

/*
 * ZFS file system features.
 */
const vol_capabilities_attr_t zfs_capabilities = {
	{
		/* Format capabilities we support: */
		VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		VOL_CAP_FMT_SYMBOLICLINKS |
		VOL_CAP_FMT_HARDLINKS |
		VOL_CAP_FMT_SPARSE_FILES |
		VOL_CAP_FMT_CASE_SENSITIVE |
		VOL_CAP_FMT_CASE_PRESERVING |
		VOL_CAP_FMT_FAST_STATFS |
		VOL_CAP_FMT_2TB_FILESIZE |
		VOL_CAP_FMT_HIDDEN_FILES |
		VOL_CAP_FMT_PATH_FROM_ID,

		/* Interface capabilities we support: */
		VOL_CAP_INT_ATTRLIST |
		VOL_CAP_INT_NFSEXPORT |
		VOL_CAP_INT_READDIRATTR |
		VOL_CAP_INT_VOL_RENAME |
		VOL_CAP_INT_ADVLOCK |
		VOL_CAP_INT_FLOCK |
		VOL_CAP_INT_EXTENDED_SECURITY |
#if NAMEDSTREAMS
		VOL_CAP_INT_NAMEDSTREAMS |
#endif
		VOL_CAP_INT_EXTENDED_ATTR ,

		0 , 0
	},
	{
		/* Format capabilities we know about: */
		VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		VOL_CAP_FMT_SYMBOLICLINKS |
		VOL_CAP_FMT_HARDLINKS |
		VOL_CAP_FMT_JOURNAL |
		VOL_CAP_FMT_JOURNAL_ACTIVE |
		VOL_CAP_FMT_NO_ROOT_TIMES |
		VOL_CAP_FMT_SPARSE_FILES |
		VOL_CAP_FMT_ZERO_RUNS |
		VOL_CAP_FMT_CASE_SENSITIVE |
		VOL_CAP_FMT_CASE_PRESERVING |
		VOL_CAP_FMT_FAST_STATFS |
		VOL_CAP_FMT_2TB_FILESIZE |
		VOL_CAP_FMT_OPENDENYMODES |
		VOL_CAP_FMT_HIDDEN_FILES |
		VOL_CAP_FMT_PATH_FROM_ID ,

		/* Interface capabilities we know about: */
		VOL_CAP_INT_SEARCHFS |
		VOL_CAP_INT_ATTRLIST |
		VOL_CAP_INT_NFSEXPORT |
		VOL_CAP_INT_READDIRATTR |
		VOL_CAP_INT_EXCHANGEDATA |
		VOL_CAP_INT_COPYFILE |
		VOL_CAP_INT_ALLOCATE |
		VOL_CAP_INT_VOL_RENAME |
		VOL_CAP_INT_ADVLOCK |
		VOL_CAP_INT_FLOCK |
		VOL_CAP_INT_EXTENDED_SECURITY |
		VOL_CAP_INT_USERACCESS |
		VOL_CAP_INT_MANLOCK |
#if NAMEDSTREAMS
		VOL_CAP_INT_NAMEDSTREAMS |
#endif
		VOL_CAP_INT_EXTENDED_ATTR ,

		0, 0
	}
};




/*
 * ZFS file system attributes (for getattrlist).
 */
const attribute_set_t zfs_attributes = {
		ATTR_CMN_NAME	|
		ATTR_CMN_DEVID	|
		ATTR_CMN_FSID	|
		ATTR_CMN_OBJTYPE |
		ATTR_CMN_OBJTAG	|
		ATTR_CMN_OBJID	|
		ATTR_CMN_OBJPERMANENTID |
		ATTR_CMN_PAROBJID |
		ATTR_CMN_CRTIME |
		ATTR_CMN_MODTIME |
		ATTR_CMN_CHGTIME |
		ATTR_CMN_ACCTIME |
		ATTR_CMN_BKUPTIME |
		ATTR_CMN_FNDRINFO |
		ATTR_CMN_OWNERID |
		ATTR_CMN_GRPID	|
		ATTR_CMN_ACCESSMASK |
		ATTR_CMN_FLAGS	|
		ATTR_CMN_USERACCESS |
		ATTR_CMN_EXTENDED_SECURITY |
		ATTR_CMN_UUID |
		ATTR_CMN_GRPUUID ,

		ATTR_VOL_FSTYPE	|
		ATTR_VOL_SIGNATURE |
		ATTR_VOL_SIZE	|
		ATTR_VOL_SPACEFREE |
		ATTR_VOL_SPACEAVAIL |
		ATTR_VOL_MINALLOCATION |
		ATTR_VOL_ALLOCATIONCLUMP |
		ATTR_VOL_IOBLOCKSIZE |
		ATTR_VOL_OBJCOUNT |
		ATTR_VOL_FILECOUNT |
		ATTR_VOL_DIRCOUNT |
		ATTR_VOL_MAXOBJCOUNT |
		ATTR_VOL_MOUNTPOINT |
		ATTR_VOL_NAME	|
		ATTR_VOL_MOUNTFLAGS |
		ATTR_VOL_MOUNTEDDEVICE |
		ATTR_VOL_CAPABILITIES |
		ATTR_VOL_ATTRIBUTES ,

		ATTR_DIR_LINKCOUNT |
		ATTR_DIR_ENTRYCOUNT |
		ATTR_DIR_MOUNTSTATUS ,

		ATTR_FILE_LINKCOUNT |
		ATTR_FILE_TOTALSIZE |
		ATTR_FILE_ALLOCSIZE |
		/* ATTR_FILE_IOBLOCKSIZE */
		ATTR_FILE_DEVTYPE |
		ATTR_FILE_DATALENGTH |
		ATTR_FILE_DATAALLOCSIZE |
		ATTR_FILE_RSRCLENGTH |
		ATTR_FILE_RSRCALLOCSIZE ,

		0
};


/*
 * Mac OS X needs a file system modify time
 *
 * We use the mtime of the "com.apple.system.mtime"
 * extended attribute, which is associated with the
 * file system root directory.  This attribute has
 * no associated data.
 */
#define ZFS_MTIME_XATTR		"com.apple.system.mtime"

extern int zfs_obtain_xattr(znode_t *, const char *, mode_t, cred_t *, vnode_t **, int);


/*
 * We need to keep a count of active fs's.
 * This is necessary to prevent our kext
 * from being unloaded after a umount -f
 */
uint32_t	zfs_active_fs_count = 0;

extern void zfs_ioctl_init(void);
extern void zfs_ioctl_fini(void);

#endif


/*
 * Table for generic options recognized in the VFS layer and acted
 * on at this level before parsing file system specific options.
 * The nosuid option is stronger than any of the devices and setuid
 * options, so those are canceled when nosuid is seen.
 *
 * All options which are added here need to be added to the
 * list of standard options in usr/src/cmd/fs.d/fslib.c as well.
 */
/*
 * VFS Mount options table
 */
static char *ro_cancel[] = { MNTOPT_RW, NULL };
static char *rw_cancel[] = { MNTOPT_RO, NULL };
static char *suid_cancel[] = { MNTOPT_NOSUID, NULL };
static char *nosuid_cancel[] = { MNTOPT_SUID, MNTOPT_DEVICES, MNTOPT_NODEVICES,
    MNTOPT_NOSETUID, MNTOPT_SETUID, NULL };
static char *devices_cancel[] = { MNTOPT_NODEVICES, NULL };
static char *nodevices_cancel[] = { MNTOPT_DEVICES, NULL };
static char *setuid_cancel[] = { MNTOPT_NOSETUID, NULL };
static char *nosetuid_cancel[] = { MNTOPT_SETUID, NULL };
static char *nbmand_cancel[] = { MNTOPT_NONBMAND, NULL };
static char *nonbmand_cancel[] = { MNTOPT_NBMAND, NULL };
static char *exec_cancel[] = { MNTOPT_NOEXEC, NULL };
static char *noexec_cancel[] = { MNTOPT_EXEC, NULL };

static const mntopt_t mntopts[] = {
/*
 *	option name		cancel options		default arg	flags
 */
	{ MNTOPT_REMOUNT,	NULL,			NULL,
		MO_NODISPLAY, (void *)0 },
	{ MNTOPT_RO,		ro_cancel,		NULL,		0,
		(void *)0 },
	{ MNTOPT_RW,		rw_cancel,		NULL,		0,
		(void *)0 },
	{ MNTOPT_SUID,		suid_cancel,		NULL,		0,
		(void *)0 },
	{ MNTOPT_NOSUID,	nosuid_cancel,		NULL,		0,
		(void *)0 },
	{ MNTOPT_DEVICES,	devices_cancel,		NULL,		0,
		(void *)0 },
	{ MNTOPT_NODEVICES,	nodevices_cancel,	NULL,		0,
		(void *)0 },
	{ MNTOPT_SETUID,	setuid_cancel,		NULL,		0,
		(void *)0 },
	{ MNTOPT_NOSETUID,	nosetuid_cancel,	NULL,		0,
		(void *)0 },
	{ MNTOPT_NBMAND,	nbmand_cancel,		NULL,		0,
		(void *)0 },
	{ MNTOPT_NONBMAND,	nonbmand_cancel,	NULL,		0,
		(void *)0 },
	{ MNTOPT_EXEC,		exec_cancel,		NULL,		0,
		(void *)0 },
	{ MNTOPT_NOEXEC,	noexec_cancel,		NULL,		0,
		(void *)0 },
};

const mntopts_t vfs_mntopts = {
	sizeof (mntopts) / sizeof (mntopt_t),
	(mntopt_t *)&mntopts[0]
};

static char *noatime_cancel[] = { MNTOPT_ATIME, NULL };
static char *atime_cancel[] = { MNTOPT_NOATIME, NULL };
static char *noxattr_cancel[] = { MNTOPT_XATTR, NULL };
static char *xattr_cancel[] = { MNTOPT_NOXATTR, NULL };
#ifdef __APPLE__
static char *nobrowse_cancel[] = { MNTOPT_BROWSE, NULL };
static char *browse_cancel[] = { MNTOPT_NOBROWSE, NULL };
static char *noowners_cancel[] = { MNTOPT_OWNERS, NULL };
static char *owners_cancel[] = { MNTOPT_NOOWNERS, NULL };
#endif

static mntopt_t zmntopts[] = {
	{ MNTOPT_NOXATTR, noxattr_cancel, NULL, 0, NULL },
	{ MNTOPT_XATTR, xattr_cancel, NULL, 0, NULL },
	{ MNTOPT_NOATIME, noatime_cancel, NULL, 0, NULL },
	{ MNTOPT_ATIME, atime_cancel, NULL, 0, NULL },
#ifdef __APPLE__
	{ MNTOPT_NOBROWSE, nobrowse_cancel, NULL, 0, NULL },
	{ MNTOPT_BROWSE, browse_cancel, NULL, 0, NULL },
	{ MNTOPT_NOOWNERS, noowners_cancel, NULL, 0, NULL },
	{ MNTOPT_OWNERS, owners_cancel, NULL, 0, NULL }
#endif
};

static mntopts_t zfs_mntopts = {
	sizeof (zmntopts) / sizeof (mntopt_t),
	zmntopts
};

static vfsdef_t vfw = {
        VFSDEF_VERSION,
        MNTTYPE_ZFS,
        zfs_vfsinit,
        VSW_HASPROTO|VSW_CANRWRO|VSW_CANREMOUNT|VSW_VOLATILEDEV|VSW_STATS|
            VSW_XID|VSW_ZMOUNT,
        &zfs_mntopts
};

int
zfs_vfs_sync(struct mount *mp, __unused int waitfor, __unused vfs_context_t context)
{

    spa_sync_allpools();

	return (0);
}



#ifndef __APPLE__
static int
zfs_create_unique_device(dev_t *dev)

	major_t new_major;

	do {
		ASSERT3U(zfs_minor, <=, MAXMIN32);
		minor_t start = zfs_minor;
		do {
			mutex_enter(&zfs_dev_mtx);
			if (zfs_minor >= MAXMIN32) {
				/*
				 * If we're still using the real major
				 * keep out of /dev/zfs and /dev/zvol minor
				 * number space.  If we're using a getudev()'ed
				 * major number, we can use all of its minors.
				 */
				if (zfs_major == ddi_name_to_major(ZFS_DRIVER))
					zfs_minor = ZFS_MIN_MINOR;
				else
					zfs_minor = 0;
			} else {
				zfs_minor++;
			}
			*dev = makedevice(zfs_major, zfs_minor);
			mutex_exit(&zfs_dev_mtx);
		} while (vfs_devismounted(*dev) && zfs_minor != start);
		if (zfs_minor == start) {
			/*
			 * We are using all ~262,000 minor numbers for the
			 * current major number.  Create a new major number.
			 */
			if ((new_major = getudev()) == (major_t)-1) {
				cmn_err(CE_WARN,
				    "zfs_mount: Can't get unique major "
				    "device number.");
				return (-1);
			}
			mutex_enter(&zfs_dev_mtx);
			zfs_major = new_major;
			zfs_minor = 0;

			mutex_exit(&zfs_dev_mtx);
		} else {
			break;
		}
		/* CONSTANTCONDITION */
	} while (1);

	return (0);
}
#endif	/* !__FreeBSD__ */

static void
atime_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+atime_changed_cb\n");
	zfsvfs_t *zfsvfs = arg;

	if (newval == TRUE) {
		zfsvfs->z_atime = TRUE;
		vfs_clearflags(zfsvfs->z_vfs, (uint64_t)MNT_NOATIME);
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_NOATIME);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_ATIME, NULL, 0);
	} else {
		zfsvfs->z_atime = FALSE;
		vfs_setflags(zfsvfs->z_vfs, (uint64_t)MNT_NOATIME);
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_ATIME);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_NOATIME, NULL, 0);
	}
	dprintf("-atime_changed_cb\n");
}

static void
xattr_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+xattr_changed_cb\n");
	zfsvfs_t *zfsvfs = arg;
	
	if (newval == TRUE) {
		/* XXX locking on vfs_flag? */
		vfs_clearflags(zfsvfs->z_vfs, (uint64_t)MNT_NOUSERXATTR);
		vfs_clearmntopt(zfsvfs, MNTOPT_NOXATTR);
		vfs_setmntopt(zfsvfs, MNTOPT_XATTR, NULL, 0);
	} else {
		/* XXX locking on vfs_flag? */
		vfs_setflags(zfsvfs->z_vfs, (uint64_t)MNT_NOUSERXATTR);
		vfs_clearmntopt(zfsvfs, MNTOPT_XATTR);
		vfs_setmntopt(zfsvfs, MNTOPT_NOXATTR, NULL, 0);
	}
	dprintf("-xattr_changed_cb\n");
}

static void
blksz_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	if (newval < SPA_MINBLOCKSIZE ||
	    newval > SPA_MAXBLOCKSIZE || !ISP2(newval))
		newval = SPA_MAXBLOCKSIZE;

	zfsvfs->z_max_blksz = newval;
	//zfsvfs->z_vfs->mnt_stat.f_iosize = newval;
	//zfsvfs->z_vfs->vfs_bsize = newval;
}

static void
readonly_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+readonly_changed_cb\n");
	zfsvfs_t *zfsvfs = arg;
    
	if (newval) {
		/* XXX locking on vfs_flag? */
		vfs_setflags(zfsvfs->z_vfs, (uint64_t)MNT_RDONLY);
		vfs_clearmntopt(zfsvfs, MNTOPT_RW);
		vfs_setmntopt(zfsvfs, MNTOPT_RO, NULL, 0);
	} else {
		/* XXX locking on vfs_flag? */
		vfs_clearflags(zfsvfs->z_vfs, (uint64_t)MNT_RDONLY);
		vfs_clearmntopt(zfsvfs, MNTOPT_RO);
		vfs_setmntopt(zfsvfs, MNTOPT_RW, NULL, 0);
	}
	dprintf("-readonly_changed_cb\n");
}

#if 0
void
readonly_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;
	if (newval == TRUE) {
		/* XXX locking on vfs_flag? */

        // We need to release the mtime_vp when readonly, as it will not
        // call VNOP_SYNC in RDONLY.

#if 0
        if (zfsvfs->z_mtime_vp) {
            vnode_rele(zfsvfs->z_mtime_vp);
            vnode_recycle(zfsvfs->z_mtime_vp);
            zfsvfs->z_mtime_vp = NULL;
        }
#endif
        // Flush any writes
        //vflush(mp, NULLVP, SKIPSYSTEM);

        vfs_setflags(zfsvfs->z_vfs, (uint64_t)MNT_RDONLY);
	} else {
        // FIXME, we don't re-open mtime_vp here.
        vfs_clearflags(zfsvfs->z_vfs, (uint64_t)MNT_RDONLY);
	}
}
#endif

static void
devices_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+devices_changed_cb\n");
	zfsvfs_t *zfsvfs = arg;
    
	if (newval == FALSE) {
		vfs_setflags(zfsvfs->z_vfs, (uint64_t)MNT_NODEV);
		vfs_clearmntopt(zfsvfs, MNTOPT_DEVICES);
		vfs_setmntopt(zfsvfs, MNTOPT_NODEVICES, NULL, 0);
	} else {
		vfs_clearflags(zfsvfs->z_vfs, (uint64_t)MNT_NODEV);
		vfs_clearmntopt(zfsvfs, MNTOPT_NODEVICES);
		vfs_setmntopt(zfsvfs, MNTOPT_DEVICES, NULL, 0);
	}
	dprintf("-devices_changed_cb\n");
}

static void
setuid_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+setuid_changed_cb\n");
	zfsvfs_t *zfsvfs = arg;
    
	if (newval == FALSE) {
		vfs_setflags(zfsvfs->z_vfs, (uint64_t)MNT_NOSUID);
		vfs_clearmntopt(zfsvfs, MNTOPT_SETUID);
		vfs_setmntopt(zfsvfs, MNTOPT_NOSETUID, NULL, 0);
	} else {
		vfs_clearflags(zfsvfs->z_vfs, (uint64_t)MNT_NOSUID);
		vfs_clearmntopt(zfsvfs, MNTOPT_NOSETUID);
		vfs_setmntopt(zfsvfs, MNTOPT_SETUID, NULL, 0);
	}
	dprintf("-setuid_changed_cb\n");
}

static void
exec_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+exec_changed_cb\n");
	zfsvfs_t *zfsvfs = arg;
    
	if (newval == FALSE) {
		vfs_setflags(zfsvfs->z_vfs, (uint64_t)MNT_NOEXEC);
		vfs_clearmntopt(zfsvfs, MNTOPT_EXEC);
		vfs_setmntopt(zfsvfs, MNTOPT_NOEXEC, NULL, 0);
	} else {
		vfs_clearflags(zfsvfs->z_vfs, (uint64_t)MNT_NOEXEC);
		vfs_clearmntopt(zfsvfs, MNTOPT_NOEXEC);
		vfs_setmntopt(zfsvfs, MNTOPT_EXEC, NULL, 0);
	}
	dprintf("-exec_changed_cb\n");
}

/*
 * The nbmand mount option can be changed at mount time.
 * We can't allow it to be toggled on live file systems or incorrect
 * behavior may be seen from cifs clients
 *
 * This property isn't registered via dsl_prop_register(), but this callback
 * will be called when a file system is first mounted
 */
static void
nbmand_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+nbmand_changed_cb\n");
	zfsvfs_t *zfsvfs = arg;
	if (newval == FALSE) {
		zfsvfs->z_nbmand = FALSE;
		vfs_clearmntopt(zfsvfs, MNTOPT_NBMAND);
		vfs_setmntopt(zfsvfs, MNTOPT_NONBMAND, NULL, 0);
	} else {
		zfsvfs->z_nbmand = TRUE;
		vfs_clearmntopt(zfsvfs, MNTOPT_NONBMAND);
		vfs_setmntopt(zfsvfs, MNTOPT_NBMAND, NULL, 0);
	}
	dprintf("-nbmand_changed_cb\n");
}


static void
snapdir_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+snapdir_changed_cb\n");
	zfsvfs_t *zfsvfs = arg;
    
	zfsvfs->z_show_ctldir = newval;
	dprintf("-snapdir_changed_cb\n");
}

static void
vscan_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+vscan_changed_cb\n");
	//zfsvfs_t *zfsvfs = arg;
    
	//zfsvfs->z_vscan = newval;
	dprintf("-vscan_changed_cb\n");
}

static void
acl_mode_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+acl_mode_changed_cb\n");
	zfsvfs_t *zfsvfs = arg;
    
	zfsvfs->z_acl_mode = newval;
	dprintf("-acl_mode_changed_cb\n");
}

static void
acl_inherit_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+acl_inherit_changed_cb\n");
	zfsvfs_t *zfsvfs = arg;
    
	zfsvfs->z_acl_inherit = newval;
	dprintf("-acl_inherit_changed_cb\n");
}

#ifdef __APPLE__
static void
finderbrowse_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+finderbrowse_changed_cb\n");
	zfsvfs_t *zfsvfs = arg;
	
	if (newval == TRUE) {
		/* XXX locking on vfs_flag? */
		vfs_clearflags(zfsvfs->z_vfs, (uint64_t)MNT_DONTBROWSE);
		vfs_clearmntopt(zfsvfs, MNTOPT_NOBROWSE);
		vfs_setmntopt(zfsvfs, MNTOPT_BROWSE, NULL, 0);
	} else {
		/* XXX locking on vfs_flag? */
		vfs_setflags(zfsvfs->z_vfs, (uint64_t)MNT_DONTBROWSE);
		vfs_clearmntopt(zfsvfs, MNTOPT_BROWSE);
		vfs_setmntopt(zfsvfs, MNTOPT_NOBROWSE, NULL, 0);
	}
	dprintf("-finderbrowse_changed_cb\n");
}

static void
ignoreowner_changed_cb(void *arg, uint64_t newval)
{
	dprintf("+ignoreowner_changed_cb\n");
	zfsvfs_t *zfsvfs = arg;
    
	if (newval) {
		/* XXX locking on vfs_flag? */
		vfs_setflags(zfsvfs->z_vfs, (uint64_t)MNT_IGNORE_OWNERSHIP);
		vfs_clearmntopt(zfsvfs, MNTOPT_OWNERS);
		vfs_setmntopt(zfsvfs, MNTOPT_NOOWNERS, NULL, 0);
	} else {
		/* XXX locking on vfs_flag? */
		vfs_clearflags(zfsvfs->z_vfs, (uint64_t)MNT_IGNORE_OWNERSHIP);
		vfs_clearmntopt(zfsvfs, MNTOPT_NOOWNERS);
		vfs_setmntopt(zfsvfs, MNTOPT_OWNERS, NULL, 0);
	}
	dprintf("-ignoreowner_changed_cb\n");
}
#endif

static int
zfs_register_callbacks(struct mount *vfsp)
{
	struct dsl_dataset *ds = NULL;

	objset_t *os = NULL;
	zfsvfs_t *zfsvfs = NULL;
	uint64_t nbmand = 0;
	boolean_t readonly = B_FALSE;
	boolean_t do_readonly = B_FALSE;
	boolean_t setuid = B_FALSE;
	boolean_t do_setuid = B_FALSE;
	boolean_t exec = B_FALSE;
	boolean_t do_exec = B_FALSE;
	boolean_t devices = B_FALSE;
	boolean_t do_devices = B_FALSE;
	boolean_t xattr = B_FALSE;
	boolean_t do_xattr = B_FALSE;
	boolean_t atime = B_FALSE;
	boolean_t do_atime = B_FALSE;
#ifdef __APPLE__
	boolean_t finderbrowse = B_FALSE;
	boolean_t do_finderbrowse = B_FALSE;
	boolean_t ignoreowner = B_FALSE;
	boolean_t do_ignoreowner = B_FALSE;
#endif
	int error = 0;

	ASSERT(vfsp);
    zfsvfs = vfs_fsprivate(vfsp);
	ASSERT(zfsvfs);
	os = zfsvfs->z_os;

	/*
	 * This function can be called for a snapshot when we update snapshot's
	 * mount point, which isn't really supported.
	 */
	if (dmu_objset_is_snapshot(os))
		return (EOPNOTSUPP);

	/*
	 * The act of registering our callbacks will destroy any mount
	 * options we may have.  In order to enable temporary overrides
	 * of mount options, we stash away the current values and
	 * restore them after we register the callbacks.
	 */

	if (vfs_optionisset(zfsvfs, MNTOPT_RO, NULL) ||
	    !spa_writeable(dmu_objset_spa(os))) {
		readonly = B_TRUE;
		do_readonly = B_TRUE;
	} else if (vfs_optionisset(zfsvfs, MNTOPT_RW, NULL)) {
		readonly = B_FALSE;
		do_readonly = B_TRUE;
	}
	if (vfs_optionisset(zfsvfs, MNTOPT_NOSUID, NULL)) {
		devices = B_FALSE;
		setuid = B_FALSE;
		do_devices = B_TRUE;
		do_setuid = B_TRUE;
	} else {
		if (vfs_optionisset(zfsvfs, MNTOPT_NODEVICES, NULL)) {
			devices = B_FALSE;
			do_devices = B_TRUE;
		} else if (vfs_optionisset(zfsvfs, MNTOPT_DEVICES, NULL)) {
			devices = B_TRUE;
			do_devices = B_TRUE;
		}
        
		if (vfs_optionisset(zfsvfs, MNTOPT_NOSETUID, NULL)) {
			setuid = B_FALSE;
			do_setuid = B_TRUE;
		} else if (vfs_optionisset(zfsvfs, MNTOPT_SETUID, NULL)) {
			setuid = B_TRUE;
			do_setuid = B_TRUE;
		}
	}
	if (vfs_optionisset(zfsvfs, MNTOPT_NOEXEC, NULL)) {
		exec = B_FALSE;
		do_exec = B_TRUE;
	} else if (vfs_optionisset(zfsvfs, MNTOPT_EXEC, NULL)) {
		exec = B_TRUE;
		do_exec = B_TRUE;
	}
	if (vfs_optionisset(zfsvfs, MNTOPT_NOXATTR, NULL)) {
		xattr = B_FALSE;
		do_xattr = B_TRUE;
	} else if (vfs_optionisset(zfsvfs, MNTOPT_XATTR, NULL)) {
		xattr = B_TRUE;
		do_xattr = B_TRUE;
	}
	if (vfs_optionisset(zfsvfs, MNTOPT_NOATIME, NULL)) {
		atime = B_FALSE;
		do_atime = B_TRUE;
	} else if (vfs_optionisset(zfsvfs, MNTOPT_ATIME, NULL)) {
		atime = B_TRUE;
		do_atime = B_TRUE;
	}
#ifdef __APPLE__
    if (vfs_optionisset(zfsvfs, MNTOPT_NOBROWSE, NULL)) {
		finderbrowse = B_FALSE;
		do_finderbrowse = B_TRUE;
	} else if (vfs_optionisset(zfsvfs, MNTOPT_BROWSE, NULL)) {
		finderbrowse = B_TRUE;
		do_finderbrowse = B_TRUE;
	}
	if (vfs_optionisset(zfsvfs, MNTOPT_NOOWNERS, NULL)) {
		ignoreowner = B_TRUE;
		do_ignoreowner = B_TRUE;
	} else if (vfs_optionisset(zfsvfs, MNTOPT_OWNERS, NULL)){
		ignoreowner = B_FALSE;
		do_ignoreowner = B_TRUE;
	}
#endif


#if 0
#define vfs_optionisset_old(X, Y, Z) (vfs_flags(X)&(Y))

	if (vfs_optionisset(vfsp, MNT_RDONLY, NULL) ||
		!spa_writeable(dmu_objset_spa(os))) {
		readonly = B_TRUE;
		do_readonly = B_TRUE;
	} else {
		readonly = B_FALSE;
		do_readonly = B_TRUE;
	}
	if (vfs_optionisset(vfsp, MNT_NOSUID, NULL)) {
		setuid = B_FALSE;
		do_setuid = B_TRUE;
	} else {
		setuid = B_TRUE;
		do_setuid = B_TRUE;
	}
	if (vfs_optionisset(vfsp, MNT_NOEXEC, NULL)) {
		exec = B_FALSE;
		do_exec = B_TRUE;
	} else {
		exec = B_TRUE;
		do_exec = B_TRUE;
	}
	if (vfs_optionisset(vfsp, MNT_NOUSERXATTR, NULL)) {
		xattr = B_FALSE;
		do_xattr = B_TRUE;
	} else {
		xattr = B_TRUE;
		do_xattr = B_TRUE;
	}
	if (vfs_optionisset(zfsvfs, MNTOPT_NOATIME, NULL)) {
		atime = B_FALSE;
		do_atime = B_TRUE;
	} else (vfs_optionisset(zfsvfs, MNTOPT_ATIME, NULL)) {
		atime = B_TRUE;
		do_atime = B_TRUE;
	}
	if (vfs_optionisset(vfsp, MNT_DONTBROWSE, NULL)) {
		finderbrowse = B_FALSE;
		do_finderbrowse = B_TRUE;
	} else {
		finderbrowse = B_TRUE;
		do_finderbrowse = B_TRUE;
	}
	if (vfs_optionisset(vfsp, MNT_IGNORE_OWNERSHIP, NULL)) {
		ignoreowner = B_TRUE;
		do_ignoreowner = B_TRUE;
	} else {
		ignoreowner = B_FALSE;
		do_ignoreowner = B_TRUE;
	}
    
#endif

	/*
	 * nbmand is a special property.  It can only be changed at
	 * mount time.
	 *
	 * This is weird, but it is documented to only be changeable
	 * at mount time.
	 */
#ifdef __LINUX__
	if (vfs_optionisset(vfsp, MNTOPT_NONBMAND, NULL)) {
		nbmand = B_FALSE;
	} else if (vfs_optionisset(vfsp, MNTOPT_NBMAND, NULL)) {
		nbmand = B_TRUE;
	} else {
		char osname[MAXNAMELEN];

		dmu_objset_name(os, osname);
		if (error = dsl_prop_get_integer(osname, "nbmand", &nbmand,
		    NULL)) {
			return (error);
		}
	}
#endif

	/*
	 * Register property callbacks.
	 *
	 * It would probably be fine to just check for i/o error from
	 * the first prop_register(), but I guess I like to go
	 * overboard...
	 */
	ds = dmu_objset_ds(os);
	dsl_pool_config_enter(dmu_objset_pool(os), FTAG);
	error = dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_ATIME), atime_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_XATTR), xattr_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_RECORDSIZE), blksz_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_READONLY), readonly_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_DEVICES), devices_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_SETUID), setuid_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_EXEC), exec_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_SNAPDIR), snapdir_changed_cb, zfsvfs);
    // This appears to be PROP_PRIVATE, investigate if we want this
	//error = error ? error : dsl_prop_register(ds,
    //   zfs_prop_to_name(ZFS_PROP_ACLMODE), acl_mode_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_ACLINHERIT), acl_inherit_changed_cb,
	    zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_VSCAN), vscan_changed_cb, zfsvfs);
#ifdef __APPLE__
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_APPLE_BROWSE), finderbrowse_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    zfs_prop_to_name(ZFS_PROP_APPLE_IGNOREOWNER), ignoreowner_changed_cb, zfsvfs);
#endif
	dsl_pool_config_exit(dmu_objset_pool(os), FTAG);
	if (error)
		goto unregister;

    
	/*
	 * Invoke our callbacks to restore temporary mount options.
	 */
	if (do_readonly)
		readonly_changed_cb(zfsvfs, readonly);
	if (do_setuid)
		setuid_changed_cb(zfsvfs, setuid);
	if (do_exec)
		exec_changed_cb(zfsvfs, exec);
	if (do_devices)
		devices_changed_cb(zfsvfs, devices);
	if (do_xattr)
		xattr_changed_cb(zfsvfs, xattr);
	if (do_atime)
		atime_changed_cb(zfsvfs, atime);
#ifdef __APPLE__
	if (do_finderbrowse)
		finderbrowse_changed_cb(zfsvfs, finderbrowse);
	if (do_ignoreowner)
		ignoreowner_changed_cb(zfsvfs, ignoreowner);
#endif

	nbmand_changed_cb(zfsvfs, nbmand);

	return (0);

unregister:
	printf("why are we here?\n");
	/*
	 * We may attempt to unregister some callbacks that are not
	 * registered, but this is OK; it will simply return ENOMSG,
	 * which we will ignore.
	 */
	(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_ATIME),
	    atime_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_XATTR),
	    xattr_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_RECORDSIZE),
	    blksz_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_READONLY),
	    readonly_changed_cb, zfsvfs);
//#ifdef illumos
	(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_DEVICES),
	    devices_changed_cb, zfsvfs);
//#endif
	(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_SETUID),
	    setuid_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_EXEC),
	    exec_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_SNAPDIR),
	    snapdir_changed_cb, zfsvfs);
	//(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_ACLMODE),
    //   acl_mode_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_ACLINHERIT),
	    acl_inherit_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_VSCAN),
	    vscan_changed_cb, zfsvfs);
#ifdef __APPLE__
	(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_APPLE_BROWSE),
	    finderbrowse_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, zfs_prop_to_name(ZFS_PROP_APPLE_IGNOREOWNER),
	    ignoreowner_changed_cb, zfsvfs);
#endif
	return (error);
}

static int
zfs_space_delta_cb(dmu_object_type_t bonustype, void *data,
    uint64_t *userp, uint64_t *groupp)
{
	//int error = 0;

	/*
	 * Is it a valid type of object to track?
	 */
	if (bonustype != DMU_OT_ZNODE && bonustype != DMU_OT_SA)
		return (ENOENT);

	/*
	 * If we have a NULL data pointer
	 * then assume the id's aren't changing and
	 * return EEXIST to the dmu to let it know to
	 * use the same ids
	 */
	if (data == NULL)
		return (EEXIST);

	if (bonustype == DMU_OT_ZNODE) {
		znode_phys_t *znp = data;
		*userp = znp->zp_uid;
		*groupp = znp->zp_gid;
	} else {
#if 0
		int hdrsize;
		sa_hdr_phys_t *sap = data;
		sa_hdr_phys_t sa = *sap;
		boolean_t swap = B_FALSE;

		ASSERT(bonustype == DMU_OT_SA);

		if (sa.sa_magic == 0) {
			/*
			 * This should only happen for newly created
			 * files that haven't had the znode data filled
			 * in yet.
			 */
			*userp = 0;
			*groupp = 0;
			return (0);
		}
		if (sa.sa_magic == BSWAP_32(SA_MAGIC)) {
			sa.sa_magic = SA_MAGIC;
			sa.sa_layout_info = BSWAP_16(sa.sa_layout_info);
			swap = B_TRUE;
		} else {
			VERIFY3U(sa.sa_magic, ==, SA_MAGIC);
		}

		hdrsize = sa_hdrsize(&sa);
		VERIFY3U(hdrsize, >=, sizeof (sa_hdr_phys_t));
		*userp = *((uint64_t *)((uintptr_t)data + hdrsize +
		    SA_UID_OFFSET));
		*groupp = *((uint64_t *)((uintptr_t)data + hdrsize +
		    SA_GID_OFFSET));
		if (swap) {
			*userp = BSWAP_64(*userp);
			*groupp = BSWAP_64(*groupp);
		}
#endif
	}
	return (0);
}

static void
fuidstr_to_sid(zfsvfs_t *zfsvfs, const char *fuidstr,
    char *domainbuf, int buflen, uid_t *ridp)
{
	uint64_t fuid;
	const char *domain;

	fuid = strtonum(fuidstr, NULL);

	domain = zfs_fuid_find_by_idx(zfsvfs, FUID_INDEX(fuid));
	if (domain)
		(void) strlcpy(domainbuf, domain, buflen);
	else
		domainbuf[0] = '\0';
	*ridp = FUID_RID(fuid);
}

static uint64_t
zfs_userquota_prop_to_obj(zfsvfs_t *zfsvfs, zfs_userquota_prop_t type)
{
	switch (type) {
	case ZFS_PROP_USERUSED:
		return (DMU_USERUSED_OBJECT);
	case ZFS_PROP_GROUPUSED:
		return (DMU_GROUPUSED_OBJECT);
	case ZFS_PROP_USERQUOTA:
		return (zfsvfs->z_userquota_obj);
	case ZFS_PROP_GROUPQUOTA:
		return (zfsvfs->z_groupquota_obj);
    default:
        break;
	}
	return (0);
}

int
zfs_userspace_many(zfsvfs_t *zfsvfs, zfs_userquota_prop_t type,
    uint64_t *cookiep, void *vbuf, uint64_t *bufsizep)
{
	int error;
	zap_cursor_t zc;
	zap_attribute_t za;
	zfs_useracct_t *buf = vbuf;
	uint64_t obj;

	if (!dmu_objset_userspace_present(zfsvfs->z_os))
		return (ENOTSUP);

	obj = zfs_userquota_prop_to_obj(zfsvfs, type);
	if (obj == 0) {
		*bufsizep = 0;
		return (0);
	}

	for (zap_cursor_init_serialized(&zc, zfsvfs->z_os, obj, *cookiep);
	    (error = zap_cursor_retrieve(&zc, &za)) == 0;
	    zap_cursor_advance(&zc)) {
		if ((uintptr_t)buf - (uintptr_t)vbuf + sizeof (zfs_useracct_t) >
		    *bufsizep)
			break;

		fuidstr_to_sid(zfsvfs, za.za_name,
		    buf->zu_domain, sizeof (buf->zu_domain), &buf->zu_rid);

		buf->zu_space = za.za_first_integer;
		buf++;
	}
	if (error == ENOENT)
		error = 0;

	ASSERT3U((uintptr_t)buf - (uintptr_t)vbuf, <=, *bufsizep);
	*bufsizep = (uintptr_t)buf - (uintptr_t)vbuf;
	*cookiep = zap_cursor_serialize(&zc);
	zap_cursor_fini(&zc);
	return (error);
}

/*
 * buf must be big enough (eg, 32 bytes)
 */
static int
id_to_fuidstr(zfsvfs_t *zfsvfs, const char *domain, uid_t rid,
    char *buf, boolean_t addok)
{
	uint64_t fuid;
	int domainid = 0;

	if (domain && domain[0]) {
		domainid = zfs_fuid_find_by_domain(zfsvfs, domain, NULL, addok);
		if (domainid == -1)
			return (ENOENT);
	}
	fuid = FUID_ENCODE(domainid, rid);
	(void) snprintf(buf, 32, "%llx", (longlong_t)fuid);
	return (0);
}

int
zfs_userspace_one(zfsvfs_t *zfsvfs, zfs_userquota_prop_t type,
    const char *domain, uint64_t rid, uint64_t *valp)
{
	char buf[32];
	int err;
	uint64_t obj;

	*valp = 0;

	if (!dmu_objset_userspace_present(zfsvfs->z_os))
		return (ENOTSUP);

	obj = zfs_userquota_prop_to_obj(zfsvfs, type);
	if (obj == 0)
		return (0);

	err = id_to_fuidstr(zfsvfs, domain, rid, buf, B_FALSE);
	if (err)
		return (err);

	err = zap_lookup(zfsvfs->z_os, obj, buf, 8, 1, valp);
	if (err == ENOENT)
		err = 0;
	return (err);
}

int
zfs_set_userquota(zfsvfs_t *zfsvfs, zfs_userquota_prop_t type,
    const char *domain, uint64_t rid, uint64_t quota)
{
	char buf[32];
	int err;
	dmu_tx_t *tx;
	uint64_t *objp;
	boolean_t fuid_dirtied;

	if (type != ZFS_PROP_USERQUOTA && type != ZFS_PROP_GROUPQUOTA)
		return (EINVAL);

	if (zfsvfs->z_version < ZPL_VERSION_USERSPACE)
		return (ENOTSUP);

	objp = (type == ZFS_PROP_USERQUOTA) ? &zfsvfs->z_userquota_obj :
	    &zfsvfs->z_groupquota_obj;

	err = id_to_fuidstr(zfsvfs, domain, rid, buf, B_TRUE);
	if (err)
		return (err);
	fuid_dirtied = zfsvfs->z_fuid_dirty;

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_zap(tx, *objp ? *objp : DMU_NEW_OBJECT, B_TRUE, NULL);
	if (*objp == 0) {
		dmu_tx_hold_zap(tx, MASTER_NODE_OBJ, B_TRUE,
		    zfs_userquota_prop_prefixes[type]);
	}
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	err = dmu_tx_assign(tx, TXG_WAIT);
	if (err) {
		dmu_tx_abort(tx);
		return (err);
	}

	mutex_enter(&zfsvfs->z_lock);
	if (*objp == 0) {
		*objp = zap_create(zfsvfs->z_os, DMU_OT_USERGROUP_QUOTA,
		    DMU_OT_NONE, 0, tx);
		VERIFY(0 == zap_add(zfsvfs->z_os, MASTER_NODE_OBJ,
		    zfs_userquota_prop_prefixes[type], 8, 1, objp, tx));
	}
	mutex_exit(&zfsvfs->z_lock);

	if (quota == 0) {
		err = zap_remove(zfsvfs->z_os, *objp, buf, tx);
		if (err == ENOENT)
			err = 0;
	} else {
		err = zap_update(zfsvfs->z_os, *objp, buf, 8, 1, &quota, tx);
	}
	ASSERT(err == 0);
	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);
	dmu_tx_commit(tx);
	return (err);
}

boolean_t
zfs_fuid_overquota(zfsvfs_t *zfsvfs, boolean_t isgroup, uint64_t fuid)
{
	char buf[32];
	uint64_t used, quota, usedobj, quotaobj;
	int err;

	usedobj = isgroup ? DMU_GROUPUSED_OBJECT : DMU_USERUSED_OBJECT;
	quotaobj = isgroup ? zfsvfs->z_groupquota_obj : zfsvfs->z_userquota_obj;

	if (quotaobj == 0 || zfsvfs->z_replay)
		return (B_FALSE);

	(void) snprintf(buf, sizeof(buf), "%llx", (longlong_t)fuid);
	err = zap_lookup(zfsvfs->z_os, quotaobj, buf, 8, 1, &quota);
	if (err != 0)
		return (B_FALSE);

	err = zap_lookup(zfsvfs->z_os, usedobj, buf, 8, 1, &used);
	if (err != 0)
		return (B_FALSE);
	return (used >= quota);
}

boolean_t
zfs_owner_overquota(zfsvfs_t *zfsvfs, znode_t *zp, boolean_t isgroup)
{
	uint64_t fuid;
	uint64_t quotaobj;

	quotaobj = isgroup ? zfsvfs->z_groupquota_obj : zfsvfs->z_userquota_obj;

	fuid = isgroup ? zp->z_gid : zp->z_uid;

	if (quotaobj == 0 || zfsvfs->z_replay)
		return (B_FALSE);

	return (zfs_fuid_overquota(zfsvfs, isgroup, fuid));
}

int
zfsvfs_create(const char *osname, zfsvfs_t **zfvp)
{
	objset_t *os;
	zfsvfs_t *zfsvfs;
	uint64_t zval;
	int i, error;
	uint64_t sa_obj;

	zfsvfs = kmem_zalloc(sizeof (zfsvfs_t), KM_SLEEP);

	/*
	 * We claim to always be readonly so we can open snapshots;
	 * other ZPL code will prevent us from writing to snapshots.
	 */
	error = dmu_objset_own(osname, DMU_OST_ZFS, B_TRUE, zfsvfs, &os);
	if (error) {
		kmem_free(zfsvfs, sizeof (zfsvfs_t));
		return (error);
	}

	/*
	 * Initialize the zfs-specific filesystem structure.
	 * Should probably make this a kmem cache, shuffle fields,
	 * and just bzero up to z_hold_mtx[].
	 */
	zfsvfs->z_vfs = NULL;
	zfsvfs->z_parent = zfsvfs;
	zfsvfs->z_max_blksz = SPA_MAXBLOCKSIZE;
	zfsvfs->z_show_ctldir = ZFS_SNAPDIR_VISIBLE;
	zfsvfs->z_os = os;

	error = zfs_get_zplprop(os, ZFS_PROP_VERSION, &zfsvfs->z_version);
	if (error) {
		goto out;
	} else if (zfsvfs->z_version >
	    zfs_zpl_version_map(spa_version(dmu_objset_spa(os)))) {
		(void) printf("Can't mount a version %lld file system "
		    "on a version %lld pool\n. Pool must be upgraded to mount "
		    "this file system.", (u_longlong_t)zfsvfs->z_version,
		    (u_longlong_t)spa_version(dmu_objset_spa(os)));
		error = ENOTSUP;
		goto out;
	}
	if ((error = zfs_get_zplprop(os, ZFS_PROP_NORMALIZE, &zval)) != 0)
		goto out;
	zfsvfs->z_norm = (int)zval;

	if ((error = zfs_get_zplprop(os, ZFS_PROP_UTF8ONLY, &zval)) != 0)
		goto out;
	zfsvfs->z_utf8 = (zval != 0);

	if ((error = zfs_get_zplprop(os, ZFS_PROP_CASE, &zval)) != 0)
		goto out;
	zfsvfs->z_case = (uint_t)zval;

	/*
	 * Fold case on file systems that are always or sometimes case
	 * insensitive.
	 */
	if (zfsvfs->z_case == ZFS_CASE_INSENSITIVE ||
	    zfsvfs->z_case == ZFS_CASE_MIXED)
		zfsvfs->z_norm |= U8_TEXTPREP_TOUPPER;

	zfsvfs->z_use_fuids = USE_FUIDS(zfsvfs->z_version, zfsvfs->z_os);
	zfsvfs->z_use_sa = USE_SA(zfsvfs->z_version, zfsvfs->z_os);

	if (zfsvfs->z_use_sa) {
		/* should either have both of these objects or none */
		error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_SA_ATTRS, 8, 1,
		    &sa_obj);
		if (error)
			return (error);
	} else {
		/*
		 * Pre SA versions file systems should never touch
		 * either the attribute registration or layout objects.
		 */
		sa_obj = 0;
	}

	error = sa_setup(os, sa_obj, zfs_attr_table, ZPL_END,
	    &zfsvfs->z_attr_table);
	if (error)
		goto out;

	if (zfsvfs->z_version >= ZPL_VERSION_SA)
		sa_register_update_callback(os, zfs_sa_upgrade);

	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_ROOT_OBJ, 8, 1,
	    &zfsvfs->z_root);
	if (error)
		goto out;
	ASSERT(zfsvfs->z_root != 0);

	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_UNLINKED_SET, 8, 1,
	    &zfsvfs->z_unlinkedobj);
	if (error)
		goto out;

	error = zap_lookup(os, MASTER_NODE_OBJ,
	    zfs_userquota_prop_prefixes[ZFS_PROP_USERQUOTA],
	    8, 1, &zfsvfs->z_userquota_obj);
	if (error && error != ENOENT)
		goto out;

	error = zap_lookup(os, MASTER_NODE_OBJ,
	    zfs_userquota_prop_prefixes[ZFS_PROP_GROUPQUOTA],
	    8, 1, &zfsvfs->z_groupquota_obj);
	if (error && error != ENOENT)
		goto out;

	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_FUID_TABLES, 8, 1,
	    &zfsvfs->z_fuid_obj);
	if (error && error != ENOENT)
		goto out;

	error = zap_lookup(os, MASTER_NODE_OBJ, ZFS_SHARES_DIR, 8, 1,
	    &zfsvfs->z_shares_dir);
	if (error && error != ENOENT)
		goto out;

	mutex_init(&zfsvfs->z_znodes_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&zfsvfs->z_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&zfsvfs->z_vnode_create_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&zfsvfs->z_reclaim_thr_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&zfsvfs->z_reclaim_thr_cv, NULL, CV_DEFAULT, NULL);
	list_create(&zfsvfs->z_all_znodes, sizeof (znode_t),
	    offsetof(znode_t, z_link_node));
	list_create(&zfsvfs->z_reclaim_znodes, sizeof (znode_t),
	    offsetof(znode_t, z_link_node));
	rrw_init(&zfsvfs->z_teardown_lock/*, B_FALSE*/);
	rw_init(&zfsvfs->z_teardown_inactive_lock, NULL, RW_DEFAULT, NULL);
	rw_init(&zfsvfs->z_fuid_lock, NULL, RW_DEFAULT, NULL);
	for (i = 0; i != ZFS_OBJ_MTX_SZ; i++)
		mutex_init(&zfsvfs->z_hold_mtx[i], NULL, MUTEX_DEFAULT, NULL);

    zfsvfs->z_reclaim_thread_exit = FALSE;
	(void) thread_create(NULL, 0, vnop_reclaim_thread, zfsvfs, 0, &p0,
	    TS_RUN, minclsyspri);

	*zfvp = zfsvfs;
	return (0);

out:
	dmu_objset_disown(os, zfsvfs);
	*zfvp = NULL;
	kmem_free(zfsvfs, sizeof (zfsvfs_t));
	return (error);
}

static int
zfsvfs_setup(zfsvfs_t *zfsvfs, boolean_t mounting)
{
	int error;

	error = zfs_register_callbacks(zfsvfs->z_vfs);
	if (error)
		return (error);

	/*
	 * Set the objset user_ptr to track its zfsvfs.
	 */
	mutex_enter(&zfsvfs->z_os->os_user_ptr_lock);
	dmu_objset_set_user(zfsvfs->z_os, zfsvfs);
	mutex_exit(&zfsvfs->z_os->os_user_ptr_lock);

	zfsvfs->z_log = zil_open(zfsvfs->z_os, zfs_get_data);

	/*
	 * If we are not mounting (ie: online recv), then we don't
	 * have to worry about replaying the log as we blocked all
	 * operations out since we closed the ZIL.
	 */
	if (mounting) {

		/*
		 * During replay we remove the read only flag to
		 * allow replays to succeed.
		 */
#if 0
		if (!vfs_isrdonly(zfsvfs->z_vfs))
			zfs_unlinked_drain(zfsvfs);
#endif

		/*
		 * Parse and replay the intent log.
		 *
		 * Because of ziltest, this must be done after
		 * zfs_unlinked_drain().  (Further note: ziltest
		 * doesn't use readonly mounts, where
		 * zfs_unlinked_drain() isn't called.)  This is because
		 * ziltest causes spa_sync() to think it's committed,
		 * but actually it is not, so the intent log contains
		 * many txg's worth of changes.
		 *
		 * In particular, if object N is in the unlinked set in
		 * the last txg to actually sync, then it could be
		 * actually freed in a later txg and then reallocated
		 * in a yet later txg.  This would write a "create
		 * object N" record to the intent log.  Normally, this
		 * would be fine because the spa_sync() would have
		 * written out the fact that object N is free, before
		 * we could write the "create object N" intent log
		 * record.
		 *
		 * But when we are in ziltest mode, we advance the "open
		 * txg" without actually spa_sync()-ing the changes to
		 * disk.  So we would see that object N is still
		 * allocated and in the unlinked set, and there is an
		 * intent log record saying to allocate it.
		 */
		if (spa_writeable(dmu_objset_spa(zfsvfs->z_os))) {
			if (zil_replay_disable) {
				zil_destroy(zfsvfs->z_log, B_FALSE);
			} else {
				zfsvfs->z_replay = B_TRUE;
				zil_replay(zfsvfs->z_os, zfsvfs,
				    zfs_replay_vector);
				zfsvfs->z_replay = B_FALSE;
			}
		}
	}

	return (0);
}

extern krwlock_t zfsvfs_lock; /* in zfs_znode.c */

void
zfsvfs_free(zfsvfs_t *zfsvfs)
{
	int i;

    dprintf("+zfsvfs_free\n");
	/*
	 * This is a barrier to prevent the filesystem from going away in
	 * zfs_znode_move() until we can safely ensure that the filesystem is
	 * not unmounted. We consider the filesystem valid before the barrier
	 * and invalid after the barrier.
	 */
	//rw_enter(&zfsvfs_lock, RW_READER);
	//rw_exit(&zfsvfs_lock);

	zfs_fuid_destroy(zfsvfs);

    dprintf("stopping reclaim thread\n");
	mutex_enter(&zfsvfs->z_reclaim_thr_lock);
    zfsvfs->z_reclaim_thread_exit = TRUE;
	cv_signal(&zfsvfs->z_reclaim_thr_cv);
	while (zfsvfs->z_reclaim_thread_exit == TRUE)
		cv_wait(&zfsvfs->z_reclaim_thr_cv, &zfsvfs->z_reclaim_thr_lock);
	mutex_exit(&zfsvfs->z_reclaim_thr_lock);

	mutex_destroy(&zfsvfs->z_reclaim_thr_lock);
	cv_destroy(&zfsvfs->z_reclaim_thr_cv);
    dprintf("Stopped, then releasing node.\n");

	mutex_destroy(&zfsvfs->z_znodes_lock);
	mutex_destroy(&zfsvfs->z_lock);
	mutex_destroy(&zfsvfs->z_vnode_create_lock);
	list_destroy(&zfsvfs->z_all_znodes);
	list_destroy(&zfsvfs->z_reclaim_znodes);
	rrw_destroy(&zfsvfs->z_teardown_lock);
	rw_destroy(&zfsvfs->z_teardown_inactive_lock);
	rw_destroy(&zfsvfs->z_fuid_lock);
	for (i = 0; i != ZFS_OBJ_MTX_SZ; i++)
		mutex_destroy(&zfsvfs->z_hold_mtx[i]);
	kmem_free(zfsvfs, sizeof (zfsvfs_t));
    dprintf("-zfsvfs_free\n");
}

static void
zfs_set_fuid_feature(zfsvfs_t *zfsvfs)
{
	zfsvfs->z_use_fuids = USE_FUIDS(zfsvfs->z_version, zfsvfs->z_os);
	if (zfsvfs->z_vfs) {
#if 0
		if (zfsvfs->z_use_fuids) {
			vfs_set_feature(zfsvfs->z_vfs, VFSFT_XVATTR);
			vfs_set_feature(zfsvfs->z_vfs, VFSFT_SYSATTR_VIEWS);
			vfs_set_feature(zfsvfs->z_vfs, VFSFT_ACEMASKONACCESS);
			vfs_set_feature(zfsvfs->z_vfs, VFSFT_ACLONCREATE);
			vfs_set_feature(zfsvfs->z_vfs, VFSFT_ACCESS_FILTER);
			vfs_set_feature(zfsvfs->z_vfs, VFSFT_REPARSE);
		} else {
			vfs_clear_feature(zfsvfs->z_vfs, VFSFT_XVATTR);
			vfs_clear_feature(zfsvfs->z_vfs, VFSFT_SYSATTR_VIEWS);
			vfs_clear_feature(zfsvfs->z_vfs, VFSFT_ACEMASKONACCESS);
			vfs_clear_feature(zfsvfs->z_vfs, VFSFT_ACLONCREATE);
			vfs_clear_feature(zfsvfs->z_vfs, VFSFT_ACCESS_FILTER);
			vfs_clear_feature(zfsvfs->z_vfs, VFSFT_REPARSE);
		}
#endif
	}
	zfsvfs->z_use_sa = USE_SA(zfsvfs->z_version, zfsvfs->z_os);
}

static int
zfs_domount(struct mount *vfsp, dev_t mount_dev, char *osname, vfs_context_t ctx,  mntopts_t *mnt_mntopts)
{
	int error = 0;
	zfsvfs_t *zfsvfs;
#ifndef __APPLE__
	uint64_t recordsize, fsid_guid;
	vnode_t *vp;
#else
	struct timeval tv;
#endif

	ASSERT(vfsp);
	ASSERT(osname);

	error = zfsvfs_create(osname, &zfsvfs);
	if (error)
		return (error);
	zfsvfs->z_vfs = vfsp;

	vfs_swapopttbl(mnt_mntopts, &zfsvfs->vfs_mntopts);

#ifdef illumos
	/* Initialize the generic filesystem structure. */
	vfsp->vfs_bcount = 0;
	vfsp->vfs_data = NULL;

	if (zfs_create_unique_device(&mount_dev) == -1) {
		error = ENODEV;
		goto out;
	}
	ASSERT(vfs_devismounted(mount_dev) == 0);
#endif


#ifdef __APPLE__
	/*
	 * Record the mount time (for Spotlight)
	 */
	microtime(&tv);
	zfsvfs->z_mount_time = tv.tv_sec;

	vfs_setfsprivate(vfsp, zfsvfs);
#else
	if (error = dsl_prop_get_integer(osname, "recordsize", &recordsize,
	    NULL))
		goto out;
	zfsvfs->z_vfs->vfs_bsize = SPA_MINBLOCKSIZE;
	zfsvfs->z_vfs->mnt_stat.f_iosize = recordsize;

	vfsp->vfs_data = zfsvfs;
	vfsp->mnt_flag |= MNT_LOCAL;
	vfsp->mnt_kern_flag |= MNTK_LOOKUP_SHARED;
	vfsp->mnt_kern_flag |= MNTK_SHARED_WRITES;
	vfsp->mnt_kern_flag |= MNTK_EXTENDED_SHARED;
#endif

	/*
	 * The fsid is 64 bits, composed of an 8-bit fs type, which
	 * separates our fsid from any other filesystem types, and a
	 * 56-bit objset unique ID.  The objset unique ID is unique to
	 * all objsets open on this system, provided by unique_create().
	 * The 8-bit fs type must be put in the low bits of fsid[1]
	 * because that's where other Solaris filesystems put it.
	 */

#ifdef __APPLE__
    vfs_getnewfsid(vfsp);
#else
	fsid_guid = dmu_objset_fsid_guid(zfsvfs->z_os);
	ASSERT((fsid_guid & ~((1ULL<<56)-1)) == 0);
	vfsp->vfs_fsid.val[0] = fsid_guid;
	vfsp->vfs_fsid.val[1] = ((fsid_guid>>32) << 8) |
	    vfsp->mnt_vfc->vfc_typenum & 0xFF;
#endif

	/*
	 * Set features for file system.
	 */
	zfs_set_fuid_feature(zfsvfs);
#if 0
	if (zfsvfs->z_case == ZFS_CASE_INSENSITIVE) {
		vfs_set_feature(vfsp, VFSFT_DIRENTFLAGS);
		vfs_set_feature(vfsp, VFSFT_CASEINSENSITIVE);
		vfs_set_feature(vfsp, VFSFT_NOCASESENSITIVE);
	} else if (zfsvfs->z_case == ZFS_CASE_MIXED) {
		vfs_set_feature(vfsp, VFSFT_DIRENTFLAGS);
		vfs_set_feature(vfsp, VFSFT_CASEINSENSITIVE);
	}
	vfs_set_feature(vfsp, VFSFT_ZEROCOPY_SUPPORTED);
#endif

	if (dmu_objset_is_snapshot(zfsvfs->z_os)) {
		uint64_t pval;

        vfs_setflags(vfsp, (u_int64_t)((unsigned int)MNT_AUTOMOUNTED));

		atime_changed_cb(zfsvfs, B_FALSE);
		readonly_changed_cb(zfsvfs, B_TRUE);
		if ((error = dsl_prop_get_integer(osname, "xattr", &pval, NULL)))
			goto out;
		xattr_changed_cb(zfsvfs, pval);
		zfsvfs->z_issnap = B_TRUE;
		zfsvfs->z_os->os_sync = ZFS_SYNC_DISABLED;

		mutex_enter(&zfsvfs->z_os->os_user_ptr_lock);
		dmu_objset_set_user(zfsvfs->z_os, zfsvfs);
		mutex_exit(&zfsvfs->z_os->os_user_ptr_lock);
	} else {
		error = zfsvfs_setup(zfsvfs, B_TRUE);
	}


	vfs_mountedfrom(vfsp, osname);
#ifdef __APPLE__

#else
	/* Grab extra reference. */
	VERIFY(VFS_ROOT(vfsp, LK_EXCLUSIVE, &vp) == 0);
	VOP_UNLOCK(vp, 0);
#endif

	if (!zfsvfs->z_issnap)
		zfsctl_create(zfsvfs);
out:
	if (error) {
		dmu_objset_disown(zfsvfs->z_os, zfsvfs);
		zfsvfs_free(zfsvfs);
	} else {
		atomic_add_32(&zfs_active_fs_count, 1);
	}

	return (error);
}

void
zfs_unregister_callbacks(zfsvfs_t *zfsvfs)
{
	objset_t *os = zfsvfs->z_os;
	struct dsl_dataset *ds;

	/*
	 * Unregister properties.
	 */
	if (!dmu_objset_is_snapshot(os)) {
		ds = dmu_objset_ds(os);
		VERIFY(dsl_prop_unregister(ds, "atime", atime_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "xattr", xattr_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "recordsize", blksz_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "readonly", readonly_changed_cb,
		    zfsvfs) == 0);

               VERIFY(dsl_prop_unregister(ds, "devices", devices_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "setuid", setuid_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "exec", exec_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "snapdir", snapdir_changed_cb,
		    zfsvfs) == 0);

        // See discussion in register_callbacks
		//VERIFY(dsl_prop_unregister(ds, "aclmode", acl_mode_changed_cb,
        //   zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "aclinherit",
		    acl_inherit_changed_cb, zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "vscan",
		    vscan_changed_cb, zfsvfs) == 0);
#ifdef __APPLE__
		VERIFY(dsl_prop_unregister(ds, "com.apple.browse",
		    finderbrowse_changed_cb, zfsvfs) == 0);
		VERIFY(dsl_prop_unregister(ds, "com.apple.ignoreowner",
		    ignoreowner_changed_cb, zfsvfs) == 0);
#endif
	}
}

#ifdef SECLABEL
/*
 * Convert a decimal digit string to a uint64_t integer.
 */
static int
str_to_uint64(char *str, uint64_t *objnum)
{
	uint64_t num = 0;

	while (*str) {
		if (*str < '0' || *str > '9')
			return (EINVAL);

		num = num*10 + *str++ - '0';
	}

	*objnum = num;
	return (0);
}

/*
 * The boot path passed from the boot loader is in the form of
 * "rootpool-name/root-filesystem-object-number'. Convert this
 * string to a dataset name: "rootpool-name/root-filesystem-name".
 */
static int
zfs_parse_bootfs(char *bpath, char *outpath)
{
	char *slashp;
	uint64_t objnum;
	int error;

	if (*bpath == 0 || *bpath == '/')
		return (EINVAL);

	(void) strcpy(outpath, bpath);

	slashp = strchr(bpath, '/');

	/* if no '/', just return the pool name */
	if (slashp == NULL) {
		return (0);
	}

	/* if not a number, just return the root dataset name */
	if (str_to_uint64(slashp+1, &objnum)) {
		return (0);
	}

	*slashp = '\0';
	error = dsl_dsobj_to_dsname(bpath, objnum, outpath);
	*slashp = '/';

	return (error);
}

/*
 * zfs_check_global_label:
 *	Check that the hex label string is appropriate for the dataset
 *	being mounted into the global_zone proper.
 *
 *	Return an error if the hex label string is not default or
 *	admin_low/admin_high.  For admin_low labels, the corresponding
 *	dataset must be readonly.
 */
int
zfs_check_global_label(const char *dsname, const char *hexsl)
{
	if (strcasecmp(hexsl, ZFS_MLSLABEL_DEFAULT) == 0)
		return (0);
	if (strcasecmp(hexsl, ADMIN_HIGH) == 0)
		return (0);
	if (strcasecmp(hexsl, ADMIN_LOW) == 0) {
		/* must be readonly */
		uint64_t rdonly;

		if (dsl_prop_get_integer(dsname,
		    zfs_prop_to_name(ZFS_PROP_READONLY), &rdonly, NULL))
			return (EACCES);
		return (rdonly ? 0 : EACCES);
	}
	return (EACCES);
}

/*
 * zfs_mount_label_policy:
 *	Determine whether the mount is allowed according to MAC check.
 *	by comparing (where appropriate) label of the dataset against
 *	the label of the zone being mounted into.  If the dataset has
 *	no label, create one.
 *
 *	Returns:
 *		 0 :	access allowed
 *		>0 :	error code, such as EACCES
 */
static int
zfs_mount_label_policy(vfs_t *vfsp, char *osname)
{
	int		error, retv;
	zone_t		*mntzone = NULL;
	ts_label_t	*mnt_tsl;
	bslabel_t	*mnt_sl;
	bslabel_t	ds_sl;
	char		ds_hexsl[MAXNAMELEN];

	retv = EACCES;				/* assume the worst */

	/*
	 * Start by getting the dataset label if it exists.
	 */
	error = dsl_prop_get(osname, zfs_prop_to_name(ZFS_PROP_MLSLABEL),
	    1, sizeof (ds_hexsl), &ds_hexsl, NULL);
	if (error)
		return (EACCES);

	/*
	 * If labeling is NOT enabled, then disallow the mount of datasets
	 * which have a non-default label already.  No other label checks
	 * are needed.
	 */
	if (!is_system_labeled()) {
		if (strcasecmp(ds_hexsl, ZFS_MLSLABEL_DEFAULT) == 0)
			return (0);
		return (EACCES);
	}

	/*
	 * Get the label of the mountpoint.  If mounting into the global
	 * zone (i.e. mountpoint is not within an active zone and the
	 * zoned property is off), the label must be default or
	 * admin_low/admin_high only; no other checks are needed.
	 */
	mntzone = zone_find_by_any_path(refstr_value(vfsp->vfs_mntpt), B_FALSE);
	if (mntzone->zone_id == GLOBAL_ZONEID) {
		uint64_t zoned;

		zone_rele(mntzone);

		if (dsl_prop_get_integer(osname,
		    zfs_prop_to_name(ZFS_PROP_ZONED), &zoned, NULL))
			return (EACCES);
		if (!zoned)
			return (zfs_check_global_label(osname, ds_hexsl));
		else
			/*
			 * This is the case of a zone dataset being mounted
			 * initially, before the zone has been fully created;
			 * allow this mount into global zone.
			 */
			return (0);
	}

	mnt_tsl = mntzone->zone_slabel;
	ASSERT(mnt_tsl != NULL);
	label_hold(mnt_tsl);
	mnt_sl = label2bslabel(mnt_tsl);

	if (strcasecmp(ds_hexsl, ZFS_MLSLABEL_DEFAULT) == 0) {
		/*
		 * The dataset doesn't have a real label, so fabricate one.
		 */
		char *str = NULL;

		if (l_to_str_internal(mnt_sl, &str) == 0 &&
		    dsl_prop_set_string(osname,
		    zfs_prop_to_name(ZFS_PROP_MLSLABEL),
		    ZPROP_SRC_LOCAL, str) == 0)
			retv = 0;
		if (str != NULL)
			kmem_free(str, strlen(str) + 1);
	} else if (hexstr_to_label(ds_hexsl, &ds_sl) == 0) {
		/*
		 * Now compare labels to complete the MAC check.  If the
		 * labels are equal then allow access.  If the mountpoint
		 * label dominates the dataset label, allow readonly access.
		 * Otherwise, access is denied.
		 */
		if (blequal(mnt_sl, &ds_sl))
			retv = 0;
		else if (bldominates(mnt_sl, &ds_sl)) {
			vfs_setmntopt(vfsp, MNTOPT_RO, NULL, 0);
			retv = 0;
		}
	}

	label_rele(mnt_tsl);
	zone_rele(mntzone);
	return (retv);
}
#endif	/* SECLABEL */

#ifdef OPENSOLARIS_MOUNTROOT
static int
zfs_mountroot(vfs_t *vfsp, enum whymountroot why)
{
	int error = 0;
	static int zfsrootdone = 0;
	zfsvfs_t *zfsvfs = NULL;
	znode_t *zp = NULL;
	vnode_t *vp = NULL;
	char *zfs_bootfs;
	char *zfs_devid;

	ASSERT(vfsp);

	/*
	 * The filesystem that we mount as root is defined in the
	 * boot property "zfs-bootfs" with a format of
	 * "poolname/root-dataset-objnum".
	 */
	if (why == ROOT_INIT) {
		if (zfsrootdone++)
			return (EBUSY);
		/*
		 * the process of doing a spa_load will require the
		 * clock to be set before we could (for example) do
		 * something better by looking at the timestamp on
		 * an uberblock, so just set it to -1.
		 */
		clkset(-1);

		if ((zfs_bootfs = spa_get_bootprop("zfs-bootfs")) == NULL) {
			cmn_err(CE_NOTE, "spa_get_bootfs: can not get "
			    "bootfs name");
			return (EINVAL);
		}
		zfs_devid = spa_get_bootprop("diskdevid");
		error = spa_import_rootpool(rootfs.bo_name, zfs_devid);
		if (zfs_devid)
			spa_free_bootprop(zfs_devid);
		if (error) {
			spa_free_bootprop(zfs_bootfs);
			cmn_err(CE_NOTE, "spa_import_rootpool: error %d",
			    error);
			return (error);
		}
		if (error = zfs_parse_bootfs(zfs_bootfs, rootfs.bo_name)) {
			spa_free_bootprop(zfs_bootfs);
			cmn_err(CE_NOTE, "zfs_parse_bootfs: error %d",
			    error);
			return (error);
		}

		spa_free_bootprop(zfs_bootfs);

		if (error = vfs_lock(vfsp))
			return (error);

		if (error = zfs_domount(vfsp, rootfs.bo_name)) {
			cmn_err(CE_NOTE, "zfs_domount: error %d", error);
			goto out;
		}

		zfsvfs = (zfsvfs_t *)vfsp->vfs_data;
		ASSERT(zfsvfs);
		if (error = zfs_zget(zfsvfs, zfsvfs->z_root, &zp)) {
			cmn_err(CE_NOTE, "zfs_zget: error %d", error);
			goto out;
		}

		vp = ZTOV(zp);
		mutex_enter(&vp->v_lock);
		vp->v_flag |= VROOT;
		mutex_exit(&vp->v_lock);
		rootvp = vp;

		/*
		 * Leave rootvp held.  The root file system is never unmounted.
		 */

		vfs_add((struct vnode *)0, vfsp,
		    (vfsp->vfs_flag & VFS_RDONLY) ? MS_RDONLY : 0);
out:
		vfs_unlock(vfsp);
		return (error);
	} else if (why == ROOT_REMOUNT) {
		readonly_changed_cb(vfsp->vfs_data, B_FALSE);
		vfsp->vfs_flag |= VFS_REMOUNT;

		/* refresh mount options */
		zfs_unregister_callbacks(vfsp->vfs_data);
		return (zfs_register_callbacks(vfsp));

	} else if (why == ROOT_UNMOUNT) {
		zfs_unregister_callbacks((zfsvfs_t *)vfsp->vfs_data);
		(void) zfs_sync(vfsp, 0, 0);
		return (0);
	}

	/*
	 * if "why" is equal to anything else other than ROOT_INIT,
	 * ROOT_REMOUNT, or ROOT_UNMOUNT, we do not support it.
	 */
	return (ENOTSUP);
}
#endif	/* OPENSOLARIS_MOUNTROOT */

#ifdef __LINUX__
static int
getpoolname(const char *osname, char *poolname)
{
	char *p;

	p = strchr(osname, '/');
	if (p == NULL) {
		if (strlen(osname) >= MAXNAMELEN)
			return (ENAMETOOLONG);
		(void) strlcpy(poolname, osname, MAXNAMELEN);
	} else {
		if (p - osname >= MAXNAMELEN)
			return (ENAMETOOLONG);
		(void) strncpy(poolname, osname, p - osname);
		poolname[p - osname] = '\0';
	}
	return (0);
}
#endif

/*ARGSUSED*/
int
zfs_vfs_mount(struct mount *vfsp, vnode_t *mvp /*devvp*/,
              user_addr_t data, vfs_context_t context)
{
	cred_t		*cr =  (cred_t *)vfs_context_ucred(context);
	char		*osname;
	int		error = 0;
	int		canwrite;

#ifdef __APPLE__
    struct zfs_mount_args mnt_args;
    size_t		osnamelen = 0;
    size_t		inargslen = 0;
    int			optlen = 0;
    char		*opts = NULL;
    char		*inargs = NULL;
    mntopts_t           mnt_mntopts;
    mnt_mntopts.mo_count = 0;
    int			copyout_error = 0;
    int			remount = 0;
    int			rdonly = 0;
    /*
     * Get the objset name (the "special" mount argument).
     */
    if (data) {
	// 10a286 renames fspec to datasetpath
        // Clear the struct, so that "flags" is null if only given path.
        bzero(&mnt_args, sizeof(mnt_args));

        if (vfs_context_is64bit(context)) {

            if ( (error = copyin(data, (caddr_t)&mnt_args, sizeof(mnt_args))) )
                goto out;
        } else {
            user32_addr_t tmp;
            if ( (error = copyin(data, (caddr_t)&tmp, sizeof(tmp))) )
                goto out;
            /* munge into LP64 addr */
            mnt_args.fspec = (char *)CAST_USER_ADDR_T(tmp);
        }

	opts = ((struct zfs_mount_args *)data)->optptr;
    	inargs = opts;
	optlen = ((struct zfs_mount_args *)data)->optlen;
        if (optlen < 0 || optlen > MAX_MNTOPT_STR) {
           error = EINVAL;
           goto out;
        }
        // Allocate string area
        osname = kmem_alloc(MAXPATHLEN, KM_SLEEP);
	inargs = kmem_alloc(MAX_MNTOPT_STR, KM_SLEEP);
	inargs[0] = '\0';
	if (optlen) {
		error = copyinstr((user_addr_t)mnt_args.optptr, inargs,
                                MAX_MNTOPT_STR, &inargslen);
		if (error) {
			goto out;
		}
	}

	dprintf("inargs is %s\n", inargs);
	dprintf("1 optlenp %p optlen %d\n", &optlen, optlen);
	optlen = mnt_args.optlen;
	dprintf("2 optlenp %p optlen %d\n", &optlen, optlen);

        // Copy over the string
        if ( (error = copyinstr((user_addr_t)mnt_args.fspec, osname,
                                MAXPATHLEN, &osnamelen)) )
            goto out;
    }

    dprintf("vfs_mount: options %04x path '%s'\n",
            mnt_args.flags, mnt_args.fspec);

    dprintf("optsp %p, opts %s optlen %d\n", opts, opts, optlen);
    dprintf("inargsp %p, inargs %s inargslen %d\n", inargs, inargs, inargslen);
#endif

    struct vfssw *vswp;
    char fstname[FSTYPSZ];
    size_t n;
    uint_t fstype;

    char *fsname = fstname;

    error = copystr(MNTTYPE_ZFS, fsname,
        FSTYPSZ, &n);

    if (error) {
        //if (error == ENAMETOOLONG)
        //    return (EINVAL);
        return (error);
    }

    if ((vswp = vfs_getvfssw(fsname)) == NULL)
        return (EINVAL);

    dprintf("vswp %p : vsw_optprotop %p : mo_count %d\n", vswp, &vswp->vsw_optproto, (&vswp->vsw_optproto)->mo_count);
    //if ( &vswp->vsw_optproto == NULL) {
        vfs_mergeopttbl(&vfs_mntopts, vfw.optproto,
		    &vswp->vsw_optproto);
    //}

    if ( &vswp->vsw_optproto != NULL ) {
        vfs_copyopttbl(&vswp->vsw_optproto, &mnt_mntopts);
    }

    vfs_parsemntopts(&mnt_mntopts, inargs, 0);

//    if (uap->flags & MS_OPTIONSTR) {
//        if (!(vswp->vsw_flag & VSW_HASPROTO)) {
//            mntopts_t tmp_mntopts;
//            tmp_mntopts.mo_count = 0;
//            vfs_createopttbl_extend(&tmp_mntopts, inargs,
//                &mnt_mntopts);
//            vfs_parsemntopts(&tmp_mntopts, inargs, 1);
//            vfs_swapopttbl_nolock(&mnt_mntopts, &tmp_mntopts);
//            vfs_freeopttbl(&tmp_mntopts);
//        }
//    }

#if 0
MNT_AUTOMOUNTED
MNT_DONTBROWSE
MNT_DOVOLFS
MNT_FORCE
MNT_IGNORE_OWNERSHIP
MNT_LOCAL
MNT_NFS
MNT_NOATIME
MNT_NOEXEC
MNT_NOSUID
MNT_NOUSERXATTR
MNT_RDONLY
MNT_ROOTFS
MNT_UPDATE
#endif

#ifdef illumos
    /*
     * Flag bits override the options string.
     */
    if (uap->flags & MS_REMOUNT)
        vfs_setmntopt_nolock(&mnt_mntopts, MNTOPT_REMOUNT, NULL, 0, 0);
    if (uap->flags & MS_RDONLY)
        vfs_setmntopt_nolock(&mnt_mntopts, MNTOPT_RO, NULL, 0, 0);
    if (uap->flags & MS_NOSUID)
        vfs_setmntopt_nolock(&mnt_mntopts, MNTOPT_NOSUID, NULL, 0, 0);
#endif

#ifdef __APPLE__
    int mntflags = vfs_flags(vfsp);

    if (mntflags & MNT_UPDATE) //same as MS_REMOUNT?
        vfs_setmntopt_nolock(&mnt_mntopts, MNTOPT_REMOUNT, NULL, 0, 0);
    if (mntflags & MNT_RDONLY)
        vfs_setmntopt_nolock(&mnt_mntopts, MNTOPT_RO, NULL, 0, 0);
    if (mntflags & MNT_NOSUID)
        vfs_setmntopt_nolock(&mnt_mntopts, MNTOPT_NOSUID, NULL, 0, 0);
#endif

#ifdef illumos
    /*
     * Check if this is a remount; must be set in the option string and
     * the file system must support a remount option.
     */
    if (remount = vfs_optionisset_nolock(&mnt_mntopts, 
        MNTOPT_REMOUNT, NULL)) {
            if (!(vswp->vsw_flag & VSW_CANREMOUNT)) {
                error = ENOTSUP;
                goto errout;
            }
            uap->flags |= MS_REMOUNT;
    }

    if (rdonly = vfs_optionisset_nolock(&mnt_mntopts, MNTOPT_RO, NULL)) {
        uap->flags |= MS_RDONLY;
    }
    if (vfs_optionisset_nolock(&mnt_mntopts, MNTOPT_NOSUID, NULL)) {
        uap->flags |= MS_NOSUID;
    }
    nbmand = vfs_optionisset_nolock(&mnt_mntopts, MNTOPT_NBMAND, NULL);
#endif

#ifdef __APPLE__
    if (remount = vfs_optionisset_nolock(&mnt_mntopts,
        MNTOPT_REMOUNT, NULL)) {
        vfs_setflags(vfsp, (uint64_t)MNT_UPDATE);
    }
    if (rdonly = vfs_optionisset_nolock(&mnt_mntopts, MNTOPT_RO, NULL)) {
        vfs_setflags(vfsp, (uint64_t)MNT_RDONLY);
    }
    if (vfs_optionisset_nolock(&mnt_mntopts, MNTOPT_NOSUID, NULL)) {
        vfs_setflags(vfsp, (uint64_t)MNT_NOSUID);
    }
#endif

#ifdef illumos
//if (splice) ...
//if (remount) ...
//else vfsp = vfs_alloc(KM_SLEEP); VFS_INIT(vfsp, vfsops, NULL);
//VFS_HOLD(vfsp);
//lofi_add
//vfs lock
//Add device to mount in progress table
//mount_in_progress();
//vfs_swapopttbl
//VFS_MOUNT, so now do the rest of zfs_vfs_mount
#endif

#ifdef illumos
	if (mvp->v_type != VDIR)
		return (ENOTDIR);

	mutex_enter(&mvp->v_lock);
	if ((uap->flags & MS_REMOUNT) == 0 &&
	    (uap->flags & MS_OVERLAY) == 0 &&
	    (mvp->v_count != 1 || (mvp->v_flag & VROOT))) {
		mutex_exit(&mvp->v_lock);
		return (EBUSY);
	}
	mutex_exit(&mvp->v_lock);

	/*
	 * ZFS does not support passing unparsed data in via MS_DATA.
	 * Users should use the MS_OPTIONSTR interface; this means
	 * that all option parsing is already done and the options struct
	 * can be interrogated.
	 */
	if ((uap->flags & MS_DATA) && uap->datalen > 0)
#endif

#if __FreeBSD__
	if (!prison_allow(td->td_ucred, PR_ALLOW_MOUNT_ZFS))
		return (EPERM);

	if (vfs_getopt(vfsp->mnt_optnew, "from", (void **)&osname, NULL))
		return (EINVAL);
#endif	/* ! illumos */

	/*
	 * If full-owner-access is enabled and delegated administration is
	 * turned on, we must set nosuid.
	 */
#if 0
	if (zfs_super_owner &&
	    dsl_deleg_access(osname, ZFS_DELEG_PERM_MOUNT, cr) != ECANCELED) {
		secpolicy_fs_mount_clearopts(cr, vfsp);
	}
#endif

	/*
	 * Check for mount privilege?
	 *
	 * If we don't have privilege then see if
	 * we have local permission to allow it
	 */
	error = secpolicy_fs_mount(cr, mvp, vfsp);
	if (error) {
		if (dsl_deleg_access(osname, ZFS_DELEG_PERM_MOUNT, cr) != 0)
			goto out;

#if 0
		if (!(vfsp->vfs_flag & MS_REMOUNT)) {
			vattr_t		vattr;

			/*
			 * Make sure user is the owner of the mount point
			 * or has sufficient privileges.
			 */

			vattr.va_mask = AT_UID;

			vn_lock(mvp, LK_SHARED | LK_RETRY);
			if (VOP_GETATTR(mvp, &vattr, cr)) {
				VOP_UNLOCK(mvp, 0);
				goto out;
			}

			if (secpolicy_vnode_owner(mvp, cr, vattr.va_uid) != 0 &&
			    VOP_ACCESS(mvp, VWRITE, cr, td) != 0) {
				VOP_UNLOCK(mvp, 0);
				goto out;
			}
			VOP_UNLOCK(mvp, 0);
		}
#endif
		secpolicy_fs_mount_clearopts(cr, vfsp);
	}

	/*
	 * Refuse to mount a filesystem if we are in a local zone and the
	 * dataset is not visible.
	 */
	if (!INGLOBALZONE(curthread) &&
	    (!zone_dataset_visible(osname, &canwrite) || !canwrite)) {
		error = EPERM;
		goto out;
	}

#ifdef SECLABEL
	error = zfs_mount_label_policy(vfsp, osname);
	if (error)
		goto out;
#endif

#ifndef __APPLE__
	vfsp->vfs_flag |= MNT_NFS4ACLS;

	/*
	 * When doing a remount, we simply refresh our temporary properties
	 * according to those options set in the current VFS options.
	 */
	if (vfsp->vfs_flag & MS_REMOUNT) {
		/* refresh mount options */
		zfs_unregister_callbacks(vfsp->vfs_data);
		error = zfs_register_callbacks(vfsp);
		goto out;
	}

	/* Initial root mount: try hard to import the requested root pool. */
	if ((vfsp->vfs_flag & MNT_ROOTFS) != 0 &&
	    (vfsp->vfs_flag & MNT_UPDATE) == 0) {
		char pname[MAXNAMELEN];

		error = getpoolname(osname, pname);
		if (error == 0)
			error = spa_import_rootpool(pname);
		if (error)
			goto out;
	}
#endif

	error = zfs_domount(vfsp, 0, osname, context, &mnt_mntopts);

#ifdef sun
	/*
	 * Add an extra VFS_HOLD on our parent vfs so that it can't
	 * disappear due to a forced unmount.
	 */
	if (error == 0 && ((zfsvfs_t *)vfsp->vfs_data)->z_issnap)
		VFS_HOLD(mvp->v_vfsp);
#endif	/* sun */

#ifdef __APPLE__
	if (error)
		dprintf("zfs_vfs_mount: error %d\n", error);
	if (error == 0) {
		//vfs_setflags(vfsp, (u_int64_t)((unsigned int)MNT_DOVOLFS));
		/* Indicate to VFS that we support ACLs. */
		vfs_setextendedsecurity(vfsp);

		/* Advisory locking should be handled at the VFS layer */
		vfs_setlocklocal(vfsp);

#if 0
		/*
		 * Mac OS X needs a file system modify time
		 *
		 * We use the mtime of the "com.apple.system.mtime"
		 * extended attribute, which is associated with the
		 * file system root directory.
		 *
		 * Here we need to take a ref on z_mtime_vp to keep it around.
		 * If the attribute isn't there, attempt to create it.
		 */
		zfsvfs = vfs_fsprivate(vfsp);
        if (zfsvfs->z_mtime_vp == NULL) {
			vnode_t *rvp;
			vnode_t *xdvp = NULLVP;
			vnode_t *xvp = NULLVP;
			znode_t *rootzp;
			timestruc_t modify_time;
			cred_t  *cr;
			timestruc_t  now;
			int flag;
			int result;

			if (zfs_zget(zfsvfs, zfsvfs->z_root, &rootzp) != 0) {
				goto out;
			}
			rvp = ZTOV(rootzp);
			cr = (cred_t *)vfs_context_ucred(context);

			/* Grab the hidden attribute directory vnode. */
			result = zfs_get_xattrdir(rootzp, &xdvp, cr, CREATE_XATTR_DIR);
			vnode_put(rvp);	/* all done with root vnode */
			rvp = NULL;
			if (result) {
				goto out;
			}

			/*
			 * HACK - workaround missing vnode_setnoflush() KPI...
			 *
			 * We tag zfsvfs so that zfs_attach_vnode() can then set
			 * vnfs_marksystem when the vnode gets created.
			 */
			zfsvfs->z_last_unmount_time = 0xBADC0DE;
			zfsvfs->z_last_mtime_synced = VTOZ(xdvp)->z_id;
			flag = vfs_isrdonly(vfsp) ? 0 : ZEXISTS;
			/* Lookup or create the named attribute. */
			if ( zfs_obtain_xattr(VTOZ(xdvp), ZFS_MTIME_XATTR,
                                  S_IRUSR | S_IWUSR, cr, &xvp,
                                  flag) ) {
                zfsvfs->z_last_unmount_time = 0;
                zfsvfs->z_last_mtime_synced = 0;
                vnode_put(xdvp);
                goto out;
            }
            gethrestime(&now);

            ZFS_TIME_ENCODE(&now, VTOZ(xvp)->z_atime);
			vnode_put(xdvp);
            /* Can't hold a ref if we are readonly. */
            if (!vfs_isrdonly(vfsp)) {

                //vnode_ref(xvp);

                zfsvfs->z_mtime_vp = xvp;
            }
            ZFS_TIME_DECODE(&modify_time, VTOZ(xvp)->z_atime);
            zfsvfs->z_last_unmount_time = modify_time.tv_sec;
            zfsvfs->z_last_mtime_synced = modify_time.tv_sec;
			/*
			 * Keep this referenced vnode from impeding an unmount.
			 *
			 * XXX vnode_setnoflush() is MIA from KPI (see workaround above).
			 */
#if 0
			vnode_setnoflush(xvp);
#endif
			vnode_put(xvp);
		}
#endif

	}
#endif /* __APPLE__ */

#ifdef illumos
        if (uap->flags & MS_RDONLY)
            vfs_setmntopt(vfsp, MNTOPT_RO, NULL, 0);
        if (uap->flags & MS_NOSUID)
            vfs_setmntopt(vfsp, MNTOPT_NOSUID, NULL, 0);
        if (uap->flags & MS_GLOBAL)
            vfs_setmntopt(vfsp, MNTOPT_GLOBAL, NULL, 0);
#endif

#ifdef __APPLE__
        zfsvfs_t *zfsvfs = vfs_fsprivate(vfsp);
        //Checking mnt_mntopts is connected to vfsp's zfsvfs_t
        dprintf("zfsvfs %p : &zfsvfs->vfs_mntopts %p : &mnt_mntopts %p\n", zfsvfs, &zfsvfs->vfs_mntopts, &mnt_mntopts);
        if (vfs_flags(vfsp) & MNT_RDONLY)
            vfs_setmntopt(zfsvfs, MNTOPT_RO, NULL, 0);
        if (vfs_flags(vfsp) & MNT_NOSUID)
            vfs_setmntopt(zfsvfs, MNTOPT_NOSUID, NULL, 0);
#endif
    
    if (error) {
#ifdef illumos
        lofi_remove(vfsp);
#endif
        if (remount) {
            /* put back pre-remount options */
#ifdef illumos
            vfs_swapopttbl(&mnt_mntopts, &vfsp->vfs_mntopts);
#endif
#ifdef __APPLE__
            vfs_swapopttbl(&mnt_mntopts, &zfsvfs->vfs_mntopts);
#endif
#ifdef illumos
            vfs_setmntpoint(vfsp, refstr_value(oldmntpt),
                VFSSP_VERBATIM);
            if (oldmntpt)
                refstr_rele(oldmntpt);
            vfs_setresource(vfsp, refstr_value(oldresource),
                VFSSP_VERBATIM);
            if (oldresource)
                refstr_rele(oldresource);
            vfsp->vfs_flag = ovflags;
            vfs_unlock(vfsp);
            VFS_RELE(vfsp);
        } else {
            vfs_unlock(vfsp);
            vfs_freemnttab(vfsp);
            vfs_free(vfsp);
#endif
        }
    } else {
#ifdef illumos
        /*
         * Set the mount time to now
         */
        vfsp->vfs_mtime = ddi_get_time();
        if (remount) {
            vfsp->vfs_flag &= ~VFS_REMOUNT;
            if (oldresource)
                refstr_rele(oldresource);
            if (oldmntpt)
                refstr_rele(oldmntpt);
        } else if (splice) {
            /*
             * Link vfsp into the name space at the mount
             * point. Vfs_add() is responsible for
             * holding the mount point which will be
             * released when vfs_remove() is called.
             */
            vfs_add(vp, vfsp, uap->flags);
        } else {
            /*
             * Hold the reference to file system which is
             * not linked into the name space.
             */
            vfsp->vfs_zone = NULL;
            VFS_HOLD(vfsp);
            vfsp->vfs_vnodecovered = NULL;
        }
#endif
#ifdef __OPPLE__
        if (remount) {
            vfs_clearflags(vfsp, (uint64_t)MNT_UPDATE);
        }
#endif
        /*
         * Set flags for global options encountered
         */
#ifdef illumos
        if (vfs_optionisset(vfsp, MNTOPT_RO, NULL))
            vfsp->vfs_flag |= VFS_RDONLY;
        else
            vfsp->vfs_flag &= ~VFS_RDONLY;
        if (vfs_optionisset(vfsp, MNTOPT_NOSUID, NULL)) {
            vfsp->vfs_flag |= (VFS_NOSETUID|VFS_NODEV);
        } else {
            if (vfs_optionisset(vfsp, MNTOPT_NODEVICES, NULL))
                vfsp->vfs_flag |= VFS_NODEV;
            else
                vfsp->vfs_flag &= ~VFS_NODEV;
            if (vfs_optionisset(vfsp, MNTOPT_NOSETUID, NULL))
                vfsp->vfs_flag |= VFS_NOSETUID;
            else
                vfsp->vfs_flag &= ~VFS_NOSETUID;
        }
        if (vfs_optionisset(vfsp, MNTOPT_NBMAND, NULL))
            vfsp->vfs_flag |= VFS_NBMAND;
        else
            vfsp->vfs_flag &= ~VFS_NBMAND;
        
        if (vfs_optionisset(vfsp, MNTOPT_XATTR, NULL))
            vfsp->vfs_flag |= VFS_XATTR;
        else
            vfsp->vfs_flag &= ~VFS_XATTR;
        
        if (vfs_optionisset(vfsp, MNTOPT_NOEXEC, NULL))
            vfsp->vfs_flag |= VFS_NOEXEC;
        else
            vfsp->vfs_flag &= ~VFS_NOEXEC;
#endif
#ifdef __APPLE__
        if (vfs_optionisset(zfsvfs, MNTOPT_RO, NULL))
            vfs_setflags(vfsp, (uint64_t)MNT_RDONLY);
        else
            vfs_clearflags(vfsp, (uint64_t)MNT_RDONLY);
        if (vfs_optionisset(zfsvfs, MNTOPT_NOSUID, NULL)) {
            //vfs_setflags(vfsp, (uint64_t)MNT_NOSETUID | (uint64_t)MNT_NODEV);
            vfs_setflags(vfsp, (uint64_t)MNT_NOSUID | (uint64_t)MNT_NODEV);
        } else {
            if (vfs_optionisset(zfsvfs, MNTOPT_NODEVICES, NULL))
                vfs_setflags(vfsp, (uint64_t)MNT_NODEV);
            else
                vfs_clearflags(vfsp, (uint64_t)MNT_NODEV);
            if (vfs_optionisset(zfsvfs, MNTOPT_NOSETUID, NULL))
                //vfs_setflags(vfsp, (uint64_t)MNT_NOSETUID);
                vfs_setflags(vfsp, (uint64_t)MNT_NOSUID);
            else
                //vfs_clearflags(vfsp, (uint64_t)MNT_NOSETUID);
                vfs_clearflags(vfsp, (uint64_t)MNT_NOSUID);
        }
	if (vfs_optionisset(zfsvfs, MNTOPT_NODEVICES, NULL))
                vfs_setflags(vfsp, (uint64_t)MNT_NODEV);
            else
                vfs_clearflags(vfsp, (uint64_t)MNT_NODEV);
	printf("MNT_NODEV is %d\n", MNT_NODEV);
        if (vfs_optionisset(zfsvfs, MNTOPT_XATTR, NULL))
            vfs_clearflags(vfsp, (uint64_t)MNT_NOUSERXATTR);
        else
            vfs_setflags(vfsp, (uint64_t)MNT_NOUSERXATTR);
        if (vfs_optionisset(zfsvfs, MNTOPT_NOEXEC, NULL))
            vfs_setflags(vfsp, (uint64_t)MNT_NOEXEC);
        else
            vfs_clearflags(vfsp, (uint64_t)MNT_NOEXEC);
#endif
#ifdef illumos
        /*
         * Now construct the output option string of options
         * we recognized.
         */
        if (uap->flags & MS_OPTIONSTR) {
            vfs_list_read_lock();
            copyout_error = vfs_buildoptionstr(
                &vfsp->vfs_mntopts, inargs, optlen);
            vfs_list_unlock();
            if (copyout_error == 0 &&
                (uap->flags & MS_SYSSPACE) == 0) {
                copyout_error = copyoutstr(inargs, opts,
                    optlen, NULL);
            }
        }
#endif
#ifdef __APPLE__
        size_t copyoutlen = 0;
        copyout_error = vfs_buildoptionstr(&zfsvfs->vfs_mntopts, inargs, optlen);
        if (copyout_error == 0) {
            copyout_error = copyoutstr(inargs, opts, optlen, &copyoutlen);
        }
        dprintf("inargs p %p inargs %s opts p %p opts %s optlen %d copyoutlen %d\n",
               inargs, inargs, opts, opts, optlen, copyoutlen);
#endif
    }
out:
errout:
#ifdef illumos
    vfs_freeopttbl(&mnt_mntopts);
    if (resource != NULL)
        kmem_free(resource, strlen(resource) + 1);
    if (mountpt != NULL)
        kmem_free(mountpt, strlen(mountpt) + 1);
    /*
     * It is possible we errored prior to adding to mount in progress 
     * table. Must free vnode we acquired with successful lookupname. 
     */
    if (addmip)
        VN_RELE(bvp);
    if (delmip)
        vfs_delmip(vfsp);
    ASSERT(vswp != NULL);
    vfs_unrefvfssw(vswp);
    if (inargs != opts)
        kmem_free(inargs, MAX_MNTOPT_STR);
    if (copyout_error) {
        lofi_remove(vfsp);
        VFS_RELE(vfsp);
        error = copyout_error;
    }
#endif
#ifdef __APPLE__
    vfs_freeopttbl(&mnt_mntopts);
    ASSERT(vswp != NULL);
    //vfs_unrefvfssw(vswp);
    if (inargs != opts)
        kmem_free(inargs, MAX_MNTOPT_STR);
    if (copyout_error) {
        error = copyout_error;
        dprintf("copyout_error %d\n", error);
    }
#endif
    return (error);
}

int
zfs_vfs_getattr(struct mount *mp, struct vfs_attr *fsap, __unused vfs_context_t context)
{
    zfsvfs_t *zfsvfs = vfs_fsprivate(mp);
	uint64_t refdbytes, availbytes, usedobjs, availobjs;

    dprintf("vfs_getattr\n");

#ifndef __APPLE__
	statp->f_version = STATFS_VERSION;
#endif

	ZFS_ENTER(zfsvfs);

	dmu_objset_space(zfsvfs->z_os,
	    &refdbytes, &availbytes, &usedobjs, &availobjs);

#ifdef __APPLE__
	VFSATTR_RETURN(fsap, f_objcount, usedobjs);
	VFSATTR_RETURN(fsap, f_maxobjcount, 0x7fffffffffffffff);
	/*
	 * Carbon depends on f_filecount and f_dircount so
	 * make up some values based on total objects.
	 */
	VFSATTR_RETURN(fsap, f_filecount, usedobjs - (usedobjs / 4));
	VFSATTR_RETURN(fsap, f_dircount, usedobjs / 4);
#endif /* __APPLE__ */

	/*
	 * The underlying storage pool actually uses multiple block sizes.
	 * We report the fragsize as the smallest block size we support,
	 * and we report our blocksize as the filesystem's maximum blocksize.
	 */
#ifdef __APPLE__
	VFSATTR_RETURN(fsap, f_bsize, 1UL << SPA_MINBLOCKSHIFT);
	VFSATTR_RETURN(fsap, f_iosize, zfsvfs->z_max_blksz);
#else
	statp->f_bsize = SPA_MINBLOCKSIZE;
	statp->f_iosize = zfsvfs->z_vfs->mnt_stat.f_iosize;
#endif

	/*
	 * The following report "total" blocks of various kinds in the
	 * file system, but reported in terms of f_frsize - the
	 * "fragment" size.
	 */
#ifdef __APPLE__
	VFSATTR_RETURN(fsap, f_blocks,
	               (u_int64_t)((refdbytes + availbytes) >> SPA_MINBLOCKSHIFT));
	VFSATTR_RETURN(fsap, f_bfree, (u_int64_t)(availbytes >> SPA_MINBLOCKSHIFT));
	VFSATTR_RETURN(fsap, f_bavail, fsap->f_bfree);  /* no root reservation */
	VFSATTR_RETURN(fsap, f_bused, fsap->f_blocks - fsap->f_bfree);
#else
	statp->f_blocks = (refdbytes + availbytes) >> SPA_MINBLOCKSHIFT;
	statp->f_bfree = availbytes / statp->f_bsize;
	statp->f_bavail = statp->f_bfree; /* no root reservation */
#endif

	/*
	 * statvfs() should really be called statufs(), because it assumes
	 * static metadata.  ZFS doesn't preallocate files, so the best
	 * we can do is report the max that could possibly fit in f_files,
	 * and that minus the number actually used in f_ffree.
	 * For f_ffree, report the smaller of the number of object available
	 * and the number of blocks (each object will take at least a block).
	 */
#ifdef __APPLE__
	VFSATTR_RETURN(fsap, f_ffree, (u_int64_t)MIN(availobjs, fsap->f_bfree));
	VFSATTR_RETURN(fsap, f_files,  fsap->f_ffree + usedobjs);

#if 0
	statp->f_flag = vf_to_stf(vfsp->vfs_flag);
#endif

	if (VFSATTR_IS_ACTIVE(fsap, f_fsid)) {
		VFSATTR_RETURN(fsap, f_fsid, vfs_statfs(mp)->f_fsid);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_capabilities)) {
		bcopy(&zfs_capabilities, &fsap->f_capabilities, sizeof (zfs_capabilities));
		VFSATTR_SET_SUPPORTED(fsap, f_capabilities);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_attributes)) {
		bcopy(&zfs_attributes, &fsap->f_attributes.validattr, sizeof (zfs_attributes));
		bcopy(&zfs_attributes, &fsap->f_attributes.nativeattr, sizeof (zfs_attributes));
		VFSATTR_SET_SUPPORTED(fsap, f_attributes);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_create_time)) {
		dmu_objset_stats_t dmu_stat;

		dmu_objset_fast_stat(zfsvfs->z_os, &dmu_stat);
		//fsap->f_create_time.tv_sec = dmu_stat.dds_creation_time;
		fsap->f_create_time.tv_nsec = 0;
		VFSATTR_SET_SUPPORTED(fsap, f_create_time);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_modify_time)) {
#if 0
		if (zfsvfs->z_mtime_vp != NULL) {
			znode_t *mzp;

			mzp = VTOZ(zfsvfs->z_mtime_vp);
			//ZFS_TIME_DECODE(&fsap->f_modify_time, mzp->zp_mtime);
		} else {
#endif
			fsap->f_modify_time.tv_sec = 0;
			fsap->f_modify_time.tv_nsec = 0;
#if 0
		}
#endif
		VFSATTR_SET_SUPPORTED(fsap, f_modify_time);
	}
	/*
	 * For Carbon compatibility, pretend to support this legacy/unused
	 * attribute
	 */
	if (VFSATTR_IS_ACTIVE(fsap, f_backup_time)) {
		fsap->f_backup_time.tv_sec = 0;
		fsap->f_backup_time.tv_nsec = 0;
		VFSATTR_SET_SUPPORTED(fsap, f_backup_time);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {
		spa_t *spa = dmu_objset_spa(zfsvfs->z_os);
		spa_config_enter(spa, SCL_ALL, FTAG, RW_READER);

		/*
		 * Finder volume name is set to the basename of the mountpoint path,
		 * unless the mountpoint path is "/" or NULL, in which case we use
		 * the f_mntfromname, such as "MyPool/mydataset"
		 */
		char *volname = strrchr(vfs_statfs(zfsvfs->z_vfs)->f_mntonname, '/');
		if (volname && (*(&volname[1]) != '\0')) {
			strlcpy(fsap->f_vol_name, &volname[1], MAXPATHLEN);
		} else {
			strlcpy(fsap->f_vol_name, vfs_statfs(zfsvfs->z_vfs)->f_mntfromname,
				MAXPATHLEN);
		}

		/*
		 * Old MacZFS way. Post OS X 10.6 would show pool name as the
		 * volume name for all mounted datasets in Finder.
		 */
		//strlcpy(fsap->f_vol_name, spa_name(spa), MAXPATHLEN);

		spa_config_exit(spa, SCL_ALL, FTAG);
		VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
	}
	VFSATTR_RETURN(fsap, f_fssubtype, 0);

    /* According to joshade over at
     * https://github.com/joshado/liberate-applefileserver/blob/master/liberate.m
     * the following values need to be returned for it to be considered
     * by Apple's AFS.
     */

	//VFSATTR_RETURN(fsap, f_signature, 0x5a21);  /* 'Z!' */
	//VFSATTR_RETURN(fsap, f_carbon_fsid, 0);
	VFSATTR_RETURN(fsap, f_signature, 18475);  /*  */
	VFSATTR_RETURN(fsap, f_carbon_fsid, 0);


#else /* OpenSolaris */
	statp->f_ffree = MIN(availobjs, statp->f_bfree);
	statp->f_files = statp->f_ffree + usedobjs;

	/*
	 * We're a zfs filesystem.
	 */
	(void) strlcpy(statp->f_fstypename, "zfs", sizeof(statp->f_fstypename));

	strlcpy(statp->f_mntfromname, vfsp->mnt_stat.f_mntfromname,
	    sizeof(statp->f_mntfromname));
	strlcpy(statp->f_mntonname, vfsp->mnt_stat.f_mntonname,
	    sizeof(statp->f_mntonname));

	statp->f_namemax = ZFS_MAXNAMELEN;
#endif

	ZFS_EXIT(zfsvfs);
	return (0);
}

int
zfs_vnode_lock(vnode_t *vp, int flags)
{
	int error;

	ASSERT(vp != NULL);

	error = vn_lock(vp, flags);
	return (error);
}

int
zfs_vfs_root(struct mount *mp, vnode_t **vpp, __unused vfs_context_t context)
{
	zfsvfs_t *zfsvfs = vfs_fsprivate(mp);
	znode_t *rootzp;
	int error;

	ZFS_ENTER_NOERROR(zfsvfs);

	error = zfs_zget(zfsvfs, zfsvfs->z_root, &rootzp);
	if (error == 0)
		*vpp = ZTOV(rootzp);

	ZFS_EXIT(zfsvfs);

#if 0
	if (error == 0) {
		error = zfs_vnode_lock(*vpp, 0);
		if (error == 0)
			(*vpp)->v_vflag |= VV_ROOT;
	}
#endif
	if (error != 0)
		*vpp = NULL;

	return (error);
}

/*
 * Teardown the zfsvfs::z_os.
 *
 * Note, if 'unmounting' if FALSE, we return with the 'z_teardown_lock'
 * and 'z_teardown_inactive_lock' held.
 */
static int
zfsvfs_teardown(zfsvfs_t *zfsvfs, boolean_t unmounting)
{
	znode_t	*zp;

    dprintf("+teardown\n");
	rrw_enter(&zfsvfs->z_teardown_lock, RW_WRITER, FTAG);

	if (!unmounting) {
		/*
		 * We purge the parent filesystem's vfsp as the parent
		 * filesystem and all of its snapshots have their vnode's
		 * v_vfsp set to the parent's filesystem's vfsp.  Note,
		 * 'z_parent' is self referential for non-snapshots.
		 */
		(void) dnlc_purge_vfsp(zfsvfs->z_parent->z_vfs, 0);
#ifdef FREEBSD_NAMECACHE
		cache_purgevfs(zfsvfs->z_parent->z_vfs);
#endif
	}

	/*
	 * If someone has not already unmounted this file system,
	 * drain the iput_taskq to ensure all active references to the
	 * zfs_sb_t have been handled only then can it be safely destroyed.
	 */
	if (zfsvfs->z_os)
		taskq_wait(dsl_pool_iput_taskq(dmu_objset_pool(zfsvfs->z_os)));

	/*
	 * Close the zil. NB: Can't close the zil while zfs_inactive
	 * threads are blocked as zil_close can call zfs_inactive.
	 */
	if (zfsvfs->z_log) {
		zil_close(zfsvfs->z_log);
		zfsvfs->z_log = NULL;
	}

	rw_enter(&zfsvfs->z_teardown_inactive_lock, RW_WRITER);

	/*
	 * If we are not unmounting (ie: online recv) and someone already
	 * unmounted this file system while we were doing the switcheroo,
	 * or a reopen of z_os failed then just bail out now.
	 */
	if (!unmounting && (zfsvfs->z_unmounted || zfsvfs->z_os == NULL)) {
		rw_exit(&zfsvfs->z_teardown_inactive_lock);
		rrw_exit(&zfsvfs->z_teardown_lock, FTAG);
		return (EIO);
	}

	/*
	 * At this point there are no VFS ops active, and any new VFS ops
	 * will fail with EIO since we have z_teardown_lock for writer (only
	 * relevant for forced unmount).
	 *
	 * Release all holds on dbufs.
	 */
	mutex_enter(&zfsvfs->z_znodes_lock);
	for (zp = list_head(&zfsvfs->z_all_znodes); zp != NULL;
	    zp = list_next(&zfsvfs->z_all_znodes, zp))
		if (zp->z_sa_hdl) {
			/* ASSERT(ZTOV(zp)->v_count >= 0); */
			zfs_znode_dmu_fini(zp);
		}
	mutex_exit(&zfsvfs->z_znodes_lock);

	/*
	 * If we are unmounting, set the unmounted flag and let new VFS ops
	 * unblock.  zfs_inactive will have the unmounted behavior, and all
	 * other VFS ops will fail with EIO.
	 */
	if (unmounting) {
		zfsvfs->z_unmounted = B_TRUE;
		rrw_exit(&zfsvfs->z_teardown_lock, FTAG);
		rw_exit(&zfsvfs->z_teardown_inactive_lock);
	}

	/*
	 * z_os will be NULL if there was an error in attempting to reopen
	 * zfsvfs, so just return as the properties had already been
	 * unregistered and cached data had been evicted before.
	 */
	if (zfsvfs->z_os == NULL)
		return (0);

	/*
	 * Unregister properties.
	 */
	zfs_unregister_callbacks(zfsvfs);

	/*
	 * Evict cached data
	 */
	if (dsl_dataset_is_dirty(dmu_objset_ds(zfsvfs->z_os)) &&
	    !(vfs_isrdonly(zfsvfs->z_vfs)))
		txg_wait_synced(dmu_objset_pool(zfsvfs->z_os), 0);
	dmu_objset_evict_dbufs(zfsvfs->z_os);

    dprintf("-teardown\n");
	return (0);
}

/*ARGSUSED*/
int
zfs_vfs_unmount(struct mount *mp, int mntflags, vfs_context_t context)
{
    zfsvfs_t *zfsvfs = vfs_fsprivate(mp);
	//kthread_t *td = (kthread_t *)curthread;
	objset_t *os;
	//cred_t *cr =  (cred_t *)vfs_context_ucred(context);
	int ret;

    dprintf("+unmount\n");

#ifndef __APPLE__
	/*XXX NOEL: delegation admin stuffs, add back if we use delg. admin */
	ret = secpolicy_fs_unmount(cr, zfsvfs->z_vfs);
	if (ret) {
		if (dsl_deleg_access((char *)refstr_value(vfsp->vfs_resource),
		    ZFS_DELEG_PERM_MOUNT, cr))
			return (ret);
	}

	/*
	 * We purge the parent filesystem's vfsp as the parent filesystem
	 * and all of its snapshots have their vnode's v_vfsp set to the
	 * parent's filesystem's vfsp.  Note, 'z_parent' is self
	 * referential for non-snapshots.
	 */
	(void) dnlc_purge_vfsp(zfsvfs->z_parent->z_vfs, 0);
#endif

	/*
	 * Unmount any snapshots mounted under .zfs before unmounting the
	 * dataset itself.
	 */
#ifndef __APPLE__
	if (zfsvfs->z_ctldir != NULL) {
		if ((ret = zfsctl_umount_snapshots(zfsvfs->z_vfs, 0 /*fflag*/, cr)) != 0)
			return (ret);
		ret = vflush(zfsvfs->z_vfs, NULLVP, 0);
		ASSERT(ret == EBUSY);
		if (!(fflag & MS_FORCE)) {
			if (zfsvfs->z_ctldir->v_count > 1)
				return (EBUSY);
			ASSERT(zfsvfs->z_ctldir->v_count == 1);
		}
		zfsctl_destroy(zfsvfs);
		ASSERT(zfsvfs->z_ctldir == NULL);
	}

	if (fflag & MS_FORCE) {
		/*
		 * Mark file system as unmounted before calling
		 * vflush(FORCECLOSE). This way we ensure no future vnops
		 * will be called and risk operating on DOOMED vnodes.
		 */
		rrw_enter(&zfsvfs->z_teardown_lock, RW_WRITER, FTAG);
		zfsvfs->z_unmounted = B_TRUE;
		rrw_exit(&zfsvfs->z_teardown_lock, FTAG);
	}
#endif

	/*
	 * Flush all the files.
	 */
	//ret = vflush(mp, NULLVP, (mntflags & MNT_FORCE) ? FORCECLOSE : 0|SKIPSYSTEM);
	ret = vflush(mp, NULLVP, (mntflags & MNT_FORCE) ? FORCECLOSE|SKIPSYSTEM : SKIPSYSTEM);
	if (ret != 0) {
		if (!zfsvfs->z_issnap) {
			zfsctl_create(zfsvfs);
			ASSERT(zfsvfs->z_ctldir != NULL);
		}
		return (ret);
	}
#ifdef __APPLE__
	/*
	 * Mac OS X needs a file system modify time
	 *
	 * We use the mtime of the "com.apple.system.mtime"
	 * extended attribute, which is associated with the
	 * file system root directory.
	 *
	 * Here we need to release the ref we took on z_mtime_vp during mount.
	 */
#if 0
	if ((ret == 0) || (mntflags & MNT_FORCE)) {
		if (zfsvfs->z_mtime_vp != NULL) {
			vnode_t *mvp;

			mvp = zfsvfs->z_mtime_vp;
			zfsvfs->z_mtime_vp = NULL;

			if (vnode_get(mvp) == 0) {
				//vnode_rele(mvp);
				vnode_recycle(mvp);
				vnode_put(mvp);
			}
		}
	}
#endif

    dprintf("Signalling reclaim sync\n");
	/* We just did final sync, tell reclaim to mop it up */
    cv_signal(&zfsvfs->z_reclaim_thr_cv);
    /* Not the classiest sync control ... */
    delay(hz);


#endif

#ifdef sun
	if (!(fflag & MS_FORCE)) {
		/*
		 * Check the number of active vnodes in the file system.
		 * Our count is maintained in the vfs structure, but the
		 * number is off by 1 to indicate a hold on the vfs
		 * structure itself.
		 *
		 * The '.zfs' directory maintains a reference of its
		 * own, and any active references underneath are
		 * reflected in the vnode count.
		 */
		if (zfsvfs->z_ctldir == NULL) {
			if (vfsp->vfs_count > 1)
				return (EBUSY);
		} else {
			if (vfsp->vfs_count > 2 ||
			    zfsvfs->z_ctldir->v_count > 1)
				return (EBUSY);
		}
	}
#endif

    dprintf("teardown\n");
	VERIFY(zfsvfs_teardown(zfsvfs, B_TRUE) == 0);
	os = zfsvfs->z_os;

    dprintf("OS %p\n", os);
	/*
	 * z_os will be NULL if there was an error in
	 * attempting to reopen zfsvfs.
	 */
	if (os != NULL) {
		/*
		 * Unset the objset user_ptr.
		 */
		mutex_enter(&os->os_user_ptr_lock);
        dprintf("mutex\n");
		dmu_objset_set_user(os, NULL);
        dprintf("set\n");
		mutex_exit(&os->os_user_ptr_lock);

		/*
		 * Finally release the objset
		 */
        dprintf("disown\n");
		dmu_objset_disown(os, zfsvfs);
	}

    dprintf("OS released\n");

	/*
	 * We can now safely destroy the '.zfs' directory node.
	 */
	if (zfsvfs->z_ctldir != NULL)
		zfsctl_destroy(zfsvfs);
#ifndef __APPLE__
	if (zfsvfs->z_issnap) {
		vnode_t *svp = vfsp->mnt_vnodecovered;

		if (svp->v_count >= 2)
			VN_RELE(svp);
	}
#endif
    dprintf("freevfs\n");
	zfs_freevfs(zfsvfs->z_vfs);

    dprintf("-unmount\n");
	return (0);
}

static int
zfs_vget_internal(zfsvfs_t *zfsvfs, ino64_t ino, vnode_t **vpp)
{
	znode_t		*zp;
	int 		err;

    dprintf("vget get %d\n", ino);
	/*
	 * zfs_zget() can't operate on virtual entries like .zfs/ or
	 * .zfs/snapshot/ directories, that's why we return EOPNOTSUPP.
	 * This will make NFS to switch to LOOKUP instead of using VGET.
	 */
	if (ino == ZFSCTL_INO_ROOT || ino == ZFSCTL_INO_SNAPDIR ||
	    (zfsvfs->z_shares_dir != 0 && ino == zfsvfs->z_shares_dir))
		return (EOPNOTSUPP);

    /* We can not be locked during zget. */

	err = zfs_zget(zfsvfs, ino, &zp);

    if (err) {
        dprintf("zget failed %d\n", err);
        return err;
    }

	/* Don't expose EA objects! */
	if (zp->z_pflags & ZFS_XATTR) {
		err = ENOENT;
        goto out;
	}
	if (zp->z_unlinked) {
		err = EINVAL;
        goto out;
	}

    *vpp = ZTOV(zp);

    err = zfs_vnode_lock(*vpp, 0/*flags*/);

    if (vnode_isvroot(*vpp))
        goto out;

 out:
    /*
     * We do not release the vp here in vget, if we do, we panic with io_count
     * != 1
     *
     * VN_RELE(ZTOV(zp));
     */
	if (err != 0)
		*vpp = NULL;
    dprintf("vget return %d\n", err);
	return (err);
}

#ifdef __APPLE__
/*
 * Get a vnode from a file id (ignoring the generation)
 *
 * Use by NFS Server (readdirplus) and VFS (build_path)
 */
int
zfs_vfs_vget(struct mount *mp, ino64_t ino, vnode_t **vpp, __unused vfs_context_t context)
{
	zfsvfs_t *zfsvfs = vfs_fsprivate(mp);
	int error;

	ZFS_ENTER(zfsvfs);

	/*
	 * On Mac OS X we always export the root directory id as 2.
	 * So we don't expect to see the real root directory id
	 * from zfs_vfs_vget KPI (unless of course the real id was
	 * already 2).
	 */
	if ((ino == zfsvfs->z_root) && (zfsvfs->z_root != 2)) {
		ZFS_EXIT(zfsvfs);
		return (ENOENT);
	}
	error = zfs_vget_internal(zfsvfs, ino, vpp);

	ZFS_EXIT(zfsvfs);
	return (error);
}
#endif /* __APPLE__ */


#ifndef __APPLE__
static int
zfs_checkexp(vfs_t *vfsp, struct sockaddr *nam, int *extflagsp,
    struct ucred **credanonp, int *numsecflavors, int **secflavors)
{
	zfsvfs_t *zfsvfs = vfsp->vfs_data;

	/*
	 * If this is regular file system vfsp is the same as
	 * zfsvfs->z_parent->z_vfs, but if it is snapshot,
	 * zfsvfs->z_parent->z_vfs represents parent file system
	 * which we have to use here, because only this file system
	 * has mnt_export configured.
	 */
	return (vfs_stdcheckexp(zfsvfs->z_parent->z_vfs, nam, extflagsp,
	    credanonp, numsecflavors, secflavors));
}

CTASSERT(SHORT_FID_LEN <= sizeof(struct fid));
CTASSERT(LONG_FID_LEN <= sizeof(struct fid));

#endif

#ifdef __APPLE__

int
zfs_vfs_setattr(__unused struct mount *mp, __unused struct vfs_attr *fsap, __unused vfs_context_t context)
{
	// 10a286 bits has an implementation of this
	return (ENOTSUP);
}

/*
 * NFS Server File Handle File ID
 */
typedef struct zfs_zfid {
	uint8_t   zf_object[8];		/* obj[i] = obj >> (8 * i) */
	uint8_t   zf_gen[8];		/* gen[i] = gen >> (8 * i) */
} zfs_zfid_t;
#endif /* __APPLE__ */

#ifdef __APPLE__
/*
 * File handle to vnode pointer
 */
int
zfs_vfs_fhtovp(struct mount *mp, int fhlen, unsigned char *fhp,
               vnode_t **vpp, __unused vfs_context_t context)
{
	zfsvfs_t *zfsvfs = vfs_fsprivate(mp);
	zfs_zfid_t	*zfid = (zfs_zfid_t *)fhp;
	znode_t		*zp;
	uint64_t	obj_num = 0;
	uint64_t	fid_gen = 0;
	uint64_t	zp_gen;
	int 		i;
	int		error;

	*vpp = NULL;

	ZFS_ENTER(zfsvfs);

	if (fhlen < sizeof (zfs_zfid_t)) {
		error = EINVAL;
		goto out;
	}

	/*
	 * Grab the object and gen numbers in an endian neutral manner
	 */
	for (i = 0; i < sizeof (zfid->zf_object); i++)
		obj_num |= ((uint64_t)zfid->zf_object[i]) << (8 * i);

	for (i = 0; i < sizeof (zfid->zf_gen); i++)
		fid_gen |= ((uint64_t)zfid->zf_gen[i]) << (8 * i);

	if ((error = zfs_zget(zfsvfs, obj_num, &zp))) {
		goto out;
	}

	zp_gen = zp->z_gen;
	if (zp_gen == 0)
		zp_gen = 1;

	if (zp->z_unlinked || zp_gen != fid_gen) {
		vnode_put(ZTOV(zp));
		error = EINVAL;
		goto out;
	}
	*vpp = ZTOV(zp);
out:
	ZFS_EXIT(zfsvfs);
	return (error);
}
#endif //__APPLE__

#ifdef __APPLE__
/*
 * Vnode pointer to File handle
 *
 * XXX Do we want to check the DSL sharenfs property?
 */
int
zfs_vfs_vptofh(vnode_t *vp, int *fhlenp, unsigned char *fhp, __unused vfs_context_t context)
{
	zfsvfs_t	*zfsvfs = vfs_fsprivate(vnode_mount(vp));
	zfs_zfid_t	*zfid = (zfs_zfid_t *)fhp;
	znode_t		*zp = VTOZ(vp);
	uint64_t	obj_num;
	uint64_t	zp_gen;
	int		i;
	//int		error;

	if (*fhlenp < sizeof (zfs_zfid_t)) {
		return (EOVERFLOW);
	}

	ZFS_ENTER(zfsvfs);

	obj_num = zp->z_id;
	zp_gen = zp->z_gen;
	if (zp_gen == 0)
		zp_gen = 1;

	/*
	 * Store the object and gen numbers in an endian neutral manner
	 */
	for (i = 0; i < sizeof (zfid->zf_object); i++)
		zfid->zf_object[i] = (uint8_t)(obj_num >> (8 * i));

	for (i = 0; i < sizeof (zfid->zf_gen); i++)
		zfid->zf_gen[i] = (uint8_t)(zp_gen >> (8 * i));

	*fhlenp = sizeof (zfs_zfid_t);

	ZFS_EXIT(zfsvfs);
	return (0);
}
#endif /* __APPLE__ */


/*
 * Block out VOPs and close zfsvfs_t::z_os
 *
 * Note, if successful, then we return with the 'z_teardown_lock' and
 * 'z_teardown_inactive_lock' write held.
 */
int
zfs_suspend_fs(zfsvfs_t *zfsvfs)
{
	int error;

	if ((error = zfsvfs_teardown(zfsvfs, B_FALSE)) != 0)
		return (error);
	dmu_objset_disown(zfsvfs->z_os, zfsvfs);

	return (0);
}

/*
 * Reopen zfsvfs_t::z_os and release VOPs.
 */
int
zfs_resume_fs(zfsvfs_t *zfsvfs, const char *osname)
{
	int err;

	ASSERT(RRW_WRITE_HELD(&zfsvfs->z_teardown_lock));
	ASSERT(RW_WRITE_HELD(&zfsvfs->z_teardown_inactive_lock));

	err = dmu_objset_own(osname, DMU_OST_ZFS, B_FALSE, zfsvfs,
	    &zfsvfs->z_os);
	if (err) {
		zfsvfs->z_os = NULL;
	} else {
		znode_t *zp;
		uint64_t sa_obj = 0;

		/*
		 * Make sure version hasn't changed
		 */

		err = zfs_get_zplprop(zfsvfs->z_os, ZFS_PROP_VERSION,
		    &zfsvfs->z_version);

		if (err)
			goto bail;

		err = zap_lookup(zfsvfs->z_os, MASTER_NODE_OBJ,
		    ZFS_SA_ATTRS, 8, 1, &sa_obj);

		if (err && zfsvfs->z_version >= ZPL_VERSION_SA)
			goto bail;

		if ((err = sa_setup(zfsvfs->z_os, sa_obj,
		    zfs_attr_table,  ZPL_END, &zfsvfs->z_attr_table)) != 0)
			goto bail;

		if (zfsvfs->z_version >= ZPL_VERSION_SA)
			sa_register_update_callback(zfsvfs->z_os,
			    zfs_sa_upgrade);

		VERIFY(zfsvfs_setup(zfsvfs, B_FALSE) == 0);

		zfs_set_fuid_feature(zfsvfs);

		/*
		 * Attempt to re-establish all the active inodes with their
		 * dbufs.  If a zfs_rezget() fails, then we unhash the inode
		 * and mark it stale.  This prevents a collision if a new
		 * inode/object is created which must use the same inode
		 * number.  The stale inode will be be released when the
		 * VFS prunes the dentry holding the remaining references
		 * on the stale inode.
		 */
		mutex_enter(&zfsvfs->z_znodes_lock);
		for (zp = list_head(&zfsvfs->z_all_znodes); zp;
		    zp = list_next(&zfsvfs->z_all_znodes, zp)) {
			(void) zfs_rezget(zp);
		}
		mutex_exit(&zfsvfs->z_znodes_lock);
	}

bail:
	/* release the VOPs */
	rw_exit(&zfsvfs->z_teardown_inactive_lock);
	rrw_exit(&zfsvfs->z_teardown_lock, FTAG);

	if (err) {
		/*
		 * Since we couldn't reopen zfsvfs::z_os, or
		 * setup the sa framework force unmount this file system.
		 */
#ifndef __APPLE__
		if (vn_vfswlock(zfsvfs->z_vfs->vfs_vnodecovered) == 0)
			(void) dounmount(zfsvfs->z_vfs,0 /*MS_FORCE*/, curthread);
#endif
	}
	return (err);
}

void
zfs_freevfs(struct mount *vfsp)
{
	zfsvfs_t *zfsvfs = vfs_fsprivate(vfsp);

    dprintf("+freevfs\n");

#ifdef sun
	/*
	 * If this is a snapshot, we have an extra VFS_HOLD on our parent
	 * from zfs_mount().  Release it here.  If we came through
	 * zfs_mountroot() instead, we didn't grab an extra hold, so
	 * skip the VFS_RELE for rootvfs.
	 */
	if (zfsvfs->z_issnap && (vfsp != rootvfs))
		VFS_RELE(zfsvfs->z_parent->z_vfs);
#endif	/* sun */

	zfsvfs_free(zfsvfs);

	atomic_add_32(&zfs_active_fs_count, -1);
    dprintf("-freevfs\n");
}

#ifdef __i386__
static int desiredvnodes_backup;
#endif

static void
zfs_vnodes_adjust(void)
{
    // What is this?
#ifdef __i386XXX__
	int newdesiredvnodes;

	desiredvnodes_backup = desiredvnodes;

	/*
	 * We calculate newdesiredvnodes the same way it is done in
	 * vntblinit(). If it is equal to desiredvnodes, it means that
	 * it wasn't tuned by the administrator and we can tune it down.
	 */
	newdesiredvnodes = min(maxproc + cnt.v_page_count / 4, 2 *
	    vm_kmem_size / (5 * (sizeof(struct vm_object) +
	    sizeof(struct vnode))));
	if (newdesiredvnodes == desiredvnodes)
		desiredvnodes = (3 * newdesiredvnodes) / 4;
#endif
}

static void
zfs_vnodes_adjust_back(void)
{

#ifdef __i386XXX__
	desiredvnodes = desiredvnodes_backup;
#endif
}

/*
 * VFS_INIT() initialization.  Note that there is no VFS_FINI(),
 * so we can't safely do any non-idempotent initialization here.
 * Leave that to zfs_init() and zfs_fini(), which are called
 * from the module's _init() and _fini() entry points.
 */
/*ARGSUSED*/

static int
zfs_vfsinit(int fstype, char *name)
{
	int error = 0;

//	zfsfstype = fstype;

//	/*
//	 * Setup vfsops and vnodeops tables.
//	 */
//	error = vfs_setfsops(fstype, zfs_vfsops_template, &zfs_vfsops);
//	if (error != 0) {
//		cmn_err(CE_WARN, "zfs: bad vfs ops template");
//	}

//	error = zfs_create_op_tables();
//	if (error) {
//		zfs_remove_op_tables();
//		cmn_err(CE_WARN, "zfs: bad vnode ops template");
//		(void) vfs_freevfsops_by_type(zfsfstype);
//		return (error);
//	}
//
//	mutex_init(&zfs_dev_mtx, NULL, MUTEX_DEFAULT, NULL);

//	/*
//	 * Unique major number for all zfs mounts.
//	 * If we run out of 32-bit minors, we'll getudev() another major.
//	 */
//	zfs_major = ddi_name_to_major(ZFS_DRIVER);
//	zfs_minor = ZFS_MIN_MINOR;

	return (0);
}

void
zfs_init(void)
{

	printf("ZFS filesystem version: " ZPL_VERSION_STRING "\n");

	/*
	 * Initialize .zfs directory structures
	 */
	zfsctl_init();

	/*
	 * Initialize znode cache, vnode ops, etc...
	 */
	zfs_znode_init();

	/*
	 * Reduce number of vnodes. Originally number of vnodes is calculated
	 * with UFS inode in mind. We reduce it here, because it's too big for
	 * ZFS/i386.
	 */
	zfs_vnodes_adjust();

	dmu_objset_register_type(DMU_OST_ZFS, zfs_space_delta_cb);
}

void
zfs_fini(void)
{
	zfsctl_fini();
	zfs_znode_fini();
	zfs_vnodes_adjust_back();
}

int
zfs_busy(void)
{
	return (zfs_active_fs_count != 0);
}

int
zfs_set_version(zfsvfs_t *zfsvfs, uint64_t newvers)
{
	int error;
	objset_t *os = zfsvfs->z_os;
	dmu_tx_t *tx;

	if (newvers < ZPL_VERSION_INITIAL || newvers > ZPL_VERSION)
		return (EINVAL);

	if (newvers < zfsvfs->z_version)
		return (EINVAL);

	if (zfs_spa_version_map(newvers) >
	    spa_version(dmu_objset_spa(zfsvfs->z_os)))
		return (ENOTSUP);

	tx = dmu_tx_create(os);
	dmu_tx_hold_zap(tx, MASTER_NODE_OBJ, B_FALSE, ZPL_VERSION_STR);
	if (newvers >= ZPL_VERSION_SA && !zfsvfs->z_use_sa) {
		dmu_tx_hold_zap(tx, MASTER_NODE_OBJ, B_TRUE,
		    ZFS_SA_ATTRS);
		dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, FALSE, NULL);
	}
	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		return (error);
	}

	error = zap_update(os, MASTER_NODE_OBJ, ZPL_VERSION_STR,
	    8, 1, &newvers, tx);

	if (error) {
		dmu_tx_commit(tx);
		return (error);
	}

	if (newvers >= ZPL_VERSION_SA && !zfsvfs->z_use_sa) {
		uint64_t sa_obj;

		ASSERT3U(spa_version(dmu_objset_spa(zfsvfs->z_os)), >=,
		    SPA_VERSION_SA);
		sa_obj = zap_create(os, DMU_OT_SA_MASTER_NODE,
		    DMU_OT_NONE, 0, tx);

		error = zap_add(os, MASTER_NODE_OBJ,
		    ZFS_SA_ATTRS, 8, 1, &sa_obj, tx);
		ASSERT(error==0);

		VERIFY(0 == sa_set_sa_object(os, sa_obj));
		sa_register_update_callback(os, zfs_sa_upgrade);
	}

	spa_history_log_internal(LOG_DS_UPGRADE,
	    dmu_objset_spa(os), tx,
	    "oldver=%llu newver=%llu dataset = %llu", zfsvfs->z_version, newvers,
	    dmu_objset_id(os));

	dmu_tx_commit(tx);

	zfsvfs->z_version = newvers;

	zfs_set_fuid_feature(zfsvfs);

	return (0);
}

/*
 * Read a property stored within the master node.
 */
int
zfs_get_zplprop(objset_t *os, zfs_prop_t prop, uint64_t *value)
{
	const char *pname;
	int error = ENOENT;

	/*
	 * Look up the file system's value for the property.  For the
	 * version property, we look up a slightly different string.
	 */
	if (prop == ZFS_PROP_VERSION)
		pname = ZPL_VERSION_STR;
	else
		pname = zfs_prop_to_name(prop);

	if (os != NULL)
		error = zap_lookup(os, MASTER_NODE_OBJ, pname, 8, 1, value);

	if (error == ENOENT) {
		/* No value set, use the default value */
		switch (prop) {
		case ZFS_PROP_VERSION:
			*value = ZPL_VERSION;
			break;
		case ZFS_PROP_NORMALIZE:
		case ZFS_PROP_UTF8ONLY:
			*value = 0;
			break;
		case ZFS_PROP_CASE:
			*value = ZFS_CASE_SENSITIVE;
			break;
		default:
			return (error);
		}
		error = 0;
	}
	return (error);
}

#ifdef _KERNEL
void
zfsvfs_update_fromname(const char *oldname, const char *newname)
{
#if 0
	char tmpbuf[MAXPATHLEN];
	struct mount *mp;
	char *fromname;
	size_t oldlen;

	oldlen = strlen(oldname);

	mtx_lock(&mountlist_mtx);
	TAILQ_FOREACH(mp, &mountlist, mnt_list) {
		fromname = mp->mnt_stat.f_mntfromname;
		if (strcmp(fromname, oldname) == 0) {
			(void)strlcpy(fromname, newname,
			    sizeof(mp->mnt_stat.f_mntfromname));
			continue;
		}
		if (strncmp(fromname, oldname, oldlen) == 0 &&
		    (fromname[oldlen] == '/' || fromname[oldlen] == '@')) {
			(void)snprintf(tmpbuf, sizeof(tmpbuf), "%s%s",
			    newname, fromname + oldlen);
			(void)strlcpy(fromname, tmpbuf,
			    sizeof(mp->mnt_stat.f_mntfromname));
			continue;
		}
	}
	mtx_unlock(&mountlist_mtx);
#endif

}
#endif
