// SPDX-License-Identifier: GPL-2.0
/*
 * NFS server file handle treatment.
 *
 * Copyright (C) 1995, 1996 Olaf Kirch <okir@monad.swb.de>
 * Portions Copyright (C) 1999 G. Allen Morris III <gam3@acm.org>
 * Extensive rewrite by Neil Brown <neilb@cse.unsw.edu.au> Southern-Spring 1999
 * ... and again Southern-Winter 2001 to support export_operations
 */

#include <linux/exportfs.h>

#include <linux/sunrpc/svcauth_gss.h>
#include "nfsd.h"
#include "vfs.h"
#include "auth.h"
#include "trace.h"

#define NFSDDBG_FACILITY		NFSDDBG_FH


/*
 * our acceptability function.
 * if NOSUBTREECHECK, accept anything
 * if not, require that we can walk up to exp->ex_dentry
 * doing some checks on the 'x' bits
 */
static int nfsd_acceptable(void *expv, struct dentry *dentry)
{
	struct svc_export *exp = expv;
	int rv;
	struct dentry *tdentry;
	struct dentry *parent;

	if (exp->ex_flags & NFSEXP_NOSUBTREECHECK)
		return 1;

	tdentry = dget(dentry);
	while (tdentry != exp->ex_path.dentry && !IS_ROOT(tdentry)) {
		/* make sure parents give x permission to user */
		int err;
		parent = dget_parent(tdentry);
		err = inode_permission(&nop_mnt_idmap,
				       d_inode(parent), MAY_EXEC);
		if (err < 0) {
			dput(parent);
			break;
		}
		dput(tdentry);
		tdentry = parent;
	}
	if (tdentry != exp->ex_path.dentry)
		dprintk("nfsd_acceptable failed at %p %pd\n", tdentry, tdentry);
	rv = (tdentry == exp->ex_path.dentry);
	dput(tdentry);
	return rv;
}

/* Type check. The correct error return for type mismatches does not seem to be
 * generally agreed upon. SunOS seems to use EISDIR if file isn't S_IFREG; a
 * comment in the NFSv3 spec says this is incorrect (implementation notes for
 * the write call).
 */
static inline __be32
nfsd_mode_check(struct dentry *dentry, umode_t requested)
{
	umode_t mode = d_inode(dentry)->i_mode & S_IFMT;

	if (requested == 0) /* the caller doesn't care */
		return nfs_ok;
	if (mode == requested) {
		if (mode == S_IFDIR && !d_can_lookup(dentry)) {
			WARN_ON_ONCE(1);
			return nfserr_notdir;
		}
		return nfs_ok;
	}
	if (mode == S_IFLNK) {
		if (requested == S_IFDIR)
			return nfserr_symlink_not_dir;
		return nfserr_symlink;
	}
	if (requested == S_IFDIR)
		return nfserr_notdir;
	if (mode == S_IFDIR)
		return nfserr_isdir;
	return nfserr_wrong_type;
}

static bool nfsd_originating_port_ok(struct svc_rqst *rqstp,
				     struct svc_cred *cred,
				     struct svc_export *exp)
{
	if (nfsexp_flags(cred, exp) & NFSEXP_INSECURE_PORT)
		return true;
	/* We don't require gss requests to use low ports: */
	if (cred->cr_flavor >= RPC_AUTH_GSS)
		return true;
	return test_bit(RQ_SECURE, &rqstp->rq_flags);
}

static __be32 nfsd_setuser_and_check_port(struct svc_rqst *rqstp,
					  struct svc_cred *cred,
					  struct svc_export *exp)
{
	/* Check if the request originated from a secure port. */
	if (rqstp && !nfsd_originating_port_ok(rqstp, cred, exp)) {
		RPC_IFDEBUG(char buf[RPC_MAX_ADDRBUFLEN]);
		dprintk("nfsd: request from insecure port %s!\n",
		        svc_print_addr(rqstp, buf, sizeof(buf)));
		return nfserr_perm;
	}

	/* Set user creds for this exportpoint */
	return nfserrno(nfsd_setuser(cred, exp));
}

