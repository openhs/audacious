/*  Audacious - Cross-platform multimedia player
 *  Copyright (C) 2005-2007  William Pitcock, Tony Vroon, George Averill,
 *                           Giacomo Lozito, Derek Pomery and Yoshiki Yazawa.
 *
 *  XMMS - Cross-platform multimedia player
 *  Copyright (C) 1998-2003  Peter Alm, Mikael Alm, Olle Hallnas,
 *                           Thomas Nilsson and 4Front Technologies
 *  Copyright (C) 1999-2003  Haavard Kvaalen
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */
#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <glib.h>
#include "audacious/titlestring.h"
#include "input.h"

G_BEGIN_DECLS

typedef enum {
    PLAYLIST_SORT_PATH,
    PLAYLIST_SORT_FILENAME,
    PLAYLIST_SORT_TITLE,
    PLAYLIST_SORT_ARTIST,
    PLAYLIST_SORT_DATE,
    PLAYLIST_SORT_TRACK,
    PLAYLIST_SORT_PLAYLIST
} PlaylistSortType;

typedef enum {
    PLAYLIST_DUPS_PATH,
    PLAYLIST_DUPS_FILENAME,
    PLAYLIST_DUPS_TITLE
} PlaylistDupsType;

typedef enum {
    PLAYLIST_FORMAT_UNKNOWN = -1,
    PLAYLIST_FORMAT_M3U,
    PLAYLIST_FORMAT_PLS,
    PLAYLIST_FORMAT_COUNT
} PlaylistFormat;

#define PLAYLIST_ENTRY(x)  ((PlaylistEntry*)(x))
typedef struct _PlaylistEntry {
    gchar *filename;
    gchar *title;
    gint length;
    gboolean selected;
    InputPlugin *decoder;
    TitleInput *tuple;		/* cached entry tuple, if available */
} PlaylistEntry;

#define PLAYLIST(x)  ((Playlist *)(x))
typedef struct _Playlist {
    gchar         *title;
    gchar         *filename;
    gint           length;
    GList         *entries;
    GList         *queue;
    GList         *shuffle;
    PlaylistEntry *position;    /* bleah */
    gulong         pl_total_time;
    gulong         pl_selection_time;
    gboolean       pl_total_more;
    gboolean       pl_selection_more;
    gboolean       loading_playlist;
    GMutex        *mutex;       /* this is required for multiple playlist */
} Playlist;

typedef enum {
    PLAYLIST_ASSOC_LINEAR,
    PLAYLIST_ASSOC_QUEUE,
    PLAYLIST_ASSOC_SHUFFLE
} PlaylistAssociation;

PlaylistEntry *playlist_entry_new(const gchar * filename,
                                  const gchar * title, const gint len,
				  InputPlugin * dec);
void playlist_entry_free(PlaylistEntry * entry);

void playlist_entry_associate(Playlist * playlist, PlaylistEntry * entry,
                              PlaylistAssociation assoc);

void playlist_entry_associate_pos(Playlist * playlist, PlaylistEntry * entry,
                                  PlaylistAssociation assoc, gint pos);

void playlist_init(void);
void playlist_add_playlist(Playlist *);
void playlist_remove_playlist(Playlist *);
void playlist_select_playlist(Playlist *);
void playlist_select_next(void);
void playlist_select_prev(void);
GList * playlist_get_playlists(void);

void playlist_clear(Playlist *playlist);
void playlist_delete(Playlist *playlist, gboolean crop);

gboolean playlist_add(Playlist *playlist, const gchar * filename);
gboolean playlist_ins(Playlist *playlist, const gchar * filename, gint pos);
guint playlist_add_dir(Playlist *playlist, const gchar * dir);
guint playlist_ins_dir(Playlist *playlist, const gchar * dir, gint pos, gboolean background);
guint playlist_add_url(Playlist *playlist, const gchar * url);
guint playlist_ins_url(Playlist *playlist, const gchar * string, gint pos);

void playlist_set_info(Playlist *playlist, const gchar * title, gint length, gint rate,
                       gint freq, gint nch);
void playlist_set_info_old_abi(const gchar * title, gint length, gint rate,
                               gint freq, gint nch);
