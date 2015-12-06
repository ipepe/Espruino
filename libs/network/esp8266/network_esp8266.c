
/*
 * This file is part of Espruino, a JavaScript interpreter for Microcontrollers
 *
 * Copyright (C) 2015 Gordon Williams <gw@pur3.co.uk>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * ----------------------------------------------------------------------------
 * This file is designed to be parsed during the build process
 *
 * Contains ESP8266 board network specific functions.
 * ----------------------------------------------------------------------------
 */

// ESP8266 specific includes
#undef ESPSDK_1_3_0
#include <c_types.h>
#include <user_interface.h>
#include <mem.h>
#include <osapi.h>
#include <espconn.h>
#include <espmissingincludes.h>

#define _GCC_WRAP_STDINT_H
typedef long long int64_t;

#include "network_esp8266.h"
#include "esp8266_board_utils.h"

/**
 * The maximum number of concurrently open sockets we support.
 * We should probably pair this with the ESP8266 concept of the maximum number of sockets
 * that an ESP8266 instance can also support.
 */
#define MAX_SOCKETS (10)

// Set NET_DBG to 0 to disable debug printf's, to 1 for important printf's, to 2 for verbose
#define NET_DBG 2
// Normal debug
#if NET_DBG > 0
#define DBG(format, ...) os_printf(format, ## __VA_ARGS__)
#else
#define DBG(format, ...) do { } while(0)
#endif
// Verbose debug
#if NET_DBG > 1
#define DBGV(format, ...) os_printf(format, ## __VA_ARGS__)
#else
#define DBGV(format, ...) do { } while(0)
#endif

static struct socketData *getSocketData(int s);

/**
 * The next socketId to be used.
 */
static int g_nextSocketId = 0;

static int  getServerSocketByLocalPort(unsigned short port);
static void setSocketInError(int socketId, const char *msg, int code);
static void dumpEspConn(struct espconn *pEspConn);
static struct socketData *allocateNewSocket();
static int connectSocket(struct socketData *pSocketData);
static void doClose(int socketId);
static void releaseSocket(int socketId);
static void resetSocketByData(struct socketData *pSocketData);
static void resetSocketById(int sckt);
static void esp8266_dumpSocketData(struct socketData *pSocketData);

static void esp8266_callback_connectCB_inbound(void *arg);
static void esp8266_callback_connectCB_outbound(void *arg);
static void esp8266_callback_disconnectCB(void *arg);
static void esp8266_callback_sentCB(void *arg);
static void esp8266_callback_writeFinishedCB(void *arg);
static void esp8266_callback_recvCB(void *arg, char *pData, unsigned short len);
static void esp8266_callback_reconnectCB(void *arg, sint8 err);

/** Socket data structure
 *
 * We maintain an array of socketData structures.  The number of such structures is defined in the
 * MAX_SOCKETS define.  The private variable that contains the array is called "socketArray".
 * Each one of these array instances represents a possible socket structure that we can use.
 *
 * Each socket maintains state and its creation purpose.
 *
 * The trickiest part is closing. If the socket lib closes a socket it forgets about the socket
 * as soon as we return. We then have to issue a disconnect to espconn and await the disconnect
 * call-back in the SOCKET_STATE_DISCONNECTING. Once that's done, we can deallocate everything.
 * If we receive a disconnect from the remote end, we free the espconn struct and we transition
 * to SOCKET_STATE_CLOSED/SOCKET_STATE_ERROR until we can respond to a send/recv call from the
 * socket library with -1 and it then calls close.
 */

/**
 * The potential states for a socket.
 * See the socket state diagram.
 */
enum SOCKET_STATE {
  SOCKET_STATE_UNUSED,         //!< Unused socket "slot"
  SOCKET_STATE_UNACCEPTED,     //!< New inbound connection that Espruino hasn't accepted yet
  SOCKET_STATE_HOST_RESOLVING, //!< Resolving a hostname, happens before CONNECTING
  SOCKET_STATE_CONNECTING,     //!< In the process of connecting
  SOCKET_STATE_IDLE,           //!< Connected but nothing in tx buffers
  SOCKET_STATE_TRANSMITTING,   //!< Connected and espconn_send has been called, awaiting CB
  SOCKET_STATE_DISCONNECTING,  //!< Did disconnect, awaiting discon callback from espconn
  SOCKET_STATE_CLOSED,         //!< Closed, espconn struct freed, awaiting close from socket lib
  SOCKET_STATE_ERROR,          //!< Error state, awaiting close from socket lib
};

/**
 * How was the socket created.
 */
enum SOCKET_CREATION_TYPE {
  SOCKET_CREATED_NONE,      //!< The socket has not yet been created.
  SOCKET_CREATED_SERVER,    //!< Listening socket ("server socket")
  SOCKET_CREATED_OUTBOUND,  //!< Outbound connection
  SOCKET_CREATED_INBOUND    //!< Inbound connection
};

/**
 * The core socket structure.
 * The structure is initialized by resetSocket.
 */
struct socketData {
  int                       socketId;     //!< The id of THIS socket.
  enum SOCKET_STATE         state;        //!< What is the socket state?
  enum SOCKET_CREATION_TYPE creationType; //!< How was the socket created?

  struct  espconn *pEspconn;              //!< The ESPConn structure.

