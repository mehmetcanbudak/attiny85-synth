// attiny85 should be running at 16mhz
/*
# mi:muz:prot#1
#                        +------+
#           RESET A0 PB5 |1    8| VCC
#                 A3 PB3 |2    7| PB2 A1 INT0    (USB D-)
#            OC1B A2 PB4 |3    6| PB1 OC0B/OC1A  (USB D+)
#                    GND |4    5| PB0 OC0A       (tx)
#                        +------+
*/
//////////////////////////////////////////////////////////////

#include "VUSBMidiATtiny.h"

//SendOnlySoftwareSerial mySerial (0); 
#define NUM_CHANS 3 // number of speaker outputs

#define CPU_MHZ F_CPU/1000000   // ATtiny CKSEL 0100: 8 Mhz, 1x prescale (lfuse=E4)



#define SPEAKER0 PB0  // output, speaker 1 (also USB-TTL RX)
#define SPEAKER1 PB4  // output, speaker 2 (also USB-TTL TX)


#define SPEAKER2 PB3 // output, speaker 1 (was input, option jumper 2)
  // output, speaker 3 (was input, option jumper 1)
//               PA2  // input, USB reset




#define PORT(x) _SFR_IO8(x)


// speaker registers
// We use constants for speaker ports because we don't want any overhead
// in the interrupt routines; the loop through the speakers there is unrolled.

#define SPEAKER0_REG PORTB  //  data register
#define SPEAKER0_DIR DDRB //  direction register

#define SPEAKER1_REG PORTB  //  data register
#define SPEAKER1_DIR DDRB //  direction register

#define SPEAKER2_REG PORTB  //  data register
#define SPEAKER2_DIR DDRB //  direction register

  //  direction register



// variable for timing

volatile unsigned int scorewait_interrupt_count;
volatile unsigned int delaywait_interrupt_count;

// variables for music-playing



volatile long accumulator [NUM_CHANS];
volatile long decrement [NUM_CHANS];
volatile boolean playing [NUM_CHANS];

/* Table of accumulator decrement values, generated by a companion Excel spreadsheet.
   These depend on the polling frequency and the accumulator restart value.
   We basically do incremental division for each channel in the polling interrupt routine:
        accum -= decrement
        if (accum < 0) {
            toggle speaker output
            accum += ACCUM_RESTART
        }
*/

#define POLLTIME_USEC 50    // polling interval in microseconds
#define ACCUM_RESTART 4194304L  // 2^22 allows 1-byte addition on 3- or 4-byte numbers
#define MAX_NOTE 123

const long decrement_PGM[MAX_NOTE+1] PROGMEM = {
    3429L, 3633L, 3849L, 4078L, 4320L, 4577L, 4850L, 5138L, 5443L, 5767L, 6110L, 6473L, 
    6858L, 7266L, 7698L, 8156L, 8641L, 9155L, 9699L, 10276L, 10887L, 11534L, 12220L, 
    12947L, 13717L, 14532L, 15396L, 16312L, 17282L, 18310L, 19398L, 20552L, 21774L, 
    23069L, 24440L, 25894L, 27433L, 29065L, 30793L, 32624L, 34564L, 36619L, 38797L, 
    41104L, 43548L, 46137L, 48881L, 51787L, 54867L, 58129L, 61586L, 65248L, 69128L, 
    73238L, 77593L, 82207L, 87096L, 92275L, 97762L, 103575L, 109734L, 116259L, 123172L, 
    130496L, 138256L, 146477L, 155187L, 164415L, 174191L, 184549L, 195523L, 207150L, 
    219467L, 232518L, 246344L, 260992L, 276512L, 292954L, 310374L, 328830L, 348383L, 
    369099L, 391047L, 414299L, 438935L, 465035L, 492688L, 521984L, 553023L, 585908L, 
    620748L, 657659L, 696766L, 738198L, 782093L, 828599L, 877870L, 930071L, 985375L, 
    1043969L, 1106047L, 1171815L, 1241495L, 1315318L, 1393531L, 1476395L, 1564186L, 
    1657197L, 1755739L, 1860141L, 1970751L, 2087938L, 2212093L, 2343631L, 2482991L, 
    2630637L, 2787063L, 2952790L, 3128372L, 3314395L, 3511479L, 3720282L, 3941502L, 
    4175876L
};
/*
void tune_playnote (byte chan, byte note);
void tune_stopnote (byte chan);
void tune_stepscore (void);*/


