/*
 *  Copyright (C) 2011, 2012 Igalia S.L
 *  Copyright (C) 2011 Zan Dobersek  <zandobersek@gmail.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <cassert>
#include <cstdio>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

#ifdef GST_API_VERSION_1
#include <gst/audio/audio.h>
#else
#include <gst/audio/multichannel.h>
#endif

#include "GOwnPtr.h"
#include "GRefPtr.h"
#include "GStreamerUtilities.h"

#ifdef GST_API_VERSION_1
static const char* gDecodebinName = "decodebin";
#else
static const char* gDecodebinName = "decodebin2";
#endif

GstBus* webkitGstPipelineGetBus(GstPipeline* pipeline)
{
#ifdef GST_API_VERSION_1
    return gst_pipeline_get_bus(pipeline);
#else
    // gst_pipeline_get_bus returns a floating reference in
    // gstreamer 0.10 so we should not adopt.
    return gst_pipeline_get_bus(pipeline);
#endif
}

GstCaps* getGstAudioCaps(int channels, float sampleRate)
{
#ifdef GST_API_VERSION_1
    return gst_caps_new_simple("audio/x-raw", "rate", G_TYPE_INT, static_cast<int>(sampleRate),
        "channels", G_TYPE_INT, channels,
        "format", G_TYPE_STRING, gst_audio_format_to_string(GST_AUDIO_FORMAT_F32),
        "layout", G_TYPE_STRING, "interleaved", NULL);
#else
    //return gst_caps_new_simple("audio/x-raw-float", "rate", G_TYPE_INT, static_cast<int>(sampleRate),
    return gst_caps_new_simple("audio/x-raw-float", "rate", G_TYPE_INT, static_cast<int>(44100),
        "channels", G_TYPE_INT, channels,
        "endianness", G_TYPE_INT, G_BYTE_ORDER,
        "width", G_TYPE_INT, 32, NULL);
#endif
}


class AudioStreamChannelsReader {

public:
    AudioStreamChannelsReader(const char* filePath);
    AudioStreamChannelsReader(const void* data, size_t dataSize);
    ~AudioStreamChannelsReader();

    void createBus(float sampleRate, bool mixToMono);

#ifdef GST_API_VERSION_1
    GstFlowReturn handleSample(GstAppSink*);
#else
    GstFlowReturn handleBuffer(GstAppSink*);
#endif
    gboolean handleMessage(GstMessage*);
    void handleNewDeinterleavePad(GstPad*);
    void deinterleavePadsConfigured();
    void buildInputPipeline();
    void plugDeinterleave(GstPad*);
    void decodeAudioForBusCreation();

private:
    const void* m_data;
    size_t m_dataSize;
    const char* m_filePath;

    float m_sampleRate;
    GstBufferList* m_frontLeftBuffers;
    GstBufferList* m_frontRightBuffers;

#ifndef GST_API_VERSION_1
    GstBufferListIterator* m_frontLeftBuffersIterator;
    GstBufferListIterator* m_frontRightBuffersIterator;
#endif

    GstElement* m_pipeline;
    unsigned m_channelSize;
    GRefPtr<GstElement> m_decodebin;
    GRefPtr<GstElement> m_deInterleave;
    GRefPtr<GMainLoop> m_loop;
    bool m_errorOccurred;
};

/*static void copyGstreamerBuffersToAudioChannel(GstBufferList* buffers, AudioChannel* audioChannel)
{
#ifdef GST_API_VERSION_1
    float* destination = audioChannel->mutableData();
    unsigned bufferCount = gst_buffer_list_length(buffers);
    for (unsigned i = 0; i < bufferCount; ++i) {
        GstBuffer* buffer = gst_buffer_list_get(buffers, i);
        ASSERT(buffer);
        gsize bufferSize = gst_buffer_get_size(buffer);
        gst_buffer_extract(buffer, 0, destination, bufferSize);
        destination += bufferSize / sizeof(float);
    }
#else
    GstBufferListIterator* iter = gst_buffer_list_iterate(buffers);
    gst_buffer_list_iterator_next_group(iter);
    GstBuffer* buffer = gst_buffer_list_iterator_merge_group(iter);
    if (buffer) {
        memcpy(audioChannel->mutableData(), reinterpret_cast<float*>(GST_BUFFER_DATA(buffer)), GST_BUFFER_SIZE(buffer));
        gst_buffer_unref(buffer);
    }

    gst_buffer_list_iterator_free(iter);
#endif
}*/