  uint8    *currentTx;        //!< Data currently being transmitted.
  uint8    *rxBuf;            //!< Data received (inbound).
  uint16_t rxBufLen;          //!< The length of data in the buffer ready for consumption.

  char     *errorMsg;         //!< Error message.
  int      errorCode;         //!< Error code.
};


/**
 * An array of socket data structures.
 */
static struct socketData socketArray[MAX_SOCKETS];

/**
 * Flag the sockets as initially NOT initialized.
 */
static bool g_socketsInitialized = false;

/**
 * Dump all the socket structures.
 * This is used exclusively for debugging.  It walks through each of the
 * socket structures and dumps their state to the debug log.
 */
void esp8266_dumpAllSocketData() {
  for (int i=0; i<MAX_SOCKETS; i++) {
    esp8266_dumpSocketData(&socketArray[i]);
  }
}


/**
 * Write the details of a socket to the debug log.
 * The data associated with the socket is dumped to the debug log.
 */
void esp8266_dumpSocket(
    int socketId //!< The ID of the socket data structure to be logged.
  ) {
  struct socketData *pSocketData = getSocketData(socketId);
  esp8266_dumpSocketData(pSocketData);
}


/**
 * Write the details of a socketData to the debug log.
 * The data associated with the socketData is dumped to the debug log.
 */
static void esp8266_dumpSocketData(
    struct socketData *pSocketData //!< The socket data structure to be logged
  ) {
  DBG("===== socket %d\n", pSocketData->socketId);
  char *creationTypeMsg;
  switch(pSocketData->creationType) {
  case SOCKET_CREATED_NONE:
    creationTypeMsg = "none";
    break;
  case SOCKET_CREATED_INBOUND:
    creationTypeMsg = "inbound";
    break;
  case SOCKET_CREATED_OUTBOUND:
    creationTypeMsg = "outbound";
    break;
  case SOCKET_CREATED_SERVER:
    creationTypeMsg = "server";
    break;
  }
  DBG("type=%s, rxBuf=%p, rxLen=%d, txBuf=%p\n", creationTypeMsg, pSocketData->rxBuf,
      pSocketData->rxBufLen, pSocketData->currentTx);
  char *stateMsg;
  switch(pSocketData->state) {
  case SOCKET_STATE_CLOSED:
    stateMsg = "closing";
    break;
  case SOCKET_STATE_CONNECTING:
    stateMsg = "connecting";
    break;
  case SOCKET_STATE_DISCONNECTING:
    stateMsg = "disconnecting";
    break;
  case SOCKET_STATE_ERROR:
    stateMsg = "error";
    break;
  case SOCKET_STATE_IDLE:
    stateMsg = "idle";
    break;
  case SOCKET_STATE_TRANSMITTING:
    stateMsg = "transmitting";
    break;
  case SOCKET_STATE_HOST_RESOLVING:
    stateMsg = "resolving";
    break;
  case SOCKET_STATE_UNACCEPTED:
    stateMsg = "unaccepted";
    break;
  case SOCKET_STATE_UNUSED:
    stateMsg = "unused";
    break;
  default:
    stateMsg = "Unexpected state!!";
    break;
  }
  DBG("      state=%s, espconn=%p, err=%d", stateMsg, pSocketData->pEspconn, pSocketData->errorCode);

  // Print the errorMsg if it has anything to say
  if (pSocketData->errorMsg != NULL && strlen(pSocketData->errorMsg) > 0) {
    DBG(", errorMsg=\"%s\"", pSocketData->errorMsg);
  }

  DBG("\n");
}


/**
 * Dump a struct espconn (for debugging purposes).
 */
#if 0
static void dumpEspConn(
    struct espconn *pEspConn //!<
  ) {
  char ipString[20];
  LOG("Dump of espconn: 0x%x\n", (int)pEspConn);
  if (pEspConn == NULL) {
    return;
  }
  switch(pEspConn->type) {
  case ESPCONN_TCP:
    LOG(" - type = TCP\n");
    LOG("   - local address    = %d.%d.%d.%d [%d]\n",
        pEspConn->proto.tcp->local_ip[0],
        pEspConn->proto.tcp->local_ip[1],
        pEspConn->proto.tcp->local_ip[2],
        pEspConn->proto.tcp->local_ip[3],
        pEspConn->proto.tcp->local_port);
    LOG("   - remote address   = %d.%d.%d.%d [%d]\n",
        pEspConn->proto.tcp->remote_ip[0],
        pEspConn->proto.tcp->remote_ip[1],
        pEspConn->proto.tcp->remote_ip[2],
        pEspConn->proto.tcp->remote_ip[3],
        pEspConn->proto.tcp->remote_port);
    break;
  case ESPCONN_UDP:
    LOG(" - type = UDP\n");
    LOG("   - local_port  = %d\n", pEspConn->proto.udp->local_port);
    LOG("   - local_ip    = %d.%d.%d.%d\n",
        pEspConn->proto.tcp->local_ip[0],
        pEspConn->proto.tcp->local_ip[1],
        pEspConn->proto.tcp->local_ip[2],
        pEspConn->proto.tcp->local_ip[3]);
    LOG("   - remote_port = %d\n", pEspConn->proto.udp->remote_port);
    LOG("   - remote_ip   = %d.%d.%d.%d\n",
        pEspConn->proto.tcp->remote_ip[0],
        pEspConn->proto.tcp->remote_ip[1],
        pEspConn->proto.tcp->remote_ip[2],
        pEspConn->proto.tcp->remote_ip[3]);
    break;
  default:
    LOG(" - type = Unknown!! 0x%x\n", pEspConn->type);
  }
  switch(pEspConn->state) {
  case ESPCONN_NONE:
    LOG(" - state=NONE");
    break;
  case ESPCONN_WAIT:
    LOG(" - state=WAIT");
    break;
  case ESPCONN_LISTEN:
    LOG(" - state=LISTEN");
    break;
  case ESPCONN_CONNECT:
    LOG(" - state=CONNECT");
    break;
  case ESPCONN_WRITE:
    LOG(" - state=WRITE");
    break;
  case ESPCONN_READ:
    LOG(" - state=READ");
    break;
  case ESPCONN_CLOSE:
    LOG(" - state=CLOSE");
    break;
  default:
    LOG(" - state=unknown!!");
    break;
  }
  LOG(", link_cnt=%d", pEspConn->link_cnt);
  LOG(", reverse=0x%x\n", (unsigned int)pEspConn->reverse);
}
#endif


