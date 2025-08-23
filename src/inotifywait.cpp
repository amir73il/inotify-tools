#include "../config.h"
#include "../libinotifytools/src/inotifytools_p.h"
#include "common.h"

#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <libgen.h>

#include <inotifytools/inotify.h>
#include <inotifytools/fanotify.h>
#include <inotifytools/inotifytools.h>

extern char* optarg;
extern int optind, opterr, optopt;

#define MAX_STRLEN 4096

// METHODS
static bool parse_opts(int* argc,
		       char*** argv,
		       int* events,
		       bool* monitor,
		       int* quiet,
		       long* timeout,
		       int* recursive,
		       bool* csv,
		       bool* daemon,
		       bool* syslog,
		       bool* no_dereference,
		       char** format,
		       char** timefmt,
		       char** fromfile,
		       char** outfile,
		       char** server,
		       char** client,
		       char** exc_regex,
		       char** exc_iregex,
		       char** inc_regex,
		       char** inc_iregex,
		       bool* no_newline,
		       int* fanotify,
		       char* scope);

void print_help(const char *tool_name);

static const char* csv_escape_len(const char* string, size_t len) {
	static char csv[MAX_STRLEN + 1];
	static unsigned int i, ind;

	if (string == NULL) {
		return "";
	}

	if (len == 0 || len > MAX_STRLEN) {
		return "";
	}

	// May not need escaping
	if (!strchr(string, '"') && !strchr(string, ',') &&
	    !strchr(string, '\n') && string[0] != ' ' &&
	    string[len - 1] != ' ') {
		strncpy(csv, string, len);
		csv[len] = '\0';
		return csv;
	}

	// OK, so now we _do_ need escaping.
	csv[0] = '"';
	ind = 1;
	for (i = 0; i < len; ++i) {
		if (string[i] == '"') {
			csv[ind++] = '"';
		}
		csv[ind++] = string[i];
	}
	csv[ind++] = '"';
	csv[ind] = '\0';

	return csv;
}

static const char* csv_escape(const char* string) {
	if (string == NULL) {
		return "";
	}

	return csv_escape_len(string, strlen(string));
}

void validate_format(char* fmt) {
	// Make a fake event
	struct inotify_event* event =
	    (struct inotify_event*)malloc(sizeof(struct inotify_event) + 4);
	if (!event) {
		fprintf(stderr, "Seem to be out of memory... yikes!\n");
		exit(EXIT_FAILURE);
	}
	event->wd = 0;
	event->mask = IN_ALL_EVENTS;
	event->len = 3;
	event->name[0] = 0;
	FILE* devnull = fopen("/dev/null", "a");
	if (!devnull) {
		fprintf(stderr, "Couldn't open /dev/null: %s\n",
			strerror(errno));
		free(event);
		return;
	}

	if (-1 == inotifytools_fprintf(devnull, event, fmt)) {
		fprintf(stderr,
			"Something is wrong with your format string.\n");
		free(event);
		fclose(devnull);
		exit(EXIT_FAILURE);
	}

	free(event);
	fclose(devnull);
}

static bool write_event_raw(struct inotify_event* event) {
	size_t len = sizeof(struct inotify_event) + event->len;
	ssize_t written = write(fileno(stdout), event, len);
	return written == (ssize_t)len;
}

void output_event_csv(struct inotify_event* event) {
	size_t dirnamelen = 0;
	const char* eventname;
	const char* filename =
	    inotifytools_filename_from_event(event, &eventname, &dirnamelen);
	filename = csv_escape_len(filename, dirnamelen);
	if (filename && *filename)
		printf("%s,", filename);
	// eventname may be pointing into snprintf buffer
	char* name = strdup(eventname);

	printf("%s,", csv_escape(inotifytools_event_to_str(event->mask)));
	if (name) {
		printf("%s", csv_escape(name));
		free(name);
	}
	printf("\n");
}

void output_error(bool syslog, const char* fmt, ...) {
	va_list va;
	va_start(va, fmt);
	if (syslog) {
		vsyslog(LOG_INFO, fmt, va);
	} else {
		vfprintf(stderr, fmt, va);
	}
	va_end(va);
}