static inline __be32 check_pseudo_root(struct dentry *dentry,
				       struct svc_export *exp)
{
	if (!(exp->ex_flags & NFSEXP_V4ROOT))
		return nfs_ok;
	/*
	 * We're exposing only the directories and symlinks that have to be
	 * traversed on the way to real exports:
	 */
	if (unlikely(!d_is_dir(dentry) &&
		     !d_is_symlink(dentry)))
		return nfserr_stale;
	/*
	 * A pseudoroot export gives permission to access only one
	 * single directory; the kernel has to make another upcall
	 * before granting access to anything else under it:
	 */
	if (unlikely(dentry != exp->ex_path.dentry))
		return nfserr_stale;
	return nfs_ok;
}

/*
 * Use the given filehandle to look up the corresponding export and
 * dentry.  On success, the results are used to set fh_export and
 * fh_dentry.
 */
static __be32 nfsd_set_fh_dentry(struct svc_rqst *rqstp, struct net *net,
				 struct svc_cred *cred,
				 struct auth_domain *client,
				 struct auth_domain *gssclient,
				 struct svc_fh *fhp)
{
	struct knfsd_fh	*fh = &fhp->fh_handle;
	struct fid *fid = NULL;
	struct svc_export *exp;
	struct dentry *dentry;
	int fileid_type;
	int data_left = fh->fh_size/4;
	int len;
	__be32 error;

	error = nfserr_badhandle;
	if (fh->fh_size == 0)
		return nfserr_nofilehandle;

	if (fh->fh_version != 1)
		return error;

	if (--data_left < 0)
		return error;
	if (fh->fh_auth_type != 0)
		return error;
	len = key_len(fh->fh_fsid_type) / 4;
	if (len == 0)
		return error;
	if (fh->fh_fsid_type == FSID_MAJOR_MINOR) {
		u32 *fsid = fh_fsid(fh);

		/* deprecated, convert to type 3 */
		len = key_len(FSID_ENCODE_DEV)/4;
		fh->fh_fsid_type = FSID_ENCODE_DEV;
		/*
		 * struct knfsd_fh uses host-endian fields, which are
		 * sometimes used to hold net-endian values. This
		 * confuses sparse, so we must use __force here to
		 * keep it from complaining.
		 */
		fsid[0] = new_encode_dev(MKDEV(ntohl((__force __be32)fsid[0]),
					       ntohl((__force __be32)fsid[1])));
		fsid[1] = fsid[2];
	}
	data_left -= len;
	if (data_left < 0)
		return error;
	exp = rqst_exp_find(rqstp ? &rqstp->rq_chandle : NULL,
			    net, client, gssclient,
			    fh->fh_fsid_type, fh_fsid(fh));
	fid = (struct fid *)(fh_fsid(fh) + len);

	error = nfserr_stale;
	if (IS_ERR(exp)) {
		trace_nfsd_set_fh_dentry_badexport(rqstp, fhp, PTR_ERR(exp));

		if (PTR_ERR(exp) == -ENOENT)
			return error;

		return nfserrno(PTR_ERR(exp));
	}

	if (exp->ex_flags & NFSEXP_NOSUBTREECHECK) {
		/* Elevate privileges so that the lack of 'r' or 'x'
		 * permission on some parent directory will
		 * not stop exportfs_decode_fh from being able
		 * to reconnect a directory into the dentry cache.
		 * The same problem can affect "SUBTREECHECK" exports,
		 * but as nfsd_acceptable depends on correct
		 * access control settings being in effect, we cannot
		 * fix that case easily.
		 */
		struct cred *new = prepare_creds();
		if (!new) {
			error =  nfserrno(-ENOMEM);
			goto out;
		}
		new->cap_effective =
			cap_raise_nfsd_set(new->cap_effective,
					   new->cap_permitted);
		put_cred(override_creds(new));
	} else {
		error = nfsd_setuser_and_check_port(rqstp, cred, exp);
		if (error)
			goto out;
	}

	/*
	 * Look up the dentry using the NFS file handle.
	 */
	error = nfserr_badhandle;

	fileid_type = fh->fh_fileid_type;

