//
// A text-based JS8 user interface.
//
// Robert Morris, AB1HL
//
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

std::string mycall;
std::string mygrid;

void
usage()
{
  fprintf(stderr, "Usage: fate -card CARD CHAN [-c MYCALL MYGRID -out CARD CHAN]\n");
  fprintf(stderr, "       fate -cardfile xxx.wav\n");
  fprintf(stderr, "       fate -levels card channel\n");
  fprintf(stderr, "       fate -list\n");
  fprintf(stderr, "       fate ... -f8101 /dev/xxx\n");
  fprintf(stderr, "       fate ... -dtr /dev/xxx\n");
  exit(1);
}

class Line {
public:
  std::string call_;
  double hz_;
  std::string text_;
  double last_; // UNIX time we last saw
  double snr_;
};

std::vector<Line> lines;
std::mutex lines_mu;

struct layout {
  int rows;
  int cols;
  int pan_rows; // all signals
  int rx_rows;  // the one received signal
  int tx_rows;  // my transmission
};

void
get_layout(layout &lay)
{
  int rows = 24;
  int cols = 80;

  struct winsize ws;
  if(ioctl(1, TIOCGWINSZ, &ws) == 0){
    rows = ws.ws_row;
    cols = ws.ws_col;
  }

  lay.rows = rows;
  lay.cols = cols;
  if(rows > 50){
    lay.tx_rows = (rows - 18 - 2) / 2;
    lay.rx_rows = (rows - 18 - 2) / 2;
  } else {
    lay.tx_rows = rows / 3.5;
    lay.rx_rows = rows / 3.5;
  }
  lay.pan_rows = rows - lay.tx_rows - lay.rx_rows - 1;
}

//
// squeeze out repeated ~
//
std::string
simplify(std::string s)
{
  // squeeze out repeated ~
  std::string ss;
  for(int i = 0; i < (int) s.size(); i++){
    if(s[i] == '~' && i > 0 && s[i-1] == '~'){
      // nothing
    } else {
      ss.push_back(s[i]);
    }
  }

  return ss;
}

//
// prints exactly n lines, the last n lines of s.
// does not print the final newline.
//
void
print_n(const char *prefix, std::string s, int n, int cols, bool suppress_final_empty)
{
  std::vector<std::string> rxlines;
  std::string line;
  for(int i = 0; i < (int) s.size(); i++){
      int c = s[i] & 0xff;
    if(c == '\n' || c == '\r' || (int) line.size() > cols - 2){
      if(line.size() > 0){
        rxlines.push_back(line);
        line = "";
      }
      if(c != '\n' && c != '\r')
        line.push_back(c);
    } else {
      line.push_back(c);
    }
  }
  int i0 = rxlines.size() - n + 1;
  if(line.size() == 0 && suppress_final_empty)
    i0 -= 1;
  if(i0 < 0)
    i0 = 0;

  for(int i = 0; i < n; i++){
    printf("%s", prefix);
    if(i0 < (int) rxlines.size()){
      printf("%s", rxlines[i0].c_str());
    } else if(i0 == (int) rxlines.size() && (!suppress_final_empty || line.size() > 0)){
      printf("%s", line.c_str());
    }
    i0++;
    if(i+1 < n)
      printf("\n");
  }
}

// file descriptor to ICOM IC-F8101 USB "serial" port for CIV
int f8101 = -1;

