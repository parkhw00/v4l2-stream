
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include <functional>
#include <thread>
#include <gst/gst.h>


#include "websocket.h"

typedef struct
{
  int id;
  GstElement *pipeline;
  GstElement *v4l2src;
  GstElement *capsfilter1;
  GstElement *videorate;
  GstElement *capsfilter1_2;
  GstElement *jpegdec;
  GstElement *queue1;
  GstElement *h264enc;
  GstElement *queue2;
  GstElement *h264parse;
  GstElement *capsfilter2;
  GstElement *appsink;
  GMainLoop *main_loop;
  BroadcastServer *server;
  bool ready;
} AppGstElements;

static GstFlowReturn new_frame(GstElement *sink, AppGstElements *elements)
{
  GstSample *sample;

  g_signal_emit_by_name(sink, "pull-sample", &sample);

  if (sample)
  {
    GstBuffer *buffer = gst_sample_get_buffer( sample );
    GstMapInfo info;
    gst_buffer_map(buffer, &info, GST_MAP_READ);
    elements->server->broadcast(info.data, info.size);
    gst_buffer_unmap(buffer, &info);
    gst_sample_unref(sample);

    /*GstStructure *stat;
    g_object_get(sink, "stats", &stat, NULL);
    GST_LOG ("structure is %" GST_PTR_FORMAT, stat);
    gst_structure_free(stat);*/
    return GST_FLOW_OK;
  }

  return GST_FLOW_ERROR;
}

static void error_cb (GstBus *bus, GstMessage *msg, AppGstElements *data) {
  GError *err;
  gchar *debug_info;

  gst_message_parse_error (msg, &err, &debug_info);
  g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
  g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
  g_clear_error (&err);
  g_free (debug_info);

  g_main_loop_quit (data->main_loop);
}

