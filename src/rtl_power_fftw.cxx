/*
* rtl_power_fftw, program for calculating power spectrum from rtl-sdr reciever.
* Copyright (C) 2015 Klemen Blokar <klemen.blokar@ad-vega.si>
*                    Andrej Lajovic <andrej.lajovic@ad-vega.si>
*
*                    Additions by: Mario Cannistra <mariocannistra@gmail.com>
*                                 (added -e param for session duration)
*                                 (added -q flag to limit verbosity)
*                                 (added -m param to produce binary matrix output
*                                                 with separate metadata file)
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/
#include <iostream>
#include <fstream>
#include <rtl-sdr.h>

#include "acquisition.h"
#include "datastore.h"
#include "device.h"
#include "exceptions.h"
#include "interrupts.h"
#include "params.h"
#include "metadata.h"

#include <time.h>

std::ofstream binfile, metafile;
int metaRows = 0;
int metaCols = 0;
float avgScanDur = 0.0;
float sumScanDur = 0.0;
time_t scanEnd, scanBeg;
int tunfreq;
int startFreq, endFreq, stepFreq;
int hops;
int currentHopNumber;
std::string firstAcqTimestamp, lastAcqTimestamp;
int cntTimeStamps;
int actual_samplerate;
int excludedBINS = 0;
int actualBINS = 0;
double cropFreqOffset = 0.0;
int got_sighup = false;
int got_sigint = false;
bool already_hooked_up = false;

int main(int argc, char **argv)
{
  bool do_exit = false;
  time_t exit_time = 0;
  bool freqsMetaNeeded = true;

  ReturnValue final_retval = ReturnValue::Success;

  try {
    // Parse command line arguments.
    Params params(argc, argv);

    // Set up RTL-SDR device.
    Rtlsdr rtldev(params.dev_index);

    // endless will take precedence on session duration time
    // so i'll force session_duration_isSet = false if endless has been specified
    if(params.endless) params.session_duration_isSet = false;
    if(params.session_duration_isSet)
    {
      // get desired session duration from cmd line args:
      exit_time = (int) params.session_duration;
      std::cerr << "Scan session duration: " << exit_time << " seconds" << std::endl;
    }

    // Print the available gains and select the one nearest to the requested gain.
    rtldev.print_gains();
    int gain = rtldev.nearest_gain(params.gain);
    std::cerr << "Selected nearest available gain: " << gain
              << " (" << 0.1*gain << " dB)" << std::endl;
    rtldev.set_gain(gain);

    // Temporarily set the frequency to params.cfreq, just so that the device does not
    // complain upon setting the sample rate. If this fails, it's not a big deal:
    // the sample rate will be read out just fine, but librtlsdr _might_ print an
    // error message to stderr.
    try {
      rtldev.set_frequency(params.cfreq);
    }
    catch (RPFexception&) {}

    // Set frequency correction
    if (params.ppm_error != 0) {
      rtldev.set_freq_correction(params.ppm_error);
      std::cerr << "PPM error set to: " << params.ppm_error << std::endl;
    }

    // Set sample rate
    rtldev.set_sample_rate(params.sample_rate);
    actual_samplerate = rtldev.sample_rate();
    std::cerr << "Actual sample rate: " << actual_samplerate << " Hz" << std::endl;

    // Create a plan of the operation. This will calculate the number of repeats,
    // adjust the buffer size for optimal performance and create a list of frequency
    // hops.
    Plan plan(params, actual_samplerate);
    // Print info on capture time and associated specifics.
    plan.print();

	// moved this step here since we need to know the number of planned hops
	// in order to properly account for the number of needed baseline values
	// matching the number of bins after the cropping if any:

    // Read auxiliary data for window function and baseline correction.
    AuxData auxData(params);
	
    //Begin the work: prepare data buffers
    Datastore data(params, auxData.window_values);

    // Install a signal handler for detecting Ctrl+C.
    // set_CtrlC_handler(true);
	HookupHandler();

    if(params.session_duration_isSet) {
      // calculate at which time we have to stop looping
      exit_time = time(NULL) + exit_time;
    }

    if(params.matrixMode) {
      //std::ofstream binfile, freqfile, metafile;
      // let's start truncating the binary file if already exists
      // we will then write appending (see write_data)
      binfile.open(params.bin_file, std::ios::out | std::ios::trunc | std::ios::binary);
      binfile.close();
    }

    params.finalfreq = plan.freqs_to_tune.back();
    //Read from device and do FFT
    do {
	  currentHopNumber = 0;
      for (auto iter = plan.freqs_to_tune.begin(); iter != plan.freqs_to_tune.end();) {
		// we need to keep track of the current hop number
		// to subtract from the corresponding section of baseline data
		// in a multi-hop scenario
		currentHopNumber = currentHopNumber + 1;

		if( (params.outcnt == 0 && params.talkless) || (params.talkless==false) )
			std::cerr << "\nHop # " << currentHopNumber << std::endl;

		// Begin a new data acquisition.
        Acquisition acquisition(params, auxData, rtldev, data, actual_samplerate, *iter);
        try {
          // Read the required amount of data and process it.
          acquisition.run();
          iter++;
        }
        catch (TuneError &e) {
          // The receiver was unable to tune to this frequency. It might be just a "dead"
          // spot of the receiver. Remove this frequency from the list and continue.
          std::cerr << "Unable to tune to " << e.frequency() << ". Dropping from frequency list." << std::endl;
          iter = plan.freqs_to_tune.erase(iter);
          continue;
        }

        // Print a summary (number of samples, readouts etc.) to stderr.
        if( (params.outcnt == 0 && params.talkless) || (params.talkless==false) ) acquisition.print_summary();


        // in matrix mode we'll write a separate file with metadata about the scan session
        // only once per run
        if(params.matrixMode && freqsMetaNeeded) {
          tunfreq = *plan.freqs_to_tune.begin();
          startFreq = params.startfreq;
          tunfreq = plan.freqs_to_tune.back();
          stepFreq = actual_samplerate / params.N;
          endFreq   = params.stopfreq;

          freqsMetaNeeded = false;
        }

        // Write the gathered data to stdout.
        acquisition.write_data();
		metaRows = metaRows + 1;

        // Print the histogram of the queue length to stderr.
        if( (params.outcnt == 0 && params.talkless) || (params.talkless==false) ) data.printQueueHistogram();

        // Check for interrupts.
        //if (checkInterrupt(InterruptState::FinishNow))
		if (got_sighup || got_sigint)
          break;
      }

      // if requested, avoid repeating all the printouts, just do that one time:
      if( (params.outcnt == 0 && params.talkless) ) params.outcnt++;

      if(params.session_duration_isSet) {
        // here we manage session duration in seconds:
        if (time(NULL) >= exit_time) {
    			do_exit = true;
          std::cerr << "Session duration elapsed." << std::endl;
        }
      }

	  if( !params.matrixMode )
      {
        // Mark the end of a measurement set with an additional empty line
        // (one was already output as a terminator for the last data set).
        std::cout << std::endl;
      }

      if(params.endless) do_exit = false;   // exactly ! will force to never exit

      if( !params.session_duration_isSet && !params.endless ) {
        do_exit = true; // not an endless, no session duration, so we finish after first run
      }

      // unless you hit ctrl-c :
      //if(checkInterrupt(InterruptState::FinishPass)) do_exit = true;
	  if (got_sighup || got_sigint) do_exit = true;

    } while ( !do_exit );

    if(params.matrixMode) {
      metafile.open(params.meta_file, std::ios::out | std::ios::trunc );
      metafile << actualBINS << " # frequency bins (columns)" << std::endl;
	  metaRows = metaRows / hops;	// in multi-hop scenarios every tune counts as an output row
      metafile << metaRows << " # scans (rows)" << std::endl;
      metafile << startFreq << " # startFreq (Hz)" << std::endl;
      metafile << endFreq << " # endFreq (Hz)" << std::endl;
      metafile << stepFreq << " # stepFreq (Hz)" << std::endl;
      metafile << (double)params.N * data.repeats_done / actual_samplerate << " # effective integration time secs" << std::endl;
      metafile << avgScanDur << " # avgScanDur (sec)" << std::endl;
      metafile << firstAcqTimestamp << " # firstAcqTimestamp UTC" << std::endl;
      metafile << lastAcqTimestamp << " # lastAcqTimestamp UTC" << std::endl;
      metafile << actual_samplerate << " # sampling rate Hz" << std::endl;
      metafile << hops << " # number of tuner hops" << std::endl;
      metafile << params.cropPercentage << " # crop percentage" << std::endl;
      metafile << excludedBINS * 2 << " # crop related excluded bins on each FFT" << std::endl;
      metafile << (-1) * cropFreqOffset << " # crop related frequency offset Hz" << std::endl;
      metafile.close();
    }

    if (plan.freqs_to_tune.size() == 0) {
      // No valid frequencies left in the list. This is certainly not OK.
      throw RPFexception("No valid frequencies left.", ReturnValue::AcquisitionError);
    }
  }
  catch (RPFexception &exception) {
    std::cerr << exception.what() << std::endl;
    final_retval = exception.returnValue();
  }

  return (int)final_retval;
}