// returns 0 if OK, -1 if error.
int
f8101_cmd(const std::vector<int> cmd)
{

#ifdef TIOCFLUSH
  // get rid of any lingering input characters.
  int what = FREAD;
  ioctl(f8101, TIOCFLUSH, &what);
#endif

#ifdef __linux__
  tcflush(f8101, TCIFLUSH);
#endif

  std::vector<char> obuf;
  obuf.push_back(0xfe); // preamble
  obuf.push_back(0xfe); // preamble
  obuf.push_back(0x8a); // IC-F8101's CIV address
  obuf.push_back(0xe0); // controller's (our) address
  for(int i = 0; i < (int) cmd.size(); i++)
    obuf.push_back(cmd[i]);
  obuf.push_back(0xfd); // end of message

  if(write(f8101, obuf.data(), (int) obuf.size()) != (long) obuf.size()){
    perror("f8101 write");
    exit(1);
  }

  time_t t0;
  time(&t0);

  // read the reply.
  std::vector<unsigned char> ibuf;
  while(1){
    time_t t1;
    time(&t1);
    if(t1 > t0 + 5){
      fprintf(stderr, "f8101 reply timeout\n");
      exit(1);
    }
    
    //
    // first, check if there's a complete reply already.
    //
    
    // discard any input before 0xfe 0xfe.
    while(ibuf.size() >= 2 && (ibuf[0] != 0xfe || ibuf[1] != 0xfe)){
      ibuf.erase(ibuf.begin());
    }

    // the minimum size reply is 6 bytes:
    // fe fe e0 8a fb fd -- OK
    // fe fe e0 8a fa fd -- error

    // is there a 0xfd, signifying end of message?
    int fdi = -1;
    for(int i = 5; i < (int) ibuf.size(); i++){
      if(ibuf[i] == 0xfd){
        fdi = i;
        break;
      }
    }

    if(ibuf.size() >= 6 && ibuf[0] == 0xfe && ibuf[1] == 0xfe && fdi >= 0){
      if(ibuf[2] == 0x8a || ibuf[3] == 0xe0){
        // an echo of the cmd we sent.
        ibuf.clear();
        continue;
      }
      if(ibuf[2] != 0xe0 || ibuf[3] != 0x8a){
        // unexpected CIV addresses
        fprintf(stderr, "CIV ???\n");
        ibuf.clear();
        continue;
      }
      if(ibuf[4] == 0xfb){
        // OK
        return 0;
      }
      if(ibuf[4] == 0xfa){
        // error
        fprintf(stderr, "CIV reply error, cmd=[");
        for(int i = 0; i < (int) cmd.size(); i++)
          fprintf(stderr, "%02x ", cmd[i] & 0xff);
        fprintf(stderr, "]\n");
        exit(1);
        return -1;
      }
      fprintf(stderr, "CIV reply not understood\n");
      ibuf.clear();
    }

    errno = 0;
    char c = 0;
    int n = read(f8101, &c, 1);
    // fprintf(stderr, "  n=%d errno=%d c=%02x \n", n, errno, c & 0xff);
    if(n == 1){
      ibuf.push_back(c);
    } else {
      usleep(10*1000);
    }
  }
}

void
f8101_tx()
{
  // TX by ACC PTT -- why does this work?
  std::vector<int> c{ 0x1a, 0x37, 0x00, 0x02 };
  f8101_cmd(c);
}

void
f8101_rx()
{
  {
    std::vector<int> c{ 0x1c, 0x00, 0x00 }; // RX
    f8101_cmd(c);
  }

  {
    std::vector<int> c{ 0x1a, 0x37, 0x00, 0x00 }; // RX
    f8101_cmd(c);
  }
}

void
f8101_init(char *devname)
{
  f8101 = open(devname, O_RDWR);
  if(f8101 < 0){
    perror(devname);
    exit(1);
  }

#if 1
  // prepare to eventually poll for replies.
  int flags = fcntl(f8101, F_GETFL, 0);
  flags |= O_NONBLOCK;
  if(fcntl(f8101, F_SETFL, flags) < 0){
    perror("f8101 fcntl O_NONBLOCK");
    exit(1);
  }
#endif

  struct termios tt;
  if(tcgetattr(f8101, &tt) != 0){
    perror("tcgetattr");
    exit(1);
  }
  cfsetspeed(&tt, B38400);
  cfmakeraw(&tt);
  tt.c_iflag = IGNBRK;
  tt.c_oflag = 0;
  tt.c_cflag = CS8 | CREAD | HUPCL | CLOCAL;
  tt.c_lflag = 0;
  if(tcsetattr(f8101, TCSANOW, &tt) != 0){
    perror("tcsetattr");
    exit(1);
  }

#if 0
  {
    std::vector<int> c{ 0x07, 0x01 }; // select VFO B
    f8101_cmd(c);
  }
  sleep(1);
  {
    std::vector<int> c{ 0x07, 0x00 }; // select VFO A
    f8101_cmd(c);
  }
#endif

  f8101_rx();
}

// file descriptor to serial port whose DTR
// toggles microHAM DSP PTT.
int dtr = -1;

void
dtr_tx()
{
#ifdef TIOCSDTR
  int junk = 0;
  if(ioctl(dtr, TIOCSDTR, &junk) < 0){
    perror("TIOCSDTR");
    exit(1);
  }
#else
  fprintf(stderr, "\n>>> no TIOCSDTR!!! <<<\n");
#endif
}

