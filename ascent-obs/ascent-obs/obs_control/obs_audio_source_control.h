#ifndef ASCENTOBS_OBS_CONTROL_OBS_AUDIO_SOURCE_CONTROL_H_
#define ASCENTOBS_OBS_CONTROL_OBS_AUDIO_SOURCE_CONTROL_H_

#include <string>
#include <memory>

#include <obs.hpp>
#include "obs_control/settings.h"

namespace obs_control {
 
extern std::string GetAudioTracksStr(uint32_t enabled_mixers);

extern const char kNoiseSuppressFilterId[];
extern const char kNoiseSuppressMethodRNNoise[];
extern const char kNoiseSuppression[];

// reads the optional 'noise_suppression' flag of an 'audio_sources[]' entry;
// missing == false, and it only applies to input devices
extern bool ParseNoiseSuppression(obs_data_t* audio_source,
                                  bool is_input_device);

class OBSAudioSourceControl {


public:
  OBSAudioSourceControl(obs_source_t* source,
                   const char* name,
                   bool is_input_device = false);
  virtual ~OBSAudioSourceControl();

  // set one track mixer id
  /*settings::AudioTracksFlags*/
  void set_mixer_track(uint32_t mixer_id);

  // add or remove mixer id
  void set_mixer_track(uint32_t mixer_id,
                       const bool checked, bool log = true);

  int volume() { return volume_; }

  bool has_source() {
    return obs_source_;
  }

  bool is_mono() { return mono_; }
  void SetMono(bool val);

  // attach/detach an RNNoise noise suppression filter (idempotent)
  void SetNoiseSuppression(bool enabled);

  // true when the loaded OBS provides the RNNoise noise suppression filter
  static bool IsNoiseSuppressionSupported();

  obs_source_t* audio_source() { return obs_source_; }

  void AddToScene(obs_scene_t* scene);

public:
  void SetVolume(int volume);
  void SetMute(bool mute);

private:
  std::string name_;
  bool is_muted_ = false;
  bool mono_ = false;
  bool is_input_device_;
  int volume_ = 100;

  OBSSource obs_source_;
  obs_fader_t* obs_fader_ = nullptr;

  // owned by |obs_source_|'s filter list while attached
  OBSSource noise_suppress_filter_;

  bool added_to_scene_ = false;
};

typedef std::shared_ptr<OBSAudioSourceControl> OBSAudioSourceControlPtr;

}; // namespace obs_control

#endif // ASCENTOBS_OBS_CONTROL_OBS_AUDIO_SOURCE_CONTROL_H_