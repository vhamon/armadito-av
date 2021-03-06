/***

Copyright (C) 2015, 2016 Teclib'

This file is part of Armadito core.

Armadito core is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Armadito core is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Armadito core.  If not, see <http://www.gnu.org/licenses/>.

***/

#include <jansson.h>
#include <glib.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <rpc/io.h>
#include <rpc/rpctypes.h>
#include <libjrpc/jrpc.h>
#include <net/unixsockclient.h>
#include <net/netdefaults.h>

#include "apiclient.h"

struct api_client {
	GAsyncQueue *event_queue;
	struct jrpc_connection *conn;
	int client_sock;
	int done;
};

struct api_client *api_client_new(void)
{
	struct api_client *client = malloc(sizeof(struct api_client));

	client->event_queue = g_async_queue_new();
	client->conn = NULL;
	client->client_sock = -1;
	client->done = 0;

	return client;
}

void api_client_free(struct api_client *client)
{
	g_async_queue_unref(client->event_queue);

	free((void *)client);
}

static int notify_event_method(struct jrpc_connection *conn, json_t *params, json_t **result)
{
	struct api_client *client = (struct api_client *)jrpc_connection_get_data(conn);
	struct a6o_event *ev;
	int ret;

	if ((ret = JRPC_JSON2STRUCT(a6o_event, params, &ev)))
		return ret;

	api_client_push_event(client, params);

	switch(ev->type) {
	case EVENT_ON_DEMAND_COMPLETED:
		client->done = 1;
		break;
	}

	return JRPC_OK;
}

static struct jrpc_mapper *create_rpcfe_mapper(void)
{
	struct jrpc_mapper *rpcfe_mapper;

	rpcfe_mapper = jrpc_mapper_new();
	jrpc_mapper_add(rpcfe_mapper, "notify_event", notify_event_method);

	return rpcfe_mapper;
}

static gpointer api_client_thread(gpointer data)
{
	struct api_client *client = (struct api_client *)data;
	int ret;

	while((ret = jrpc_process(client->conn)) != JRPC_EOF && !client->done)
		;

	if (close(client->client_sock) < 0)
		perror("closing connection");

	return NULL;
}

int api_client_connect(struct api_client *client)
{
	int client_sock;

	client_sock = unix_client_connect(DEFAULT_SOCKET_PATH, 10);

	if (client_sock < 0) {
		perror("cannot connect");
		return -1;
	}

	client->client_sock = client_sock;

	client->conn = jrpc_connection_new(create_rpcfe_mapper(), client);

	jrpc_connection_set_read_cb(client->conn, unix_fd_read_cb, &client->client_sock);
	jrpc_connection_set_write_cb(client->conn, unix_fd_write_cb, &client->client_sock);

	g_thread_new("api client thread", api_client_thread, client);

	return 0;
}

struct jrpc_connection *api_client_get_connection(struct api_client *client)
{
	return client->conn;
}

void api_client_push_event(struct api_client *client, json_t *event)
{
	g_async_queue_push(client->event_queue, event);
}

void api_client_pop_event(struct api_client *client, json_t **p_event)
{
	*p_event = g_async_queue_pop(client->event_queue);
}

