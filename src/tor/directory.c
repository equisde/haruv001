/* Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2016, The Tor Project, Inc. */
/* See LICENSE for licensing information */

#define DIRECTORY_PRIVATE

#include "or.h"
#include "backtrace.h"
#include "bridges.h"
#include "buffers.h"
#include "circuitbuild.h"
#include "config.h"
#include "connection.h"
#include "connection_edge.h"
#include "control.h"
#define DIRECTORY_PRIVATE
#include "directory.h"
#include "dirserv.h"
#include "dirvote.h"
#include "entrynodes.h"
#include "geoip.h"
#include "hs_cache.h"
#include "hs_common.h"
#include "tormain.h"
#include "microdesc.h"
#include "networkstatus.h"
#include "nodelist.h"
#include "policies.h"
#include "relay.h"
#include "rendclient.h"
#include "rendcommon.h"
#include "rendservice.h"
#include "rephist.h"
#include "router.h"
#include "routerlist.h"
#include "routerparse.h"
#include "routerset.h"
#include "shared_random.h"

#if defined(EXPORTMALLINFO) && defined(HAVE_MALLOC_H) && defined(HAVE_MALLINFO)
#if !defined(OpenBSD)
#include <malloc.h>
#endif
#endif

/**
 * \file directory.c
 * \brief Code to send and fetch information from directory authorities and
 * caches via HTTP.
 *
 * Directory caches and authorities use dirserv.c to generate the results of a
 * query and stream them to the connection; clients use routerparse.c to parse
 * them.
 *
 * Every directory request has a dir_connection_t on the client side and on
 * the server side.  In most cases, the dir_connection_t object is a linked
 * connection, tunneled through an edge_connection_t so that it can be a
 * stream on the Tor network.  The only non-tunneled connections are those
 * that are used to upload material (descriptors and votes) to authorities.
 * Among tunneled connections, some use one-hop circuits, and others use
 * multi-hop circuits for anonymity.
 *
 * Directory requests are launched by calling
 * directory_initiate_command_rend() or one of its numerous variants. This
 * launch the connection, will construct an HTTP request with
 * directory_send_command(), send the and wait for a response.  The client
 * later handles the response with connection_dir_client_reached_eof(),
 * which passes the information received to another part of Tor.
 *
 * On the server side, requests are read in directory_handle_command(),
 * which dispatches first on the request type (GET or POST), and then on
 * the URL requested. GET requests are processed with a table-based
 * dispatcher in url_table[].  The process of handling larger GET requests
 * is complicated because we need to avoid allocating a copy of all the
 * data to be sent to the client in one huge buffer.  Instead, we spool the
 * data into the buffer using logic in connection_dirserv_flushed_some() in
 * dirserv.c.  (TODO: If we extended buf.c to have a zero-copy
 * reference-based buffer type, we could remove most of that code, at the
 * cost of a bit more reference counting.)
 **/

/* In-points to directory.c:
 *
 * - directory_post_to_dirservers(), called from
 *   router_upload_dir_desc_to_dirservers() in router.c
 *   upload_service_descriptor() in rendservice.c
 * - directory_get_from_dirserver(), called from
 *   rend_client_refetch_renddesc() in rendclient.c
 *   run_scheduled_events() in main.c
 *   do_hup() in main.c
 * - connection_dir_process_inbuf(), called from
 *   connection_process_inbuf() in connection.c
 * - connection_dir_finished_flushing(), called from
 *   connection_finished_flushing() in connection.c
 * - connection_dir_finished_connecting(), called from
 *   connection_finished_connecting() in connection.c
 */
static void directory_send_command(dir_connection_t *conn,
                             int purpose, int direct, const char *resource,
                             const char *payload, size_t payload_len,
                             time_t if_modified_since);
static int body_is_plausible(const char *body, size_t body_len, int purpose);
static char *http_get_header(const char *headers, const char *which);
static void http_set_address_origin(const char *headers, connection_t *conn);
static void connection_dir_download_routerdesc_failed(dir_connection_t *conn);
static void connection_dir_bridge_routerdesc_failed(dir_connection_t *conn);
static void connection_dir_download_cert_failed(
                               dir_connection_t *conn, int status_code);
static void connection_dir_retry_bridges(smartlist_t *descs);
static void dir_routerdesc_download_failed(smartlist_t *failed,
                                           int status_code,
                                           int router_purpose,
                                           int was_extrainfo,
                                           int was_descriptor_digests);
static void dir_microdesc_download_failed(smartlist_t *failed,
                                          int status_code);
static int client_likes_consensus(networkstatus_t *v, const char *want_url);

static void directory_initiate_command_rend(
                                          const tor_addr_port_t *or_addr_port,
                                          const tor_addr_port_t *dir_addr_port,
                                          const char *digest,
                                          uint8_t dir_purpose,
                                          uint8_t router_purpose,
                                          dir_indirection_t indirection,
                                          const char *resource,
                                          const char *payload,
                                          size_t payload_len,
                                          time_t if_modified_since,
                                          const rend_data_t *rend_query,
                                          circuit_guard_state_t *guard_state);

static void connection_dir_close_consensus_fetches(
                   dir_connection_t *except_this_one, const char *resource);

/********* START VARIABLES **********/

/** How far in the future do we allow a directory server to tell us it is
 * before deciding that one of us has the wrong time? */
#define ALLOW_DIRECTORY_TIME_SKEW (30*60)

#define X_ADDRESS_HEADER "X-Your-Address-Is: "

/** HTTP cache control: how long do we tell proxies they can cache each
 * kind of document we serve? */
#define FULL_DIR_CACHE_LIFETIME (60*60)
#define RUNNINGROUTERS_CACHE_LIFETIME (20*60)
#define DIRPORTFRONTPAGE_CACHE_LIFETIME (20*60)
#define NETWORKSTATUS_CACHE_LIFETIME (5*60)
#define ROUTERDESC_CACHE_LIFETIME (30*60)
#define ROUTERDESC_BY_DIGEST_CACHE_LIFETIME (48*60*60)
#define ROBOTS_CACHE_LIFETIME (24*60*60)
#define MICRODESC_CACHE_LIFETIME (48*60*60)

/********* END VARIABLES ************/

/** Return false if the directory purpose <b>dir_purpose</b>
 * does not require an anonymous (three-hop) connection.
 *
 * Return true 1) by default, 2) if all directory actions have
 * specifically been configured to be over an anonymous connection,
 * or 3) if the router is a bridge */
int
purpose_needs_anonymity(uint8_t dir_purpose, uint8_t router_purpose,
                        const char *resource)
{
  if (get_options()->AllDirActionsPrivate)
    return 1;

  if (router_purpose == ROUTER_PURPOSE_BRIDGE) {
    if (dir_purpose == DIR_PURPOSE_FETCH_SERVERDESC
        && resource && !strcmp(resource, "authority.z")) {
      /* We are asking a bridge for its own descriptor. That doesn't need
         anonymity. */
      return 0;
    }
    /* Assume all other bridge stuff needs anonymity. */
    return 1; /* if no circuits yet, this might break bootstrapharu, but it's
               * needed to be safe. */
  }

  switch (dir_purpose)
  {
    case DIR_PURPOSE_UPLOAD_DIR:
    case DIR_PURPOSE_UPLOAD_VOTE:
    case DIR_PURPOSE_UPLOAD_SIGNATURES:
    case DIR_PURPOSE_FETCH_STATUS_VOTE:
    case DIR_PURPOSE_FETCH_DETACHED_SIGNATURES:
    case DIR_PURPOSE_FETCH_CONSENSUS:
    case DIR_PURPOSE_FETCH_CERTIFICATE:
    case DIR_PURPOSE_FETCH_SERVERDESC:
    case DIR_PURPOSE_FETCH_EXTRAINFO:
    case DIR_PURPOSE_FETCH_MICRODESC:
      return 0;
    case DIR_PURPOSE_HAS_FETCHED_RENDDESC_V2:
    case DIR_PURPOSE_UPLOAD_RENDDESC_V2:
    case DIR_PURPOSE_FETCH_RENDDESC_V2:
      return 1;
    case DIR_PURPOSE_SERVER:
    default:
      log_warn(LD_BUG, "Called with dir_purpose=%d, router_purpose=%d",
               dir_purpose, router_purpose);
      tor_assert_nonfatal_unreached();
      return 1; /* Assume it needs anonymity; better safe than sorry. */
  }
}

/** Return a newly allocated string describing <b>auth</b>. Only describes
 * authority features. */
STATIC char *
authdir_type_to_string(dirinfo_type_t auth)
{
  char *result;
  smartlist_t *lst = smartlist_new();
  if (auth & V3_DIRINFO)
    smartlist_add(lst, (void*)"V3");
  if (auth & BRIDGE_DIRINFO)
    smartlist_add(lst, (void*)"Bridge");
  if (smartlist_len(lst)) {
    result = smartlist_join_strings(lst, ", ", 0, NULL);
  } else {
    result = tor_strdup("[Not an authority]");
  }
  smartlist_free(lst);
  return result;
}

/** Return a string describing a given directory connection purpose. */
STATIC const char *
dir_conn_purpose_to_string(int purpose)
{
  switch (purpose)
    {
    case DIR_PURPOSE_UPLOAD_DIR:
      return "server descriptor upload";
    case DIR_PURPOSE_UPLOAD_VOTE:
      return "server vote upload";
    case DIR_PURPOSE_UPLOAD_SIGNATURES:
      return "consensus signature upload";
    case DIR_PURPOSE_FETCH_SERVERDESC:
      return "server descriptor fetch";
    case DIR_PURPOSE_FETCH_EXTRAINFO:
      return "extra-info fetch";
    case DIR_PURPOSE_FETCH_CONSENSUS:
      return "consensus network-status fetch";
    case DIR_PURPOSE_FETCH_CERTIFICATE:
      return "authority cert fetch";
    case DIR_PURPOSE_FETCH_STATUS_VOTE:
      return "status vote fetch";
    case DIR_PURPOSE_FETCH_DETACHED_SIGNATURES:
      return "consensus signature fetch";
    case DIR_PURPOSE_FETCH_RENDDESC_V2:
      return "hidden-service v2 descriptor fetch";
    case DIR_PURPOSE_UPLOAD_RENDDESC_V2:
      return "hidden-service v2 descriptor upload";
    case DIR_PURPOSE_FETCH_MICRODESC:
      return "microdescriptor fetch";
    }

  log_warn(LD_BUG, "Called with unknown purpose %d", purpose);
  return "(unknown)";
}

/** Return the requisite directory information types. */
STATIC dirinfo_type_t
dir_fetch_type(int dir_purpose, int router_purpose, const char *resource)
{
  dirinfo_type_t type;
  switch (dir_purpose) {
    case DIR_PURPOSE_FETCH_EXTRAINFO:
      type = EXTRAINFO_DIRINFO;
      if (router_purpose == ROUTER_PURPOSE_BRIDGE)
        type |= BRIDGE_DIRINFO;
      else
        type |= V3_DIRINFO;
      break;
    case DIR_PURPOSE_FETCH_SERVERDESC:
      if (router_purpose == ROUTER_PURPOSE_BRIDGE)
        type = BRIDGE_DIRINFO;
      else
        type = V3_DIRINFO;
      break;
    case DIR_PURPOSE_FETCH_STATUS_VOTE:
    case DIR_PURPOSE_FETCH_DETACHED_SIGNATURES:
    case DIR_PURPOSE_FETCH_CERTIFICATE:
      type = V3_DIRINFO;
      break;
    case DIR_PURPOSE_FETCH_CONSENSUS:
      type = V3_DIRINFO;
      if (resource && !strcmp(resource, "microdesc"))
        type |= MICRODESC_DIRINFO;
      break;
    case DIR_PURPOSE_FETCH_MICRODESC:
      type = MICRODESC_DIRINFO;
      break;
    default:
      log_warn(LD_BUG, "Unexpected purpose %d", (int)dir_purpose);
      type = NO_DIRINFO;
      break;
  }
  return type;
}

/** Return true iff <b>identity_digest</b> is the digest of a router which
 * says that it caches extrainfos.  (If <b>is_authority</b> we always
 * believe that to be true.) */
int
router_supports_extrainfo(const char *identity_digest, int is_authority)
{
  const node_t *node = node_get_by_id(identity_digest);

  if (node && node->ri) {
    if (node->ri->caches_extra_info)
      return 1;
  }
  if (is_authority) {
    return 1;
  }
  return 0;
}

/** Return true iff any trusted directory authority has accepted our
 * server descriptor.
 *
 * We consider any authority sufficient because waiting for all of
 * them means it never happens while any authority is down; we don't
 * go for something more complex in the middle (like \>1/3 or \>1/2 or
 * \>=1/2) because that doesn't seem necessary yet.
 */
int
directories_have_accepted_server_descriptor(void)
{
  const smartlist_t *servers = router_get_trusted_dir_servers();
  const or_options_t *options = get_options();
  SMARTLIST_FOREACH(servers, dir_server_t *, d, {
    if ((d->type & options->PublishServerDescriptor_) &&
        d->has_accepted_serverdesc) {
      return 1;
    }
  });
  return 0;
}

/** Start a connection to every suitable directory authority, using
 * connection purpose <b>dir_purpose</b> and uploading <b>payload</b>
 * (of length <b>payload_len</b>). The dir_purpose should be one of
 * 'DIR_PURPOSE_UPLOAD_{DIR|VOTE|SIGNATURES}'.
 *
 * <b>router_purpose</b> describes the type of descriptor we're
 * publishing, if we're publishing a descriptor -- e.g. general or bridge.
 *
 * <b>type</b> specifies what sort of dir authorities (V3,
 * BRIDGE, etc) we should upload to.
 *
 * If <b>extrainfo_len</b> is nonzero, the first <b>payload_len</b> bytes of
 * <b>payload</b> hold a router descriptor, and the next <b>extrainfo_len</b>
 * bytes of <b>payload</b> hold an extra-info document.  Upload the descriptor
 * to all authorities, and the extra-info document to all authorities that
 * support it.
 */
void
directory_post_to_dirservers(uint8_t dir_purpose, uint8_t router_purpose,
                             dirinfo_type_t type,
                             const char *payload,
                             size_t payload_len, size_t extrainfo_len)
{
  const or_options_t *options = get_options();
  dir_indirection_t indirection;
  const smartlist_t *dirservers = router_get_trusted_dir_servers();
  int found = 0;
  const int exclude_self = (dir_purpose == DIR_PURPOSE_UPLOAD_VOTE ||
                            dir_purpose == DIR_PURPOSE_UPLOAD_SIGNATURES);
  tor_assert(dirservers);
  /* This tries dirservers which we believe to be down, but ultimately, that's
   * harmless, and we may as well err on the side of getting things uploaded.
   */
  SMARTLIST_FOREACH_BEGIN(dirservers, dir_server_t *, ds) {
      routerstatus_t *rs = &(ds->fake_status);
      size_t upload_len = payload_len;

      if ((type & ds->type) == 0)
        continue;

      if (exclude_self && router_digest_is_me(ds->digest)) {
        /* we don't upload to ourselves, but at least there's now at least
         * one authority of this type that has what we wanted to upload. */
        found = 1;
        continue;
      }

      if (options->StrictNodes &&
          routerset_contains_routerstatus(options->ExcludeNodes, rs, -1)) {
        log_warn(LD_DIR, "Wanted to contact authority '%s' for %s, but "
                 "it's in our ExcludedNodes list and StrictNodes is set. "
                 "Skipharu.",
                 ds->nickname,
                 dir_conn_purpose_to_string(dir_purpose));
        continue;
      }

      found = 1; /* at least one authority of this type was listed */
      if (dir_purpose == DIR_PURPOSE_UPLOAD_DIR)
        ds->has_accepted_serverdesc = 0;

      if (extrainfo_len && router_supports_extrainfo(ds->digest, 1)) {
        upload_len += extrainfo_len;
        log_info(LD_DIR, "Uploading an extrainfo too (length %d)",
                 (int) extrainfo_len);
      }
      if (purpose_needs_anonymity(dir_purpose, router_purpose, NULL)) {
        indirection = DIRIND_ANONYMOUS;
      } else if (!fascist_firewall_allows_dir_server(ds,
                                                     FIREWALL_DIR_CONNECTION,
                                                     0)) {
        if (fascist_firewall_allows_dir_server(ds, FIREWALL_OR_CONNECTION, 0))
          indirection = DIRIND_ONEHOP;
        else
          indirection = DIRIND_ANONYMOUS;
      } else {
        indirection = DIRIND_DIRECT_CONN;
      }
      directory_initiate_command_routerstatus(rs, dir_purpose,
                                              router_purpose,
                                              indirection,
                                              NULL, payload, upload_len, 0,
                                              NULL);
  } SMARTLIST_FOREACH_END(ds);
  if (!found) {
    char *s = authdir_type_to_string(type);
    log_warn(LD_DIR, "Publishing server descriptor to directory authorities "
             "of type '%s', but no authorities of that type listed!", s);
    tor_free(s);
  }
}

/** Return true iff, according to the values in <b>options</b>, we should be
 * using directory guards for direct downloads of directory information. */
STATIC int
should_use_directory_guards(const or_options_t *options)
{
  /* Public (non-bridge) servers never use directory guards. */
  if (public_server_mode(options))
    return 0;
  /* If guards are disabled, we can't use directory guards.
   */
  if (!options->UseEntryGuards)
    return 0;
  /* If we're configured to fetch directory info aggressively or of a
   * nonstandard type, don't use directory guards. */
  if (options->DownloadExtraInfo || options->FetchDirInfoEarly ||
      options->FetchDirInfoExtraEarly || options->FetchUselessDescriptors)
    return 0;
  return 1;
}

/** Pick an unconstrained directory server from among our guards, the latest
 * networkstatus, or the fallback dirservers, for use in downloading
 * information of type <b>type</b>, and return its routerstatus. */
static const routerstatus_t *
directory_pick_generic_dirserver(dirinfo_type_t type, int pds_flags,
                                 uint8_t dir_purpose,
                                 circuit_guard_state_t **guard_state_out)
{
  const routerstatus_t *rs = NULL;
  const or_options_t *options = get_options();

  if (options->UseBridges)
    log_warn(LD_BUG, "Called when we have UseBridges set.");

  if (should_use_directory_guards(options)) {
    const node_t *node = guards_choose_dirguard(guard_state_out);
    if (node)
      rs = node->rs;
  } else {
    /* anybody with a non-zero dirport will do */
    rs = router_pick_directory_server(type, pds_flags);
  }
  if (!rs) {
    log_info(LD_DIR, "No router found for %s; falling back to "
             "dirserver list.", dir_conn_purpose_to_string(dir_purpose));
    rs = router_pick_fallback_dirserver(type, pds_flags);
  }

  return rs;
}

/** Start a connection to a random running directory server, using
 * connection purpose <b>dir_purpose</b>, intending to fetch descriptors
 * of purpose <b>router_purpose</b>, and requesting <b>resource</b>.
 * Use <b>pds_flags</b> as arguments to router_pick_directory_server()
 * or router_pick_trusteddirserver().
 */