static GstFlowReturn onAppsinkPullRequiredCallback(GstAppSink* sink, gpointer userData)
{
#ifdef GST_API_VERSION_1
    return static_cast<AudioStreamChannelsReader*>(userData)->handleSample(sink);
#else
    return static_cast<AudioStreamChannelsReader*>(userData)->handleBuffer(sink);
#endif
}

gboolean messageCallback(GstBus*, GstMessage* message, AudioStreamChannelsReader* reader)
{
    return reader->handleMessage(message);
}

static void onGStreamerDeinterleavePadAddedCallback(GstElement*, GstPad* pad, AudioStreamChannelsReader* reader)
{
    reader->handleNewDeinterleavePad(pad);
}

static void onGStreamerDeinterleaveReadyCallback(GstElement*, AudioStreamChannelsReader* reader)
{
    reader->deinterleavePadsConfigured();
}

static void onGStreamerDecodebinPadAddedCallback(GstElement*, GstPad* pad, AudioStreamChannelsReader* reader)
{
    reader->plugDeinterleave(pad);
}

gboolean enteredMainLoopCallback(gpointer userData)
{
    AudioStreamChannelsReader* reader = reinterpret_cast<AudioStreamChannelsReader*>(userData);
    reader->decodeAudioForBusCreation();
    return FALSE;
}

AudioStreamChannelsReader::AudioStreamChannelsReader(const char* filePath)
    : m_data(0)
    , m_dataSize(0)
    , m_filePath(filePath)
    , m_channelSize(0)
    , m_errorOccurred(false)
{
}

AudioStreamChannelsReader::AudioStreamChannelsReader(const void* data, size_t dataSize)
    : m_data(data)
    , m_dataSize(dataSize)
    , m_filePath(0)
    , m_channelSize(0)
    , m_errorOccurred(false)
{
}

AudioStreamChannelsReader::~AudioStreamChannelsReader()
{
    if (m_pipeline) {
        GRefPtr<GstBus> bus = webkitGstPipelineGetBus(GST_PIPELINE(m_pipeline));
        ASSERT(bus);
        g_signal_handlers_disconnect_by_func(bus.get(), reinterpret_cast<gpointer>(messageCallback), this);
        gst_bus_remove_signal_watch(bus.get());

        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(GST_OBJECT(m_pipeline));
    }

    if (m_decodebin) {
        g_signal_handlers_disconnect_by_func(m_decodebin.get(), reinterpret_cast<gpointer>(onGStreamerDecodebinPadAddedCallback), this);
        m_decodebin.clear();
    }

    if (m_deInterleave) {
        g_signal_handlers_disconnect_by_func(m_deInterleave.get(), reinterpret_cast<gpointer>(onGStreamerDeinterleavePadAddedCallback), this);
        g_signal_handlers_disconnect_by_func(m_deInterleave.get(), reinterpret_cast<gpointer>(onGStreamerDeinterleaveReadyCallback), this);
        m_deInterleave.clear();
    }

#ifndef GST_API_VERSION_1
    gst_buffer_list_iterator_free(m_frontLeftBuffersIterator);
    gst_buffer_list_iterator_free(m_frontRightBuffersIterator);
#endif
    gst_buffer_list_unref(m_frontLeftBuffers);
    gst_buffer_list_unref(m_frontRightBuffers);
}

#ifdef GST_API_VERSION_1
GstFlowReturn AudioStreamChannelsReader::handleSample(GstAppSink* sink)
{
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample)
        return GST_FLOW_ERROR;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstCaps* caps = gst_sample_get_caps(sample);
    if (!caps) {
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    GstAudioInfo info;
    gst_audio_info_from_caps(&info, caps);
    int frames = GST_CLOCK_TIME_TO_FRAMES(GST_BUFFER_DURATION(buffer), GST_AUDIO_INFO_RATE(&info));

    // Check the first audio channel. The buffer is supposed to store
    // data of a single channel anyway.
    switch (GST_AUDIO_INFO_POSITION(&info, 0)) {
    case GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT:
        gst_buffer_list_add(m_frontLeftBuffers, gst_buffer_ref(buffer));
        m_channelSize += frames;
        break;
    case GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT:
        gst_buffer_list_add(m_frontRightBuffers, gst_buffer_ref(buffer));
        break;
    default:
        break;
    }

    gst_sample_unref(sample);
    return GST_FLOW_OK;

}
#endif