	if (fileid_type == FILEID_ROOT)
		dentry = dget(exp->ex_path.dentry);
	else {
		dentry = exportfs_decode_fh_raw(exp->ex_path.mnt, fid,
						data_left, fileid_type, 0,
						nfsd_acceptable, exp);
		if (IS_ERR_OR_NULL(dentry)) {
			trace_nfsd_set_fh_dentry_badhandle(rqstp, fhp,
					dentry ?  PTR_ERR(dentry) : -ESTALE);
			switch (PTR_ERR(dentry)) {
			case -ENOMEM:
			case -ETIMEDOUT:
				break;
			default:
				dentry = ERR_PTR(-ESTALE);
			}
		}
	}
	if (dentry == NULL)
		goto out;
	if (IS_ERR(dentry)) {
		if (PTR_ERR(dentry) != -EINVAL)
			error = nfserrno(PTR_ERR(dentry));
		goto out;
	}

	if (d_is_dir(dentry) &&
			(dentry->d_flags & DCACHE_DISCONNECTED)) {
		printk("nfsd: find_fh_dentry returned a DISCONNECTED directory: %pd2\n",
				dentry);
	}

	fhp->fh_dentry = dentry;
	fhp->fh_export = exp;

	switch (fhp->fh_maxsize) {
	case NFS4_FHSIZE:
		if (dentry->d_sb->s_export_op->flags & EXPORT_OP_NOATOMIC_ATTR)
			fhp->fh_no_atomic_attr = true;
		fhp->fh_64bit_cookies = true;
		break;
	case NFS3_FHSIZE:
		if (dentry->d_sb->s_export_op->flags & EXPORT_OP_NOWCC)
			fhp->fh_no_wcc = true;
		fhp->fh_64bit_cookies = true;
		if (exp->ex_flags & NFSEXP_V4ROOT)
			goto out;
		break;
	case NFS_FHSIZE:
		fhp->fh_no_wcc = true;
		if (EX_WGATHER(exp))
			fhp->fh_use_wgather = true;
		if (exp->ex_flags & NFSEXP_V4ROOT)
			goto out;
	}

	return 0;
out:
	exp_put(exp);
	return error;
}

/**
 * __fh_verify - filehandle lookup and access checking
 * @rqstp: RPC transaction context, or NULL
 * @net: net namespace in which to perform the export lookup
 * @cred: RPC user credential
 * @client: RPC auth domain
 * @gssclient: RPC GSS auth domain, or NULL
 * @fhp: filehandle to be verified
 * @type: expected type of object pointed to by filehandle
 * @access: type of access needed to object
 *
 * See fh_verify() for further descriptions of @fhp, @type, and @access.
 */
static __be32
__fh_verify(struct svc_rqst *rqstp,
	    struct net *net, struct svc_cred *cred,
	    struct auth_domain *client,
	    struct auth_domain *gssclient,
	    struct svc_fh *fhp, umode_t type, int access)
{
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	struct svc_export *exp = NULL;
	bool may_bypass_gss = false;
	struct dentry	*dentry;
	__be32		error;

	if (!fhp->fh_dentry) {
		error = nfsd_set_fh_dentry(rqstp, net, cred, client,
					   gssclient, fhp);
		if (error)
			goto out;
	}
	dentry = fhp->fh_dentry;
	exp = fhp->fh_export;

	trace_nfsd_fh_verify(rqstp, fhp, type, access);

	/*
	 * We still have to do all these permission checks, even when
	 * fh_dentry is already set:
	 * 	- fh_verify may be called multiple times with different
	 * 	  "access" arguments (e.g. nfsd_proc_create calls
	 * 	  fh_verify(...,NFSD_MAY_EXEC) first, then later (in
	 * 	  nfsd_create) calls fh_verify(...,NFSD_MAY_CREATE).
	 *	- in the NFSv4 case, the filehandle may have been filled
	 *	  in by fh_compose, and given a dentry, but further
	 *	  compound operations performed with that filehandle
	 *	  still need permissions checks.  In the worst case, a
	 *	  mountpoint crossing may have changed the export
	 *	  options, and we may now need to use a different uid
	 *	  (for example, if different id-squashing options are in
	 *	  effect on the new filesystem).
	 */
	error = check_pseudo_root(dentry, exp);
	if (error)
		goto out;

	error = nfsd_setuser_and_check_port(rqstp, cred, exp);
	if (error)
		goto out;