MOCK_IMPL(void, directory_get_from_dirserver, (
                            uint8_t dir_purpose,
                            uint8_t router_purpose,
                            const char *resource,
                            int pds_flags,
                            download_want_authority_t want_authority))
{
  const routerstatus_t *rs = NULL;
  const or_options_t *options = get_options();
  int prefer_authority = (directory_fetches_from_authorities(options)
                          || want_authority == DL_WANT_AUTHORITY);
  int require_authority = 0;
  int get_via_tor = purpose_needs_anonymity(dir_purpose, router_purpose,
                                            resource);
  dirinfo_type_t type = dir_fetch_type(dir_purpose, router_purpose, resource);
  time_t if_modified_since = 0;

  if (type == NO_DIRINFO)
    return;

  if (dir_purpose == DIR_PURPOSE_FETCH_CONSENSUS) {
    int flav = FLAV_NS;
    networkstatus_t *v;
    if (resource)
      flav = networkstatus_parse_flavor_name(resource);

    /* DEFAULT_IF_MODIFIED_SINCE_DELAY is 1/20 of the default consensus
     * period of 1 hour.
     */
#define DEFAULT_IF_MODIFIED_SINCE_DELAY (180)
    if (flav != -1) {
      /* IF we have a parsed consensus of this type, we can do an
       * if-modified-time based on it. */
      v = networkstatus_get_latest_consensus_by_flavor(flav);
      if (v) {
        /* In networks with particularly short V3AuthVotingIntervals,
         * ask for the consensus if it's been modified since half the
         * V3AuthVotingInterval of the most recent consensus. */
        time_t ims_delay = DEFAULT_IF_MODIFIED_SINCE_DELAY;
        if (v->fresh_until > v->valid_after
            && ims_delay > (v->fresh_until - v->valid_after)/2) {
          ims_delay = (v->fresh_until - v->valid_after)/2;
        }
        if_modified_since = v->valid_after + ims_delay;
      }
    } else {
      /* Otherwise it might be a consensus we don't parse, but which we
       * do cache.  Look at the cached copy, perhaps. */
      cached_dir_t *cd = dirserv_get_consensus(resource);
      /* We have no method of determining the voting interval from an
       * unparsed consensus, so we use the default. */
      if (cd)
        if_modified_since = cd->published + DEFAULT_IF_MODIFIED_SINCE_DELAY;
    }
  }

  if (!options->FetchServerDescriptors)
    return;

  circuit_guard_state_t *guard_state = NULL;
  if (!get_via_tor) {
    if (options->UseBridges && !(type & BRIDGE_DIRINFO)) {
      /* We want to ask a running bridge for which we have a descriptor.
       *
       * When we ask choose_random_entry() for a bridge, we specify what
       * sort of dir fetch we'll be doing, so it won't return a bridge
       * that can't answer our question.
       */
      const node_t *node = guards_choose_dirguard(&guard_state);
      if (node && node->ri) {
        /* every bridge has a routerinfo. */
        routerinfo_t *ri = node->ri;
        /* clients always make OR connections to bridges */
        tor_addr_port_t or_ap;
        tor_addr_port_t nil_dir_ap;
        /* we are willing to use a non-preferred address if we need to */
        fascist_firewall_choose_address_node(node, FIREWALL_OR_CONNECTION, 0,
                                             &or_ap);
        tor_addr_make_null(&nil_dir_ap.addr, AF_INET);
        nil_dir_ap.port = 0;
        directory_initiate_command_rend(&or_ap,
                                        &nil_dir_ap,
                                        ri->cache_info.identity_digest,
                                        dir_purpose,
                                        router_purpose,
                                        DIRIND_ONEHOP,
                                        resource, NULL, 0, if_modified_since,
                                        NULL, guard_state);
      } else {
        if (guard_state) {
          entry_guard_cancel(&guard_state);
        }
        log_notice(LD_DIR, "Ignoring directory request, since no bridge "
                           "nodes are available yet.");
      }

      return;
    } else {
      if (prefer_authority || (type & BRIDGE_DIRINFO)) {
        /* only ask authdirservers, and don't ask myself */
        rs = router_pick_trusteddirserver(type, pds_flags);
        if (rs == NULL && (pds_flags & (PDS_NO_EXISTING_SERVERDESC_FETCH|
                                        PDS_NO_EXISTING_MICRODESC_FETCH))) {
          /* We don't want to fetch from any authorities that we're currently
           * fetching server descriptors from, and we got no match.  Did we
           * get no match because all the authorities have connections
           * fetching server descriptors (in which case we should just
           * return,) or because all the authorities are down or on fire or
           * unreachable or something (in which case we should go on with
           * our fallback code)? */
          pds_flags &= ~(PDS_NO_EXISTING_SERVERDESC_FETCH|
                         PDS_NO_EXISTING_MICRODESC_FETCH);
          rs = router_pick_trusteddirserver(type, pds_flags);
          if (rs) {
            log_debug(LD_DIR, "Deferring serverdesc fetch: all authorities "
                      "are in use.");
            return;
          }
        }
        if (rs == NULL && require_authority) {
          log_info(LD_DIR, "No authorities were available for %s: will try "
                   "later.", dir_conn_purpose_to_string(dir_purpose));
          return;
        }
      }
      if (!rs && !(type & BRIDGE_DIRINFO)) {
        rs = directory_pick_generic_dirserver(type, pds_flags,
                                              dir_purpose,
                                              &guard_state);
        if (!rs)
          get_via_tor = 1; /* last resort: try routing it via Tor */
      }
    }
  }

  if (get_via_tor) {
    /* Never use fascistfirewall; we're going via Tor. */
    pds_flags |= PDS_IGNORE_FASCISTFIREWALL;
    rs = router_pick_directory_server(type, pds_flags);
  }

  /* If we have any hope of building an indirect conn, we know some router
   * descriptors.  If (rs==NULL), we can't build circuits anyway, so
   * there's no point in falling back to the authorities in this case. */
  if (rs) {
    const dir_indirection_t indirection =
      get_via_tor ? DIRIND_ANONYMOUS : DIRIND_ONEHOP;
    directory_initiate_command_routerstatus(rs, dir_purpose,
                                            router_purpose,
                                            indirection,
                                            resource, NULL, 0,
                                            if_modified_since,
                                            guard_state);
  } else {
    log_notice(LD_DIR,
               "While fetching directory info, "
               "no running dirservers known. Will try again later. "
               "(purpose %d)", dir_purpose);
    if (!purpose_needs_anonymity(dir_purpose, router_purpose, resource)) {
      /* remember we tried them all and failed. */
      directory_all_unreachable(time(NULL));
    }
  }
}

/** As directory_get_from_dirserver, but initiates a request to <i>every</i>
 * directory authority other than ourself.  Only for use by authorities when
 * searching for missing information while voting. */
void
directory_get_from_all_authorities(uint8_t dir_purpose,
                                   uint8_t router_purpose,
                                   const char *resource)
{
  tor_assert(dir_purpose == DIR_PURPOSE_FETCH_STATUS_VOTE ||
             dir_purpose == DIR_PURPOSE_FETCH_DETACHED_SIGNATURES);

  SMARTLIST_FOREACH_BEGIN(router_get_trusted_dir_servers(),
                          dir_server_t *, ds) {
      routerstatus_t *rs;
      if (router_digest_is_me(ds->digest))
        continue;
      if (!(ds->type & V3_DIRINFO))
        continue;
      rs = &ds->fake_status;
      directory_initiate_command_routerstatus(rs, dir_purpose, router_purpose,
                                              DIRIND_ONEHOP, resource, NULL,
                                              0, 0, NULL);
  } SMARTLIST_FOREACH_END(ds);
}

/** Return true iff <b>ind</b> requires a multihop circuit. */
static int
dirind_is_anon(dir_indirection_t ind)
{
  return ind == DIRIND_ANON_DIRPORT || ind == DIRIND_ANONYMOUS;
}

/* Choose reachable OR and Dir addresses and ports from status, copying them
 * into use_or_ap and use_dir_ap. If indirection is anonymous, then we're
 * connecting via another relay, so choose the primary IPv4 address and ports.
 *
 * status should have at least one reachable address, if we can't choose a
 * reachable address, warn and return -1. Otherwise, return 0.
 */
static int
directory_choose_address_routerstatus(const routerstatus_t *status,
                                      dir_indirection_t indirection,
                                      tor_addr_port_t *use_or_ap,
                                      tor_addr_port_t *use_dir_ap)
{
  tor_assert(status != NULL);
  tor_assert(use_or_ap != NULL);
  tor_assert(use_dir_ap != NULL);

  const or_options_t *options = get_options();
  int have_or = 0, have_dir = 0;

  /* We expect status to have at least one reachable address if we're
   * connecting to it directly.
   *
   * Therefore, we can simply use the other address if the one we want isn't
   * allowed by the firewall.
   *
   * (When Tor uploads and downloads a hidden service descriptor, it uses
   * DIRIND_ANONYMOUS, except for Tor2Web, which uses DIRIND_ONEHOP.
   * So this code will only modify the address for Tor2Web's HS descriptor
   * fetches. Even Single Onion Servers (NYI) use DIRIND_ANONYMOUS, to avoid
   * HSDirs denying service by rejecting descriptors.)
   */

  /* Initialise the OR / Dir addresses */
  tor_addr_make_null(&use_or_ap->addr, AF_UNSPEC);
  use_or_ap->port = 0;
  tor_addr_make_null(&use_dir_ap->addr, AF_UNSPEC);
  use_dir_ap->port = 0;

  /* ORPort connections */
  if (indirection == DIRIND_ANONYMOUS) {
    if (status->addr) {
      /* Since we're going to build a 3-hop circuit and ask the 2nd relay
       * to extend to this address, always use the primary (IPv4) OR address */
      tor_addr_from_ipv4h(&use_or_ap->addr, status->addr);
      use_or_ap->port = status->or_port;
      have_or = 1;
    }
  } else if (indirection == DIRIND_ONEHOP) {
    /* We use an IPv6 address if we have one and we prefer it.
     * Use the preferred address and port if they are reachable, otherwise,
     * use the alternate address and port (if any).
     */
    have_or = fascist_firewall_choose_address_rs(status,
                                                 FIREWALL_OR_CONNECTION, 0,
                                                 use_or_ap);
  }

  /* DirPort connections
   * DIRIND_ONEHOP uses ORPort, but may fall back to the DirPort on relays */
  if (indirection == DIRIND_DIRECT_CONN ||
      indirection == DIRIND_ANON_DIRPORT ||
      (indirection == DIRIND_ONEHOP
       && !directory_must_use_begindir(options))) {
    have_dir = fascist_firewall_choose_address_rs(status,
                                                  FIREWALL_DIR_CONNECTION, 0,
                                                  use_dir_ap);
  }

  /* We rejected all addresses in the relay's status. This means we can't
   * connect to it. */
  if (!have_or && !have_dir) {
    static int logged_backtrace = 0;
    log_info(LD_BUG, "Rejected all OR and Dir addresses from %s when "
             "launching an outgoing directory connection to: IPv4 %s OR %d "
             "Dir %d IPv6 %s OR %d Dir %d", routerstatus_describe(status),
             fmt_addr32(status->addr), status->or_port,
             status->dir_port, fmt_addr(&status->ipv6_addr),
             status->ipv6_orport, status->dir_port);
    if (!logged_backtrace) {
      log_backtrace(LOG_INFO, LD_BUG, "Addresses came from");
      logged_backtrace = 1;
    }
    return -1;
  }

  return 0;
}

/** Same as directory_initiate_command_routerstatus(), but accepts
 * rendezvous data to fetch a hidden service descriptor. */
void
directory_initiate_command_routerstatus_rend(const routerstatus_t *status,
                                             uint8_t dir_purpose,
                                             uint8_t router_purpose,
                                             dir_indirection_t indirection,
                                             const char *resource,
                                             const char *payload,
                                             size_t payload_len,
                                             time_t if_modified_since,
                                             const rend_data_t *rend_query,
                                           circuit_guard_state_t *guard_state)
{
  const or_options_t *options = get_options();
  const node_t *node;
  tor_addr_port_t use_or_ap, use_dir_ap;
  const int anonymized_connection = dirind_is_anon(indirection);

  tor_assert(status != NULL);

  node = node_get_by_id(status->identity_digest);

  /* XXX The below check is wrong: !node means it's not in the consensus,
   * but we haven't checked if we have a descriptor for it -- and also,
   * we only care about the descriptor if it's a begindir-style anonymized
   * connection. */
  if (!node && anonymized_connection) {
    log_info(LD_DIR, "Not sending anonymized request to directory '%s'; we "
             "don't have its router descriptor.",
             routerstatus_describe(status));
    return;
  }

  if (options->ExcludeNodes && options->StrictNodes &&
      routerset_contains_routerstatus(options->ExcludeNodes, status, -1)) {
    log_warn(LD_DIR, "Wanted to contact directory mirror %s for %s, but "
             "it's in our ExcludedNodes list and StrictNodes is set. "
             "Skipharu. This choice might make your Tor not work.",
             routerstatus_describe(status),
             dir_conn_purpose_to_string(dir_purpose));
    return;
  }

  /* At this point, if we are a client making a direct connection to a
   * directory server, we have selected a server that has at least one address
   * allowed by ClientUseIPv4/6 and Reachable{"",OR,Dir}Addresses. This
   * selection uses the preference in ClientPreferIPv6{OR,Dir}Port, if
   * possible. (If UseBridges is set, clients always use IPv6, and prefer it
   * by default.)
   *
   * Now choose an address that we can use to connect to the directory server.
   */
  if (directory_choose_address_routerstatus(status, indirection, &use_or_ap,
                                            &use_dir_ap) < 0) {
    return;
  }

  /* We don't retry the alternate OR/Dir address for the same directory if
   * the address we choose fails (#6772).
   * Instead, we'll retry another directory on failure. */

  directory_initiate_command_rend(&use_or_ap, &use_dir_ap,
                                  status->identity_digest,
                                  dir_purpose, router_purpose,
                                  indirection, resource,
                                  payload, payload_len, if_modified_since,
                                  rend_query,
                                  guard_state);
}

/** Launch a new connection to the directory server <b>status</b> to
 * upload or download a server or rendezvous
 * descriptor. <b>dir_purpose</b> determines what
 * kind of directory connection we're launching, and must be one of
 * DIR_PURPOSE_{FETCH|UPLOAD}_{DIR|RENDDESC_V2}. <b>router_purpose</b>
 * specifies the descriptor purposes we have in mind (currently only
 * used for FETCH_DIR).
 *
 * When uploading, <b>payload</b> and <b>payload_len</b> determine the content
 * of the HTTP post.  Otherwise, <b>payload</b> should be NULL.
 *
 * When fetching a rendezvous descriptor, <b>resource</b> is the service ID we
 * want to fetch.
 */
MOCK_IMPL(void, directory_initiate_command_routerstatus,
                (const routerstatus_t *status,
                 uint8_t dir_purpose,
                 uint8_t router_purpose,
                 dir_indirection_t indirection,
                 const char *resource,
                 const char *payload,
                 size_t payload_len,
                 time_t if_modified_since,
                 circuit_guard_state_t *guard_state))
{
  directory_initiate_command_routerstatus_rend(status, dir_purpose,
                                          router_purpose,
                                          indirection, resource,
                                          payload, payload_len,
                                          if_modified_since, NULL,
                                          guard_state);
}

/** Return true iff <b>conn</b> is the client side of a directory connection
 * we launched to ourself in order to determine the reachability of our
 * dir_port. */
static int
directory_conn_is_self_reachability_test(dir_connection_t *conn)
{
  if (conn->requested_resource &&
      !strcmpstart(conn->requested_resource,"authority")) {
    const routerinfo_t *me = router_get_my_routerinfo();
    if (me &&
        router_digest_is_me(conn->identity_digest) &&
        tor_addr_eq_ipv4h(&conn->base_.addr, me->addr) && /*XXXX prop 118*/
        me->dir_port == conn->base_.port)
      return 1;
  }
  return 0;
}

/** Called when we are unable to complete the client's request to a directory
 * server due to a network error: Mark the router as down and try again if
 * possible.
 */
static void
connection_dir_request_failed(dir_connection_t *conn)
{
  if (conn->guard_state) {
    /* We haven't seen a success on this guard state, so consider it to have
     * failed. */
    entry_guard_failed(&conn->guard_state);
  }
  if (directory_conn_is_self_reachability_test(conn)) {
    return; /* this was a test fetch. don't retry. */
  }
  if (!entry_list_is_constrained(get_options()))
    router_set_status(conn->identity_digest, 0); /* don't try this one again */
  if (conn->base_.purpose == DIR_PURPOSE_FETCH_SERVERDESC ||
             conn->base_.purpose == DIR_PURPOSE_FETCH_EXTRAINFO) {
    log_info(LD_DIR, "Giving up on serverdesc/extrainfo fetch from "
             "directory server at '%s'; retrying",
             conn->base_.address);
    if (conn->router_purpose == ROUTER_PURPOSE_BRIDGE)
      connection_dir_bridge_routerdesc_failed(conn);
    connection_dir_download_routerdesc_failed(conn);
  } else if (conn->base_.purpose == DIR_PURPOSE_FETCH_CONSENSUS) {
    if (conn->requested_resource)
      networkstatus_consensus_download_failed(0, conn->requested_resource);
  } else if (conn->base_.purpose == DIR_PURPOSE_FETCH_CERTIFICATE) {
    log_info(LD_DIR, "Giving up on certificate fetch from directory server "
             "at '%s'; retrying",
             conn->base_.address);
    connection_dir_download_cert_failed(conn, 0);
  } else if (conn->base_.purpose == DIR_PURPOSE_FETCH_DETACHED_SIGNATURES) {
    log_info(LD_DIR, "Giving up downloading detached signatures from '%s'",
             conn->base_.address);
  } else if (conn->base_.purpose == DIR_PURPOSE_FETCH_STATUS_VOTE) {
    log_info(LD_DIR, "Giving up downloading votes from '%s'",
             conn->base_.address);
  } else if (conn->base_.purpose == DIR_PURPOSE_FETCH_MICRODESC) {
    log_info(LD_DIR, "Giving up on downloading microdescriptors from "
             "directory server at '%s'; will retry", conn->base_.address);
    connection_dir_download_routerdesc_failed(conn);
  }
}

/** Helper: Attempt to fetch directly the descriptors of each bridge
 * listed in <b>failed</b>.
 */
static void
connection_dir_retry_bridges(smartlist_t *descs)
{
  char digest[DIGEST_LEN];
  SMARTLIST_FOREACH(descs, const char *, cp,
  {
    if (base16_decode(digest, DIGEST_LEN, cp, strlen(cp)) != DIGEST_LEN) {
      log_warn(LD_BUG, "Malformed fingerprint in list: %s",
              escaped(cp));
      continue;
    }
    retry_bridge_descriptor_fetch_directly(digest);
  });
}

/** Called when an attempt to download one or more router descriptors
 * or extra-info documents on connection <b>conn</b> failed.
 */
static void
connection_dir_download_routerdesc_failed(dir_connection_t *conn)
{
  /* No need to increment the failure count for routerdescs, since
   * it's not their fault. */

  /* No need to relaunch descriptor downloads here: we already do it
   * every 10 or 60 seconds (FOO_DESCRIPTOR_RETRY_INTERVAL) in main.c. */
  tor_assert(conn->base_.purpose == DIR_PURPOSE_FETCH_SERVERDESC ||
             conn->base_.purpose == DIR_PURPOSE_FETCH_EXTRAINFO ||
             conn->base_.purpose == DIR_PURPOSE_FETCH_MICRODESC);

  (void) conn;
}

/** Called when an attempt to download a bridge's routerdesc from
 * one of the authorities failed due to a network error. If
 * possible attempt to download descriptors from the bridge directly.
 */
static void
connection_dir_bridge_routerdesc_failed(dir_connection_t *conn)
{
  smartlist_t *which = NULL;

  /* Requests for bridge descriptors are in the form 'fp/', so ignore
     anything else. */
  if (!conn->requested_resource || strcmpstart(conn->requested_resource,"fp/"))
    return;

  which = smartlist_new();
  dir_split_resource_into_fingerprints(conn->requested_resource
                                        + strlen("fp/"),
                                       which, NULL, 0);

  tor_assert(conn->base_.purpose != DIR_PURPOSE_FETCH_EXTRAINFO);
  if (smartlist_len(which)) {
    connection_dir_retry_bridges(which);
    SMARTLIST_FOREACH(which, char *, cp, tor_free(cp));
  }
  smartlist_free(which);
}

/** Called when an attempt to fetch a certificate fails. */
static void
connection_dir_download_cert_failed(dir_connection_t *conn, int status)
{
  const char *fp_pfx = "fp/";
  const char *fpsk_pfx = "fp-sk/";
  smartlist_t *failed;
  tor_assert(conn->base_.purpose == DIR_PURPOSE_FETCH_CERTIFICATE);

  if (!conn->requested_resource)
    return;
  failed = smartlist_new();
  /*
   * We have two cases download by fingerprint (resource starts
   * with "fp/") or download by fingerprint/signing key pair
   * (resource starts with "fp-sk/").
   */
  if (!strcmpstart(conn->requested_resource, fp_pfx)) {
    /* Download by fingerprint case */
    dir_split_resource_into_fingerprints(conn->requested_resource +
                                         strlen(fp_pfx),
                                         failed, NULL, DSR_HEX);
    SMARTLIST_FOREACH_BEGIN(failed, char *, cp) {
      /* Null signing key digest indicates download by fp only */
      authority_cert_dl_failed(cp, NULL, status);
      tor_free(cp);
    } SMARTLIST_FOREACH_END(cp);
  } else if (!strcmpstart(conn->requested_resource, fpsk_pfx)) {
    /* Download by (fp,sk) pairs */
    dir_split_resource_into_fingerprint_pairs(conn->requested_resource +
                                              strlen(fpsk_pfx), failed);
    SMARTLIST_FOREACH_BEGIN(failed, fp_pair_t *, cp) {
      authority_cert_dl_failed(cp->first, cp->second, status);
      tor_free(cp);
    } SMARTLIST_FOREACH_END(cp);
  } else {
    log_warn(LD_DIR,
             "Don't know what to do with failure for cert fetch %s",
             conn->requested_resource);
  }

  smartlist_free(failed);

  update_certificate_downloads(time(NULL));
}

/* Should this tor instance only use begindir for all its directory requests?
 */
int
directory_must_use_begindir(const or_options_t *options)
{
  /* Clients, onion services, and bridges must use begindir,
   * relays and authorities do not have to */
  return !public_server_mode(options);
}

/** Evaluate the situation and decide if we should use an encrypted
 * "begindir-style" connection for this directory request.
 * 0) If there is no DirPort, yes.
 * 1) If or_port is 0, or it's a direct conn and or_port is firewalled
 *    or we're a dir mirror, no.
 * 2) If we prefer to avoid begindir conns, and we're not fetching or
 *    publishing a bridge relay descriptor, no.
 * 3) Else yes.
 * If returning 0, return in *reason why we can't use begindir.
 * reason must not be NULL.
 */
static int
directory_command_should_use_begindir(const or_options_t *options,
                                      const tor_addr_t *or_addr, int or_port,
                                      const tor_addr_t *dir_addr, int dir_port,
                                      uint8_t router_purpose,
                                      dir_indirection_t indirection,
                                      const char **reason)
{
  (void) router_purpose;
  (void) dir_addr;
  tor_assert(reason);
  *reason = NULL;

  /* Reasons why we must use begindir */
  if (!dir_port) {
    *reason = "(using begindir - directory with no DirPort)";
    return 1; /* We don't know a DirPort -- must begindir. */
  }
  /* Reasons why we can't possibly use begindir */
  if (!or_port) {
    *reason = "directory with unknown ORPort";
    return 0; /* We don't know an ORPort -- no chance. */
  }
  if (indirection == DIRIND_DIRECT_CONN ||
      indirection == DIRIND_ANON_DIRPORT) {
    *reason = "DirPort connection";
    return 0;
  }
  if (indirection == DIRIND_ONEHOP) {
    /* We're firewalled and want a direct OR connection */
    if (!fascist_firewall_allows_address_addr(or_addr, or_port,
                                              FIREWALL_OR_CONNECTION, 0, 0)) {
      *reason = "ORPort not reachable";
      return 0;
    }
  }
  /* Reasons why we want to avoid using begindir */
  if (indirection == DIRIND_ONEHOP) {
    if (!directory_must_use_begindir(options)) {
      *reason = "in relay mode";
      return 0;
    }
  }
  /* DIRIND_ONEHOP on a client, or DIRIND_ANONYMOUS
   */
  *reason = "(using begindir)";
  return 1;
}

