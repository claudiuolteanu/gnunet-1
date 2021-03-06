/*
 * This file is part of GNUnet
 * (C) 2013 Christian Grothoff (and other contributing authors)
 *
 * GNUnet is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 3, or (at your
 * option) any later version.
 *
 * GNUnet is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNUnet; see the file COPYING.  If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * @file psyc/gnunet-service-psyc.c
 * @brief PSYC service
 * @author Gabor X Toth
 */

#include "platform.h"
#include "gnunet_util_lib.h"
#include "gnunet_constants.h"
#include "gnunet_protocols.h"
#include "gnunet_statistics_service.h"
#include "gnunet_multicast_service.h"
#include "gnunet_psycstore_service.h"
#include "gnunet_psyc_service.h"
#include "psyc.h"


/**
 * Handle to our current configuration.
 */
static const struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Handle to the statistics service.
 */
static struct GNUNET_STATISTICS_Handle *stats;

/**
 * Notification context, simplifies client broadcasts.
 */
static struct GNUNET_SERVER_NotificationContext *nc;

/**
 * Handle to the PSYCstore.
 */
static struct GNUNET_PSYCSTORE_Handle *store;

/**
 * Channel's pub_key_hash -> struct Channel
 */
static struct GNUNET_CONTAINER_MultiHashMap *clients;

/**
 * Message in the transmission queue.
 */
struct TransmitMessage
{
  struct TransmitMessage *prev;
  struct TransmitMessage *next;

  char *buf;
  uint16_t size;
  /**
   * enum MessageState
   */
  uint8_t state;
};

/**
 * Common part of the client context for both a master and slave channel.
 */
struct Channel
{
  struct GNUNET_SERVER_Client *client;

  struct TransmitMessage *tmit_head;
  struct TransmitMessage *tmit_tail;

  GNUNET_SCHEDULER_TaskIdentifier tmit_task;

  /**
   * Expected value size for the modifier being received from the PSYC service.
   */
  uint32_t tmit_mod_value_size_expected;

  /**
   * Actual value size for the modifier being received from the PSYC service.
   */
  uint32_t tmit_mod_value_size;

  /**
   * enum MessageState
   */
  uint8_t tmit_state;

  uint8_t in_transmit;
  uint8_t is_master;
  uint8_t disconnected;
};

/**
 * Client context for a channel master.
 */
struct Master
{
  struct Channel channel;
  struct GNUNET_CRYPTO_EddsaPrivateKey priv_key;
  struct GNUNET_CRYPTO_EddsaPublicKey pub_key;
  struct GNUNET_HashCode pub_key_hash;

  struct GNUNET_MULTICAST_Origin *origin;
  struct GNUNET_MULTICAST_OriginMessageHandle *tmit_handle;

  /**
   * Maximum message ID for this channel.
   *
   * Incremented before sending a message, thus the message_id in messages sent
   * starts from 1.
   */
  uint64_t max_message_id;

  /**
   * ID of the last message that contains any state operations.
   * 0 if there is no such message.
   */
  uint64_t max_state_message_id;

  /**
   * Maximum group generation for this channel.
   */
  uint64_t max_group_generation;

  /**
   * @see enum GNUNET_PSYC_Policy
   */
  uint32_t policy;
};


/**
 * Client context for a channel slave.
 */
struct Slave
{
  struct Channel channel;
  struct GNUNET_CRYPTO_EddsaPrivateKey slave_key;
  struct GNUNET_CRYPTO_EddsaPublicKey chan_key;
  struct GNUNET_HashCode chan_key_hash;

  struct GNUNET_MULTICAST_Member *member;
  struct GNUNET_MULTICAST_MemberRequestHandle *tmit_handle;

  struct GNUNET_PeerIdentity origin;
  struct GNUNET_PeerIdentity *relays;
  struct GNUNET_MessageHeader *join_req;

  uint64_t max_message_id;
  uint64_t max_request_id;

  uint32_t relay_count;
};


static inline void
transmit_message (struct Channel *ch);


/**
 * Task run during shutdown.
 *
 * @param cls unused
 * @param tc unused
 */
