
#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <asterisk/lock.h>
#include <asterisk/file.h>
#include <asterisk/logger.h>
#include <asterisk/channel.h>
#include <asterisk/pbx.h>
#include <asterisk/module.h>
#include <asterisk/options.h>
#include <asterisk/logger.h>
#include <asterisk/version.h>

/* FUCK YOU ASTERSISK */
#undef pthread_mutex_t
#undef pthread_mutex_lock
#undef pthread_mutex_unlock
#undef pthread_mutex_trylock
#undef pthread_mutex_init
#undef pthread_mutex_destroy
#undef pthread_cond_t
#undef pthread_cond_init
#undef pthread_cond_destroy
#undef pthread_cond_signal
#undef pthread_cond_broadcast
#undef pthread_cond_wait
#undef pthread_cond_timedwait

#if defined(AST_MODULE_INFO)
#define SANE_ASTERISK_VERSION_NUM 0x00010400
#else
#define SANE_ASTERISK_VERSION_NUM 0x00010200
#endif

#include <libkstreamer.h>

#include <chan_visdn.h>

static char *app = "vISDNppp";
static char *synopsis = "Runs pppd and connects channel to visdn-ppp gateway";

static char *descrip = 
"  vISDNppp(args): Spawns pppd and connects the channel to a newly created\n"
" visdn-ppp channel. pppd must support visdn.so plugin.\n"
"Arguments are passed to pppd and should be separated by | characters.\n"
"Always returns -1.\n";

#define PPP_MAX_ARGS	32
#define PPP_EXEC	"/usr/sbin/pppd"

static int get_max_fds(void)
{
#ifdef OPEN_MAX
	return OPEN_MAX;
#else
	int max;

	max = sysconf(_SC_OPEN_MAX);
	if (max <= 0)
		return 1024;

	return max;
#endif
}

static pid_t spawn_ppp(
	struct ast_channel *chan,
	const char *argv[],
	int argc)
{
	/* Start by forking */
	pid_t pid = fork();
	if (pid)
		return pid;

	close(0);

	int i;
	int max_fds = get_max_fds();
	for (i=STDERR_FILENO + 1; i < max_fds; i++)
		close(i);

	/* Restore original signal handlers */
	for (i=0; i<NSIG; i++)
		signal(i, SIG_DFL);

	/* Finally launch PPP */
	execv(PPP_EXEC, (char * const *)argv);
	fprintf(stderr, "Failed to exec pppd!: %s\n", strerror(errno));
	exit(1);
}


static int visdn_ppp_exec(struct ast_channel *chan, void *data)
{
	int res=-1;
	struct ast_frame *f;

	if (chan->_state != AST_STATE_UP)
		ast_answer(chan);

	ast_mutex_lock(&chan->lock);

#ifdef SANE_ASTERISK_VERSION_NUM
	if (strcmp(chan->tech->type, "VISDN")) {
#else
	if (strcmp(chan->type, "VISDN")) {
#endif
		ast_log(LOG_WARNING,
			"Only VISDN channels may be connected to"
			" this application\n");

		ast_mutex_unlock(&chan->lock);
		return -1;
	}

	struct visdn_chan *visdn_chan = to_visdn_chan(chan);

	if (!visdn_chan->node_bearer) {
		ast_log(LOG_WARNING,
			"vISDN bearer channel not present\n");
		ast_mutex_unlock(&chan->lock);
		return -1;
	}

	const char *argv[PPP_MAX_ARGS] = { };
	int argc = 0;

	argv[argc++] = PPP_EXEC;
	argv[argc++] = "nodetach";

	char *stringp = strdup(data);
	char *arg;
	while((arg = strsep(&stringp, "|"))) {

		if (!strlen(arg))
			break;

		if (argc >= PPP_MAX_ARGS - 4)
			break;

		argv[argc++] = arg;
	}

	char chan_id_arg[10];
	snprintf(chan_id_arg, sizeof(chan_id_arg),
		"%06d", visdn_chan->node_bearer->id);

	argv[argc++] = "plugin";
	argv[argc++] = "visdn.so";
	argv[argc++] = chan_id_arg;

	ast_mutex_unlock(&chan->lock);

#if 0
	int i;
	for (i=0;i<argc;i++) {
		ast_log(LOG_NOTICE, "Arg %d: %s\n", i, argv[i]);
	}
#endif

	signal(SIGCHLD, SIG_DFL);

	pid_t pid = spawn_ppp(chan, argv, argc);
	if (pid < 0) {
		ast_log(LOG_WARNING, "Failed to spawn pppd\n");
		return -1;
	}

	while(ast_waitfor(chan, -1) > -1) {

		f = ast_read(chan);
		if (!f) {
			ast_log(LOG_NOTICE,
				"Channel '%s' hungup."
				" Signalling PPP at %d to die...\n",
				chan->name, pid);

			kill(pid, SIGTERM);

			break;
		}

		ast_frfree(f);

		int status;
		res = wait4(pid, &status, WNOHANG, NULL);
		if (res < 0) {
			ast_log(LOG_WARNING,
				"wait4 returned %d: %s\n",
				res, strerror(errno));

			break;
		} else if (res > 0) {
			if (option_verbose > 2) {
				if (WIFEXITED(status)) {
					ast_verbose(VERBOSE_PREFIX_3
						"PPP on %s terminated with status %d\n",
						chan->name, WEXITSTATUS(status));
				} else if (WIFSIGNALED(status)) {
					ast_verbose(VERBOSE_PREFIX_3
						"PPP on %s terminated with signal %d\n", 
						chan->name, WTERMSIG(status));
				} else {
					ast_verbose(VERBOSE_PREFIX_3
						"PPP on %s terminated weirdly.\n", chan->name);
				}
			}

			break;
		}
	}

	return res;
}

#if SANE_ASTERISK_VERSION_NUM < 0x00010400
int load_module(void)
#else
static int visdn_ppp_load_module(void)
#endif
{
	return ast_register_application(app, visdn_ppp_exec, synopsis, descrip);
}

#if SANE_ASTERISK_VERSION_NUM < 0x00010400
int unload_module(void)
#else
static int visdn_ppp_unload_module(void)
#endif
{
	return ast_unregister_application(app);
}

#if SANE_ASTERISK_VERSION_NUM < 0x00010400

char *description(void)
{
	return "vISDN PPP handler";
}

char *key()
{
	return ASTERISK_GPL_KEY;
}

#else

AST_MODULE_INFO(ASTERISK_GPL_KEY,
		AST_MODFLAG_GLOBAL_SYMBOLS,
		"vISDN PPP handler",
		.load = visdn_ppp_load_module,
		.unload = visdn_ppp_unload_module,
	);
#endif
