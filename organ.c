/*

Simple pipe / drawbar organ synthesizer using additive synthesis
for the ALSA framework (MIDI in and PCM audio out)

(compile with c++ organ.c -lasound)

Blame RG 2014

*/

#define debug

#define ALSA_PCM_NEW_HW_PARAMS_API
#include <alsa/asoundlib.h> //Sound output and midi input
#include <math.h> //For sin(), pow() etc.
#include <stdio.h>
#include <sys/poll.h>
#define unitAmpl -2048.0 //Unity amplitude of an oscillator

// function declarations:
void errormessage(const char *format, ...);

float timbre[10][10]; //Storage for harmonic series of organ stops

float freq_table[96]; //Store frequencies for MIDI codes

void make_table()
{
 //Create lookup table using root inversion
 float tw_root_two = pow(2.0, (1.0/12.0));
 //fprintf(stderr, "12:th root of 2: %f \n", tw_root_two);
 for (int i=0; i< 96; i++)
 {
   freq_table[i] = 65.406 * pow(tw_root_two, i);
 }
}

class Oscillator
//The basic unit, where sine waves are produced
{
 private:
   float val[3];
   float a1;
 public:
   Oscillator();
   void set(float f, float a);
   float freq;
   float ampl;
   float next();
};

Oscillator::Oscillator()
{
 freq = 0.02;
 a1   = 2.0*cos(freq);
 //val[0] = 0.0;
 val[0] = 0.0; //y(-1)
 val[1] = unitAmpl*sin(freq);
}

void Oscillator::set(float f, float a)
{
 freq = f;
 ampl = a;
 a1   = 2.0*cos(freq);
 //val[0] = 0.0;
 val[0] = 0.0; //y(-1)
 val[1] = -1.0*ampl*sin(freq);
}

float Oscillator::next()
{
 val[2] = val[1]; //y(-2)
 val[1] = val[0]; //y(-1)
 val[0] = a1*val[1]-val[2]; //y(0)
 return val[0];
}

class Voice
//A set of multiple sine oscillators to produce fundamental and harmonics
{
 private:
   Oscillator waves[7]; //Fundamental [0], harmonics [1,2, ...]
   float volume;
   float ampl;
   float attack;
   float release;
 public:
   Voice();
   void play(int n, int k, float f);
   void rel();
   void reTrig();
   float next();
   int note; //Which MIDI note is playing?
   int channel; //Which MIDI channel is this voice allocated to?
   float mixer[7];
};

Voice::Voice()
{
 volume = 0.0;
 ampl   = 0.0;
 attack = 0.0;
 note   = -1;
 for(int i=0;i<7;i++)
 {
   mixer[i]=0.0;
 }
}

void Voice::play(int theChannel, int theNote, float theFreq)
{
 note = theNote; //Save note code
 channel = theChannel; //Dito with channel info
 float f=theFreq/44100.0;

 waves[0].set(1.0*f, 1024.0); //Fundamental
 waves[1].set(2.0*f, 1024.0);  //First (octave)
 waves[2].set(3.0*f, 1024.0);    //Second (octave + 5.)
 waves[3].set(4.0*f, 1024.0);   //Third (octave + )
 waves[4].set(5.0*f, 1024.0);   //Fourth (2. octave)
 waves[5].set(6.0*f, 1024.0);   //Fift
 waves[6].set(7.0*f, 1024.0);   //Sixth

 volume = 1.0;
 ampl   = 0.0;
 attack = 0.005;
 release  = 0;
}

void Voice::reTrig()
{
 volume = 1.0;
 ampl   = 0.0;
 attack = 0.005;
 release  = 0;
}

void Voice::rel()
{
 volume = 0.0;
 attack = 0;
 release  = 0.0008;
}

float Voice::next()
{
 float sum 
 = waves[0].next()*mixer[0]
 + waves[1].next()*mixer[1]
 + waves[2].next()*mixer[2]
 + waves[3].next()*mixer[3]
 + waves[4].next()*mixer[4]
 + waves[5].next()*mixer[5]
 + waves[6].next()*mixer[6];
 if(volume > 0 && ampl < volume)
 {
   ampl+=attack;
 }
 else
 {
   if(volume == 0 && ampl > 0)
   {
    ampl-=release;
    if (ampl < 0.2 && ampl > -0.2)
    {
     ampl = 0.0;
    }
   }
 }
 return sum*ampl;
}

class Arbiter
//Handle the oscillator resourses
{
  public:
   Arbiter();
   int resources[10];
   void shift();
   void reorder(int voice);
};