/**
 * Get the next new global socket id.
 * \return A new socketId that is assured to be unique.
 */
static int getNextGlobalSocketId() {
  int ret = g_nextSocketId;
  g_nextSocketId++;
  return ret;
}


/**
 * Allocate a new socket
 * Look for the first free socket in the array of sockets and return the first one
 * that is available.  The socketId property is set to a unique and new socketId value
 * that will not previously have been seen.
 * \return The socketData structure for the returned socket.
 */
static struct socketData *allocateNewSocket() {
  // Walk through each of the sockets in the array of possible sockets and stop
  // at the first one that is flagged as not in use.  For that socket, set its
  // socketId to the next global socketId value.
  for (int i=0; i<MAX_SOCKETS; i++) {
    if (socketArray[i].state == SOCKET_STATE_UNUSED) {
      socketArray[i].socketId = getNextGlobalSocketId();
      return &socketArray[i];
    }
  }
  return(NULL);
}


/**
 * Retrieve the socketData for the given socket index.
 * \return The socket data for the given socket or NULL if there is no matching socket.
 */
static struct socketData *getSocketData(int socketId) {
  struct socketData *pSocketData = socketArray;
  for (int socketArrayIndex=0; socketArrayIndex<MAX_SOCKETS; socketArrayIndex++) {
    if (pSocketData->socketId == socketId) {
      return pSocketData;
    }
    pSocketData++;
  }
  return NULL;
}


/**
 * Find the server socket that is bound to the given local port.
 * \return The socket id of the socket listening on the given port or -1 if there is no
 * server socket that matches.
 */
static int getServerSocketByLocalPort(
    unsigned short port //!< The port number on which a server socket is listening.
  ) {
  // Loop through each of the sockets in the socket array looking for a socket
  // that is inuse, a server and has a local_port of the passed in port number.
  int socketArrayIndex;
  struct socketData *pSocketData = socketArray;
  for (socketArrayIndex=0; socketArrayIndex<MAX_SOCKETS; socketArrayIndex++) {
    if (pSocketData->state  != SOCKET_STATE_UNUSED &&
      pSocketData->creationType == SOCKET_CREATED_SERVER &&
      pSocketData->pEspconn->proto.tcp->local_port == port)
    {
      return pSocketData->socketId;
    }
    pSocketData++;
  } // End of for each socket
  return -1;
}


/**
 * Reset the socket to its clean and unused state.
 * The socket is found by its socket id.
 */
static void resetSocketById(
    int sckt //!< The socket id to be reset.
  ) {
  struct socketData *pSocketData = getSocketData(sckt);
  resetSocketByData(pSocketData);
}

/**
 * Reset the socket to its clean and unused state.
 * The socket is specified by its socket data pointer.
 */
static void resetSocketByData(
      struct socketData *pSocketData //!< The data pointer to the socket.
  ) {
  assert(pSocketData != NULL);
  os_memset(pSocketData, 0, sizeof(pSocketData));
}


/**
 * Release the socket and return it to the free pool.
 * The connection (espconn) must be closed and deallocated before calling releaseSocket.
 */
static void releaseSocket(
    int socketId //!< The socket id of the socket to be released.
  ) {
  DBGV("> releaseSocket: %d\n", socketId);
  //esp8266_dumpSocket(socketId);

  struct socketData *pSocketData = getSocketData(socketId);
  assert(pSocketData->state != SOCKET_STATE_UNUSED);
  assert(pSocketData->pEspconn == NULL);
  assert(pSocketData->rxBuf == NULL);

  if (pSocketData->currentTx != NULL) {
    os_free(pSocketData->currentTx);
    pSocketData->currentTx = NULL;
  }

  resetSocketByData(pSocketData);
  DBGV("< releaseSocket\n");
}

/**
 * Release the espconn structure
 */
