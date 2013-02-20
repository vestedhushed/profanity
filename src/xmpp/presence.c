/*
 * presence.c
 *
 * Copyright (C) 2012, 2013 James Booth <boothj5@gmail.com>
 *
 * This file is part of Profanity.
 *
 * Profanity is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Profanity is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Profanity.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "common.h"
#include "config/preferences.h"
#include "log.h"
#include "muc.h"
#include "profanity.h"
#include "xmpp/capabilities.h"
#include "xmpp/connection.h"
#include "xmpp/stanza.h"
#include "xmpp/xmpp.h"

static GHashTable *sub_requests;

#define HANDLE(ns, type, func) xmpp_handler_add(conn, func, ns, \
                                                STANZA_NAME_PRESENCE, type, ctx)

static int _unavailable_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _subscribe_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _subscribed_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _unsubscribed_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);
static int _available_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);

static int _room_presence_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata);

static char* _get_caps_key(xmpp_stanza_t * const stanza);
static void _send_room_presence(xmpp_conn_t *conn, xmpp_stanza_t *presence);

void
presence_init(void)
{
    sub_requests =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

void
presence_add_handlers(void)
{
    xmpp_conn_t * const conn = connection_get_conn();
    xmpp_ctx_t * const ctx = connection_get_ctx();

    HANDLE(NULL,               STANZA_TYPE_ERROR,        connection_error_handler);
    HANDLE(STANZA_NS_MUC_USER, NULL,                     _room_presence_handler);
    HANDLE(NULL,               STANZA_TYPE_UNAVAILABLE,  _unavailable_handler);
    HANDLE(NULL,               STANZA_TYPE_SUBSCRIBE,    _subscribe_handler);
    HANDLE(NULL,               STANZA_TYPE_SUBSCRIBED,   _subscribed_handler);
    HANDLE(NULL,               STANZA_TYPE_UNSUBSCRIBED, _unsubscribed_handler);
    HANDLE(NULL,               NULL,                     _available_handler);
}

void
presence_subscription(const char * const jid, const jabber_subscr_t action)
{
    assert(jid != NULL);

    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_conn_t * const conn = connection_get_conn();
    const char *type = NULL;

    Jid *jidp = jid_create(jid);
    g_hash_table_remove(sub_requests, jidp->barejid);

    switch (action)
    {
        case PRESENCE_SUBSCRIBE:
            log_debug("Sending presence subscribe: %s", jid);
            type = STANZA_TYPE_SUBSCRIBE;
            break;
        case PRESENCE_SUBSCRIBED:
            log_debug("Sending presence subscribed: %s", jid);
            type = STANZA_TYPE_SUBSCRIBED;
            break;
        case PRESENCE_UNSUBSCRIBED:
            log_debug("Sending presence usubscribed: %s", jid);
            type = STANZA_TYPE_UNSUBSCRIBED;
            break;
        default:
            log_warning("Attempt to send unknown subscription action: %s", jid);
            break;
    }

    xmpp_stanza_t *presence = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(presence, STANZA_NAME_PRESENCE);
    xmpp_stanza_set_type(presence, type);
    xmpp_stanza_set_attribute(presence, STANZA_ATTR_TO, jidp->barejid);
    xmpp_send(conn, presence);
    xmpp_stanza_release(presence);

    jid_destroy(jidp);
}

GList *
presence_get_subscription_requests(void)
{
    return g_hash_table_get_keys(sub_requests);
}

void
presence_free_sub_requests(void)
{
    if (sub_requests != NULL) {
        g_hash_table_remove_all(sub_requests);
    }
}

void
presence_update(const resource_presence_t presence_type, const char * const msg,
    const int idle)
{
    if (jabber_get_connection_status() != JABBER_CONNECTED) {
        log_warning("Error setting presence, not connected.");
        return;
    }

    if (msg != NULL) {
        log_debug("Updating presence: %s, \"%s\"",
            string_from_resource_presence(presence_type), msg);
    } else {
        log_debug("Updating presence: %s",
            string_from_resource_presence(presence_type));
    }

    xmpp_ctx_t * const ctx = connection_get_ctx();
    xmpp_conn_t * const conn = connection_get_conn();
    const int pri =
        accounts_get_priority_for_presence_type(jabber_get_account_name(),
                                                presence_type);
    const char *show = stanza_get_presence_string_from_type(presence_type);

    connection_set_presence_message(msg);
    connection_set_priority(pri);

    xmpp_stanza_t *presence = stanza_create_presence(ctx);
    stanza_attach_show(ctx, presence, show);
    stanza_attach_status(ctx, presence, msg);
    stanza_attach_priority(ctx, presence, pri);
    stanza_attach_last_activity(ctx, presence, idle);
    stanza_attach_caps(ctx, presence);
    xmpp_send(conn, presence);
    _send_room_presence(conn, presence);
    xmpp_stanza_release(presence);

    // set last presence for account
    const char *last = show;
    if (last == NULL) {
        last = STANZA_TEXT_ONLINE;
    }
    accounts_set_last_presence(jabber_get_account_name(), last);
}

static void
_send_room_presence(xmpp_conn_t *conn, xmpp_stanza_t *presence)
{
    GList *rooms = muc_get_active_room_list();
    while (rooms != NULL) {
        const char *room = rooms->data;
        const char *nick = muc_get_room_nick(room);
        const char *full_room_jid = create_fulljid(room, nick);

        xmpp_stanza_set_attribute(presence, STANZA_ATTR_TO, full_room_jid);
        log_debug("Sending presence to room: %s", full_room_jid);
        xmpp_send(conn, presence);

        rooms = g_list_next(rooms);
    }
    g_list_free(rooms);
}

void
presence_join_room(Jid *jid)
{
    assert(jid != NULL);
    assert(jid->fulljid != NULL);

    log_debug("Sending room join presence to: %s", jid->fulljid);
    xmpp_ctx_t *ctx = connection_get_ctx();
    xmpp_conn_t *conn = connection_get_conn();
    contact_presence_t presence_type =
        accounts_get_last_presence(jabber_get_account_name());
    const char *show = stanza_get_presence_string_from_type(presence_type);
    char *status = jabber_get_presence_message();
    int pri = accounts_get_priority_for_presence_type(jabber_get_account_name(),
        presence_type);

    xmpp_stanza_t *presence = stanza_create_room_join_presence(ctx, jid->fulljid);
    stanza_attach_show(ctx, presence, show);
    stanza_attach_status(ctx, presence, status);
    stanza_attach_priority(ctx, presence, pri);
    stanza_attach_caps(ctx, presence);

    xmpp_send(conn, presence);
    xmpp_stanza_release(presence);

    muc_join_room(jid->barejid, jid->resourcepart);
}

void
presence_change_room_nick(const char * const room, const char * const nick)
{
    assert(room != NULL);
    assert(nick != NULL);

    log_debug("Sending room nickname change to: %s, nick: %s", room, nick);
    xmpp_ctx_t *ctx = connection_get_ctx();
    xmpp_conn_t *conn = connection_get_conn();
    contact_presence_t presence_type =
        accounts_get_last_presence(jabber_get_account_name());
    const char *show = stanza_get_presence_string_from_type(presence_type);
    char *status = jabber_get_presence_message();
    int pri = accounts_get_priority_for_presence_type(jabber_get_account_name(),
        presence_type);

    char *full_room_jid = create_fulljid(room, nick);
    xmpp_stanza_t *presence =
        stanza_create_room_newnick_presence(ctx, full_room_jid);
    stanza_attach_show(ctx, presence, show);
    stanza_attach_status(ctx, presence, status);
    stanza_attach_priority(ctx, presence, pri);
    stanza_attach_caps(ctx, presence);

    xmpp_send(conn, presence);
    xmpp_stanza_release(presence);

    free(full_room_jid);
}

void
presence_leave_chat_room(const char * const room_jid)
{
    assert(room_jid != NULL);

    log_debug("Sending room leave presence to: %s", room_jid);
    xmpp_ctx_t *ctx = connection_get_ctx();
    xmpp_conn_t *conn = connection_get_conn();
    char *nick = muc_get_room_nick(room_jid);

    xmpp_stanza_t *presence = stanza_create_room_leave_presence(ctx, room_jid,
        nick);
    xmpp_send(conn, presence);
    xmpp_stanza_release(presence);
}

static int
_unsubscribed_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata)
{
    char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    Jid *from_jid = jid_create(from);
    log_debug("Unsubscribed presence handler fired for %s", from);

    prof_handle_subscription(from_jid->barejid, PRESENCE_UNSUBSCRIBED);
    g_hash_table_remove(sub_requests, from_jid->barejid);

    jid_destroy(from_jid);

    return 1;
}

static int
_subscribed_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata)
{
    char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    Jid *from_jid = jid_create(from);
    log_debug("Subscribed presence handler fired for %s", from);

    prof_handle_subscription(from_jid->barejid, PRESENCE_SUBSCRIBED);
    g_hash_table_remove(sub_requests, from_jid->barejid);

    jid_destroy(from_jid);

    return 1;
}

static int
_subscribe_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata)
{
    char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    Jid *from_jid = jid_create(from);
    log_debug("Subscribe presence handler fired for %s", from);

    prof_handle_subscription(from_jid->barejid, PRESENCE_SUBSCRIBE);
    g_hash_table_insert(sub_requests, strdup(from_jid->barejid),
        strdup(from_jid->barejid));

    jid_destroy(from_jid);

    return 1;
}

static int
_unavailable_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata)
{
    const char *jid = xmpp_conn_get_jid(conn);
    char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    log_debug("Unavailable presence handler fired for %s", from);

    Jid *my_jid = jid_create(jid);
    Jid *from_jid = jid_create(from);

    char *status_str;
    xmpp_stanza_t *status =
        xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_STATUS);

    if (status != NULL)
        status_str = xmpp_stanza_get_text(status);
    else
        status_str = NULL;

    if (strcmp(my_jid->barejid, from_jid->barejid) !=0) {
        prof_handle_contact_offline(from_jid->barejid, from_jid->resourcepart, status_str);
/*
    } else {
        connection_remove_available_resource(from_jid->resourcepart);
*/
    }


    jid_destroy(my_jid);
    jid_destroy(from_jid);

    return 1;
}