class Organ
{
 private:
   Arbiter theArbiter;
 public:
   Organ();
   Voice voices[10];
   void setReg(int r);
   float next();

   void noteOn(int theChannel, int theNote);
   void noteOff(int theChannel, int theNote);
};

void Organ::setReg(int r)
{
 for (int i=0;i<10;i++) //Iterate over voices
 {
   for (int h=0;h<7;h++) //Iterate over harmonic's mixer values
   if(timbre[r][h]>0.0)
   {
    voices[i].mixer[h]=pow(2.0,(timbre[r][h]/400.0))/8;
   }
   else
   {
    voices[i].mixer[h]=0.0;
   }
 }
}

Organ::Organ()
{
 setReg(3);
}

void Organ::noteOn(int theChannel, int theNote)
{
  //Is that note already playing? (Don't play the same note on two oscillators)
  int ok=0;
  int i;
  for(i=0; i<10; i++)
  {
    if (voices[i].note == theNote && voices[i].channel==theChannel)
    {
      //Only re-trigger ADSR gate
      voices[i].reTrig();
      ok=1;
      theArbiter.reorder(i);
     break;
    }
  }

  if(!ok) //If not, then allocate least recently used oscillator
  {
      i=theArbiter.resources[0];
      theArbiter.shift();
      voices[i].play(theChannel, theNote, freq_table[theNote]);
      #ifdef debug
       fprintf(stderr, "use voice#: %d \n", i);
      #endif
  }

}

void Organ::noteOff(int theChannel, int theNote)
{
  //Is that note (still) playing?
  int ok=0;
  int i;
  for(i=0; i<10; i++)
  {
    if (voices[i].note == theNote && voices[i].channel == theChannel)
    {
      //Only re-trigger ADSR gate
      voices[i].rel();
      break;
    }
  }
  voices[i].rel();
}

float Organ::next()
{
 float val
  = voices[0].next()
  + voices[1].next()
  + voices[2].next()
  + voices[3].next()
  + voices[4].next()
  + voices[5].next()
  + voices[6].next()
  + voices[7].next()
  + voices[8].next()
  + voices[9].next();
 return val;
}


Arbiter::Arbiter()
{
 for (int i=0;i<10;i++)
 {
   resources[i]=i;
 }
}

void Arbiter::shift()
{
  int temp = resources[0];
  for (int i=0;i<10-1;i++)
  {
    resources[i]=resources[i+1];
  }
  resources[9]=temp;
}

void Arbiter::reorder(int voice)
{
  int i;
  int found=0;
  for (i=0;i<9;i++)
  {
    if(resources[i]==voice)
    {
      found=1;
    }
    if(found)
    {
     resources[i]=resources[i+1];
    }
  }
  resources[9]=voice;
}

class Reverb
{
  private:
   float *delayPipe;
   int   length;
   float feedback;
   int   pointerIn;
   int   pointerOut;

  public:
    Reverb();
    Reverb(int l, float f);
    float next(float input);
};

Reverb::Reverb()
{
  for (int i=0;i<32768;i++)
  {
    delayPipe[i]=0;
  }
  pointerIn=0;
  pointerOut=2;
}

Reverb::Reverb(int l, float f)
{
  delayPipe = new float[32768];
  for (int i=0;i<32768;i++)
  {
    delayPipe[i]=0;
  }

  length     = l;
  feedback   = f;
  pointerIn  = 0;
  pointerOut = l;
}

float Reverb::next(float input)
{
 float outValue       = delayPipe[pointerOut];
 if(pointerIn > length)
    fprintf(stderr, "pointerIn Out of bounds\n");
 if(pointerOut > length)
    fprintf(stderr, "pointerOut Out of bounds\n");

 delayPipe[pointerIn] = input+outValue*feedback;
 ++pointerIn  %= length;
 ++pointerOut %= length;
 return outValue;
}

