/*!
 * @file Server deamon source.
 *
 * Source for server deamon - a something between client and device,
 * what takes care of priorities and authentification.
 *
 * Contains list of clients with their id's and with their access rights.
 *
 * @author petr
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <mcheck.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>

#include "riseset.h"

#include "../utils/devser.h"
#include "../status.h"

#define PORT	5557

#define NOT_DEFINED_SERVER	0
#define CLIENT_SERVER		1
#define DEVICE_SERVER		2

int msgid;			// id of server message que

struct device
{
  pid_t pid;
  char name[DEVICE_NAME_SIZE];
  char hostname[DEVICE_URI_SIZE];
  uint16_t port;
  int type;
  int authorizations[MAX_CLIENT];
};

struct client
{
  pid_t pid;
  char login[CLIENT_LOGIN_SIZE];
  int authorized;
  int priority;
};

//! information about serverd stored in shared mem
struct serverd_info
{
  int priority_client;

  int current_state;

  int next_event_type;
  time_t next_event_time;

  struct device devices[MAX_DEVICE];
  struct client clients[MAX_CLIENT];
};

//! global server type
int server_type = NOT_DEFINED_SERVER;
int serverd_id = -1;

//! pointers to shared memory
struct serverd_info *shm_info;
struct device *shm_devices;
struct client *shm_clients;

/*!
 * Find one device.
 * 
 * @param name		device name
 * @param id		device id; if NULL, device id is not returned
 * @param device	device
 *
 * @return 0 and set pointer do device on success, -1 and set errno on error
 */
int
find_device (char *name, int *id, struct device **device)
{
  int i;
  for (i = 0; i < MAX_DEVICE; i++)
    if (strcmp (shm_devices[i].name, name) == 0)
      {
	*device = &shm_devices[i];
	if (id)
	  *id = i;
	return 0;
      }
  errno = ENODEV;
  return -1;
}

/*!
 * Send IPC message to all clients.
 *
 * @param format	message format
 * @param ...		format arguments
 */
int
clients_all_msg_snd (char *format, ...)
{
  va_list ap;
  int i;
  va_start (ap, format);
  for (i = 0; i < MAX_CLIENT; i++)
    {
      /* device exists */
      if (*shm_clients[i].login)
	{
	  if (devser_2devser_message_va (i, format, ap))
	    return -1;
	}
    }
  va_end (ap);
  return 0;
}

/*!
 * Send IPC message to one device.
 *
 * @param name		device name
 * @param format	message format
 * @param ...		format arguments
 *
 * @execption ENODEV	device don't exists
 *
 * @return 0 on success, -1 on error, set errno
 */
int
device_msg_snd (char *name, char *format, ...)
{
  struct device *device;
  int id;
  if (find_device (name, &id, &device))
    {
      int ret;
      va_list ap;

      va_start (ap, format);
      ret = devser_2devser_message_va (id + MAX_CLIENT + 1, format, ap);
      va_end (ap);
    }
  errno = ENODEV;
  return -1;
}

/*!
 * Send IPC messsages to all registered devices.
 *
 * @param format	message format
 * @param ...		format arguments
 * 
 * @return 0 on success, -1 and set errno on any error
 */
int
devices_all_msg_snd (char *format, ...)
{
  va_list ap;
  int i;
  va_start (ap, format);
  for (i = 0; i < MAX_DEVICE; i++)
    {
      /* device exists */
      if (*shm_devices[i].name)
	{
	  if (devser_2devser_message_va (i + MAX_CLIENT, format, ap))
	    return -1;
	}
    }
  va_end (ap);
  return 0;
}

int
device_serverd_handle_command (char *command)
{
  if (strcmp (command, "authorize") == 0)
    {
      int client;
      int key;
      if (devser_param_test_length (2))
	return -1;
      if (devser_param_next_integer (&client)
	  || devser_param_next_integer (&key))
	return -1;

      devser_shm_data_lock ();
      if (shm_devices[serverd_id].authorizations[client] == 0)
	{
	  devser_dprintf ("authorization_failed %i", client);
	  devser_write_command_end (DEVDEM_E_SYSTEM,
				    "client %i didn't ask for authorization",
				    client);
	  devser_shm_data_unlock ();
	  return -1;
	}

      if (shm_devices[serverd_id].authorizations[client] != key)
	{
	  devser_dprintf ("authorization_failed %i", client);
	  devser_write_command_end (DEVDEM_E_SYSTEM,
				    "invalid authorization key: %i %i", key,
				    shm_devices[serverd_id].
				    authorizations[client]);
	  shm_devices[serverd_id].authorizations[client] = 0;
	  devser_shm_data_unlock ();
	  return -1;
	}
      shm_devices[serverd_id].authorizations[client] = 0;
      devser_shm_data_unlock ();

      devser_dprintf ("authorization_ok %i", client);

      return 0;
    }
  devser_write_command_end (DEVDEM_E_COMMAND, "unknow command: %s", command);
  return -1;
}

