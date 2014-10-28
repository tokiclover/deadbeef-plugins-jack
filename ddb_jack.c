/*
 * JACK output plugin for DeaDBeeF
 * Copyright (C) 2010 Steven McDonald <steven.mcdonald@libremail.me>
 * CopyLeft  (c) 2014 -tclover <tokiclover@gmail.com>
 * License: MIT (see COPYING file).
 */

#define _GNU_SOURCE
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#define DB_CLIENT_NAME "deadbeef"
#define DB_PLUG_NAME "ddb_jack"

#include <errno.h>
#include <unistd.h>
#include <jack/jack.h>
#include <deadbeef/deadbeef.h>
#include <signal.h>
#include <limits.h>

//#define trace(...) { fprintf(stderr, __VA_ARGS__); }
#define trace(fmt,...)

typedef struct {
    jack_client_t *client;
    jack_status_t status;
    const char *name;
    char active;
    char autorestart;
    char autostart;
    char autoconnect;
    char connect;
    ddb_waveformat_t *fmt;
    unsigned short state;
    jack_port_t   *ports[];
} ddb_client_t;

static DB_output_t plugin;
static DB_functions_t *ddb_api;
static ddb_client_t ddb_client = {
    .name        = DB_CLIENT_NAME,
    .active      = 0,
    .autostart   = 1,
    .autoconnect = 1,
    .autorestart = 1,
    .connect     = 0,
    .fmt         = &plugin.fmt,
};

static int ddb_playback_stop (void);
static int ddb_jack_init (void);

static int jack_proc_callback (jack_nframes_t nframes, void *arg)
{
    trace (__func__);
    ddb_client_t *dbc = arg;

    if (!dbc->connect) return EPERM;

    // FIXME: This function copies from the streamer to a local buffer,
    //        and then to JACK's buffer. This is wasteful.

    //            Update 2011-01-01:
    //        The streamer can now use floating point samples, but there
    //        is still no easy solution to this because the streamer
    //        outputs both channels multiplexed, whereas JACK expects
    //        each channel to be written to a separate buffer.

    switch (dbc->state) {
        case OUTPUT_STATE_PLAYING: {
            char buf[nframes * dbc->fmt->channels * (dbc->fmt->bps / CHAR_BIT)];
            unsigned bytesread = ddb_api->streamer_read (buf, sizeof(buf));

        // this avoids a crash if we are playing and change to a plugin
        // with no valid output and then switch back
            if (bytesread == -1) {
                dbc->state = OUTPUT_STATE_STOPPED;
                return 0;
            }

            // this is intended to make playback less jittery in case of
            // inadequate read from streamer
/*            while (bytesread < sizeof(buf)) {
                //usleep (100);
                unsigned morebytesread = ddb_api->streamer_read (buf+bytesread, sizeof(buf)-bytesread);
                if (morebytesread != -1) bytesread += morebytesread;
            } */

            jack_nframes_t framesread = bytesread * CHAR_BIT / (dbc->fmt->channels * dbc->fmt->bps);
            float *jack_port_buffer[dbc->fmt->channels];
            for (unsigned short i = 0; i < dbc->fmt->channels; i++) {
                jack_port_buffer[i] = jack_port_get_buffer(dbc->ports[i], framesread);//nframes);
            }

            float vol = ddb_api->volume_get_amp ();

            for (unsigned i = 0; i < framesread; i++) {
                for (unsigned short j = 0; j < dbc->fmt->channels; j++) {
                    // JACK expects floating point samples, so we need to convert from integer
                    *jack_port_buffer[j]++ = ((float*)buf)[(dbc->fmt->channels*i) + j] * vol; // / 32768;
                }
            }

            return 0;
        }

        // this is necessary to stop JACK going berserk when we pause/stop
        default: {
            float *jack_port_buffer[dbc->fmt->channels];
            for (unsigned short i = 0; i < dbc->fmt->channels; i++) {
                jack_port_buffer[i] = jack_port_get_buffer(dbc->ports[i], nframes);
            }

            for (unsigned i = 0; i < nframes; i++) {
                for (unsigned short j = 0; j < dbc->fmt->channels; j++) {
                    *jack_port_buffer[j]++ = 0;
                }
            }

            return 0;
        }
    }
}

static int jack_rate_callback (void *arg)
{
    trace (__func__);
    ddb_client_t *dbc = &ddb_client;

    if (!dbc->connect)
        return EPERM;
    dbc->fmt->samplerate = (int)jack_get_sample_rate(dbc->client);

    return 0;
}