	error = nfsd_mode_check(dentry, type);
	if (error)
		goto out;

	if ((access & NFSD_MAY_NLM) && (exp->ex_flags & NFSEXP_NOAUTHNLM))
		/* NLM is allowed to fully bypass authentication */
		goto out;

	if (access & NFSD_MAY_BYPASS_GSS)
		may_bypass_gss = true;
	/*
	 * Clients may expect to be able to use auth_sys during mount,
	 * even if they use gss for everything else; see section 2.3.2
	 * of rfc 2623.
	 */
	if (access & NFSD_MAY_BYPASS_GSS_ON_ROOT
			&& exp->ex_path.dentry == dentry)
		may_bypass_gss = true;

	error = check_nfsd_access(exp, rqstp, may_bypass_gss);
	if (error)
		goto out;
	/* During LOCALIO call to fh_verify will be called with a NULL rqstp */
	if (rqstp)
		svc_xprt_set_valid(rqstp->rq_xprt);

	/* Finally, check access permissions. */
	error = nfsd_permission(cred, exp, dentry, access);
out:
	trace_nfsd_fh_verify_err(rqstp, fhp, type, access, error);
	if (error == nfserr_stale)
		nfsd_stats_fh_stale_inc(nn, exp);
	return error;
}

/**
 * fh_verify_local - filehandle lookup and access checking
 * @net: net namespace in which to perform the export lookup
 * @cred: RPC user credential
 * @client: RPC auth domain
 * @fhp: filehandle to be verified
 * @type: expected type of object pointed to by filehandle
 * @access: type of access needed to object
 *
 * This API can be used by callers who do not have an RPC
 * transaction context (ie are not running in an nfsd thread).
 *
 * See fh_verify() for further descriptions of @fhp, @type, and @access.
 */
__be32
fh_verify_local(struct net *net, struct svc_cred *cred,
		struct auth_domain *client, struct svc_fh *fhp,
		umode_t type, int access)
{
	return __fh_verify(NULL, net, cred, client, NULL,
			   fhp, type, access);
}

/**
 * fh_verify - filehandle lookup and access checking
 * @rqstp: pointer to current rpc request
 * @fhp: filehandle to be verified
 * @type: expected type of object pointed to by filehandle
 * @access: type of access needed to object
 *
 * Look up a dentry from the on-the-wire filehandle, check the client's
 * access to the export, and set the current task's credentials.
 *
 * Regardless of success or failure of fh_verify(), fh_put() should be
 * called on @fhp when the caller is finished with the filehandle.
 *
 * fh_verify() may be called multiple times on a given filehandle, for
 * example, when processing an NFSv4 compound.  The first call will look
 * up a dentry using the on-the-wire filehandle.  Subsequent calls will
 * skip the lookup and just perform the other checks and possibly change
 * the current task's credentials.
 *
 * @type specifies the type of object expected using one of the S_IF*
 * constants defined in include/linux/stat.h.  The caller may use zero
 * to indicate that it doesn't care, or a negative integer to indicate
 * that it expects something not of the given type.
 *
 * @access is formed from the NFSD_MAY_* constants defined in
 * fs/nfsd/vfs.h.
 */
__be32
fh_verify(struct svc_rqst *rqstp, struct svc_fh *fhp, umode_t type, int access)
{
	return __fh_verify(rqstp, SVC_NET(rqstp), &rqstp->rq_cred,
			   rqstp->rq_client, rqstp->rq_gssclient,
			   fhp, type, access);
}

/*
 * Compose a file handle for an NFS reply.
 *
 * Note that when first composed, the dentry may not yet have
 * an inode.  In this case a call to fh_update should be made
 * before the fh goes out on the wire ...
 */
static void _fh_update(struct svc_fh *fhp, struct svc_export *exp,
		struct dentry *dentry)
{
	if (dentry != exp->ex_path.dentry) {
		struct fid *fid = (struct fid *)
			(fh_fsid(&fhp->fh_handle) + fhp->fh_handle.fh_size/4 - 1);
		int maxsize = (fhp->fh_maxsize - fhp->fh_handle.fh_size)/4;
		int fh_flags = (exp->ex_flags & NFSEXP_NOSUBTREECHECK) ? 0 :
				EXPORT_FH_CONNECTABLE;
		int fileid_type =
			exportfs_encode_fh(dentry, fid, &maxsize, fh_flags);

		fhp->fh_handle.fh_fileid_type =
			fileid_type > 0 ? fileid_type : FILEID_INVALID;
		fhp->fh_handle.fh_size += maxsize * 4;
	} else {
		fhp->fh_handle.fh_fileid_type = FILEID_ROOT;
	}
}