#ifndef GST_API_VERSION_1
GstFlowReturn AudioStreamChannelsReader::handleBuffer(GstAppSink* sink)
{
    static int leftBuffersCount = 0;
    static int rightBuffersCount = 0;

    GstBuffer* buffer = gst_app_sink_pull_buffer(sink);
    if (!buffer)
        return GST_FLOW_ERROR;

    GstCaps* caps = gst_buffer_get_caps(buffer);
    GstStructure* structure = gst_caps_get_structure(caps, 0);

    gint channels = 0;
    if (!gst_structure_get_int(structure, "channels", &channels) || !channels) {
        gst_caps_unref(caps);
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }

    gint sampleRate = 0;
    if (!gst_structure_get_int(structure, "rate", &sampleRate) || !sampleRate) {
        gst_caps_unref(caps);
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }

    gint width = 0;
    if (!gst_structure_get_int(structure, "width", &width) || !width) {
        gst_caps_unref(caps);
        gst_buffer_unref(buffer);
        return GST_FLOW_ERROR;
    }

    GstClockTime duration = (static_cast<guint64>(GST_BUFFER_SIZE(buffer)) * 8 * GST_SECOND) / (sampleRate * channels * width);
    int frames = GST_CLOCK_TIME_TO_FRAMES(duration, sampleRate);

    // Check the first audio channel. The buffer is supposed to store
    // data of a single channel anyway.
    GstAudioChannelPosition* positions = gst_audio_get_channel_positions(structure);
    switch (positions[0]) {
    case GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT:
        printf("buffer %d [LEFT]  - rate: %d - size %d\n", ++leftBuffersCount, sampleRate, GST_BUFFER_SIZE(buffer));
        gst_buffer_list_iterator_add(m_frontLeftBuffersIterator, buffer);
        m_channelSize += frames;
        break;
    case GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT:
        printf("buffer %d [RIGHT] - rate: %d - size %d\n", ++rightBuffersCount, sampleRate, GST_BUFFER_SIZE(buffer));
        gst_buffer_list_iterator_add(m_frontRightBuffersIterator, buffer);
        break;
    default:
        gst_buffer_unref(buffer);
        break;
    }

    g_free(positions);
    gst_caps_unref(caps);
    return GST_FLOW_OK;
}
#endif

gboolean AudioStreamChannelsReader::handleMessage(GstMessage* message)
{
    GOwnPtr<GError> error;
    GOwnPtr<gchar> debug;

    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS:
        g_main_loop_quit(m_loop.get());
        break;
    case GST_MESSAGE_WARNING:
        gst_message_parse_warning(message, &error.outPtr(), &debug.outPtr());
        g_warning("Warning: %d, %s. Debug output: %s", error->code,  error->message, debug.get());
        break;
    case GST_MESSAGE_ERROR:
        gst_message_parse_error(message, &error.outPtr(), &debug.outPtr());
        g_warning("Error: %d, %s. Debug output: %s", error->code,  error->message, debug.get());
        m_errorOccurred = true;
        g_main_loop_quit(m_loop.get());
        break;
    case GST_MESSAGE_STATE_CHANGED:
        GstState old_state, new_state;
        gst_message_parse_state_changed (message, &old_state, &new_state, NULL);
        g_print ("Element %s changed state from %s to %s.\n",
           GST_OBJECT_NAME (message->src),
           gst_element_state_get_name (old_state),
           gst_element_state_get_name (new_state));
        break;
    case GST_MESSAGE_STREAM_STATUS:
        GstStreamStatusType status;
        GstElement *owner;
        gst_message_parse_stream_status(message, &status, &owner);
        g_print ("Element %s(%s) changed stream status to %d.\n",
           GST_OBJECT_NAME (owner),
           GST_OBJECT_NAME (message->src), status);
        break;

    default:
        break;
    }
    return TRUE;
}

void AudioStreamChannelsReader::handleNewDeinterleavePad(GstPad* pad)
{
    // A new pad for a planar channel was added in deinterleave. Plug
    // in an appsink so we can pull the data from each
    // channel. Pipeline looks like:
    // ... deinterleave ! queue ! appsink.
    GstElement* queue = gst_element_factory_make("queue", 0);
    GstElement* sink = gst_element_factory_make("appsink", 0);

    GstAppSinkCallbacks callbacks;
    callbacks.eos = 0;
    callbacks.new_preroll = 0;
#ifdef GST_API_VERSION_1
    callbacks.new_sample = onAppsinkPullRequiredCallback;
#else
    callbacks.new_buffer_list = 0;
    callbacks.new_buffer = onAppsinkPullRequiredCallback;
#endif
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, this, 0);

    g_object_set(sink, "sync", FALSE, NULL);

    gst_bin_add_many(GST_BIN(m_pipeline), queue, sink, NULL);

    GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
    gst_pad_link_full(pad, sinkPad, GST_PAD_LINK_CHECK_NOTHING);
    gst_object_unref(GST_OBJECT(sinkPad));

    gst_element_link_pads_full(queue, "src", sink, "sink", GST_PAD_LINK_CHECK_NOTHING);

    gst_element_set_state(queue, GST_STATE_READY);
    gst_element_set_state(sink, GST_STATE_READY);
}

