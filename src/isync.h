/*
 * mbsync - mailbox synchronizer
 * Copyright (C) 2000-2002 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2002-2006,2010-2012 Oswald Buddenhagen <ossi@users.sf.net>
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
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, mbsync may be linked with the OpenSSL library,
 * despite that library's more restrictive license.
 */

#include <config.h>

#include <sys/types.h>
#include <stdarg.h>
#include <stdio.h>

#define as(ar) (sizeof(ar)/sizeof(ar[0]))

#define __stringify(x) #x
#define stringify(x) __stringify(x)

#if __GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ > 4)
# define ATTR_UNUSED __attribute__((unused))
# define ATTR_NORETURN __attribute__((noreturn))
# define ATTR_PRINTFLIKE(fmt,var) __attribute__((format(printf,fmt,var)))
#else
# define ATTR_UNUSED
# define ATTR_NORETURN
# define ATTR_PRINTFLIKE(fmt,var)
#endif

#ifdef __GNUC__
# define INLINE __inline__
#else
# define INLINE
#endif

#define EXE "mbsync"

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct x509_store_st X509_STORE;

typedef struct server_conf {
	char *tunnel;
	char *host;
	int port;
#ifdef HAVE_LIBSSL
	char *cert_file;
	unsigned use_imaps:1;
	unsigned use_sslv2:1;
	unsigned use_sslv3:1;
	unsigned use_tlsv1:1;
	unsigned use_tlsv11:1;
	unsigned use_tlsv12:1;

	/* these are actually variables and are leaked at the end */
	unsigned ssl_ctx_valid:1;
	unsigned num_trusted;
	SSL_CTX *SSLContext;
#endif
} server_conf_t;

typedef struct buff_chunk {
	struct buff_chunk *next;
	char *data;
	int len;
	char buf[1];
} buff_chunk_t;

typedef struct {
	/* connection */
	int fd;
	int state;
	const server_conf_t *conf; /* needed during connect */
	char *name;
#ifdef HAVE_LIBSSL
	SSL *ssl;
	int force_trusted;
#endif

	void (*bad_callback)( void *aux ); /* async fail while sending or listening */
	void (*read_callback)( void *aux ); /* data available for reading */
	int (*write_callback)( void *aux ); /* all *queued* data was sent */
	union {
		void (*connect)( int ok, void *aux );
		void (*starttls)( int ok, void *aux );
	} callbacks;
	void *callback_aux;

	/* writing */
	buff_chunk_t *write_buf, **write_buf_append; /* buffer head & tail */
	int write_offset; /* offset into buffer head */

	/* reading */
	int offset; /* start of filled bytes in buffer */
	int bytes; /* number of filled bytes in buffer */
	int scanoff; /* offset to continue scanning for newline at, relative to 'offset' */
	char buf[100000];
} conn_t;

typedef struct {
	const char *file;
	FILE *fp;
	char *buf;
	int bufl;
	int line;
	int err;
	char *cmd, *val, *rest;
} conffile_t;

#define OP_NEW             (1<<0)
#define OP_RENEW           (1<<1)
#define OP_DELETE          (1<<2)
#define OP_FLAGS           (1<<3)
#define  OP_MASK_TYPE      (OP_NEW|OP_RENEW|OP_DELETE|OP_FLAGS) /* asserted in the target ops */
#define OP_EXPUNGE         (1<<4)
#define OP_CREATE          (1<<5)
#define XOP_PUSH           (1<<6)
#define XOP_PULL           (1<<7)
#define  XOP_MASK_DIR      (XOP_PUSH|XOP_PULL)
#define XOP_HAVE_TYPE      (1<<8)
#define XOP_HAVE_EXPUNGE   (1<<9)
#define XOP_HAVE_CREATE    (1<<10)

typedef struct driver driver_t;

typedef struct store_conf {
	struct store_conf *next;
	char *name;
	driver_t *driver;
	const char *path; /* should this be here? its interpretation is driver-specific */
	const char *map_inbox;
	const char *trash;
	unsigned max_size; /* off_t is overkill */
	unsigned trash_remote_new:1, trash_only_new:1;
	char flat_delim;
} store_conf_t;

typedef struct string_list {
	struct string_list *next;
	char string[1];
} string_list_t;

#define M 0 /* master */
#define S 1 /* slave */