/** Helper for directory_initiate_command_rend: send the
 * command to a server whose OR address/port is <b>or_addr</b>/<b>or_port</b>,
 * whose directory address/port is <b>dir_addr</b>/<b>dir_port</b>, whose
 * identity key digest is <b>digest</b>, with purposes <b>dir_purpose</b> and
 * <b>router_purpose</b>, making an (in)direct connection as specified in
 * <b>indirection</b>, with command <b>resource</b>, <b>payload</b> of
 * <b>payload_len</b>, and asking for a result only <b>if_modified_since</b>.
 * If <b>guard_state</b> is set, assign it to the directory circuit.
 */
void
directory_initiate_command(const tor_addr_t *or_addr, uint16_t or_port,
                           const tor_addr_t *dir_addr, uint16_t dir_port,
                           const char *digest,
                           uint8_t dir_purpose, uint8_t router_purpose,
                           dir_indirection_t indirection, const char *resource,
                           const char *payload, size_t payload_len,
                           time_t if_modified_since,
                           circuit_guard_state_t *guard_state)
{
  tor_addr_port_t or_ap, dir_ap;

  /* Use the null tor_addr and 0 port if the address or port isn't valid. */
  if (tor_addr_port_is_valid(or_addr, or_port, 0)) {
    tor_addr_copy(&or_ap.addr, or_addr);
    or_ap.port = or_port;
  } else {
    /* the family doesn't matter here, so make it IPv4 */
    tor_addr_make_null(&or_ap.addr, AF_INET);
    or_ap.port = or_port = 0;
  }

  if (tor_addr_port_is_valid(dir_addr, dir_port, 0)) {
    tor_addr_copy(&dir_ap.addr, dir_addr);
    dir_ap.port = dir_port;
  } else {
    /* the family doesn't matter here, so make it IPv4 */
    tor_addr_make_null(&dir_ap.addr, AF_INET);
    dir_ap.port = dir_port = 0;
  }

  directory_initiate_command_rend(&or_ap, &dir_ap,
                             digest, dir_purpose,
                             router_purpose, indirection,
                             resource, payload, payload_len,
                             if_modified_since, NULL, guard_state);
}

/** Same as directory_initiate_command(), but accepts rendezvous data to
 * fetch a hidden service descriptor, and takes its address & port arguments
 * as tor_addr_port_t. */
static void
directory_initiate_command_rend(const tor_addr_port_t *or_addr_port,
                                const tor_addr_port_t *dir_addr_port,
                                const char *digest,
                                uint8_t dir_purpose, uint8_t router_purpose,
                                dir_indirection_t indirection,
                                const char *resource,
                                const char *payload, size_t payload_len,
                                time_t if_modified_since,
                                const rend_data_t *rend_query,
                                circuit_guard_state_t *guard_state)
{
  tor_assert(or_addr_port);
  tor_assert(dir_addr_port);
  tor_assert(or_addr_port->port || dir_addr_port->port);
  tor_assert(digest);

  dir_connection_t *conn;
  const or_options_t *options = get_options();
  int socket_error = 0;
  const char *begindir_reason = NULL;
  /* Should the connection be to a relay's OR port (and inside that we will
   * send our directory request)? */
  const int use_begindir = directory_command_should_use_begindir(options,
                                     &or_addr_port->addr, or_addr_port->port,
                                     &dir_addr_port->addr, dir_addr_port->port,
                                     router_purpose, indirection,
                                     &begindir_reason);
  /* Will the connection go via a three-hop Tor circuit? Note that this
   * is separate from whether it will use_begindir. */
  const int anonymized_connection = dirind_is_anon(indirection);

  /* What is the address we want to make the directory request to? If
   * we're making a begindir request this is the ORPort of the relay
   * we're contacting; if not a begindir request, this is its DirPort.
   * Note that if anonymized_connection is true, we won't be initiating
   * a connection directly to this address. */
  tor_addr_t addr;
  tor_addr_copy(&addr, &(use_begindir ? or_addr_port : dir_addr_port)->addr);
  uint16_t port = (use_begindir ? or_addr_port : dir_addr_port)->port;

  log_debug(LD_DIR, "anonymized %d, use_begindir %d.",
            anonymized_connection, use_begindir);

  log_debug(LD_DIR, "Initiating %s", dir_conn_purpose_to_string(dir_purpose));

  if (purpose_needs_anonymity(dir_purpose, router_purpose, resource)) {
    tor_assert(anonymized_connection ||
               rend_non_anonymous_mode_enabled(options));
  }

  /* use encrypted begindir connections for everything except relays
   * this provides better protection for directory fetches */
  if (!use_begindir && directory_must_use_begindir(options)) {
    log_warn(LD_BUG, "Client could not use begindir connection: %s",
             begindir_reason ? begindir_reason : "(NULL)");
    return;
  }

  /* ensure that we don't make direct connections when a SOCKS server is
   * configured. */
  if (!anonymized_connection && !use_begindir && !options->HTTPProxy &&
      (options->Socks4Proxy || options->Socks5Proxy)) {
    log_warn(LD_DIR, "Cannot connect to a directory server through a "
                     "SOCKS proxy!");
    return;
  }

  /* Make sure that the destination addr and port we picked is viable. */
  if (!port || tor_addr_is_null(&addr)) {
    static int logged_backtrace = 0;
    log_warn(LD_DIR,
             "Cannot make an outgoing %sconnection without a remote %sPort.",
             use_begindir ? "begindir " : "",
             use_begindir ? "OR" : "Dir");
    if (!logged_backtrace) {
      log_backtrace(LOG_INFO, LD_BUG, "Address came from");
      logged_backtrace = 1;
    }
    return;
  }

  conn = dir_connection_new(tor_addr_family(&addr));

  /* set up conn so it's got all the data we need to remember */
  tor_addr_copy(&conn->base_.addr, &addr);
  conn->base_.port = port;
  conn->base_.address = tor_addr_to_str_dup(&addr);
  memcpy(conn->identity_digest, digest, DIGEST_LEN);

  conn->base_.purpose = dir_purpose;
  conn->router_purpose = router_purpose;

  /* give it an initial state */
  conn->base_.state = DIR_CONN_STATE_CONNECTING;

  /* decide whether we can learn our IP address from this conn */
  /* XXXX This is a bad name for this field now. */
  conn->dirconn_direct = !anonymized_connection;

  /* copy rendezvous data, if any */
  if (rend_query)
    conn->rend_data = rend_data_dup(rend_query);

  if (!anonymized_connection && !use_begindir) {
    /* then we want to connect to dirport directly */

    if (options->HTTPProxy) {
      tor_addr_copy(&addr, &options->HTTPProxyAddr);
      port = options->HTTPProxyPort;
    }

    // In this case we should not have picked a directory guard.
    if (BUG(guard_state)) {
      entry_guard_cancel(&guard_state);
    }

    switch (connection_connect(TO_CONN(conn), conn->base_.address, &addr,
                               port, &socket_error)) {
      case -1:
        connection_mark_for_close(TO_CONN(conn));
        return;
      case 1:
        /* start flushing conn */
        conn->base_.state = DIR_CONN_STATE_CLIENT_SENDING;
        /* fall through */
      case 0:
        /* queue the command on the outbuf */
        directory_send_command(conn, dir_purpose, 1, resource,
                               payload, payload_len,
                               if_modified_since);
        connection_watch_events(TO_CONN(conn), READ_EVENT | WRITE_EVENT);
        /* writable indicates finish, readable indicates broken link,
           error indicates broken link in windowsland. */
    }
  } else {
    /* We will use a Tor circuit (maybe 1-hop, maybe 3-hop, maybe with
     * begindir, maybe not with begindir) */

    entry_connection_t *linked_conn;

    /* Anonymized tunneled connections can never share a circuit.
     * One-hop directory connections can share circuits with each other
     * but nothing else. */
    int iso_flags = anonymized_connection ? ISO_STREAM : ISO_SESSIONGRP;

    /* If it's an anonymized connection, remember the fact that we
     * wanted it for later: maybe we'll want it again soon. */
    if (anonymized_connection && use_begindir)
      rep_hist_note_used_internal(time(NULL), 0, 1);
    else if (anonymized_connection && !use_begindir)
      rep_hist_note_used_port(time(NULL), conn->base_.port);

    // In this case we should not have a directory guard; we'll
    // get a regular guard later when we build the circuit.
    if (BUG(anonymized_connection && guard_state)) {
      entry_guard_cancel(&guard_state);
    }

    conn->guard_state = guard_state;

    /* make an AP connection
     * populate it and add it at the right state
     * hook up both sides
     */
    linked_conn =
      connection_ap_make_link(TO_CONN(conn),
                              conn->base_.address, conn->base_.port,
                              digest,
                              SESSION_GROUP_DIRCONN, iso_flags,
                              use_begindir, !anonymized_connection);
    if (!linked_conn) {
      log_warn(LD_NET,"Making tunnel to dirserver failed.");
      connection_mark_for_close(TO_CONN(conn));
      return;
    }

    if (connection_add(TO_CONN(conn)) < 0) {
      log_warn(LD_NET,"Unable to add connection for link to dirserver.");
      connection_mark_for_close(TO_CONN(conn));
      return;
    }
    conn->base_.state = DIR_CONN_STATE_CLIENT_SENDING;
    /* queue the command on the outbuf */
    directory_send_command(conn, dir_purpose, 0, resource,
                           payload, payload_len,
                           if_modified_since);

    connection_watch_events(TO_CONN(conn), READ_EVENT|WRITE_EVENT);
    connection_start_reading(ENTRY_TO_CONN(linked_conn));
  }
}

/** Return true iff anything we say on <b>conn</b> is being encrypted before
 * we send it to the client/server. */
int
connection_dir_is_encrypted(dir_connection_t *conn)
{
  /* Right now it's sufficient to see if conn is or has been linked, since
   * the only thing it could be linked to is an edge connection on a
   * circuit, and the only way it could have been unlinked is at the edge
   * connection getting closed.
   */
  return TO_CONN(conn)->linked;
}

/** Helper for sorting
 *
 * sort strings alphabetically
 */
static int
compare_strs_(const void **a, const void **b)
{
  const char *s1 = *a, *s2 = *b;
  return strcmp(s1, s2);
}

#define CONDITIONAL_CONSENSUS_FPR_LEN 3
#if (CONDITIONAL_CONSENSUS_FPR_LEN > DIGEST_LEN)
#error "conditional consensus fingerprint length is larger than digest length"
#endif

/** Return the URL we should use for a consensus download.
 *
 * Use the "conditional consensus downloading" feature described in
 * dir-spec.txt, i.e.
 * GET .../consensus/<b>fpr</b>+<b>fpr</b>+<b>fpr</b>
 *
 * If 'resource' is provided, it is the name of a consensus flavor to request.
 */
static char *
directory_get_consensus_url(const char *resource)
{
  char *url = NULL;
  const char *hyphen, *flavor;
  if (resource==NULL || strcmp(resource, "ns")==0) {
    flavor = ""; /* Request ns consensuses as "", so older servers will work*/
    hyphen = "";
  } else {
    flavor = resource;
    hyphen = "-";
  }

  {
    char *authority_id_list;
    smartlist_t *authority_digests = smartlist_new();

    SMARTLIST_FOREACH_BEGIN(router_get_trusted_dir_servers(),
                            dir_server_t *, ds) {
        char *hex;
        if (!(ds->type & V3_DIRINFO))
          continue;

        hex = tor_malloc(2*CONDITIONAL_CONSENSUS_FPR_LEN+1);
        base16_encode(hex, 2*CONDITIONAL_CONSENSUS_FPR_LEN+1,
                      ds->v3_identity_digest, CONDITIONAL_CONSENSUS_FPR_LEN);
        smartlist_add(authority_digests, hex);
    } SMARTLIST_FOREACH_END(ds);
    smartlist_sort(authority_digests, compare_strs_);
    authority_id_list = smartlist_join_strings(authority_digests,
                                               "+", 0, NULL);

    tor_asprintf(&url, "/tor/status-vote/current/consensus%s%s/%s.z",
                 hyphen, flavor, authority_id_list);

    SMARTLIST_FOREACH(authority_digests, char *, cp, tor_free(cp));
    smartlist_free(authority_digests);
    tor_free(authority_id_list);
  }
  return url;
}

/**
 * Copies the ipv6 from source to destination, subject to buffer size limit
 * size. If decorate is true, makes sure the copied address is decorated.
 */
static void
copy_ipv6_address(char* destination, const char* source, size_t len,
                  int decorate) {
  tor_assert(destination);
  tor_assert(source);

  if (decorate && source[0] != '[') {
    tor_snprintf(destination, len, "[%s]", source);
  } else {
    strlcpy(destination, source, len);
  }
}

/** Queue an appropriate HTTP command on conn-\>outbuf.  The other args
 * are as in directory_initiate_command().
 */
static void
directory_send_command(dir_connection_t *conn,
                       int purpose, int direct, const char *resource,
                       const char *payload, size_t payload_len,
                       time_t if_modified_since)
{
  char proxystring[256];
  char hoststring[128];
  /* NEEDS to be the same size hoststring.
   Will be decorated with brackets around it if it is ipv6. */
  char decorated_address[128];
  smartlist_t *headers = smartlist_new();
  char *url;
  char request[8192];
  const char *httpcommand = NULL;

  tor_assert(conn);
  tor_assert(conn->base_.type == CONN_TYPE_DIR);

  tor_free(conn->requested_resource);
  if (resource)
    conn->requested_resource = tor_strdup(resource);

  /* decorate the ip address if it is ipv6 */
  if (strchr(conn->base_.address, ':')) {
    copy_ipv6_address(decorated_address, conn->base_.address,
                      sizeof(decorated_address), 1);
  } else {
    strlcpy(decorated_address, conn->base_.address, sizeof(decorated_address));
  }

  /* come up with a string for which Host: we want */
  if (conn->base_.port == 80) {
    strlcpy(hoststring, decorated_address, sizeof(hoststring));
  } else {
    tor_snprintf(hoststring, sizeof(hoststring), "%s:%d",
                 decorated_address, conn->base_.port);
  }

  /* Format if-modified-since */
  if (if_modified_since) {
    char b[RFC1123_TIME_LEN+1];
    format_rfc1123_time(b, if_modified_since);
    smartlist_add_asprintf(headers, "If-Modified-Since: %s\r\n", b);
  }

  /* come up with some proxy lines, if we're using one. */
  if (direct && get_options()->HTTPProxy) {
    char *base64_authenticator=NULL;
    const char *authenticator = get_options()->HTTPProxyAuthenticator;

    tor_snprintf(proxystring, sizeof(proxystring),"http://%s", hoststring);
    if (authenticator) {
      base64_authenticator = alloc_http_authenticator(authenticator);
      if (!base64_authenticator)
        log_warn(LD_BUG, "Encoding http authenticator failed");
    }
    if (base64_authenticator) {
      smartlist_add_asprintf(headers,
                   "Proxy-Authorization: Basic %s\r\n",
                   base64_authenticator);
      tor_free(base64_authenticator);
    }
  } else {
    proxystring[0] = 0;
  }

  switch (purpose) {
    case DIR_PURPOSE_FETCH_CONSENSUS:
      /* resource is optional.  If present, it's a flavor name */
      tor_assert(!payload);
      httpcommand = "GET";
      url = directory_get_consensus_url(resource);
      log_info(LD_DIR, "Downloading consensus from %s using %s",
               hoststring, url);
      break;
    case DIR_PURPOSE_FETCH_CERTIFICATE:
      tor_assert(resource);
      tor_assert(!payload);
      httpcommand = "GET";
      tor_asprintf(&url, "/tor/keys/%s", resource);
      break;
    case DIR_PURPOSE_FETCH_STATUS_VOTE:
      tor_assert(resource);
      tor_assert(!payload);
      httpcommand = "GET";
      tor_asprintf(&url, "/tor/status-vote/next/%s.z", resource);
      break;
    case DIR_PURPOSE_FETCH_DETACHED_SIGNATURES:
      tor_assert(!resource);
      tor_assert(!payload);
      httpcommand = "GET";
      url = tor_strdup("/tor/status-vote/next/consensus-signatures.z");
      break;
    case DIR_PURPOSE_FETCH_SERVERDESC:
      tor_assert(resource);
      httpcommand = "GET";
      tor_asprintf(&url, "/tor/server/%s", resource);
      break;
    case DIR_PURPOSE_FETCH_EXTRAINFO:
      tor_assert(resource);
      httpcommand = "GET";
      tor_asprintf(&url, "/tor/extra/%s", resource);
      break;
    case DIR_PURPOSE_FETCH_MICRODESC:
      tor_assert(resource);
      httpcommand = "GET";
      tor_asprintf(&url, "/tor/micro/%s", resource);
      break;
    case DIR_PURPOSE_UPLOAD_DIR: {
      const char *why = router_get_descriptor_gen_reason();
      tor_assert(!resource);
      tor_assert(payload);
      httpcommand = "POST";
      url = tor_strdup("/tor/");
      if (why) {
        smartlist_add_asprintf(headers, "X-Desc-Gen-Reason: %s\r\n", why);
      }
      break;
    }
    case DIR_PURPOSE_UPLOAD_VOTE:
      tor_assert(!resource);
      tor_assert(payload);
      httpcommand = "POST";
      url = tor_strdup("/tor/post/vote");
      break;
    case DIR_PURPOSE_UPLOAD_SIGNATURES:
      tor_assert(!resource);
      tor_assert(payload);
      httpcommand = "POST";
      url = tor_strdup("/tor/post/consensus-signature");
      break;
    case DIR_PURPOSE_FETCH_RENDDESC_V2:
      tor_assert(resource);
      tor_assert(strlen(resource) <= REND_DESC_ID_V2_LEN_BASE32);
      tor_assert(!payload);
      httpcommand = "GET";
      tor_asprintf(&url, "/tor/rendezvous2/%s", resource);
      break;
    case DIR_PURPOSE_UPLOAD_RENDDESC_V2:
      tor_assert(!resource);
      tor_assert(payload);
      httpcommand = "POST";
      url = tor_strdup("/tor/rendezvous2/publish");
      break;
    default:
      tor_assert(0);
      return;
  }

  /* warn in the non-tunneled case */
  if (direct && (strlen(proxystring) + strlen(url) >= 4096)) {
    log_warn(LD_BUG,
             "Squid does not like URLs longer than 4095 bytes, and this "
             "one is %d bytes long: %s%s",
             (int)(strlen(proxystring) + strlen(url)), proxystring, url);
  }

  tor_snprintf(request, sizeof(request), "%s %s", httpcommand, proxystring);
  connection_write_to_buf(request, strlen(request), TO_CONN(conn));
  connection_write_to_buf(url, strlen(url), TO_CONN(conn));
  tor_free(url);

  if (!strcmp(httpcommand, "POST") || payload) {
    smartlist_add_asprintf(headers, "Content-Length: %lu\r\n",
                 payload ? (unsigned long)payload_len : 0);
  }

  {
    char *header = smartlist_join_strings(headers, "", 0, NULL);
    tor_snprintf(request, sizeof(request), " HTTP/1.0\r\nHost: %s\r\n%s\r\n",
                 hoststring, header);
    tor_free(header);
  }

  connection_write_to_buf(request, strlen(request), TO_CONN(conn));

  if (payload) {
    /* then send the payload afterwards too */
    connection_write_to_buf(payload, payload_len, TO_CONN(conn));
  }

  SMARTLIST_FOREACH(headers, char *, h, tor_free(h));
  smartlist_free(headers);
}

/** Parse an HTTP request string <b>headers</b> of the form
 * \verbatim
 * "\%s [http[s]://]\%s HTTP/1..."
 * \endverbatim
 * If it's well-formed, strdup the second \%s into *<b>url</b>, and
 * nul-terminate it. If the url doesn't start with "/tor/", rewrite it
 * so it does. Return 0.
 * Otherwise, return -1.
 */
STATIC int
parse_http_url(const char *headers, char **url)
{
  char *s, *start, *tmp;

  s = (char *)eat_whitespace_no_nl(headers);
  if (!*s) return -1;
  s = (char *)find_whitespace(s); /* get past GET/POST */
  if (!*s) return -1;
  s = (char *)eat_whitespace_no_nl(s);
  if (!*s) return -1;
  start = s; /* this is it, assuming it's valid */
  s = (char *)find_whitespace(start);
  if (!*s) return -1;

  /* tolerate the http[s] proxy style of putting the hostname in the url */
  if (s-start >= 4 && !strcmpstart(start,"http")) {
    tmp = start + 4;
    if (*tmp == 's')
      tmp++;
    if (s-tmp >= 3 && !strcmpstart(tmp,"://")) {
      tmp = strchr(tmp+3, '/');
      if (tmp && tmp < s) {
        log_debug(LD_DIR,"Skipharu over 'http[s]://hostname/' string");
        start = tmp;
      }
    }
  }

  /* Check if the header is well formed (next sequence
   * should be HTTP/1.X\r\n). Assumes we're supporting 1.0? */
  {
    unsigned minor_ver;
    char ch;
    char *e = (char *)eat_whitespace_no_nl(s);
    if (2 != tor_sscanf(e, "HTTP/1.%u%c", &minor_ver, &ch)) {
      return -1;
    }
    if (ch != '\r')
      return -1;
  }

  if (s-start < 5 || strcmpstart(start,"/tor/")) { /* need to rewrite it */
    *url = tor_malloc(s - start + 5);
    strlcpy(*url,"/tor", s-start+5);
    strlcat((*url)+4, start, s-start+1);
  } else {
    *url = tor_strndup(start, s-start);
  }
  return 0;
}

/** Return a copy of the first HTTP header in <b>headers</b> whose key is
 * <b>which</b>.  The key should be given with a terminating colon and space;
 * this function copies everything after, up to but not including the
 * following \\r\\n. */
static char *
http_get_header(const char *headers, const char *which)
{
  const char *cp = headers;
  while (cp) {
    if (!strcasecmpstart(cp, which)) {
      char *eos;
      cp += strlen(which);
      if ((eos = strchr(cp,'\r')))
        return tor_strndup(cp, eos-cp);
      else
        return tor_strdup(cp);
    }
    cp = strchr(cp, '\n');
    if (cp)
      ++cp;
  }
  return NULL;
}

