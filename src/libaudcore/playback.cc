/*
 * playback.c
 * Copyright 2009-2013 John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include "drct.h"
#include "internal.h"
#include "plugins-internal.h"

#include <assert.h>
#include <pthread.h>

#include "audstrings.h"
#include "hook.h"
#include "i18n.h"
#include "interface.h"
#include "mainloop.h"
#include "output.h"
#include "playlist-internal.h"
#include "plugin.h"
#include "plugins.h"
#include "runtime.h"

static pthread_t playback_thread_handle;
static QueuedFunc end_queue;

static pthread_mutex_t ready_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_cond = PTHREAD_COND_INITIALIZER;

static pthread_mutex_t control_mutex = PTHREAD_MUTEX_INITIALIZER;

/* level 1 data (persists to end of song) */
static bool playing = false;
static int time_offset = 0;
static int stop_time = -1;
static bool paused = false;
static bool ready_flag = false;
static bool playback_error = false;
static bool song_finished = false;

static int seek_request = -1; /* under control_mutex */
static int repeat_a = -1; /* under control_mutex */
static int repeat_b = -1; /* under control_mutex */
static bool stop_flag = false; /* under control_mutex */

static int current_bitrate = -1, current_samplerate = -1, current_channels = -1;

static int current_entry = -1;
static String current_filename;
static Tuple current_tuple;
static String current_title;
static int current_length = -1;

static InputPlugin * current_decoder = nullptr;
static VFSFile current_file;
static ReplayGainInfo current_gain;

/* level 2 data (persists to end of playlist) */
static bool stopped = true;
static int failed_entries = 0;

/* clears gain info if tuple == nullptr */
static void read_gain_from_tuple (const Tuple & tuple)
{
    current_gain = ReplayGainInfo ();

    if (! tuple)
        return;

    int album_gain = tuple.get_int (Tuple::AlbumGain);
    int album_peak = tuple.get_int (Tuple::AlbumPeak);
    int track_gain = tuple.get_int (Tuple::TrackGain);
    int track_peak = tuple.get_int (Tuple::TrackPeak);
    int gain_unit = tuple.get_int (Tuple::GainDivisor);
    int peak_unit = tuple.get_int (Tuple::PeakDivisor);

    if (gain_unit)
    {
        current_gain.album_gain = album_gain / (float) gain_unit;
        current_gain.track_gain = track_gain / (float) gain_unit;
    }

    if (peak_unit)
    {
        current_gain.album_peak = album_peak / (float) peak_unit;
        current_gain.track_peak = track_peak / (float) peak_unit;
    }
}

static void update_from_playlist ()
{
    // if we already read good data, do not overwrite it with a guess
    Tuple tuple = playback_entry_get_tuple (current_tuple ? Playlist::Nothing : Playlist::Guess);

    if (tuple && tuple != current_tuple)
    {
        current_tuple = std::move (tuple);
        if (ready_flag)
            event_queue ("tuple change", nullptr);
    }

    int entry = playback_entry_get_position ();
    String title = current_tuple.get_str (Tuple::FormattedTitle);
    int length = current_tuple.get_int (Tuple::Length);

    if (entry != current_entry || title != current_title || length != current_length)
    {
        current_entry = entry;
        current_title = title;
        current_length = length;
        if (ready_flag)
            event_queue ("title change", nullptr);
    }
}

// TODO for 3.7: make this thread-safe
EXPORT bool aud_drct_get_ready ()
{
    if (! playing)
        return false;

    pthread_mutex_lock (& ready_mutex);
    bool ready = ready_flag;
    pthread_mutex_unlock (& ready_mutex);
    return ready;
}

static void set_ready ()
{
    assert (playing);

    pthread_mutex_lock (& ready_mutex);

    update_from_playlist ();
    event_queue ("playback ready", nullptr);
    ready_flag = true;

    pthread_cond_signal (& ready_cond);
    pthread_mutex_unlock (& ready_mutex);
}

static void wait_until_ready ()
{
    assert (playing);
    pthread_mutex_lock (& ready_mutex);

    /* on restart, we still have to wait, but presumably not long */
    while (! ready_flag)
        pthread_cond_wait (& ready_cond, & ready_mutex);

    pthread_mutex_unlock (& ready_mutex);
}