typedef struct channel_conf {
	struct channel_conf *next;
	const char *name;
	store_conf_t *stores[2];
	const char *boxes[2];
	char *sync_state;
	string_list_t *patterns;
	int ops[2];
	unsigned max_messages; /* for slave only */
	unsigned keep_modification_time:1;
} channel_conf_t;

typedef struct group_conf {
	struct group_conf *next;
	const char *name;
	string_list_t *channels;
} group_conf_t;

/* For message->flags */
/* Keep the mailbox driver flag definitions in sync! */
/* The order is according to alphabetical maildir flag sort */
#define F_DRAFT	     (1<<0) /* Draft */
#define F_FLAGGED    (1<<1) /* Flagged */
#define F_ANSWERED   (1<<2) /* Replied */
#define F_SEEN       (1<<3) /* Seen */
#define F_DELETED    (1<<4) /* Trashed */
#define NUM_FLAGS 5

/* For message->status */
#define M_RECENT       (1<<0) /* unsyncable flag; maildir_* depend on this being 1<<0 */
#define M_DEAD         (1<<1) /* expunged */
#define M_FLAGS        (1<<2) /* flags fetched */
#define M_TIME         (1<<3) /* time fetched */

#define TUIDL 12

typedef struct message {
	struct message *next;
	struct sync_rec *srec;
	/* string_list_t *keywords; */
	size_t size; /* zero implies "not fetched" */
	int uid;
	unsigned char flags, status;
	char tuid[TUIDL];
	time_t time;
} message_t;

/* For opts, both in store and driver_t->select() */
#define OPEN_OLD        (1<<0)
#define OPEN_NEW        (1<<1)
#define OPEN_FLAGS      (1<<2)
#define OPEN_SIZE       (1<<3)
#define OPEN_EXPUNGE    (1<<5)
#define OPEN_SETFLAGS   (1<<6)
#define OPEN_APPEND     (1<<7)
#define OPEN_FIND       (1<<8)
#define OPEN_TIME       (1<<9)

typedef struct store {
	struct store *next;
	store_conf_t *conf; /* foreign */
	string_list_t *boxes; /* _list results - own */
	unsigned listed:1; /* was _list already run? */

	void (*bad_callback)( void *aux );
	void *bad_callback_aux;

	/* currently open mailbox */
	const char *orig_name; /* foreign! maybe preset? */
	char *name; /* foreign! maybe preset? */
	char *path; /* own */
	message_t *msgs; /* own */
	int uidvalidity;
	int uidnext; /* from SELECT responses */
	unsigned opts; /* maybe preset? */
	/* note that the following do _not_ reflect stats from msgs, but mailbox totals */
	int count; /* # of messages */
	int recent; /* # of recent messages - don't trust this beyond the initial read */
} store_t;

/* When the callback is invoked (at most once per store), the store is fubar;
 * call the driver's cancel_store() to dispose of it. */
static INLINE void
set_bad_callback( store_t *ctx, void (*cb)( void *aux ), void *aux )
{
	ctx->bad_callback = cb;
	ctx->bad_callback_aux = aux;
}

typedef struct {
	char *data;
	int len;
	unsigned char flags;
	time_t time;
} msg_data_t;

#define DRV_OK          0
/* Message went missing, or mailbox is full, etc. */
#define DRV_MSG_BAD     1
/* Something is wrong with the current mailbox - probably it is somehow inaccessible. */
#define DRV_BOX_BAD     2
/* The command has been cancel()ed or cancel_store()d. */
#define DRV_CANCELED    3

/* All memory belongs to the driver's user, unless stated otherwise. */

/*
   This flag says that the driver CAN store messages with CRLFs,
   not that it must. The lack of it OTOH implies that it CANNOT,
   and as CRLF is the canonical format, we convert.
*/
#define DRV_CRLF        1

#define LIST_PATH       1
#define LIST_INBOX      2

struct driver {
	int flags;

	/* Parse configuration. */
	int (*parse_store)( conffile_t *cfg, store_conf_t **storep );

	/* Close remaining server connections. All stores must be disowned first. */
	void (*cleanup)( void );

	/* Open a store with the given configuration. This may recycle existing
	 * server connections. Upon failure, a null store is passed to the callback. */
	void (*open_store)( store_conf_t *conf,
	                    void (*cb)( store_t *ctx, void *aux ), void *aux );