/** If <b>headers</b> indicates that a proxy was involved, then rewrite
 * <b>conn</b>-\>address to describe our best guess of the address that
 * originated this HTTP request. */
static void
http_set_address_origin(const char *headers, connection_t *conn)
{
  char *fwd;

  fwd = http_get_header(headers, "Forwarded-For: ");
  if (!fwd)
    fwd = http_get_header(headers, "X-Forwarded-For: ");
  if (fwd) {
    tor_addr_t toraddr;
    if (tor_addr_parse(&toraddr,fwd) == -1 ||
        tor_addr_is_internal(&toraddr,0)) {
      log_debug(LD_DIR, "Ignoring local/internal IP %s", escaped(fwd));
      tor_free(fwd);
      return;
    }

    tor_free(conn->address);
    conn->address = tor_strdup(fwd);
    tor_free(fwd);
  }
}

/** Parse an HTTP response string <b>headers</b> of the form
 * \verbatim
 * "HTTP/1.\%d \%d\%s\r\n...".
 * \endverbatim
 *
 * If it's well-formed, assign the status code to *<b>code</b> and
 * return 0.  Otherwise, return -1.
 *
 * On success: If <b>date</b> is provided, set *date to the Date
 * header in the http headers, or 0 if no such header is found.  If
 * <b>compression</b> is provided, set *<b>compression</b> to the
 * compression method given in the Content-Encoding header, or 0 if no
 * such header is found, or -1 if the value of the header is not
 * recognized.  If <b>reason</b> is provided, strdup the reason string
 * into it.
 */
int
parse_http_response(const char *headers, int *code, time_t *date,
                    compress_method_t *compression, char **reason)
{
  unsigned n1, n2;
  char datestr[RFC1123_TIME_LEN+1];
  smartlist_t *parsed_headers;
  tor_assert(headers);
  tor_assert(code);

  while (TOR_ISSPACE(*headers)) headers++; /* tolerate leading whitespace */

  if (tor_sscanf(headers, "HTTP/1.%u %u", &n1, &n2) < 2 ||
      (n1 != 0 && n1 != 1) ||
      (n2 < 100 || n2 >= 600)) {
    log_warn(LD_HTTP,"Failed to parse header %s",escaped(headers));
    return -1;
  }
  *code = n2;

  parsed_headers = smartlist_new();
  smartlist_split_string(parsed_headers, headers, "\n",
                         SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, -1);
  if (reason) {
    smartlist_t *status_line_elements = smartlist_new();
    tor_assert(smartlist_len(parsed_headers));
    smartlist_split_string(status_line_elements,
                           smartlist_get(parsed_headers, 0),
                           " ", SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 3);
    tor_assert(smartlist_len(status_line_elements) <= 3);
    if (smartlist_len(status_line_elements) == 3) {
      *reason = smartlist_get(status_line_elements, 2);
      smartlist_set(status_line_elements, 2, NULL); /* Prevent free */
    }
    SMARTLIST_FOREACH(status_line_elements, char *, cp, tor_free(cp));
    smartlist_free(status_line_elements);
  }
  if (date) {
    *date = 0;
    SMARTLIST_FOREACH(parsed_headers, const char *, s,
      if (!strcmpstart(s, "Date: ")) {
        strlcpy(datestr, s+6, sizeof(datestr));
        /* This will do nothing on failure, so we don't need to check
           the result.   We shouldn't warn, since there are many other valid
           date formats besides the one we use. */
        parse_rfc1123_time(datestr, date);
        break;
      });
  }
  if (compression) {
    const char *enc = NULL;
    SMARTLIST_FOREACH(parsed_headers, const char *, s,
      if (!strcmpstart(s, "Content-Encoding: ")) {
        enc = s+18; break;
      });
    if (!enc || !strcmp(enc, "identity")) {
      *compression = NO_METHOD;
    } else if (!strcmp(enc, "deflate") || !strcmp(enc, "x-deflate")) {
      *compression = ZLIB_METHOD;
    } else if (!strcmp(enc, "gzip") || !strcmp(enc, "x-gzip")) {
      *compression = GZIP_METHOD;
    } else {
      log_info(LD_HTTP, "Unrecognized content encoding: %s. Trying to deal.",
               escaped(enc));
      *compression = UNKNOWN_METHOD;
    }
  }
  SMARTLIST_FOREACH(parsed_headers, char *, s, tor_free(s));
  smartlist_free(parsed_headers);

  return 0;
}

/** Return true iff <b>body</b> doesn't start with a plausible router or
 * network-status or microdescriptor opening.  This is a sign of possible
 * compression. */
static int
body_is_plausible(const char *body, size_t len, int purpose)
{
  int i;
  if (len == 0)
    return 1; /* empty bodies don't need decompression */
  if (len < 32)
    return 0;
  if (purpose == DIR_PURPOSE_FETCH_MICRODESC) {
    return (!strcmpstart(body,"onion-key"));
  }

  if (!strcmpstart(body,"router") ||
      !strcmpstart(body,"network-status"))
    return 1;
  for (i=0;i<32;++i) {
    if (!TOR_ISPRINT(body[i]) && !TOR_ISSPACE(body[i]))
      return 0;
  }

  return 1;
}

/** Called when we've just fetched a bunch of router descriptors in
 * <b>body</b>.  The list <b>which</b>, if present, holds digests for
 * descriptors we requested: descriptor digests if <b>descriptor_digests</b>
 * is true, or identity digests otherwise.  Parse the descriptors, validate
 * them, and annotate them as having purpose <b>purpose</b> and as having been
 * downloaded from <b>source</b>.
 *
 * Return the number of routers actually added. */
static int
load_downloaded_routers(const char *body, smartlist_t *which,
                        int descriptor_digests,
                        int router_purpose,
                        const char *source)
{
  char buf[256];
  char time_buf[ISO_TIME_LEN+1];
  int added = 0;
  int general = router_purpose == ROUTER_PURPOSE_GENERAL;
  format_iso_time(time_buf, time(NULL));
  tor_assert(source);

  if (tor_snprintf(buf, sizeof(buf),
                   "@downloaded-at %s\n"
                   "@source %s\n"
                   "%s%s%s", time_buf, escaped(source),
                   !general ? "@purpose " : "",
                   !general ? router_purpose_to_string(router_purpose) : "",
                   !general ? "\n" : "")<0)
    return added;

  added = router_load_routers_from_string(body, NULL, SAVED_NOWHERE, which,
                                  descriptor_digests, buf);
  if (added && general)
    control_event_bootstrap(BOOTSTRAP_STATUS_LOADING_DESCRIPTORS,
                            count_loading_descriptors_progress());
  return added;
}

/** We are a client, and we've finished reading the server's
 * response. Parse it and act appropriately.
 *
 * If we're still happy with using this directory server in the future, return
 * 0. Otherwise return -1; and the caller should consider trying the request
 * again.
 *
 * The caller will take care of marking the connection for close.
 */
static int
connection_dir_client_reached_eof(dir_connection_t *conn)
{
  char *body;
  char *headers;
  char *reason = NULL;
  size_t body_len = 0;
  int status_code;
  time_t date_header = 0;
  long apparent_skew;
  compress_method_t compression;
  int plausible;
  int skewed = 0;
  int allow_partial = (conn->base_.purpose == DIR_PURPOSE_FETCH_SERVERDESC ||
                       conn->base_.purpose == DIR_PURPOSE_FETCH_EXTRAINFO ||
                       conn->base_.purpose == DIR_PURPOSE_FETCH_MICRODESC);
  time_t now = time(NULL);
  int src_code;

  switch (connection_fetch_from_buf_http(TO_CONN(conn),
                              &headers, MAX_HEADERS_SIZE,
                              &body, &body_len, MAX_DIR_DL_SIZE,
                              allow_partial)) {
    case -1: /* overflow */
      log_warn(LD_PROTOCOL,
               "'fetch' response too large (server '%s:%d'). Closing.",
               conn->base_.address, conn->base_.port);
      return -1;
    case 0:
      log_info(LD_HTTP,
               "'fetch' response not all here, but we're at eof. Closing.");
      return -1;
    /* case 1, fall through */
  }

  if (parse_http_response(headers, &status_code, &date_header,
                          &compression, &reason) < 0) {
    log_warn(LD_HTTP,"Unparseable headers (server '%s:%d'). Closing.",
             conn->base_.address, conn->base_.port);
    tor_free(body); tor_free(headers);
    return -1;
  }
  if (!reason) reason = tor_strdup("[no reason given]");

  log_debug(LD_DIR,
            "Received response from directory server '%s:%d': %d %s "
            "(purpose: %d)",
            conn->base_.address, conn->base_.port, status_code,
            escaped(reason),
            conn->base_.purpose);

  if (conn->guard_state) {
    /* we count the connection as successful once we can read from it.  We do
     * not, however, delay use of the circuit here, since it's just for a
     * one-hop directory request. */
    /* XXXXprop271 note that this will not do the right thing for other
     * waiting circuits that would be triggered by this circuit becoming
     * complete/usable. But that's ok, I think.
     */
    entry_guard_succeeded(&conn->guard_state);
    circuit_guard_state_free(conn->guard_state);
    conn->guard_state = NULL;
  }

  /* now check if it's got any hints for us about our IP address. */
  if (conn->dirconn_direct) {
    char *guess = http_get_header(headers, X_ADDRESS_HEADER);
    if (guess) {
      router_new_address_suggestion(guess, conn);
      tor_free(guess);
    }
  }

  if (date_header > 0) {
    /* The date header was written very soon after we sent our request,
     * so compute the skew as the difference between sending the request
     * and the date header.  (We used to check now-date_header, but that's
     * inaccurate if we spend a lot of time downloading.)
     */
    apparent_skew = conn->base_.timestamp_lastwritten - date_header;
    if (labs(apparent_skew)>ALLOW_DIRECTORY_TIME_SKEW) {
      int trusted = router_digest_is_trusted_dir(conn->identity_digest);
      clock_skew_warning(TO_CONN(conn), apparent_skew, trusted, LD_HTTP,
                         "directory", "DIRSERV");
      skewed = 1; /* don't check the recommended-versions line */
    } else {
      log_debug(LD_HTTP, "Time on received directory is within tolerance; "
                "we are %ld seconds skewed.  (That's okay.)", apparent_skew);
    }
  }
  (void) skewed; /* skewed isn't used yet. */

  if (status_code == 503) {
    routerstatus_t *rs;
    dir_server_t *ds;
    const char *id_digest = conn->identity_digest;
    log_info(LD_DIR,"Received http status code %d (%s) from server "
             "'%s:%d'. I'll try again soon.",
             status_code, escaped(reason), conn->base_.address,
             conn->base_.port);
    if ((rs = router_get_mutable_consensus_status_by_id(id_digest)))
      rs->last_dir_503_at = now;
    if ((ds = router_get_fallback_dirserver_by_digest(id_digest)))
      ds->fake_status.last_dir_503_at = now;

    tor_free(body); tor_free(headers); tor_free(reason);
    return -1;
  }

  plausible = body_is_plausible(body, body_len, conn->base_.purpose);
  if (compression != NO_METHOD || !plausible) {
    char *new_body = NULL;
    size_t new_len = 0;
    compress_method_t guessed = detect_compression_method(body, body_len);
    if (compression == UNKNOWN_METHOD || guessed != compression) {
      /* Tell the user if we don't believe what we're told about compression.*/
      const char *description1, *description2;
      if (compression == ZLIB_METHOD)
        description1 = "as deflated";
      else if (compression == GZIP_METHOD)
        description1 = "as gzipped";
      else if (compression == NO_METHOD)
        description1 = "as uncompressed";
      else
        description1 = "with an unknown Content-Encoding";
      if (guessed == ZLIB_METHOD)
        description2 = "deflated";
      else if (guessed == GZIP_METHOD)
        description2 = "gzipped";
      else if (!plausible)
        description2 = "confusing binary junk";
      else
        description2 = "uncompressed";

      log_info(LD_HTTP, "HTTP body from server '%s:%d' was labeled %s, "
               "but it seems to be %s.%s",
               conn->base_.address, conn->base_.port, description1,
               description2,
               (compression>0 && guessed>0)?"  Trying both.":"");
    }
    /* Try declared compression first if we can. */
    if (compression == GZIP_METHOD  || compression == ZLIB_METHOD)
      tor_gzip_uncompress(&new_body, &new_len, body, body_len, compression,
                          !allow_partial, LOG_PROTOCOL_WARN);
    /* Okay, if that didn't work, and we think that it was compressed
     * differently, try that. */
    if (!new_body &&
        (guessed == GZIP_METHOD || guessed == ZLIB_METHOD) &&
        compression != guessed)
      tor_gzip_uncompress(&new_body, &new_len, body, body_len, guessed,
                          !allow_partial, LOG_PROTOCOL_WARN);
    /* If we're pretty sure that we have a compressed directory, and
     * we didn't manage to uncompress it, then warn and bail. */
    if (!plausible && !new_body) {
      log_fn(LOG_PROTOCOL_WARN, LD_HTTP,
             "Unable to decompress HTTP body (server '%s:%d').",
             conn->base_.address, conn->base_.port);
      tor_free(body); tor_free(headers); tor_free(reason);
      return -1;
    }
    if (new_body) {
      tor_free(body);
      body = new_body;
      body_len = new_len;
    }
  }

  if (conn->base_.purpose == DIR_PURPOSE_FETCH_CONSENSUS) {
    int r;
    const char *flavname = conn->requested_resource;
    if (status_code != 200) {
      int severity = (status_code == 304) ? LOG_INFO : LOG_WARN;
      tor_log(severity, LD_DIR,
          "Received http status code %d (%s) from server "
          "'%s:%d' while fetching consensus directory.",
           status_code, escaped(reason), conn->base_.address,
           conn->base_.port);
      tor_free(body); tor_free(headers); tor_free(reason);
      networkstatus_consensus_download_failed(status_code, flavname);
      return -1;
    }
    log_info(LD_DIR,"Received consensus directory (size %d) from server "
             "'%s:%d'", (int)body_len, conn->base_.address, conn->base_.port);
    if ((r=networkstatus_set_current_consensus(body, flavname, 0,
                                               conn->identity_digest))<0) {
      log_fn(r<-1?LOG_WARN:LOG_INFO, LD_DIR,
             "Unable to load %s consensus directory downloaded from "
             "server '%s:%d'. I'll try again soon.",
             flavname, conn->base_.address, conn->base_.port);
      tor_free(body); tor_free(headers); tor_free(reason);
      networkstatus_consensus_download_failed(0, flavname);
      return -1;
    }

    /* If we launched other fetches for this consensus, cancel them. */
    connection_dir_close_consensus_fetches(conn, flavname);

    /* launches router downloads as needed */
    routers_update_all_from_networkstatus(now, 3);
    update_microdescs_from_networkstatus(now);
    update_microdesc_downloads(now);
    directory_info_has_arrived(now, 0, 0);
    if (authdir_mode_v3(get_options())) {
      sr_act_post_consensus(
                   networkstatus_get_latest_consensus_by_flavor(FLAV_NS));
    }
    log_info(LD_DIR, "Successfully loaded consensus.");
  }

  if (conn->base_.purpose == DIR_PURPOSE_FETCH_CERTIFICATE) {
    if (status_code != 200) {
      log_warn(LD_DIR,
          "Received http status code %d (%s) from server "
          "'%s:%d' while fetching \"/tor/keys/%s\".",
           status_code, escaped(reason), conn->base_.address,
           conn->base_.port, conn->requested_resource);
      connection_dir_download_cert_failed(conn, status_code);
      tor_free(body); tor_free(headers); tor_free(reason);
      return -1;
    }
    log_info(LD_DIR,"Received authority certificates (size %d) from server "
             "'%s:%d'", (int)body_len, conn->base_.address, conn->base_.port);

    /*
     * Tell trusted_dirs_load_certs_from_string() whether it was by fp
     * or fp-sk pair.
     */
    src_code = -1;
    if (!strcmpstart(conn->requested_resource, "fp/")) {
      src_code = TRUSTED_DIRS_CERTS_SRC_DL_BY_ID_DIGEST;
    } else if (!strcmpstart(conn->requested_resource, "fp-sk/")) {
      src_code = TRUSTED_DIRS_CERTS_SRC_DL_BY_ID_SK_DIGEST;
    }

    if (src_code != -1) {
      if (trusted_dirs_load_certs_from_string(body, src_code, 1,
                                              conn->identity_digest)<0) {
        log_warn(LD_DIR, "Unable to parse fetched certificates");
        /* if we fetched more than one and only some failed, the successful
         * ones got flushed to disk so it's safe to call this on them */
        connection_dir_download_cert_failed(conn, status_code);
      } else {
        directory_info_has_arrived(now, 0, 0);
        log_info(LD_DIR, "Successfully loaded certificates from fetch.");
      }
    } else {
      log_warn(LD_DIR,
               "Couldn't figure out what to do with fetched certificates for "
               "unknown resource %s",
               conn->requested_resource);
      connection_dir_download_cert_failed(conn, status_code);
    }
  }
  if (conn->base_.purpose == DIR_PURPOSE_FETCH_STATUS_VOTE) {
    const char *msg;
    int st;
    log_info(LD_DIR,"Got votes (size %d) from server %s:%d",
             (int)body_len, conn->base_.address, conn->base_.port);
    if (status_code != 200) {
      log_warn(LD_DIR,
             "Received http status code %d (%s) from server "
             "'%s:%d' while fetching \"/tor/status-vote/next/%s.z\".",
             status_code, escaped(reason), conn->base_.address,
             conn->base_.port, conn->requested_resource);
      tor_free(body); tor_free(headers); tor_free(reason);
      return -1;
    }
    dirvote_add_vote(body, &msg, &st);
    if (st > 299) {
      log_warn(LD_DIR, "Error adding retrieved vote: %s", msg);
    } else {
      log_info(LD_DIR, "Added vote(s) successfully [msg: %s]", msg);
    }
  }
  if (conn->base_.purpose == DIR_PURPOSE_FETCH_DETACHED_SIGNATURES) {
    const char *msg = NULL;
    log_info(LD_DIR,"Got detached signatures (size %d) from server %s:%d",
             (int)body_len, conn->base_.address, conn->base_.port);
    if (status_code != 200) {
      log_warn(LD_DIR,
        "Received http status code %d (%s) from server '%s:%d' while fetching "
        "\"/tor/status-vote/next/consensus-signatures.z\".",
             status_code, escaped(reason), conn->base_.address,
             conn->base_.port);
      tor_free(body); tor_free(headers); tor_free(reason);
      return -1;
    }
    if (dirvote_add_signatures(body, conn->base_.address, &msg)<0) {
      log_warn(LD_DIR, "Problem adding detached signatures from %s:%d: %s",
               conn->base_.address, conn->base_.port, msg?msg:"???");
    }
  }

  if (conn->base_.purpose == DIR_PURPOSE_FETCH_SERVERDESC ||
      conn->base_.purpose == DIR_PURPOSE_FETCH_EXTRAINFO) {
    int was_ei = conn->base_.purpose == DIR_PURPOSE_FETCH_EXTRAINFO;
    smartlist_t *which = NULL;
    int n_asked_for = 0;
    int descriptor_digests = conn->requested_resource &&
                             !strcmpstart(conn->requested_resource,"d/");
    log_info(LD_DIR,"Received %s (size %d) from server '%s:%d'",
             was_ei ? "extra server info" : "server info",
             (int)body_len, conn->base_.address, conn->base_.port);
    if (conn->requested_resource &&
        (!strcmpstart(conn->requested_resource,"d/") ||
         !strcmpstart(conn->requested_resource,"fp/"))) {
      which = smartlist_new();
      dir_split_resource_into_fingerprints(conn->requested_resource +
                                             (descriptor_digests ? 2 : 3),
                                           which, NULL, 0);
      n_asked_for = smartlist_len(which);
    }
    if (status_code != 200) {
      int dir_okay = status_code == 404 ||
        (status_code == 400 && !strcmp(reason, "Servers unavailable."));
      /* 404 means that it didn't have them; no big deal.
       * Older (pre-0.1.1.8) servers said 400 Servers unavailable instead. */
      log_fn(dir_okay ? LOG_INFO : LOG_WARN, LD_DIR,
             "Received http status code %d (%s) from server '%s:%d' "
             "while fetching \"/tor/server/%s\". I'll try again soon.",
             status_code, escaped(reason), conn->base_.address,
             conn->base_.port, conn->requested_resource);
      if (!which) {
        connection_dir_download_routerdesc_failed(conn);
      } else {
        dir_routerdesc_download_failed(which, status_code,
                                       conn->router_purpose,
                                       was_ei, descriptor_digests);
        SMARTLIST_FOREACH(which, char *, cp, tor_free(cp));
        smartlist_free(which);
      }
      tor_free(body); tor_free(headers); tor_free(reason);
      return dir_okay ? 0 : -1;
    }
    /* Learn the routers, assuming we requested by fingerprint or "all"
     * or "authority".
     *
     * We use "authority" to fetch our own descriptor for
     * testing, and to fetch bridge descriptors for bootstrapharu. Ignore
     * the output of "authority" requests unless we are using bridges,
     * since otherwise they'll be the response from reachability tests,
     * and we don't really want to add that to our routerlist. */
    if (which || (conn->requested_resource &&
                  (!strcmpstart(conn->requested_resource, "all") ||
                   (!strcmpstart(conn->requested_resource, "authority") &&
                    get_options()->UseBridges)))) {
      /* as we learn from them, we remove them from 'which' */
      if (was_ei) {
        router_load_extrainfo_from_string(body, NULL, SAVED_NOWHERE, which,
                                          descriptor_digests);
      } else {
        //router_load_routers_from_string(body, NULL, SAVED_NOWHERE, which,
        //                       descriptor_digests, conn->router_purpose);
        if (load_downloaded_routers(body, which, descriptor_digests,
                                conn->router_purpose,
                                conn->base_.address))
          directory_info_has_arrived(now, 0, 0);
      }
    }
    if (which) { /* mark remaining ones as failed */
      log_info(LD_DIR, "Received %d/%d %s requested from %s:%d",
               n_asked_for-smartlist_len(which), n_asked_for,
               was_ei ? "extra-info documents" : "router descriptors",
               conn->base_.address, (int)conn->base_.port);
      if (smartlist_len(which)) {
        dir_routerdesc_download_failed(which, status_code,
                                       conn->router_purpose,
                                       was_ei, descriptor_digests);
      }
      SMARTLIST_FOREACH(which, char *, cp, tor_free(cp));
      smartlist_free(which);
    }
    if (directory_conn_is_self_reachability_test(conn))
      router_dirport_found_reachable();
  }
  if (conn->base_.purpose == DIR_PURPOSE_FETCH_MICRODESC) {
    smartlist_t *which = NULL;
    log_info(LD_DIR,"Received answer to microdescriptor request (status %d, "
             "size %d) from server '%s:%d'",
             status_code, (int)body_len, conn->base_.address,
             conn->base_.port);
    tor_assert(conn->requested_resource &&
               !strcmpstart(conn->requested_resource, "d/"));
    which = smartlist_new();
    dir_split_resource_into_fingerprints(conn->requested_resource+2,
                                         which, NULL,
                                         DSR_DIGEST256|DSR_BASE64);
    if (status_code != 200) {
      log_info(LD_DIR, "Received status code %d (%s) from server "
               "'%s:%d' while fetching \"/tor/micro/%s\".  I'll try again "
               "soon.",
               status_code, escaped(reason), conn->base_.address,
               (int)conn->base_.port, conn->requested_resource);
      dir_microdesc_download_failed(which, status_code);
      SMARTLIST_FOREACH(which, char *, cp, tor_free(cp));
      smartlist_free(which);
      tor_free(body); tor_free(headers); tor_free(reason);
      return 0;
    } else {
      smartlist_t *mds;
      mds = microdescs_add_to_cache(get_microdesc_cache(),
                                    body, body+body_len, SAVED_NOWHERE, 0,
                                    now, which);
      if (smartlist_len(which)) {
        /* Mark remaining ones as failed. */
        dir_microdesc_download_failed(which, status_code);
      }
      if (mds && smartlist_len(mds)) {
        control_event_bootstrap(BOOTSTRAP_STATUS_LOADING_DESCRIPTORS,
                                count_loading_descriptors_progress());
        directory_info_has_arrived(now, 0, 1);
      }
      SMARTLIST_FOREACH(which, char *, cp, tor_free(cp));
      smartlist_free(which);
      smartlist_free(mds);
    }
  }

  if (conn->base_.purpose == DIR_PURPOSE_UPLOAD_DIR) {
    switch (status_code) {
      case 200: {
          dir_server_t *ds =
            router_get_trusteddirserver_by_digest(conn->identity_digest);
          char *rejected_hdr = http_get_header(headers,
                                               "X-Descriptor-Not-New: ");
          if (rejected_hdr) {
            if (!strcmp(rejected_hdr, "Yes")) {
              log_info(LD_GENERAL,
                       "Authority '%s' declined our descriptor (not new)",
                       ds->nickname);
              /* XXXX use this information; be sure to upload next one
               * sooner. -NM */
              /* XXXX++ On further thought, the task above implies that we're
               * basing our regenerate-descriptor time on when we uploaded the
               * last descriptor, not on the published time of the last
               * descriptor.  If those are different, that's a bad thing to
               * do. -NM */
            }
            tor_free(rejected_hdr);
          }
          log_info(LD_GENERAL,"eof (status 200) after uploading server "
                   "descriptor: finished.");
          control_event_server_status(
                      LOG_NOTICE, "ACCEPTED_SERVER_DESCRIPTOR DIRAUTH=%s:%d",
                      conn->base_.address, conn->base_.port);

          ds->has_accepted_serverdesc = 1;
          if (directories_have_accepted_server_descriptor())
            control_event_server_status(LOG_NOTICE, "GOOD_SERVER_DESCRIPTOR");
        }
        break;
      case 400:
        log_warn(LD_GENERAL,"http status 400 (%s) response from "
                 "dirserver '%s:%d'. Please correct.",
                 escaped(reason), conn->base_.address, conn->base_.port);
        control_event_server_status(LOG_WARN,
                      "BAD_SERVER_DESCRIPTOR DIRAUTH=%s:%d REASON=\"%s\"",
                      conn->base_.address, conn->base_.port, escaped(reason));
        break;
      default:
        log_warn(LD_GENERAL,
             "http status %d (%s) reason unexpected while uploading "
             "descriptor to server '%s:%d').",
             status_code, escaped(reason), conn->base_.address,
             conn->base_.port);
        break;
    }
    /* return 0 in all cases, since we don't want to mark any
     * dirservers down just because they don't like us. */
  }

  if (conn->base_.purpose == DIR_PURPOSE_UPLOAD_VOTE) {
    switch (status_code) {
      case 200: {
        log_notice(LD_DIR,"Uploaded a vote to dirserver %s:%d",
                   conn->base_.address, conn->base_.port);
        }
        break;
      case 400:
        log_warn(LD_DIR,"http status 400 (%s) response after uploading "
                 "vote to dirserver '%s:%d'. Please correct.",
                 escaped(reason), conn->base_.address, conn->base_.port);
        break;
      default:
        log_warn(LD_GENERAL,
             "http status %d (%s) reason unexpected while uploading "
             "vote to server '%s:%d').",
             status_code, escaped(reason), conn->base_.address,
             conn->base_.port);
        break;
    }
    /* return 0 in all cases, since we don't want to mark any
     * dirservers down just because they don't like us. */
  }

  if (conn->base_.purpose == DIR_PURPOSE_UPLOAD_SIGNATURES) {
    switch (status_code) {
      case 200: {
        log_notice(LD_DIR,"Uploaded signature(s) to dirserver %s:%d",
                   conn->base_.address, conn->base_.port);
        }
        break;
      case 400:
        log_warn(LD_DIR,"http status 400 (%s) response after uploading "
                 "signatures to dirserver '%s:%d'. Please correct.",
                 escaped(reason), conn->base_.address, conn->base_.port);
        break;
      default:
        log_warn(LD_GENERAL,
             "http status %d (%s) reason unexpected while uploading "
             "signatures to server '%s:%d').",
             status_code, escaped(reason), conn->base_.address,
             conn->base_.port);
        break;
    }
    /* return 0 in all cases, since we don't want to mark any
     * dirservers down just because they don't like us. */
  }

  if (conn->base_.purpose == DIR_PURPOSE_FETCH_RENDDESC_V2) {
    #define SEND_HS_DESC_FAILED_EVENT(reason) ( \
      control_event_hs_descriptor_failed(conn->rend_data, \
                                         conn->identity_digest, \
                                         reason) )
    #define SEND_HS_DESC_FAILED_CONTENT() ( \
  control_event_hs_descriptor_content(rend_data_get_address(conn->rend_data), \
                                      conn->requested_resource,         \
                                      conn->identity_digest,            \
                                      NULL) )
    tor_assert(conn->rend_data);
    log_info(LD_REND,"Received rendezvous descriptor (size %d, status %d "
             "(%s))",
             (int)body_len, status_code, escaped(reason));
    switch (status_code) {
      case 200:
      {
        rend_cache_entry_t *entry = NULL;

        if (rend_cache_store_v2_desc_as_client(body,
            conn->requested_resource, conn->rend_data, &entry) < 0) {
          log_warn(LD_REND,"Fetching v2 rendezvous descriptor failed. "
                   "Retrying at another directory.");
          /* We'll retry when connection_about_to_close_connection()
           * cleans this dir conn up. */
          SEND_HS_DESC_FAILED_EVENT("BAD_DESC");
          SEND_HS_DESC_FAILED_CONTENT();
        } else {
          char service_id[REND_SERVICE_ID_LEN_BASE32 + 1];
          /* Should never be NULL here if we found the descriptor. */
          tor_assert(entry);
          rend_get_service_id(entry->parsed->pk, service_id);

          /* success. notify pending connections about this. */
          log_info(LD_REND, "Successfully fetched v2 rendezvous "
                   "descriptor.");
          control_event_hs_descriptor_received(service_id,
                                               conn->rend_data,
                                               conn->identity_digest);
          control_event_hs_descriptor_content(service_id,
                                              conn->requested_resource,
                                              conn->identity_digest,
                                              body);
          conn->base_.purpose = DIR_PURPOSE_HAS_FETCHED_RENDDESC_V2;
          rend_client_desc_trynow(service_id);
          memwipe(service_id, 0, sizeof(service_id));
        }
        break;
      }
      case 404:
        /* Not there. We'll retry when
         * connection_about_to_close_connection() cleans this conn up. */
        log_info(LD_REND,"Fetching v2 rendezvous descriptor failed: "
                         "Retrying at another directory.");
        SEND_HS_DESC_FAILED_EVENT("NOT_FOUND");
        SEND_HS_DESC_FAILED_CONTENT();
        break;
      case 400:
        log_warn(LD_REND, "Fetching v2 rendezvous descriptor failed: "
                 "http status 400 (%s). Dirserver didn't like our "
                 "v2 rendezvous query? Retrying at another directory.",
                 escaped(reason));
        SEND_HS_DESC_FAILED_EVENT("QUERY_REJECTED");
        SEND_HS_DESC_FAILED_CONTENT();
        break;
      default:
        log_warn(LD_REND, "Fetching v2 rendezvous descriptor failed: "
                 "http status %d (%s) response unexpected while "
                 "fetching v2 hidden service descriptor (server '%s:%d'). "
                 "Retrying at another directory.",
                 status_code, escaped(reason), conn->base_.address,
                 conn->base_.port);
        SEND_HS_DESC_FAILED_EVENT("UNEXPECTED");
        SEND_HS_DESC_FAILED_CONTENT();
        break;
    }
  }

  if (conn->base_.purpose == DIR_PURPOSE_UPLOAD_RENDDESC_V2) {
    #define SEND_HS_DESC_UPLOAD_FAILED_EVENT(reason) ( \
      control_event_hs_descriptor_upload_failed( \
        conn->identity_digest, \
        rend_data_get_address(conn->rend_data), \
        reason) )
    log_info(LD_REND,"Uploaded rendezvous descriptor (status %d "
             "(%s))",
             status_code, escaped(reason));
    /* Without the rend data, we'll have a problem identifying what has been
     * uploaded for which service. */
    tor_assert(conn->rend_data);
    switch (status_code) {
      case 200:
        log_info(LD_REND,
                 "Uploading rendezvous descriptor: finished with status "
                 "200 (%s)", escaped(reason));
        control_event_hs_descriptor_uploaded(conn->identity_digest,
                                    rend_data_get_address(conn->rend_data));
        rend_service_desc_has_uploaded(conn->rend_data);
        break;
      case 400:
        log_warn(LD_REND,"http status 400 (%s) response from dirserver "
                 "'%s:%d'. Malformed rendezvous descriptor?",
                 escaped(reason), conn->base_.address, conn->base_.port);
        SEND_HS_DESC_UPLOAD_FAILED_EVENT("UPLOAD_REJECTED");
        break;
      default:
        log_warn(LD_REND,"http status %d (%s) response unexpected (server "
                 "'%s:%d').",
                 status_code, escaped(reason), conn->base_.address,
                 conn->base_.port);
        SEND_HS_DESC_UPLOAD_FAILED_EVENT("UNEXPECTED");
        break;
    }
  }
  tor_free(body); tor_free(headers); tor_free(reason);
  return 0;
}

