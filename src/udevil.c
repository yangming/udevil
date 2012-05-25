/*
 * udevil.c    GPL v3  Copyright 2012
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>

// time
#ifndef __USE_XOPEN
#define __USE_XOPEN
#endif
#include <time.h>

// glib
#include <glib.h>
#include <glib/gstdio.h>

// udev
#include <libudev.h>
#include <fcntl.h>
#include <errno.h>

// network
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

// groups
#include <grp.h>

// environ
#include <paths.h>

// priviledges
#include <sys/param.h>
#include <sys/types.h>

// utmp tty
//#include <utmpx.h>

// wildcards
#include <fnmatch.h>

// ac config
#include "config.h"

// intltool
#include <glib/gi18n.h>

// use mount's realpath
#include "realpath.h"

#include "device-info.h"


#define ALLOWED_OPTIONS "nosuid,noexec,nodev,user=$USER,uid=$UID,gid=$GID"
#define ALLOWED_TYPES "$KNOWN_FILESYSTEMS,smbfs,nfs,ftpfs,curlftpfs,file"
#define MAX_LOG_DAYS 60   // don't set this too high

int verbose = 1;
char* logfile = NULL;
char* logmem = NULL;
char* cmd_line = NULL;
GList* devmounts = NULL;

enum {
    CMD_UNSET,
    CMD_MOUNT,
    CMD_UNMOUNT,
    CMD_MONITOR,
    CMD_INFO,
    CMD_CLEAN
};

typedef struct
{
    int cmd_type;
    char* device_file;
    char* point;
    char* fstype;
    char* options;
    char* label;
    char* uuid;
    gboolean force;
    gboolean lazy;
} CommandData;

typedef struct netmount_t {
    char* url;
    char* fstype;
    char* host;
    char* ip;
    char* port;
    char* user;
    char* pass;
    char* path;
} netmount_t;

struct udev         *udev = NULL;
struct udev_monitor *umonitor = NULL;
GIOChannel* uchannel = NULL;
GIOChannel* mchannel = NULL;
GList* config = NULL;


/* ************************************************************************
 * udev & mount monitors
 * ************************************************************************ */

gint cmp_devmounts( devmount_t *a, devmount_t *b )
{
    if ( !a && !b )
        return 0;
    if ( !a || !b )
        return 1;
    if ( a->major == b->major && a->minor == b->minor )
        return 0;
    return 1;
}

void parse_mounts( gboolean report )
{
    gchar *contents;
    gchar **lines;
    GError *error;
    guint n;
//fprintf( stderr, "\n@@@@@@@@@@@@@ parse_mounts %s\n\n", report ? "TRUE" : "FALSE" );
    contents = NULL;
    lines = NULL;

    error = NULL;
    if (!g_file_get_contents ("/proc/self/mountinfo", &contents, NULL, &error))
    {
        g_warning ("Error reading /proc/self/mountinfo: %s", error->message);
        g_error_free (error);
        return;
    }

    // get all mount points for all devices
    GList* newmounts = NULL;
    GList* l;
    GList* changed = NULL;
    devmount_t *devmount;

    /* See Documentation/filesystems/proc.txt for the format of /proc/self/mountinfo
    *
    * Note that things like space are encoded as \020.
    */
    lines = g_strsplit (contents, "\n", 0);
    for ( n = 0; lines[n] != NULL; n++ )
    {
        guint mount_id;
        guint parent_id;
        guint major, minor;
        gchar encoded_root[PATH_MAX];
        gchar encoded_mount_point[PATH_MAX];
        gchar *mount_point;

        if ( strlen( lines[n] ) == 0 )
            continue;

        if ( sscanf( lines[n],
                  "%d %d %d:%d %s %s",
                  &mount_id,
                  &parent_id,
                  &major,
                  &minor,
                  encoded_root,
                  encoded_mount_point ) != 6 )
        {
            g_warning ("Error reading /proc/self/mountinfo: Error parsing line '%s'", lines[n]);
            continue;
        }

        /* ignore mounts where only a subtree of a filesystem is mounted */
        if ( g_strcmp0( encoded_root, "/" ) != 0 )
            continue;

        mount_point = g_strcompress( encoded_mount_point );
        if ( !mount_point || ( mount_point && mount_point[0] == '\0' ) )
        {
            g_free( mount_point );
            continue;
        }

//printf("mount_point(%d:%d)=%s\n", major, minor, mount_point );
        devmount = NULL;
        for ( l = newmounts; l; l = l->next )
        {
            if ( ((devmount_t*)l->data)->major == major &&
                                        ((devmount_t*)l->data)->minor == minor )
            {
                devmount = (devmount_t*)l->data;
                break;
            }
        }
        if ( !devmount )
        {
//printf("     new devmount\n");
            devmount = g_slice_new0( devmount_t );
            devmount->major = major;
            devmount->minor = minor;
            devmount->mount_points = NULL;
            devmount->mounts = NULL;
            newmounts = g_list_prepend( newmounts, devmount );
        }

        if ( !g_list_find( devmount->mounts, mount_point ) )
        {
//printf("    prepended\n");
            devmount->mounts = g_list_prepend( devmount->mounts, mount_point );
        }
        else
            g_free (mount_point);
    }
    g_free( contents );
    g_strfreev( lines );
//fprintf( stderr, "\nLINES DONE\n\n");
    // translate each mount points list to string
    gchar *points, *old_points;
    GList* m;
    for ( l = newmounts; l; l = l->next )
    {
        devmount = (devmount_t*)l->data;
        // Sort the list to ensure that shortest mount paths appear first
        devmount->mounts = g_list_sort( devmount->mounts, (GCompareFunc) g_strcmp0 );
        m = devmount->mounts;
        points = g_strdup( (gchar*)m->data );
        while ( m = m->next )
        {
            old_points = points;
            points = g_strdup_printf( "%s, %s", old_points, (gchar*)m->data );
            g_free( old_points );
        }
        g_list_foreach( devmount->mounts, (GFunc)g_free, NULL );
        g_list_free( devmount->mounts );
        devmount->mounts = NULL;
        devmount->mount_points = points;
//fprintf( stderr, "translate %d:%d %s\n", devmount->major, devmount->minor, points );
    }

    // compare old and new lists
    GList* found;
    struct udev_device *udevice;
    dev_t dev;
    if ( report )
    {
        for ( l = newmounts; l; l = l->next )
        {
            devmount = (devmount_t*)l->data;
//fprintf( stderr, "finding %d:%d\n", devmount->major, devmount->minor );
            found = g_list_find_custom( devmounts, (gconstpointer)devmount,
                                                    (GCompareFunc)cmp_devmounts );
            if ( found && found->data )
            {
//fprintf( stderr, "    found\n");
                if ( !g_strcmp0( ((devmount_t*)found->data)->mount_points,
                                                        devmount->mount_points ) )
                {
//fprintf( stderr, "        freed\n");
                    // no change to mount points, so remove from old list
                    devmount = (devmount_t*)found->data;
                    g_free( devmount->mount_points );
                    devmounts = g_list_remove( devmounts, devmount );
                    g_slice_free( devmount_t, devmount );
                }
            }
            else
            {
                // new mount
//fprintf( stderr, "    new mount %d:%d\n", devmount->major, devmount->minor );
                dev = makedev( devmount->major, devmount->minor );
                udevice = udev_device_new_from_devnum( udev, 'b', dev );
                if ( udevice )
                    changed = g_list_prepend( changed, udevice );
            }
        }
    }
//fprintf( stderr, "\nREMAINING\n\n");
    // any remaining devices in old list have changed mount status
    for ( l = devmounts; l; l = l->next )
    {
        devmount = (devmount_t*)l->data;
//fprintf( stderr, "remain %d:%d\n", devmount->major, devmount->minor );
        if ( report )
        {
            dev = makedev( devmount->major, devmount->minor );
            udevice = udev_device_new_from_devnum( udev, 'b', dev );
            if ( udevice )
                changed = g_list_prepend( changed, udevice );
        }
        g_free( devmount->mount_points );
        g_slice_free( devmount_t, devmount );
    }
    g_list_free( devmounts );
    devmounts = newmounts;

    // report
    if ( report && changed )
    {
        char* devnode;
        for ( l = changed; l; l = l->next )
        {
            udevice = (struct udev_device*)l->data;
            if ( udevice )
            {
                devnode = g_strdup( udev_device_get_devnode( udevice ) );
                if ( devnode )
                {
                    char* bdev = g_path_get_basename( devnode );
                    printf( "changed:     /org/freedesktop/UDisks/devices/%s\n", bdev );
                    fflush( stdout );
                    g_free( bdev );
                    g_free( devnode );
                }
                udev_device_unref( udevice );
            }
        }
        g_list_free( changed );
    }
}

static void free_devmounts()
{
    GList* l;
    devmount_t *devmount;

    if ( !devmounts )
        return;
    for ( l = devmounts; l; l = l->next )
    {
        devmount = (devmount_t*)l->data;
        if ( devmount )
        {
            g_free( devmount->mount_points );
            g_slice_free( devmount_t, devmount );
        }
    }
    g_list_free( devmounts );
    devmounts = NULL;
}

static gboolean cb_mount_monitor_watch( GIOChannel *channel, GIOCondition cond,
                                                            gpointer user_data )
{
    if ( cond & ~G_IO_ERR )
        return TRUE;

    //printf ("@@@ /proc/self/mountinfo changed\n");
    parse_mounts( TRUE );

    return TRUE;
}

static gboolean cb_udev_monitor_watch( GIOChannel *channel, GIOCondition cond,
                                                            gpointer user_data )
{

/*
printf("cb_monitor_watch %d\n", channel);
if ( cond & G_IO_IN )
    printf("    G_IO_IN\n");
if ( cond & G_IO_OUT )
    printf("    G_IO_OUT\n");
if ( cond & G_IO_PRI )
    printf("    G_IO_PRI\n");
if ( cond & G_IO_ERR )
    printf("    G_IO_ERR\n");
if ( cond & G_IO_HUP )
    printf("    G_IO_HUP\n");
if ( cond & G_IO_NVAL )
    printf("    G_IO_NVAL\n");

if ( !( cond & G_IO_NVAL ) )
{
    gint fd = g_io_channel_unix_get_fd( channel );
    printf("    fd=%d\n", fd);
    if ( fcntl(fd, F_GETFL) != -1 || errno != EBADF )
    {
        int flags = g_io_channel_get_flags( channel );
        if ( flags & G_IO_FLAG_IS_READABLE )
            printf( "    G_IO_FLAG_IS_READABLE\n");
    }
    else
        printf("    Invalid FD\n");
}
*/
    if ( ( cond & G_IO_NVAL ) )
    {
        g_warning( "udev g_io_channel_unref G_IO_NVAL" );
        g_io_channel_unref( channel );
        return FALSE;
    }
    else if ( !( cond & G_IO_IN ) )
    {
        if ( ( cond & G_IO_HUP ) )
        {
        g_warning( "udev g_io_channel_unref !G_IO_IN && G_IO_HUP" );
            g_io_channel_unref( channel );
            return FALSE;
        }
        else
            return TRUE;
    }
    else if ( !( fcntl( g_io_channel_unix_get_fd( channel ), F_GETFL ) != -1
                                                    || errno != EBADF ) )
    {
        // bad file descriptor
        g_warning( "udev g_io_channel_unref BAD_FD" );
        g_io_channel_unref( channel );
        return FALSE;
    }

    struct udev_device *udevice;
    const char *action;
    const char *acted = NULL;
    char* devnode;
    if ( udevice = udev_monitor_receive_device( umonitor ) )
    {
        action = udev_device_get_action( udevice );
        devnode = g_strdup( udev_device_get_devnode( udevice ) );
        if ( action && devnode )
        {
            // print action
            char* bdev = g_path_get_basename( devnode );
            if ( !strcmp( action, "add" ) )
                printf( "added:     /org/freedesktop/UDisks/devices/%s\n", bdev );
            else if ( !strcmp( action, "remove" ) )
                printf( "removed:   /org/freedesktop/UDisks/devices/%s\n", bdev );
            else if ( !strcmp( action, "change" ) )
                printf( "changed:     /org/freedesktop/UDisks/devices/%s\n", bdev );
            else if ( !strcmp( action, "move" ) )
                printf( "moved:     /org/freedesktop/UDisks/devices/%s\n", bdev );
            g_free( bdev );
            fflush( stdout );
            fflush( stderr );
        }
        g_free( devnode );
        udev_device_unref( udevice );
    }
    return TRUE;
}


/* *************************************************************************
 * sanitize environ
************************************************************************** */

extern char **environ;

/* These arrays are both NULL-terminated. */
static char *spc_restricted_environ[  ] = {
  "IFS= \t\n",
  "PATH=" _PATH_STDPATH,
  0
};

static char *spc_preserve_environ[  ] = {
  "TZ",
  "LANG",
  "LC_ALL",
  "LC_COLLATE",
  "LC_CTYPE",
  0
};

void spc_sanitize_environment(int preservec, char **preservev) {
  int    i;
  char   **new_environ, *ptr, *value, *var;
  size_t arr_size = 1, arr_ptr = 0, len, new_size = 0;

  for (i = 0;  (var = spc_restricted_environ[i]) != 0;  i++) {
    new_size += strlen(var) + 1;
    arr_size++;
  }
  for (i = 0;  (var = spc_preserve_environ[i]) != 0;  i++) {
    if (!(value = getenv(var))) continue;
    new_size += strlen(var) + strlen(value) + 2; /* include the '=' */
    arr_size++;
  }
  if (preservec && preservev) {
    for (i = 0;  i < preservec && (var = preservev[i]) != 0;  i++) {
      if (!(value = getenv(var))) continue;
      new_size += strlen(var) + strlen(value) + 2; /* include the '=' */
      arr_size++;
    }
  }

  new_size += (arr_size * sizeof(char *));
  if (!(new_environ = (char **)malloc(new_size))) abort(  );
  new_environ[arr_size - 1] = 0;

  ptr = (char *)new_environ + (arr_size * sizeof(char *));
  for (i = 0;  (var = spc_restricted_environ[i]) != 0;  i++) {
    new_environ[arr_ptr++] = ptr;
    len = strlen(var);
    memcpy(ptr, var, len + 1);
    ptr += len + 1;
  }
  for (i = 0;  (var = spc_preserve_environ[i]) != 0;  i++) {
    if (!(value = getenv(var))) continue;
    new_environ[arr_ptr++] = ptr;
    len = strlen(var);
    memcpy(ptr, var, len);
    // corrected ptr + len + 1
    *(ptr + len) = '=';
    memcpy(ptr + len + 1, value, strlen(value) + 1);
    ptr += len + strlen(value) + 2; /* include the '=' */
  }
  if (preservec && preservev) {
    for (i = 0;  i < preservec && (var = preservev[i]) != 0;  i++) {
      if (!(value = getenv(var))) continue;
      new_environ[arr_ptr++] = ptr;
      len = strlen(var);
      memcpy(ptr, var, len);
      // corrected ptr + len + 1
      *(ptr + len) = '=';
      memcpy(ptr + len + 1, value, strlen(value) + 1);
      ptr += len + strlen(value) + 2; /* include the '=' */
    }
  }

  environ = new_environ;
}

/* *************************************************************************
 * priviledges
************************************************************************** */

static int   orig_ngroups = -1;
static gid_t orig_groups[NGROUPS_MAX];
static gid_t orig_rgid = -1;
static uid_t orig_ruid = -1;
static gid_t orig_egid = -1;
static uid_t orig_euid = -1;