/*!
 * Made priority update, distribute messages to devices
 * about priority update.
 *
 * @param timeout	time to wait for priority change.. 
 *
 * @return 0 on success, -1 and set errno otherwise
 */
int
clients_change_priority (time_t timeout)
{
  int new_priority_client = -1;
  int new_priority_max = -2;
  int i;

  // change priority and find new client with highest priority
  for (i = 0; i < MAX_CLIENT; i++)
    {
      if (*shm_clients[i].login && shm_clients[i].priority > new_priority_max)
	{
	  new_priority_client = i;
	  new_priority_max = shm_clients[i].priority;
	}
    }
  shm_info->priority_client = new_priority_client;
  return devices_all_msg_snd ("priority %i %i", new_priority_client, timeout);
}

void *
serverd_riseset_thread (void *arg)
{
  time_t curr_time;
  syslog (LOG_DEBUG, "riseset thread start");
  while (1)
    {
      curr_time = time (NULL);
      devser_shm_data_lock ();

      next_event (&curr_time, &shm_info->next_event_type,
		  &shm_info->next_event_time);
      if (shm_info->current_state < SERVERD_MAINTANCE)
	{
	  shm_info->current_state = (shm_info->next_event_type + 3) % 4;
	  devices_all_msg_snd ("current_state %i", shm_info->current_state);
	  clients_all_msg_snd ("I current_state %i", shm_info->current_state);
	}

      devser_shm_data_unlock ();

      syslog (LOG_DEBUG, "riseset thread sleeping %li seconds",
	      shm_info->next_event_time - curr_time);
      sleep (shm_info->next_event_time - curr_time);
    }
}

/*!
 * Called on server exit.
 *
 * Delete client|device login|name, updates priorities, detach shared
 * memory.
 *
 * @see on_exit(3)
 */
void
serverd_exit (int status, void *arg)
{
  devser_shm_data_lock ();

  switch (server_type)
    {
    case CLIENT_SERVER:
      *shm_clients[serverd_id].login = 0;
      clients_change_priority (0);
      break;
    case DEVICE_SERVER:
      *shm_devices[serverd_id].name = 0;
      break;
    default:
      // I'm not expecting that one to occur, since we
      // are registering at_exit after registering device,
      // but no one never knows..
      syslog (LOG_DEBUG, "exit of unknow type server %i", server_type);
    }

  devser_shm_data_dt (shm_info);
  devser_shm_data_unlock ();
}

/*!
 * @param new_state		new state, if -1 -> 3
 */
int
serverd_change_state (int new_state)
{
  devser_shm_data_lock ();
  if (new_state == -1)
    shm_info->current_state = (shm_info->next_event_type + 3) % 4;
  else
    shm_info->current_state = new_state;
  devices_all_msg_snd ("current_state %i", shm_info->current_state);
  clients_all_msg_snd ("I current_state %i", shm_info->current_state);
  devser_shm_data_unlock ();
  return 0;
}

