/* Portions of this file are subject to the following copyright(s).  See
 * the Net-SNMP's COPYING file for more details and other copyrights
 * that may apply:
 */
/*
 * Portions of this file are copyrighted by:
 * Copyright Copyright 2003 Sun Microsystems, Inc. All rights reserved.
 * Use is subject to license terms specified in the COPYING file
 * distributed with the Net-SNMP package.
 */
/* 
 * See the following web pages for useful documentation on this transport:
 * http://www.net-snmp.org/wiki/index.php/TUT:Using_TLS
 * http://www.net-snmp.org/wiki/index.php/Using_DTLS
 */

#include <net-snmp/net-snmp-config.h>

#ifdef HAVE_LIBSSL_DTLS

#include <net-snmp/library/snmpDTLSUDPDomain.h>

#include <stdio.h>
#include <sys/types.h>
#include <ctype.h>
#include <errno.h>

#if HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#if HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#if HAVE_NETDB_H
#include <netdb.h>
#endif
#if HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif

#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

#include <net-snmp/types.h>
#include <net-snmp/output_api.h>
#include <net-snmp/config_api.h>

#include <net-snmp/library/snmp_transport.h>
#include <net-snmp/library/system.h>
#include <net-snmp/library/tools.h>
#include <net-snmp/library/callback.h>

#include "openssl/bio.h"
#include "openssl/ssl.h"
#include "openssl/err.h"

#include <net-snmp/library/snmpSocketBaseDomain.h>
#include <net-snmp/library/snmpTLSBaseDomain.h>
#include <net-snmp/library/snmpUDPDomain.h>
#include <net-snmp/library/cert_util.h>
#include <net-snmp/library/snmp_openssl.h>

#ifndef INADDR_NONE
#define INADDR_NONE	-1
#endif

#define WE_ARE_SERVER 0
#define WE_ARE_CLIENT 1

oid             netsnmpDTLSUDPDomain[] = { TRANSPORT_DOMAIN_DTLS_UDP_IP };
size_t          netsnmpDTLSUDPDomain_len = OID_LENGTH(netsnmpDTLSUDPDomain);

static netsnmp_tdomain dtlsudpDomain;

/* this stores openssl credentials for each connection since openssl
   can't do it for us at the moment; hopefully future versions will
   change */
typedef struct bio_cache_s {
   BIO *read_bio;  /* OpenSSL will read its incoming SSL packets from here */
   BIO *write_bio; /* OpenSSL will write its outgoing SSL packets to here */
   struct sockaddr_in sockaddr;
   uint32_t ipv4addr;
   u_short portnum;
   struct bio_cache_s *next;
   int msgnum;
   char *write_cache;
   size_t write_cache_len;
    _netsnmpTLSBaseData *tlsdata;
} bio_cache;

bio_cache *biocache = NULL;

/* this stores remote connections in a list to search through */
/* XXX: optimize for searching */
/* XXX: handle state issues for new connections to reduce DOS issues */
/*      (TLS should do this, but openssl can't do more than one ctx per sock */
/* XXX: put a timer on the cache for expirary purposes */
static bio_cache *find_bio_cache(struct sockaddr_in *from_addr) {
    bio_cache *cachep = NULL;
    cachep = biocache;
    while(cachep) {

        if (cachep->ipv4addr == from_addr->sin_addr.s_addr &&
            cachep->portnum == from_addr->sin_port) {
            /* found an existing connection */
            break;
        }
            
        cachep = cachep->next;
    }
    return cachep;
}

/* XXX: lots of malloc/state cleanup needed */
#define DIEHERE(msg) { snmp_log(LOG_ERR, "%s\n", msg); return NULL; }