static void update_cb (void * hook_data, void *)
{
    assert (playing);

    auto level = aud::from_ptr<Playlist::Update> (hook_data);
    if (level >= Playlist::Metadata && aud_drct_get_ready ())
        update_from_playlist ();
}

// TODO for 3.7: make this thread-safe
EXPORT int aud_drct_get_time ()
{
    if (! playing)
        return 0;

    wait_until_ready ();

    return output_get_time () - time_offset;
}

EXPORT void aud_drct_pause ()
{
    if (! playing)
        return;

    wait_until_ready ();

    paused = ! paused;

    output_pause (paused);

    if (paused)
        hook_call ("playback pause", nullptr);
    else
        hook_call ("playback unpause", nullptr);
}

static void playback_cleanup ()
{
    assert (playing);
    wait_until_ready ();

    if (! song_finished)
    {
        pthread_mutex_lock (& control_mutex);
        stop_flag = true;
        output_flush (0);
        pthread_mutex_unlock (& control_mutex);
    }

    pthread_join (playback_thread_handle, nullptr);
    output_close_audio ();

    hook_dissociate ("playlist update", update_cb);

    event_queue_cancel ("playback ready", nullptr);
    event_queue_cancel ("playback seek", nullptr);
    event_queue_cancel ("info change", nullptr);
    event_queue_cancel ("title change", nullptr);
    event_queue_cancel ("tuple change", nullptr);

    end_queue.stop ();

    /* level 1 data cleanup */
    playing = false;
    time_offset = 0;
    stop_time = -1;
    paused = false;
    ready_flag = false;
    playback_error = false;
    song_finished = false;

    seek_request = -1;
    repeat_a = -1;
    repeat_b = -1;
    stop_flag = false;

    current_bitrate = current_samplerate = current_channels = -1;

    current_entry = -1;
    current_filename = String ();
    current_tuple = Tuple ();
    current_title = String ();
    current_length = -1;

    current_decoder = nullptr;
    current_file = VFSFile ();

    read_gain_from_tuple (Tuple ());

    aud_set_bool (nullptr, "stop_after_current_song", false);
}

void playback_stop ()
{
    if (stopped)
        return;

    if (playing)
        playback_cleanup ();

    output_drain ();

    /* level 2 data cleanup */
    stopped = true;
    failed_entries = 0;

    hook_call ("playback stop", nullptr);
}

static void do_stop (int playlist)
{
    aud_playlist_play (-1);
    aud_playlist_set_position (playlist, aud_playlist_get_position (playlist));
}

static void do_next (int playlist)
{
    if (! playlist_next_song (playlist, aud_get_bool (nullptr, "repeat")))
    {
        aud_playlist_set_position (playlist, -1);
        hook_call ("playlist end reached", nullptr);
    }
}

static void end_cb (void * unused)
{
    if (! playing)
        return;

    if (! playback_error)
        song_finished = true;

    hook_call ("playback end", nullptr);

    if (playback_error)
        failed_entries ++;
    else
        failed_entries = 0;

    int playlist = aud_playlist_get_playing ();

    if (aud_get_bool (nullptr, "stop_after_current_song"))
    {
        do_stop (playlist);

        if (! aud_get_bool (nullptr, "no_playlist_advance"))
            do_next (playlist);
    }
    else if (aud_get_bool (nullptr, "no_playlist_advance"))
    {
        if (aud_get_bool (nullptr, "repeat") && ! failed_entries)
            playback_play (0, false);
        else
            do_stop (playlist);
    }
    else
    {
        if (failed_entries < 10)
            do_next (playlist);
        else
            do_stop (playlist);
    }
}

static bool open_file (String & error)
{
    /* no need to open a handle for custom URI schemes */
    if (current_decoder->input_info.keys[INPUT_KEY_SCHEME])
        return true;

    current_file = VFSFile (current_filename, "r");
    if (! current_file)
        error = String (current_file.error ());

    return (bool) current_file;
}