void
dtr_rx()
{
#ifdef TIOCCDTR
  int junk = 0;
  if(ioctl(dtr, TIOCCDTR, &junk) < 0){
    perror("TIOCCDTR");
    exit(1);
  }
#else
  fprintf(stderr, "\n>>> no TIOCCDTR!!! <<<\n");
#endif
}

void
dtr_init(char *devname)
{
  dtr = open(devname, 0);
  if(dtr < 0){
    perror(devname);
    exit(1);
  }
  dtr_rx();
}

// tell the rig interface (if any) to tell
// the radio switch to transmit.
void
ptt_tx()
{
  if(f8101 >= 0)
    f8101_tx();
  if(dtr >= 0)
    dtr_tx();
}

void
ptt_rx()
{
  if(f8101 >= 0)
    f8101_rx();
  if(dtr >= 0)
    dtr_rx();
}

//
// the signal we're talking to.
//
std::string rx_call;
double rx_hz = 1400;
double rx_snr = 0;
std::mutex rx_buf_mu;
FILE *qso_fp = 0;
std::string rx_buf;

// buffer of characters I want to transmit.
std::mutex tx_buf_mu;
std::string tx_buf;
double tx_hz = 1400;

volatile int transmitting = 0;
std::string display_status;

void
draw_screen()
{
  layout lay;

  get_layout(lay);
  
  // clear screen
  printf("\033[H");  // home
  printf("\033[2J"); // clear

  if(transmitting){
    printf("TX ");
  } else {
    printf("RX ");
  }
  if(rx_hz > 0){
    printf("%.0f ", rx_hz);
  }
  printf("%s ", rx_call.c_str());
  printf("%s ", display_status.c_str());
  printf("\n");
  
  for(int i = 0; i < lay.pan_rows; i++){
    lines_mu.lock();
    if(i >= (int) lines.size())
      break;
    Line ll = lines[i];
    lines_mu.unlock();
    
    printf("%c %4.0f ",
           'A' + i,
           ll.hz_);
    
    std::string lt = simplify(ll.text_);
    
    int j0 = lt.size() - lay.cols + 12; // leave room for hz, q
    // int j0 = lt.size() - lay.cols + 26; // leave room for hz, snr
    if(j0 < 0)
      j0 = 0;
    for(int j = j0; j < (int) lt.size(); j++){
      int c = lt[j];
      if(c == '\n' || c == '\r'){
        printf(" ");
      } else {
        printf("%c", c);
      }
    }
    printf("\n");
  }

  // --- first divider.
  for(int i = 0; i < lay.cols-1; i++)
    printf("-");

  // the selected signal.
  printf("\n");
  rx_buf_mu.lock();
  std::string s = rx_buf;
  rx_buf_mu.unlock();
  s = simplify(s);
  print_n("- ", s, lay.rx_rows-1, lay.cols-1, true);
  printf("\n");


  // --- second divider.
  char info[64];
  sprintf(info, "---- %4.0f %2.0f %s ",
          rx_hz, rx_snr, transmitting ? "TX" : "RX");
  printf("%s", info);
  for(int i = 0; i < lay.cols - (int) strlen(info) - 1; i++)
    printf("-");

  // my transmitted text, if any.
  printf("\n");
  tx_buf_mu.lock();
  std::string tmp = tx_buf;
  tx_buf_mu.unlock();
  print_n("> ", tmp, lay.tx_rows-1, lay.cols-1, false);
  
  fflush(stdout);
}

