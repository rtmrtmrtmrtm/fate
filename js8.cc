
//
// A JS8 decoder in C++.
//
// Robert Morris, AB1HL
//

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <complex>
#include <fftw3.h>
#include <vector>
#include <algorithm>
#include <complex>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <random>
#include "defs.h"
#include "fft.h"
#include "util.h"

// JS8:
// 1920-point FFT at 12000 samples/second
//   yields 6.25 Hz spacing, 0.16 seconds/symbol
// encode chain:
//   72 bits of data
//   append 3 zero bits, for 75 bits
//   append 12 bits CRC (for 87 bits) -- but exclusive-or with 42
//   LDPC(174,87) yields 174 bits
//   that's 58 3-bit FSK-8 symbols
//   insert three 7-symbol Costas sync arrays
//     at symbol #s 0, 36, 72 of final signal
//     normal: 4, 2, 5, 6, 1, 3, 0
//   thus: 79 FSK-8 symbols
// total transmission time is 12.64 seconds

// tunable parameters
double budget = 2.2;
int snr_win = 5;
int snr_how = 0;
int soft_ranges = 1;
int best_in_noise = 1;
int ldpc_iters = 25;
double shoulder = 10; // for bandpass filter
double shoulder_extra = 0.0; // for bandpass filter
double bandpass_block = 1.0; // units are symbol times
int bandpass_order = 4;
int bandpass_type = 0; // 0=BUTTER, 2=CHEBY2, 3=ELLIP
double bandpass_stop_db = 60;
double bandpass_pass_db = 1; // passband ripple
double second_hz_inc = 1.2; // second search_both()
double second_hz_win = 2.7;
double second_off_inc = 0.2;
double second_off_win = 0.7; // search window in symbol-times
double third_hz_inc = 6.25 / 32;
double third_hz_win = 6.25 / 2;
int third_off_inc = 1;
int third_off_win = 16;
double log_tail = 0.1;
double log_rate = 8.0;
int problt_how = 3;
int use_apriori = 0;
int use_hints = 0; // 1 means use all hints, 2 means just CQ hints
double drift =  -1; // 2.0;
int win_type = 1;
int osd_depth = -1; // don't increase beyond 6, produces too much garbage
int osd_ldpc_thresh = 70; // demand this many correct LDPC parity bits for OSD
int ncoarse = 1; // number of offsets per hz produced by coarse()
int ncoarse_blocks = 1;
double tminus = 2.2; // start looking at 0.5 - tminus seconds
double tplus = 2.7;
int coarse_off_fracs = 4;
int coarse_hz_fracs = 4;
double already_hz = 27;
int nthreads = 2;
int npasses = 2;
double overlap = 20;
int sub_amp_win = 1;
double nyquist = 0.925;
int oddrate = 1;
double reduce_shoulder = -1;
double reduce_factor = 0.25;
double reduce_extra = 0;
int reduce_how = 2;
double pass0_frac = 1.0;
int fancy_subtract = 0;
int sub_phase_win = 0;
double go_extra = 3.5;
int do_reduce = 1;

typedef std::vector< std::vector< std::complex<double> > > ffts_t;

//
// return a Hamming window of length n.
//
std::vector<double>
hamming(int n)
{
  std::vector<double> h(n);
  for(int k = 0; k < n; k++){
    h[k] = 0.54 - 0.46 * cos(2 * M_PI * k / (n - 1.0));
  }
  return h;
}

std::vector<double>
blackman(int n)
{
  std::vector<double> h(n);
  for(int k = 0; k < n; k++){
    h[k] = 0.42 - 0.5 * cos(2 * M_PI * k / n) + 0.08*cos(4 * M_PI * k / n);
  }
  return h;
}

// symmetric blackman
std::vector<double>
sym_blackman(int n)
{
  std::vector<double> h(n);
  for(int k = 0; k < (n/2)+1; k++){
    h[k] = 0.42 - 0.5 * cos(2 * M_PI * k / n) + 0.08*cos(4 * M_PI * k / n);
  }
  for(int k = n-1; k >= (n/2)+1; --k){
    h[k] = h[(n-1)-k];
  }
  return h;
}

std::vector<double>
blackmanharris(int n)
{
  double a0 = 0.35875;
  double a1 = 0.48829;
  double a2 = 0.14128;
  double a3 = 0.01168;
  std::vector<double> h(n);
  for(int k = 0; k < n; k++){
    // symmetric
    h[k] =
      a0 
      - a1 * cos(2 * M_PI * k / (n-1))
      + a2 * cos(4 * M_PI * k / (n-1))
      - a3 * cos(6 * M_PI * k / (n-1));
    // periodic
    //h[k] =
    //  a0 
    //  - a1 * cos(2 * M_PI * k / n)
    //  + a2 * cos(4 * M_PI * k / n)
    //  - a3 * cos(6 * M_PI * k / n);
  }
  return h;
}

int
check_crc(const int a87[87])
{
  int aa[87];
  int non_zero = 0;
  for(int i = 0; i < 87; i++){
    if(i < 75){
      aa[i] = a87[i];
    } else {
      aa[i] = 0;
    }
    if(aa[i] != 0)
      non_zero++;
  }
  int out1[12];

  // don't bother with all-zero messages.
  if(non_zero == 0){
    return 0;
  }
  
  // why 76 and not 75 or 72?
  ft8_crc(aa, 76, out1);

  for(int i = 0; i < 12; i++){
    if(out1[i] != a87[87-12+i]){
      //printf("crc failed\n");
      return 0;
    }
  }
  //printf("crc OK\n");
  return 1;
}

class Stats {
public:
  std::vector<double> a_;
  bool finalized_;
  double mean_; // cached
  double stddev_; // cached
  
public:
  Stats() : finalized_(false) { }

  void add(double x) {
    a_.push_back(x);
    finalized_ = false;
  }

  void finalize() {
    finalized_ = true;
    
    int n = a_.size();
    mean_ = 0.0;
    for(int i = 0; i < n; i++){
      mean_ += a_[i];
    }
    mean_ /= n;

    double var = 0;
    for(int i = 0; i < n; i++){
      double y = a_[i] - mean_;
      var += y * y;
    }
    var /= n;
    stddev_ = sqrt(var);

    // prepare for binary search to find where values lie
    // in the distribution.
    std::sort(a_.begin(), a_.end());
  }

  double mean() {
    if(!finalized_)
      finalize();
    return mean_;
  }

  double stddev() {
    if(!finalized_)
      finalize();
    return stddev_;
  }

  // fraction of distribution that's less than x.
  // assumes normal distribution.
  double gaussian_problt(double x) {
    double SDs = (x - mean()) / stddev();
    double frac = 0.5 * (1.0 + erf(SDs / sqrt(2.0)));
    return frac;
  }

  // look into the actual distribution.
  double problt(double x, int how) {
    if(!finalized_)
      finalize();

    if(how == 0){
      return gaussian_problt(x);
    }

    // binary search.
    auto it = std::lower_bound(a_.begin(), a_.end(), x);
    int i = it - a_.begin();
    int n = a_.size();

    if(how == 1){
      // index into the distribution.
      // works poorly for values that are off the ends
      // of the distribution, since those are all
      // mapped to 0.0 or 1.0, regardless of magnitude.
      return i / (double) n;
    }

    if(how == 2){
      // use a kind of logistic regression for
      // values near the edges of the distribution.
      if(i < log_tail * n){
        double x0 = a_[(int)(log_tail * n)];
        double y = 1.0 / (1.0 + exp(-log_rate*(x-x0)));
        // y is 0..0.5
        y /= 5;
        return y;
      } else if(i > (1-log_tail) * n){
        double x0 = a_[(int)((1-log_tail) * n)];
        double y = 1.0 / (1.0 + exp(-log_rate*(x-x0)));
        // y is 0.5..1
        // we want (1-log_tail)..1
        y -= 0.5;
        y *= 2;
        y *= log_tail;
        y += (1-log_tail);
        return y;
      } else {
        return i / (double) n;
      }
    }

    if(how == 3){
      // gaussian for values near the edge of the distribution.
      if(i < log_tail * n){
        return gaussian_problt(x);
      } else if(i > (1-log_tail) * n){
        return gaussian_problt(x);
      } else {
        return i / (double) n;
      }
    }

    if(how == 4){
      // gaussian for values outside the distribution.
      if(x < a_[0] || x > a_.back()){
        return gaussian_problt(x);
      } else {
        return i / (double) n;
      }
    }

    assert(0);
  }
};

// a-priori probability of each of the 174 LDPC codeword
// bits being one. measured from reconstructed correct
// codewords, into ft8bits, then python bprob.py.
// from ft8-n4
double apriori174[] = {
  0.47, 0.32, 0.29, 0.37, 0.52, 0.36, 0.40, 0.42, 0.42, 0.53, 0.44,
  0.44, 0.39, 0.46, 0.39, 0.38, 0.42, 0.43, 0.45, 0.51, 0.42, 0.48,
  0.31, 0.45, 0.47, 0.53, 0.59, 0.41, 0.03, 0.50, 0.30, 0.26, 0.40,
  0.65, 0.34, 0.49, 0.46, 0.49, 0.69, 0.40, 0.45, 0.45, 0.60, 0.46,
  0.43, 0.49, 0.56, 0.45, 0.55, 0.51, 0.46, 0.37, 0.55, 0.52, 0.56,
  0.55, 0.50, 0.01, 0.19, 0.70, 0.88, 0.75, 0.75, 0.74, 0.73, 0.18,
  0.71, 0.35, 0.60, 0.58, 0.36, 0.60, 0.38, 0.50, 0.02, 0.01, 0.98,
  0.48, 0.49, 0.54, 0.50, 0.49, 0.53, 0.50, 0.49, 0.49, 0.51, 0.51,
  0.51, 0.47, 0.50, 0.53, 0.51, 0.46, 0.51, 0.51, 0.48, 0.51, 0.52,
  0.50, 0.52, 0.51, 0.50, 0.49, 0.53, 0.52, 0.50, 0.46, 0.47, 0.48,
  0.52, 0.50, 0.49, 0.51, 0.49, 0.49, 0.50, 0.50, 0.50, 0.50, 0.51,
  0.50, 0.49, 0.49, 0.55, 0.49, 0.51, 0.48, 0.55, 0.49, 0.48, 0.50,
  0.51, 0.50, 0.51, 0.50, 0.51, 0.53, 0.49, 0.54, 0.50, 0.48, 0.49,
  0.46, 0.51, 0.51, 0.52, 0.49, 0.51, 0.49, 0.51, 0.50, 0.49, 0.50,
  0.50, 0.47, 0.49, 0.52, 0.49, 0.51, 0.49, 0.48, 0.52, 0.48, 0.49,
  0.47, 0.50, 0.48, 0.50, 0.49, 0.51, 0.51, 0.51, 0.49,
};