static int
_available_handler(xmpp_conn_t * const conn,
    xmpp_stanza_t * const stanza, void * const userdata)
{
    // handler still fires if error
    if (g_strcmp0(xmpp_stanza_get_type(stanza), STANZA_TYPE_ERROR) == 0) {
        return 1;
    }

    // handler still fires if other types
    if ((g_strcmp0(xmpp_stanza_get_type(stanza), STANZA_TYPE_UNAVAILABLE) == 0) ||
            (g_strcmp0(xmpp_stanza_get_type(stanza), STANZA_TYPE_SUBSCRIBE) == 0) ||
            (g_strcmp0(xmpp_stanza_get_type(stanza), STANZA_TYPE_SUBSCRIBED) == 0) ||
            (g_strcmp0(xmpp_stanza_get_type(stanza), STANZA_TYPE_UNSUBSCRIBED) == 0)) {
        return 1;
    }

    // handler still fires for muc presence
    if (stanza_is_muc_presence(stanza)) {
        return 1;
    }

    const char *jid = xmpp_conn_get_jid(conn);
    char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    log_debug("Available presence handler fired for %s", from);

    Jid *my_jid = jid_create(jid);
    Jid *from_jid = jid_create(from);

    char *show_str, *status_str;
    char *caps_key = _get_caps_key(stanza);
    int idle_seconds = stanza_get_idle_time(stanza);
    GDateTime *last_activity = NULL;

    if (idle_seconds > 0) {
        GDateTime *now = g_date_time_new_now_local();
        last_activity = g_date_time_add_seconds(now, 0 - idle_seconds);
        g_date_time_unref(now);
    }

    xmpp_stanza_t *show = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_SHOW);

    if (show != NULL)
        show_str = xmpp_stanza_get_text(show);
    else
        show_str = "online";

    xmpp_stanza_t *status =
        xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_STATUS);

    if (status != NULL)
        status_str = xmpp_stanza_get_text(status);
    else
        status_str = NULL;

    // get priority
    int priority = 0;
    xmpp_stanza_t *priority_stanza =
        xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_PRIORITY);

    if (priority_stanza != NULL) {
        char *priority_str = xmpp_stanza_get_text(priority_stanza);
        if (priority_str != NULL) {
            priority = atoi(priority_str);
        }
    }

    // if not self presence (TODO allow self presence from other resources?)
    if (strcmp(my_jid->barejid, from_jid->barejid) !=0) {
        // create the resource
        resource_presence_t presence = resource_presence_from_string(show_str);
        Resource *resource = resource_new(from_jid->resourcepart, presence,
            status_str, priority, caps_key);
        prof_handle_contact_online(from_jid->barejid, resource, last_activity);
/*
    } else {
        // handle self presence
        resource_presence_t presence = resource_presence_from_string(show_str);
        Resource *resource = resource_new(from_jid->resourcepart, presence,
            status_str, priority, caps_key);
        connection_add_available_resource(resource);
*/
    }


    jid_destroy(my_jid);
    jid_destroy(from_jid);

    if (last_activity != NULL) {
        g_date_time_unref(last_activity);
    }

    return 1;
}


