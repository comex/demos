#include <vector>
#include <deque>
#include <unordered_map>
#include <stdint.h>
#include <random>
#include <future>
#include <mutex>
#include <stdlib.h>

#define USE_EPOLL !__APPLE__

#if USE_EPOLL
#include <sys/epoll.h>
#endif

#include <libwebsockets.h>
#define CMAKE_BUILD
#include <private-libwebsockets.h> // derp

#define NO_VOTE 0xffff
#define UNVOTE_DELAY (60*5)
#define BATCH_SIZE 10
#define WORKERS 4

struct player_data {
	libwebsocket *wsi;
	int fd;
	uint64_t pid;
	size_t voting_players_idx;
	uint16_t vote;
	uint64_t vote_frame;
};

struct queued_event {
	uint64_t frame;
	int fd;
	uint64_t pid;
};

struct popularity_packet {
	uint16_t vote;
	uint32_t popularity;
} __attribute__((packed));

struct batch_packet {
	uint8_t type;
	uint64_t frame;
	uint8_t num_inputs;
	uint16_t inputs[BATCH_SIZE];
	popularity_packet popcnt_data[0];
} __attribute__((packed));

static libwebsocket_context *g_lws_ctx;

static std::default_random_engine g_rand;

static uint64_t g_next_pid;

static uint64_t g_frame;

static std::unordered_map<uint16_t, size_t> g_popularity;

static std::vector<player_data> g_players;
static size_t g_num_players;
static std::mutex g_conn_mtx;

// oh, why not
static std::vector<player_data *> g_voting_players;
static size_t g_voting_players_count;

static std::deque<queued_event> g_events;

static uint16_t g_current_batch[BATCH_SIZE];
static int g_current_batch_idx;

#if USE_EPOLL
static int g_epoll_fd;
#endif


static void disconnect_player(player_data *pl) {
	close(pl->fd);
}


static void set_vote(player_data *pl, uint32_t vote) {
	if(vote == pl->vote)
		return;
	if(pl->vote != NO_VOTE) {
		if(--g_popularity[pl->vote] == 0)
			g_popularity.erase(pl->vote);
	}
	if(vote == NO_VOTE) {
		std::swap(g_voting_players[pl->voting_players_idx], g_voting_players[--g_voting_players_count]);
		g_voting_players.resize(g_voting_players_count);
	} else {
		g_voting_players.push_back(pl);
		g_popularity[vote]++;
	}
	pl->vote = vote;
	pl->vote_frame = g_frame;
	if(vote == 0) { // release
		g_events.push_back(queued_event{g_frame + UNVOTE_DELAY, pl->fd, pl->pid});
	}
}

static uint32_t get_input() {
	if(g_voting_players_count == 0)
		return 0;
	std::uniform_int_distribution<size_t> distr(0, g_voting_players_count - 1);
	size_t lucky = distr(g_rand);
	return g_voting_players[lucky]->vote;
}

static void inc_frame() {
	g_frame++;
	while(g_events.size() > 0 && g_frame >= g_events.front().frame) {
		queued_event *ev = &g_events.front();
		if(ev->fd < g_players.size()) {
			player_data *pl = &g_players[ev->fd];
			if(pl->pid == ev->pid && g_frame == pl->vote_frame + UNVOTE_DELAY) {
				set_vote(pl, NO_VOTE);
			}
		}
		g_events.pop_front();
	}
}

static void scattershot_packet(std::vector<uint8_t>& packet) {
	// In the /extremely/ unlikely case that this becomes TPP-level popular,
	// this is actually going to be eating most of the CPU just popping between
	// userland and kernelland and poking TCP queues.  Whatever.  If I double
	// the batch rate it will almost certainly be OK, so...
	std::future<void> f[4];
	for(int i = 0; i < WORKERS; i++) {
		f[i] = std::async(std::launch::async, [=] {
			for(size_t j = 0; j < g_players.size(); j += WORKERS) {
				player_data *pl = &g_players[j];
				libwebsocket *wsi = pl->wsi;
				if(!wsi)
					continue;
				// Note: Docs say not to do this outside of
				// LWS_CALLBACK_SERVER_WRITEABLE or on multiple threads.  But
				// it doesn't actually matter (if you don't mind sometimes
				// rudely disconnecting people).
				int ret = libwebsocket_write(wsi, (unsigned char *) packet.data(), packet.size(), LWS_WRITE_BINARY);
				if(ret != packet.size())
					disconnect_player(pl);
			}
		});
	}
	for(int i = 0; i < WORKERS; i++)
		f[i].wait();
}