void set_timbres()
{
 //Principal 8"
 timbre[0][0] = 500.0;
 timbre[0][1] = 700.0;
 timbre[0][2] = 500.0;
 timbre[0][3] = 400.0;
 timbre[0][4] = 200.0;
 timbre[0][5] = 100.0;
 timbre[0][6] = 0.0;

 //Diapson 8"
 timbre[1][0] = 500.0;
 timbre[1][1] = 600.0;
 timbre[1][2] = 400.0;
 timbre[1][3] = 200.0;
 timbre[1][4] = 100.0;
 timbre[1][5] = 100.0;
 timbre[1][6] = 0.0;

 //Clarinet 8"
 timbre[2][0] = 800.0;
 timbre[2][1] = 0.0;
 timbre[2][2] = 800.0;
 timbre[2][3] = 0.0;
 timbre[2][4] = 800.0;
 timbre[2][5] = 400.0;
 timbre[2][6] = 0.0;

 //Trumpet 8"
 timbre[3][0] = 600.0;
 timbre[3][1] = 700.0;
 timbre[3][2] = 800.0;
 timbre[3][3] = 600.0;
 timbre[3][4] = 500.0;
 timbre[3][5] = 300.0;
 timbre[3][6] = 0.0;

 //Cello 8"
 timbre[4][0] = 400.0;
 timbre[4][1] = 500.0;
 timbre[4][2] = 400.0;
 timbre[4][3] = 500.0;
 timbre[4][4] = 400.0;
 timbre[4][5] = 400.0;
 timbre[4][6] = 200.0;
}