void
kb_loop()
{
  int state = -1;
  while(1){
    unsigned char c;
    if(read(0, &c, 1) != 1){
      exit(1);
    }
    if(state < 0 && c == '\001'){
      // control-A
      state = '\001';
    } else if(c == '\030'){
      // control-X
      exit(0);
    } else if(state == '\001'){
      // control-A <letter>
      // chooses a signal to receive.

      if(isupper(c)) c = tolower(c);
      int i = c - 'a';
      lines_mu.lock();
      if(i >= 0 && i < (int) lines.size()){
        rx_hz = lines[i].hz_;
        rx_snr = lines[i].snr_;
        rx_call = lines[i].call_;
        rx_buf_mu.lock();
        rx_buf.clear();
        rx_buf = lines[i].text_;
        if(qso_fp){
          time_t clock;
          time(&clock);
          struct tm tmx;
          gmtime_r(&clock, &tmx);
          fprintf(qso_fp, "\n\n----- %02d/%02d/%04d %02d:%02d\n\n%s",
                  tmx.tm_mon+1,
                  tmx.tm_mday,
                  tmx.tm_year+1900,
                  tmx.tm_hour,
                  tmx.tm_min,
                  rx_buf.c_str());
        }
        rx_buf_mu.unlock();
      }
      lines_mu.unlock();

      state = -1;
    } else {
      tx_buf_mu.lock();
      if(c == '\010' || c == '\177'){
        // backspace
        if(tx_buf.size() > 0){
          // forget most recent pending char.
          tx_buf.pop_back();
        }
      } else if(c == '\024'){
        // ^T -- send one second of test tone
        tx_buf.push_back((char)128);
      } else if(c == '\005'){
        // ^E -- send CQ
        tx_buf.push_back((char)129);
      } else if(c == '\006'){
        // ^F -- reply to CQ with HW CPY?
        tx_buf.push_back((char)130);
      } else if(c == '\007'){
        // ^G -- report back received SNR
        tx_buf.push_back((char)131);
      } else {
        tx_buf.push_back((char)c);
      }
      tx_buf_mu.unlock();
    }
  }
}

int
fate_cb(int *a87, double hz0, double hz1, double off,
    const char *comment, double snr)
{
  std::string other_call;
  
  std::string txt = unpack(a87, other_call);

  dups_mu.lock();
  bool already = dups[txt];
  dups[txt] = true;
  dups_mu.unlock();

  if(already){
    return(1); // not a new decode
  }

  lines_mu.lock();

  int found = -1;
  double found_dhz = 1000;
  int empty = -1;
  int oldest_i = -1;
  double oldest_time = 0;
  for(int i = 0; i < (int) lines.size(); i++){
    if(lines[i].hz_ < 0 && empty < 0){
      empty = i;
    }
    if(oldest_i < 0 || lines[i].last_ < oldest_time){
      oldest_i = i;
      oldest_time = lines[i].last_;
    }
    if(lines[i].hz_ >= 0){
      double dhz = std::abs(lines[i].hz_ - hz0);
      if(dhz < 20 && dhz < found_dhz){
        found_dhz = dhz;
        found = i;
      }
    }
  }

  if(found < 0 && empty >= 0){
    found = empty;
    lines[found].text_ = "";
    lines[found].call_ = "";
  }

  if(found < 0 && oldest_i >= 0){
    found = oldest_i;
    lines[found].text_ = "";
    lines[found].call_ = "";
  }

  std::string line_call;
  if(found >= 0){
    if(other_call.size() > 0){
      lines[found].call_ = other_call;
    }
    lines[found].hz_ = hz0;
    lines[found].snr_ = snr;
    lines[found].text_ += txt;
    lines[found].last_ = now();
    line_call = lines[found].call_;
  } else {
    // can never happen...
    display_status = "*** NO SPACE IN LINES ***";
  }

  lines_mu.unlock();

  if(line_call.size() > 0 && line_call == rx_call){
    rx_buf_mu.lock();
    rx_buf += txt;
    rx_snr = snr;
    rx_buf_mu.unlock();
    if(qso_fp)
      fprintf(qso_fp, "%s", txt.c_str());
  }

  return 2; // indicate it's a good-looking new decode.
}

std::vector<double>
costone(double hz, int rate, int n)
{
  std::vector<double> v;
  assert(hz < rate/2);
  static double phase = 0;
  for(int i = 0; i < n; i++){
    v.push_back(cos(phase));
    phase += hz * 2 * M_PI / rate;
  }
  return v;
}

void
transmit(SoundOut *sout, std::vector<double> samples, const std::string status)
{
  if(sout == 0)
    return;

  if(f8101 >= 0)
    f8101_tx();
  if(dtr >= 0)
    dtr_tx();
  
  display_status = status;
  double howlong = samples.size() / (double) sout->rate();

  transmitting = 1;
  double t0 = now();

  sout->write(samples);

  double t1 = now();
  usleep((howlong - (t1 - t0)) * 1000000);

  transmitting = 0;
  display_status = "";

  if(f8101 >= 0)
    f8101_rx();
  if(dtr >= 0)
    dtr_rx();
}

