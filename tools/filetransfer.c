
#include "common.h"
#include "utils.h"
#include "filetransfer.h"
#include "globbing.h"

static void ft_del(GS_LIST_ITEM *li);
static void free_get_li(GS_LIST_ITEM *li);
static void ft_done(GS_FT *ft);
static void qerr_add(GS_FT *ft, uint32_t id, uint8_t code, const char *str);
static void mk_stats_total(GS_FT *ft);
static void mk_stats(GS_FT *ft, uint32_t id, struct _gs_ft_file *f, const char *fname, int err);
static mode_t GS_fperm2mode(uint32_t u);
static uint32_t GS_mode2fperm(mode_t m);
static void update_stats(struct _gs_ft_file *f, size_t sz);
static const char *str_dotslash(const char *src);
static const char *str_stripslash(const char *src);
static int mkdirp(const char *dir, mode_t mode);





#if 0
0 FIXME: must make sure that server cant request transfer from client!
- test when file becomes unavaialble after it was added.
- Sym Link
- max buffer size 64 macro somewhere needed
- queue up all 'put' requests and send as 1 large message or loop around write() while we can.
- What do we do if a pathname/filename does not fit into wbuf? Filename can be rather long (like 4k?)
  and larger than channel buffer
  (add files until it does not fit. Return to caller the ID that failed and let caller decide
  if he likes to remove that ID from our list or just let caller try again until it fits...)
  is there a limt?
- empty directories
- fnmatch for receiving files (hehe. yes please).
STOP HERE: 
- Test '/usr/./share/' downloads etc
- deal with server sending /etc/hosts or ../../ shit (strchr?)

TEST CASES:
1. pathname + filename 4096 long
#2. dest file not writeable
#3. re-start transmission
4. same file in command line
#5. src file can not be opened or read error.
#6. write to symlink - as per unix (follow symlinks)
#7. zero file size
#8. retain timestamp
#endif

void
GS_FT_init(GS_FT *ft, gsft_cb_stats_t func_stats, gsft_cb_status_t func_status, int is_server)
{
	memset(ft, 0, sizeof *ft);

	// PUT (upload) - Client side
	GS_LIST_init(&ft->fqueue, 0);
	GS_LIST_init(&ft->fputs, 0);
	GS_LIST_init(&ft->faccepted, 0);
	GS_LIST_init(&ft->fcompleted, 0);
	// PUT (upload) - Server Side
	GS_LIST_init(&ft->fadded, 0);
	GS_LIST_init(&ft->freceiving, 0);

	// GET (download) - Client side
	GS_LIST_init(&ft->plistreq, 0);
	GS_LIST_init(&ft->plistreq_waiting, 0);
	GS_LIST_init(&ft->flist, 0);
	GS_LIST_init(&ft->fdl_waiting, 0);
	// GET (download) - Server side
	GS_LIST_init(&ft->flistreply, 0);
	GS_LIST_init(&ft->fdl, 0);

	GS_LIST_init(&ft->qerrs, 0);

	ft->func_stats = func_stats;
	ft->func_status = func_status;
	ft->is_server = is_server;
}

static struct _gs_ft_file *
file_new(const char *fname, const char *fn_local, int64_t fz_local, int64_t fz_remote, uint32_t mtime, uint32_t fperm)
{
	struct _gs_ft_file *f;
	f = calloc(1, sizeof *f);

	f->mode = fperm;
	f->mtime = mtime;
	f->fz_local = fz_local;
	f->fz_remote = fz_remote;

	if (fn_local != NULL)
	{
		// CLIENT
		f->realname = strdup(fn_local);
	} else {
		// SERVER (put & get)
		f->realname = strdup(fname);
	}

	f->name = strdup(fname);

	return f;
}

// SERVER. put & get
static int
gs_ft_add_file(GS_FT *ft, GS_LIST *gsl, uint32_t id, const char *fname, uint32_t mtime, uint32_t fperm, int64_t fz_remote, uint8_t flags)
{
	struct stat res;
	int ret;
	int64_t fz = 0;

	// First: Directory struture.
	if (flags & GS_FT_FL_ISDIR)
	{
		// mkdirp() will remove any ordinary file that is in its way
		if (mkdirp(fname, GS_fperm2mode(fperm)) != 0)
		{
			DEBUGF_R("mkdir(%s): %s\n", fname, strerror(errno));
			qerr_add(ft, id, GS_FT_ERR_PERM, NULL);
			return -GS_FT_ERR_PERM;
		}

		// HERE: Directory (not file). Return immediately.
		return 0;
	}

	// HERE: Remote is sending an ordinary FILE (not directory)
	ret = stat(fname, &res);
	if (ret != 0)
	{
		if (errno != ENOENT)
		{
			DEBUGF_R("Permission Denied: %s\n", fname);
			qerr_add(ft, id, GS_FT_ERR_PERM, NULL);
			return -GS_FT_ERR_PERM; // Exists but stat() failed 
		}
		DEBUGF_R("Not found: %s\n", fname);
	} else {
		// FILE exists
		if (!S_ISREG(res.st_mode))
		{
			DEBUGF_R("WARN: %s not a regular file\n", fname);
			qerr_add(ft, id, GS_FT_ERR_BADF, NULL);
			return -GS_FT_ERR_BADF; // Not a regular file
		}

		if (S_ISDIR(res.st_mode))
		{
			// Remote wants to send file but directory is in the way
			DEBUGF_R("WARN: Directory is in the way: %s\n", fname);
			qerr_add(ft, id, GS_FT_ERR_PERM, NULL);
			return -GS_FT_ERR_PERM;
		}

		fz = res.st_size;
	}

	// HERE: File (not directory)
	struct _gs_ft_file *f;
	f = file_new(fname, NULL, fz, fz_remote, mtime, GS_fperm2mode(fperm));
	f->li = GS_LIST_add(gsl, NULL, f, id);

	return 0;	
}
/*
 * SERVER, PUT (upload)
 * Return < 0 on error.
 * Return 0 otherwise.
 */