//--------------------------------------------------------------------------
// Initialize the timers
//--------------------------------------------------------------------------
int squarewavespins[3]={SPEAKER0,SPEAKER1,SPEAKER2};
void init_timers () {
    
    // We use the 8 bit timer to generate the polling interrupt for notes.
    // It should interrupt often, like every 50 microseconds.
    
    TCCR0A = (1 << WGM01);  // mode 010: CTC   
#if CPU_MHZ==16
    TCCR0B = 1 << CS00; 
    TCCR0B = 1 << CS01; // clock select 011: clk/16 prescaling
    OCR0A = CPU_MHZ/8 * POLLTIME_USEC;
#elif CPU_MHZ==8
    TCCR0B = 1 << CS01;   // clock select 010: clk/8 prescaling
    OCR0A = CPU_MHZ/8 * POLLTIME_USEC;
#else
 unusual frequency
#endif
    

    TIMSK =(1<<OCIE0A) | (1<<OCIE1A); // turn on match A interrupts for both timers


}


void tune_playnote (byte chan, byte note) {

    if (chan < NUM_CHANS) {
        if (note>MAX_NOTE) note=MAX_NOTE;
        decrement[chan] = pgm_read_dword(decrement_PGM + note);
        accumulator[chan] = ACCUM_RESTART;
        playing[chan]=true;
    }
}



void tune_stopnote (byte chan) {
    playing[chan]= false;
    PORTB = (0<<squarewavespins[chan]);
}


ISR(TIMER0_COMPA_vect) { //******* 8-bit timer: 50 microsecond interrupts
    
// We unroll code with a macro to avoid loop overhead.
// For even greater efficiency, we could write this in assembly code
// and do 3-byte instead of 4-byte arithmetic.

  #define dospeaker(spkr) if (playing[spkr]) {   \
      accumulator[spkr] -= decrement[spkr];    \
  if (accumulator[spkr]<0) {           \
      SPEAKER##spkr##_REG ^= (1<<SPEAKER##spkr); \
      accumulator[spkr] += ACCUM_RESTART;    \
    }                      \
  } 
  
  dospeaker(0);
  dospeaker(1);
  dospeaker(2);

}

ISR(TIMER1_COMPA_vect) { //******* 16-bit timer: millisecond interrupts
  

 }// countdown for tune_delay()
byte a;
byte b;
byte c;
bool chan1;
bool chan2;
bool chan3;

void startchan2(int notaa){
 
if(chan2==0&&b!=notaa){        
tune_playnote(1,notaa);
b=notaa;
chan2=1;
}
}
void startchan3(int notaa){
 
if(chan3==0&&c!=notaa){        
tune_playnote(0,notaa);
c=notaa;
chan3=1;
}
}
void midinote(byte ch,byte note,byte vel){
  if(ch>0x8F&&ch<0xA0&&ch!=0x99){

}
  
    if(ch>0x7F&&ch<0x90&&ch!=0x89){
 
  }
}
void onNoteOn(byte ch, byte note, byte vel){
    if(chan2==1&&chan1==1){
    startchan3(note);
    }
    if(chan1==1){
  startchan2(note);
}
if(chan1==0&&a!=note){        
tune_playnote(2,note);
a=note;
chan1=1;
}
}

void onNoteOff(byte ch, byte note, byte vel){
    if(chan1==1&&a==note){ 
    tune_stopnote(2); 
    a=0;
    chan1=0;
  }
      if(chan2==1&&b==note){ 
    tune_stopnote(1); 
    b=0;
    chan2=0;
  }
      if(chan3==1&&c==note){ 
    tune_stopnote(0); 
    c=0;
    chan3=0;
  }
}

void onCtlChange(byte ch, byte num, byte value){

}


void setup() {

 
  UsbMidi.init();
  UsbMidi.setHdlNoteOff(onNoteOff);
  UsbMidi.setHdlNoteOn(onNoteOn);
  UsbMidi.setHdlCtlChange(onCtlChange);
    init_timers();      // initialize both timers, for music and for delays
    
    // configure I/O ports

  DDRB = (1<<SPEAKER0)+(1<<SPEAKER1)+(1<<SPEAKER2);
}

void loop() {
 
  wdt_reset();
  UsbMidi.update();
}