//
// send one "over" of text.
// the over is over when there's a newline or
// a whole cycle with nothing new typed.
//
void
send_over(SoundOut *sout, int &tx_i)
{
  int finish_up = 0;
  int sent_empty = 0;

  if(rx_call.size() > 0 && mycall.size() > 0){
    std::vector<double> samples = pack_directed(mycall, rx_call, 31, 0, 1, sout->rate(), tx_hz);
    transmit(sout, samples, "START");
  }

  while(1){
    double sec = cycle_second();
    if(sec >= 0.4 && sec < 1.5){
      // we need to know how much would be consumed in order
      // to decide whether to set itype = 2 (end of "over").
      int consumed;
      std::string all = tx_buf.substr(tx_i);
      pack_text(all, consumed, 0, sout->rate(), tx_hz);

      std::string txt;
      int got_newline = 0;
      int got_magic = 0;
      for(int i = 0; i < consumed; i++){
        unsigned int c = all[i] & 0xff;
        if(c == '\n'){
          got_newline = 1;
          break;
        }
        if(c >= 128){
          got_magic = 1;
          break;
        }
        txt.push_back(c);
      }

      int itype = 0;
      if(got_newline || got_magic || (sent_empty && txt.size() == 0)){
        itype = 2;
        if(txt.size() == 0){
          txt = " ";
        }
      }

      std::vector<double> samples = pack_text(txt, consumed, itype, sout->rate(), tx_hz);
      std::string xtxt = txt;
      if(itype == 2)
        xtxt += " <>";
      if(txt.size() > 0){
        transmit(sout, samples, xtxt);
      } else {
        display_status = "waiting for more keyboard input...";
        sleep(12);
      }
      tx_i += consumed;
      if(got_newline)
        tx_i += 1;

      if(txt.size() == 0){
        sent_empty = 1;
      } else {
        sent_empty = 0;
      }

      if(itype & 2)
        break;
    }
    usleep(100 * 1000);
  }
}

void
tx_loop(SoundOut *sout)
{
  int tx_i = tx_buf.size(); // how far we've gotten in tx_buf[].

  while(1){
    // look for ^T -- send steady tone for tuning.
    int tune = 0;
    tx_buf_mu.lock();
    if((int) tx_buf.size() > tx_i && tx_buf[tx_i] == (char)128){
      tune = 1;
      tx_i++;
    }
    tx_buf_mu.unlock();

    if(tune){
      // a one-second tone 
      std::vector<double> v = costone(tx_hz, sout->rate(), sout->rate());
      transmit(sout, v, "tone");
    }

    double sec = cycle_second();
    if(sec >= 0.4 && sec < 1.5){
      //
      // start of a 15-second cycle -- send if there's
      // any typed input.
      //
      int do_text = 0;
      int do_cq = 0;
      int do_reply = 0;
      int do_snr = 0;
      tx_buf_mu.lock();
      if(tx_i < (int) tx_buf.size()){
        unsigned int c = tx_buf[tx_i] & 0xff;
        if(c == 129){
          do_cq = 1;
          tx_i += 1;
        } else if(c == 130){
          // HOW CPY?
          do_reply = 1;
          tx_i += 1;
        } else if(c == 131){
          // SNR
          do_snr = 1;
          tx_i += 1;
        } else if(c < 128){
          do_text = 1;
        }
      }
      tx_buf_mu.unlock();

      if(do_cq){
        std::vector<double> samples = pack_cq(mycall, mygrid, sout->rate(), tx_hz);
        transmit(sout, samples, "CQ CQ CQ");
      } else if(do_reply){
        if(rx_call.size() > 0){
          // 19 is HW CPY?
          std::vector<double> samples = pack_directed(mycall, rx_call, 19, 0, 3, sout->rate(), tx_hz);
          transmit(sout, samples, "HW CPY?");
        }
      } else if(do_snr){
        if(rx_call.size() > 0){
          std::vector<double> samples = pack_directed(mycall, rx_call, 25, rx_snr + 31, 3, sout->rate(), tx_hz);
          transmit(sout, samples, "SNR");
        }
      } else if(do_text){
        send_over(sout, tx_i);
      }
    }

    usleep(100 * 1000);
  }
}

struct termios save_tt;

