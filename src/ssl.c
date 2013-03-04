
/*
 *	This OpenSSL interface code has been proudly copied from
 *	the excellent NGINX web server.
 *
 *	Its license is reproduced here.
 */

/* 
 * Copyright (C) 2002-2013 Igor Sysoev
 * Copyright (C) 2011-2013 Nginx, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 *	OpenSSL thread-safe example code (ssl_thread_* functions) have been
 *	proudly copied from the excellent CURL package, the original author
 *	is Jeremy Brown.
 *
 *	https://github.com/bagder/curl/blob/master/docs/examples/opensslthreadlock.c
 *
 * COPYRIGHT AND PERMISSION NOTICE
 * Copyright (c) 1996 - 2013, Daniel Stenberg, <daniel@haxx.se>.
 *
 * All rights reserved.
 * 
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name of a copyright holder shall not
 * be used in advertising or otherwise to promote the sale, use or other dealings
 * in this Software without prior written authorization of the copyright holder.
 *	
 */

#include "config.h"
#include "ssl.h"
#include "hlog.h"
#include "hmalloc.h"
#include "worker.h"

#ifdef USE_SSL

#include <pthread.h>
#include <openssl/conf.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define SSL_DEFAULT_CIPHERS     "HIGH:!aNULL:!MD5"
#define SSL_PROTOCOLS (NGX_SSL_SSLv3|NGX_SSL_TLSv1 |NGX_SSL_TLSv1_1|NGX_SSL_TLSv1_2)

/* pthread wrapping for openssl */
#define MUTEX_TYPE       pthread_mutex_t
#define MUTEX_SETUP(x)   pthread_mutex_init(&(x), NULL)
#define MUTEX_CLEANUP(x) pthread_mutex_destroy(&(x))
#define MUTEX_LOCK(x)    pthread_mutex_lock(&(x))
#define MUTEX_UNLOCK(x)  pthread_mutex_unlock(&(x))
#define THREAD_ID        pthread_self(  )

int  ssl_available;
int  ssl_connection_index;
int  ssl_server_conf_index;
int  ssl_session_cache_index;


/* This array will store all of the mutexes available to OpenSSL. */
static MUTEX_TYPE *mutex_buf= NULL;

static void ssl_thread_locking_function(int mode, int n, const char * file, int line)
{
	if (mode & CRYPTO_LOCK)
		MUTEX_LOCK(mutex_buf[n]);
	else
		MUTEX_UNLOCK(mutex_buf[n]);
}

static unsigned long ssl_thread_id_function(void)
{
	return ((unsigned long)THREAD_ID);
}

static int ssl_thread_setup(void)
{
	int i;
	
	hlog(LOG_DEBUG, "Creating OpenSSL mutexes (%d)...", CRYPTO_num_locks());
	
	mutex_buf = hmalloc(CRYPTO_num_locks() * sizeof(MUTEX_TYPE));
	
	for (i = 0;  i < CRYPTO_num_locks();  i++)
		MUTEX_SETUP(mutex_buf[i]);
	
	CRYPTO_set_id_callback(ssl_thread_id_function);
	CRYPTO_set_locking_callback(ssl_thread_locking_function);
	
	return 0;
}

static int ssl_thread_cleanup(void)
{
	int i;
	
	if (!mutex_buf)
		return 0;
	
	CRYPTO_set_id_callback(NULL);
	CRYPTO_set_locking_callback(NULL);
	
	for (i = 0;  i < CRYPTO_num_locks(  );  i++)
		MUTEX_CLEANUP(mutex_buf[i]);
		
	hfree(mutex_buf);
	mutex_buf = NULL;
	
	return 0;
}