int setup_server_socket(const char* socket_path) {
	int server_fd;
	struct sockaddr_un addr;

	// Create socket
	server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (server_fd == -1) {
		output_error(true, "Failed to create server socket: %s\n",
			     strerror(errno));
		return -1;
	}

	// Remove existing socket file if it exists
	unlink(socket_path);

	// Set up address
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(socket_path) >= sizeof(addr.sun_path)) {
		output_error(true, "Socket path too long: %s\n", socket_path);
		close(server_fd);
		return -1;
	}
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	// Bind socket
	if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		output_error(true, "Failed to bind to socket %s: %s\n",
			     socket_path, strerror(errno));
		close(server_fd);
		return -1;
	}

	// Listen for connections
	if (listen(server_fd, 1) == -1) {
		output_error(true, "Failed to listen on socket: %s\n",
			     strerror(errno));
		close(server_fd);
		return -1;
	}

	return server_fd;
}

int wait_for_client_and_send_fd(int server_fd, int inotify_fd, char scope) {
	int client_fd;
	struct sockaddr_un client_addr;
	socklen_t client_len = sizeof(client_addr);
	struct msghdr msg;
	struct cmsghdr *cmsg;
	struct iovec iov;
	char buf[1];
	char cmsgbuf[CMSG_SPACE(sizeof(int))];

	output_error(true, "Waiting for client connection...\n");

	client_fd = accept(server_fd, (struct sockaddr*)&client_addr,
			   &client_len);
	if (client_fd == -1) {
		output_error(true, "Failed to accept client connection: %s\n",
			     strerror(errno));
		return -1;
	}

	output_error(true, "Client connected, sending fanotify fd and scope...\n");

	// Send the scope and inotify file descriptor to client
	buf[0] = scope;  // Send scope character
	iov.iov_base = buf;
	iov.iov_len = 1;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	*((int*)CMSG_DATA(cmsg)) = inotify_fd;

	if (sendmsg(client_fd, &msg, 0) == -1) {
		output_error(true, "Failed to send inotify fd and scope: %s\n",
			     strerror(errno));
		close(client_fd);
		return -1;
	}

	output_error(true, "Successfully sent fanotify fd and scope to client.\n");
	return client_fd;
}

int connect_to_server_and_recv_fd(const char* socket_path, char* recv_scope) {
	int client_fd, inotify_fd;
	struct sockaddr_un addr;
	struct msghdr msg;
	struct cmsghdr *cmsg;
	struct iovec iov;
	char buf[1];
	char cmsgbuf[CMSG_SPACE(sizeof(int))];

	// Create socket
	client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (client_fd == -1) {
		fprintf(stderr, "Failed to create client socket: %s\n",
			strerror(errno));
		return -1;
	}

	// Set up address
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(socket_path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "Socket path too long: %s\n", socket_path);
		close(client_fd);
		return -1;
	}
	strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

	// Connect to server
	if (connect(client_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
		fprintf(stderr, "Failed to connect to server %s: %s\n",
			socket_path, strerror(errno));
		close(client_fd);
		return -1;
	}

	// Receive the scope and fanotify file descriptor from server
	iov.iov_base = buf;
	iov.iov_len = 1;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cmsgbuf;
	msg.msg_controllen = sizeof(cmsgbuf);

	if (recvmsg(client_fd, &msg, 0) == -1) {
		fprintf(stderr, "Failed to receive fanotify fd and scope: %s\n",
			strerror(errno));
		close(client_fd);
		return -1;
	}

	// Extract the scope and file descriptor
	*recv_scope = buf[0];  // Get the scope character

	cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
	    cmsg->cmsg_type != SCM_RIGHTS) {
		fprintf(stderr, "Invalid control message received\n");
		close(client_fd);
		return -1;
	}

	inotify_fd = *((int*)CMSG_DATA(cmsg));
	close(client_fd);  // We don't need the socket connection anymore

	fprintf(stderr, "Received fanotify fd %d and scope '%c' from server\n",
		inotify_fd, *recv_scope ? *recv_scope : '0');
	return inotify_fd;
}