static void
shutdown_task (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  if (NULL != nc)
  {
    GNUNET_SERVER_notification_context_destroy (nc);
    nc = NULL;
  }
  if (NULL != stats)
  {
    GNUNET_STATISTICS_destroy (stats, GNUNET_NO);
    stats = NULL;
  }
}


static void
client_cleanup (struct Channel *ch)
{
  if (ch->is_master)
  {
    struct Master *mst = (struct Master *) ch;
    if (NULL != mst->origin)
      GNUNET_MULTICAST_origin_stop (mst->origin);
    GNUNET_CONTAINER_multihashmap_remove (clients, &mst->pub_key_hash, mst);
  }
  else
  {
    struct Slave *slv = (struct Slave *) ch;
    if (NULL != slv->join_req)
      GNUNET_free (slv->join_req);
    if (NULL != slv->relays)
      GNUNET_free (slv->relays);
    if (NULL != slv->member)
      GNUNET_MULTICAST_member_part (slv->member);
  }

  GNUNET_free (ch);
}


/**
 * Called whenever a client is disconnected.
 * Frees our resources associated with that client.
 *
 * @param cls Closure.
 * @param client Identification of the client.
 */
static void
client_disconnect (void *cls, struct GNUNET_SERVER_Client *client)
{
  if (NULL == client)
    return;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Client %p disconnected\n", client);

  struct Channel *ch
    = GNUNET_SERVER_client_get_user_context (client, struct Channel);
  if (NULL == ch)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "User context is NULL in client_disconnect()\n");
    GNUNET_break (0);
    return;
  }

  ch->disconnected = GNUNET_YES;

  /* Send pending messages to multicast before cleanup. */
  if (NULL != ch->tmit_head)
  {
    transmit_message (ch);
  }
  else
  {
    client_cleanup (ch);
  }
}


static void
join_cb (void *cls, const struct GNUNET_CRYPTO_EddsaPublicKey *member_key,
         const struct GNUNET_MessageHeader *join_req,
         struct GNUNET_MULTICAST_JoinHandle *jh)
{

}


static void
membership_test_cb (void *cls,
                    const struct GNUNET_CRYPTO_EddsaPublicKey *member_key,
                    uint64_t message_id, uint64_t group_generation,
                    struct GNUNET_MULTICAST_MembershipTestHandle *mth)
{

}


static void
replay_fragment_cb (void *cls,
                    const struct GNUNET_CRYPTO_EddsaPublicKey *member_key,
                    uint64_t fragment_id, uint64_t flags,
                    struct GNUNET_MULTICAST_ReplayHandle *rh)

{
}


static void
replay_message_cb (void *cls,
                   const struct GNUNET_CRYPTO_EddsaPublicKey *member_key,
                   uint64_t message_id,
                   uint64_t fragment_offset,
                   uint64_t flags,
                   struct GNUNET_MULTICAST_ReplayHandle *rh)
{

}


static void
fragment_store_result (void *cls, int64_t result, const char *err_msg)
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "fragment_store() returned %l (%s)\n", result, err_msg);
}


/**
 * Iterator callback for sending a message to a client.
 *
 * @see message_cb()
 */
static int
message_to_client (void *cls, const struct GNUNET_HashCode *chan_key_hash,
                   void *chan)
{
  const struct GNUNET_MessageHeader *msg = cls;
  struct Channel *ch = chan;

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Sending message of type %u and size %u to client 0x%zx.\n",
              ntohs (msg->type), ntohs (msg->size), ch->client);

  GNUNET_SERVER_notification_context_add (nc, ch->client);
  GNUNET_SERVER_notification_context_unicast (nc, ch->client, msg, GNUNET_NO);

  return GNUNET_YES;
}


/**
 * Incoming message fragment from multicast.
 *
 * Store it using PSYCstore and send it to all clients of the channel.
 */