/** Called when a directory connection reaches EOF. */
int
connection_dir_reached_eof(dir_connection_t *conn)
{
  int retval;
  if (conn->base_.state != DIR_CONN_STATE_CLIENT_READING) {
    log_info(LD_HTTP,"conn reached eof, not reading. [state=%d] Closing.",
             conn->base_.state);
    connection_close_immediate(TO_CONN(conn)); /* error: give up on flushing */
    connection_mark_for_close(TO_CONN(conn));
    return -1;
  }

  retval = connection_dir_client_reached_eof(conn);
  if (retval == 0) /* success */
    conn->base_.state = DIR_CONN_STATE_CLIENT_FINISHED;
  connection_mark_for_close(TO_CONN(conn));
  return retval;
}

/** If any directory object is arriving, and it's over 10MB large, we're
 * getting DoS'd.  (As of 0.1.2.x, raw directories are about 1MB, and we never
 * ask for more than 96 router descriptors at a time.)
 */
#define MAX_DIRECTORY_OBJECT_SIZE (10*(1<<20))

#define MAX_VOTE_DL_SIZE (MAX_DIRECTORY_OBJECT_SIZE * 5)

/** Read handler for directory connections.  (That's connections <em>to</em>
 * directory servers and connections <em>at</em> directory servers.)
 */
int
connection_dir_process_inbuf(dir_connection_t *conn)
{
  size_t max_size;
  tor_assert(conn);
  tor_assert(conn->base_.type == CONN_TYPE_DIR);

  /* Directory clients write, then read data until they receive EOF;
   * directory servers read data until they get an HTTP command, then
   * write their response (when it's finished flushing, they mark for
   * close).
   */

  /* If we're on the dirserver side, look for a command. */
  if (conn->base_.state == DIR_CONN_STATE_SERVER_COMMAND_WAIT) {
    if (directory_handle_command(conn) < 0) {
      connection_mark_for_close(TO_CONN(conn));
      return -1;
    }
    return 0;
  }

  max_size =
    (TO_CONN(conn)->purpose == DIR_PURPOSE_FETCH_STATUS_VOTE) ?
    MAX_VOTE_DL_SIZE : MAX_DIRECTORY_OBJECT_SIZE;

  if (connection_get_inbuf_len(TO_CONN(conn)) > max_size) {
    log_warn(LD_HTTP,
             "Too much data received from directory connection (%s): "
             "denial of service attempt, or you need to upgrade?",
             conn->base_.address);
    connection_mark_for_close(TO_CONN(conn));
    return -1;
  }

  if (!conn->base_.inbuf_reached_eof)
    log_debug(LD_HTTP,"Got data, not eof. Leaving on inbuf.");
  return 0;
}

/** Called when we're about to finally unlink and free a directory connection:
 * perform necessary accounting and cleanup */
void
connection_dir_about_to_close(dir_connection_t *dir_conn)
{
  connection_t *conn = TO_CONN(dir_conn);

  if (conn->state < DIR_CONN_STATE_CLIENT_FINISHED) {
    /* It's a directory connection and connecting or fetching
     * failed: forget about this router, and maybe try again. */
    connection_dir_request_failed(dir_conn);
  }
  /* If we were trying to fetch a v2 rend desc and did not succeed,
   * retry as needed. (If a fetch is successful, the connection state
   * is changed to DIR_PURPOSE_HAS_FETCHED_RENDDESC_V2 to mark that
   * refetching is unnecessary.) */
  if (conn->purpose == DIR_PURPOSE_FETCH_RENDDESC_V2 &&
      dir_conn->rend_data &&
      strlen(rend_data_get_address(dir_conn->rend_data)) ==
             REND_SERVICE_ID_LEN_BASE32)
    rend_client_refetch_v2_renddesc(dir_conn->rend_data);
}

/** Create an http response for the client <b>conn</b> out of
 * <b>status</b> and <b>reason_phrase</b>. Write it to <b>conn</b>.
 */
static void
write_http_status_line(dir_connection_t *conn, int status,
                       const char *reason_phrase)
{
  char buf[256];
  if (tor_snprintf(buf, sizeof(buf), "HTTP/1.0 %d %s\r\n\r\n",
      status, reason_phrase ? reason_phrase : "OK") < 0) {
    log_warn(LD_BUG,"status line too long.");
    return;
  }
  log_debug(LD_DIRSERV,"Wrote status 'HTTP/1.0 %d %s'", status, reason_phrase);
  connection_write_to_buf(buf, strlen(buf), TO_CONN(conn));
}

/** Write the header for an HTTP/1.0 response onto <b>conn</b>-\>outbuf,
 * with <b>type</b> as the Content-Type.
 *
 * If <b>length</b> is nonnegative, it is the Content-Length.
 * If <b>encoding</b> is provided, it is the Content-Encoding.
 * If <b>cache_lifetime</b> is greater than 0, the content may be cached for
 * up to cache_lifetime seconds.  Otherwise, the content may not be cached. */
static void
write_http_response_header_impl(dir_connection_t *conn, ssize_t length,
                           const char *type, const char *encoding,
                           const char *extra_headers,
                           long cache_lifetime)
{
  char date[RFC1123_TIME_LEN+1];
  char tmp[1024];
  char *cp;
  time_t now = time(NULL);

  tor_assert(conn);

  format_rfc1123_time(date, now);
  cp = tmp;
  tor_snprintf(cp, sizeof(tmp),
               "HTTP/1.0 200 OK\r\nDate: %s\r\n",
               date);
  cp += strlen(tmp);
  if (type) {
    tor_snprintf(cp, sizeof(tmp)-(cp-tmp), "Content-Type: %s\r\n", type);
    cp += strlen(cp);
  }
  if (!is_local_addr(&conn->base_.addr)) {
    /* Don't report the source address for a nearby/private connection.
     * Otherwise we tend to mis-report in cases where incoming ports are
     * being forwarded to a Tor server running behind the firewall. */
    tor_snprintf(cp, sizeof(tmp)-(cp-tmp),
                 X_ADDRESS_HEADER "%s\r\n", conn->base_.address);
    cp += strlen(cp);
  }
  if (encoding) {
    tor_snprintf(cp, sizeof(tmp)-(cp-tmp),
                 "Content-Encoding: %s\r\n", encoding);
    cp += strlen(cp);
  }
  if (length >= 0) {
    tor_snprintf(cp, sizeof(tmp)-(cp-tmp),
                 "Content-Length: %ld\r\n", (long)length);
    cp += strlen(cp);
  }
  if (cache_lifetime > 0) {
    char expbuf[RFC1123_TIME_LEN+1];
    format_rfc1123_time(expbuf, (time_t)(now + cache_lifetime));
    /* We could say 'Cache-control: max-age=%d' here if we start doing
     * http/1.1 */
    tor_snprintf(cp, sizeof(tmp)-(cp-tmp),
                 "Expires: %s\r\n", expbuf);
    cp += strlen(cp);
  } else if (cache_lifetime == 0) {
    /* We could say 'Cache-control: no-cache' here if we start doing
     * http/1.1 */
    strlcpy(cp, "Pragma: no-cache\r\n", sizeof(tmp)-(cp-tmp));
    cp += strlen(cp);
  }
  if (extra_headers) {
    strlcpy(cp, extra_headers, sizeof(tmp)-(cp-tmp));
    cp += strlen(cp);
  }
  if (sizeof(tmp)-(cp-tmp) > 3)
    memcpy(cp, "\r\n", 3);
  else
    tor_assert(0);
  connection_write_to_buf(tmp, strlen(tmp), TO_CONN(conn));
}

/** As write_http_response_header_impl, but sets encoding and content-typed
 * based on whether the response will be <b>compressed</b> or not. */
static void
write_http_response_header(dir_connection_t *conn, ssize_t length,
                           int compressed, long cache_lifetime)
{
  write_http_response_header_impl(conn, length,
                          compressed?"application/octet-stream":"text/plain",
                          compressed?"deflate":"identity",
                             NULL,
                             cache_lifetime);
}

/** Decide whether a client would accept the consensus we have.
 *
 * Clients can say they only want a consensus if it's signed by more
 * than half the authorities in a list.  They pass this list in
 * the url as "...consensus/<b>fpr</b>+<b>fpr</b>+<b>fpr</b>".
 *
 * <b>fpr</b> may be an abbreviated fingerprint, i.e. only a left substring
 * of the full authority identity digest. (Only strings of even length,
 * i.e. encodings of full bytes, are handled correctly.  In the case
 * of an odd number of hex digits the last one is silently ignored.)
 *
 * Returns 1 if more than half of the requested authorities signed the
 * consensus, 0 otherwise.
 */
int
client_likes_consensus(networkstatus_t *v, const char *want_url)
{
  smartlist_t *want_authorities = smartlist_new();
  int need_at_least;
  int have = 0;

  dir_split_resource_into_fingerprints(want_url, want_authorities, NULL, 0);
  need_at_least = smartlist_len(want_authorities)/2+1;
  SMARTLIST_FOREACH_BEGIN(want_authorities, const char *, d) {
    char want_digest[DIGEST_LEN];
    size_t want_len = strlen(d)/2;
    if (want_len > DIGEST_LEN)
      want_len = DIGEST_LEN;

    if (base16_decode(want_digest, DIGEST_LEN, d, want_len*2)
                      != (int) want_len) {
      log_fn(LOG_PROTOCOL_WARN, LD_DIR,
             "Failed to decode requested authority digest %s.", escaped(d));
      continue;
    };

    SMARTLIST_FOREACH_BEGIN(v->voters, networkstatus_voter_info_t *, vi) {
      if (smartlist_len(vi->sigs) &&
          tor_memeq(vi->identity_digest, want_digest, want_len)) {
        have++;
        break;
      };
    } SMARTLIST_FOREACH_END(vi);

    /* early exit, if we already have enough */
    if (have >= need_at_least)
      break;
  } SMARTLIST_FOREACH_END(d);

  SMARTLIST_FOREACH(want_authorities, char *, d, tor_free(d));
  smartlist_free(want_authorities);
  return (have >= need_at_least);
}

/** Return the compression level we should use for sending a compressed
 * response of size <b>n_bytes</b>. */
STATIC zlib_compression_level_t
choose_compression_level(ssize_t n_bytes)
{
  if (! have_been_under_memory_pressure()) {
    return HIGH_COMPRESSION; /* we have plenty of RAM. */
  } else if (n_bytes < 0) {
    return HIGH_COMPRESSION; /* unknown; might be big. */
  } else if (n_bytes < 1024) {
    return LOW_COMPRESSION;
  } else if (n_bytes < 2048) {
    return MEDIUM_COMPRESSION;
  } else {
    return HIGH_COMPRESSION;
  }
}