void drop_privileges( int permanent )
{
    if ( geteuid() != 0 )
        return;

    if ( orig_euid == -1 )
    {
        orig_euid = geteuid();
        orig_egid = getegid();
        orig_ruid = getuid();
        orig_rgid = getgid();
        orig_ngroups = getgroups( NGROUPS_MAX, orig_groups );
    }

    // drop groups
    /* If root privileges are to be dropped, be sure to pare down the ancillary
    * groups for the process before doing anything else because the setgroups(  )
    * system call requires root privileges.  Drop ancillary groups regardless of
    * whether privileges are being dropped temporarily or permanently.
    */
    gid_t newgid = orig_rgid;
    setgroups( 1, &newgid );
#if !defined(linux)
    setegid( newgid );
    if ( permanent && setgid( newgid ) == -1 ) goto _drop_abort;
#else
    if ( setregid( permanent ? newgid : -1, newgid ) == -1 ) goto _drop_abort;
#endif

    // drop user
#if !defined(linux)
    seteuid( orig_ruid );
    if ( permanent && setuid( orig_ruid ) == -1 ) goto _drop_abort;
#else
    if ( setreuid( ( permanent ? orig_ruid : -1 ), orig_ruid ) == -1 ) goto _drop_abort;
#endif

    // verify if not originally root
    if ( orig_ruid != 0 )
    {
        if ( permanent )
        {
            if ( setegid( 0 ) != -1 || getegid() != newgid )
                goto _drop_abort;
            if ( seteuid( 0 ) != -1 || geteuid() != orig_ruid )
                goto _drop_abort;
        }
        else
        {
            if ( getegid() != newgid ) goto _drop_abort;
            if ( geteuid() != orig_ruid ) goto _drop_abort;
        }
    }

    return;
_drop_abort:
    printf( "udevil: error: unable to drop priviledges - please report this problem\n" );
    abort();
}

void restore_privileges()
{
    if ( orig_euid != 0 )
        return;

    seteuid( 0 );
    setegid( orig_egid );
    setgroups(orig_ngroups, orig_groups);
}

/* ************************************************************************ */

char* get_known_filesystems()
{
    // common types
    char* list = g_strdup( "btrfs,ext2,ext3,ext4,udf,iso9660,xfs,jfs,nilfs,reiserfs,reiser4,msdos,umsdos,vfat,exfat,ntfs" );

    // get additional types from files
    static const char *type_files[] = { "/proc/filesystems", "/etc/filesystems", NULL };
    gchar *filesystems;
    GError *error;
    gchar **lines;
    guint n;
    char* str;
    int i = 0;
    while ( type_files[i] )
    {
        filesystems = NULL;
        lines = NULL;
        error = NULL;
        if ( g_file_get_contents( type_files[i], &filesystems, NULL, NULL ) )
        {
            lines = g_strsplit (filesystems, "\n", -1);
            for (n = 0; lines != NULL && lines[n] != NULL; n++)
            {
                gchar **tokens;
                gint num_tokens;
                g_strdelimit (lines[n], " \t", ' ');
                g_strstrip (lines[n]);
                tokens = g_strsplit (lines[n], " ", -1);
                num_tokens = g_strv_length (tokens);
                if ( num_tokens == 1 )
                {
                    str = list;
                    list = g_strdup_printf( "%s,%s", str, tokens[0] );
                    g_free( str );
                }
                g_strfreev( tokens );
            }
            g_strfreev( lines );
            g_free( filesystems );
        }
        i++;
    }
    return list;
}

char *replace_string( const char* orig, const char* str, const char* replace,
                                                                gboolean quote )
{   // replace all occurrences of str in orig with replace, optionally quoting
    char* rep;
    const char* cur;
    char* result = NULL;
    char* old_result;
    char* s;

    if ( !orig || !( s = strstr( orig, str ) ) )
        return g_strdup( orig );  // str not in orig

    if ( !replace )
    {
        if ( quote )
            rep = g_strdup_printf( "''" );
        else
            rep = g_strdup_printf( "" );
    }
    else if ( quote )
        rep = g_strdup_printf( "'%s'", replace );
    else
        rep = g_strdup( replace );

    cur = orig;
    do
    {
        if ( result )
        {
            old_result = result;
            result = g_strdup_printf( "%s%s%s", old_result,
                                            g_strndup( cur, s - cur ), rep );
            g_free( old_result );
        }
        else
            result = g_strdup_printf( "%s%s", g_strndup( cur, s - cur ), rep );
        cur = s + strlen( str );
        s = strstr( cur, str );
    } while ( s );
    old_result = result;
    result = g_strdup_printf( "%s%s", old_result, cur );
    g_free( old_result );
    g_free( rep );
    return result;
}

static void free_command_data( CommandData* data )
{
    if ( !data )
        return;
    g_free( data->device_file );
    g_free( data->point );
    g_free( data->fstype );
    g_free( data->options );
    g_free( data->label );
    g_free( data->uuid );
    g_slice_free( CommandData, data );
}

char* read_config( const char* var, const char* type )
{
    char* line;

    if ( type && type[0] != '\0' )
    {
        // return config entry with _type if available
        char* var2 = g_strdup_printf( "%s_%s", var, type );
        line = read_config( var2, NULL );
        g_free( var2 );
        if ( line )
            return line;
    }

    GList* l;
    char* equal;

    int len = strlen( var );

    for ( l = config; l; l = l->next )
    {
        line = (char*)l->data;
        equal = strchr( line, '=' );
        if ( !equal || line == equal || equal - line != len )
            continue;
        if ( !strncmp( line, var, len ) )
            return equal + 1;
    }
    return NULL;
}

gboolean test_config( const char* var, const char* type )
{
    char* value = read_config( var, type );
    if ( !value )
        return FALSE;
    if ( !g_ascii_strcasecmp( value, "true" )
            || !g_ascii_strcasecmp( value, "yes" )
            || !strcmp( value, "1" ) )
        return TRUE;
    return FALSE;
}

static char* parse_config()
{
    FILE* file;
    char line[ 2048 ];
    char* conf_path;
    char* sline;
    char* equal;
    char* var;
    char* value;
    char* str;
    char* msg = NULL;

    conf_path = g_strdup_printf( "/etc/udevil/udevil-user-%s.conf", g_get_user_name() );
    file = fopen( conf_path, "r" );
    if ( !file )
    {
        g_free( conf_path );
        conf_path = g_strdup_printf( "/etc/udevil/udevil.conf" );
        file = fopen( conf_path, "r" );
    }
    if ( file )
    {
        int lc = 0;
        while ( fgets( line, sizeof( line ), file ) )
        {
            lc++;
            if ( !g_utf8_validate( line, -1, NULL ) )
            {
                fprintf( stderr, "udevil: error: %s line %d is not valid UTF-8\n", conf_path, lc );
                fclose( file );
                return NULL;
            }
            if ( !g_str_has_suffix( line, "\n" ) )
            {
                fprintf( stderr, "udevil: error: %s line %d is too long\n", conf_path, lc );
                fclose( file );
                return NULL;
            }
            strtok( line, "\r\n" );
            g_strstrip( line );
            if ( line[0] == '\0' || line[0] == '#' )
                continue;
            if ( !( equal = strchr( line, '=' ) ) )
            {
                fprintf( stderr, "udevil: error: %s line %d syntax error:\n", conf_path, lc );
                fprintf( stderr, "               %s\n", line );
                fclose( file );
                return NULL;
            }
            equal[0] = '\0';
            var = g_strdup( line );
            value = g_strdup( equal + 1 );
            equal[0] = '=';
            g_strstrip( var );
            g_strstrip( value );
            if ( var[0] == '\0' )
            {
                fprintf( stderr, "udevil: error: %s line %d syntax error:\n", conf_path, lc );
                fprintf( stderr, "               %s\n", line );
                fclose( file );
                return NULL;
            }
            if ( read_config( var, NULL ) )
            {
                fprintf( stderr, "udevil: error: %s line %d duplicate assignment:\n",
                                                                conf_path, lc );
                fprintf( stderr, "               %s\n", line );
                fclose( file );
                return NULL;
            }
            if ( g_str_has_prefix( var, "allowed_media_dirs" ) )
            {
                const char* user = g_get_user_name();
                if ( user && user[0] != '\0' )
                {
                    str = value;
                    value = replace_string( str, "$USER", user, FALSE );
                    g_free( str );
                }
                if ( strstr( value, "$UID" ) )
                {
                    char* uid = g_strdup_printf( "%d", getuid() );
                    str = value;
                    value = replace_string( str, "$UID", uid, FALSE );
                    g_free( str );
                    g_free( uid );
                }
                if ( strstr( value, "$GID" ) )
                {
                    char* gid = g_strdup_printf( "%d", getgid() );
                    str = value;
                    value = replace_string( str, "$GID", gid, FALSE );
                    g_free( str );
                    g_free( gid );
                }
            }
            else if ( g_str_has_prefix( var, "allowed_types" ) )
            {
                if ( !strcmp( value, "*" ) )
                {
                    str = value;
                    value = g_strdup( ALLOWED_TYPES );
                    g_free( str );
                }
                if ( strstr( value, "$KNOWN_FILESYSTEMS" ) )
                {
                    char* alltypes = get_known_filesystems();
                    str = value;
                    value = replace_string( str, "$KNOWN_FILESYSTEMS", alltypes, FALSE );
                    g_free( str );
                    g_free( alltypes );
                }
            }
            config = g_list_prepend( config, g_strdup_printf( "%s=%s", var, value ) );
            //fprintf( stderr, "LINE=[%s]  [%s]\n", line, (char*)config->data );
            //fprintf( stderr, "    READ %s\n", read_config( var, NULL ));
            g_free( var );
            g_free( value );
        }
        fclose( file );
    }
    else
    {
        msg = g_strdup_printf( "udevil: warning: /etc/udevil/udevil.conf could not be read\n" );
        g_free( conf_path );
        conf_path = NULL;
    }

    if ( ( str = read_config( "log_file", NULL ) ) && str[0] != '\0' )
        logfile = str;

    if ( conf_path )
    {
        msg = g_strdup_printf( "udevil: read config %s\n", conf_path );
        g_free( conf_path );
    }
    return msg;
}

static void wlog( const char* msg, const char* sub1, int volume )
{
    if ( abs( volume ) >= verbose )
    {
        if ( volume >= 0 )
            fprintf( stderr, msg, sub1 );
        else
            printf( msg, sub1 );
    }
    if ( logfile )
    {
        char* msgt = g_strdup_printf( msg, sub1 );
        if ( logmem )
        {
            char* str = logmem;
            logmem = g_strdup_printf( "%s%s", str, msgt );
            g_free( str );
        }
        else
            logmem = g_strdup( msgt );
        g_free( msgt );
    }
}

static void lock_log( gboolean lock )
{
    FILE* file;
    char* name;
    struct stat statbuf;

    const char* rlock = "/run/lock";
    if ( !( stat( rlock, &statbuf ) == 0 && S_ISDIR( statbuf.st_mode ) ) )
    {
        rlock = "/var/lock";
        if ( !( stat( rlock, &statbuf ) == 0 && S_ISDIR( statbuf.st_mode ) ) )
            return;
    }

    char* log_lock_file = g_build_filename( rlock, ".udevil-log-lock", NULL );
    if ( lock )
    {
        // wait up to 3 seconds for another udevil using log
        int i = 0;
        while ( i < 3 && stat( log_lock_file, &statbuf ) == 0 )
        {
            sleep( 1 );
            i++;
        }
        // create lock file
        if ( file = fopen( log_lock_file, "w" ) )
            fclose( file );
    }
    else
        unlink( log_lock_file );
    g_free( log_lock_file );
}

char* randhex8()
{
    char hex[9];
    uint n;

    n = rand();
    sprintf(hex, "%08x", n);
    return g_strdup( hex );
}

gboolean copy_file( const char* src, const char* dest )
{   // overwrites!
    int inF, ouF, bytes;
    char line[ 1024 ];

    if ( ( inF = open( src, O_RDONLY ) ) == -1 )
        return FALSE;

    unlink( dest );
    if ( ( ouF = open( dest, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR ) ) == -1 )
    {
        close(inF);
        return FALSE;
    }
    while ( ( bytes = read( inF, line, sizeof( line ) ) ) > 0 )
    {
        if ( write(ouF, line, bytes) <= 0 )
        {
            close(inF);
            close(ouF);
            unlink( dest );
            return FALSE;
        }
    }
    close(inF);
    close(ouF);
    return TRUE;
}

static void expire_log( guint days )
{
    FILE* file;
    FILE* file_new = NULL;
    char line[LINE_MAX];
    struct tm tm;
    time_t t;
    char* datestring;
    char* str;
    char* path_new = NULL;
    struct stat statbuf;

    if ( geteuid() || !days )
        return;

    // last cleaning over a day ago?
    const char* rlock = "/run/lock";
    if ( !( stat( rlock, &statbuf ) == 0 && S_ISDIR( statbuf.st_mode ) ) )
    {
        rlock = "/var/lock";
        if ( !( stat( rlock, &statbuf ) == 0 && S_ISDIR( statbuf.st_mode ) ) )
            rlock = NULL;
    }
    if ( rlock )
    {
        str = g_build_filename( rlock, ".udevil-log-clean", NULL );
        if ( stat( str, &statbuf ) == 0
                            && time( NULL ) - statbuf.st_mtime < 24 * 60 * 60 )
        {
            // cleaned less than 24 hours ago
            g_free( str );
            return;
        }
        unlink( str );
        if ( file = fopen( str, "w" ) )
            fclose( file );
        g_free( str );
    }

    // clean
    memset( &tm, 0, sizeof(struct tm) );

    file = fopen( logfile, "r" );
    if ( !file )
        return;

    time_t sec = days * 24 * 60 * 60;
    gboolean old_line = FALSE;
    while ( fgets( line, LINE_MAX, file ) != NULL )
    {
        // ignore lines until we find a date within range
        if ( !file_new )
        {
            if ( line[0] == '@' && strstr( line, "::" ) )
            {
                str = strstr( line, "::" );
                datestring = g_strndup( line + 1, str - line - 1 );
                t = 0;
                if ( strptime( datestring, "%d %b %Y %H:%M:%S", &tm ) )
                {
                    t = mktime( &tm );
                    if ( t == -1 )
                        t = 0;
                }
                g_free( datestring );
                if ( t != 0 && time( NULL ) - t < sec )
                {
                    // found a date within range
                    if ( !old_line )
                        break;   // no old material to remove

                    // start copying lines to tmp file
                    str = randhex8();
                    path_new = g_strdup_printf( "%s-%s.tmp", logfile, str );
                    g_free( str );
                    file_new = fopen( path_new, "w" );
                    if ( !file_new )
                        break;
                    chmod( path_new, S_IRWXU );
                }
                else if ( !old_line )
                    old_line = TRUE;
            }
        }
        if ( file_new && fputs( line, file_new ) < 0 )
        {
            // write error
            fclose( file_new );
            file_new = NULL;
            break;
        }
    }
    fclose( file );

    if ( file_new && fclose( file_new ) == 0 )
        copy_file( path_new, logfile );

    if ( path_new )
        unlink( path_new );
    g_free( path_new );
}

static void dump_log()
{
    if ( !logfile || !logmem || orig_euid != 0 )
        return;

    restore_privileges();
    if ( geteuid() != 0 )
        return;
    lock_log( TRUE );

    // clean expired log entries
    const char* daystr;
    struct stat statbuf;
    if ( daystr = read_config( "log_keep_days", NULL ) )
    {
        guint days = atoi( daystr );
        if ( days > 0 )
            expire_log( days > MAX_LOG_DAYS ? MAX_LOG_DAYS : days );
    }

    // write to log file
    gboolean fail = FALSE;
    FILE* file = fopen( logfile, "a" );
    if ( !file )
    {
        sleep( 1 );
        file = fopen( logfile, "a" );
    }
    if ( file )
    {
        if ( fprintf( file, logmem ) < 1 )
            fail = TRUE;
        if ( fclose( file ) != 0 )
            fail = TRUE;
    }
    if ( !file || fail )
        fprintf( stderr, "udevil: error: failed writing to log file '%s'\n", logfile );

    lock_log( FALSE );
    chmod( logfile, S_IRWXU );
    drop_privileges( 0 );
    
    g_free( logmem );
    logmem = NULL;
}

