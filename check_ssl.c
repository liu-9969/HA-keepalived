/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        SSL GET CHECK. Perform an ssl get query to a specified
 *              url, compute a MD5 over this result and match it to the
 *              expected value.
 *
 * Version:     $Id: check_ssl.c,v 0.6.2 2002/06/16 05:23:31 acassen Exp $
 *
 * Authors:     Alexandre Cassen, <acassen@linux-vs.org>
 *              Jan Holmberg, <jan@artech.net>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 */

#include <openssl/err.h>
#include "check_ssl.h"
#include "check_api.h"
#include "memory.h"
#include "parser.h"
#include "smtp.h"
#include "utils.h"

extern data *conf_data;

/* SSL primitives */
/* Free an SSL context */
void clear_ssl(SSL_DATA *ssl)
{
  if (ssl)
    if (ssl->ctx)
      SSL_CTX_free(ssl->ctx);
}

/* PEM password callback function */
static int password_cb(char *buf, int num, int rwflag, void *userdata)
{
  SSL_DATA *ssl = (SSL_DATA *)userdata;

  if (num < strlen(ssl->password)+1)
    return(0);

  strcpy(buf, ssl->password);
  return(strlen(ssl->password));
}

/* Inititalize global SSL context */
static BIO *bio_err = 0;
static int build_ssl_ctx(void)
{
  SSL_DATA *ssl;

  /* Library initialization */
  SSL_library_init();

  SSL_load_error_strings();
  bio_err = BIO_new_fp(stderr, BIO_NOCLOSE);

  if (!conf_data->ssl)
    ssl = (SSL_DATA *)MALLOC(sizeof(ssl_data));
  else
    ssl = conf_data->ssl;

  /* Initialize SSL context for SSL v2/3 */
  ssl->meth = SSLv23_method();
  ssl->ctx  = SSL_CTX_new(ssl->meth);

  /* return for autogen context */
  if (!conf_data->ssl) {
    conf_data->ssl = ssl;
    goto end;
  }

  /* Load our keys and certificates */
  if (conf_data->ssl->keyfile)
    if (!(SSL_CTX_use_certificate_chain_file(ssl->ctx, conf_data->ssl->keyfile))) {
      syslog(LOG_INFO, "SSL error : Cant load certificate file...");
      return 0;
    }

  /* Handle password callback using userdata ssl */
  if (conf_data->ssl->password) {
    SSL_CTX_set_default_passwd_cb_userdata(ssl->ctx, conf_data->ssl);
    SSL_CTX_set_default_passwd_cb(ssl->ctx, password_cb);
  }

  if (conf_data->ssl->keyfile)
    if (!(SSL_CTX_use_PrivateKey_file(ssl->ctx, conf_data->ssl->keyfile
                                              , SSL_FILETYPE_PEM))) {
      syslog(LOG_INFO, "SSL error : Cant load key file...");
      return 0;
    }

  /* Load the CAs we trust */
  if (conf_data->ssl->cafile)
    if (!(SSL_CTX_load_verify_locations(ssl->ctx, conf_data->ssl->cafile, 0))) {
      syslog(LOG_INFO, "SSL error : Cant load CA file...");
      return 0;
    }

end:
#if (OPENSSL_VERSION_NUMBER < 0x00905100L)
  SSL_CTX_set_verify_depth(ssl->ctx, 1);
#endif

  return 1;
}

/*
 * Initialize the SSL context, with or without specific
 * configuration files.
 */
int init_ssl_ctx(void)
{
  SSL_DATA *ssl = conf_data->ssl;

  if (!build_ssl_ctx()) {
    syslog(LOG_INFO, "Error Initialize SSL, ctx Instance");
    syslog(LOG_INFO, "  SSL  keyfile:%s", ssl->keyfile);
    syslog(LOG_INFO, "  SSL password:%s", ssl->password);
    syslog(LOG_INFO, "  SSL   cafile:%s", ssl->cafile);
    syslog(LOG_INFO, "Terminate...\n");
    clear_ssl(ssl);
    return 0;
  }
  return 1;
}