static void releaseEspconn(
    struct socketData *pSocketData
) {
  if (pSocketData->pEspconn == NULL) return;
  if (pSocketData->creationType == SOCKET_CREATED_INBOUND) return; // we did not allocate it
  DBGV("Freeing espconn %p for socket %d\n", pSocketData->pEspconn, pSocketData->socketId);
  os_free(pSocketData->pEspconn->proto.tcp);
  pSocketData->pEspconn->proto.tcp = NULL;
  os_free(pSocketData->pEspconn);
  pSocketData->pEspconn = NULL;
}


/**
 * Initialize the ESP8266_BOARD environment.
 * Walk through each of the sockets and initialize each one.
 */
void netInit_esp8266_board() {
  if (g_socketsInitialized) {
    return;
  }
  g_socketsInitialized = true;

  struct socketData *pSocketData = socketArray;
  for (int socketArrayIndex=0; socketArrayIndex<MAX_SOCKETS; socketArrayIndex++) {
    resetSocketByData(pSocketData);
    pSocketData++;
  } // End of for each socket
}


/**
 * Perform an actual closure of the socket by calling the ESP8266 disconnect API.
 * This is broken out into its own function because this can happen in
 * a number of possible places.
 */
static void doClose(
    int socketId //!< The socket id to be closed.
  ) {
  struct socketData *pSocketData = getSocketData(socketId);
  if (pSocketData == NULL) return; // just in case

  // if we're already closing, then don't do anything
  if (pSocketData->state == SOCKET_STATE_CLOSED ||
      pSocketData->state == SOCKET_STATE_DISCONNECTING ||
      pSocketData->state == SOCKET_STATE_ERROR)
  {
    return;
  }

  // if we're in the name resolution phase, we need to be careful FIXME
  if (pSocketData->state == SOCKET_STATE_HOST_RESOLVING) {
    return; // FIXME
  }

  // Tell espconn to disconnect/delete the connection
  if (pSocketData->creationType == SOCKET_CREATED_SERVER) {
    //dumpEspConn(pSocketData->pEspconn);
    int rc = espconn_delete(pSocketData->pEspconn);
    if (rc != 0) {
      DBG("espconn_delete: rc=%s (%d)\n", esp8266_errorToString(rc), rc);
      setSocketInError(socketId, "espconn_delete", rc);
    }

  } else {
    int rc = espconn_disconnect(pSocketData->pEspconn);
    if (rc != 0) {
      DBG("espconn_disconnect: rc=%s (%d)\n",esp8266_errorToString(rc),  rc);
      setSocketInError(socketId, "espconn_disconnect", rc);
    }
  }

  pSocketData->state = SOCKET_STATE_DISCONNECTING;
}


/**
 * Set the given socket as being in error supplying a message and a code.
 * The socket state is placed in `SOCKET_STATE_ERROR`.
 */
static void setSocketInError(
    int socketId,      //!< The socket id that is being flagged as in error.
    const char *msg,   //!< A message to associate with the error.
    int code           //!< A low level error code.
  ) {
  struct socketData *pSocketData = getSocketData(socketId);
  pSocketData->state     = SOCKET_STATE_ERROR;
  pSocketData->errorMsg  = (char *)msg;
  pSocketData->errorCode = code;
}

/**
 * Callback function registered to the ESP8266 environment that is
 * invoked when a new inbound connection has been formed.
 */
static void esp8266_callback_connectCB_inbound(
    void *arg //!<
  ) {
  struct espconn *pEspconn = (struct espconn *)arg;
  assert(pEspconn != NULL);
  DBG(">> connectCB_inbound for port %d\n", pEspconn->proto.tcp->local_port);

  struct socketData *pClientSocketData = allocateNewSocket();
  if (pClientSocketData == NULL) {
    DBG("Out of sockets !!!\n");
    espconn_disconnect(pEspconn);
    return;
  }
  DBGV("   allocated socket %d\n", pClientSocketData->socketId);
  //dumpEspConn(pEspconn);

  // register callbacks on the new connection
  espconn_regist_disconcb(pEspconn, esp8266_callback_disconnectCB);
  espconn_regist_reconcb(pEspconn, esp8266_callback_reconnectCB);
  espconn_regist_sentcb(pEspconn, esp8266_callback_sentCB);
  espconn_regist_recvcb(pEspconn, esp8266_callback_recvCB);

  pClientSocketData->pEspconn          = pEspconn;
  pClientSocketData->pEspconn->reverse = pClientSocketData;
  pClientSocketData->creationType      = SOCKET_CREATED_INBOUND;
  pClientSocketData->state             = SOCKET_STATE_UNACCEPTED;
}

/**
 * Callback function registered to the ESP8266 environment that is
 * invoked when a new outbound connection has been formed.
 */
static void esp8266_callback_connectCB_outbound(
    void *arg //!< A pointer to a `struct espconn`.
  ) {
  struct espconn *pEspconn = (struct espconn *)arg;
  assert(pEspconn != NULL);
  struct socketData *pSocketData = (struct socketData *)pEspconn->reverse;
  if (pSocketData == NULL) return; // stray callback (possibly after a disconnect)
  DBGV(">> connectCB_outbound on socket %d\n", pSocketData->socketId);
  //dumpEspConn(pEspconn);

  //esp8266_dumpSocket(pSocketData->socketId);

  assert(pSocketData->state == SOCKET_STATE_CONNECTING);
  pSocketData->state = SOCKET_STATE_IDLE;
}