void AudioStreamChannelsReader::deinterleavePadsConfigured()
{
    // All deinterleave src pads are now available, let's roll to
    // PLAYING so data flows towards the sinks and it can be retrieved.
    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
}

void AudioStreamChannelsReader::plugDeinterleave(GstPad* pad)
{
    printf("Pluging deinterleave...");

    // A decodebin pad was added, plug in a deinterleave element to
    // separate each planar channel. Sub pipeline looks like
    // ... decodebin2 ! audioconvert ! audioresample ! capsfilter ! deinterleave.
    GstElement* audioConvert  = gst_element_factory_make("audioconvert", 0);
    GstElement* audioResample = gst_element_factory_make("audioresample", 0);
    GstElement* capsFilter = gst_element_factory_make("capsfilter", 0);
    m_deInterleave = gst_element_factory_make("deinterleave", "deinterleave");

    g_object_set(m_deInterleave.get(), "keep-positions", TRUE, NULL);
    g_signal_connect(m_deInterleave.get(), "pad-added", G_CALLBACK(onGStreamerDeinterleavePadAddedCallback), this);
    g_signal_connect(m_deInterleave.get(), "no-more-pads", G_CALLBACK(onGStreamerDeinterleaveReadyCallback), this);

    GstCaps* caps = getGstAudioCaps(2, m_sampleRate);
    g_object_set(capsFilter, "caps", caps, NULL);
    gst_caps_unref(caps);

    gst_bin_add_many(GST_BIN(m_pipeline), audioConvert, audioResample, capsFilter, m_deInterleave.get(), NULL);

    GstPad* sinkPad = gst_element_get_static_pad(audioConvert, "sink");
    gst_pad_link_full(pad, sinkPad, GST_PAD_LINK_CHECK_NOTHING);
    gst_object_unref(GST_OBJECT(sinkPad));

    gst_element_link_pads_full(audioConvert, "src", audioResample, "sink", GST_PAD_LINK_CHECK_NOTHING);
    gst_element_link_pads_full(audioResample, "src", capsFilter, "sink", GST_PAD_LINK_CHECK_NOTHING);
    gst_element_link_pads_full(capsFilter, "src", m_deInterleave.get(), "sink", GST_PAD_LINK_CHECK_NOTHING);

    gst_element_sync_state_with_parent(audioConvert);
    gst_element_sync_state_with_parent(audioResample);
    gst_element_sync_state_with_parent(capsFilter);
    gst_element_sync_state_with_parent(m_deInterleave.get());
}

void AudioStreamChannelsReader::buildInputPipeline()
{
    // A decodebin pad was added, plug in a deinterleave element to
    // separate each planar channel. Sub pipeline looks like
    // ... autoaudiosrc ! audioconvert ! audioresample ! capsfilter ! deinterleave.

    printf("configuring audio input...\n");
    GstElement *source = gst_element_factory_make("pulsesrc", 0);
    //GstElement *source = gst_element_factory_make("autoaudiosrc", 0);
    GstElement* audioConvert  = gst_element_factory_make("audioconvert", 0);

    GstElement* audioResample = gst_element_factory_make("audioresample", 0);
    GstElement* capsFilter = gst_element_factory_make("capsfilter", 0);
    m_deInterleave = gst_element_factory_make("deinterleave", "deinterleave");

    g_object_set(m_deInterleave.get(), "keep-positions", TRUE, NULL);
    g_signal_connect(m_deInterleave.get(), "pad-added", G_CALLBACK(onGStreamerDeinterleavePadAddedCallback), this);
    g_signal_connect(m_deInterleave.get(), "no-more-pads", G_CALLBACK(onGStreamerDeinterleaveReadyCallback), this);

    GstCaps* caps = getGstAudioCaps(2, m_sampleRate);
    g_object_set(capsFilter, "caps", caps, NULL);
    gst_caps_unref(caps);

    gst_bin_add_many(GST_BIN(m_pipeline), source, audioConvert, audioResample, capsFilter, m_deInterleave.get(), NULL);
    gst_element_link_pads_full(source, "src", audioConvert, "sink", GST_PAD_LINK_CHECK_NOTHING);
    gst_element_link_pads_full(audioConvert, "src", audioResample, "sink", GST_PAD_LINK_CHECK_NOTHING);
    gst_element_link_pads_full(audioResample, "src", capsFilter, "sink", GST_PAD_LINK_CHECK_NOTHING);
    gst_element_link_pads_full(capsFilter, "src", m_deInterleave.get(), "sink", GST_PAD_LINK_CHECK_NOTHING);

    gst_element_sync_state_with_parent(source);
    gst_element_sync_state_with_parent(audioConvert);
    gst_element_sync_state_with_parent(audioResample);
    gst_element_sync_state_with_parent(capsFilter);
    gst_element_sync_state_with_parent(m_deInterleave.get());
    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
}