int
GS_FT_add_file(GS_FT *ft, uint32_t id, const char *fname, size_t len, int64_t fsize, uint32_t mtime, uint32_t fperm, uint8_t flags)
{
	if (fname[len] != '\0')
		return -1; // protocol error. Not 0 terminated.

	// FIXME: sanitize file name
	DEBUGF_Y("#%u ADD-FILE - size %"PRIu64", fperm 0%o, '%s' mtime=%d flags=0x%02x\n", id, fsize, fperm, fname, mtime, flags);

	char fn_local[4096];
	char *wdir = getwd(NULL);
	snprintf(fn_local, sizeof fn_local, "%s/%s", wdir, fname);
	XFREE(wdir);

	return gs_ft_add_file(ft, &ft->fadded, id, fn_local, mtime, fperm, fsize /*remote size*/, flags);
}

/*
 * SERVER, GET (download)
 * Client requested this file. Add to list of files that need to be send to peer.
 * Server (if idle) will send a get_switch message and start transmitting data.
 */
int
GS_FT_dl_add_file(GS_FT *ft, uint32_t id, const char *fname, size_t len, int64_t fz_remote)
{
	if (fname[len] != '\0')
		return -1; // protocol error. Not 0 terminated.

	DEBUGF_Y("#%u DL-ADD-FILE - '%s' fz_remote=%"PRId64"\n", id, fname, fz_remote);

	struct stat res;
	int ret;
	ret = stat(fname, &res);
	if (ret != 0)
	{
		DEBUGF_R("NOT FOUND: %s\n", fname);
		char err[128];
		snprintf(err, sizeof err, "Not found: %s", fname);
		qerr_add(ft, id, GS_FT_ERR_NOENT, err);
		return -1;
	}

	if (!S_ISREG(res.st_mode))
	{
		DEBUGF_R("WARN2: %s not a regular file\n", fname);
		return -2;
	}

	return gs_ft_add_file(ft, &ft->fdl, id, fname, res.st_mtime, res.st_mode, fz_remote, 0 /*flags, unused*/);
}


/*
 * CLIENT, Get (download) - receiving a file list from server
 * Add a single file (that exists on server) to the list that the client may request for download.
 */
int
GS_FT_list_add(GS_FT *ft, uint32_t globbing_id, const char *fname, size_t len, int64_t fz_remote, uint32_t mtime, uint32_t fperm, uint8_t flags)
{
	struct _gs_ft_list_pattern *p = NULL;
	int ret;

	if (fname[len] != '\0')
		return -1;

	DEBUGF_Y("G#%u LISTREPLY - fperm 0%o, '%s'\n", globbing_id, fperm, fname);

	// Check if returned file matches what we asked for. This stops the server
	// from sending files that we did not request.
	GS_LIST_ITEM *li = GS_LIST_by_id(&ft->plistreq_waiting, globbing_id);
	if (li == NULL)
	{
		DEBUGF_R("Oops. Received G#%u but no file requested?\n", globbing_id);
		return -1;
	}
	p = (struct _gs_ft_list_pattern *)li->data;

	char fn_local[4096];
	const char *ptr;
	ptr = str_dotslash(fname);
	if (ptr == NULL)
	{
		ptr = str_stripslash(fname);
	}
	snprintf(fn_local, sizeof fn_local, "%s/%s", p->wdir, ptr);

	if (fnmatch(p->pattern, fn_local, 0) != 0)
	{
		DEBUGF_R("filename does not match request (%s != %s)\n", fname, p->pattern);
		// goto done; // FIXME: C 'foo/*' would return 'bar/x.txt' but should return 'foo/bar/x.txt'
	}

	// FIXME: Check for '..' and '/' and that realpath matches when send the globbing
	// request to remote. Oops, this should be done when requesting file name?
	if (strstr(fn_local, "..") != NULL)
	{
		DEBUGF_R("Bad file name (%s)...\n", fn_local);
		goto done;
	}

	if (flags & GS_FT_FL_ISDIR)
	{
		DEBUGF_C("Directory: %s\n", fn_local);
		// mkdirp() will remove any ordinary file that is in its way
		if (mkdirp(fn_local, GS_fperm2mode(fperm)) != 0)
		{
			DEBUGF_R("mkdir(%s): %s\n", fn_local, strerror(errno));
		}

		goto done;
	}


	struct stat res;
	ret = stat(fn_local, &res);
	int64_t fz_local = 0;
	if (ret == 0)
	{
		if (res.st_size == fz_remote)
		{
			DEBUGF_R("File of equal size already exists (%s)\n", fn_local);
			mk_stats(ft, -1, NULL, fn_local, 0 /*success*/);

			goto done;
		}
		if (res.st_size < fz_remote)
			fz_local = res.st_size;
	}

	struct _gs_ft_file *f;
	f = file_new(fname, fn_local, fz_local, fz_remote, mtime, GS_fperm2mode(fperm));

	f->li = GS_LIST_add(&ft->flist, NULL, f, ft->g_id);
	ft->g_id += 1;

	ft->n_files_waiting += 1;
	ret = 0;

done:
	if (flags & GS_FT_LISTREPLY_FL_LAST)
	{
		DEBUGF_G("Last file for this globbing id. Free'ing get-list\n");
		free_get_li(li);
	}
	return 0;
}


struct _mperm
{
	mode_t mode;
	uint32_t perm;
};

struct _mperm x_mperm[] = {
	{S_ISUID, 04000},
	{S_ISGID, 02000},
	{S_ISVTX, 01000},

	{S_IRUSR, 00400},
	{S_IWUSR, 00200},
	{S_IXUSR, 00100},

	{S_IRGRP, 00040},
	{S_IWGRP, 00020},
	{S_IXGRP, 00010},

	{S_IROTH, 00004},
	{S_IWOTH, 00002},
	{S_IXOTH, 00001}
};

// Host to network byte order for st_mode file permission
static uint32_t
GS_mode2fperm(mode_t m)
{
	uint32_t u = 0;
	int n;

	m &= ~S_IFMT;
	for (n = 0; n < sizeof x_mperm / sizeof *x_mperm; n++)
	{
		if (m & x_mperm[n].mode)
			u |= x_mperm[n].perm;
	}

	// DEBUGF_B("mode2fperm 0%o\n", u);
	return u;
}

// Network to host byte order for st_mode file permission
static mode_t
GS_fperm2mode(uint32_t u)
{
	mode_t m = 0;
	int n;

	// DEBUGF_B("fperm2mode 0%o\n", u);

	for (n = 0; n < sizeof x_mperm / sizeof *x_mperm; n++)
	{
		if (u & x_mperm[n].perm)
			m |= x_mperm[n].mode;
	}

	return m;
}