static void * playback_thread (void *)
{
    String error;
    Tuple tuple;
    int length;

    if (! current_decoder)
    {
        PluginHandle * p = playback_entry_get_decoder (& error);
        if (p && ! (current_decoder = (InputPlugin *) aud_plugin_get_header (p)))
            error = String (_("Error loading plugin"));

        if (! current_decoder)
        {
            playback_error = true;
            goto DONE;
        }
    }

    if (! (tuple = playback_entry_get_tuple (Playlist::Wait, & error)))
    {
        playback_error = true;
        goto DONE;
    }

    length = tuple.get_int (Tuple::Length);

    if (length < 1)
        seek_request = -1;

    if (tuple && length > 0)
    {
        if (tuple.get_value_type (Tuple::StartTime) == Tuple::Int)
        {
            time_offset = tuple.get_int (Tuple::StartTime);
            if (time_offset)
                seek_request = time_offset + aud::max (seek_request, 0);
        }

        if (tuple.get_value_type (Tuple::EndTime) == Tuple::Int)
            stop_time = tuple.get_int (Tuple::EndTime);
    }

    read_gain_from_tuple (tuple);

    if (! open_file (error))
    {
        playback_error = true;
        goto DONE;
    }

    current_tuple = std::move (tuple);

    if (! current_decoder->play (current_filename, current_file))
        playback_error = true;

DONE:
    if (playback_error)
    {
        if (! error)
            error = String (_("Unknown playback error"));

        aud_ui_show_error (str_printf (_("Error opening %s:\n%s"),
         (const char *) current_filename, (const char *) error));
    }

    if (! ready_flag)
        set_ready ();

    end_queue.queue (end_cb, nullptr);
    return nullptr;
}

void playback_play (int seek_time, bool pause)
{
    String new_filename = playback_entry_get_filename ();

    if (! new_filename)
    {
        AUDERR ("Nothing to play!");
        return;
    }

    if (playing)
        playback_cleanup ();

    current_filename = new_filename;

    playing = true;
    paused = pause;

    seek_request = (seek_time > 0) ? seek_time : -1;

    stopped = false;

    hook_associate ("playlist update", update_cb, nullptr);
    pthread_create (& playback_thread_handle, nullptr, playback_thread, nullptr);

    hook_call ("playback begin", nullptr);
}

// TODO for 3.7: make this thread-safe
EXPORT bool aud_drct_get_playing ()
{
    return playing;
}

// TODO for 3.7: make this thread-safe
EXPORT bool aud_drct_get_paused ()
{
    return paused;
}

static void request_seek_locked (int time)
{
    seek_request = time;
    output_flush (time);
    event_queue ("playback seek", nullptr);
}

EXPORT void aud_drct_seek (int time)
{
    if (! playing)
        return;

    wait_until_ready ();

    if (current_length < 1)
        return;

    pthread_mutex_lock (& control_mutex);
    request_seek_locked (time_offset + aud::clamp (time, 0, current_length));
    pthread_mutex_unlock (& control_mutex);
}

EXPORT void InputPlugin::open_audio (int format, int rate, int channels)
{
    assert (playing);

    // some output plugins (e.g. filewriter) call aud_drct_get_tuple();
    // hence, to avoid a deadlock, set_ready() must precede output_open_audio()
    if (! ready_flag)
        set_ready ();

    if (! output_open_audio (format, rate, channels, aud::max (0, seek_request)))
    {
        AUDERR ("Invalid audio format: %d, %d Hz, %d channels\n", format, rate, channels);
        playback_error = true;
        pthread_mutex_lock (& control_mutex);
        stop_flag = true;
        pthread_mutex_unlock (& control_mutex);
        return;
    }

    output_set_replay_gain (current_gain);

    if (paused)
        output_pause (true);

    current_samplerate = rate;
    current_channels = channels;

    if (ready_flag)
        event_queue ("info change", nullptr);
}

EXPORT void InputPlugin::set_replay_gain (const ReplayGainInfo & info)
{
    assert (playing);
    current_gain = info;
    output_set_replay_gain (current_gain);
}