static int jack_shutdown_callback (void *arg)
{
    trace (__func__);
    ddb_client_t *dbc = arg;

    if (!dbc->connect)
        return EPERM;
    dbc->connect = 0;

    // if JACK crashes or is shut down, start a new server instance
    if (dbc->autorestart) {
        fprintf (stderr, "%s: JACK server shut down unexpectedly, restarting...\n", DB_PLUG_NAME);
        sleep (1);
        ddb_jack_init ();
    }
    else {
        fprintf (stderr, "%s: JACK server shut down unexpectedly, stopping playback\n", DB_PLUG_NAME);
        ddb_api->streamer_reset (1);
    }

    return 0;
}

static int ddb_jack_init (void)
{
    trace (__func__);
    ddb_client_t *dbc = &ddb_client;

    dbc->connect      = 1;
    dbc->autorestart  = (char)ddb_api->conf_get_int("ddb_jack.autorestart", 1);
    dbc->autostart    = (char)ddb_api->conf_get_int("ddb_jack.autostart"  , 1);
    dbc->autoconnect  = (char)ddb_api->conf_get_int("ddb_jack.autoconnect", 1);

    // create new client on JACK server
    jack_options_t options = JackNullOption|(JackNoStartServer && !dbc->autostart);
    dbc->client = jack_client_open (dbc->name, options, &dbc->status);
    if (dbc->status & JackInitFailure) {
        fprintf (stderr, "%s: Could not connect to JACK server\n", DB_PLUG_NAME);
        plugin.free();
        return EPERM;
    }

    dbc->fmt->samplerate = (int)jack_get_sample_rate(dbc->client);

    // Did we start JACK, or was it already running?
    if (dbc->status & JackServerStarted)
        dbc->autostart = 1;
    else
        dbc->autostart = 0;

    // set process callback
    if (jack_set_process_callback(dbc->client, &jack_proc_callback, dbc)) {
        fprintf (stderr, "%s: Could not set process callback\n", DB_PLUG_NAME);
        plugin.free();
        return ESRCH;
    }

    // set sample rate callback 
    if (jack_set_sample_rate_callback(dbc->client, (JackSampleRateCallback)&jack_rate_callback, NULL)) {
        fprintf (stderr, "%s: Could not set sample rate callback\n", DB_PLUG_NAME);
        plugin.free();
        return ESRCH;
    }

    // set shutdown callback
    jack_on_shutdown (dbc->client, (JackShutdownCallback)&jack_shutdown_callback, dbc);

    // register ports
    for (unsigned short i=0; i < dbc->fmt->channels; i++) {
        char port_name[16];

        // i+1 used to adhere to JACK convention of counting ports from 1, not 0
        sprintf (port_name, "ddb_playback_%d", i+1);
        jack_options_t options = JackPortIsOutput|JackPortIsTerminal;
        dbc->ports[i] = jack_port_register(dbc->client, (const char*)&port_name,
                JACK_DEFAULT_AUDIO_TYPE, options, 0);
        if (!dbc->ports[i]) {
            fprintf (stderr, "%s: Could not register port number %d\n", DB_PLUG_NAME, i+1);
            plugin.free();
            return ENXIO;
        }
    }

    // tell JACK we are ready to roll
    if (jack_activate(dbc->client)) {
        fprintf (stderr, "%s: Could not activate client\n", DB_PLUG_NAME);
        plugin.free();
        return EACCES;
    }

    // connect ports to hardware output
    if (dbc->autoconnect) {
        const char **playback_ports;
        jack_options_t options = JackPortIsPhysical|JackPortIsInput;

        if (!(playback_ports = jack_get_ports (dbc->client, 0, 0, options))) {
            fprintf (stderr, "%s: Could not find any playback ports to connect to\n", DB_PLUG_NAME);
            return ENXIO;
        }
        else {
            int ret;
            for (unsigned short i=0; i < dbc->fmt->channels; i++) {
                ret = jack_connect(dbc->client, jack_port_name (dbc->ports[i]), playback_ports[i]); 
                if (ret != 0 && ret != EEXIST) {
                    fprintf (stderr, "%s: Could not create connection from %s to %s\n",
                            DB_PLUG_NAME, jack_port_name (dbc->ports[i]), playback_ports[i]);
                    plugin.free();
                    return EACCES;
                }
            }
        }
    }

    return 0;
}