// given 174 bits corrected by LDPC, work
// backwards to the symbols that must have
// been sent.
std::vector<int>
recode(int a174[])
{
  int i174 = 0;
  int costas[] = { 4, 2, 5, 6, 1, 3, 0 };
  std::vector<int> out79;
  for(int i79 = 0; i79 < 79; i79++){
    if(i79 < 7){
      out79.push_back(costas[i79]);
    } else if(i79 >= 36 && i79 < 36+7){
      out79.push_back(costas[i79-36]);
    } else if(i79 >= 72){
      out79.push_back(costas[i79-72]);
    } else {
      int sym = (a174[i174+0] << 2) | (a174[i174+1] << 1) | (a174[i174+2] << 0);
      i174 += 3;
      out79.push_back(sym);
    }
  }
  assert(out79.size() == 79);
  assert(i174 == 174);
  return out79;
}

class Strength {
public:
  double hz_;
  int off_;
  double strength_; // higher is better
};

class FT8 {
public:
  std::thread *th_;

  double min_hz_;
  double max_hz_;
  std::vector<double> samples_;  // input to each pass
  std::vector<double> nsamples_; // subtract from here

  int start_; // sample number of 0.5 seconds into samples[]
  int rate_;  // samples/second
  double deadline_; // start time + budget
  double final_deadline_; // keep going this long if no decodes
  std::vector<int> hints1_;
  std::vector<int> hints2_;
  int pass_;
  double down_hz_;

  static std::mutex cb_mu_;
  cb_t cb_; // call-back into Python

  std::mutex hack_mu_;
  int hack_size_;
  int hack_off_;
  int hack_len_;
  double hack_0_;
  double hack_1_;
  const double *hack_data_;
  std::vector<std::complex<double>> hack_bins_;

  Plan *plan32_;

  FT8(const std::vector<double> &samples,
      double min_hz,
      double max_hz,
      int start, int rate,
      int hints1[], int hints2[], double deadline,
      double final_deadline, cb_t cb)
  {
    samples_ = samples;
    min_hz_ = min_hz;
    max_hz_ = max_hz;
    
    start_ = start;
    rate_ = rate;
    deadline_ = deadline;
    final_deadline_ = final_deadline;
    cb_ = cb;
    down_hz_ = 0;

    for(int i = 0; hints1[i]; i++){
      hints1_.push_back(hints1[i]);
    }
    for(int i = 0; hints2[i]; i++){
      hints2_.push_back(hints2[i]);
    }

    hack_size_ = -1;
    hack_data_ = 0;
    hack_off_ = -1;
    hack_len_ = -1;

    plan32_ = 0;

    setup();
  }

  // for testing.
  FT8() {
    setup();
  }

  void setup() {
  }