int
client_serverd_handle_command (char *command)
{
  if (strcmp (command, "password") == 0)
    {
      char *passwd;
      if (devser_param_test_length (1))
	return -1;
      if (devser_param_next_string (&passwd))
	return -1;

      /* authorize password
       *
       * TODO some more complicated code would be necessary */

      if (strncmp (passwd, shm_clients[serverd_id].login, CLIENT_LOGIN_SIZE)
	  == 0)
	{
	  shm_clients[serverd_id].authorized = 1;
	  devser_dprintf ("logged_as %i", serverd_id);
	  devser_shm_data_lock ();
	  devser_dprintf ("I current_state %i", shm_info->current_state);
	  devser_shm_data_unlock ();
	  return 0;
	}
      else
	{
	  sleep (5);		// wait some time to prevent repeat attack
	  devser_write_command_end (DEVDEM_E_SYSTEM,
				    "invalid login or password");
	  return -1;
	}
    }
  if (shm_clients[serverd_id].authorized)
    {
      if (strcmp (command, "info") == 0)
	{
	  int i;
	  if (devser_param_test_length (0))
	    return -1;

	  for (i = 0; i < MAX_CLIENT; i++)
	    if (*shm_clients[i].login)
	      devser_dprintf ("user %i %s", i, shm_clients[i].login);

	  return 0;
	}
      else if (strcmp (command, "devinfo") == 0)
	{
	  int i;
	  struct device *dev;
	  if (devser_param_test_length (0))
	    return -1;

	  for (i = 0, dev = shm_devices; i < MAX_DEVICE; i++, dev++)

	    if (*dev->name)
	      devser_dprintf ("I device %i %s %s:%i", i, dev->name,
			      dev->hostname, dev->port);
	  return 0;
	}
      else if (strcmp (command, "priority") == 0
	       || strcmp (command, "prioritydeferred") == 0)
	{
	  int timeout;
	  int new_priority;

	  if (strcmp (command, "priority") == 0)
	    {
	      if (devser_param_test_length (1))
		return -1;
	      if (devser_param_next_integer (&new_priority))
		return -1;
	      timeout = 0;
	    }
	  else
	    {
	      if (devser_param_test_length (2))
		return -1;
	      if (devser_param_next_integer (&new_priority))
		return -1;
	      if (devser_param_next_integer (&timeout))
		return -1;
	      timeout += time (NULL);
	    }

	  // prevent others from accesing priority
	  // since we don't want any other process to change
	  // priority while we are testing it
	  devser_shm_data_lock ();

	  devser_dprintf ("old_priority %i %i", serverd_id,
			  shm_clients[serverd_id].priority);

	  devser_dprintf ("actual_priority %i %i", shm_info->priority_client,
			  shm_clients[shm_info->priority_client].priority);

	  shm_clients[serverd_id].priority = new_priority;

	  if (clients_change_priority (timeout))
	    {
	      devser_shm_data_unlock ();
	      devser_write_command_end (DEVDEM_E_PRIORITY,
					"error when processing priority request");
	      return -1;
	    }

	  devser_dprintf ("actual_priority %i %i", shm_info->priority_client,
			  shm_clients[shm_info->priority_client].priority);

	  devser_shm_data_unlock ();

	  return 0;
	}
      else if (strcmp (command, "key") == 0)
	{
	  int key = random ();
	  // device number could change..device names don't
	  char *dev_name;
	  struct device *device;
	  int dev_id;
	  if (devser_param_test_length (1)
	      || devser_param_next_string (&dev_name))
	    return -1;
	  // find device, set it authorization key
	  if (find_device (dev_name, &dev_id, &device))
	    {
	      devser_write_command_end (DEVDEM_E_SYSTEM,
					"cannot find device with name %s",
					dev_name);
	      return -1;
	    }

	  devser_shm_data_lock ();
	  device->authorizations[serverd_id] = key;
	  devser_shm_data_unlock ();

	  devser_dprintf ("authorization_key %s %i", dev_name, key);
	  return 0;
	}
      else if (strcmp (command, "on") == 0)
	{
	  return serverd_change_state (-1);
	}
      else if (strcmp (command, "maintance") == 0)
	{
	  return serverd_change_state (SERVERD_MAINTANCE);
	}
      else if (strcmp (command, "off") == 0)
	{
	  return serverd_change_state (SERVERD_OFF);
	}
    }
  devser_write_command_end (DEVDEM_E_COMMAND,
			    "unknow command / not authorized (please use pasword): %s",
			    command);
  return -1;
}

/*!
 * Handle receiving of IPC message.
 *
 * @param message	string message
 */
int
serverd_client_handle_msg (char *message)
{
  return devser_dprintf (message);
}

/*!
 * Handle receiving of IPC message.
 *
 * @param message	string message
 */
int
serverd_device_handle_msg (char *message)
{
  return devser_message (message);
}

/*! 
 * Handle serverd commands.
 *
 * @param command 
 * @return -2 on exit, -1 and set errno on HW failure, 0 otherwise
 */