EXPORT void InputPlugin::write_audio (const void * data, int length)
{
    assert (playing);

    pthread_mutex_lock (& control_mutex);
    int a = repeat_a, b = repeat_b;
    pthread_mutex_unlock (& control_mutex);

    if (! output_write_audio (data, length, b >= 0 ? b : stop_time))
    {
        pthread_mutex_lock (& control_mutex);

        if (seek_request < 0 && ! stop_flag)
        {
            // When output_write_audio() returns false on its own, without a
            // seek or stop request, we have reached the requested stop time.
            if (b >= 0)
                request_seek_locked (aud::max (a, time_offset));
            else
                stop_flag = true;
        }

        pthread_mutex_unlock (& control_mutex);
    }
}

EXPORT Tuple InputPlugin::get_playback_tuple ()
{
    assert (playing);

    Tuple tuple = current_tuple.ref ();
    tuple.delete_fallbacks ();
    return tuple;
}

EXPORT void InputPlugin::set_playback_tuple (Tuple && tuple)
{
    assert (playing);
    playback_entry_set_tuple (std::move (tuple));
}

EXPORT void InputPlugin::set_stream_bitrate (int bitrate)
{
    assert (playing);
    current_bitrate = bitrate;

    if (ready_flag)
        event_queue ("info change", nullptr);
}

EXPORT bool InputPlugin::check_stop ()
{
    assert (playing);
    pthread_mutex_lock (& control_mutex);
    bool stopped = stop_flag;
    pthread_mutex_unlock (& control_mutex);
    return stopped;
}

EXPORT int InputPlugin::check_seek ()
{
    assert (playing);

    pthread_mutex_lock (& control_mutex);
    int seek = seek_request;

    if (seek != -1)
    {
        output_resume ();
        seek_request = -1;
    }

    pthread_mutex_unlock (& control_mutex);
    return seek;
}

// TODO for 3.7: make this thread-safe
EXPORT int aud_drct_get_position ()
{
    return playback_entry_get_position ();
}

// TODO for 3.7: make this thread-safe
EXPORT String aud_drct_get_filename ()
{
    return current_filename;
}

// TODO for 3.7: make this thread-safe
EXPORT String aud_drct_get_title ()
{
    if (! playing)
        return String ();

    wait_until_ready ();

    StringBuf prefix = aud_get_bool (nullptr, "show_numbers_in_pl") ?
     str_printf ("%d. ", 1 + current_entry) : StringBuf (0);

    StringBuf time = (current_length > 0) ? str_format_time (current_length) : StringBuf ();
    StringBuf suffix = time ? str_concat ({" (", time, ")"}) : StringBuf (0);

    return String (str_concat ({prefix, current_title, suffix}));
}

// TODO for 3.7: make this thread-safe
EXPORT Tuple aud_drct_get_tuple ()
{
    if (playing)
        wait_until_ready ();

    return current_tuple.ref ();
}

// TODO for 3.7: make this thread-safe
EXPORT int aud_drct_get_length ()
{
    if (playing)
        wait_until_ready ();

    return current_length;
}

// TODO for 3.7: make this thread-safe
EXPORT void aud_drct_get_info (int * bitrate, int * samplerate, int * channels)
{
    if (playing)
        wait_until_ready ();

    * bitrate = current_bitrate;
    * samplerate = current_samplerate;
    * channels = current_channels;
}

EXPORT void aud_drct_set_ab_repeat (int a, int b)
{
    if (! playing)
        return;

    wait_until_ready ();

    if (current_length < 1)
        return;

    if (a >= 0)
        a += time_offset;
    if (b >= 0)
        b += time_offset;

    pthread_mutex_lock (& control_mutex);

    repeat_a = a;
    repeat_b = b;

    if (b != -1 && output_get_time () >= b)
        request_seek_locked (aud::max (a, time_offset));

    pthread_mutex_unlock (& control_mutex);
}

// TODO for 3.7: make this thread-safe
EXPORT void aud_drct_get_ab_repeat (int * a, int * b)
{
    * a = (playing && repeat_a != -1) ? repeat_a - time_offset : -1;
    * b = (playing && repeat_b != -1) ? repeat_b - time_offset : -1;
}