/**
 * Callback function registered to the ESP8266 environment that is
 * Invoked when a connection has been disconnected. This does get invoked if we
 * initiated the disconnect (new since SDK 1.5?).
 */
static void esp8266_callback_disconnectCB(
    void *arg //!< A pointer to a `struct espconn`.
  ) {
  struct espconn *pEspconn = (struct espconn *)arg;
  struct socketData *pSocketData = (struct socketData *)pEspconn->reverse;
  if (pSocketData == NULL) return;
  assert(pSocketData->state != SOCKET_STATE_UNUSED);
  DBGV(">> disconnectCB on socket %d\n", pSocketData->socketId);

  // we can deallocate the espconn structure
  releaseEspconn(pSocketData);

  // if we were in SOCKET_STATE_DISCONNECTING the socket lib is already done with this socket,
  // so we can free the whole thing. Otherwise, we transition to SOCKET_STATE_CLOSED because
  // we will need to tell the socket lib about the disconnect.
  if (pSocketData->state == SOCKET_STATE_DISCONNECTING) {
    releaseSocket(pSocketData->socketId);
  } else {
    // we can deallocate the tx buffer
    if (pSocketData->currentTx != NULL) {
      os_free(pSocketData->currentTx);
      pSocketData->currentTx = NULL;
    }

    pSocketData->state = SOCKET_STATE_CLOSED;
  }

  //dumpEspConn(pEspconn);
  //esp8266_dumpSocket(pSocketData->socketId);

  DBGV("<< disconnectCB\n");
}


/**
 * Error handler callback.
 * Although this is called `reconnect` by Espressif, this is really a connection reset callback.
 */
static void esp8266_callback_reconnectCB(
    void *arg, //!< A pointer to a `struct espconn`.
    sint8 err  //!< The error code.
  ) {
  struct espconn *pEspconn = (struct espconn *)arg;
  struct socketData *pSocketData = (struct socketData *)pEspconn->reverse;
  if (pSocketData == NULL) return; // we already closed this.
  DBGV(">> resetCB on socket %d: Err %d - %s\n",
      pSocketData->socketId, err, esp8266_errorToString(err));
  bool disconnecting = pSocketData->state == SOCKET_STATE_DISCONNECTING;
  // Do the same as for a disconnect
  esp8266_callback_disconnectCB(arg);
  // Set the socket state as in error.(unless it got freed by esp8266_callback_disconnectCB)
  if (!disconnecting)
    setSocketInError(pSocketData->socketId, esp8266_errorToString(err), err);
  DBGV("<< resetCB\n");
}


/**
 * Callback function registered to the ESP8266 environment that is
 * invoked when a send operation has been completed. This signals that we can reuse the tx buffer
 * and that we can send the next chunk of data.
 */
static void esp8266_callback_sentCB(
    void *arg //!< A pointer to a `struct espconn`.
  ) {
  struct espconn *pEspconn = (struct espconn *)arg;
  struct socketData *pSocketData = (struct socketData *)pEspconn->reverse;
  if (pSocketData == NULL) return; // we already closed this.
  DBGV(">> sendCB on socket %d\n", pSocketData->socketId);

  assert(pSocketData->state == SOCKET_STATE_TRANSMITTING ||
      pSocketData->state == SOCKET_STATE_DISCONNECTING);

  // We have transmitted the data ... which means that the data that was in the transmission
  // buffer can be released.
  if (pSocketData->currentTx != NULL) {
    os_free(pSocketData->currentTx);
    pSocketData->currentTx = NULL;
  }

  if (pSocketData->state == SOCKET_STATE_TRANSMITTING) {
    pSocketData->state = SOCKET_STATE_IDLE;
  }
  DBGV("<< sendCB\n");
}


/**
 * ESP8266 callback function that is invoked when new data has arrived over
 * the TCP/IP connection.
 */
static void esp8266_callback_recvCB(
    void *arg,         //!< A pointer to a `struct espconn`.
    char *pData,       //!< A pointer to data received over the socket.
    unsigned short len //!< The length of the data.
  ) {
  struct espconn *pEspconn = (struct espconn *)arg;
  struct socketData *pSocketData = (struct socketData *)pEspconn->reverse;
  if (pSocketData == NULL) return; // we closed this socket
  assert(pSocketData->state != SOCKET_STATE_UNUSED);

  DBGV(">> recvCB for socket=%d, length=%d\n", pSocketData->socketId, len);

  // If we don't have any existing unconsumed data then malloc some storage and
  // copy the received data into that storage.
  if (pSocketData->rxBufLen == 0) {
    pSocketData->rxBuf = (void *)os_zalloc(len);
    if (pSocketData->rxBuf == NULL) {
      DBG(" - Out of memory allocating %d\n", len);
      return; // FIXME: need to put socket into error state and close it
    }
    os_memcpy(pSocketData->rxBuf, pData, len);
    pSocketData->rxBufLen = len;
  } else {
    // Allocate a new buffer big enough for the original data and the new data and copy the data
    pSocketData->rxBuf = (void *)os_realloc(pSocketData->rxBuf, len + pSocketData->rxBufLen);
    if (pSocketData->rxBuf == NULL) {
      DBG(" - Out of memory re-allocating %d\n", len + pSocketData->rxBufLen);
      return; // FIXME: need to put socket into error state and close it
    }
    os_memcpy(pSocketData->rxBuf + pSocketData->rxBufLen, pData, len);
    pSocketData->rxBufLen += len;
  }
  //dumpEspConn(pEspconn);
  DBGV("<< recvCB\n");
}