static gboolean validate_in_list( const char* name, const char* type, const char* test )
{
    char* list = NULL;
    char* str;
    char* comma;
    char* element;
    char* selement;

    if ( !name || !test )
        return FALSE;

    if ( !( list = read_config( name, type ) ) )
        return FALSE;

//printf("list[%s_%s] = {%s}\n", name, type, list );
    while ( list && list[0] )
    {
        if ( comma = strchr( list, ',' ) )
        {
            element = g_strndup( list, comma - list );
            list = comma + 1;
        }
        else
        {
            element = g_strdup( list );
            list = NULL;
        }
        selement = g_strstrip( element );
        if ( selement[0] == '\0' )
            continue;
//printf("    selement = {%s}\n", selement );
        if ( strcmp( selement, "*" ) == 0 ||
                                fnmatch( selement, test, FNM_PATHNAME ) == 0 )
        {
            g_free( element );
//printf( "    MATCH\n" );
            return TRUE;
        }
        g_free( element );
    }
    return FALSE;
}

static gboolean validate_in_groups( const char* name, const char* type,
                                                            const char* username )
{
    char* list = NULL;
    char* str;
    char* comma;
    char* element;
    char* selement;
    struct group *grp;
    char** members;

    if ( !name || !username )
        return FALSE;

    if ( !( list = read_config( name, type ) ) )
        return FALSE;

    while ( list && list[0] )
    {
        if ( comma = strchr( list, ',' ) )
        {
            element = g_strndup( list, comma - list );
            list = comma + 1;
        }
        else
        {
            element = g_strdup( list );
            list = NULL;
        }
        selement = g_strstrip( element );
        if ( selement[0] == '\0' )
            continue;
        if ( !strcmp( selement, "*" ) )
            return TRUE;

        if ( !strcmp( selement, "root" ) && geteuid() == 0 )
        {
            // Note: root is not a member of 'root' group according to members list
            g_free( element );
            return TRUE;
        }

        // username is member of group selement?
        grp = getgrnam ( selement );
        if ( grp )
        {
            members = grp->gr_mem;
            while ( *members )
            {
                if ( !strcmp( *(members), username ) )
                {
                    g_free( element );
                    return TRUE;
                }
                members++;
            }
        }
        g_free( element );
    }
    return FALSE;
}

static char* validate_options( const char* name, const char* type,
                                                            const char* options )
{
    char* fulllist = NULL;
    char* list;
    char* str;
    char* comma;
    char* element;
    char* oelement;
    char* selement;
    char* opt;
    const char* opts;
    gboolean found;
    char* ret = NULL;

    if ( !name || !options )
        return g_strdup( "INVALID" );

    if ( !( fulllist = read_config( name, type ) ) )
        // use no-conf default
        fulllist = ALLOWED_OPTIONS;

    opts = options;
    while ( opts && opts[0] )
    {
        // get an option
        if ( comma = strchr( opts, ',' ) )
        {
            oelement = g_strndup( opts, comma - opts );
            opts = comma + 1;
        }
        else
        {
            oelement = g_strdup( opts );
            opts = NULL;
        }
        opt = g_strstrip( oelement );
        if ( opt[0] == '\0' )
            continue;

        // option is in list?
        list = fulllist;
        found = FALSE;
        while ( list && list[0] )
        {
            if ( comma = strchr( list, ',' ) )
            {
                element = g_strndup( list, comma - list );
                list = comma + 1;
            }
            else
            {
                element = g_strdup( list );
                list = NULL;
            }
            selement = g_strstrip( element );
            if ( selement[0] == '\0' )
                continue;
            if ( fnmatch( selement, opt, 0 ) == 0 )
            {
                g_free( element );
                found = TRUE;
                break;
            }
            g_free( element );
        }
        if ( !found )
            ret = g_strdup( opt );
        g_free( oelement );
        if ( ret )
            return ret;
    }
    return NULL;  // all are valid
}

static char* get_ip( const char* hostname )
{
    struct addrinfo hints;
    struct addrinfo *result;
    char* ret = NULL;

    if ( !hostname )
        return NULL;

    memset( &hints, 0, sizeof( struct addrinfo ) );
    hints.ai_family = AF_UNSPEC;    // Allow IPv4 and IPv6
    hints.ai_socktype = 0;          // Any
    hints.ai_flags = 0;
    hints.ai_protocol = 0;          // Any

    if ( getaddrinfo( hostname, NULL, &hints, &result ) == 0 && result )
    {
        void *addr;
        char ipstr[INET6_ADDRSTRLEN];

        if ( result->ai_family == AF_INET )
        {   // IPv4
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)result->ai_addr;
            addr = &(ipv4->sin_addr);
        }
        else
        {   // IPv6
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)result->ai_addr;
            addr = &(ipv6->sin6_addr);
        }
        inet_ntop(result->ai_family, addr, ipstr, sizeof ipstr);
        ret = g_strdup( ipstr );
        freeaddrinfo(result);
    }
    return ret;
}

static gboolean get_realpath( char** path )
{
    char res_path[PATH_MAX];

    if ( !path || !*path || !( *path && *path[0] == '/' ) )
    {
        if ( path )
        {
            g_free( *path );
            *path = NULL;
        }
        return FALSE;
    }

    if ( realpath( *path, res_path ) )
    {
        g_free( *path );
        *path = g_strdup( res_path );
        return TRUE;
    }
    else
    {
        g_free( *path );
        *path = NULL;
        return FALSE;
    }
}

/*
int user_on_tty()
{   // ( source code taken from pmount - but this doesn't seem to find tty )
    // Checks if the user is physically logged in, by looking for an utmp
    // record pointing to a real tty.
    int retval = 0;

    const char* username = g_get_user_name();
    if ( !username )
        return retval;

    // parse the utmpx database
    struct utmpx * s;
    setutxent();			// rewind
    while(s = getutxent())
    {
        if(s->ut_type != USER_PROCESS)
            continue;
        if(! strcmp(s->ut_user, username))
        {
//printf( "line = %s\n", s->ut_line );
            if(! strncmp(s->ut_line, "tty", 3) && isdigit(s->ut_line[3]))
            {
                // Logged to a tty !
                retval = 1;
                break;
            }
        }
    }
    endutxent();
    return retval;
}
*/

static char* get_loop_from_file( const char* path )
{
    char* stdout = NULL;
    char* ret = NULL;
    char* str;
    int status = 0;
    int exit_status = 1;
    gchar *argv[4] = { NULL };

    int a = 0;
    argv[a++] = g_strdup( read_config( "losetup_program", NULL ) );
    if ( !argv[0] )
        return NULL;
    argv[a++] = g_strdup( "-j" );
    argv[a++] = g_strdup( path );
    restore_privileges();
    if ( g_spawn_sync( NULL, argv, NULL,
                        G_SPAWN_STDERR_TO_DEV_NULL,
                        NULL, NULL, &stdout, NULL, &status, NULL ) )
    {
        if ( status && WIFEXITED( status ) )
            exit_status = WEXITSTATUS( status );
        else
            exit_status = 0;

        if ( !exit_status && stdout )
        {
            if ( str = strchr( stdout, ':' ) )
            {
                str[0] = '\0';
                if ( g_str_has_prefix( stdout, "/dev/loop" ) )
                    ret = g_strdup( stdout );
            }
        }
        g_free( stdout );
    }
    else
        wlog( "udevil: warning: unable to run losetup (%s)\n",
                                    read_config( "losetup_program", NULL ), 1 );
    drop_privileges( 0 );
    return ret;
}

static char* get_file_from_loop( const char* device_file )
{
    char* stdout = NULL;
    char* ret = NULL;
    char* str;
    int status = 0;
    int exit_status = 1;
    gchar *argv[4] = { NULL };
    guint n;
    gchar **lines;

    char* devloop = g_strdup_printf( "%s: ", device_file );

    int a = 0;
    argv[a++] = g_strdup( read_config( "losetup_program", NULL ) );
    if ( !argv[0] )
        return NULL;
    argv[a++] = g_strdup( "-a" );
    restore_privileges();
    if ( g_spawn_sync( NULL, argv, NULL,
                        G_SPAWN_STDERR_TO_DEV_NULL,
                        NULL, NULL, &stdout, NULL, &status, NULL ) )
    {
        if ( status && WIFEXITED( status ) )
            exit_status = WEXITSTATUS( status );
        else
            exit_status = 0;

        if ( !exit_status && stdout )
        {
            lines = g_strsplit( stdout, "\n", 0 );
            for ( n = 0; lines[n] != NULL; n++ )
            {
                if ( g_str_has_prefix( lines[n], devloop )
                                        && ( str = strchr( lines[n], '(' ) ) )
                {
                    ret = g_strdup( str + 1 );
                    if ( ret[ strlen( ret ) - 1 ] == ')' )
                        ret[ strlen( ret ) - 1 ] = '\0';
                    else
                    {
                        g_free( ret );
                        ret = NULL;
                    }
                }
            }
            g_strfreev( lines );
        }
        g_free( stdout );
    }
    else
        wlog( "udevil: warning: unable to run losetup (%s)\n",
                                    read_config( "losetup_program", NULL ), 1 );
    drop_privileges( 0 );
    g_free( devloop );
    return ret;
}

static gboolean device_is_mounted_mtab( const char* device_file, char** mount_point )
{
    gchar *contents;
    gchar **lines;
    GError *error;
    guint n;
    gboolean ret = FALSE;
    char* str;
    char* file;
    gchar encoded_file[PATH_MAX];
    gchar encoded_point[PATH_MAX];


    if ( !device_file )
        return FALSE;

    contents = NULL;
    lines = NULL;
    error = NULL;
    if ( !g_file_get_contents( "/proc/mounts", &contents, NULL, NULL ) )
    {
        if ( !g_file_get_contents( "/etc/mtab", &contents, NULL, &error ) )
        {
            g_warning ("Error reading mtab: %s", error->message);
            g_error_free (error);
            return FALSE;
        }
    }
    lines = g_strsplit( contents, "\n", 0 );
    for ( n = 0; lines[n] != NULL; n++ )
    {
        if ( lines[n][0] == '\0' )
            continue;

        if ( sscanf( lines[n],
                  "%s %s ",
                  encoded_file,
                  encoded_point ) != 2 )
        {
            g_warning ("Error parsing mtab line '%s'", lines[n]);
            continue;
        }

        file = g_strcompress( encoded_file );
        if ( !g_strcmp0( device_file, file ) )
        {
            if ( mount_point )
                *mount_point = g_strcompress( encoded_point );
            ret = TRUE;
            break;
        }
        g_free( file );
    }
    g_free( contents );
    g_strfreev( lines );
    return ret;
}

static gboolean path_is_mounted_mtab( const char* path, char** device_file )
{
    gchar *contents;
    gchar **lines;
    GError *error;
    guint n;
    gboolean ret = FALSE;
    char* str;
    char* file;
    char* point;
    gchar encoded_file[PATH_MAX];
    gchar encoded_point[PATH_MAX];

    if ( !path )
        return FALSE;

    contents = NULL;
    lines = NULL;
    error = NULL;
    if ( !g_file_get_contents( "/proc/mounts", &contents, NULL, NULL ) )
    {
        if ( !g_file_get_contents( "/etc/mtab", &contents, NULL, &error ) )
        {
            g_warning ("Error reading mtab: %s", error->message);
            g_error_free (error);
            return FALSE;
        }
    }
    lines = g_strsplit( contents, "\n", 0 );
    for ( n = 0; lines[n] != NULL; n++ )
    {
        if ( lines[n][0] == '\0' )
            continue;

        if ( sscanf( lines[n],
                  "%s %s ",
                  encoded_file,
                  encoded_point ) != 2 )
        {
            g_warning ("Error parsing mtab line '%s'", lines[n]);
            continue;
        }

        point = g_strcompress( encoded_point );
        if ( !g_strcmp0( point, path ) )
        {
            if ( device_file )
                *device_file = g_strcompress( encoded_file );
            ret = TRUE;
            break;
        }
        g_free( point );
    }
    g_free( contents );
    g_strfreev( lines );
    return ret;
}

static gboolean path_is_mounted_block( const char* path, char** device_file )
{
    gchar *contents;
    gchar **lines;
    GError *error;
    guint n;
    gboolean ret = FALSE;
    guint major, minor;
    guint mount_id;
    guint parent_id;
    gchar encoded_root[PATH_MAX];
    gchar encoded_mount_point[PATH_MAX];
    gchar *mount_point;

    contents = NULL;
    lines = NULL;
    error = NULL;
    if (!g_file_get_contents ("/proc/self/mountinfo", &contents, NULL, &error))
    {
        g_warning ("Error reading /proc/self/mountinfo: %s", error->message);
        g_error_free (error);
        return FALSE;
    }

    /* See Documentation/filesystems/proc.txt for the format of /proc/self/mountinfo
    *
    * Note that things like space are encoded as \020.
    */
    lines = g_strsplit (contents, "\n", 0);
    for ( n = 0; lines[n] != NULL; n++ )
    {
        if ( strlen( lines[n] ) == 0 )
            continue;

        if ( sscanf( lines[n],
                  "%d %d %d:%d %s %s",
                  &mount_id,
                  &parent_id,
                  &major,
                  &minor,
                  encoded_root,
                  encoded_mount_point ) != 6 )
        {
            g_warning ("Error reading /proc/self/mountinfo: Error parsing line '%s'", lines[n]);
            continue;
        }

        /* ignore mounts where only a subtree of a filesystem is mounted */
        if ( g_strcmp0( encoded_root, "/" ) != 0 )
            continue;

        mount_point = g_strcompress( encoded_mount_point );
        if ( !mount_point || ( mount_point && mount_point[0] == '\0' ) )
        {
            g_free( mount_point );
            continue;
        }

        if ( !strcmp( path, mount_point ) )
        {
            ret = TRUE;
            break;
        }
        g_free (mount_point);
    }
    g_free( contents );
    g_strfreev( lines );
    if ( ret && device_file )
    {
        if ( udev = udev_new() )
        {
            struct udev_device *udevice;
            dev_t dev;
            dev = makedev( major, minor );
            udevice = udev_device_new_from_devnum( udev, 'b', dev );
            if ( udevice )
            {
                *device_file = g_strdup( udev_device_get_devnode( udevice ) );
                udev_device_unref( udevice );
            }
            udev_unref( udev );
        }
        else
            *device_file = NULL;
    }
    return ret;
}

