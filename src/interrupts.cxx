/*
* rtl_power_fftw, program for calculating power spectrum from rtl-sdr reciever.
* Copyright (C) 2015 Klemen Blokar <klemen.blokar@ad-vega.si>
*                    Andrej Lajovic <andrej.lajovic@ad-vega.si>
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
#include <signal.h>

#include "interrupts.h"


void handle_signal(int signal) {
    switch (signal) {
#ifdef _WIN32
		case SIGTERM:
		case SIGABRT:
		case SIGBREAK:
#else
		case SIGHUP:
#endif
		  got_sighup = true;
		  std::cerr << "\nInterrupted, finishing now.\n" << std::endl;
		  break;
		case SIGINT:
		  got_sigint = true;
		  std::cerr << "\nInterrupted, finishing now.\n" << std::endl;
		  break;
    }
}

void HookupHandler() {
    already_hooked_up = true;
#ifdef _WIN32
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGABRT, handle_signal);
#else
    struct sigaction sa;
    // Setup the handler
    sa.sa_handler = &handle_signal;
    // Restart the system call, if at all possible
    sa.sa_flags = SA_RESTART;
    // Block every signal during the handler
    sigfillset(&sa.sa_mask);
    // Intercept SIGHUP and SIGINT
    if (sigaction(SIGHUP, &sa, NULL) == -1) {
      std::cerr << "Cannot install SIGHUP handler." << std::endl;
    }
    if (sigaction(SIGINT, &sa, NULL) == -1) {
      std::cerr << "Cannot install SIGINT handler." << std::endl;
    }
#endif
}

void UnhookHandler() {
    if (already_hooked_up) {
#ifdef _WIN32
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGABRT, SIG_DFL);
#else
      struct sigaction sa;
      // Setup the sighub handler
      sa.sa_handler = SIG_DFL;
      // Restart the system call, if at all possible
      sa.sa_flags = SA_RESTART;
      // Block every signal during the handler
      sigfillset(&sa.sa_mask);
      // Intercept SIGHUP and SIGINT
      if (sigaction(SIGHUP, &sa, NULL) == -1) {
        std::cerr << "Cannot uninstall SIGHUP handler." << std::endl;
      }
      if (sigaction(SIGINT, &sa, NULL) == -1) {
        std::cerr << "Cannot uninstall SIGINT handler." << std::endl;
      }
#endif

      already_hooked_up = false;
    }
}