static bio_cache *
start_new_cached_connection(netsnmp_transport *t,
                            struct sockaddr_in *remote_addr,
                            int we_are_client) {
    bio_cache *cachep = NULL;
    _netsnmpTLSBaseData *tlsdata;

    DEBUGTRACETOK("dtlsudp");

    /* RFC5953: section 5.3.1, step 1:
       1)  The snmpTlstmSessionOpens counter is incremented.
    */
    if (we_are_client)
        snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONOPENS);

    if (!t->sock)
        DIEHERE("no socket passed in to start_new_cached_connection\n");
    if (!remote_addr)
        DIEHERE("no remote_addr passed in to start_new_cached_connection\n");
        
    cachep = SNMP_MALLOC_TYPEDEF(bio_cache);
    if (!cachep)
        return NULL;
    
    /* allocate our TLS specific data */
    if (NULL == (tlsdata = netsnmp_tlsbase_allocate_tlsdata(t, !we_are_client)))
        return NULL;
    cachep->tlsdata = tlsdata;

    /* RFC5953: section 5.3.1, step 1:
       2)  The client selects the appropriate certificate and cipher_suites
           for the key agreement based on the tmSecurityName and the
           tmRequestedSecurityLevel for the session.  For sessions being
           established as a result of a SNMP-TARGET-MIB based operation, the
           certificate will potentially have been identified via the
           snmpTlstmParamsTable mapping and the cipher_suites will have to
           be taken from system-wide or implementation-specific
           configuration.  If no row in the snmpTlstmParamsTable exists then
           implementations MAY choose to establish the connection using a
           default client certificate available to the application.
           Otherwise, the certificate and appropriate cipher_suites will
           need to be passed to the openSession() ASI as supplemental
           information or configured through an implementation-dependent
           mechanism.  It is also implementation-dependent and possibly
           policy-dependent how tmRequestedSecurityLevel will be used to
           influence the security capabilities provided by the (D)TLS
           connection.  However this is done, the security capabilities
           provided by (D)TLS MUST be at least as high as the level of
           security indicated by the tmRequestedSecurityLevel parameter.
           The actual security level of the session is reported in the
           tmStateReference cache as tmSecurityLevel.  For (D)TLS to provide
           strong authentication, each principal acting as a command
           generator SHOULD have its own certificate.
    */
    /* Implementation notes:
       + This Information is passed in via the transport and default
         paremeters
    */
    /* see if we have base configuration to copy in to this new one */
    if (NULL != t->data && t->data_length == sizeof(_netsnmpTLSBaseData)) {
        _netsnmpTLSBaseData *parentdata = t->data;
        if (parentdata->our_identity)
            tlsdata->our_identity = strdup(parentdata->our_identity);
        if (parentdata->their_identity)
            tlsdata->their_identity = strdup(parentdata->their_identity);
        if (parentdata->their_fingerprint)
            tlsdata->their_fingerprint = strdup(parentdata->their_fingerprint);
        if (parentdata->trust_cert)
            tlsdata->trust_cert = strdup(parentdata->trust_cert);
        if (parentdata->their_hostname)
            tlsdata->their_hostname = strdup(parentdata->their_hostname);
    }
    
    DEBUGMSGTL(("dtlsudp", "starting a new connection\n"));
    cachep->next = biocache;
    biocache = cachep;

    cachep->ipv4addr = remote_addr->sin_addr.s_addr;
    cachep->portnum = remote_addr->sin_port;
    memcpy(&cachep->sockaddr, remote_addr, sizeof(*remote_addr));

    /* create caching memory bios for OpenSSL to read and write to */

    cachep->read_bio = BIO_new(BIO_s_mem()); /* openssl reads from */
    if (!cachep->read_bio)
        DIEHERE("failed to create the openssl read_bio");

    cachep->write_bio = BIO_new(BIO_s_mem()); /* openssl writes to */
    if (!cachep->write_bio) {
        DIEHERE("failed to create the openssl write_bio");
        BIO_free(cachep->read_bio);
    }

    BIO_set_mem_eof_return(cachep->read_bio, -1);
    BIO_set_mem_eof_return(cachep->write_bio, -1);

    if (we_are_client) {
        /* we're the client */
        DEBUGMSGTL(("dtlsudp",
                    "starting a new connection as a client to sock: %d\n",
                    t->sock));
        tlsdata->ssl = SSL_new(sslctx_client_setup(DTLSv1_method(), tlsdata));

        /* XXX: session setting 735 */
    } else {
        /* we're the server */
        
        tlsdata->ssl = SSL_new(sslctx_server_setup(DTLSv1_method()));
    }

    if (!tlsdata->ssl) {
        BIO_free(cachep->read_bio);
        BIO_free(cachep->write_bio);
        DIEHERE("failed to create the SSL session structure");
    }
        
    SSL_set_mode(tlsdata->ssl, SSL_MODE_AUTO_RETRY);

    /* turn on cookie exchange */
    /* XXX: we need to only create cache entries when cookies succeed */
    /* SSL_set_options(tlsdata->ssl, SSL_OP_COOKIE_EXCHANGE); */

    /* set the bios that openssl should read from and write to */
    /* (and we'll do the opposite) */
    SSL_set_bio(tlsdata->ssl, cachep->read_bio, cachep->write_bio);

    /* RFC5953: section 5.3.1, step 1:
       3)  Using the destTransportDomain and destTransportAddress values,
           the client will initiate the (D)TLS handshake protocol to
           establish session keys for message integrity and encryption.

           If the attempt to establish a session is unsuccessful, then
           snmpTlstmSessionOpenErrors is incremented, an error indication is
           returned, and processing stops.  If the session failed to open
           because the presented server certificate was unknown or invalid
           then the snmpTlstmSessionUnknownServerCertificate or
           snmpTlstmSessionInvalidServerCertificates MUST be incremented and
           a snmpTlstmServerCertificateUnknown or
           snmpTlstmServerInvalidCertificate notification SHOULD be sent as
           appropriate.  Reasons for server certificate invalidation
           includes, but is not limited to, cryptographic validation
           failures and an unexpected presented certificate identity.
    */
    /* Implementation notes:
       + Because we're working asyncronously the real "end" point of
         opening a connection doesn't occur here as certificate
         verification and other things needs to happen first in the
         verify callback, etc.  See the netsnmp_dtlsudp_recv()
         function for the final processing.
    */
    /* set the SSL notion of we_are_client/server */
    if (we_are_client)
        SSL_set_connect_state(tlsdata->ssl);
    else
        SSL_set_accept_state(tlsdata->ssl);

    /* RFC5953: section 5.3.1, step 1:
       6)  The TLSTM-specific session identifier (tlstmSessionID) is set in
           the tmSessionID of the tmStateReference passed to the TLS
           Transport Model to indicate that the session has been established
           successfully and to point to a specific (D)TLS connection for
           future use.  The tlstmSessionID is also stored in the LCD for
           later lookup during processing of incoming messages
           (Section 5.1.2).
    */
    /* Implementation notes:
       + our sessionID is stored as the transport's data pointer member
    */
    return cachep;
}

static bio_cache *
find_or_create_bio_cache(netsnmp_transport *t, struct sockaddr_in *from_addr,
                         int we_are_client) {
    bio_cache *cachep = find_bio_cache(from_addr);
    if (NULL == cachep) {
        /* none found; need to start a new context */
        cachep = start_new_cached_connection(t, from_addr, we_are_client);
        if (NULL == cachep) {
            snmp_log(LOG_ERR, "failed to open a new dtls connection\n");
        }
    }
    return cachep;
}

/*
 * Reads data from our internal openssl outgoing BIO and sends any
 * queued packets out the UDP port
 */
static int
_netsnmp_send_queued_dtls_pkts(netsnmp_transport *t, bio_cache *cachep) {
    int outsize, rc2;
    u_char outbuf[65535];
    
    DEBUGTRACETOK("dtlsudp");

    /* for memory bios, we now read from openssl's write
       buffer (ie, the packet to go out) and send it out
       the udp port manually */

    outsize = BIO_read(cachep->write_bio, outbuf, sizeof(outbuf));
    DEBUGMSGTL(("dtlsudp", "have %d bytes to send\n", outsize));
    if (outsize > 0) {
        /* should always be true. */
#if defined(XXXFIXME) && defined(linux) && defined(IP_PKTINFO)
        /* XXX: before this can work, we need to remember address we
           received it from (addr_pair) */
        rc2 = netsnmp_udp_sendto(t->sock, addr_pair->local_addr,
                                 addr_pair->if_index, addr_pair->remote_addr,
                                 outbuf, outsize);
#else
        rc2 = sendto(t->sock, outbuf, outsize, 0,
                     &cachep->sockaddr, sizeof(struct sockaddr));
#endif /* linux && IP_PKTINFO */

        if (rc2 == -1) {
            snmp_log(LOG_ERR, "failed to send a DTLS specific packet\n");
        }
    }
    return outsize;
}