static bool is_root_export(struct svc_export *exp)
{
	return exp->ex_path.dentry == exp->ex_path.dentry->d_sb->s_root;
}

static struct super_block *exp_sb(struct svc_export *exp)
{
	return exp->ex_path.dentry->d_sb;
}

static bool fsid_type_ok_for_exp(u8 fsid_type, struct svc_export *exp)
{
	switch (fsid_type) {
	case FSID_DEV:
		if (!old_valid_dev(exp_sb(exp)->s_dev))
			return false;
		fallthrough;
	case FSID_MAJOR_MINOR:
	case FSID_ENCODE_DEV:
		return exp_sb(exp)->s_type->fs_flags & FS_REQUIRES_DEV;
	case FSID_NUM:
		return exp->ex_flags & NFSEXP_FSID;
	case FSID_UUID8:
	case FSID_UUID16:
		if (!is_root_export(exp))
			return false;
		fallthrough;
	case FSID_UUID4_INUM:
	case FSID_UUID16_INUM:
		return exp->ex_uuid != NULL;
	}
	return true;
}


static void set_version_and_fsid_type(struct svc_fh *fhp, struct svc_export *exp, struct svc_fh *ref_fh)
{
	u8 version;
	u8 fsid_type;
retry:
	version = 1;
	if (ref_fh && ref_fh->fh_export == exp) {
		version = ref_fh->fh_handle.fh_version;
		fsid_type = ref_fh->fh_handle.fh_fsid_type;

		ref_fh = NULL;

		switch (version) {
		case 0xca:
			fsid_type = FSID_DEV;
			break;
		case 1:
			break;
		default:
			goto retry;
		}

		/*
		 * As the fsid -> filesystem mapping was guided by
		 * user-space, there is no guarantee that the filesystem
		 * actually supports that fsid type. If it doesn't we
		 * loop around again without ref_fh set.
		 */
		if (!fsid_type_ok_for_exp(fsid_type, exp))
			goto retry;
	} else if (exp->ex_flags & NFSEXP_FSID) {
		fsid_type = FSID_NUM;
	} else if (exp->ex_uuid) {
		if (fhp->fh_maxsize >= 64) {
			if (is_root_export(exp))
				fsid_type = FSID_UUID16;
			else
				fsid_type = FSID_UUID16_INUM;
		} else {
			if (is_root_export(exp))
				fsid_type = FSID_UUID8;
			else
				fsid_type = FSID_UUID4_INUM;
		}
	} else if (!old_valid_dev(exp_sb(exp)->s_dev))
		/* for newer device numbers, we must use a newer fsid format */
		fsid_type = FSID_ENCODE_DEV;
	else
		fsid_type = FSID_DEV;
	fhp->fh_handle.fh_version = version;
	if (version)
		fhp->fh_handle.fh_fsid_type = fsid_type;
}

__be32
fh_compose(struct svc_fh *fhp, struct svc_export *exp, struct dentry *dentry,
	   struct svc_fh *ref_fh)
{
	/* ref_fh is a reference file handle.
	 * if it is non-null and for the same filesystem, then we should compose
	 * a filehandle which is of the same version, where possible.
	 */

	struct inode * inode = d_inode(dentry);
	dev_t ex_dev = exp_sb(exp)->s_dev;

	dprintk("nfsd: fh_compose(exp %02x:%02x/%ld %pd2, ino=%ld)\n",
		MAJOR(ex_dev), MINOR(ex_dev),
		(long) d_inode(exp->ex_path.dentry)->i_ino,
		dentry,
		(inode ? inode->i_ino : 0));

	/* Choose filehandle version and fsid type based on
	 * the reference filehandle (if it is in the same export)
	 * or the export options.
	 */
	set_version_and_fsid_type(fhp, exp, ref_fh);