// strip all '/' from beginning of string.
static const char *
str_stripslash(const char *src)
{
	while (*src == '/')
		src++;

	return src;
}

// Find '/./' in string
// "/foo/./bar/test1.txt" -> "bar/test1.txt"
// "/foo/bar/./test1.txt" -> "test1.txt"
// "/foo/bar/test1.txt"   -> NULL
// "test1.txt"            -> NULL
static const char *
str_dotslash(const char *src)
{

	char *token;
	token = strstr(src, "/./");
	if (token == NULL)
		return NULL;

	src = token + 3; // skip '/./'
	src = str_stripslash(src);

	return src;
}

// Takes a <dir>/./<dir2>/<filename> construct and returns <dir2>/<filename> part.
// Space is allocated to hold the new string. Caller must call free() to free it.
static char *
dotslash_filename(const char *src)
{
	char *dst = NULL;

	/*
	 * Consider these possibilities (examples)
	 * /tmp/foo/bar/hosts 
	 * /./tmp/foo/bar/hosts
	 * /tmp/foo/./bar/./hosts
	 * /tmp/./foo/bar/hosts
	 * hosts 
	 * foo/bar/host 
	 */
	// Find token after last occurance of '/./'
	const char *str;
	str = str_dotslash(src);

	// str contains everything after '/./' or fname if '/./' not found.
	if (str == NULL)
	{
		// HERE: No '/./'. Use basename (file only, no directory part)
		char *s = strdup(src); // basename() might modify str
		dst = strdup(basename(s));
		free(s);
	} else {
		// Scenario: put 'foo/'' will turn into 'foo/./'' which will call
		// GS_FT_put(foo/.//bar/file.txt) which must end up with 'bar/file.txt'
		// and not with '/bar/file.txt'
		str = str_stripslash(str);
		dst = strdup(str);
	}

	return dst;
}

// CLIENT, put (upload)
// SERVER, get (download), after globbing.
static int
add_file_to_list(GS_FT *ft, GS_LIST *gsl, const char *fname, uint32_t globbing_id, int is_get)
{
	struct _gs_ft_file *f;
	f = calloc(1, sizeof *f);
	f->globbing_id = globbing_id;

	if (is_get == 1)
	{
		// Server, GET (download). 
		f->name = strdup(fname);
	} else {
		// Client, PUT (upload).
		f->name = dotslash_filename(fname);
	}

	DEBUGF_Y("#%u name = %s\n", ft->g_id, f->name);
	f->li = GS_LIST_add(gsl, NULL, f, ft->g_id);
	ft->g_id += 1;

	// Even if it does not exist then we still need to add it to the LISTREPLY list
	// Client will request it and only then will we send an error.
	// This can happen when client requests "foo[123].notexist[233].da*" and globbing
	// fails.
	int ret;
	struct stat res;
	ret = stat(fname, &res);
	if (ret != 0)
	{
		DEBUGF_R("%s NOT FOUND\n", fname);
		return -1;
	}

	// Get absolute and real path as CWD may change before
	// upload starts.
	char *realfname;
	realfname = realpath(fname, NULL);
	if (realfname == NULL)
		return -3;

	f->realname = realfname;
	f->fz_local = res.st_size;
	f->mode = res.st_mode;
	f->mtime = res.st_mtime;

	return 0;
}

/*
 * CLIENT: Add this file (not directory) to queue.
 */
int
GS_FT_put(GS_FT *ft, const char *fname)
{
	int ret;

	ret = add_file_to_list(ft, &ft->fqueue, fname, 0 /*unused*/, 0 /*is_get*/);
	if (ret != 0)
		return ret;
	ft->n_files_waiting += 1;

	return 0;
}


// SERVER: Add a single file
static int
get_add_file(GS_FT *ft, const char *fname, uint32_t globbing_id)
{
	int ret;

	DEBUGF_G("Adding %s\n", fname);

	// Retrieve all info for the file.
	ret = add_file_to_list(ft, &ft->flistreply, fname, globbing_id, 1 /*is_get*/);
	if (ret != 0)
	{
		return ret;
	}

	return 0;
}

// SERVER
static void
glob_cb(GS_GL *res)
{
	GS_FT *ft = (GS_FT *)res->arg_ptr;

	DEBUGF_Y("G#%u \n", res->globbing_id);
	get_add_file(ft, res->name, res->globbing_id);
}

// SERVER: Generate file list based on globbing pattern sent by client.
int
GS_FT_list_add_files(GS_FT *ft, uint32_t globbing_id, const char *pattern, size_t len)
{
	int n_found;

	if (pattern[len] != '\0')
		return -1; // protocol error. Not 0-terminated.

	DEBUGF_Y("G#%u GET-ADD-FILE: %s\n", globbing_id, pattern);

	n_found = GS_GLOBBING(glob_cb, pattern, globbing_id, ft, 0);

	if (n_found <= 0)
	{
		DEBUGF_R("NOT FOUND: %s\n", pattern);
		char err[128];
		snprintf(err, sizeof err, "Not found: %s", pattern);
		qerr_add(ft, globbing_id, GS_FT_ERR_NOENT, err);
	}

	return n_found;
}


/*
 * CLIENT: Add a get-requst to the listrequest queue.
 */
int
GS_FT_get(GS_FT *ft, const char *pattern)
{
	struct _gs_ft_list_pattern *p;

	p = calloc(1, sizeof *p);
	p->pattern = strdup(pattern);
	p->wdir = getwd(NULL);
	p->globbing_id = ft->g_globbing_id;
	ft->g_globbing_id++;

	GS_LIST_add(&ft->plistreq, NULL, p, ft->g_id);
	ft->g_id += 1;

	return 0;
}

static void
qerr_add(GS_FT *ft, uint32_t id, uint8_t code, const char *str)
{
	struct _gs_ft_qerr *qerr;

	qerr = calloc(1, sizeof *qerr);
	qerr->id = id;
	qerr->code = code;
	qerr->str = NULL;
	if (str != NULL)
		qerr->str = strdup(str);

	// Must add in sequence of occurance (add_count)
	GS_LIST_add(&ft->qerrs, NULL, qerr, ft->qerrs.add_count);
}