/*
 * If we have any outgoing SNMP data queued that OpenSSL/DTLS couldn't send
 * (likely due to DTLS control packets needing to go out first)
 * then this function attempts to send them.
 */
/* returns SNMPERR_SUCCESS if we succeeded in getting the data out */
/* returns SNMPERR_GENERR if we still need more time */
static int
_netsnmp_bio_try_and_write_buffered(netsnmp_transport *t, bio_cache *cachep) {
    int rc;
    _netsnmpTLSBaseData *tlsdata;
    
    DEBUGTRACETOK("dtlsudp");

    tlsdata = cachep->tlsdata;

    /* make sure we have something to write */
    if (!cachep->write_cache || cachep->write_cache_len == 0)
        return SNMPERR_SUCCESS;

    DEBUGMSGTL(("dtlsudp", "Trying to write %" NETSNMP_PRIz "d of buffered data\n",
                cachep->write_cache_len));

    /* try and write out the cached data */
    rc = SSL_write(tlsdata->ssl, cachep->write_cache, cachep->write_cache_len);

    while (rc == -1) {
        int errnum = SSL_get_error(tlsdata->ssl, rc);
        int bytesout;

        /* don't treat want_read/write errors as real errors */
        if (errnum != SSL_ERROR_WANT_READ &&
            errnum != SSL_ERROR_WANT_WRITE) {
            DEBUGMSGTL(("dtlsudp", "ssl_write error (of buffered data)\n")); 
            _openssl_log_error(rc, tlsdata->ssl, "SSL_write");
            return SNMPERR_GENERR;
        }

        /* check to see if we have outgoing DTLS packets to send */
        /* (SSL_write could have created DTLS control packets) */ 
        bytesout = _netsnmp_send_queued_dtls_pkts(t, cachep);

        /* If want_read/write but failed to actually send anything
           then we need to wait for the other side, so quit */
        if ((errnum == SSL_ERROR_WANT_READ ||
             errnum == SSL_ERROR_WANT_WRITE) &&
            bytesout <= 0) {
            /* we've failed; must need to wait longer */
            return SNMPERR_GENERR;
        }

        /* retry writing */
        DEBUGMSGTL(("dtlsudp", "recalling ssl_write\n")); 
        rc = SSL_write(tlsdata->ssl, cachep->write_cache,
                       cachep->write_cache_len);
    }

    if (rc > 0)
        cachep->msgnum++;
    
    if (_netsnmp_send_queued_dtls_pkts(t, cachep) > 0) {
        SNMP_FREE(cachep->write_cache);
        cachep->write_cache_len = 0;
        DEBUGMSGTL(("dtlsudp", "  Write was successful\n"));
        return SNMPERR_SUCCESS;
    }
    DEBUGMSGTL(("dtlsudp", "  failed to send over UDP socket\n"));
    return SNMPERR_GENERR;
}

static int
_netsnmp_add_buffered_data(bio_cache *cachep, char *buf, size_t size) {
    if (cachep->write_cache && cachep->write_cache_len > 0) {
        size_t newsize = cachep->write_cache_len + size;

        char *newbuf = realloc(cachep->write_cache, newsize);
        if (NULL == newbuf) {
            /* ack! malloc failure */
            /* XXX: free and close */
            return SNMPERR_GENERR;
        }
        cachep->write_cache = newbuf;

        /* write the new packet to the end */
        memcpy(cachep->write_cache + cachep->write_cache_len,
               buf, size);
        cachep->write_cache_len = newsize;
    } else {
        if (SNMPERR_SUCCESS !=
            memdup((u_char **) &cachep->write_cache, buf, size)) {
            /* ack! malloc failure */
            /* XXX: free and close */
            return SNMPERR_GENERR;
        }
        cachep->write_cache_len = size;
    }
    return SNMPERR_SUCCESS;
}