//int main() {
int main(int argc, char *argv[]) {

  //initscr(); //Initialize ncurses
  //noecho();
  set_timbres();
  make_table(); //Set up note frequencies int the table freq_table[]

  Organ theOrgan;

  Reverb* rev1 = new Reverb(2500, 0.8);
  //Reverb* rev2 = new Reverb(4000, 0.8);
  //Reverb* rev3 = new Reverb(5500, 0.8);

  int rc;
  int size;
  snd_pcm_t *handle;
  snd_pcm_hw_params_t *params;
  unsigned int val;
  int dir;
  snd_pcm_uframes_t frames;
  char *buffer;

  /* Open PCM device for playback. */
  rc = snd_pcm_open(&handle, "default",
                    SND_PCM_STREAM_PLAYBACK, 0);
  if (rc < 0) {
    fprintf(stderr,
            "unable to open pcm device: %s\n",
            snd_strerror(rc));
    exit(1);
  }

  /* Allocate a hardware parameters object. */
  snd_pcm_hw_params_alloca(&params);

  /* Fill it in with default values. */
  snd_pcm_hw_params_any(handle, params);

  /* Set the desired hardware parameters. */


  /* Set period size to 32 frames. */
  frames = 16;
  snd_pcm_hw_params_set_period_size_near(handle,
                              params, &frames, &dir);

  /* Write the parameters to the driver */
  rc = snd_pcm_hw_params(handle, params);
  if (rc < 0) {
    fprintf(stderr,
            "unable to set hw parameters: %s\n",
            snd_strerror(rc));
    exit(1);
  }

  snd_pcm_uframes_t bufferSize;

  snd_pcm_hw_params_get_buffer_size( params, &bufferSize );

      fprintf(stderr,
              "alsa_buffer_size: %lu frames\n", bufferSize);


    rc = snd_pcm_set_params(handle,
    SND_PCM_FORMAT_S16_LE,
    SND_PCM_ACCESS_RW_INTERLEAVED,
    2,
    44100,
    32,
    50000); //Latency


  /* Use a buffer large enough to hold one period */
  snd_pcm_hw_params_get_period_size(params, &frames,
                                    &dir);

  size = frames * 4; /* 2 bytes/sample, 2 channels */
  buffer = (char *) malloc(size);
  int16_t* buffer_16 = (int16_t *) buffer; //Cast buffer for 16-bit values

  /* We want to loop for 5 seconds */
  snd_pcm_hw_params_get_period_time(params,
                                    &val, &dir);
  /* 5 seconds in microseconds divided by
   * period time */
  //loops = 5000000 / val;

  //rc = snd_pcm_nonblock(handle, 0);


// Make init for MIDI INPUT
   int status;
   int mode = SND_RAWMIDI_SYNC | SND_RAWMIDI_NONBLOCK;
   snd_rawmidi_t* midiin = NULL;
   const char* portname = "hw:1,0,0";  // see alsarawportlist.c example program
   if ((argc > 1) && (strncmp("hw:", argv[1], 3) == 0)) {
      portname = argv[1];
   }
   if ((status = snd_rawmidi_open(&midiin, NULL, portname, mode)) < 0) {
      errormessage("Problem opening MIDI input: %s", snd_strerror(status));
      exit(1);
   }

   int count = 0;         // Current count of bytes received.
   char mid_buffer[1];        // Storage for input buffer received
   int framesleft=0;

   //Storage for MIDI-parsing:
   int midi_cmd;
   int midi_count=0;
   int midi_channel=0;
   int midi_note;
   int midi_offset=1; //Transpose value
//

  while (true) {

    for(int i=0;i<frames;i++)
    {
      float organOut = theOrgan.next(); //Read output from organ
      float outValue = organOut*3.0
        + rev1->next(organOut)*0.5 
	//+ rev2->next(organOut)*0.25
	//+ rev3->next(organOut)*0.12
;

      buffer_16[i*2] = (int16_t)outValue;
      buffer_16[i*2+1] = (int16_t)outValue;
    }
   rc = snd_pcm_writei(handle, buffer, frames);

    if (rc == -EPIPE) {
      /* EPIPE means underrun */
      fprintf(stderr, "underrun occurred\n");
      snd_pcm_prepare(handle);
    } else if (rc < 0) {
      fprintf(stderr,
              "error from writei: %s\n",
              snd_strerror(rc));
    }  else if (rc != (int)frames) {
      fprintf(stderr,
              "short write, write %d frames\n", rc);
    }
else
{
/*
*/
}

     //MIDI code

      status = snd_rawmidi_read(midiin, mid_buffer, 1);
      if(status > 0)
      {
       unsigned char theByte = mid_buffer[0];
       //Simple MIDI parser //Blame RG 2014

                if((theByte & 0x80) == 0x80) //Is it a command?
                {
                  if((theByte & 0x90) == 0x90) //Note on
                  {
                    midi_channel = (theByte & 0x0f);
                    #ifdef debug
                      fprintf(stderr, "midi_channel: %d \n", midi_channel);
                    #endif
                    midi_count=1;
                    midi_cmd=0x90;
                  }
                  else
                  {
                   if((theByte & 0x90) == 0x80) //Note off
                   {
                    midi_channel = (theByte & 0x0f);
                    #ifdef debug
                     fprintf(stderr, "midi_channel: %d \n", midi_channel);
                    #endif
                    midi_count=1;
                    midi_cmd=0x80;
                   }
                  }
                } //End of command processing
                else
                {
                  if (midi_count==1)
                  {
                   midi_note = (theByte+midi_offset);
                   midi_count++;
                  }
                  else
                  {
                   if (midi_count==2)
                    {
                     //Velocity info here
                     midi_count=1;
                     if(midi_cmd == 0x90)
                     {
                      if(theByte!=00) //21.11.2014 (Check if note on with vel 0 is used as note off!)
                      {
                       theOrgan.noteOn(midi_channel, midi_note);
                       #ifdef debug
                        fprintf(stderr, "play note: %d \n", midi_note);
                       #endif
                      }
                      else
                      {
                       theOrgan.noteOff(midi_channel, midi_note);
                       #ifdef debug
                        fprintf(stderr, "note on, volume 0: %d \n", midi_note);
                       #endif
                      }

                     }
                     if(midi_cmd == 0x80)
                     {
                      theOrgan.noteOff(midi_channel, midi_note);
                      #ifdef debug
                       fprintf(stderr, "note off: %d \n", midi_note);
                      #endif
                     }
                   }
                  }
                 }
       //count++;
     }
      //END MIDI CODE

     char command[80];

     struct pollfd fds;
        int ret;
        fds.fd = 0; /* this is STDIN */
        fds.events = POLLIN;
        ret = poll(&fds, 1, 0);
        if(ret == 1)
        {
          read(STDIN_FILENO, command, 80);
          switch(command[0])
          {
            case '0':
                     printf("Command 0\n");
                     theOrgan.setReg(0);
                     break;
            case '1':
                     printf("Command 1\n");
                     theOrgan.setReg(1);
                     break;
            case '2':
                     printf("Command 2\n");
                     theOrgan.setReg(2);
                     break;
            case '3':
                     printf("Command 3\n");
                     theOrgan.setReg(3);
                     break;
            case '4':
                     printf("Command 4\n");
                     theOrgan.setReg(4);
                     break;
          }
        }
        else if(ret == 0)
               {
                //printf("No\n");
                }
        else
                printf("Error\n");

  }

  snd_pcm_drain(handle);
  snd_pcm_close(handle);
  free(buffer);

  snd_rawmidi_close(midiin);
  midiin  = NULL;    // snd_rawmidi_close() does not clear invalid pointer,

  return 0;
}

// error -- print error message
//

void errormessage(const char *format, ...) {
   va_list ap;
   va_start(ap, format);
   vfprintf(stderr, format, ap);
   va_end(ap);
   putc('\n', stderr);
}