static void
do_error(GS_FT *ft, GS_LIST_ITEM *li, uint32_t code, const char *str)
{
	qerr_add(ft, li->id, code, str);
	ft_del(li);
}

static void
do_complete(GS_FT *ft, struct _gs_ft_file *f)
{
	if (f->fp != NULL)
	{
		fflush(f->fp);
		fchmod(fileno(f->fp), f->mode & ~S_IFMT);
		if (f->mtime != 0)
		{
			DEBUGF_B("Setting time to %ld\n", f->mtime);
			struct timeval t[] = {{f->mtime, 0}, {f->mtime, 0}};
			futimes(fileno(f->fp), t);
		}
	}
	do_error(ft, f->li, GS_FT_ERR_COMPLETED, NULL);
}

static void
qerr_free(struct _gs_ft_qerr *qerr)
{
	XFREE(qerr->str);
	XFREE(qerr);
}

// SERVER & CLIENT
void
GS_FT_data(GS_FT *ft, const void *data, size_t len)
{
	struct _gs_ft_file *f;
	struct _gs_ft_file **active;
	size_t sz;

	if (ft->is_server)
		active = &ft->active_put_file;
	else
		active = &ft->active_dl_file;

	f = *active;

	if (f == NULL)
	{
		DEBUGF_R("Receiving data but no active receiving file (len=%zu)\n", len);
		HEXDUMP(data, MIN(len, 16));
		return;
	}
	
	XASSERT(f->fp != NULL, "fp is NULL\n");
	if (f->fz_local + len > f->fz_remote)
	{
		DEBUGF_R("More data than we want (%"PRIu64")! (fz_local=%"PRIu64", len == %zu, fz_remote = %"PRIu64"\n", f->fz_local + len - f->fz_remote, f->fz_local, len, f->fz_remote);
		HEXDUMP((uint8_t *)data + (f->fz_remote - f->fz_local), f->fz_local + len - f->fz_remote);
		len = f->fz_remote - f->fz_local; // truncating
	}

	sz = fwrite(data, 1, len, f->fp);
	f->fz_local += sz;

	if (sz != len)
	{
		if (ft->is_server == 0)
			mk_stats(ft, f->li->id, f->li->data, NULL, 1 /*err*/);

		do_error(ft, f->li, GS_FT_ERR_BADF, NULL);
		*active = NULL;
		return;
	}

	if (ft->is_server == 0)
	{
		// CLIENT, get (download);
		update_stats(f, sz);
	}

	if (f->fz_local < f->fz_remote)
		return; // still data missing...

	DEBUGF_B("All data received (%"PRIu64" of %"PRIu64")\n", f->fz_local, f->fz_remote);
	if (ft->is_server == 0)
	{
		// CLIENT, get (download)
		mk_stats(ft, f->li->id, f->li->data, NULL, 0 /*err*/);
		ft_done(ft);
		ft_del(f->li);
	} else {
		// SERVER, put (upload)
		do_complete(ft, f); 
	}

	*active = NULL;
}

// CLIENT
void
GS_FT_accept(GS_FT *ft, uint32_t id, int64_t fz_remote)
{
	GS_LIST_ITEM *li;

	DEBUGF("#%u acc fz_remote = %"PRId64"\n", id, fz_remote);

	li = GS_LIST_by_id(&ft->fputs, id);
	if (li == NULL)
	{
		DEBUGF_R("Unknown file id %u\n", id);
		return; // actually a protocol error....
	}

	GS_LIST_move(&ft->faccepted, li);

	struct _gs_ft_file *f = (struct _gs_ft_file *)li->data;
	f->fz_remote = fz_remote;
}

// unlink any file in our way and create directory.
static int
mkdir_agressive(const char *path, mode_t mode)
{
	struct stat res;
	if (stat(path, &res) == 0)
	{
		if (S_ISDIR(res.st_mode))
			return 0; // Diretory already exists. return.

		// Remove any other file or link.
		DEBUGF_R("unlink(%s)\n", path);
		unlink(path);
	}

	if (mkdir(path, mode) != 0)
	{
		DEBUGF_R("mkdir(%s) failed: %s\n", path, strerror(errno));
		return -1;
	}

	return 0;
}

/*
 * Create all directories recursively.
 *
 * /tmp/foo/bar/test.dat would create /tmp/foo/bar/test.dat [directory]
 * /tmp/foo/bar/test.dat/ would create /tmp/foo/bar/test.dat/
 * ///// would return success ('/' always exists)
 */
static int
mkdirp(const char *path, mode_t mode)
{
	int rv = 0;
	char *f = strdup(path);
	struct stat res;

	// Return 0 if directory already exist
	if (stat(f, &res) == 0)
	{
		if (S_ISDIR(res.st_mode))
		{
			// HERE: Directory exists.
			if (mode == 0)
			{
				// HERE: Not allowed to overwrite permissions
				// if (access(path, W_OK) != 0)
				goto done; // Done have access
			} else {
				chmod(f, mode);
			}
			DEBUGF_W("%s already exists\n", f);
			goto done;
		}
	}

	// Directory does not exist. Use default permission to create it.
	if (mode == 0)
		mode = 0755;

	// If parent directory exist then only create last directory.
	char *dn = dirname(f);
	DEBUGF_W("Parent of %s is %s\n", f, dn);
	if ((dn != NULL) && (stat(dn, &res) == 0))
	{
		if (S_ISDIR(res.st_mode))
		{
			// HERE: Parent directory exists.
			DEBUGF_W("1-mkdir(%s)\n", f);
			if (mkdir_agressive(f, mode) != 0)
				rv = -1;
			goto done;
		}
	}

	// HERE: Neither directory nor parent directory exist. Start from
	// the left and create entire hirachy....

	// No need to check '/' which always exists.
	char *ptr = f;
	while (*ptr == '/')
		ptr++;

	// Remove '/' from the right (tmp/foo///// -> tmp/foo)
	char *end = ptr + strlen(ptr);
	while (end > ptr)
	{
		end--;
		if (*end != '/')
			break;
		*end = '\0';
	}

	// Empty string.
	if (ptr == end)
		goto done;

	// HERE: Create all directory starting from left
	// '/tmp' -> '/tmp/foo' -> '/tmp/foo/bar' -> ...
	while (1)
	{
		ptr = index(ptr, '/');
		if (ptr != NULL)
			*ptr = '\0';
		DEBUGF_W("2-mkdir(%s)\n", f);
		if (mkdir_agressive(f, mode) != 0)
		{
			rv = -1;
			goto done;
		}
		if (ptr == NULL)
			break;
		*ptr = '/';
		ptr += 1;
	}

done:
	XFREE(f);
	return rv;
}