static int
netsnmp_dtlsudp_recv(netsnmp_transport *t, void *buf, int size,
                     void **opaque, int *olength)
{
    int             rc = -1;
    socklen_t       fromlen = sizeof(struct sockaddr);
    netsnmp_indexed_addr_pair *addr_pair = NULL;
    struct sockaddr *from;
    netsnmp_tmStateReference *tmStateRef = NULL;
    _netsnmpTLSBaseData *tlsdata;
    bio_cache *cachep;

    DEBUGTRACETOK("dtlsudp");

    if (NULL == t || t->sock == 0)
        return -1;

    /* create a tmStateRef cache for slow fill-in */
    tmStateRef = SNMP_MALLOC_TYPEDEF(netsnmp_tmStateReference);

    if (tmStateRef == NULL) {
        *opaque = NULL;
        *olength = 0;
        return -1;
    }

    /* Set the transportDomain */
    memcpy(tmStateRef->transportDomain,
           netsnmpDTLSUDPDomain, sizeof(netsnmpDTLSUDPDomain[0]) *
           netsnmpDTLSUDPDomain_len);
    tmStateRef->transportDomainLen = netsnmpDTLSUDPDomain_len;

    addr_pair = &tmStateRef->addresses;
    tmStateRef->have_addresses = 1;
    from = (struct sockaddr *) &(addr_pair->remote_addr);

    while (rc < 0) {
#if defined(linux) && defined(IP_PKTINFO)
        socklen_t local_addr_len = sizeof(addr_pair->local_addr);
        rc = netsnmp_udp_recvfrom(t->sock, buf, size, from, &fromlen,
                                  (struct sockaddr*)&(addr_pair->local_addr),
                                  &local_addr_len, &(addr_pair->if_index));
#else
        rc = recvfrom(t->sock, buf, size, NETSNMP_DONTWAIT, from, &fromlen);
#endif /* linux && IP_PKTINFO */
        if (rc < 0 && errno != EINTR) {
            break;
        }
    }

    DEBUGMSGTL(("dtlsudp", "received %d raw bytes on way to dtls\n", rc));
    if (rc < 0) {
        DEBUGMSGTL(("dtlsudp", "recvfrom fd %d err %d (\"%s\")\n",
                    t->sock, errno, strerror(errno)));
        SNMP_FREE(tmStateRef);
        return -1;
    }

    /* now that we have the from address filled in, we can look up
       the openssl context and have openssl read and process
       appropriately */

    /* RFC5953: section 5.1, step 1:
    1)  Determine the tlstmSessionID for the incoming message.  The
        tlstmSessionID MUST be a unique session identifier for this
        (D)TLS connection.  The contents and format of this identifier
        are implementation-dependent as long as it is unique to the
        session.  A session identifier MUST NOT be reused until all
        references to it are no longer in use.  The tmSessionID is equal
        to the tlstmSessionID discussed in Section 5.1.1. tmSessionID
        refers to the session identifier when stored in the
        tmStateReference and tlstmSessionID refers to the session
        identifier when stored in the LCD.  They MUST always be equal
        when processing a given session's traffic.

        If this is the first message received through this session and
        the session does not have an assigned tlstmSessionID yet then the
        snmpTlstmSessionAccepts counter is incremented and a
        tlstmSessionID for the session is created.  This will only happen
        on the server side of a connection because a client would have
        already assigned a tlstmSessionID during the openSession()
        invocation.  Implementations may have performed the procedures
        described in Section 5.3.2 prior to this point or they may
        perform them now, but the procedures described in Section 5.3.2
        MUST be performed before continuing beyond this point.
    */

    /* RFC5953: section 5.1, step 2:
       2)  Create a tmStateReference cache for the subsequent reference and
           assign the following values within it:

           tmTransportDomain  = snmpTLSTCPDomain or snmpDTLSUDPDomain as
              appropriate.

           tmTransportAddress  = The address the message originated from.

           tmSecurityLevel  = The derived tmSecurityLevel for the session,
              as discussed in Section 3.1.2 and Section 5.3.

           tmSecurityName  = The derived tmSecurityName for the session as
              discussed in Section 5.3.  This value MUST be constant during
              the lifetime of the session.

           tmSessionID  = The tlstmSessionID described in step 1 above.
    */

    /* if we don't have a cachep for this connection then
       we're receiving something new and are the server
       side */
    cachep =
        find_or_create_bio_cache(t, &addr_pair->remote_addr, WE_ARE_SERVER);
    if (NULL == cachep) {
        snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONACCEPTS);
        SNMP_FREE(tmStateRef);
        return -1;
    }
    tlsdata = cachep->tlsdata;

    /* Implementation notes:
       - we use the t->data memory pointer as the session ID
       - the transport domain is already the correct type if we got here
       - if we don't have a session yet (eg, no tmSessionID from the
         specs) then we create one automatically here.
    */

    /* write the received buffer to the memory-based input bio */
    BIO_write(cachep->read_bio, buf, rc);

    /* RFC5953: section 5.1, step 3:
       3)  The incomingMessage and incomingMessageLength are assigned values
           from the (D)TLS processing.
     */
    /* Implementation notes:
       + rc = incomingMessageLength
       + buf = IncomingMessage
    */

    /* XXX: in Wes' other example we do a SSL_pending() call
       too to ensure we're ready to read...  it's possible
       that buffered stuff in openssl won't be caught by the
       net-snmp select loop because it's already been pulled
       out; need to deal with this) */
    rc = SSL_read(tlsdata->ssl, buf, size);
            
    while (rc == -1) {
        int errnum = SSL_get_error(tlsdata->ssl, rc);
        int bytesout;

        /* don't treat want_read/write errors as real errors */
        if (errnum != SSL_ERROR_WANT_READ &&
            errnum != SSL_ERROR_WANT_WRITE) {
            _openssl_log_error(rc, tlsdata->ssl, "SSL_read");
            break;
        }

        /* check to see if we have outgoing DTLS packets to send */
        /* (SSL_read could have created DTLS control packets) */ 
        bytesout = _netsnmp_send_queued_dtls_pkts(t, cachep);

        /* If want_read/write but failed to actually send
           anything then we need to wait for the other side,
           so quit */
        if ((errnum == SSL_ERROR_WANT_READ ||
             errnum == SSL_ERROR_WANT_WRITE) &&
            bytesout <= 0)
            break;

        /* retry reading */
        DEBUGMSGTL(("dtlsudp", "recalling ssl_read\n")); 
        rc = SSL_read(tlsdata->ssl, buf, size);
    }

    DEBUGMSGTL(("dtlsudp",
                "received %d decoded bytes from dtls\n", rc));

    if (rc == -1) {
        SNMP_FREE(tmStateRef);

        if (SSL_get_error(tlsdata->ssl, rc) == SSL_ERROR_WANT_READ) {
            DEBUGMSGTL(("dtlsudp","here: want read!\n"));

            /* see if we have buffered write date to send out first */
            if (cachep->write_cache) {
                _netsnmp_bio_try_and_write_buffered(t, cachep);
                /* XXX: check error or not here? */
                /* (what would we do differently?) */
            }

            return -1; /* XXX: it's ok, but what's the right return? */
        }
        _openssl_log_error(rc, tlsdata->ssl, "SSL_read");
        return rc;
    }

    /* Until we've locally assured ourselves that all is well in
       certificate-verification-land we need to be prepared to stop
       here and ensure all our required checks have been done. */ 
    if (0 == (tlsdata->flags & NETSNMP_TLSBASE_CERT_FP_VERIFIED)) {
        int verifyresult;

        if (tlsdata->flags & NETSNMP_TLSBASE_IS_CLIENT) {

            /* verify that the server's certificate is the correct one */

    	    /* RFC5953: section 5.3.1, step 1:
    	       3)  Using the destTransportDomain and
    	           destTransportAddress values, the client will
    	           initiate the (D)TLS handshake protocol to establish
    	           session keys for message integrity and encryption.

    	           If the attempt to establish a session is
    	           unsuccessful, then snmpTlstmSessionOpenErrors is
    	           incremented, an error indication is returned, and
    	           processing stops.  If the session failed to open
    	           because the presented server certificate was
    	           unknown or invalid then the
    	           snmpTlstmSessionUnknownServerCertificate or
    	           snmpTlstmSessionInvalidServerCertificates MUST be
    	           incremented and a snmpTlstmServerCertificateUnknown
    	           or snmpTlstmServerInvalidCertificate notification
    	           SHOULD be sent as appropriate.  Reasons for server
    	           certificate invalidation includes, but is not
    	           limited to, cryptographic validation failures and
    	           an unexpected presented certificate identity.
    	    */
    	    /* RFC5953: section 5.3.1, step 1:
    	       4)  The (D)TLS client MUST then verify that the (D)TLS
    	           server's presented certificate is the expected
    	           certificate.  The (D)TLS client MUST NOT transmit
    	           SNMP messages until the server certificate has been
    	           authenticated, the client certificate has been
    	           transmitted and the TLS connection has been fully
    	           established.

    	           If the connection is being established from
    	           configuration based on SNMP-TARGET-MIB
    	           configuration, then the snmpTlstmAddrTable
    	           DESCRIPTION clause describes how the verification
    	           is done (using either a certificate fingerprint, or
    	           an identity authenticated via certification path
    	           validation).

    	           If the connection is being established for reasons
    	           other than configuration found in the
    	           SNMP-TARGET-MIB then configuration and procedures
    	           outside the scope of this document should be
    	           followed.  Configuration mechanisms SHOULD be
    	           similar in nature to those defined in the
    	           snmpTlstmAddrTable to ensure consistency across
    	           management configuration systems.  For example, a
    	           command-line tool for generating SNMP GETs might
    	           support specifying either the server's certificate
    	           fingerprint or the expected host name as a command
    	           line argument.
    	    */
    	    /* RFC5953: section 5.3.1, step 1:
    	       5)  (D)TLS provides assurance that the authenticated
    	           identity has been signed by a trusted configured
    	           certification authority.  If verification of the
    	           server's certificate fails in any way (for example
    	           because of failures in cryptographic verification
    	           or the presented identity did not match the
    	           expected named entity) then the session
    	           establishment MUST fail, the
    	           snmpTlstmSessionInvalidServerCertificates object is
    	           incremented.  If the session can not be opened for
    	           any reason at all, including cryptographic
    	           verification failures and snmpTlstmCertToTSNTable
    	           lookup failures, then the
    	           snmpTlstmSessionOpenErrors counter is incremented
    	           and processing stops.
    	    */

	    /* Implementation notes:
	       + in the following function the server's certificate and
	         presented commonname or subjectAltName is checked
	         according to the rules in the snmpTlstmAddrTable.
	    */ 
            if ((verifyresult = netsnmp_tlsbase_verify_server_cert(tlsdata->ssl, tlsdata))
                != SNMPERR_SUCCESS) {
                if (verifyresult == SNMPERR_TLS_NO_CERTIFICATE) {
                    /* assume we simply haven't received it yet and there
                       is more data to wait-for or send */
                    /* XXX: probably need to check for whether we should
                       send stuff from our end to continue the transaction
                    */
                    return -1;
                } else {
                    /* XXX: free needed memory */
                    snmp_log(LOG_ERR,
                             "DTLSUDP: failed to verify ssl certificate (of the server)\n");
		    snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONUNKNOWNSERVERCERTIFICATE);
		    /* Step 5 says these are always incremented */
		    snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONINVALIDSERVERCERTIFICATES);
		    snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONOPENERRORS);
                    return -1;
                }
            }
            tlsdata->flags |= NETSNMP_TLSBASE_CERT_FP_VERIFIED;
            DEBUGMSGTL(("dtlsudp", "Verified the server's certificate\n"));
        } else {
            /* verify that the client's certificate is the correct one */
        
            if ((verifyresult = netsnmp_tlsbase_verify_client_cert(tlsdata->ssl, tlsdata))
                != SNMPERR_SUCCESS) {
                if (verifyresult == SNMPERR_TLS_NO_CERTIFICATE) {
                    /* assume we simply haven't received it yet and there
                       is more data to wait-for or send */
                    /* XXX: probably need to check for whether we should
                       send stuff from our end to continue the transaction
                    */
                    return -1;
                } else {
                    /* XXX: free needed memory */
                    snmp_log(LOG_ERR,
                             "DTLSUDP: failed to verify ssl certificate (of the client)\n");
                    snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONINVALIDCLIENTCERTIFICATES);
                    return -1;
                }
            }
            tlsdata->flags |= NETSNMP_TLSBASE_CERT_FP_VERIFIED;
            DEBUGMSGTL(("dtlsudp", "Verified the client's certificate\n"));
        }
    }

    if (rc > 0)
        cachep->msgnum++;

    if (BIO_ctrl_pending(cachep->write_bio) > 0) {
        _netsnmp_send_queued_dtls_pkts(t, cachep);
    }

    {
        char *str = netsnmp_udp_fmtaddr(NULL, addr_pair, sizeof(netsnmp_indexed_addr_pair));
        DEBUGMSGTL(("dtlsudp",
                    "recvfrom fd %d got %d bytes (from %s)\n",
                    t->sock, rc, str));
        free(str);
    }

    /* see if we have buffered write date to send out first */
    if (cachep->write_cache) {
        if (SNMPERR_GENERR ==
            _netsnmp_bio_try_and_write_buffered(t, cachep)) {
            /* we still have data that can't get out in the buffer */
            /* XXX: nothing to do here? */
        }
    }

    if (netsnmp_tlsbase_wrapup_recv(tmStateRef, tlsdata, opaque, olength) !=
        SNMPERR_SUCCESS)
        return SNMPERR_GENERR;

    /* RFC5953: section 5.1, step 4:
       4)  The TLS Transport Model passes the transportDomain,
           transportAddress, incomingMessage, and incomingMessageLength to
           the Dispatcher using the receiveMessage ASI:

          statusInformation =
          receiveMessage(
          IN   transportDomain     -- snmpTLSTCPDomain or snmpDTLSUDPDomain,
          IN   transportAddress    -- address for the received message
          IN   incomingMessage        -- the whole SNMP message from (D)TLS
          IN   incomingMessageLength  -- the length of the SNMP message
          IN   tmStateReference    -- transport info
           )
    */
    /* Implementation notes: those pamateres are all passed outward
       using the functions arguments and the return code below (the length) */

    return rc;
}



