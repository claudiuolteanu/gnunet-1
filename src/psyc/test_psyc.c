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
 * @file psyc/test_psyc.c
 * @brief Test for the PSYC service.
 * @author Gabor X Toth
 * @author Christian Grothoff
 */

#include <inttypes.h>

#include "platform.h"
#include "gnunet_crypto_lib.h"
#include "gnunet_common.h"
#include "gnunet_util_lib.h"
#include "gnunet_testing_lib.h"
#include "gnunet_env_lib.h"
#include "gnunet_psyc_service.h"

#define TIMEOUT GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 10)

#define DEBUG_SERVICE 0


/**
 * Return value from 'main'.
 */
static int res;

static const struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Handle for task for timeout termination.
 */
static GNUNET_SCHEDULER_TaskIdentifier end_badly_task;

static struct GNUNET_PSYC_Master *mst;
static struct GNUNET_PSYC_Slave *slv;
static struct GNUNET_PSYC_Channel *ch;

static struct GNUNET_CRYPTO_EddsaPrivateKey *channel_key;
static struct GNUNET_CRYPTO_EddsaPrivateKey *slave_key;

static struct GNUNET_CRYPTO_EddsaPublicKey channel_pub_key;
static struct GNUNET_CRYPTO_EddsaPublicKey slave_pub_key;

struct GNUNET_PSYC_MasterTransmitHandle *mth;

struct TransmitClosure
{
  struct GNUNET_PSYC_MasterTransmitHandle *handle;
  struct GNUNET_ENV_Environment *env;
  char *data[16];
  const char *mod_value;
  size_t mod_value_size;
  uint8_t data_count;
  uint8_t paused;
  uint8_t n;
};

struct TransmitClosure *tmit;

/**
 * Clean up all resources used.
 */
static void
cleanup ()
{
  if (NULL != mst)
  {
    GNUNET_PSYC_master_stop (mst);
    mst = NULL;
  }
  if (NULL != tmit)
  {
    GNUNET_ENV_environment_destroy (tmit->env);
    GNUNET_free (tmit);
    tmit = NULL;
  }
  GNUNET_SCHEDULER_shutdown ();
}


/**
 * Terminate the testcase (failure).
 *
 * @param cls NULL
 * @param tc scheduler context
 */
static void
end_badly (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  res = 1;
  cleanup ();
}


/**
 * Terminate the testcase (success).
 *
 * @param cls NULL
 * @param tc scheduler context
 */
static void
end_normally (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  res = 0;
  cleanup ();
}


/**
 * Finish the testcase (successfully).
 */
static void
end ()
{
  if (end_badly_task != GNUNET_SCHEDULER_NO_TASK)
  {
    GNUNET_SCHEDULER_cancel (end_badly_task);
    end_badly_task = GNUNET_SCHEDULER_NO_TASK;
  }
  GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_MILLISECONDS,
				&end_normally, NULL);
}


static void
message (void *cls, uint64_t message_id, uint32_t flags,
         const struct GNUNET_MessageHeader *msg)
{
  if (NULL == msg)
  {
    GNUNET_log (GNUNET_ERROR_TYPE_ERROR,
                "Error while receiving message %llu\n", message_id);
    return;
  }

  uint16_t type = ntohs (msg->type);
  uint16_t size = ntohs (msg->size);

  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Got message part of type %u and size %u "
              "belonging to message ID %llu with flags %u\n",
              type, size, message_id, flags);

  if (GNUNET_MESSAGE_TYPE_PSYC_MESSAGE_END == type)
    end ();
}


static void
join_request (void *cls, const struct GNUNET_CRYPTO_EddsaPublicKey *slave_key,
              const char *method_name,
              size_t variable_count, const struct GNUNET_ENV_Modifier *variables,
              const void *data, size_t data_size,
              struct GNUNET_PSYC_JoinHandle *jh)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING,
              "Got join request.");
}


static void
transmit_resume (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Transmission resumed.\n");
  struct TransmitClosure *tmit = cls;
  tmit->paused = GNUNET_NO;
  GNUNET_PSYC_master_transmit_resume (tmit->handle);
}


static int
tmit_notify_mod (void *cls, uint16_t *data_size, void *data, uint8_t *oper)
{
  struct TransmitClosure *tmit = cls;
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Transmit notify modifier: %lu bytes available, "
              "%u modifiers left to process.\n",
              *data_size, GNUNET_ENV_environment_get_count (tmit->env));

  enum GNUNET_ENV_Operator op = 0;
  const char *name = NULL;
  const char *value = NULL;
  uint16_t name_size = 0;
  size_t value_size = 0;

  if (NULL != tmit->mod_value && 0 < tmit->mod_value_size)
  { /* Modifier continuation */
    value = tmit->mod_value;
    if (tmit->mod_value_size <= *data_size)
    {
      value_size = tmit->mod_value_size;
      tmit->mod_value = NULL;
    }
    else
    {
      value_size = *data_size;
      tmit->mod_value += value_size;
    }
    tmit->mod_value_size -= value_size;

    if (*data_size < value_size)
    {
      GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
                  "value larger than buffer: %u < %zu\n",
                  *data_size, value_size);
      *data_size = 0;
      return GNUNET_NO;
    }

    *data_size = value_size;
    memcpy (data, value, value_size);
  }
  else if (NULL != oper)
  {
    if (GNUNET_NO == GNUNET_ENV_environment_shift (tmit->env, &op, &name,
                                                   (void *) &value, &value_size))
    { /* No more modifiers, continue with data */
      *data_size = 0;
      return GNUNET_YES;
    }

    *oper = op;
    name_size = strlen (name);

    if (name_size + 1 + value_size <= *data_size)
    {
      *data_size = name_size + 1 + value_size;
    }
    else
    {
      tmit->mod_value_size = value_size;
      value_size = *data_size - name_size - 1;
      tmit->mod_value_size -= value_size;
      tmit->mod_value = value + value_size;
    }

    memcpy (data, name, name_size);
    ((char *)data)[name_size] = '\0';
    memcpy ((char *)data + name_size + 1, value, value_size);
  }

  return 0 == tmit->mod_value_size ? GNUNET_YES : GNUNET_NO;
}