// -------------------------------------------------

/**
 * Define the implementation functions for the logical network functions.
 */
void netSetCallbacks_esp8266_board(
    JsNetwork *net //!< The Network we are going to use.
  ) {
    net->idle          = net_ESP8266_BOARD_idle;
    net->checkError    = net_ESP8266_BOARD_checkError;
    net->createsocket  = net_ESP8266_BOARD_createSocket;
    net->closesocket   = net_ESP8266_BOARD_closeSocket;
    net->accept        = net_ESP8266_BOARD_accept;
    net->gethostbyname = net_ESP8266_BOARD_gethostbyname;
    net->recv          = net_ESP8266_BOARD_recv;
    net->send          = net_ESP8266_BOARD_send;
}

/**
 * Determine if there is a new client connection on the server socket.
 * This function is called to poll to see if the serverSckt has a new
 * accepted connection (socket) and, if it does, return it else return -1 to indicate
 * that there was no new accepted socket.
 */
int net_ESP8266_BOARD_accept(
    JsNetwork *net, //!< The Network we are going to use to create the socket.
    int serverSckt  //!< The socket that we are checking to see if there is a new client connection.
  ) {
  //DBGV("> net_ESP8266_BOARD_accept\n");
  struct socketData *pSocketData = getSocketData(serverSckt);
  assert(pSocketData->state    != SOCKET_STATE_UNUSED);
  assert(pSocketData->creationType == SOCKET_CREATED_SERVER);

  // iterate through all sockets and see whether there is one in the UNACCEPTED state that is for
  // the server socket's local port.
  uint16_t serverPort = pSocketData->pEspconn->proto.tcp->local_port;
  for (uint8_t i=0; i<MAX_SOCKETS; i++) {
    if (socketArray[i].state == SOCKET_STATE_UNACCEPTED &&
        socketArray[i].pEspconn != NULL &&
        socketArray[i].pEspconn->proto.tcp->local_port == serverPort)
    {
      DBG("> net_ESP8266_BOARD_accept: Accepted socket %d\n", socketArray[i].socketId);
      return socketArray[i].socketId;
    }
  }

  return -1;
}


/**
 * Receive data from the network device.
 * Returns the number of bytes received which may be 0 and -1 if there was an error.
 */
int net_ESP8266_BOARD_recv(
    JsNetwork *net, //!< The Network we are going to use to create the socket.
    int sckt,       //!< The socket from which we are to receive data.
    void *buf,      //!< The storage buffer into which we will receive data.
    size_t len      //!< The length of the buffer.
  ) {
  struct socketData *pSocketData = getSocketData(sckt);
  assert(pSocketData->state != SOCKET_STATE_UNUSED);

  // If there is no data in the receive buffer, then all we need do is return
  // 0 bytes as the length of data moved or -1 if the socket is actually closed.
  if (pSocketData->rxBufLen == 0) {
    return
      (pSocketData->state == SOCKET_STATE_CLOSED || pSocketData->state == SOCKET_STATE_ERROR)
      ? -1 : 0;
  }

  // If the receive buffer contains data and is it is able to completely fit in the buffer
  // passed into us then we can copy all the data and the receive buffer will be clear.
  if (pSocketData->rxBufLen <= len) {
    os_memcpy(buf, pSocketData->rxBuf, pSocketData->rxBufLen);
    int retLen = pSocketData->rxBufLen;
    pSocketData->rxBufLen = 0;
    os_free(pSocketData->rxBuf);
    pSocketData->rxBuf = NULL;
    DBGV("> net_ESP8266_BOARD_recv: recv %d on socket %d\n", retLen, sckt);
    return retLen;
  }

  // If we are here, then we have more data in the receive buffer than is available
  // to be returned in this request for data.  So we have to copy the amount of data
  // that is allowed to be returned and then strip that from the beginning of the
  // receive buffer.

  // First we copy the data we are going to return.
  os_memcpy(buf, pSocketData->rxBuf, len);
  // Next we shift up the remaining data
  uint16_t newLen = pSocketData->rxBufLen-len;
  os_memmove(pSocketData->rxBuf, pSocketData->rxBuf+len, newLen);
  // And now we shrink the buffer size
  pSocketData->rxBuf = os_realloc(pSocketData->rxBuf, newLen);
  assert(pSocketData->rxBuf != NULL); // won't happen since we have more allocated
  pSocketData->rxBufLen = newLen;
  DBGV("> net_ESP8266_BOARD_recv: recv %d on socket %d\n", len, sckt);
  return len;
}


/**
 * Send data to the partner.
 * The return is the number of bytes actually transmitted which may also be
 * 0 to indicate no bytes sent or -1 to indicate an error.  For the ESP8266 implementation we
 * will return 0 if the socket is not connected or we are in the `SOCKET_STATE_TRANSMITTING`
 * state.
 */