static int exec_program( const char* var, const char* msg, gboolean show_error,
                                                           gboolean as_root )
{
    int status = 0;
    int exit_status = 1;
    gchar *argv[6] = { NULL };
    int a = 0;

    if ( !as_root && orig_ruid == 0 )
        return 0;
        
    const char* prog = read_config( var, NULL );
    if ( !( prog && prog[0] != '\0' ) )
        return 0;
    
    argv[a++] = g_strdup( prog );
    argv[a++] = g_strdup( g_get_user_name() );
    argv[a++] = g_strdup( msg );
    argv[a++] = g_strdup( cmd_line );

    // print
    char* allarg = g_strjoinv( "  ", argv );
    wlog( as_root ? "ROOT: %s\n" : "USER: %s\n", allarg, 0 );
    g_free( allarg );

    // priv
    if ( as_root )
    {
        restore_privileges();
        setreuid( 0, -1 );  // mount needs real uid
        setregid( 0, -1 );
    }

    // run
    if ( g_spawn_sync( NULL, argv, NULL, G_SPAWN_CHILD_INHERITS_STDIN,
                                        NULL, NULL, NULL, NULL, &status, NULL ) )
    {
        if ( status && WIFEXITED( status ) )
            exit_status = WEXITSTATUS( status );
        else
            exit_status = 0;
    }
    else
        wlog( "udevil: error: unable to run %s\n", prog, 2 );

    // unpriv
    if ( as_root )
    {
        setreuid( orig_ruid, -1 );
        setregid( orig_rgid, -1 );
        drop_privileges( 0 );
    }

    if ( exit_status )
    {
        char* str = g_strdup_printf( "      %s exit status = %d\n", prog, exit_status );
        wlog( str, NULL, 0 );
        g_free( str );
        if ( show_error )
        {
            str = g_strdup_printf( "udevil: denied: %s returned exit status %d\n",
                                                            var, exit_status );
            wlog( str, NULL, 2 );
            g_free( str );
        }
    }
    return exit_status;
}

static int umount_path( const char* path, gboolean force, gboolean lazy )
{
    // setup command
    int status = 0;
    int exit_status = 1;
    gchar *argv[6] = { NULL };
    int a = 0;
    argv[a++] = g_strdup( read_config( "umount_program", NULL ) );
    if ( !argv[0] )
        return 1;
    if ( verbose == 0 )
        argv[a++] = g_strdup( "-v" );
    if ( force )
        argv[a++] = g_strdup( "-f" );
    if ( lazy )
        argv[a++] = g_strdup( "-l" );    
    argv[a++] = g_strdup( path );

    // print
    char* allarg = g_strjoinv( " ", argv );
    wlog( "ROOT: %s\n", allarg, 0 );
    g_free( allarg );

    // priv
    restore_privileges();
    setreuid( 0, -1 );  // mount needs real uid
    setregid( 0, -1 );

    // run
    if ( g_spawn_sync( NULL, argv, NULL, 0, NULL, NULL, NULL, NULL, &status, NULL ) )
    {
        if ( status && WIFEXITED( status ) )
            exit_status = WEXITSTATUS( status );
        else
            exit_status = 0;
    }
    else
        wlog( "udevil: error: unable to run umount (%s)\n",
                                    read_config( "umount_program", NULL ), 2 );

    // unpriv
    setreuid( orig_ruid, -1 );
    setregid( orig_rgid, -1 );
    drop_privileges( 0 );

    if ( exit_status )
    {
        char* str = g_strdup_printf( "      umount exit status = %d\n", exit_status );
        wlog( str, NULL, 0 );
        g_free( str );
    }
    return exit_status;
}

static int mount_device( const char* device_file, const char* fstype,
                         const char* options, const char* point, gboolean as_root )
{
    // setup command
    int status = 0;
    int exit_status = 1;
    gchar *argv[11] = { NULL };
    int a = 0;
    argv[a++] = g_strdup( read_config( "mount_program", NULL ) );
    if ( !argv[0] )
        return 1;
    if ( verbose == 0 )
        argv[a++] = g_strdup( "-v" );
    if ( fstype && fstype[0] != '\0' )
    {
        argv[a++] = g_strdup( "-t" );
        argv[a++] = g_strdup( fstype );
    }
    if ( options && options[0] != '\0' )
    {
        argv[a++] = g_strdup( "-o" );
        argv[a++] = g_strdup( options );
    }
    argv[a++] = g_strdup( device_file );
    if ( point && point[0] != '\0' )
        argv[a++] = g_strdup( point );

    // print
    char* allarg = g_strjoinv( " ", argv );
    wlog( as_root ? "ROOT: %s\n" : "USER: %s\n", allarg, 0 );
    g_free( allarg );

    // priv
    if ( as_root )
    {
        restore_privileges();
        setreuid( 0, -1 );  // mount needs real uid
        setregid( 0, -1 );
    }

    // run
    if ( g_spawn_sync( NULL, argv, NULL, 0, NULL, NULL, NULL, NULL, &status, NULL ) )
    {
        if ( status && WIFEXITED( status ) )
            exit_status = WEXITSTATUS( status );
        else
            exit_status = 0;
    }
    else
        wlog( "udevil: error: unable to run mount (%s)\n",
                                    read_config( "mount_program", NULL ), 2 );

    // unpriv
    if ( as_root )
    {
        setreuid( orig_ruid, -1 );
        setregid( orig_rgid, -1 );
        drop_privileges( 0 );
    }

    if ( exit_status )
    {
        char* str = g_strdup_printf( "      mount exit status = %d\n", exit_status );
        wlog( str, NULL, 0 );
        g_free( str );
    }
    return exit_status;
}

static gboolean mount_knows( const char* device_file )
{
    int status = 0;
    int exit_status = 1;
    gchar *argv[4] = { NULL };
    int a = 0;
    argv[a++] = g_strdup( read_config( "mount_program", NULL ) );
    if ( !argv[0] )
        return FALSE;
    argv[a++] = g_strdup( "--fake" );
    argv[a++] = g_strdup( device_file );

    restore_privileges();
    setreuid( 0, -1 );  // mount needs real uid
    setregid( 0, -1 );
    if ( g_spawn_sync( NULL, argv, NULL,
                        G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                        NULL, NULL, NULL, NULL, &status, NULL ) )
    {
        if ( status && WIFEXITED( status ) )
            exit_status = WEXITSTATUS( status );
        else
            exit_status = 0;
    }
    else
        wlog( "udevil: warning: unable to run mount (%s)\n",
                                    read_config( "mount_program", NULL ), 1 );
    setreuid( orig_ruid, -1 );
    setregid( orig_rgid, -1 );
    drop_privileges( 0 );

    return ( exit_status == 0 );
}

static gboolean valid_mount_path( const char* path, char** errmsg )
{
    const char* msg = NULL;
    struct stat statbuf;

    if ( !path )
    {
        if ( errmsg )
            *errmsg = g_strdup_printf( "udevil: denied: Invalid path\n" );
        return FALSE;
    }

    if ( !g_file_test( path, G_FILE_TEST_IS_DIR )
                            || g_file_test( path, G_FILE_TEST_IS_SYMLINK ) )
        msg = "udevil: denied: mount path '%s' is not a directory\n";
    else if ( path_is_mounted_block( path, NULL ) )
        msg = "udevil: denied: mount path '%s' is already mounted\n";
    else if ( geteuid() != 0 )
    {
        if ( stat( path, &statbuf ) != 0 )
            msg = "udevil: denied: cannot stat '%s': %s\n";
        else if ( statbuf.st_uid != 0 && statbuf.st_uid != getuid() )
            msg = "udevil: denied: neither you nor root owns '%s'\n";
    }
    if ( errmsg )
        *errmsg = msg ? g_strdup_printf( msg, path, g_strerror( errno ) ) : NULL;
    return !msg;
}

static char* get_default_mount_dir( const char* type )
{
    char* list = NULL;
    char* str;
    char* comma;
    char* element;
    char* selement;

    if ( !( list = read_config( "allowed_media_dirs", type ) ) )
        return NULL;

    while ( list && list[0] )
    {
        if ( comma = strchr( list, ',' ) )
        {
            element = g_strndup( list, comma - list );
            list = comma + 1;
        }
        else
        {
            element = g_strdup( list );
            list = NULL;
        }
        selement = g_strstrip( element );
        if ( selement[0] == '\0' )
            continue;
        if ( selement[0] != '/' )
            continue;
        if ( !strchr( selement, '*' ) && !strchr( selement, '?' ) &&
                                g_file_test( selement, G_FILE_TEST_IS_DIR ) )
        {
            str = g_strdup( selement );
            g_free( element );
            return str;
        }
        g_free( element );
    }
    return NULL;
}

static int parse_network_url( const char* url, const char* fstype,
                                                        netmount_t** netmount )
{   // returns 0=not a network url  1=valid network url  2=invalid network url
    if ( !url || !netmount )
        return 0;

    int ret = 0;
    char* str;
    char* str2;
    netmount_t* nm = g_slice_new0( netmount_t );
    nm->fstype = NULL;
    nm->host = NULL;
    nm->ip = NULL;
    nm->port = NULL;
    nm->user = NULL;
    nm->pass = NULL;
    nm->path = NULL;

    if ( fstype && ( !strcmp( fstype, "nfs" ) || !strcmp( fstype, "smbfs" ) ) )
        ret = 2;

    char* orig_url = strdup( url );
    char* xurl = orig_url;
    if ( g_str_has_prefix( xurl, "ftp:" ) || g_str_has_prefix( xurl, "smb:" )
        || g_str_has_prefix( xurl, "nfs:" ) || g_str_has_prefix( xurl, "curlftpfs#ftp:" )
        || ( fstype && ( !strcmp( fstype, "curlftpfs" ) || !strcmp( fstype, "ftpfs" ) ) ) )
    {
        // mount nfs|smb|ftp:[//][<user>[:<pass>]@]<host>[:<port>]/<path>
        // mount -t [curl]ftpfs [<user>[:<pass>]@]<host>[:<port>][/<path>]
        if ( g_str_has_prefix( xurl, "smb:" ) )
        {
            nm->fstype = g_strdup( "smbfs" );
            xurl += 4;
        }
        else if ( g_str_has_prefix( xurl, "nfs:" ) )
        {
            nm->fstype = g_strdup( "nfs" );
            xurl += 4;
        }
        else if ( fstype && ( !strcmp( fstype, "curlftpfs" )
                                                || !strcmp( fstype, "ftpfs" ) ) )
        {
            if ( g_str_has_prefix( xurl, "ftp:" ) )
                xurl += 4;
            else if ( g_str_has_prefix( xurl, "curlftpfs#ftp:" ) )
                xurl += 14;
            nm->fstype = g_strdup( fstype );
        }
        else if ( g_str_has_prefix( xurl, "ftp:" ) )
        {
            // detect curlftpfs or ftpfs
            if ( str = g_find_program_in_path( "curlftpfs" ) )
                nm->fstype = g_strdup( "curlftpfs" );
            else
                nm->fstype = g_strdup( "ftpfs" );
            g_free( str );
            xurl += 4;
        }
        else if ( g_str_has_prefix( xurl, "curlftpfs#ftp:" ) )
        {
            nm->fstype = g_strdup( "curlftpfs" );
            xurl += 14;
        }

        ret = 2;
        while ( xurl[0] == '/' )
            xurl++;
        char* trim_url = g_strdup( xurl );

        // path
        if ( str = strchr( xurl, '/' ) )
        {
            nm->path = g_strdup( str );
            str[0] = '\0';
        }
        // user:pass
        if ( str = strchr( xurl, '@' ) )
        {
            str[0] = '\0';
            if ( str2 = strchr( xurl, ':' ) )
            {
                str2[0] = '\0';
                if ( str2[1] != '\0' )
                    nm->pass = g_strdup( str2 + 1 );
            }
            if ( xurl[0] != '\0' )
                nm->user = g_strdup( xurl );
            xurl = str + 1;
        }
        // host:port
        if ( xurl[0] == '[' )
        {
            // ipv6 literal
            if ( str = strchr( xurl, ']' ) )
            {
                str[0] = '\0';
                if ( xurl[1] != '\0' )
                    nm->host = g_strdup( xurl + 1 );
                if ( str[1] == ':' && str[2] != '\0' )
                    nm->port = g_strdup( str + 1 );
            }
        }
        else if ( xurl[0] != '\0' )
        {
            if ( str = strchr( xurl, ':' ) )
            {
                str[0] = '\0';
                if ( str[1] != '\0' )
                    nm->port = g_strdup( str + 1 );
            }
            nm->host = g_strdup( xurl );
        }

        // url
        if ( nm->host )
        {
            if ( g_str_has_prefix( url, "smb:" ) )
                nm->url = g_strdup_printf( "//%s%s", nm->host, nm->path ? nm->path : "/" );
            else if ( g_str_has_prefix( url, "nfs:" ) )
                nm->url = g_strdup_printf( "%s:%s", nm->host, nm->path ? nm->path : "/" );
            else if ( !g_strcmp0( nm->fstype, "curlftpfs" ) )
                nm->url = g_strdup_printf( "curlftpfs#ftp://%s%s%s%s%s%s%s%s",
                                nm->user ? nm->user : "",
                                nm->pass ? ":" : "",
                                nm->pass ? nm->pass : "",
                                nm->user || nm->pass ? "@" : "",
                                nm->host,
                                nm->port ? ":" : "",
                                nm->port ? nm->port : "",
                                nm->path ? nm->path : "/" );
            else if ( !g_strcmp0( nm->fstype, "ftpfs" ) )
                nm->url = g_strdup( "none" );
            else
                nm->url = g_strdup( trim_url );
        }
        g_free( trim_url );
    }
    else if ( g_str_has_prefix( xurl, "//" ) )
    {
        // mount [-t smbfs] //host[:<port>]/<path>
        nm->fstype = g_strdup( "smbfs" );
        nm->url = g_strdup( xurl );
        ret = 2;

        while ( xurl[0] == '/' )
            xurl++;

        // path
        if ( str = strchr( xurl, '/' ) )
        {
            nm->path = g_strdup( str );
            str[0] = '\0';
        }

        // host:port
        if ( xurl[0] == '[' )
        {
            // ipv6 literal
            if ( str = strchr( xurl, ']' ) )
            {
                str[0] = '\0';
                if ( xurl[1] != '\0' )
                    nm->host = g_strdup( xurl + 1 );
                if ( str[1] == ':' && str[2] != '\0' )
                    nm->port = g_strdup( str + 1 );
            }
        }
        else if ( xurl[0] != '\0' )
        {
            if ( str = strchr( xurl, ':' ) )
            {
                str[0] = '\0';
                if ( str[1] != '\0' )
                    nm->port = g_strdup( str + 1 );
            }
            nm->host = g_strdup( xurl );
        }
    }
    else if ( strstr( xurl, ":/" ) && xurl[0] != ':' && xurl[0] != '/' )
    {
        // mount [-t nfs] host:/path
        nm->fstype = g_strdup( "nfs" );
        nm->url = g_strdup( xurl );
        ret = 2;

        // path
        str = strstr( xurl, ":/" );
        nm->path = g_strdup( str + 1 );
        str[0] = '\0';

        // host
        if ( xurl[0] == '[' )
        {
            // ipv6 literal
            if ( str = strchr( xurl, ']' ) )
            {
                str[0] = '\0';
                if ( xurl[1] != '\0' )
                    nm->host = g_strdup( xurl + 1 );
            }
        }
        else if ( xurl[0] != '\0' )
            nm->host = g_strdup( xurl );
    }
    g_free( orig_url );

    // check user supplied fstype
    gboolean bad_type = FALSE;
    if ( fstype && ret == 2 && nm->host )
    {
        if ( !strcmp( nm->fstype, "nfs" ) && strcmp( fstype, "nfs" ) )
        {
            wlog( "udevil: error: invalid type '%s' for NFS share - must be 'nfs'\n",
                                                                fstype, 2 );
            bad_type = TRUE;
        }
        else if ( !strcmp( nm->fstype, "smbfs" ) && strcmp( fstype, "smbfs" ) )
        {
            wlog( "udevil: error: invalid type '%s' for SMB share - must be 'smbfs'\n",
                                                                fstype, 2 );
            bad_type = TRUE;
        }
        else if ( !strcmp( nm->fstype, "ftpfs" ) || !strcmp( nm->fstype, "curlftpfs" ) )
        {
            if ( strcmp( fstype, "ftpfs" ) && strcmp( fstype, "curlftpfs" ) )
            {
                wlog( "udevil: error: invalid type '%s' for FTP share - must be 'ftpfs' or 'curlftpfs'\n",
                                                                    fstype, 2 );
                bad_type = TRUE;
            }
            else
            {
                g_free( nm->fstype );
                nm->fstype = g_strdup( fstype );
            }
        }
    }

    // check user pass port
    if ( ret == 2 && !bad_type && nm->host )
    {
        if ( ( nm->user && strchr( nm->user, ' ' ) )
                || ( nm->pass && strchr( nm->pass, ' ' ) )
                || ( nm->port && strchr( nm->port, ' ' ) ) )
        {
            wlog( "udevil: error: invalid network url\n", fstype, 2 );
            bad_type = TRUE;
        }
    }

    // lookup ip
    if ( ret == 2 && !bad_type )
    {
        if ( nm->host )
        {
            if ( !( nm->ip = get_ip( nm->host ) ) || ( nm->ip && nm->ip[0] == '\0' ) )
            {
                wlog( "udevil: error: lookup host '%s' failed\n", nm->host, 2 );
                g_free( nm->host );
                nm->host = NULL;
            }
        }
        else
            wlog( "udevil: error: '%s' is not a recognized network url\n", url, 2 );
    }

    if ( ret == 0 || !nm->host || bad_type )
    {
        g_free( nm->url );
        g_free( nm->fstype );
        g_free( nm->host );
        g_free( nm->ip );
        g_free( nm->port );
        g_free( nm->user );
        g_free( nm->pass );
        g_free( nm->path );
        g_slice_free( netmount_t, nm );
        return ret;
    }

    *netmount = nm;
    return 1;
}