void AudioStreamChannelsReader::decodeAudioForBusCreation()
{
    // Build the pipeline (giostreamsrc | filesrc) ! decodebin2
    // A deinterleave element is added once a src pad becomes available in decodebin.
    m_pipeline = gst_pipeline_new(0);

    GstBus* bus = webkitGstPipelineGetBus(GST_PIPELINE(m_pipeline));
    ASSERT(bus);
    gst_bus_add_signal_watch(bus);
    g_signal_connect(bus, "message", G_CALLBACK(messageCallback), this);

    GstElement* source;
    if (m_filePath) {
        source = gst_element_factory_make("filesrc", 0);
        g_object_set(source, "location", m_filePath, NULL);

        m_decodebin = gst_element_factory_make(gDecodebinName, "decodebin");
        g_signal_connect(m_decodebin.get(), "pad-added", G_CALLBACK(onGStreamerDecodebinPadAddedCallback), this);

        gst_bin_add_many(GST_BIN(m_pipeline), source, m_decodebin.get(), NULL);
        gst_element_link_pads_full(source, "src", m_decodebin.get(), "sink", GST_PAD_LINK_CHECK_NOTHING);
        gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
    } else {
        buildInputPipeline();
    }

}

void AudioStreamChannelsReader::createBus(float sampleRate, bool mixToMono)
{
    m_sampleRate = sampleRate;

    m_frontLeftBuffers = gst_buffer_list_new();
    m_frontRightBuffers = gst_buffer_list_new();

#ifndef GST_API_VERSION_1
    m_frontLeftBuffersIterator = gst_buffer_list_iterate(m_frontLeftBuffers);
    gst_buffer_list_iterator_add_group(m_frontLeftBuffersIterator);

    m_frontRightBuffersIterator = gst_buffer_list_iterate(m_frontRightBuffers);
    gst_buffer_list_iterator_add_group(m_frontRightBuffersIterator);
#endif

    /*GRefPtr<GMainContext> context = adoptGRef(g_main_context_new());
    g_main_context_push_thread_default(context.get());
    m_loop = adoptGRef(g_main_loop_new(context.get(), FALSE));

    // Start the pipeline processing just after the loop is started.
    GRefPtr<GSource> timeoutSource = adoptGRef(g_timeout_source_new(0));
    g_source_attach(timeoutSource.get(), context.get());
    g_source_set_callback(timeoutSource.get(), reinterpret_cast<GSourceFunc>(enteredMainLoopCallback), this, 0);

    g_main_loop_run(m_loop.get());
    g_main_context_pop_thread_default(context.get());
    */

    m_loop = adoptGRef(g_main_loop_new(NULL, FALSE));
    enteredMainLoopCallback(this);
    g_main_loop_run(m_loop.get());
    printf("finished decoding loop!\n");

    /*if (m_errorOccurred)
        return 0;

    unsigned channels = mixToMono ? 1 : 2;
    RefPtr<AudioBus> audioBus = AudioBus::create(channels, m_channelSize, true);
    audioBus->setSampleRate(m_sampleRate);

    copyGstreamerBuffersToAudioChannel(m_frontLeftBuffers, audioBus->channel(0));
    if (!mixToMono)
        copyGstreamerBuffersToAudioChannel(m_frontRightBuffers, audioBus->channel(1));

    return audioBus;
    */
}

void createBusFromAudioFile(const char* filePath, bool mixToMono, float sampleRate)
{
    return AudioStreamChannelsReader(filePath).createBus(sampleRate, mixToMono);
}

int main(int argc, char **argv)
{
    const char *filePath = 0;

    if (argc == 2) {
        filePath = argv[1];
    }

    if (!initializeGStreamer()) {
        fprintf(stderr, "Error trying to initialize gstreamer :(\n");
        return -1;
    }

    createBusFromAudioFile(filePath, false, 44100);
    printf("finished main!\n");
    return 0;
}