int launch_pipeline(
    AppGstElements *elements,
    char *video_device,
    int width,
    int height,
    int framerate,
    int bitrate = 0,
    bool jpegdec_vaapi = false,
    bool h264enc_vaapi = false,
    bool h264parse = true
    )
{
  elements->v4l2src = gst_element_factory_make("v4l2src", NULL);
  if (!elements->v4l2src)
    g_printerr ("no v4l2src\n");

  elements->capsfilter1 = gst_element_factory_make("capsfilter", NULL);
  if (!elements->capsfilter1)
    g_printerr ("no capsfilter\n");

  elements->videorate = gst_element_factory_make("videorate", NULL);
  if (!elements->videorate)
    g_printerr ("no videorate\n");

  elements->capsfilter1_2 = gst_element_factory_make("capsfilter", NULL);
  if (!elements->capsfilter1)
    g_printerr ("no capsfilter\n");

  elements->jpegdec = gst_element_factory_make((jpegdec_vaapi) ? "vaapijpegdec" : "jpegdec", NULL);
  if (!elements->jpegdec)
    g_printerr ("no jpegdec\n");

  elements->queue1 = gst_element_factory_make("queue", NULL);
  if (!elements->queue1)
    g_printerr ("no queue\n");

  elements->h264enc = gst_element_factory_make((h264enc_vaapi) ? "vaapih264enc" : "x264enc", NULL);
  if (!elements->h264enc)
    g_printerr ("no h264enc\n");

  elements->queue2 = gst_element_factory_make("queue", NULL);
  if (!elements->queue2)
    g_printerr ("no queue\n");

  if (h264parse) {
    elements->h264parse = gst_element_factory_make("h264parse", NULL);
    if (!elements->h264parse)
      g_printerr ("no h264parse\n");
  }

  elements->capsfilter2 = gst_element_factory_make("capsfilter", NULL);
  if (!elements->capsfilter2)
    g_printerr ("no capsfilter\n");

  elements->appsink = gst_element_factory_make("appsink", NULL);
  if (!elements->appsink)
    g_printerr ("no appsink\n");

  char pipeline_name[32];
  sprintf(pipeline_name, "pipeline%d", elements->id);
  elements->pipeline = gst_pipeline_new(pipeline_name);

  if (!elements->pipeline ||
      !elements->v4l2src ||
      !elements->capsfilter1 ||
      !elements->videorate ||
      !elements->capsfilter1_2 ||
      !elements->jpegdec ||
      !elements->queue1 ||
      !elements->h264enc ||
      !elements->queue2 || (h264parse && !elements->h264parse) ||
      !elements->capsfilter2 ||
      !elements->appsink)
  {
    g_printerr("Not all elements could be created.\n");
    exit (1);
    return 1;
  }

  g_object_set(elements->h264enc, "bitrate", bitrate, "keyframe-period", 300, NULL);
  {
    int flag_zerolatency = 0x4;
    g_object_set(elements->h264enc, "tune", flag_zerolatency, NULL);
  }
  g_object_set(elements->v4l2src, "do-timestamp", TRUE, NULL);

  if (video_device != NULL)
    g_object_set(elements->v4l2src, "device", video_device, NULL);

  g_object_set(elements->capsfilter1, "caps", gst_caps_new_simple(
    "image/jpeg",
    "width", G_TYPE_INT, width,
    "height", G_TYPE_INT, height,
    NULL), NULL);

  g_object_set(elements->capsfilter1_2, "caps", gst_caps_new_simple(
    "image/jpeg",
    "width", G_TYPE_INT, width,
    "height", G_TYPE_INT, height,
    "framerate", GST_TYPE_FRACTION, framerate, 1,
    NULL), NULL);

  g_object_set(elements->capsfilter2, "caps", gst_caps_new_simple(
    "video/x-h264",
    "stream-format", G_TYPE_STRING, "byte-stream",
    "alignment", G_TYPE_STRING, "au",
    "profile", G_TYPE_STRING, "constrained-baseline",
    NULL), NULL);

  g_object_set(elements->appsink,
    "emit-signals", TRUE,
    "sync", FALSE,
    "drop", TRUE,
    "max-buffers", 2,
    NULL);

  g_signal_connect(elements->appsink, "new-sample", G_CALLBACK(new_frame), elements);

  gst_bin_add_many(GST_BIN(elements->pipeline),
      elements->v4l2src,
      elements->capsfilter1,
      elements->videorate,
      elements->capsfilter1_2,
      elements->jpegdec,
      elements->queue1,
      elements->h264enc,
      elements->queue2,
      elements->capsfilter2,
      elements->appsink,
      NULL);

  gboolean link_ret;

  if (h264parse) {
    gst_bin_add(GST_BIN(elements->pipeline), elements->h264parse);
    link_ret = gst_element_link_many(elements->v4l2src,
        elements->capsfilter1,
        elements->videorate,
        elements->capsfilter1_2,
        elements->jpegdec,
        elements->queue1,
        elements->h264enc,
        elements->queue2,
        elements->h264parse,
        elements->capsfilter2,
        elements->appsink,
        NULL);
  }
  else {
    link_ret = gst_element_link_many(elements->v4l2src,
        elements->capsfilter1,
        elements->videorate,
        elements->capsfilter1_2,
        elements->jpegdec,
        elements->queue1,
        elements->h264enc,
        elements->capsfilter2,
        elements->appsink,
        NULL);
  }

  if (link_ret != TRUE) {
    g_printerr ("Elements could not be linked.\n");
    gst_object_unref (elements->pipeline);
    return 1;
  }

  GstBus *bus = gst_element_get_bus (elements->pipeline);
  gst_bus_add_signal_watch (bus);
  g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)error_cb, elements);
  gst_object_unref (bus);

  /* Start playing the pipeline */
  g_print("Pipeline is ready for device %s\n", video_device);
  //gst_element_set_state (elements->pipeline, GST_STATE_PLAYING);
  elements->ready = true;

  elements->main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (elements->main_loop);

  gst_element_set_state (elements->pipeline, GST_STATE_NULL);
  gst_object_unref (elements->pipeline);
  elements->ready = false;
  return 0;
}


static pid_t *childs;
static int child_num;

static void on_signal(int sig)
{
  fprintf(stderr, "signal %d\n", sig);
  if (sig == SIGCHLD)
  {
    while(1)
    {
      pid_t p;

      p = waitpid (-1, NULL, WNOHANG);
      if (p == (pid_t)-1 || p == 0)
        break;
      fprintf(stderr, "child %d terminated.\n", (int)p);
    }
    return;
  }

  if (sig == SIGINT)
  {
    int i;
    for (i=0; i<child_num; i++)
      kill (childs[i], SIGINT);
    return;
  }
}