static int command_mount( CommandData* data )
{
    int type;
    enum {
        MOUNT_BLOCK,
        MOUNT_NET,
        MOUNT_FILE
    };
    struct stat statbuf;
    char* str;
    char* parent_dir;
    char* fstype;
    char* options = NULL;
    char* point = NULL;
    device_t *device = NULL;
    netmount_t *netmount = NULL;
    int ret = 0;
    int i;
    gboolean pass_loop = FALSE;

    // got root?
    if ( orig_euid != 0 )
    {
        wlog( "udevil: error: udevil was not run suid root\n", NULL, 2 );
        wlog( "        To correct this problem: sudo chmod +s /usr/bin/udevil\n", NULL, 2 );
        return 1;
    }

    //////////////// get device from label or uuid first

_get_type:
    // first argument ?
    if ( !( data->device_file && data->device_file[0] != '\0' ) )
    {
        if ( data->cmd_type == CMD_MOUNT )
            wlog( "udevil: error: mount requires DEVICE argument\n", NULL, 2 );
        else
            wlog( "udevil: error: unmount requires DEVICE argument\n", NULL, 2 );
        return 1;
    }
    if ( data->options && data->options[0] == '\0' )
    {
        g_free( data->options );
        data->options = NULL;
    }

    // determine mount type
    if ( i = parse_network_url( data->device_file, data->fstype, &netmount ) )
    {
        if ( i == 2 )
        {
            // invalid network url
            ret = 1;
            goto _finish;
        }
        type = MOUNT_NET;
    }
    else
    {
        // clean device_file
        while ( g_str_has_suffix( data->device_file, "/" )
                                                && data->device_file[1] != '\0' )
            data->device_file[ strlen( data->device_file ) - 1] = '\0';
        if ( data->device_file[0] != '/' )
        {
            wlog( "udevil: error: relative argument '%s' not permitted\n",
                                                            data->device_file, 2 );
            return 1;
        }

        // stat
        if ( stat( data->device_file, &statbuf ) != 0 )
        {
            str = g_strdup_printf( "udevil: error: cannot stat %s: %s\n",
                                        data->device_file, g_strerror( errno ) );
            wlog( str, NULL, 2 );
            g_free ( str );
            return 1;
        }
        else if ( statbuf.st_rdev == 0 || !S_ISBLK( statbuf.st_mode ) )
        {
            // not a block device
            if ( !S_ISREG( statbuf.st_mode ) && !S_ISDIR( statbuf.st_mode ) )
            {
                wlog( "udevil: error: '%s' is not a regular file or directory\n",
                                                        data->device_file, 2 );
                return 1;
            }
            type = MOUNT_FILE;
        }
        else
            type = MOUNT_BLOCK;
    }

    // determine device from unmount point
    if ( data->cmd_type == CMD_UNMOUNT && type == MOUNT_FILE )
    {
        if ( g_file_test( data->device_file, G_FILE_TEST_IS_DIR ) )
        {
            // unmounting a mount point directory - need to find device
            data->point = data->device_file;
            data->device_file = NULL;
            if ( path_is_mounted_block( data->point, &data->device_file )
                            && data->device_file && data->device_file[0] != '\0' )
                    type = MOUNT_BLOCK;
            else
            {
                g_free( data->device_file );
                data->device_file = NULL;
            }
            if ( !data->device_file )
            {
                // was not a mounted block device, look in mtab
                if ( path_is_mounted_mtab( data->point, &data->device_file )
                            && data->device_file && data->device_file[0] != '\0'
                            && !g_file_test( data->device_file, G_FILE_TEST_IS_DIR ) )
                {
                    goto _get_type;
                }
                else
                {
                    wlog( "udevil: error: cannot find '%s' mounted in mtab\n",
                                                                data->point, 2 );
                    return 1;
                }
            }
        }
        else
        {
            // unmounting a file
            if ( !get_realpath( &data->device_file ) )
            {
                wlog( "udevil: error: cannot canonicalize file path\n", NULL, 2 );
                return 1;
            }
            if ( str = get_loop_from_file( data->device_file ) )
            {
                // unmounting a file attached to loop
                if ( !get_realpath( &str ) )
                {
                    wlog( "udevil: error: cannot canonicalize attached loop device\n",
                                                            NULL, 2 );
                    return 2;
                }            
                if ( !validate_in_list( "allowed_types", g_get_user_name(), "file" ) )
                {
                    wlog( "udevil: denied: 'file' is not an allowed type\n",
                                                            NULL, 2 );
                    g_free( str );
                    return 2;
                }
                if ( !validate_in_list( "allowed_files", "file", data->device_file ) )
                {
                    wlog( "udevil: denied: '%s' is not an allowed file\n",
                                                            data->device_file, 2 );
                    g_free( str );
                    return 2;
                }
                if ( !device_is_mounted_mtab( str, &data->point ) )
                {
                    wlog( "udevil: error: cannot find '%s' mounted in mtab\n",
                                                            str, 2 );
                    g_free( str );
                    return 1;
                }

                // set device_file to loop device
                g_free( data->device_file );
                data->device_file = str;

                if ( stat( data->device_file, &statbuf ) == 0
                            && statbuf.st_rdev != 0 && S_ISBLK( statbuf.st_mode )
                            && g_str_has_prefix( data->device_file, "/dev/loop" ) )
                {
                    type = MOUNT_BLOCK;
                    pass_loop = TRUE;   // file is allowed so allow loop device
                }
                else
                {
                    wlog( "udevil: warning: attached device %s is not a loop device\n",
                                                            data->device_file, 1 );
                    g_free( data->point );
                    data->point = NULL;
                    goto _get_type;
                }
            }
        }
    }

    // canonicalize device_file
    if ( type != MOUNT_NET && !get_realpath( &data->device_file ) )
    {
        wlog( "udevil: error: cannot canonicalize device path\n", NULL, 2 );
        return 1;
    }

    // get fstype and device info
    if ( type == MOUNT_NET )
    {
        fstype = g_strdup( netmount->fstype );
    }
    else if ( type == MOUNT_FILE )
    {
        if ( stat( data->device_file, &statbuf ) != 0 )
        {
            str = g_strdup_printf( "udevil: error: cannot stat %s: %s\n",
                                    data->device_file, g_strerror( errno ) );
            wlog( str, NULL, 2 );
            g_free( str );
            ret = 1;
            goto _finish;
        }
        if ( data->fstype && data->fstype[0] != '\0' )
            fstype = g_strdup( data->fstype );
        else
            fstype = g_strdup( "file" );
    }
    else
    {
        // block device - get info
        udev = udev_new();
        if ( udev == NULL )
        {
            wlog( "udevil: error: error initializing libudev\n", NULL, 2 );
            ret = 1;
            goto _finish;
        }

        if ( stat( data->device_file, &statbuf ) != 0 )
        {
            str = g_strdup_printf( "udevil: error: cannot stat %s: %s\n",
                                    data->device_file, g_strerror( errno ) );
            wlog( str, NULL, 2 );
            g_free( str );
            udev_unref( udev );
            ret = 1;
            goto _finish;
        }
        if ( statbuf.st_rdev == 0 || !S_ISBLK( statbuf.st_mode ) )
        {
            wlog( "udevil: error: %s is not a block device\n",
                                                        data->device_file, 2 );
            udev_unref( udev );
            ret = 1;
            goto _finish;
        }
        struct udev_device *udevice = udev_device_new_from_devnum( udev, 'b',
                                                                statbuf.st_rdev );
        if ( udevice == NULL )
        {
            wlog( "udevil: error: no udev device for device %s\n",
                                                        data->device_file, 2 );
            udev_unref( udev );
            ret = 1;
            goto _finish;
        }

        device = device_alloc( udevice );
        if ( !device_get_info( device, devmounts ) )
        {
            wlog( "udevil: error: unable to get device info for device %s\n",
                                                            data->device_file, 2 );
            ret = 1;
        }
        udev_device_unref( udevice );
        udev_unref( udev );
        if ( ret != 0 )
            goto _finish;

        if ( data->fstype && data->fstype[0] != '\0' )
        {
            // user specified fstype
            if ( !strcmp( data->fstype, "nfs" ) || !strcmp( data->fstype, "smbfs" )
                                                 || !strcmp( data->fstype, "file" ) )
            {
                wlog( "udevil: error: invalid type '%s' for block device\n",
                                                                    data->fstype, 2 );
                ret = 1;
                goto _finish;
            }
            fstype = g_strdup( data->fstype );
        }
        else
        {
            // use udev determined type
            if ( device->id_type && device->id_type[0] != '\0' )
                fstype = g_strdup( device->id_type );
            else
            {
                if ( !device->device_is_mounted && !device->device_is_media_available )
                    wlog( "udevil: error: no media in device %s (or specify type with -t)\n",
                                                        data->device_file, 2 );
                else
                    wlog( "udevil: error: unable to determine device fstype - specify with -t\n", NULL, 2 );
                ret = 1;
                goto _finish;
            }
        }
    }

    // determine unmount point from device (requires device info)
    if ( data->cmd_type == CMD_UNMOUNT && !( data->point && data->point[0] != '\0' ) )
    {
        // user specified only a device
        if ( type == MOUNT_BLOCK && device )
        {
            // parse this device's mount points to see if one is valid - if so
            // use it as unmount point
            char* comma;
            char* element;
            char* selement;
            char* list = device->mount_points;
            while ( list && list[0] )
            {
                if ( comma = strchr( list, ',' ) )
                {
                    element = g_strndup( list, comma - list );
                    list = comma + 1;
                }
                else
                {
                    element = g_strdup( list );
                    list = NULL;
                }
                selement = g_strstrip( element );
                if ( selement[0] == '\0' )
                    continue;
                parent_dir = g_path_get_dirname( selement );
                if ( parent_dir[0] == '/' &&
                                    get_realpath( &parent_dir ) &&
                                    validate_in_list( "allowed_media_dirs",
                                                    fstype, parent_dir ) )
                {
                    // found a valid mount point
                    data->point = g_strdup( selement );
                    g_free( parent_dir );
                    g_free( element );
                    break;
                }
                g_free( parent_dir );
                g_free( element );
            }
        }
        if ( !data->point )
        {
            // no block device mount point found so look in mtab
            if ( !( device_is_mounted_mtab(
                        type == MOUNT_NET ? netmount->url : data->device_file,
                         &data->point )
                                && data->point && data->point[0] == '/' ) )
            {
                if ( device && !device->device_is_mounted )
                    wlog( "udevil: denied: device %s is not mounted\n",
                                                        data->device_file, 2 );
                else
                    wlog( "udevil: denied: could not find mount point for '%s'\n",
                            type == MOUNT_NET ? netmount->url : data->device_file, 2 );
                ret = 2;
                goto _finish;
            }
        }
    }

    // clean, canonicalize & test mount point
    if ( data->point )
    {
        // remove trailing slashes and spaces
        while ( ( g_str_has_suffix( data->point, "/" ) && data->point[1] != '\0' )
                    || g_str_has_suffix( data->point, " " ) )
            data->point[ strlen( data->point ) - 1] = '\0';
        if ( data->point[0] != '/' )
        {
            wlog( "udevil: error: relative argument '%s' not permitted\n",
                                                            data->point, 2 );
            ret = 1;
            goto _finish;
        }
    
        // canonicalize
        if ( stat( data->point, &statbuf ) == 0 )
        {
            if ( data->point[0] != '/' || !get_realpath( &data->point ) )
            {
                wlog( "udevil: error: cannot canonicalize mount point path\n", NULL, 2 );
                
                ret = 1;
                goto _finish;
            }
        }
        else
        {
            // point doesn't exist
            if ( data->cmd_type == CMD_UNMOUNT )
            {
                wlog( "udevil: error: cannot stat '%s'\n", data->point, 2 );
                ret = 1;
                goto _finish;
            }
            // canonicalize parent
            parent_dir = g_path_get_dirname( data->point );
            if ( !( parent_dir && parent_dir[0] == '/' ) )
            {
                wlog( "udevil: error: invalid mount point path\n", NULL, 2 );
                ret = 1;
                goto _finish;
            }
            if ( !get_realpath( &parent_dir ) )
            {
                wlog( "udevil: error: cannot canonicalize mount point path\n", NULL, 2 );
                ret = 1;
                goto _finish;
            }
            str = g_path_get_basename( data->point );
            g_free( data->point );
            data->point = g_build_filename( parent_dir, str, NULL );
            g_free( str );
            g_free( parent_dir );
            if ( stat( data->point, &statbuf ) == 0 && !get_realpath( &data->point ) )
            {
                wlog( "udevil: error: cannot canonicalize mount point path\n", NULL, 2 );
                ret = 1;
                goto _finish;
            }
        }

        // is parent dir an allowed media dir?
        parent_dir = g_path_get_dirname( data->point );
        if ( parent_dir[0] != '/' || !validate_in_list( "allowed_media_dirs", fstype,
                                                                parent_dir ) )
        {
            wlog( "udevil: denied: '%s' is not an allowed media directory\n",
                                                                parent_dir, 2 );
            g_free( parent_dir );
            ret = 2;
            goto _finish;
        }
        g_free( parent_dir );
    }
    
    // test fstype
    if ( fstype && strchr( fstype, ',' ) )
    {
        wlog( "udevil: error: multiple fstypes not allowed\n", NULL, 2 );
        ret = 1;
        goto _finish;
    }
    if ( !validate_in_list( "allowed_types", g_get_user_name(), fstype ) )
    {
        wlog( "udevil: denied: fstype '%s' is not an allowed type\n", fstype, 2 );
        ret = 2;
        goto _finish;
    }

    // test user
    const char* user_name = g_get_user_name();
    if ( !user_name || ( user_name && user_name[0] == '\0' ) )
    {
        wlog( "udevil: error: could not get username\n", NULL, 2 );
        ret = 1;
        goto _finish;
    }
    char* uid = g_strdup_printf( "UID=%d", getuid() );
    if ( !validate_in_list( "allowed_users", fstype, user_name ) &&
         !validate_in_list( "allowed_users", fstype, uid ) )
    {
        str = g_strdup_printf( "udevil: denied: user '%s' (%s) is not in allowed users\n",
                                                            user_name, uid );
        wlog( str, NULL, 2 );
        g_free( uid );
        g_free( str );
        ret = 2;
        goto _finish;
    }
    g_free( uid );
    /*
    if ( test_config( "tty_required", fstype ) && !user_on_tty() )
    {
        wlog( "udevil: denied: user '%s' is not on a real TTY (tty_required=1)\n",
                                                                user_name, 2 );
        ret = 2;
        goto _finish;
    }
    */

    // test groups
    if ( !validate_in_groups( "allowed_groups", fstype, user_name ) )
    {
        wlog( "udevil: denied: user '%s' is not in allowed groups\n", user_name, 2 );
        ret = 2;
        goto _finish;
    }

    // test network or device
    if ( type == MOUNT_NET )
    {
        str = NULL;
        if ( !validate_in_list( "allowed_networks", fstype, netmount->host )
                && !validate_in_list( "allowed_networks", fstype, netmount->ip ) )
        {
            str = g_strdup_printf( "udevil: denied: host '%s' (%s) is not an allowed network\n",
                                                        netmount->host, netmount->ip );
        }
        else if ( validate_in_list( "forbidden_networks", fstype, netmount->host )
                    || validate_in_list( "forbidden_networks", fstype, netmount->ip ) )
        {
            str = g_strdup_printf( "udevil: denied: host '%s' (%s) is a forbidden network\n",
                                                    netmount->host, netmount->ip );
        }
        if ( str )
        {
            wlog( str, NULL, 2 );
            g_free( str );
            ret = 2;
            goto _finish;
        }
    }
    else if ( type == MOUNT_BLOCK )
    {
        if ( !validate_in_list( "allowed_devices", fstype, data->device_file ) )
        {
            wlog( "udevil: denied: device %s is not an allowed device\n",
                                                            data->device_file, 2 );
            ret = 2;
            goto _finish;
        }
        if ( validate_in_list( "forbidden_devices", fstype, data->device_file ) )
        {
            wlog( "udevil: denied: device %s is a forbidden device\n",
                                                            data->device_file, 2 );
            ret = 2;
            goto _finish;
        }
    }
    else if ( type == MOUNT_FILE )
    {
        if ( !g_file_test( data->device_file, G_FILE_TEST_IS_DIR ) )
        {
            if ( !validate_in_list( "allowed_files", "file", data->device_file )
                 || validate_in_list( "forbidden_files", "file", data->device_file ) )
            {
                wlog( "udevil: denied: '%s' is not an allowed file\n",
                                                                data->device_file, 2 );
                ret = 2;
                goto _finish;
            }
            if ( g_access( data->device_file, R_OK ) != 0 )
            {
                wlog( "udevil: denied: you don't have read permission for file '%s'\n",
                                                                data->device_file, 2 );
                ret = 2;
                goto _finish;
            }
        }
        else if ( data->cmd_type == CMD_MOUNT && data->point )
        {
            wlog( "udevil: error: cannot specify mount point for directory\n", NULL, 2 );
            ret = 1;
            goto _finish;
        }
    }

    // allow unmount internal loop exception ?
    if ( data->cmd_type == CMD_UNMOUNT )
    {
        if ( type == MOUNT_BLOCK && !pass_loop
                    && g_str_has_prefix( data->device_file, "/dev/loop" )
                    && validate_in_list( "allowed_devices", fstype, data->device_file )
                    && !validate_in_list( "forbidden_devices", fstype, data->device_file ) )
        {
            // user is unmounting a loop device - attached to allowed file?
            if ( validate_in_list( "allowed_types", g_get_user_name(), "file" ) &&
                            ( str = get_file_from_loop( data->device_file ) ) )
            {
                if ( str[0] != '/' || !get_realpath( &str ) )
                {
                    wlog( "udevil: denied: %s is attached to an invalid file\n",
                                                        data->device_file, 2 );
                    ret = 2;
                    goto _finish;
                }

                if ( str[0] != '/' || g_access( str, R_OK ) )
                {
                    wlog( "udevil: denied: '%s' is not a permitted file\n",
                                                            str, 2 );
                    g_free( str );
                    ret = 2;
                    goto _finish;
                }

                if ( !validate_in_list( "allowed_files", "file", str ) )
                {
                    wlog( "udevil: denied: '%s' is not an allowed file\n",
                                                            str, 2 );
                    g_free( str );
                    ret = 2;
                    goto _finish;
                }
                g_free( str );
                pass_loop = TRUE;
            }
        }
    }

    // is device internal and real user is not root?
    if ( type == MOUNT_BLOCK && orig_ruid != 0 )
    {
        if ( device->device_is_system_internal &&
                !( device->id_uuid && device->id_uuid[0] != '\0' &&
                 validate_in_list( "allowed_internal_uuids", fstype, device->id_uuid ) ) &&
                !validate_in_list( "allowed_internal_devices", fstype, data->device_file ) &&
                !( g_str_has_prefix( data->device_file, "/dev/loop" ) && pass_loop ) )
        {
            wlog( "udevil: denied: device %s is an internal device and you're not root\n",
                                                            data->device_file, 2 );
            ret = 2;
            goto _finish;
        }
    }

    // CMD_UNMOUNT
    if ( data->cmd_type == CMD_UNMOUNT )
    {
        // validate exec
        str = g_strdup_printf( "%s is unmounting %s", g_get_user_name(),
                                                            data->point );
        ret = exec_program( "validate_rootexec", str, TRUE, TRUE );
        if ( !ret )
            ret = exec_program( "validate_exec", str, TRUE, FALSE );
        g_free( str );
        if ( ret )
            goto _finish;        
        
        // unmount
        if ( data->point && !( ret = umount_path( data->point, data->force,
                                                                data->lazy ) ) )
        {
            // remove mount point if udevil created
            str = g_build_filename( data->point, ".udevil-mount-point", NULL );
            restore_privileges();  // needed for stat
            if ( stat( str, &statbuf ) == 0 && statbuf.st_uid == 0 )
            {
                // .udevil-mount-point exists and is root-owned
                unlink ( str );
                rmdir( data->point );
            }
            g_free( str );
            drop_privileges( 0 );

            // success_exec
            str = g_strdup_printf( "%s unmounted %s", g_get_user_name(),
                                                        data->point );
            exec_program( "success_rootexec", str, FALSE, TRUE );
            exec_program( "success_exec", str, FALSE, FALSE );
            g_free( str );
        }
        goto _finish;
    }


    // ONLY CMD_MOUNT BELOW /////////////////////////////////////////////////

    // form options
    gboolean remount = FALSE;
    if ( data->options )
    {
        options = replace_string( data->options, " ", "", FALSE );
        if ( g_str_has_prefix( options, "remount," ) ||
                        g_str_has_suffix( options, ",remount" ) ||
                        !strcmp( options, "remount" ) ||
                        strstr( options, ",remount," ) )
             remount = TRUE;
        else
        {
            g_free( options );
            options = NULL;
        }
    }
    if ( !remount )
    {
        options = g_strdup( read_config( "default_options", fstype ) );
        if ( !options )
            options = g_strdup( ALLOWED_OPTIONS );
        if ( data->options )
        {
            str = options;
            options = g_strdup_printf( "%s,%s", str, data->options );
            g_free( str );
        }
        str = options;
        options = replace_string( str, " ", "", FALSE );
        g_free( str );
    }
    if ( type == MOUNT_NET )
    {
        char* net_opts = NULL;
        if ( !strcmp( fstype, "ftpfs" ) )
        {
            // ftpfs
            // eg mount -n -t ftpfs none /mnt/ftpfs -o ip=192.168.1.100 user=jim pass=123abc port=21 root=/pub/updates
            net_opts = g_strdup_printf( "ip=%s", netmount->ip );
            if ( netmount->user )
            {
                str = net_opts;
                net_opts = g_strdup_printf( "%s,user='%s'", str, netmount->user );
                g_free( str );
            }
            if ( netmount->pass )
            {
                str = net_opts;
                net_opts = g_strdup_printf( "%s,pass='%s'", str, netmount->pass );
                g_free( str );
            }
            if ( netmount->port )
            {
                str = net_opts;
                net_opts = g_strdup_printf( "%s,port=%s", str, netmount->port );
                g_free( str );
            }
            if ( netmount->path && strcmp( netmount->path, "/" ) )
            {
                str = net_opts;
                net_opts = g_strdup_printf( "%s,root='%s'", str, netmount->path );
                g_free( str );
            }
        }
        else if ( !strcmp( fstype, "nfs" ) )
        {
            if ( netmount->port )
                net_opts = g_strdup_printf( "port=%s", netmount->port );
        }
        else if ( !strcmp( fstype, "smbfs" ) )
        {
            net_opts = g_strdup( "" );
            if ( netmount->user )
            {
                str = net_opts;
                net_opts = g_strdup_printf( "%s,user='%s'", str, netmount->user );
                g_free( str );
            }
            if ( netmount->pass )
            {
                str = net_opts;
                net_opts = g_strdup_printf( "%s,password='%s'", str, netmount->pass );
                g_free( str );
            }
            if ( netmount->port )
            {
                str = net_opts;
                net_opts = g_strdup_printf( "%s,port=%s", str, netmount->port );
                g_free( str );
            }
        }
        if ( net_opts && net_opts[0] != '\0' )
        {
            if ( options )
            {
                str = options;
                options = g_strdup_printf( "%s,%s", str,
                                net_opts[0] == ',' ? net_opts + 1 : net_opts );
                g_free( str );
                g_free( net_opts );
            }
            else
                options = net_opts;
        }
        else
            g_free( net_opts );
    }

    // test options
    if ( ( i = strspn( options,
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-=+_,.\"'$/" ) )
                                                    != strlen( options ) )
    {
        str = g_strdup_printf( "udevil: error: options contain an invalid character ('%c')\n",
                                                                    options[i] );
        wlog( str, NULL, 2 );
        g_free( str );
        ret = 1;
        goto _finish;
    }
    if ( str = validate_options( "allowed_options", fstype, options ) )
    {
        wlog( "udevil: denied: option '%s' is not an allowed option\n", str, 2 );
        g_free( str );
        ret = 2;
        goto _finish;
    }
    if ( remount && type == MOUNT_NET && ( !strcmp( fstype, "ftpfs" )
                                        || !strcmp( fstype, "curlftpfs" ) ) )
    {
            wlog( "udevil: denied: cannot use remount option with FTP share\n",
                                                                    NULL, 2 );
            ret = 1;
            goto _finish;
    }

    // replace option variables
    if ( strstr( options, "$UID" ) )
    {
        char* uid = g_strdup_printf( "%d", getuid() );
        str = options;
        options = replace_string( str, "$UID", uid, FALSE );
        g_free( str );
        g_free( uid );
    }
    if ( strstr( options, "$GID" ) )
    {
        char* gid = g_strdup_printf( "%d", getgid() );
        str = options;
        options = replace_string( str, "$GID", gid, FALSE );
        g_free( str );
        g_free( gid );
    }
    if ( strstr( options, "$USER" ) )
    {
        str = options;
        options = replace_string( str, "$USER", g_get_user_name(), FALSE );
        g_free( str );
    }

    // replace fstype
    if ( type == MOUNT_NET && !strcmp( fstype, "curlftpfs" ) )
    {
        g_free( fstype );
        fstype = g_strdup( "fuse" );
    }

    // no point and not remount
    if ( !data->point && !remount )
    {
        if ( type == MOUNT_BLOCK && device->device_is_mounted )
        {
            wlog( "udevil: denied: device %s is already mounted (or specify mount point)\n",
                                                            data->device_file, 2 );
            ret = 2;
            goto _finish;
        }
        if ( type == MOUNT_FILE && !g_file_test( data->device_file, G_FILE_TEST_IS_DIR )
                            && ( str = get_loop_from_file( data->device_file ) ) )
        {
            wlog( "udevil: denied: file %s is already mounted (or specify mount point)\n",
                                                            data->device_file, 2 );
            g_free( str );
            ret = 2;
            goto _finish;
        }
        if ( type == MOUNT_NET && device_is_mounted_mtab( netmount->url, NULL ) )
        {
            wlog( "udevil: denied: %s is already mounted (or specify mount point)\n",
                                                            netmount->url, 2 );
            g_free( str );
            ret = 2;
            goto _finish;
        }

        if ( mount_knows( type == MOUNT_NET ? netmount->url : data->device_file ) )
        {
            // mount knows (in fstab) so mount as normal user with only specified opts
            wlog( "udevil: %s is known to mount - running mount as normal user\n",
                                                            data->device_file, 1 );
            if ( data->fstype )
                wlog( "udevil: warning: fstype ignored for device in fstab (or specify mount point)\n",
                                                                        NULL, 1 );
            if ( data->options )
                wlog( "udevil: warning: options ignored for device in fstab (or specify mount point)\n",
                                                                        NULL, 1 );

            ret = mount_device( type == MOUNT_NET ? netmount->url : data->device_file,
                                                        NULL, NULL, NULL, FALSE );
            // print
            if ( !ret )
            {
                if ( device_is_mounted_mtab(
                        type == MOUNT_NET ? netmount->url : data->device_file,
                        &str ) )
                {
                    str = g_strdup_printf( "Mounted %s at %s\n",
                            type == MOUNT_NET ? netmount->url : data->device_file,
                            str );
                }
                else
                    str = g_strdup_printf( "Mounted %s\n",
                            type == MOUNT_NET ? netmount->url : data->device_file );
                wlog( str, NULL, 1 );
                
                // success_exec
                if ( !ret )
                {
                    str = g_strdup_printf( "%s mounted %s (in fstab)",
                                g_get_user_name(),
                                type == MOUNT_NET ? netmount->url : data->device_file );
                    exec_program( "success_rootexec", str, FALSE, TRUE );
                    exec_program( "success_exec", str, FALSE, FALSE );
                    g_free( str );
                }
            }
            goto _finish;
        }
        else if ( type == MOUNT_FILE && g_file_test( data->device_file, G_FILE_TEST_IS_DIR ) )
        {
            // device is directory
            if ( path_is_mounted_mtab( data->device_file, NULL ) )
                wlog( "udevil: denied: '%s' is already mounted (or specify mount point)\n",
                                                        data->device_file, 2 );
            else
                wlog( "udevil: denied: can't mount '%s' (not in fstab?) (or specify mount point)\n",
                                                        data->device_file, 2 );
            ret = 2;
            goto _finish;
        }
    }

    // remount
    if ( remount )
    {
        if ( ( type != MOUNT_BLOCK && type != MOUNT_NET )
                        || g_file_test( data->device_file, G_FILE_TEST_IS_DIR ) )
        {
            wlog( "udevil: denied: must specify device or network for remount\n",
                                                                        NULL, 2 );
            ret = 2;
            goto _finish;
        }
        if ( data->point )
            wlog( "udevil: warning: specified mount point with remount ignored\n",
                                                                        NULL, 2 );

        // validate exec
        str = g_strdup_printf( "%s is remounting %s", g_get_user_name(),
                        type == MOUNT_NET ? netmount->url : data->device_file );
        ret = exec_program( "validate_rootexec", str, TRUE, TRUE );
        if ( !ret )
            ret = exec_program( "validate_exec", str, TRUE, FALSE );
        g_free( str );
        if ( ret )
            goto _finish;
            
        // remount
        if ( type == MOUNT_NET )
            ret = mount_device( netmount->url, data->fstype ? fstype : NULL, options,
                                                                        NULL, TRUE );
        else
            ret = mount_device( data->device_file,
                        data->fstype ? ( g_strcmp0( fstype, "file" ) ? fstype : NULL ) : NULL,
                        options, NULL, TRUE );

        // success_exec
        if ( !ret )
        {
            str = g_strdup_printf( "%s remounted %s",
                        g_get_user_name(),
                        type == MOUNT_NET ? netmount->url : data->device_file );
            exec_program( "success_rootexec", str, FALSE, TRUE );
            exec_program( "success_exec", str, FALSE, FALSE );
            g_free( str );
        }

        goto _finish;
    }

    // determine mount point
    if ( data->point )
    {
        // use user's mount point
        point = g_strdup( data->point );
        if ( type == MOUNT_BLOCK && device->device_is_mounted )
        {
            str = g_strdup_printf( "udevil: warning: device %s is already mounted on %s\n",
                                        data->device_file, device->mount_points );
            wlog( str, NULL, 1 );
            g_free( str );
        }
    }
    else
    {
        // get mount dir
        char* mount_dir = get_default_mount_dir( fstype );
        if ( !mount_dir )
        {
            wlog( "udevil: error: no existing directory in allowed_media_dirs\n",
                                                                        NULL, 2 );
            ret = 1;
            goto _finish;
        }

        // imagine a mount point
        char* mname = NULL;
        char* bdev = g_path_get_basename( data->device_file );
        if ( type == MOUNT_BLOCK )
        {
            if ( device->id_label && device->id_label[0] != '\0'
                                && device->id_label[0] != ' '
                                && g_utf8_validate( device->id_label, -1, NULL )
                                && !strchr( device->id_label, '/' ) )
                mname = g_strdup_printf( "%.20s", device->id_label );
            else if ( device->device_by_id && device->device_by_id[0] != '\0'
                                && g_utf8_validate( device->device_by_id, -1, NULL ) )
            {
                str = g_path_get_basename( device->device_by_id );
                mname = g_strdup_printf( "%s-%.20s", bdev, str );
                g_free( str );
            }
            else if ( device->id_uuid && device->id_uuid[0] != '\0' )
                mname = g_strdup_printf( "%s-%s", bdev, device->id_uuid );
            else
                mname = g_strdup( bdev );
        }
        else if ( type == MOUNT_NET )
        {
            if ( !strcmp( netmount->fstype, "nfs" ) )
                str = "nfs";
            else if ( !strcmp( netmount->fstype, "smbfs" ) )
                str = "smb";
            else
                str = "ftp";

            if ( netmount->host && g_utf8_validate( netmount->host, -1, NULL ) )
                mname = g_strdup_printf( "%s-%s", str, netmount->host );
            else
                mname = g_strdup( str );
        }

        if ( !mname )
            mname = g_strdup( bdev );

        // complete mount point
        char* point1 = g_build_filename( mount_dir, mname, NULL );
        g_free( bdev );
        g_free( mount_dir );
        g_free( mname );
        int r = 2;
        point = g_strdup( point1 );
        while ( g_file_test( point, G_FILE_TEST_EXISTS ) )
        {
            if ( valid_mount_path( point, NULL ) )
                break;
            g_free( point );
            point = g_strdup_printf( "%s-%d", point1, r++ );
        }
    }

    // validate mount point
    gboolean made_point = FALSE;
    if ( !g_utf8_validate( point, -1, NULL ) )
    {
        wlog( "udevil: error: mount point '%s' is not a valid UTF8 string\n", point, 2 );
        ret = 1;
        goto _finish;
    }
    if ( g_file_test( point, G_FILE_TEST_EXISTS ) )
    {
        if ( !valid_mount_path( point, &str ) )
        {
            printf( str );
            g_free( str );
            ret = 2;
            goto _finish;
        }
    }
    else
    {
        made_point = TRUE;
        restore_privileges();
        // mkdir
        if ( mkdir( point, S_IRWXU ) != 0 )
        {
            drop_privileges( 0 );
            str = g_strdup_printf( "udevil: error: mkdir '%s' as user %s failed\n", point,
                                                            g_get_user_name() );
            wlog( str, NULL, 2 );
            g_free( str );
            ret = 1;
            goto _finish;
        }
        // tag mount point created by udevil
        str = g_build_filename( point, ".udevil-mount-point", NULL );
        FILE* file = fopen( str, "w" );
        fclose( file );
        g_free( str );
        drop_privileges( 0 );
    }

    // validate exec
    str = g_strdup_printf( "%s is mounting %s to %s", g_get_user_name(),
                    type == MOUNT_NET ? netmount->url : data->device_file, point );
    ret = exec_program( "validate_rootexec", str, TRUE, TRUE );
    if ( !ret )
        ret = exec_program( "validate_exec", str, TRUE, FALSE );
    g_free( str );
    if ( ret )
        goto _finish;

    // mount
    if ( type == MOUNT_NET )
        ret = mount_device( netmount->url, fstype, options, point, TRUE );
    else
        ret = mount_device( data->device_file,
                            g_strcmp0( fstype, "file" ) ? fstype : NULL,
                            options, point, TRUE );

    // result
    if ( ret )
    {
        // remove mount point on error
        if ( made_point )
        {
            str = g_build_filename( point, ".udevil-mount-point", NULL );
            restore_privileges();
            unlink ( str );
            g_free( str );
            rmdir( point );
            drop_privileges( 0 );
        }
    }
    else
    {
        // set mount point mode if in conf
        int mode = 0755;
        str = read_config( "mount_point_mode", fstype );
        if ( str && str[0] != '\0' )
        {
            mode = strtol( str, NULL, 8 );
            if ( mode == 0 || str[0] != '0' )
            {
                wlog( "udevil: warning: invalid mount_point_mode in udevil.conf - using 0755\n",
                                                                    NULL, 1 );
                mode = 0755;
            }
            restore_privileges();
            chmod( point, mode );
            drop_privileges( 0 );
        }

        // print
        str = g_strdup_printf( "Mounted %s at %s\n",
                    type == MOUNT_NET ? netmount->url : data->device_file,
                    point );
        wlog( str, NULL, 1 );
        g_free( str );
        
        // success_exec
        str = g_strdup_printf( "%s mounted %s at %s",
                    g_get_user_name(),
                    type == MOUNT_NET ? netmount->url : data->device_file,
                    point );
        exec_program( "success_rootexec", str, FALSE, TRUE );
        exec_program( "success_exec", str, FALSE, FALSE );
        g_free( str );
    }

_finish:
    if ( netmount )
    {
        g_free( netmount->url );
        g_free( netmount->fstype );
        g_free( netmount->host );
        g_free( netmount->ip );
        g_free( netmount->port );
        g_free( netmount->user );
        g_free( netmount->pass );
        g_free( netmount->path );
        g_slice_free( netmount_t, netmount );
    }
    device_free( device );
    g_free( options );
    g_free( point );
    return ret;
}

