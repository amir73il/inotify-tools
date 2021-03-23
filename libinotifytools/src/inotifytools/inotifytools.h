#ifndef _inotifytools_H
#define _inotifytools_H

#ifdef __FreeBSD__
#define stat64 stat
#define lstat64 lstat
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdio.h>

#define IN_ALL_READ_EVENTS (IN_OPEN | IN_ACCESS | IN_CLOSE_NOWRITE)
#define IN_ALL_WRITE_EVENTS (IN_ALL_EVENTS & ~IN_ALL_READ_EVENTS)

#define MAX_STRLEN 4096

/** @struct nstring
 *  @brief This structure holds string that can contain any character including NULL.
 *  @var nstring::buf
 *  Member 'buf' contains character buffer.  It can hold up to 4096 characters.
 *  @var nstring::len
 *  Member 'len' contains number of characters in buffer.
 */
struct nstring {
	char buf[MAX_STRLEN];
	unsigned int len;
};

int inotifytools_str_to_event(char const * event);
int inotifytools_str_to_event_sep(char const * event, char sep);
char * inotifytools_event_to_str(int events);
char * inotifytools_event_to_str_sep(int events, char sep);
void inotifytools_set_filename_by_wd( int wd, char const * filename );
void inotifytools_set_filename_by_filename( char const * oldname,
                                            char const * newname );
void inotifytools_replace_filename( char const * oldname,
                                    char const * newname );
char * inotifytools_filename_from_wd( int wd );
int inotifytools_wd_from_filename( char const * filename );
int inotifytools_remove_watch_by_filename( char const * filename );
int inotifytools_remove_watch_by_wd( int wd );
int inotifytools_watch_file( char const * filename, int events );
int inotifytools_watch_files( char const * filenames[], int events );
int inotifytools_watch_recursively( char const * path, int events );
int inotifytools_watch_recursively_with_exclude( char const * path,
                                                 int events,
                                                 char const ** exclude_list );
                                                 // [UH]
int inotifytools_ignore_events_by_regex( char const *pattern, int flags );
int inotifytools_ignore_events_by_inverted_regex( char const *pattern, int flags );
struct inotify_event * inotifytools_next_event( long int timeout );
struct inotify_event * inotifytools_next_events( long int timeout, int num_events );
int inotifytools_error();
int inotifytools_get_stat_by_wd( int wd, int event );
int inotifytools_get_stat_total( int event );
int inotifytools_get_stat_by_filename( char const * filename,
                                                int event );
void inotifytools_initialize_stats();
int inotifytools_initialize();
int inotifytools_init(int fanotify);
void inotifytools_cleanup();
int inotifytools_get_num_watches();

int inotifytools_printf( struct inotify_event* event, char* fmt );
int inotifytools_fprintf( FILE* file, struct inotify_event* event, char* fmt );
int inotifytools_sprintf( struct nstring * out, struct inotify_event* event, char* fmt );
int inotifytools_snprintf( struct nstring * out, int size, struct inotify_event* event,
                           char* fmt );
void inotifytools_set_printf_timefmt( char * fmt );

int inotifytools_get_max_user_watches();
int inotifytools_get_max_user_instances();
int inotifytools_get_max_queued_events();

#ifdef __cplusplus
}
#endif

#endif     // _inotifytools_H