static void
message_cb (void *cls, const struct GNUNET_MessageHeader *msg)
{
  uint16_t type = ntohs (msg->type);
  uint16_t size = ntohs (msg->size);

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Received message of type %u and size %u from multicast.\n",
              type, size);

  struct Channel *ch = cls;
  struct Master *mst = cls;
  struct Slave *slv = cls;

  /* const struct GNUNET_MULTICAST_MessageHeader *mmsg
     = (const struct GNUNET_MULTICAST_MessageHeader *) msg; */
  struct GNUNET_CRYPTO_EddsaPublicKey *chan_key
    = ch->is_master ? &mst->pub_key : &slv->chan_key;
  struct GNUNET_HashCode *chan_key_hash
    = ch->is_master ? &mst->pub_key_hash : &slv->chan_key_hash;

  switch (type)
  {
  case GNUNET_MESSAGE_TYPE_MULTICAST_MESSAGE:
  {
    GNUNET_PSYCSTORE_fragment_store (store, chan_key,
                                     (const struct
                                      GNUNET_MULTICAST_MessageHeader *) msg,
                                     0, NULL, NULL);

#if TODO
    /* FIXME: apply modifiers to state in PSYCstore */
    GNUNET_PSYCSTORE_state_modify (store, chan_key,
                                   GNUNET_ntohll (mmsg->message_id),
                                   meth->mod_count, mods,
                                   rcb, rcb_cls);
#endif

    const struct GNUNET_MULTICAST_MessageHeader *mmsg
      = (const struct GNUNET_MULTICAST_MessageHeader *) msg;
    struct GNUNET_PSYC_MessageHeader *pmsg;

    uint16_t size = ntohs (msg->size);
    uint16_t psize = 0;
    uint16_t pos = 0;

    for (pos = 0; sizeof (*mmsg) + pos < size; pos += psize)
    {
      const struct GNUNET_MessageHeader *pmsg
        = (const struct GNUNET_MessageHeader *) ((char *) &mmsg[1] + pos);
      psize = ntohs (pmsg->size);
      if (psize < sizeof (*pmsg) || sizeof (*mmsg) + pos + psize > size)
      {
        GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                    "Received invalid message part of type %u and size %u "
                    "from multicast. Not sending to clients.\n",
                    ntohs (pmsg->type), psize);
        GNUNET_break_op (0);
        return;
      }
    }

    psize = sizeof (*pmsg) + size - sizeof (*mmsg);
    pmsg = GNUNET_malloc (psize);
    pmsg->header.size = htons (psize);
    pmsg->header.type = htons (GNUNET_MESSAGE_TYPE_PSYC_MESSAGE);
    pmsg->message_id = mmsg->message_id;

    memcpy (&pmsg[1], &mmsg[1], size - sizeof (*mmsg));

    GNUNET_CONTAINER_multihashmap_get_multiple (clients, chan_key_hash,
                                                message_to_client,
                                                (void *) pmsg);
    GNUNET_free (pmsg);
    break;
  }
  default:
    GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
                "Discarding unknown message of type %u and size %u.\n",
                type, size);
  }
}


/**
 * Send a request received from multicast to a client.
 */
static int
request_to_client (void *cls, const struct GNUNET_HashCode *chan_key_hash,
                   void *chan)
{
  /* TODO */

  return GNUNET_YES;
}


static void
request_cb (void *cls, const struct GNUNET_CRYPTO_EddsaPublicKey *member_key,
            const struct GNUNET_MessageHeader *req,
            enum GNUNET_MULTICAST_MessageFlags flags)
{

}


/**
 * Response from PSYCstore with the current counter values for a channel master.
 */
static void
master_counters_cb (void *cls, int result, uint64_t max_fragment_id,
                    uint64_t max_message_id, uint64_t max_group_generation,
                    uint64_t max_state_message_id)
{
  struct Master *mst = cls;
  struct Channel *ch = &mst->channel;
  struct CountersResult *res = GNUNET_malloc (sizeof (*res));
  res->header.type = htons (GNUNET_MESSAGE_TYPE_PSYC_MASTER_START_ACK);
  res->header.size = htons (sizeof (*res));
  res->result_code = htonl (result);
  res->max_message_id = GNUNET_htonll (max_message_id);

  if (GNUNET_OK == result || GNUNET_NO == result)
  {
    mst->max_message_id = max_message_id;
    mst->max_state_message_id = max_state_message_id;
    mst->max_group_generation = max_group_generation;
    mst->origin
      = GNUNET_MULTICAST_origin_start (cfg, &mst->priv_key,
                                       max_fragment_id + 1,
                                       join_cb, membership_test_cb,
                                       replay_fragment_cb, replay_message_cb,
                                       request_cb, message_cb, ch);
  }
  GNUNET_SERVER_notification_context_add (nc, ch->client);
  GNUNET_SERVER_notification_context_unicast (nc, ch->client, &res->header,
                                              GNUNET_NO);
  GNUNET_free (res);
}