static int command_clean()
{
    char* list = NULL;
    char* str;
    char* comma;
    char* element;
    char* selement;
    GDir* dir;
    char* path;
    const char* name;
    struct stat statbuf;

    // got root?
    if ( orig_euid != 0 )
    {
        wlog( "udevil: error: udevil was not run suid root\n", NULL, 2 );
        wlog( "        To correct this problem: sudo chmod +s /usr/bin/udevil\n", NULL, 2 );
        return 1;
    }

    /*
    if ( test_config( "tty_required", NULL ) && !user_on_tty() )
    {
        wlog( "udevil: denied: user '%s' is not on a real TTY (tty_required=1)\n",
                                                        g_get_user_name(), 2 );
        return 1;
    }
    */

    if ( !( list = read_config( "allowed_media_dirs", NULL ) ) )
        return 0;

    restore_privileges();
    while ( list && list[0] )
    {
        if ( comma = strchr( list, ',' ) )
        {
            element = g_strndup( list, comma - list );
            list = comma + 1;
        }
        else
        {
            element = g_strdup( list );
            list = NULL;
        }
        selement = g_strstrip( element );
        if ( selement[0] == '\0' )
            continue;
        if ( selement[0] != '/' )
            continue;
        if ( g_file_test( selement, G_FILE_TEST_IS_DIR ) )
        {
            dir = g_dir_open( selement, 0, NULL );
            if ( dir )
            {
                while ( name = g_dir_read_name( dir ) )
                {
                    path = g_build_filename( selement, name, ".udevil-mount-point", NULL );
                    if ( stat( path, &statbuf ) == 0 && S_ISREG( statbuf.st_mode )
                                                        && statbuf.st_uid == 0 )
                    {
                        unlink( path );
                        g_free( path );
                        path = g_build_filename( selement, name, NULL );
                        rmdir( path );
                        wlog( "udevil: cleaned '%s'\n", path, 0 );
                    }
                    g_free( path );
                }
                g_dir_close( dir );
            }
        }
        g_free( element );
    }
    drop_privileges( 0 );
    return 0;
}