static int
netsnmp_dtlsudp_send(netsnmp_transport *t, void *buf, int size,
		 void **opaque, int *olength)
{
    int rc = -1;
    netsnmp_indexed_addr_pair *addr_pair = NULL;
    struct sockaddr *to = NULL;
    bio_cache *cachep = NULL;
    netsnmp_tmStateReference *tmStateRef = NULL;
    u_char outbuf[65535];
    _netsnmpTLSBaseData *tlsdata = NULL;
    
    DEBUGTRACETOK("dtlsudp");

    /* determine remote addresses */
    if (opaque != NULL && *opaque != NULL &&
        *olength == sizeof(netsnmp_tmStateReference)) {
        tmStateRef = (netsnmp_tmStateReference *) *opaque;

        if (tmStateRef->have_addresses)
            addr_pair = &(tmStateRef->addresses);
        else if (t != NULL && t->data != NULL &&
                 t->data_length == sizeof(netsnmp_indexed_addr_pair))
            addr_pair = (netsnmp_indexed_addr_pair *) (t->data);
        else if (t != NULL && t->data != NULL &&
                 t->data_length == sizeof(_netsnmpTLSBaseData)) {
            tlsdata = (_netsnmpTLSBaseData *) t->data;
            addr_pair = (netsnmp_indexed_addr_pair *) (tlsdata->remote_addr);
        }
    } else if (t != NULL && t->data != NULL &&
               t->data_length == sizeof(netsnmp_indexed_addr_pair)) {
        addr_pair = (netsnmp_indexed_addr_pair *) (t->data);
    } else if (t != NULL && t->data != NULL &&
               t->data_length == sizeof(_netsnmpTLSBaseData)) {
        tlsdata = (_netsnmpTLSBaseData *) t->data;
        addr_pair = (netsnmp_indexed_addr_pair *) (tlsdata->remote_addr);
    }

    if (NULL == addr_pair) {
      /* RFC5953: section 5.2, step 1:
       1)  If tmStateReference does not refer to a cache containing values
           for tmTransportDomain, tmTransportAddress, tmSecurityName,
           tmRequestedSecurityLevel, and tmSameSecurity, then increment the
           snmpTlstmSessionInvalidCaches counter, discard the message, and
           return the error indication in the statusInformation.  Processing
           of this message stops.
      */
        snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONINVALIDCACHES);
        snmp_log(LOG_ERR, "dtlsudp_send: can't get address to send to\n");
        return -1;
    }

    to = (struct sockaddr *) &(addr_pair->remote_addr);

    if (NULL == to || NULL == t || t->sock <= 0) {
        snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONINVALIDCACHES);
        snmp_log(LOG_ERR, "invalid netsnmp_dtlsudp_send usage\n");
        return -1;
    }

    /* RFC5953: section 5.2, step 2:
       2)  Extract the tmSessionID, tmTransportDomain, tmTransportAddress,
           tmSecurityName, tmRequestedSecurityLevel, and tmSameSecurity
           values from the tmStateReference.  Note: The tmSessionID value
           may be undefined if no session exists yet over which the message
           can be sent.
    */
    /* Implementation notes:
       - we use the t->data memory pointer as the session ID
       - the transport domain is already the correct type if we got here
       - if we don't have a session yet (eg, no tmSessionID from the
         specs) then we create one automatically here.
    */


    /* RFC5953: section 5.2, step 3:
       3)  If tmSameSecurity is true and either tmSessionID is undefined or
           refers to a session that is no longer open then increment the
           snmpTlstmSessionNoSessions counter, discard the message and
           return the error indication in the statusInformation.  Processing
           of this message stops.
    */
    /* RFC5953: section 5.2, step 4:
       4)  If tmSameSecurity is false and tmSessionID refers to a session
           that is no longer available then an implementation SHOULD open a
           new session using the openSession() ASI (described in greater
           detail in step 5b).  Instead of opening a new session an
           implementation MAY return a snmpTlstmSessionNoSessions error to
           the calling module and stop processing of the message.
    */
    /* Implementation Notes:
       - We would never get here if the sessionID was different.  We
         tie packets directly to the transport object and it could
         never be sent back over a different transport, which is what
         the above text is trying to prevent.
       - Auto-connections are handled higher in the Net-SNMP library stack
     */

    /* RFC5953: section 5.2, step 5:
       5)  If tmSessionID is undefined, then use tmTransportDomain,
           tmTransportAddress, tmSecurityName and tmRequestedSecurityLevel
           to see if there is a corresponding entry in the LCD suitable to
           send the message over.

           5a)  If there is a corresponding LCD entry, then this session
                will be used to send the message.

           5b)  If there is no corresponding LCD entry, then open a session
                using the openSession() ASI (discussed further in
                Section 5.3.1).  Implementations MAY wish to offer message
                buffering to prevent redundant openSession() calls for the
                same cache entry.  If an error is returned from
                openSession(), then discard the message, discard the
                tmStateReference, increment the snmpTlstmSessionOpenErrors,
                return an error indication to the calling module and stop
                processing of the message.
    */

    /* we're always a client if we're sending to something unknown yet */
    if (NULL ==
        (cachep = find_or_create_bio_cache(t, &addr_pair->remote_addr,
                                           WE_ARE_CLIENT))) {
        snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONOPENERRORS);
        return -1;
    }

    tlsdata = cachep->tlsdata;
    if (NULL == tlsdata || NULL == tlsdata->ssl) {
        /** xxx mem lean? free created bio cache? */
        snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONNOSESSIONS);
        snmp_log(LOG_ERR, "bad tls data or ssl ptr in netsnmp_dtlsudp_send\n");
        return -1;
    }
        
    if (!tlsdata->securityName && tmStateRef &&
	tmStateRef->securityNameLen > 0) {
        tlsdata->securityName = strdup(tmStateRef->securityName);
    }

    /* see if we have previous outgoing data to send */
    if (cachep->write_cache) {
        if (SNMPERR_GENERR == _netsnmp_bio_try_and_write_buffered(t, cachep)) {
            /* we still have data that can't get out in the buffer */

            /* add the new data to the end of the existing cache */
            if (_netsnmp_add_buffered_data(cachep, buf, size) !=
                SNMPERR_SUCCESS) {
                /* XXX: free and close */
            }
            return -1;
        }
    }

    {
        char *str = netsnmp_udp_fmtaddr(NULL, (void *) addr_pair,
                                        sizeof(netsnmp_indexed_addr_pair));
        DEBUGMSGTL(("dtlsudp", "send %d bytes from %p to %s on fd %d\n",
                    size, buf, str, t->sock));
        free(str);
    }

    /* RFC5953: section 5.2, step 6:
       6)  Using either the session indicated by the tmSessionID if there
           was one or the session resulting from a previous step (4 or 5),
           pass the outgoingMessage to (D)TLS for encapsulation and
           transmission.
    */
    rc = SSL_write(tlsdata->ssl, buf, size);

    while (rc == -1) {
        int bytesout;
        int errnum = SSL_get_error(tlsdata->ssl, rc);

        /* don't treat want_read/write errors as real errors */
        if (errnum != SSL_ERROR_WANT_READ &&
            errnum != SSL_ERROR_WANT_WRITE) {
            DEBUGMSGTL(("dtlsudp", "ssl_write error\n")); 
            _openssl_log_error(rc, tlsdata->ssl, "SSL_write");
            break;
        }

        /* check to see if we have outgoing DTLS packets to send */
        /* (SSL_read could have created DTLS control packets) */ 
        bytesout = _netsnmp_send_queued_dtls_pkts(t, cachep);

        /* If want_read/write but failed to actually send
           anything then we need to wait for the other side,
           so quit */
        if ((errnum == SSL_ERROR_WANT_READ ||
             errnum == SSL_ERROR_WANT_WRITE) &&
            bytesout <= 0) {
            /* We need more data written to or read from the socket
               but we're failing to do so and need to wait till the
               socket is ready again; unfortunately this means we need
               to buffer the SNMP data temporarily in the mean time */

            /* remember the packet */
            if (_netsnmp_add_buffered_data(cachep, buf, size) !=
                SNMPERR_SUCCESS) {

                /* XXX: free and close */
                return -1;
            }

            /* exit out of the loop until we get caled again from
               socket data */ 
            break;
        }
        DEBUGMSGTL(("dtlsudp", "recalling ssl_write\n")); 
        rc = SSL_write(tlsdata->ssl, buf, size);
    }

    if (rc > 0)
        cachep->msgnum++;

    /* for memory bios, we now read from openssl's write buffer (ie,
       the packet to go out) and send it out the udp port manually */
    rc = BIO_read(cachep->write_bio, outbuf, sizeof(outbuf));
    if (rc <= 0) {
        /* in theory an ok thing */
        return 0;
    }