/** Information passed to handle a GET request. */
typedef struct get_handler_args_t {
  /** True if the client asked for compressed data. */
  int compressed;
  /** If nonzero, the time included an if-modified-since header with this
   * value. */
  time_t if_modified_since;
  /** String containing the requested URL or resource. */
  const char *url;
  /** String containing the HTTP headers */
  const char *headers;
} get_handler_args_t;

/** Entry for handling an HTTP GET request.
 *
 * This entry matches a request if "string" is equal to the requested
 * resource, or if "is_prefix" is true and "string" is a prefix of the
 * requested resource.
 *
 * The 'handler' function is called to handle the request.  It receives
 * an arguments structure, and must return 0 on success or -1 if we should
 * close the connection.
 **/
typedef struct url_table_ent_s {
  const char *string;
  int is_prefix;
  int (*handler)(dir_connection_t *conn, const get_handler_args_t *args);
} url_table_ent_t;

static int handle_get_frontpage(dir_connection_t *conn,
                                const get_handler_args_t *args);
static int handle_get_current_consensus(dir_connection_t *conn,
                                const get_handler_args_t *args);
static int handle_get_status_vote(dir_connection_t *conn,
                                const get_handler_args_t *args);
static int handle_get_microdesc(dir_connection_t *conn,
                                const get_handler_args_t *args);
static int handle_get_descriptor(dir_connection_t *conn,
                                const get_handler_args_t *args);
static int handle_get_keys(dir_connection_t *conn,
                                const get_handler_args_t *args);
static int handle_get_hs_descriptor_v2(dir_connection_t *conn,
                                       const get_handler_args_t *args);
static int handle_get_robots(dir_connection_t *conn,
                                const get_handler_args_t *args);
static int handle_get_networkstatus_bridges(dir_connection_t *conn,
                                const get_handler_args_t *args);

/** Table for handling GET requests. */
static const url_table_ent_t url_table[] = {
  { "/tor/", 0, handle_get_frontpage },
  { "/tor/status-vote/current/consensus", 1, handle_get_current_consensus },
  { "/tor/status-vote/current/", 1, handle_get_status_vote },
  { "/tor/status-vote/next/", 1, handle_get_status_vote },
  { "/tor/micro/d/", 1, handle_get_microdesc },
  { "/tor/server/", 1, handle_get_descriptor },
  { "/tor/extra/", 1, handle_get_descriptor },
  { "/tor/keys/", 1, handle_get_keys },
  { "/tor/rendezvous2/", 1, handle_get_hs_descriptor_v2 },
  { "/tor/hs/3/", 1, handle_get_hs_descriptor_v3 },
  { "/tor/robots.txt", 0, handle_get_robots },
  { "/tor/networkstatus-bridges", 0, handle_get_networkstatus_bridges },
  { NULL, 0, NULL },
};

/** Helper function: called when a dirserver gets a complete HTTP GET
 * request.  Look for a request for a directory or for a rendezvous
 * service descriptor.  On finding one, write a response into
 * conn-\>outbuf.  If the request is unrecognized, send a 404.
 * Return 0 if we handled this successfully, or -1 if we need to close
 * the connection. */
MOCK_IMPL(STATIC int,
directory_handle_command_get,(dir_connection_t *conn, const char *headers,
                              const char *req_body, size_t req_body_len))
{
  char *url, *url_mem, *header;
  time_t if_modified_since = 0;
  int compressed;
  size_t url_len;

  /* We ignore the body of a GET request. */
  (void)req_body;
  (void)req_body_len;

  log_debug(LD_DIRSERV,"Received GET command.");

  conn->base_.state = DIR_CONN_STATE_SERVER_WRITING;

  if (parse_http_url(headers, &url) < 0) {
    write_http_status_line(conn, 400, "Bad request");
    return 0;
  }
  if ((header = http_get_header(headers, "If-Modified-Since: "))) {
    struct tm tm;
    if (parse_http_time(header, &tm) == 0) {
      if (tor_timegm(&tm, &if_modified_since)<0) {
        if_modified_since = 0;
      } else {
        log_debug(LD_DIRSERV, "If-Modified-Since is '%s'.", escaped(header));
      }
    }
    /* The correct behavior on a malformed If-Modified-Since header is to
     * act as if no If-Modified-Since header had been given. */
    tor_free(header);
  }
  log_debug(LD_DIRSERV,"rewritten url as '%s'.", escaped(url));

  url_mem = url;
  url_len = strlen(url);
  compressed = url_len > 2 && !strcmp(url+url_len-2, ".z");
  if (compressed) {
    url[url_len-2] = '\0';
    url_len -= 2;
  }

  get_handler_args_t args;
  args.url = url;
  args.headers = headers;
  args.if_modified_since = if_modified_since;
  args.compressed = compressed;

  int i, result = -1;
  for (i = 0; url_table[i].string; ++i) {
    int match;
    if (url_table[i].is_prefix) {
      match = !strcmpstart(url, url_table[i].string);
    } else {
      match = !strcmp(url, url_table[i].string);
    }
    if (match) {
      result = url_table[i].handler(conn, &args);
      goto done;
    }
  }

  /* we didn't recognize the url */
  write_http_status_line(conn, 404, "Not found");
  result = 0;

 done:
  tor_free(url_mem);
  return result;
}

/** Helper function for GET / or GET /tor/
 */
static int
handle_get_frontpage(dir_connection_t *conn, const get_handler_args_t *args)
{
  (void) args; /* unused */
  const char *frontpage = get_dirportfrontpage();

  if (frontpage) {
    size_t dlen;
    dlen = strlen(frontpage);
    /* Let's return a disclaimer page (users shouldn't use V1 anymore,
       and caches don't fetch '/', so this is safe). */

    /* [We don't check for write_bucket_low here, since we want to serve
     *  this page no matter what.] */
    write_http_response_header_impl(conn, dlen, "text/html", "identity",
                                    NULL, DIRPORTFRONTPAGE_CACHE_LIFETIME);
    connection_write_to_buf(frontpage, dlen, TO_CONN(conn));
  } else {
    write_http_status_line(conn, 404, "Not found");
  }
  return 0;
}

/** Warn that the consensus <b>v</b> of type <b>flavor</b> is too old and will
 * not be served to clients. Rate-limit the warning to avoid logging an entry
 * on every request.
 */
static void
warn_consensus_is_too_old(networkstatus_t *v, const char *flavor, time_t now)
{
#define TOO_OLD_WARNING_INTERVAL (60*60)
  static ratelim_t warned = RATELIM_INIT(TOO_OLD_WARNING_INTERVAL);
  char timestamp[ISO_TIME_LEN+1];
  char *dupes;

  if ((dupes = rate_limit_log(&warned, now))) {
    format_local_iso_time(timestamp, v->valid_until);
    log_warn(LD_DIRSERV, "Our %s%sconsensus is too old, so we will not "
             "serve it to clients. It was valid until %s local time and we "
             "continued to serve it for up to 24 hours after it expired.%s",
             flavor ? flavor : "", flavor ? " " : "", timestamp, dupes);
    tor_free(dupes);
  }
}

/** Helper function for GET /tor/status-vote/current/consensus
 */
static int
handle_get_current_consensus(dir_connection_t *conn,
                             const get_handler_args_t *args)
{
  const char *url = args->url;
  const int compressed = args->compressed;
  const time_t if_modified_since = args->if_modified_since;

  {
    /* v3 network status fetch. */
    smartlist_t *dir_fps = smartlist_new();
    long lifetime = NETWORKSTATUS_CACHE_LIFETIME;

    networkstatus_t *v;
    time_t now = time(NULL);
    const char *want_fps = NULL;
    char *flavor = NULL;
    int flav = FLAV_NS;
#define CONSENSUS_URL_PREFIX "/tor/status-vote/current/consensus/"
#define CONSENSUS_FLAVORED_PREFIX "/tor/status-vote/current/consensus-"
    /* figure out the flavor if any, and who we wanted to sign the thing */
    if (!strcmpstart(url, CONSENSUS_FLAVORED_PREFIX)) {
      const char *f, *cp;
      f = url + strlen(CONSENSUS_FLAVORED_PREFIX);
      cp = strchr(f, '/');
      if (cp) {
        want_fps = cp+1;
        flavor = tor_strndup(f, cp-f);
      } else {
        flavor = tor_strdup(f);
      }
      flav = networkstatus_parse_flavor_name(flavor);
      if (flav < 0)
        flav = FLAV_NS;
    } else {
      if (!strcmpstart(url, CONSENSUS_URL_PREFIX))
        want_fps = url+strlen(CONSENSUS_URL_PREFIX);
    }

    v = networkstatus_get_latest_consensus_by_flavor(flav);

    if (v && !networkstatus_consensus_reasonably_live(v, now)) {
      write_http_status_line(conn, 404, "Consensus is too old");
      warn_consensus_is_too_old(v, flavor, now);
      smartlist_free(dir_fps);
      geoip_note_ns_response(GEOIP_REJECT_NOT_FOUND);
      tor_free(flavor);
      goto done;
    }

    if (v && want_fps &&
        !client_likes_consensus(v, want_fps)) {
      write_http_status_line(conn, 404, "Consensus not signed by sufficient "
                             "number of requested authorities");
      smartlist_free(dir_fps);
      geoip_note_ns_response(GEOIP_REJECT_NOT_ENOUGH_SIGS);
      tor_free(flavor);
      goto done;
    }

    {
      char *fp = tor_malloc_zero(DIGEST_LEN);
      if (flavor)
        strlcpy(fp, flavor, DIGEST_LEN);
      tor_free(flavor);
      smartlist_add(dir_fps, fp);
    }
    lifetime = (v && v->fresh_until > now) ? v->fresh_until - now : 0;

    if (!smartlist_len(dir_fps)) { /* we failed to create/cache cp */
      write_http_status_line(conn, 503, "Network status object unavailable");
      smartlist_free(dir_fps);
      geoip_note_ns_response(GEOIP_REJECT_UNAVAILABLE);
      goto done;
    }

    if (!dirserv_remove_old_statuses(dir_fps, if_modified_since)) {
      write_http_status_line(conn, 404, "Not found");
      SMARTLIST_FOREACH(dir_fps, char *, cp, tor_free(cp));
      smartlist_free(dir_fps);
      geoip_note_ns_response(GEOIP_REJECT_NOT_FOUND);
      goto done;
    } else if (!smartlist_len(dir_fps)) {
      write_http_status_line(conn, 304, "Not modified");
      SMARTLIST_FOREACH(dir_fps, char *, cp, tor_free(cp));
      smartlist_free(dir_fps);
      geoip_note_ns_response(GEOIP_REJECT_NOT_MODIFIED);
      goto done;
    }

    size_t dlen = dirserv_estimate_data_size(dir_fps, 0, compressed);
    if (global_write_bucket_low(TO_CONN(conn), dlen, 2)) {
      log_debug(LD_DIRSERV,
               "Client asked for network status lists, but we've been "
               "writing too many bytes lately. Sending 503 Dir busy.");
      write_http_status_line(conn, 503, "Directory busy, try again later");
      SMARTLIST_FOREACH(dir_fps, char *, fp, tor_free(fp));
      smartlist_free(dir_fps);

      geoip_note_ns_response(GEOIP_REJECT_BUSY);
      goto done;
    }

    tor_addr_t addr;
    if (tor_addr_parse(&addr, (TO_CONN(conn))->address) >= 0) {
      geoip_note_client_seen(GEOIP_CLIENT_NETWORKSTATUS,
                             &addr, NULL,
                             time(NULL));
      geoip_note_ns_response(GEOIP_SUCCESS);
      /* Note that a request for a network status has started, so that we
       * can measure the download time later on. */
      if (conn->dirreq_id)
        geoip_start_dirreq(conn->dirreq_id, dlen, DIRREQ_TUNNELED);
      else
        geoip_start_dirreq(TO_CONN(conn)->global_identifier, dlen,
                           DIRREQ_DIRECT);
    }

    write_http_response_header(conn, -1, compressed,
                               smartlist_len(dir_fps) == 1 ? lifetime : 0);
    conn->fingerprint_stack = dir_fps;
    if (! compressed)
      conn->zlib_state = tor_zlib_new(0, ZLIB_METHOD, HIGH_COMPRESSION);

    /* Prime the connection with some data. */
    conn->dir_spool_src = DIR_SPOOL_NETWORKSTATUS;
    connection_dirserv_flushed_some(conn);
    goto done;
  }

 done:
  return 0;
}

/** Helper function for GET /tor/status-vote/{current,next}/...
 */
static int
handle_get_status_vote(dir_connection_t *conn, const get_handler_args_t *args)
{
  const char *url = args->url;
  const int compressed = args->compressed;
  {
    int current;
    ssize_t body_len = 0;
    ssize_t estimated_len = 0;
    smartlist_t *items = smartlist_new();
    smartlist_t *dir_items = smartlist_new();
    int lifetime = 60; /* XXXX?? should actually use vote intervals. */
    url += strlen("/tor/status-vote/");
    current = !strcmpstart(url, "current/");
    url = strchr(url, '/');
    tor_assert(url);
    ++url;
    if (!strcmp(url, "consensus")) {
      const char *item;
      tor_assert(!current); /* we handle current consensus specially above,
                             * since it wants to be spooled. */
      if ((item = dirvote_get_pending_consensus(FLAV_NS)))
        smartlist_add(items, (char*)item);
    } else if (!current && !strcmp(url, "consensus-signatures")) {
      /* XXXX the spec says that we should implement
       * current/consensus-signatures too.  It doesn't seem to be needed,
       * though. */
      const char *item;
      if ((item=dirvote_get_pending_detached_signatures()))
        smartlist_add(items, (char*)item);
    } else if (!strcmp(url, "authority")) {
      const cached_dir_t *d;
      int flags = DGV_BY_ID |
        (current ? DGV_INCLUDE_PREVIOUS : DGV_INCLUDE_PENDING);
      if ((d=dirvote_get_vote(NULL, flags)))
        smartlist_add(dir_items, (cached_dir_t*)d);
    } else {
      const cached_dir_t *d;
      smartlist_t *fps = smartlist_new();
      int flags;
      if (!strcmpstart(url, "d/")) {
        url += 2;
        flags = DGV_INCLUDE_PENDING | DGV_INCLUDE_PREVIOUS;
      } else {
        flags = DGV_BY_ID |
          (current ? DGV_INCLUDE_PREVIOUS : DGV_INCLUDE_PENDING);
      }
      dir_split_resource_into_fingerprints(url, fps, NULL,
                                           DSR_HEX|DSR_SORT_UNIQ);
      SMARTLIST_FOREACH(fps, char *, fp, {
          if ((d = dirvote_get_vote(fp, flags)))
            smartlist_add(dir_items, (cached_dir_t*)d);
          tor_free(fp);
        });
      smartlist_free(fps);
    }
    if (!smartlist_len(dir_items) && !smartlist_len(items)) {
      write_http_status_line(conn, 404, "Not found");
      goto vote_done;
    }
    SMARTLIST_FOREACH(dir_items, cached_dir_t *, d,
                      body_len += compressed ? d->dir_z_len : d->dir_len);
    estimated_len += body_len;
    SMARTLIST_FOREACH(items, const char *, item, {
        size_t ln = strlen(item);
        if (compressed) {
          estimated_len += ln/2;
        } else {
          body_len += ln; estimated_len += ln;
        }
      });

    if (global_write_bucket_low(TO_CONN(conn), estimated_len, 2)) {
      write_http_status_line(conn, 503, "Directory busy, try again later");
      goto vote_done;
    }
    write_http_response_header(conn, body_len ? body_len : -1, compressed,
                 lifetime);

    if (smartlist_len(items)) {
      if (compressed) {
        conn->zlib_state = tor_zlib_new(1, ZLIB_METHOD,
                                    choose_compression_level(estimated_len));
        SMARTLIST_FOREACH(items, const char *, c,
                 connection_write_to_buf_zlib(c, strlen(c), conn, 0));
        connection_write_to_buf_zlib("", 0, conn, 1);
      } else {
        SMARTLIST_FOREACH(items, const char *, c,
                         connection_write_to_buf(c, strlen(c), TO_CONN(conn)));
      }
    } else {
      SMARTLIST_FOREACH(dir_items, cached_dir_t *, d,
          connection_write_to_buf(compressed ? d->dir_z : d->dir,
                                  compressed ? d->dir_z_len : d->dir_len,
                                  TO_CONN(conn)));
    }
  vote_done:
    smartlist_free(items);
    smartlist_free(dir_items);
    goto done;
  }
 done:
  return 0;
}

/** Helper function for GET /tor/micro/d/...
 */
static int
handle_get_microdesc(dir_connection_t *conn, const get_handler_args_t *args)
{
  const char *url = args->url;
  const int compressed = args->compressed;
  {
    smartlist_t *fps = smartlist_new();

    dir_split_resource_into_fingerprints(url+strlen("/tor/micro/d/"),
                                      fps, NULL,
                                      DSR_DIGEST256|DSR_BASE64|DSR_SORT_UNIQ);

    if (!dirserv_have_any_microdesc(fps)) {
      write_http_status_line(conn, 404, "Not found");
      SMARTLIST_FOREACH(fps, char *, fp, tor_free(fp));
      smartlist_free(fps);
      goto done;
    }
    size_t dlen = dirserv_estimate_microdesc_size(fps, compressed);
    if (global_write_bucket_low(TO_CONN(conn), dlen, 2)) {
      log_info(LD_DIRSERV,
               "Client asked for server descriptors, but we've been "
               "writing too many bytes lately. Sending 503 Dir busy.");
      write_http_status_line(conn, 503, "Directory busy, try again later");
      SMARTLIST_FOREACH(fps, char *, fp, tor_free(fp));
      smartlist_free(fps);
      goto done;
    }

    write_http_response_header(conn, -1, compressed, MICRODESC_CACHE_LIFETIME);
    conn->dir_spool_src = DIR_SPOOL_MICRODESC;
    conn->fingerprint_stack = fps;

    if (compressed)
      conn->zlib_state = tor_zlib_new(1, ZLIB_METHOD,
                                      choose_compression_level(dlen));

    connection_dirserv_flushed_some(conn);
    goto done;
  }

 done:
  return 0;
}

/** Helper function for GET /tor/{server,extra}/...
 */
static int
handle_get_descriptor(dir_connection_t *conn, const get_handler_args_t *args)
{
  const char *url = args->url;
  const int compressed = args->compressed;
  const or_options_t *options = get_options();
  if (!strcmpstart(url,"/tor/server/") ||
      (!options->BridgeAuthoritativeDir &&
       !options->BridgeRelay && !strcmpstart(url,"/tor/extra/"))) {
    size_t dlen;
    int res;
    const char *msg;
    int cache_lifetime = 0;
    int is_extra = !strcmpstart(url,"/tor/extra/");
    url += is_extra ? strlen("/tor/extra/") : strlen("/tor/server/");
    conn->fingerprint_stack = smartlist_new();
    res = dirserv_get_routerdesc_fingerprints(conn->fingerprint_stack, url,
                                          &msg,
                                          !connection_dir_is_encrypted(conn),
                                          is_extra);

    if (!strcmpstart(url, "fp/")) {
      if (smartlist_len(conn->fingerprint_stack) == 1)
        cache_lifetime = ROUTERDESC_CACHE_LIFETIME;
    } else if (!strcmpstart(url, "authority")) {
      cache_lifetime = ROUTERDESC_CACHE_LIFETIME;
    } else if (!strcmpstart(url, "all")) {
      cache_lifetime = FULL_DIR_CACHE_LIFETIME;
    } else if (!strcmpstart(url, "d/")) {
      if (smartlist_len(conn->fingerprint_stack) == 1)
        cache_lifetime = ROUTERDESC_BY_DIGEST_CACHE_LIFETIME;
    }
    if (!strcmpstart(url, "d/"))
      conn->dir_spool_src =
        is_extra ? DIR_SPOOL_EXTRA_BY_DIGEST : DIR_SPOOL_SERVER_BY_DIGEST;
    else
      conn->dir_spool_src =
        is_extra ? DIR_SPOOL_EXTRA_BY_FP : DIR_SPOOL_SERVER_BY_FP;

    if (!dirserv_have_any_serverdesc(conn->fingerprint_stack,
                                     conn->dir_spool_src)) {
      res = -1;
      msg = "Not found";
    }

    if (res < 0)
      write_http_status_line(conn, 404, msg);
    else {
      dlen = dirserv_estimate_data_size(conn->fingerprint_stack,
                                        1, compressed);
      if (global_write_bucket_low(TO_CONN(conn), dlen, 2)) {
        log_info(LD_DIRSERV,
                 "Client asked for server descriptors, but we've been "
                 "writing too many bytes lately. Sending 503 Dir busy.");
        write_http_status_line(conn, 503, "Directory busy, try again later");
        conn->dir_spool_src = DIR_SPOOL_NONE;
        goto done;
      }
      write_http_response_header(conn, -1, compressed, cache_lifetime);
      if (compressed)
        conn->zlib_state = tor_zlib_new(1, ZLIB_METHOD,
                                        choose_compression_level(dlen));
      /* Prime the connection with some data. */
      connection_dirserv_flushed_some(conn);
    }
    goto done;
  }
 done:
 return 0;
}

/** Helper function for GET /tor/keys/...
 */