static char *
_get_caps_key(xmpp_stanza_t * const stanza)
{
    xmpp_ctx_t *ctx = connection_get_ctx();
    xmpp_conn_t *conn = connection_get_conn();
    char *caps_key = NULL;
    char *node = NULL;
    char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    if (stanza_contains_caps(stanza)) {
        log_debug("Presence contains capabilities.");
        char *hash_type = stanza_caps_get_hash(stanza);

        // xep-0115
        if (hash_type != NULL) {
            log_debug("Hash type: %s", hash_type);

            // supported hash
            if (strcmp(hash_type, "sha-1") == 0) {
                log_debug("Hash type supported.");
                node = stanza_get_caps_str(stanza);
                caps_key = node;

                if (node != NULL) {
                    log_debug("Node string: %s.", node);
                    if (!caps_contains(caps_key)) {
                        log_debug("Capabilities not cached for '%s', sending discovery IQ.", caps_key);
                        xmpp_stanza_t *iq = stanza_create_disco_iq(ctx, "disco", from, node);
                        xmpp_send(conn, iq);
                        xmpp_stanza_release(iq);
                    } else {
                        log_debug("Capabilities already cached, for %s", caps_key);
                    }
                } else {
                    log_debug("No node string, not sending discovery IQ.");
                }

            // unsupported hash
            } else {
                log_debug("Hash type unsupported.");
                node = stanza_get_caps_str(stanza);
                caps_key = from;

                if (node != NULL) {
                    log_debug("Node string: %s.", node);
                    if (!caps_contains(caps_key)) {
                        log_debug("Capabilities not cached for '%s', sending discovery IQ.", caps_key);
                        GString *id = g_string_new("disco_");
                        g_string_append(id, from);
                        xmpp_stanza_t *iq = stanza_create_disco_iq(ctx, id->str, from, node);
                        xmpp_send(conn, iq);
                        xmpp_stanza_release(iq);
                        g_string_free(id, TRUE);
                    } else {
                        log_debug("Capabilities already cached, for %s", caps_key);
                    }
                } else {
                    log_debug("No node string, not sending discovery IQ.");
                }
            }

            return strdup(caps_key);

        //ignore or handle legacy caps
        } else {
            log_debug("No hash type, using legacy capabilities.");
            node = stanza_get_caps_str(stanza);
            caps_key = from;

            if (node != NULL) {
                log_debug("Node string: %s.", node);
                if (!caps_contains(caps_key)) {
                    log_debug("Capabilities not cached for '%s', sending discovery IQ.", caps_key);
                    GString *id = g_string_new("disco_");
                    g_string_append(id, from);
                    xmpp_stanza_t *iq = stanza_create_disco_iq(ctx, id->str, from, node);
                    xmpp_send(conn, iq);
                    xmpp_stanza_release(iq);
                    g_string_free(id, TRUE);
                } else {
                    log_debug("Capabilities already cached, for %s", caps_key);
                }
            } else {
                log_debug("No node string, not sending discovery IQ.");
            }

            return caps_key;
        }
    }
    return NULL;
}