  ~FT8() {
  }

// strength of costas block of signal with tone 0 at bi0,
// and symbol zero at si0.
double
one_coarse_strength(const ffts_t &bins, int bi0, int si0)
{
  int costas[] = { 4, 2, 5, 6, 1, 3, 0 };

  assert(si0 >= 0 && si0+72+8 <= bins.size());
  assert(bi0 >= 0 && bi0 + 8 <= bins[0].size());

  double signal = 0.0;
  double noise = 0.0;

  for(int si = 0; si < 7; si++){
    for(int bi = 0; bi < 8; bi++){
      double x = 0;
      x += std::abs(bins[si0+si][bi0+bi]);
      x += std::abs(bins[si0+36+si][bi0+bi]);
      x += std::abs(bins[si0+72+si][bi0+bi]);
      if(bi == costas[si]){
        signal += x;
      } else {
        noise += x;
      }
    }
  }

  if(noise == 0.0){
    return 1.0;
  } else {
    return signal / noise;
  }
}

std::vector<Strength>
coarse(const ffts_t &bins, int si0, int si1)
{
  int block = 1920 / (12000 / rate_); // samples per symbol
  int nbins = bins[0].size();
  double bin_hz = rate_ / (double) block;
  int min_bin = min_hz_ / bin_hz;
  int max_bin = max_hz_ / bin_hz;

  std::vector<Strength> strengths;
  
  for(int bi = min_bin; bi < max_bin && bi+8 <= nbins; bi++){
    std::vector<Strength> sv;
    for(int si = si0; si < si1 && si + 79 < bins.size(); si++){
      double s = one_coarse_strength(bins, bi, si);
      Strength st;
      st.strength_ = s;
      st.hz_ = bi * 6.25;
      st.off_ = si * block;
      sv.push_back(st);
    }
    if(sv.size() < 1)
      break;

    // save best ncoarse offsets, but require that they be separated
    // by at least one symbol time.

    std::sort(sv.begin(), sv.end(),
              [](const Strength &a, const Strength &b) -> bool
              { return a.strength_ > b.strength_; } );

    strengths.push_back(sv[0]);

    int nn = 1;
    for(int i = 1; nn < ncoarse && i < sv.size(); i++){
      if(std::abs(sv[i].off_ - sv[0].off_) > ncoarse_blocks*block){
        strengths.push_back(sv[i]);
        nn++;
      }
    }
  }

  return strengths;
}

//
// change rate.
// interpolates brate doesn't divide arate.
// caller must have low-passed filtered.
//
std::vector<double>
resample(const std::vector<double> &a, int arate, int brate)
{
  assert(brate <= arate);
  if(arate == brate)
    return a;

  int alen = a.size();
  double ratio = brate / (double) arate;
  int blen = round(alen * ratio);
  std::vector<double> b(blen);

  if((arate % brate) == 0){
    int inc = arate / brate;
    for(int i = 0; i < blen; i++){
      if(i * inc < alen){
        b[i] = a[i * inc];
      } else {
        b[i] = 0;
      }
    }
  } else {
    for(int i = 0; i < blen; i++){
      double jj = i / ratio;
      int j0 = jj;
      int j1 = j0 + 1;
      if(j1 >= alen){
        b[i] = 0;
      } else {
        double y = 0;
        y += a[j0] * (1.0 - (jj - j0));
        y += a[j1] * (jj - j0);
        b[i] = y;
      }
    }
  }
  return b;
}

//
// bandpass filter some FFT bins.
// smooth transition from stop-band to pass-band,
// so that it's not a brick-wall filter, so that it
// doesn't ring.
//
std::vector<std::complex<double>>
fbandpass(const std::vector<std::complex<double>> &bins0,
          double bin_hz,
          double low_outer,  // start of transition
          double low_inner,  // start of flat area
          double high_inner, // end of flat area
          double high_outer) // end of transition
{
  // assert(low_outer >= 0);
  assert(low_outer <= low_inner);
  assert(low_inner <= high_inner);
  assert(high_inner <= high_outer);
  // assert(high_outer <= bin_hz * bins0.size());

  int nbins = bins0.size();
  std::vector<std::complex<double>> bins1(nbins);

  for(int i = 0; i < nbins; i++){
    double ihz = i * bin_hz;
    // cos(x)+flat+cos(x) taper
    double factor;
    if(ihz <= low_outer || ihz >= high_outer){
      factor = 0;
    } else if(ihz >= low_outer && ihz < low_inner){
      // rising shoulder
#if 1
      factor = (ihz - low_outer) / (low_inner-low_outer); // 0 .. 1
#else
      double theta = (ihz - low_outer) / (low_inner-low_outer); // 0 .. 1
      theta -= 1; // -1 .. 0
      theta *= 3.14159; // -pi .. 0
      factor = cos(theta); // -1 .. 1
      factor = (factor + 1) / 2; // 0 .. 1
#endif
    } else if(ihz > high_inner && ihz <= high_outer){
      // falling shoulder
#if 1
      factor =  (high_outer - ihz) / (high_outer-high_inner); // 1 .. 0
#else
      double theta =  (high_outer - ihz) / (high_outer-high_inner); // 1 .. 0
      theta = 1.0 - theta; // 0 .. 1
      theta *= 3.14159; // 0 .. pi
      factor = cos(theta); // 1 .. -1
      factor = (factor + 1) / 2; // 1 .. 0
#endif
    } else {
      factor = 1.0;
    }
    bins1[i] = bins0[i] * factor;
  }

  return bins1;
}

//
// reduce the sample rate from arate to brate.
// center hz0..hz1 in the new nyquist range.
// but first filter to that range.
// sets delta_hz to hz moved down.
//
std::vector<double>
reduce_rate(const std::vector<double> &a, double hz0, double hz1,
            int arate, int brate,
            double &delta_hz)
{
  assert(brate < arate);
  assert(hz1 - hz0 <= brate / 2);

  // the pass band is hz0..hz1
  // stop bands are 0..hz00 and hz11..nyquist.
  double hz00, hz11;

  hz0 = std::max(0.0, hz0 - reduce_extra);
  hz1 = std::min(arate / 2.0, hz1 + reduce_extra);

  if(reduce_shoulder > 0){
    hz00 = hz0 - reduce_shoulder;
    hz11 = hz1 + reduce_shoulder;
  } else {
    double mid = (hz0 + hz1) / 2;
    hz00 = mid - (brate * reduce_factor);
    hz00 = std::min(hz00, hz0);
    hz11 = mid + (brate * reduce_factor);
    hz11 = std::max(hz11, hz1);
  }

  int alen = a.size();
  std::vector<std::complex<double>> bins1 = one_fft(a, 0, alen,
                                                    "reduce_rate1", 0);
  int nbins1 = bins1.size();
  double bin_hz = arate / (double) alen;

  if(reduce_how == 2){
    // band-pass filter the FFT output.
    bins1 = fbandpass(bins1, bin_hz,
                      hz00,
                      hz0,
                      hz1,
                      hz11);
  }
  
  if(reduce_how == 3){
    for(int i = 0; i < nbins1; i++){
      if(i < (hz0 / bin_hz)){
        bins1[i] = 0;
      } else if(i > (hz1 / bin_hz)){
        bins1[i] = 0;
      }
    }
  }

  // shift down.
  int omid = ((hz0 + hz1) / 2) / bin_hz;
  int nmid = (brate / 4.0) / bin_hz;

  int delta = omid - nmid; // amount to move down
  assert(delta < nbins1);
  int blen = round(alen * (brate / (double) arate));
  std::vector<std::complex<double>> bbins(blen / 2 + 1);
  for(int i = 0; i < (int) bbins.size(); i++){
    if(delta > 0){
      if(i + delta < bins1.size()){
        bbins[i] = bins1[i + delta];
      } else {
        bbins[i] = 0;
      }
    } else {
      bbins[i] = bins1[i];
    }
  }

  // use ifft to reduce the rate.
  std::vector<double> vvv = one_ifft(bbins, "reduce_rate2");

  delta_hz = delta * bin_hz;

  return vvv;
}


void
go()
{
  // cache to avoid cost of fftw planner mutex.
  plan32_ = get_plan(32, "cache32");

  // trim to make samples_ a good size for FFTW.
  // http://www.fftw.org/fftw2_doc/fftw_3.html
  int nice_sizes[] = {
    47250, 48020, 49000, 49392, 50625, 51450, 52500, 52920, 54000, 55125, 55566,
    55566, 56700, 57624, 58800, 60025, 61250, 61740, 63000,
    96040, 98000, 99225, 101250, 102900, 105000, 105840, 108045,
    110250, 111132, 113400, 115248, 118125, 120050,
    185220, 189000, 192080, 194481, 198450, 202500, 205800, 210000, 214375, 216090,
    220500, 222264, 226800, 231525,
    0 };
  int nice = -1;
  for(int i = 0; nice_sizes[i]; i++){
    int sz = nice_sizes[i];
    if(fabs(samples_.size() - sz) < 0.05 * samples_.size()){
      nice = sz;
      break;
    }
  }
  if(nice != -1){
    samples_.resize(nice);
  }

  assert(min_hz_ >= 0 && max_hz_ + 50 <= rate_/2);

#if 1
  // can we reduce the sample rate?
  double hzrange = (max_hz_ - min_hz_) + 50;
  int rates[] = { 1000, 1500, 2000, 3000, 4000, 6000, -1 };
  int nrate = -1;
  for(int ratei = 0; rates[ratei] > 0; ratei++){
    int xrate = rates[ratei];
    if(oddrate || (rate_ % xrate) == 0){
      if(hzrange < nyquist * (xrate / 2)){
        nrate = xrate;
        break;
      }
    }
  }
#else
  // can we reduce the sample rate?
  int nrate = -1;
  for(int xrate = 100; xrate < rate_; xrate += 100){
    if(xrate < rate_ && (oddrate || (rate_ % xrate) == 0)){
      if(((max_hz_ - min_hz_) + 50 + 2*go_extra) < nyquist * (xrate / 2)){
        nrate = xrate;
        break;
      }
    }
  }
#endif

  if(do_reduce && nrate > 0 && nrate < rate_ * 0.75){
    // filter and reduce the sample rate from rate_ to nrate.

    double t0 = now();
    int osize = samples_.size();

#if 0
    { char file[64];
      sprintf(file, "x%d.wav", (int)min_hz_);
      writewav(samples_, file, rate_);
      printf("writing %s\n", file); }
#endif
    
    double delta_hz; // how much it moved down
    samples_ = reduce_rate(samples_,
                           min_hz_-3.1-go_extra,
                           max_hz_+50-3.1+go_extra,
                           rate_, nrate, delta_hz);

#if 0
    { char file[64];
      sprintf(file, "y%d.wav", (int)min_hz_);
      writewav(samples_, file, nrate);
      printf("writing %s\n", file);
      sleep(1); exit(0);
    }
#endif

    double t1 = now();
    if(t1 - t0 > 0.15){
      fprintf(stderr, "reduce oops, size %d -> %d, rate %d -> %d, took %.2f\n",
              osize,
              (int) samples_.size(),
              rate_,
              nrate,
              t1 - t0);
    }
    if(0){
      fprintf(stderr, "%.0f..%.0f, range %.0f, rate %d -> %d, delta hz %.0f, %.6f sec\n",
              min_hz_, max_hz_,
              max_hz_ - min_hz_,
              rate_, nrate, delta_hz, t1 - t0);
    }

    if(delta_hz > 0){
      down_hz_ = delta_hz; // to adjust hz for Python.
      min_hz_ -= down_hz_;
      max_hz_ -= down_hz_;
    }
    assert(max_hz_ + 50 < nrate / 2);
    assert(min_hz_ >= 0);

    double ratio = nrate / (double) rate_;
    rate_ = nrate;
    start_ = round(start_ * ratio);
  }

  int block = blocksize(rate_);

  // start_ is the sample number of 0.5 seconds, the nominal start time.

  // make sure there's at least tplus*rate_ samples after the end.
  if(start_ + tplus*rate_ + 79 * block + block > samples_.size()){
    int need = start_ + tplus*rate_ + 79 * block - samples_.size();

    // round up to a whole second, to ease fft plan caching.
    if((need % rate_) != 0)
      need += rate_ - (need % rate_);

    std::default_random_engine generator;
    std::uniform_int_distribution<int> distribution(0, samples_.size()-1);
    auto rnd = std::bind(distribution, generator);

    std::vector<double> v(need);
    for(int i = 0; i < need; i++){
      //v[i] = 0;
      v[i] = samples_[rnd()];
    }
    samples_.insert(samples_.end(), v.begin(), v.end());
  }
  
  int si0 = (start_ - tminus*rate_) / block;
  if(si0 < 0)
    si0 = 0;
  int si1 = (start_ + tplus*rate_) / block;

  // a copy from which to subtract.
  nsamples_ = samples_;

  for(pass_ = 0; pass_ < npasses; pass_++){
    double total_remaining = deadline_ - now();
    double remaining = total_remaining / (npasses - pass_);
    if(pass_ == 0){
      remaining *= pass0_frac;
    }
    double deadline = now() + remaining;
    
    int new_decodes = 0;
    samples_ = nsamples_;

    std::vector<Strength> order;

    //
    // search coarsely for Costas blocks.
    // in fractions of bins in off and hz.
    //

    for(int hz_frac_i = 0; hz_frac_i < coarse_hz_fracs; hz_frac_i++){
      // shift down by hz_frac
      double hz_frac = hz_frac_i * (6.25 / coarse_hz_fracs);
      std::vector<double> samples1;
      if(hz_frac_i == 0){
        samples1 = samples_;
      } else {
        samples1 = fft_shift(samples_, 0, samples_.size(),
                             rate_, hz_frac);
      }
      
      for(int off_frac_i = 0; off_frac_i < coarse_off_fracs; off_frac_i++){
        int off_frac = off_frac_i * (block / coarse_off_fracs);
        ffts_t bins = ffts(samples1, off_frac, block, 0);
        std::vector<Strength> oo = coarse(bins, si0, si1);
        for(int i = 0; i < oo.size(); i++){
          oo[i].hz_ += hz_frac;
          oo[i].off_ += off_frac;
        }
        order.insert(order.end(), oo.begin(), oo.end());
      }
    }

    //
    // sort strongest-first.
    //
    std::sort(order.begin(), order.end(),
              [](const Strength &a, const Strength &b) -> bool
              { return a.strength_ > b.strength_; } );
    
    char already[2000]; // XXX
    for(int i = 0; i < sizeof(already); i++)
      already[i] = 0;
    
    for(int ii = 0; ii < order.size(); ii++){
      double tt = now();
      if(ii > 0 &&
         tt > deadline &&
         (tt > deadline_ || new_decodes > 0) &&
         (pass_ < npasses-1 || tt > final_deadline_)){
        break;
      }

      double hz = order[ii].hz_;
      if(already[(int)round(hz / already_hz)])
        continue;
      int off = order[ii].off_;
      //printf("hz %.1f\n", hz); fflush(stdout);
      int ret = one(samples_, hz, off);
      if(ret){
        if(ret == 2){
          new_decodes++;
        }
        already[(int)round(hz / already_hz)] = 1;
      }
    }
  }
}

//
// what's the strength of the Costas sync blocks of
// the signal starting at hz and off?
//
double
one_strength(const std::vector<double> &samples200, double hz, int off)
{
  int bin0 = round(hz / 6.25);

  int costas[] = { 4, 2, 5, 6, 1, 3, 0 };

  double sum = 0;
  for(int si = 0; si < 7; si++){
    auto fft1 = one_fft(samples200, off+(si+0)*32, 32, 0, plan32_);
    auto fft2 = one_fft(samples200, off+(si+36)*32, 32, 0, plan32_);
    auto fft3 = one_fft(samples200, off+(si+72)*32, 32, 0, plan32_);
    for(int bi = 0; bi < 8; bi++){
      double x = 0;
      x += std::abs(fft1[bin0+bi]);
      x += std::abs(fft2[bin0+bi]);
      x += std::abs(fft3[bin0+bi]);
      if(bi == costas[si]){
        sum += x;
      } else {
        sum -= x / 7.0;
      }
    }
  }
  return sum;
}

double
one_strength_known(const std::vector<double> &samples200,
                   const std::vector<int> syms,
                   double hz, int off)
{
  assert(syms.size() == 79);
  
  int bin0 = round(hz / 6.25);

  double sum = 0;
  for(int si = 0; si < 79; si++){
    auto fft1 = one_fft(samples200, off+si*32, 32, 0, 0);
    for(int bi = 0; bi < 8; bi++){
      double x = std::abs(fft1[bin0+bi]);
      if(bi == syms[si]){
        sum += x;
      } else {
        sum -= x / 7.0;
      }
    }
  }
  return sum;
}

int
search_time_fine(const std::vector<double> &samples200,
                 int off0, int offN,
                 double hz,
                 int gran,
                 double &str)
{
  if(off0 < 0)
    off0 = 0;

  //
  // shift in frequency to put hz at 25.
  // only shift the samples we need, both for speed,
  // and try to always shift down the same number of samples
  // to make it easier to cache fftw plans.
  //
  int len = (offN - off0) + 79*32 + 32;
  if(off0 + len > samples200.size()){
    // len = samples200.size() - off0;
    // don't provoke random-length FFTs.
    return -1;
  }
  std::vector<double> downsamples200 = shift200(samples200, off0, len, hz);

  int best_off = -1;
  double best_sum = 0.0;

  for(int g = 0; g <= (offN-off0) && g + 79*32 <= len; g += gran){
    double sum = one_strength(downsamples200, 25, g);
    if(sum > best_sum || best_off == -1){
      best_off = g;
      best_sum = sum;
    }
  }

  str = best_sum;
  assert(best_off >= 0);
  return off0 + best_off;
}

int
search_time_fine_known(const std::vector<double> &samples200,
                       const std::vector<int> &syms,
                       int off0, int offN,
                       double hz,
                       int freq_factor, int time_factor,
                       double &str)
{
  if(off0 < 0)
    off0 = 0;
  
  // put hz at 25.
  std::vector<double> downsamples200 = shift200(samples200, 0, samples200.size(), hz);

  assert(time_factor > 0);
  int gran = 32 / time_factor;

  int best_off = -1;
  double best_sum = 0.0;
  int g = off0;
  if(g < 0)
    g = 0;

  for( ; g <= offN && g + 79*32 <= downsamples200.size(); g += gran){
    double sum = one_strength_known(downsamples200, syms, 25, g);
    if(sum > best_sum || best_off == -1){
      best_off = g;
      best_sum = sum;
    }
  }

  if(best_off < 0)
    return -1;

  str = best_sum;
  return best_off;
}

//
// search for costas blocks in an MxN time/frequency grid.
// hz0 +/- hz_win in hz_inc increments. hz0 should be near 25.
// off0 +/- off_win in off_inc incremenents.
//
int
search_both(const std::vector<double> &samples200,
            double hz0, double hz_inc, double hz_win,
            int off0, int off_inc, int off_win,
            double &hz_out, int &off_out)
{
  assert(hz0 >= 25 - 6.25/2 && hz0 <= 25 + 6.25/2);
  
  int got_best = 0;
  double best_hz = 0;
  int best_off = 0;
  double best_str = 0;

  for(double hz = hz0 - hz_win; hz <= hz0 + hz_win; hz += hz_inc){
    double str = 0;
    int off = search_time_fine(samples200, off0 - off_win, off0 + off_win, hz,
                               off_inc, str);
    if(off >= 0 && (got_best == 0 || str > best_str)){
      got_best = 1;
      best_hz = hz;
      best_off = off;
      best_str = str;
    }
  }

  if(got_best){
    hz_out = best_hz;
    off_out = best_off;
    return 1;
  } else {
    return 0;
  }
}

void
search_both_known(const std::vector<double> &samples200,
                  const std::vector<int> &syms,
                  double hz0, double hz_inc, double hz_win,
                  int off0, int off_inc, int off_win,
                  double &hz_out, int &off_out, double &strength_out)
{
  assert(hz0 >= 25 - 6.25 && hz0 <= 25 + 6.25);
  
  int got_best = 0;
  double best_hz = 0;
  int best_off = 0;
  double best_strength = 0;

  for(double hz = hz0 - hz_win; hz <= hz0 + hz_win; hz += hz_inc){
    double strength = 0;
    int time_factor = 32 / off_inc;
    int off = search_time_fine_known(samples200, syms,
                                     off0 - off_win, off0 + off_win, hz,
                                     0, time_factor, strength);
    if(off >= 0 && (got_best == 0 || strength > best_strength)){
      got_best = 1;
      best_hz = hz;
      best_off = off;
      best_strength = strength;
    }
  }

  if(got_best){
    hz_out = best_hz;
    off_out = best_off;
    strength_out = best_strength;
  }
}

// one giant FFT.
// so no problem with phase mismatch &c at block boundaries.
// surprisingly fast at 200 samples/second.
// shifts *down* by hz.
std::vector<double>
fft_shift(const std::vector<double> &samples, int off, int len,
          int rate, double hz)
{
#if 0
  std::vector<std::complex<double>> bins = one_fft(samples, off, len, 0, 0);
#else
  // horrible hack to avoid repeated FFTs on the same input.
  hack_mu_.lock();
  std::vector<std::complex<double>> bins;
  if(samples.size() == hack_size_ && samples.data() == hack_data_ &&
     off == hack_off_ && len == hack_len_ &&
     samples[0] == hack_0_ && samples[1] == hack_1_){
    bins = hack_bins_;
  } else {
    bins = one_fft(samples, off, len, 0, 0);
    hack_bins_ = bins;
    hack_size_ = samples.size();
    hack_off_ = off;
    hack_len_ = len;
    hack_0_ = samples[0];
    hack_1_ = samples[1];
    hack_data_ = samples.data();
  }
  hack_mu_.unlock();
#endif
  
  int nbins = bins.size();

  double bin_hz = rate / (double) len;
  int down = round(hz / bin_hz);
  std::vector<std::complex<double>> bins1(nbins);
  for(int i = 0; i < nbins; i++){
    int j = i + down;
    if(j >= 0 && j < nbins){
      bins1[i] = bins[j];
    } else {
      bins1[i] = 0;
    }
  }
  std::vector<double> out = one_ifft(bins1, 0);
  return out;
}

// shift the frequency by a fraction of 6.25,
// to center hz on bin 4 (25 hz).
std::vector<double>
shift200(const std::vector<double> &samples200, int off, int len, double hz)
{
  if(std::abs(hz - 25) < 0.001 && off == 0 && len == samples200.size()){
    return samples200;
  } else {
    return fft_shift(samples200, off, len, 200, hz - 25.0);
  }
  // return hilbert_shift(samples200, hz - 25.0, hz - 25.0, 200);
}

// returns a mini-FFT of 79 8-tone symbols.
ffts_t
extract(const std::vector<double> &samples200, double hz, int off, int factor)
{
  // put hz in the middle of bin 4, at 25.0.
  std::vector<double> downsamples200 = shift200(samples200, 0,
                                                samples200.size(), hz);

  ffts_t bins3 = ffts(downsamples200, off, 32, 0);

  ffts_t m79(79);
  for(int si = 0; si < 79; si++){
    m79[si].resize(8);
    if(si < (int)bins3.size()){
      for(int bi = 0; bi < 8; bi++){
        auto x = bins3[si][4+bi];
        m79[si][bi] = x;
      }
    } else {
      for(int bi = 0; bi < 8; bi++){
        m79[si][bi] = 0;
      }
    }
  }

  return m79;
}

//
// turn 79 symbol numbers into 174 bits.
// strip out the three Costas sync blocks,
// leaving 58 symbol numbers.
// each represents three bits.
//
std::vector<int>
extract_bits(const std::vector<int> &syms)
{
  assert(syms.size() == 79);

  std::vector<int> bits;
  for(int si = 0; si < 79; si++){
    if(si < 7 || (si >= 36 && si < 36+7) || si >= 72){
      // costas -- skip
    } else {
      bits.push_back((syms[si] & 4) != 0);
      bits.push_back((syms[si] & 2) != 0);
      bits.push_back((syms[si] & 1) != 0);
    }
  }

  return bits;
}

void
prepare_hard(const ffts_t &m79a, double ll174[])
{
  std::vector<int> syms(79);
  for(int si = 0; si < 79; si++){
    int best_tone = -1;
    double best_amp = 0;
    for(int bi = 0; bi < 8; bi++){
      double amp = std::abs(m79a[si][bi]);
      if(amp > best_amp || best_tone == -1){
        best_tone = bi;
        best_amp = amp;
      }
    }
    syms[si] = best_tone;
  }

  std::vector<int> bits = extract_bits(syms);
  assert(bits.size() == 174);

  // turn bits into log-likihood for ldpc_decode.
  // a zero bit is positive, a 1 bit is negative.
  for(int i = 0; i < 174; i++){
    if(bits[i]){
      ll174[i] = -4.99;
    } else {
      ll174[i] = 4.99;
    }
  }
}

//
// convert_to_snr(m79, snr_how, snr_win)
//
// hack to normalize levels by windowed median.
// this helps, but why?
//
//
std::vector< std::vector<double> >
convert_to_snr(const std::vector< std::vector<double> > &m79, int how, int win)
{
  if(how < 0 || win < 0)
    return m79;

  // for each symbol time, what's its "noise" level?
  //
  std::vector<double> mm(79);
  for(int si = 0; si < 79; si++){
    std::vector<double> v(8);
    double sum = 0.0;
    for(int bi = 0; bi < 8; bi++){
      double x = m79[si][bi];
      v[bi] = x;
      sum += x;
    }
    if(how != 1)
      std::sort(v.begin(), v.end());
    if(how == 0){
      // median
      mm[si] = (v[3] + v[4]) / 2;
    } else if(how == 1){
      mm[si] = sum / 8;
    } else if(how == 2){
      // all but strongest tone.
      mm[si] = (v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6]) / 7;
    } else if(how == 3){
      mm[si] = v[0]; // weakest tone
    } else if(how == 4){
      mm[si] = v[7]; // strongest tone
    } else if(how == 5){
      mm[si] = v[6]; // second-strongest tone
    } else {
      mm[si] = 1.0;
    }
  }