static void
gs_ft_switch(GS_FT *ft, uint32_t id, int64_t fz_remote, struct _gs_ft_file **active, GS_LIST *fsource)
{
	GS_LIST_ITEM *li;

	li = GS_LIST_by_id(fsource, id);
	if (li == NULL)
	{
		DEBUGF_R("Unknown file id %u\n", id);
		return; // actually a protocol error....
	}

	// Switch from active receiving file to new file
	struct _gs_ft_file *new = (struct _gs_ft_file *)li->data;

	DEBUGF_W("Switching to id %u '%s' (got %"PRId64", want %"PRId64")\n", id, new->name, new->fz_local, new->fz_remote); 

	if ((*active != NULL) && (*active != new))
	{
		fclose((*active)->fp);
	}

	*active = NULL;

	// Server: Existing file is larger. Overwrite.
	if (new->fz_local > new->fz_remote)
	{
		DEBUGF_W("File larger. Overwritting...\n");
		XASSERT(fz_remote == 0, "fz_remote not 0 but server's file is larger\n");
		new->fz_local = 0;
		fz_remote = 0;
	}

	if ((new->fz_remote == new->fz_local) && (new->fz_local != 0))
	{
		// FIXME: currently _not_ updating mtime/fperm if file is already on peer side.
		// Do we want this? (if so then move this code block after fopen().)
		do_complete(ft, new);
		return;
	}

	if (fz_remote == 0)
	{
		DEBUGF_G("New file (%s) %s\n", new->realname, new->name);
		
		char *ptr = new->realname;
		// mkdir(): Remove leading './///' in './////foo/bar' (when globbing './')
		if (*ptr == '.')
		{
			ptr++;
			while (*ptr == '/')
				ptr++;
		}
		ptr = dirname(ptr);
		// put(test1k.dat) must not modify the permission of parent directory.
		mkdirp(ptr, 0 /*do not update permission on existing directory*/);

		new->fp = fopen(new->realname, "w");
	} else {
		// Check fsize of local file.
		DEBUGF_G("Appending to file\n");
		struct stat res;
		if (stat(new->realname, &res) != 0)
			goto err;
		if (res.st_size != new->fz_local)
		{
			// Size changed
			do_error(ft, new->li, GS_FT_ERR_BADFSIZE, NULL);
			return;
		}
		new->fp = fopen(new->realname, "a");
	}

	if (new->fp == NULL)
	{
		DEBUGF("fopen(%s) failed: %s\n", new->realname, strerror(errno));
		goto err;
	}

	if (new->fz_remote == 0)
	{
		// Zero sized file. Completed.
		do_complete(ft, new);
		return;
	}

	*active = new;
	return;
err:
	do_error(ft, new->li, GS_FT_ERR_PERM, NULL);
}

void
GS_FT_switch(GS_FT *ft, uint32_t id, int64_t offset)
{
	if (ft->is_server)
	{
		// Client is uploading (client send SWITCH command)
		gs_ft_switch(ft, id, offset, &ft->active_put_file, &ft->freceiving);
	} else {
		// Client is downloading (server sent SWITCH command)
		gs_ft_switch(ft, id, offset, &ft->active_dl_file, &ft->fdl_waiting);
	}
}

static void
file_free(struct _gs_ft_file *f)
{
	XFREE(f->name);
	XFREE(f->realname);
	XFREE(f);
}

// Reduce counter of outstanding files/errors.
static void
ft_done(GS_FT *ft)
{
	if (ft->n_files_waiting > 0)
		ft->n_files_waiting -= 1;
	else
		DEBUGF_R("Oops, n_files_waiting == %d\n", ft->n_files_waiting);
}

/*
 * Remove file from queue.
 */
static void
ft_del(GS_LIST_ITEM *li)
{
	struct _gs_ft_file *f = (struct _gs_ft_file *)li->data;

	XFCLOSE(f->fp);

	file_free(f);
	GS_LIST_del(li);
}

// Human readable bps string
static void
mk_bps(char *str, size_t sz, uint64_t duration, uint64_t amount, int err)
{
	if (err != 0)
	{
		snprintf(str, sz, "ERROR");
		return;
	}
	if (duration > 0)
		GS_format_bps(str, sz, (amount * 1000000 / duration));
	else
		snprintf(str, sz, "SKIPPED");
}

// Generate stats per file and call call-back.
static void
mk_stats(GS_FT *ft, uint32_t id, struct _gs_ft_file *f, const char *fname, int err)
{
	struct _gs_ft_stats s;

	memset(&s, 0, sizeof s);
	s.id = id;
	if (fname != NULL)
		s.fname = fname;
	else if (f != NULL)
		s.fname = f->name;

	if (f != NULL)
	{
		s.xfer_amount = f->xfer_amount;
		if (f->usec_start > f->usec_end)
			f->usec_end = GS_usec();

		if (f->usec_suspend_start != 0)
		{
			DEBUGF_R("Oops, Reporting stats on a suspended file\n");
			f->usec_suspend_duration += (GS_usec() - f->usec_suspend_start);
		}
		s.xfer_duration = (f->usec_end - f->usec_start) - f->usec_suspend_duration;
	}

	mk_bps(s.speed_str, sizeof s.speed_str, s.xfer_duration, s.xfer_amount, err);

	// Global stats for all files
	ft->stats_total.xfer_duration += s.xfer_duration;
	ft->stats_total.xfer_amount += s.xfer_amount;
	if (err == 0)
		ft->stats_total.n_files_success += 1;
	else 
		ft->stats_total.n_files_error += 1;
	mk_stats_total(ft);

	// Call call-back
	if (ft->func_stats != NULL)
		(*ft->func_stats)(&s);
}