/* Display SSL error to readable string */
int ssl_printerr(int err)
{
  unsigned long extended_error = 0;
  char *ssl_strerr;

  switch (err) {
    case SSL_ERROR_ZERO_RETURN:
      syslog(LOG_DEBUG, "  SSL error: (zero return)");
      break;
    case SSL_ERROR_WANT_READ:
      syslog(LOG_DEBUG, "  SSL error: (read error)");
      break;
    case SSL_ERROR_WANT_WRITE:
      syslog(LOG_DEBUG, "  SSL error: (write error)");
      break;
    case SSL_ERROR_WANT_CONNECT:
      syslog(LOG_DEBUG, "  SSL error: (connect error)");
      break;
    case SSL_ERROR_WANT_X509_LOOKUP:
      syslog(LOG_DEBUG, "  SSL error: (X509 lookup error)");
      break;
    case SSL_ERROR_SYSCALL:
      syslog(LOG_DEBUG, "  SSL error: (syscall error)");
      break;
    case SSL_ERROR_SSL: {
      ssl_strerr = (char *)MALLOC(500);

      extended_error = ERR_get_error();
      ERR_error_string(extended_error, ssl_strerr);
      syslog(LOG_DEBUG, "  SSL error: (%s)", ssl_strerr);
      FREE(ssl_strerr);
      break;
    }
  }
  return 0;
}

int ssl_connect(thread *thread)
{
  checker *checker                 = THREAD_ARG(thread);
  http_get_checker *http_get_check = CHECKER_ARG(checker);
  http_arg *http_arg               = HTTP_ARG(http_get_check);
  REQ *req                         = HTTP_REQ(http_arg);

  req->ssl = SSL_new(conf_data->ssl->ctx);
  req->bio = BIO_new_socket(thread->u.fd, BIO_NOCLOSE);
  SSL_set_bio(req->ssl, req->bio, req->bio);

  return (SSL_connect(req->ssl) > 0)?1:0;
}

int ssl_send_request(SSL *ssl, char *str_request, int request_len)
{
  int err, r = 0;

  while (1) {
    err = 1;
    r = SSL_write(ssl, str_request, request_len);
    if (SSL_ERROR_NONE != SSL_get_error(ssl, r))
      break;
    err++;
    if (request_len != r)
      break;
    err++;
    break;
  }

  return (err == 3)?1:0;
}

/* Asynchronous SSL stream reader */
int ssl_read_thread(thread *thread)
{
  checker *checker                 = THREAD_ARG(thread);
  http_get_checker *http_get_check = CHECKER_ARG(checker);
  http_arg *http_arg               = HTTP_ARG(http_get_check);
  REQ *req                         = HTTP_REQ(http_arg);
  unsigned char digest[16];
  int r = 0;

  /* Handle read timeout */
  if (thread->type == THREAD_READ_TIMEOUT &&
      !req->extracted)
    return timeout_epilog(thread, "=> SSL CHECK failed on service"
                                  " : recevice data <=\n\n"
                                , "SSL read");

  /* read the SSL stream */
  r = SSL_read(req->ssl, req->buffer + req->len
                       , MAX_BUFFER_LENGTH - req->len);
  req->error = SSL_get_error(req->ssl, r);

  if (req->error) {

    /* All the SSL streal has been parsed */
    MD5_Final(digest, &req->context);
    SSL_set_quiet_shutdown(req->ssl, 1); 

    r = (req->error == SSL_ERROR_ZERO_RETURN) ? SSL_shutdown(req->ssl) : 0;

    if (r && !req->extracted) {
      /* check if server is currently alive */
      if (ISALIVE(checker->rs)) {
        smtp_alert(thread->master, checker->rs
                                 , NULL
                                 , "DOWN"
                                 , "=> SSL CHECK failed on service"
                                   " : cannot receive data <=\n\n");
        perform_svr_state(DOWN, checker->vs, checker->rs);
      }
      return epilog(thread,1,0,0);
    }

    /* Handle response stream */
    http_handle_response(thread, digest, (!req->extracted)?1:0);

  } else if (r > 0 && req->error == 0) {

    req->len += r;
    if (!req->extracted) {
       if ((req->extracted = extract_html(req->buffer, req->len))) { 
         r = req->len-(req->extracted-req->buffer);
         if (r) {
           memcpy(req->buffer, req->extracted, r);
           MD5_Update(&req->context, req->buffer, r);
           r=0;
         }
         req->len = r;
       } else {
         /* minimize buffer using no 2*CR/LF found yet */
         if (req->len > 3) {
           memcpy(req->buffer, req->buffer + req->len - 3, 3);
           req->len = 3;
         }
       }
    } else {
      if (req->len) {
        MD5_Update(&req->context, req->buffer, req->len);
        req->len = 0;
      }
    }

    /*
     * Register next ssl stream reader.
     * Register itself to not perturbe global I/O multiplexer.
     */
    thread_add_read(thread->master, ssl_read_thread
                                  , checker
                                  , thread->u.fd
                                  , http_get_check->connection_to);
  }

  return 0;
}