void
screen_main(SoundIn *sin, SoundOut *sout)
{
  struct termios tt;
  if(tcgetattr(0, &tt) != 0){
    fprintf(stderr, "tcgetattr failed\n");
    exit(1);
  }
  save_tt = tt;
  tt.c_lflag &= ~ICANON;
  if(tcsetattr(0, TCSANOW, &tt) != 0){
    fprintf(stderr, "tcsetattr failed\n");
    exit(1);
  }

  // add a bunch of newlines to the tx_buf to make
  // the initial keyboard input lines appear at
  // the very bottom. tx_loop() won't send these.
  for(int i = 0; i < 20; i++){
    tx_buf.push_back(' ');
    tx_buf.push_back('\n');
  }

  qso_fp = fopen("qso-trace.txt", "a");
  if(qso_fp)
    setbuf(qso_fp, 0);
  
  sin->start();

  if(sout){
    sout->start();
  }

  std::thread in_th( [ sin ] () {
    rx_loop(sin, fate_cb);
  } );

  std::thread kb_th( [ ] () { kb_loop(); } );

  std::thread tx_th( [ sout ] () {
    if(sout){
      tx_loop(sout);
    }
 } );

  while(1){
    layout lay;
    get_layout(lay);

    lines_mu.lock();
    if(lay.pan_rows > (int) lines.size()){
      for(int i = lines.size(); i < lay.pan_rows; i++){
        Line x;
        x.last_ = 0;
        x.hz_ = -1;
        x.snr_ = 0;
        lines.push_back(x);
      }
    }

    while(lay.pan_rows < (int) lines.size()){
      lines.pop_back();
    }

    lines_mu.unlock();

    draw_screen();

    usleep(200*1000);
  }
}


int
main(int argc, char *argv[])
{
#if 0
  int g = pack_grid("FN42");
  printf("FN42 -> %d 0x%04x -> %s\n", g, g, unpack_grid(g).c_str());
  exit(1);
#endif
#if 0
  unsigned int x;
  pack_call_28("X1YZ", x);
  std::string s = unpack_call(x);
  printf("%u -> >>>%s<<<\n", x, s.c_str());
  exit(1);
#endif
#if 0
  pack_cq("X1XX", 3000, 1500);
  exit(0);
#endif
#if 0
  void test_pack();
  test_pack();
  exit(0);
#endif
  
  double only = -1;
  char *incard = 0;
  char *inchan = 0;
  char *outcard = 0;
  char *outchan = 0;
  const char *cardfile = 0;

  if(argc < 2)
    usage();

  int ai = 1;
  while(ai < argc){
    if(strcmp(argv[ai], "-list") == 0){
      snd_list();
      exit(0);
    } else if(strcmp(argv[ai], "-bench") == 0){
      benchmark();
      exit(0);
    } else if(strcmp(argv[ai], "-opt") == 0){
      optimize();
      exit(0);
    } else if(strcmp(argv[ai], "-only") == 0 && ai+1 < argc){
      ai++;
      only = atof(argv[ai]);
      ai++;
    } else if(strcmp(argv[ai], "-card") == 0 && ai+2 < argc){
      ai++;
      incard = argv[ai];
      ai++;
      inchan = argv[ai];
      ai++;
    } else if(strcmp(argv[ai], "-c") == 0 && ai+2 < argc){
      ai++;
      mycall = argv[ai];
      ai++;
      mygrid = argv[ai];
      ai++;
    } else if(strcmp(argv[ai], "-cardfile") == 0 && ai+1 < argc){
      ai++;
      cardfile = argv[ai];
      ai++;
    } else if(strcmp(argv[ai], "-out") == 0 && ai+2 < argc){
      ai++;
      outcard = argv[ai++];
      outchan = argv[ai++];
    } else if(strcmp(argv[ai], "-levels") == 0 && ai+2 < argc){
      SoundIn *sin = SoundIn::open(argv[ai+1], argv[ai+2], 6000);
      sin->start();
      sin->levels();
      exit(0);
    } else if(strcmp(argv[ai], "-f8101") == 0 && ai + 1 < argc){
      ai++;
      f8101_init(argv[ai]);
      ai++;
    } else if(strcmp(argv[ai], "-dtr") == 0 && ai + 1 < argc){
      ai++;
      dtr_init(argv[ai]);
      if(dtr >= 0)
        dtr_rx();
      ai++;
    } else {
      usage();
    }
  }

  fflush(stdout);
  setvbuf(stdout, 0, _IOFBF, 0);

  if(incard != 0){
    SoundIn *sin = SoundIn::open(incard, inchan, 6000);
    SoundOut *sout = 0;
    if(outcard != NULL){
      fprintf(stderr, "warning: -out but no -c CALL GRID\n");
      sleep(1);
      sout = SoundOut::open(outcard, outchan, 6000);
    }
    screen_main(sin, sout);
  }

  if(cardfile){
    FileSoundIn *sin = new FileSoundIn(cardfile, 6000);
    screen_main(sin, 0);
  }
}