/**
 * Response from PSYCstore with the current counter values for a channel slave.
 */
void
slave_counters_cb (void *cls, int result, uint64_t max_fragment_id,
                   uint64_t max_message_id, uint64_t max_group_generation,
                   uint64_t max_state_message_id)
{
  struct Slave *slv = cls;
  struct Channel *ch = &slv->channel;
  struct CountersResult *res = GNUNET_malloc (sizeof (*res));
  res->header.type = htons (GNUNET_MESSAGE_TYPE_PSYC_SLAVE_JOIN_ACK);
  res->header.size = htons (sizeof (*res));
  res->result_code = htonl (result);
  res->max_message_id = GNUNET_htonll (max_message_id);

  if (GNUNET_OK == result || GNUNET_NO == result)
  {
    slv->max_message_id = max_message_id;
    slv->member
      = GNUNET_MULTICAST_member_join (cfg, &slv->chan_key, &slv->slave_key,
                                      &slv->origin,
                                      slv->relay_count, slv->relays,
                                      slv->join_req, join_cb,
                                      membership_test_cb,
                                      replay_fragment_cb, replay_message_cb,
                                      message_cb, ch);
  }

  GNUNET_SERVER_notification_context_add (nc, ch->client);
  GNUNET_SERVER_notification_context_unicast (nc, ch->client, &res->header,
                                              GNUNET_NO);
  GNUNET_free (res);
}


/**
 * Handle a connecting client starting a channel master.
 */
static void
handle_master_start (void *cls, struct GNUNET_SERVER_Client *client,
                     const struct GNUNET_MessageHeader *msg)
{
  const struct MasterStartRequest *req
    = (const struct MasterStartRequest *) msg;
  struct Master *mst = GNUNET_new (struct Master);
  mst->channel.client = client;
  mst->channel.is_master = GNUNET_YES;
  mst->policy = ntohl (req->policy);
  mst->priv_key = req->channel_key;
  GNUNET_CRYPTO_eddsa_key_get_public (&mst->priv_key,
                                                  &mst->pub_key);
  GNUNET_CRYPTO_hash (&mst->pub_key, sizeof (mst->pub_key), &mst->pub_key_hash);

  GNUNET_PSYCSTORE_counters_get (store, &mst->pub_key,
                                 master_counters_cb, mst);

  GNUNET_SERVER_client_set_user_context (client, &mst->channel);
  GNUNET_CONTAINER_multihashmap_put (clients, &mst->pub_key_hash, mst,
                                     GNUNET_CONTAINER_MULTIHASHMAPOPTION_MULTIPLE);
  GNUNET_SERVER_receive_done (client, GNUNET_OK);
}


/**
 * Handle a connecting client joining as a channel slave.
 */
static void
handle_slave_join (void *cls, struct GNUNET_SERVER_Client *client,
                   const struct GNUNET_MessageHeader *msg)
{
  const struct SlaveJoinRequest *req
    = (const struct SlaveJoinRequest *) msg;
  struct Slave *slv = GNUNET_new (struct Slave);
  slv->channel.client = client;
  slv->channel.is_master = GNUNET_NO;
  slv->slave_key = req->slave_key;
  slv->chan_key = req->channel_key;
  GNUNET_CRYPTO_hash (&slv->chan_key, sizeof (slv->chan_key),
                      &slv->chan_key_hash);
  slv->origin = req->origin;
  slv->relay_count = ntohl (req->relay_count);

  const struct GNUNET_PeerIdentity *relays
    = (const struct GNUNET_PeerIdentity *) &req[1];
  slv->relays
    = GNUNET_malloc (slv->relay_count * sizeof (struct GNUNET_PeerIdentity));
  uint32_t i;
  for (i = 0; i < slv->relay_count; i++)
    memcpy (&slv->relays[i], &relays[i], sizeof (*relays));

  GNUNET_PSYCSTORE_counters_get (store, &slv->chan_key,
                                 slave_counters_cb, slv);

  GNUNET_SERVER_client_set_user_context (client, &slv->channel);
  GNUNET_SERVER_receive_done (client, GNUNET_OK);
}


