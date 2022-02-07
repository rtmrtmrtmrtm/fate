#include <math.h>
#include <liquid/liquid.h>
#include <complex>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <stdio.h>
#include <mutex>
#include <map>
#include <list>
#include <thread>
#include <algorithm>
#include <string>
#include <math.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#include <time.h>

#include "defs.h"
#include "snd.h"
#include "util.h"
#include "pack.h"
#include "common.h"

int verbose = 1;
std::mutex dups_mu;
std::map<std::string, bool> dups;

void
call_entry(std::vector<double> samples, int nominal_start,
           int rate, cb_t cb)
{
  int hints[2] = { 0 };

  assert(nominal_start >= 0 && nominal_start < samples.size());

  // want 2.5 seconds before nominal start.
  int wanted = (5 * rate) / 2 - nominal_start;
  if(wanted > 0){
    std::vector<double> pad(wanted, 0.0);
    samples.insert(samples.begin(), pad.begin(), pad.end());
    nominal_start += pad.size();
  }

#if 0
        {
          static int seq = 0;
          char file[128];
          sprintf(file, "r%03d.wav", seq);
          seq++;
          display_status = std::string("writing ") + file;
          std::vector<double> xx = samples;
          xx.erase(xx.begin(), xx.begin() + nominal_start - (rate / 2));
          xx.resize(rate * 15);
          writewav(xx, file, rate);
        }
#endif

  entry(samples.data(), samples.size(), nominal_start, rate,
        300,
        2950,
        hints, hints, cb);
}

void
rx_loop(SoundIn *sin, cb_t cb)
{
  int rate = sin->rate();

  // last two seconds of previous cycle, to prepend to
  // next cycle, for early signals.
  std::vector<double> saved(2 * rate, 0.0);
  
  while(1){
    // sleep until 14 seconds into the next 15-second cycle.
    if(sin->is_file() || cycle_second() >= 14){
      // the "1" asks for the most recent 15 seconds of samples,
      // not the oldest buffered. it causes samples before the
      // most recent 15 seconds to be discarded.
      double ttt_start;
      std::vector<double> samples = sin->get(15 * rate, ttt_start, 1);
      
      // sample # of 0.5 seconds into the 15-second cycle.
      long long nominal_start;

      if(sin->is_file()){
        nominal_start = rate * 0.5;
      } else {
        // ttt_start is UNIX time of samples[0].
        double ttt_end = ttt_start + samples.size() / (double) rate;
        double cycle_start = ((long long) (ttt_end / 15)) * 15;
        nominal_start = samples.size() - rate * (ttt_end - cycle_start - 0.5);
      }

      if(nominal_start >= 0 && nominal_start + 10*rate < (int) samples.size()){

        dups_mu.lock();
        dups.erase(dups.begin(), dups.end());
        dups_mu.unlock();

        // make samples always the same size, to make
        // fftw plan caching more effective.
        // samples.resize(16 * rate, 0.0);
        
        samples.insert(samples.begin(), saved.begin(), saved.end());
        nominal_start += saved.size();
        
        call_entry(samples, nominal_start, rate, cb);

        saved.assign(samples.begin() + samples.size() - 2*rate, samples.end());
        assert(saved.size() == 2*rate);
      }
      
      // ensure we're into the next cycle.
      sleep(2);
    }
    usleep(100 * 1000); // 0.1 seconds
  }
}

int
benchmark_cb(int *a87, double hz0, double hz1, double off,
             const char *comment, double snr)
{
  std::string other_call_dummy;
  std::string txt = unpack(a87, other_call_dummy);

  dups_mu.lock();
  bool already = dups[txt];
  dups[txt] = true;
  dups_mu.unlock();

  if(already){
    return(1); // not a new decode
  }
  
  if(verbose){
    printf("%.0f %s\n", hz0, txt.c_str());
    fflush(stdout);
  }

  return 2; // indicate it's a good-looking new decode.
}


int
benchmark()
{
  int hints[2] = { 0 };
  const char *files[] = {
    "rrr/r000.wav",
    "rrr/r011.wav",
    "rrr/r013.wav",
    "rrr/r018.wav",
    "rrr/r023.wav",
    "rrr/r025.wav",
    "rrr/r027.wav",
    "rrr/r032.wav",
    "rrr/r034.wav",
    "rrr/r036.wav",
    "rrr/r038.wav",
    "rrr/r053.wav",
    "rrr/r055.wav",
    "rrr/r056.wav",
    "rrr/r057.wav",
    "rrr/r060.wav",
    "rrr/r061.wav",
    "rrr/r062.wav",
    "rrr/r064.wav",
    "rrr/r068.wav",
    "rrr/r069.wav",
    "rrr/r076.wav",
    "rrr/r079.wav",
    "rrr/r085.wav",
    "rrr/r090.wav",
    "rrr/r096.wav",
    "rrr/r101.wav",
    "rrr/r111.wav",
    "rrr/r121.wav",
    "rrr/r137.wav",
    "rrr/r140.wav",
    "rrr/r154.wav",
    "rrr/r175.wav",
    "rrr/r187.wav",
    "rrr/r204.wav",
    "rrr/r205.wav",
    "rrr/r210.wav",
    "rrr/r273.wav",
    "rrr/r280.wav",
    "rrr/r283.wav",
    "rrr/r286.wav",
    "rrr/r310.wav",
    "rrr/r326.wav",
    "rrr/r329.wav",
    "rrr/r340.wav",
    "rrr/r354.wav",
    "rrr/r391.wav",
    "rrr/r396.wav",
    "rrr/r397.wav",
    "rrr/r402.wav",
    "rrr/r408.wav",
    "rrr/r413.wav",
    "rrr/r421.wav",
    "rrr/r435.wav",
    "rrr/r445.wav",
    "rrr/r465.wav",
    "rrr/r469.wav",
    "rrr/r472.wav",
    "rrr/r484.wav",
    "rrr/r494.wav",
    "rrr/r518.wav",
    "rrr/r520.wav",
    "rrr/r540.wav",
    "rrr/r570.wav",
    "rrr/r582.wav",
    "rrr/r593.wav",
    "rrr/r602.wav",
    "rrr/r616.wav",
    "rrr/r648.wav",
    "rrr/r654.wav",
    0
  };

  int rate = 6000;
  int total = 0;
  
  for(int i = 0; files[i]; i++){
    if(verbose)
      printf("%s\n", files[i]);
    std::vector<double> big = readwav(files[i], rate);
    int nominal_start = rate / 2;
    dups_mu.lock();
    dups.erase(dups.begin(), dups.end());
    dups_mu.unlock();
    call_entry(big, nominal_start, rate, benchmark_cb);
    total += dups.size();
  }
  if(verbose)
    printf("%d\n", total);

  return total;
}