int net_ESP8266_BOARD_send(
    JsNetwork *net,  //!< The Network we are going to use to create the socket.
    int sckt,        //!< The socket over which we will send data.
    const void *buf, //!< The buffer containing the data to be sent.
    size_t len       //!< The length of data in the buffer to send.
  ) {
  //DBGV("> net_ESP8266_BOARD_send: Request to send data to socket %d of size %d: ", sckt, len);

  struct socketData *pSocketData = getSocketData(sckt);
  assert(pSocketData->state != SOCKET_STATE_UNUSED);

  // If the socket is in error or it is closing return -1
  if (pSocketData->state == SOCKET_STATE_ERROR ||
      pSocketData->state == SOCKET_STATE_CLOSED) {
    return -1;
  }

  // Unless we are in the idle state, we can't send more shtuff
  if (pSocketData->state != SOCKET_STATE_IDLE) {
    return 0;
  }

  // Log the content of the data we are sending.
  //esp8266_board_writeString(buf, len);
  //os_printf("\n");

  // Copy the data to be sent into a transmit buffer we hand off to espconn
  assert(pSocketData->currentTx == NULL);
  pSocketData->currentTx = (uint8_t *)os_malloc(len);
  memcpy(pSocketData->currentTx, buf, len);

  // Send the data over the ESP8266 SDK.
  int rc = espconn_send(pSocketData->pEspconn, pSocketData->currentTx, len);
  if (rc < 0) {
    setSocketInError(sckt, "espconn_send error", rc);
    os_free(pSocketData->currentTx);
    pSocketData->currentTx = NULL;
    // ?? espconn_abort(pSocketData->pEspconn);
    return -1; // FIXME: need to close the connection
  }

  pSocketData->state = SOCKET_STATE_TRANSMITTING;

  //esp8266_dumpSocket(sckt);
  DBGV("< net_ESP8266_BOARD_send: sending %d on socket %d\n", len, sckt);
  return len;
}


/**
 * Perform idle processing.
 * There is the possibility that we may wish to perform logic when we are idle.  For the
 * ESP8266 there is no specific idle network processing needed.
 */
void net_ESP8266_BOARD_idle(
    JsNetwork *net //!< The Network we are part of.
  ) {
  // Don't echo here because it is called continuously
  //os_printf("> net_ESP8266_BOARD_idle\n");
}


/**
 * Check for errors.
 * Returns true if there are NO errors.
 */
bool net_ESP8266_BOARD_checkError(
    JsNetwork *net //!< The Network we are checking.
  ) {
  //os_printf("> net_ESP8266_BOARD_checkError\n");
  return true;
}

/* Static variable hack to support async DNS resolutions. This is not great, but it works.
 * There is only one call to net_ESP8266_BOARD_gethostbyname and it is immediately followed
 * by a call to net_ESP8266_BOARD_createSocket, so we save the hostname from the first call
 * in a global variable and then use it in the second to actually kick off the name resolution.
 */
static char *savedHostname = 0;

/**
 * Get an IP address from a name. See the hack description above. This always returns -1
 */
void net_ESP8266_BOARD_gethostbyname(
    JsNetwork *net, //!< The Network we are going to use to create the socket.
    char *hostname, //!< The string representing the hostname we wish to lookup.
    uint32_t *outIp //!< The address into which the resolved IP address will be stored.
  ) {
  assert(hostname != NULL);
  savedHostname = hostname;
  *outIp = -1;
}

/**
 * Callback handler for espconn_gethostbyname.
 */
static void dnsFoundCallback(const char *hostName, ip_addr_t *ipAddr, void *arg) {
  assert(arg != NULL); // arg points to the espconn struct where the resolved IP address needs to go
  struct espconn *pEspconn = arg;
  struct socketData *pSocketData = pEspconn->reverse;

  // ipAddr will be NULL if the IP address can not be resolved.
  if (ipAddr != NULL) {
    *(uint32_t *)&pEspconn->proto.tcp->remote_ip = ipAddr->addr;
    if (pSocketData != NULL) connectSocket(pSocketData);
  } else {
    releaseEspconn(pSocketData);
    if (pSocketData != NULL) setSocketInError(pSocketData->socketId, "hostname not found", 1);
  }
}


/**
 * Create a new socket.
 * if `ipAddress == 0`, creates a server otherwise creates a client (and automatically connects).
 * Returns >=0 on success.
 */