// Generate total stats
static void
mk_stats_total(GS_FT *ft)
{
	GS_FT_stats_total *st = &ft->stats_total;

	mk_bps(st->speed_str, sizeof st->speed_str, st->xfer_duration, st->xfer_amount, st->n_files_success==0?1:0);
}

/*
 * Error received from Server or Client.
 * Remove and free item.
 */
void
GS_FT_status(GS_FT *ft, uint32_t id, uint8_t code, const char *err_str, size_t len)
{
	GS_LIST_ITEM *li;
	int err = 1;
	int is_make_stats = 1;

	if (err_str[len] != '\0')
		return; // protocol error. Not 0 terminated.
	DEBUGF_R("#%u STATUS: code=%u (%s)\n", id, code, err_str);

	// There can not be an error in fqueue or plistreq or flist as those are
	// local lists. Here we only care about lists that send a request
	// to the server and are waiting for a reply.

	// li can be one of two structures only:
	// _gs_ft_file or gs_ft_list_pattern
	li = GS_LIST_by_id(&ft->fcompleted, id);
	if (li == NULL)
	{
		li = GS_LIST_by_id(&ft->fputs, id);
		if (li == NULL)
		{
			li = GS_LIST_by_id(&ft->fadded, id);
			if (li == NULL)
			{
				li = GS_LIST_by_id(&ft->freceiving, id);
				if (li == NULL)
				{
					li = GS_LIST_by_id(&ft->fdl_waiting, id);
					if (li == NULL)
					{
						li = GS_LIST_by_id(&ft->plistreq_waiting, id);
						if (li == NULL)
						{
							DEBUGF_R("id %u not found\n", id);
							return; // not found
						}
						// This 'li' does not hold a _gs_ft_file structure and thus
						// must not generate stats or try to free a _gs_ft_file when it is not.
						is_make_stats = 0;
					}
				}
			}
		}
	} else {
		// Was waiting for 'complete' signal. No error if 'complete' is a success.
		if (code == GS_FT_ERR_COMPLETED)
			err = 0;
	}

	// Report (error-)status to caller
	struct _gs_ft_status s;
	memset(&s, 0, sizeof s);
	s.code = code;
	s.file = li->data;
	// FIXME: sanitize error string
	snprintf(s.err_str, sizeof s.err_str, "%s", err_str);
	if (ft->func_status != NULL)
		(*ft->func_status)(ft, &s);


	if (is_make_stats)
	{
		// Report stats to caller
		mk_stats(ft, id, li->data, NULL, err);

		if (li->data == ft->active_put_file)
			ft->active_put_file = NULL;

		ft_done(ft);
		ft_del(li);
	} else {
		ft_done(ft);
		GS_LIST_del(li);
	}

}

/*
 * Make an error packet. Return length.
 */
static size_t
mk_error(void *dst, size_t len, uint32_t id, uint8_t code, const char *str)
{
	size_t sz;
	struct _gs_ft_error err;

	memset(&err, 0, sizeof err);
	err.id = htonl(id);
	err.code = code;
	memcpy(dst, &err, sizeof err);

	sz = 1;
	struct _gs_ft_error *p = (struct _gs_ft_error *)dst;
	if ((str != NULL) && (strlen(str) > 0))
	{
		sz = MIN(len - sizeof err, strlen(str) + 1);
		memcpy(p->str, str, sz - 1);
	}
	p->str[sz - 1] = '\0';

	return sizeof err + sz;
}

/*
 * Make an error packet. Remove errornous file from queue
 */
static size_t
ft_mk_error(GS_FT *ft, void *dst, size_t len, int *pkt_type, GS_LIST_ITEM *li, uint8_t code, const char *str)
{
	size_t sz;

	*pkt_type = GS_FT_TYPE_ERROR;
	sz = mk_error(dst, len, li->id, code, str);

	ft_done(ft);
	ft_del(li);

	return sz;
}

static void
update_stats(struct _gs_ft_file *f, size_t sz)
{
	// -----BEGIN Log statistics-----
	if (f->usec_start == 0)
	{
		f->usec_start = GS_usec();
	}

	if (f->usec_suspend_start != 0)
	{
		f->usec_suspend_duration += (GS_usec() - f->usec_suspend_start);
		f->usec_suspend_start = 0;
	}
	f->xfer_amount += sz;
	// -----END Log statistics-----
}

static size_t
mk_xfer_data(GS_FT *ft, struct _gs_ft_file **active, GS_LIST *fcompleted, int *pkt_type, void *dst, size_t len, int is_log_stats)
{
	struct _gs_ft_file *f = *active;
	size_t sz;
	*pkt_type = GS_FT_TYPE_DATA;

	sz = fread(dst, 1, len, f->fp);

	if (sz <= 0)
	{
		*active = NULL;
		return ft_mk_error(ft, dst, len, pkt_type, f->li, GS_FT_ERR_BADF, NULL);
	}
	f->fz_remote += sz;

	if (f->fz_remote >= f->fz_local)
	{
		GS_LIST_move(fcompleted, f->li);
		*active = NULL;
	}

	if (is_log_stats)
	{
		update_stats(f, sz);
	}

	return sz;
}
 