  // we're going to take a windowed average.
  std::vector<double> winwin;
  if(win > 0){
    winwin = blackman(2*win+1);
  } else {
    winwin.push_back(1.0);
  }

  std::vector<std::vector<double>> n79(79);
    
  for(int si = 0; si < 79; si++){
    double sum = 0;
    for(int dd = si - win; dd <= si + win; dd++){
      int wi = dd - (si - win);
      if(dd >= 0 && dd < 79){
        sum += mm[dd] * winwin[wi];
      } else if(dd < 0){
        sum += mm[0] * winwin[wi];
      } else {
        sum += mm[78] * winwin[wi];
      }
    }
    n79[si].resize(8);
    for(int bi = 0; bi < 8; bi++){
      n79[si][bi] = m79[si][bi] / sum;
    }
  }

  return n79;
}

// statistics to decide soft probabilities.
// distribution of strongest tones, and
// distribution of noise.
// multiple ranges in case things change over time.
// nranges is soft_ranges.
void
make_stats(const std::vector<std::vector<double>> &m79,
           std::vector<Stats> &noises,
           std::vector<Stats> &bests,
           int nranges,
           int x_best_in_noise)
{
  noises.resize(nranges);
  bests.resize(nranges);

  int costas[] = { 4, 2, 5, 6, 1, 3, 0 };

  for(int range = 0; range < nranges; range++){
    int si0 = range * (79 / nranges);
    int si1;
    if(range == nranges - 1)
      si1 = 79;
    else
      si1 = (range + 1) * (79 / nranges);
    for(int si = si0; si < si1 && si < 79; si++){
      if(si < 7 || (si >= 36 && si < 36 + 7) || si >= 72){
        // Costas.
        int ci;
        if(si >= 72) ci = si - 72;
        else if(si >= 36) ci = si - 36;
        else ci = si;
        for(int bi = 0; bi < 8; bi++){
          double x = m79[si][bi];
          if(bi == costas[ci]){
            bests[range].add(x);
            if(x_best_in_noise)
              noises[range].add(x); // include best in noises too
          } else {
            noises[range].add(x);
          }
        }
      } else {
        std::vector<double> v(8);
        for(int bi = 0; bi < 8; bi++){
          v[bi] = m79[si][bi];
        }
        std::sort(v.begin(), v.end());
        for(int i = 0; i < 7; i++){
          noises[range].add(v[i]);
        }
        bests[range].add(v[7]);
        if(x_best_in_noise)
          noises[range].add(v[7]); // include best in noises too
      }
    }
  }
}

