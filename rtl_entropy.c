/*
 * rtl-entropy, turns your Realtek RTL2832 based DVB dongle into a
 * high quality entropy source.
 *
 * Copyright (C) 2013 by Paul Warren <pwarren@pwarren.id.au>

 * Parts taken from:
 *  rtl_test. Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 *  http://openfortress.org/cryptodoc/random/noise-filter.c
 *    by Rick van Rein <rick@openfortress.nl>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifdef __APPLE__
#include <sys/time.h>
#else
#include <time.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#else
#include <Windows.h>
#include "getopt/getopt.h"
#endif

#include "rtl-sdr.h"

#define DEFAULT_SAMPLE_RATE		2048000
#define DEFAULT_ASYNC_BUF_NUMBER	32
#define DEFAULT_BUF_LENGTH		(16 * 16384)
#define MINIMAL_BUF_LENGTH		512
#define MAXIMAL_BUF_LENGTH		(256 * 16384)

#define MHZ(x)	((x)*1000*1000)

#define DEFAULT_FREQUENCY MHZ(70)

static int do_exit = 0;
static rtlsdr_dev_t *dev = NULL;

void usage(void)
{
  fprintf(stderr,
	  "rtl_test, a benchmark tool for RTL2832 based DVB-T receivers\n\n"
	  "Usage:\n"
	  "\t[-s samplerate (default: 2048000 Hz)]\n"
	  "\t[-d device_index (default: 0)]\n"
	  "\t[-f set frequency to listen (default: 54MHz )]\n"
	  //	  "\t[-g daemonise and add to system entropy pool (linux only)\n"
	  );
  
  exit(1);
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
  if (CTRL_C_EVENT == signum) {
    do_exit = 1;
    rtlsdr_cancel_async(dev);
    return TRUE;
  }
  return FALSE;
}
#else
static void sighandler(int signum)
{
  do_exit = 1;
  rtlsdr_cancel_async(dev);
}
#endif

int main(int argc, char **argv)
{
#ifndef _WIN32
  struct sigaction sigact;
#endif
  int n_read;
  int r, opt, i;
  uint8_t *buffer;
  uint32_t dev_index = 0;
  uint32_t samp_rate = DEFAULT_SAMPLE_RATE;
  uint32_t out_block_size = MAXIMAL_BUF_LENGTH;
  uint32_t frequency = DEFAULT_FREQUENCY;
  int device_count;
  int ch, ch2;
  uint8_t bitbuffer[250] = {0};
  int bitcounter = 0;
  int buffercounter = 0;
  int gains[100];
  int count;
  
  while ((opt = getopt(argc, argv, "d:s:f:")) != -1) {
    switch (opt) {
    case 'd':
      dev_index = atoi(optarg);
      break;
    case 's':
      samp_rate = (uint32_t)atof(optarg);
      break;
    case 'f':
      frequency = (uint32_t)atof(optarg);
      break;
    default:
      usage();
      break;
    }
  }
    
  buffer = malloc(out_block_size * sizeof(uint8_t));
  
  device_count = rtlsdr_get_device_count();
  if (!device_count) {
    fprintf(stderr, "No supported devices found.\n");
    exit(1);
  }
  
  fprintf(stderr, "Found %d device(s):\n", device_count);
  for (i = 0; i < device_count; i++)
    fprintf(stderr, "  %d:  %s\n", i, rtlsdr_get_device_name(i));
  fprintf(stderr, "\n");
  
  fprintf(stderr, "Using device %d: %s\n",
	  dev_index,
	  rtlsdr_get_device_name(dev_index));
  
  r = rtlsdr_open(&dev, dev_index);
  if (r < 0) {
    fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dev_index);
    exit(1);
  }
  
#ifndef _WIN32
  sigact.sa_handler = sighandler;
  sigemptyset(&sigact.sa_mask);
  sigact.sa_flags = 0;
  sigaction(SIGINT, &sigact, NULL);
  sigaction(SIGTERM, &sigact, NULL);
  sigaction(SIGQUIT, &sigact, NULL);
  sigaction(SIGPIPE, &sigact, NULL);
#else
  SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif
  
  /* Set the sample rate */
  r = rtlsdr_set_sample_rate(dev, samp_rate);
  if (r < 0)
    fprintf(stderr, "WARNING: Failed to set sample rate.\n");
  
  /* Reset endpoint before we start reading from it (mandatory) */
  r = rtlsdr_reset_buffer(dev);
  if (r < 0)
    fprintf(stderr, "WARNING: Failed to reset buffers.\n");
  
  /* Set start frequency */
  fprintf(stderr, "Setting Frequency to %d\n", frequency);
  r = rtlsdr_set_center_freq(dev, (uint32_t)frequency);
    
  /* set gain to max */
  count = rtlsdr_get_tuner_gains(dev, gains);
  fprintf(stderr, "Setting gain to %d\n", gains[count-1]);
  r = rtlsdr_set_tuner_gain_mode(dev, 1);
  if (r < 0)
    fprintf(stderr, "WARNING: Couldn't set gain mode to manual\n");
  
  r = rtlsdr_set_tuner_gain(dev, gains[count-1]);
  if (r < 0)
    fprintf(stderr, "WARNING: Failed to set gain\n");
  
  fprintf(stderr, "Reading samples in sync mode...\n");
  while (!do_exit) {
    r = rtlsdr_read_sync(dev, buffer, out_block_size, &n_read);
    
    if (r < 0) {
      fprintf(stderr, "ERROR: sync read failed.\n");
      break;
    }
    
    if ((uint32_t)n_read < out_block_size) {
      fprintf(stderr, "ERROR: Short read, samples lost, exiting!\n");
      break;
    }
    
    // for each byte in the read buffer
    // pick LSB and LSB+1 as they'll vary the most
    // debias and store in the write buffer till it's full
    for (i=0; i < n_read * sizeof(buffer[0]); i++) {
      ch = (buffer[i]) & 0x01;
      ch2 = (buffer[i] >> 1) & 0x01;
      if (ch != ch2) {
	if (ch) {
	  // store a 1 in our bitbuffer
	  bitbuffer[buffercounter] |= 1 << bitcounter;
	} else {
	  // store a 0, yay for bitwise C magic (aka "I've no idea how this works!")
	  bitbuffer[buffercounter] &= ~(1 << bitcounter);
	}
	bitcounter++;
      }
      // if our bitbuffer is full 
      if (bitcounter >= sizeof(bitbuffer[0]) * 8) { //bits per byte
	buffercounter++;
	bitcounter = 0;
      }
      if (buffercounter >= 250) {
	// We have 2000 bits of entropy
	// Can now send it to FIPS!
	// print it for now
	fwrite(&bitbuffer,sizeof(bitbuffer[0]),250,stdout);
	// reset it, and the counter
	memset(bitbuffer,0,sizeof(bitbuffer));
	buffercounter = 0;
      }
    }
  }
  if (do_exit) {
    fprintf(stderr, "\nUser cancel, exiting...\n");
  }
  else
    fprintf(stderr, "\nLibrary error %d, exiting...\n", r);
  
  rtlsdr_close(dev);
  free (buffer);
  return 0;
}


// Fragment to do the IOCTL on linux to /dev/random
// From https://github.com/rfinnie/twuewand/blob/master/src/rndaddentropy.c
/*
 int randfd;
  if((randfd = open("/dev/random", O_WRONLY)) < 0) {
    perror("/dev/random");
    return(1);
  }

  int count;
  while((count = fread(entropy.buf, 1, sizeof(entropy.buf), stdin)) > 0) {
    entropy.entropy_count = count * 8;
    entropy.buf_size = count;
    if(ioctl(randfd, RNDADDENTROPY, &entropy) < 0) {
      perror("RNDADDENTROPY");
      return(1);
    }
  }
*/