int main(int argc, char** argv) {
	int events = 0;
	int orig_events;
	bool monitor = false;
	int quiet = 0;
	long timeout = BLOCKING_TIMEOUT;
	int recursive = 0;
	int fanotify = 0;
	char scope = 0;
	bool csv = false;
	bool dodaemon = false;
	bool sysl = false;
	bool no_dereference = false;
	char* format = NULL;
	char* timefmt = NULL;
	char* fromfile = NULL;
	char* outfile = NULL;
	char* server = NULL;
	char* client = NULL;
	char* exc_regex = NULL;
	char* exc_iregex = NULL;
	char* inc_regex = NULL;
	char* inc_iregex = NULL;
	bool no_newline = false;
	int fd, rc;

	if ((argc > 0) && (strncmp(basename(argv[0]), "fsnotify", 8) == 0)) {
		// Default to fanotify for the fsnotify* tools.
		fanotify = 1;
	}

	// Parse commandline options, aborting if something goes wrong
	if (!parse_opts(&argc, &argv, &events, &monitor, &quiet, &timeout,
			&recursive, &csv, &dodaemon, &sysl, &no_dereference,
			&format, &timefmt, &fromfile, &outfile, &server,
			&client, &exc_regex, &exc_iregex, &inc_regex,
			&inc_iregex, &no_newline, &fanotify, &scope)) {
		return EXIT_FAILURE;
	}

	// Handle server socket setup if --server option was provided
	int server_fd = -1;
	if (server) {
		server_fd = setup_server_socket(server);
		if (server_fd == -1) {
			return EXIT_FAILURE;
		}
	} else if (client) {
		// Connect to server and receive inotify fd and scope
		server_fd = connect_to_server_and_recv_fd(client, &scope);
		if (server_fd == -1) {
			return EXIT_FAILURE;
		}
		// Use the received fd as fanotify param
		fanotify = server_fd;
	}

	rc = inotifytools_init(fanotify, scope, !quiet);
	if (!rc) {
		warn_inotify_init_error(fanotify);
		return EXIT_FAILURE;
	}

	if (timefmt)
		inotifytools_set_printf_timefmt(timefmt);
	if ((exc_regex &&
	     !inotifytools_ignore_events_by_regex(exc_regex, REG_EXTENDED, recursive)) ||
	    (exc_iregex && !inotifytools_ignore_events_by_regex(
			       exc_iregex, REG_EXTENDED | REG_ICASE, recursive))) {
		fprintf(stderr, "Error in `exclude' regular expression.\n");
		return EXIT_FAILURE;
	}
	if ((inc_regex && !inotifytools_ignore_events_by_inverted_regex(
			      inc_regex, REG_EXTENDED, recursive)) ||
	    (inc_iregex && !inotifytools_ignore_events_by_inverted_regex(
			       inc_iregex, REG_EXTENDED | REG_ICASE, recursive))) {
		fprintf(stderr, "Error in `include' regular expression.\n");
		return EXIT_FAILURE;
	}

	if (format)
		validate_format(format);

	// Attempt to watch file
	// If events is still 0, make it all events.
	// fanotify mount watch supports only legacy fanotify events
	if (!events)
		events = (scope == 'M') ? FAN_ALL_EVENTS : IN_ALL_EVENTS;

	orig_events = events;
	if (monitor && recursive)
		events = events | IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM;

	if (no_dereference)
		events = events | IN_DONT_FOLLOW;

	if (fanotify)
		events |= IN_ISDIR;

	FileList list(argc, argv);
	construct_path_list(argc, argv, fromfile, &list);

	if (!client && 0 == list.watch_files_[0]) {
		fprintf(stderr, "No files specified to watch!\n");

		return EXIT_FAILURE;
	}

	// Daemonize - BSD double-fork approach
	if (dodaemon) {
		// Absolute path for outfile before entering the child.
		char* logfile = (char*)calloc(PATH_MAX + 1, sizeof(char));
		if (outfile && realpath(outfile, logfile) == NULL) {
			fprintf(stderr, "%s: %s\n", strerror(errno), outfile);
			free(logfile);
			return EXIT_FAILURE;
		}

		if (daemon(0, 0)) {
			fprintf(stderr, "Failed to daemonize!\n");
			free(logfile);
			return EXIT_FAILURE;
		}

		// Redirect stdin from /dev/null
		fd = open("/dev/null", O_RDONLY);
		if (fd != fileno(stdin)) {
			dup2(fd, fileno(stdin));
			close(fd);
		}

		// Redirect stdout to a file/socket
		if (server) {
			// For server mode, we don't need a logfile
			fd = STDOUT_FILENO;
		} else {
			fd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0600);
		}
		if (fd < 0) {
			fprintf(stderr, "Failed to open output file %s\n",
				logfile);
			free(logfile);
			return EXIT_FAILURE;
		}
		free(logfile);

		if (fd != fileno(stdout)) {
			dup2(fd, fileno(stdout));
			close(fd);
		}

		// Redirect stderr to /dev/null
		fd = open("/dev/null", O_WRONLY);
		if (fd != fileno(stderr)) {
			dup2(fd, fileno(stderr));
			close(fd);
		}

	} else if (outfile != NULL) {  // Redirect stdout to a file if specified
		fd = open(outfile, O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (fd < 0) {
			fprintf(stderr, "Failed to open output file %s\n",
				outfile);

			return EXIT_FAILURE;
		}
		if (fd != fileno(stdout)) {
			dup2(fd, fileno(stdout));
			close(fd);
		}
	}

	if (sysl) {
		openlog("inotifywait", LOG_CONS | LOG_PID | LOG_NDELAY,
			LOG_DAEMON);
	}

	if (!quiet && !client) {
		if (scope) {
			output_error(sysl, "Setting up %s watches.\n",
				     scope == 'M' ? "mount" : "filesystem");
		} else if (recursive) {
			output_error(sysl,
				     "Setting up watches.  Beware: since -r "
				     "was given, this may take a while!\n");
		} else {
			output_error(sysl, "Setting up watches.\n");
		}
	}

	// now watch files
	for (int i = 0; list.watch_files_[i] && !client; ++i) {
		char const* this_file = list.watch_files_[i];
		if (scope) {
			if (!inotifytools_watch_files(list.watch_files_,
						      events)) {
				output_error(
				    sysl,
				    "Couldn't add %s watch %s: %s\n",
				    scope == 'M' ? "mount" : "filesystem",
				    this_file, strerror(inotifytools_error()));

				return EXIT_FAILURE;
			}
			break;
		}

		if ((recursive &&
		     !inotifytools_watch_recursively_with_exclude(
			 this_file, events, list.exclude_files_)) ||
		    (!recursive &&
		     !inotifytools_watch_file(this_file, events))) {
			if (inotifytools_error() == ENOSPC) {
				const char* backend =
				    fanotify ? "fanotify" : "inotify";
				const char* resource =
				    fanotify ? "marks" : "watches";
				output_error(
				    sysl,
				    "Failed to watch %s; upper limit on %s %s "
				    "reached!\n",
				    this_file, backend, resource);
				output_error(
				    sysl,
				    "Please increase the amount of %s %s "
				    "allowed per user via `/proc/sys/fs/%s/"
				    "max_user_%s'.\n",
				    backend, resource, backend, resource);
			} else {
				output_error(sysl, "Couldn't watch %s: %s\n",
					     this_file,
					     strerror(inotifytools_error()));
			}

			return EXIT_FAILURE;
		}
	}

	if (!quiet && !client) {
		output_error(sysl, "Watches established.\n");
	}

	// For server mode, wait for client and send the inotify fd
	if (server) {
		int inotify_fd = inotifytools_get_fd();
		int client_fd = wait_for_client_and_send_fd(server_fd, inotify_fd, scope);
		if (client_fd == -1) {
			return EXIT_FAILURE;
		}
		close(client_fd);  // Close after sending fd
	}

	if (timeout < 0) {
		// Used to test filesystem support for inotify/fanotify
		fprintf(stderr, "Negative timeout specified - abort!\n");
		return EXIT_FAILURE;
	}

	// Now wait till we get event
	struct inotify_event* event;
	char* moved_from = 0;

	do {
		event = inotifytools_next_event(timeout);
		if (!event) {
			if (!inotifytools_error()) {
				return EXIT_TIMEOUT;
			} else {
				output_error(sysl, "%s\n",
					     strerror(inotifytools_error()));

				return EXIT_FAILURE;
			}
		}

		if (quiet < 2 && (event->mask & orig_events)) {
			bool is_dir_event = (event->mask & IN_ISDIR);
			// Only output to stdout if the event is for a file matching our filters
			// or if we don't have any include filters
			// For non-directory events, only show if they match the filter
			// The filter is already applied by inotifytools internally
			if ((!inc_regex && !inc_iregex) || !is_dir_event) {
				if (server) {
					if (!write_event_raw(event)) {
						output_error(sysl, "Failed to write event to server: %s\n", strerror(errno));
						return EXIT_FAILURE;
					}
				} else if (csv) {
					output_event_csv(event);
				} else if (format) {
					inotifytools_printf(event, format);
				} else {
					inotifytools_printf(event, "%w %,e %f\n");
				}
			}
		}

		// TODO: replace filename of renamed filesystem watch entries
		if (scope || client)
			continue;

		// if we last had MOVED_FROM and don't currently have MOVED_TO,
		// moved_from file must have been moved outside of tree - so
		// unwatch it.
		if (moved_from && !(event->mask & IN_MOVED_TO)) {
			if (!inotifytools_remove_watch_by_filename(
				moved_from)) {
				output_error(
				    sysl, "Error removing watch on %s: %s\n",
				    moved_from, strerror(inotifytools_error()));
			}
			free(moved_from);
			moved_from = 0;
		}

		if (monitor && recursive) {
			if ((event->mask & IN_CREATE) ||
			    (!moved_from && (event->mask & IN_MOVED_TO))) {
				// New file - if it is a directory, watch it
				char* new_file = inotifytools_dirpath_from_event(event);
				if (new_file && *new_file && isdir(new_file)) {
					if (!quiet) {
						output_error(sysl, "Watching new directory %s\n", new_file);
					}
					if (!inotifytools_watch_recursively(new_file, events)) {
						output_error(sysl,
							"Couldn't watch new directory %s: %s\n",
							new_file,
							strerror(inotifytools_error()));
					}
				}
				free(new_file);
			}  // IN_CREATE
			else if (event->mask & IN_MOVED_FROM) {
				moved_from =
				    inotifytools_dirpath_from_event(event);
				// if not watched...
				if (inotifytools_wd_from_filename(moved_from) ==
				    -1) {
					free(moved_from);
					moved_from = 0;
				}
			}  // IN_MOVED_FROM
			else if (event->mask & IN_MOVED_TO) {
				if (moved_from) {
					char* new_name =
					    inotifytools_dirpath_from_event(
						event);
					inotifytools_replace_filename(
					    moved_from, new_name);
					free(new_name);
					free(moved_from);
					moved_from = 0;
				}  // moved_from
			}
		}

		fflush(NULL);

	} while (monitor);

	// If we weren't trying to listen for this event...
	if ((events & event->mask) == 0) {
		// ...then most likely something bad happened, like IGNORE etc.
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static bool parse_opts(int* argc,
		       char*** argv,
		       int* events,
		       bool* monitor,
		       int* quiet,
		       long* timeout,
		       int* recursive,
		       bool* csv,
		       bool* daemon,
		       bool* syslog,
		       bool* no_dereference,
		       char** format,
		       char** timefmt,
		       char** fromfile,
		       char** outfile,
		       char** server,
		       char** client,
		       char** exc_regex,
		       char** exc_iregex,
		       char** inc_regex,
		       char** inc_iregex,
		       bool* no_newline,
		       int* fanotify,
		       char* scope) {
	assert(argc);
	assert(argv);
	assert(events);
	assert(monitor);
	assert(quiet);
	assert(timeout);
	assert(recursive);
	assert(fanotify);
	assert(scope);
	assert(csv);
	assert(daemon);
	assert(syslog);
	assert(no_dereference);
	assert(format);
	assert(timefmt);
	assert(fromfile);
	assert(outfile);
	assert(server);
	assert(client);
	assert(exc_regex);
	assert(exc_iregex);
	assert(inc_regex);
	assert(inc_iregex);

	// Settings for options
	int new_event;

	// How many times --exclude has been specified
	unsigned int exclude_count = 0;

	// How many times --excludei has been specified
	unsigned int excludei_count = 0;

	const char* regex_warning =
	    "only the last option will be taken into consideration.\n";

	// Short options
	static const char opt_string[] = "mrhcdsPqt:fo:e:IFMSU:u:";

	// Long options
	static const struct option long_opts[] = {
	    {"help", no_argument, NULL, 'h'},
	    {"event", required_argument, NULL, 'e'},
	    {"monitor", no_argument, NULL, 'm'},
	    {"quiet", no_argument, NULL, 'q'},
	    {"timeout", required_argument, NULL, 't'},
	    {"filename", no_argument, NULL, 'f'},
	    {"recursive", no_argument, NULL, 'r'},
	    {"inotify", no_argument, NULL, 'I'},
	    {"fanotify", no_argument, NULL, 'F'},
	    {"filesystem", no_argument, NULL, 'S'},
	    {"mount", no_argument, NULL, 'M'},
	    {"csv", no_argument, NULL, 'c'},
	    {"daemon", no_argument, NULL, 'd'},
	    {"syslog", no_argument, NULL, 's'},
	    {"no-dereference", no_argument, NULL, 'P'},
	    {"format", required_argument, NULL, 'n'},
	    {"no-newline", no_argument, NULL, '0'},
	    {"timefmt", required_argument, NULL, 'i'},
	    {"fromfile", required_argument, NULL, 'z'},
	    {"outfile", required_argument, NULL, 'o'},
	    {"exclude", required_argument, NULL, 'a'},
	    {"excludei", required_argument, NULL, 'b'},
	    {"include", required_argument, NULL, 'j'},
	    {"includei", required_argument, NULL, 'k'},
	    {"server", required_argument, NULL, 'U'},
	    {"client", required_argument, NULL, 'u'},
	    {NULL, 0, 0, 0},
	};

	// Get first option
	char curr_opt = getopt_long(*argc, *argv, opt_string, long_opts, NULL);

	// While more options exist...
	while ((curr_opt != '?') && (curr_opt != (char)-1)) {
		switch (curr_opt) {
			// --help or -h
			case 'h':
				print_help(((*argc) > 0)
					   ? basename((*argv)[0])
					   : "<executable>");
				// Shouldn't process any further...
				return false;

			// --monitor or -m
			case 'm':
				*monitor = true;
				break;

			// --quiet or -q
			case 'q':
				(*quiet)++;
				break;

			// --recursive or -r
			case 'r':
				(*recursive)++;
				break;

			// --inotify or -I
			case 'I':
				if (*scope) {
					fprintf(stderr,
						"Please specify -I -S or -M once "
						"only!\n");
					return false;
				}
				(*scope) = curr_opt;
				(*fanotify) = 0;
				break;

			// --fanotify or -F
			case 'F':
				(*fanotify) = 1;
				break;

			// --mount or -M
			case 'M':
			// --filesystem or -S
			case 'S':
				if (*scope) {
					fprintf(stderr,
						"Please specify -S or -M once "
						"only!\n");
					return false;
				}
				(*scope) = curr_opt;
				(*fanotify) = 1;
				break;

			// --csv or -c
			case 'c':
				(*csv) = true;
				break;

			// --server or -U
			case 'U':
				if (*server) {
					fprintf(stderr,
						"Multiple --server options "
						"given.\n");
					return false;
				}
				(*server) = optarg;
				// server mode implies fanotify
				(*fanotify) = 1;
				// fall through
			// --daemon or -d
			case 'd':
				(*daemon) = true;
				(*monitor) = true;
				(*syslog) = true;
				break;

			// --client or -u
			case 'u':
				if (*client) {
					fprintf(stderr,
						"Multiple --client options "
						"given.\n");
					return false;
				}
				(*client) = optarg;
				// client mode implies fanotify
				(*fanotify) = 1;
				break;

			// --syslog or -s
			case 's':
				(*syslog) = true;
				break;

			// --no-dereference or -P
			case 'P':
				(*no_dereference) = true;
				break;

			// --filename or -f
			case 'f':
				fprintf(stderr,
					"The '--filename' option no longer "
					"exists.  "
					"The option it enabled in "
					"earlier\nversions of "
					"inotifywait is now turned on by "
					"default.\n");
				return false;

			// --format
			case 'n':
				assert(optarg);
				if (!(*format)) {
					*format =
					    (char*)malloc(strlen(optarg) + 2);
				}

				if (*format)
					strcpy(*format, optarg);

				break;

			// --no-newline
			case '0':
				(*no_newline) = true;
				break;

			// --timefmt
			case 'i':
				(*timefmt) = optarg;
				break;

			// --exclude
			case 'a':
				(*exc_regex) = optarg;
				exclude_count++;
				break;

			// --excludei
			case 'b':
				(*exc_iregex) = optarg;
				excludei_count++;
				break;

			// --include
			case 'j':
				(*inc_regex) = optarg;
				break;

			// --includei
			case 'k':
				(*inc_iregex) = optarg;
				break;

			// --fromfile
			case 'z':
				if (*fromfile) {
					fprintf(stderr,
						"Multiple --fromfile options "
						"given.\n");
					return false;
				}

				(*fromfile) = optarg;
				break;

			// --outfile
			case 'o':
				if (*outfile) {
					fprintf(stderr,
						"Multiple --outfile options "
						"given.\n");
					return false;
				}

				(*outfile) = optarg;
				break;

			// --timeout or -t
			case 't':
				if (!is_timeout_option_valid(timeout, optarg)) {
					return false;
				}

				break;

			// --event or -e
			case 'e':
				// Get event mask from event string
				new_event = inotifytools_str_to_event(optarg);

				// If optarg was invalid, abort
				if (new_event == -1) {
					fprintf(
					    stderr,
					    "'%s' is not a valid event!  Run "
					    "with the "
					    "'--help' option to see a list of "
					    "events.\n",
					    optarg);
					return false;
				}

				// Add the new event to the event mask
				(*events) = ((*events) | new_event);

				break;
		}

		curr_opt =
		    getopt_long(*argc, *argv, opt_string, long_opts, NULL);
	}

	if (*format && !(*no_newline)) {
		strncat(*format, "\n", 2);
	}

	if (*exc_regex && *exc_iregex) {
		fprintf(stderr,
			"--exclude and --excludei cannot both be specified.\n");
		return false;
	}

	if (*inc_regex && *inc_iregex) {
		fprintf(stderr,
			"--include and --includei cannot both be specified.\n");
		return false;
	}

	if ((*inc_regex && *exc_regex) || (*inc_regex && *exc_iregex) ||
	    (*inc_iregex && *exc_regex) || (*inc_iregex && *exc_iregex)) {
		fprintf(
		    stderr,
		    "include and exclude regexp cannot both be specified.\n");
		return false;
	}

	if (*server && (*format || *csv || *timeout)) {
		fprintf(stderr, "-t -c and --format cannot be used with --server.\n");
		return false;
	}

	if (*format && *csv) {
		fprintf(stderr, "-c and --format cannot both be specified.\n");
		return false;
	}

	if (!*format && *no_newline) {
		fprintf(stderr,
			"--no-newline cannot be specified without --format.\n");
		return false;
	}

	if (!*format && *timefmt) {
		fprintf(stderr,
			"--timefmt cannot be specified without --format.\n");
		return false;
	}

	if (*format && strstr(*format, "%T") && !*timefmt) {
		fprintf(stderr,
			"%%T is in --format string, but --timefmt was not "
			"specified.\n");
		return false;
	}

	if (*daemon && !*outfile && !*server) {
		fprintf(stderr, "-o or --server must be specified with -d.\n");
		return false;
	}

	if (*client && *server) {
		fprintf(stderr, "--client and --server cannot both be specified.\n");
		return false;
	}

	if (*client && (!*fanotify || *scope || *recursive)) {
		fprintf(stderr, "--client conflicts with --inotify, "
			"--filesystem, --mount, and --recursive.\n");
		return false;
	}

	if (exclude_count > 1) {
		fprintf(stderr, "--exclude: %s", regex_warning);
	}

	if (excludei_count > 1) {
		fprintf(stderr, "--excludei: %s", regex_warning);
	}

	(*argc) -= optind;
	*argv = &(*argv)[optind];

	if (*scope == 'I') {
		*scope = 0;
	}

	// If ? returned, invalid option
	return (curr_opt != '?');
}

void print_help(const char *tool_name) {
	printf("%s %s\n", tool_name, PACKAGE_VERSION);
	printf("Wait for a particular event on a file or set of files.\n");
	printf("Usage: %s [ options ] file1 [ file2 ] [ file3 ] [ ... ]\n",
	       tool_name);
	printf("Options:\n");
	printf("\t-h|--help     \tShow this help text.\n");
	printf(
	    "\t@<file>       \tExclude the specified file from being "
	    "watched.\n");
	printf(
	    "\t--exclude <pattern>\n"
	    "\t              \tExclude all events on files matching the\n"
	    "\t              \textended regular expression <pattern>.\n"
	    "\t              \tOnly the last --exclude option will be\n"
	    "\t              \ttaken into consideration.\n");
	printf(
	    "\t--excludei <pattern>\n"
	    "\t              \tLike --exclude but case insensitive.\n");
	printf(
	    "\t--include <pattern>\n"
	    "\t              \tExclude all events on files except the ones\n"
	    "\t              \tmatching the extended regular expression\n"
	    "\t              \t<pattern>.\n");
	printf(
	    "\t--includei <pattern>\n"
	    "\t              \tLike --include but case insensitive.\n");
	printf(
	    "\t-m|--monitor  \tKeep listening for events forever or until "
	    "--timeout expires.\n"
	    "\t              \tWithout this option, %s will exit after "
	    "one event is received.\n",
	    tool_name);
	printf(
	    "\t-d|--daemon   \tSame as --monitor, except run in the "
	    "background\n"
	    "\t              \tlogging events to a file specified by "
	    "--outfile.\n"
	    "\t              \tImplies --syslog.\n");
	printf(
	    "\t-P|--no-dereference\n"
	    "\t              \tDo not follow symlinks.\n");
	printf("\t-r|--recursive\tWatch directories recursively.\n");
	printf("\t-I|--inotify\tWatch with inotify.\n");
	printf("\t-F|--fanotify\tWatch with fanotify.\n");
	printf("\t-S|--filesystem\tWatch entire filesystem with fanotify.\n");
	printf("\t-M|--mount\tWatch entire mount with fanotify.\n");
	printf(
	    "\t--fromfile <file>\n"
	    "\t              \tRead files to watch from <file> or `-' for "
	    "stdin.\n");
	printf(
	    "\t-o|--outfile <file>\n"
	    "\t              \tPrint events to <file> rather than stdout.\n");
	printf("\t-s|--syslog   \tSend errors to syslog rather than stderr.\n");
	printf("\t-q|--quiet    \tPrint less (only print events).\n");
	printf("\t-qq           \tPrint nothing (not even events).\n");
	printf(
	    "\t--format <fmt>\tPrint using a specified printf-like format\n"
	    "\t              \tstring; read the man page for more details.\n");
	printf(
	    "\t--no-newline  \tDon't print newline symbol after\n"
	    "\t              \t--format string.\n");
	printf(
	    "\t--timefmt <fmt>\tstrftime-compatible format string for use "
	    "with\n"
	    "\t              \t%%T in --format string.\n");
	printf("\t-c|--csv      \tPrint events in CSV format.\n");
	printf(
	    "\t-t|--timeout <seconds>\n"
	    "\t              \tWhen listening for a single event, time out "
	    "after\n"
	    "\t              \twaiting for an event for <seconds> seconds.\n"
	    "\t              \tIf <seconds> is zero, %s will never time "
	    "out.\n",
	    tool_name);
	printf(
	    "\t-e|--event <event1> [ -e|--event <event2> ... ]\n"
	    "\t\tListen for specific event(s).  If omitted, all events are \n"
	    "\t\tlistened for.\n");
	printf(
	    "\t-u|--client <socket_path>\n"
	    "\t\tConnect to a server socket at <socket_path> and receive\n"
	    "\t\tevents from the server. Conflicts with --inotify,\n"
	    "\t\t--filesystem, --mount, --recursive and implies --fanotify.\n");
	printf(
	    "\t-U|--server <socket_path>\n"
	    "\t\tCreate a Unix domain socket at <socket_path> and wait for\n"
	    "\t\tclient connections before establishing watches.\n"
	    "\t\tImplies --fanotify, --daemon, --monitor, and --syslog.\n\n");
	printf("Exit status:\n");
	printf("\t%d  -  An event you asked to watch for was received.\n",
	       EXIT_SUCCESS);
	printf("\t%d  -  An event you did not ask to watch for was received\n",
	       EXIT_FAILURE);
	printf(
	    "\t      (usually delete_self or unmount), or some error "
	    "occurred.\n");
	printf(
	    "\t%d  -  The --timeout option was given and no events occurred\n",
	    EXIT_TIMEOUT);
	printf("\t      in the specified interval of time.\n\n");
	printf("Events:\n");
	print_event_descriptions();
}