int
serverd_handle_command (char *command)
{
  if (strcmp (command, "login") == 0)
    {
      if (server_type == NOT_DEFINED_SERVER)
	{
	  char *login;
	  int i;

	  srandom (time (NULL));

	  if (devser_param_test_length (1))
	    return -1;

	  if (devser_param_next_string (&login) < 0)
	    return -1;

	  devser_shm_data_lock ();

	  // find not used client
	  for (i = 0; i < MAX_CLIENT; i++)
	    if (!*shm_clients[i].login)
	      {
		shm_clients[i].authorized = 0;
		shm_clients[i].priority = -1;
		strncpy (shm_clients[i].login, login, CLIENT_LOGIN_SIZE);
		serverd_id = i;
		shm_clients[i].pid = devser_child_pid;
		if (devser_set_server_id (i, serverd_client_handle_msg))
		  {
		    devser_shm_data_unlock ();
		    return -1;
		  }
		break;
	      }

	  devser_shm_data_unlock ();

	  if (i == MAX_CLIENT)
	    {
	      devser_write_command_end (DEVDEM_E_SYSTEM,
					"cannot allocate client - not enough resources");
	      return -1;
	    }

	  server_type = CLIENT_SERVER;
	  on_exit (serverd_exit, NULL);
	  return 0;
	}
      else
	{
	  devser_write_command_end (DEVDEM_E_COMMAND,
				    "cannot switch server type to CLIENT_SERVER");
	  return -1;
	}
    }
  else if (strcmp (command, "register") == 0)
    {
      if (server_type == NOT_DEFINED_SERVER)
	{
	  char *reg_device;
	  int reg_type;
	  char *hostname;
	  unsigned int port;
	  int i;

	  if (devser_param_test_length (3))
	    return -1;
	  if (devser_param_next_string (&reg_device))
	    return -1;
	  if (devser_param_next_integer (&reg_type))
	    return -1;
	  if (devser_param_next_ip_address (&hostname, &port))
	    return -1;

	  devser_shm_data_lock ();
	  // find not used device
	  for (i = 0; i < MAX_DEVICE; i++)
	    if (!*(shm_devices[i].name))
	      {
		strncpy (shm_devices[i].name, reg_device, DEVICE_NAME_SIZE);
		strncpy (shm_devices[i].hostname, hostname, DEVICE_URI_SIZE);
		shm_devices[i].port = port;
		shm_devices[i].type = reg_type;
		shm_devices[i].pid = devser_child_pid;
		if (devser_set_server_id
		    (i + MAX_CLIENT, serverd_device_handle_msg))
		  {
		    devser_shm_data_unlock ();
		    return -1;
		  }
		serverd_id = i;
		break;
	      }
	    else if (strcmp (shm_devices[i].name, reg_device) == 0)
	      {
		devser_shm_data_unlock ();

		devser_write_command_end (DEVDEM_E_SYSTEM,
					  "name %s already registered",
					  reg_device);
		return -1;
	      }


	  if (i == MAX_DEVICE)
	    {
	      devser_write_command_end (DEVDEM_E_SYSTEM,
					"cannot allocate new device - not enough resources");
	      devser_shm_data_unlock ();
	      return -1;
	    }

	  server_type = DEVICE_SERVER;
	  on_exit (serverd_exit, NULL);
	  devser_message ("current_state %i", shm_info->current_state);
	  devser_shm_data_unlock ();

	  return 0;
	}
      else
	{
	  devser_write_command_end (DEVDEM_E_COMMAND,
				    "cannot switch server type to DEVICE_SERVER");
	  return -1;
	}
    }
  else if (server_type == DEVICE_SERVER)
    return device_serverd_handle_command (command);
  else if (server_type == CLIENT_SERVER)
    return client_serverd_handle_command (command);
  else
    {
      devser_write_command_end (DEVDEM_E_COMMAND, "unknow command: %s",
				command);
      return -1;
    }
  return 0;
}

int
main (void)
{
#ifdef DEBUG
  mtrace ();
#endif
  shm_info = NULL;
  shm_clients = NULL;
  shm_devices = NULL;

  openlog (NULL, LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
  if (devser_init (sizeof (struct serverd_info)))
    {
      syslog (LOG_ERR, "error in devser_init: %m");
      return -1;
    }

  shm_info = (struct serverd_info *) devser_shm_data_at ();
  shm_clients = shm_info->clients;
  shm_devices = shm_info->devices;

  shm_info->current_state = 0;

  devser_thread_create (serverd_riseset_thread, NULL, 0, NULL, NULL);

  return devser_run (PORT, serverd_handle_command);
}