int ssl_init(void)
{
	hlog(LOG_INFO, "Initializing OpenSSL...");
	
	OPENSSL_config(NULL);
	
	SSL_library_init();
	SSL_load_error_strings();
	
	ssl_thread_setup();
	
	OpenSSL_add_all_algorithms();
	
#if OPENSSL_VERSION_NUMBER >= 0x0090800fL
#ifndef SSL_OP_NO_COMPRESSION
	{
	/*
	 * Disable gzip compression in OpenSSL prior to 1.0.0 version,
	 * this saves about 522K per connection.
	 */
	int                  n;
	STACK_OF(SSL_COMP)  *ssl_comp_methods;
	
	ssl_comp_methods = SSL_COMP_get_compression_methods();
	n = sk_SSL_COMP_num(ssl_comp_methods);
	
	while (n--) {
		(void) sk_SSL_COMP_pop(ssl_comp_methods);
	}
	
	}
#endif
#endif

	ssl_connection_index = SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
	
	if (ssl_connection_index == -1) {
		hlog(LOG_ERR, "SSL_get_ex_new_index() failed");
		return -1;
	}
	
	ssl_server_conf_index = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
	
	if (ssl_server_conf_index == -1) {
		hlog(LOG_ERR, "SSL_CTX_get_ex_new_index() failed");
		return -1;
	}
	
	ssl_session_cache_index = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
	
	if (ssl_session_cache_index == -1) {
		hlog(LOG_ERR, "SSL_CTX_get_ex_new_index() failed");
		return -1;
	}
	
	ssl_available = 1;
	
	return 0;
}

void ssl_atend(void)
{
	ssl_thread_cleanup();
}

struct ssl_t *ssl_alloc(void)
{
	struct ssl_t *ssl;
	
	ssl = hmalloc(sizeof(*ssl));
	memset(ssl, 0, sizeof(*ssl));
	
	return ssl;
}

void ssl_free(struct ssl_t *ssl)
{
	if (ssl->ctx)
	    SSL_CTX_free(ssl->ctx);
	    
	hfree(ssl);
}

int ssl_create(struct ssl_t *ssl, void *data)
{
	ssl->ctx = SSL_CTX_new(SSLv23_method());
	
	if (ssl->ctx == NULL) {
		hlog(LOG_ERR, "SSL_CTX_new() failed");
		return -1;
	}
	
	if (SSL_CTX_set_ex_data(ssl->ctx, ssl_server_conf_index, data) == 0) {
		hlog(LOG_ERR, "SSL_CTX_set_ex_data() failed");
		return -1;
	}
	
	/* client side options */
	
	SSL_CTX_set_options(ssl->ctx, SSL_OP_MICROSOFT_SESS_ID_BUG);
	SSL_CTX_set_options(ssl->ctx, SSL_OP_NETSCAPE_CHALLENGE_BUG);
	
	/* server side options */
	
	SSL_CTX_set_options(ssl->ctx, SSL_OP_SSLREF2_REUSE_CERT_TYPE_BUG);
	SSL_CTX_set_options(ssl->ctx, SSL_OP_MICROSOFT_BIG_SSLV3_BUFFER);
	
	/* this option allow a potential SSL 2.0 rollback (CAN-2005-2969) */
	SSL_CTX_set_options(ssl->ctx, SSL_OP_MSIE_SSLV2_RSA_PADDING);
	
	SSL_CTX_set_options(ssl->ctx, SSL_OP_SSLEAY_080_CLIENT_DH_BUG);
	SSL_CTX_set_options(ssl->ctx, SSL_OP_TLS_D5_BUG);
	SSL_CTX_set_options(ssl->ctx, SSL_OP_TLS_BLOCK_PADDING_BUG);
	
	SSL_CTX_set_options(ssl->ctx, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
	
	SSL_CTX_set_options(ssl->ctx, SSL_OP_SINGLE_DH_USE);
	
	/* SSL protocols not configurable for now */
	int protocols = SSL_PROTOCOLS;
	
	if (!(protocols & NGX_SSL_SSLv2)) {
		SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_SSLv2);
	}
	if (!(protocols & NGX_SSL_SSLv3)) {
		SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_SSLv3);
	}
	if (!(protocols & NGX_SSL_TLSv1)) {
		SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_TLSv1);
	}
	
#ifdef SSL_OP_NO_TLSv1_1
	if (!(protocols & NGX_SSL_TLSv1_1)) {
		SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_TLSv1_1);
	}
#endif
#ifdef SSL_OP_NO_TLSv1_2
	if (!(protocols & NGX_SSL_TLSv1_2)) {
		SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_TLSv1_2);
	}