	/* Mark the store as available for recycling. Server connection may be kept alive. */
	void (*disown_store)( store_t *ctx );

	/* Try to recycle a store with the given configuration. */
	store_t *(*own_store)( store_conf_t *conf );

	/* Discard the store after a bad_callback. The server connections will be closed.
	 * Pending commands will have their callbacks synchronously invoked with DRV_CANCELED. */
	void (*cancel_store)( store_t *ctx );

	/* List the mailboxes in this store. Flags are ORed LIST_* values. */
	void (*list)( store_t *ctx, int flags,
	              void (*cb)( int sts, void *aux ), void *aux );

	/* Invoked before select(), this informs the driver which operations (OP_*)
	 * will be performed on the mailbox. The driver may extend the set by implicitly
	 * needed or available operations. */
	void (*prepare_opts)( store_t *ctx, int opts );

	/* Open the mailbox ctx->name. Optionally create missing boxes.
	 * As a side effect, this should resolve ctx->path if applicable. */
	void (*select)( store_t *ctx, int create,
	               void (*cb)( int sts, void *aux ), void *aux );

	/* Load the message attributes needed to perform the requested operations.
	 * Consider only messages with UIDs between minuid and maxuid (inclusive)
	 * and those named in the excs array (smaller than minuid).
	 * The driver takes ownership of the excs array. Messages below newuid do not need
	 * to have the TUID populated even if OPEN_FIND is set. */
	void (*load)( store_t *ctx, int minuid, int maxuid, int newuid, int *excs, int nexcs,
	              void (*cb)( int sts, void *aux ), void *aux );

	/* Fetch the contents and flags of the given message from the current mailbox. */
	void (*fetch_msg)( store_t *ctx, message_t *msg, msg_data_t *data,
	                   void (*cb)( int sts, void *aux ), void *aux );

	/* Store the given message to either the current mailbox or the trash folder.
	 * If the new copy's UID can be immediately determined, return it, otherwise -1. */
	void (*store_msg)( store_t *ctx, msg_data_t *data, int to_trash,
	                   void (*cb)( int sts, int uid, void *aux ), void *aux );

	/* Index the messages which have newly appeared in the mailbox, including their
	 * temporary UID headers. This is needed if store_msg() does not guarantee returning
	 * a UID; otherwise the driver needs to implement only the OPEN_FIND flag. */
	void (*find_new_msgs)( store_t *ctx,
	                       void (*cb)( int sts, void *aux ), void *aux );

	/* Add/remove the named flags to/from the given message. The message may be either
	 * a pre-fetched one (in which case the in-memory representation is updated),
	 * or it may be identifed by UID only. The operation may be delayed until commit()
	 * is called. */
	void (*set_flags)( store_t *ctx, message_t *msg, int uid, int add, int del, /* msg can be null, therefore uid as a fallback */
	                   void (*cb)( int sts, void *aux ), void *aux );

	/* Move the given message from the current mailbox to the trash folder.
	 * This may expunge the original message immediately, but it needn't to. */
	void (*trash_msg)( store_t *ctx, message_t *msg, /* This may expunge the original message immediately, but it needn't to */
	                   void (*cb)( int sts, void *aux ), void *aux );

	/* Expunge deleted messages from the current mailbox and close it.
	 * There is no need to explicitly close a mailbox if no expunge is needed. */
	void (*close)( store_t *ctx, /* IMAP-style: expunge inclusive */
	               void (*cb)( int sts, void *aux ), void *aux );

	/* Cancel queued commands which are not in flight yet; they will have their
	 * callbacks invoked with DRV_CANCELED. Afterwards, wait for the completion of
	 * the in-flight commands. If the store is canceled before this command completes,
	 * the callback will *not* be invoked. */
	void (*cancel)( store_t *ctx,
	                void (*cb)( void *aux ), void *aux );

	/* Commit any pending set_flags() commands. */
	void (*commit)( store_t *ctx );
};


/* main.c */

extern int Pid;
extern char Hostname[256];
extern const char *Home;


/* socket.c */