#if defined(FIXME) && defined(linux) && defined(IP_PKTINFO)
    /* XXX: before this can work, we need to remember address we
       received it from (addr_pair) */
    rc = netsnmp_udpbase_sendto(t->sock, &cachep->sockaddr  remote  addr_pair ? &(addr_pair->local_addr) : NULL, to, outbuf, rc);
#else
    rc = sendto(t->sock, outbuf, rc, 0, &cachep->sockaddr,
                sizeof(struct sockaddr));
#endif /* linux && IP_PKTINFO */

    return rc;
}



static int
netsnmp_dtlsudp_close(netsnmp_transport *t)
{
    /* XXX: issue a proper dtls closure notification(s) */

    bio_cache *cachep = NULL;
    _netsnmpTLSBaseData *tlsbase = NULL;

    DEBUGTRACETOK("dtlsudp");

    /* RFC5953: section 5.4, step 1:
        1)  Increment either the snmpTlstmSessionClientCloses or the
            snmpTlstmSessionServerCloses counter as appropriate.
    */
    snmp_increment_statistic(STAT_TLSTM_SNMPTLSTMSESSIONCLIENTCLOSES);
    
    /* RFC5953: section 5.4, step 2:
        2)  Look up the session using the tmSessionID.
    */
    /* Implementation notes:
       + Our session id is stored as the t->data pointer
    */
    if (NULL != t->data && t->data_length == sizeof(_netsnmpTLSBaseData)) {
        tlsbase = (_netsnmpTLSBaseData *) t->data;

        if (tlsbase->remote_addr)
            cachep = find_bio_cache((struct sockaddr_in *)tlsbase->remote_addr);
    }

    /* RFC5953: section 5.4, step 3:
        3)  If there is no open session associated with the tmSessionID, then
            closeSession processing is completed.
    */
    /* if we have any remaining packtes to send, try to send them */
    if (NULL != cachep && cachep->write_cache_len > 0) {
        int i = 0;
        char buf[8192];
        int rc;
        void *opaque = NULL;
        int opaque_len = 0;
        fd_set readfs;
        struct timeval tv;

        /* make configurable:
           - do this at all?
           - retries
           - timeout
        */
        while (i < 6 && cachep->write_cache_len != 0) {

            /* first see if we can send out what we have */
            _netsnmp_bio_try_and_write_buffered(t, cachep);

            if (cachep->write_cache_len != 0) {

                /* if we've failed that, we probably need to wait for packets */
                FD_ZERO(&readfs);
                FD_SET(t->sock, &readfs);
                tv.tv_sec = 0;
                tv.tv_usec = 500000;

                rc = select(1, &readfs, NULL, NULL, &tv);
                if (1 || rc > 0) {
                    /* junk recv for catching negotations still in play */
                    netsnmp_dtlsudp_recv(t, buf, sizeof(buf),
                                         &opaque, &opaque_len);
                    if (opaque)
                        SNMP_FREE(opaque);
                    opaque_len = 0;
                }
            }
            i++;
        }
    }

    /* RFC5953: section 5.4, step 4:
        4)  Have (D)TLS close the specified connection.  This MUST include
            sending a close_notify TLS Alert to inform the other side that
            session cleanup may be performed.
    */
    if (NULL != tlsbase && NULL != tlsbase->ssl)
        SSL_shutdown(tlsbase->ssl);
    return netsnmp_socketbase_close(t);
}

