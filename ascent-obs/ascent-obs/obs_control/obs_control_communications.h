#ifndef ASCENTOBS_OBS_CONTROL_OBS_CONTROL_COMMUNICATIONS_H_
#define ASCENTOBS_OBS_CONTROL_OBS_CONTROL_COMMUNICATIONS_H_

#include <obs.hpp>
#include <string>
namespace obs_control {

class OBSControlCommunications {
public:
  virtual void Send(int command_id, OBSData& data) = 0;
  virtual void Send(int command_id) = 0;
  // |data| plus the member "|key|": |raw_json| (JSON the caller built, for
  // values obs_data can't hold); plain Send when |raw_json| is empty
  virtual void SendWithRawMember(int command_id, OBSData& data,
                                 const char* key,
                                 const std::string& raw_json) = 0;

  virtual void Shutdown() = 0;
};

}; // namespace obs_control

#endif // ASCENTOBS_OBS_CONTROL_OBS_CONTROL_COMMUNICATIONS_H_