/* call this before doing anything with the socket */
static INLINE void socket_init( conn_t *conn,
                                const server_conf_t *conf,
                                void (*bad_callback)( void *aux ),
                                void (*read_callback)( void *aux ),
                                int (*write_callback)( void *aux ),
                                void *aux )
{
	conn->conf = conf;
	conn->bad_callback = bad_callback;
	conn->read_callback = read_callback;
	conn->write_callback = write_callback;
	conn->callback_aux = aux;
	conn->fd = -1;
	conn->name = 0;
	conn->write_buf_append = &conn->write_buf;
}
void socket_connect( conn_t *conn, void (*cb)( int ok, void *aux ) );
void socket_start_tls(conn_t *conn, void (*cb)( int ok, void *aux ) );
void socket_close( conn_t *sock );
int socket_read( conn_t *sock, char *buf, int len ); /* never waits */
char *socket_read_line( conn_t *sock ); /* don't free return value; never waits */
typedef enum { KeepOwn = 0, GiveOwn } ownership_t;
int socket_write( conn_t *sock, char *buf, int len, ownership_t takeOwn );

void cram( const char *challenge, const char *user, const char *pass,
           char **_final, int *_finallen );


/* util.c */

#define DEBUG        1
#define VERBOSE      2
#define XVERBOSE     4
#define QUIET        8
#define VERYQUIET    16
#define KEEPJOURNAL  32
#define ZERODELAY    64
#define CRASHDEBUG   128

extern int DFlags;

void ATTR_PRINTFLIKE(1, 2) debug( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) debugn( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) info( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) infon( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) warn( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) error( const char *, ... );
void ATTR_PRINTFLIKE(1, 2) sys_error( const char *, ... );
void flushn( void );

void add_string_list( string_list_t **list, const char *str );
void free_string_list( string_list_t *list );

void free_generic_messages( message_t * );

#ifndef HAVE_MEMRCHR
void *memrchr( const void *s, int c, size_t n );
#endif

void *nfmalloc( size_t sz );
void *nfcalloc( size_t sz );
void *nfrealloc( void *mem, size_t sz );
char *nfstrdup( const char *str );
int nfvasprintf( char **str, const char *fmt, va_list va );
int ATTR_PRINTFLIKE(2, 3) nfasprintf( char **str, const char *fmt, ... );
int ATTR_PRINTFLIKE(3, 4) nfsnprintf( char *buf, int blen, const char *fmt, ... );
void ATTR_NORETURN oob( void );

char *expand_strdup( const char *s );

int map_name( char *arg, char in, char out );

void sort_ints( int *arr, int len );

void arc4_init( void );
unsigned char arc4_getbyte( void );

int bucketsForSize( int size );

#ifdef HAVE_SYS_POLL_H
# include <sys/poll.h>
#else
# define POLLIN 1
# define POLLOUT 4
# define POLLERR 8
#endif

void add_fd( int fd, void (*cb)( int events, void *aux ), void *aux );
void conf_fd( int fd, int and_events, int or_events );
void fake_fd( int fd, int events );
void del_fd( int fd );
void main_loop( void );

/* sync.c */

extern const char *str_ms[2], *str_hl[2];

#define SYNC_OK       0 /* assumed to be 0 */
#define SYNC_FAIL     1
#define SYNC_FAIL_ALL 2
#define SYNC_BAD(ms)  (4<<(ms))
#define SYNC_NOGOOD   16 /* internal */
#define SYNC_CANCELED 32 /* internal */

/* All passed pointers must stay alive until cb is called. */
void sync_boxes( store_t *ctx[], const char *names[], channel_conf_t *chan,
                 void (*cb)( int sts, void *aux ), void *aux );

/* config.c */

#define N_DRIVERS 2
extern driver_t *drivers[N_DRIVERS];

extern channel_conf_t *channels;
extern group_conf_t *groups;
extern int global_ops[2];
extern char *global_sync_state;

#define FSYNC_NONE     0
#define FSYNC_NORMAL   1
#define FSYNC_THOROUGH 2

extern int FSyncLevel;

int parse_bool( conffile_t *cfile );
int parse_int( conffile_t *cfile );
int parse_size( conffile_t *cfile );
int getcline( conffile_t *cfile );
int merge_ops( int cops, int ops[] );
int load_config( const char *filename, int pseudo );
void parse_generic_store( store_conf_t *store, conffile_t *cfg );

/* drv_*.c */
extern driver_t maildir_driver, imap_driver;
