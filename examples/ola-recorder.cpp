/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * ola-recorder.cpp
 * A simple tool to record & playback shows.
 * Copyright (C) 2011 Simon Newton
 */

#include <ola/Callback.h>
#include <ola/DmxBuffer.h>
#include <ola/Logging.h>
#include <ola/StringUtils.h>
#include <ola/base/Flags.h>
#include <ola/base/Init.h>
#include <ola/base/SysExits.h>
#include <signal.h>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "examples/ShowPlayer.h"
#include "examples/ShowLoader.h"
#include "examples/ShowRecorder.h"

// On MinGW, SignalThread.h pulls in pthread.h which pulls in Windows.h, which
// needs to be after WinSock2.h, hence this order
#include <ola/thread/SignalThread.h>  // NOLINT(build/include_order)

using std::auto_ptr;
using std::cout;
using std::endl;
using std::map;
using std::vector;
using std::string;

DEFINE_s_string(playback, p, "", "The show file to playback.");
DEFINE_s_string(record, r, "", "The show file to record data to.");
DEFINE_string(verify, "", "The show file to verify.");
DEFINE_s_string(universes, u, "",
                "A comma separated list of universes to record");
DEFINE_s_uint32(delay, d, 0, "The delay in ms between successive iterations.");
DEFINE_uint32(duration, 0, "The length of time (seconds) to run for.");
// 0 means infinite looping
DEFINE_s_uint32(iterations, i, 1,
                "The number of times to repeat the show, 0 means unlimited.");
DEFINE_uint32(start, 0,
              "Time (milliseconds) in show file to start playback from.");
DEFINE_uint32(stop, 0,
              "Time (milliseconds) in show file to stop playback at. If"
              " the show file is shorter, this option is ignored.");


void TerminateRecorder(ShowRecorder *recorder) {
  recorder->Stop();
}

/**
 * Record a show
 */
int RecordShow() {
  if (FLAGS_universes.str().empty()) {
    OLA_FATAL << "No universes specified, use -u";
    exit(ola::EXIT_USAGE);
  }

  vector<string> universe_strs;
  vector<unsigned int> universes;
  ola::StringSplit(FLAGS_universes.str(), &universe_strs, ",");
  vector<string>::const_iterator iter = universe_strs.begin();
  for (; iter != universe_strs.end(); ++iter) {
    unsigned int universe;
    if (!ola::StringToInt(*iter, &universe)) {
      OLA_FATAL << *iter << " isn't a valid universe number";
      exit(ola::EXIT_USAGE);
    }
    universes.push_back(universe);
  }

  ShowRecorder show_recorder(FLAGS_record.str(), universes);
  int status = show_recorder.Init();
  if (status)
    return status;

  {
    ola::thread::SignalThread signal_thread;
    cout << "Recording, hit Control-C to end" << endl;
    signal_thread.InstallSignalHandler(
        SIGINT, ola::NewCallback(TerminateRecorder, &show_recorder));
    signal_thread.InstallSignalHandler(
        SIGTERM, ola::NewCallback(TerminateRecorder, &show_recorder));
    if (!signal_thread.Start()) {
      show_recorder.Stop();
    }
    show_recorder.Record();
  }
  cout << "Saved " << show_recorder.FrameCount() << " frames" << endl;
  return ola::EXIT_OK;
}


/**
 * Clamps the frame count between 0 and 1.
 *
 * This allows frames that would be cached during playback to be counted.
 */
void ClampVerifyFrameCount(map<unsigned int, unsigned int> &frames) {
  map<unsigned int, unsigned int>::iterator iter;
  for (iter = frames.begin(); iter != frames.end(); ++iter) {
    if (iter->second > 1) {
      iter->second = 1;
    }
  }
}


/**
 * Verify a show file is valid
 */
int VerifyShow(const string &filename) {
  ShowLoader loader(filename);
  if (!loader.Load())
    return ola::EXIT_NOINPUT;

  map<unsigned int, unsigned int> frames_by_universe;

  ShowEntry entry;
  ShowLoader::State state;
  uint64_t playback_pos = 0;
  bool playing = false;
  while (true) {
    state = loader.NextEntry(&entry);
    if (state != ShowLoader::OK)
      break;
    playback_pos += entry.next_wait;
    frames_by_universe[entry.universe]++;
    if (FLAGS_stop > 0 && playback_pos >= FLAGS_stop) {
      // Compensate for overshooting the stop time
      playback_pos = FLAGS_stop;
      break;
    }
    if (!playing && playback_pos > FLAGS_start) {
      // Found the start point
      playing = true;
      ClampVerifyFrameCount(frames_by_universe);
    }
  }
  if (FLAGS_start > playback_pos) {
    OLA_WARN << "Show file ends before the start time (actual length "
             << playback_pos << " ms)";
  }
  if (FLAGS_stop > playback_pos) {
    OLA_WARN << "Show file ends before the stop time (actual length "
             << playback_pos << " ms)";
  }

  uint64_t total_time;
  if (playback_pos >= FLAGS_start) {
    total_time = playback_pos - FLAGS_start;
  } else {
    total_time = 0;
  }
  map<unsigned int, unsigned int>::const_iterator iter;
  unsigned int total = 0;
  cout << "------------ Summary ----------" << endl;
  if (FLAGS_start > 0) {
    cout << "Starting at: " << FLAGS_start / 1000.0 << " second(s)" << endl;
  }
  if (FLAGS_stop > 0) {
    cout << "Stopping at: " << FLAGS_stop / 1000.0 << " second(s)" << endl;
  }
  for (iter = frames_by_universe.begin(); iter != frames_by_universe.end();
       ++iter) {
    cout << "Universe " << iter->first << ": " << iter->second << " frames"
         << endl;
    total += iter->second;
  }
  cout << endl;
  cout << "Total frames: " << total << endl;
  cout << "Playback time: " << total_time / 1000.0 << " second(s)" << endl;

  if ((state == ShowLoader::OK) || (state == ShowLoader::END_OF_FILE)) {
    return ola::EXIT_OK;
  } else {
    OLA_FATAL << "Error loading show, got state " << state;
    return ola::EXIT_DATAERR;
  }
}


/**
 * Playback a recorded show
 */
int PlaybackShow() {
  ShowPlayer player(FLAGS_playback.str());
  int status = player.Init();
  if (!status) {
    status = player.Playback(FLAGS_iterations,
                             FLAGS_duration,
                             FLAGS_delay,
                             FLAGS_start,
                             FLAGS_stop);
  }
  return status;
}


/*
 * Main
 */
int main(int argc, char *argv[]) {
  ola::AppInit(&argc, argv,
               "[--record <file> --universes <universe_list>] [--playback "
               "<file>] [--verify <file>]",
               "Record a series of universes, or playback a previously "
               "recorded show.");

  if (FLAGS_stop > 0 && FLAGS_stop < FLAGS_start) {
    OLA_FATAL << "Stop time must be later than start time.";
    return ola::EXIT_USAGE;
  }

  if (!FLAGS_playback.str().empty()) {
    return PlaybackShow();
  } else if (!FLAGS_record.str().empty()) {
    return RecordShow();
  } else if (!FLAGS_verify.str().empty()) {
    return VerifyShow(FLAGS_verify.str());
  } else {
    OLA_FATAL << "One of --record or --playback or --verify must be provided";
    ola::DisplayUsage();
  }
  return ola::EXIT_OK;
}