/**
 * Send acknowledgement to a client.
 *
 * Sent after a message fragment has been passed on to multicast.
 *
 * @param ch The channel struct for the client.
 */
static void
send_message_ack (struct Channel *ch)
{
  struct GNUNET_MessageHeader res;
  res.size = htons (sizeof (res));
  res.type = htons (GNUNET_MESSAGE_TYPE_PSYC_MESSAGE_ACK);

  GNUNET_SERVER_notification_context_add (nc, ch->client);
  GNUNET_SERVER_notification_context_unicast (nc, ch->client, &res,
                                              GNUNET_NO);
}


/**
 * Callback for the transmit functions of multicast.
 */
static int
transmit_notify (void *cls, size_t *data_size, void *data)
{
  struct Channel *ch = cls;
  struct TransmitMessage *tmit_msg = ch->tmit_head;

  if (NULL == tmit_msg || *data_size < tmit_msg->size)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "transmit_notify: nothing to send.\n");
    *data_size = 0;
    return GNUNET_NO;
  }

  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "transmit_notify: sending %u bytes.\n", tmit_msg->size);

  *data_size = tmit_msg->size;
  memcpy (data, tmit_msg->buf, *data_size);

  GNUNET_CONTAINER_DLL_remove (ch->tmit_head, ch->tmit_tail, tmit_msg);
  GNUNET_free (tmit_msg);

  int ret = (MSG_STATE_END < ch->tmit_state) ? GNUNET_NO : GNUNET_YES;
  send_message_ack (ch);

  if (0 == ch->tmit_task)
  {
    if (NULL != ch->tmit_head)
    {
      transmit_message (ch);
    }
    else if (ch->disconnected)
    {
      /* FIXME: handle partial message (when still in_transmit) */
      client_cleanup (ch);
    }
  }

  return ret;
}


/**
 * Transmit a message from a channel master to the multicast group.
 */
static void
master_transmit_message (struct Master *mst)
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "master_transmit_message()\n");
  mst->channel.tmit_task = 0;
  if (NULL == mst->tmit_handle)
  {
    mst->tmit_handle
      = GNUNET_MULTICAST_origin_to_all (mst->origin, ++mst->max_message_id,
                                        mst->max_group_generation,
                                        transmit_notify, mst);
  }
  else
  {
    GNUNET_MULTICAST_origin_to_all_resume (mst->tmit_handle);
  }
}


/**
 * Transmit a message from a channel slave to the multicast group.
 */
static void
slave_transmit_message (struct Slave *slv)
{
  slv->channel.tmit_task = 0;
  if (NULL == slv->tmit_handle)
  {
    slv->tmit_handle
      = GNUNET_MULTICAST_member_to_origin(slv->member, ++slv->max_request_id,
                                          transmit_notify, slv);
  }
  else
  {
    GNUNET_MULTICAST_member_to_origin_resume (slv->tmit_handle);
  }
}


static inline void
transmit_message (struct Channel *ch)
{
  ch->is_master
    ? master_transmit_message ((struct Master *) ch)
    : slave_transmit_message ((struct Slave *) ch);
}


static void
transmit_error (struct Channel *ch)
{
  struct GNUNET_MessageHeader *msg;
  struct TransmitMessage *tmit_msg = GNUNET_malloc (sizeof (*tmit_msg)
                                                    + sizeof (*msg));
  msg = (struct GNUNET_MessageHeader *) &tmit_msg[1];
  msg->size = ntohs (sizeof (*msg));
  msg->type = ntohs (GNUNET_MESSAGE_TYPE_PSYC_MESSAGE_CANCEL);

  tmit_msg->buf = (char *) &tmit_msg[1];
  tmit_msg->size = sizeof (*msg);
  tmit_msg->state = ch->tmit_state;
  GNUNET_CONTAINER_DLL_insert_tail (ch->tmit_head, ch->tmit_tail, tmit_msg);
  transmit_message (ch);

  /* FIXME: cleanup */
  GNUNET_SERVER_client_disconnect (ch->client);
}