//
// c79 is 79x8 complex tones.
//
void
prepare_soft(const ffts_t &c79, double ll174[], int best_off)
{
  // m79 = absolute values of c79.
  std::vector< std::vector<double> > m79(79);
  for(int si = 0; si < 79; si++){
    m79[si].resize(8);
    for(int bi = 0; bi < 8; bi++){
      m79[si][bi] = std::abs(c79[si][bi]);
    }
  }

  m79 = convert_to_snr(m79, snr_how, snr_win);
    
  // statistics to decide soft probabilities.
  // distribution of strongest tones, and
  // distribution of noise.
  // multiple ranges in case things change over time.
  std::vector<Stats> noises;
  std::vector<Stats> bests;
  make_stats(m79, noises, bests, soft_ranges, best_in_noise);

  int lli = 0;
  for(int i79 = 0; i79 < 79; i79++){
    if(i79 < 7 || (i79 >= 36 && i79 < 36+7) || i79 >= 72){
      // Costas, skip
      continue;
    }

    int range = i79 / (79 / soft_ranges);

    // for each of the three bits, look at the strongest tone
    // that would make it a zero, and the strongest tone that
    // would make it a one. use Bayes to decide which is more
    // likely, comparing each against the distribution of noise
    // and the distribution of strongest tones.
    // most-significant-bit first.

    for(int biti = 0; biti < 3; biti++){
      // tone numbers that make this bit zero or one.
      int zeroi[4];
      int onei[4];
      if(biti == 0){
        // high bit
        zeroi[0] = 0; zeroi[1] = 1; zeroi[2] = 2; zeroi[3] = 3;
        onei[0] = 4; onei[1] = 5; onei[2] = 6; onei[3] = 7;
      }
      if(biti == 1){
        // middle bit
        zeroi[0] = 0; zeroi[1] = 1; zeroi[2] = 4; zeroi[3] = 5;
        onei[0] = 2; onei[1] = 3; onei[2] = 6; onei[3] = 7;
      }
      if(biti == 2){
        // low bit
        zeroi[0] = 0; zeroi[1] = 2; zeroi[2] = 4; zeroi[3] = 6;
        onei[0] = 1; onei[1] = 3; onei[2] = 5; onei[3] = 7;
      }

      // strongest tone that would make this bit be zero.
      int got_best_zero = 0;
      double best_zero = 0;
      for(int i = 0; i < 4; i++){
        double x = m79[i79][zeroi[i]];
        if(got_best_zero == 0 || x > best_zero){
          got_best_zero = 1;
          best_zero = x;
        }
      }

      // strongest tone that would make this bit be one.
      int got_best_one = 0;
      double best_one = 0;
      for(int i = 0; i < 4; i++){
        double x = m79[i79][onei[i]];
        if(got_best_one == 0 || x > best_one){
          got_best_one = 1;
          best_one = x;
        }
      }

      //
      // Bayes combining rule normalization from:
      // http://cs.wellesley.edu/~anderson/writing/naive-bayes.pdf
      //
      // a = P(zero)P(e0|zero)P(e1|zero)
      // b = P(one)P(e0|one)P(e1|one)
      // p = a / (a + b)
      //
      
      double pzero = 0.5;
      double pone = 0.5;
      if(use_apriori){
        pzero = 1.0 - apriori174[lli];
        pone = apriori174[lli];
      }

      // zero
      double a = pzero *
        bests[range].problt(best_zero, problt_how) *
        (1.0 - noises[range].problt(best_one, problt_how));
      
      // one
      double b = pone *
        bests[range].problt(best_one, problt_how) *
        (1.0 - noises[range].problt(best_zero, problt_how));

      double p;
      if(a + b == 0){
        p = 0.5;
      } else {
        p = a / (a + b);
      }

      double maxlog = 4.97;

      double ll;
      if(1 - p == 0.0){
        ll = maxlog;
      } else {
        ll = log(p / (1 - p));
      }
      
      if(ll > maxlog)
        ll = maxlog;
      if(ll < -maxlog)
        ll = -maxlog;

      ll174[lli++] = ll;
    }
  }
  assert(lli == 174);
}

int
decode(const double ll174[], int a174[], int use_osd, std::string &comment)
{
  int plain[174]; // will be 0/1 bits.
  int ldpc_ok = 0;     // 87 will mean success.

  ldpc_decode((double*)ll174, ldpc_iters, plain, &ldpc_ok);

  int ok_thresh = 87; // 87 is perfect
  if(ldpc_ok >= ok_thresh){
    // plain[] is 87 parity bits, then 87 data bits.
    for(int i = 0; i < 174; i++){
      a174[i] = plain[i];
    }
    if(check_crc(a174+87)){
      // success!
      return 1;
    }
  }

  if(use_osd && osd_depth >= 0 && ldpc_ok >= osd_ldpc_thresh){

    int oplain[87];
    int got_depth = -1;
#if 0
    int osd_ok = osd_decode((double*)ll174, osd_depth, oplain, &got_depth);
    if(osd_ok){
      // reconstruct all 174.
      comment += "OSD-" + std::to_string(got_depth) + "-" + std::to_string(ldpc_ok);
      ldpc_encode(oplain, a174);
      return 1;
    }
#endif
  }
  
  return 0;
}

//
// like fft_shift(). one big FFT, move bins down and
// zero out those outside the band, then IFFT,
// then re-sample.
// moves hz down to 25.
//
// XXX maybe merge w/ fft_shift() / shift200().
//
std::vector<double>
down_v7(const std::vector<double> &samples, double hz)
{
  int len = samples.size();
  std::vector<std::complex<double>> bins = one_fft(samples, 0, len, 0, 0);
  int nbins = bins.size();

  double bin_hz = rate_ / (double) len;
  int down = round((hz - 25) / bin_hz);
  std::vector<std::complex<double>> bins1(nbins);
  for(int i = 0; i < nbins; i++){
    int j = i + down;
    if(j >= 0 && j < nbins){
      bins1[i] = bins[j];
    } else {
      bins1[i] = 0;
    }
  }

  // now filter to fit in 200 samples/second.

  double edge01 = 25.0 - shoulder_extra;
  double edge00 = edge01 - shoulder;
  if(edge00 < 0)
    edge00 = 0;
  double edge10 = 75 - 6.25 + shoulder_extra;
  double edge11 = edge10 + shoulder;
  if(edge11 > 100)
    edge11 = 100;

  for(int i = 0; i < nbins; i++){
    double ihz = i * bin_hz;
    if(shoulder < -1.5){
      // the full 100 hz for 200 samples/second, no cutoff or taper.
      // this works pretty well.
      if(ihz >= 100){
        bins1[i] = 0;
      }
    } else if(shoulder < 0){
      // sharp cutoff around tones, no taper
      // this works poorly if shoulder_extra=0.
      if(ihz < edge01 || ihz > edge10){
        bins1[i] = 0;
      }
    } else {
      // cos(x)+flat+cos(x) taper
      double factor;
      if(ihz <= edge00 || ihz >= edge11){
        factor = 0;
      } else if(ihz >= edge00 && ihz < edge01){
        // rising shoulder
        double theta = (ihz - edge00) / (edge01-edge00); // 0 .. 1
        theta -= 1; // -1 .. 0
        theta *= 3.14159; // -pi .. 0
        factor = cos(theta); // -1 .. 1
        factor = (factor + 1) / 2; // 0 .. 1
      } else if(ihz > edge10 && ihz <= edge11){
        // falling shoulder
        double theta =  (edge11 - ihz) / (edge11-edge10); // 1 .. 0
        theta = 1.0 - theta; // 0 .. 1
        theta *= 3.14159; // 0 .. pi
        factor = cos(theta); // 1 .. -1
        factor = (factor + 1) / 2; // 1 .. 0
      } else {
        factor = 1.0;
      }
      bins1[i] *= factor;
    }
  }

  // convert back to time domain
  std::vector<double> vv = one_ifft(bins1, 0);

  // re-sample to 200 samples/second.
  // no need to worry about aliasing, due to the bandpass.
  std::vector<double> out = resample(vv, rate_, 200);

  return out;
}