	/* If we have a ref_fh, then copy the fh_no_wcc setting from it. */
	fhp->fh_no_wcc = ref_fh ? ref_fh->fh_no_wcc : false;

	if (ref_fh == fhp)
		fh_put(ref_fh);

	if (fhp->fh_dentry) {
		printk(KERN_ERR "fh_compose: fh %pd2 not initialized!\n",
		       dentry);
	}
	if (fhp->fh_maxsize < NFS_FHSIZE)
		printk(KERN_ERR "fh_compose: called with maxsize %d! %pd2\n",
		       fhp->fh_maxsize,
		       dentry);

	fhp->fh_dentry = dget(dentry); /* our internal copy */
	fhp->fh_export = exp_get(exp);

	fhp->fh_handle.fh_size =
		key_len(fhp->fh_handle.fh_fsid_type) + 4;
	fhp->fh_handle.fh_auth_type = 0;

	mk_fsid(fhp->fh_handle.fh_fsid_type,
		fh_fsid(&fhp->fh_handle),
		ex_dev,
		d_inode(exp->ex_path.dentry)->i_ino,
		exp->ex_fsid, exp->ex_uuid);

	if (inode)
		_fh_update(fhp, exp, dentry);
	if (fhp->fh_handle.fh_fileid_type == FILEID_INVALID) {
		fh_put(fhp);
		return nfserr_stale;
	}

	return 0;
}

/*
 * Update file handle information after changing a dentry.
 * This is only called by nfsd_create, nfsd_create_v3 and nfsd_proc_create
 */
__be32
fh_update(struct svc_fh *fhp)
{
	struct dentry *dentry;

	if (!fhp->fh_dentry)
		goto out_bad;

	dentry = fhp->fh_dentry;
	if (d_really_is_negative(dentry))
		goto out_negative;
	if (fhp->fh_handle.fh_fileid_type != FILEID_ROOT)
		return 0;

	_fh_update(fhp, fhp->fh_export, dentry);
	if (fhp->fh_handle.fh_fileid_type == FILEID_INVALID)
		return nfserr_stale;
	return 0;
out_bad:
	printk(KERN_ERR "fh_update: fh not verified!\n");
	return nfserr_serverfault;
out_negative:
	printk(KERN_ERR "fh_update: %pd2 still negative!\n",
		dentry);
	return nfserr_serverfault;
}

/**
 * fh_fill_pre_attrs - Fill in pre-op attributes
 * @fhp: file handle to be updated
 *
 */
__be32 __must_check fh_fill_pre_attrs(struct svc_fh *fhp)
{
	bool v4 = (fhp->fh_maxsize == NFS4_FHSIZE);
	struct kstat stat;
	__be32 err;

	if (fhp->fh_no_wcc || fhp->fh_pre_saved)
		return nfs_ok;

	err = fh_getattr(fhp, &stat);
	if (err)
		return err;

	if (v4)
		fhp->fh_pre_change = nfsd4_change_attribute(&stat);

	fhp->fh_pre_mtime = stat.mtime;
	fhp->fh_pre_ctime = stat.ctime;
	fhp->fh_pre_size  = stat.size;
	fhp->fh_pre_saved = true;
	return nfs_ok;
}

/**
 * fh_fill_post_attrs - Fill in post-op attributes
 * @fhp: file handle to be updated
 *
 */
__be32 fh_fill_post_attrs(struct svc_fh *fhp)
{
	bool v4 = (fhp->fh_maxsize == NFS4_FHSIZE);
	__be32 err;

	if (fhp->fh_no_wcc)
		return nfs_ok;

	if (fhp->fh_post_saved)
		printk("nfsd: inode locked twice during operation.\n");

	err = fh_getattr(fhp, &fhp->fh_post_attr);
	if (err)
		return err;

	fhp->fh_post_saved = true;
	if (v4)
		fhp->fh_post_change =
			nfsd4_change_attribute(&fhp->fh_post_attr);
	return nfs_ok;
}

/**
 * fh_fill_both_attrs - Fill pre-op and post-op attributes
 * @fhp: file handle to be updated
 *
 * This is used when the directory wasn't changed, but wcc attributes
 * are needed anyway.
 */
