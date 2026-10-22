#ifndef ASCENTOBS_OBS_CONTROL_SETTINGS_H_
#define ASCENTOBS_OBS_CONTROL_SETTINGS_H_

#include <obs.hpp>

#include <string>
#include <vector>

namespace settings {

extern const char kSettingsAudioSampleRate[];
extern const char kSettingsAudioMono[];

extern const char kSettingsVideo[];
extern const char kSettingsVideoFPS[];
extern const char kSettingsVideoBaseWidth[];
extern const char kSettingsVideoBaseHeight[];
extern const char kSettingsVideoOutputWidth[];
extern const char kSettingsVideoOutputHeight[];
extern const char kSettingsVideoCompatibilityMode[];
extern const char kSettingsGameCursor[];

extern const char kSettingsAudio[];
extern const char kSettingsAudioOutput[];
extern const char kSettingsAudioInput[];
extern const char kSettingsExtraOptions[];
extern const char kSettingsTrackNames[];

extern const char kSettingsVideoEncoder[];
extern const char kSettingsFileoutput[];

extern const char kSettingsScene[];
extern const char kSettingsSourceMonitor[];
extern const char kSettingsSourceWindowCapture[];
extern const char kSettingsSourceGame[];
extern const char kSettingsReplay[];
extern const char kSettingsStreaming[];
extern const char kSettingsSourceBRB[];
extern const char kSettingsSourceAux[];
extern const char kSettingsSourceTobii[];
extern const char kSettingsForeground[];
extern const char kAllowTransparency[];
extern const char kKeepRecordingOnLostForeground[];
extern const char kEncoderCustomParameters[];
extern const char kSettingsSecondaryFile[];
extern const char kCustomParameters[];

void SetDefaultAudio(OBSData& audio_settings);
void SetDefaultVideo(OBSData& video_settings);
void SetDefaultVideoEncoder(OBSData& video_settings);
void SetCustomEncoderParameters(OBSData& video_settings,
                                OBSData& custom_parameters);

bool GetAudioExtraParam(OBSData& audio_settings, const char *name);

// reads the optional 'track_names' array of the audio 'extra_options': one
// {"name": "..."} object per mixer, index 0 = track 1. Always returns
// |max_tracks| names; missing, empty or extra entries give "" (default name).
// Non-object entries are dropped by obs_data's JSON parser, so they shift
// the following names; send {} to keep a slot on its default.
std::vector<std::string> ParseTrackNames(obs_data_t* audio_extra_options,
                                         size_t max_tracks);

enum AudioTracksFlags : uint32_t {
  AudioTrack1 = (1 << 0),
  AudioTrack2 = (1 << 1),
  AudioTrack3 = (1 << 2),
  AudioTrack4 = (1 << 3),
  AudioTrack5 = (1 << 4),
  AudioTrack6 = (1 << 5),
  AudioTrackAll = 0xff
};

int GetSupportedAudioTracksCount(const uint32_t& tracks);
};

#endif // ASCENTOBS_OBS_CONTROL_SETTINGS_H_