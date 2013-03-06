#include <gst/gst.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Uncomment to compile under GStreamer-0.10 instead of GStreamer-1.0
#define GSTREAMER_010

// Global vars for cmd line args
//
static int waitSecs = 0;
static int state_change_timeout_secs = 45;
static gfloat requested_rate = 0;
static int rrid = 2;
static char host[256];
static char uri[256];
static gboolean usePlaybin = TRUE;
static gboolean use_dtcp = FALSE;

static gboolean test_positioning = FALSE;
static gboolean test_uri_switch = FALSE;
static gboolean test_rate_change = FALSE;
static gboolean test_seeking = FALSE;

static gboolean use_file = FALSE;
static char file_name[256];
// *TODO* - change this to env var
//static gchar* file_path = "file:///home/landerson/RUIHRI/git-dlnaplugin/gst-plugin-la/";
static gchar* file_path = NULL;
static gchar* TEST_FILE_URL_PREFIX_ENV = "TEST_FILE_URL_PREFIX";


/* Structure to contain all our information, so we can pass it around */
typedef struct _CustomData {
	GstElement *pipeline;  /* Our pipeline element */
	gboolean playing;      /* Are we in the PLAYING state? */
	gboolean terminate;    /* Should we terminate execution? */
	gboolean seek_enabled; /* Is seeking enabled for this media? */
	gboolean seek_done;    /* Have we performed the seek already? */
	gint64 duration;       /* How long does this media last, in nanoseconds */
	gdouble rate;		   /* current playspeed */
} CustomData;

// Local method declarations
//
static gboolean process_cmd_line_args(int argc, char*argv[]);
static GstElement* create_playbin_pipeline();
static GstElement* create_manual_playbin_pipeline();
static GstElement* create_fancy_pipeline();
static void on_source_changed(GstElement* element, GParamSpec* param, gpointer data);
static void handle_message (CustomData *data, GstMessage *msg);

static void perform_positioning(CustomData* data);
static void perform_rate_change(CustomData* data);
static void perform_uri_switch(CustomData* data);
static void perform_seek(CustomData* data);

static void log_bin_elements(GstBin* bin);
static gboolean set_pipeline_state(CustomData* data, GstState desired_state, gint timeoutSecs);
static gboolean set_new_uri(CustomData* data);

/**
 * Test program for playspeed testing
 */
