
extern "C" {
  typedef int (*cb_t)(int *a87, double hz0, double hz1, double off,
                      const char *, double snr);
  void entry(double xsamples[], int nsamples, int start, int rate,
             double min_hz,
             double max_hz,
             int hints1[], int hints2[],
             cb_t cb);

  double set(char *param, char *val);
};

extern void gauss_jordan(int rows, int cols, int m[174][2*91], int which[91], int *ok);

extern int check_crc(const int a91[91]);

void ldpc_decode(double llcodeword[], int iters, int plain[], int *ok);

extern int osd_decode(double codeword[174], int depth, int out[87], int*);

extern void ldpc_encode(int plain[87], int codeword[174]);

extern void ft8_crc(int msg1[], int msglen, int out[12]);

extern int gen_sys[174][91];

std::string unpack(const int a87[87], std::string &other_call);

std::string unpack_call(unsigned int x);

std::string unpack_grid(int ng);

void snd_list();