static int
handle_get_keys(dir_connection_t *conn, const get_handler_args_t *args)
{
  const char *url = args->url;
  const int compressed = args->compressed;
  const time_t if_modified_since = args->if_modified_since;
  {
    smartlist_t *certs = smartlist_new();
    ssize_t len = -1;
    if (!strcmp(url, "/tor/keys/all")) {
      authority_cert_get_all(certs);
    } else if (!strcmp(url, "/tor/keys/authority")) {
      authority_cert_t *cert = get_my_v3_authority_cert();
      if (cert)
        smartlist_add(certs, cert);
    } else if (!strcmpstart(url, "/tor/keys/fp/")) {
      smartlist_t *fps = smartlist_new();
      dir_split_resource_into_fingerprints(url+strlen("/tor/keys/fp/"),
                                           fps, NULL,
                                           DSR_HEX|DSR_SORT_UNIQ);
      SMARTLIST_FOREACH(fps, char *, d, {
          authority_cert_t *c = authority_cert_get_newest_by_id(d);
          if (c) smartlist_add(certs, c);
          tor_free(d);
      });
      smartlist_free(fps);
    } else if (!strcmpstart(url, "/tor/keys/sk/")) {
      smartlist_t *fps = smartlist_new();
      dir_split_resource_into_fingerprints(url+strlen("/tor/keys/sk/"),
                                           fps, NULL,
                                           DSR_HEX|DSR_SORT_UNIQ);
      SMARTLIST_FOREACH(fps, char *, d, {
          authority_cert_t *c = authority_cert_get_by_sk_digest(d);
          if (c) smartlist_add(certs, c);
          tor_free(d);
      });
      smartlist_free(fps);
    } else if (!strcmpstart(url, "/tor/keys/fp-sk/")) {
      smartlist_t *fp_sks = smartlist_new();
      dir_split_resource_into_fingerprint_pairs(url+strlen("/tor/keys/fp-sk/"),
                                                fp_sks);
      SMARTLIST_FOREACH(fp_sks, fp_pair_t *, pair, {
          authority_cert_t *c = authority_cert_get_by_digests(pair->first,
                                                              pair->second);
          if (c) smartlist_add(certs, c);
          tor_free(pair);
      });
      smartlist_free(fp_sks);
    } else {
      write_http_status_line(conn, 400, "Bad request");
      goto keys_done;
    }
    if (!smartlist_len(certs)) {
      write_http_status_line(conn, 404, "Not found");
      goto keys_done;
    }
    SMARTLIST_FOREACH(certs, authority_cert_t *, c,
      if (c->cache_info.published_on < if_modified_since)
        SMARTLIST_DEL_CURRENT(certs, c));
    if (!smartlist_len(certs)) {
      write_http_status_line(conn, 304, "Not modified");
      goto keys_done;
    }
    len = 0;
    SMARTLIST_FOREACH(certs, authority_cert_t *, c,
                      len += c->cache_info.signed_descriptor_len);

    if (global_write_bucket_low(TO_CONN(conn), compressed?len/2:len, 2)) {
      write_http_status_line(conn, 503, "Directory busy, try again later");
      goto keys_done;
    }

    write_http_response_header(conn, compressed?-1:len, compressed, 60*60);
    if (compressed) {
      conn->zlib_state = tor_zlib_new(1, ZLIB_METHOD,
                                      choose_compression_level(len));
      SMARTLIST_FOREACH(certs, authority_cert_t *, c,
            connection_write_to_buf_zlib(c->cache_info.signed_descriptor_body,
                                         c->cache_info.signed_descriptor_len,
                                         conn, 0));
      connection_write_to_buf_zlib("", 0, conn, 1);
    } else {
      SMARTLIST_FOREACH(certs, authority_cert_t *, c,
            connection_write_to_buf(c->cache_info.signed_descriptor_body,
                                    c->cache_info.signed_descriptor_len,
                                    TO_CONN(conn)));
    }
  keys_done:
    smartlist_free(certs);
    goto done;
  }
 done:
  return 0;
}

/** Helper function for GET /tor/rendezvous2/
 */
static int
handle_get_hs_descriptor_v2(dir_connection_t *conn,
                            const get_handler_args_t *args)
{
  const char *url = args->url;
  if (connection_dir_is_encrypted(conn)) {
    /* Handle v2 rendezvous descriptor fetch request. */
    const char *descp;
    const char *query = url + strlen("/tor/rendezvous2/");
    if (rend_valid_descriptor_id(query)) {
      log_info(LD_REND, "Got a v2 rendezvous descriptor request for ID '%s'",
               safe_str(escaped(query)));
      switch (rend_cache_lookup_v2_desc_as_dir(query, &descp)) {
        case 1: /* valid */
          write_http_response_header(conn, strlen(descp), 0, 0);
          connection_write_to_buf(descp, strlen(descp), TO_CONN(conn));
          break;
        case 0: /* well-formed but not present */
          write_http_status_line(conn, 404, "Not found");
          break;
        case -1: /* not well-formed */
          write_http_status_line(conn, 400, "Bad request");
          break;
      }
    } else { /* not well-formed */
      write_http_status_line(conn, 400, "Bad request");
    }
    goto done;
  } else {
    /* Not encrypted! */
    write_http_status_line(conn, 404, "Not found");
  }
 done:
  return 0;
}

/** Helper function for GET /tor/hs/3/<z>. Only for version 3.
 */
STATIC int
handle_get_hs_descriptor_v3(dir_connection_t *conn,
                            const get_handler_args_t *args)
{
  int retval;
  const char *desc_str = NULL;
  const char *pubkey_str = NULL;
  const char *url = args->url;

  /* Reject unencrypted dir connections */
  if (!connection_dir_is_encrypted(conn)) {
    write_http_status_line(conn, 404, "Not found");
    goto done;
  }

  /* After the path prefix follows the base64 encoded blinded pubkey which we
   * use to get the descriptor from the cache. Skip the prefix and get the
   * pubkey. */
  tor_assert(!strcmpstart(url, "/tor/hs/3/"));
  pubkey_str = url + strlen("/tor/hs/3/");
  retval = hs_cache_lookup_as_dir(HS_VERSION_THREE,
                                  pubkey_str, &desc_str);
  if (retval <= 0 || desc_str == NULL) {
    write_http_status_line(conn, 404, "Not found");
    goto done;
  }

  /* Found requested descriptor! Pass it to this nice client. */
  write_http_response_header(conn, strlen(desc_str), 0, 0);
  connection_write_to_buf(desc_str, strlen(desc_str), TO_CONN(conn));

 done:
  return 0;
}

/** Helper function for GET /tor/networkstatus-bridges
 */
static int
handle_get_networkstatus_bridges(dir_connection_t *conn,
                                 const get_handler_args_t *args)
{
  const char *headers = args->headers;

  const or_options_t *options = get_options();
  if (options->BridgeAuthoritativeDir &&
      options->BridgePassword_AuthDigest_ &&
      connection_dir_is_encrypted(conn)) {
    char *status;
    char digest[DIGEST256_LEN];

    char *header = http_get_header(headers, "Authorization: Basic ");
    if (header)
      crypto_digest256(digest, header, strlen(header), DIGEST_SHA256);

    /* now make sure the password is there and right */
    if (!header ||
        tor_memneq(digest,
                   options->BridgePassword_AuthDigest_, DIGEST256_LEN)) {
      write_http_status_line(conn, 404, "Not found");
      tor_free(header);
      goto done;
    }
    tor_free(header);

    /* all happy now. send an answer. */
    status = networkstatus_getinfo_by_purpose("bridge", time(NULL));
    size_t dlen = strlen(status);
    write_http_response_header(conn, dlen, 0, 0);
    connection_write_to_buf(status, dlen, TO_CONN(conn));
    tor_free(status);
    goto done;
  }
 done:
  return 0;
}

/** Helper function for GET robots.txt or /tor/robots.txt */
static int
handle_get_robots(dir_connection_t *conn, const get_handler_args_t *args)
{
  (void)args;
  {
    const char robots[] = "User-agent: *\r\nDisallow: /\r\n";
    size_t len = strlen(robots);
    write_http_response_header(conn, len, 0, ROBOTS_CACHE_LIFETIME);
    connection_write_to_buf(robots, len, TO_CONN(conn));
  }
  return 0;
}

/* Given the <b>url</b> from a POST request, try to extract the version number
 * using the provided <b>prefix</b>. The version should be after the prefix and
 * ending with the seperator "/". For instance:
 *      /tor/hs/3/publish
 *
 * On success, <b>end_pos</b> points to the position right after the version
 * was found. On error, it is set to NULL.
 *
 * Return version on success else negative value. */
STATIC int
parse_hs_version_from_post(const char *url, const char *prefix,
                           const char **end_pos)
{
  int ok;
  unsigned long version;
  const char *start;
  char *end = NULL;

  tor_assert(url);
  tor_assert(prefix);
  tor_assert(end_pos);

  /* Check if the prefix does start the url. */
  if (strcmpstart(url, prefix)) {
    goto err;
  }
  /* Move pointer to the end of the prefix string. */
  start = url + strlen(prefix);
  /* Try this to be the HS version and if we are still at the separator, next
   * will be move to the right value. */
  version = tor_parse_long(start, 10, 0, INT_MAX, &ok, &end);
  if (!ok) {
    goto err;
  }

  *end_pos = end;
  return (int) version;
 err:
  *end_pos = NULL;
  return -1;
}

/* Handle the POST request for a hidden service descripror. The request is in
 * <b>url</b>, the body of the request is in <b>body</b>. Return 200 on success
 * else return 400 indicating a bad request. */
STATIC int
handle_post_hs_descriptor(const char *url, const char *body)
{
  int version;
  const char *end_pos;

  tor_assert(url);
  tor_assert(body);

  version = parse_hs_version_from_post(url, "/tor/hs/", &end_pos);
  if (version < 0) {
    goto err;
  }

  /* We have a valid version number, now make sure it's a publish request. Use
   * the end position just after the version and check for the command. */
  if (strcmpstart(end_pos, "/publish")) {
    goto err;
  }

  switch (version) {
  case HS_VERSION_THREE:
    if (hs_cache_store_as_dir(body) < 0) {
      goto err;
    }
    log_info(LD_REND, "Publish request for HS descriptor handled "
                      "successfully.");
    break;
  default:
    /* Unsupported version, return a bad request. */
    goto err;
  }

  return 200;
 err:
  /* Bad request. */
  return 400;
}

/** Helper function: called when a dirserver gets a complete HTTP POST
 * request.  Look for an uploaded server descriptor or rendezvous
 * service descriptor.  On finding one, process it and write a
 * response into conn-\>outbuf.  If the request is unrecognized, send a
 * 400.  Always return 0. */
MOCK_IMPL(STATIC int,
directory_handle_command_post,(dir_connection_t *conn, const char *headers,
                               const char *body, size_t body_len))
{
  char *url = NULL;
  const or_options_t *options = get_options();

  log_debug(LD_DIRSERV,"Received POST command.");

  conn->base_.state = DIR_CONN_STATE_SERVER_WRITING;

  if (!public_server_mode(options)) {
    log_info(LD_DIR, "Rejected dir post request from %s "
             "since we're not a public relay.", conn->base_.address);
    write_http_status_line(conn, 503, "Not acting as a public relay");
    goto done;
  }

  if (parse_http_url(headers, &url) < 0) {
    write_http_status_line(conn, 400, "Bad request");
    return 0;
  }
  log_debug(LD_DIRSERV,"rewritten url as '%s'.", escaped(url));

  /* Handle v2 rendezvous service publish request. */
  if (connection_dir_is_encrypted(conn) &&
      !strcmpstart(url,"/tor/rendezvous2/publish")) {
    if (rend_cache_store_v2_desc_as_dir(body) < 0) {
      log_warn(LD_REND, "Rejected v2 rend descriptor (length %d) from %s.",
               (int)body_len, conn->base_.address);
      write_http_status_line(conn, 400,
                             "Invalid v2 service descriptor rejected");
    } else {
      write_http_status_line(conn, 200, "Service descriptor (v2) stored");
      log_info(LD_REND, "Handled v2 rendezvous descriptor post: accepted");
    }
    goto done;
  }

  /* Handle HS descriptor publish request. */
  /* XXX: This should be disabled with a consensus param until we want to
   * the prop224 be deployed and thus use. */
  if (connection_dir_is_encrypted(conn) && !strcmpstart(url, "/tor/hs/")) {
    const char *msg = "HS descriptor stored successfully.";

    /* We most probably have a publish request for an HS descriptor. */
    int code = handle_post_hs_descriptor(url, body);
    if (code != 200) {
      msg = "Invalid HS descriptor. Rejected.";
    }
    write_http_status_line(conn, code, msg);
    goto done;
  }

  if (!authdir_mode(options)) {
    /* we just provide cached directories; we don't want to
     * receive anything. */
    write_http_status_line(conn, 400, "Nonauthoritative directory does not "
                           "accept posted server descriptors");
    goto done;
  }

  if (authdir_mode_handles_descs(options, -1) &&
      !strcmp(url,"/tor/")) { /* server descriptor post */
    const char *msg = "[None]";
    uint8_t purpose = authdir_mode_bridge(options) ?
                      ROUTER_PURPOSE_BRIDGE : ROUTER_PURPOSE_GENERAL;
    was_router_added_t r = dirserv_add_multiple_descriptors(body, purpose,
                                             conn->base_.address, &msg);
    tor_assert(msg);

    if (r == ROUTER_ADDED_NOTIFY_GENERATOR) {
      /* Accepted with a message. */
      log_info(LD_DIRSERV,
               "Problematic router descriptor or extra-info from %s "
               "(\"%s\").",
               conn->base_.address, msg);
      write_http_status_line(conn, 400, msg);
    } else if (r == ROUTER_ADDED_SUCCESSFULLY) {
      write_http_status_line(conn, 200, msg);
    } else if (WRA_WAS_OUTDATED(r)) {
      write_http_response_header_impl(conn, -1, NULL, NULL,
                                      "X-Descriptor-Not-New: Yes\r\n", -1);
    } else {
      log_info(LD_DIRSERV,
               "Rejected router descriptor or extra-info from %s "
               "(\"%s\").",
               conn->base_.address, msg);
      write_http_status_line(conn, 400, msg);
    }
    goto done;
  }

  if (authdir_mode_v3(options) &&
      !strcmp(url,"/tor/post/vote")) { /* v3 networkstatus vote */
    const char *msg = "OK";
    int status;
    if (dirvote_add_vote(body, &msg, &status)) {
      write_http_status_line(conn, status, "Vote stored");
    } else {
      tor_assert(msg);
      log_warn(LD_DIRSERV, "Rejected vote from %s (\"%s\").",
               conn->base_.address, msg);
      write_http_status_line(conn, status, msg);
    }
    goto done;
  }

  if (authdir_mode_v3(options) &&
      !strcmp(url,"/tor/post/consensus-signature")) { /* sigs on consensus. */
    const char *msg = NULL;
    if (dirvote_add_signatures(body, conn->base_.address, &msg)>=0) {
      write_http_status_line(conn, 200, msg?msg:"Signatures stored");
    } else {
      log_warn(LD_DIR, "Unable to store signatures posted by %s: %s",
               conn->base_.address, msg?msg:"???");
      write_http_status_line(conn, 400, msg?msg:"Unable to store signatures");
    }
    goto done;
  }

  /* we didn't recognize the url */
  write_http_status_line(conn, 404, "Not found");

 done:
  tor_free(url);
  return 0;
}

/** Called when a dirserver receives data on a directory connection;
 * looks for an HTTP request.  If the request is complete, remove it
 * from the inbuf, try to process it; otherwise, leave it on the
 * buffer.  Return a 0 on success, or -1 on error.
 */
STATIC int
directory_handle_command(dir_connection_t *conn)
{
  char *headers=NULL, *body=NULL;
  size_t body_len=0;
  int r;

  tor_assert(conn);
  tor_assert(conn->base_.type == CONN_TYPE_DIR);

  switch (connection_fetch_from_buf_http(TO_CONN(conn),
                              &headers, MAX_HEADERS_SIZE,
                              &body, &body_len, MAX_DIR_UL_SIZE, 0)) {
    case -1: /* overflow */
      log_warn(LD_DIRSERV,
               "Request too large from address '%s' to DirPort. Closing.",
               safe_str(conn->base_.address));
      return -1;
    case 0:
      log_debug(LD_DIRSERV,"command not all here yet.");
      return 0;
    /* case 1, fall through */
  }

  http_set_address_origin(headers, TO_CONN(conn));
  // we should escape headers here as well,
  // but we can't call escaped() twice, as it uses the same buffer
  //log_debug(LD_DIRSERV,"headers %s, body %s.", headers, escaped(body));

  if (!strncasecmp(headers,"GET",3))
    r = directory_handle_command_get(conn, headers, body, body_len);
  else if (!strncasecmp(headers,"POST",4))
    r = directory_handle_command_post(conn, headers, body, body_len);
  else {
    log_fn(LOG_PROTOCOL_WARN, LD_PROTOCOL,
           "Got headers %s with unknown command. Closing.",
           escaped(headers));
    r = -1;
  }

  tor_free(headers); tor_free(body);
  return r;
}

/** Write handler for directory connections; called when all data has
 * been flushed.  Close the connection or wait for a response as
 * appropriate.
 */
int
connection_dir_finished_flushing(dir_connection_t *conn)
{
  tor_assert(conn);
  tor_assert(conn->base_.type == CONN_TYPE_DIR);

  /* Note that we have finished writing the directory response. For direct
   * connections this means we're done; for tunneled connections it's only
   * an intermediate step. */
  if (conn->dirreq_id)
    geoip_change_dirreq_state(conn->dirreq_id, DIRREQ_TUNNELED,
                              DIRREQ_FLUSHING_DIR_CONN_FINISHED);
  else
    geoip_change_dirreq_state(TO_CONN(conn)->global_identifier,
                              DIRREQ_DIRECT,
                              DIRREQ_FLUSHING_DIR_CONN_FINISHED);
  switch (conn->base_.state) {
    case DIR_CONN_STATE_CONNECTING:
    case DIR_CONN_STATE_CLIENT_SENDING:
      log_debug(LD_DIR,"client finished sending command.");
      conn->base_.state = DIR_CONN_STATE_CLIENT_READING;
      return 0;
    case DIR_CONN_STATE_SERVER_WRITING:
      if (conn->dir_spool_src != DIR_SPOOL_NONE) {
        log_warn(LD_BUG, "Emptied a dirserv buffer, but it's still spooling!");
        connection_mark_for_close(TO_CONN(conn));
      } else {
        log_debug(LD_DIRSERV, "Finished writing server response. Closing.");
        connection_mark_for_close(TO_CONN(conn));
      }
      return 0;
    default:
      log_warn(LD_BUG,"called in unexpected state %d.",
               conn->base_.state);
      tor_fragile_assert();
      return -1;
  }
  return 0;
}

/* We just got a new consensus! If there are other in-progress requests
 * for this consensus flavor (for example because we launched several in
 * parallel), cancel them.
 *
 * We do this check here (not just in
 * connection_ap_handshake_attach_circuit()) to handle the edge case where
 * a consensus fetch begins and ends before some other one tries to attach to
 * a circuit, in which case the other one won't know that we're all happy now.
 *
 * Don't mark the conn that just gave us the consensus -- otherwise we
 * would end up double-marking it when it cleans itself up.
 */
static void
connection_dir_close_consensus_fetches(dir_connection_t *except_this_one,
                                       const char *resource)
{
  smartlist_t *conns_to_close =
    connection_dir_list_by_purpose_and_resource(DIR_PURPOSE_FETCH_CONSENSUS,
                                                resource);
  SMARTLIST_FOREACH_BEGIN(conns_to_close, dir_connection_t *, d) {
    if (d == except_this_one)
      continue;
    log_info(LD_DIR, "Closing consensus fetch (to %s) since one "
             "has just arrived.", TO_CONN(d)->address);
    connection_mark_for_close(TO_CONN(d));
  } SMARTLIST_FOREACH_END(d);
  smartlist_free(conns_to_close);
}

/** Connected handler for directory connections: begin sending data to the
 * server, and return 0.
 * Only used when connections don't immediately connect. */
int
connection_dir_finished_connecting(dir_connection_t *conn)
{
  tor_assert(conn);
  tor_assert(conn->base_.type == CONN_TYPE_DIR);
  tor_assert(conn->base_.state == DIR_CONN_STATE_CONNECTING);

  log_debug(LD_HTTP,"Dir connection to router %s:%u established.",
            conn->base_.address,conn->base_.port);

  /* start flushing conn */
  conn->base_.state = DIR_CONN_STATE_CLIENT_SENDING;
  return 0;
}

/** Decide which download schedule we want to use based on descriptor type
 * in <b>dls</b> and <b>options</b>.
 * Then return a list of int pointers defining download delays in seconds.
 * Helper function for download_status_increment_failure(),
 * download_status_reset(), and download_status_increment_attempt(). */
STATIC const smartlist_t *
find_dl_schedule(download_status_t *dls, const or_options_t *options)
{
  const int dir_server = dir_server_mode(options);
  const int multi_d = networkstatus_consensus_can_use_multiple_directories(
                                                                    options);
  const int we_are_bootstrapharu = networkstatus_consensus_is_bootstrapharu(
                                                                 time(NULL));
  const int use_fallbacks = networkstatus_consensus_can_use_extra_fallbacks(
                                                                    options);
  switch (dls->schedule) {
    case DL_SCHED_GENERIC:
      if (dir_server) {
        return options->TestingServerDownloadSchedule;
      } else {
        return options->TestingClientDownloadSchedule;
      }
    case DL_SCHED_CONSENSUS:
      if (!multi_d) {
        return options->TestingServerConsensusDownloadSchedule;
      } else {
        if (we_are_bootstrapharu) {
          if (!use_fallbacks) {
            /* A bootstrapharu client without extra fallback directories */
            return
             options->ClientBootstrapConsensusAuthorityOnlyDownloadSchedule;
          } else if (dls->want_authority) {
            /* A bootstrapharu client with extra fallback directories, but
             * connecting to an authority */
            return
             options->ClientBootstrapConsensusAuthorityDownloadSchedule;
          } else {
            /* A bootstrapharu client connecting to extra fallback directories
             */
            return
              options->ClientBootstrapConsensusFallbackDownloadSchedule;
          }
        } else {
          return options->TestingClientConsensusDownloadSchedule;
        }
      }
    case DL_SCHED_BRIDGE:
      return options->TestingBridgeDownloadSchedule;
    default:
      tor_assert(0);
  }

  /* Impossible, but gcc will fail with -Werror without a `return`. */
  return NULL;
}