static void do_frame() {
	inc_frame();
	uint32_t input = get_input();
	g_current_batch[g_current_batch_idx++] = input;
	if(g_current_batch_idx == BATCH_SIZE) {
		std::vector<uint8_t> packet;
		// actually, add the WS bullshit here
		packet.resize(LWS_SEND_BUFFER_PRE_PADDING + sizeof(batch_packet) + g_popularity.size() * sizeof(uint16_t) + LWS_SEND_BUFFER_POST_PADDING);
		batch_packet *bp = (batch_packet *) (packet.data() + LWS_SEND_BUFFER_POST_PADDING);
		bp->type = 0;
		bp->frame = g_frame - BATCH_SIZE + 1;
		bp->num_inputs = BATCH_SIZE;
		memcpy(bp->inputs, g_current_batch, sizeof(bp->inputs));
		size_t i = 0;
		for(auto it : g_popularity) {
			bp->popcnt_data[i++] = {it.first, (uint32_t) it.second};
		}
		scattershot_packet(packet);

		g_current_batch_idx = 0;
	}

}

#define please(x) do { if((x) == -1) fprintf(stderr, "%s failed: %s\n", #x, strerror(errno)); } while(0)

static int keyserver_callback(struct libwebsocket_context *context, struct libwebsocket *wsi, enum libwebsocket_callback_reasons reason, void *user, void *in, size_t len) {
#if USE_EPOLL
	libwebsocket_pollargs *pa = (libwebsocket_pollargs *) in;
#endif
	switch(reason) {
#if USE_EPOLL
	case LWS_CALLBACK_ADD_POLL_FD:
		please(epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, pa->fd, (epoll_event[]) {{translate_pa_mode(pa->events), {.ptr = pa}}}));
		break;
	case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
		please(epoll_ctl(g_epoll_fd, EPOLL_CTL_MOD, pa->fd, (epoll_event[]) {{translate_pa_mode(pa->events), {.ptr = pa}}}));
		break;
	case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
		please(epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, pa->fd, NULL));
		break;
#endif
	default:
		break;
	}
	return 0;
}

static int keyserver_admin_callback(struct libwebsocket_context *context, struct libwebsocket *wsi, enum libwebsocket_callback_reasons reason, void *user, void *in, size_t len) {

	return 0;
}

static void serve() {
#if USE_EPOLL
	struct epoll_event events[16];
	int ready = epoll_wait(g_epoll_fd, events, sizeof(events)/sizeof(*events), 4) == -1;
	if(ready == -1) {
		perror("epoll_wait");
		exit(1);
	}
	for(int i = 0; i < ready; i++) {
		auto pa = (libwebsocket_pollargs *) events[i].data.ptr;
		please(libwebsocket_service_fd(g_lws_ctx, pa));
	}
#else
	please(libwebsocket_service(g_lws_ctx, 4));
#endif
}

int main() {
	g_rand = std::default_random_engine(std::random_device()());
	g_lws_ctx = libwebsocket_create_context((lws_context_creation_info[]) {{
		.port = 4321,
		.iface = NULL,
		.protocols = (libwebsocket_protocols[]) {
			{
				.name = "keyserver",
				.callback = keyserver_callback,
				.per_session_data_size = 0,
				.rx_buffer_size = 8
			},
			{
				.name = "keyserver-admin",
				.callback = keyserver_admin_callback,
				.per_session_data_size = 0,
				.rx_buffer_size = 120000,
			},
		},
		.gid = -1,
		.uid = -1
	}});
#if USE_EPOLL
	g_epoll_fd = epoll_create(1);
#endif
	while(1) {
		serve();
		// gettimeofday, frame...
	}
}