static size_t
mk_switch(GS_FT *ft, struct _gs_ft_file **active, GS_LIST *fsource, GS_LIST *fcompleted, int *pkt_type, void *dst, size_t len)
{
	GS_LIST_ITEM *li;
	struct _gs_ft_file *f;
	int ret;

	li = GS_LIST_next(fsource, NULL);
	f = (struct _gs_ft_file *)li->data;

	f->fp = fopen(f->realname, "r");
	if (f->fp == NULL)
	{
		DEBUGF("Could not open file %s\n", f->realname);
		return ft_mk_error(ft, dst, len, pkt_type, li, GS_FT_ERR_PERM, NULL);
	}

	ret = fseek(f->fp, 0, SEEK_END);
	if (ret != 0)
		return ft_mk_error(ft, dst, len, pkt_type, li, GS_FT_ERR_BADF, NULL);
	f->fz_local = ftell(f->fp);

	// Peer already has this file.
	// Overwrite if remote size is smaller _or_ larger.
	if ((f->fz_local == f->fz_remote) && (f->fz_local != 0))
	{
		DEBUGF("#%u Skipping %s (already on peer)\n", (unsigned int)f->li->id, f->name);
		mk_stats(ft, li->id, f, NULL, 0 /*success*/);
		return ft_mk_error(ft, dst, len, pkt_type, li, GS_FT_ERR_NODATA, NULL);
	}

	// Remote size is larger. Overwrite from beginning.
	if (f->fz_local < f->fz_remote)
		f->fz_remote = 0;

	// Remote size is smaller. Restart transmission.
	ret = fseek(f->fp, f->fz_remote, SEEK_SET);
	if (ret != 0)
		return ft_mk_error(ft, dst, len, pkt_type, li, GS_FT_ERR_BADF, NULL);

	struct _gs_ft_switch sw;
	memset(&sw, 0, sizeof sw);
	sw.id = htonl(li->id);
	sw.offset = htonll(f->fz_remote);
	memcpy(dst, &sw, sizeof sw);

	// Handle zero size files
	if (f->fz_local == 0)
	{
		GS_LIST_move(fcompleted, f->li);
		*active = NULL;
	} else {
		// HERE: fsize is not zero.
		*active = f;
	}

	*pkt_type = GS_FT_TYPE_SWITCH;
	DEBUGF_W("#%"PRIu64" Switch to %s (fz_local=%"PRIu64", fz_remote=%"PRIu64")\n", li->id, f->name, f->fz_local, f->fz_remote);

	return sizeof sw;
}

/*
 * Create a data packet (from job/queue that need attention).
 * Return the length.
 * Return 0 if no data needs to be written or an error occured.
 * check pkt_type to for packet code that needs to be written.
 *
 * Set TYPE to SWITCH or DATA depending on the type of packet created.
 * Set to DONE when done and NONE when nothing is to be done.
 */
size_t
GS_FT_packet(GS_FT *ft, void *dst, size_t len, int *pkt_type)
{
	struct _gs_ft_file *f;
	size_t sz;

	// DEBUGF("GS_FT_packet() %d, accepted %d, len %zu\n", ft->fputs.n_items, ft->faccepted.n_items, len);

	*pkt_type = GS_FT_TYPE_NONE;
	if (len < GS_FT_MIN_BUF_SIZE)
	{
		DEBUGF_R("Does this ever happen?\n");
		return 0;
	}

	// Check if any queue'd errors needs sending.
	// Server & Client
	if (ft->qerrs.n_items > 0)
	{
		GS_LIST_ITEM *li = NULL;
		li = GS_LIST_next(&ft->qerrs, NULL);
		struct _gs_ft_qerr *qerr = (struct _gs_ft_qerr *)li->data;

		sz = mk_error(dst, len, qerr->id, qerr->code, qerr->str);

		*pkt_type = GS_FT_TYPE_ERROR;
		qerr_free(qerr);
		GS_LIST_del(li);
		return sz;
	}

	// Check if any files in the queue that need to be 'put'
	// on offer to the remote side (and then awaiting 'accept').
	// Client
	if (ft->fqueue.n_items > 0)
	{
		DEBUGF("%d items in queue (waiting for put to be send)\n", ft->fqueue.n_items);
		GS_LIST_ITEM *li = NULL;
		li = GS_LIST_next(&ft->fqueue, NULL);
		f = (struct _gs_ft_file *)li->data;

		struct _gs_ft_put put;
		struct _gs_ft_put *p = (struct _gs_ft_put *)dst;
		memset(&put, 0, sizeof put);
		put.fperm = htonl(GS_mode2fperm(f->mode));
		put.id = htonl(li->id);
		put.mtime = htonl(f->mtime);
		if (S_ISDIR(f->mode))
			put.flags |= GS_FT_FL_ISDIR;
		else
			put.fsize = htonll(f->fz_local);
		memcpy(dst, &put, sizeof put);

		sz = MIN(len - sizeof put, strlen(f->name) + 1);
		// DEBUGF("name len %zu + hdr %zu\n", n, sizeof *hdr);
		memcpy(p->name, f->name, sz - 1);
		p->name[sz - 1] = '\0';

		if (S_ISDIR(f->mode))
		{
			ft_done(ft);
			ft_del(li);
		} else {
			GS_LIST_move(&ft->fputs, li);
		}
		*pkt_type = GS_FT_TYPE_PUT;
		return sizeof put + sz;
	}

	// Client
	if (ft->plistreq.n_items > 0)
	{
		GS_LIST_ITEM *li = NULL;
		li = GS_LIST_next(&ft->plistreq, NULL);
		struct _gs_ft_list_pattern *p = li->data;

		DEBUGF("%d LIST-REQ '%s' in queue (LIST-REQ to be send)\n", ft->plistreq.n_items, p->pattern);

		struct _gs_ft_list_request list_req;
		struct _gs_ft_list_request *lr = (struct _gs_ft_list_request *)dst;
		memset(&list_req, 0, sizeof list_req);
		list_req.globbing_id = htonl(p->globbing_id);
		memcpy(dst, &list_req, sizeof list_req);

		sz = MIN(len - sizeof list_req, strlen(p->pattern) + 1);
		memcpy(lr->pattern, p->pattern, sz - 1);
		lr->pattern[sz - 1] = '\0';

		GS_LIST_move(&ft->plistreq_waiting, li);
		*pkt_type = GS_FT_TYPE_LISTREQUEST;
		return sizeof list_req + sz;
	}

	// Server
	if (ft->flistreply.n_items > 0)
	{
		// FIXME: do stats here on the list of files?! No need to keep this all in memory.
		DEBUGF("%d LIST-REPLY in queue (LIST-REPLY to be send)\n", ft->flistreply.n_items);
		GS_LIST_ITEM *li = NULL;
		li = GS_LIST_next(&ft->flistreply, NULL);
		f = (struct _gs_ft_file *)li->data;

		struct _gs_ft_list_reply list_reply;
		struct _gs_ft_list_reply *lr = (struct _gs_ft_list_reply *)dst;
		memset(&list_reply, 0, sizeof list_reply);

		list_reply.fperm = htonl(GS_mode2fperm(f->mode));
		list_reply.globbing_id = htonl(f->globbing_id);
		list_reply.fsize = htonll(f->fz_local);
		list_reply.mtime = htonl(f->mtime);
		if (S_ISDIR(f->mode))
			list_reply.flags |= GS_FT_FL_ISDIR;
		// Check if this is the last entry for this globbing-id
		GS_LIST_ITEM *nli = GS_LIST_next(&ft->flistreply, li);
		if ((nli == NULL) || (f->globbing_id != ((struct _gs_ft_file *)nli->data)->globbing_id))
		{
			DEBUGF_C("Last entry for this globbing G#%u\n", f->globbing_id);
			list_reply.flags |= GS_FT_LISTREPLY_FL_LAST;
		}
		memcpy(dst, &list_reply, sizeof list_reply);

		sz = MIN(len - sizeof list_reply, strlen(f->name) + 1);
		memcpy(lr->name, f->name, sz - 1);
		lr->name[sz - 1] = '\0';

		file_free(f);
		GS_LIST_del(li);
		*pkt_type = GS_FT_TYPE_LISTREPLY;
		return sizeof list_reply + sz;
	}

	// Client, download
	if (ft->flist.n_items > 0)
	{
		DEBUGF("%d files in list. Download them...\n", ft->flist.n_items);
		GS_LIST_ITEM *li;
		li = GS_LIST_next(&ft->flist, NULL);
		f = (struct _gs_ft_file *)li->data;

		struct _gs_ft_dl dl;
		struct _gs_ft_dl *d = (struct _gs_ft_dl *)dst;
		memset(&dl, 0, sizeof dl);
		dl.id = htonl(li->id);
		dl.offset = htonll(f->fz_local);
		memcpy(dst, &dl, sizeof dl);

		sz = MIN(len - sizeof dl, strlen(f->name) + 1);
		memcpy(d->name, f->name, sz - 1);
		d->name[sz - 1] = '\0';

		GS_LIST_move(&ft->fdl_waiting, li);
		*pkt_type = GS_FT_TYPE_DL;
		return sizeof dl + sz;

	}

	// Server, PUT (upload), make accept message
	if (ft->fadded.n_items > 0)
	{
		GS_LIST_ITEM *li = NULL;
		li = GS_LIST_next(&ft->fadded, NULL);
		f = (struct _gs_ft_file *)li->data;

		struct _gs_ft_accept acc;
		memset(&acc, 0, sizeof acc);
		acc.id = htonl(li->id);
		acc.offset_dst = htonll(f->fz_local);
		memcpy(dst, &acc, sizeof acc);
		// HERE: Server. Inform client that we accepted.

		GS_LIST_move(&ft->freceiving, li);
		*pkt_type = GS_FT_TYPE_ACCEPT;
		return sizeof acc;
	}

	// Server - download to client
	if (ft->fdl.n_items > 0)
	{
		DEBUGF("Files to send: %d\n", ft->fdl.n_items);
		if (ft->active_dl_file == NULL)
		{
			DEBUGF("Switching to new file\n");
			return mk_switch(ft, &ft->active_dl_file, &ft->fdl, &ft->fdl_completed, pkt_type, dst, len);
		}

		if (ft->is_paused_data)
			return 0;

		return mk_xfer_data(ft, &ft->active_dl_file, &ft->fdl_completed, pkt_type, dst, len, 0 /*is_log_stats*/);
	}

	// Client
	if (ft->faccepted.n_items > 0)
	{
		if (ft->active_put_file == NULL)
		{
			return mk_switch(ft, &ft->active_put_file, &ft->faccepted, &ft->fcompleted, pkt_type, dst, len);
		}

		if (ft->is_paused_data)
		{
			DEBUGF_W("IS-PAUSED-DATA==1. Not sending file data....\n");
			return 0;
		}

		return mk_xfer_data(ft, &ft->active_put_file, &ft->fcompleted, pkt_type, dst, len, 1 /*is_log_stats*/);
	}

	if (ft->is_server == 0)
	{
		// CLIENT
		DEBUGF_R("n_files_waiting=%d, plist.n_items=%d\n", ft->n_files_waiting, ft->plistreq_waiting.n_items);
		if ((ft->n_files_waiting == 0) && (ft->plistreq_waiting.n_items == 0))
		{
			*pkt_type = GS_FT_TYPE_DONE;
			return 0;
		}
	}

	return 0;
}