char *
netsnmp_dtlsudp_fmtaddr(netsnmp_transport *t, void *data, int len)
{
    return netsnmp_ipv4_fmtaddr("DTLSUDP", t, data, len);
}

/*
 * Open a DTLS-based transport for SNMP.  Local is TRUE if addr is the local
 * address to bind to (i.e. this is a server-type session); otherwise addr is 
 * the remote address to send things to.  
 */

netsnmp_transport *
netsnmp_dtlsudp_transport(struct sockaddr_in *addr, int local)
{
    netsnmp_transport *t = NULL;

    DEBUGTRACETOK("dtlsudp");

    t = netsnmp_udpipv4base_transport(addr, local);
    if (NULL == t)
        return NULL;

    if (NULL != t->data &&
        t->data_length == sizeof(netsnmp_indexed_addr_pair)) {
        _netsnmpTLSBaseData *tlsdata =
            netsnmp_tlsbase_allocate_tlsdata(t, local);
        tlsdata->remote_addr = t->data;
        t->data = tlsdata;
        t->data_length = sizeof(_netsnmpTLSBaseData);
    }

    if (!local) {
        /* dtls needs to bind the socket for SSL_write to work */
        if (connect(t->sock, (struct sockaddr *) addr, sizeof(*addr)) == -1)
            snmp_log(LOG_ERR, "dtls: failed to connect\n");
    }

    /* XXX: Potentially set sock opts here (SO_SNDBUF/SO_RCV_BUF) */      
    /* XXX: and buf size */        

    /*
     * Set Domain
     */
    t->domain = netsnmpDTLSUDPDomain;                                     
    t->domain_length = netsnmpDTLSUDPDomain_len;     

    /*
     * 16-bit length field, 8 byte DTLS header, 20 byte IPv4 header  
     */

    t->msgMaxSize      = 0xffff - 8 - 20;
    t->f_recv          = netsnmp_dtlsudp_recv;
    t->f_send          = netsnmp_dtlsudp_send;
    t->f_close         = netsnmp_dtlsudp_close;
    t->f_config        = netsnmp_tlsbase_config;
    t->f_setup_session = netsnmp_tlsbase_session_init;
    t->f_accept        = NULL;
    t->f_fmtaddr       = netsnmp_dtlsudp_fmtaddr;

    t->flags = NETSNMP_TRANSPORT_FLAG_TUNNELED;

    return t;
}


