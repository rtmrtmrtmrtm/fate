
CXX = c++ -O

FLAGS = -std=c++17 -I/opt/local/include -I/usr/local/include
LIBS = -L/opt/local/lib -L/usr/local/lib -lfftw3 -lsndfile -lportaudio

MOREC = 
MOREH = 

# uncomment if you have the airspyhf and liquid dsp libraries.
# try -lusb or -lusb-1.0 ; also apt install libusb-1.0-0-dev
# CXX += -DUSE_AIRSPYHF
# LIBS += -lairspyhf -lliquid -lusb
# FLAGS += -I/opt/local/include/libairspyhf -I/usr/include/libairspyhf -I/usr/rtm/airspyhf/libairspyhf/src

# for the RFSpace SDR-IP, NetSDR, CloudIQ and CloudSDR in I/Q mode.
# CXX += -DUSE_SDRIP
# MOREC += sdrip.cc cloudsdr.cc
# MOREH += sdrip.h cloudsdr.h
# LIBS += -lliquid

SRC = js8.cc common.cc libldpc.cc pack.cc unpack.cc snd.cc fft.cc util.cc $(MOREC)

fate: fate.cc $(SRC) $(MOREH)
	$(CXX) $(FLAGS) fate.cc $(SRC) -o fate $(LIBS) -pthread

clean:
	rm -f fate