void playlist_check_pos_current(Playlist *playlist);
void playlist_next(Playlist *playlist);
void playlist_prev(Playlist *playlist);
void playlist_queue(Playlist *playlist);
void playlist_queue_position(Playlist *playlist, guint pos);
void playlist_queue_remove(Playlist *playlist, guint pos);
gint playlist_queue_get_length(Playlist *playlist);
gboolean playlist_is_position_queued(Playlist *playlist, guint pos);
void playlist_clear_queue(Playlist *playlist);
gint playlist_get_queue_position(Playlist *playlist, PlaylistEntry * entry);
gint playlist_get_queue_position_number(Playlist *playlist, guint pos);
gint playlist_get_queue_qposition_number(Playlist *playlist, guint pos);
void playlist_eof_reached(Playlist *playlist);
void playlist_set_position(Playlist *playlist, guint pos);
gint playlist_get_length(Playlist *playlist);
gint playlist_get_length_nolock(Playlist *playlist);
gint playlist_get_position(Playlist *playlist);
gint playlist_get_position_nolock(Playlist *playlist);
gchar *playlist_get_info_text(Playlist *playlist);
gint playlist_get_current_length(Playlist *playlist);

gboolean playlist_save(Playlist *playlist, const gchar * filename);
gboolean playlist_load(Playlist *playlist, const gchar * filename);

void playlist_start_get_info_thread(void);
void playlist_stop_get_info_thread();
void playlist_start_get_info_scan(void);

void playlist_sort(Playlist *playlist, PlaylistSortType type);
void playlist_sort_selected(Playlist *playlist, PlaylistSortType type);

void playlist_reverse(Playlist *playlist);
void playlist_random(Playlist *playlist);
void playlist_remove_duplicates(Playlist *playlist, PlaylistDupsType);
void playlist_remove_dead_files(Playlist *playlist);

void playlist_fileinfo_current(Playlist *playlist);
void playlist_fileinfo(Playlist *playlist, guint pos);

void playlist_delete_index(Playlist *playlist, guint pos);
void playlist_delete_filenames(Playlist *playlist, GList * filenames);

PlaylistEntry *playlist_get_entry_to_play(Playlist *playlist);

/* XXX this is for reverse compatibility --nenolod */
const gchar *playlist_get_filename_to_play(Playlist *playlist);

gchar *playlist_get_filename(Playlist *playlist, guint pos);
gchar *playlist_get_songtitle(Playlist *playlist, guint pos);
TitleInput *playlist_get_tuple(Playlist *playlist, guint pos);
gint playlist_get_songtime(Playlist *playlist, guint pos);

GList *playlist_get_selected(Playlist *playlist);
GList *playlist_get_selected_list(Playlist *playlist);
int playlist_get_num_selected(Playlist *playlist);

void playlist_get_total_time(Playlist *playlist, gulong * total_time, gulong * selection_time,
                             gboolean * total_more,
                             gboolean * selection_more);

gint playlist_select_search(Playlist *playlist, TitleInput *tuple, gint action);
void playlist_select_all(Playlist *playlist, gboolean set);
void playlist_select_range(Playlist *playlist, gint min, gint max, gboolean sel);
void playlist_select_invert_all(Playlist *playlist);
gboolean playlist_select_invert(Playlist *playlist, guint pos);

gboolean playlist_read_info_selection(Playlist *playlist);
void playlist_read_info(Playlist *playlist, guint pos);

void playlist_set_shuffle(gboolean shuffle);

void playlist_clear_selected(Playlist *playlist);

GList *get_playlist_nth(Playlist *playlist, guint);
gboolean playlist_set_current_name(Playlist *playlist, const gchar * filename);
const gchar *playlist_get_current_name(Playlist *playlist);
Playlist *playlist_new(void);
void playlist_free(Playlist *playlist);
Playlist *playlist_new_from_selected(void);

PlaylistFormat playlist_format_get_from_name(const gchar * filename);
gboolean is_playlist_name(const gchar * filename);

#define PLAYLIST_LOCK(m)    g_mutex_lock(m)
#define PLAYLIST_UNLOCK(m)  g_mutex_unlock(m)

G_LOCK_EXTERN(playlists);

extern void playlist_load_ins_file(Playlist *playlist, const gchar * filename,
                                   const gchar * playlist_name, gint pos,
                                   const gchar * title, gint len);

extern void playlist_load_ins_file_tuple(Playlist *playlist, const gchar * filename_p,
					 const gchar * playlist_name, gint pos,
					 TitleInput *tuple);

Playlist *playlist_get_active(void);

G_END_DECLS

#endif