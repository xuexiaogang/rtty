#include <mongoose.h>
#include <syslog.h>
#include <pty.h>
#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <signal.h>
#include "list.h"

void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen);

#define MAX_HTTP_AUTH	5
const char *http_auth[MAX_HTTP_AUTH] = {"xterminal:xterminal"};

static struct mg_serve_http_opts http_server_opts = {
	.index_files = "xterminal.html",
	.document_root = "www"
};

#define HTTP_SESSION_COOKIE_NAME "mgs"
/* In our example sessions are destroyed after 30 seconds of inactivity. */
#define HTTP_SESSION_TTL 30.0

/* HTTP Session information structure. */
struct http_session {
	/* Session ID. Must be unique and hard to guess. */
	uint64_t id;
	/*
	* Time when the session was created and time of last activity.
	* Used to clean up stale sessions.
	*/
	double created;
	double last_used; /* Time when the session was last active. */

	/* User name this session is associated with. */
	char *username;
	
	struct list_head node;
};

LIST_HEAD(http_sessions); /* HTTP Session list */

static int check_pass(const char *username, const char *password)
{
	int i = 0;
	char buf[128];
	
	snprintf(buf, sizeof(buf), "%s:%s", username, password);
	for (i = 0; i < MAX_HTTP_AUTH; i++) {
		if (http_auth[i] && !strcmp(http_auth[i], buf))
			return 1;
	}
	
	return 0;
}


/*
 * Parses the session cookie and returns a pointer to the session struct
 * or NULL if not found.
 */
static struct http_session *get_http_session(struct http_message *hm)
{
	char ssid[21];
	uint64_t sid;
	struct http_session *s;
	struct mg_str *cookie_header = mg_get_http_header(hm, "cookie");
	
	if (cookie_header == NULL)
		return NULL;
	
	if (!mg_http_parse_header(cookie_header, HTTP_SESSION_COOKIE_NAME, ssid, sizeof(ssid)))
		return NULL;
	
	sid = strtoull(ssid, NULL, 16);
	
	list_for_each_entry(s, &http_sessions, node) {
		if (s->id == sid) {
			s->last_used = mg_time();
			return s;
		}
	}
	
	return NULL;
}

/* Destroys the session state. */
static void destroy_http_session(struct http_session *s)
{
	list_del(&s->node);
	free(s->username);
	free(s);
}

/* Creates a new http session for the user. */
static struct http_session *create_http_session(const char *username, const struct http_message *hm)
{
	unsigned char digest[20];
	/* Find first available slot or use the oldest one. */
	struct http_session *s = calloc(1, sizeof(struct http_session));
	if (!s)
		return NULL;
	
	/* Initialize new session. */
	s->created = s->last_used = mg_time();
	s->username = strdup(username);
	
	/* Create an ID by putting various volatiles into a pot and stirring. */
	cs_sha1_ctx ctx;
	cs_sha1_init(&ctx);
	cs_sha1_update(&ctx, (const unsigned char *)hm->message.p, hm->message.len);
	cs_sha1_update(&ctx, (const unsigned char *)s, sizeof(*s));
	
	cs_sha1_final(digest, &ctx);
	s->id = *((uint64_t *)digest);

	list_add(&s->node, &http_sessions);
	
	return s;
}

static int http_login(struct mg_connection *nc, struct http_message *hm)
{
	struct http_session *s;
	struct mg_str *uri = &hm->uri;
	
	if (memmem(uri->p, uri->len, ".js", 3) || memmem(uri->p, uri->len, ".css", 3))
		return 1;
	
	if (!mg_vcmp(uri, "/login.html")) {
		int ul, pl;
		char username[50], password[50];
		
		if (mg_vcmp(&hm->method, "POST"))
			return 1;
		
		ul = mg_get_http_var(&hm->body, "username", username, sizeof(username));
		pl = mg_get_http_var(&hm->body, "password", password, sizeof(password));
		
		if (ul > 0 && pl > 0) {
			if (check_pass(username, password)) {
				struct http_session *s = create_http_session(username, hm);
				char shead[100];

				if (!s) {
					mg_http_send_error(nc, 503, NULL);
					return 0;
				}
				
				snprintf(shead, sizeof(shead), "Set-Cookie: %s=%" INT64_X_FMT "; path=/", HTTP_SESSION_COOKIE_NAME, s->id);
				mg_http_send_redirect(nc, 302, mg_mk_str("/"), mg_mk_str(shead));
				return 0;
			}
		}
	}

	s = get_http_session(hm);
	if (!s) {
		mg_http_send_redirect(nc, 302, mg_mk_str("/login.html"), mg_mk_str(""));
		return 0;
	}
	
	return 1;
}

static void http_ev_handler(struct mg_connection *nc, int ev, void *ev_data)
{
	switch (ev) {
	case MG_EV_HTTP_REQUEST: {
			struct http_message *hm = (struct http_message *)ev_data;
			if (!http_login(nc, hm))
				return;
			
			mg_serve_http(nc, hm, http_server_opts); /* Serve static content */
			break;
		}
	}
}