static void
free_gsl(GS_LIST *gsl)
{
	GS_LIST_ITEM *li = GS_LIST_next(gsl, NULL);

	for (; li != NULL; li = GS_LIST_next(gsl, li))
	{
		struct _gs_ft_file *f = (struct _gs_ft_file *)li->data;

		file_free(f);
		GS_LIST_del(li);
	}
}

static void
free_get_li(GS_LIST_ITEM *li)
{
	struct _gs_ft_list_pattern *p = (struct _gs_ft_list_pattern *)li->data;
	XFREE(p->pattern);
	XFREE(p->wdir);
	GS_LIST_del(li);
}

static void
free_get_gsl(GS_LIST *gsl)
{
	GS_LIST_ITEM *li = GS_LIST_next(gsl, NULL);

	for (; li != NULL; li = GS_LIST_next(gsl, li))
	{
		free_get_li(li);
	}
}

void
GS_FT_free(GS_FT *ft)
{
	free_gsl(&ft->fqueue);
	free_gsl(&ft->fputs);
	free_gsl(&ft->faccepted);
	free_gsl(&ft->fcompleted);

	free_gsl(&ft->fadded);
	free_gsl(&ft->freceiving);

	free_get_gsl(&ft->plistreq);
	free_get_gsl(&ft->plistreq_waiting);

	free_gsl(&ft->flist);
	free_gsl(&ft->fdl_waiting);

	// GS_LIST_del_all(&ft->flistreply, 1 /*deep*/); FIXME: why is thsi deep and not free_gsl????
	free_gsl(&ft->flistreply);
	free_gsl(&ft->fdl);

	GS_LIST_ITEM *li;
	for (li = GS_LIST_next(&ft->qerrs, NULL); li != NULL; li = GS_LIST_next(&ft->qerrs, li))
	{
		qerr_free((struct _gs_ft_qerr *)li->data);

		GS_LIST_del(li);
	}

	ft->active_put_file = NULL;
	ft->active_dl_file = NULL;
}

void
GS_FT_pause_data(GS_FT *ft)
{
	ft->is_paused_data = 1;
}

void
GS_FT_unpause_data(GS_FT *ft)
{
	ft->is_paused_data = 0;
}




