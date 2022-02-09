# fate
JS8 software

This is a JS8 program suitable for running in a text-only terminal
window on Linux, MacOS, or FreeBSD. It's pretty minimal compared to
JS8Call, implementing only what's needed for a QSO.

It requires fftw3, sndfile, and portaudio libraries to already
be installed. To compile:

```
  make
```

To run:

```
  ./fate -c MYCALL MYGRID -card CARD CHAN -out CARD CHAN
```

To find the numbers of sound cards, run fate -list. If you don't want
to transmit, omit the -c and -out. If you need to toggle DTR on a serial line
to transmit, try the -dtr argument. You can omit the -c arguments
if you set the MYCALL and MYGRID environment variables.

You should see something like this in your terminal window:

```
RX 1400   
A  654 /L/HKM/ HEARTBEAT SNR +09<>[WP4OH M0SUY ACK 29]
B 2056 KA2YNT: CQ CQ CQ FM29
C  558 SV1UH: HB KM08
D  495 M0SUY: HB IO94
E 1625 [EB5TT M0SUY ACK 22]
F  950 [W5ODJ M0SUY ACK 25]
G  896 [F4IOG M0SUY ACK 25]
H 1924 [ZS1DCC ?00ABG GRID 0]JF96GC75ME<>
I 1399 [AK4ZX KK4WRE SNR? 0]
J   -1 
K   -1 
-------------------------------------------------------------------------------
- 
- 
- 
- 
- 
---- 1400  0 RX ---------------------------------------------------------------
>  
>  
>  
>  
> 
```

The lines starting with upper-case letters are a Digipan-like display
of received signals; the number is audio frequency in Hz. You can
select one of the signals as the call-sign to talk to with control-A
followed by the letter. From then on, messages from that call-sign
will show up in the middle "pane". Your typed input will appear in the
lower pane. Type newline to send the <> at the end of an "over".

Type control-E to send a CQ. Type control-F to send HW CPY? to the
selected call-sign. Type control-G to send an SNR report to the
selected call-sign. Type control-C or control-X to quit. The file
qso-trace.txt will contain the text of signals you have selected with
control-A.

Missing: decoding and transmitting compound call-signs; speeds other
than Normal; indication of input audio level.

Robert Morris, AB1HL