#endif

#ifdef SSL_OP_NO_COMPRESSION
	SSL_CTX_set_options(ssl->ctx, SSL_OP_NO_COMPRESSION);
#endif

#ifdef SSL_MODE_RELEASE_BUFFERS
	SSL_CTX_set_mode(ssl->ctx, SSL_MODE_RELEASE_BUFFERS);
#endif
	
	SSL_CTX_set_read_ahead(ssl->ctx, 1);
	
	//SSL_CTX_set_info_callback(ssl->ctx, ngx_ssl_info_callback);
	
	if (SSL_CTX_set_cipher_list(ssl->ctx, SSL_DEFAULT_CIPHERS) == 0) {
		hlog(LOG_ERR, "SSL_CTX_set_cipher_list() failed");
		return -1;
	}
	
	/* prefer server-selected ciphers */
	SSL_CTX_set_options(ssl->ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
	
	return 0;
}

/*
 *	Load server key and certificate
 */

int ssl_certificate(struct ssl_t *ssl, const char *certfile, const char *keyfile)
{
	if (SSL_CTX_use_certificate_chain_file(ssl->ctx, certfile) == 0) {
		hlog(LOG_ERR, "SSL_CTX_use_certificate_chain_file(\"%s\") failed", certfile);
		return -1;
	}
	
	
	if (SSL_CTX_use_PrivateKey_file(ssl->ctx, keyfile, SSL_FILETYPE_PEM) == 0) {
		hlog(LOG_ERR, "SSL_CTX_use_PrivateKey_file(\"%s\") failed", keyfile);
		return -1;
	}
	
	return 0;
}

/*
 *	Create a connect */

int ssl_create_connection(struct ssl_t *ssl, struct client_t *c, int i_am_client)
{
	struct ssl_connection_t  *sc;
	
	sc = hmalloc(sizeof(*sc));
	
	//sc->buffer = ((flags & NGX_SSL_BUFFER) != 0);
	sc->connection = SSL_new(ssl->ctx);
	
	if (sc->connection == NULL) {
		hlog(LOG_ERR, "SSL_new() failed (fd %d)", c->fd);
		return -1;
	}
	
	if (SSL_set_fd(sc->connection, c->fd) == 0) {
		hlog(LOG_ERR, "SSL_set_fd() failed (fd %d)", c->fd);
		return -1;
	}
	
	if (i_am_client) {
		SSL_set_connect_state(sc->connection);
	} else {
		SSL_set_accept_state(sc->connection);
	}
	
	if (SSL_set_ex_data(sc->connection, ssl_connection_index, c) == 0) {
		hlog(LOG_ERR, "SSL_set_ex_data() failed (fd %d)", c->fd);
		return -1;
	}
	
	c->ssl_con = sc;
	
	return 0;
}


