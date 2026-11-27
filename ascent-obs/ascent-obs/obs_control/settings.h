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
extern const char kSettingsMonitorMethod[];
extern const char kSettingsMonitorBrbWhenUnfocused[];
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

// track names end up quoted on the ffmpeg-mux command line, so each one is
// capped (in bytes, on a UTF-8 boundary) to keep the line well inside the
// CreateProcess limit
const size_t kTrackNameMaxBytes = 64;

// makes a 'track_names' entry safe for the muxer command line: drops invalid
// UTF-8, control characters (C0, DEL, C1) and '"' / '\' (they would break
// the quoted argument), trims spaces at both ends and cuts the result to
// kTrackNameMaxBytes without splitting a character. "" means: use the
// default name.
std::string SanitizeTrackName(const char* name);

// reads the optional 'track_names' array of the audio 'extra_options': one
// {"name": "..."} object per mixer, index 0 = track 1. Always returns
// |max_tracks| sanitized names; missing, empty or extra entries give ""
// (default TrackN name).
// - Entries must be objects: obs_data's JSON parser drops anything else, so a
//   string or null entry shifts the following names. Send {} to keep a slot
//   on its default.
// - Like the other audio settings, names are read only when the helper is
//   idle and apply at the next fresh start (all outputs stopped); a START
//   that joins an active capture keeps that capture's names.
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

// monitor payload 'method': OBS monitor_capture method (0 auto, 1 DXGI,
// 2 WGC). Absent gives 0; out of range logs a warning and gives 0.
int ParseMonitorCaptureMethod(obs_data_t* monitor_settings);

// monitor payload 'brb_when_unfocused', absent = false
bool ParseMonitorBrbWhenUnfocused(obs_data_t* monitor_settings);

extern const char kSettingsMonitorFollowWindow[];

// monitor payload 'follow_window': the game window's HWND as an integer.
// Absent, 0, not an integer, negative or above 32 bits give 0 (off); the
// invalid ones log a warning.
uint64_t ParseMonitorFollowWindow(obs_data_t* monitor_settings);

// follow-window mode runs only for a window on the DXGI method in
// monitor-primary BRB mode (the only way the desktop sends it); any other
// combination ignores the window and records as before
bool FollowWindowActive(uint64_t follow_window, int method,
                        bool brb_when_unfocused);

// What a monitor source records with, as its START payload asked. A
// compatibility fallback rebuilds the source from this whole struct, so it
// keeps following the window with the same cursor and BRB mode.
struct MonitorConfig {
  bool cursor = false;
  int method = 0;
  bool brb_when_unfocused = false;
  uint64_t follow_window = 0;
};

// cursor, method, brb_when_unfocused and follow_window of a monitor payload
MonitorConfig ParseMonitorConfig(obs_data_t* monitor_settings);

bool FollowWindowActive(const MonitorConfig& config);

// What a later START does to a monitor source that is already running.
enum class LaterStart {
  kKeep,            // nothing changes
  kRetarget,        // the guard follows the new window (updated in place)
  kRebuildGuarded,  // an unguarded source must become guarded: rebuild it
                    // with the guard attached before it is shown
};

// |guarded|: the running source has its follow-window guard. A guard is
// never dropped by a later START: a payload without a usable follow_window
// keeps following the window it follows now.
LaterStart ReconcileLaterStart(const MonitorConfig& running, bool guarded,
                               const MonitorConfig& requested);

// EVT_DISPLAY_SOURCE_CHANGED payload for a follow-window reason change:
// {"source": "monitor"|"brb", "reason": <key>, "follow": true}. The scene's
// own event (1.3.1) has only "source"; desktops ignore the extra keys.
void FillFollowWindowSourceEvent(obs_data_t* out, bool show_game,
                                 const char* reason);

// |json| (one object, as obs_data_get_json writes it) with the member
// "|key|": |raw| added before its closing brace. |raw| is JSON the caller
// built (obs_data can't hold an array of arrays, e.g. the follow-window
// event's "masks"). Unchanged when |raw| is empty or |json| has no object.
std::string AddRawJsonMember(const std::string& json, const char* key,
                             const std::string& raw);