static int command_info( CommandData* data )
{
    struct stat statbuf;
    struct udev_device  *udevice;
    const char* device_file = data->device_file;
    char* str;

    if ( !device_file || ( device_file && device_file[0] == '\0' ) )
    {
        wlog( "udevil: error: info requires DEVICE argument\n", NULL, 2 );
        return 1;
    }

    if ( stat( device_file, &statbuf ) != 0 )
    {
        str = g_strdup_printf( "udevil: error: cannot stat %s: %s\n",
                                    device_file, g_strerror( errno ) );
        wlog( str, NULL, 2 );
        g_free( str );
        return 1;
    }
    if ( statbuf.st_rdev == 0 || !S_ISBLK( statbuf.st_mode ) )
    {
        wlog( "udevil: error: %s is not a block device\n", device_file, 2 );
        return 1;
    }

    udev = udev_new();
    if ( udev == NULL )
    {
        wlog( "udevil: error: error initializing libudev\n", NULL, 2 );
        return 1;
    }

    udevice = udev_device_new_from_devnum( udev, 'b', statbuf.st_rdev );
    if ( udevice == NULL )
    {
        wlog( "udevil: error: no udev device for device %s\n", device_file, 2 );
        udev_unref( udev );
        return 1;
    }

    char* info;
    int ret = 0;
    device_t *device = device_alloc( udevice );
    if ( device_get_info( device, devmounts ) && ( info = device_show_info( device ) ) )
    {
        printf( "%s", info );
        g_free( info );
    }
    else
    {
        wlog( "udevil: error: unable to get device info\n", NULL, 2 );
        ret = 1;
    }

    device_free( device );
    udev_device_unref( udevice );
    udev_unref( udev );
    fflush( stdout );
    fflush( stderr );
    return ret;
}

void command_monitor_finalize()
{
    //if (signal == SIGINT || signal == SIGTERM)
    //printf( "\nudevil: SIGINT || SIGTERM\n");

    // stop mount monitor
    if ( mchannel )
    {
        g_io_channel_unref( mchannel );
        mchannel = NULL;
    }
    free_devmounts();

    // stop udev monitor
    if ( uchannel )
    {
        g_io_channel_unref( uchannel );
        uchannel = NULL;
    }
    if ( umonitor )
    {
        udev_monitor_unref( umonitor );
        umonitor = NULL;
    }
    if ( udev )
    {
        udev_unref( udev );
        udev = NULL;
    }
    //printf( "udevil: exit\n" );
    exit( 130 );  // same exit status as udisks v1
}

static int command_monitor()
{
    // create udev
    udev = udev_new();
    if ( !udev )
    {
        wlog( "udevil: error: unable to initialize udev\n", NULL, 2 );
        return 1;
    }

    // read all mount points
    parse_mounts( FALSE );

    // start udev monitor
    umonitor = udev_monitor_new_from_netlink( udev, "udev" );
    if ( !umonitor )
    {
        wlog( "udevil: error: cannot create udev monitor\n", NULL, 2 );
        goto finish_;
    }
    if ( udev_monitor_enable_receiving( umonitor ) )
    {
        wlog( "udevil: error: cannot enable udev monitor receiving\n", NULL, 2);
        goto finish_;
    }
    if ( udev_monitor_filter_add_match_subsystem_devtype( umonitor, "block", NULL ) )
    {
        wlog( "udevil: error: cannot set udev filter\n", NULL, 2);
        goto finish_;
    }

    gint ufd = udev_monitor_get_fd( umonitor );
    if ( ufd == 0 )
    {
        wlog( "udevil: error: cannot get udev monitor socket file descriptor\n", NULL, 2);
        goto finish_;
    }

    uchannel = g_io_channel_unix_new( ufd );
    g_io_channel_set_flags( uchannel, G_IO_FLAG_NONBLOCK, NULL );
    g_io_channel_set_close_on_unref( uchannel, TRUE );
    g_io_add_watch( uchannel, G_IO_IN | G_IO_HUP, // | G_IO_NVAL | G_IO_ERR,
                                            (GIOFunc)cb_udev_monitor_watch, NULL );

    // start mount monitor
    GError *error = NULL;
    mchannel = g_io_channel_new_file ( "/proc/self/mountinfo", "r", &error );
    if ( mchannel != NULL )
    {
        g_io_channel_set_close_on_unref( mchannel, TRUE );
        g_io_add_watch ( mchannel, G_IO_ERR, (GIOFunc)cb_mount_monitor_watch, NULL );
    }
    else
    {
        free_devmounts();
        wlog( "udevil: error: monitoring /proc/self/mountinfo: %s\n", error->message, 2 );
        g_error_free (error);
    }

    // signals
    signal(SIGTERM, command_monitor_finalize );
    signal(SIGINT,  command_monitor_finalize );

    wlog( "Monitoring activity from the disks daemon. Press Ctrl+C to cancel.\n", NULL, 1 );

    // main loop
    GMainLoop *main_loop = g_main_loop_new( NULL, FALSE );
    g_main_loop_run( main_loop );

    return 1;
finish_:
    if ( umonitor )
    {
        udev_monitor_unref( umonitor );
        umonitor = NULL;
    }
    if ( udev )
    {
        udev_unref( udev );
        udev = NULL;
    }
    return 1;
}

void command_interrupt()
{
    //if (signal == SIGINT || signal == SIGTERM)
    //printf( "\nudevil: SIGINT || SIGTERM\n");

    if ( udev )
    {
        udev_unref( udev );
        udev = NULL;
    }

    wlog( "\nudevil: exit: user aborted\n", NULL, 1 );
    dump_log();
    g_free( cmd_line );
    cmd_line = NULL;
    // free command data ?

    exit( 1 );
}