int ssl_write(struct worker_t *self, struct client_t *c)
{
	int n;
	unsigned long sslerr;
	int err;
	int to_write;
	
	to_write = c->obuf_end - c->obuf_start;
	
	/* SSL_write does not appreciate writing a 0-length buffer */
	if (to_write == 0) {
		/* tell the poller that we have no outgoing data */
		xpoll_outgoing(&self->xp, c->xfd, 0);
		return 0;
	}
	
	//hlog(LOG_DEBUG, "ssl_write fd %d of %d bytes", c->fd, to_write);
	
	n = SSL_write(c->ssl_con->connection, c->obuf + c->obuf_start, to_write);
	
	//hlog(LOG_DEBUG, "SSL_write fd %d returned %d", c->fd, n);
	
	if (n > 0) {
		/* ok, we wrote some */
		c->obuf_start += n;
		c->obuf_wtime = tick;
		
		/* All done ? */
		if (c->obuf_start >= c->obuf_end) {
			//hlog(LOG_DEBUG, "ssl_write fd %d (%s) obuf empty", c->fd, c->addr_rem);
			c->obuf_start = 0;
			c->obuf_end   = 0;
			
			/* tell the poller that we have no outgoing data */
			xpoll_outgoing(&self->xp, c->xfd, 0);
			return n;
		}
		
		xpoll_outgoing(&self->xp, c->xfd, 1);
		
		return n;
	}
	
	sslerr = SSL_get_error(c->ssl_con->connection, n);
	err = (sslerr == SSL_ERROR_SYSCALL) ? errno : 0;
	
	if (err) {
		hlog(LOG_DEBUG, "ssl_write fd %d: I/O syscall error: %s", c->fd, strerror(err));
	} else {
		char ebuf[255];
		
		ERR_error_string_n(sslerr, ebuf, sizeof(ebuf));
		hlog(LOG_DEBUG, "ssl_write fd %d: SSL_get_error %u: %s (%s)", c->fd, sslerr, ebuf, ERR_reason_error_string(sslerr));
	}
	
	if (sslerr == SSL_ERROR_WANT_WRITE) {
		hlog(LOG_INFO, "ssl_write fd %d: says SSL_ERROR_WANT_WRITE, marking socket for write events", c->fd);
		
		/* tell the poller that we have outgoing data */
		xpoll_outgoing(&self->xp, c->xfd, 1);
		
		return 0;
	}
	
	if (sslerr == SSL_ERROR_WANT_READ) {
		hlog(LOG_INFO, "ssl_write fd %d: says SSL_ERROR_WANT_READ, returning 0", c->fd);
		
		return 0;
	}
	
	c->ssl_con->no_wait_shutdown = 1;
	c->ssl_con->no_send_shutdown = 1;
	
	hlog(LOG_DEBUG, "ssl_write fd %d: SSL_write() failed", c->fd);
	client_close(self, c, errno);
	
	return -13;
}

int ssl_writeable(struct worker_t *self, struct client_t *c)
{
	//hlog(LOG_DEBUG, "ssl_writeable fd %d", c->fd);
	
	return ssl_write(self, c);
}

int ssl_readable(struct worker_t *self, struct client_t *c)
{
	int r;
	int sslerr, err;
	
	//hlog(LOG_DEBUG, "ssl_readable fd %d", c->fd);
	
	r = SSL_read(c->ssl_con->connection, c->ibuf + c->ibuf_end, c->ibuf_size - c->ibuf_end - 1);
	
	if (r > 0) {
		/* we got some data... process */
		//hlog(LOG_DEBUG, "SSL_read fd %d returned %d bytes of data", c->fd, r);
		
		/* TODO: whatever the client_readable does */
		return client_postread(self, c, r);
	}
	
	sslerr = SSL_get_error(c->ssl_con->connection, r);
	err = (sslerr == SSL_ERROR_SYSCALL) ? errno : 0;
	
	hlog(LOG_DEBUG, "ssl_readable fd %d: SSL_get_error: %d", c->fd, sslerr);
	
	if (sslerr == SSL_ERROR_WANT_READ) {
		hlog(LOG_DEBUG, "ssl_readable fd %d: SSL_read says SSL_ERROR_WANT_READ, doing it later", c->fd);
		
		if (c->obuf_end - c->obuf_start > 0) {
			/* tell the poller that we have outgoing data */
			xpoll_outgoing(&self->xp, c->xfd, 1);
		}
		
		return 0;
	}
	
	if (sslerr == SSL_ERROR_WANT_WRITE) {
		hlog(LOG_INFO, "ssl_readable fd %d: SSL_read says SSL_ERROR_WANT_WRITE (peer starts SSL renegotiation?), calling ssl_writeable", c->fd);
		return ssl_writeable(self, c);
	}
	
	c->ssl_con->no_wait_shutdown = 1;
	c->ssl_con->no_send_shutdown = 1;
	
	if (sslerr == SSL_ERROR_ZERO_RETURN || ERR_peek_error() == 0) {
		hlog(LOG_DEBUG, "ssl_readable fd %d: peer shutdown SSL cleanly", c->fd);
		client_close(self, c, CLIERR_EOF);
		return -1;
	}
	
	hlog(LOG_DEBUG, "ssl_readable fd %d: SSL_read() failed", c->fd);
	client_close(self, c, errno);
	return -1;
}


#endif