std::vector<double>
bandpass_fir(const std::vector<double> &samples, int rate, int symsamples, double hz0, double hz1)
{
  int block = bandpass_block * symsamples;
  
  // FIR taps for a hz0..hz1 bandpass filter, via inverse FFT.
  double bin_hz = rate / (double) block;
  int bin0 = round(hz0 / bin_hz);
  int bin1 = round(hz1 / bin_hz);
  int nbins = (block / 2) + 1;
  std::vector<std::complex<double>> bins(nbins);
  for(int i = 0; i < nbins; i++){
    bins[i] = 0;
  }
  for(int i = bin0; i < bin1; i++){
    if(i >= 0 && i < nbins){
      bins[i] = 1;
    }
  }
  std::vector<double> taps1 = one_ifft(bins, 0);
  int xntaps = taps1.size();

  // unwrap IFFT output.
  std::vector<double> xtaps(xntaps);
  for(int i = 0; i < xntaps; i++){
    int ii = (i+(xntaps/2)) % xntaps;
    xtaps[i] = taps1[ii];
  }

  // drop the first tap.
  int ntaps = xntaps - 1;
  std::vector<double> taps(ntaps);
  for(int i = 0; i < ntaps; i++){
    taps[i] = xtaps[i+1];
  }

  std::vector<double> win;
  if(win_type == 1){
    win = blackman(ntaps);
  } else if(win_type == 2){
    win = blackmanharris(ntaps);
  } else if(win_type == 3){
    win = hamming(ntaps);
  } else if(win_type == 4){
    win = sym_blackman(ntaps);
  } else {
    assert(0);
  }
  for(int i = 0; i < ntaps; i++){
    taps[i] *= win[i];
    taps[i] /= taps.size();
  }

  if(0){
    FILE *fp = fopen("x", "w");
    for(int i = 0; i < (int)taps.size(); i++){
      fprintf(fp, "%.12f\n", taps[i]);
    }
    fclose(fp);
    fprintf(stderr, "%lu taps for %.2f .. %.2f\n", taps.size(), hz0, hz1);
    exit(0);
  }

  int len = samples.size();
  std::vector<double> out(len);

  for(int out_i = 0; out_i < len; out_i++){
    double x = 0;
    
    // start at samples[in_i] to compensate for delay.
    int in_i = out_i - (ntaps / 2);
    
    for(int k = 0; k < ntaps; k++){
      if(in_i + k >= 0 && in_i + k < len){
        x += samples[in_i + k] * taps[ntaps-1-k];
      }
    }
    out[out_i] = x;
  }
  
  return out;
}

#if 0
//
// IIR using the Liquid DSP library.
// code is from Liquid DSP examples.
//
std::vector<double>
bandpass(const std::vector<double> &samples, int rate, int symsamples, double hz0, double hz1)
{
  if(hz0 < 10)
    hz0 = 10;
  if(hz1 > (rate/2) - 10)
    hz1 = (rate/2) - 10;
  
  double fc = hz0 / rate; // low cutoff
  double f0 = 0.5 * (hz0 + hz1) / rate; // center frequency

  // XXX try Butter and Cheby type II and Elliptical
  
  //printf("rate=%d, %.1f %.1f, %.3f %.3f\n", rate, hz0, hz1, fc, f0);
  
  iirfilt_rrrf ff = iirfilt_rrrf_create_prototype((liquid_iirdes_filtertype) bandpass_type,
                                                  LIQUID_IIRDES_BANDPASS,
                                                  LIQUID_IIRDES_SOS,
                                                  bandpass_order,
                                                  fc, f0,
                                                  bandpass_pass_db,
                                                  bandpass_stop_db);

  int n = samples.size();
  std::vector<double> out(n);

  // filter delays by this many samples.
  // compensate so that subtraction aligns well.
  int delay = iirfilt_rrrf_groupdelay(ff, f0);

  // XXX filtfilt?

  for(int i = 0; i < n; i++){
    float x = samples[i];
    float y = 0;
    iirfilt_rrrf_execute(ff, x, &y);
    if(i >= delay)
      out[i-delay] = y;
  }

  for(int i = 0; i < delay; i++){
    float y = 0;
    iirfilt_rrrf_execute(ff, 0, &y);
    out[n-delay+i] = y;
  }

  iirfilt_rrrf_destroy(ff);

  return out;
}
#endif

//
// putative start of signal is at hz and symbol si0.
// 
// return 2 if it decodes to a brand-new message.
// return 1 if it decodes but we've already seen it,
//   perhaps in a different pass.
// return 0 if we could not decode.
//
// XXX merge with one_iter().
//
int
one(const std::vector<double> &samples, double hz, int off)
{
  // printf("one %.1f %.1f\n", hz, off / (double)rate_);

  //
  // set up to search for best frequency and time offset.
  //

  //
  // move down to 25 hz and re-sample to 200 samples/second,
  // i.e. 32 samples/symbol.
  //
  std::vector<double> samples200 = down_v7(samples, hz);

  int off200 = (off / (double) rate_) * 200;

  int ret = one_iter(samples200, off200, hz);
  return ret;
}

// we got a decode.
// re79 are the reconstructed correct symbols.
// append to analyze.out.
// the goal is to produce features for sklearn in
// order to guess probability that the strongest
// tone is actually incorrect. so we're only
// producing rows for strongest tones.
void
analyze(const std::vector<double> &samples200,
        const std::vector<int> &re79,
        double best_hz,
        int best_off)
{
  // mimic prepare_soft().
  ffts_t c79 = extract(samples200, best_hz, best_off, 0);

  // m79a = absolute values of c79.
  std::vector< std::vector<double> > m79a(79);
  for(int si = 0; si < 79; si++){
    m79a[si].resize(8);
    for(int bi = 0; bi < 8; bi++){
      m79a[si][bi] = std::abs(c79[si][bi]);
    }
  }

  std::vector< std::vector<double> > m79 = convert_to_snr(m79a, snr_how, snr_win);

  std::vector<Stats> noises0; // not best_in_noise
  std::vector<Stats> noises1; // best_in_noise
  std::vector<Stats> bests;
  make_stats(m79, noises0, bests, 1, 0);
  make_stats(m79, noises1, bests, 1, 1);

  FILE *fp = fopen("analyze.out", "a");
  for(int si = 0; si < 79; si++){
    // set mxi to the index of the strongest tone.
    double mx = -1;
    int mxi = -1;
    for(int bi = 0; bi < 8; bi++){
      if(mxi < 0 || m79[si][bi] > mx){
        mx = m79[si][bi];
        mxi = bi;
      }
    }
      
    // a: correct tone?
    // this is the "label", what ML is trying to predict.
    fprintf(fp, "%d", mxi == re79[si]);

    double x = m79[si][mxi];

    // b: pseudo-snr from convert_to_snr.
    fprintf(fp, " %f", x);

    // c: fraction of the total power in this signal.
    double sum = 0;
    for(int bi = 0; bi < 8; bi++){
      sum += m79[si][bi];
    }
    fprintf(fp, " %f", x / sum);

    // sort the tones to find second-strongest.
    std::vector<double> v(8);
    for(int bi = 0; bi < 8; bi++){
      double x = m79[si][bi];
      v[bi] = x;
    }
    std::sort(v.begin(), v.end());
    double x2 = v[6];

    // d: second-strongest signal.
    fprintf(fp, " %f", x2);

    // e: ratio of strongest to second-strongest.
    fprintf(fp, " %f", x / x2);

    // f: difference
    fprintf(fp, " %f", x - x2);

    // g: relative to overall "best" mean.
    fprintf(fp, " %f", x / bests[0].mean());

    // h: relative to overall "noise" mean.
    fprintf(fp, " %f", x / noises1[0].mean());

    // i: P(best) from gaussian.
    fprintf(fp, " %f", bests[0].problt(x, 0));

    // j: P(best) from distribution.
    fprintf(fp, " %f", bests[0].problt(x, 1));

    // k: P(best) from distribution and logistic.
    fprintf(fp, " %f", bests[0].problt(x, 2));

    // l: P(best) from distribution and gaussian.
    fprintf(fp, " %f", bests[0].problt(x, 3));

    // m: P(best) from distribution and gaussian.
    fprintf(fp, " %f", bests[0].problt(x, 4));

    // n: second-strongest's P(best) from gaussian.
    fprintf(fp, " %f", bests[0].problt(x2, 0));

    // o: second-strongest's P(best) from distribution and gaussian.
    fprintf(fp, " %f", bests[0].problt(x2, 3));


    // p: P(noise) from gaussian.
    fprintf(fp, " %f", noises0[0].problt(x, 0));

    // q: P(noise) from distribution.
    fprintf(fp, " %f", noises0[0].problt(x, 1));

    // r: P(noise) from distribution and logistic.
    fprintf(fp, " %f", noises0[0].problt(x, 2));

    // s: P(noise) from distribution and gaussian.
    fprintf(fp, " %f", noises0[0].problt(x, 3));

    // t: P(noise) from distribution and gaussian.
    fprintf(fp, " %f", noises0[0].problt(x, 4));

    // u: second-strongest's P(noise) from gaussian.
    fprintf(fp, " %f", noises0[0].problt(x2, 0));

    // v: second-strongest's P(noise) from distribution and gaussian.
    fprintf(fp, " %f", noises0[0].problt(x2, 3));


    // w: P(noise) from gaussian.
    fprintf(fp, " %f", noises1[0].problt(x, 0));

    // x: P(noise) from distribution.
    fprintf(fp, " %f", noises1[0].problt(x, 1));

    // y: P(noise) from distribution and logistic.
    fprintf(fp, " %f", noises1[0].problt(x, 2));

    // z: P(noise) from distribution and gaussian.
    fprintf(fp, " %f", noises1[0].problt(x, 3));

    // aa: P(noise) from distribution and gaussian.
    fprintf(fp, " %f", noises1[0].problt(x, 4));

    // bb: second-strongest's P(noise) from gaussian.
    fprintf(fp, " %f", noises1[0].problt(x2, 0));

    // cc: second-strongest's P(noise) from distribution and gaussian.
    fprintf(fp, " %f", noises1[0].problt(x2, 3));

    fprintf(fp, "\n");

  }

  fclose(fp);
}

// return 2 if it decodes to a brand-new message.
// return 1 if it decodes but we've already seen it,
//   perhaps in a different pass.
// return 0 if we could not decode.
int
one_iter(const std::vector<double> &samples200, int best_off, double hz_for_cb)
{
  double best_hz;
  int ok = search_both(samples200,
                       25, second_hz_inc, second_hz_win,
                       best_off, second_off_inc * 32, second_off_win * 32,
                       best_hz, best_off);
  if(ok != 1){
    return 0;
  }

#if 1
  if(drift <= 0){
    int ret = one_iter1(samples200, best_off, best_hz, hz_for_cb, hz_for_cb);
    return ret;
  }
  
  double drifts[3] = { 0, -drift, drift };

  double best_st = 0;
  int got_best = 0;
  double best_dr = 0;
  std::vector<double> best_ss;

  for(int drifti = 0; drifti < 3; drifti++){
    double dr = drifts[drifti];

    // apply frequency drift, and put best_hz at 25.
    std::vector<double> ss1;
    if(drifti == 0){
      assert(dr == 0.0);
      ss1 = shift200(samples200, 0, samples200.size(), best_hz);
    } else {
      assert(dr != 0.0);
      ss1 = hilbert_shift(samples200, (25-best_hz)+dr, (25-best_hz)-dr, 200);
    }

    double st = one_strength(ss1, 25, best_off);
    if(!got_best || st > best_st){
      got_best = 1;
      best_st = st;
      best_dr = dr;
      best_ss = ss1;
    }
  }

  int ret = one_iter1(best_ss, best_off, 25.0,
                      hz_for_cb-best_dr+(best_hz-25),
                      hz_for_cb+best_dr+(best_hz-25));
  return ret;
#else
  int ret = one_iter1(samples200, best_off, best_hz, hz_for_cb, hz_for_cb);
  return ret;
#endif
  
}