void
netsnmp_dtlsudp_agent_config_tokens_register(void)
{
}




netsnmp_transport *
netsnmp_dtlsudp_create_tstring(const char *str, int isserver,
                               const char *default_target)
{
    struct sockaddr_in addr;
    netsnmp_transport *t;
    _netsnmpTLSBaseData *tlsdata;
    char buf[SPRINT_MAX_LEN], *cp;

    if (netsnmp_sockaddr_in2(&addr, str, default_target)) {
        t = netsnmp_dtlsudp_transport(&addr, isserver);
    } else {
        return NULL;
    }

    /* see if we can extract the remote hostname */
    if (!isserver && t && t->data && str) {
        tlsdata = (_netsnmpTLSBaseData *) t->data;
        /* search for a : */
        if (NULL != (cp = strrchr(str, ':'))) {
            strncpy(buf, str, SNMP_MIN(cp-str, sizeof(buf)-1));
            buf[SNMP_MIN(cp-str, sizeof(buf)-1)] = '\0';
        } else {
            /* else the entire spec is a host name only */
            strncpy(buf, str,
                    SNMP_MIN(strlen(str), sizeof(buf)-1));
            buf[SNMP_MIN(strlen(str), sizeof(buf)-1)] = '\0';
        }
        tlsdata->their_hostname = strdup(buf);
    }
    return t;
}


netsnmp_transport *
netsnmp_dtlsudp_create_ostring(const u_char * o, size_t o_len, int local)
{
    struct sockaddr_in addr;

    if (o_len == 6) {
        unsigned short porttmp = (o[4] << 8) + o[5];
        addr.sin_family = AF_INET;
        memcpy((u_char *) & (addr.sin_addr.s_addr), o, 4);
        addr.sin_port = htons(porttmp);
        return netsnmp_dtlsudp_transport(&addr, local);
    }
    return NULL;
}

void
netsnmp_dtlsudp_ctor(void)
{
    DEBUGMSGTL(("dtlsudp", "registering DTLS constructor\n"));

    /* config settings */

    dtlsudpDomain.name = netsnmpDTLSUDPDomain;
    dtlsudpDomain.name_length = netsnmpDTLSUDPDomain_len;
    dtlsudpDomain.prefix = (const char**)calloc(3, sizeof(char *));
    dtlsudpDomain.prefix[0] = "dtlsudp";
    dtlsudpDomain.prefix[1] = "dtls";

    dtlsudpDomain.f_create_from_tstring_new = netsnmp_dtlsudp_create_tstring;
    dtlsudpDomain.f_create_from_ostring = netsnmp_dtlsudp_create_ostring;

    netsnmp_tdomain_register(&dtlsudpDomain);
}
#endif /* HAVE_LIBSSL_DTLS */