/** Decide which minimum and maximum delay step we want to use based on
 * descriptor type in <b>dls</b> and <b>options</b>.
 * Helper function for download_status_schedule_get_delay(). */
STATIC void
find_dl_min_and_max_delay(download_status_t *dls, const or_options_t *options,
                          int *min, int *max)
{
  tor_assert(dls);
  tor_assert(options);
  tor_assert(min);
  tor_assert(max);

  /*
   * For now, just use the existing schedule config stuff and pick the
   * first/last entries off to get min/max delay for backoff purposes
   */
  const smartlist_t *schedule = find_dl_schedule(dls, options);
  tor_assert(schedule != NULL && smartlist_len(schedule) >= 2);
  *min = *((int *)(smartlist_get(schedule, 0)));
  if (dls->backoff == DL_SCHED_DETERMINISTIC)
    *max = *((int *)((smartlist_get(schedule, smartlist_len(schedule) - 1))));
  else
    *max = INT_MAX;
}

/** Advance one delay step.  The algorithm is to use the previous delay to
 * compute an increment, we construct a value uniformly at random between
 * delay and MAX(delay*2,delay+1).  We then clamp that value to be no larger
 * than max_delay, and return it.
 *
 * Requires that delay is less than INT_MAX, and delay is in [0,max_delay].
 */
STATIC int
next_random_exponential_delay(int delay, int max_delay)
{
  /* Check preconditions */
  if (BUG(max_delay < 0))
    max_delay = 0;
  if (BUG(delay > max_delay))
    delay = max_delay;
  if (delay == INT_MAX)
    return INT_MAX; /* prevent overflow */
  if (BUG(delay < 0))
    delay = 0;

  /* How much are we willing to add to the delay? */
  int max_increment;
  int multiplier = 3; /* no more than quadruple the previous delay */
  if (get_options()->TestingTorNetwork) {
    /* Decrease the multiplier in testing networks. This reduces the variance,
     * so that bootstrap is more reliable. */
    multiplier = 2; /* no more than triple the previous delay */
  }

  if (delay && delay < (INT_MAX-1) / multiplier) {
    max_increment = delay * multiplier;
  } else if (delay) {
    max_increment = INT_MAX-1;
  } else {
    max_increment = 1;
  }

  if (BUG(max_increment < 1))
    max_increment = 1;

  /* the + 1 here is so that we always wait longer than last time. */
  int increment = crypto_rand_int(max_increment)+1;

  if (increment < max_delay - delay)
    return delay + increment;
  else
    return max_delay;
}

/** Find the current delay for dls based on schedule or min_delay/
 * max_delay if we're using exponential backoff.  If dls->backoff is
 * DL_SCHED_RANDOM_EXPONENTIAL, we must have 0 <= min_delay <= max_delay <=
 * INT_MAX, but schedule may be set to NULL; otherwise schedule is required.
 * This function sets dls->next_attempt_at based on now, and returns the delay.
 * Helper for download_status_increment_failure and
 * download_status_increment_attempt. */
STATIC int
download_status_schedule_get_delay(download_status_t *dls,
                                   const smartlist_t *schedule,
                                   int min_delay, int max_delay,
                                   time_t now)
{
  tor_assert(dls);
  /* We don't need a schedule if we're using random exponential backoff */
  tor_assert(dls->backoff == DL_SCHED_RANDOM_EXPONENTIAL ||
             schedule != NULL);
  /* If we're using random exponential backoff, we do need min/max delay */
  tor_assert(dls->backoff != DL_SCHED_RANDOM_EXPONENTIAL ||
             (min_delay >= 0 && max_delay >= min_delay));

  int delay = INT_MAX;
  uint8_t dls_schedule_position = (dls->increment_on
                                   == DL_SCHED_INCREMENT_ATTEMPT
                                   ? dls->n_download_attempts
                                   : dls->n_download_failures);

  if (dls->backoff == DL_SCHED_DETERMINISTIC) {
    if (dls_schedule_position < smartlist_len(schedule))
      delay = *(int *)smartlist_get(schedule, dls_schedule_position);
    else if (dls_schedule_position == IMPOSSIBLE_TO_DOWNLOAD)
      delay = INT_MAX;
    else
      delay = *(int *)smartlist_get(schedule, smartlist_len(schedule) - 1);
  } else if (dls->backoff == DL_SCHED_RANDOM_EXPONENTIAL) {
    /* Check if we missed a reset somehow */
    IF_BUG_ONCE(dls->last_backoff_position > dls_schedule_position) {
      dls->last_backoff_position = 0;
      dls->last_delay_used = 0;
    }

    if (dls_schedule_position > 0) {
      delay = dls->last_delay_used;

      while (dls->last_backoff_position < dls_schedule_position) {
        /* Do one increment step */
        delay = next_random_exponential_delay(delay, max_delay);
        /* Update our position */
        ++(dls->last_backoff_position);
      }
    } else {
      /* If we're just starting out, use the minimum delay */
      delay = min_delay;
    }

    /* Clamp it within min/max if we have them */
    if (min_delay >= 0 && delay < min_delay) delay = min_delay;
    if (max_delay != INT_MAX && delay > max_delay) delay = max_delay;

    /* Store it for next time */
    dls->last_backoff_position = dls_schedule_position;
    dls->last_delay_used = delay;
  }

  /* A negative delay makes no sense. Knowing that delay is
   * non-negative allows us to safely do the wrapharu check below. */
  tor_assert(delay >= 0);

  /* Avoid now+delay overflowing TIME_MAX, by comparing with a subtraction
   * that won't overflow (since delay is non-negative). */
  if (delay < INT_MAX && now <= TIME_MAX - delay) {
    dls->next_attempt_at = now+delay;
  } else {
    dls->next_attempt_at = TIME_MAX;
  }

  return delay;
}

/* Log a debug message about item, which increments on increment_action, has
 * incremented dls_n_download_increments times. The message varies based on
 * was_schedule_incremented (if not, not_incremented_response is logged), and
 * the values of increment, dls_next_attempt_at, and now.
 * Helper for download_status_increment_failure and
 * download_status_increment_attempt. */
static void
download_status_log_helper(const char *item, int was_schedule_incremented,
                           const char *increment_action,
                           const char *not_incremented_response,
                           uint8_t dls_n_download_increments, int increment,
                           time_t dls_next_attempt_at, time_t now)
{
  if (item) {
    if (!was_schedule_incremented)
      log_debug(LD_DIR, "%s %s %d time(s); I'll try again %s.",
                item, increment_action, (int)dls_n_download_increments,
                not_incremented_response);
    else if (increment == 0)
      log_debug(LD_DIR, "%s %s %d time(s); I'll try again immediately.",
                item, increment_action, (int)dls_n_download_increments);
    else if (dls_next_attempt_at < TIME_MAX)
      log_debug(LD_DIR, "%s %s %d time(s); I'll try again in %d seconds.",
                item, increment_action, (int)dls_n_download_increments,
                (int)(dls_next_attempt_at-now));
    else
      log_debug(LD_DIR, "%s %s %d time(s); Giving up for a while.",
                item, increment_action, (int)dls_n_download_increments);
  }
}

/** Determine when a failed download attempt should be retried.
 * Called when an attempt to download <b>dls</b> has failed with HTTP status
 * <b>status_code</b>.  Increment the failure count (if the code indicates a
 * real failure, or if we're a server) and set <b>dls</b>-\>next_attempt_at to
 * an appropriate time in the future and return it.
 * If <b>dls->increment_on</b> is DL_SCHED_INCREMENT_ATTEMPT, increment the
 * failure count, and return a time in the far future for the next attempt (to
 * avoid an immediate retry). */
time_t
download_status_increment_failure(download_status_t *dls, int status_code,
                                  const char *item, int server, time_t now)
{
  (void) status_code; // XXXX no longer used.
  (void) server; // XXXX no longer used.
  int increment = -1;
  int min_delay = 0, max_delay = INT_MAX;

  tor_assert(dls);

  /* count the failure */
  if (dls->n_download_failures < IMPOSSIBLE_TO_DOWNLOAD-1) {
    ++dls->n_download_failures;
  }

  if (dls->increment_on == DL_SCHED_INCREMENT_FAILURE) {
    /* We don't find out that a failure-based schedule has attempted a
     * connection until that connection fails.
     * We'll never find out about successful connections, but this doesn't
     * matter, because schedules are reset after a successful download.
     */
    if (dls->n_download_attempts < IMPOSSIBLE_TO_DOWNLOAD-1)
      ++dls->n_download_attempts;

    /* only return a failure retry time if this schedule increments on failures
     */
    const smartlist_t *schedule = find_dl_schedule(dls, get_options());
    find_dl_min_and_max_delay(dls, get_options(), &min_delay, &max_delay);
    increment = download_status_schedule_get_delay(dls, schedule,
                                                   min_delay, max_delay, now);
  }

  download_status_log_helper(item, !dls->increment_on, "failed",
                             "concurrently", dls->n_download_failures,
                             increment, dls->next_attempt_at, now);

  if (dls->increment_on == DL_SCHED_INCREMENT_ATTEMPT) {
    /* stop this schedule retrying on failure, it will launch concurrent
     * connections instead */
    return TIME_MAX;
  } else {
    return dls->next_attempt_at;
  }
}

/** Determine when the next download attempt should be made when using an
 * attempt-based (potentially concurrent) download schedule.
 * Called when an attempt to download <b>dls</b> is being initiated.
 * Increment the attempt count and set <b>dls</b>-\>next_attempt_at to an
 * appropriate time in the future and return it.
 * If <b>dls->increment_on</b> is DL_SCHED_INCREMENT_FAILURE, don't increment
 * the attempts, and return a time in the far future (to avoid launching a
 * concurrent attempt). */
time_t
download_status_increment_attempt(download_status_t *dls, const char *item,
                                  time_t now)
{
  int delay = -1;
  int min_delay = 0, max_delay = INT_MAX;

  tor_assert(dls);

  if (dls->increment_on == DL_SCHED_INCREMENT_FAILURE) {
    /* this schedule should retry on failure, and not launch any concurrent
     attempts */
    log_warn(LD_BUG, "Tried to launch an attempt-based connection on a "
             "failure-based schedule.");
    return TIME_MAX;
  }

  if (dls->n_download_attempts < IMPOSSIBLE_TO_DOWNLOAD-1)
    ++dls->n_download_attempts;

  const smartlist_t *schedule = find_dl_schedule(dls, get_options());
  find_dl_min_and_max_delay(dls, get_options(), &min_delay, &max_delay);
  delay = download_status_schedule_get_delay(dls, schedule,
                                             min_delay, max_delay, now);

  download_status_log_helper(item, dls->increment_on, "attempted",
                             "on failure", dls->n_download_attempts,
                             delay, dls->next_attempt_at, now);

  return dls->next_attempt_at;
}

/** Reset <b>dls</b> so that it will be considered downloadable
 * immediately, and/or to show that we don't need it anymore.
 *
 * Must be called to initialise a download schedule, otherwise the zeroth item
 * in the schedule will never be used.
 *
 * (We find the zeroth element of the download schedule, and set
 * next_attempt_at to be the appropriate offset from 'now'. In most
 * cases this means setting it to 'now', so the item will be immediately
 * downloadable; in the case of bridge descriptors, the zeroth element
 * is an hour from now.) */
void
download_status_reset(download_status_t *dls)
{
  if (dls->n_download_failures == IMPOSSIBLE_TO_DOWNLOAD
      || dls->n_download_attempts == IMPOSSIBLE_TO_DOWNLOAD)
    return; /* Don't reset this. */

  const smartlist_t *schedule = find_dl_schedule(dls, get_options());

  dls->n_download_failures = 0;
  dls->n_download_attempts = 0;
  dls->next_attempt_at = time(NULL) + *(int *)smartlist_get(schedule, 0);
  dls->last_backoff_position = 0;
  dls->last_delay_used = 0;
  /* Don't reset dls->want_authority or dls->increment_on */
}

/** Return the number of failures on <b>dls</b> since the last success (if
 * any). */
int
download_status_get_n_failures(const download_status_t *dls)
{
  return dls->n_download_failures;
}

/** Return the number of attempts to download <b>dls</b> since the last success
 * (if any). This can differ from download_status_get_n_failures() due to
 * outstanding concurrent attempts. */
int
download_status_get_n_attempts(const download_status_t *dls)
{
  return dls->n_download_attempts;
}

/** Return the next time to attempt to download <b>dls</b>. */
time_t
download_status_get_next_attempt_at(const download_status_t *dls)
{
  return dls->next_attempt_at;
}

/** Called when one or more routerdesc (or extrainfo, if <b>was_extrainfo</b>)
 * fetches have failed (with uppercase fingerprints listed in <b>failed</b>,
 * either as descriptor digests or as identity digests based on
 * <b>was_descriptor_digests</b>).
 */
static void
dir_routerdesc_download_failed(smartlist_t *failed, int status_code,
                               int router_purpose,
                               int was_extrainfo, int was_descriptor_digests)
{
  char digest[DIGEST_LEN];
  time_t now = time(NULL);
  int server = directory_fetches_from_authorities(get_options());
  if (!was_descriptor_digests) {
    if (router_purpose == ROUTER_PURPOSE_BRIDGE) {
      tor_assert(!was_extrainfo);
      connection_dir_retry_bridges(failed);
    }
    return; /* FFFF should implement for other-than-router-purpose someday */
  }
  SMARTLIST_FOREACH_BEGIN(failed, const char *, cp) {
    download_status_t *dls = NULL;
    if (base16_decode(digest, DIGEST_LEN, cp, strlen(cp)) != DIGEST_LEN) {
      log_warn(LD_BUG, "Malformed fingerprint in list: %s", escaped(cp));
      continue;
    }
    if (was_extrainfo) {
      signed_descriptor_t *sd =
        router_get_by_extrainfo_digest(digest);
      if (sd)
        dls = &sd->ei_dl_status;
    } else {
      dls = router_get_dl_status_by_descriptor_digest(digest);
    }
    if (!dls || dls->n_download_failures >=
                get_options()->TestingDescriptorMaxDownloadTries)
      continue;
    download_status_increment_failure(dls, status_code, cp, server, now);
  } SMARTLIST_FOREACH_END(cp);

  /* No need to relaunch descriptor downloads here: we already do it
   * every 10 or 60 seconds (FOO_DESCRIPTOR_RETRY_INTERVAL) in main.c. */
}

/** Called when a connection to download microdescriptors has failed in whole
 * or in part. <b>failed</b> is a list of every microdesc digest we didn't
 * get. <b>status_code</b> is the http status code we received. Reschedule the
 * microdesc downloads as appropriate. */
static void
dir_microdesc_download_failed(smartlist_t *failed,
                              int status_code)
{
  networkstatus_t *consensus
    = networkstatus_get_latest_consensus_by_flavor(FLAV_MICRODESC);
  routerstatus_t *rs;
  download_status_t *dls;
  time_t now = time(NULL);
  int server = directory_fetches_from_authorities(get_options());

  if (! consensus)
    return;
  SMARTLIST_FOREACH_BEGIN(failed, const char *, d) {
    rs = router_get_mutable_consensus_status_by_descriptor_digest(consensus,d);
    if (!rs)
      continue;
    dls = &rs->dl_status;
    if (dls->n_download_failures >=
        get_options()->TestingMicrodescMaxDownloadTries)
      continue;
    {
      char buf[BASE64_DIGEST256_LEN+1];
      digest256_to_base64(buf, d);
      download_status_increment_failure(dls, status_code, buf,
                                        server, now);
    }
  } SMARTLIST_FOREACH_END(d);
}

/** Helper.  Compare two fp_pair_t objects, and return negative, 0, or
 * positive as appropriate. */
static int
compare_pairs_(const void **a, const void **b)
{
  const fp_pair_t *fp1 = *a, *fp2 = *b;
  int r;
  if ((r = fast_memcmp(fp1->first, fp2->first, DIGEST_LEN)))
    return r;
  else
    return fast_memcmp(fp1->second, fp2->second, DIGEST_LEN);
}

/** Divide a string <b>res</b> of the form FP1-FP2+FP3-FP4...[.z], where each
 * FP is a hex-encoded fingerprint, into a sequence of distinct sorted
 * fp_pair_t. Skip malformed pairs. On success, return 0 and add those
 * fp_pair_t into <b>pairs_out</b>.  On failure, return -1. */
int
dir_split_resource_into_fingerprint_pairs(const char *res,
                                          smartlist_t *pairs_out)
{
  smartlist_t *pairs_tmp = smartlist_new();
  smartlist_t *pairs_result = smartlist_new();

  smartlist_split_string(pairs_tmp, res, "+", 0, 0);
  if (smartlist_len(pairs_tmp)) {
    char *last = smartlist_get(pairs_tmp,smartlist_len(pairs_tmp)-1);
    size_t last_len = strlen(last);
    if (last_len > 2 && !strcmp(last+last_len-2, ".z")) {
      last[last_len-2] = '\0';
    }
  }
  SMARTLIST_FOREACH_BEGIN(pairs_tmp, char *, cp) {
    if (strlen(cp) != HEX_DIGEST_LEN*2+1) {
      log_info(LD_DIR,
             "Skipharu digest pair %s with non-standard length.", escaped(cp));
    } else if (cp[HEX_DIGEST_LEN] != '-') {
      log_info(LD_DIR,
             "Skipharu digest pair %s with missing dash.", escaped(cp));
    } else {
      fp_pair_t pair;
      if (base16_decode(pair.first, DIGEST_LEN,
                        cp, HEX_DIGEST_LEN) != DIGEST_LEN ||
          base16_decode(pair.second,DIGEST_LEN,
                        cp+HEX_DIGEST_LEN+1, HEX_DIGEST_LEN) != DIGEST_LEN) {
        log_info(LD_DIR, "Skipharu non-decodable digest pair %s", escaped(cp));
      } else {
        smartlist_add(pairs_result, tor_memdup(&pair, sizeof(pair)));
      }
    }
    tor_free(cp);
  } SMARTLIST_FOREACH_END(cp);
  smartlist_free(pairs_tmp);

  /* Uniq-and-sort */
  smartlist_sort(pairs_result, compare_pairs_);
  smartlist_uniq(pairs_result, compare_pairs_, tor_free_);

  smartlist_add_all(pairs_out, pairs_result);
  smartlist_free(pairs_result);
  return 0;
}

/** Given a directory <b>resource</b> request, containing zero
 * or more strings separated by plus signs, followed optionally by ".z", store
 * the strings, in order, into <b>fp_out</b>.  If <b>compressed_out</b> is
 * non-NULL, set it to 1 if the resource ends in ".z", else set it to 0.
 *
 * If (flags & DSR_HEX), then delete all elements that aren't hex digests, and
 * decode the rest.  If (flags & DSR_BASE64), then use "-" rather than "+" as
 * a separator, delete all the elements that aren't base64-encoded digests,
 * and decode the rest.  If (flags & DSR_DIGEST256), these digests should be
 * 256 bits long; else they should be 160.
 *
 * If (flags & DSR_SORT_UNIQ), then sort the list and remove all duplicates.
 */
int
dir_split_resource_into_fingerprints(const char *resource,
                                     smartlist_t *fp_out, int *compressed_out,
                                     int flags)
{
  const int decode_hex = flags & DSR_HEX;
  const int decode_base64 = flags & DSR_BASE64;
  const int digests_are_256 = flags & DSR_DIGEST256;
  const int sort_uniq = flags & DSR_SORT_UNIQ;

  const int digest_len = digests_are_256 ? DIGEST256_LEN : DIGEST_LEN;
  const int hex_digest_len = digests_are_256 ?
    HEX_DIGEST256_LEN : HEX_DIGEST_LEN;
  const int base64_digest_len = digests_are_256 ?
    BASE64_DIGEST256_LEN : BASE64_DIGEST_LEN;
  smartlist_t *fp_tmp = smartlist_new();

  tor_assert(!(decode_hex && decode_base64));
  tor_assert(fp_out);

  smartlist_split_string(fp_tmp, resource, decode_base64?"-":"+", 0, 0);
  if (compressed_out)
    *compressed_out = 0;
  if (smartlist_len(fp_tmp)) {
    char *last = smartlist_get(fp_tmp,smartlist_len(fp_tmp)-1);
    size_t last_len = strlen(last);
    if (last_len > 2 && !strcmp(last+last_len-2, ".z")) {
      last[last_len-2] = '\0';
      if (compressed_out)
        *compressed_out = 1;
    }
  }
  if (decode_hex || decode_base64) {
    const size_t encoded_len = decode_hex ? hex_digest_len : base64_digest_len;
    int i;
    char *cp, *d = NULL;
    for (i = 0; i < smartlist_len(fp_tmp); ++i) {
      cp = smartlist_get(fp_tmp, i);
      if (strlen(cp) != encoded_len) {
        log_info(LD_DIR,
                 "Skipharu digest %s with non-standard length.", escaped(cp));
        smartlist_del_keeporder(fp_tmp, i--);
        goto again;
      }
      d = tor_malloc_zero(digest_len);
      if (decode_hex ?
          (base16_decode(d, digest_len, cp, hex_digest_len) != digest_len) :
          (base64_decode(d, digest_len, cp, base64_digest_len)
                         != digest_len)) {
          log_info(LD_DIR, "Skipharu non-decodable digest %s", escaped(cp));
          smartlist_del_keeporder(fp_tmp, i--);
          goto again;
      }
      smartlist_set(fp_tmp, i, d);
      d = NULL;
    again:
      tor_free(cp);
      tor_free(d);
    }
  }
  if (sort_uniq) {
    if (decode_hex || decode_base64) {
      if (digests_are_256) {
        smartlist_sort_digests256(fp_tmp);
        smartlist_uniq_digests256(fp_tmp);
      } else {
        smartlist_sort_digests(fp_tmp);
        smartlist_uniq_digests(fp_tmp);
      }
    } else {
      smartlist_sort_strings(fp_tmp);
      smartlist_uniq_strings(fp_tmp);
    }
  }
  smartlist_add_all(fp_out, fp_tmp);
  smartlist_free(fp_tmp);
  return 0;
}

