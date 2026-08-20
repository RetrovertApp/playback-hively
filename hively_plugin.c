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
    uint32_t capacity_frames = dest.channels_output_max_bytes_size / (sizeof(int16_t) * 2);
    uint32_t max_frames = dest.info.frame_count < capacity_frames ? dest.info.frame_count : capacity_frames;
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
// Visualization API
//
// AHX/HVL is a native random-access tracker: positions in the order list map to
// per-channel tracks, all decoded up front, so it advertises the Synchronized
// model with the whole song known. Each position has one track per channel; a
// "pattern" in the viz model is the current position.

#define HIVELY_COLUMN_COUNT 6

static void hively_render_note(uint8_t note, char* out, size_t cap) {
    static const char* names[12]
        = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };
    if (note == 0) {
        out[0] = '\0';
        return;
    }
    int n = note - 1;
    snprintf(out, cap, "%s%d", names[n % 12], n / 12 + 1);
}

static bool hively_get_structure(void* user_data, RVVizInfo* out) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;
    if (data == nullptr || data->tune == nullptr || out == nullptr) {
        return false;
    }
    out->caps = RVVizCaps_PatternCells | RVVizCaps_Scope | RVVizCaps_WholeSongKnown;
    out->scroll_mode = RVScrollMode_Synchronized;
    out->pattern_channel_count = data->tune->ht_Channels;
    out->scope_channel_count = data->tune->ht_Channels;
    out->column_count = HIVELY_COLUMN_COUNT;
    return true;
}

static uint32_t hively_get_columns(void* user_data, RVColumnDesc* out, uint32_t cap) {
    (void)user_data;
    static const struct {
        const char* label;
        uint8_t width;
        RVColumnKind kind;
    } cols[HIVELY_COLUMN_COUNT] = {
        { "Note", 3, RVColumnKind_Note }, { "Inst", 2, RVColumnKind_Instrument },
        { "FX", 1, RVColumnKind_Effect }, { "Prm", 2, RVColumnKind_Param },
        { "FX2", 1, RVColumnKind_Effect }, { "Pr2", 2, RVColumnKind_Param },
    };
    uint32_t n = cap < HIVELY_COLUMN_COUNT ? cap : HIVELY_COLUMN_COUNT;
    for (uint32_t i = 0; i < n; i++) {
        memset(out[i].label, 0, sizeof(out[i].label));
        strncpy((char*)out[i].label, cols[i].label, sizeof(out[i].label) - 1);
        out[i].char_width = cols[i].width;
        out[i].kind = cols[i].kind;
    }
    return n;
}

static uint32_t hively_fill_channels(struct HivelyReplayerData* data, RVChannelDesc* out, uint32_t cap) {
    uint32_t count = data->tune->ht_Channels;
    if (count > cap) {
        count = cap;
    }
    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        snprintf((char*)out[i].name, sizeof(out[i].name), "Ch %u", i + 1);
        out[i].scope_width = 1;
    }
    return count;
}

static uint32_t hively_get_pattern_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;
    if (data == nullptr || data->tune == nullptr) {
        return 0;
    }
    return hively_fill_channels(data, out, cap);
}

static uint32_t hively_get_scope_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;
    if (data == nullptr || data->tune == nullptr) {
        return 0;
    }
    return hively_fill_channels(data, out, cap);
}

static bool hively_get_position(void* user_data, RVTrackerPosition* out) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;
    if (data == nullptr || data->tune == nullptr || out == nullptr) {
        return false;
    }
    struct hvl_tune* tune = data->tune;
    out->order = (uint32_t)tune->ht_PosNr;
    out->pattern = (uint32_t)tune->ht_PosNr;
    out->row = (uint32_t)tune->ht_NoteNr;
    out->window_lo = 0;
    out->window_hi = tune->ht_TrackLength;
    return true;
}

static uint32_t hively_get_channel_rows(void* user_data, uint32_t* out, uint32_t cap) {
    (void)user_data;
    (void)out;
    (void)cap;
    return 0; // Synchronized: window comes from get_position
}