//
// estimate SNR, yielding numbers vaguely similar to WSJT-X.
// m79 is a 79x8 complex FFT output.
//
double
guess_snr(const ffts_t &m79)
{
  int costas[] = { 4, 2, 5, 6, 1, 3, 0 };
  double noises = 0;
  double signals = 0;

  for(int i = 0; i < 7; i++){
    signals += std::abs(m79[i][costas[i]]);
    signals += std::abs(m79[36+i][costas[i]]);
    signals += std::abs(m79[72+i][costas[i]]);
    noises += std::abs(m79[i][(costas[i]+4)%8]);
    noises += std::abs(m79[36+i][(costas[i]+4)%8]);
    noises += std::abs(m79[72+i][(costas[i]+4)%8]);
  }

  for(int i = 0; i < 79; i++){
    if(i < 7 || (i >= 36 && i < 36+7) || (i >= 72 && i < 72+7))
      continue;
    std::vector<double> v(8);
    for(int j = 0; j < 8; j++){
      v[j] = std::abs(m79[i][j]);
    }
    std::sort(v.begin(), v.end());
    signals += v[7]; // strongest tone, probably the signal
    noises += (v[2]+v[3]+v[4])/3;
  }

  noises /= 79;
  signals /= 79;

  noises *= noises; // square yields power
  signals *= signals;

  double raw = signals / noises;
  raw -= 1; // turn (s+n)/n into s/n
  if(raw < 0.1)
    raw = 0.1;
  raw /= (2500.0 / 2.7); // 2.7 hz noise b/w -> 2500 hz b/w
  double snr = 10 * log10(raw);
  snr += 5;
  snr *= 1.4;
  return snr;
}

//
// the signal is at 25 hz in samples200.
// 
// return 2 if it decodes to a brand-new message.
// return 1 if it decodes but we've already seen it,
//   perhaps in a different pass.
// return 0 if we could not decode.
//
int
one_iter1(const std::vector<double> &samples200,
          int best_off, double best_hz,
          double hz0_for_cb, double hz1_for_cb)
{
  // printf("  %.1f %.1f %.1f\n", best_off / 200.0, best_hz, hz0_for_cb);

  // mini 79x8 FFT.
  ffts_t m79 = extract(samples200, best_hz, best_off, 0);

  double ll174[174];
  //prepare_hard(m79, ll174);
  prepare_soft(m79, ll174, best_off);


  int ret = try_decode(ll174, samples200, best_hz, best_off,
                       hz0_for_cb, hz1_for_cb, 1, "", m79);
  if(ret){
    return ret;
  }

  if(use_hints){
    for(int hi = 0; hi < (int)hints1_.size(); hi++){
      int h = hints1_[hi]; // 28-bit number, goes in ll174 0..28
      if(use_hints == 2 && h != 2){
        // just CQ
        continue;
      }
      double n174[174];
      for(int i = 0; i < 174; i++){
        if(i < 28){
          int bit = h & (1 << 27);
          if(bit){
            n174[i] = -4.97;
          } else {
            n174[i] = 4.97;
          }
          h <<= 1;
        } else {
          n174[i] = ll174[i];
        }
      }
      int ret = try_decode(n174, samples200, best_hz, best_off,
                           hz0_for_cb, hz1_for_cb, 0, "hint1", m79);
      if(ret){
        return ret;
      }
    }
  }
  
  if(use_hints == 1){
    for(int hi = 0; hi < (int)hints2_.size(); hi++){
      int h = hints2_[hi]; // 28-bit number, goes in ll174 29:29+28
      double n174[174];
      for(int i = 0; i < 174; i++){
        if(i >= 29 && i < 29+28){
          int bit = h & (1 << 27);
          if(bit){
            n174[i] = -4.97;
          } else {
            n174[i] = 4.97;
          }
          h <<= 1;
        } else {
          n174[i] = ll174[i];
        }
      }
      int ret = try_decode(n174, samples200, best_hz, best_off,
                           hz0_for_cb, hz1_for_cb, 0, "hint2", m79);
      if(ret){
        return ret;
      }
    }
  }

  return 0;
}

//
// subtract a corrected decoded signal from nsamples_.
// re79 is the corrected symbol numbers, as sent over the air.
//
// just zeros out the relevant bin.
// XXX surrounding median amplitude.
// XXX leakage into neighboring bins.
//
void
old_subtract(const std::vector<int> re79,
         double hz0,
         double hz1,
         double off_sec)
{
  int block = 1920 / (12000 / rate_);
  double bin_hz = rate_ / (double) block;
  int off0 = off_sec * rate_;

  double mhz = (hz0 + hz1) / 2.0;
  int bin0 = round(mhz / bin_hz);

  // move nsamples so that signal is centered in bin0.
  double diff0 = (bin0 * bin_hz) - hz0;
  double diff1 = (bin0 * bin_hz) - hz1;
  std::vector<double> moved = hilbert_shift(nsamples_, diff0, diff1, rate_);

  ffts_t bins = ffts(moved, off0, block, 0);

  if(bin0 + 8 > bins[0].size())
    return;
  if(bins.size() < 79)
    return;

  std::vector<double> tone_avg(79);
  if(sub_amp_win > 0){
#if 1
    for(int si = 0; si < 79; si++){
      std::vector<double> v;
      for(int i = -sub_amp_win; i <= sub_amp_win; i++){
        if(si+i >= 0 && si+i < 79){
          double x = std::abs(bins[si+i][bin0+re79[si+i]]);
          v.push_back(x);
        }
      }
      std::sort(v.begin(), v.end());
      tone_avg[si] = v[v.size() / 2];
    }
#else
    for(int si = 0; si < 79; si++){
      double x = 0;
      int n = 0;
      for(int i = -sub_amp_win; i <= sub_amp_win; i++){
        if(si+i >= 0 && si+i < 79){
          x += std::abs(bins[si+i][bin0+re79[si+i]]);
          n++;
        }
      }
      tone_avg[si] = x / n;
    }
#endif
  }

  for(int si = 0; si < 79; si++){
    int sym = bin0 + re79[si];

    if(sub_amp_win > 0){
      double aa = std::abs(bins[si][sym]);

      double ampl = tone_avg[si];
      if(ampl > aa)
        ampl = aa;
        
      if(aa > 0.0){
        bins[si][sym] /= aa;
        bins[si][sym] *= (aa - ampl);
      }
    } else {
      bins[si][sym] = 0;
    }

    std::vector<double> ss = one_ifft(bins[si], 0);
    assert(ss.size() == block);
    for(int jj = 0; jj < block; jj++){
      // the "/ block" is to correct for the scaling factor
      // generated by fft().
      double x = ss[jj];
      if(sub_amp_win <= 0){
        x /= block;
      }
      moved[off0 + block*si + jj] = x;
    }
  }

  nsamples_ = hilbert_shift(moved, -diff0, -diff1, rate_);
}

// return symbol length in samples at the given rate.
// insist on integer symbol lengths so that we can
// use whole FFT bins.
int
blocksize(int rate)
{
  // FT8 symbol length is 1920 at 12000 samples/second.
  int xblock = 1920 / (12000.0 / rate);
  assert(xblock == (int) xblock);
  int block = xblock;
  return block;
}

//
// subtract a corrected decoded signal from nsamples_,
// perhaps revealing a weaker signal underneath,
// to be decoded in a subsequent pass.
// re79 is the corrected symbol numbers, as sent over the air.
//
void
subtract(const std::vector<int> re79,
         double hz0,
         double hz1,
         double off_sec)
{
  int block = blocksize(rate_);
  double bin_hz = rate_ / (double) block;
  int off0 = off_sec * rate_;

  double mhz = (hz0 + hz1) / 2.0;
  int bin0 = round(mhz / bin_hz);

  // move nsamples so that signal is centered in bin0.
  double diff0 = (bin0 * bin_hz) - hz0;
  double diff1 = (bin0 * bin_hz) - hz1;
  std::vector<double> moved = hilbert_shift(nsamples_, diff0, diff1, rate_);

  ffts_t bins = ffts(moved, off0, block, "subtract1");

  if(bin0 + 8 > bins[0].size())
    return;
  if(bins.size() < 79)
    return;

  // for each symbol, median amplitude of nearby tones.
  std::vector<double> nearby_amp(79);
  if(sub_amp_win > 0){
    for(int si = 0; si < 79; si++){
      std::vector<double> v;
      for(int i = -sub_amp_win; i <= sub_amp_win; i++){
        if(si+i >= 0 && si+i < 79){
          double x = std::abs(bins[si+i][bin0+re79[si+i]]);
          v.push_back(x);
        }
      }
      std::sort(v.begin(), v.end());
      nearby_amp[si] = v[v.size() / 2];
    }
  }

  // for each symbol, median phase of nearby symbols.
  std::vector<double> nearby_phase(79);
  if(fancy_subtract && sub_phase_win > 0){
    std::vector<double> ph(79);
    for(int si = 0; si < 79; si++){
      ph[si] = std::arg(bins[si][bin0+re79[si]]); // -pi .. pi
    }
    
    for(int si = 0; si < 79; si++){
      std::vector<double> v;
      for(int i = -sub_phase_win; i <= sub_phase_win; i++){
        if(si+i >= 0 && si+i < 79){
          double x = ph[si+i];
          v.push_back(x);
        }
      }

      // choose the phase that has the lowest total distance to other
      // phases. like median but avoids -pi..pi wrap-around.
      int n = v.size();
      int best = -1;
      double best_score;
      for(int i = 0; i < n; i++){
        double score = 0;
        for(int j = 0; j < n; j++){
          if(i == j)
            continue;
          double d = fabs(v[i] - v[j]);
          if(d > M_PI)
            d = 2*M_PI - d;
          score += d;
        }
        if(best == -1 || score < best_score){
          best = i;
          best_score = score;
        }
      }

      nearby_phase[si] = v[best];
    }
  }

  for(int si = 0; si < 79; si++){
    int sym = bin0 + re79[si];

    if(fancy_subtract){
      std::complex<double> c = bins[si][sym];

      double phase;
      if(sub_phase_win > 0){
        phase = nearby_phase[si];
      } else {
        phase = std::arg(c);
      }

      double amp;
      if(sub_amp_win > 0){
        amp = nearby_amp[si];
      } else {
        amp = std::abs(c);
      }

      // amp = std::min(amp, std::abs(c));

      // we know what the symbol was, and we have guesses for
      // the signal's phase and amplitude, so subtract.
      
      // subtract in the time domain, since it's pretty involved
      // to do it correctly in the frequency domain. the point is
      // to subtract the likely signal, but to leave behind
      // stuff at that tone that's not the same phase as the signal.
      
      // FFT multiplies magnitudes by number of bins,
      // or half the number of samples.
      amp /= (block / 2);
      
      double theta = phase;
      
      double tonehz = 6.25 * (bin0 + re79[si]);
      
      for(int jj = 0; jj < block; jj++){
        double x = amp * cos(theta);
        int iii = off0 + block*si + jj;
        moved[iii] -= x;
        theta += 2 * M_PI / (rate_ / tonehz);
      }
    } else {
      if(sub_amp_win > 0){
        double aa = std::abs(bins[si][sym]);
        
        double amp = nearby_amp[si];
        if(amp > aa)
          amp = aa;
        
        if(aa > 0.0){
          bins[si][sym] /= aa;
          bins[si][sym] *= (aa - amp);
        }
      } else {
        bins[si][sym] = 0;
      }

      // to get the original amplitude scale, you have to divide
      // by blocksize before calling ifft.
      std::vector<std::complex<double>> bb(bins[si].size());
      for(int i = 0; i < bb.size(); i++)
        bb[i] = bins[si][i] / (double)block;
      
      std::vector<double> ss = one_ifft(bb, "subtract2");
      assert(ss.size() == block);
      for(int jj = 0; jj < block; jj++){
        moved[off0 + block*si + jj] = ss[jj];
      }
    }
  }

  nsamples_ = hilbert_shift(moved, -diff0, -diff1, rate_);
}