static int
tmit_notify_data (void *cls, uint16_t *data_size, void *data)
{
  struct TransmitClosure *tmit = cls;
  uint16_t size = strlen (tmit->data[tmit->n]);
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Transmit notify data: %lu bytes available, "
              "processing fragment %u/%u (size %u).\n",
              *data_size, tmit->n + 1, tmit->data_count, size);
  if (*data_size < size)
  {
    *data_size = 0;
    GNUNET_assert (0);
    return GNUNET_SYSERR;
  }

  if (GNUNET_YES == tmit->paused && tmit->n == tmit->data_count - 1)
  {
    /* Send last fragment later. */
    GNUNET_log (GNUNET_ERROR_TYPE_DEBUG, "Transmission paused.\n");
    tmit->paused = GNUNET_YES;
    GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_relative_multiply
                                  (GNUNET_TIME_UNIT_SECONDS, 3),
                                  &transmit_resume, tmit);
    *data_size = 0;
    return GNUNET_NO;
  }

  *data_size = size;
  memcpy (data, tmit->data[tmit->n], size);

  return ++tmit->n < tmit->data_count ? GNUNET_NO : GNUNET_YES;
}


static void
master_started (void *cls, uint64_t max_message_id)
{
  GNUNET_log (GNUNET_ERROR_TYPE_DEBUG,
              "Master started: %" PRIu64 "\n", max_message_id);

  tmit = GNUNET_new (struct TransmitClosure);
  tmit->env = GNUNET_ENV_environment_create ();
  GNUNET_ENV_environment_add (tmit->env, GNUNET_ENV_OP_ASSIGN,
                              "_foo", "bar baz", 7);
  GNUNET_ENV_environment_add (tmit->env, GNUNET_ENV_OP_ASSIGN,
                              "_foo_bar", "foo bar baz", 11);
  tmit->data[0] = "foo";
  tmit->data[1] = "foo bar";
  tmit->data[2] = "foo bar baz";
  tmit->data_count = 3;
  tmit->handle
    = GNUNET_PSYC_master_transmit (mst, "_test", tmit_notify_mod,
                                   tmit_notify_data, tmit,
                                   GNUNET_PSYC_MASTER_TRANSMIT_INC_GROUP_GEN);
}


static void
slave_joined (void *cls, uint64_t max_message_id)
{
  GNUNET_log (GNUNET_ERROR_TYPE_WARNING, "Slave joined: %lu\n", max_message_id);
}


/**
 * Main function of the test, run from scheduler.
 *
 * @param cls NULL
 * @param cfg configuration we use (also to connect to PSYC service)
 * @param peer handle to access more of the peer (not used)
 */
static void
#if DEBUG_SERVICE
run (void *cls, char *const *args, const char *cfgfile,
     const struct GNUNET_CONFIGURATION_Handle *c)
#else
run (void *cls,
     const struct GNUNET_CONFIGURATION_Handle *c,
     struct GNUNET_TESTING_Peer *peer)
#endif
{
  cfg = c;
  end_badly_task = GNUNET_SCHEDULER_add_delayed (TIMEOUT, &end_badly, NULL);

  channel_key = GNUNET_CRYPTO_eddsa_key_create ();
  slave_key = GNUNET_CRYPTO_eddsa_key_create ();

  GNUNET_CRYPTO_eddsa_key_get_public (channel_key, &channel_pub_key);
  GNUNET_CRYPTO_eddsa_key_get_public (slave_key, &slave_pub_key);

  mst = GNUNET_PSYC_master_start (cfg, channel_key, GNUNET_PSYC_CHANNEL_PRIVATE,
                                  &message, &join_request, &master_started, NULL);
  return; /* FIXME: test slave */

  struct GNUNET_PeerIdentity origin;
  struct GNUNET_PeerIdentity relays[16];
  struct GNUNET_ENV_Environment *env = GNUNET_ENV_environment_create ();
  GNUNET_ENV_environment_add (env, GNUNET_ENV_OP_ASSIGN,
                              "_foo", "bar baz", 7);
  GNUNET_ENV_environment_add (env, GNUNET_ENV_OP_ASSIGN,
                              "_foo_bar", "foo bar baz", 11);
  slv = GNUNET_PSYC_slave_join (cfg, &channel_pub_key, slave_key, &origin,
                                16, relays, &message, &join_request, &slave_joined,
                                NULL, "_request_join", env, "some data", 9);
  GNUNET_ENV_environment_destroy (env);
}


int
main (int argc, char *argv[])
{
  res = 1;
#if DEBUG_SERVICE
  const struct GNUNET_GETOPT_CommandLineOption opts[] = {
    GNUNET_GETOPT_OPTION_END
  };
  if (GNUNET_OK != GNUNET_PROGRAM_run (argc, argv, "test-psyc",
                                       "test-psyc [options]",
                                       opts, &run, NULL))
    return 1;
#else
  if (0 != GNUNET_TESTING_peer_run ("test-psyc", "test_psyc.conf", &run, NULL))
    return 1;
#endif
  return res;
}

/* end of test_psyc.c */