// the window or monitor handle the desktop sent under |key|
int64_t ParseHandle(obs_data_t* data, const char* key);

// the settings the monitor source gives OBS's monitor_capture. Follow-window
// mode adds nothing here: it is a filter on top of the same source.
void FillMonitorCaptureSettings(obs_data_t* out, uint32_t monitor_handle,
                                int monitor_index, bool cursor, int method);

// BRB replaces the monitor while the game is unfocused only when the
// monitor is the sole capture: no game and no window source
bool IsMonitorPrimaryWithBrb(bool brb_when_unfocused,
                             bool has_monitor,
                             bool has_game,
                             bool has_window);

// monitor primary: show the monitor only while the game is in the
// foreground and not minimized, otherwise show BRB
bool MonitorPrimaryShowsMonitor(bool game_foreground, bool is_minimized);

// a game source is never added while brb_when_unfocused is set: the game
// branch would select BRB while the monitor (the desktop) stays on top
bool MonitorPrimaryRefusesGameSource(bool brb_when_unfocused,
                                     bool has_monitor);

// what a START does about monitor-primary BRB mode (sources live across
// STARTs in one recorder process)
enum class MonitorPrimaryStart {
  kProceed,  // build the scene as usual
  kRelease,  // release the retained game/window source first
  kRefuse,   // fail the START: it would mix modes on a running capture
};

// |requested|: this START's monitor payload is enabled with
// brb_when_unfocused. |current_mode|: the mode the recorder is in now.
// |has_retained_source|: a game or window source from an earlier START.
// |output_active|: a recording, replay or stream is running (a START that
// joins it can't change the sources).
MonitorPrimaryStart DecideMonitorPrimaryStart(bool requested,
                                              bool current_mode,
                                              bool has_retained_source,
                                              bool output_active);
};

namespace settings {

// Windows' AppCapabilityAccessStatus for GraphicsCaptureAccessKind::Borderless
// (0..4), as libobs-winrt reports it, plus two states of our own
enum BorderlessAccess : int {
  kBorderlessAccessNotAsked = -2,  // no request ran in this process
  kBorderlessAccessNoAnswer = -1,  // the request threw or timed out
  kBorderlessAccessDeniedBySystem = 0,
  kBorderlessAccessNotDeclaredByApp = 1,
  kBorderlessAccessDeniedByUser = 2,
  kBorderlessAccessUserPromptRequired = 3,
  kBorderlessAccessAllowed = 4,
};

// the global libobs signal libobs-winrt raises after each request
extern const char kBorderlessAccessSignal[];
extern const char kBorderlessAccessSignalDecl[];
extern const char kBorderlessAccessSiteVideoStart[];

struct BorderlessAccessProbe {
  int status = kBorderlessAccessNoAnswer;
  int hresult = 0;
  std::string site;
};

// the signal's calldata; a missing param gives NoAnswer / 0 / "unknown"
BorderlessAccessProbe ReadBorderlessAccessSignal(calldata_t* params);

// "Allowed", "DeniedByUser", ...; "no answer", "not asked"; else "unknown"
const char* BorderlessAccessName(int status);

// A video start is always sent: one per START (the one at process start is
// usually lost, the pipe's sender thread starts after OBSControl::Init). A
// capture start only when the answer differs from the last one sent: monitor
// WGC re-inits on every alt-tab and every 3 s while failing.
bool BorderlessAccessEventDue(const BorderlessAccessProbe& probe,
                              int last_sent);

// event payload {"access": <int>, "site": <string>}, "hresult" only when the
// request threw. No identifier.
void FillBorderlessAccessEvent(obs_data_t* out,
                               const BorderlessAccessProbe& probe);

// QUERY_MACHINE_INFO "wgc_borderless_access": set for 0..4 only
void FillBorderlessAccessMachineInfo(obs_data_t* out, int latest);

}  // namespace settings

#endif // ASCENTOBS_OBS_CONTROL_SETTINGS_H_