static int ddb_jack_setformat(ddb_waveformat_t *fmt)
{
    trace (__func__);

    /* Support only changing channels numbers */
    if (plugin.fmt.channels == fmt->channels)
        return 0;
    else
        plugin.fmt.channels = fmt->channels;

    if (ddb_client.active) {
        if (ddb_playback_stop())
            return EPERM;
        if (jack_client_close(ddb_client.client))
            return ESRCH;
        ddb_client.active = 0;
    }
    if(!ddb_jack_init())
        return ENOEXEC;

    return 0;
}

static int ddb_playback_play (void)
{
    trace (__func__);

    if (!ddb_client.connect) {
        if (ddb_jack_init()) {
            plugin.free();
            return EPERM;
        }
    }
    ddb_client.state = OUTPUT_STATE_PLAYING;

    return 0;
}

static int ddb_playback_stop (void)
{
    trace (__func__);

    ddb_client.state = OUTPUT_STATE_STOPPED;
    ddb_api->streamer_reset (1);

    return 0;
}

static int ddb_playback_pause (void)
{
    trace (__func__);

    if (ddb_client.state == OUTPUT_STATE_STOPPED)
        return 0;
    // set pause state
    ddb_client.state = OUTPUT_STATE_PAUSED;

    return 0;
}

static int ddb_jack_start (void)
{
    trace (__func__);
    sigset_t set;
    sigemptyset (&set);
    sigaddset (&set, SIGPIPE);
    sigprocmask (SIG_BLOCK, &set, 0);
    return 0;
}

static int ddb_jack_stop (void)
{
    trace (__func__);
    return 0;
}

static int ddb_playback_unpause (void)
{
    trace (__func__);
    ddb_playback_play ();
    return 0;
}

static int ddb_playback_state (void)
{
    trace (__func__);
    return ddb_client.state;
}

static int ddb_jack_free (void)
{
    trace (__func__);
    ddb_client.connect = 0;

    // stop playback if we didn't start jack
    // this prevents problems with not disconnecting gracefully
    if (!ddb_client.autostart) {
        ddb_playback_stop ();
        sleep (1);
    }

    if (ddb_client.client) {
        if (jack_client_close (ddb_client.client)) {
            fprintf (stderr, "%s: Could not disconnect from JACK server\n", DB_PLUG_NAME);
            return EPERM;
        }
        ddb_client.client = NULL;
    }

    // sleeping here is necessary to give JACK time to disconnect from the backend
    // if we are switching to another backend, it will fail without this
    if (ddb_client.autostart)
        sleep (1);

    return 0;
}

DB_plugin_t * ddb_jack_load (DB_functions_t *api)
{
    ddb_api = api;
    return DB_PLUGIN (&plugin);
}

static const char settings_dlg[] =
    "property \"Start JACK server automatically, if not already running\" checkbox jack.autostart 1;\n"
    "property \"Automatically connect to system playback ports\" checkbox jack.autoconnect 1;\n"
    "property \"Automatically restart JACK server if shut down\" checkbox jack.autorestart 0;\n"
;

// define plugin interface
static DB_output_t plugin = {
    DB_PLUGIN_SET_API_VERSION
    .plugin.version_major = 0,
    .plugin.version_minor = 3,
    .plugin.type = DB_PLUGIN_OUTPUT,
    .plugin.id = DB_PLUG_NAME,
    .plugin.name = "JACK output plugin",
    .plugin.descr = "plays sound via JACK API",
    .plugin.copyright = "CopyLeft (C) 2014 -tclover <tokiclover@gmail.com>",
    .plugin.website = "https://github.com/tokiclover/deadbeef-plugins-jack",
    .plugin.start = ddb_jack_start,
    .plugin.stop = ddb_jack_stop,
    .plugin.configdialog = settings_dlg,
    .init = ddb_jack_init,
    .free = ddb_jack_free,
    .setformat = ddb_jack_setformat,
    .play = ddb_playback_play,
    .stop = ddb_playback_stop,
    .pause = ddb_playback_pause,
    .unpause = ddb_playback_unpause,
    .state = ddb_playback_state,
    .fmt = {
        .bps = 32,
        .is_float = 1,
        .channels = 2,
        .channelmask = DDB_SPEAKER_FRONT_LEFT | DDB_SPEAKER_FRONT_RIGHT,
        .is_bigendian = 0,
    },
    .has_volume = 1,
};

/*
 * vim:fenc=utf-8:expandtab:sts=4:sw=4:ts=4:
 */