/**
 * Incoming message from a client.
 */
static void
handle_psyc_message (void *cls, struct GNUNET_SERVER_Client *client,
                         const struct GNUNET_MessageHeader *msg)
{
  struct Channel *ch
    = GNUNET_SERVER_client_get_user_context (client, struct Channel);
  GNUNET_assert (NULL != ch);

  uint16_t size = ntohs (msg->size);
  uint16_t psize = 0, pos = 0;

  if (GNUNET_MULTICAST_FRAGMENT_MAX_PAYLOAD < size - sizeof (*msg))
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR, "Message payload too large\n");
    GNUNET_break (0);
    transmit_error (ch);
    return;
  }

  for (pos = 0; sizeof (*msg) + pos < size; pos += psize)
  {
    const struct GNUNET_MessageHeader *pmsg
      = (const struct GNUNET_MessageHeader *) ((char *) &msg[1] + pos);
    psize = ntohs (pmsg->size);
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                "Received message part of type %u and size %u "
                "from client.\n", ntohs (pmsg->type), psize);
    if (psize < sizeof (*pmsg) || sizeof (*msg) + pos + psize > size)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                  "Received invalid message part of type %u and size %u "
                  "from client.\n", ntohs (pmsg->type), psize);
      GNUNET_break (0);
      transmit_error (ch);
      return;
    }
  }

  size -= sizeof (*msg);
  struct TransmitMessage *tmit_msg = GNUNET_malloc (sizeof (*tmit_msg) + size);
  tmit_msg->buf = (char *) &tmit_msg[1];
  memcpy (tmit_msg->buf, &msg[1], size);
  tmit_msg->size = size;
  tmit_msg->state = ch->tmit_state;
  GNUNET_CONTAINER_DLL_insert_tail (ch->tmit_head, ch->tmit_tail, tmit_msg);
  transmit_message (ch);

  GNUNET_SERVER_receive_done (client, GNUNET_OK);
};


/**
 * Initialize the PSYC service.
 *
 * @param cls Closure.
 * @param server The initialized server.
 * @param c Configuration to use.
 */
static void
run (void *cls, struct GNUNET_SERVER_Handle *server,
     const struct GNUNET_CONFIGURATION_Handle *c)
{
  static const struct GNUNET_SERVER_MessageHandler handlers[] = {
    { &handle_master_start, NULL,
      GNUNET_MESSAGE_TYPE_PSYC_MASTER_START, 0 },

    { &handle_slave_join, NULL,
      GNUNET_MESSAGE_TYPE_PSYC_SLAVE_JOIN, 0 },

    { &handle_psyc_message, NULL,
      GNUNET_MESSAGE_TYPE_PSYC_MESSAGE, 0 },
  };

  cfg = c;
  store = GNUNET_PSYCSTORE_connect (cfg);
  stats = GNUNET_STATISTICS_create ("psyc", cfg);
  clients = GNUNET_CONTAINER_multihashmap_create (1, GNUNET_YES);
  nc = GNUNET_SERVER_notification_context_create (server, 1);
  GNUNET_SERVER_add_handlers (server, handlers);
  GNUNET_SERVER_disconnect_notify (server, &client_disconnect, NULL);
  GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_FOREVER_REL, &shutdown_task,
                                NULL);
}


/**
 * The main function for the service.
 *
 * @param argc number of arguments from the command line
 * @param argv command line arguments
 * @return 0 ok, 1 on error
 */
int
main (int argc, char *const *argv)
{
  return (GNUNET_OK ==
          GNUNET_SERVICE_run (argc, argv, "psyc",
			      GNUNET_SERVICE_OPTION_NONE,
                              &run, NULL)) ? 0 : 1;
}

/* end of gnunet-service-psycstore.c */