// return 2 if it decodes to a brand-new message.
// return 1 if it decodes but we've already seen it,
//   perhaps in a different pass.
// return 0 if we could not decode.
int
try_decode(double ll174[174], const std::vector<double> &samples200,
           double best_hz, int best_off, double hz0_for_cb, double hz1_for_cb,
           int use_osd, const char *comment1,
           const ffts_t &m79)
{
  int a174[174];
  std::string comment(comment1);

  if(decode(ll174, a174, use_osd, comment)){
    // a174 is 87 bits of parity, 87 bits of data.

    // reconstruct correct 79 symbols from LDPC output.
    std::vector<int> re79 = recode(a174);

    // save some statistics.
    //analyze(samples200, re79, best_hz, best_off);

    // and fine-tune offset and hz.
    double best_strength = 0;
    search_both_known(samples200, re79,
                      best_hz, third_hz_inc, third_hz_win,
                      best_off, third_off_inc, third_off_win,
                      best_hz, best_off, best_strength);

    double off_sec = best_off / 200.0;

    // hz0_for_cb and corrected_hz* refers to samples_,
    // so that's what we want for subtraction.
    // but we down-shifted what Python gave us by down_hz_,
    // so we also need to add that for the callback.
    
    double corrected_hz0 = hz0_for_cb + (best_hz - 25.0);
    double corrected_hz1 = hz1_for_cb + (best_hz - 25.0);

    double snr = guess_snr(m79);
    
    if(cb_ != 0){
      cb_mu_.lock();
      int ret = cb_(a174+87, corrected_hz0 + down_hz_, corrected_hz1 + down_hz_,
                    off_sec, comment.c_str(), snr);
      cb_mu_.unlock();
      if(ret == 2){
        // a new decode.
        // subtract from nsamples_.

        //writewav(samples_, "a.wav", rate_);
        //writewav(nsamples_, "b.wav", rate_);

        subtract(re79, corrected_hz0, corrected_hz1, off_sec);

        //writewav(nsamples_, "c.wav", rate_);
        //exit(1);
      }
      return ret;
    }
    return 1;
  } else {
    return 0;
  }
}

};

std::mutex FT8::cb_mu_;

void
test_hilbert_shift()
{
  FT8 ft8;

  std::vector<double> x(8);
  x[0] = 1;
  x[1] = 2;
  x[2] = 3;
  x[3] = 4;
  x[4] = 1;
  x[5] = 2;
  x[6] = 3;
  x[7] = 4;

  std::vector<double> y = hilbert_shift(x, 1.0, 1.0, 8);
  assert(y.size() == x.size());

  //for(int i = 0; i < y.size(); i++){
  //  printf("%f\n", y[i]);
  //}

  // y should be:
  // 1.000000
  // 2.121320
  // 1.000000
  // -3.535534
  // -1.000000
  // -2.121320
  // -1.000000
  // 3.535534
}

//
// Python calls these.
//
void
entry(double xsamples[], int nsamples, int start, int rate,
      double min_hz,
      double max_hz,
      int hints1[],
      int hints2[],
      cb_t cb)
{
  double time_left = budget;
  double total_time_left = budget;
  
  double t0 = now();
  double deadline = t0 + time_left;
  double final_deadline = t0 + total_time_left;
  
  std::vector<double> samples(nsamples);
  for(int i = 0; i < nsamples; i++){
    samples[i] = xsamples[i];
  }

  if(min_hz < 0){
    min_hz = 0;
  }
  if(max_hz > rate/2){
    max_hz = rate/2;
  }
  double per = (max_hz - min_hz) / nthreads;

  std::vector<FT8 *> thv;

  for(int i = 0; i < nthreads; i++){
    double hz0 = min_hz + i * per;
    if(i > 0)
      hz0 -= overlap;
    
    double hz1 = min_hz + (i + 1) * per;
    if(i != nthreads-1)
      hz1 += overlap;

    hz0 = std::max(hz0, 0.0);
    hz1 = std::min(hz1, (rate / 2.0) - 50);

    FT8 *ft8 = new FT8(samples,
                       hz0, hz1,
                       start, rate, hints1, hints2, deadline, final_deadline, cb);

    ft8->th_ = new std::thread( [ ft8 ] () { ft8->go(); } );
    thv.push_back(ft8);
  }

  for(int i = 0; i < thv.size(); i++){
    thv[i]->th_->join();
    delete thv[i]->th_;
    delete thv[i];
  }
}

double
set(std::string param, std::string val)
{
  struct sss {
    const char *name;
    void *addr;
    int type; // 0 int, 1 double
  };
  struct sss params[] =
    {
     { "snr_win", &snr_win, 0 },
     { "snr_how", &snr_how, 0 },
     { "soft_ranges", &soft_ranges, 0 },
     { "best_in_noise", &best_in_noise, 0 },
     { "ldpc_iters", &ldpc_iters, 0 },
     { "shoulder", &shoulder, 1 },
     { "shoulder_extra", &shoulder_extra, 1 },
     { "bandpass_block", &bandpass_block, 1 },
     { "bandpass_order", &bandpass_order, 0 },
     { "bandpass_type", &bandpass_type, 0 },
     { "bandpass_stop_db", &bandpass_stop_db, 1 },
     { "bandpass_pass_db", &bandpass_pass_db, 1 },
     { "second_hz_inc", &second_hz_inc, 1 },
     { "second_hz_win", &second_hz_win, 1 },
     { "second_off_inc", &second_off_inc, 1 },
     { "second_off_win", &second_off_win, 1 },
     { "third_hz_inc", &third_hz_inc, 1 },
     { "third_hz_win", &third_hz_win, 1 },
     { "third_off_inc", &third_off_inc, 0 },
     { "third_off_win", &third_off_win, 0 },
     { "log_tail", &log_tail, 1 },
     { "log_rate", &log_rate, 1 },
     { "problt_how", &problt_how, 0 },
     { "use_apriori", &use_apriori, 0 },
     { "use_hints", &use_hints, 0 },
     { "drift", &drift, 1 },
     { "win_type", &win_type, 0 },
     { "osd_depth", &osd_depth, 0 },
     { "ncoarse", &ncoarse, 0 },
     { "ncoarse_blocks", &ncoarse_blocks, 0 },
     { "tminus", &tminus, 1 },
     { "tplus", &tplus, 1 },
     { "coarse_off_fracs", &coarse_off_fracs, 0 },
     { "coarse_hz_fracs", &coarse_hz_fracs, 0 },
     { "already_hz", &already_hz, 1 },
     { "nthreads", &nthreads, 0 },
     { "npasses", &npasses, 0 },
     { "overlap", &overlap, 1 },
     { "sub_amp_win", &sub_amp_win, 0 },
     { "nyquist", &nyquist, 1 },
     { "oddrate", &oddrate, 0 },
     { "osd_ldpc_thresh", &osd_ldpc_thresh, 0 },
     { "pass0_frac", &pass0_frac, 1 },
     { "fancy_subtract", &fancy_subtract, 0 },
     { "sub_phase_win", &sub_phase_win, 0 },
     { "go_extra", &go_extra, 1 },
     { "do_reduce", &do_reduce, 0 },
     { "reduce_how", &reduce_how, 0 },
     { "reduce_shoulder", &reduce_shoulder, 1 },
     { "reduce_factor", &reduce_factor, 1 },
     { "reduce_extra", &reduce_extra, 1 },
     { "budget", &budget, 1 },
    };
  int nparams = sizeof(params) / sizeof(params[0]);

  for(int i = 0; i < nparams; i++){
    if(param == params[i].name){
      if(val.size() > 0){
        if(params[i].type == 0){
          *(int*)params[i].addr = round(atof(val.c_str()));
        } else if(params[i].type == 1){
          *(double*)params[i].addr = atof(val.c_str());
        }
      }
      if(params[i].type == 0){
        return *(int*)params[i].addr;
      } else if(params[i].type == 1){
        return *(double*)params[i].addr;
      } else {
        fprintf(stderr, "weird type %d\n", params[i].type);
        return 0;
      }
    }
  }
  fprintf(stderr, "ft8.cc set(%s, %s) unknown parameter\n", param.c_str(), val.c_str());
  return 0;
}