static uint32_t hively_get_cells(void* user_data, int32_t channel, uint32_t row_lo, uint32_t row_hi, RVPatternCell* out,
                                 uint32_t cap) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;
    if (data == nullptr || data->tune == nullptr || out == nullptr) {
        return 0;
    }
    struct hvl_tune* tune = data->tune;
    uint32_t num_rows = tune->ht_TrackLength;
    int num_ch = tune->ht_Channels;
    if (row_hi > num_rows) {
        row_hi = num_rows;
    }
    int pos = tune->ht_PosNr;

    int ch_start = channel < 0 ? 0 : channel;
    int ch_end = channel < 0 ? num_ch : channel + 1;
    if (ch_start >= num_ch) {
        return 0;
    }

    uint32_t written = 0;
    for (uint32_t row = row_lo; row < row_hi; row++) {
        for (int ch = ch_start; ch < ch_end; ch++) {
            uint8_t track = tune->ht_Positions[pos].pos_Track[ch];
            struct hvl_step* step = &tune->ht_Tracks[track][row];
            uint32_t raws[HIVELY_COLUMN_COUNT]
                = { step->stp_Note,   step->stp_Instrument, step->stp_FX,
                    step->stp_FXParam, step->stp_FXb,       step->stp_FXbParam };
            for (int c = 0; c < HIVELY_COLUMN_COUNT; c++) {
                if (written >= cap) {
                    return written;
                }
                RVPatternCell* cell = &out[written++];
                cell->raw = raws[c];
                memset(cell->text, 0, sizeof(cell->text));
                char* txt = (char*)cell->text;
                switch (c) {
                    case 0:
                        hively_render_note(step->stp_Note, txt, sizeof(cell->text));
                        break;
                    case 1:
                        if (step->stp_Instrument) {
                            snprintf(txt, sizeof(cell->text), "%02X", step->stp_Instrument);
                        }
                        break;
                    case 2:
                        if (step->stp_FX || step->stp_FXParam) {
                            snprintf(txt, sizeof(cell->text), "%X", step->stp_FX & 0xF);
                        }
                        break;
                    case 3:
                        if (step->stp_FX || step->stp_FXParam) {
                            snprintf(txt, sizeof(cell->text), "%02X", step->stp_FXParam);
                        }
                        break;
                    case 4:
                        if (step->stp_FXb || step->stp_FXbParam) {
                            snprintf(txt, sizeof(cell->text), "%X", step->stp_FXb & 0xF);
                        }
                        break;
                    case 5:
                        if (step->stp_FXb || step->stp_FXbParam) {
                            snprintf(txt, sizeof(cell->text), "%02X", step->stp_FXbParam);
                        }
                        break;
                }
            }
        }
    }
    return written;
}

static void hively_set_scope_enabled(void* user_data, bool on) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;
    if (data == nullptr || data->tune == nullptr) {
        return;
    }
    hvl_set_scope_enabled(data->tune, on ? 1 : 0);
}

static uint32_t hively_get_scope_samples(void* user_data, int32_t channel, float* out, uint32_t cap) {
    struct HivelyReplayerData* data = (struct HivelyReplayerData*)user_data;
    if (data == nullptr || data->tune == nullptr || out == nullptr) {
        return 0;
    }
    return hvl_get_scope_data(data->tune, channel, out, cap);
}

static uint32_t hively_get_vu(void* user_data, float* out, uint32_t cap) {
    (void)user_data;
    (void)out;
    (void)cap;
    return 0;
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
    NULL, // static_destroy
    hively_get_structure,
    hively_get_columns,
    hively_get_pattern_channels,
    hively_get_scope_channels,
    hively_get_position,
    hively_get_channel_rows,
    hively_get_cells,
    hively_set_scope_enabled,
    hively_get_scope_samples,
    hively_get_vu,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_hively_plugin;
}