int main(int argc, char *argv[])
{
  char *opt_device = (char*)"/dev/video%d";
  int opt_devcount = 2;
  int opt_width = 1280;
  int opt_height = 720;
  int opt_framerate = 20;
  int opt_port = 8000;

  while (1) {
    int opt;
    opt = getopt (argc, argv, "?d:n:w:h:f:p:");
    if (opt < 0)
      break;

    switch (opt) {
      default:
      case '?':
        fprintf (stderr,
            " $ v4l2-streamer <options>\n"
            "options:\n"
            " -d <devname>        : v4l2 device name\n"
            " -n <devcount>       : v4l2 device count to monitor\n"
            "                       non-zero for fork server\n"
            " -w <width>          : width of captured screen\n"
            " -h <height>         : height of captured screen\n"
            " -f <framerate>      : framerate\n"
            " -p <server-port>    : server port number\n");
        exit (1);

      case 'd':
        opt_device = optarg;
        break;

      case 'n':
        opt_devcount = atoi (optarg);
        break;

      case 'w':
        opt_width = atoi (optarg);
        break;

      case 'h':
        opt_height = atoi (optarg);
        break;

      case 'f':
        opt_framerate = atoi (optarg);
        break;

      case 'p':
        opt_port = atoi (optarg);
        break;
    }
  }

  fprintf(stderr, "... %d\n", opt_devcount);
  if (opt_devcount > 0)
  {
    fprintf(stderr, "fork server..\n");

    signal(SIGCHLD, on_signal);
    signal(SIGINT, on_signal);

    int i;
    for (i=0; i<opt_devcount; i++)
    {
      pid_t pid;
      char *devname, *port;

      asprintf (&devname, opt_device, i);
      asprintf (&port, "%d", opt_port + i);

      pid = fork();
      if (pid == 0)
      {
        char *w, *h, *f;

        fprintf(stderr, "child for %s..\n", devname);
        asprintf (&w, "%d", opt_width);
        asprintf (&h, "%d", opt_height);
        asprintf (&f, "%d", opt_framerate);

        char * const cargv[] =
        {
          argv[0],
          "-d", devname,
          "-n", "0",
          "-w", w,
          "-h", h,
          "-f", f,
          "-p", port,
          NULL,
        };

        execvp (argv[0], cargv);
      }

      childs = (pid_t*)realloc(childs, sizeof(childs[0]) * (child_num+1));
      childs[child_num] = pid;
      child_num ++;

      fprintf (stderr, "child %d for device \"%s\", port %s\n", (int)pid, devname, port);

      free (devname);
      free (port);
    }

    fprintf(stderr, "wait child..\n");
    while(1)
    {
      pid_t pid;
      pid = wait(NULL);
      if (pid == (pid_t)-1)
      {
        fprintf(stderr, "wait(), %d. %s\n", (int)pid, strerror(errno));
        break;
      }
    }
    fprintf(stderr, "fork server terminate..\n");

    return 0;
  }

  fprintf(stderr, "wait on port %d for device %s\n", opt_port, opt_device);
  gst_init (&argc, &argv);

  AppGstElements elements;
  memset(&elements, 0, sizeof(AppGstElements));

  elements.server = new BroadcastServer([&](BroadcastServerEvent e) {
    if (!elements.ready) {
      g_printerr("Pipeline is not ready for device %s, port %d\n", opt_device, opt_port);
      return;
    }
    switch (e) {
    case BroadcastServerEvent::FirstOpen:
      g_print("Streaming started\n");
      gst_element_set_state (elements.pipeline, GST_STATE_PLAYING);
      break;
    case BroadcastServerEvent::LastClose:
      g_print("Streaming paused\n");
      gst_element_set_state (elements.pipeline, GST_STATE_READY);
      break;
    }
  });
  std::thread th([&]() {
    g_print("Server started\n");
    elements.server->run(opt_port);
  });

  int ret = launch_pipeline(&elements, opt_device, opt_width, opt_height, opt_framerate);
  g_print("launch_pipeline returned %d\n", ret);
  exit(1);
  th.join();
  g_print("done.\n");

  return 0;
}

/* vim: set sw=2 et: */
