///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Hively Playback Plugin
//
// Implements RVPlaybackPlugin interface for AHX and HVL (Hively Tracker) music formats.
// Based on hvl_replay by Xeron/IRIS.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// C11 nullptr compatibility
#ifndef nullptr
#define nullptr ((void*)0)
#endif

#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>
#include <retrovert/settings.h>

#include "hvl_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define FREQ 48000
#define FRAME_SIZE ((FREQ * 2) / 50)

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_METADATA_API();
RV_PLUGIN_USE_LOG_API();

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct HivelyReplayerData {
    struct hvl_tune* tune;
    void* song_data;
    int16_t temp_data[FRAME_SIZE * 4];
    int read_index;
    int frames_decoded;
} HivelyReplayerData;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* hively_supported_extensions(void) {
    return "ahx,hvl";
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* hively_create(const RVService* service_api) {
    void* data = malloc(sizeof(struct HivelyReplayerData));
    memset(data, 0, sizeof(struct HivelyReplayerData));

    return data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int hively_destroy(void* user_data) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;

    if (data->tune) {
        hvl_FreeTune(data->tune);
    }

    free(data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int hively_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    RVIoReadUrlResult read_res;

    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        rv_error("Failed to load %s to memory", url);
        return -1;
    }

    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;

    // Free previous tune if any
    if (data->tune) {
        hvl_FreeTune(data->tune);
        data->tune = nullptr;
    }

    data->tune = hvl_LoadTuneMemory(read_res.data, (int)read_res.data_size, FREQ, 0);
    if (data->tune == nullptr) {
        rv_error("Failed to parse %s", url);
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    hvl_InitSubsong(data->tune, subsong);

    // Reset buffer state
    data->read_index = 0;
    data->frames_decoded = 0;

    rv_io_free_url_to_memory(read_res.data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void hively_close(void* user_data) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;

    if (data->tune) {
        hvl_FreeTune(data->tune);
        data->tune = nullptr;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult hively_probe_can_play(uint8_t* data, uint64_t data_size, const char* url, uint64_t total_size) {
    (void)data_size;
    (void)url;
    (void)total_size;

    // Check for AHX format (THX header)
    if ((data[0] == 'T') && (data[1] == 'H') && (data[2] == 'X') && (data[3] < 3)) {
        return RVProbeResult_Supported;
    }

    // Check for HVL format
    if ((data[0] == 'H') && (data[1] == 'V') && (data[2] == 'L')) {
        return RVProbeResult_Supported;
    }

    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo hively_read_data(void* user_data, RVReadData dest) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;

    // Calculate how many S16 stereo frames fit in the output buffer
    uint32_t max_frames = dest.channels_output_max_bytes_size / (sizeof(int16_t) * 2);
    int16_t* output = (int16_t*)dest.channels_output;
    uint32_t frames_written = 0;
    int reached_end = 0;

    while (frames_written < max_frames) {
        // If internal decode buffer is empty, decode another frame
        if (data->read_index >= data->frames_decoded) {
            int16_t* temp_buf = data->temp_data;
            int bytes = hvl_DecodeFrame(data->tune, (int8_t*)temp_buf, (int8_t*)temp_buf + 2, 4, &reached_end);
            data->frames_decoded = bytes / 4;
            data->read_index = 0;

            if (data->frames_decoded == 0 || reached_end) {
                break;
            }
        }

        // Copy S16 from internal decode buffer to output
        uint32_t available = (uint32_t)(data->frames_decoded - data->read_index);
        uint32_t to_copy = max_frames - frames_written;
        if (to_copy > available) {
            to_copy = available;
        }

        int16_t* src = &data->temp_data[data->read_index * 2];
        memcpy(&output[frames_written * 2], src, to_copy * 2 * sizeof(int16_t));

        data->read_index += (int)to_copy;
        frames_written += to_copy;
    }

    RVAudioFormat format = { RVAudioStreamFormat_S16, 2, FREQ };
    RVReadStatus status = (reached_end && frames_written == 0) ? RVReadStatus_Finished : RVReadStatus_Ok;
    return (RVReadInfo) { format, (uint16_t)frames_written, status};
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t hively_seek(void* user_data, int64_t ms) {
    (void)user_data;
    (void)ms;
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int hively_metadata(const char* url, const RVService* service_api) {
    (void)service_api;
    RVIoReadUrlResult read_res;

    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        rv_error("Failed to load %s to memory", url);
        return -1;
    }

    bool is_ahx = true;
    uint8_t* t = (uint8_t*)read_res.data;

    if ((t[0] == 'H') && (t[1] == 'V') && (t[2] == 'L')) {
        is_ahx = false;
    }

    struct hvl_tune* tune = hvl_LoadTuneMemory((uint8_t*)read_res.data, (int)read_res.data_size, FREQ, 0);
    if (tune == nullptr) {
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    // Length calculation not supported by hvl_replay
    float length = 0.0f;

    const char* tool = is_ahx ? "AHX Tracker" : "Hively Tracker";

    RVMetadataId index = rv_metadata_create_url(url);

    rv_metadata_set_tag(index, RV_METADATA_TITLE_TAG, tune->ht_Name);
    rv_metadata_set_tag(index, RV_METADATA_SONGTYPE_TAG, tool);
    rv_metadata_set_tag(index, RV_METADATA_ARTIST_TAG, tool);
    rv_metadata_set_tag_f64(index, RV_METADATA_LENGTH_TAG, length);

    // Instruments start from 1 in hively so skip 0
    for (int i = 1; i < tune->ht_InstrumentNr; ++i) {
        rv_metadata_add_instrument(index, tune->ht_Instruments[i].ins_Name);
    }

    if (tune->ht_SubsongNr > 1) {
        for (int i = 0, c = tune->ht_SubsongNr; i < c; ++i) {
            rv_metadata_add_subsong(index, i, "", 0.0f);
        }
    }

    hvl_FreeTune(tune);
    rv_io_free_url_to_memory(read_res.data);

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void hively_event(void* user_data, uint8_t* data, uint64_t len) {
    struct HivelyReplayerData* replayerData = (struct HivelyReplayerData*)user_data;

    if (replayerData->tune == nullptr || data == nullptr || len < 8) {
        return;
    }

    struct hvl_tune* tune = replayerData->tune;

    // Report current pattern (position) and row
    data[7] = (uint8_t)(tune->ht_PosNr & 0xFF);
    data[6] = (uint8_t)(tune->ht_NoteNr & 0xFF);
    data[5] = 0;
    data[4] = 0;

    // VU meters from voice volumes (scale 0-64 to 0-255)
    int num_channels = tune->ht_Channels < 4 ? tune->ht_Channels : 4;
    for (int i = 0; i < 4; i++) {
        if (i < num_channels) {
            data[3 - i] = (uint8_t)(tune->ht_Voices[i].vc_VoiceVolume * 4);
        } else {
            data[3 - i] = 0;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void hively_static_init(const RVService* service_api) {
    rv_init_log_api(service_api);
    rv_init_io_api(service_api);
    rv_init_metadata_api(service_api);

    hvl_InitReplayer();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Tracker visualization API

static int hively_get_tracker_info(void* user_data, RVTrackerInfo* info) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;

    if (data->tune == nullptr) {
        return -1;
    }

    struct hvl_tune* tune = data->tune;

    // In Hively, positions contain track assignments per channel
    // num_patterns = number of positions in the order list
    info->num_patterns = tune->ht_PositionNr;
    info->num_channels = tune->ht_Channels;
    info->num_orders = tune->ht_PositionNr;
    info->num_samples = tune->ht_InstrumentNr;
    info->current_pattern = (uint16_t)tune->ht_PosNr;
    info->current_row = (uint16_t)tune->ht_NoteNr;
    info->current_order = (uint16_t)tune->ht_PosNr;
    info->rows_per_pattern = tune->ht_TrackLength;

    // Copy song name
    strncpy(info->song_name, tune->ht_Name, sizeof(info->song_name) - 1);
    info->song_name[sizeof(info->song_name) - 1] = '\0';

    // Copy instrument names (instruments start from 1)
    int copy_count = tune->ht_InstrumentNr < 32 ? tune->ht_InstrumentNr : 32;
    for (int i = 1; i < copy_count; ++i) {
        strncpy(info->sample_names[i - 1], tune->ht_Instruments[i].ins_Name, sizeof(info->sample_names[0]) - 1);
        info->sample_names[i - 1][sizeof(info->sample_names[0]) - 1] = '\0';
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int hively_get_pattern_cell(void* user_data, int pattern, int row, int channel, RVPatternCell* cell) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;

    if (data->tune == nullptr) {
        return -1;
    }

    struct hvl_tune* tune = data->tune;

    // Bounds check - pattern is actually position index in Hively
    if (pattern < 0 || pattern >= tune->ht_PositionNr || row < 0 || row >= tune->ht_TrackLength || channel < 0
        || channel >= tune->ht_Channels) {
        return -1;
    }

    // In Hively, each position has a track number for each channel
    // Look up which track is assigned to this channel at this position
    uint8_t track_num = tune->ht_Positions[pattern].pos_Track[channel];

    // Get the step from the track
    struct hvl_step* step = &tune->ht_Tracks[track_num][row];

    cell->note = step->stp_Note;
    cell->instrument = step->stp_Instrument;
    cell->volume = 0; // Hively doesn't have a volume column
    cell->effect = step->stp_FX;
    cell->effect_param = step->stp_FXParam;

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int hively_get_pattern_num_rows(void* user_data, int pattern) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;

    if (data->tune == nullptr || pattern < 0) {
        return 0;
    }

    // All patterns in Hively have the same length
    return data->tune->ht_TrackLength;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_hively_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "hively",
    "0.0.1",
    "hvl_replay 1.9",
    hively_probe_can_play,
    hively_supported_extensions,
    hively_create,
    hively_destroy,
    hively_event,
    hively_open,
    hively_close,
    hively_read_data,
    hively_seek,
    hively_metadata,
    hively_static_init,
    NULL, // settings_updated

    // Tracker visualization API
    hively_get_tracker_info,
    hively_get_pattern_cell,
    hively_get_pattern_num_rows,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_hively_plugin;
}