struct opt_var {
  const char *name;
  double vals[30];
};

struct opt_var vars [] =
  {
    { "second_hz_inc", { 0.4, 0.6, 0.8, 1, 1.2, 1.4, 1.8, 2.2, -9999 } },
    { "second_hz_win", { 1.5, 2, 2.4, 2.7, 3.0, 3.3, 4, -9999 } },
    { "second_off_inc", { 0.05, 0.1, 0.2, 0.3, 0.4, -9999 } },
    { "second_off_win", { 0.25, 0.5, 0.7, 0.9, -9999 } },
    { "third_hz_inc", { 0.05, 0.1, 0.15, 0.195, 2.5, 3, 4, -9999 } },
    { "third_hz_win", { 1, 2, 3, 4, 5, -9999 } },
    { "third_off_inc", { 1, 2, 3, 4, 5, -9999 } },
    { "third_off_win", { 4, 8, 12, 16, 20, 30, -9999 } },
    { "snr_win", { -1, 1, 2, 5, 8, -9999 } },
    { "snr_how", { 0, 1, 2, 3, 4, 5, 6, -9999 } },
    { "best_in_noise", { 0, 1, -9999 } },
    { "ldpc_iters", { 15, 20, 25, 30, 40, 60, -9999 } },
    { "shoulder", { 1, 5, 10, 15, 20, -9999 } },
    { "shoulder_extra", { 0, 1, 2, -9999 } },
    { "bandpass_block", { 0.5, 1, 2, -9999 } },
    { "bandpass_order", { 2, 3, 4, 5, 6, 8, 10, -9999 } },
    { "bandpass_type", { 0, 2, 3, -9999 } },
    { "bandpass_stop_db", { 40, 60, 80, 90, -9999 } },
    { "bandpass_pass_db", { 0.5, 1, 3, -9999 } },
    { "problt_how", { 0, 1, 2, 3, 4, -9999 } },
    { "use_apriori", { 0, 1, -9999 } },
    { "use_hints", { 0, 1, -9999 } },
    { "drift", { -1, 1, 2, 3, 4, -9999 } },
    { "win_type", { 1, 2, 3, 4, -9999 } },
    { "ncoarse", { 1, 2, 3, -9999 } },
    { "ncoarse_blocks", { 1, 2, 3, 4, -9999 } },
    { "tminus", { 1, 2, 3, -9999 } },
    { "tplus", { 1, 2, 3, 4, -9999 } },
    { "coarse_off_fracs", { 2, 3, 4, 5, 6, 8, -9999 } },
    { "coarse_hz_fracs", { 2, 3, 4, 5, 6, 8, -9999 } },
    { "already_hz", { 15, 20, 27, 35, 50, -9999 } },
    { "npasses", { 1, 2, 3, -9999 } },
    { "overlap", { 10, 20, 30, 40, -9999 } },
    { "sub_amp_win", { 0, 1, 2, 3, 4, 6, -9999 } },
    { "sub_phase_win", { 0, 1, 2, 3, -9999 } },
    { "nyquist", { 0.7, 0.8, 0.85, 0.9, 0.925, 0.95, -9999 } },
    { "reduce_shoulder", { -1, 1, 10, 20, 30, 50, 70, -9999 } },
    { "reduce_factor", { 0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.4, -9999 } },
    { "reduce_extra", { -50, -30, -25, -20, -10, 0, 10, 25, 50, -9999 } },
    { "reduce_how", { 0, 1, 2, 3, 4, -9999 } },
    { "pass0_frac", { 0.8, 1.0, 1.2, 1.5, -9999 } },
    { "fancy_subtract", { 0, 1, -9999 } },
    { "go_extra", { -6, -3, 0, 3, 6, 12, 20, -9999 } },
    { "do_reduce", { 0, 1, -9999 } },
    { "nthreads", { 1, 2, 3, 4, -9999 } },
    { "budget", { 0.5, 1.5, 2.2, 5.0, -9999 } },
    { 0, { -9999 } },
  };

void
optimize()
{
  extern double set(std::string param, std::string val);

  verbose = 0;
  for(int i = 0; vars[i].name; i++){
    double original = set(vars[i].name, "");

    for(int j = 0; vars[i].vals[j] > -9998; j++){
      printf("%s %.4f : ", vars[i].name, vars[i].vals[j]);
      fflush(stdout);

      set(vars[i].name, std::to_string(vars[i].vals[j]));

      int score = benchmark();

      printf("%d\n", score);
    }

    set(vars[i].name, std::to_string(original));
  }
}

