#ifndef __RTS2_BLOCK__
#define __RTS2_BLOCK__

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <syslog.h>
#include "status.h"

#define MSG_COMMAND             0x01
#define MSG_REPLY               0x02
#define MSG_DATA                0x04

#define MAX_CONN		20
#define MAX_DATA		200

typedef enum conn_type_t
{ NOT_DEFINED_SERVER, CLIENT_SERVER, DEVICE_SERVER };

class Rts2Block;

class Rts2Conn
{
  conn_type_t type;
  char name[DEVICE_NAME_SIZE];	// name of device/client this connection goes to
  int key;
  int priority;			// priority - number
  int have_priority;		// priority - flag if we have priority
  int centrald_id;		// id of connection on central server
  in_addr addr;
  int port;			// local port & connection
  virtual int connectionError ()
  {
    return -1;
  }
protected:
    Rts2Block * master;
  char *command_start;
  int sock;
  int conn_state;
public:
  char buf[MAX_DATA + 1];
  char *buf_top;

  Rts2Conn (Rts2Block * in_master);
  Rts2Conn (int in_sock, Rts2Block * in_master);
  ~Rts2Conn (void);
  int add (fd_set * set);
  virtual int init ()
  {
    return -1;
  };
  virtual int send (char *message);
  int sendValue (char *name, int value);
  int sendValue (char *name, int val1, int val2);
  int sendValue (char *name, char *value);
  int sendValue (char *name, double value);
  int sendCommandEnd (int num, char *message);
  int acceptConn ();
  int receive (fd_set * set);
  conn_type_t getType ()
  {
    return type;
  };
  void setType (conn_type_t in_type)
  {
    type = in_type;
  }
  int getName (struct sockaddr_in *addr);
  void setAddress (struct in_addr *in_address);
  void setPort (int in_port)
  {
    port = in_port;
  }
  void getAddress (char *addrBuf, int buf_size);
  int getLocalPort ()
  {
    return port;
  }
  const char *getName ()
  {
    return name;
  };
  void setName (char *in_name)
  {
    strncpy (name, in_name, DEVICE_NAME_SIZE);
  };
  int getKey ()
  {
    return key;
  };
  void setKey (int in_key)
  {
    key = in_key;
  };
  int havePriority ()
  {
    return have_priority;
  };
  void setHavePriority (int in_have_priority)
  {
    if (in_have_priority)
      send ("S priority 1 priority received");
    else
      send ("S priority 0 priority lost");
    have_priority = in_have_priority;
  };
  int getPriority ()
  {
    return priority;
  };
  void setPriority (int in_priority)
  {
    priority = in_priority;
  };
  int getCentraldId ()
  {
    return centrald_id;
  };
  void setCentraldId (int in_centrald_id);
  int sendPriorityInfo (int number);
  int endConnection ()
  {
    conn_state = 5;		// mark for deleting..
  }
  virtual int sendInfo (Rts2Conn * conn)
  {
    return -1;
  }
protected:
  virtual int command ();
  virtual int message ();
  virtual int informations ();
  virtual int status ();
  inline char *getCommand ()
  {
    return command_start;
  }
  inline int isCommand (const char *cmd)
  {
    return !strcmp (cmd, getCommand ());
  }
  int paramEnd ();
  int paramNextString (char **str);
  int paramNextInteger (int *num);
  int paramNextDouble (double *num);
  int paramNextFloat (float *num);
};

class Rts2Block
{
  int sock;
  int port;
  long int idle_timeout;	// in msec
  int priority_client;

public:
    Rts2Conn * connections[MAX_CONN];

    Rts2Block ();
   ~Rts2Block (void);
  void setPort (int in_port);
  virtual int init ();
  virtual int addConnection (int in_sock);
  Rts2Conn *findName (char *in_name);
  virtual int sendStatusMessage (char *state_name, int state);
  virtual int sendMessage (char *message);
  virtual int sendMessage (char *message, int val1, int val2);
  virtual int idle ();
  int setTimeout (long int new_timeout)
  {
    idle_timeout = new_timeout;
  }
  int run ();
  int setPriorityClient (int in_priority_client, int timeout);
  void checkPriority (Rts2Conn * conn)
  {
    if (conn->getCentraldId () == priority_client)
      {
	conn->setHavePriority (1);
      }
  }
  virtual int setMasterState (int new_state)
  {
    return 0;
  }
};

#endif /*! __RTS2_NETBLOCK__ */
