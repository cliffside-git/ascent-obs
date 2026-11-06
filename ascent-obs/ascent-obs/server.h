#ifndef ASCENTOBS_SERVER_H_
#define ASCENTOBS_SERVER_H_

//-----------------------------------------------------------------------------

#include <memory>
#include <base/macros.h>
#include <communications/communication_channel_delegate.h>
#include "obs_control/obs_control.h"
#include "message_loop.h"
#include "command_framer.h"

//-----------------------------------------------------------------------------

class CommandLine;

//-----------------------------------------------------------------------------

class Server : public libascentobs::CommunicationChannelDelegate,
                      obs_control::OBSControlCommunications {
public:

  static int Run(CommandLine* options);
  virtual ~Server();

// communications::CommunicationChannelDelegate
private:

  virtual void OnConnected();
  virtual void OnDisconnected();
  virtual void OnData(const uint8_t* data, size_t size);
  virtual void OnSendDataError(const std::string& data, int error_code);

// BSControlCommunications
private:

  virtual void Send(int command_id, OBSData& data);
  virtual void Send(int command_id);
  virtual void Shutdown();

private:

  Server();

  bool Init(CommandLine* options);
  void Run();
  bool HandleShutdownCommand(int command);
  // returns false after a shutdown command (stop dispatching)
  bool DispatchCommand(const std::string& buffer);

private:

  std::unique_ptr<libascentobs::ICommunicationChannel> communications_;
  MessageLoop message_loop_;
  std::unique_ptr<obs_control::OBSControl> obs_control_;
  // OnData is called from the channel's single reader thread only
  CommandFramer framer_;

  DISALLOW_COPY_AND_ASSIGN(Server);
};

#endif // ASCENTOBS_SERVER_H_