int net_ESP8266_BOARD_createSocket(
    JsNetwork *net,     //!< The Network we are going to use to create the socket.
    uint32_t ipAddress, //!< The address of the partner of the socket or 0 if we are to be a server.
    unsigned short port //!< The port number that the partner is listening upon.
  ) {
  DBGV("> net_ESP8266_BOARD_createSocket: host: %d.%d.%d.%d, port:%d \n",
      ((char *)(&ipAddress))[0], ((char *)(&ipAddress))[1],
      ((char *)(&ipAddress))[2], ((char *)(&ipAddress))[3], port);

  // allocate a socket data structure
  struct socketData *pSocketData = allocateNewSocket();
  if (pSocketData == NULL) { // No free socket
    DBG("< net_ESP8266_BOARD_createSocket: No free sockets\n");
    return -1;
  }

  // allocate espconn data structure and initialize it
  pSocketData->pEspconn = (struct espconn *)os_zalloc(sizeof(struct espconn));
  if (pSocketData->pEspconn == NULL) {
    DBG("< net_ESP8266_BOARD_createSocket: out of memory!\n");
    releaseSocket(pSocketData->socketId);
    return -1;
  }

  struct espconn *pEspconn = pSocketData->pEspconn;
  pEspconn->type      = ESPCONN_TCP;
  pEspconn->state     = ESPCONN_NONE;
  pEspconn->proto.tcp = (esp_tcp *)os_zalloc(sizeof(esp_tcp));
  assert(pEspconn->proto.tcp != NULL);
  pEspconn->proto.tcp->remote_port = port;
  pEspconn->reverse   = pSocketData;
  espconn_set_opt(pEspconn, ESPCONN_NODELAY); // disable nagle, don't need the extra delay

  if (ipAddress == (uint32_t)-1) {
    // We need DNS resolution, kick it off
    DBG("  resolving: %s\n", savedHostname);
    int rc = espconn_gethostbyname(pEspconn, savedHostname,
        (void*)&pEspconn->proto.tcp->remote_ip, dnsFoundCallback);
    pSocketData->state = SOCKET_STATE_HOST_RESOLVING;
    return pSocketData->socketId;
  } else {
    // No DNS resolution needed, go right ahead
    *(uint32_t *)&pEspconn->proto.tcp->remote_ip = ipAddress;
    return connectSocket(pSocketData);
  }
}

/**
 * Continue creating a socket, the name resolution having completed
 */
static int connectSocket(
    struct socketData *pSocketData //!< Allocated socket data structure
) {
  struct espconn *pEspconn = pSocketData->pEspconn;
  bool isServer = *(uint32_t *)&pEspconn->proto.tcp->remote_ip == 0;

  int newSocket = pSocketData->socketId;
  assert(pSocketData->rxBuf == NULL);
  assert(pSocketData->rxBufLen == 0);
  assert(pSocketData->currentTx == NULL);

  // If we are a client
  if (!isServer) {
    pSocketData->state = SOCKET_STATE_CONNECTING;
    pSocketData->creationType = SOCKET_CREATED_OUTBOUND;

    espconn_regist_connectcb(pEspconn, esp8266_callback_connectCB_outbound);
    espconn_regist_disconcb(pEspconn, esp8266_callback_disconnectCB);
    espconn_regist_reconcb(pEspconn, esp8266_callback_reconnectCB);
    espconn_regist_sentcb(pEspconn, esp8266_callback_sentCB);
    espconn_regist_recvcb(pEspconn, esp8266_callback_recvCB);

    // Make a call to espconn_connect.
    int rc = espconn_connect(pEspconn);
    if (rc != 0) {
      DBG("net_ESP8266_BOARD_createSocketCont: Error %d. Using local port: %d\n",
          rc, pEspconn->proto.tcp->local_port);
      setSocketInError(newSocket, "connect error", rc);
    }
  }

  // If the ipAddress IS 0 ... then we are a server.
  else {
    // We are going to set ourselves up as a server
    pSocketData->state        = SOCKET_STATE_IDLE;
    pSocketData->creationType = SOCKET_CREATED_SERVER;
    pEspconn->proto.tcp->local_port = pEspconn->proto.tcp->remote_port;
    pEspconn->proto.tcp->remote_port = 0;

    espconn_regist_connectcb(pEspconn, esp8266_callback_connectCB_inbound);

    // Make a call to espconn_accept (this should really be called espconn_listen, sigh)
    int rc = espconn_accept(pEspconn);
    if (rc != 0) {
      DBG("net_ESP8266_BOARD_createSocket: Error %d. Using local port: %d\n",
          rc, pEspconn->proto.tcp->local_port);
      setSocketInError(newSocket, "listen error", rc);
    }
  }

  //dumpEspConn(pEspconn);
  DBGV("< net_ESP8266_BOARD_createSocket, socket=%d\n", newSocket);
  return newSocket;
}

/**
 * Close a socket.
 * This gets called in two situations: when the user requests the close of a socket and as
 * an acknowledgment after we signal the socket library that a connection has closed by
 * returning -1 to a send or recv call.
 */
void net_ESP8266_BOARD_closeSocket(
    JsNetwork *net, //!< The Network we are going to use to create the socket.
    int socketId    //!< The socket to be closed.
  ) {
  DBGV("> net_ESP8266_BOARD_closeSocket, socket=%d\n", socketId);

  struct socketData *pSocketData = getSocketData(socketId);
  assert(pSocketData != NULL); // We had better have found a socket to be closed.
  assert(pSocketData->state != SOCKET_STATE_UNUSED);  // Shouldn't be closing an unused socket.
  assert(pSocketData->state != SOCKET_STATE_DISCONNECTING);// Shouldn't be closing an unused socket.

  if (pSocketData->state == SOCKET_STATE_CLOSED ||
      pSocketData->state == SOCKET_STATE_ERROR)
  {
    // In these states we have already freed the espconn structures, so all that's left is to
    // free the socket structure
    releaseSocket(pSocketData->socketId);

  } else {
    // Looks like this is the user telling us to close a connection, let's do it.
    doClose(socketId);
  }

  // FIXME: we need to somehow propagate any error that has occurred, but it's not clear how!

  DBGV("< net_ESP8266_BOARD_closeSocket\n");
}