static void http_session_timer_cb(struct ev_loop *loop, ev_timer *w, int revents)
{
	struct http_session *s, *tmp;
	double threshold = mg_time() - HTTP_SESSION_TTL;
	
	list_for_each_entry_safe(s, tmp, &http_sessions, node) {
		if (s->id && s->last_used < threshold) {
			printf("session timeoud:%"INT64_X_FMT"\n", s->id);
			destroy_http_session(s);
		}
	}
}

static void mqtt_ev_handler(struct mg_connection *nc, int ev, void *ev_data)
{
	switch (ev) {
	case MG_EV_CONNECT: {
			struct mg_send_mqtt_handshake_opts opts;
			int err = *(int *)ev_data;
			char client_id[32] = "";
			if (err) {
				syslog(LOG_ERR, "connect() failed: %s", strerror(err));
				return;
			}
			
			memset(&opts, 0, sizeof(opts));
			opts.flags |= MG_MQTT_CLEAN_SESSION;

			snprintf(client_id, sizeof(client_id), "xterminal:%f", mg_time());
			
			mg_set_protocol_mqtt(nc);
			mg_send_mqtt_handshake_opt(nc, client_id, opts);
			break;
		}

	case MG_EV_MQTT_CONNACK: {

			break;
		}

	case MG_EV_MQTT_PUBLISH: {

			
			break;
		}
	}
}

static void signal_cb(struct ev_loop *loop, ev_signal *w, int revents)
{
	ev_break(loop, EVBREAK_ALL);
}

static  void usage(const char *program)
{
	printf("Usage:%s [options]\n", program);
	printf("     -d              Log to stderr\n"
        "     --mqtt-port     default is 1883\n"
        "     --http-port     default is 8443\n"
        "     --document      default is ./www\n"
        "     --http-auth     set http auth(username:password), default is xterminal:xterminal\n"
        "     --ssl-cert      default is ./server.pem\n"
        "     --ssl-key       default is ./server.key\n");
	
	exit(0);
}

int main(int argc, char *argv[])
{
	struct ev_loop *loop = EV_DEFAULT;
	ev_signal sig_watcher;
	int log_to_stderr = 0;
	const char *mqtt_port = "1883", *http_port = "8443";
	const char *ssl_cert = "server.pem", *ssl_key = "server.key";
	int http_auth_cnt = 1;
	struct mg_bind_opts bind_opts;
	static ev_timer http_session_timer;
	
	struct mg_mgr mgr;
	struct mg_connection *nc;
	struct option longopts[] = {
		{"help",  no_argument, NULL, 'h'},
		{"mqtt-port", required_argument, NULL, 0},
		{"http-port", required_argument, NULL, 0},
		{"document", required_argument, NULL, 0},
		{"http-auth", required_argument, NULL, 0},
		{"ssl-cert", required_argument, NULL, 0},
		{"ssl-key", required_argument, NULL, 0},
		{0, 0, 0, 0}
	};
	
	while (1) {
		int c, option_index;
		c = getopt_long(argc, argv, "hd", longopts, &option_index);
		if (c == -1)
			break;
		
		switch (c) {
		case 'd':
			log_to_stderr = 1;
			break;
		case 0:
			if (!strcmp(longopts[option_index].name, "mqtt-port"))
				mqtt_port = optarg;
			else if (!strcmp(longopts[option_index].name, "http-port"))
				http_port = optarg;
			else if (!strcmp(longopts[option_index].name, "document"))
				http_server_opts.document_root = optarg;
			else if (!strcmp(longopts[option_index].name, "http-auth")) {
				if (http_auth_cnt < MAX_HTTP_AUTH)
					http_auth[http_auth_cnt++] = optarg;
			} else if (!strcmp(longopts[option_index].name, "ssl-cert"))
				ssl_cert = optarg;
			else if (!strcmp(longopts[option_index].name, "ssl-key"))
				ssl_key = optarg;
			break;
		default:
			usage(argv[0]);
			break;
		}
	}
	
	if (log_to_stderr)
		openlog("xterminal broker", LOG_ODELAY | LOG_PERROR, LOG_USER);
	else
		openlog("xterminal broker", LOG_ODELAY, LOG_USER);
	
	mg_mgr_init(&mgr, NULL, loop);
	
	ev_signal_init(&sig_watcher, signal_cb, SIGINT);
	ev_signal_start(loop, &sig_watcher);

	nc = mg_connect(&mgr, mqtt_port, mqtt_ev_handler);
	if (!nc) {
		syslog(LOG_ERR, "mg_connect(%s) failed", mqtt_port);
		goto err;
	}
	
	memset(&bind_opts, 0, sizeof(bind_opts));
	bind_opts.ssl_cert = ssl_cert;
	bind_opts.ssl_key = ssl_key;
	
	nc = mg_bind_opt(&mgr, http_port, http_ev_handler, bind_opts);
	if (nc == NULL) {
		syslog(LOG_ERR, "Failed to create listener on %s", http_port);
		goto err;
	}
	
	/* Set up HTTP server parameters */
	mg_set_protocol_http_websocket(nc);
	
	ev_timer_init(&http_session_timer, http_session_timer_cb, 5, 5);
	ev_timer_start(loop, &http_session_timer);
	
	ev_run(loop, 0);

err:
	mg_mgr_free(&mgr);

	return 0;
}