__be32 __must_check fh_fill_both_attrs(struct svc_fh *fhp)
{
	__be32 err;

	err = fh_fill_post_attrs(fhp);
	if (err)
		return err;

	fhp->fh_pre_change = fhp->fh_post_change;
	fhp->fh_pre_mtime = fhp->fh_post_attr.mtime;
	fhp->fh_pre_ctime = fhp->fh_post_attr.ctime;
	fhp->fh_pre_size = fhp->fh_post_attr.size;
	fhp->fh_pre_saved = true;
	return nfs_ok;
}

/*
 * Release a file handle.
 */
void
fh_put(struct svc_fh *fhp)
{
	struct dentry * dentry = fhp->fh_dentry;
	struct svc_export * exp = fhp->fh_export;
	if (dentry) {
		fhp->fh_dentry = NULL;
		dput(dentry);
		fh_clear_pre_post_attrs(fhp);
	}
	fh_drop_write(fhp);
	if (exp) {
		exp_put(exp);
		fhp->fh_export = NULL;
	}
	fhp->fh_no_wcc = false;
	return;
}

/*
 * Shorthand for dprintk()'s
 */
char * SVCFH_fmt(struct svc_fh *fhp)
{
	struct knfsd_fh *fh = &fhp->fh_handle;
	static char buf[2+1+1+64*3+1];

	if (fh->fh_size > 64)
		return "bad-fh";
	sprintf(buf, "%d: %*ph", fh->fh_size, fh->fh_size, fh->fh_raw);
	return buf;
}

enum fsid_source fsid_source(const struct svc_fh *fhp)
{
	if (fhp->fh_handle.fh_version != 1)
		return FSIDSOURCE_DEV;
	switch(fhp->fh_handle.fh_fsid_type) {
	case FSID_DEV:
	case FSID_ENCODE_DEV:
	case FSID_MAJOR_MINOR:
		if (exp_sb(fhp->fh_export)->s_type->fs_flags & FS_REQUIRES_DEV)
			return FSIDSOURCE_DEV;
		break;
	case FSID_NUM:
		if (fhp->fh_export->ex_flags & NFSEXP_FSID)
			return FSIDSOURCE_FSID;
		break;
	default:
		break;
	}
	/* either a UUID type filehandle, or the filehandle doesn't
	 * match the export.
	 */
	if (fhp->fh_export->ex_flags & NFSEXP_FSID)
		return FSIDSOURCE_FSID;
	if (fhp->fh_export->ex_uuid)
		return FSIDSOURCE_UUID;
	return FSIDSOURCE_DEV;
}

/**
 * nfsd4_change_attribute - Generate an NFSv4 change_attribute value
 * @stat: inode attributes
 *
 * Caller must fill in @stat before calling, typically by invoking
 * vfs_getattr() with STATX_MODE, STATX_CTIME, and STATX_CHANGE_COOKIE.
 * Returns an unsigned 64-bit changeid4 value (RFC 8881 Section 3.2).
 *
 * We could use i_version alone as the change attribute.  However, i_version
 * can go backwards on a regular file after an unclean shutdown.  On its own
 * that doesn't necessarily cause a problem, but if i_version goes backwards
 * and then is incremented again it could reuse a value that was previously
 * used before boot, and a client who queried the two values might incorrectly
 * assume nothing changed.
 *
 * By using both ctime and the i_version counter we guarantee that as long as
 * time doesn't go backwards we never reuse an old value. If the filesystem
 * advertises STATX_ATTR_CHANGE_MONOTONIC, then this mitigation is not
 * needed.
 *
 * We only need to do this for regular files as well. For directories, we
 * assume that the new change attr is always logged to stable storage in some
 * fashion before the results can be seen.
 */
u64 nfsd4_change_attribute(const struct kstat *stat)
{
	u64 chattr;

	if (stat->result_mask & STATX_CHANGE_COOKIE) {
		chattr = stat->change_cookie;
		if (S_ISREG(stat->mode) &&
		    !(stat->attributes & STATX_ATTR_CHANGE_MONOTONIC)) {
			chattr += (u64)stat->ctime.tv_sec << 30;
			chattr += stat->ctime.tv_nsec;
		}
	} else {
		chattr = time_to_chattr(&stat->ctime);
	}
	return chattr;
}