int main(int argc, char *argv[]) 
{
	GstBus *bus = NULL;

	CustomData data;
	data.playing = FALSE;
	data.terminate = FALSE;
	data.seek_enabled = FALSE;
	data.seek_done = FALSE;
	data.duration = GST_CLOCK_TIME_NONE;

	// Assign default values
	strcpy(host, "192.168.2.2");
	uri[0] = '\0';
	file_name[0] = '\0';
	if (!process_cmd_line_args(argc, argv))
	{
		g_printerr("Exit due to problems with cmd line args\n");
		return -1;
	}

	// Build default URI if one was not specified
	if (uri[0] == '\0')
	{
		char* line2 = "http://";
		char* line3 = ":8008/ocaphn/recording?rrid=";
		char* line4 = "&profile=MPEG_TS_SD_NA_ISO&mime=video/mpeg";
		if (use_dtcp)
		{
			line4 = "&profile=DTCP_MPEG_TS_SD_NA_ISO&mime=video/mpeg";
		}
		sprintf(uri, "%s%s%s%d%s", line2, host, line3, rrid, line4);
	}

	// Initialize GStreamer
	gst_init (&argc, &argv);

	// Build the pipeline
	if (usePlaybin)
	{
		g_print("Creating pipeline using playbin\n");
		data.pipeline = create_playbin_pipeline();
	}
	else
	{
		if (1)
		{
			g_print("Creating pipeline by assembling elements\n");
			data.pipeline = create_manual_playbin_pipeline();
		}
		else
		{
			g_print("Creating pipeline used by Fancy\n");
			data.pipeline = create_fancy_pipeline();
		}
	}

	// Check that pipeline was properly created
	if (data.pipeline == NULL)
	{
		g_printerr("Problems creating pipeline\n");
		return -1;
	}

	// Start playing
	g_print("Pipeline created, start playing\n");
	if (!set_pipeline_state(&data, GST_STATE_PLAYING, state_change_timeout_secs))
	{
		g_printerr ("Unable to set the pipeline to the playing state.\n");
		return -1;
	}
	else
	{
		g_print("Pipeline in playing state\n");
	}

	// Print out elements in playbin
	log_bin_elements(GST_BIN(data.pipeline));

	// Perform requested testing
	g_print("Begin pipeline test\n");
	if (test_positioning)
	{
		perform_positioning(&data);
	}
	else if (test_rate_change)
	{
		perform_rate_change(&data);
	}
	else if (test_uri_switch)
	{
		perform_uri_switch(&data);
	}
	else if (test_seeking)
	{
		perform_seek(&data);
	}
	else
	{
		g_print("Testing 1x playback\n");
		bus = gst_element_get_bus (data.pipeline);
		if (bus != NULL)
		{
			gst_bus_timed_pop_filtered (bus,
				GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
			gst_object_unref (bus);
		}
	}
	return 0;
}

/**
 * Test which queries current position, duration and rate (via new segment).
 * If queries are successful, initiates a rate change.
 */
static void perform_positioning(CustomData* data)
{
	GstBus *bus;
	GstMessage *msg;
	GstQuery* query;
	gint64 start;
	gint64 stop;
	GstFormat fmt;
	GstElement *video_sink = NULL;

	// Wait until error or EOS
	bus = gst_element_get_bus (data->pipeline);
	do {
		msg = gst_bus_timed_pop_filtered (bus, 100 * GST_MSECOND,
				GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_ERROR | GST_MESSAGE_EOS | GST_MESSAGE_DURATION);

		// Parse message
		if (msg != NULL)
		{
			handle_message(data, msg);
		}
		else
		{
			// We got no message, this means the timeout expired
			if (data->playing)
			{
				gint64 current = -1;

				// Query the current position of the stream
#ifdef GSTREAMER_010
				if (!gst_element_query_position (data->pipeline, &fmt, &current))
#else
				if (!gst_element_query_position (data->pipeline, GST_FORMAT_TIME, &current))
#endif
				{
					g_printerr ("Could not query current position.\n");
					return;
				}

				// If we didn't know it yet, query the stream duration
				if (!GST_CLOCK_TIME_IS_VALID (data->duration))
				{
					//g_print ("Current duration is invalid, query for duration\n");
#ifdef GSTREAMER_010
					if (!gst_element_query_duration (data->pipeline, &fmt, &data->duration))
#else
					if (!gst_element_query_duration (data->pipeline, GST_FORMAT_TIME, &data->duration))
#endif
					{
						g_printerr ("Could not query current duration.\n");
						return;
					}
					else
					{
						g_print ("Current duration is: %llu\n", data->duration);
					}
				}

				// Get the current playback rate
#ifdef GSTREAMER_010
				query = gst_query_new_segment(fmt);
#else
				query = gst_query_new_segment(GST_FORMAT_TIME);
#endif
				if (query != NULL)
				{
					gst_element_query(data->pipeline, query);
					gst_query_parse_segment(query, &data->rate, &fmt, &start, &stop);
					gst_query_unref(query);
					//g_print ("\nCurrent playspeed: %3.1f\n\n", data.rate);
				}
				else
				{
					g_printerr ("Could not query segment to get playback rate\n");
					return;
				}

				// Print current position and total duration
				g_print ("Position %" GST_TIME_FORMAT " / %" GST_TIME_FORMAT "\r",
						GST_TIME_ARGS (current), GST_TIME_ARGS (data->duration));

				// If seeking is enabled, we have not done it yet, and the time is right, seek
				if (data->seek_enabled && !data->seek_done && current > 10 * GST_SECOND)
				{
					g_print ("\nReached 10s, performing seek with requested rate %3.1f...\n", requested_rate);

					// If not changing rate, use seek simple
					if (requested_rate == 0.0)
					{
						// Seek simple will send rate of 0 which will be ignored
						if (!gst_element_seek_simple (data->pipeline, GST_FORMAT_TIME,
								GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 30 * GST_SECOND))
						{
							g_printerr ("Problems doing a simple seek with no rate change\n");
							return;
						}
						else
						{
							g_print("Completed seek with no rate change\n");
						}
					}
					else
					{
						if (1)
						{
							GstEvent* seek_event = gst_event_new_seek (requested_rate, 		// rate
																	   GST_FORMAT_TIME, 	// format
																	   GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, // flags
																	   GST_SEEK_TYPE_NONE, 	// start type?
																	   current,				// start
																	   GST_SEEK_TYPE_NONE, 	// stop type
																	   -1);					// stop

							if (seek_event != NULL)
							{
								if (1)
								{
									g_print("Created seek event for rate change, sending through video sink\n");
									g_object_get (data->pipeline, "video-sink", &video_sink, NULL);
									if (video_sink != NULL)
									{
										if (!gst_element_send_event(video_sink, seek_event))
										{
											g_printerr ("Problems sending seek event through video sink for rate change\n");
											return;
										}
										else
										{
											g_print("Sent seek event through video sink for rate change\n");
										}
									}
									else
									{
										g_printerr("Not ending seek event due to NULL video sink\n");
										return;
									}
								}
								else
								{
									if (!gst_element_send_event (data->pipeline, seek_event))
									{
										g_printerr ("Problems sending seek event through playbin for rate change\n");
										return;
									}
									else
									{
										g_print("Sent seek event through playbin for rate change\n");
									}
								}
							}
							else
							{
								g_printerr ("Unable to create seek event for rate change\n");
								return;
							}
						}
						else
						{
							if (!gst_element_seek(data->pipeline, requested_rate, GST_FORMAT_TIME,
									GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
									GST_SEEK_TYPE_END, 30 * GST_SECOND, GST_SEEK_TYPE_NONE, 0))
							{
								g_printerr ("Problems doing a seek with rate change to %3.1f\n",
										requested_rate);
								return;
							}
							else
							{
								g_print("Completed seek with rate change to %3.1f\n",
										requested_rate);
							}
						}
					}
					data->seek_done = TRUE;
				}
			}
		}
	} while (!data->terminate);
}

static void perform_rate_change(CustomData* data)
{
	GstEvent* event;
	GstBus *bus;
	GstMessage *msg;
	GstFormat format = GST_FORMAT_TIME;
	//GstFormat format = GST_FORMAT_BYTES;
	//GstFormat format = GST_FORMAT_DEFAULT;
	GstElement* video_sink = NULL;

	// Wait for 10 seconds for playback to start up
	long secs = waitSecs;
	g_print("Waiting %ld secs for startup prior to rate change\n", secs);
	g_usleep(secs * 1000000L);

	// If requested, send rate change
	if (requested_rate != 0)
	{
		g_print("Requesting rate change to %4.1f\n", requested_rate);

		gint64 position = -1;

		// Obtain the current position, needed for the seek event
		if (0)
		{
			g_print("Get video sink in order to send event\n");
			g_object_get (data->pipeline, "video-sink", &video_sink, NULL);
			if (video_sink != NULL)
			{
#ifdef GSTREAMER_010
				if (!gst_element_query_position(video_sink, &format, &position))
#else
				if (!gst_element_query_position(video_sink, format, &position))
#endif
				{
					g_printerr("Unable to retrieve current position.\n");
					return;
				}
			}
			else
			{
				g_printerr("Not ending seek event due to NULL video sink\n");
				return;
			}

			// Create the seek event
			g_print("Creating seek event\n");
			event = gst_event_new_seek(requested_rate, format,
					//GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, // see flush stop err
					//GST_SEEK_FLAG_NONE, // don't see error but playback stalls
					GST_SEEK_FLAG_FLUSH,
					GST_SEEK_TYPE_SET, position, GST_SEEK_TYPE_NONE, -1);
			if (event == NULL)
			{
				g_printerr("Unable to create SEEK event\n");
				return;
			}

			// Send the event
			g_print("Sending seek event to video sink\n");
			if (!gst_element_send_event(video_sink, event))
			{
				g_printerr("Unable to send seek event via video sink\n");
				return;
			}
			else
			{
				g_print("Seek event sent via video sink\n");
			}
		}
		else
		{
			g_print("Query position in format: %s\n", gst_format_get_name(format));
			format = GST_FORMAT_TIME;
#ifdef GSTREAMER_010
			if (!gst_element_query_position(data->pipeline, &format, &position))
#else
			if (!gst_element_query_position(data->pipeline, format, &position))
#endif
			{
				g_printerr("Unable to retrieve current position.\n");
				return;
			}
			g_print("Got current position in format %s: %llu\n",
					gst_format_get_name(format), position);

			if (0)
			{
				// Create the EOS event
				g_print("Creating EOS event\n");
				event = gst_event_new_eos();
				if (event == NULL)
				{
					g_printerr("Unable to create EOS event\n");
					return;
				}

				// Send the event
				g_print("Sending EOS to video sink\n");
				if (!gst_element_send_event(data->pipeline, event))
				{
					g_printerr("Unable to send EOS event via video sink\n");
					return;
				}
				else
				{
					g_print("EOS event sent via video sink\n");
				}
			}
			g_print("Seeking on pipeline element\n");
			if (!gst_element_seek (data->pipeline, requested_rate,
					GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
					//GST_SEEK_TYPE_SET, position,
					GST_SEEK_TYPE_SET, 0,
						GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE))
			{
				g_printerr("Problems changing rate.\n");
				return;
			}
			g_print("Seeking on pipeline element complete\n");
		}
	}
	else
	{
		g_print("Not requesting rate change\n");
	}

	// Initiate pause if rate was set to zero but wait time was not zero
	if ((requested_rate == 0) && (waitSecs != 0))
	{
		g_print("Pausing pipeline for %d secs due to rate=0 & wait!=0\n", waitSecs);
		set_pipeline_state (data, GST_STATE_PAUSED, state_change_timeout_secs);

		g_usleep(secs * 1000000L);

		g_print("Resuming pipeline after %d sec pause\n", waitSecs);
		set_pipeline_state (data, GST_STATE_PLAYING, state_change_timeout_secs);
	}
	gboolean done = FALSE;

	// Wait until error or EOS
	bus = gst_element_get_bus (data->pipeline);

	g_print("Waiting for EOS or ERROR\n");
	while (!done)
	{
		msg = gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
		if (msg != NULL)
		{
			g_print("Received msg type: %s\n", GST_MESSAGE_TYPE_NAME(msg));

			if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
			{
				GError *err = NULL;
				gchar *dbg_info = NULL;
				gst_message_parse_error (msg, &err, &dbg_info);
				g_print("ERROR from element %s: %s\n",
						GST_OBJECT_NAME (msg->src), err->message);
				g_print("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
				g_error_free(err);
				g_free(dbg_info);
			}
			gst_message_unref (msg);
			done = TRUE;
		}
	}
}

static void perform_uri_switch(CustomData* data)
{
	GstBus *bus;
	GstMessage *msg;

	// Wait for 10 seconds for playback to start up
	long secs = waitSecs;
	g_print("Waiting %ld secs for startup prior to switching uri\n", secs);
	g_usleep(secs * 1000000L);

	if (!set_new_uri(data))
	{
		g_print("Problems setting new URI\n");
		return;
	}

	// Wait for playback to complete
	g_print("Wait for playback to complete with new URI\n");
	gint eosCnt = 0;
	bus = gst_element_get_bus (data->pipeline);
	gboolean done = FALSE;
	while (!done)
	{
		msg = gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
		if (msg != NULL)
		{
			g_print("Received msg type: %s\n", GST_MESSAGE_TYPE_NAME(msg));

			if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
			{
				GError *err = NULL;
				gchar *dbg_info = NULL;
				gst_message_parse_error (msg, &err, &dbg_info);
				g_print("ERROR from element %s: %s\n",
						GST_OBJECT_NAME (msg->src), err->message);
				g_print("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
				g_error_free(err);
				g_free(dbg_info);
				done = TRUE;
			}
			else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
			{
				eosCnt++;
				if (eosCnt > 1)
				{
					g_print("Got EOS\n");
					done = TRUE;
				}
			}
			gst_message_unref (msg);
		}
	}
	msg = gst_bus_timed_pop_filtered (bus,
			GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
}

static void perform_seek(CustomData* data)
{
	GstEvent* event;
	GstBus *bus;
	GstMessage *msg;

	// Wait for 10 seconds for playback to start up
	long secs = waitSecs;
	g_print("Waiting %ld secs for startup prior to sending seek event\n", secs);
	g_usleep(secs * 1000000L);

	// Create the seek event
	g_print("Creating seek event\n");
	event = gst_event_new_seek(4.0, // new rate
			//GST_FORMAT_BYTES,
			GST_FORMAT_TIME, // souphttpsrc doesn't support time based seeks???
			GST_SEEK_FLAG_FLUSH, // flags - GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
			GST_SEEK_TYPE_SET,
			0,
			GST_SEEK_TYPE_NONE,
			-1);
	/*
	 * basesrc gstbasesrc.c:1747:gst_base_src_default_event:<source> handle event seek
	 *  event from 'sink' at time 99:99:99.999999999: GstEventSeek, rate=(double)4,
	 *  format=(GstFormat)GST_FORMAT_TIME, flags=(GstSeekFlags)GST_SEEK_FLAG_FLUSH,
	 *  cur-type=(GstSeekType)GST_SEEK_TYPE_SET, cur=(gint64)0,
	 *  stop-type=(GstSeekType)GST_SEEK_TYPE_NONE, stop=(gint64)-1;
	 *
	 * basesrc gstbasesrc.c:1785:gst_base_src_default_event:<source> is not seekable
	 *
	 * basesrc gstbasesrc.c:1819:gst_base_src_event_handler:<source> subclass refused event
	 */

	/*
	g_print("Setting new URI on playbin prior to sending seek event\n");
	if (!set_new_uri(data))
	{
		g_print("Problems setting new URI\n");
		return;
	}
	*/

	// Sending seek event
	g_print("Sending seek event\n");
	gst_element_send_event(data->pipeline, event);

	// Wait until error or EOS
	g_print("Sent seek event, getting bus\n");
	bus = gst_element_get_bus (data->pipeline);

	g_print("Waiting for EOS or ERROR\n");
	gboolean done = FALSE;
	gint eosCnt = 0;
	while (!done)
	{
		msg = gst_bus_timed_pop_filtered (bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
		if (msg != NULL)
		{
			g_print("Received msg type: %s\n", GST_MESSAGE_TYPE_NAME(msg));

			if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
			{
				GError *err = NULL;
				gchar *dbg_info = NULL;
				gst_message_parse_error (msg, &err, &dbg_info);
				g_print("ERROR from element %s: %s\n",
						GST_OBJECT_NAME (msg->src), err->message);
				g_print("Debugging info: %s\n", (dbg_info) ? dbg_info : "none");
				g_error_free(err);
				g_free(dbg_info);
				done = TRUE;
			}
			else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
			{
				eosCnt++;
				g_print("Got EOS %d\n", eosCnt);
				if (eosCnt > 1)
				{
					g_print("Got final EOS\n");
					done = TRUE;
				}
			}
			gst_message_unref (msg);
		}
	}
}

/**
 *
 */
static void handle_message (CustomData *data, GstMessage *msg)
{
	GError *err;


	gchar *debug_info;
	//g_print("Got message type: %s\n", GST_MESSAGE_TYPE_NAME (msg));

	switch (GST_MESSAGE_TYPE (msg))
	{
	case GST_MESSAGE_ERROR:
		gst_message_parse_error (msg, &err, &debug_info);
		g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
		g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
		g_clear_error (&err);
		g_free (debug_info);
		data->terminate = TRUE;
		break;

	case GST_MESSAGE_EOS:
		g_print ("End-Of-Stream reached.\n");
		data->terminate = TRUE;
		break;

	case GST_MESSAGE_DURATION:
		// The duration has changed, mark thdata.pipeline,e current one as invalid
		data->duration = GST_CLOCK_TIME_NONE;
		break;

	case GST_MESSAGE_STATE_CHANGED:
	{
		GstState old_state, new_state, pending_state;
		gst_message_parse_state_changed (msg, &old_state, &new_state, &pending_state);
		if (GST_MESSAGE_SRC (msg) == GST_OBJECT (data->pipeline))
		{
			g_print ("Pipeline state changed from %s to %s:\n",
					gst_element_state_get_name (old_state), gst_element_state_get_name (new_state));

			// Remember whether we are in the PLAYING state or not
			data->playing = (new_state == GST_STATE_PLAYING);

			if (data->playing)
			{
				// We just moved to PLAYING. Check if seeking is possible
				GstQuery *query;
				gint64 start, end;
				query = gst_query_new_seeking (GST_FORMAT_TIME);
				if (gst_element_query (data->pipeline, query))
				{
					gst_query_parse_seeking (query, NULL, &data->seek_enabled, &start, &end);
					if (data->seek_enabled)
					{
						g_print ("Seeking is ENABLED from %" GST_TIME_FORMAT " to %" GST_TIME_FORMAT "\n",
								GST_TIME_ARGS (start), GST_TIME_ARGS (end));
					}
					else
					{
						g_print ("Seeking is DISABLED for this stream.\n");
					}
				}
				else
				{
					g_printerr ("Seeking query failed.");
				}
				gst_query_unref (query);
			}
		}
	} break;
	default:
		// We should not reach here
		g_printerr ("Unexpected message received.\n");
		break;
	}
	gst_message_unref (msg);
}

/**
 * Handle command line args
 */
static gboolean process_cmd_line_args(int argc, char *argv[])
{
	int i = 0;
	for (i = 1; i < argc; i++)
	{
		if (strstr(argv[i], "rate=") != NULL)
		{
			if (sscanf(argv[i], "rate=%f", &requested_rate) != 1)
			{
				g_printerr("Invalid rate arg specified: %s\n", argv[i]);
				return FALSE;
			}
			else
			{
				g_print("Set requested rate change to %4.1f\n", requested_rate);
				test_rate_change = TRUE;
			}
		}
		else if (strstr(argv[i], "wait=") != NULL)
		{
			if (sscanf(argv[i], "wait=%d", &waitSecs) != 1)
			{
				g_printerr("Invalid wait arg specified: %s\n", argv[i]);
				return FALSE;
			}
			else
			{
				g_print("Set requested wait secs to %d\n", waitSecs);
			}
		}
		else if (strstr(argv[i], "uri=") != NULL)
		{
			if (sscanf(argv[i], "uri=%s\n", &uri[0]) != 1)
			{
				g_printerr("Invalid uri arg specified: %s\n", argv[i]);
				return FALSE;

			}
			else
			{
				g_print("Set requested URI to %s\n", uri);
			}
		}
		else if (strstr(argv[i], "rrid=") != NULL)
		{
			if (sscanf(argv[i], "rrid=%d", &rrid) != 1)
			{
				g_printerr("Invalid rrid specified: %s\n", argv[i]);
				return FALSE;
			}
			else
			{
				g_print("Set requested rrid to %d\n", rrid);
			}
		}
		else if (strstr(argv[i], "host=") != NULL)
		{
			if (sscanf(argv[i], "host=%s\n", &host[0]) != 1)
			{
				g_printerr("Invalid host arg specified: %s\n", argv[i]);
				return FALSE;
			}
			else
			{
				g_print("Set requested host ip to %s\n", host);
			}
		}
		else if (strstr(argv[i], "pipeline") != NULL)
		{
			usePlaybin = FALSE;
			g_print("Set to manually build pipeline\n");
		}
		else if (strstr(argv[i], "switch") != NULL)
		{
			test_uri_switch = TRUE;
			g_print("Set to test uri switching\n");
		}
		else if (strstr(argv[i], "position") != NULL)
		{
			test_positioning = TRUE;
			g_print("Set to test positioning\n");
		}
		else if (strstr(argv[i], "seek") != NULL)
		{
			test_seeking = TRUE;
			g_print("Set to test seeking\n");
		}
		else if (strstr(argv[i], "file=") != NULL)
		{
			if (sscanf(argv[i], "file=%s\n", &file_name[0]) != 1)
			{
				g_printerr("Invalid file name specified: %s\n", argv[i]);
				return FALSE;
			}
			else
			{
				use_file = TRUE;
				g_print("Test using local file %s rather than URI\n", file_name);
			}
		}
		else if (strstr(argv[i], "dtcp") != NULL)
		{
			use_dtcp = TRUE;
			g_print("Set to use dtcp URI\n");
		}
		else
		{
			g_printerr("Invalid option: %s\n", argv[i]);
			g_printerr("Usage:\t wait=x where x is secs, rate=y where y is desired rate\n");
			g_printerr("\t\t rrid=i where i is cds recording id, host=ip addr of server\n");
			g_printerr("\t\t uri=l where l is uri of desired content\n");
			return FALSE;
		}
	}
	return TRUE;
}

/**
 * Create playbin pipeline
 */
static GstElement* create_playbin_pipeline()
{
	GstElement* pipeline = NULL;
	char launchLine[256];
#ifdef GSTREAMER_010
	char* line1 = "playbin2 uri=";
#else
	char* line1 = "playbin uri=";
#endif
	if (!use_file)
	{
		sprintf(launchLine, "%s%s", line1, uri);
	}
	else
	{
		file_path = getenv(TEST_FILE_URL_PREFIX_ENV);
		if (file_path == NULL)
		{
			g_printerr ("Could not get env var %s value\n", TEST_FILE_URL_PREFIX_ENV);
			return NULL;
		}
		else
		{
			sprintf(launchLine, "%s%s%s", line1, file_path, file_name);
		}
	}

	g_print("Starting up playbin using line: %s\n", launchLine);
	pipeline = gst_parse_launch(launchLine, NULL);

	// Register to receive playbin source signal & call get property on sourc
	// to get supported playspeeds just like webkit would do
    g_signal_connect(pipeline, "notify::source", G_CALLBACK(on_source_changed), pipeline);

    // Uncomment this line to limit the amount of downloaded data
    //g_object_set (pipeline, "ring-buffer-max-size", (guint64)4000000, NULL);
    //g_object_set (pipeline, "ring-buffer-max-size", (guint64)400000, NULL);
    //g_object_set (pipeline, "ring-buffer-max-size", (guint64)400, NULL); - seg faults?
    g_object_set (pipeline, "ring-buffer-max-size", (guint64)4000, NULL);

    // Tried to force audio to fake sink since it complained about audio decoder
    // when trying to change URIs
    //GstElement* fake_sink = gst_element_factory_make ("fakesink", "fakesink");
    //g_object_set (pipeline, "audio-sink", fake_sink, NULL);

    return pipeline;
}

/**
 * Create pipeline manually based on pipeline which
 * playbin creates which is:
 *
 * Bin playbin20 has element: inputselector0
 * Bin playbin20 has element: uridecodebin0
 * Bin playbin20 has element: playsink0
 * Bin playsink0 has element: vbin
 *
 * Bin uridecodebin0 has element: queue20
 * Bin uridecodebin0 has element: decodebin20
 * Bin uridecodebin0 has element: typefindelement0
 * Bin uridecodebin0 has element: source
 *
 * Bin decodebin20 has element: mpeg2dec0
 * Bin decodebin20 has element: mpegvparse0
 * Bin decodebin20 has element: multiqueue0
 * Bin decodebin20 has element: mpegtsdemux0
 * Bin decodebin20 has element: typefind
 *
 * Bin source has element: dlna-soup-http-source
 *
 * Bin vbin has element: vconv
 *
 * Bin vconv has element: scale
 * Bin vconv has element: conv
 * Bin vconv has element: identity
 *
 * Bin vbin has element: vqueue
 * Bin vbin has element: videosink
 * Create pipeline which is built from manual assembly of components
 */
static GstElement* create_manual_playbin_pipeline()
{
	// Create gstreamer elements
	GstElement* pipeline = gst_pipeline_new("manual-playbin");
	if (!pipeline)
	{
		g_printerr ("Pipeline element could not be created.\n");
		return NULL;
	}
	GstElement* dlnasrc  = gst_element_factory_make ("dlnasrc", "http-source");
	if (!dlnasrc)
	{
		g_printerr ("Dlnasrc element could not be created.\n");
		return NULL;
	}
	g_object_set(G_OBJECT(dlnasrc), "uri", uri, NULL);

	GstElement* mpegtsdemux = gst_element_factory_make ("mpegtsdemux", "mpeg ts demux");
	if (!mpegtsdemux)
	{
		g_printerr ("mpegvideoparse element could not be created.\n");
		return NULL;
	}

	GstElement* mpegvideoparse = gst_element_factory_make ("mpegvideoparse", "mpeg parser");
	if (!mpegvideoparse)
	{
		g_printerr ("mpegvideoparse element could not be created.\n");
		return NULL;
	}

	GstElement* mpeg2dec = gst_element_factory_make ("mpeg2dec",  "mpeg decoder");
	if (!mpeg2dec)
	{
		g_printerr ("mpeg2dec element could not be created.\n");
		return NULL;
	}

	// Create a file sink to dump output to verify results
	GstElement* filesink = gst_element_factory_make ("filesink", "output_sink");
	if (!filesink)
	{
		g_printerr ("Filesink element could not be created.\n");
		return NULL;
	}
	g_object_set(G_OBJECT(filesink), "location", "file.output", NULL);

	// Add all elements into the pipeline
	gst_bin_add_many (GST_BIN (pipeline),
			dlnasrc, mpegtsdemux, mpegvideoparse, mpeg2dec, filesink, NULL);

	// Link the elements together
	if (!gst_element_link_many (dlnasrc, mpegtsdemux, mpegvideoparse, mpeg2dec, filesink, NULL))
	{
		g_printerr ("Problems linking elements together\n");
		return NULL;
	}

	return pipeline;
}

/**
 * Create pipeline which is built from manual assembly of components
 * which Fancy uses for her testing.
 */
static GstElement* create_fancy_pipeline()
{
	// Create gstreamer elements
	GstElement* pipeline = gst_pipeline_new("manual-playbin");
	if (!pipeline)
	{
		g_printerr ("Pipeline element could not be created.\n");
		return NULL;
	}
	GstElement* dlnasrc  = gst_element_factory_make ("dlnasrc", "http-source");
	if (!dlnasrc)
	{
		g_printerr ("Dlnasrc element could not be created.\n");
		return NULL;
	}
	GstElement* dlnabin  = gst_element_factory_make ("dlnabin", "cablelabs demuxer");
	if (!dlnabin)
	{
		g_printerr ("dlnabin element could not be created.\n");
		return NULL;
	}
	GstElement* mpegvideoparse = gst_element_factory_make ("mpegvideoparse", "mpeg parser");
	if (!mpegvideoparse)
	{
		g_printerr ("mpegvideoparse element could not be created.\n");
		return NULL;
	}
	GstElement* mpeg2dec = gst_element_factory_make ("mpeg2dec",  "mpeg decoder");
	if (!mpeg2dec)
	{
		g_printerr ("mpeg2dec element could not be created.\n");
		return NULL;
	}

	GstElement* ffmpegcolorspace = gst_element_factory_make ("ffmpegcolorspace",  "RGB YUV converter");
	if (!ffmpegcolorspace)
	{
		g_printerr ("ffmpegcolorspace element could not be created.\n");
		return NULL;
	}
	GstElement* videoscale = gst_element_factory_make ("videoscale", "videoscale");
	if (!videoscale)
	{
		g_printerr ("videoscale element could not be created.\n");
		return NULL;
	}
	GstElement* autovideosink = gst_element_factory_make ("autovideosink", "video-output");
	if (!autovideosink)
	{
		g_printerr ("autovideosink element could not be created.\n");
		return NULL;
	}
	GstElement* queue1 =  gst_element_factory_make ("queue", "queue1");
	GstElement* queue2 =  gst_element_factory_make ("queue", "queue2");
	if (!queue1 || !queue2)
	{
		g_printerr ("One of queue elements could not be created.\n");
		return NULL;
	}
	GstElement* textsink = gst_element_factory_make ("filesink", "text file");
	GstElement* captionsink = gst_element_factory_make ("filesink", "caption file");
	if (!textsink || !captionsink)
	{
		g_printerr ("One of filesink elements could not be created.\n");
		return NULL;
	}

	// Set properties as necessary on elements
	g_object_set(G_OBJECT(dlnasrc), "uri", uri, NULL);

	// Verify URI was properly set
	gchar* tmpUri = NULL;
	g_object_get(G_OBJECT(dlnasrc), "uri", &tmpUri, NULL);
	if ((tmpUri == NULL) || (strcmp(uri, tmpUri) != 0))
	{
		g_printerr ("Problems setting URI to: %s. Exiting.\n", uri);
		return NULL;
	}
	else
	{
		g_free(tmpUri);
	}

	g_object_set(G_OBJECT(textsink), "location", "demuxTextOutput", NULL);

	g_object_set(G_OBJECT(captionsink), "location", "captionsTextOutput", NULL);

	// Add all elements into the pipeline
	gst_bin_add_many (GST_BIN (pipeline),
			dlnasrc, dlnabin, queue1, mpegvideoparse, queue2, mpeg2dec, ffmpegcolorspace,
			videoscale, autovideosink, textsink, captionsink, NULL);

	// Link the elements together
	if (!gst_element_link_many (dlnasrc, dlnabin, queue1, mpegvideoparse, queue2, mpeg2dec, ffmpegcolorspace,
			videoscale, autovideosink, textsink, captionsink, NULL))
	{
		g_printerr ("Problems linking elements together\n");
		return NULL;
	}

	return pipeline;
}

/**
 * Callback when playbin's source element changes
 */
static void on_source_changed(GstElement* element, GParamSpec* param, gpointer data)
{
	g_print("Notified of source change, gather supported rates\n");

	int i = 0;
    GstElement* src = NULL;
    gchar *strVal = NULL;

    g_object_get(element, "source", &src, NULL);
    if (src != NULL)
    {
    	g_print("Got src from callback, determine if dlnasrc\n");

    	g_object_get(src, "cl_name", &strVal, NULL);
    	if (strVal != NULL)
    	{
    		// Get supported rates property value which is a GArray
    		g_print("Getting supported rates\n");
    		GArray* arrayVal = NULL;
    		g_object_get(src, "supported_rates", &arrayVal, NULL);
    		if (arrayVal != NULL)
    		{
    			g_print("Supported rates cnt: %d\n", arrayVal->len);
    			for (i = 0; i < arrayVal->len; i++)
    			{
    				g_print("Retrieved rate %d: %f\n", (i+1), g_array_index(arrayVal, gfloat, i));
    			}
    		}
    		else
    		{
    			g_printerr("Got null value for supported rates property\n");
    		}
    	}
    	else
    	{
       		g_print("dlnasrc is NOT source for pipeline\n");
    	}
    }
    else
    {
    	g_printerr("Unable to get src from callback\n");
    }
}

static void log_bin_elements(GstBin* bin)
{
	g_print("Playbin elements:\n");

	GstIterator* it = gst_bin_iterate_elements(bin);
	gboolean done = FALSE;
	GstElement *elem = NULL;
#ifdef GSTREAMER_010
	gpointer value;
#else
	GValue value = { 0 };
#endif

	while (!done)
	{
		switch (gst_iterator_next (it, &value))
		{
		case GST_ITERATOR_OK:
#ifdef GSTREAMER_010
			elem = GST_ELEMENT(value);
#else
			elem = (GstElement *) g_value_get_object (&value);
#endif
			g_print("Bin %s has element: %s\n",
				GST_ELEMENT_NAME(GST_ELEMENT(bin)),
				GST_ELEMENT_NAME(elem));

			// If this is a bin, log its elements
			if (GST_IS_BIN(elem))
			{
				log_bin_elements(GST_BIN(elem));
			}
#ifndef GSTREAMER_010
			g_value_unset (&value);
#endif
			break;
		case GST_ITERATOR_RESYNC:
			gst_iterator_resync (it);
			break;
		case GST_ITERATOR_ERROR:
			g_printerr("Unable to iterate through elements\n");
			done = TRUE;
			break;
		case GST_ITERATOR_DONE:
			done = TRUE;
			break;
		}
	}
	gst_iterator_free (it);
}

static gboolean set_pipeline_state(CustomData* data, GstState desired_state, gint timeoutSecs)
{
    GstStateChangeReturn ret = gst_element_set_state(GST_ELEMENT(data->pipeline), desired_state);
    printf("Set state returned: %d\n", ret);

    if (ret == GST_STATE_CHANGE_FAILURE)
    {
    	g_printerr ("Unable to set playbin to desired state: %s\n",
    			gst_element_state_get_name(desired_state));
    	return FALSE;
    }
    if (ret == GST_STATE_CHANGE_ASYNC)
    {
        printf("State change is async, calling get state to wait %d secs for state\n",
        		timeoutSecs);
        int maxCnt = timeoutSecs;
        int curCnt = 0;
        GstState state = GST_STATE_NULL;
        do
        {
             ret = gst_element_get_state(GST_ELEMENT(data->pipeline), // element
                    &state, // state
                    NULL, // pending
                    100000000LL); // timeout(1 second = 10^9 nanoseconds)

             if (ret == GST_STATE_CHANGE_SUCCESS)
             {
             	printf("State change succeeded, now in desired state: %s\n",
             			gst_element_state_get_name(desired_state));
             	return TRUE;
             }
             else if (ret == GST_STATE_CHANGE_FAILURE)
             {
                 g_printerr ("State change failed\n");
                 return FALSE;
             }
             else if (ret == GST_STATE_CHANGE_ASYNC)
             {
                 g_printerr ("State change time out: %d secs\n", curCnt);
             }
             else
             {
                 g_printerr ("Unknown state change return value: %d\n", ret);
                 return FALSE;
             }
             curCnt++;

             // Sleep for a short time
             g_usleep(1000000L);
        }
        while ((desired_state != state) && (curCnt < maxCnt));
     }
 	g_printerr("Timed out waiting %d secs for desired state: %s\n",
 			timeoutSecs, gst_element_state_get_name(desired_state));
    return FALSE;
}

static gboolean set_new_uri(CustomData* data)
{
	// Formulate a new URI
	gchar new_uri[256];
	new_uri[0] = '\0';
	char* line2 = "http://";
	char* line3 = ":8008/ocaphn/recording?rrid=";
	char* line4 = "&profile=MPEG_TS_SD_NA_ISO&mime=video/mpeg";
	sprintf(new_uri, "%s%s%s%d%s", line2, host, line3, 7, line4);

	// Get source of pipeline which is a playbin
	g_print("Getting source of playbin\n");
	GstElement* dlna_src = NULL;
	if (data->pipeline != NULL)
	{
	    g_object_get(data->pipeline, "source", &dlna_src, NULL);
		if (dlna_src != NULL)
		{
			g_print("Setting new uri: %s\n", new_uri);
			g_object_set(G_OBJECT(dlna_src), "uri", &new_uri, NULL);
			g_print("Done setting new uri: %s\n", new_uri);
		}
		else
		{
			g_printerr("Unable to get source of pipeline to change URI\n");
			return FALSE;
		}
	}
	else
	{
		g_printerr("Unable to change URI due to NULL pipeline\n");
		return FALSE;
	}
	return TRUE;
}