static void show_help()
{
    printf( _("udevil version %s\n"), UDEVIL_VERSION );
    printf( _("Mounts and unmounts devices without password, shows device info, monitors\n") );
    printf( "device changes.  Emulates udisks1/2 command line usage and udisks1 output.\n" );
    printf( "Usage: udevil [OPTIONS] COMMAND [COMMAND-OPTIONS] [COMMAND-ARGUMENTS]\n" );
    printf( "OPTIONS:\n" );
    printf( "    --verbose                                   print details\n" );
    printf( "    --quiet                                     minimal output\n" );
    printf( "MOUNT  -  Mounts DEVICE to mount point DIR with MOUNT-OPTIONS:\n" );
    printf( "    udevil mount|--mount [MOUNT-OPTIONS] [[-b|--block-device] DEVICE] [DIR]\n" );
    printf( "    MOUNT-OPTIONS:\n" );
    printf( "    -t|--types|--filesystem-type|--mount-fstype TYPE    (see man mount)\n" );
    printf( "    -o|--options|--mount-options OPT,...                (see man mount)\n" );
    //printf( "    -L LABEL                                    mount device by label LABEL\n" );
    //printf( "    -U UUID                                     mount device by UUID\n" );
    printf( "    --no-user-interaction                       ignored (for compatibility)\n" );
    printf( "    EXAMPLES:\n" );
    printf( "    udevil mount /dev/sdd1\n" );
    printf( "    udevil mount -o ro,noatime /dev/sdd1\n" );
    printf( "    udevil mount -o ro,noatime /dev/sdd1 /media/custom\n" );
    //printf( "    udevil mount -L 'Disk Label'\n" );
    printf( "    udevil mount /tmp/example.iso                    # ISO file\n" );
    printf( "    udevil mount ftp://sys.domain                    # ftp site - requires\n" );
    printf( "                                                       ftpfs or curlftpfs\n" );
    printf( "    udevil mount ftp://user:pass@sys.domain/share    # ftp share with\n" );
    printf( "                                                       user and password\n" );
    printf( "    udevil mount ftp://user:pass@sys.domain:21/share # ftp share with\n" );
    printf( "                                                       port, user and password\n" );
    printf( "    udevil mount -t ftpfs sys.domain                 # ftp site with ftpfs\n" );
    printf( "    udevil mount -t curlftpfs sys.domain             # ftp site with curl\n" );
    printf( "    udevil mount -t curlftpfs user:pass@sys.domain   # ftp site with curl u/p\n" );
    printf( "    udevil mount nfs://sys.domain:/share             # nfs share\n" );
    printf( "    udevil mount sys.domain:/share                   # nfs share\n" );
    printf( "    udevil mount smb://sys.domain/share              # samba share\n" );
    printf( "    udevil mount smb://user:pass@10.0.0.1:50/share   # samba share w/ u/p/port\n" );
    printf( "    udevil mount //sys.domain/share                  # samba share\n" );
    printf( "UNMOUNT  -  Unmount DEVICE or DIR with UNMOUNT-OPTIONS:\n" );
    printf( "    udevil umount|unmount|--unmount|--umount [UNMOUNT-OPTIONS] \n" );
    printf( "                                              {[-b|--block-device] DEVICE}|DIR\n" );
    printf( "    UNMOUNT-OPTIONS:\n" );
    printf( "    -l                                          lazy unmount (see man umount)\n" );
    printf( "    -f                                          force unmount (see man umount)\n" );
    printf( "    --no-user-interaction                       ignored (for compatibility)\n" );
    printf( "    EXAMPLES: udevil umount /dev/sdd1\n" );
    printf( "              udevil umount /media/disk\n" );
    printf( "              udevil umount -l /media/disk\n" );
    printf( "              udevil umount /tmp/example.iso\n" );
    printf( "INFO  -  Show information about DEVICE emulating udisks v1 output:\n" );
    printf( "    udevil info|--show-info|--info [-b|--block-device] DEVICE\n" );
    printf( "    EXAMPLE:  udevil info /dev/sdd1\n" );
    printf( "MONITOR  -  Display device events emulating udisks v1 output:\n" );
    printf( "    udevil monitor|--monitor\n" );
    printf( "    EXAMPLE:  udevil monitor\n" );
    printf( "CLEAN  -  Remove unmounted udevil-created mount dirs in media dirs\n" );
    printf( "    udevil clean\n" );
    printf( "HELP  -  Show this help\n" );
    printf( "    udevil help|--help|-h\n" );
    printf( "\n" );
    printf( "http://ignorantguru.github.com/udevil/  See /etc/udevil/udevil.conf for config.\n" );
    printf( "For automounting with udevil run 'devmon --help'\n" );

    printf( "\n" );
}

int main( int argc, char **argv )
{
    struct stat statbuf;
    char* str;
    char* config_msg = NULL;
    
    signal( SIGTERM, command_interrupt );
    signal( SIGINT,  command_interrupt );
/*
printf("\n-----------------------PRE-SANITIZE\n");
int i = 0;
while ( environ[i] )
    printf( "%s\n", environ[i++] );
*/
    spc_sanitize_environment( 0, NULL );
/*
printf("\n-----------------------POST-SANITIZE\n");
i = 0;
while ( environ[i] )
    printf( "%s\n", environ[i++] );
printf("\n-----------------------\n");
*/


    // read config
    if ( !( config_msg = parse_config() ) )
        return 1;

    // defaults
    str = read_config( "mount_program", NULL );
    if ( !str )
        config = g_list_prepend( config, g_strdup_printf( "mount_program=%s",
                                                            MOUNTPROG ) );
    str = read_config( "umount_program", NULL );
    if ( !str )
        config = g_list_prepend( config, g_strdup_printf( "umount_program=%s",
                                                            UMOUNTPROG ) );
    str = read_config( "losetup_program", NULL );
    if ( !str )
    {
        // find losetup
        if ( stat( LOSETUPPROG, &statbuf ) == 0 )
            config = g_list_prepend( config, g_strdup_printf( "losetup_program=%s",
                                                                LOSETUPPROG ) );
        else if ( stat( "/sbin/losetup", &statbuf ) == 0 )
            config = g_list_prepend( config, g_strdup_printf( "losetup_program=%s",
                                                                "/sbin/losetup" ) );
        else if ( stat( "/bin/losetup", &statbuf ) == 0 )
            config = g_list_prepend( config, g_strdup_printf( "losetup_program=%s",
                                                                "/bin/losetup" ) );
        else
            config = g_list_prepend( config, g_strdup_printf( "losetup_program=%s",
                                                                LOSETUPPROG ) );
    }


//printf( "R=%d:%d E=%d:%d\n", getuid(), getgid(), geteuid(), getegid() );
    drop_privileges( 0 );
//printf( "R=%d:%d E=%d:%d\n", getuid(), getgid(), geteuid(), getegid() );


    // log
    srand( (unsigned int)time( 0 ) + getpid() );
    cmd_line = g_strjoinv( " ", argv );
    char datestring[256];
    time_t t;
    time( &t );
    if ( strftime( datestring, sizeof( datestring ), "%d %b %Y %H:%M:%S",
                                                    localtime( &t ) ) != 0 )
    {
        str = g_strdup_printf( "\n@%s::%s$ %s\n", datestring, g_get_user_name(),
                                                                        cmd_line );
        wlog( str, NULL, 0 );
        g_free( str );
    }
    if ( config_msg )
        wlog( config_msg, NULL, strstr( config_msg, "warning:" ) ? 1 : 0 );
    g_free( config_msg );

    // init data
    CommandData* data = g_slice_new0( CommandData );
    data->cmd_type = CMD_UNSET;
    data->device_file = NULL;
    data->point = NULL;
    data->fstype = NULL;
    data->options = NULL;
    data->label = NULL;
    data->uuid = NULL;
    data->force = FALSE;
    data->lazy = FALSE;

    // parse arguments
    char* arg;
    char* arg_next;
    char* arg_short = NULL;
    char* equal;
    int ac = 1;
    int next_inc;
    while ( ac < argc )
    {
        arg = argv[ac];
        next_inc = 0;
        if ( g_str_has_prefix( arg, "--" ) && ( equal = strchr( arg, '=' ) ) )
        {
            arg = arg_short = g_strndup( arg, equal - arg );
            arg_next = equal + 1;
        }
        else if ( g_str_has_prefix( arg, "-" ) && ac + 1 < argc )
        {
            arg_next = argv[ac + 1];
            if ( arg_next[0] == '-' )
                arg_next = NULL;
            else
                next_inc = 1;
        }
        else
            arg_next = NULL;

        switch ( data->cmd_type )
        {
            case CMD_UNSET:
                if ( !strcmp( arg, "mount" ) || !strcmp( arg, "--mount" ) )
                {
                    data->cmd_type = CMD_MOUNT;
                    if ( arg_next )
                    {
                        data->device_file = g_strdup( arg_next );
                        ac += next_inc;
                    }
                }
                else if ( !strcmp( arg, "unmount" ) || !strcmp( arg, "--unmount" )
                           || !strcmp( arg, "umount" ) || !strcmp( arg, "--umount" ) )
                {
                    data->cmd_type = CMD_UNMOUNT;
                    if ( arg_next )
                    {
                        data->device_file = g_strdup( arg_next );
                        ac += next_inc;
                    }
                }
                else if ( !strcmp( arg, "monitor" ) || !strcmp( arg, "--monitor" ) )
                {
                    data->cmd_type = CMD_MONITOR;
                    if ( arg_next )
                    {
                        arg = arg_next;
                        goto _reject_arg;
                    }
                }
                else if ( !strcmp( arg, "clean" ) )
                {
                    data->cmd_type = CMD_CLEAN;
                    if ( arg_next )
                    {
                        arg = arg_next;
                        goto _reject_arg;
                    }
                }
                else if ( !strcmp( arg, "info" ) || !strcmp( arg, "--show-info" )
                                                    || !strcmp( arg, "--info" ) )
                {
                    data->cmd_type = CMD_INFO;
                    if ( arg_next )
                    {
                        data->device_file = g_strdup( arg_next );
                        ac += next_inc;
                    }
                }
                else if ( !strcmp( arg, "--verbose" ) )
                    verbose = 0;
                else if ( !strcmp( arg, "--quiet" ) )
                    verbose = 2;
                else if ( !strcmp( arg, "help" ) || !strcmp( arg, "--help" )
                                                    || !strcmp( arg, "-h" ) )
                {
                    dump_log();
                    drop_privileges( 1 );
                    show_help();
                    goto _exit;
                }
                else
                    goto _reject_arg;
                break;
            case CMD_MOUNT:
                if ( !strcmp( arg, "-b" ) || !strcmp( arg, "--block-device" ) )
                {
                    if ( !arg_next )
                        goto _reject_missing_arg;
                    if ( data->device_file )
                        goto _reject_too_many;
                    data->device_file = g_strdup( arg_next );
                    ac += next_inc;
                }
                else if ( !strcmp( arg, "-t" ) || !strcmp( arg, "--filesystem-type" )
                                                || !strcmp( arg, "--types" )
                                                || !strcmp( arg, "--mount-fstype" ) )
                {
                    if ( !arg_next )
                        goto _reject_missing_arg;
                    if ( data->fstype )
                        goto _reject_too_many;
                    data->fstype = g_strdup( arg_next );
                    ac += next_inc;
                }
                else if ( !strcmp( arg, "-o" ) || !strcmp( arg, "--options" )
                                        || !strcmp( arg, "--mount-options" ) )
                {
                    if ( !arg_next )
                        goto _reject_missing_arg;
                    if ( data->options )
                        goto _reject_too_many;
                    data->options = g_strdup( arg_next );
                    ac += next_inc;
                }
                else if ( !strcmp( arg, "-L" ) )
                {
                    if ( !arg_next )
                        goto _reject_missing_arg;
                    if ( data->label )
                        goto _reject_too_many;
                    data->label = g_strdup( arg_next );
                    ac += next_inc;
                }
                else if ( !strcmp( arg, "-U" ) )
                {
                    if ( !arg_next )
                        goto _reject_missing_arg;
                    if ( data->uuid )
                        goto _reject_too_many;
                    data->uuid = g_strdup( arg_next );
                    ac += next_inc;
                }
                else if ( !strcmp( arg, "--no-user-interaction" ) )
                {
                    // ignore
                }
                else if ( !strcmp( arg, "--verbose" ) )
                    verbose = 0;
                else if ( !strcmp( arg, "--quiet" ) )
                    verbose = 2;
                else if ( arg[0] == '-' )
                    goto _reject_arg;
                else
                {
                    if ( data->device_file )
                    {
                        if ( data->point )
                            goto _reject_too_many;
                        data->point = g_strdup( arg );
                    }
                    else
                        data->device_file = g_strdup( arg );
                }
                break;
            case CMD_UNMOUNT:
                if ( !strcmp( arg, "-b" ) || !strcmp( arg, "--block-device" ) )
                {
                    if ( !arg_next )
                        goto _reject_missing_arg;
                    if ( data->device_file )
                        goto _reject_too_many;
                    data->device_file = g_strdup( arg_next );
                    ac += next_inc;
                }
                else if ( !strcmp( arg, "-f" ) || !strcmp( arg, "--force" ) )
                {
                    data->force = TRUE;
                }
                else if ( !strcmp( arg, "-l" ) )
                {
                    data->lazy = TRUE;
                }
                else if ( !strcmp( arg, "-fl" ) || !strcmp( arg, "-lf" ) )
                {
                    data->force = TRUE;
                    data->lazy = TRUE;
                }
                else if ( !strcmp( arg, "--no-user-interaction" ) )
                {
                    // ignore
                }
                else if ( !strcmp( arg, "--verbose" ) )
                    verbose = 0;
                else if ( !strcmp( arg, "--quiet" ) )
                    verbose = 2;
                else if ( arg[0] == '-' )
                    goto _reject_arg;
                else
                {
                    if ( data->device_file )
                        goto _reject_too_many;
                    data->device_file = g_strdup( arg );
                }
                break;
            case CMD_MONITOR:
                if ( !strcmp( arg, "--verbose" ) )
                    verbose = 0;
                else if ( !strcmp( arg, "--quiet" ) )
                    verbose = 2;
                else if ( arg[0] == '-' )
                    goto _reject_arg;
                goto _reject_too_many;
            case CMD_CLEAN:
                if ( !strcmp( arg, "--verbose" ) )
                    verbose = 0;
                else if ( !strcmp( arg, "--quiet" ) )
                    verbose = 2;
                else if ( arg[0] == '-' )
                    goto _reject_arg;
                goto _reject_too_many;
            case CMD_INFO:
                if ( !strcmp( arg, "-b" ) || !strcmp( arg, "--block-device" ) )
                {
                    if ( !arg_next )
                        goto _reject_missing_arg;
                    if ( data->device_file )
                        goto _reject_too_many;
                    data->device_file = g_strdup( arg_next );
                    ac += next_inc;
                }
                else if ( !strcmp( arg, "--verbose" ) )
                    verbose = 0;
                else if ( !strcmp( arg, "--quiet" ) )
                    verbose = 2;
                else if ( arg[0] == '-' )
                    goto _reject_arg;
                else
                {
                    if ( data->device_file )
                        goto _reject_too_many;
                    data->device_file = g_strdup( arg );
                }
                break;
        }
        g_free( arg_short );
        arg_short = NULL;
        ac++;
    }

    // print command data
/*
    printf( "\ntype = \t\t%d\n", data->cmd_type );
    if ( data->device_file )
        printf( "device_file = \t%s\n", data->device_file );
    if ( data->point )
        printf( "point = \t%s\n", data->point );
    if ( data->fstype )
        printf( "fstype = \t%s\n", data->fstype );
    if ( data->options )
        printf( "options = \t%s\n", data->options );
    if ( data->label )
        printf( "label = \t%s\n", data->label );
    if ( data->uuid )
        printf( "uuid = \t\t%s\n", data->uuid );
    if ( data->lazy )
        printf( "is_lazy\n" );
    if ( data->force )
        printf( "is_force\n" );
    printf( "\n");
*/

    // perform command
    int ret = 0;
    switch ( data->cmd_type )
    {
        case CMD_MOUNT:
            ret = command_mount( data );
            break;
        case CMD_UNMOUNT:
            ret = command_mount( data );
            break;
        case CMD_MONITOR:
            dump_log();
            drop_privileges( 1 );
            g_free( cmd_line );
            cmd_line = NULL;
            free_command_data( data );
            data = NULL;
            ret = command_monitor();  // likely will exit
            break;
        case CMD_CLEAN:
            ret = command_clean();
            break;
        case CMD_INFO:
            dump_log();
            drop_privileges( 1 );
            ret = command_info( data );
            break;
        default:
            dump_log();
            drop_privileges( 1 );
            show_help();
    }

    free_command_data( data );
    dump_log();
    g_free( cmd_line );
    return ret;

_reject_too_many:
    wlog( "udevil: error: too many arguments\n", NULL, 2 );
    goto _exit;
_reject_missing_arg:
    wlog( "udevil: error: option '%s' requires an argument\n", arg, 2 );
    goto _exit;
_reject_arg:
    if ( arg[0] == '-' )
        wlog( "udevil: error: invalid option '%s'\n", arg, 2 );
    else
        wlog( "udevil: error: invalid or unexpected argument '%s'\n", arg, 2 );
_exit:
    g_free( arg_short );
    free_command_data( data );
    dump_log();
    g_free( cmd_line );
    return 1;
}