static int
_room_presence_handler(xmpp_conn_t * const conn, xmpp_stanza_t * const stanza,
    void * const userdata)
{
    // handler still fires if error
    if (g_strcmp0(xmpp_stanza_get_type(stanza), STANZA_TYPE_ERROR) == 0) {
        return 1;
    }

    const char *jid = xmpp_conn_get_jid(conn);
    char *from = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_FROM);
    Jid *my_jid = jid_create(jid);
    Jid *from_jid = jid_create(from);

    char *room = from_jid->barejid;
    char *nick = from_jid->resourcepart;

    // handle self presence
    if (stanza_is_muc_self_presence(stanza, jabber_get_jid())) {
        char *type = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_TYPE);
        gboolean nick_change = stanza_is_room_nick_change(stanza);

        if ((type != NULL) && (strcmp(type, STANZA_TYPE_UNAVAILABLE) == 0)) {

            // leave room if not self nick change
            if (nick_change) {
                muc_set_room_pending_nick_change(room);
            } else {
                prof_handle_leave_room(room);
            }

        // handle self nick change
        } else if (muc_is_room_pending_nick_change(room)) {
            muc_complete_room_nick_change(room, nick);
            prof_handle_room_nick_change(room, nick);

        // handle roster complete
        } else if (!muc_get_roster_received(room)) {
            prof_handle_room_roster_complete(room);

        }

    // handle presence from room members
    } else {
        char *type = xmpp_stanza_get_attribute(stanza, STANZA_ATTR_TYPE);
        char *show_str, *status_str;
        char *caps_key = _get_caps_key(stanza);

        log_debug("Room presence received from %s", from_jid->fulljid);

        xmpp_stanza_t *status = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_STATUS);
        if (status != NULL) {
            status_str = xmpp_stanza_get_text(status);
        } else {
            status_str = NULL;
        }

        if ((type != NULL) && (strcmp(type, STANZA_TYPE_UNAVAILABLE) == 0)) {

            // handle nickname change
            if (stanza_is_room_nick_change(stanza)) {
                char *new_nick = stanza_get_new_nick(stanza);
                muc_set_roster_pending_nick_change(room, new_nick, nick);
            } else {
                prof_handle_room_member_offline(room, nick, "offline", status_str);
            }
        } else {
            xmpp_stanza_t *show = xmpp_stanza_get_child_by_name(stanza, STANZA_NAME_SHOW);
            if (show != NULL) {
                show_str = xmpp_stanza_get_text(show);
            } else {
                show_str = "online";
            }
            if (!muc_get_roster_received(room)) {
                muc_add_to_roster(room, nick, show_str, status_str, caps_key);
            } else {
                char *old_nick = muc_complete_roster_nick_change(room, nick);

                if (old_nick != NULL) {
                    muc_add_to_roster(room, nick, show_str, status_str, caps_key);
                    prof_handle_room_member_nick_change(room, old_nick, nick);
                } else {
                    if (!muc_nick_in_roster(room, nick)) {
                        prof_handle_room_member_online(room, nick, show_str, status_str, caps_key);
                    } else {
                        prof_handle_room_member_presence(room, nick, show_str, status_str, caps_key);
                    }
                }
            }
        }
    }

    jid_destroy(my_jid);
    jid_destroy(from_jid);

    return 1;
}
